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
#if ANDROID
#include "GLES3/gl32.h"
#endif

#if !defined(USE_VULKAN)

static void R_ListFramebuffers_f( const idCmdArgs& args )
{
	if( !glConfig.framebufferObjectAvailable )
	{
		common->Printf( "GL_EXT_framebuffer_object is not available.\n" );
		return;
	}
}

Framebuffer::Framebuffer( const char* name, int w, int h )
{
	fboName = name;
	
	frameBuffer = 0;
	
	memset( colorBuffers, 0, sizeof( colorBuffers ) );
	colorFormat = 0;
	
	depthBuffer = 0;
	depthFormat = 0;
	
	stencilBuffer = 0;
	stencilFormat = 0;
	
	width = w;
	height = h;
	
	msaaSamples = false;
#ifndef ANDROID
	if (!glConfig.directStateAccess)
#endif
	{
		glGenFramebuffers(1, &frameBuffer);
	}
#ifndef ANDROID
	else {
		glCreateFramebuffers(1, &frameBuffer);
	}
#endif
	
	framebuffers.Append( this );
}

Framebuffer::~Framebuffer()
{
	glDeleteFramebuffers( 1, &frameBuffer );
}

void Framebuffer::Init()
{
	cmdSystem->AddCommand( "listFramebuffers", R_ListFramebuffers_f, CMD_FL_RENDERER, "lists framebuffers" );
	
	tr.backend.currentFramebuffer = NULL;
	
	// SHADOWMAPS
	
	int width, height;
	width = height = r_shadowMapImageSize.GetInteger();
	
	for( int i = 0; i < MAX_SHADOWMAP_RESOLUTIONS; i++ )
	{
		width = height = shadowMapResolutions[i];
		
		globalFramebuffers.shadowFBO[i] = new Framebuffer( va( "_shadowMap%i", i ) , width, height );
#ifndef ANDROID
		if (!glConfig.directStateAccess)
#endif
		{
			globalFramebuffers.shadowFBO[i]->Bind();
			glDrawBuffers(0, NULL);
		}
#ifndef ANDROID
		else {
			glNamedFramebufferDrawBuffers(globalFramebuffers.shadowFBO[i]->frameBuffer, 0, NULL);
		}
#endif
	}
	
	// HDR
#ifndef _WIN32
	int screenWidth = renderSystem->GetWidth() > 0 ? renderSystem->GetWidth() : 1280;
	int screenHeight = renderSystem->GetHeight() > 0 ? renderSystem->GetHeight() : 720;
#else
	int screenWidth = renderSystem->GetWidth();
	int screenHeight = renderSystem->GetHeight();
#endif
	
	globalFramebuffers.hdrFBO = new Framebuffer( "_hdr", screenWidth, screenHeight );
	if (!glConfig.directStateAccess) {
		globalFramebuffers.hdrFBO->Bind();
	}
	
#if defined(USE_HDR_MSAA)
	if( glConfig.multisamples )
	{
		globalFramebuffers.hdrFBO->AddColorBuffer( GL_RGBA16F, 0, glConfig.multisamples );
		globalFramebuffers.hdrFBO->AddDepthBuffer( GL_DEPTH24_STENCIL8, glConfig.multisamples );
		
		globalFramebuffers.hdrFBO->AttachImage2D( GL_TEXTURE_2D_MULTISAMPLE, globalImages->currentRenderHDRImage, 0 );
		globalFramebuffers.hdrFBO->AttachImageDepth( GL_TEXTURE_2D_MULTISAMPLE, globalImages->currentDepthImage );
	}
	else
#endif
	{
		globalFramebuffers.hdrFBO->AddColorBuffer( GL_RGBA16F, 0 );
		globalFramebuffers.hdrFBO->AddDepthBuffer( GL_DEPTH24_STENCIL8 );

		globalFramebuffers.hdrFBO->AttachImage2D( GL_TEXTURE_2D, globalImages->currentRenderHDRImage, 0 );
		globalFramebuffers.hdrFBO->AttachImageDepth( GL_TEXTURE_2D, globalImages->currentDepthImage );
	}
	
	globalFramebuffers.hdrFBO->Check();
	
	// HDR no MSAA
#if defined(USE_HDR_MSAA)
	globalFramebuffers.hdrNonMSAAFBO = new Framebuffer( "_hdrNoMSAA", screenWidth, screenHeight );
	globalFramebuffers.hdrNonMSAAFBO->Bind();
	
	globalFramebuffers.hdrNonMSAAFBO->AddColorBuffer( GL_RGBA16F, 0 );
	globalFramebuffers.hdrNonMSAAFBO->AttachImage2D( GL_TEXTURE_2D, globalImages->currentRenderHDRImageNoMSAA, 0 );
	
	globalFramebuffers.hdrNonMSAAFBO->Check();
#endif
	
	// HDR DOWNSCALE
	
	globalFramebuffers.hdr64FBO = new Framebuffer( "_hdr64", 64, 64 );
	if (!glConfig.directStateAccess) {
		globalFramebuffers.hdr64FBO->Bind();
	}
	globalFramebuffers.hdr64FBO->AddColorBuffer( GL_RGBA16F, 0 );
	globalFramebuffers.hdr64FBO->AttachImage2D( GL_TEXTURE_2D, globalImages->currentRenderHDRImage64, 0 );
	
	globalFramebuffers.hdr64FBO->Check();
	
	
	// BLOOM
	
	for( int i = 0; i < MAX_BLOOM_BUFFERS; i++ )
	{
		globalFramebuffers.bloomRenderFBO[i] = new Framebuffer( va( "_bloomRender%i", i ), screenWidth, screenHeight );
		if (!glConfig.directStateAccess) {
			globalFramebuffers.bloomRenderFBO[i]->Bind();
		}
		globalFramebuffers.bloomRenderFBO[i]->AddColorBuffer( GL_RGBA8, 0 );
		globalFramebuffers.bloomRenderFBO[i]->AttachImage2D( GL_TEXTURE_2D, globalImages->bloomRenderImage[i], 0 );
		globalFramebuffers.bloomRenderFBO[i]->Check();
	}
	
	if (r_ssaoFiltering.GetBool() || r_ssgiFiltering.GetBool()) {
		// AMBIENT OCCLUSION

		for (int i = 0; i < MAX_SSAO_BUFFERS; i++)
		{
			globalFramebuffers.ambientOcclusionFBO[i] = new Framebuffer(va("_aoRender%i", i), screenWidth, screenHeight);
			if (!glConfig.directStateAccess) {
				globalFramebuffers.ambientOcclusionFBO[i]->Bind();
			}
			globalFramebuffers.ambientOcclusionFBO[i]->AddColorBuffer(GL_RGBA8, 0);
			globalFramebuffers.ambientOcclusionFBO[i]->AttachImage2D(GL_TEXTURE_2D, globalImages->ambientOcclusionImage[i], 0);
			globalFramebuffers.ambientOcclusionFBO[i]->Check();
		}
	}
	
	// HIERARCHICAL Z BUFFER
	
	for( int i = 0; i < MAX_HIERARCHICAL_ZBUFFERS; i++ )
	{
		globalFramebuffers.csDepthFBO[i] = new Framebuffer( va( "_csz%i", i ), screenWidth / ( 1 << i ), screenHeight / ( 1 << i ) );
		if (!glConfig.directStateAccess) {
			globalFramebuffers.csDepthFBO[i]->Bind();
		}
		globalFramebuffers.csDepthFBO[i]->AddColorBuffer( GL_R32F, 0 );
		globalFramebuffers.csDepthFBO[i]->AttachImage2D( GL_TEXTURE_2D, globalImages->hierarchicalZbufferImage, 0, i );
		globalFramebuffers.csDepthFBO[i]->Check();
	}
	
	// GEOMETRY BUFFER
	
	globalFramebuffers.geometryBufferFBO = new Framebuffer( "_gbuffer", screenWidth, screenHeight );
	if (!glConfig.directStateAccess) {
		globalFramebuffers.geometryBufferFBO->Bind();
	}
	globalFramebuffers.geometryBufferFBO->AddColorBuffer( GL_RGBA16F, 0 );
	globalFramebuffers.geometryBufferFBO->AddStencilBuffer(GL_DEPTH24_STENCIL8);
	globalFramebuffers.geometryBufferFBO->AttachImage2D( GL_TEXTURE_2D, globalImages->currentNormalsImage, 0 );
	globalFramebuffers.geometryBufferFBO->AttachImageDepth(GL_TEXTURE_2D, globalImages->currentDepthImage);
	globalFramebuffers.geometryBufferFBO->Check();
	
	// SMAA
	
	globalFramebuffers.smaaEdgesFBO = new Framebuffer( "_smaaEdges", screenWidth, screenHeight );
	if (!glConfig.directStateAccess) {
		globalFramebuffers.smaaEdgesFBO->Bind();
	}
	globalFramebuffers.smaaEdgesFBO->AddColorBuffer( GL_RGBA8, 0 );
	globalFramebuffers.smaaEdgesFBO->AttachImage2D( GL_TEXTURE_2D, globalImages->smaaEdgesImage, 0 );
	globalFramebuffers.smaaEdgesFBO->Check();
	
	globalFramebuffers.smaaBlendFBO = new Framebuffer( "_smaaBlend", screenWidth, screenHeight );
	if (!glConfig.directStateAccess) {
		globalFramebuffers.smaaBlendFBO->Bind();
	}
	globalFramebuffers.smaaBlendFBO->AddColorBuffer( GL_RGBA8, 0 );
	globalFramebuffers.smaaBlendFBO->AttachImage2D( GL_TEXTURE_2D, globalImages->smaaBlendImage, 0 );
	globalFramebuffers.smaaBlendFBO->Check();
	
	if (!glConfig.directStateAccess) {
		Unbind();
	}
}

