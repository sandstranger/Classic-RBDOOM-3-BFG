/*
===========================================================================

Doom 3 BFG Edition GPL Source Code
Copyright (C) 2014-2018 Robert Beckebans

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
#pragma hdrstop

#include "../RenderCommon.h"
#include "../Framebuffer.h"

#if !defined(USE_VULKAN)

/*
================================================================================================

RENDER TARGETS

Desktop GL allocates every render target in Init, as it always did.

Android allocates on demand. Init only creates the framebuffer objects, which have no storage.
The images of optional features are lazy render targets (idImageManager::LazyRenderTargetFromFunction):
their generator describes them, nothing is allocated until the feature runs and calls its
Framebuffer::Ensure*Targets, which allocates the images at their current size and attaches them.
UpdateOptionalTargets, once per frame after the back end, frees the targets of a feature that was
turned off (and the shadow maps that weren't used for a while), so toggling a feature or changing
the resolution never leaves storage behind and never allocates during normal frames.

	feature						targets											lifetime
	HDR							_currentRenderHDR, _currentDepth -> hdrFBO		the 3D views of a frame
	HDR exposure				_currentRenderHDR64 -> hdr64FBO					blit, read back by a PBO
	HDR bloom					_bloomRender0/1 -> bloomRenderFBO				Bloom -> BloomComposite of a view
	SSAO / SSGI filtering		_ao0/1 -> ambientOcclusionFBO, quarter size		SSAO of a view
	SSAO / SSGI					_cszBuffer mips -> csDepthFBO					SSAO of a view
	SSAO / SSGI					_currentNormals -> geometryBufferFBO			ambient pass -> SSAO of a view
	SMAA						_smaaEdges, _smaaBlend -> smaa*FBO				PostProcess
	shadow mapping				_shadowAtlas -> shadowAtlasFBO					ShadowAtlasPass -> the views of a frame
	shadow mapping				_shadowCache -> shadowCacheFBO					static casters, kept between frames
	shadow mapping				_shadowMapArray0..4 -> shadowFBO[]				lights that don't fit in the atlas

_bloomRender0/1 and _ao0/1, _currentNormals and _smaaEdges are never in use at the same time and
share their textures through the alias keys of the RenderTargetPool, see renderTargetAlias_t.

================================================================================================
*/

static idCVar r_postProcessScale("r_postProcessScale", "0.25", CVAR_FLOAT | CVAR_ARCHIVE,
                                 "Resolution scale for post-process effects (bloom, SSAO). Range: 0.25 to 1.0");
static idCVar r_useTileBasedOptimizations("r_useTileBasedOptimizations", "1",
        CVAR_RENDERER | CVAR_BOOL | CVAR_ARCHIVE,
        "Use framebuffer invalidation for tile-based mobile GPUs");
static idCVar r_renderTargetIdleFrames( "r_renderTargetIdleFrames", "600", CVAR_RENDERER | CVAR_INTEGER,
        "frames a per light shadow map array or a shadow atlas keeps its storage after its last use", 1, 100000 );

const int shadowMapResolutions[MAX_SHADOWMAP_RESOLUTIONS] = { 512, 256, 256, 128, 64 };

idList<Framebuffer*> Framebuffer::framebuffers;
RenderTargetPool Framebuffer::renderTargetPool;
Framebuffer* Framebuffer::currentBoundFramebuffer = nullptr;
globalFramebuffers_t globalFramebuffers;

#if defined(ANDROID)
extern idCVar r_shadowAtlasSize;
extern idCVar r_shadowCacheSize;
#endif

// frames since start up, for the ages of the pool entries and of the optional targets
static int s_renderTargetFrame = 0;

// how many attachments are counted as invalidated: their contents are dropped, the storage stays
int r_framebufferInvalidations = 0;

/*
================================================================================================

FORMATS

================================================================================================
*/

int R_InternalFormatBytes( GLenum internalFormat )
{
	switch( internalFormat )
	{
		case GL_R8:
			return 1;
		case GL_RG8:
		case GL_R16F:
		case GL_DEPTH_COMPONENT16:
		case GL_RGB565:
			return 2;
		case GL_RGBA16F:
			return 8;
		case GL_RGBA32F:
			return 16;
		// GL_RGB8 and GL_DEPTH_COMPONENT24 are padded to 32 bits by the hardware we know of
		default:
			return 4;
	}
}

const char* R_InternalFormatName( GLenum internalFormat )
{
	switch( internalFormat )
	{
		case GL_R8:
			return "R8";
		case GL_RG8:
			return "RG8";
		case GL_R16F:
			return "R16F";
		case GL_RGB565:
			return "RGB565";
		case GL_RGB8:
			return "RGB8";
		case GL_RGBA8:
			return "RGBA8";
		case GL_RGB10_A2:
			return "RGB10_A2";
		case GL_RGBA16F:
			return "RGBA16F";
		case GL_RGBA32F:
			return "RGBA32F";
		case GL_DEPTH_COMPONENT16:
			return "D16";
		case GL_DEPTH_COMPONENT24:
			return "D24";
		case GL_DEPTH24_STENCIL8:
			return "D24S8";
		case GL_DEPTH_COMPONENT32F:
			return "D32F";
		default:
			return "?";
	}
}

/*
================================================================================================

RENDER TARGET POOL, see Framebuffer.h

================================================================================================
*/

RenderTargetPool::RenderTargetPool() : _currentFrame( 0 ), _initialized( false ) {}
RenderTargetPool::~RenderTargetPool()
{
	// the context is gone at exit, Shutdown ran before it was
	_pool.clear();
}

void RenderTargetPool::Initialize( const PoolConfig& config )
{
	if( _initialized )
	{
		return;
	}
	_config = config;
	_pool.reserve( config.preallocateCount );
	_initialized = true;
	common->Printf( "RenderTargetPool initialized (limit %zu, idle textures live %d frames)\n", _config.maxPoolSize, _config.maxFrameAge );
}

int64_t RenderTargetPool::TextureBytes( const RenderTargetDesc& desc )
{
	int64_t bytes = 0;
	int w = Max( 1, desc.width );
	int h = Max( 1, desc.height );
	for( int level = 0; level < Max( 1, desc.levels ); level++ )
	{
		bytes += ( int64_t )w * h * R_InternalFormatBytes( desc.internalFormat );
		w = Max( 1, w >> 1 );
		h = Max( 1, h >> 1 );
	}
	return bytes * Max( 1, desc.layers ) * Max( 1, desc.samples );
}

void RenderTargetPool::RemoveOwner( RenderTarget& rt, const std::string& name )
{
	for( size_t i = 0; i < rt.owners.size(); i++ )
	{
		if( rt.owners[i] == name )
		{
			rt.owners.erase( rt.owners.begin() + i );
			return;
		}
	}
}

GLuint RenderTargetPool::Acquire( const std::string& name, const RenderTargetDesc& desc, int aliasKey )
{
	if( !_initialized )
	{
		PoolConfig config;
		Initialize( config );
	}
	if( name.empty() || desc.width <= 0 || desc.height <= 0 || desc.levels <= 0 || desc.target != GL_TEXTURE_2D || desc.layers != 1 )
	{
		// only single layer 2D textures are pooled, arrays and cube maps belong to their image
		return 0;
	}

	// an alias of a texture another target of the key holds: their passes never overlap
	if( aliasKey != RT_ALIAS_NONE )
	{
		for( RenderTarget& rt : _pool )
		{
			if( rt.textureID != 0 && rt.aliasKey == aliasKey && rt.logicalRefs > 0 && CanReuse( rt, desc ) )
			{
				rt.logicalRefs++;
				rt.owners.push_back( name );
				rt.lastUsedFrame = _currentFrame;
				_stats.aliasHits++;
				return rt.textureID;
			}
		}
	}

	// a texture of the description nobody holds. Without an alias key it must not be attached
	// anywhere either, a stale attachment would draw into the new owner's storage. Attachments
	// of an aliased texture belong to the framebuffers of its key, which re-attach before use.
	for( RenderTarget& rt : _pool )
	{
		if( rt.textureID != 0 && rt.logicalRefs == 0 && rt.aliasKey == aliasKey && CanReuse( rt, desc )
				&& ( rt.attachmentRefs == 0 || aliasKey != RT_ALIAS_NONE ) )
		{
			rt.logicalRefs = 1;
			rt.owners.clear();
			rt.owners.push_back( name );
			rt.lastUsedFrame = _currentFrame;
			_stats.cacheHits++;
			return rt.textureID;
		}
	}

	// make room among the textures nobody holds, the oldest first
	while( _pool.size() >= _config.maxPoolSize && _config.enableLRUEviction )
	{
		size_t candidate = _pool.size();
		int oldest = 0x7fffffff;
		for( size_t i = 0; i < _pool.size(); i++ )
		{
			const RenderTarget& rt = _pool[i];
			if( rt.logicalRefs == 0 && rt.attachmentRefs == 0 && rt.lastUsedFrame < oldest )
			{
				candidate = i;
				oldest = rt.lastUsedFrame;
			}
		}
		if( candidate == _pool.size() )
		{
			// every texture is in use, the pool grows: a missing render target is never an option
			common->Warning( "RenderTargetPool: %zu textures in use, over the limit of %zu", _pool.size(), _config.maxPoolSize );
			break;
		}
		DestroyRenderTarget( _pool[candidate] );
		_pool.erase( _pool.begin() + candidate );
		_stats.evictions++;
	}

	RenderTarget rt;
	if( !CreateRenderTarget( desc, name, aliasKey, rt ) )
	{
		_stats.allocationFailures++;
		return 0;
	}
	_stats.cacheMisses++;
	_pool.push_back( rt );
	return rt.textureID;
}

void RenderTargetPool::Release( GLuint textureID, const std::string& name )
{
	for( RenderTarget& rt : _pool )
	{
		if( rt.textureID == textureID )
		{
			if( rt.logicalRefs > 0 )
			{
				rt.logicalRefs--;
			}
			RemoveOwner( rt, name );
			rt.lastUsedFrame = _currentFrame;
			return;
		}
	}
}

