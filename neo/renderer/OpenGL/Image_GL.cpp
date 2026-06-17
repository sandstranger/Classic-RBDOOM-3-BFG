/*
===========================================================================

Doom 3 BFG Edition GPL Source Code
Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company.
Copyright (C) 2013-2016 Robert Beckebans

This file is part of the Doom 3 BFG Edition GPL Source Code ("Doom 3 BFG Edition Source Code").

Doom 3 BFG Edition Source Code is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

Doom 3 BFG Edition Source Code is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Doom 3 BFG Edition Source Code.  If not, see <http://www.gnu.org/licenses/>.

In addition, the Doom 3 BFG Edition Source Code is also subject to certain additional terms. You should have received a copy of these additional terms immediately following the terms and conditions of the GNU General Public License which accompanied the Doom 3 BFG Edition Source Code.  If not, please request a copy in writing from id Software at the address below.

If you have questions concerning this license or the applicable additional terms, you may contact in writing id Software LLC, c/o ZeniMax Media Inc., Suite 120, Rockville, Maryland 20850 USA.

===========================================================================
*/

#include "precompiled.h"
#if ANDROID
#include "ProcessRGB.hpp"
#include <string>
#include "../TextureCache.h"
#endif
#if defined(__ARM_NEON) || defined(__ARM_NEON__) || defined(__aarch64__) || defined(_M_ARM64)
#include "arm_neon.h"
#endif

#pragma hdrstop
/*
================================================================================================
Contains the Image implementation for OpenGL.
================================================================================================
*/

#include "../RenderCommon.h"
#include "renderer/DXT/DXTCodec.h"

#define GL_LUMINANCE16_ALPHA16			0x8048
#define GL_INTENSITY				0x8049
#define GL_INTENSITY4				0x804A
#define GL_INTENSITY8				0x804B
#define GL_INTENSITY12				0x804C
#define GL_INTENSITY16				0x804D
#define GL_TEXTURE_MAX_ANISOTROPY_EXT     0x84FE
#define GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT 0x84FF
#if ANDROID
#define MIPMAPS_SKIP_LEVEL				1
static bool g_enableTexturesShrinking = false;
bool g_enableTextureCache = false;
static std::string g_pathToTextureCacheDir;
static thread_local std::vector<uint8_t> s_decodeBuffer;
static thread_local std::vector<uint8_t> s_etc2Buffer;

extern "C" {
__attribute__((used)) __attribute__((visibility("default")))
void enableTexturesShrinking(const bool enableTexturesShrinking)
{
    g_enableTexturesShrinking = enableTexturesShrinking;
}

__attribute__((used)) __attribute__((visibility("default")))
void setTextureCacheData(const bool enableTextureCache, const char* pathToTextureCacheDir)
{
    g_enableTextureCache = enableTextureCache;
    g_pathToTextureCacheDir = pathToTextureCacheDir;
    if (enableTextureCache)
    {
        idTextureCache::Instance().Init(g_pathToTextureCacheDir.c_str(),
                                        512 * 1024 * 1024 * 2);
    }
}
}
#endif

int getMipmapSkipLevel(const int numLevels) {
#if ANDROID
	return g_enableTexturesShrinking && MIPMAPS_SKIP_LEVEL < numLevels ? MIPMAPS_SKIP_LEVEL : 0;
#else
	return 0;
#endif
}

/*
====================
idImage::idImage
====================
*/
idImage::idImage( const char* name ) : imgName( name )
{
	texnum = TEXTURE_NOT_LOADED;
	internalFormat = 0;
	dataFormat = 0;
	dataType = 0;
	generatorFunction = NULL;
	filter = TF_DEFAULT;
	repeat = TR_REPEAT;
	usage = TD_DEFAULT;
	cubeFiles = CF_2D;
	cubeMapSize = 0;
	
	referencedOutsideLevelLoad = false;
	levelLoadReferenced = false;
	defaulted = false;
	sourceFileTime = FILE_NOT_FOUND_TIMESTAMP;
	binaryFileTime = FILE_NOT_FOUND_TIMESTAMP;
	refCount = 0;
}

/*
====================
idImage::~idImage
====================
*/
idImage::~idImage()
{
	PurgeImage();
}

/*
====================
idImage::IsLoaded
====================
*/
bool idImage::IsLoaded() const
{
	return texnum != TEXTURE_NOT_LOADED;
}

/*
==============
Bind

Automatically enables 2D mapping or cube mapping if needed
==============
*/
void idImage::Bind()
{
	RENDERLOG_PRINTF( "idImage::Bind( %s )\n", GetName() );
	
	// load the image if necessary (FIXME: not SMP safe!)
	if( !IsLoaded() )
	{
		// load the image on demand here, which isn't our normal game operating mode
		ActuallyLoadImage( true );
	}
	
	const int texUnit = tr.backend.GetCurrentTextureUnit();

	// RB: added support for more types
	tmu_t* tmu = &glcontext.tmu[texUnit];
	// bind the texture
	uint currentMap = 0;
	if (opts.textureType == TT_CUBIC) {
		currentMap = tmu->currentCubeMap;
	}
	else if (opts.textureType == TT_2D_ARRAY) {
		currentMap = tmu->current2DArray;
	}
	else {
		currentMap = tmu->current2DMap;
	}
	if( currentMap != texnum )
	{

#if !defined(USE_GLES2) && !defined(USE_GLES3) && !ANDROID
		if( glConfig.directStateAccess )
		{
			glBindTextureUnit( texUnit, texnum );
		}
		else
#endif
		{
			int target = GL_TEXTURE_2D;
			switch (opts.textureType) {
			case TT_2D:
				target = GL_TEXTURE_2D;
				break;
			case TT_CUBIC:
				target = GL_TEXTURE_CUBE_MAP;
				break;
			case TT_2D_ARRAY:
				target = GL_TEXTURE_2D_ARRAY;
				break;
			case TT_2D_MULTISAMPLE:
				target = GL_TEXTURE_2D_MULTISAMPLE;
				break;
			}
			glActiveTexture( GL_TEXTURE0 + texUnit );
			glBindTexture( target, texnum );
		}
	}
	// RB end
}

/*
====================
CopyFramebuffer
====================
*/