void Framebuffer::CheckFramebuffers()
{
	int screenWidth = renderSystem->GetWidth();
	int screenHeight = renderSystem->GetHeight();
	
	if( globalFramebuffers.hdrFBO->GetWidth() != screenWidth || globalFramebuffers.hdrFBO->GetHeight() != screenHeight )
	{
		if (!glConfig.directStateAccess) {
			Unbind();
		}
		
		// HDR
		globalImages->currentRenderHDRImage->Resize( screenWidth, screenHeight );
		globalImages->currentDepthImage->Resize( screenWidth, screenHeight );
		
#if defined(USE_HDR_MSAA)
		if( glConfig.multisamples )
		{
			globalImages->currentRenderHDRImageNoMSAA->Resize( screenWidth, screenHeight );
			
			globalFramebuffers.hdrNonMSAAFBO->Bind();
			globalFramebuffers.hdrNonMSAAFBO->AttachImage2D( GL_TEXTURE_2D, globalImages->currentRenderHDRImageNoMSAA, 0 );
			globalFramebuffers.hdrNonMSAAFBO->Check();
			
			globalFramebuffers.hdrNonMSAAFBO->width = screenWidth;
			globalFramebuffers.hdrNonMSAAFBO->height = screenHeight;
			
			globalFramebuffers.hdrFBO->Bind();
			globalFramebuffers.hdrFBO->AttachImage2D( GL_TEXTURE_2D_MULTISAMPLE, globalImages->currentRenderHDRImage, 0 );
			globalFramebuffers.hdrFBO->AttachImageDepth( GL_TEXTURE_2D_MULTISAMPLE, globalImages->currentDepthImage );
			globalFramebuffers.hdrFBO->Check();
		}
		else
#endif
		{
			if (!glConfig.directStateAccess) {
				globalFramebuffers.hdrFBO->Bind();
			}
			globalFramebuffers.hdrFBO->AttachImage2D( GL_TEXTURE_2D, globalImages->currentRenderHDRImage, 0 );
			globalFramebuffers.hdrFBO->AttachImageDepth( GL_TEXTURE_2D, globalImages->currentDepthImage );
			globalFramebuffers.hdrFBO->Check();
		}
		
		globalFramebuffers.hdrFBO->width = screenWidth;
		globalFramebuffers.hdrFBO->height = screenHeight;
		
		// HDR quarter
		/*
		globalImages->currentRenderHDRImageQuarter->Resize( screenWidth / 4, screenHeight / 4 );
		
		globalFramebuffers.hdrQuarterFBO->Bind();
		globalFramebuffers.hdrQuarterFBO->AttachImage2D( GL_TEXTURE_2D, globalImages->currentRenderHDRImageQuarter, 0 );
		globalFramebuffers.hdrQuarterFBO->Check();
		*/
		
		// BLOOM
		
		for( int i = 0; i < MAX_BLOOM_BUFFERS; i++ )
		{
			globalImages->bloomRenderImage[i]->Resize( screenWidth / 4, screenHeight / 4 );
			
			globalFramebuffers.bloomRenderFBO[i]->width = screenWidth / 4;
			globalFramebuffers.bloomRenderFBO[i]->height = screenHeight / 4;
			
			if (!glConfig.directStateAccess) {
				globalFramebuffers.bloomRenderFBO[i]->Bind();
			}
			globalFramebuffers.bloomRenderFBO[i]->AttachImage2D( GL_TEXTURE_2D, globalImages->bloomRenderImage[i], 0 );
			globalFramebuffers.bloomRenderFBO[i]->Check();
		}
		
		// AMBIENT OCCLUSION
		if (r_ssaoFiltering.GetBool() || r_ssgiFiltering.GetBool()) {
			for (int i = 0; i < MAX_SSAO_BUFFERS; i++)
			{
				globalImages->ambientOcclusionImage[i]->Resize(screenWidth, screenHeight);

				globalFramebuffers.ambientOcclusionFBO[i]->width = screenWidth;
				globalFramebuffers.ambientOcclusionFBO[i]->height = screenHeight;

				if (!glConfig.directStateAccess) {
					globalFramebuffers.ambientOcclusionFBO[i]->Bind();
				}
				globalFramebuffers.ambientOcclusionFBO[i]->AttachImage2D(GL_TEXTURE_2D, globalImages->ambientOcclusionImage[i], 0);
				globalFramebuffers.ambientOcclusionFBO[i]->Check();
			}
		}
		
		// HIERARCHICAL Z BUFFER

		globalImages->hierarchicalZbufferImage->Resize( screenWidth, screenHeight, true ); //GK: Recalculate Also MipMap levels for that Texture
		
		for( int i = 0; i < MAX_HIERARCHICAL_ZBUFFERS; i++ )
		{
			globalFramebuffers.csDepthFBO[i]->width = screenWidth / ( 1 << i );
			globalFramebuffers.csDepthFBO[i]->height = screenHeight / ( 1 << i );
			
			if (!glConfig.directStateAccess) {
				globalFramebuffers.csDepthFBO[i]->Bind();
			}
			globalFramebuffers.csDepthFBO[i]->AttachImage2D( GL_TEXTURE_2D, globalImages->hierarchicalZbufferImage, 0, i );
			globalFramebuffers.csDepthFBO[i]->Check();
		}
		
		// GEOMETRY BUFFER
		
		globalImages->currentNormalsImage->Resize( screenWidth, screenHeight );
		
		globalFramebuffers.geometryBufferFBO->width = screenWidth;
		globalFramebuffers.geometryBufferFBO->height = screenHeight;
		
		if (!glConfig.directStateAccess) {
			globalFramebuffers.geometryBufferFBO->Bind();
		}
		globalFramebuffers.geometryBufferFBO->AttachImage2D( GL_TEXTURE_2D, globalImages->currentNormalsImage, 0 );
		globalFramebuffers.geometryBufferFBO->AttachImageDepth(GL_TEXTURE_2D, globalImages->currentDepthImage);
		globalFramebuffers.geometryBufferFBO->Check();
		
		// SMAA
		
		globalImages->smaaEdgesImage->Resize( screenWidth, screenHeight );
		
		globalFramebuffers.smaaEdgesFBO->width = screenWidth;
		globalFramebuffers.smaaEdgesFBO->height = screenHeight;
		
		if (!glConfig.directStateAccess) {
			globalFramebuffers.smaaEdgesFBO->Bind();
		}
		globalFramebuffers.smaaEdgesFBO->AttachImage2D( GL_TEXTURE_2D, globalImages->smaaEdgesImage, 0 );
		globalFramebuffers.smaaEdgesFBO->Check();
		
		globalImages->smaaBlendImage->Resize( screenWidth, screenHeight );
		
		globalFramebuffers.smaaBlendFBO->width = screenWidth;
		globalFramebuffers.smaaBlendFBO->height = screenHeight;
		
		if (!glConfig.directStateAccess) {
			globalFramebuffers.smaaBlendFBO->Bind();
		}
		globalFramebuffers.smaaBlendFBO->AttachImage2D( GL_TEXTURE_2D, globalImages->smaaBlendImage, 0 );
		globalFramebuffers.smaaBlendFBO->Check();
		
		if (!glConfig.directStateAccess) {
			Unbind();
		}
	}
}