bool RenderTargetPool::RetainTexture( GLuint textureID )
{
	if( !textureID )
	{
		return false;
	}
	for( RenderTarget& rt : _pool )
	{
		if( rt.textureID == textureID )
		{
			++rt.attachmentRefs;
			return true;
		}
	}
	return false;
}

void RenderTargetPool::ReleaseAttachmentRef( GLuint textureID )
{
	if( !textureID )
	{
		return;
	}
	for( RenderTarget& rt : _pool )
	{
		if( rt.textureID == textureID )
		{
			if( rt.attachmentRefs > 0 )
			{
				--rt.attachmentRefs;
			}
			rt.lastUsedFrame = _currentFrame;
			return;
		}
	}
}

const RenderTarget* RenderTargetPool::FindTexture( GLuint textureID ) const
{
	for( const RenderTarget& rt : _pool )
	{
		if( rt.textureID == textureID )
		{
			return &rt;
		}
	}
	return nullptr;
}

void RenderTargetPool::BeginFrame()
{
	++_currentFrame;
}

void RenderTargetPool::EndFrame()
{
	if( !_initialized )
	{
		return;
	}

	// textures nobody held for maxFrameAge frames go
	if( _config.enableLRUEviction && _config.maxFrameAge >= 0 )
	{
		for( size_t i = 0; i < _pool.size(); )
		{
			RenderTarget& rt = _pool[i];
			if( rt.logicalRefs == 0 && rt.attachmentRefs == 0 && ( _currentFrame - rt.lastUsedFrame ) > _config.maxFrameAge )
			{
				DestroyRenderTarget( rt );
				_pool.erase( _pool.begin() + i );
				_stats.evictions++;
				continue;
			}
			i++;
		}
	}

	_stats.peakTextures = Max( _stats.peakTextures, ( int )_pool.size() );
	_stats.peakBytes = Max( _stats.peakBytes, GetAllocatedBytes() );
}

void RenderTargetPool::Shutdown()
{
	for( RenderTarget& rt : _pool )
	{
		DestroyRenderTarget( rt );
	}
	_pool.clear();
	_currentFrame = 0;
	_initialized = false;
}

size_t RenderTargetPool::GetActiveCount() const
{
	size_t n = 0;
	for( const RenderTarget& rt : _pool )
	{
		if( rt.logicalRefs > 0 )
		{
			++n;
		}
	}
	return n;
}

int64_t RenderTargetPool::GetAllocatedBytes() const
{
	int64_t bytes = 0;
	for( const RenderTarget& rt : _pool )
	{
		bytes += rt.bytes;
	}
	return bytes;
}

int64_t RenderTargetPool::GetIdleBytes() const
{
	int64_t bytes = 0;
	for( const RenderTarget& rt : _pool )
	{
		if( rt.logicalRefs == 0 )
		{
			bytes += rt.bytes;
		}
	}
	return bytes;
}

bool RenderTargetPool::CreateRenderTarget( const RenderTargetDesc& desc, const std::string& name, int aliasKey, RenderTarget& rt )
{
	rt = RenderTarget();
	rt.desc = desc;
	rt.aliasKey = aliasKey;
	rt.owners.push_back( name );
	rt.logicalRefs = 1;
	rt.lastUsedFrame = _currentFrame;
	rt.bytes = TextureBytes( desc );

	// earlier errors must not fail the allocation
	for( int i = 0; i < 8 && glGetError() != GL_NO_ERROR; i++ )
	{
	}

	glGenTextures( 1, &rt.textureID );
	if( !rt.textureID )
	{
		return false;
	}
	R_DirectBindTexture( GL_TEXTURE_2D, rt.textureID );
	glTexStorage2D( GL_TEXTURE_2D, desc.levels, desc.internalFormat, desc.width, desc.height );
	if( glGetError() != GL_NO_ERROR )
	{
		common->Warning( "RenderTargetPool: no storage for %s (%d x %d %s)", name.c_str(), desc.width, desc.height, R_InternalFormatName( desc.internalFormat ) );
		R_DirectDeleteTextures( 1, &rt.textureID );
		rt.textureID = 0;
		return false;
	}
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, desc.levels - 1 );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, ( desc.isDepth || !desc.linearFilter ) ? GL_NEAREST : GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, ( desc.isDepth || !desc.linearFilter ) ? GL_NEAREST : GL_LINEAR );

	_stats.allocations++;
	return true;
}

bool RenderTargetPool::CanReuse( const RenderTarget& rt, const RenderTargetDesc& desc ) const
{
	return rt.textureID != 0 && rt.desc == desc;
}

void RenderTargetPool::DestroyRenderTarget( RenderTarget& rt )
{
	if( rt.textureID )
	{
		R_DirectDeleteTextures( 1, &rt.textureID );
		rt.textureID = 0;
		_stats.deallocations++;
	}
	rt.attachmentRefs = 0;
	rt.logicalRefs = 0;
}

void RenderTargetPool::Print() const
{
	common->Printf( "Render target pool: %zu textures (%zu held), %.2f MiB, %.2f MiB cached idle\n", _pool.size(), GetActiveCount(),
					GetAllocatedBytes() / ( 1024.0f * 1024.0f ), GetIdleBytes() / ( 1024.0f * 1024.0f ) );
	for( const RenderTarget& rt : _pool )
	{
		idStr owners;
		for( const std::string& owner : rt.owners )
		{
			if( owners.Length() )
			{
				owners += ", ";
			}
			owners += owner.c_str();
		}
		common->Printf( "  tex %4u %5d x %-5d %-8s mips %2d %8.2f MiB  images %d  attachments %d  alias %d  idle %d frames  [%s]\n",
						rt.textureID, rt.desc.width, rt.desc.height, R_InternalFormatName( rt.desc.internalFormat ), rt.desc.levels,
						rt.bytes / ( 1024.0f * 1024.0f ), rt.logicalRefs, rt.attachmentRefs, rt.aliasKey,
						rt.logicalRefs > 0 ? 0 : _currentFrame - rt.lastUsedFrame, owners.c_str() );
	}
	common->Printf( "  hits %d, misses %d, alias hits %d, allocations %d, deallocations %d, evictions %d, failures %d, peak %d textures %.2f MiB\n",
					_stats.cacheHits, _stats.cacheMisses, _stats.aliasHits, _stats.allocations, _stats.deallocations, _stats.evictions,
					_stats.allocationFailures, _stats.peakTextures, _stats.peakBytes / ( 1024.0f * 1024.0f ) );
}

/*
================================================================================================

FRAMEBUFFER

================================================================================================
*/

static void R_ListFramebuffers_f( const idCmdArgs& args )
{
	if( !glConfig.framebufferObjectAvailable )
	{
		common->Printf( "GL_EXT_framebuffer_object is not available.\n" );
		return;
	}

	Framebuffer::PrintRenderTargets( args.Argc() > 1 && idStr::Icmp( args.Argv( 1 ), "-v" ) == 0 );
}

Framebuffer::Framebuffer( const char* name, int w, int h )
{
	fboName = name;
	width = w;
	height = h;

	glGenFramebuffers( 1, &frameBuffer );

	framebuffers.Append( this );
}

Framebuffer::~Framebuffer()
{
	PurgeFramebuffer();
}

#if defined(ANDROID)
/*
================================================================================================

ANDROID: OPTIONAL RENDER TARGETS ON DEMAND

================================================================================================
*/

enum optionalTargetGroup_t
{
	RTG_SCENE,
	RTG_EXPOSURE,
	RTG_BLOOM,
	RTG_AMBIENT_OCCLUSION,
	RTG_HIERARCHICAL_DEPTH,
	RTG_GEOMETRY_BUFFER,
	RTG_SMAA,
	RTG_SHADOW_ATLAS,
	RTG_SHADOW_CACHE,
	RTG_SHADOW_LOD0,
	RTG_NUM = RTG_SHADOW_LOD0 + MAX_SHADOWMAP_RESOLUTIONS
};

struct optionalTargetState_t
{
	bool	allocated;
	int		lastUsedFrame;
};

static optionalTargetState_t s_targetGroups[RTG_NUM];

static void R_ScreenSize( int& w, int& h )
{
	w = renderSystem->GetWidth() > 0 ? renderSystem->GetWidth() : 1280;
	h = renderSystem->GetHeight() > 0 ? renderSystem->GetHeight() : 720;
}

static void R_MarkTargetGroup( optionalTargetGroup_t group )
{
	s_targetGroups[group].allocated = true;
	s_targetGroups[group].lastUsedFrame = s_renderTargetFrame;
}

// attaches without disturbing what is bound: the Ensure functions run between passes
class idScopedFramebufferBind
{
public:
	idScopedFramebufferBind( Framebuffer* fbo ) : previous( Framebuffer::currentBoundFramebuffer )
	{
		fbo->Bind();
	}
	~idScopedFramebufferBind()
	{
		if( previous != nullptr )
		{
			previous->Bind();
		}
		else
		{
			Framebuffer::Unbind();
		}
	}
private:
	Framebuffer* previous;
};

static void R_AttachColor( Framebuffer* fbo, idImage* image, int width, int height, int mipLevel = 0 )
{
	if( fbo->GetColorAttachment( 0 ) == image->texnum && fbo->GetWidth() == width && fbo->GetHeight() == height )
	{
		return;
	}
	idScopedFramebufferBind bind( fbo );
	fbo->AttachImage2D( GL_TEXTURE_2D, image, 0, mipLevel );
	fbo->Resize( width, height );
	fbo->Check();
}

static void R_ReleaseImage( idImage* image )
{
	if( image != NULL && image->IsLoaded() )
	{
		image->ReleaseStorage();
	}
}

static void R_DetachFramebuffer( Framebuffer* fbo )
{
	if( fbo != NULL && fbo->GetNumAttachments() > 0 )
	{
		fbo->DetachAll();
	}
}