void idImage::CopyFramebufferLegacy(int x, int y, int imageWidth, int imageHeight, bool forceLDR) {
	int target = GL_TEXTURE_2D;
	switch (opts.textureType)
	{
	case TT_2D:
		target = GL_TEXTURE_2D;
		break;
	case TT_CUBIC:
		target = GL_TEXTURE_CUBE_MAP;
		break;
	case TT_2D_ARRAY:
		target = GL_TEXTURE_2D_ARRAY;
		break;
	case TT_2D_MULTISAMPLE:
		target = GL_TEXTURE_2D_MULTISAMPLE;
		break;
	default:
		//idLib::FatalError( "%s: bad texture type %d", GetName(), opts.textureType );
		return;
	}

	glBindTexture(target, texnum);
#if !defined(USE_GLES2)
	if (Framebuffer::IsDefaultFramebufferActive())
	{
		glReadBuffer(GL_BACK);
	}
#endif
	opts.width = imageWidth;
	opts.height = imageHeight;
#if defined(USE_GLES2)
	glCopyTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, x, y, imageWidth, imageHeight, 0);
#else
	if (r_useHDR.GetBool() && globalFramebuffers.hdrFBO->IsBound())
	{

		//if( backEnd.glState.currentFramebuffer != NULL && backEnd.glState.currentFramebuffer->IsMultiSampled() )

#if defined(USE_HDR_MSAA)
		if (globalFramebuffers.hdrFBO->IsMultiSampled())
		{
			glBindFramebuffer(GL_READ_FRAMEBUFFER, globalFramebuffers.hdrFBO->GetFramebuffer());
			glBindFramebuffer(GL_DRAW_FRAMEBUFFER, globalFramebuffers.hdrNonMSAAFBO->GetFramebuffer());
			glBlitFramebuffer(0, 0, glConfig.nativeScreenWidth, glConfig.nativeScreenHeight,
				0, 0, glConfig.nativeScreenWidth, glConfig.nativeScreenHeight,
				GL_COLOR_BUFFER_BIT,
				GL_LINEAR);

			globalFramebuffers.hdrNonMSAAFBO->Bind();

			glCopyTexImage2D(target, 0, forceLDR ? GL_RGBA8 : GL_RGBA16F, x, y, imageWidth, imageHeight, 0);

			globalFramebuffers.hdrFBO->Bind();
		}
		else
#endif
		{
#ifndef ANDROID
			glCopyTexImage2D(target, 0, forceLDR ? GL_RGBA8 : GL_RGBA16F, x, y, imageWidth, imageHeight, 0);
#else
			glCopyTexImage2D(target, 0, GL_RGBA8, x, y, imageWidth, imageHeight, 0);
#endif
		}
	}
	else
	{
		glCopyTexImage2D(target, 0, GL_RGBA8, x, y, imageWidth, imageHeight, 0);
	}