void Framebuffer::Shutdown()
{
	framebuffers.DeleteContents( true );
}

void Framebuffer::Bind()
{
	RENDERLOG_PRINTF( "Framebuffer::Bind( %s )\n", fboName.c_str() );
	
	if( tr.backend.currentFramebuffer != this )
	{
		glBindFramebuffer( GL_FRAMEBUFFER, frameBuffer );
		tr.backend.currentFramebuffer = this;
	}
}

bool Framebuffer::IsBound()
{
	return ( tr.backend.currentFramebuffer == this );
}

void Framebuffer::Unbind()
{
	RENDERLOG_PRINTF( "Framebuffer::Unbind()\n" );
	
	//if(tr.backend.currentFramebuffer != NULL)
	{
		glBindFramebuffer( GL_FRAMEBUFFER, 0 );
		glBindRenderbuffer( GL_RENDERBUFFER, 0 );
		tr.backend.currentFramebuffer = NULL;
	}
}

bool Framebuffer::IsDefaultFramebufferActive()
{
	return ( tr.backend.currentFramebuffer == NULL );
}

void Framebuffer::AddColorBuffer( int format, int index, int multiSamples )
{
	if( index < 0 || index >= glConfig.maxColorAttachments )
	{
		common->Warning( "Framebuffer::AddColorBuffer( %s ): bad index = %i", fboName.c_str(), index );
		return;
	}
	
	colorFormat = format;
	
	bool notCreatedYet = colorBuffers[index] == 0;
#ifndef ANDROID
	if (!glConfig.directStateAccess)
#endif
	{
		if (notCreatedYet)
		{
			glGenRenderbuffers(1, &colorBuffers[index]);
		}

		glBindRenderbuffer(GL_RENDERBUFFER, colorBuffers[index]);

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
			glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0 + index, GL_RENDERBUFFER, colorBuffers[index]);
		}
	}