static void R_ReleaseTargetGroup( optionalTargetGroup_t group )
{
	if( !s_targetGroups[group].allocated )
	{
		return;
	}
	s_targetGroups[group].allocated = false;

	// detach first, the textures go back to the pool without attachment references
	switch( group )
	{
		case RTG_SCENE:
			R_DetachFramebuffer( globalFramebuffers.hdrFBO );
			R_DetachFramebuffer( globalFramebuffers.geometryBufferFBO );
			R_ReleaseImage( globalImages->currentRenderHDRImage );
			R_ReleaseImage( globalImages->currentDepthImage );
			break;
		case RTG_EXPOSURE:
			R_DetachFramebuffer( globalFramebuffers.hdr64FBO );
			R_ReleaseImage( globalImages->currentRenderHDRImage64 );
			break;
		case RTG_BLOOM:
			for( int i = 0; i < MAX_BLOOM_BUFFERS; i++ )
			{
				R_DetachFramebuffer( globalFramebuffers.bloomRenderFBO[i] );
				R_ReleaseImage( globalImages->bloomRenderImage[i] );
			}
			break;
		case RTG_AMBIENT_OCCLUSION:
			for( int i = 0; i < MAX_SSAO_BUFFERS; i++ )
			{
				R_DetachFramebuffer( globalFramebuffers.ambientOcclusionFBO[i] );
				R_ReleaseImage( globalImages->ambientOcclusionImage[i] );
			}
			break;
		case RTG_HIERARCHICAL_DEPTH:
			for( int i = 0; i < MAX_HIERARCHICAL_ZBUFFERS; i++ )
			{
				R_DetachFramebuffer( globalFramebuffers.csDepthFBO[i] );
			}
			R_ReleaseImage( globalImages->hierarchicalZbufferImage );
			break;
		case RTG_GEOMETRY_BUFFER:
			R_DetachFramebuffer( globalFramebuffers.geometryBufferFBO );
			R_ReleaseImage( globalImages->currentNormalsImage );
			break;
		case RTG_SMAA:
			R_DetachFramebuffer( globalFramebuffers.smaaEdgesFBO );
			R_DetachFramebuffer( globalFramebuffers.smaaBlendFBO );
			R_ReleaseImage( globalImages->smaaEdgesImage );
			R_ReleaseImage( globalImages->smaaBlendImage );
			// the screen copy SMAA reads, CopyFramebuffer makes it again
			R_ReleaseImage( globalImages->smaaInputImage );
			break;
		case RTG_SHADOW_ATLAS:
			R_DetachFramebuffer( globalFramebuffers.shadowAtlasFBO );
			R_ReleaseImage( globalImages->shadowAtlasImage );
			break;
		case RTG_SHADOW_CACHE:
			R_DetachFramebuffer( globalFramebuffers.shadowCacheFBO );
			R_ReleaseImage( globalImages->shadowCacheImage );
			break;
		default:
		{
			const int lod = group - RTG_SHADOW_LOD0;
			R_DetachFramebuffer( globalFramebuffers.shadowFBO[lod] );
			R_ReleaseImage( globalImages->shadowImage[lod] );
			break;
		}
	}
}

bool Framebuffer::EnsureSceneTargets()
{
	int w, h;
	R_ScreenSize( w, h );

	idImage* color = globalImages->currentRenderHDRImage;
	idImage* depth = globalImages->currentDepthImage;
	color->Resize( w, h );
	depth->Resize( w, h );
	if( !color->EnsureAllocated() || !depth->EnsureAllocated() )
	{
		return false;
	}

	Framebuffer* fbo = globalFramebuffers.hdrFBO;
	if( fbo->GetColorAttachment( 0 ) != color->texnum || fbo->GetDepthAttachment() != depth->texnum || fbo->GetWidth() != w || fbo->GetHeight() != h )
	{
		idScopedFramebufferBind bind( fbo );
		fbo->AttachImage2D( GL_TEXTURE_2D, color, 0 );
		fbo->AttachImageDepth( GL_TEXTURE_2D, depth );
		fbo->Resize( w, h );
		fbo->Check();
	}
	R_MarkTargetGroup( RTG_SCENE );
	return true;
}

bool Framebuffer::EnsureExposureTargets()
{
	idImage* image = globalImages->currentRenderHDRImage64;
	if( !image->EnsureAllocated() )
	{
		return false;
	}
	R_AttachColor( globalFramebuffers.hdr64FBO, image, 64, 64 );
	R_MarkTargetGroup( RTG_EXPOSURE );
	return true;
}

bool Framebuffer::EnsureBloomTargets()
{
	int w, h;
	R_ScreenSize( w, h );
	const float scale = idMath::ClampFloat( 0.1f, 1.0f, r_postProcessScale.GetFloat() );
	const int bloomWidth = Max( 1, idMath::Ftoi( w * scale ) );
	const int bloomHeight = Max( 1, idMath::Ftoi( h * scale ) );

	for( int i = 0; i < MAX_BLOOM_BUFFERS; i++ )
	{
		idImage* image = globalImages->bloomRenderImage[i];
		image->Resize( bloomWidth, bloomHeight );
		if( !image->EnsureAllocated() )
		{
			return false;
		}
		R_AttachColor( globalFramebuffers.bloomRenderFBO[i], image, bloomWidth, bloomHeight );
	}
	R_MarkTargetGroup( RTG_BLOOM );
	return true;
}

bool Framebuffer::EnsureAmbientOcclusionTargets()
{
	int w, h;
	R_ScreenSize( w, h );
	const int aoWidth = Max( 1, w / 4 );
	const int aoHeight = Max( 1, h / 4 );

	for( int i = 0; i < MAX_SSAO_BUFFERS; i++ )
	{
		idImage* image = globalImages->ambientOcclusionImage[i];
		image->Resize( aoWidth, aoHeight );
		if( !image->EnsureAllocated() )
		{
			return false;
		}
		R_AttachColor( globalFramebuffers.ambientOcclusionFBO[i], image, aoWidth, aoHeight );
	}
	R_MarkTargetGroup( RTG_AMBIENT_OCCLUSION );
	return true;
}

bool Framebuffer::EnsureHierarchicalDepthTargets()
{
	int w, h;
	R_ScreenSize( w, h );

	idImage* image = globalImages->hierarchicalZbufferImage;
	image->Resize( w, h, true );
	if( !image->EnsureAllocated() )
	{
		return false;
	}
	for( int i = 0; i < MAX_HIERARCHICAL_ZBUFFERS; i++ )
	{
		R_AttachColor( globalFramebuffers.csDepthFBO[i], image, Max( 1, w >> i ), Max( 1, h >> i ), i );
	}
	R_MarkTargetGroup( RTG_HIERARCHICAL_DEPTH );
	return true;
}

bool Framebuffer::EnsureGeometryBufferTargets()
{
	// the depth test of the normals pass needs the scene depth
	if( !EnsureSceneTargets() )
	{
		return false;
	}

	int w, h;
	R_ScreenSize( w, h );
	idImage* normals = globalImages->currentNormalsImage;
	idImage* depth = globalImages->currentDepthImage;
	normals->Resize( w, h );
	if( !normals->EnsureAllocated() )
	{
		return false;
	}

	Framebuffer* fbo = globalFramebuffers.geometryBufferFBO;
	if( fbo->GetColorAttachment( 0 ) != normals->texnum || fbo->GetDepthAttachment() != depth->texnum || fbo->GetWidth() != w || fbo->GetHeight() != h )
	{
		idScopedFramebufferBind bind( fbo );
		fbo->AttachImage2D( GL_TEXTURE_2D, normals, 0 );
		fbo->AttachImageDepth( GL_TEXTURE_2D, depth );
		fbo->Resize( w, h );
		fbo->Check();
	}
	R_MarkTargetGroup( RTG_GEOMETRY_BUFFER );
	return true;
}

bool Framebuffer::EnsureSMAATargets()
{
	int w, h;
	R_ScreenSize( w, h );

	idImage* edges = globalImages->smaaEdgesImage;
	idImage* blend = globalImages->smaaBlendImage;
	edges->Resize( w, h );
	blend->Resize( w, h );
	if( !edges->EnsureAllocated() || !blend->EnsureAllocated() )
	{
		return false;
	}
	R_AttachColor( globalFramebuffers.smaaEdgesFBO, edges, w, h );
	R_AttachColor( globalFramebuffers.smaaBlendFBO, blend, w, h );
	R_MarkTargetGroup( RTG_SMAA );
	return true;
}

bool Framebuffer::EnsureShadowMapTargets( int lod )
{
	if( lod < 0 || lod >= MAX_SHADOWMAP_RESOLUTIONS )
	{
		return false;
	}
	// ShadowMapPass attaches the layer of every side itself
	if( !globalImages->shadowImage[lod]->EnsureAllocated() )
	{
		return false;
	}
	R_MarkTargetGroup( ( optionalTargetGroup_t )( RTG_SHADOW_LOD0 + lod ) );
	return true;
}

static bool R_EnsureShadowAtlasImage( idImage* image, Framebuffer* fbo, int size, optionalTargetGroup_t group )
{
	// new storage is attached again even under the same texture name: GL hands a deleted name out
	// again, and a framebuffer the texture was deleted from while it wasn't bound keeps the old one
	const bool hadStorage = image->IsLoaded();
	image->Resize( size, size );
	if( !image->EnsureAllocated() )
	{
		return false;
	}
	if( !hadStorage || fbo->GetDepthAttachment() != image->texnum || fbo->GetWidth() != size )
	{
		idScopedFramebufferBind bind( fbo );
		fbo->AttachImageDepthLayer( image, 0 );
		fbo->Resize( size, size );
		fbo->Check();
	}
	R_MarkTargetGroup( group );
	return true;
}

bool Framebuffer::EnsureShadowAtlasTarget( int size )
{
	return R_EnsureShadowAtlasImage( globalImages->shadowAtlasImage, globalFramebuffers.shadowAtlasFBO, size, RTG_SHADOW_ATLAS );
}

bool Framebuffer::EnsureShadowCacheTarget( int size )
{
	return R_EnsureShadowAtlasImage( globalImages->shadowCacheImage, globalFramebuffers.shadowCacheFBO, size, RTG_SHADOW_CACHE );
}

void Framebuffer::ReleaseShadowCacheTarget()
{
	R_ReleaseTargetGroup( RTG_SHADOW_CACHE );
}