#endif

	// these shouldn't be necessary if the image was initialized properly
	glTexParameterf(target, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameterf(target, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

	glTexParameterf(target, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameterf(target, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}

void idImage::CopyFramebufferDSA(int x, int y, int imageWidth, int imageHeight, bool forceLDR) {
	textureFormat_t HDRFormat = FMT_RGBA8;

	if (r_useHDR.GetBool() && globalFramebuffers.hdrFBO->IsBound() && !forceLDR) {
		HDRFormat = FMT_RGBA16F;
	}
	if (opts.width != imageWidth || opts.height != imageHeight || opts.format != HDRFormat) {
		opts.width = imageWidth;
		opts.height = imageHeight;
		opts.format = HDRFormat;
		//GK: Since DSA doesn't support muttable Textures 
		//then every time we resize the texture we also have 
		//to wipe the old one and create it as new
		AllocImage();
	}
#if !defined(USE_GLES2) && !ANDROID
	if (Framebuffer::IsDefaultFramebufferActive())
	{
		glNamedFramebufferReadBuffer(0, GL_BACK);
	}
#endif
#ifndef ANDROID
	glCopyTextureSubImage2D(texnum, 0, 0, 0, x, y, imageWidth, imageHeight);

	glTextureParameterf(texnum, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTextureParameterf(texnum, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

	glTextureParameterf(texnum, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTextureParameterf(texnum, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
#else
	glBindTexture(GL_TEXTURE_2D, texnum);

	glCopyTexSubImage2D(
			GL_TEXTURE_2D,
			0,
			0, 0,
			x, y,
			imageWidth,
			imageHeight
	);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
#endif
}

void idImage::CopyFramebuffer( int x, int y, int imageWidth, int imageHeight, bool forceLDR )
{
#ifndef ANDROID
	if (!glConfig.directStateAccess) {
		CopyFramebufferLegacy(x, y, imageWidth, imageHeight, forceLDR);
	}
	else {
		CopyFramebufferDSA(x, y, imageWidth, imageHeight, forceLDR);
	}
#else
    CopyFramebufferLegacy(x, y, imageWidth, imageHeight, forceLDR);
#endif
	tr.backend.pc.c_copyFrameBuffer++;
}

/*
====================
CopyDepthbuffer
====================
*/
void idImage::CopyDepthbuffer( int x, int y, int imageWidth, int imageHeight )
{
#ifndef ANDROID
	if (!glConfig.directStateAccess)
#endif
	{
		glBindTexture((opts.textureType == TT_CUBIC) ? GL_TEXTURE_CUBE_MAP : GL_TEXTURE_2D, texnum);

		opts.width = imageWidth;
		opts.height = imageHeight;
		glCopyTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT, x, y, imageWidth, imageHeight, 0);
	}
#ifndef ANDROID
	else {
		if (opts.width != imageWidth || opts.height != imageHeight) {
			opts.width = imageWidth;
			opts.height = imageHeight;
			AllocImage();
		}

		if (opts.textureType == TT_CUBIC) {
			glCopyTextureSubImage3D(texnum, 0, 0, 0, 0, x, y, imageWidth, imageHeight);
		}
		else {
			glCopyTextureSubImage2D(texnum, 0, 0, 0, x, y, imageWidth, imageHeight);
		}
	}
#endif
	tr.backend.pc.c_copyFrameBuffer++;
}

/*
========================
idImage::SubImageUpload
========================
*/

void idImage::SubImageUpload(int mipLevel, int mipLevelToSkip, int x, int y, int z,
                             int width, int height, const void* pic, int pixelPitch)
{
	if (mipLevel < mipLevelToSkip)
	{
		return;
	}

	assert(x >= 0 && y >= 0 && mipLevel >= 0 && width >= 0 && height >= 0 && mipLevel < opts.numLevels);

	int compressedSize = 0;
	const int gpuMipLevel = mipLevel - mipLevelToSkip;

	if (IsCompressed())
	{
		assert(!(x & 3) && !(y & 3));

		int quadW = (width + 3) & ~3;
		int quadH = (height + 3) & ~3;
		compressedSize = quadW * quadH * BitsForFormat(opts.format) / 8;

#ifdef _DEBUG
		int padW = (opts.width + 3) & ~3;
		int padH = (opts.height + 3) & ~3;
#endif

		assert(x + width <= padW && y + height <= padH);

		if (x + width > opts.width)
		{
			width = opts.width - x;
		}
		if (y + height > opts.height)
		{
			height = opts.height - y;
		}
	}
	else
	{
		assert(x + width <= opts.width && y + height <= opts.height);
	}

	int target;
	int uploadTarget;

	if (opts.textureType == TT_2D)
	{
		target = uploadTarget = GL_TEXTURE_2D;
	}
	else if (opts.textureType == TT_CUBIC)
	{
		target = GL_TEXTURE_CUBE_MAP;
		uploadTarget = GL_TEXTURE_CUBE_MAP_POSITIVE_X + z;
	}
	else
	{
		assert(!"invalid opts.textureType");
		target = uploadTarget = GL_TEXTURE_2D;
	}

	glBindTexture(target, texnum);

	if (pixelPitch != 0)
	{
		glPixelStorei(GL_UNPACK_ROW_LENGTH, pixelPitch);
	}

	if (opts.format == FMT_RGB565)
	{
#if !defined(USE_GLES3) && !ANDROID
		glPixelStorei(GL_UNPACK_SWAP_BYTES, GL_TRUE);
#endif
	}

	if (IsCompressed())
	{
#ifdef ANDROID
		if (!glConfig.textureCompressionAvailable)
		{
			const int dxtWidth = (width + 3) & ~3;
			const int dxtHeight = (height + 3) & ~3;
			bool cacheHit = false;
			uint64_t hash = 0;

			if (g_enableTextureCache)
			{
				hash = ComputeTextureHash(pic, compressedSize, dxtWidth, dxtHeight, GL_COMPRESSED_RGBA8_ETC2_EAC);
				std::byte* cachedEtc2 = nullptr;
				size_t cachedSize = 0;

				// Шаг 1: Ищем в RAM-кэше (быстрый путь)
				cacheHit = idTextureCache::Instance().TryGetFromRamCache(hash, &cachedEtc2, &cachedSize);

				if (!cacheHit)
				{
					cacheHit = idTextureCache::Instance().TryGetCachedETC2(
							imgName.c_str(),
							pic, compressedSize,
							dxtWidth, dxtHeight,
							GL_COMPRESSED_RGBA8_ETC2_EAC,
							1,
							&cachedEtc2, &cachedSize
					);

					if (cacheHit)
					{
						idTextureCache::Instance().SaveToRamCache(hash, cachedEtc2, cachedSize,
						                                          dxtWidth, dxtHeight, GL_COMPRESSED_RGBA8_ETC2_EAC);
					}
				}

				if (cacheHit)
				{
					glCompressedTexSubImage2D(uploadTarget, gpuMipLevel, x, y,
					                          dxtWidth, dxtHeight,
					                          GL_COMPRESSED_RGBA8_ETC2_EAC,
					                          static_cast<GLsizei>(cachedSize),
					                          cachedEtc2);
					Mem_Free(cachedEtc2);
					cachedEtc2 = nullptr;
				}
			}

			if (!cacheHit)
			{
				const size_t decodeSize = dxtWidth * dxtHeight * 4;
				if (s_decodeBuffer.size() < decodeSize)
				{
					s_decodeBuffer.resize(decodeSize);
				}
				uint8_t* dpic = s_decodeBuffer.data();

				idDxtDecoder decoder;
				if (opts.format == FMT_DXT1)
				{
					decoder.DecompressImageDXT1((const byte*)pic, dpic, width, height);
				}
				else
				{
					if (opts.colorFormat == CFM_YCOCG_DXT5)
					{
						decoder.DecompressYCoCgDXT5((const byte*)pic, dpic, width, height);
					}
					else if (opts.colorFormat == CFM_NORMAL_DXT5)
					{
						decoder.DecompressNormalMapDXT5Renormalize((const byte*)pic, dpic, width, height);
					}
					else
					{
						decoder.DecompressImageDXT5((const byte*)pic, dpic, width, height);
					}
				}

				const int pixelCount = dxtWidth * dxtHeight;
#if defined(__ARM_NEON) || defined(__ARM_NEON__) || defined(__aarch64__) || defined(_M_ARM64)
				int i = 0;
                for (; i <= pixelCount - 16; i += 16)
                {
                    uint8x16x4_t pixels = vld4q_u8(&dpic[i * 4]);
                    uint8x16_t temp = pixels.val[0];
                    pixels.val[0] = pixels.val[2];
                    pixels.val[2] = temp;
                    vst4q_u8(&dpic[i * 4], pixels);
                }
                for (; i < pixelCount; i++)
                {
                    std::swap(dpic[i * 4], dpic[i * 4 + 2]);
                }
#else
				for (int i = 0; i < pixelCount; i++)
				{
					std::swap(dpic[i * 4], dpic[i * 4 + 2]);
				}
#endif

				const uint32_t blocks = (dxtWidth / 4) * (dxtHeight / 4);
				const size_t etc2CompressedSize = blocks * 16;
				if (s_etc2Buffer.size() < etc2CompressedSize)
				{
					s_etc2Buffer.resize(etc2CompressedSize);
				}
				uint8_t* etc2Data = s_etc2Buffer.data();

				CompressEtc2Rgba(
						reinterpret_cast<const uint32_t*>(dpic),
						reinterpret_cast<uint64_t*>(etc2Data),
						blocks,
						dxtWidth,
						true
				);

				if (g_enableTextureCache)
				{
					idTextureCache::Instance().SaveToRamCache(hash, etc2Data, etc2CompressedSize,
					                                          dxtWidth, dxtHeight, GL_COMPRESSED_RGBA8_ETC2_EAC);
					idTextureCache::Instance().SaveToCacheAsync(
							imgName.c_str(),
							pic, compressedSize,
							dxtWidth, dxtHeight,
							GL_COMPRESSED_RGBA8_ETC2_EAC,
							1,
							etc2Data, etc2CompressedSize
					);
				}

				glCompressedTexSubImage2D(uploadTarget, gpuMipLevel, x, y,
				                          dxtWidth, dxtHeight,
				                          GL_COMPRESSED_RGBA8_ETC2_EAC,
				                          static_cast<GLsizei>(etc2CompressedSize),
				                          etc2Data);
			}
		}
		else
		{
			glCompressedTexSubImage2D(uploadTarget, gpuMipLevel, x, y,
			                          width, height, internalFormat, compressedSize, pic);
		}
#else
		glCompressedTexSubImage2D(uploadTarget, mipLevel, x, y,
            width, height, internalFormat, compressedSize, pic);
#endif
	}
	else
	{
		int unpackAlignment = width * BitsForFormat((textureFormat_t)opts.format) / 8;
		if ((unpackAlignment & 3) == 0)
		{
			glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
		}
		else
		{
			glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
		}

		glTexSubImage2D(uploadTarget, gpuMipLevel, x, y, width, height, dataFormat, dataType, pic);
	}

	if (opts.format == FMT_RGB565)
	{
#ifndef ANDROID
		glPixelStorei(GL_UNPACK_SWAP_BYTES, GL_FALSE);
#endif
	}
	if (pixelPitch != 0)
	{
		glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
	}
}

/*
========================
idImage::SetSamplerState
========================
*/
void idImage::SetSamplerState( textureFilter_t tf, textureRepeat_t tr )
{
	if( tf == filter && tr == repeat )
	{
		return;
	}
	filter = tf;
	repeat = tr;
	if (!glConfig.directStateAccess) {
		glBindTexture((opts.textureType == TT_CUBIC) ? GL_TEXTURE_CUBE_MAP : GL_TEXTURE_2D, texnum);
	}
	SetTexParameters();

}

/*
========================
idImage::SetPixel
========================
*/
void idImage::SetPixel( int mipLevel, int x, int y, const void* data, int dataSize )
{
	SubImageUpload( mipLevel,0, x, y, 0, 1, 1, data, 0  );
}

/*
========================
idImage::SetTexParameters
========================
*/

void idImage::SetTexParametersLegacy() {
	int target = GL_TEXTURE_2D;
	switch (opts.textureType)
	{
	case TT_2D:
		target = GL_TEXTURE_2D;
		break;
	case TT_CUBIC:
		target = GL_TEXTURE_CUBE_MAP;
		break;
		// RB begin
	case TT_2D_ARRAY:
		target = GL_TEXTURE_2D_ARRAY;
		break;
	case TT_2D_MULTISAMPLE:
		//target = GL_TEXTURE_2D_MULTISAMPLE;
		//break;
		// no texture parameters for MSAA FBO textures
		return;
		// RB end
	default:
		idLib::FatalError("%s: bad texture type %d", GetName(), opts.textureType);
		return;
	}
	// ALPHA, LUMINANCE, LUMINANCE_ALPHA, and INTENSITY have been removed
		// in OpenGL 3.2. In order to mimic those modes, we use the swizzle operators
	if (opts.colorFormat == CFM_GREEN_ALPHA)
	{
		glTexParameteri(target, GL_TEXTURE_SWIZZLE_R, GL_ONE);
		glTexParameteri(target, GL_TEXTURE_SWIZZLE_G, GL_ONE);
		glTexParameteri(target, GL_TEXTURE_SWIZZLE_B, GL_ONE);
		glTexParameteri(target, GL_TEXTURE_SWIZZLE_A, GL_GREEN);
	}
	else if (opts.format == FMT_LUM8)
	{
		glTexParameteri(target, GL_TEXTURE_SWIZZLE_R, GL_RED);
		glTexParameteri(target, GL_TEXTURE_SWIZZLE_G, GL_RED);
		glTexParameteri(target, GL_TEXTURE_SWIZZLE_B, GL_RED);
		glTexParameteri(target, GL_TEXTURE_SWIZZLE_A, GL_ONE);
	}
	else if (opts.format == FMT_L8A8)
	{
		glTexParameteri(target, GL_TEXTURE_SWIZZLE_R, GL_RED);
		glTexParameteri(target, GL_TEXTURE_SWIZZLE_G, GL_RED);
		glTexParameteri(target, GL_TEXTURE_SWIZZLE_B, GL_RED);
		glTexParameteri(target, GL_TEXTURE_SWIZZLE_A, GL_GREEN);
	}
	else if (opts.format == FMT_ALPHA)
	{
		glTexParameteri(target, GL_TEXTURE_SWIZZLE_R, GL_ONE);
		glTexParameteri(target, GL_TEXTURE_SWIZZLE_G, GL_ONE);
		glTexParameteri(target, GL_TEXTURE_SWIZZLE_B, GL_ONE);
		glTexParameteri(target, GL_TEXTURE_SWIZZLE_A, GL_RED);
	}
	else if (opts.format == FMT_INT8)
	{
		glTexParameteri(target, GL_TEXTURE_SWIZZLE_R, GL_RED);
		glTexParameteri(target, GL_TEXTURE_SWIZZLE_G, GL_RED);
		glTexParameteri(target, GL_TEXTURE_SWIZZLE_B, GL_RED);
		glTexParameteri(target, GL_TEXTURE_SWIZZLE_A, GL_RED);
	}
	else
	{
		glTexParameteri(target, GL_TEXTURE_SWIZZLE_R, GL_RED);
		glTexParameteri(target, GL_TEXTURE_SWIZZLE_G, GL_GREEN);
		glTexParameteri(target, GL_TEXTURE_SWIZZLE_B, GL_BLUE);
		glTexParameteri(target, GL_TEXTURE_SWIZZLE_A, GL_ALPHA);
	}

	switch (filter)
	{
	case TF_DEFAULT:
		if (r_useTrilinearFiltering.GetBool())
		{
			glTexParameterf(target, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
		}
		else
		{
			glTexParameterf(target, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_NEAREST);
		}
		glTexParameterf(target, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		break;
	case TF_LINEAR:
		glTexParameterf(target, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameterf(target, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		break;
	case TF_NEAREST:
	case TF_NEAREST_MIPMAP:
		glTexParameterf(target, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glTexParameterf(target, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		break;
	default:
		common->FatalError("%s: bad texture filter %d", GetName(), filter);
	}

	if (glConfig.anisotropicFilterAvailable)
	{
		// only do aniso filtering on mip mapped images
		if (filter == TF_DEFAULT)
		{
			int aniso = r_maxAnisotropicFiltering.GetInteger();
			if (aniso > glConfig.maxTextureAnisotropy)
			{
				aniso = glConfig.maxTextureAnisotropy;
			}
			if (aniso < 0)
			{
				aniso = 0;
			}
			glTexParameterf(target, GL_TEXTURE_MAX_ANISOTROPY_EXT, aniso);
		}
		else
		{
			glTexParameterf(target, GL_TEXTURE_MAX_ANISOTROPY_EXT, 1);
		}
	}

	// RB: disabled use of unreliable extension that can make the game look worse
	/*
	if( glConfig.textureLODBiasAvailable && ( usage != TD_FONT ) )
	{
		// use a blurring LOD bias in combination with high anisotropy to fix our aliasing grate textures...
		glTexParameterf( target, GL_TEXTURE_LOD_BIAS_EXT, 0.5 ); //r_lodBias.GetFloat() );
	}
	*/
	// RB end

	// set the wrap/clamp modes
	switch (repeat)
	{
	case TR_REPEAT:
		glTexParameterf(target, GL_TEXTURE_WRAP_S, GL_REPEAT);
		glTexParameterf(target, GL_TEXTURE_WRAP_T, GL_REPEAT);
		break;
	case TR_CLAMP_TO_ZERO:
	{
#ifndef ANDROID
		float color[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
		glTexParameterfv(target, GL_TEXTURE_BORDER_COLOR, color);
		glTexParameterf(target, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
		glTexParameterf(target, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
#else
		glTexParameterf( target, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
		glTexParameterf( target, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
#endif
	}
	break;
	case TR_CLAMP_TO_ZERO_ALPHA:
	{
#ifndef ANDROID
		float color[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
		glTexParameterfv(target, GL_TEXTURE_BORDER_COLOR, color);
		glTexParameterf(target, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
		glTexParameterf(target, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
#else
		glTexParameterf( target, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
		glTexParameterf( target, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
#endif
	}
	break;
	case TR_CLAMP:
		glTexParameterf(target, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameterf(target, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		break;
	default:
		common->FatalError("%s: bad texture repeat %d", GetName(), repeat);
	}

	// RB: added shadow compare parameters for shadow map textures
	if (opts.format == FMT_SHADOW_ARRAY)
	{
		//glTexParameteri( target, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
#ifndef ANDROID
		glTexParameteri(target, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_R_TO_TEXTURE);
#else
        glTexParameteri(target, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
#endif
		glTexParameteri(target, GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL);
	}
}

void idImage::SetTextureParameters() {
	if (opts.textureType == TT_2D_MULTISAMPLE) {
		return;
	}
#ifndef ANDROID
	// ALPHA, LUMINANCE, LUMINANCE_ALPHA, and INTENSITY have been removed
	// in OpenGL 3.2. In order to mimic those modes, we use the swizzle operators
	if (opts.colorFormat == CFM_GREEN_ALPHA) {
		glTextureParameteri(texnum, GL_TEXTURE_SWIZZLE_R, GL_ONE);
		glTextureParameteri(texnum, GL_TEXTURE_SWIZZLE_G, GL_ONE);
		glTextureParameteri(texnum, GL_TEXTURE_SWIZZLE_B, GL_ONE);
		glTextureParameteri(texnum, GL_TEXTURE_SWIZZLE_A, GL_GREEN);
	}
	else {
		switch (opts.format) {
		case FMT_ALPHA:
			glTextureParameteri(texnum, GL_TEXTURE_SWIZZLE_R, GL_ONE);
			glTextureParameteri(texnum, GL_TEXTURE_SWIZZLE_G, GL_ONE);
			glTextureParameteri(texnum, GL_TEXTURE_SWIZZLE_B, GL_ONE);
			glTextureParameteri(texnum, GL_TEXTURE_SWIZZLE_A, GL_RED);
			break;
		case FMT_L8A8:
			glTextureParameteri(texnum, GL_TEXTURE_SWIZZLE_R, GL_RED);
			glTextureParameteri(texnum, GL_TEXTURE_SWIZZLE_G, GL_RED);
			glTextureParameteri(texnum, GL_TEXTURE_SWIZZLE_B, GL_RED);
			glTextureParameteri(texnum, GL_TEXTURE_SWIZZLE_A, GL_GREEN);
			break;
		case FMT_LUM8:
			glTextureParameteri(texnum, GL_TEXTURE_SWIZZLE_R, GL_RED);
			glTextureParameteri(texnum, GL_TEXTURE_SWIZZLE_G, GL_RED);
			glTextureParameteri(texnum, GL_TEXTURE_SWIZZLE_B, GL_RED);
			glTextureParameteri(texnum, GL_TEXTURE_SWIZZLE_A, GL_ONE);
			break;
		case FMT_INT8:
			glTextureParameteri(texnum, GL_TEXTURE_SWIZZLE_R, GL_RED);
			glTextureParameteri(texnum, GL_TEXTURE_SWIZZLE_G, GL_RED);
			glTextureParameteri(texnum, GL_TEXTURE_SWIZZLE_B, GL_RED);
			glTextureParameteri(texnum, GL_TEXTURE_SWIZZLE_A, GL_RED);
			break;
		default:
			glTextureParameteri(texnum, GL_TEXTURE_SWIZZLE_R, GL_RED);
			glTextureParameteri(texnum, GL_TEXTURE_SWIZZLE_G, GL_GREEN);
			glTextureParameteri(texnum, GL_TEXTURE_SWIZZLE_B, GL_BLUE);
			glTextureParameteri(texnum, GL_TEXTURE_SWIZZLE_A, GL_ALPHA);
		}
	}

	switch (filter)
	{
	case TF_DEFAULT:
		if (r_useTrilinearFiltering.GetBool())
		{
			glTextureParameterf(texnum, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
		}
		else
		{
			glTextureParameterf(texnum, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_NEAREST);
		}
		glTextureParameterf(texnum, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		break;
	case TF_LINEAR:
		glTextureParameterf(texnum, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTextureParameterf(texnum, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		break;
	case TF_NEAREST:
	case TF_NEAREST_MIPMAP:
		glTextureParameterf(texnum, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glTextureParameterf(texnum, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		break;
	default:
		common->FatalError("%s: bad texture filter %d", GetName(), filter);
	}
#else
/*
========================
 SWIZZLE SETUP
========================
GLES 3.0+ supports swizzle.
We emulate legacy formats (L8A8, ALPHA, LUM, INT) using RGBA swizzle.
*/

	if (opts.colorFormat == CFM_GREEN_ALPHA)
	{
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_R, GL_ONE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_G, GL_ONE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_B, GL_ONE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_A, GL_GREEN);
	}
	else
	{
		switch (opts.format)
		{
			case FMT_ALPHA:
				glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_R, GL_ONE);
				glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_G, GL_ONE);
				glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_B, GL_ONE);
				glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_A, GL_RED);
				break;

			case FMT_L8A8:
				glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_R, GL_RED);
				glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_G, GL_RED);
				glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_B, GL_RED);
				glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_A, GL_GREEN);
				break;

			case FMT_LUM8:
				glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_R, GL_RED);
				glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_G, GL_RED);
				glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_B, GL_RED);
				glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_A, GL_ONE);
				break;

			case FMT_INT8:
				glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_R, GL_RED);
				glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_G, GL_RED);
				glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_B, GL_RED);
				glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_A, GL_RED);
				break;

			default:
				glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_R, GL_RED);
				glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_G, GL_GREEN);
				glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_B, GL_BLUE);
				glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_A, GL_ALPHA);
				break;
		}
	}

/*
========================
 FILTERING SETUP
========================
GLES only supports glTexParameteri (no DSA, no glTextureParameterf)
*/
	switch (filter)
	{
		case TF_DEFAULT:
			if (r_useTrilinearFiltering.GetBool())
			{
				glTexParameteri(GL_TEXTURE_2D,
								GL_TEXTURE_MIN_FILTER,
								GL_LINEAR_MIPMAP_LINEAR);
			}
			else
			{
				glTexParameteri(GL_TEXTURE_2D,
								GL_TEXTURE_MIN_FILTER,
								GL_LINEAR_MIPMAP_NEAREST);
			}

			glTexParameteri(GL_TEXTURE_2D,
							GL_TEXTURE_MAG_FILTER,
							GL_LINEAR);
			break;

		case TF_LINEAR:
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
			break;

		case TF_NEAREST:
		case TF_NEAREST_MIPMAP:
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
			break;

		default:
			common->FatalError("%s: bad texture filter %d", GetName(), filter);
			break;
	}
#endif
	if (glConfig.anisotropicFilterAvailable)
	{
		// only do aniso filtering on mip mapped images
#ifndef ANDROID
		if (filter == TF_DEFAULT)
		{
			int aniso = r_maxAnisotropicFiltering.GetInteger();
			if (aniso > glConfig.maxTextureAnisotropy)
			{
				aniso = glConfig.maxTextureAnisotropy;
			}
			if (aniso < 0)
			{
				aniso = 0;
			}
			glTextureParameterf(texnum, (glConfig.glVersion == 4.6) ? GL_TEXTURE_MAX_ANISOTROPY : GL_TEXTURE_MAX_ANISOTROPY_EXT, aniso);
		}
		else
		{
			glTextureParameterf(texnum, (glConfig.glVersion == 4.6) ? GL_TEXTURE_MAX_ANISOTROPY : GL_TEXTURE_MAX_ANISOTROPY_EXT, 1);
		}
#else
		if (glConfig.anisotropicFilterAvailable)
		{
			const GLenum ANISO_ENUM = GL_TEXTURE_MAX_ANISOTROPY_EXT;
			if (filter == TF_DEFAULT)
			{
				int aniso = r_maxAnisotropicFiltering.GetInteger();

				if (aniso > glConfig.maxTextureAnisotropy)
				{
					aniso = glConfig.maxTextureAnisotropy;
				}

				if (aniso < 1)
				{
					aniso = 1;
				}

				glTexParameterf(GL_TEXTURE_2D, ANISO_ENUM, (GLfloat)aniso);
			}
			else
			{
				glTexParameterf(GL_TEXTURE_2D, ANISO_ENUM, 1.0f);
			}
		}
#endif
	}

	// RB: disabled use of unreliable extension that can make the game look worse
	/*
	if( glConfig.textureLODBiasAvailable && ( usage != TD_FONT ) )
	{
		// use a blurring LOD bias in combination with high anisotropy to fix our aliasing grate textures...
		glTextureParameterf( texnum, (glConfig.glVersion == 4.6) ? GL_TEXTURE_LOD_BIAS : GL_TEXTURE_LOD_BIAS_EXT, r_lodBias.GetFloat() );
	}
	*/
	// RB end

#ifndef ANDROID
	// set the wrap/clamp modes
	switch (repeat)
	{
	case TR_REPEAT:
		glTextureParameterf(texnum, GL_TEXTURE_WRAP_S, GL_REPEAT);
		glTextureParameterf(texnum, GL_TEXTURE_WRAP_T, GL_REPEAT);
		break;
	case TR_CLAMP_TO_ZERO:
	{
		float color[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
		glTextureParameterfv(texnum, GL_TEXTURE_BORDER_COLOR, color);
		glTextureParameterf(texnum, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
		glTextureParameterf(texnum, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
	}
	break;
	case TR_CLAMP_TO_ZERO_ALPHA:
	{
		float color[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
		glTextureParameterfv(texnum, GL_TEXTURE_BORDER_COLOR, color);
		glTextureParameterf(texnum, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
		glTextureParameterf(texnum, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
	}
	break;
	case TR_CLAMP:
		glTextureParameterf(texnum, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTextureParameterf(texnum, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		break;
	default:
		common->FatalError("%s: bad texture repeat %d", GetName(), repeat);
	}

	// RB: added shadow compare parameters for shadow map textures
	if (opts.format == FMT_SHADOW_ARRAY)
	{
		//glTexParameteri( target, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
		glTextureParameteri(texnum, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_R_TO_TEXTURE);
		glTextureParameteri(texnum, GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL);
	}
#else
/*
========================
 WRAP / CLAMP
========================
NOTE:
GLES does NOT support:
- GL_CLAMP_TO_BORDER
- GL_TEXTURE_BORDER_COLOR

So we emulate via CLAMP_TO_EDGE
*/

	switch (repeat)
	{
		case TR_REPEAT:
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
			break;

		case TR_CLAMP_TO_ZERO:
		case TR_CLAMP_TO_ZERO_ALPHA:
			// ❌ no border color in GLES
			// ✔️ fallback
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
			break;

		case TR_CLAMP:
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
			break;

		default:
			common->FatalError("%s: bad texture repeat %d", GetName(), repeat);
			break;
	}

/*
========================
 SHADOW MAP COMPARE
========================
GLES 3.0+ supports shadow samplers
*/

	if (opts.format == FMT_SHADOW_ARRAY)
	{
		glTexParameteri(GL_TEXTURE_2D,
						GL_TEXTURE_COMPARE_MODE,
						GL_COMPARE_REF_TO_TEXTURE);

		glTexParameteri(GL_TEXTURE_2D,
						GL_TEXTURE_COMPARE_FUNC,
						GL_LEQUAL);
	}
#endif
}


void idImage::SetTexParameters()
{
#ifndef ANDROID
	if (!glConfig.directStateAccess) {
		SetTexParametersLegacy();
	}
	else {
		SetTextureParameters();
	}
#else
    SetTexParametersLegacy();
#endif
}

/*
========================
idImage::AllocImage

Every image will pass through this function. Allocates all the necessary MipMap levels for the
Image, but doesn't put anything in them.

This should not be done during normal game-play, if you can avoid it.
========================
*/
void idImage::AllocImage()
{
	//GL_CheckErrors();
	PurgeImage();

	int sRGB = r_useSRGB.GetInteger();

	switch( opts.format )
	{
		case FMT_RGBA8:
			//internalFormat = GL_RGBA8;
			//internalFormat = ( glConfig.sRGBFramebufferAvailable && ( sRGB == 1 || sRGB == 3 ) ) ? GL_SRGB8_ALPHA8 : GL_RGBA8;
#ifndef ANDROID
			internalFormat = ( glConfig.sRGBFramebufferAvailable && ( sRGB == 1 || sRGB == 3 ) ) ? GL_SRGB8_ALPHA8 : GL_RGBA8;
#else
			internalFormat = GL_RGBA8;
#endif
			dataFormat = GL_RGBA;
			dataType = GL_UNSIGNED_BYTE;
			break;
		case FMT_XRGB8:
#ifndef ANDROID
			internalFormat = ( glConfig.sRGBFramebufferAvailable && ( sRGB == 1 || sRGB == 3 ) ) ? GL_SRGB8 : GL_RGB8;
#else
			internalFormat = GL_RGBA8;
#endif
			dataFormat = GL_RGBA;
			dataType = GL_UNSIGNED_BYTE;
			break;
		case FMT_RGB565:
			//internalFormat = ( glConfig.sRGBFramebufferAvailable && ( sRGB == 1 || sRGB == 3 ) ) ? GL_SRGB : GL_RGB;
#ifndef ANDROID
            internalFormat = glConfig.directStateAccess ? GL_RGB8 : GL_RGB;
#else
            internalFormat = GL_RGB565;
#endif
			dataFormat = GL_RGB;
			dataType = GL_UNSIGNED_SHORT_5_6_5;
			break;
		case FMT_ALPHA:
#ifndef ANDROID
#if 1
			if( ( glConfig.sRGBFramebufferAvailable && ( sRGB == 1 || sRGB == 3 ) ) )
			{
				internalFormat = glConfig.directStateAccess? GL_SRGB8 : GL_SRGB;
				dataFormat = GL_RED;
			}
			else
#endif
#endif
			{
				internalFormat = GL_R8;
				dataFormat = GL_RED;
			}
			dataType = GL_UNSIGNED_BYTE;
			break;
		case FMT_L8A8:
			internalFormat = GL_RG8;
			dataFormat = GL_RG;
			dataType = GL_UNSIGNED_BYTE;
			break;
        case FMT_LUM8:
        case FMT_R32F:
		case FMT_INT8:
			internalFormat = GL_R8;
			dataFormat = GL_RED;
			dataType = GL_UNSIGNED_BYTE;
			break;
		case FMT_DXT1:
#ifndef ANDROID
			internalFormat = ( glConfig.sRGBFramebufferAvailable && ( sRGB == 1 || sRGB == 3 ) ) ? GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT1_EXT : GL_COMPRESSED_RGBA_S3TC_DXT1_EXT;
#else
			internalFormat = !glConfig.textureCompressionAvailable ? GL_COMPRESSED_RGBA8_ETC2_EAC : GL_COMPRESSED_RGBA_S3TC_DXT1_EXT;
#endif
			dataFormat = GL_RGBA;
			dataType = GL_UNSIGNED_BYTE;
			break;
		case FMT_DXT5:
#ifndef ANDROID
			internalFormat = ( glConfig.sRGBFramebufferAvailable && ( sRGB == 1 || sRGB == 3 ) && opts.colorFormat != CFM_YCOCG_DXT5 && opts.colorFormat != CFM_NORMAL_DXT5 ) ? GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT5_EXT : GL_COMPRESSED_RGBA_S3TC_DXT5_EXT;
#else
			internalFormat = !glConfig.textureCompressionAvailable ? GL_COMPRESSED_RGBA8_ETC2_EAC : GL_COMPRESSED_RGBA_S3TC_DXT5_EXT;
#endif
			//internalFormat = GL_COMPRESSED_RGBA_S3TC_DXT5_EXT;
			dataFormat = GL_RGBA;
			dataType = GL_UNSIGNED_BYTE;
			break;
		case FMT_DEPTH:
#ifndef ANDROID
			internalFormat = glConfig.directStateAccess? GL_DEPTH_COMPONENT24 : GL_DEPTH_COMPONENT;
#else
			internalFormat = GL_DEPTH_COMPONENT16;
#endif
			dataFormat = GL_DEPTH_COMPONENT;
#ifndef ANDROID
			dataType = GL_UNSIGNED_BYTE;
#else
            dataType = GL_UNSIGNED_SHORT;
#endif
			break;

		//SP Begin
		case FMT_DEPTH_STENCIL:
			internalFormat = GL_DEPTH24_STENCIL8;
			dataFormat = GL_DEPTH_STENCIL;
			dataType = GL_UNSIGNED_INT_24_8;
			break;
		//SP End

		case FMT_SHADOW_ARRAY:
#ifndef ANDROID
			internalFormat = glConfig.directStateAccess? GL_DEPTH_COMPONENT24 : GL_DEPTH_COMPONENT;
#else
			internalFormat = GL_DEPTH_COMPONENT24;
#endif
			dataFormat = GL_DEPTH_COMPONENT;
#ifndef ANDROID
			dataType = GL_UNSIGNED_BYTE;
#else
            dataType = GL_UNSIGNED_INT;
#endif
			break;

        case FMT_RGBA32F:
		case FMT_RGBA16F:
			internalFormat = GL_RGBA16F;
			dataFormat = GL_RGBA;
#ifndef ANDROID
			dataType = GL_UNSIGNED_BYTE;
#else
			dataType = GL_HALF_FLOAT;
#endif
			break;
		case FMT_X16:
			internalFormat = GL_INTENSITY16;
			dataFormat = GL_LUMINANCE;
			dataType = GL_UNSIGNED_SHORT;
			break;
		case FMT_Y16_X16:
			internalFormat = GL_LUMINANCE16_ALPHA16;
			dataFormat = GL_LUMINANCE_ALPHA;
			dataType = GL_UNSIGNED_SHORT;
			break;
		default:
			idLib::Error( "Unhandled image format %d in %s\n", opts.format, GetName() );
	}

	// if we don't have a rendering context, just return after we
	// have filled in the parms.  We must have the values set, or
	// an image match from a shader before OpenGL starts would miss
	// the generated texture
	if( !tr.IsInitialized() )
	{
		return;
	}

	//----------------------------------------------------
	// allocate all the mip levels with NULL data
	//----------------------------------------------------

	int numSides;
	int target;
	int uploadTarget;
	switch (opts.textureType) {
		case TT_2D:
			target = uploadTarget = GL_TEXTURE_2D;
			numSides = 1;
			break;
		case TT_CUBIC:
			target = GL_TEXTURE_CUBE_MAP;
			uploadTarget = GL_TEXTURE_CUBE_MAP_POSITIVE_X;
			numSides = 6;
			break;
			//RB Begin
		case TT_2D_ARRAY:
			target = GL_TEXTURE_2D_ARRAY;
			uploadTarget = GL_TEXTURE_2D_ARRAY;
			numSides = 6;
			break;
		case TT_2D_MULTISAMPLE:
			target = GL_TEXTURE_2D_MULTISAMPLE;
			uploadTarget = GL_TEXTURE_2D_MULTISAMPLE;
			numSides = 1;
			break;
			//RB End
		default:
			assert(!"opts.textureType");
			target = uploadTarget = GL_TEXTURE_2D;
			numSides = 1;
	}
#ifndef _WIN32
	int w = opts.width > 0 ? opts.width : 1280;
	int h = opts.height > 0 ? opts.height : 720;
#else
	int w = opts.width;
	int h = opts.height;
#endif
#ifndef ANDROID
	if (!glConfig.directStateAccess)
#endif
	{
		glGenTextures(1, (GLuint*)&texnum);
		assert(texnum != TEXTURE_NOT_LOADED);
		glBindTexture(target, texnum);
		if (opts.textureType == TT_2D_ARRAY) {
#ifndef ANDROID
			glTexImage3D(uploadTarget, 0, internalFormat, w, h, numSides, 0, dataFormat, GL_UNSIGNED_BYTE, NULL);
#else
			glTexImage3D( uploadTarget, 0, internalFormat, opts.width, opts.height, numSides, 0, dataFormat, internalFormat == GL_DEPTH_COMPONENT24 ? GL_UNSIGNED_INT : GL_FLOAT, NULL );
#endif
		}
        else if (opts.textureType == TT_2D_MULTISAMPLE) {
#ifndef ANDROID
			glTexImage2DMultisample(uploadTarget, opts.samples, internalFormat, w, h, GL_FALSE);
#else
			if (glTexStorage2DMultisample!= nullptr) {
				glTexStorage2DMultisample(
						GL_TEXTURE_2D_MULTISAMPLE,
						opts.samples,
						internalFormat,
						w, h,
						GL_FALSE
				);
			}
#endif
		}else{
			if (opts.textureType == TT_CUBIC)
			{
				h = w;
			}
            int mipSkip = getMipmapSkipLevel(opts.numLevels);
            int effectiveNumLevels = opts.numLevels - mipSkip;
            if (effectiveNumLevels < 1) effectiveNumLevels = 1;

            for (int side = 0; side < numSides; side++)
            {
#ifndef _WIN32
                int baseW = opts.width > 0 ? opts.width : 1280;
                int baseH = opts.height > 0 ? opts.height : 720;
#else
                int baseW = opts.width;
			    int baseH = opts.height;
#endif

                int w = baseW >> mipSkip;
                int h = baseH >> mipSkip;
                if (w < 1) w = 1;
                if (h < 1) h = 1;

                for (int level = 0; level < effectiveNumLevels; level++)
                {
                    if (IsCompressed())
                    {
                        int compressedSize = (((w + 3) / 4) * ((h + 3) / 4) * int64(16) * BitsForFormat(opts.format)) / 8;

#if defined(_WIN32)
                        void* data = HeapAlloc(GetProcessHeap(), 0, compressedSize);
			            glCompressedTexImage2D(uploadTarget + side, level, internalFormat, w, h, 0, compressedSize, data);
            			if (data != NULL) HeapFree(GetProcessHeap(), 0, data);

#elif ANDROID
                        if (!glConfig.textureCompressionAvailable) {
                            w = (w + 3) & ~3;
                            h = (h + 3) & ~3;
                            const int etc2CompressedSize = ((w / 4) * (h / 4)) * 16;
                            byte *data = (byte *) Mem_Alloc(etc2CompressedSize, TAG_TEMP);

                            glCompressedTexImage2D(uploadTarget + side, level, GL_COMPRESSED_RGBA8_ETC2_EAC, w, h, 0, etc2CompressedSize, data);
                            if (data != nullptr) Mem_Free(data);
                        } else {
                            byte* data = (byte*)Mem_Alloc(compressedSize, TAG_TEMP);
                            glCompressedTexImage2D(uploadTarget + side, level, internalFormat, w, h, 0, compressedSize, data);
                            if (data != nullptr) Mem_Free(data);
                        }
#else
                        byte* data = (byte*)Mem_Alloc(compressedSize, TAG_TEMP);
			            glCompressedTexImage2D(uploadTarget + side, level, internalFormat, w, h, 0, compressedSize, data);
            			if (data != NULL) Mem_Free(data);
#endif
                    }
                    else
                    {
                        glTexImage2D(uploadTarget + side, level, internalFormat, w, h, 0, dataFormat, dataType, NULL);
                    }

                    w = Max(1, w >> 1);
                    h = Max(1, h >> 1);
                }
            }

            glTexParameteri(target, GL_TEXTURE_MAX_LEVEL, effectiveNumLevels - 1);
		}
	}
#ifndef ANDROID
	else
	{
		glCreateTextures(target, 1, (GLuint*)&texnum);
		if (texnum != TEXTURE_NOT_LOADED) {
			if (opts.textureType == TT_2D_ARRAY) {
				glTextureStorage3D(texnum, opts.numLevels, internalFormat, w, h, numSides);
			} else if (opts.textureType == TT_2D_MULTISAMPLE) {
				glTextureStorage2DMultisample(texnum, opts.samples, internalFormat, w, h, GL_FALSE);
		} else {

				if (opts.textureType == TT_CUBIC) {
					h = w;
				}
				glTextureStorage2D(texnum, opts.numLevels, internalFormat, w, h);
			}
			glTextureParameteri(texnum, GL_TEXTURE_MAX_LEVEL, opts.numLevels - 1);
		}
	}
#endif
	// see if we messed anything up
	//GL_CheckErrors();

	SetTexParameters();
}

/*
========================
idImage::PurgeImage
========================
*/
void idImage::PurgeImage()
{
	if( texnum != TEXTURE_NOT_LOADED )
	{
		glDeleteTextures( 1, ( GLuint* )&texnum );	// this should be the ONLY place it is ever called!
		texnum = TEXTURE_NOT_LOADED;
	}
	
	// clear all the current binding caches, so the next bind will do a real one
	for( int i = 0 ; i < MAX_MULTITEXTURE_UNITS ; i++ )
	{
		glcontext.tmu[i].current2DMap = TEXTURE_NOT_LOADED;
		glcontext.tmu[i].current2DArray = TEXTURE_NOT_LOADED;
		glcontext.tmu[i].currentCubeMap = TEXTURE_NOT_LOADED;
	}
}

/*
========================
idImage::Resize
========================
*/
void idImage::Resize( int width, int height, bool recalculateLevels )
{
	if( opts.width == width && opts.height == height )
	{
		return;
	}
	opts.width = width;
	opts.height = height;
	if (recalculateLevels) {
		opts.numLevels = 0;
		DeriveOpts();
	}
	AllocImage();
}