#ifndef ANDROID
	else {
		if (notCreatedYet)
		{
			glCreateRenderbuffers(1, &colorBuffers[index]);
		}

		if (multiSamples > 0)
		{
			glNamedRenderbufferStorageMultisample(colorBuffers[index], multiSamples, format, width, height);

			msaaSamples = true;
		}
		else
		{
			glNamedRenderbufferStorage(colorBuffers[index], format, width, height);
		}

		if (notCreatedYet)
		{
			glNamedFramebufferRenderbuffer(frameBuffer, GL_COLOR_ATTACHMENT0 + index, GL_RENDERBUFFER, colorBuffers[index]);
		}
	}
#endif
	
	//GL_CheckErrors();
}

void Framebuffer::AddDepthBuffer( int format, int multiSamples )
{
	depthFormat = format;
	
	bool notCreatedYet = depthBuffer == 0;
#ifndef ANDROID
	if (!glConfig.directStateAccess)
#endif
	{
		if (notCreatedYet)
		{
			glGenRenderbuffers(1, &depthBuffer);
		}

		glBindRenderbuffer(GL_RENDERBUFFER, depthBuffer);

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
			glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, depthBuffer);
		}
	}
#ifndef ANDROID
	else {
		if (notCreatedYet)
		{
			glCreateRenderbuffers(1, &depthBuffer);
		}

		if (multiSamples > 0)
		{
			glNamedRenderbufferStorageMultisample(depthBuffer, multiSamples, format, width, height);

			msaaSamples = true;
		}
		else
		{
			glNamedRenderbufferStorage(depthBuffer, format, width, height);
		}

		if (notCreatedYet)
		{
			glNamedFramebufferRenderbuffer(frameBuffer, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, depthBuffer);
		}
	}