/*
==================
Framebuffer::UpdateOptionalTargets

Frees the targets of the features that are off. The shadow maps also go when no light used them
for r_renderTargetIdleFrames frames. Runs between frames, nothing of the next frame is recorded yet.
==================
*/
void Framebuffer::UpdateOptionalTargets()
{
	const bool ssao = r_useSSAO.GetBool();
	const bool ssgi = r_useSSGI.GetBool();
	const int idleFrames = r_renderTargetIdleFrames.GetInteger();

	// a new depth format for the shadow maps: the next use allocates them with it
	if( r_shadowMapDepth16.IsModified() )
	{
		r_shadowMapDepth16.ClearModified();
		R_ReleaseTargetGroup( RTG_SHADOW_ATLAS );
		R_ReleaseTargetGroup( RTG_SHADOW_CACHE );
		for( int i = 0; i < MAX_SHADOWMAP_RESOLUTIONS; i++ )
		{
			R_ReleaseTargetGroup( ( optionalTargetGroup_t )( RTG_SHADOW_LOD0 + i ) );
		}
	}

	if( !r_useHDR.GetBool() && !ssao && !ssgi )
	{
		R_ReleaseTargetGroup( RTG_SCENE );
	}
	if( !r_useHDR.GetBool() )
	{
		R_ReleaseTargetGroup( RTG_EXPOSURE );
		R_ReleaseTargetGroup( RTG_BLOOM );
	}
	if( !( ssao && r_ssaoFiltering.GetBool() ) && !( ssgi && r_ssgiFiltering.GetBool() ) )
	{
		R_ReleaseTargetGroup( RTG_AMBIENT_OCCLUSION );
	}
	if( !( ( ssao || ssgi ) && r_useHierarchicalDepthBuffer.GetBool() ) )
	{
		R_ReleaseTargetGroup( RTG_HIERARCHICAL_DEPTH );
	}
	if( !ssao && !ssgi )
	{
		R_ReleaseTargetGroup( RTG_GEOMETRY_BUFFER );
	}
	if( r_antiAliasing.GetInteger() != ANTI_ALIASING_SMAA_1X )
	{
		R_ReleaseTargetGroup( RTG_SMAA );
	}

	const bool shadowMapping = r_useShadowMapping.GetBool();
	for( int i = 0; i < MAX_SHADOWMAP_RESOLUTIONS; i++ )
	{
		const optionalTargetGroup_t group = ( optionalTargetGroup_t )( RTG_SHADOW_LOD0 + i );
		if( !shadowMapping || s_renderTargetFrame - s_targetGroups[group].lastUsedFrame > idleFrames )
		{
			R_ReleaseTargetGroup( group );
		}
	}
	if( !shadowMapping || r_shadowAtlasSize.GetInteger() <= 0 || s_renderTargetFrame - s_targetGroups[RTG_SHADOW_ATLAS].lastUsedFrame > idleFrames )
	{
		R_ReleaseTargetGroup( RTG_SHADOW_ATLAS );
	}
	if( !shadowMapping || r_shadowCacheSize.GetInteger() <= 0 || s_renderTargetFrame - s_targetGroups[RTG_SHADOW_CACHE].lastUsedFrame > idleFrames )
	{
		R_ReleaseTargetGroup( RTG_SHADOW_CACHE );
	}
}

/*
==================
Framebuffer::Init

Only the framebuffer objects, nothing is attached: the Ensure functions attach the images when
their features run. No framebuffer gets a throwaway texture of its own any more.
==================
*/
void Framebuffer::Init()
{
	if( !renderTargetPool.IsInitialized() )
	{
		InitializePool();
	}
	cmdSystem->AddCommand( "listFramebuffers", R_ListFramebuffers_f, CMD_FL_RENDERER, "lists the render targets, their storage and the framebuffers, -v for every pool texture" );
	tr.backend.currentFramebuffer = NULL;
	memset( s_targetGroups, 0, sizeof( s_targetGroups ) );

	int screenWidth, screenHeight;
	R_ScreenSize( screenWidth, screenHeight );

	const GLenum noDrawBuffers[] = { GL_NONE };
	for( int i = 0; i < MAX_SHADOWMAP_RESOLUTIONS; i++ )
	{
		globalFramebuffers.shadowFBO[i] = new Framebuffer( va( "_shadowMap%i", i ), shadowMapResolutions[i], shadowMapResolutions[i] );
		globalFramebuffers.shadowFBO[i]->Bind();
		glDrawBuffers( 1, noDrawBuffers );
		glReadBuffer( GL_NONE );
	}

	// depth only targets of the shadow atlas and of the shadow cache, see ShadowAtlasPass
	globalFramebuffers.shadowAtlasFBO = new Framebuffer( "_shadowAtlas", shadowMapResolutions[0], shadowMapResolutions[0] );
	globalFramebuffers.shadowAtlasFBO->Bind();
	glDrawBuffers( 1, noDrawBuffers );
	glReadBuffer( GL_NONE );

	globalFramebuffers.shadowCacheFBO = new Framebuffer( "_shadowCache", shadowMapResolutions[0], shadowMapResolutions[0] );
	globalFramebuffers.shadowCacheFBO->Bind();
	glDrawBuffers( 1, noDrawBuffers );
	glReadBuffer( GL_NONE );

	globalFramebuffers.hdrFBO = new Framebuffer( "_hdr", screenWidth, screenHeight );
	globalFramebuffers.hdr64FBO = new Framebuffer( "_hdr64", 64, 64 );

	for( int i = 0; i < MAX_BLOOM_BUFFERS; i++ )
	{
		globalFramebuffers.bloomRenderFBO[i] = new Framebuffer( va( "_bloomRender%i", i ), screenWidth / 4, screenHeight / 4 );
	}

	// always made: SSAO bound them when r_ssaoFiltering was turned on after start up, while they didn't exist
	for( int i = 0; i < MAX_SSAO_BUFFERS; i++ )
	{
		globalFramebuffers.ambientOcclusionFBO[i] = new Framebuffer( va( "_aoRender%i", i ), screenWidth / 4, screenHeight / 4 );
	}

	for( int i = 0; i < MAX_HIERARCHICAL_ZBUFFERS; i++ )
	{
		globalFramebuffers.csDepthFBO[i] = new Framebuffer( va( "_csz%i", i ), screenWidth / ( 1 << i ), screenHeight / ( 1 << i ) );
	}

	globalFramebuffers.geometryBufferFBO = new Framebuffer( "_gbuffer", screenWidth, screenHeight );
	globalFramebuffers.smaaEdgesFBO = new Framebuffer( "_smaaEdges", screenWidth, screenHeight );
	globalFramebuffers.smaaBlendFBO = new Framebuffer( "_smaaBlend", screenWidth, screenHeight );

	Unbind();
}

/*
==================
Framebuffer::CheckFramebuffers

After every frame: the targets that are allocated follow the screen size the next time their
feature runs (the Ensure functions resize them), the targets of features that are off go.
==================
*/
void Framebuffer::CheckFramebuffers()
{
	UpdateOptionalTargets();
}

#else // !ANDROID

void Framebuffer::Init()
{
    if(!renderTargetPool.IsInitialized()) InitializePool();
    cmdSystem->AddCommand("listFramebuffers", R_ListFramebuffers_f, CMD_FL_RENDERER, "lists framebuffers");
    tr.backend.currentFramebuffer = NULL;
    int width, height;
    width = height = r_shadowMapImageSize.GetInteger();

    for (int i = 0; i < MAX_SHADOWMAP_RESOLUTIONS; i++)
    {
        width = height = shadowMapResolutions[i];

        globalFramebuffers.shadowFBO[i] = new Framebuffer(va("_shadowMap%i", i), width, height);

        if (!glConfig.directStateAccess)
        {
            globalFramebuffers.shadowFBO[i]->Bind();
            glDrawBuffers(0, NULL);
        }
        else
        {
            glNamedFramebufferDrawBuffers(globalFramebuffers.shadowFBO[i]->frameBuffer, 0, NULL);
        }
    }

    // depth only target for the shadow atlas, the back end attaches the atlas image every frame
    globalFramebuffers.shadowAtlasFBO = new Framebuffer("_shadowAtlas", shadowMapResolutions[0], shadowMapResolutions[0]);
    if (!glConfig.directStateAccess)
    {
        globalFramebuffers.shadowAtlasFBO->Bind();
        glDrawBuffers(0, NULL);
    }
    else
    {
        glNamedFramebufferDrawBuffers(globalFramebuffers.shadowAtlasFBO->frameBuffer, 0, NULL);
    }
#ifndef _WIN32
    int screenWidth = renderSystem->GetWidth() > 0 ? renderSystem->GetWidth() : 1280;
    int screenHeight = renderSystem->GetHeight() > 0 ? renderSystem->GetHeight() : 720;
#else
    int screenWidth = renderSystem->GetWidth();
    int screenHeight = renderSystem->GetHeight();
#endif

    const int hdrWidth = screenWidth / 3;
    const int hdrHeight = screenHeight / 3;

    globalFramebuffers.hdrFBO = new Framebuffer("_hdr", hdrWidth, hdrHeight);

    if (!glConfig.directStateAccess)
    {
        globalFramebuffers.hdrFBO->Bind();
    }

#if defined(USE_HDR_MSAA)
    if (glConfig.multisamples)
    {
        globalFramebuffers.hdrFBO->AddColorBuffer(GL_RGBA16F, 0, glConfig.multisamples);
        globalFramebuffers.hdrFBO->AddDepthBuffer(GL_DEPTH24_STENCIL8, glConfig.multisamples);

        globalFramebuffers.hdrFBO->AttachImage2D(GL_TEXTURE_2D_MULTISAMPLE, globalImages->currentRenderHDRImage, 0);
        globalFramebuffers.hdrFBO->AttachImageDepth(GL_TEXTURE_2D_MULTISAMPLE, globalImages->currentDepthImage);
    }
    else
#endif
    {
        globalFramebuffers.hdrFBO->AddColorBuffer(GL_RGB10_A2, 0);
        globalFramebuffers.hdrFBO->AddDepthBuffer(GL_DEPTH_COMPONENT24);
        globalFramebuffers.hdrFBO->AttachImage2D(GL_TEXTURE_2D, globalImages->currentRenderHDRImage, 0);
        globalFramebuffers.hdrFBO->AttachImageDepth(GL_TEXTURE_2D, globalImages->currentDepthImage);
    }

    globalFramebuffers.hdrFBO->Check();

#if defined(USE_HDR_MSAA)
    globalFramebuffers.hdrNonMSAAFBO = new Framebuffer("_hdrNoMSAA", screenWidth, screenHeight);
    globalFramebuffers.hdrNonMSAAFBO->Bind();

    globalFramebuffers.hdrNonMSAAFBO->AddColorBuffer(GL_RGBA16F, 0);
    globalFramebuffers.hdrNonMSAAFBO->AttachImage2D(GL_TEXTURE_2D, globalImages->currentRenderHDRImageNoMSAA, 0);

    globalFramebuffers.hdrNonMSAAFBO->Check();
#endif

    globalFramebuffers.hdr64FBO = new Framebuffer("_hdr64", 64, 64);

    if (!glConfig.directStateAccess)
    {
        globalFramebuffers.hdr64FBO->Bind();
    }

    globalFramebuffers.hdr64FBO->AddColorBuffer(GL_RGB10_A2, 0);
    globalFramebuffers.hdr64FBO->AttachImage2D(GL_TEXTURE_2D, globalImages->currentRenderHDRImage64, 0);

    globalFramebuffers.hdr64FBO->Check();

    for (int i = 0; i < MAX_BLOOM_BUFFERS; i++)
    {
        int bloomWidth = screenWidth / 4;
        int bloomHeight = screenHeight / 4;

        globalFramebuffers.bloomRenderFBO[i] = new Framebuffer(va("_bloomRender%i", i), bloomWidth, bloomHeight);

        if (!glConfig.directStateAccess)
        {
            globalFramebuffers.bloomRenderFBO[i]->Bind();
        }

        globalFramebuffers.bloomRenderFBO[i]->AddColorBuffer(GL_RGBA8, 0);
        globalFramebuffers.bloomRenderFBO[i]->AttachImage2D(GL_TEXTURE_2D, globalImages->bloomRenderImage[i], 0);
        globalFramebuffers.bloomRenderFBO[i]->Check();
    }

    if (r_ssaoFiltering.GetBool() || r_ssgiFiltering.GetBool())
    {
        for (int i = 0; i < MAX_SSAO_BUFFERS; i++)
        {
            int ssaoWidth = screenWidth / 4;
            int ssaoHeight = screenHeight / 4;

            globalFramebuffers.ambientOcclusionFBO[i] = new Framebuffer(va("_aoRender%i", i), ssaoWidth, ssaoHeight);

            if (!glConfig.directStateAccess)
            {
                globalFramebuffers.ambientOcclusionFBO[i]->Bind();
            }

            globalFramebuffers.ambientOcclusionFBO[i]->AddColorBuffer(GL_RGBA8, 0);
            globalFramebuffers.ambientOcclusionFBO[i]->AttachImage2D(GL_TEXTURE_2D, globalImages->ambientOcclusionImage[i], 0);
            globalFramebuffers.ambientOcclusionFBO[i]->Check();
        }
    }

    for (int i = 0; i < MAX_HIERARCHICAL_ZBUFFERS; i++)
    {
        globalFramebuffers.csDepthFBO[i] = new Framebuffer(va("_csz%i", i), screenWidth / (1 << i), screenHeight / (1 << i));

        if (!glConfig.directStateAccess)
        {
            globalFramebuffers.csDepthFBO[i]->Bind();
        }

        globalFramebuffers.csDepthFBO[i]->AddColorBuffer(GL_R16F, 0);
        globalFramebuffers.csDepthFBO[i]->AttachImage2D(GL_TEXTURE_2D, globalImages->hierarchicalZbufferImage, 0, i);
        globalFramebuffers.csDepthFBO[i]->Check();
    }

    globalFramebuffers.geometryBufferFBO = new Framebuffer("_gbuffer", screenWidth, screenHeight);

    if (!glConfig.directStateAccess)
    {
        globalFramebuffers.geometryBufferFBO->Bind();
    }

    globalFramebuffers.geometryBufferFBO->AddColorBuffer(GL_RGB10_A2, 0);
    globalFramebuffers.geometryBufferFBO->AddStencilBuffer(GL_DEPTH24_STENCIL8);
    globalFramebuffers.geometryBufferFBO->AttachImage2D(GL_TEXTURE_2D, globalImages->currentNormalsImage, 0);
    globalFramebuffers.geometryBufferFBO->AttachImageDepth(GL_TEXTURE_2D, globalImages->currentDepthImage);
    globalFramebuffers.geometryBufferFBO->Check();

    globalFramebuffers.smaaEdgesFBO = new Framebuffer("_smaaEdges", screenWidth, screenHeight);

    if (!glConfig.directStateAccess)
    {
        globalFramebuffers.smaaEdgesFBO->Bind();
    }

    globalFramebuffers.smaaEdgesFBO->AddColorBuffer(GL_RGBA8, 0);
    globalFramebuffers.smaaEdgesFBO->AttachImage2D(GL_TEXTURE_2D, globalImages->smaaEdgesImage, 0);
    globalFramebuffers.smaaEdgesFBO->Check();

    globalFramebuffers.smaaBlendFBO = new Framebuffer("_smaaBlend", screenWidth, screenHeight);

    if (!glConfig.directStateAccess)
    {
        globalFramebuffers.smaaBlendFBO->Bind();
    }

    globalFramebuffers.smaaBlendFBO->AddColorBuffer(GL_RGBA8, 0);
    globalFramebuffers.smaaBlendFBO->AttachImage2D(GL_TEXTURE_2D, globalImages->smaaBlendImage, 0);
    globalFramebuffers.smaaBlendFBO->Check();

    if (!glConfig.directStateAccess)
    {
        Unbind();
    }
}

void Framebuffer::CheckFramebuffers()
{
    int screenWidth = renderSystem->GetWidth();
    int screenHeight = renderSystem->GetHeight();

    if (globalFramebuffers.hdrFBO->GetWidth() != screenWidth ||
        globalFramebuffers.hdrFBO->GetHeight() != screenHeight)
    {
        Unbind();


        globalImages->currentRenderHDRImage->Resize(screenWidth, screenHeight);
        globalImages->currentDepthImage->Resize(screenWidth, screenHeight);

        globalFramebuffers.hdrFBO->Bind();
        globalFramebuffers.hdrFBO->AttachImage2D(GL_TEXTURE_2D, globalImages->currentRenderHDRImage, 0);
        globalFramebuffers.hdrFBO->AttachImageDepth(GL_TEXTURE_2D, globalImages->currentDepthImage);
        globalFramebuffers.hdrFBO->Check();

        globalFramebuffers.hdrFBO->width = screenWidth;
        globalFramebuffers.hdrFBO->height = screenHeight;


        float postProcessScale = r_postProcessScale.GetFloat();
        int bloomWidth = static_cast<int>(screenWidth * postProcessScale);
        int bloomHeight = static_cast<int>(screenHeight * postProcessScale);

        for (int i = 0; i < MAX_BLOOM_BUFFERS; i++)
        {
            globalImages->bloomRenderImage[i]->Resize(bloomWidth, bloomHeight);

            globalFramebuffers.bloomRenderFBO[i]->width = bloomWidth;
            globalFramebuffers.bloomRenderFBO[i]->height = bloomHeight;

            globalFramebuffers.bloomRenderFBO[i]->Bind();
            globalFramebuffers.bloomRenderFBO[i]->AttachImage2D(GL_TEXTURE_2D, globalImages->bloomRenderImage[i], 0);
            globalFramebuffers.bloomRenderFBO[i]->Check();
        }


        if ((r_ssaoFiltering.GetBool() || r_ssgiFiltering.GetBool()) && globalFramebuffers.ambientOcclusionFBO[0] != NULL)
        {
            int ssaoWidth = screenWidth / 4;
            int ssaoHeight = screenHeight / 4;

            for (int i = 0; i < MAX_SSAO_BUFFERS; i++)
            {
                globalImages->ambientOcclusionImage[i]->Resize(ssaoWidth, ssaoHeight);

                globalFramebuffers.ambientOcclusionFBO[i]->width = ssaoWidth;
                globalFramebuffers.ambientOcclusionFBO[i]->height = ssaoHeight;

                globalFramebuffers.ambientOcclusionFBO[i]->Bind();
                globalFramebuffers.ambientOcclusionFBO[i]->AttachImage2D(GL_TEXTURE_2D, globalImages->ambientOcclusionImage[i], 0);
                globalFramebuffers.ambientOcclusionFBO[i]->Check();
            }
        }


        globalImages->hierarchicalZbufferImage->Resize(screenWidth, screenHeight, true);

        for (int i = 0; i < MAX_HIERARCHICAL_ZBUFFERS; i++)
        {
            globalFramebuffers.csDepthFBO[i]->width = screenWidth / (1 << i);
            globalFramebuffers.csDepthFBO[i]->height = screenHeight / (1 << i);

            globalFramebuffers.csDepthFBO[i]->Bind();
            globalFramebuffers.csDepthFBO[i]->AttachImage2D(GL_TEXTURE_2D, globalImages->hierarchicalZbufferImage, 0, i);
            globalFramebuffers.csDepthFBO[i]->Check();
        }


        globalImages->currentNormalsImage->Resize(screenWidth, screenHeight);

        globalFramebuffers.geometryBufferFBO->width = screenWidth;
        globalFramebuffers.geometryBufferFBO->height = screenHeight;

        globalFramebuffers.geometryBufferFBO->Bind();
        globalFramebuffers.geometryBufferFBO->AttachImage2D(GL_TEXTURE_2D, globalImages->currentNormalsImage, 0);
        globalFramebuffers.geometryBufferFBO->AttachImageDepth(GL_TEXTURE_2D, globalImages->currentDepthImage);
        globalFramebuffers.geometryBufferFBO->Check();


        globalImages->smaaEdgesImage->Resize(screenWidth, screenHeight);

        globalFramebuffers.smaaEdgesFBO->width = screenWidth;
        globalFramebuffers.smaaEdgesFBO->height = screenHeight;

        globalFramebuffers.smaaEdgesFBO->Bind();
        globalFramebuffers.smaaEdgesFBO->AttachImage2D(GL_TEXTURE_2D, globalImages->smaaEdgesImage, 0);
        globalFramebuffers.smaaEdgesFBO->Check();

        globalImages->smaaBlendImage->Resize(screenWidth, screenHeight);

        globalFramebuffers.smaaBlendFBO->width = screenWidth;
        globalFramebuffers.smaaBlendFBO->height = screenHeight;

        globalFramebuffers.smaaBlendFBO->Bind();
        globalFramebuffers.smaaBlendFBO->AttachImage2D(GL_TEXTURE_2D, globalImages->smaaBlendImage, 0);
        globalFramebuffers.smaaBlendFBO->Check();

        Unbind();
    }
}