#endif
	//GL_CheckErrors();
}

//SP Begin
void Framebuffer::AddStencilBuffer(int format, int multiSamples)
{
	stencilFormat = format;

	bool notCreatedYet = stencilBuffer == 0;
	if (notCreatedYet)
	{
#ifndef ANDROID
		if (glConfig.directStateAccess) {
			glCreateRenderbuffers(1, &stencilBuffer);
		}
		else
#endif
		{
			 glGenRenderbuffers(1, &stencilBuffer);
		}
	}
#ifndef ANDROID
	if (!glConfig.directStateAccess)
#endif
	{
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
#ifndef ANDROID
	else {
		if (multiSamples > 0)
		{
			glNamedRenderbufferStorageMultisample(stencilBuffer, multiSamples, format, width, height);

			msaaSamples = true;
		}
		else
		{
			glNamedRenderbufferStorage(stencilBuffer, format, width, height);
		}

		if (notCreatedYet)
		{
			glNamedFramebufferRenderbuffer(frameBuffer, GL_STENCIL_ATTACHMENT, GL_RENDERBUFFER, stencilBuffer);
		}
	}
#endif
	//GL_CheckErrors();
}
//SP End

void Framebuffer::AttachImage2D( int target, const idImage* image, int index, int mipmapLod )
{
	if( ( target != GL_TEXTURE_2D ) && ( target != GL_TEXTURE_2D_MULTISAMPLE ) && ( target < GL_TEXTURE_CUBE_MAP_POSITIVE_X || target > GL_TEXTURE_CUBE_MAP_NEGATIVE_Z ) )
	{
		common->Warning( "Framebuffer::AttachImage2D( %s ): invalid target", fboName.c_str() );
		return;
	}
	
	if( index < 0 || index >= glConfig.maxColorAttachments )
	{
		common->Warning( "Framebuffer::AttachImage2D( %s ): bad index = %i", fboName.c_str(), index );
		return;
	}
#ifndef ANDROID
	if (!glConfig.directStateAccess)
#endif
	{
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0 + index, target, image->texnum, mipmapLod);
	}
#ifndef ANDROID
	else {
		glNamedFramebufferTexture(frameBuffer, GL_COLOR_ATTACHMENT0 + index, image->texnum, mipmapLod);
	}
#endif
}