// the other builds allocate every target in Init
bool Framebuffer::EnsureSceneTargets() { return true; }
bool Framebuffer::EnsureExposureTargets() { return true; }
bool Framebuffer::EnsureBloomTargets() { return true; }
bool Framebuffer::EnsureAmbientOcclusionTargets() { return globalFramebuffers.ambientOcclusionFBO[0] != NULL; }
bool Framebuffer::EnsureHierarchicalDepthTargets() { return true; }
bool Framebuffer::EnsureGeometryBufferTargets() { return true; }
bool Framebuffer::EnsureSMAATargets() { return true; }
bool Framebuffer::EnsureShadowMapTargets( int lod ) { return lod >= 0 && lod < MAX_SHADOWMAP_RESOLUTIONS; }
bool Framebuffer::EnsureShadowAtlasTarget( int size ) { return false; }
bool Framebuffer::EnsureShadowCacheTarget( int size ) { return false; }
void Framebuffer::ReleaseShadowCacheTarget() {}
void Framebuffer::UpdateOptionalTargets() {}

#endif // ANDROID

void Framebuffer::ResizeFramebuffers()
{
	CheckFramebuffers();
}

void Framebuffer::InitializePool()
{
	RenderTargetPool::PoolConfig config;
	config.maxPoolSize = 64;
	config.preallocateCount = 16;	// reserve CPU entries, allocate GPU storage on demand
	config.enableLRUEviction = true;
	config.maxFrameAge = 10;
	renderTargetPool.Initialize( config );
}

void Framebuffer::BeginFrame()
{
	++s_renderTargetFrame;
	renderTargetPool.BeginFrame();
}

void R_UpdateRenderTargetPeaks();

void Framebuffer::EndFrame()
{
	renderTargetPool.EndFrame();
	R_UpdateRenderTargetPeaks();
}

void Framebuffer::Shutdown()
{
	framebuffers.DeleteContents( true );
	renderTargetPool.Shutdown();
	currentBoundFramebuffer = nullptr;
	memset( &globalFramebuffers, 0, sizeof( globalFramebuffers ) );
}

/*
================================================================================================

STATISTICS, listFramebuffers

================================================================================================
*/

struct renderTargetInfo_t
{
	idImage*	image;
	const char*	subsystem;
	const char*	lifetime;
};

static int R_GatherRenderTargets( renderTargetInfo_t* targets, int maxTargets )
{
	int num = 0;
	auto add = [&]( idImage * image, const char* subsystem, const char* lifetime )
	{
		if( image != NULL && num < maxTargets )
		{
			targets[num].image = image;
			targets[num].subsystem = subsystem;
			targets[num].lifetime = lifetime;
			num++;
		}
	};

	add( globalImages->currentRenderHDRImage, "scene", "the 3D views of a frame with HDR" );
	add( globalImages->currentDepthImage, "scene", "the 3D views of a frame with HDR, depth copies" );
	add( globalImages->currentRenderHDRImage64, "hdr", "per HDR view: blit -> luminance readback" );
	add( globalImages->currentRenderHDRImageQuarter, "hdr", "unused, never allocated" );
	for( int i = 0; i < MAX_BLOOM_BUFFERS; i++ )
	{
		add( globalImages->bloomRenderImage[i], "bloom", "per HDR view: Bloom -> BloomComposite, aliased with _ao" );
	}
	for( int i = 0; i < MAX_SSAO_BUFFERS; i++ )
	{
		add( globalImages->ambientOcclusionImage[i], "ssao", "per view: SSAO, aliased with _bloomRender" );
	}
	add( globalImages->hierarchicalZbufferImage, "hzb", "per view: SSAO" );
	add( globalImages->currentNormalsImage, "ssao", "per view: ambient pass -> SSAO, aliased with _smaaEdges" );
	add( globalImages->smaaInputImage, "smaa", "per frame: PostProcess" );
	add( globalImages->smaaEdgesImage, "smaa", "per frame: PostProcess, aliased with _currentNormals" );
	add( globalImages->smaaBlendImage, "smaa", "per frame: PostProcess" );
	add( globalImages->shadowAtlasImage, "shadow atlas", "per frame: ShadowAtlasPass -> views" );
	add( globalImages->shadowCacheImage, "shadow cache", "persistent: static casters of stationary lights" );
	for( int i = 0; i < MAX_SHADOWMAP_RESOLUTIONS; i++ )
	{
		add( globalImages->shadowImage[i], "shadow maps", "per light: lights without an atlas tile" );
	}
	add( globalImages->currentRenderImage, "copies", "_currentRender screen copy" );
	add( globalImages->scratchImage, "copies", "screen copies" );
	add( globalImages->scratchImage2, "copies", "screen copies" );
	add( globalImages->accumImage, "copies", "screen copies" );
	return num;
}

static int R_ImageLayers( const idImage* image )
{
	const idImageOpts& opts = image->GetOpts();
	if( opts.textureType == TT_CUBIC )
	{
		return 6;
	}
	if( opts.textureType == TT_2D_ARRAY )
	{
		// the atlases are one layer, a shadow map array has a layer per cube side
		return ( image == globalImages->shadowAtlasImage || image == globalImages->shadowCacheImage ) ? 1 : 6;
	}
	return 1;
}

static int64_t R_ImageBytes( const idImage* image )
{
	if( !image->IsLoaded() )
	{
		return 0;
	}
	if( image->IsRenderTargetPoolAllocated() )
	{
		const RenderTarget* rt = Framebuffer::renderTargetPool.FindTexture( image->texnum );
		if( rt != nullptr )
		{
			return rt->bytes;
		}
	}
	const idImageOpts& opts = image->GetOpts();
	RenderTargetDesc desc;
	desc.width = opts.width;
	desc.height = opts.height;
	desc.levels = Max( 1, opts.numLevels );
	desc.layers = R_ImageLayers( image );
	desc.samples = opts.textureType == TT_2D_MULTISAMPLE ? ( int )opts.samples : 1;
	desc.internalFormat = image->GetInternalFormat();
	return RenderTargetPool::TextureBytes( desc );
}

struct renderTargetTotals_t
{
	int		textures;
	int		logical;
	int64_t	bytes;
};

static renderTargetTotals_t R_RenderTargetTotals()
{
	renderTargetTotals_t totals = {};
	renderTargetInfo_t targets[64];
	const int numTargets = R_GatherRenderTargets( targets, 64 );

	// aliased images share a texture, count it once
	idList<GLuint> counted;
	for( int i = 0; i < numTargets; i++ )
	{
		const idImage* image = targets[i].image;
		if( !image->IsLoaded() )
		{
			continue;
		}
		totals.logical++;
		if( counted.FindIndex( image->texnum ) != -1 )
		{
			continue;
		}
		counted.Append( image->texnum );
		totals.textures++;
		totals.bytes += R_ImageBytes( image );
	}

	// pool textures nobody holds are still allocated
	totals.bytes += Framebuffer::renderTargetPool.GetIdleBytes();
	return totals;
}

static int		s_peakRenderTargetTextures = 0;
static int		s_peakRenderTargetLogical = 0;
static int64_t	s_peakRenderTargetBytes = 0;

// called from the back end at the end of every frame
void R_UpdateRenderTargetPeaks()
{
	const renderTargetTotals_t totals = R_RenderTargetTotals();
	s_peakRenderTargetTextures = Max( s_peakRenderTargetTextures, totals.textures );
	s_peakRenderTargetLogical = Max( s_peakRenderTargetLogical, totals.logical );
	s_peakRenderTargetBytes = Max( s_peakRenderTargetBytes, totals.bytes );
}

void Framebuffer::PrintRenderTargets( bool verbose )
{
	const float MiB = 1.0f / ( 1024.0f * 1024.0f );

	renderTargetInfo_t targets[64];
	const int numTargets = R_GatherRenderTargets( targets, 64 );

	common->Printf( "Render targets (sizes are logical estimates from the format, not measured driver memory):\n" );
	common->Printf( "  %-26s %-13s %11s %-8s %3s %6s %4s %3s %10s %-10s %s\n", "name", "subsystem", "size", "format", "Bpp", "layers", "mips", "spp", "MiB", "storage", "lifetime" );

	struct subsystemTotal_t
	{
		const char*	name;
		int64_t		bytes;
	};
	subsystemTotal_t subsystems[16];
	int numSubsystems = 0;
	idList<GLuint> counted;

	for( int i = 0; i < numTargets; i++ )
	{
		const idImage* image = targets[i].image;
		const idImageOpts& opts = image->GetOpts();
		const bool loaded = image->IsLoaded();
		const GLuint internalFormat = loaded ? image->GetInternalFormat() : 0;

		const char* storage = "none";
		const RenderTarget* rt = NULL;
		if( loaded )
		{
			storage = image->IsLazyRenderTarget() ? "lazy" : "eager";
			if( image->IsRenderTargetPoolAllocated() )
			{
				rt = renderTargetPool.FindTexture( image->texnum );
				storage = ( rt != NULL && rt->logicalRefs > 1 ) ? "aliased" : "pool";
			}
		}

		int64_t bytes = 0;
		if( loaded && counted.FindIndex( image->texnum ) == -1 )
		{
			counted.Append( image->texnum );
			bytes = R_ImageBytes( image );
		}

		int s = 0;
		for( ; s < numSubsystems && idStr::Cmp( subsystems[s].name, targets[i].subsystem ) != 0; s++ )
		{
		}
		if( s == numSubsystems && numSubsystems < 16 )
		{
			subsystems[numSubsystems].name = targets[i].subsystem;
			subsystems[numSubsystems].bytes = 0;
			numSubsystems++;
		}
		if( s < numSubsystems )
		{
			subsystems[s].bytes += bytes;
		}

		common->Printf( "  %-26s %-13s %5d x %-5d %-8s %3d %6d %4d %3d %10.2f %-10s %s%s\n", image->GetName(), targets[i].subsystem,
						opts.width, opts.height, loaded ? R_InternalFormatName( internalFormat ) : "-", loaded ? R_InternalFormatBytes( internalFormat ) : 0,
						R_ImageLayers( image ), Max( 1, opts.numLevels ), opts.textureType == TT_2D_MULTISAMPLE ? ( int )opts.samples : 1,
						bytes * MiB, storage, targets[i].lifetime, ( loaded && bytes == 0 ) ? " (shares a texture listed above)" : "" );
	}

	common->Printf( "Memory by subsystem:\n" );
	for( int s = 0; s < numSubsystems; s++ )
	{
		common->Printf( "  %-13s %8.2f MiB\n", subsystems[s].name, subsystems[s].bytes * MiB );
	}
	common->Printf( "  %-13s %8.2f MiB\n", "pool idle", renderTargetPool.GetIdleBytes() * MiB );

	const renderTargetTotals_t totals = R_RenderTargetTotals();
	common->Printf( "Physical textures %d, logical render targets %d, %.2f MiB allocated\n",
					totals.textures, totals.logical, totals.bytes * MiB );
	common->Printf( "Peak: %d physical targets, %d logical, %.2f MiB at once\n", s_peakRenderTargetTextures, s_peakRenderTargetLogical, s_peakRenderTargetBytes * MiB );
	common->Printf( "Render target textures allocated %d, freed %d, lazy targets described without storage %d, allocated late by a bind %d\n",
					renderTargetCounters.allocations, renderTargetCounters.deallocations, renderTargetCounters.lazyAllocationsSkipped, renderTargetCounters.lateAllocations );
	common->Printf( "Attachment invalidations (contents dropped, storage kept) %d\n", r_framebufferInvalidations );

	common->Printf( "Framebuffer objects: %d (objects without storage of their own)\n", framebuffers.Num() );
	for( int i = 0; i < framebuffers.Num(); i++ )
	{
		const Framebuffer* fbo = framebuffers[i];
		common->Printf( "  %-16s %5d x %-5d attachments %d (color0 tex %u, depth tex %u)\n", fbo->GetName(), fbo->GetWidth(), fbo->GetHeight(),
						fbo->GetNumAttachments(), fbo->GetColorAttachment( 0 ), fbo->GetDepthAttachment() );
	}

	if( verbose )
	{
		renderTargetPool.Print();
	}
	else
	{
		const RenderTargetPool::Stats& stats = renderTargetPool.GetStats();
		common->Printf( "Pool: %zu textures, hits %d, misses %d, alias hits %d, evictions %d, failures %d (listFramebuffers -v for all)\n",
						renderTargetPool.GetPoolSize(), stats.cacheHits, stats.cacheMisses, stats.aliasHits, stats.evictions, stats.allocationFailures );
	}
}

/*
================================================================================================

FRAMEBUFFER OBJECTS

================================================================================================
*/

void Framebuffer::Bind()
{
    RENDERLOG_PRINTF("Framebuffer::Bind( %s )\n", fboName.c_str());

    if (currentBoundFramebuffer != this)
    {
#if defined(ANDROID) && !defined(USE_VULKAN)
        tr.backend.FlushMultiDrawBatch();
#endif
        glBindFramebuffer(GL_FRAMEBUFFER, frameBuffer);
        currentBoundFramebuffer = this;
#if defined(ANDROID)
        tr.backend.pc.c_framebufferBinds++;
#endif
    }
}

bool Framebuffer::IsBound() const
{
    return (currentBoundFramebuffer == this);
}

bool Framebuffer::InvalidateAttachments(const GLenum* attachments, GLsizei count)
{
#if defined(ANDROID)
    if (!r_useTileBasedOptimizations.GetBool() || attachments == nullptr || count <= 0)
    {
        return false;
    }

    // Call at the beginning of a pass that overwrites the complete attachment.
    // The target is already bound, so this does not add another FBO switch.
    if (glInvalidateFramebuffer != nullptr)
    {
        glInvalidateFramebuffer(GL_FRAMEBUFFER, count, attachments);
        r_framebufferInvalidations += count;
        return true;
    }
    if (GLAD_GL_EXT_discard_framebuffer && glDiscardFramebufferEXT != nullptr)
    {
        glDiscardFramebufferEXT(GL_FRAMEBUFFER, count, attachments);
        r_framebufferInvalidations += count;
        return true;
    }
#else
    (void)attachments;
    (void)count;
#endif
    return false;
}


bool Framebuffer::InvalidateDrawFramebufferAttachments(const GLenum* attachments, GLsizei count)
{
#if defined(ANDROID)
    if (!r_useTileBasedOptimizations.GetBool() || attachments == nullptr || count <= 0)
    {
        return false;
    }

    // Used when READ_FRAMEBUFFER and DRAW_FRAMEBUFFER are bound separately
    // for a full-overwrite blit. Invalidate only the destination; rebinding
    // GL_FRAMEBUFFER here would also change the read target.
    if (glInvalidateFramebuffer != nullptr)
    {
        glInvalidateFramebuffer(GL_DRAW_FRAMEBUFFER, count, attachments);
        r_framebufferInvalidations += count;
        return true;
    }
#else
    (void)attachments;
    (void)count;
#endif
    return false;
}

void Framebuffer::Unbind()
{
    RENDERLOG_PRINTF("Framebuffer::Unbind()\n");

    if (currentBoundFramebuffer != nullptr)
    {
#if defined(ANDROID) && !defined(USE_VULKAN)
        tr.backend.FlushMultiDrawBatch();
#endif
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glBindRenderbuffer(GL_RENDERBUFFER, 0);
        currentBoundFramebuffer = nullptr;
#if defined(ANDROID)
        tr.backend.pc.c_framebufferBinds++;
#endif
    }
}

void Framebuffer::InvalidateDefaultFramebufferAttachments()
{
#if defined(ANDROID)
    if (!r_useTileBasedOptimizations.GetBool())
    {
        return;
    }

    // The default framebuffer's depth and stencil data are never presented.
    // At the end of the frame, discard them instead of storing transient tile data.
    // Unbind any renderer FBO first; do not invalidate the color buffer that is presented.
    Unbind();

    const GLenum attachments[] = { GL_DEPTH, GL_STENCIL };
    if (glInvalidateFramebuffer != nullptr)
    {
        glInvalidateFramebuffer(GL_FRAMEBUFFER, 2, attachments);
        r_framebufferInvalidations += 2;
    }
    else if (GLAD_GL_EXT_discard_framebuffer && glDiscardFramebufferEXT != nullptr)
    {
        glDiscardFramebufferEXT(GL_FRAMEBUFFER, 2, attachments);
        r_framebufferInvalidations += 2;
    }
#endif
}

void Framebuffer::InvalidateDefaultFramebufferForOverwrite()
{
    Unbind();
#if defined(ANDROID)
    if (!r_useTileBasedOptimizations.GetBool())
    {
        return;
    }

    // The next draw covers the whole back buffer, so a tile based GPU
    // must not read the old color, depth or stencil back into its tiles.
    const GLenum attachments[] = { GL_COLOR, GL_DEPTH, GL_STENCIL };
    if (glInvalidateFramebuffer != nullptr)
    {
        glInvalidateFramebuffer(GL_FRAMEBUFFER, 3, attachments);
        r_framebufferInvalidations += 3;
    }
    else if (GLAD_GL_EXT_discard_framebuffer && glDiscardFramebufferEXT != nullptr)
    {
        glDiscardFramebufferEXT(GL_FRAMEBUFFER, 3, attachments);
        r_framebufferInvalidations += 3;
    }
#endif
}

bool Framebuffer::IsDefaultFramebufferActive()
{
    return (currentBoundFramebuffer == nullptr);
}

void Framebuffer::AddColorBuffer(int format, int index, int multiSamples)
{
    if (index < 0 || index >= MAX_COLOR_ATTACHMENTS)
    {
        common->Warning("Framebuffer::AddColorBuffer( %s ): bad index = %i", fboName.c_str(), index);
        return;
    }

    colorFormat = format;

    const bool useMSAA = (multiSamples > 0);
    msaaSamples = useMSAA;
    const bool notCreatedYet = (colorTextures[index] == 0);

    if (notCreatedYet)
    {
        glGenTextures(1, &colorTextures[index]);
    }

    R_DirectBindTexture(GL_TEXTURE_2D, colorTextures[index]);
    if (useMSAA)
    {
        glFramebufferTexture2DMultisampleEXT(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0 + index,GL_TEXTURE_2D,colorTextures[index],0,multiSamples);
    }
    else
    {
        glTexImage2D(GL_TEXTURE_2D,0,format,width,height,0,GL_RGBA,GL_UNSIGNED_BYTE,nullptr);
        glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0 + index,GL_TEXTURE_2D,colorTextures[index],0);

        if (notCreatedYet)
        {
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        }
    }
    TrackColorAttachment(index, colorTextures[index]);
}