void Framebuffer::AttachImageDepth( int target, const idImage* image )
{
	if( ( target != GL_TEXTURE_2D ) && ( target != GL_TEXTURE_2D_MULTISAMPLE ) )
	{
		common->Warning( "Framebuffer::AttachImageDepth( %s ): invalid target", fboName.c_str() );
		return;
	}
#ifndef ANDROID
	if (!glConfig.directStateAccess)
#endif
	{
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, target, image->texnum, 0);
	}
#ifndef ANDROID
	else {
		glNamedFramebufferTexture(frameBuffer, GL_DEPTH_STENCIL_ATTACHMENT, image->texnum, 0);
	}
#endif
}

void Framebuffer::AttachImageDepthLayer( const idImage* image, int layer )
{
#ifndef ANDROID
	if (!glConfig.directStateAccess)
#endif
	{
		glFramebufferTextureLayer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, image->texnum, 0, layer);
	}
#ifndef ANDROID
	else {
		glNamedFramebufferTextureLayer(frameBuffer, GL_DEPTH_ATTACHMENT, image->texnum, 0, layer);
	}
#endif
}

void Framebuffer::Check()
{
	int status;
	int prev = -1;
#ifndef ANDROID
	if (!glConfig.directStateAccess)
#endif
	{
		glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev);

		glBindFramebuffer(GL_FRAMEBUFFER, frameBuffer);

		status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
		if (status == GL_FRAMEBUFFER_COMPLETE)
		{
			glBindFramebuffer(GL_FRAMEBUFFER, prev);
			return;
		}
	}
#ifndef ANDROID
	else {
		status = glCheckNamedFramebufferStatus(frameBuffer, GL_FRAMEBUFFER);
		if (status == GL_FRAMEBUFFER_COMPLETE) {
			return;
		}
	}
#endif
	// something went wrong
	switch( status )
	{
		case GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT:
			common->Error( "Framebuffer::Check( %s ): Framebuffer incomplete, incomplete attachment", fboName.c_str() );
			break;
			
		case GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT:
			common->Error( "Framebuffer::Check( %s ): Framebuffer incomplete, missing attachment", fboName.c_str() );
			break;
#ifndef ANDROID
		case GL_FRAMEBUFFER_INCOMPLETE_DRAW_BUFFER:
			common->Error( "Framebuffer::Check( %s ): Framebuffer incomplete, missing draw buffer", fboName.c_str() );
			break;
			
		case GL_FRAMEBUFFER_INCOMPLETE_READ_BUFFER:
			common->Error( "Framebuffer::Check( %s ): Framebuffer incomplete, missing read buffer", fboName.c_str() );
			break;
#endif
		case GL_FRAMEBUFFER_INCOMPLETE_LAYER_TARGETS:
			common->Error( "Framebuffer::Check( %s ): Framebuffer incomplete, missing layer targets", fboName.c_str() );
			break;
			
		case GL_FRAMEBUFFER_INCOMPLETE_MULTISAMPLE:
			common->Error( "Framebuffer::Check( %s ): Framebuffer incomplete, missing multisample", fboName.c_str() );
			break;
			
		case GL_FRAMEBUFFER_UNSUPPORTED:
			common->Error( "Framebuffer::Check( %s ): Unsupported framebuffer format", fboName.c_str() );
			break;
			
		default:
			common->Error( "Framebuffer::Check( %s ): Unknown error 0x%X", fboName.c_str(), status );
			break;
	};
	
	if (!glConfig.directStateAccess) {
		glBindFramebuffer(GL_FRAMEBUFFER, prev);
	}
}

#endif // #if !defined(USE_VULKAN)