void Framebuffer::AddDepthBuffer(int format, int multiSamples)
{
    depthFormat = format;
    const bool useMSAA = (multiSamples > 0);
    msaaSamples = useMSAA;
    const bool notCreatedYet = (depthTexture == 0);

    if (notCreatedYet)
    {
        glGenTextures(1, &depthTexture);
    }

    R_DirectBindTexture(GL_TEXTURE_2D, depthTexture);

    if (useMSAA)
    {
        glFramebufferTexture2DMultisampleEXT(GL_FRAMEBUFFER,GL_DEPTH_ATTACHMENT,GL_TEXTURE_2D,depthTexture,0,multiSamples);
    }
    else
    {
        glTexImage2D(GL_TEXTURE_2D,0,format,width,height,0,GL_DEPTH_COMPONENT,GL_UNSIGNED_SHORT,nullptr);
        glFramebufferTexture2D(GL_FRAMEBUFFER,GL_DEPTH_ATTACHMENT,GL_TEXTURE_2D,depthTexture,0);

        if (notCreatedYet)
        {
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL);
        }
    }
    TrackDepthAttachment(depthTexture, GL_DEPTH_ATTACHMENT);
}

void Framebuffer::AddStencilBuffer(int format, int multiSamples)
{
    stencilFormat = format;

    bool notCreatedYet = stencilBuffer == 0;

    if (notCreatedYet)
    {
        glGenRenderbuffers(1, &stencilBuffer);
    }

    glBindRenderbuffer(GL_RENDERBUFFER, stencilBuffer);

    if (multiSamples > 0)
    {
        glRenderbufferStorageMultisample(GL_RENDERBUFFER, multiSamples, format, width, height);
        msaaSamples = true;
    }
    else
    {
        glRenderbufferStorage(GL_RENDERBUFFER, format, width, height);
    }

    if (notCreatedYet)
    {
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_RENDERBUFFER, stencilBuffer);
    }
}

void Framebuffer::TrackColorAttachment( int index, GLuint texture )
{
	const GLuint previous = attachedColor[index];
	const bool previousRetained = attachedColorRetained[index];

	attachedColor[index] = texture;
	attachedColorRetained[index] = ( texture != 0 ) && renderTargetPool.RetainTexture( texture );

	// after the retain, the same texture attached again keeps its reference
	if( previous != 0 && previousRetained )
	{
		renderTargetPool.ReleaseAttachmentRef( previous );
	}
}

void Framebuffer::TrackDepthAttachment( GLuint texture, GLenum attachmentPoint )
{
	const GLuint previous = attachedDepth;
	const bool previousRetained = attachedDepthRetained;

	attachedDepth = texture;
	attachedDepthPoint = attachmentPoint;
	attachedDepthRetained = ( texture != 0 ) && renderTargetPool.RetainTexture( texture );

	if( previous != 0 && previousRetained )
	{
		renderTargetPool.ReleaseAttachmentRef( previous );
	}
}

int Framebuffer::GetNumAttachments() const
{
	int num = ( attachedDepth != 0 ) ? 1 : 0;
	for( int i = 0; i < MAX_COLOR_ATTACHMENTS; i++ )
	{
		if( attachedColor[i] != 0 )
		{
			num++;
		}
	}
	return num;
}

void Framebuffer::AttachImage2D(int target, const idImage* image, int index, int mipmapLod)
{
    if ((target != GL_TEXTURE_2D) && (target != GL_TEXTURE_2D_MULTISAMPLE) &&
        (target < GL_TEXTURE_CUBE_MAP_POSITIVE_X || target > GL_TEXTURE_CUBE_MAP_NEGATIVE_Z))
    {
        common->Warning("Framebuffer::AttachImage2D( %s ): invalid target", fboName.c_str());
        return;
    }

    if (index < 0 || index >= MAX_COLOR_ATTACHMENTS)
    {
        common->Warning("Framebuffer::AttachImage2D( %s ): bad index = %i", fboName.c_str(), index);
        return;
    }

    if (image->opts.textureType == TT_2D_MULTISAMPLE && glConfig.hasMSAAEXT)
    {
        glFramebufferTexture2DMultisampleEXT(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0 + index,GL_TEXTURE_2D,image->texnum,
                                             mipmapLod,image->opts.samples);
    }
    else
    {
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0 + index, target, image->texnum, mipmapLod);
    }
#if ANDROID
    if (colorTextures[index] != 0 && colorTextures[index] != image->texnum)
    {
        R_DirectDeleteTextures(1, &colorTextures[index]);
        colorTextures[index] = 0;
    }
#endif
    TrackColorAttachment(index, image->texnum);
}

void Framebuffer::AttachImage3D(const idImage* image)
{
    glFramebufferTexture(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, image->texnum, 0);
    TrackColorAttachment(0, image->texnum);
}

void Framebuffer::AttachImageDepth(int target, const idImage* image)
{
    if ((target != GL_TEXTURE_2D) && (target != GL_TEXTURE_2D_MULTISAMPLE))
    {
        common->Warning("Framebuffer::AttachImageDepth( %s ): invalid target", fboName.c_str());
        return;
    }

    if (image->opts.textureType == TT_2D_MULTISAMPLE && glConfig.hasMSAAEXT)
    {
        glFramebufferTexture2DMultisampleEXT(GL_FRAMEBUFFER,GL_DEPTH_STENCIL_ATTACHMENT,GL_TEXTURE_2D,image->texnum,0,
                                             image->opts.samples);
    } else
    {
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, target, image->texnum,
                               0);
    }
#if ANDROID
    // a texture AddDepthBuffer made is superseded by the idImage depth attachment
    if (depthTexture != 0 && depthTexture != image->texnum)
    {
        R_DirectDeleteTextures(1, &depthTexture);
        depthTexture = 0;
    }
#endif
    TrackDepthAttachment(image->texnum, GL_DEPTH_STENCIL_ATTACHMENT);
}

void Framebuffer::AttachImageDepthLayer(const idImage* image, int layer)
{
    glFramebufferTextureLayer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, image->texnum, 0, layer);
    TrackDepthAttachment(image->texnum, GL_DEPTH_ATTACHMENT);
}

void Framebuffer::DetachAll()
{
	const bool wasBound = IsBound();
	Framebuffer* previous = currentBoundFramebuffer;
	Bind();

	for( int i = 0; i < MAX_COLOR_ATTACHMENTS; i++ )
	{
		if( attachedColor[i] != 0 )
		{
			glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0 + i, GL_TEXTURE_2D, 0, 0 );
			TrackColorAttachment( i, 0 );
		}
	}
	if( attachedDepth != 0 )
	{
		glFramebufferTexture2D( GL_FRAMEBUFFER, attachedDepthPoint != 0 ? attachedDepthPoint : GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, 0, 0 );
		TrackDepthAttachment( 0, 0 );
	}

	if( !wasBound )
	{
		if( previous != nullptr )
		{
			previous->Bind();
		}
		else
		{
			Unbind();
		}
	}
}

void Framebuffer::Check()
{
#ifndef ANDROID
    int status;
    int prev = -1;

    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev);
    glBindFramebuffer(GL_FRAMEBUFFER, frameBuffer);

    status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status == GL_FRAMEBUFFER_COMPLETE)
    {
        glBindFramebuffer(GL_FRAMEBUFFER, prev);
        return;
    }

    switch (status)
    {
        case GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT:
            common->Error("Framebuffer::Check( %s ): Framebuffer incomplete, incomplete attachment", fboName.c_str());
            break;

        case GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT:
            common->Error("Framebuffer::Check( %s ): Framebuffer incomplete, missing attachment", fboName.c_str());
            break;
        case GL_FRAMEBUFFER_INCOMPLETE_DRAW_BUFFER:
            common->Error("Framebuffer::Check( %s ): Framebuffer incomplete, missing draw buffer", fboName.c_str());
            break;

        case GL_FRAMEBUFFER_INCOMPLETE_READ_BUFFER:
            common->Error("Framebuffer::Check( %s ): Framebuffer incomplete, missing read buffer", fboName.c_str());
            break;
        case GL_FRAMEBUFFER_INCOMPLETE_LAYER_TARGETS:
            common->Error("Framebuffer::Check( %s ): Framebuffer incomplete, missing layer targets", fboName.c_str());
            break;

        case GL_FRAMEBUFFER_INCOMPLETE_MULTISAMPLE:
            common->Error("Framebuffer::Check( %s ): Framebuffer incomplete, missing multisample", fboName.c_str());
            break;

        case GL_FRAMEBUFFER_UNSUPPORTED:
            common->Error("Framebuffer::Check( %s ): Unsupported framebuffer format", fboName.c_str());
            break;

        default:
            common->Error("Framebuffer::Check( %s ): Unknown error 0x%X", fboName.c_str(), status);
            break;
    }

    glBindFramebuffer(GL_FRAMEBUFFER, prev);
#endif
}

Framebuffer* Framebuffer::Find(const char* name)
{
    for (int i = 0; i < framebuffers.Num(); i++)
    {
        if (framebuffers[i]->fboName.Cmp(name) == 0)
        {
            return framebuffers[i];
        }
    }
    return nullptr;
}

void Framebuffer::PurgeFramebuffer()
{
    if(frameBuffer){glDeleteFramebuffers(1,&frameBuffer);frameBuffer=0;}
    if(currentBoundFramebuffer==this)currentBoundFramebuffer=nullptr;
    for(int i=0;i<MAX_COLOR_ATTACHMENTS;++i)
    {
        if(attachedColor[i]&&attachedColorRetained[i])
        {
            renderTargetPool.ReleaseAttachmentRef(attachedColor[i]);
        }
        attachedColor[i]=0;
        attachedColorRetained[i]=false;
        if(colorTextures[i]){R_DirectDeleteTextures(1,&colorTextures[i]);colorTextures[i]=0;}
    }
    if(attachedDepth&&attachedDepthRetained)
    {
        renderTargetPool.ReleaseAttachmentRef(attachedDepth);
    }
    attachedDepth=0;
    attachedDepthRetained=false;
    if(depthTexture){R_DirectDeleteTextures(1,&depthTexture);depthTexture=0;}
    if(depthBuffer){glDeleteRenderbuffers(1,&depthBuffer);depthBuffer=0;}
    if(stencilBuffer){glDeleteRenderbuffers(1,&stencilBuffer);stencilBuffer=0;}
}

#endif
