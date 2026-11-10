/*
===========================================================================

Doom 3 BFG Edition GPL Source Code
Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company.
Copyright (C) 2013-2015 Robert Beckebans
Copyright (C) 2014 Carl Kenner

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

#pragma hdrstop
#include "precompiled.h"

#include "../framework/Common_local.h"
#include "RenderCommon.h"
#include "Framebuffer.h"

#define GL_READ_FRAMEBUFFER_EXT           0x8CA8
#define GL_DRAW_FRAMEBUFFER_EXT           0x8CA9

#if defined(ANDROID) && !defined(USE_VULKAN)
namespace
{
static const int RB_EXPOSURE_READBACK_COUNT = 3;
static const GLsizeiptr RB_EXPOSURE_READBACK_BYTES = 64 * 64 * 4 * sizeof(float);

struct rbExposureReadbackSlot_t
{
	GLuint buffer;
	GLsync fence;
	unsigned long long sequence;
};

static rbExposureReadbackSlot_t rbExposureReadbackSlots[RB_EXPOSURE_READBACK_COUNT] = {};
static unsigned long long rbExposureReadbackNextSequence = 1;
static int rbExposureReadbackWriteIndex = 0;
static bool rbExposureReadbackInitialized = false;
static bool rbExposureReadbackHasInitialSample = false;
}

void RB_InitExposureReadback()
{
	if ( rbExposureReadbackInitialized )
	{
		return;
	}

	GLint previousPackBuffer = 0;
	glGetIntegerv( GL_PIXEL_PACK_BUFFER_BINDING, &previousPackBuffer );

	GLuint buffers[RB_EXPOSURE_READBACK_COUNT] = {};
	glGenBuffers( RB_EXPOSURE_READBACK_COUNT, buffers );

	bool success = true;
	for ( int i = 0; i < RB_EXPOSURE_READBACK_COUNT; ++i )
	{
		if ( buffers[i] == 0 )
		{
			success = false;
			break;
		}

		glBindBuffer( GL_PIXEL_PACK_BUFFER, buffers[i] );
		glBufferData( GL_PIXEL_PACK_BUFFER, RB_EXPOSURE_READBACK_BYTES, NULL, GL_STREAM_READ );
		GLint allocatedBytes = 0;
		glGetBufferParameteriv( GL_PIXEL_PACK_BUFFER, GL_BUFFER_SIZE, &allocatedBytes );
		if ( allocatedBytes < RB_EXPOSURE_READBACK_BYTES )
		{
			success = false;
			break;
		}
	}

	glBindBuffer( GL_PIXEL_PACK_BUFFER, previousPackBuffer );

	if ( !success )
	{
		glDeleteBuffers( RB_EXPOSURE_READBACK_COUNT, buffers );
		return;
	}

	for ( int i = 0; i < RB_EXPOSURE_READBACK_COUNT; ++i )
	{
		rbExposureReadbackSlots[i].buffer = buffers[i];
		rbExposureReadbackSlots[i].fence = nullptr;
		rbExposureReadbackSlots[i].sequence = 0;
	}

	rbExposureReadbackNextSequence = 1;
	rbExposureReadbackWriteIndex = 0;
	rbExposureReadbackHasInitialSample = false;
	rbExposureReadbackInitialized = true;
}

void RB_ShutdownExposureReadback()
{
	if ( !rbExposureReadbackInitialized )
	{
		return;
	}

	GLuint buffers[RB_EXPOSURE_READBACK_COUNT] = {};
	for ( int i = 0; i < RB_EXPOSURE_READBACK_COUNT; ++i )
	{
		if ( rbExposureReadbackSlots[i].fence != nullptr )
		{
			glDeleteSync( rbExposureReadbackSlots[i].fence );
		}
		buffers[i] = rbExposureReadbackSlots[i].buffer;
		rbExposureReadbackSlots[i].buffer = 0;
		rbExposureReadbackSlots[i].fence = nullptr;
		rbExposureReadbackSlots[i].sequence = 0;
	}

	glDeleteBuffers( RB_EXPOSURE_READBACK_COUNT, buffers );
	rbExposureReadbackNextSequence = 1;
	rbExposureReadbackWriteIndex = 0;
	rbExposureReadbackHasInitialSample = false;
	rbExposureReadbackInitialized = false;
}

static bool RB_ReadExposurePixelsAsync( float* image )
{
	if ( !rbExposureReadbackInitialized )
	{
		// Keep HDR working on drivers where PBO allocation is unavailable.
		glReadPixels( 0, 0, 64, 64, GL_RGBA, GL_FLOAT, image );
		return true;
	}

	if ( !rbExposureReadbackHasInitialSample )
	{
		// Seed exposure synchronously once so the first HDR frame never tonemaps
		// with uninitialized luminance while the asynchronous ring is warming up.
		glReadPixels( 0, 0, 64, 64, GL_RGBA, GL_FLOAT, image );
		rbExposureReadbackHasInitialSample = true;
		return true;
	}

	int readyIndex = -1;
	unsigned long long oldestReadySequence = ~0ULL;
	bool waitFailed = false;

	// Never wait for the current frame's readback. Only consume a completed
	// buffer from an earlier frame, otherwise keep the previous exposure.
	for ( int i = 0; i < RB_EXPOSURE_READBACK_COUNT; ++i )
	{
		rbExposureReadbackSlot_t& slot = rbExposureReadbackSlots[i];
		if ( slot.fence == nullptr )
		{
			continue;
		}

		const GLenum waitResult = glClientWaitSync( slot.fence, 0, 0 );
		if ( waitResult == GL_WAIT_FAILED )
		{
			waitFailed = true;
			break;
		}

		if ( ( waitResult == GL_ALREADY_SIGNALED || waitResult == GL_CONDITION_SATISFIED ) &&
			 slot.sequence < oldestReadySequence )
		{
			oldestReadySequence = slot.sequence;
			readyIndex = i;
		}
	}

	if ( waitFailed )
	{
		RB_ShutdownExposureReadback();
		glReadPixels( 0, 0, 64, 64, GL_RGBA, GL_FLOAT, image );
		return true;
	}

	bool hasSample = false;
	if ( readyIndex >= 0 )
	{
		rbExposureReadbackSlot_t& slot = rbExposureReadbackSlots[readyIndex];
		GLint previousPackBuffer = 0;
		glGetIntegerv( GL_PIXEL_PACK_BUFFER_BINDING, &previousPackBuffer );
		glBindBuffer( GL_PIXEL_PACK_BUFFER, slot.buffer );

		const void* mapped = glMapBufferRange( GL_PIXEL_PACK_BUFFER, 0, RB_EXPOSURE_READBACK_BYTES, GL_MAP_READ_BIT );
		bool mapSucceeded = false;
		if ( mapped != nullptr )
		{
			memcpy( image, mapped, (size_t)RB_EXPOSURE_READBACK_BYTES );
			mapSucceeded = ( glUnmapBuffer( GL_PIXEL_PACK_BUFFER ) == GL_TRUE );
		}

		glBindBuffer( GL_PIXEL_PACK_BUFFER, previousPackBuffer );
		glDeleteSync( slot.fence );
		slot.fence = nullptr;
		slot.sequence = 0;

		if ( !mapSucceeded )
		{
			// If a completed PBO cannot be mapped reliably, fall back to the
			// synchronous path instead of leaving exposure frozen indefinitely.
			RB_ShutdownExposureReadback();
			glReadPixels( 0, 0, 64, 64, GL_RGBA, GL_FLOAT, image );
			return true;
		}
		hasSample = true;
	}

	// Use a three-buffer ring. If the next slot is still in flight, skip this
	// submission rather than blocking the render thread or overwriting it.
	const int writeIndex = rbExposureReadbackWriteIndex;
	rbExposureReadbackSlot_t& writeSlot = rbExposureReadbackSlots[writeIndex];
	if ( writeSlot.fence == nullptr && writeSlot.buffer != 0 )
	{
		GLint previousPackBuffer = 0;
		glGetIntegerv( GL_PIXEL_PACK_BUFFER_BINDING, &previousPackBuffer );
		glBindBuffer( GL_PIXEL_PACK_BUFFER, writeSlot.buffer );
		glReadPixels( 0, 0, 64, 64, GL_RGBA, GL_FLOAT, NULL );
		GLsync fence = glFenceSync( GL_SYNC_GPU_COMMANDS_COMPLETE, 0 );
		glBindBuffer( GL_PIXEL_PACK_BUFFER, previousPackBuffer );

		if ( fence == nullptr )
		{
			// A missing fence makes safe asynchronous reuse impossible.
			// Tear down the ring and fall back to the original synchronous path.
			RB_ShutdownExposureReadback();
			glReadPixels( 0, 0, 64, 64, GL_RGBA, GL_FLOAT, image );
			return true;
		}

		writeSlot.fence = fence;
		writeSlot.sequence = rbExposureReadbackNextSequence++;
	}

	rbExposureReadbackWriteIndex = ( rbExposureReadbackWriteIndex + 1 ) % RB_EXPOSURE_READBACK_COUNT;
	return hasSample;
}
#endif


idCVar r_drawEyeColor( "r_drawEyeColor", "0", CVAR_RENDERER | CVAR_BOOL, "Draw a colored box, red = left eye, blue = right eye, grey = non-stereo" );
idCVar r_motionBlur( "r_motionBlur", "0", CVAR_RENDERER | CVAR_INTEGER | CVAR_ARCHIVE, "1 - 5, log2 of the number of motion blur samples" );
idCVar r_forceZPassStencilShadows( "r_forceZPassStencilShadows", "0", CVAR_RENDERER | CVAR_BOOL, "force Z-pass rendering for performance testing" );
idCVar r_useStencilShadowPreload( "r_useStencilShadowPreload", "0", CVAR_RENDERER | CVAR_BOOL, "use stencil shadow preload algorithm instead of Z-fail" );
idCVar r_skipShaderPasses( "r_skipShaderPasses", "0", CVAR_RENDERER | CVAR_BOOL, "" );
idCVar r_skipInteractionFastPath( "r_skipInteractionFastPath", "1", CVAR_RENDERER | CVAR_BOOL, "" );
idCVar r_useLightStencilSelect( "r_useLightStencilSelect", "0", CVAR_RENDERER | CVAR_BOOL, "use stencil select pass" );

idCVar r_clShaderScaler("r_clShaderScaler", "2.0f", CVAR_RENDERER | CVAR_FLOAT, "set resolution scale factor for Classic DOOM's screen shader");
//extern idCVar stereoRender_swapEyes;

/*
================
SetVertexParm
================
*/
static ID_INLINE void SetVertexParm( renderParm_t rp, const float* value )
{
	renderProgManager.SetUniformValue( rp, value );
}

/*
================
SetVertexParms
================
*/
static ID_INLINE void SetVertexParms( renderParm_t rp, const float* value, int num )
{
	for( int i = 0; i < num; i++ )
	{
		renderProgManager.SetUniformValue( ( renderParm_t )( rp + i ), value + ( i * 4 ) );
	}
}

/*
================
SetFragmentParm
================
*/
static ID_INLINE void SetFragmentParm( renderParm_t rp, const float* value )
{
	renderProgManager.SetUniformValue( rp, value );
}

/*
====================
PrintState
====================
*/
#if 0
void PrintState( uint64 stateBits, uint64* stencilBits )
{
	if( renderLog.Active() == 0 )
	{
		return;
	}
	
	renderLog.OpenBlock( "GL_State" );
	
	// culling
	renderLog.Printf( "Culling: " );
	switch( stateBits & GLS_CULL_BITS )
	{
		case GLS_CULL_FRONTSIDED:
			renderLog.Printf_NoIndent( "FRONTSIDED -> BACK" );
			break;
		case GLS_CULL_BACKSIDED:
			renderLog.Printf_NoIndent( "BACKSIDED -> FRONT" );
			break;
		case GLS_CULL_TWOSIDED:
			renderLog.Printf_NoIndent( "TWOSIDED" );
			break;
		default:
			renderLog.Printf_NoIndent( "NA" );
			break;
	}
	renderLog.Printf_NoIndent( "\n" );
	
	// polygon mode
	renderLog.Printf( "PolygonMode: %s\n", ( stateBits & GLS_POLYMODE_LINE ) ? "LINE" : "FILL" );
	
	// color mask
	renderLog.Printf( "ColorMask: " );
	renderLog.Printf_NoIndent( ( stateBits & GLS_REDMASK ) ? "_" : "R" );
	renderLog.Printf_NoIndent( ( stateBits & GLS_GREENMASK ) ? "_" : "G" );
	renderLog.Printf_NoIndent( ( stateBits & GLS_BLUEMASK ) ? "_" : "B" );
	renderLog.Printf_NoIndent( ( stateBits & GLS_ALPHAMASK ) ? "_" : "A" );
	renderLog.Printf_NoIndent( "\n" );
	
	// blend
	renderLog.Printf( "Blend: src=" );
	switch( stateBits & GLS_SRCBLEND_BITS )
	{
		case GLS_SRCBLEND_ZERO:
			renderLog.Printf_NoIndent( "ZERO" );
			break;
		case GLS_SRCBLEND_ONE:
			renderLog.Printf_NoIndent( "ONE" );
			break;
		case GLS_SRCBLEND_DST_COLOR:
			renderLog.Printf_NoIndent( "DST_COLOR" );
			break;
		case GLS_SRCBLEND_ONE_MINUS_DST_COLOR:
			renderLog.Printf_NoIndent( "ONE_MINUS_DST_COLOR" );
			break;
		case GLS_SRCBLEND_SRC_ALPHA:
			renderLog.Printf_NoIndent( "SRC_ALPHA" );
			break;
		case GLS_SRCBLEND_ONE_MINUS_SRC_ALPHA:
			renderLog.Printf_NoIndent( "ONE_MINUS_SRC_ALPHA" );
			break;
		case GLS_SRCBLEND_DST_ALPHA:
			renderLog.Printf_NoIndent( "DST_ALPHA" );
			break;
		case GLS_SRCBLEND_ONE_MINUS_DST_ALPHA:
			renderLog.Printf_NoIndent( "ONE_MINUS_DST_ALPHA" );
			break;
		default:
			renderLog.Printf_NoIndent( "NA" );
			break;
	}
	renderLog.Printf_NoIndent( ", dst=" );
	switch( stateBits & GLS_DSTBLEND_BITS )
	{
		case GLS_DSTBLEND_ZERO:
			renderLog.Printf_NoIndent( "ZERO" );
			break;
		case GLS_DSTBLEND_ONE:
			renderLog.Printf_NoIndent( "ONE" );
			break;
		case GLS_DSTBLEND_SRC_COLOR:
			renderLog.Printf_NoIndent( "SRC_COLOR" );
			break;
		case GLS_DSTBLEND_ONE_MINUS_SRC_COLOR:
			renderLog.Printf_NoIndent( "ONE_MINUS_SRC_COLOR" );
			break;
		case GLS_DSTBLEND_SRC_ALPHA:
			renderLog.Printf_NoIndent( "SRC_ALPHA" );
			break;
		case GLS_DSTBLEND_ONE_MINUS_SRC_ALPHA:
			renderLog.Printf_NoIndent( "ONE_MINUS_SRC_ALPHA" );
			break;
		case GLS_DSTBLEND_DST_ALPHA:
			renderLog.Printf_NoIndent( "DST_ALPHA" );
			break;
		case GLS_DSTBLEND_ONE_MINUS_DST_ALPHA:
			renderLog.Printf_NoIndent( "ONE_MINUS_DST_ALPHA" );
			break;
		default:
			renderLog.Printf_NoIndent( "NA" );
	}
	renderLog.Printf_NoIndent( "\n" );
	
	// depth func
	renderLog.Printf( "DepthFunc: " );
	switch( stateBits & GLS_DEPTHFUNC_BITS )
	{
		case GLS_DEPTHFUNC_EQUAL:
			renderLog.Printf_NoIndent( "EQUAL" );
			break;
		case GLS_DEPTHFUNC_ALWAYS:
			renderLog.Printf_NoIndent( "ALWAYS" );
			break;
		case GLS_DEPTHFUNC_LESS:
			renderLog.Printf_NoIndent( "LEQUAL" );
			break;
		case GLS_DEPTHFUNC_GREATER:
			renderLog.Printf_NoIndent( "GEQUAL" );
			break;
		default:
			renderLog.Printf_NoIndent( "NA" );
			break;
	}
	renderLog.Printf_NoIndent( "\n" );
	
	// depth mask
	renderLog.Printf( "DepthWrite: %s\n", ( stateBits & GLS_DEPTHMASK ) ? "FALSE" : "TRUE" );
	
	renderLog.Printf( "DepthBounds: %s\n", ( stateBits & GLS_DEPTH_TEST_MASK ) ? "TRUE" : "FALSE" );
	
	// depth bias
	renderLog.Printf( "DepthBias: %s\n", ( stateBits & GLS_POLYGON_OFFSET ) ? "TRUE" : "FALSE" );
	
	// stencil
	auto printStencil = [&]( stencilFace_t face, uint64 bits, uint64 mask, uint64 ref )
	{
		renderLog.Printf( "Stencil: %s, ", ( bits & ( GLS_STENCIL_FUNC_BITS | GLS_STENCIL_OP_BITS ) ) ? "ON" : "OFF" );
		renderLog.Printf_NoIndent( "Face=" );
		switch( face )
		{
			case STENCIL_FACE_FRONT:
				renderLog.Printf_NoIndent( "FRONT" );
				break;
			case STENCIL_FACE_BACK:
				renderLog.Printf_NoIndent( "BACK" );
				break;
			default:
				renderLog.Printf_NoIndent( "BOTH" );
				break;
		}
		renderLog.Printf_NoIndent( ", Func=" );
		switch( bits & GLS_STENCIL_FUNC_BITS )
		{
			case GLS_STENCIL_FUNC_NEVER:
				renderLog.Printf_NoIndent( "NEVER" );
				break;
			case GLS_STENCIL_FUNC_LESS:
				renderLog.Printf_NoIndent( "LESS" );
				break;
			case GLS_STENCIL_FUNC_EQUAL:
				renderLog.Printf_NoIndent( "EQUAL" );
				break;
			case GLS_STENCIL_FUNC_LEQUAL:
				renderLog.Printf_NoIndent( "LEQUAL" );
				break;
			case GLS_STENCIL_FUNC_GREATER:
				renderLog.Printf_NoIndent( "GREATER" );
				break;
			case GLS_STENCIL_FUNC_NOTEQUAL:
				renderLog.Printf_NoIndent( "NOTEQUAL" );
				break;
			case GLS_STENCIL_FUNC_GEQUAL:
				renderLog.Printf_NoIndent( "GEQUAL" );
				break;
			case GLS_STENCIL_FUNC_ALWAYS:
				renderLog.Printf_NoIndent( "ALWAYS" );
				break;
			default:
				renderLog.Printf_NoIndent( "NA" );
				break;
		}
		renderLog.Printf_NoIndent( ", OpFail=" );
		switch( bits & GLS_STENCIL_OP_FAIL_BITS )
		{
			case GLS_STENCIL_OP_FAIL_KEEP:
				renderLog.Printf_NoIndent( "KEEP" );
				break;
			case GLS_STENCIL_OP_FAIL_ZERO:
				renderLog.Printf_NoIndent( "ZERO" );
				break;
			case GLS_STENCIL_OP_FAIL_REPLACE:
				renderLog.Printf_NoIndent( "REPLACE" );
				break;
			case GLS_STENCIL_OP_FAIL_INCR:
				renderLog.Printf_NoIndent( "INCR" );
				break;
			case GLS_STENCIL_OP_FAIL_DECR:
				renderLog.Printf_NoIndent( "DECR" );
				break;
			case GLS_STENCIL_OP_FAIL_INVERT:
				renderLog.Printf_NoIndent( "INVERT" );
				break;
			case GLS_STENCIL_OP_FAIL_INCR_WRAP:
				renderLog.Printf_NoIndent( "INCR_WRAP" );
				break;
			case GLS_STENCIL_OP_FAIL_DECR_WRAP:
				renderLog.Printf_NoIndent( "DECR_WRAP" );
				break;
			default:
				renderLog.Printf_NoIndent( "NA" );
				break;
		}
		renderLog.Printf_NoIndent( ", ZFail=" );
		switch( bits & GLS_STENCIL_OP_ZFAIL_BITS )
		{
			case GLS_STENCIL_OP_ZFAIL_KEEP:
				renderLog.Printf_NoIndent( "KEEP" );
				break;
			case GLS_STENCIL_OP_ZFAIL_ZERO:
				renderLog.Printf_NoIndent( "ZERO" );
				break;
			case GLS_STENCIL_OP_ZFAIL_REPLACE:
				renderLog.Printf_NoIndent( "REPLACE" );
				break;
			case GLS_STENCIL_OP_ZFAIL_INCR:
				renderLog.Printf_NoIndent( "INCR" );
				break;
			case GLS_STENCIL_OP_ZFAIL_DECR:
				renderLog.Printf_NoIndent( "DECR" );
				break;
			case GLS_STENCIL_OP_ZFAIL_INVERT:
				renderLog.Printf_NoIndent( "INVERT" );
				break;
			case GLS_STENCIL_OP_ZFAIL_INCR_WRAP:
				renderLog.Printf_NoIndent( "INCR_WRAP" );
				break;
			case GLS_STENCIL_OP_ZFAIL_DECR_WRAP:
				renderLog.Printf_NoIndent( "DECR_WRAP" );
				break;
			default:
				renderLog.Printf_NoIndent( "NA" );
				break;
		}
		renderLog.Printf_NoIndent( ", OpPass=" );
		switch( bits & GLS_STENCIL_OP_PASS_BITS )
		{
			case GLS_STENCIL_OP_PASS_KEEP:
				renderLog.Printf_NoIndent( "KEEP" );
				break;
			case GLS_STENCIL_OP_PASS_ZERO:
				renderLog.Printf_NoIndent( "ZERO" );
				break;
			case GLS_STENCIL_OP_PASS_REPLACE:
				renderLog.Printf_NoIndent( "REPLACE" );
				break;
			case GLS_STENCIL_OP_PASS_INCR:
				renderLog.Printf_NoIndent( "INCR" );
				break;
			case GLS_STENCIL_OP_PASS_DECR:
				renderLog.Printf_NoIndent( "DECR" );
				break;
			case GLS_STENCIL_OP_PASS_INVERT:
				renderLog.Printf_NoIndent( "INVERT" );
				break;
			case GLS_STENCIL_OP_PASS_INCR_WRAP:
				renderLog.Printf_NoIndent( "INCR_WRAP" );
				break;
			case GLS_STENCIL_OP_PASS_DECR_WRAP:
				renderLog.Printf_NoIndent( "DECR_WRAP" );
				break;
			default:
				renderLog.Printf_NoIndent( "NA" );
				break;
		}
		renderLog.Printf_NoIndent( ", mask=%llu, ref=%llu\n", mask, ref );
	};
	
	uint32 mask = uint32( ( stateBits & GLS_STENCIL_FUNC_MASK_BITS ) >> GLS_STENCIL_FUNC_MASK_SHIFT );
	uint32 ref = uint32( ( stateBits & GLS_STENCIL_FUNC_REF_BITS ) >> GLS_STENCIL_FUNC_REF_SHIFT );
	if( stateBits & GLS_SEPARATE_STENCIL )
	{
		printStencil( STENCIL_FACE_FRONT, stencilBits[ 0 ], mask, ref );
		printStencil( STENCIL_FACE_BACK, stencilBits[ 1 ], mask, ref );
	}
	else
	{
		printStencil( STENCIL_FACE_NUM, stateBits, mask, ref );
	}
	
	renderLog.CloseBlock();
}
#endif

/*
================
RB_SetMVP
================
*/
void RB_SetMVP( const idRenderMatrix& mvp )
{
	SetVertexParms( RENDERPARM_MVPMATRIX_X, mvp[0], 4 );
}

/*
================
RB_SetMVPWithStereoOffset
================
*/
static void RB_SetMVPWithStereoOffset( const idRenderMatrix& mvp, const float stereoOffset )
{
	idRenderMatrix offset = mvp;
	offset[0][3] += stereoOffset;
	
	SetVertexParms( RENDERPARM_MVPMATRIX_X, offset[0], 4 );
}

static const float zero[4] = { 0, 0, 0, 0 };
static const float one[4] = { 1, 1, 1, 1 };
static const float negOne[4] = { -1, -1, -1, -1 };

/*
================
RB_SetVertexColorParms
================
*/
void RB_SetVertexColorParms( stageVertexColor_t svc )
{
	switch( svc )
	{
		case SVC_IGNORE:
			SetVertexParm( RENDERPARM_VERTEXCOLOR_MODULATE, zero );
			SetVertexParm( RENDERPARM_VERTEXCOLOR_ADD, one );
			break;
		case SVC_MODULATE:
			SetVertexParm( RENDERPARM_VERTEXCOLOR_MODULATE, one );
			SetVertexParm( RENDERPARM_VERTEXCOLOR_ADD, zero );
			break;
		case SVC_INVERSE_MODULATE:
			SetVertexParm( RENDERPARM_VERTEXCOLOR_MODULATE, negOne );
			SetVertexParm( RENDERPARM_VERTEXCOLOR_ADD, one );
			break;
	}
}



/*
======================
RB_GetShaderTextureMatrix
======================
*/
void RB_GetShaderTextureMatrix( const float* shaderRegisters, const textureStage_t* texture, float matrix[16] )
{
	matrix[0 * 4 + 0] = shaderRegisters[ texture->matrix[0][0] ];
	matrix[1 * 4 + 0] = shaderRegisters[ texture->matrix[0][1] ];
	matrix[2 * 4 + 0] = 0.0f;
	matrix[3 * 4 + 0] = shaderRegisters[ texture->matrix[0][2] ];
	
	matrix[0 * 4 + 1] = shaderRegisters[ texture->matrix[1][0] ];
	matrix[1 * 4 + 1] = shaderRegisters[ texture->matrix[1][1] ];
	matrix[2 * 4 + 1] = 0.0f;
	matrix[3 * 4 + 1] = shaderRegisters[ texture->matrix[1][2] ];
	
	// we attempt to keep scrolls from generating incredibly large texture values, but
	// center rotations and center scales can still generate offsets that need to be > 1
	if( matrix[3 * 4 + 0] < -40.0f || matrix[12] > 40.0f )
	{
		matrix[3 * 4 + 0] -= ( int )matrix[3 * 4 + 0];
	}
	if( matrix[13] < -40.0f || matrix[13] > 40.0f )
	{
		matrix[13] -= ( int )matrix[13];
	}
	
	matrix[0 * 4 + 2] = 0.0f;
	matrix[1 * 4 + 2] = 0.0f;
	matrix[2 * 4 + 2] = 1.0f;
	matrix[3 * 4 + 2] = 0.0f;
	
	matrix[0 * 4 + 3] = 0.0f;
	matrix[1 * 4 + 3] = 0.0f;
	matrix[2 * 4 + 3] = 0.0f;
	matrix[3 * 4 + 3] = 1.0f;
}

/*
======================
RB_LoadShaderTextureMatrix
======================
*/
void RB_LoadShaderTextureMatrix( const float* shaderRegisters, const textureStage_t* texture )
{
	float texS[4] = { 1.0f, 0.0f, 0.0f, 0.0f };
	float texT[4] = { 0.0f, 1.0f, 0.0f, 0.0f };
	
	if( texture->hasMatrix )
	{
		float matrix[16];
		RB_GetShaderTextureMatrix( shaderRegisters, texture, matrix );
		texS[0] = matrix[0 * 4 + 0];
		texS[1] = matrix[1 * 4 + 0];
		texS[2] = matrix[2 * 4 + 0];
		texS[3] = matrix[3 * 4 + 0];
		
		texT[0] = matrix[0 * 4 + 1];
		texT[1] = matrix[1 * 4 + 1];
		texT[2] = matrix[2 * 4 + 1];
		texT[3] = matrix[3 * 4 + 1];
		
		RENDERLOG_PRINTF( "Setting Texture Matrix\n" );
		renderLog.Indent();
		RENDERLOG_PRINTF( "Texture Matrix S : %4.3f, %4.3f, %4.3f, %4.3f\n", texS[0], texS[1], texS[2], texS[3] );
		RENDERLOG_PRINTF( "Texture Matrix T : %4.3f, %4.3f, %4.3f, %4.3f\n", texT[0], texT[1], texT[2], texT[3] );
		renderLog.Outdent();
	}
	
	SetVertexParm( RENDERPARM_TEXTUREMATRIX_S, texS );
	SetVertexParm( RENDERPARM_TEXTUREMATRIX_T, texT );
}

/*
=====================
RB_BakeTextureMatrixIntoTexgen
=====================
*/
void RB_BakeTextureMatrixIntoTexgen( idPlane lightProject[3], const float* textureMatrix )
{
	float genMatrix[16];
	float final[16];
	
	genMatrix[0 * 4 + 0] = lightProject[0][0];
	genMatrix[1 * 4 + 0] = lightProject[0][1];
	genMatrix[2 * 4 + 0] = lightProject[0][2];
	genMatrix[3 * 4 + 0] = lightProject[0][3];
	
	genMatrix[0 * 4 + 1] = lightProject[1][0];
	genMatrix[1 * 4 + 1] = lightProject[1][1];
	genMatrix[2 * 4 + 1] = lightProject[1][2];
	genMatrix[3 * 4 + 1] = lightProject[1][3];
	
	genMatrix[0 * 4 + 2] = 0.0f;
	genMatrix[1 * 4 + 2] = 0.0f;
	genMatrix[2 * 4 + 2] = 0.0f;
	genMatrix[3 * 4 + 2] = 0.0f;
	
	genMatrix[0 * 4 + 3] = lightProject[2][0];
	genMatrix[1 * 4 + 3] = lightProject[2][1];
	genMatrix[2 * 4 + 3] = lightProject[2][2];
	genMatrix[3 * 4 + 3] = lightProject[2][3];
	
	R_MatrixMultiply( genMatrix, textureMatrix, final );
	
	lightProject[0][0] = final[0 * 4 + 0];
	lightProject[0][1] = final[1 * 4 + 0];
	lightProject[0][2] = final[2 * 4 + 0];
	lightProject[0][3] = final[3 * 4 + 0];
	
	lightProject[1][0] = final[0 * 4 + 1];
	lightProject[1][1] = final[1 * 4 + 1];
	lightProject[1][2] = final[2 * 4 + 1];
	lightProject[1][3] = final[3 * 4 + 1];
}

/*
======================
idRenderBackend::BindVariableStageImage

Handles generating a cinematic frame if needed
======================
*/
void idRenderBackend::BindVariableStageImage( const textureStage_t* texture, const float* shaderRegisters )
{
	if( texture->cinematic )
	{
//#if defined(USE_VULKAN)
//	
//		// FIXME upload of video image sub resources
//		GL_SelectTexture( 0 );
//		globalImages->defaultImage->Bind();
//#else
		cinData_t cin;
		
		if( r_skipDynamicTextures.GetBool() )
		{
			globalImages->defaultImage->Bind();
			return;
		}
		
		// offset time by shaderParm[7] (FIXME: make the time offset a parameter of the shader?)
		// We make no attempt to optimize for multiple identical cinematics being in view, or
		// for cinematics going at a lower framerate than the renderer.
		cin = texture->cinematic->ImageForTime( viewDef->renderView.time[0] + idMath::Ftoi( 1000.0f * viewDef->renderView.shaderParms[11] ) );
		if( cin.imageY != NULL )
		{
			GL_SelectTexture( 0 );
			cin.imageY->Bind();
		
			GL_SelectTexture( 1 );
			cin.imageCr->Bind();
		
			GL_SelectTexture( 2 );
			cin.imageCb->Bind();
		
			// DG: imageY is only used for bink videos (with libbinkdec), so the bink shader must be used
			if( viewDef->is2Dgui )
			{
				renderProgManager.BindShader_BinkGUI();
			}
			else
			{
				renderProgManager.BindShader_Bink();
			}
		}
		else if( cin.image != NULL )
		{
			// Carl: A single RGB image works better with the FFMPEG BINK codec.
			GL_SelectTexture( 0 );
			cin.image->Bind();
		
			
			if( viewDef->is2Dgui )
			{
				renderProgManager.BindShader_TextureVertexColor_sRGB();
			}
			else
			{
				renderProgManager.BindShader_TextureVertexColor();
			}
			
		}
		else
		{
			globalImages->blackImage->Bind();
			// because the shaders may have already been set - we need to make sure we are not using a bink shader which would
			// display incorrectly.  We may want to get rid of RB_BindVariableStageImage and inline the code so that the
			// SWF GUI case is handled better, too
			renderProgManager.BindShader_TextureVertexColor();
		}
//#endif
	}
	else
	{
		// FIXME: see why image is invalid
		if( texture->image != NULL )
		{
			texture->image->Bind();
		}
	}
}

/*
================
idRenderBackend::PrepareStageTexturing
================
*/
void idRenderBackend::PrepareStageTexturing( const shaderStage_t* pStage,  const drawSurf_t* surf )
{
	float useTexGenParm[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
	
	// set the texture matrix if needed
	RB_LoadShaderTextureMatrix( surf->shaderRegisters, &pStage->texture );
	
	// texgens
	if( pStage->texture.texgen == TG_REFLECT_CUBE )
	{
	
		// see if there is also a bump map specified
		const shaderStage_t* bumpStage = surf->material->GetBumpStage();
		if( bumpStage != NULL )
		{
			// per-pixel reflection mapping with bump mapping
			GL_SelectTexture( 1 );
			bumpStage->texture.image->Bind();
			GL_SelectTexture( 0 );
			
			RENDERLOG_PRINTF( "TexGen: TG_REFLECT_CUBE: Bumpy Environment\n" );
			if( surf->jointCache )
			{
				renderProgManager.BindShader_BumpyEnvironmentSkinned();
			}
			else
			{
				renderProgManager.BindShader_BumpyEnvironment();
			}
		}
		else
		{
			RENDERLOG_PRINTF( "TexGen: TG_REFLECT_CUBE: Environment\n" );
			if( surf->jointCache )
			{
				renderProgManager.BindShader_EnvironmentSkinned();
			}
			else
			{
				renderProgManager.BindShader_Environment();
			}
		}
		
	}
	else if( pStage->texture.texgen == TG_SKYBOX_CUBE )
	{
	
		renderProgManager.BindShader_SkyBox();
		
	}
	else if( pStage->texture.texgen == TG_WOBBLESKY_CUBE )
	{
	
		const int* parms = surf->material->GetTexGenRegisters();
		
		float wobbleDegrees = surf->shaderRegisters[ parms[0] ] * ( idMath::PI / 180.0f );
		float wobbleSpeed = surf->shaderRegisters[ parms[1] ] * ( 2.0f * idMath::PI / 60.0f );
		float rotateSpeed = surf->shaderRegisters[ parms[2] ] * ( 2.0f * idMath::PI / 60.0f );
		
		idVec3 axis[3];
		{
			// very ad-hoc "wobble" transform
			float s, c;
			idMath::SinCos( wobbleSpeed * viewDef->renderView.time[0] * 0.001f, s, c );
			
			float ws, wc;
			idMath::SinCos( wobbleDegrees, ws, wc );
			
			axis[2][0] = ws * c;
			axis[2][1] = ws * s;
			axis[2][2] = wc;
			
			axis[1][0] = -s * s * ws;
			axis[1][2] = -s * ws * ws;
			axis[1][1] = idMath::Sqrt( idMath::Fabs( 1.0f - ( axis[1][0] * axis[1][0] + axis[1][2] * axis[1][2] ) ) );
			
			// make the second vector exactly perpendicular to the first
			axis[1] -= ( axis[2] * axis[1] ) * axis[2];
			axis[1].Normalize();
			
			// construct the third with a cross
			axis[0].Cross( axis[1], axis[2] );
		}
		
		// add the rotate
		float rs, rc;
		idMath::SinCos( rotateSpeed * viewDef->renderView.time[0] * 0.001f, rs, rc );
		
		float transform[12];
		transform[0 * 4 + 0] = axis[0][0] * rc + axis[1][0] * rs;
		transform[0 * 4 + 1] = axis[0][1] * rc + axis[1][1] * rs;
		transform[0 * 4 + 2] = axis[0][2] * rc + axis[1][2] * rs;
		transform[0 * 4 + 3] = 0.0f;
		
		transform[1 * 4 + 0] = axis[1][0] * rc - axis[0][0] * rs;
		transform[1 * 4 + 1] = axis[1][1] * rc - axis[0][1] * rs;
		transform[1 * 4 + 2] = axis[1][2] * rc - axis[0][2] * rs;
		transform[1 * 4 + 3] = 0.0f;
		
		transform[2 * 4 + 0] = axis[2][0];
		transform[2 * 4 + 1] = axis[2][1];
		transform[2 * 4 + 2] = axis[2][2];
		transform[2 * 4 + 3] = 0.0f;
		
		SetVertexParms( RENDERPARM_WOBBLESKY_X, transform, 3 );
		renderProgManager.BindShader_WobbleSky();
		
	}
	else if( ( pStage->texture.texgen == TG_SCREEN ) || ( pStage->texture.texgen == TG_SCREEN2 ) )
	{
	
		useTexGenParm[0] = 1.0f;
		useTexGenParm[1] = 1.0f;
		useTexGenParm[2] = 1.0f;
		useTexGenParm[3] = 1.0f;
		
		float mat[16];
		R_MatrixMultiply( surf->space->modelViewMatrix, viewDef->projectionMatrix, mat );
		
		RENDERLOG_PRINTF( "TexGen : %s\n", ( pStage->texture.texgen == TG_SCREEN ) ? "TG_SCREEN" : "TG_SCREEN2" );
		renderLog.Indent();
		
		float plane[4];
		plane[0] = mat[0 * 4 + 0];
		plane[1] = mat[1 * 4 + 0];
		plane[2] = mat[2 * 4 + 0];
		plane[3] = mat[3 * 4 + 0];
		SetVertexParm( RENDERPARM_TEXGEN_0_S, plane );
		RENDERLOG_PRINTF( "TEXGEN_S = %4.3f, %4.3f, %4.3f, %4.3f\n",  plane[0], plane[1], plane[2], plane[3] );
		
		plane[0] = mat[0 * 4 + 1];
		plane[1] = mat[1 * 4 + 1];
		plane[2] = mat[2 * 4 + 1];
		plane[3] = mat[3 * 4 + 1];
		SetVertexParm( RENDERPARM_TEXGEN_0_T, plane );
		RENDERLOG_PRINTF( "TEXGEN_T = %4.3f, %4.3f, %4.3f, %4.3f\n",  plane[0], plane[1], plane[2], plane[3] );
		
		plane[0] = mat[0 * 4 + 3];
		plane[1] = mat[1 * 4 + 3];
		plane[2] = mat[2 * 4 + 3];
		plane[3] = mat[3 * 4 + 3];
		SetVertexParm( RENDERPARM_TEXGEN_0_Q, plane );
		RENDERLOG_PRINTF( "TEXGEN_Q = %4.3f, %4.3f, %4.3f, %4.3f\n",  plane[0], plane[1], plane[2], plane[3] );
		
		renderLog.Outdent();
		
	}
	else if( pStage->texture.texgen == TG_DIFFUSE_CUBE )
	{
	
		// As far as I can tell, this is never used
		idLib::Warning( "Using Diffuse Cube! Please contact Brian!" );
		
	}
	else if( pStage->texture.texgen == TG_GLASSWARP )
	{
	
		// As far as I can tell, this is never used
		idLib::Warning( "Using GlassWarp! Please contact Brian!" );
	}
	
	SetVertexParm( RENDERPARM_TEXGEN_0_ENABLED, useTexGenParm );
}

/*
================
idRenderBackend::FinishStageTexturing
================
*/
void idRenderBackend::FinishStageTexturing( const shaderStage_t* pStage, const drawSurf_t* surf )
{
	if( pStage->texture.cinematic )
	{
		// unbind the extra bink textures
		GL_SelectTexture( 0 );
	}
	
	if( pStage->texture.texgen == TG_REFLECT_CUBE )
	{
		// see if there is also a bump map specified
		const shaderStage_t* bumpStage = surf->material->GetBumpStage();
		if( bumpStage != NULL )
		{
			// per-pixel reflection mapping with bump mapping
			GL_SelectTexture( 0 );
		}
		else
		{
			// per-pixel reflection mapping without bump mapping
		}
		renderProgManager.Unbind();
	}
}

// RB: moved this up because we need to call this several times for shadow mapping
void idRenderBackend::ResetViewportAndScissorToDefaultCamera( const viewDef_t* _viewDef )
{
	// set the window clipping
	GL_Viewport( _viewDef->viewport.x1,
				 _viewDef->viewport.y1,
				 _viewDef->viewport.x2 + 1 - _viewDef->viewport.x1,
				 _viewDef->viewport.y2 + 1 - _viewDef->viewport.y1 );
				 
	// the scissor may be smaller than the viewport for subviews
	GL_Scissor( viewDef->viewport.x1 + _viewDef->scissor.x1,
				viewDef->viewport.y1 + _viewDef->scissor.y1,
				_viewDef->scissor.x2 + 1 - _viewDef->scissor.x1,
				_viewDef->scissor.y2 + 1 - _viewDef->scissor.y1 );
				
	currentScissor = viewDef->scissor;
}
// RB end

/*
==================
idRenderBackend::UseSceneFramebuffer

The 3D views draw into hdrFBO for HDR and for the on-tile deferred shading, whose G-buffer
is attached to it. Its color goes to the back buffer when the view is done: the tonemap or
DeferredResolve.
==================
*/
bool idRenderBackend::UseSceneFramebuffer() const
{
#if defined(ANDROID) && !defined(USE_VULKAN)
	return r_useHDR.GetBool() || deferredFrame;
#else
	return r_useHDR.GetBool();
#endif
}

/*
=========================================================================================

DEPTH BUFFER RENDERING

=========================================================================================
*/

#if defined(ANDROID) && !defined(USE_VULKAN)
static idCVar r_gpuInstancingDebug( "r_gpuInstancingDebug", "0", CVAR_BOOL, "Log GPU instancing batch statistics" );
static const int MAX_GPU_INSTANCES_PER_DRAW = 128;
static bool R_GPUInstanceable( const drawSurf_t* surf, bool allowPerforated = false )
{
	// merged world surfaces are drawn with their group (see WorldMergePass)
	return surf != NULL && surf->mergedGroup == 0 && surf->space != NULL && surf->material != NULL
		&& surf->shaderRegisters != NULL
		&& surf->ambientCache != 0 && surf->indexCache != 0
		&& surf->numIndexes > 0 && surf->jointCache == 0
		&& !surf->space->isGuiSurface
		&& ( surf->material->Coverage() == MC_OPAQUE
			|| ( allowPerforated && surf->material->Coverage() == MC_PERFORATED ) )
		&& surf->material->Deform() == DFRM_NONE
		&& surf->material->GetSort() != SS_SUBVIEW;
}
static bool R_CanBatchGPUInstances( const drawSurf_t* first, const drawSurf_t* candidate,
		bool requireSameSort = true, bool allowPerforated = false )
{
	// The batch builders ask this for every surface up to GPU_INSTANCE_BATCH_SCAN_AHEAD
	// places ahead, and almost none share the geometry: reject on the drawSurf fields
	// alone before anything follows the material and space pointers.
	if( candidate == NULL || first->ambientCache != candidate->ambientCache || first->indexCache != candidate->indexCache
		|| first->numIndexes != candidate->numIndexes || first->material != candidate->material )
	{
		return false;
	}

	if( !R_GPUInstanceable( first, allowPerforated ) || !R_GPUInstanceable( candidate, allowPerforated )
		|| ( requireSameSort && first->sort != candidate->sort )
		|| first->extraGLState != candidate->extraGLState
		|| !first->scissorRect.Equals( candidate->scissorRect )
		|| first->space->weaponDepthHack != candidate->space->weaponDepthHack
		|| first->space->modelDepthHack != candidate->space->modelDepthHack
		|| first->ambientCache != candidate->ambientCache || first->indexCache != candidate->indexCache
		|| first->numIndexes != candidate->numIndexes || first->material != candidate->material )
	{
		return false;
	}

	// Different entities can have separate register arrays. The batch is safe only
	// when the resulting material parameters are identical for every instance.
	const int numRegisters = first->material->GetNumRegisters();
	for( int i = 0; i < numRegisters; ++i )
	{
		if( first->shaderRegisters[i] != candidate->shaderRegisters[i] )
		{
			return false;
		}
	}
	return true;
}
static void R_FillGPUInstanceData( gpuInstanceData_t& instance, const drawSurf_t* surf, const idVec3& viewOrigin )
{
	memset( &instance, 0, sizeof( instance ) );
	memcpy( instance.mvp, surf->space->mvp[0], sizeof( instance.mvp ) );
	float modelViewMatrixTranspose[16];
	R_MatrixTranspose( surf->space->modelViewMatrix, modelViewMatrixTranspose );
	memcpy( instance.modelView, modelViewMatrixTranspose, sizeof( instance.modelView ) );
	idVec4 localViewOrigin( 1.0f );
	R_GlobalPointToLocal( surf->space->modelMatrix, viewOrigin, localViewOrigin.ToVec3() );
	memcpy( instance.localViewOrigin, localViewOrigin.ToFloatPtr(), sizeof( instance.localViewOrigin ) );
}
static const int GPU_INSTANCE_BATCH_SCAN_AHEAD = 512;
static int R_BuildGPUInstanceBatch( const drawSurf_t* const* drawSurfs, int numDrawSurfs, int firstIndex, const idVec3& viewOrigin, bool* consumed, gpuInstanceData_t* instances, bool allowPerforated = false, const drawSurf_t** instanceSurfs = NULL )
{
	if( glConfig.glVersion < 3.2f || !renderProgManager.GPUInstancingShadersReady() || firstIndex < 0 || firstIndex >= numDrawSurfs ) return 1;
	const drawSurf_t* first = drawSurfs[firstIndex];
	if( !R_GPUInstanceable( first, allowPerforated ) ) return 1;
	R_FillGPUInstanceData( instances[0], first, viewOrigin );
	if( instanceSurfs != NULL ) instanceSurfs[0] = first;
	int count = 1;

	// Repeated geometry is rarely adjacent in the draw-surface list. Search a
	// bounded window and only regroup surfaces with matching material/sort state.
	const int requestedEnd = firstIndex + GPU_INSTANCE_BATCH_SCAN_AHEAD + 1;
	const int scanEnd = requestedEnd < numDrawSurfs ? requestedEnd : numDrawSurfs;
	for( int i = firstIndex + 1; i < scanEnd && count < MAX_GPU_INSTANCES_PER_DRAW; ++i )
	{
		if( consumed[i] )
		{
			continue;
		}
		const drawSurf_t* candidate = drawSurfs[i];
		if( candidate == NULL )
		{
			continue;
		}
		if( !R_CanBatchGPUInstances( first, candidate, true, allowPerforated ) )
		{
			continue;
		}
		R_FillGPUInstanceData( instances[count], candidate, viewOrigin );
		if( instanceSurfs != NULL ) instanceSurfs[count] = candidate;
		consumed[i] = true;
		++count;
	}
	return count;
}

// Shadow maps use a per-light/cascade/face MVP rather than the camera MVP.
static void R_FillShadowGPUInstanceData( gpuInstanceData_t& instance, const drawSurf_t* surf,
		const idVec3& viewOrigin, const idRenderMatrix& lightViewRenderMatrix,
		const idRenderMatrix& lightProjectionRenderMatrix, const viewLight_t* vLight, int side )
{
	R_FillGPUInstanceData( instance, surf, viewOrigin );

	idRenderMatrix modelRenderMatrix;
	idRenderMatrix::Transpose( *( idRenderMatrix* )surf->space->modelMatrix, modelRenderMatrix );
	idRenderMatrix modelToLightRenderMatrix;
	idRenderMatrix::Multiply( lightViewRenderMatrix, modelRenderMatrix, modelToLightRenderMatrix );
	idRenderMatrix clipMVP;
	idRenderMatrix::Multiply( lightProjectionRenderMatrix, modelToLightRenderMatrix, clipMVP );

	if( !vLight->parallel && side < 0 )
	{
		idRenderMatrix MVP;
		idRenderMatrix::Multiply( renderMatrix_windowSpaceToClipSpace, clipMVP, MVP );
		memcpy( instance.mvp, MVP[0], sizeof( instance.mvp ) );
	}
	else
	{
		memcpy( instance.mvp, clipMVP[0], sizeof( instance.mvp ) );
	}
}

static int R_BuildShadowGPUInstanceBatch( const drawSurf_t* const* drawSurfs, int numDrawSurfs,
		int firstIndex, const idVec3& viewOrigin, bool* consumed, gpuInstanceData_t* instances,
		const idRenderMatrix& lightViewRenderMatrix, const idRenderMatrix& lightProjectionRenderMatrix,
		const viewLight_t* vLight, int side )
{
	if( glConfig.glVersion < 3.2f || !renderProgManager.GPUInstancingShadersReady() || firstIndex < 0 || firstIndex >= numDrawSurfs )
	{
		return 1;
	}

	const drawSurf_t* first = drawSurfs[firstIndex];
	if( !R_GPUInstanceable( first, true ) || first->shadowVolumeState != SHADOWVOLUME_DONE )
	{
		return 1;
	}

	R_FillShadowGPUInstanceData( instances[0], first, viewOrigin,
			lightViewRenderMatrix, lightProjectionRenderMatrix, vLight, side );
	int count = 1;
	const int requestedEnd = firstIndex + GPU_INSTANCE_BATCH_SCAN_AHEAD + 1;
	const int scanEnd = requestedEnd < numDrawSurfs ? requestedEnd : numDrawSurfs;
	for( int i = firstIndex + 1; i < scanEnd && count < MAX_GPU_INSTANCES_PER_DRAW; ++i )
	{
		if( consumed[i] )
		{
			continue;
		}
		const drawSurf_t* candidate = drawSurfs[i];
		// Let surfaces with unfinished async shadow work take their normal iteration.
		if( candidate == NULL || candidate->shadowVolumeState != SHADOWVOLUME_DONE
			|| !R_CanBatchGPUInstances( first, candidate, false, true ) )
		{
			continue;
		}

		R_FillShadowGPUInstanceData( instances[count], candidate, viewOrigin,
				lightViewRenderMatrix, lightProjectionRenderMatrix, vLight, side );
		consumed[i] = true;
		++count;
	}
	return count;
}

static bool R_CanBatchStencilShadowInstances( const drawSurf_t* first, const drawSurf_t* candidate )
{
	return first != NULL && candidate != NULL && first->space != NULL && candidate->space != NULL
		&& first->shadowCache != 0 && candidate->shadowCache == first->shadowCache
		&& candidate->indexCache == first->indexCache && candidate->numIndexes == first->numIndexes
		&& candidate->jointCache == 0 && first->jointCache == 0
		&& first->shadowVolumeState == SHADOWVOLUME_DONE && candidate->shadowVolumeState == SHADOWVOLUME_DONE
		&& candidate->renderZFail == first->renderZFail
		&& candidate->scissorRect.Equals( first->scissorRect )
		&& candidate->scissorRect.zmin == first->scissorRect.zmin
		&& candidate->scissorRect.zmax == first->scissorRect.zmax;
}

static int R_BuildStencilShadowGPUInstanceBatch( const drawSurf_t* const* drawSurfs, int numDrawSurfs,
		int firstIndex, const idVec3& viewOrigin, const idVec3& globalLightOrigin,
		bool* consumed, gpuInstanceData_t* instances )
{
	if( glConfig.glVersion < 3.2f || !renderProgManager.GPUInstancingShadersReady() || firstIndex < 0 || firstIndex >= numDrawSurfs ) return 1;
	const drawSurf_t* first = drawSurfs[firstIndex];
	if( first == NULL || first->space == NULL || first->jointCache != 0 || first->shadowCache == 0
		|| first->numIndexes <= 0 || first->shadowVolumeState != SHADOWVOLUME_DONE ) return 1;
	R_FillGPUInstanceData( instances[0], first, viewOrigin );
	idVec4 localLightOrigin( 0.0f );
	R_GlobalPointToLocal( first->space->modelMatrix, globalLightOrigin, localLightOrigin.ToVec3() );
	memcpy( instances[0].localLightOrigin, localLightOrigin.ToFloatPtr(), sizeof( instances[0].localLightOrigin ) );
	int count = 1;
	const int requestedEnd = firstIndex + GPU_INSTANCE_BATCH_SCAN_AHEAD + 1;
	const int scanEnd = requestedEnd < numDrawSurfs ? requestedEnd : numDrawSurfs;
	for( int i = firstIndex + 1; i < scanEnd && count < MAX_GPU_INSTANCES_PER_DRAW; ++i )
	{
		if( consumed[i] || !R_CanBatchStencilShadowInstances( first, drawSurfs[i] ) ) continue;
		const drawSurf_t* candidate = drawSurfs[i];
		R_FillGPUInstanceData( instances[count], candidate, viewOrigin );
		idVec4 instanceLocalLightOrigin( 0.0f );
		R_GlobalPointToLocal( candidate->space->modelMatrix, globalLightOrigin, instanceLocalLightOrigin.ToVec3() );
		memcpy( instances[count].localLightOrigin, instanceLocalLightOrigin.ToFloatPtr(), sizeof( instances[count].localLightOrigin ) );
		consumed[i] = true;
		++count;
	}
	return count;
}

static void R_FillInteractionGPUInstanceData( gpuInstanceData_t& instance, const drawSurf_t* surf,
		const idVec3& viewOrigin, const idVec3& globalLightOrigin, const idPlane globalLightProject[4],
		const float* lightTextureMatrix, bool hasLightTextureMatrix )
{
	R_FillGPUInstanceData( instance, surf, viewOrigin );

	idVec4 localLightOrigin( 0.0f );
	R_GlobalPointToLocal( surf->space->modelMatrix, globalLightOrigin, localLightOrigin.ToVec3() );
	memcpy( instance.localLightOrigin, localLightOrigin.ToFloatPtr(), sizeof( instance.localLightOrigin ) );

	idPlane localLightProject[4];
	for( int planeIndex = 0; planeIndex < 4; ++planeIndex )
	{
		R_GlobalPlaneToLocal( surf->space->modelMatrix, globalLightProject[planeIndex], localLightProject[planeIndex] );
	}
	if( hasLightTextureMatrix )
	{
		RB_BakeTextureMatrixIntoTexgen( localLightProject, lightTextureMatrix );
	}

	// The unshadowed interaction variants reinterpret these 4 vectors as local
	// light projection planes, not as the model-view matrix.
	for( int planeIndex = 0; planeIndex < 4; ++planeIndex )
	{
		memcpy( &instance.modelView[planeIndex * 4], localLightProject[planeIndex].ToFloatPtr(), 4 * sizeof( float ) );
	}
}


static void R_FillShadowInteractionGPUInstanceData( gpuInstanceData_t& instance, const drawSurf_t* surf,
		const idVec3& viewOrigin, const idVec3& globalLightOrigin )
{
	R_FillGPUInstanceData( instance, surf, viewOrigin );

	idVec4 localLightOrigin( 0.0f );
	R_GlobalPointToLocal( surf->space->modelMatrix, globalLightOrigin, localLightOrigin.ToVec3() );
	memcpy( instance.localLightOrigin, localLightOrigin.ToFloatPtr(), sizeof( instance.localLightOrigin ) );

	// Shadow-map interaction variants use modelView as the per-instance model
	// matrix. Light projection and shadow matrices are shared in world space.
	idRenderMatrix modelMatrix;
	idRenderMatrix::Transpose( *( idRenderMatrix* )surf->space->modelMatrix, modelMatrix );
	memcpy( instance.modelView, modelMatrix[0], sizeof( instance.modelView ) );
}

static void R_FillTexGenGPUInstanceData( gpuInstanceData_t& instance, const drawSurf_t* surf,
		const idVec3& viewOrigin, const idPlane globalPlanes[4] )
{
	R_FillGPUInstanceData( instance, surf, viewOrigin );
	idPlane localPlanes[4];
	for( int planeIndex = 0; planeIndex < 4; ++planeIndex )
	{
		R_GlobalPlaneToLocal( surf->space->modelMatrix, globalPlanes[planeIndex], localPlanes[planeIndex] );
		memcpy( &instance.modelView[planeIndex * 4], localPlanes[planeIndex].ToFloatPtr(), 4 * sizeof( float ) );
	}
}
static bool R_GPUShaderPassMaterialInstanceable( const idMaterial* material )
{
	if( material == NULL || material->GetSort() >= SS_POST_PROCESS
		|| ( material->Coverage() != MC_OPAQUE && material->Coverage() != MC_PERFORATED )
		|| material->Deform() != DFRM_NONE || material->HasSubview() )
	{
		return false;
	}
	for( int stageIndex = 0; stageIndex < material->GetNumStages(); ++stageIndex )
	{
		const shaderStage_t* stage = material->GetStage( stageIndex );
		if( stage->newStage != NULL || stage->texture.cinematic
			|| stage->texture.texgen != TG_EXPLICIT
			|| ( stage->drawStateBits & ( GLS_SRCBLEND_BITS | GLS_DSTBLEND_BITS ) ) != 0 )
		{
			return false;
		}
		if( r_useClShader.GetBool() && stage->texture.image != NULL
			&& !idStr::Cmp( stage->texture.image->GetName(), "_doomClassic" ) )
		{
			return false;
		}
	}
	return material->GetNumStages() > 0;
}

/*
==============================================================================================

WORLD SURFACE MERGING

dmap gives every BSP area its own model with one surface per material, so the
visible world costs a draw call per area and material in the depth pass, in the
ambient pass, for every light and every shadow map side, even where the areas
share their materials. All area models have the identity transform and keep
their geometry in the static vertex buffer: any set of their surfaces can be
drawn with one call once their indexes sit next to each other, rebased to the
start of that buffer.

Before the first command of a frame WorldMergePass collects, for every 3D view
- the opaque world surfaces, ordered by material: the depth pass draws all of
  them with one call, the ambient pass one call per material
- per light, the lit world surfaces of the same material: one interaction draw
  per material instead of one per area
- per fog and blend light, all of its world surfaces: one draw, the material
  plays no part in their drawing
- per light shadow map side, the opaque world casters: one draw
and writes their 32 bit indexes into one buffer for the frame. Only surfaces
the front end put into the view or the light are merged, so nothing is drawn
that wasn't drawn before, and the old static batching's merged entities, which
were drawn whenever any area they spanned was visible and added shadow volumes
on top of the prelight ones, are not needed.

Entity models stay out: a copy of them in world space would differ from what
their own transform makes of them by float rounding, and the passes that still
draw them that way (stencil shadow volumes, depth equal tests against the depth
pass) would flicker against it. World areas are drawn with the same vertexes and
the same MVP either way.

==============================================================================================
*/

idCVar r_mergeWorldSurfaces( "r_mergeWorldSurfaces", "1", CVAR_RENDERER | CVAR_BOOL, "draw the visible world surfaces of a material together: in the depth and ambient passes, for each light and each shadow map side" );
idCVar r_mergeWorldSurfacesDebug( "r_mergeWorldSurfacesDebug", "0", CVAR_RENDERER | CVAR_BOOL, "print what WorldMergePass merged every frame" );

static const int WORLD_MERGE_SHADOW_CASTER = 0x7fffffff;	// drawSurf_t::mergedGroup of a caster drawn with its light's merged casters
static const int WORLD_MERGE_MIN_RING_SIZE = 1024 * 1024;
static const int WORLD_MERGE_MAX_RING_SIZE = 16 * 1024 * 1024;
static const int WORLD_MERGE_MAX_GROUPS = 4096;
static const int WORLD_MERGE_MAX_VIEWS = 16;
static const int WORLD_MERGE_MAX_CASTER_LIGHTS = 128;

struct worldMergeGroup_t
{
	int					firstIndex;		// in the merge buffer of the frame
	int					numIndexes;
	int					drawnStamp;		// the pass that drew the group already
};

struct worldMergeView_t
{
	const viewDef_t*	viewDef;
	const viewEntity_t*	space;			// any of the view's world areas, they share the MVP
	int					firstIndex;		// all opaque world surfaces of the view
	int					numIndexes;
};

struct worldMergeCasters_t
{
	const viewLight_t*	vLight;
	int					firstIndex[6];	// per shadow map side, a projected light only has the first
	int					numIndexes[6];
};

struct worldMergeWriter_t
{
	uint32*				indexes;		// the mapped merge buffer
	int					capacity;		// in indexes
	int					used;
	int					wanted;			// what all merges of the frame needed, the buffer grows to it
};

// plain arrays: nothing here may run a destructor at exit()
static worldMergeGroup_t	s_worldMergeGroups[WORLD_MERGE_MAX_GROUPS];
static int					s_numWorldMergeGroups;
static worldMergeView_t		s_worldMergeViews[WORLD_MERGE_MAX_VIEWS];
static int					s_numWorldMergeViews;
static worldMergeCasters_t	s_worldMergeCasters[WORLD_MERGE_MAX_CASTER_LIGHTS];
static int					s_numWorldMergeCasters;
static int					s_worldMergeStamp;
static idGLBufferRing*		s_worldMergeRing;			// allocated on first use, destroyed by RB_ShutdownWorldMerge
static int					s_worldMergeRingSize;		// bytes per frame slot
static int					s_worldMergeWantedBytes;

// nothing of the last frame may be found by a frame WorldMergePass doesn't run for
static void R_ResetWorldMerge()
{
	s_numWorldMergeGroups = 0;
	s_numWorldMergeViews = 0;
	s_numWorldMergeCasters = 0;
}

void RB_ShutdownWorldMerge()
{
	if( s_worldMergeRing != NULL )
	{
		s_worldMergeRing->Destroy();
		delete s_worldMergeRing;
		s_worldMergeRing = NULL;
	}
	s_worldMergeRingSize = 0;
	s_numWorldMergeGroups = 0;
	s_numWorldMergeViews = 0;
	s_numWorldMergeCasters = 0;
}

// static world area geometry with a CPU copy of the indexes it draws
static bool R_WorldMergeable( const drawSurf_t* surf )
{
	if( surf->staticIndexes == NULL || surf->numIndexes <= 0 || surf->space == NULL || !surf->space->staticWorldArea
			|| surf->jointCache != 0 || surf->material == NULL
			|| !vertexCache.CacheIsStatic( surf->ambientCache ) || !vertexCache.CacheIsStatic( surf->indexCache )
			|| surf->shadowVolumeState != SHADOWVOLUME_DONE
			|| surf->material->Deform() != DFRM_NONE || surf->material->TestMaterialFlag( MF_POLYGONOFFSET ) )
	{
		return false;
	}

	// the merged indexes count idDrawVerts from the start of the static vertex buffer
	const int vertOffset = ( int )( surf->ambientCache >> VERTCACHE_OFFSET_SHIFT ) & VERTCACHE_OFFSET_MASK;
	return ( vertOffset % ( int )sizeof( idDrawVert ) ) == 0;
}

// surfaces drawn with exactly the same material state
static bool R_WorldMergeSameMaterial( const drawSurf_t* a, const drawSurf_t* b )
{
	if( a->material != b->material )
	{
		return false;
	}
	if( a->shaderRegisters == b->shaderRegisters )
	{
		return true;
	}
	if( a->shaderRegisters == NULL || b->shaderRegisters == NULL )
	{
		return false;
	}
	return memcmp( a->shaderRegisters, b->shaderRegisters, a->material->GetNumRegisters() * sizeof( float ) ) == 0;
}

// the indexes of the surfaces, rebased to the start of the static vertex buffer
static int R_WorldMergeWrite( uint32* out, const drawSurf_t* const* surfs, int numSurfs )
{
	int written = 0;
	for( int i = 0; i < numSurfs; i++ )
	{
		const drawSurf_t* surf = surfs[i];
		const uint32 baseVertex = ( uint32 )( ( ( surf->ambientCache >> VERTCACHE_OFFSET_SHIFT ) & VERTCACHE_OFFSET_MASK ) / sizeof( idDrawVert ) );
		const triIndex_t* in = surf->staticIndexes;
		uint32* dst = out + written;
		const int numIndexes = surf->numIndexes;
		for( int j = 0; j < numIndexes; j++ )
		{
			dst[j] = baseVertex + in[j];
		}
		written += numIndexes;
	}
	return written;
}

/*
=====================
R_WorldMergeGroups

Groups the surfaces by material, or all in one when their drawing ignores the
material, and writes every group with at least minGroupSize surfaces that still
fits, consecutively. A surface of a written group gets the group in
drawSurf_t::mergedGroup.
=====================
*/
static void R_WorldMergeGroups( worldMergeWriter_t& writer, const drawSurf_t** surfs, int numSurfs, int minGroupSize, bool byMaterial,
								int& firstIndex, int& numIndexes )
{
	firstIndex = writer.used;
	numIndexes = 0;

	int* groupOf = ( int* )_alloca( numSurfs * sizeof( int ) );
	const drawSurf_t** groupFirst = ( const drawSurf_t** )_alloca( numSurfs * sizeof( const drawSurf_t* ) );
	int* groupSurfs = ( int* )_alloca( numSurfs * sizeof( int ) );
	int* groupIndexes = ( int* )_alloca( numSurfs * sizeof( int ) );
	const drawSurf_t** members = ( const drawSurf_t** )_alloca( numSurfs * sizeof( const drawSurf_t* ) );
	int numGroups = 0;

	for( int i = 0; i < numSurfs; i++ )
	{
		int group = 0;
		if( byMaterial )
		{
			while( group < numGroups && !R_WorldMergeSameMaterial( groupFirst[group], surfs[i] ) )
			{
				group++;
			}
		}
		if( group == numGroups )
		{
			groupFirst[group] = surfs[i];
			groupSurfs[group] = 0;
			groupIndexes[group] = 0;
			numGroups++;
		}
		groupOf[i] = group;
		groupSurfs[group]++;
		groupIndexes[group] += surfs[i]->numIndexes;
	}

	for( int group = 0; group < numGroups; group++ )
	{
		if( groupSurfs[group] < minGroupSize )
		{
			continue;
		}

		writer.wanted += groupIndexes[group];
		if( s_numWorldMergeGroups == WORLD_MERGE_MAX_GROUPS || writer.used + groupIndexes[group] > writer.capacity )
		{
			// these surfaces are drawn one by one as before
			continue;
		}

		int numMembers = 0;
		for( int i = 0; i < numSurfs; i++ )
		{
			if( groupOf[i] == group )
			{
				members[numMembers++] = surfs[i];
			}
		}

		worldMergeGroup_t& mergeGroup = s_worldMergeGroups[s_numWorldMergeGroups++];
		mergeGroup.firstIndex = writer.used;
		mergeGroup.numIndexes = R_WorldMergeWrite( writer.indexes + writer.used, members, numMembers );
		mergeGroup.drawnStamp = 0;
		writer.used += mergeGroup.numIndexes;
		numIndexes += mergeGroup.numIndexes;

		for( int i = 0; i < numMembers; i++ )
		{
			const_cast<drawSurf_t*>( members[i] )->mergedGroup = s_numWorldMergeGroups;
		}
	}
}

// the lit world surfaces of a light chain, merged per material
static void R_WorldMergeInteractions( worldMergeWriter_t& writer, const drawSurf_t* chain )
{
	int numChain = 0;
	for( const drawSurf_t* surf = chain; surf != NULL; surf = surf->nextOnLight )
	{
		numChain++;
	}
	if( numChain < 2 )
	{
		return;
	}

	const drawSurf_t** candidates = ( const drawSurf_t** )_alloca( numChain * sizeof( const drawSurf_t* ) );
	int numCandidates = 0;
	for( const drawSurf_t* surf = chain; surf != NULL; surf = surf->nextOnLight )
	{
		if( R_WorldMergeable( surf ) )
		{
			candidates[numCandidates++] = surf;
		}
	}

	if( numCandidates >= 2 )
	{
		// a material lit on one area only keeps its own draw
		int firstIndex, numIndexes;
		R_WorldMergeGroups( writer, candidates, numCandidates, 2, true, firstIndex, numIndexes );
	}
}

// the world surfaces of a fog or blend light: FogPass and BlendLight draw both chains with one state
static void R_WorldMergeFogLight( worldMergeWriter_t& writer, const drawSurf_t* chain1, const drawSurf_t* chain2 )
{
	const drawSurf_t* chains[2] = { chain1, chain2 };
	int numChain = 0;
	for( int c = 0; c < 2; c++ )
	{
		for( const drawSurf_t* surf = chains[c]; surf != NULL; surf = surf->nextOnLight )
		{
			numChain++;
		}
	}
	if( numChain < 2 )
	{
		return;
	}

	const drawSurf_t** candidates = ( const drawSurf_t** )_alloca( numChain * sizeof( const drawSurf_t* ) );
	int numCandidates = 0;
	for( int c = 0; c < 2; c++ )
	{
		for( const drawSurf_t* surf = chains[c]; surf != NULL; surf = surf->nextOnLight )
		{
			// T_BasicFog and T_BlendLight skip the surfaces without a scissor rect
			if( !surf->scissorRect.IsEmpty() && R_WorldMergeable( surf ) )
			{
				candidates[numCandidates++] = surf;
			}
		}
	}

	if( numCandidates >= 2 )
	{
		int firstIndex, numIndexes;
		R_WorldMergeGroups( writer, candidates, numCandidates, 2, false, firstIndex, numIndexes );
	}
}

// the opaque world casters of a light's shadow map sides
static void R_WorldMergeCasters( worldMergeWriter_t& writer, const viewLight_t* vLight )
{
	int numChain = 0;
	for( const drawSurf_t* surf = vLight->globalShadows; surf != NULL; surf = surf->nextOnLight )
	{
		numChain++;
	}
	if( numChain < 2 || s_numWorldMergeCasters == WORLD_MERGE_MAX_CASTER_LIGHTS )
	{
		return;
	}

	// alpha tested casters need their own texture state
	const drawSurf_t** casters = ( const drawSurf_t** )_alloca( numChain * sizeof( const drawSurf_t* ) );
	int numCasters = 0;
	for( const drawSurf_t* surf = vLight->globalShadows; surf != NULL; surf = surf->nextOnLight )
	{
		if( surf->material != NULL && surf->material->Coverage() == MC_OPAQUE && R_WorldMergeable( surf ) )
		{
			casters[numCasters++] = surf;
		}
	}
	if( numCasters < 2 )
	{
		return;
	}

	const bool pointLightSides = vLight->pointLight && !vLight->parallel;
	int numSides = 1;
	if( vLight->parallel )
	{
		numSides = Min( r_shadowMapSplits.GetInteger() + 1, 6 );
	}
	else if( vLight->pointLight )
	{
		numSides = 6;
	}

	// all sides fit, or the light keeps drawing its casters one by one
	int total = 0;
	for( int side = 0; side < numSides; side++ )
	{
		for( int i = 0; i < numCasters; i++ )
		{
			if( !pointLightSides || !( casters[i]->shadowMapCulledSides & ( 1 << side ) ) )
			{
				total += casters[i]->numIndexes;
			}
		}
	}
	writer.wanted += total;
	if( writer.used + total > writer.capacity )
	{
		return;
	}

	worldMergeCasters_t& merged = s_worldMergeCasters[s_numWorldMergeCasters++];
	merged.vLight = vLight;

	const drawSurf_t** sideCasters = ( const drawSurf_t** )_alloca( numCasters * sizeof( const drawSurf_t* ) );
	for( int side = 0; side < 6; side++ )
	{
		merged.firstIndex[side] = writer.used;
		merged.numIndexes[side] = 0;
		if( side >= numSides )
		{
			continue;
		}

		int numSideCasters = 0;
		for( int i = 0; i < numCasters; i++ )
		{
			if( !pointLightSides || !( casters[i]->shadowMapCulledSides & ( 1 << side ) ) )
			{
				sideCasters[numSideCasters++] = casters[i];
			}
		}
		merged.numIndexes[side] = R_WorldMergeWrite( writer.indexes + writer.used, sideCasters, numSideCasters );
		writer.used += merged.numIndexes[side];
	}

	for( int i = 0; i < numCasters; i++ )
	{
		const_cast<drawSurf_t*>( casters[i] )->mergedGroup = WORLD_MERGE_SHADOW_CASTER;
	}
}

static const worldMergeView_t* R_FindWorldMergeView( const viewDef_t* viewDef )
{
	for( int i = 0; i < s_numWorldMergeViews; i++ )
	{
		if( s_worldMergeViews[i].viewDef == viewDef )
		{
			return &s_worldMergeViews[i];
		}
	}
	return NULL;
}

static const worldMergeCasters_t* R_FindWorldMergeCasters( const viewLight_t* vLight )
{
	for( int i = 0; i < s_numWorldMergeCasters; i++ )
	{
		if( s_worldMergeCasters[i].vLight == vLight )
		{
			return &s_worldMergeCasters[i];
		}
	}
	return NULL;
}

/*
=====================
idRenderBackend::WorldMergePass
=====================
*/
void idRenderBackend::WorldMergePass( const emptyCommand_t* cmds )
{
	s_numWorldMergeGroups = 0;
	s_numWorldMergeViews = 0;
	s_numWorldMergeCasters = 0;
	worldMergeSurf = NULL;
	worldMergeIndexBuffer = 0;

	if( !r_mergeWorldSurfaces.GetBool() || r_skipRender.GetBool() )
	{
		return;
	}

	bool anyView = false;
	for( const emptyCommand_t* cmd = cmds; cmd != NULL && !anyView; cmd = ( const emptyCommand_t* )cmd->next )
	{
		if( cmd->commandId == RC_DRAW_VIEW_3D )
		{
			const viewDef_t* view = ( ( const drawSurfsCommand_t* )cmd )->viewDef;
			anyView = ( view != NULL && view->viewEntitys != NULL && view->numDrawSurfs > 0 );
		}
	}
	if( !anyView )
	{
		return;
	}

	// the buffer grows to what the frames need
	const int wantedSize = Min( Max( WORLD_MERGE_MIN_RING_SIZE, idMath::CeilPowerOfTwo( s_worldMergeWantedBytes ) ), WORLD_MERGE_MAX_RING_SIZE );
	if( s_worldMergeRing == NULL || s_worldMergeRingSize < wantedSize )
	{
		if( s_worldMergeRing == NULL )
		{
			s_worldMergeRing = new idGLBufferRing;
		}
		else
		{
			s_worldMergeRing->Destroy();
		}

		// managed through the copy target, binding it as element buffer would change the VAO
		if( !s_worldMergeRing->Create( GL_COPY_WRITE_BUFFER, wantedSize, R_HasBufferStorage() ) )
		{
			delete s_worldMergeRing;
			s_worldMergeRing = NULL;
			s_worldMergeRingSize = 0;
			return;
		}
		s_worldMergeRingSize = wantedSize;
		idLib::Printf( "World surface merging: %d KB of indexes per frame, %s\n", wantedSize / 1024,
					   s_worldMergeRing->IsPersistent() ? "persistently mapped" : "mapped every frame" );
	}

	const int slot = g_frameFences.CurrentSlot();
	uint32* mapped = ( uint32* )s_worldMergeRing->MapForWrite( slot );
	if( mapped == NULL )
	{
		return;
	}

	worldMergeWriter_t writer;
	writer.indexes = mapped;
	writer.capacity = s_worldMergeRingSize / sizeof( uint32 );
	writer.used = 0;
	writer.wanted = 0;

	const bool mergeCasters = r_useShadowMapping.GetBool() && !r_skipShadows.GetBool();

	for( const emptyCommand_t* cmd = cmds; cmd != NULL && s_numWorldMergeViews < WORLD_MERGE_MAX_VIEWS; cmd = ( const emptyCommand_t* )cmd->next )
	{
		if( cmd->commandId != RC_DRAW_VIEW_3D )
		{
			continue;
		}
		const viewDef_t* view = ( ( const drawSurfsCommand_t* )cmd )->viewDef;
		if( view == NULL || view->viewEntitys == NULL || view->numDrawSurfs == 0 )
		{
			continue;
		}

		// the opaque world surfaces of the view, for the depth and the ambient pass
		const drawSurf_t** candidates = ( const drawSurf_t** )_alloca( view->numDrawSurfs * sizeof( const drawSurf_t* ) );
		int numCandidates = 0;
		const viewEntity_t* worldSpace = NULL;
		for( int i = 0; i < view->numDrawSurfs; i++ )
		{
			const drawSurf_t* surf = view->drawSurfs[i];
			if( surf->material != NULL && surf->material->Coverage() == MC_OPAQUE && surf->material->GetSort() != SS_SUBVIEW && R_WorldMergeable( surf ) )
			{
				candidates[numCandidates++] = surf;
				worldSpace = surf->space;
			}
		}
		if( numCandidates >= 2 )
		{
			int firstIndex, numIndexes;
			R_WorldMergeGroups( writer, candidates, numCandidates, 1, true, firstIndex, numIndexes );
			if( numIndexes > 0 )
			{
				worldMergeView_t& mergeView = s_worldMergeViews[s_numWorldMergeViews++];
				mergeView.viewDef = view;
				mergeView.space = worldSpace;
				mergeView.firstIndex = firstIndex;
				mergeView.numIndexes = numIndexes;
			}
		}

		// the lights, as DrawInteractions, FogAllLights and the shadow maps draw them
		for( const viewLight_t* vLight = view->viewLights; vLight != NULL; vLight = vLight->next )
		{
			if( vLight->lightShader->IsFogLight() || vLight->lightShader->IsBlendLight() )
			{
				if( !r_skipFogLights.GetBool() )
				{
					R_WorldMergeFogLight( writer, vLight->globalInteractions, vLight->localInteractions );
				}
				continue;
			}

			R_WorldMergeInteractions( writer, vLight->localInteractions );
			R_WorldMergeInteractions( writer, vLight->globalInteractions );

			if( mergeCasters && vLight->globalShadows != NULL && !vLight->lightShader->IsAmbientLight() )
			{
				R_WorldMergeCasters( writer, vLight );
			}
		}
	}

	s_worldMergeRing->Unmap( slot, writer.used * sizeof( uint32 ) );
	s_worldMergeWantedBytes = writer.wanted * ( int )sizeof( uint32 );
	worldMergeIndexBuffer = ( writer.used > 0 ) ? s_worldMergeRing->Buffer( slot ) : 0;

	if( r_mergeWorldSurfacesDebug.GetBool() )
	{
		idLib::Printf( "World surface merging: %d views, %d material groups, %d lights' casters, %d of %d indexes fit\n",
					   s_numWorldMergeViews, s_numWorldMergeGroups, s_numWorldMergeCasters, writer.used, writer.wanted );
	}
}

/*
=====================
idRenderBackend::BeginWorldMergeGroup

The next draws of surf draw its whole merged group
=====================
*/
void idRenderBackend::BeginWorldMergeGroup( const drawSurf_t* surf )
{
	const worldMergeGroup_t& group = s_worldMergeGroups[surf->mergedGroup - 1];
	worldMergeSurf = surf;
	worldMergeFirstIndex = group.firstIndex;
	worldMergeNumIndexes = group.numIndexes;
}

// a member of one of this frame's merged material groups
static bool R_IsWorldMergeGroupMember( const drawSurf_t* surf )
{
	return surf->mergedGroup > 0 && surf->mergedGroup <= s_numWorldMergeGroups;
}

// true when surf belongs to a merged group another surface of the pass drew already;
// otherwise the group, if any, is drawn with surf's draws
static bool R_WorldMergeSkip( const drawSurf_t* surf )
{
	if( !R_IsWorldMergeGroupMember( surf ) )
	{
		return false;
	}
	worldMergeGroup_t& group = s_worldMergeGroups[surf->mergedGroup - 1];
	if( group.drawnStamp == s_worldMergeStamp )
	{
		return true;
	}
	group.drawnStamp = s_worldMergeStamp;
	return false;
}

#endif


/*
==================
idRenderBackend::FillDepthBufferGeneric
==================
*/
void idRenderBackend::FillDepthBufferGeneric( const drawSurf_t* const* drawSurfs, int numDrawSurfs )
{
#if defined(ANDROID) && !defined(USE_VULKAN)
	bool* gpuGenericConsumed = (bool*)_alloca( numDrawSurfs * sizeof( bool ) );
	memset( gpuGenericConsumed, 0, numDrawSurfs * sizeof( bool ) );
	int gpuGenericBatchCount = 0;
	int gpuGenericDrawCallsSaved = 0;
#endif
	for( int i = 0; i < numDrawSurfs; i++ )
	{
#if defined(ANDROID) && !defined(USE_VULKAN)
		if( gpuGenericConsumed[i] )
		{
			continue;
		}
		gpuInstanceData_t gpuGenericInstances[MAX_GPU_INSTANCES_PER_DRAW];
		const gpuInstanceData_t* gpuGenericInstanceData = NULL;
		int gpuGenericInstanceCount = 1;
#endif
		const drawSurf_t* drawSurf = drawSurfs[i];
		const idMaterial* shader = drawSurf->material;
		
		// translucent surfaces don't put anything in the depth buffer and don't
		// test against it, which makes them fail the mirror clip plane operation
		if( shader->Coverage() == MC_TRANSLUCENT )
		{
			continue;
		}
		
		// get the expressions for conditionals / color / texcoords
		const float* regs = drawSurf->shaderRegisters;
		
		// if all stages of a material have been conditioned off, don't do anything
		int stage = 0;
		for( ; stage < shader->GetNumStages(); stage++ )
		{
			const shaderStage_t* pStage = shader->GetStage( stage );
			// check the stage enable condition
			if( regs[ pStage->conditionRegister ] != 0 )
			{
				break;
			}
		}
		if( stage == shader->GetNumStages() )
		{
			continue;
		}

#if defined(ANDROID) && !defined(USE_VULKAN)
		if( glConfig.glVersion >= 3.2f && shader->Coverage() == MC_PERFORATED )
		{
			gpuGenericInstanceCount = R_BuildGPUInstanceBatch( drawSurfs, numDrawSurfs, i,
					viewDef->renderView.vieworg, gpuGenericConsumed, gpuGenericInstances, true );
			if( gpuGenericInstanceCount > 1 )
			{
				gpuGenericInstanceData = gpuGenericInstances;
				++gpuGenericBatchCount;
			}
		}
#endif
		
		// change the matrix if needed
		if( drawSurf->space != currentSpace )
		{
			RB_SetMVP( drawSurf->space->mvp );
			
			currentSpace = drawSurf->space;
		}
		
		uint64 surfGLState = 0;
		
		// set polygon offset if necessary
		if( shader->TestMaterialFlag( MF_POLYGONOFFSET ) )
		{
			surfGLState |= GLS_POLYGON_OFFSET;
			GL_PolygonOffset( r_offsetFactor.GetFloat(), r_offsetUnits.GetFloat() * shader->GetPolygonOffset() );
		}
		
		// subviews will just down-modulate the color buffer
		idVec4 color;
		if( shader->GetSort() == SS_SUBVIEW )
		{
			surfGLState |= GLS_SRCBLEND_DST_COLOR | GLS_DSTBLEND_ZERO | GLS_DEPTHFUNC_LESS;
			color[0] = 1.0f;
			color[1] = 1.0f;
			color[2] = 1.0f;
			color[3] = 1.0f;
		}
		else
		{
			// others just draw black
			color[0] = 0.0f;
			color[1] = 0.0f;
			color[2] = 0.0f;
			color[3] = 1.0f;
		}
		
		renderLog.OpenBlock( shader->GetName() );
		
		bool drawSolid = false;
		if( shader->Coverage() == MC_OPAQUE )
		{
			drawSolid = true;
		}
		else if( shader->Coverage() == MC_PERFORATED )
		{
			// we may have multiple alpha tested stages
			// if the only alpha tested stages are condition register omitted,
			// draw a normal opaque surface
			bool didDraw = false;
			
			// perforated surfaces may have multiple alpha tested stages
			for( stage = 0; stage < shader->GetNumStages(); stage++ )
			{
				const shaderStage_t* pStage = shader->GetStage( stage );
				
				if( !pStage->hasAlphaTest )
				{
					continue;
				}
				
				// check the stage enable condition
				if( regs[ pStage->conditionRegister ] == 0 )
				{
					continue;
				}
				
				// if we at least tried to draw an alpha tested stage,
				// we won't draw the opaque surface
				didDraw = true;
				
				// set the alpha modulate
				color[3] = regs[ pStage->color.registers[3] ];
				
				// skip the entire stage if alpha would be black
				if( color[3] <= 0.0f )
				{
					continue;
				}
				
				uint64 stageGLState = surfGLState;
				
				// set privatePolygonOffset if necessary
				if( pStage->privatePolygonOffset )
				{
					GL_PolygonOffset( r_offsetFactor.GetFloat(), r_offsetUnits.GetFloat() * pStage->privatePolygonOffset );
					stageGLState |= GLS_POLYGON_OFFSET;
				}
				
				GL_Color( color );
				
				GL_State( stageGLState );
				idVec4 alphaTestValue( regs[ pStage->alphaTestRegister ] );
				SetFragmentParm( RENDERPARM_ALPHA_TEST, alphaTestValue.ToFloatPtr() );
				
				if( drawSurf->jointCache )
				{
					renderProgManager.BindShader_TextureVertexColorSkinned();
				}
				else
				{
#if defined(ANDROID) && !defined(USE_VULKAN)
					if( gpuGenericInstanceCount > 1 )
					{
						renderProgManager.BindShader_TextureVertexColorInstanced();
					}
					else
#endif
					{
						renderProgManager.BindShader_TextureVertexColor();
					}
				}
				
				RB_SetVertexColorParms( SVC_IGNORE );
				
				// bind the texture
				GL_SelectTexture( 0 );
				pStage->texture.image->Bind();
				
				// set texture matrix and texGens
				PrepareStageTexturing( pStage, drawSurf );
				
				// must render with less-equal for Z-Cull to work properly
				assert( ( GL_GetCurrentState() & GLS_DEPTHFUNC_BITS ) == GLS_DEPTHFUNC_LESS );
				
				// draw it
#if defined(ANDROID) && !defined(USE_VULKAN)
				DrawElementsWithCounters( drawSurf, gpuGenericInstanceData, gpuGenericInstanceCount );
				if( gpuGenericInstanceCount > 1 )
				{
					gpuGenericDrawCallsSaved += gpuGenericInstanceCount - 1;
				}
#else
				DrawElementsWithCounters( drawSurf );
#endif
				
				// clean up
				FinishStageTexturing( pStage, drawSurf );
				
				// unset privatePolygonOffset if necessary
				if( pStage->privatePolygonOffset )
				{
					GL_PolygonOffset( r_offsetFactor.GetFloat(), r_offsetUnits.GetFloat() * shader->GetPolygonOffset() );
				}
			}
			
			if( !didDraw )
			{
				drawSolid = true;
			}
		}
		
		// draw the entire surface solid
		if( drawSolid )
		{
			if( shader->GetSort() == SS_SUBVIEW )
			{
				renderProgManager.BindShader_Color();
				GL_Color( color );
				GL_State( surfGLState );
			}
			else
			{
				if( drawSurf->jointCache )
				{
					renderProgManager.BindShader_DepthSkinned();
				}
				else
				{
#if defined(ANDROID) && !defined(USE_VULKAN)
					if( gpuGenericInstanceCount > 1 )
					{
						renderProgManager.BindShader_DepthInstanced();
					}
					else
#endif
					{
						renderProgManager.BindShader_Depth();
					}
				}
				GL_State( surfGLState | GLS_ALPHAMASK );
			}
			
			// must render with less-equal for Z-Cull to work properly
			assert( ( GL_GetCurrentState() & GLS_DEPTHFUNC_BITS ) == GLS_DEPTHFUNC_LESS );
			
			// draw it
#if defined(ANDROID) && !defined(USE_VULKAN)
			DrawElementsWithCounters( drawSurf, gpuGenericInstanceData, gpuGenericInstanceCount );
			if( gpuGenericInstanceCount > 1 )
			{
				gpuGenericDrawCallsSaved += gpuGenericInstanceCount - 1;
			}
#else
			DrawElementsWithCounters( drawSurf );
#endif
		}
		
		renderLog.CloseBlock();
	}
	
#if defined(ANDROID) && !defined(USE_VULKAN)
	if( r_gpuInstancingDebug.GetBool() && numDrawSurfs > 1 && gpuGenericBatchCount > 0 )
	{
		idLib::Printf( "GPU instancing generic depth pass: %d batches, %d draw calls saved\n",
				gpuGenericBatchCount, gpuGenericDrawCallsSaved );
	}
#endif
	SetFragmentParm( RENDERPARM_ALPHA_TEST, vec4_zero.ToFloatPtr() );
}

/*
=====================
idRenderBackend::FillDepthBufferFast

Optimized fast path code.

If there are subview surfaces, they must be guarded in the depth buffer to allow
the mirror / subview to show through underneath the current view rendering.

Surfaces with perforated shaders need the full shader setup done, but should be
drawn after the opaque surfaces.

The bulk of the surfaces should be simple opaque geometry that can be drawn very rapidly.

If there are no subview surfaces, we could clear to black and use fast-Z rendering
on the 360.
=====================
*/
void idRenderBackend::FillDepthBufferFast( drawSurf_t** drawSurfs, int numDrawSurfs )
{
	if( numDrawSurfs == 0 )
	{
		return;
	}
	
	// if we are just doing 2D rendering, no need to fill the depth buffer
	if( viewDef->viewEntitys == NULL )
	{
		return;
	}

#if defined(ANDROID) && !defined(USE_VULKAN)
	bool* gpuInstanceConsumed = (bool*)_alloca( numDrawSurfs * sizeof(bool) );
	memset( gpuInstanceConsumed, 0, numDrawSurfs * sizeof(bool) );
	int gpuInstanceBatchCount = 0;
	int gpuInstanceDrawCallsSaved = 0;
#endif
	
	renderLog.OpenMainBlock( MRB_FILL_DEPTH_BUFFER );
	renderLog.OpenBlock( "RB_FillDepthBufferFast" );
	
	// force MVP change on first surface
	currentSpace = NULL;
	
	// draw all the subview surfaces, which will already be at the start of the sorted list,
	// with the general purpose path
	GL_State( GLS_DEFAULT );
	
	int	surfNum;
	for( surfNum = 0; surfNum < numDrawSurfs; surfNum++ )
	{
		if( drawSurfs[surfNum]->material->GetSort() != SS_SUBVIEW )
		{
			break;
		}
		
		FillDepthBufferGeneric( &drawSurfs[surfNum], 1 );
	}
	
	const drawSurf_t** perforatedSurfaces = ( const drawSurf_t** )_alloca( numDrawSurfs * sizeof( drawSurf_t* ) );
	int numPerforatedSurfaces = 0;
	
	// draw all the opaque surfaces and build up a list of perforated surfaces that
	// we will defer drawing until all opaque surfaces are done
	GL_State( GLS_DEFAULT );

#if defined(ANDROID) && !defined(USE_VULKAN)
	// the opaque world surfaces of all visible areas, all materials, with one call
	const worldMergeView_t* mergedView = R_FindWorldMergeView( viewDef );
	if( mergedView != NULL )
	{
		renderLog.OpenBlock( "Merged World Surfaces" );

		renderProgManager.BindShader_Depth();
		RB_SetMVP( mergedView->space->mvp );
		currentSpace = mergedView->space;
		DrawWorldMergeRange( mergedView->firstIndex, mergedView->numIndexes );

		renderLog.CloseBlock();
	}
#endif

	// continue checking past the subview surfaces
	for( ; surfNum < numDrawSurfs; surfNum++ )
	{
#if defined(ANDROID) && !defined(USE_VULKAN)
		if( gpuInstanceConsumed[surfNum] )
		{
			continue;
		}
#endif
		const drawSurf_t* surf = drawSurfs[ surfNum ];
		const idMaterial* shader = surf->material;

		// translucent surfaces don't put anything in the depth buffer
		if( shader->Coverage() == MC_TRANSLUCENT )
		{
			continue;
		}

#if defined(ANDROID) && !defined(USE_VULKAN)
		// drawn with the merged world surfaces above
		if( mergedView != NULL && R_IsWorldMergeGroupMember( surf ) )
		{
			continue;
		}
#endif
		if( shader->Coverage() == MC_PERFORATED )
		{
			// save for later drawing
			perforatedSurfaces[ numPerforatedSurfaces ] = surf;
			numPerforatedSurfaces++;
			continue;
		}
		
		// Batch only identical static opaque meshes.
#if defined(ANDROID) && !defined(USE_VULKAN)
		gpuInstanceData_t gpuInstances[MAX_GPU_INSTANCES_PER_DRAW];
		const gpuInstanceData_t* gpuInstanceData = NULL;
		int gpuInstanceCount = 1;
		if( glConfig.glVersion >= 3.2f )
		{
			gpuInstanceCount = R_BuildGPUInstanceBatch( (const drawSurf_t* const*)drawSurfs, numDrawSurfs, surfNum, viewDef->renderView.vieworg, gpuInstanceConsumed, gpuInstances );
			if( gpuInstanceCount > 1 )
			{
				gpuInstanceData = gpuInstances;
				++gpuInstanceBatchCount;
				gpuInstanceDrawCallsSaved += gpuInstanceCount - 1;
			}
		}
#endif
		// set polygon offset?
		
		// set mvp matrix
		if( surf->space != currentSpace )
		{
			RB_SetMVP( surf->space->mvp );
			currentSpace = surf->space;
		}
		
		renderLog.OpenBlock( shader->GetName() );
		
		if( surf->jointCache )
		{
			renderProgManager.BindShader_DepthSkinned();
		}
		else
		{
#if defined(ANDROID) && !defined(USE_VULKAN)
			if( gpuInstanceCount > 1 ) renderProgManager.BindShader_DepthInstanced();
			else
#endif
			{
				renderProgManager.BindShader_Depth();
			}
		}
		// must render with less-equal for Z-Cull to work properly
		assert( ( GL_GetCurrentState() & GLS_DEPTHFUNC_BITS ) == GLS_DEPTHFUNC_LESS );
		// draw it solid
#if defined(ANDROID) && !defined(USE_VULKAN)
		DrawElementsWithCounters( surf, gpuInstanceData, gpuInstanceCount );
#else
		DrawElementsWithCounters( surf );
#endif
		
		renderLog.CloseBlock();
	}
	
	// draw all perforated surfaces with the general code path
	if( numPerforatedSurfaces > 0 )
	{
		FillDepthBufferGeneric( perforatedSurfaces, numPerforatedSurfaces );
	}

#if defined(ANDROID) && !defined(USE_VULKAN)
	if( r_gpuInstancingDebug.GetBool() )
	{
		idLib::Printf( "GPU instancing depth pass: %d batches, %d draw calls saved\n", gpuInstanceBatchCount, gpuInstanceDrawCallsSaved );
	}
#endif
	
	renderLog.CloseBlock();
	renderLog.CloseMainBlock();
}

/*
=========================================================================================

GENERAL INTERACTION RENDERING

=========================================================================================
*/

const int INTERACTION_TEXUNIT_BUMP			= 0;
const int INTERACTION_TEXUNIT_SPECULARMIX	= 1;
const int INTERACTION_TEXUNIT_BASECOLOR		= 2;
const int INTERACTION_TEXUNIT_FALLOFF		= 3;
const int INTERACTION_TEXUNIT_PROJECTION	= 4;
const int INTERACTION_TEXUNIT_SHADOWMAPS	= 5;
const int INTERACTION_TEXUNIT_JITTER		= 6;

/*
==================
idRenderBackend::SetupInteractionStage
==================
*/
void idRenderBackend::SetupInteractionStage( const shaderStage_t* surfaceStage, const float* surfaceRegs, const float lightColor[4],
		idVec4 matrix[2], float color[4] )
{

	if( surfaceStage->texture.hasMatrix )
	{
		matrix[0][0] = surfaceRegs[surfaceStage->texture.matrix[0][0]];
		matrix[0][1] = surfaceRegs[surfaceStage->texture.matrix[0][1]];
		matrix[0][2] = 0.0f;
		matrix[0][3] = surfaceRegs[surfaceStage->texture.matrix[0][2]];
		
		matrix[1][0] = surfaceRegs[surfaceStage->texture.matrix[1][0]];
		matrix[1][1] = surfaceRegs[surfaceStage->texture.matrix[1][1]];
		matrix[1][2] = 0.0f;
		matrix[1][3] = surfaceRegs[surfaceStage->texture.matrix[1][2]];
		
		// we attempt to keep scrolls from generating incredibly large texture values, but
		// center rotations and center scales can still generate offsets that need to be > 1
		if( matrix[0][3] < -40.0f || matrix[0][3] > 40.0f )
		{
			matrix[0][3] -= idMath::Ftoi( matrix[0][3] );
		}
		if( matrix[1][3] < -40.0f || matrix[1][3] > 40.0f )
		{
			matrix[1][3] -= idMath::Ftoi( matrix[1][3] );
		}
	}
	else
	{
		matrix[0][0] = 1.0f;
		matrix[0][1] = 0.0f;
		matrix[0][2] = 0.0f;
		matrix[0][3] = 0.0f;
		
		matrix[1][0] = 0.0f;
		matrix[1][1] = 1.0f;
		matrix[1][2] = 0.0f;
		matrix[1][3] = 0.0f;
	}
	
	if( color != NULL )
	{
		for( int i = 0; i < 4; i++ )
		{
			// clamp here, so cards with a greater range don't look different.
			// we could perform overbrighting like we do for lights, but
			// it doesn't currently look worth it.
			color[i] = idMath::ClampFloat( 0.0f, 1.0f, surfaceRegs[surfaceStage->color.registers[i]] ) * lightColor[i];
		}
	}
}

/*
=================
idRenderBackend::DrawSingleInteraction
=================
*/
void idRenderBackend::DrawSingleInteraction( drawInteraction_t* din, const gpuInstanceData_t* instances, int instanceCount )
{
	if( din->bumpImage == NULL )
	{
		// stage wasn't actually an interaction
		return;
	}
	
	if( din->diffuseImage == NULL || r_skipDiffuse.GetBool() )
	{
		// this isn't a YCoCg black, but it doesn't matter, because
		// the diffuseColor will also be 0
		din->diffuseImage = globalImages->blackImage;
	}
	if( din->specularImage == NULL || r_skipSpecular.GetBool() || din->ambientLight )
	{
		din->specularImage = globalImages->blackImage;
	}
	if( r_skipBump.GetBool() )
	{
		din->bumpImage = globalImages->flatNormalMap;
	}
	
	// if we wouldn't draw anything, don't call the Draw function
	const bool diffuseIsBlack = ( din->diffuseImage == globalImages->blackImage )
								|| ( ( din->diffuseColor[0] <= 0 ) && ( din->diffuseColor[1] <= 0 ) && ( din->diffuseColor[2] <= 0 ) );
	const bool specularIsBlack = ( din->specularImage == globalImages->blackImage )
								 || ( ( din->specularColor[0] <= 0 ) && ( din->specularColor[1] <= 0 ) && ( din->specularColor[2] <= 0 ) );
	if( diffuseIsBlack && specularIsBlack )
	{
		return;
	}
	
	// bump matrix
	SetVertexParm( RENDERPARM_BUMPMATRIX_S, din->bumpMatrix[0].ToFloatPtr() );
	SetVertexParm( RENDERPARM_BUMPMATRIX_T, din->bumpMatrix[1].ToFloatPtr() );
	
	// diffuse matrix
	SetVertexParm( RENDERPARM_DIFFUSEMATRIX_S, din->diffuseMatrix[0].ToFloatPtr() );
	SetVertexParm( RENDERPARM_DIFFUSEMATRIX_T, din->diffuseMatrix[1].ToFloatPtr() );
	
	// specular matrix
	SetVertexParm( RENDERPARM_SPECULARMATRIX_S, din->specularMatrix[0].ToFloatPtr() );
	SetVertexParm( RENDERPARM_SPECULARMATRIX_T, din->specularMatrix[1].ToFloatPtr() );
	
	RB_SetVertexColorParms( din->vertexColor );
	
	SetFragmentParm( RENDERPARM_DIFFUSEMODIFIER, din->diffuseColor.ToFloatPtr() );
	SetFragmentParm( RENDERPARM_SPECULARMODIFIER, din->specularColor.ToFloatPtr() );
	
	// texture 0 will be the per-surface bump map
	GL_SelectTexture( INTERACTION_TEXUNIT_BUMP );
	din->bumpImage->Bind();
	
	// texture 1 is the per-surface specular map
	GL_SelectTexture( INTERACTION_TEXUNIT_SPECULARMIX );
	din->specularImage->Bind();
	
	// texture 2 is the per-surface diffuse map
	GL_SelectTexture( INTERACTION_TEXUNIT_BASECOLOR );
	din->diffuseImage->Bind();
	
#if defined(ANDROID) && !defined(USE_VULKAN)
	if( instances != NULL && instanceCount > 1 ) DrawElementsWithCounters( din->surf, instances, instanceCount );
	else
#endif
	{
		DrawElementsWithCounters( din->surf );
	}
}

/*
=================
RB_SetupForFastPathInteractions

These are common for all fast path surfaces
=================
*/
static void RB_SetupForFastPathInteractions( const idVec4& diffuseColor, const idVec4& specularColor )
{
	const idVec4 sMatrix( 1, 0, 0, 0 );
	const idVec4 tMatrix( 0, 1, 0, 0 );
	
	// bump matrix
	SetVertexParm( RENDERPARM_BUMPMATRIX_S, sMatrix.ToFloatPtr() );
	SetVertexParm( RENDERPARM_BUMPMATRIX_T, tMatrix.ToFloatPtr() );
	
	// diffuse matrix
	SetVertexParm( RENDERPARM_DIFFUSEMATRIX_S, sMatrix.ToFloatPtr() );
	SetVertexParm( RENDERPARM_DIFFUSEMATRIX_T, tMatrix.ToFloatPtr() );
	
	// specular matrix
	SetVertexParm( RENDERPARM_SPECULARMATRIX_S, sMatrix.ToFloatPtr() );
	SetVertexParm( RENDERPARM_SPECULARMATRIX_T, tMatrix.ToFloatPtr() );
	
	RB_SetVertexColorParms( SVC_IGNORE );
	
	SetFragmentParm( RENDERPARM_DIFFUSEMODIFIER, diffuseColor.ToFloatPtr() );
	SetFragmentParm( RENDERPARM_SPECULARMODIFIER, specularColor.ToFloatPtr() );
}

/*
=============
idRenderBackend::RenderInteractions

With added sorting and trivial path work.
=============
*/
void idRenderBackend::RenderInteractions( const drawSurf_t* surfList, const viewLight_t* vLight, int depthFunc, bool performStencilTest, bool useLightDepthBounds, bool skipDeferredSurfaces )
{
	if( surfList == NULL )
	{
		return;
	}

	
	// change the scissor if needed, it will be constant across all the surfaces lit by the light
	if( !currentScissor.Equals( vLight->scissorRect ) && r_useScissor.GetBool() )
	{
		GL_Scissor( viewDef->viewport.x1 + vLight->scissorRect.x1,
					viewDef->viewport.y1 + vLight->scissorRect.y1,
					vLight->scissorRect.x2 + 1 - vLight->scissorRect.x1,
					vLight->scissorRect.y2 + 1 - vLight->scissorRect.y1 );
					
		currentScissor = vLight->scissorRect;
	}
	
	// perform setup here that will be constant for all interactions
	if( performStencilTest )
	{
		GL_State( GLS_SRCBLEND_ONE | GLS_DSTBLEND_ONE | GLS_DEPTHMASK | depthFunc | GLS_STENCIL_FUNC_EQUAL | GLS_STENCIL_MAKE_REF( STENCIL_SHADOW_TEST_VALUE ) | GLS_STENCIL_MAKE_MASK( STENCIL_SHADOW_MASK_VALUE ) );
		
	}
	else
	{
		GL_State( GLS_SRCBLEND_ONE | GLS_DSTBLEND_ONE | GLS_DEPTHMASK | depthFunc | GLS_STENCIL_FUNC_ALWAYS );
	}
	
	// some rare lights have multiple animating stages, loop over them outside the surface list
	const idMaterial* lightShader = vLight->lightShader;
	const float* lightRegs = vLight->shaderRegisters;
	
	drawInteraction_t inter = {};
	inter.ambientLight = lightShader->IsAmbientLight();
	
	//---------------------------------
	// Split out the complex surfaces from the fast-path surfaces
	// so we can do the fast path ones all in a row.
	// The surfaces should already be sorted by space because they
	// are added single-threaded, and there is only a negligable amount
	// of benefit to trying to sort by materials.
	//---------------------------------
	static const int MAX_INTERACTIONS_PER_LIGHT = 1024;
	static const int MAX_COMPLEX_INTERACTIONS_PER_LIGHT = 256;
	idStaticList< const drawSurf_t*, MAX_INTERACTIONS_PER_LIGHT > allSurfaces;
	idStaticList< const drawSurf_t*, MAX_COMPLEX_INTERACTIONS_PER_LIGHT > complexSurfaces;
	for( const drawSurf_t* walk = surfList; walk != NULL; walk = walk->nextOnLight )
	{

		// make sure the triangle culling is done
		if( walk->shadowVolumeState != SHADOWVOLUME_DONE )
		{
			assert( walk->shadowVolumeState == SHADOWVOLUME_UNFINISHED || walk->shadowVolumeState == SHADOWVOLUME_DONE );

			uint64 start = Sys_Microseconds();
			while( walk->shadowVolumeState == SHADOWVOLUME_UNFINISHED )
			{
				Sys_Yield();
			}
			uint64 end = Sys_Microseconds();

			pc.shadowMicroSec += end - start;
		}

#if defined(ANDROID) && !defined(USE_VULKAN)
		// DrawDeferredLight lit the surfaces of the G-buffer, the others are drawn as interactions
		if( skipDeferredSurfaces && DeferredSurface( walk ) )
		{
			continue;
		}
#endif

		const idMaterial* surfaceShader = walk->material;
		if( surfaceShader->GetFastPathBumpImage() )
		{
			allSurfaces.Append( walk );
		}
		else
		{
			complexSurfaces.Append( walk );
		}
	}
	for( int i = 0; i < complexSurfaces.Num(); i++ )
	{
		allSurfaces.Append( complexSurfaces[i] );
	}

#if defined(ANDROID) && !defined(USE_VULKAN)
	if( allSurfaces.Num() == 0 )
	{
		// all of them were in the G-buffer
		return;
	}

	const int gpuInteractionSurfaceCount = allSurfaces.Num();
	const drawSurf_t** gpuInteractionSurfs = (const drawSurf_t**)_alloca(
			MAX_INTERACTIONS_PER_LIGHT * sizeof( const drawSurf_t* ) );
	bool* gpuInteractionConsumed = (bool*)_alloca( MAX_INTERACTIONS_PER_LIGHT * sizeof( bool ) );
	for( int i = 0; i < gpuInteractionSurfaceCount; ++i )
	{
		gpuInteractionSurfs[i] = allSurfaces[i];
	}
	int gpuInteractionBatchCount = 0;
	int gpuInteractionDrawCallsSaved = 0;
#endif
	
	bool lightDepthBoundsDisabled = false;

	// shadow map lookup: world space -> 0..1 shadow map texture space and depth for every side,
	// the per surface model matrix is applied below
	const bool useShadowMaps = r_useShadowMapping.GetBool() && vLight->globalShadows != NULL && !lightShader->IsAmbientLight();
#if !defined(USE_VULKAN)
	const shadowAtlasLight_t* atlasLight = useShadowMaps ? FindShadowAtlasLight( vLight ) : NULL;
#endif
	idRenderMatrix lightWorldToShadow[6];
	int numShadowSides = 0;
	if( useShadowMaps )
	{
#if !defined(USE_VULKAN)
		if( atlasLight != NULL )
		{
			numShadowSides = atlasLight->numSides;
			for( int i = 0; i < numShadowSides; i++ )
			{
				lightWorldToShadow[i] = atlasLight->worldToShadow[i];
			}
		}
		else
#endif
		{
			// ShadowMapPass left the matrices of this light in shadowV / shadowP
			if( vLight->parallel )
			{
				numShadowSides = r_shadowMapSplits.GetInteger() + 1;
			}
			else if( vLight->pointLight )
			{
				numShadowSides = 6;
			}
			else
			{
				numShadowSides = 1;
			}
			for( int i = 0; i < numShadowSides; i++ )
			{
				idRenderMatrix lightViewProjection;
				idRenderMatrix::Multiply( shadowP[i], shadowV[i], lightViewProjection );
				if( vLight->parallel || vLight->pointLight )
				{
					idRenderMatrix::Multiply( renderMatrix_clipSpaceToWindowSpace, lightViewProjection, lightWorldToShadow[i] );
				}
				else
				{
					// the base light projection of a projected light is 0..1 already
					lightWorldToShadow[i] = lightViewProjection;
				}
			}
		}
	}

	// RB begin
	if( r_useShadowMapping.GetBool() )
	{
		const static int JITTER_SIZE = 128;

		// default high quality
		float jitterSampleScale = 1.0f;
		float shadowMapSamples = r_shadowMapSamples.GetInteger();

		// screen power of two correction factor
		float screenCorrectionParm[4];
		screenCorrectionParm[0] = 1.0f / ( JITTER_SIZE * shadowMapSamples ) ;
		screenCorrectionParm[1] = 1.0f / JITTER_SIZE;
		// texel size for the PCF taps, the atlas texel is as big as a texel of the light's own resolution
#if !defined(USE_VULKAN)
		screenCorrectionParm[2] = ( atlasLight != NULL ) ? 1.0f / shadowAtlasSize : 1.0f / shadowMapResolutions[vLight->shadowLOD];
#else
		screenCorrectionParm[2] = 1.0f / shadowMapResolutions[vLight->shadowLOD];
#endif
		screenCorrectionParm[3] = vLight->parallel ? r_shadowMapSunDepthBiasScale.GetFloat() : r_shadowMapRegularDepthBiasScale.GetFloat();
		SetFragmentParm( RENDERPARM_SCREENCORRECTIONFACTOR, screenCorrectionParm ); // rpScreenCorrectionFactor
		
		float jitterTexScale[4];
		jitterTexScale[0] = r_shadowMapJitterScale.GetFloat() * jitterSampleScale;	// TODO shadow buffer size fraction shadowMapSize / maxShadowMapSize
		jitterTexScale[1] = r_shadowMapJitterScale.GetFloat() * jitterSampleScale;
		jitterTexScale[2] = -r_shadowMapBiasScale.GetFloat();
		jitterTexScale[3] = 0.0f;
		SetFragmentParm( RENDERPARM_JITTERTEXSCALE, jitterTexScale ); // rpJitterTexScale
		
		float jitterTexOffset[4];
		if( r_shadowMapRandomizeJitter.GetBool() )
		{
			jitterTexOffset[0] = ( rand() & 255 ) / 255.0;
			jitterTexOffset[1] = ( rand() & 255 ) / 255.0;
		}
		else
		{
			jitterTexOffset[0] = 0;
			jitterTexOffset[1] = 0;
		}
		jitterTexOffset[2] = 0.0f;
		jitterTexOffset[3] = 0.0f;
		SetFragmentParm( RENDERPARM_JITTERTEXOFFSET, jitterTexOffset ); // rpJitterTexOffset
		
		if( vLight->parallel )
		{
			float cascadeDistances[4];
			cascadeDistances[0] = viewDef->frustumSplitDistances[0];
			cascadeDistances[1] = viewDef->frustumSplitDistances[1];
			cascadeDistances[2] = viewDef->frustumSplitDistances[2];
			cascadeDistances[3] = viewDef->frustumSplitDistances[3];
			SetFragmentParm( RENDERPARM_CASCADEDISTANCES, cascadeDistances ); // rpCascadeDistances
		}
		
	}
	// RB end
	
	float lightScale = /*r_useHDR.GetBool() ? 3.0f :*/ r_lightScale.GetFloat(); //GK: Allow the ldr lightlevels to work also on HDR
	
	for( int lightStageNum = 0; lightStageNum < lightShader->GetNumStages(); lightStageNum++ )
	{
		const shaderStage_t*	lightStage = lightShader->GetStage( lightStageNum );
		
		// ignore stages that fail the condition
		if( !lightRegs[ lightStage->conditionRegister ] )
		{
			continue;
		}

#if defined(ANDROID) && !defined(USE_VULKAN)
		memset( gpuInteractionConsumed, 0, gpuInteractionSurfaceCount * sizeof( bool ) );
#endif
		
		const idVec4 lightColor(
			lightScale * lightRegs[ lightStage->color.registers[0] ],
			lightScale * lightRegs[ lightStage->color.registers[1] ],
			lightScale * lightRegs[ lightStage->color.registers[2] ],
			lightRegs[ lightStage->color.registers[3] ] );
		// apply the world-global overbright and the 2x factor for specular
		const idVec4 diffuseColor = lightColor;
		const idVec4 specularColor = lightColor * 2.0f;
		
		float lightTextureMatrix[16];
		if( lightStage->texture.hasMatrix )
		{
			RB_GetShaderTextureMatrix( lightRegs, &lightStage->texture, lightTextureMatrix );
		}
		
		// texture 1 will be the light falloff texture
		GL_SelectTexture( INTERACTION_TEXUNIT_FALLOFF );
		vLight->falloffImage->Bind();
		
		// texture 2 will be the light projection texture
		GL_SelectTexture( INTERACTION_TEXUNIT_PROJECTION );
		lightStage->texture.image->Bind();
		
		if( r_useShadowMapping.GetBool() )
		{
			// texture 5 will be the shadow maps array
			GL_SelectTexture( INTERACTION_TEXUNIT_SHADOWMAPS );
#if !defined(USE_VULKAN)
			if( atlasLight != NULL )
			{
				globalImages->shadowAtlasImage->Bind();
			}
			else
#endif
			{
				globalImages->shadowImage[vLight->shadowLOD]->Bind();
			}
			
			// texture 6 will be the jitter texture for soft shadowing
			GL_SelectTexture( INTERACTION_TEXUNIT_JITTER );
			if( r_shadowMapSamples.GetInteger() == 16 )
			{
				globalImages->jitterImage16->Bind();
			}
			else if( r_shadowMapSamples.GetInteger() == 4 )
			{
				globalImages->jitterImage4->Bind();
			}
			else
			{
				globalImages->jitterImage1->Bind();
			}
		}
		
		// force the light textures to not use anisotropic filtering, which is wasted on them
		// all of the texture sampler parms should be constant for all interactions, only
		// the actual texture image bindings will change
		
		//----------------------------------
		// For all surfaces on this light list, generate an interaction for this light stage
		//----------------------------------
		
		// setup renderparms assuming we will be drawing trivial surfaces first
		RB_SetupForFastPathInteractions( diffuseColor, specularColor );
		
		// even if the space does not change between light stages, each light stage may need a different lightTextureMatrix baked in
		currentSpace = NULL;

		// instanced shadow map batches take world space light parameters, single draws model space ones,
		// so a change between the two must set them up again even for the same space
		bool currentSpaceShadowInstanced = false;

#if defined(ANDROID) && !defined(USE_VULKAN)
		// every merged world group is drawn once per light stage
		++s_worldMergeStamp;
#endif

		for( int sortedSurfNum = 0; sortedSurfNum < allSurfaces.Num(); sortedSurfNum++ )
		{
#if defined(ANDROID) && !defined(USE_VULKAN)
			if( gpuInteractionConsumed[sortedSurfNum] )
			{
				continue;
			}

			// a merged world surface draws all of its material group this light reaches
			worldMergeSurf = NULL;
			if( R_WorldMergeSkip( allSurfaces[ sortedSurfNum ] ) )
			{
				continue;
			}
			if( R_IsWorldMergeGroupMember( allSurfaces[ sortedSurfNum ] ) )
			{
				BeginWorldMergeGroup( allSurfaces[ sortedSurfNum ] );
			}

			gpuInstanceData_t gpuInteractionInstances[MAX_GPU_INSTANCES_PER_DRAW];
			const drawSurf_t* gpuInteractionInstanceSurfs[MAX_GPU_INSTANCES_PER_DRAW];
			const gpuInstanceData_t* gpuInteractionInstanceData = NULL;
			int gpuInteractionInstanceCount = 1;
#endif
			const drawSurf_t* const surf = allSurfaces[ sortedSurfNum ];
			bool gpuShadowInteractionInstanced = false;

#if defined(ANDROID) && !defined(USE_VULKAN)
			const bool useShadowMapInteraction = !lightShader->IsAmbientLight()
					&& r_useShadowMapping.GetBool() && vLight->globalShadows != NULL;
			if( glConfig.glVersion >= 3.2f && !surf->jointCache )
			{
				gpuInteractionInstanceCount = R_BuildGPUInstanceBatch( gpuInteractionSurfs,
						gpuInteractionSurfaceCount, sortedSurfNum, viewDef->renderView.vieworg,
						gpuInteractionConsumed, gpuInteractionInstances, true, gpuInteractionInstanceSurfs );
				if( gpuInteractionInstanceCount > 1 )
				{
					gpuShadowInteractionInstanced = useShadowMapInteraction;
					for( int instanceIndex = 0; instanceIndex < gpuInteractionInstanceCount; ++instanceIndex )
					{
						if( useShadowMapInteraction )
						{
							R_FillShadowInteractionGPUInstanceData( gpuInteractionInstances[instanceIndex],
									gpuInteractionInstanceSurfs[instanceIndex], viewDef->renderView.vieworg,
									vLight->globalLightOrigin );
						}
						else
						{
							R_FillInteractionGPUInstanceData( gpuInteractionInstances[instanceIndex],
									gpuInteractionInstanceSurfs[instanceIndex], viewDef->renderView.vieworg,
									vLight->globalLightOrigin, vLight->lightProject,
									lightTextureMatrix, lightStage->texture.hasMatrix );
						}
					}
					gpuInteractionInstanceData = gpuInteractionInstances;
					++gpuInteractionBatchCount;
					gpuInteractionDrawCallsSaved += gpuInteractionInstanceCount - 1;
				}
			}
#endif
			
			// select the render prog
			if( lightShader->IsAmbientLight() )
			{
				if( surf->jointCache )
				{
					renderProgManager.BindShader_InteractionAmbientSkinned();
				}
				else
				{
#if defined(ANDROID) && !defined(USE_VULKAN)
					if( gpuInteractionInstanceCount > 1 )
					{
						renderProgManager.BindShader_InteractionAmbientInstanced();
					}
					else
#endif
					{
						renderProgManager.BindShader_InteractionAmbient();
					}
				}
			}
			else
			{
				if( r_useShadowMapping.GetBool() && vLight->globalShadows )
				{
					// RB: we have shadow mapping enabled and shadow maps so do a shadow compare
					
					if( vLight->parallel )
					{
						if( surf->jointCache )
						{
							renderProgManager.BindShader_Interaction_ShadowMapping_Parallel_Skinned();
						}
						else
						{
#if defined(ANDROID) && !defined(USE_VULKAN)
							if( gpuInteractionInstanceCount > 1 )
							{
								renderProgManager.BindShader_Interaction_ShadowMapping_Parallel_Instanced();
							}
							else
#endif
							{
								renderProgManager.BindShader_Interaction_ShadowMapping_Parallel();
							}
						}
					}
					else if( vLight->pointLight )
					{
						if( surf->jointCache )
						{
							renderProgManager.BindShader_Interaction_ShadowMapping_Point_Skinned();
						}
						else
						{
#if defined(ANDROID) && !defined(USE_VULKAN)
							if( gpuInteractionInstanceCount > 1 )
							{
								renderProgManager.BindShader_Interaction_ShadowMapping_Point_Instanced();
							}
							else
#endif
							{
								renderProgManager.BindShader_Interaction_ShadowMapping_Point();
							}
						}
					}
					else
					{
						if( surf->jointCache )
						{
							renderProgManager.BindShader_Interaction_ShadowMapping_Spot_Skinned();
						}
						else
						{
#if defined(ANDROID) && !defined(USE_VULKAN)
							if( gpuInteractionInstanceCount > 1 )
							{
								renderProgManager.BindShader_Interaction_ShadowMapping_Spot_Instanced();
							}
							else
#endif
							{
								renderProgManager.BindShader_Interaction_ShadowMapping_Spot();
							}
						}
					}
				}
				else
				{
					if( surf->jointCache )
					{
						renderProgManager.BindShader_InteractionSkinned();
					}
					else
					{
#if defined(ANDROID) && !defined(USE_VULKAN)
						if( gpuInteractionInstanceCount > 1 )
						{
							renderProgManager.BindShader_InteractionInstanced();
						}
						else
#endif
						{
							renderProgManager.BindShader_Interaction();
						}
					}
				}
			}
			
			const idMaterial* surfaceShader = surf->material;
			const float* surfaceRegs = surf->shaderRegisters;
			
			inter.surf = surf;
			
			// change the MVP matrix, view/light origin and light projection vectors if needed
			if( surf->space != currentSpace || gpuShadowInteractionInstanced != currentSpaceShadowInstanced )
			{
				currentSpace = surf->space;
				currentSpaceShadowInstanced = gpuShadowInteractionInstanced;

				// turn off the light depth bounds test if this model is rendered with a depth hack
				if( useLightDepthBounds )
				{
					if( !surf->space->weaponDepthHack && surf->space->modelDepthHack == 0.0f )
					{
						if( lightDepthBoundsDisabled )
						{
							GL_DepthBoundsTest( vLight->scissorRect.zmin, vLight->scissorRect.zmax );
							lightDepthBoundsDisabled = false;
						}
					}
					else
					{
						if( !lightDepthBoundsDisabled )
						{
							GL_DepthBoundsTest( 0.0f, 0.0f );
							lightDepthBoundsDisabled = true;
						}
					}
				}
				
				// model-view-projection
				RB_SetMVP( surf->space->mvp );
				
				// RB begin
				idRenderMatrix modelMatrix;
				idRenderMatrix::Transpose( *( idRenderMatrix* )surf->space->modelMatrix, modelMatrix );
				
				SetVertexParms( RENDERPARM_MODELMATRIX_X, modelMatrix[0], 4 );
				
				// for determining the shadow mapping cascades
				idRenderMatrix modelViewMatrix, tmp;
				idRenderMatrix::Transpose( *( idRenderMatrix* )surf->space->modelViewMatrix, modelViewMatrix );
				if( gpuShadowInteractionInstanced )
				{
					idRenderMatrix inverseModelMatrix;
					idRenderMatrix viewMatrix;
					if( idRenderMatrix::Inverse( modelMatrix, inverseModelMatrix ) )
					{
						idRenderMatrix::Multiply( modelViewMatrix, inverseModelMatrix, viewMatrix );
						SetVertexParms( RENDERPARM_MODELVIEWMATRIX_X, viewMatrix[0], 4 );
					}
					else
					{
						SetVertexParms( RENDERPARM_MODELVIEWMATRIX_X, modelViewMatrix[0], 4 );
					}
				}
				else
				{
					SetVertexParms( RENDERPARM_MODELVIEWMATRIX_X, modelViewMatrix[0], 4 );
				}
				
				idVec4 globalLightOrigin( vLight->globalLightOrigin.x, vLight->globalLightOrigin.y, vLight->globalLightOrigin.z, 1.0f );
				SetVertexParm( RENDERPARM_GLOBALLIGHTORIGIN, globalLightOrigin.ToFloatPtr() );
				// RB end
				
				// tranform the light/view origin into model local space
				idVec4 localLightOrigin( 0.0f );
				idVec4 localViewOrigin( 1.0f );
				R_GlobalPointToLocal( surf->space->modelMatrix, vLight->globalLightOrigin, localLightOrigin.ToVec3() );
				R_GlobalPointToLocal( surf->space->modelMatrix, viewDef->renderView.vieworg, localViewOrigin.ToVec3() );
				
				// set the local light/view origin
				SetVertexParm( RENDERPARM_LOCALLIGHTORIGIN, localLightOrigin.ToFloatPtr() );
				SetVertexParm( RENDERPARM_LOCALVIEWORIGIN, localViewOrigin.ToFloatPtr() );
				
				// transform the light project into model local space
				idPlane lightProjection[4];
				for( int i = 0; i < 4; i++ )
				{
					if( gpuShadowInteractionInstanced )
					{
						lightProjection[i] = vLight->lightProject[i];
					}
					else
					{
						R_GlobalPlaneToLocal( surf->space->modelMatrix, vLight->lightProject[i], lightProjection[i] );
					}
				}
				
				// optionally multiply the local light projection by the light texture matrix
				if( lightStage->texture.hasMatrix )
				{
					RB_BakeTextureMatrixIntoTexgen( lightProjection, lightTextureMatrix );
				}
				
				// set the light projection
				SetVertexParm( RENDERPARM_LIGHTPROJECTION_S, lightProjection[0].ToFloatPtr() );
				SetVertexParm( RENDERPARM_LIGHTPROJECTION_T, lightProjection[1].ToFloatPtr() );
				SetVertexParm( RENDERPARM_LIGHTPROJECTION_Q, lightProjection[2].ToFloatPtr() );
				SetVertexParm( RENDERPARM_LIGHTFALLOFF_S, lightProjection[3].ToFloatPtr() );
				
				// RB begin
				for( int i = 0; i < numShadowSides; i++ )
				{
					if( gpuShadowInteractionInstanced )
					{
						// the instanced shaders look up world space positions
						SetVertexParms( ( renderParm_t )( RENDERPARM_SHADOW_MATRIX_0_X + i * 4 ), lightWorldToShadow[i][0], 4 );
					}
					else
					{
						idRenderMatrix modelToShadowMatrix;
						idRenderMatrix::Multiply( lightWorldToShadow[i], modelMatrix, modelToShadowMatrix );
						SetVertexParms( ( renderParm_t )( RENDERPARM_SHADOW_MATRIX_0_X + i * 4 ), modelToShadowMatrix[0], 4 );
					}
				}
				// RB end
			}
			
			// check for the fast path
			if( surfaceShader->GetFastPathBumpImage() && !r_skipInteractionFastPath.GetBool() )
			{
				renderLog.OpenBlock( surf->material->GetName() );
				
				// texture 0 will be the per-surface bump map
				GL_SelectTexture( INTERACTION_TEXUNIT_BUMP );
				surfaceShader->GetFastPathBumpImage()->Bind();
				
				// texture 1 is the per-surface specular map
				GL_SelectTexture( INTERACTION_TEXUNIT_SPECULARMIX );
				surfaceShader->GetFastPathSpecularImage()->Bind();
				
				// texture 2 is the per-surface diffuse map
				GL_SelectTexture( INTERACTION_TEXUNIT_BASECOLOR );
				surfaceShader->GetFastPathDiffuseImage()->Bind();
				
#if defined(ANDROID) && !defined(USE_VULKAN)
				DrawElementsWithCounters( surf, gpuInteractionInstanceData, gpuInteractionInstanceCount );
#else
				DrawElementsWithCounters( surf );
#endif
				
				renderLog.CloseBlock();
				continue;
			}
			
			renderLog.OpenBlock( surf->material->GetName() );
			
			inter.bumpImage = NULL;
			inter.specularImage = NULL;
			inter.diffuseImage = NULL;
			inter.diffuseColor[0] = inter.diffuseColor[1] = inter.diffuseColor[2] = inter.diffuseColor[3] = 0;
			inter.specularColor[0] = inter.specularColor[1] = inter.specularColor[2] = inter.specularColor[3] = 0;
			
			// go through the individual surface stages
			//
			// This is somewhat arcane because of the old support for video cards that had to render
			// interactions in multiple passes.
			//
			// We also have the very rare case of some materials that have conditional interactions
			// for the "hell writing" that can be shined on them.
			for( int surfaceStageNum = 0; surfaceStageNum < surfaceShader->GetNumStages(); surfaceStageNum++ )
			{
				const shaderStage_t*	surfaceStage = surfaceShader->GetStage( surfaceStageNum );
				
				switch( surfaceStage->lighting )
				{
					case SL_COVERAGE:
					{
						// ignore any coverage stages since they should only be used for the depth fill pass
						// for diffuse stages that use alpha test.
						break;
					}
					case SL_AMBIENT:
					{
						// ignore ambient stages while drawing interactions
						break;
					}
					case SL_BUMP:
					{
						// ignore stage that fails the condition
						if( !surfaceRegs[ surfaceStage->conditionRegister ] )
						{
							break;
						}
						// draw any previous interaction
						if( inter.bumpImage != NULL )
						{
							#if defined(ANDROID) && !defined(USE_VULKAN)
							DrawSingleInteraction( &inter, gpuInteractionInstanceData, gpuInteractionInstanceCount );
							#else
							DrawSingleInteraction( &inter );
							#endif
						}
						inter.bumpImage = surfaceStage->texture.image;
						inter.diffuseImage = NULL;
						inter.specularImage = NULL;
						SetupInteractionStage( surfaceStage, surfaceRegs, NULL, inter.bumpMatrix, NULL );
						break;
					}
					case SL_DIFFUSE:
					{
						// ignore stage that fails the condition
						if( !surfaceRegs[ surfaceStage->conditionRegister ] )
						{
							break;
						}
						// draw any previous interaction
						if( inter.diffuseImage != NULL )
						{
							#if defined(ANDROID) && !defined(USE_VULKAN)
							DrawSingleInteraction( &inter, gpuInteractionInstanceData, gpuInteractionInstanceCount );
							#else
							DrawSingleInteraction( &inter );
							#endif
						}
						inter.diffuseImage = surfaceStage->texture.image;
						inter.vertexColor = surfaceStage->vertexColor;
						SetupInteractionStage( surfaceStage, surfaceRegs, diffuseColor.ToFloatPtr(),
											   inter.diffuseMatrix, inter.diffuseColor.ToFloatPtr() );
						break;
					}
					case SL_SPECULAR:
					{
						// ignore stage that fails the condition
						if( !surfaceRegs[ surfaceStage->conditionRegister ] )
						{
							break;
						}
						// draw any previous interaction
						if( inter.specularImage != NULL )
						{
							#if defined(ANDROID) && !defined(USE_VULKAN)
							DrawSingleInteraction( &inter, gpuInteractionInstanceData, gpuInteractionInstanceCount );
							#else
							DrawSingleInteraction( &inter );
							#endif
						}
						inter.specularImage = surfaceStage->texture.image;
						inter.vertexColor = surfaceStage->vertexColor;
						SetupInteractionStage( surfaceStage, surfaceRegs, specularColor.ToFloatPtr(),
											   inter.specularMatrix, inter.specularColor.ToFloatPtr() );
						break;
					}
				}
			}
			
			// draw the final interaction
			#if defined(ANDROID) && !defined(USE_VULKAN)
			DrawSingleInteraction( &inter, gpuInteractionInstanceData, gpuInteractionInstanceCount );
			#else
			DrawSingleInteraction( &inter );
			#endif
			
			renderLog.CloseBlock();
		}
	}
	
	if( useLightDepthBounds && lightDepthBoundsDisabled )
	{
		GL_DepthBoundsTest( vLight->scissorRect.zmin, vLight->scissorRect.zmax );
	}

#if defined(ANDROID) && !defined(USE_VULKAN)
	if( r_gpuInstancingDebug.GetBool() && gpuInteractionBatchCount > 0 )
	{
		idLib::Printf( "GPU instancing interaction pass: %d batches, at least %d draw calls saved\n",
				gpuInteractionBatchCount, gpuInteractionDrawCallsSaved );
	}
	worldMergeSurf = NULL;
#endif

	renderProgManager.Unbind();
}

// RB begin

/*
=========================================================================================

AMBIENT PASS RENDERING

=========================================================================================
*/

/*
==================
idRenderBackend::AmbientPass
==================
*/
void idRenderBackend::AmbientPass( const drawSurf_t* const* drawSurfs, int numDrawSurfs, bool fillGbuffer, bool skipDeferredSurfaces )
{
	if( fillGbuffer )
	{
		if( !r_useSSGI.GetBool() && !r_useSSAO.GetBool() )
		{
			return;
		}
	}
	else
	{
		if( r_forceAmbient.GetFloat() <= 0 || r_skipAmbient.GetBool() )
		{
			return;
		}
	}
	
	if( numDrawSurfs == 0 )
	{
		return;
	}
	
	// if we are just doing 2D rendering, no need to fill the depth buffer
	if( viewDef->viewEntitys == NULL )
	{
		return;
	}

#if defined(ANDROID) && !defined(USE_VULKAN)
	bool* gpuInstanceConsumed = (bool*)_alloca( numDrawSurfs * sizeof(bool) );
	memset( gpuInstanceConsumed, 0, numDrawSurfs * sizeof(bool) );
	int gpuInstanceBatchCount = 0;
	int gpuInstanceDrawCallsSaved = 0;

	// every merged world group is drawn once in this pass
	++s_worldMergeStamp;
#endif

	const bool hdrIsActive = ( globalFramebuffers.hdrFBO != NULL && globalFramebuffers.hdrFBO->IsBound() );

	renderLog.OpenMainBlock(MRB_AMBIENT_PASS);
	renderLog.OpenBlock("RB_AmbientPass");

	if( fillGbuffer )
	{
		globalFramebuffers.geometryBufferFBO->Bind();
	
		GL_Clear(true, false, false, 0, 0.0f, 0.0f, 0.0f, 1.0f, false);
	}
	
	
	// RB: not needed
	// GL_StartDepthPass( backEnd.viewDef->scissor );
	
	// force MVP change on first surface
	currentSpace = NULL;
	
	// draw all the subview surfaces, which will already be at the start of the sorted list,
	// with the general purpose path
	GL_State( GLS_DEFAULT );
	
#define BLEND_NORMALS 1
	//if( fillGbuffer )
	{
		GL_State( GLS_SRCBLEND_ONE | GLS_DSTBLEND_ZERO | GLS_DEPTHMASK | GLS_DEPTHFUNC_EQUAL );
	}
	/*else
	{
		GL_State( GLS_SRCBLEND_ONE | GLS_DSTBLEND_ONE | GLS_DEPTHMASK | GLS_DEPTHFUNC_EQUAL );
	}*/
	
	GL_Color( colorWhite );
	
	const float lightScale = r_lightScale.GetFloat();
	const idVec4 lightColor = colorWhite * lightScale;
	// apply the world-global overbright and the 2x factor for specular
	const idVec4 diffuseColor = lightColor;
	const idVec4 specularColor = lightColor * 2.0f;
	
	idVec4 ambientColor;
	float ambientBoost = 1.0f;
	ambientBoost += r_useSSAO.GetBool() ? 0.2f : 0.0f;
	ambientBoost *= r_useHDR.GetBool() ? 1.1f : 1.0f;
	ambientColor.x = r_forceAmbient.GetFloat() * ambientBoost;
	ambientColor.y = r_forceAmbient.GetFloat() * ambientBoost;
	ambientColor.z = r_forceAmbient.GetFloat() * ambientBoost;
	ambientColor.w = 1;
	
	renderProgManager.SetRenderParm( RENDERPARM_AMBIENT_COLOR, ambientColor.ToFloatPtr() );
	
	// setup renderparms assuming we will be drawing trivial surfaces first
	RB_SetupForFastPathInteractions( diffuseColor, specularColor );
	
	for( int i = 0; i < numDrawSurfs; i++ )
	{
#if defined(ANDROID) && !defined(USE_VULKAN)
		if( gpuInstanceConsumed[i] )
		{
			continue;
		}
#endif
		const drawSurf_t* drawSurf = drawSurfs[i];
		const idMaterial* surfaceMaterial = drawSurf->material;
		
		// translucent surfaces don't put anything in the depth buffer and don't
		// test against it, which makes them fail the mirror clip plane operation
		if( surfaceMaterial->Coverage() == MC_TRANSLUCENT )
		{
			continue;
		}
		
		// get the expressions for conditionals / color / texcoords
		const float* surfaceRegs = drawSurf->shaderRegisters;
		
		// if all stages of a material have been conditioned off, don't do anything
		int stage = 0;
		for( ; stage < surfaceMaterial->GetNumStages(); stage++ )
		{
			const shaderStage_t* pStage = surfaceMaterial->GetStage( stage );
			// check the stage enable condition
			if( surfaceRegs[ pStage->conditionRegister ] != 0 )
			{
				break;
			}
		}
		if( stage == surfaceMaterial->GetNumStages() )
		{
			continue;
		}

#if defined(ANDROID) && !defined(USE_VULKAN)
		// DeferredGBufferPass wrote the ambient of the surfaces in the G-buffer
		if( skipDeferredSurfaces && DeferredSurface( drawSurf ) )
		{
			continue;
		}

		// a merged world surface draws its whole material group, the rest of the group is done then
		worldMergeSurf = NULL;
		if( R_WorldMergeSkip( drawSurf ) )
		{
			continue;
		}
		if( R_IsWorldMergeGroupMember( drawSurf ) )
		{
			BeginWorldMergeGroup( drawSurf );
		}
#endif

		const gpuInstanceData_t* gpuInstanceData = NULL;
		int gpuInstanceCount = 1;
#if defined(ANDROID) && !defined(USE_VULKAN)
		gpuInstanceData_t gpuInstances[MAX_GPU_INSTANCES_PER_DRAW];
		if( glConfig.glVersion >= 3.2f )
		{
			gpuInstanceCount = R_BuildGPUInstanceBatch( drawSurfs, numDrawSurfs, i, viewDef->renderView.vieworg, gpuInstanceConsumed, gpuInstances, surfaceMaterial->Coverage() == MC_PERFORATED );
			if( gpuInstanceCount > 1 )
			{
				gpuInstanceData = gpuInstances;
				++gpuInstanceBatchCount;
				gpuInstanceDrawCallsSaved += gpuInstanceCount - 1;
			}
		}
#endif
		
		//bool isWorldModel = ( drawSurf->space->entityDef->parms.origin == vec3_origin );
		
		//if( isWorldModel )
		//{
		//	renderProgManager.BindShader_VertexLighting();
		//}
		//else
		{
#if 1
			if( fillGbuffer )
			{
				// fill geometry buffer with normal/roughness information
				if( drawSurf->jointCache )
				{
					renderProgManager.BindShader_SmallGeometryBufferSkinned();
				}
				else
				{
#if defined(ANDROID) && !defined(USE_VULKAN)
					if( gpuInstanceCount > 1 ) renderProgManager.BindShader_SmallGeometryBufferInstanced();
					else
#endif
					{
						renderProgManager.BindShader_SmallGeometryBuffer();
					}
				}
			}
			else
#endif
			{
				// draw Quake 4 style ambient
				if( drawSurf->jointCache )
				{
					renderProgManager.BindShader_AmbientLightingSkinned();
				}
				else
				{
#if defined(ANDROID) && !defined(USE_VULKAN)
					if( gpuInstanceCount > 1 ) renderProgManager.BindShader_AmbientLightingInstanced();
					else
#endif
					{
						renderProgManager.BindShader_AmbientLighting();
					}
				}
			}
		}
		
		// change the matrix if needed
		if( drawSurf->space != currentSpace )
		{
			currentSpace = drawSurf->space;
			
			RB_SetMVP( drawSurf->space->mvp );
			
			// tranform the view origin into model local space
			idVec4 localViewOrigin( 1.0f );
			R_GlobalPointToLocal( drawSurf->space->modelMatrix, viewDef->renderView.vieworg, localViewOrigin.ToVec3() );
			SetVertexParm( RENDERPARM_LOCALVIEWORIGIN, localViewOrigin.ToFloatPtr() );
			
			//if( !isWorldModel )
			//{
			//	// tranform the light direction into model local space
			//	idVec3 globalLightDirection( 0.0f, 0.0f, -1.0f ); // HACK
			//	idVec4 localLightDirection( 0.0f );
			//	R_GlobalVectorToLocal( drawSurf->space->modelMatrix, globalLightDirection, localLightDirection.ToVec3() );
			//
			//	SetVertexParm( RENDERPARM_LOCALLIGHTORIGIN, localLightDirection.ToFloatPtr() );
			//}
			
			// RB: if we want to store the normals in world space so we need the model -> world matrix
			idRenderMatrix modelMatrix;
			idRenderMatrix::Transpose( *( idRenderMatrix* )drawSurf->space->modelMatrix, modelMatrix );
			
			SetVertexParms( RENDERPARM_MODELMATRIX_X, modelMatrix[0], 4 );
			
			// RB: if we want to store the normals in camera space so we need the model -> camera matrix
			float modelViewMatrixTranspose[16];
			R_MatrixTranspose( drawSurf->space->modelViewMatrix, modelViewMatrixTranspose );
			SetVertexParms( RENDERPARM_MODELVIEWMATRIX_X, modelViewMatrixTranspose, 4 );
		}
		
#if 0
		if( !isWorldModel )
		{
			idVec4 directedColor;
			directedColor.x = drawSurf->space->gridDirectedLight.x;
			directedColor.y = drawSurf->space->gridDirectedLight.y;
			directedColor.z = drawSurf->space->gridDirectedLight.z;
			directedColor.w = 1;
			
			idVec4 ambientColor;
			ambientColor.x = drawSurf->space->gridAmbientLight.x;
			ambientColor.y = drawSurf->space->gridAmbientLight.y;
			ambientColor.z = drawSurf->space->gridAmbientLight.z;
			ambientColor.w = 1;
			
			renderProgManager.SetRenderParm( RENDERPARM_COLOR, directedColor.ToFloatPtr() );
			renderProgManager.SetRenderParm( RENDERPARM_AMBIENT_COLOR, ambientColor.ToFloatPtr() );
		}
#endif
		
		/*
		uint64 surfGLState = 0;
		
		// set polygon offset if necessary
		if( surfaceMaterial->TestMaterialFlag( MF_POLYGONOFFSET ) )
		{
			surfGLState |= GLS_POLYGON_OFFSET;
			GL_PolygonOffset( r_offsetFactor.GetFloat(), r_offsetUnits.GetFloat() * surfaceMaterial->GetPolygonOffset() );
		}
		
		// subviews will just down-modulate the color buffer
		idVec4 color;
		if( surfaceMaterial->GetSort() == SS_SUBVIEW )
		{
			surfGLState |= GLS_SRCBLEND_DST_COLOR | GLS_DSTBLEND_ZERO | GLS_DEPTHFUNC_LESS;
			color[0] = 1.0f;
			color[1] = 1.0f;
			color[2] = 1.0f;
			color[3] = 1.0f;
		}
		else
		{
			// others just draw black
		#if 0
			color[0] = 0.0f;
			color[1] = 0.0f;
			color[2] = 0.0f;
			color[3] = 1.0f;
		#else
			color = colorWhite;
		#endif
		}
		*/
		
		// check for the fast path
		if( surfaceMaterial->GetFastPathBumpImage() && !r_skipInteractionFastPath.GetBool() )
		{
			renderLog.OpenBlock( surfaceMaterial->GetName() );
			
			// texture 0 will be the per-surface bump map
			GL_SelectTexture( INTERACTION_TEXUNIT_BUMP );
			surfaceMaterial->GetFastPathBumpImage()->Bind();
			
			// texture 1 is the per-surface specular map
			GL_SelectTexture( INTERACTION_TEXUNIT_SPECULARMIX );
			surfaceMaterial->GetFastPathSpecularImage()->Bind();
			
			// texture 2 is the per-surface diffuse map
			GL_SelectTexture( INTERACTION_TEXUNIT_BASECOLOR );
			surfaceMaterial->GetFastPathDiffuseImage()->Bind();
			
#if defined(ANDROID) && !defined(USE_VULKAN)
			DrawElementsWithCounters( drawSurf, gpuInstanceData, gpuInstanceCount );
#else
			DrawElementsWithCounters( drawSurf );
#endif
			
			renderLog.CloseBlock();
			continue;
		}
		
		renderLog.OpenBlock( surfaceMaterial->GetName() );
		
		//bool drawSolid = false;
		
		
		// we may have multiple alpha tested stages
		// if the only alpha tested stages are condition register omitted,
		// draw a normal opaque surface
		//bool didDraw = false;
		
		drawInteraction_t inter = {};
		inter.surf = drawSurf;
		inter.bumpImage = NULL;
		inter.specularImage = NULL;
		inter.diffuseImage = NULL;
		
		inter.diffuseColor[0] = inter.diffuseColor[1] = inter.diffuseColor[2] = inter.diffuseColor[3] = 1;
		inter.specularColor[0] = inter.specularColor[1] = inter.specularColor[2] = inter.specularColor[3] = 0;
		
		// perforated surfaces may have multiple alpha tested stages
		for( stage = 0; stage < surfaceMaterial->GetNumStages(); stage++ )
		{
			const shaderStage_t* surfaceStage = surfaceMaterial->GetStage( stage );
			
			switch( surfaceStage->lighting )
			{
				case SL_COVERAGE:
				{
					// ignore any coverage stages since they should only be used for the depth fill pass
					// for diffuse stages that use alpha test.
					break;
				}
				
				case SL_AMBIENT:
				{
					// ignore ambient stages while drawing interactions
					break;
				}
				
				case SL_BUMP:
				{
					// ignore stage that fails the condition
					if( !surfaceRegs[ surfaceStage->conditionRegister ] )
					{
						break;
					}
					// draw any previous interaction
					if( inter.bumpImage != NULL )
					{
#if BLEND_NORMALS
						if (inter.vertexColor == SVC_IGNORE)
						{
							GL_State(GLS_SRCBLEND_ONE | GLS_DSTBLEND_ZERO | GLS_DEPTHMASK | GLS_DEPTHFUNC_EQUAL);
						}
						else
						{
							// RB: this is a bit hacky: use additive blending to blend the normals
							GL_State(GLS_SRCBLEND_ONE | GLS_DSTBLEND_ONE | GLS_DEPTHMASK | GLS_DEPTHFUNC_EQUAL);
						}
#endif
						DrawSingleInteraction( &inter, gpuInstanceData, gpuInstanceCount );
					}
					inter.bumpImage = surfaceStage->texture.image;
					inter.diffuseImage = NULL;
					inter.specularImage = NULL;
					SetupInteractionStage( surfaceStage, surfaceRegs, NULL,
										   inter.bumpMatrix, NULL );
					break;
				}
				
				case SL_DIFFUSE:
				{
					// ignore stage that fails the condition
					if( !surfaceRegs[ surfaceStage->conditionRegister ] )
					{
						break;
					}
					
					// draw any previous interaction
					if( inter.diffuseImage != NULL )
					{
#if BLEND_NORMALS
						if (inter.vertexColor == SVC_IGNORE)
						{
							GL_State(GLS_SRCBLEND_ONE | GLS_DSTBLEND_ZERO | GLS_DEPTHMASK | GLS_DEPTHFUNC_EQUAL);
						}
						else
						{
							// RB: this is a bit hacky: use additive blending to blend the normals
							GL_State(GLS_SRCBLEND_ONE | GLS_DSTBLEND_ONE | GLS_DEPTHMASK | GLS_DEPTHFUNC_EQUAL);
						}
#endif
						DrawSingleInteraction( &inter, gpuInstanceData, gpuInstanceCount );
					}
					
					inter.diffuseImage = surfaceStage->texture.image;
					inter.vertexColor = surfaceStage->vertexColor;
					SetupInteractionStage( surfaceStage, surfaceRegs, diffuseColor.ToFloatPtr(),
										   inter.diffuseMatrix, inter.diffuseColor.ToFloatPtr() );
					break;
				}
				
				case SL_SPECULAR:
				{
					// ignore stage that fails the condition
					if( !surfaceRegs[ surfaceStage->conditionRegister ] )
					{
						break;
					}
					// draw any previous interaction
					if( inter.specularImage != NULL )
					{
#if BLEND_NORMALS
						if (inter.vertexColor == SVC_IGNORE)
						{
							GL_State(GLS_SRCBLEND_ONE | GLS_DSTBLEND_ZERO | GLS_DEPTHMASK | GLS_DEPTHFUNC_EQUAL);
						}
						else
						{
							// RB: this is a bit hacky: use additive blending to blend the normals
							GL_State(GLS_SRCBLEND_ONE | GLS_DSTBLEND_ONE | GLS_DEPTHMASK | GLS_DEPTHFUNC_EQUAL);
						}
#endif
						DrawSingleInteraction( &inter, gpuInstanceData, gpuInstanceCount );
					}
					inter.specularImage = surfaceStage->texture.image;
					inter.vertexColor = surfaceStage->vertexColor;
					SetupInteractionStage( surfaceStage, surfaceRegs, specularColor.ToFloatPtr(),
										   inter.specularMatrix, inter.specularColor.ToFloatPtr() );
					break;
				}
			}
		}
		
		// draw the final interaction
#if BLEND_NORMALS
						if( inter.vertexColor == SVC_IGNORE )
						{
							GL_State( GLS_SRCBLEND_ONE | GLS_DSTBLEND_ZERO | GLS_DEPTHMASK | GLS_DEPTHFUNC_EQUAL );
						}
						else
						{
							// RB: this is a bit hacky: use additive blending to blend the normals
							GL_State( GLS_SRCBLEND_ONE | GLS_DSTBLEND_ONE | GLS_DEPTHMASK | GLS_DEPTHFUNC_EQUAL );
						}
#endif
		DrawSingleInteraction( &inter, gpuInstanceData, gpuInstanceCount );
		
		renderLog.CloseBlock();
	}
	
	SetFragmentParm( RENDERPARM_ALPHA_TEST, vec4_zero.ToFloatPtr() );
	
	renderLog.CloseBlock();
	renderLog.CloseMainBlock();
	
	GL_SelectTexture( 0 );
	
	if( fillGbuffer )
	{
		if( hdrIsActive )
		{
			globalFramebuffers.hdrFBO->Bind();
		}
		else
		{
			Framebuffer::Unbind();
		}
	}
	
#if defined(ANDROID) && !defined(USE_VULKAN)
	if( r_gpuInstancingDebug.GetBool() )
	{
		idLib::Printf( "GPU instancing ambient pass: %d batches, %d draw calls saved\n", gpuInstanceBatchCount, gpuInstanceDrawCallsSaved );
	}
	worldMergeSurf = NULL;
#endif
	renderProgManager.Unbind();
}

// RB end

/*
==============================================================================================

STENCIL SHADOW RENDERING

==============================================================================================
*/

/*
=====================
idRenderBackend::StencilShadowPass

The stencil buffer should have been set to 128 on any surfaces that might receive shadows.
=====================
*/
void idRenderBackend::StencilShadowPass( const drawSurf_t* drawSurfs, const viewLight_t* vLight )
{
	if( r_skipShadows.GetBool() )
	{
		return;
	}
	
	if( drawSurfs == NULL )
	{
		return;
	}
	
	RENDERLOG_PRINTF( "---------- RB_StencilShadowPass ----------\n" );
	
	renderProgManager.BindShader_Shadow();
	
	GL_SelectTexture( 0 );
	
	uint64 glState = 0;
	
	// for visualizing the shadows
	if( r_showShadows.GetInteger() )
	{
		// set the debug shadow color
		SetFragmentParm( RENDERPARM_COLOR, colorMagenta.ToFloatPtr() );
		if( r_showShadows.GetInteger() == 2 )
		{
			// draw filled in
			glState = GLS_DEPTHMASK | GLS_SRCBLEND_ONE | GLS_DSTBLEND_ONE | GLS_DEPTHFUNC_LESS;
		}
		else
		{
			// draw as lines, filling the depth buffer
			glState = GLS_SRCBLEND_ONE | GLS_DSTBLEND_ZERO | GLS_POLYMODE_LINE | GLS_DEPTHFUNC_ALWAYS;
		}
	}
	else
	{
		// don't write to the color or depth buffer, just the stencil buffer
		glState = GLS_DEPTHMASK | GLS_COLORMASK | GLS_ALPHAMASK | GLS_DEPTHFUNC_LESS;
	}
	
	GL_PolygonOffset( r_shadowPolygonFactor.GetFloat(), -r_shadowPolygonOffset.GetFloat() );
	
	// the actual stencil func will be set in the draw code, but we need to make sure it isn't
	// disabled here, and that the value will get reset for the interactions without looking
	// like a no-change-required
	
	// Two Sided Stencil reduces two draw calls to one for slightly faster shadows
	GL_State(
		glState |
		GLS_STENCIL_OP_FAIL_KEEP |
		GLS_STENCIL_OP_ZFAIL_KEEP |
		GLS_STENCIL_OP_PASS_INCR |
		GLS_STENCIL_MAKE_REF( STENCIL_SHADOW_TEST_VALUE ) |
		GLS_STENCIL_MAKE_MASK( STENCIL_SHADOW_MASK_VALUE ) |
		GLS_POLYGON_OFFSET |
		GLS_CULL_TWOSIDED );
		
	// process the chain of shadows with the current rendering state
	currentSpace = NULL;
#if defined(ANDROID) && !defined(USE_VULKAN)
    // Cache stencil mode for this draw list so same-state surfaces can batch.
    int previousStencilShadowMode = -1;
#endif

#if defined(ANDROID) && !defined(USE_VULKAN)
	int numStencilShadowSurfs = 0;
	for( const drawSurf_t* scan = drawSurfs; scan != NULL; scan = scan->nextOnLight ) ++numStencilShadowSurfs;
	const int stencilShadowAllocCount = numStencilShadowSurfs > 0 ? numStencilShadowSurfs : 1;
	const drawSurf_t** stencilShadowSurfs = (const drawSurf_t**)_alloca( stencilShadowAllocCount * sizeof( const drawSurf_t* ) );
	bool* stencilShadowConsumed = (bool*)_alloca( stencilShadowAllocCount * sizeof( bool ) );
	int stencilShadowArrayIndex = 0;
	for( const drawSurf_t* scan = drawSurfs; scan != NULL; scan = scan->nextOnLight ) stencilShadowSurfs[stencilShadowArrayIndex++] = scan;
	memset( stencilShadowConsumed, 0, stencilShadowAllocCount * sizeof( bool ) );
	int stencilShadowBatchCount = 0;
	int stencilShadowDrawCallsSaved = 0;
	for( int stencilShadowIndex = 0; stencilShadowIndex < numStencilShadowSurfs; ++stencilShadowIndex )
	{
		if( stencilShadowConsumed[stencilShadowIndex] ) continue;
		const drawSurf_t* drawSurf = stencilShadowSurfs[stencilShadowIndex];
#else
	for( const drawSurf_t* drawSurf = drawSurfs; drawSurf != NULL; drawSurf = drawSurf->nextOnLight )
	{
#endif
		if( drawSurf->scissorRect.IsEmpty() )
		{
			continue;	// !@# FIXME: find out why this is sometimes being hit!
			// temporarily jump over the scissor and draw so the gl error callback doesn't get hit
		}
		
		// make sure the shadow volume is done
		if( drawSurf->shadowVolumeState != SHADOWVOLUME_DONE )
		{
			assert( drawSurf->shadowVolumeState == SHADOWVOLUME_UNFINISHED || drawSurf->shadowVolumeState == SHADOWVOLUME_DONE );
			
			uint64 start = Sys_Microseconds();
			while( drawSurf->shadowVolumeState == SHADOWVOLUME_UNFINISHED )
			{
				Sys_Yield();
			}
			uint64 end = Sys_Microseconds();
			
			pc.shadowMicroSec += end - start;
		}
		
		if( drawSurf->numIndexes == 0 )
		{
			continue;	// a job may have created an empty shadow volume
		}

#if defined(ANDROID) && !defined(USE_VULKAN)
		gpuInstanceData_t stencilShadowInstances[MAX_GPU_INSTANCES_PER_DRAW];
		const gpuInstanceData_t* stencilShadowInstanceData = NULL;
		int stencilShadowInstanceCount = 1;
		if( glConfig.glVersion >= 3.2f && r_showShadows.GetInteger() == 0 && !drawSurf->jointCache )
		{
			stencilShadowInstanceCount = R_BuildStencilShadowGPUInstanceBatch( stencilShadowSurfs,
					numStencilShadowSurfs, stencilShadowIndex, viewDef->renderView.vieworg,
					vLight->globalLightOrigin, stencilShadowConsumed, stencilShadowInstances );
			if( stencilShadowInstanceCount > 1 )
			{
				stencilShadowInstanceData = stencilShadowInstances;
				++stencilShadowBatchCount;
				stencilShadowDrawCallsSaved += stencilShadowInstanceCount - 1;
			}
		}
#endif
		
		if( !currentScissor.Equals( drawSurf->scissorRect ) && r_useScissor.GetBool() )
		{
			// change the scissor
			GL_Scissor( viewDef->viewport.x1 + drawSurf->scissorRect.x1,
						viewDef->viewport.y1 + drawSurf->scissorRect.y1,
						drawSurf->scissorRect.x2 + 1 - drawSurf->scissorRect.x1,
						drawSurf->scissorRect.y2 + 1 - drawSurf->scissorRect.y1 );
						
			currentScissor = drawSurf->scissorRect;
		}
		
		if( drawSurf->space != currentSpace )
		{
			// change the matrix
			RB_SetMVP( drawSurf->space->mvp );
			
			// set the local light position to allow the vertex program to project the shadow volume end cap to infinity
			idVec4 localLight( 0.0f );
			R_GlobalPointToLocal( drawSurf->space->modelMatrix, vLight->globalLightOrigin, localLight.ToVec3() );
			SetVertexParm( RENDERPARM_LOCALLIGHTORIGIN, localLight.ToFloatPtr() );
			
			currentSpace = drawSurf->space;
		}
		
		if( r_showShadows.GetInteger() == 0 )
		{
			if( drawSurf->jointCache )
			{
				renderProgManager.BindShader_ShadowSkinned();
			}
			else
			{
				renderProgManager.BindShader_Shadow();
			}
		}
		else
		{
			if( drawSurf->jointCache )
			{
				renderProgManager.BindShader_ShadowDebugSkinned();
			}
			else
			{
				renderProgManager.BindShader_ShadowDebug();
			}
		}
		
		// set depth bounds per shadow
		if( r_useShadowDepthBounds.GetBool() )
		{
			GL_DepthBoundsTest( drawSurf->scissorRect.zmin, drawSurf->scissorRect.zmax );
		}
		
		// Determine whether or not the shadow volume needs to be rendered with Z-pass or
		// Z-fail. It is worthwhile to spend significant resources to reduce the number of
		// cases where shadow volumes need to be rendered with Z-fail because Z-fail
		// rendering can be significantly slower even on today's hardware. For instance,
		// on NVIDIA hardware Z-fail rendering causes the Z-Cull to be used in reverse:
		// Z-near becomes Z-far (trivial accept becomes trivial reject). Using the Z-Cull
		// in reverse is far less efficient because the Z-Cull only stores Z-near per 16x16
		// pixels while the Z-far is stored per 4x2 pixels. (The Z-near coallesce buffer
		// which has 4x4 granularity is only used when updating the depth which is not the
		// case for shadow volumes.) Note that it is also important to NOT use a Z-Cull
		// reconstruct because that would clear the Z-near of the Z-Cull which results in
		// no trivial rejection for Z-fail stencil shadow rendering.
		
		const bool renderZPass = ( drawSurf->renderZFail == 0 ) || r_forceZPassStencilShadows.GetBool();
		
		
#if !defined(USE_VULKAN)
#if defined(ANDROID) && !defined(USE_VULKAN)
        const bool usePreload = !renderZPass && r_useStencilShadowPreload.GetBool();
        const int stencilShadowMode = usePreload ? 1 : (renderZPass ? 0 : 2);
        if (usePreload || stencilShadowMode != previousStencilShadowMode)
        {
            // Finish earlier draws before changing stencil operations.
            tr.backend.FlushMultiDrawBatch();
            if (renderZPass)
            {
                glStencilOpSeparate(GL_FRONT, GL_KEEP, GL_KEEP, GL_INCR);
                glStencilOpSeparate(GL_BACK, GL_KEEP, GL_KEEP, GL_DECR);
            }
            else if (usePreload)
            {
                glStencilOpSeparate(GL_FRONT, GL_KEEP, GL_DECR, GL_DECR);
                glStencilOpSeparate(GL_BACK, GL_KEEP, GL_INCR, GL_INCR);
            }
            else
            {
                glStencilOpSeparate(GL_FRONT, GL_KEEP, GL_DECR, GL_KEEP);
                glStencilOpSeparate(GL_BACK, GL_KEEP, GL_INCR, GL_KEEP);
            }
            // The preload draw rewrites the stencil ops internally; reapply
            // them for every preload surface rather than caching that mode.
            previousStencilShadowMode = usePreload ? -1 : stencilShadowMode;
        }
#else
        if (renderZPass)
        {
            glStencilOpSeparate(GL_FRONT, GL_KEEP, GL_KEEP, GL_INCR);
            glStencilOpSeparate(GL_BACK, GL_KEEP, GL_KEEP, GL_DECR);
        }
        else if (r_useStencilShadowPreload.GetBool())
        {
            glStencilOpSeparate(GL_FRONT, GL_KEEP, GL_DECR, GL_DECR);
            glStencilOpSeparate(GL_BACK, GL_KEEP, GL_INCR, GL_INCR);
        }
        else
        {
            glStencilOpSeparate(GL_FRONT, GL_KEEP, GL_DECR, GL_KEEP);
            glStencilOpSeparate(GL_BACK, GL_KEEP, GL_INCR, GL_KEEP);
        }
#endif
#endif
		
#if defined(ANDROID) && !defined(USE_VULKAN)
		if( stencilShadowInstanceCount > 1 )
		{
			renderProgManager.BindShader_ShadowInstanced();
			DrawStencilShadowPass( drawSurf, renderZPass, stencilShadowInstanceData, stencilShadowInstanceCount );
		}
		else
		{
			DrawStencilShadowPass( drawSurf, renderZPass );
		}
#else
		DrawStencilShadowPass( drawSurf, renderZPass );
#endif
	}

#if defined(ANDROID) && !defined(USE_VULKAN)
    // Finish queued shadows before the caller restores the stencil state.
    tr.backend.FlushMultiDrawBatch();
	if( r_gpuInstancingDebug.GetBool() && stencilShadowBatchCount > 0 )
	{
		idLib::Printf( "GPU instancing stencil-shadow pass: %d batches, %d draw calls saved\n",
				stencilShadowBatchCount, stencilShadowDrawCallsSaved );
	}
#endif
	
	// cleanup the shadow specific rendering state
	
	GL_State( glStateBits & ~( GLS_CULL_MASK ) | GLS_CULL_FRONTSIDED );
	
	// reset depth bounds
	if( r_useShadowDepthBounds.GetBool() )
	{
		if( r_useLightDepthBounds.GetBool() )
		{
			GL_DepthBoundsTest( vLight->scissorRect.zmin, vLight->scissorRect.zmax );
		}
		else
		{
			GL_DepthBoundsTest( 0.0f, 0.0f );
		}
	}
}

/*
==================
idRenderBackend::StencilSelectLight

Deform the zeroOneCubeModel to exactly cover the light volume. Render the deformed cube model to the stencil buffer in
such a way that only fragments that are directly visible and contained within the volume will be written creating a
mask to be used by the following stencil shadow and draw interaction passes.
==================
*/
void idRenderBackend::StencilSelectLight( const viewLight_t* vLight )
{
	renderLog.OpenBlock( "Stencil Select" );
	
	// enable the light scissor
	if( !currentScissor.Equals( vLight->scissorRect ) && r_useScissor.GetBool() )
	{
		GL_Scissor( viewDef->viewport.x1 + vLight->scissorRect.x1,
					viewDef->viewport.y1 + vLight->scissorRect.y1,
					vLight->scissorRect.x2 + 1 - vLight->scissorRect.x1,
					vLight->scissorRect.y2 + 1 - vLight->scissorRect.y1 );
					
		currentScissor = vLight->scissorRect;
	}
	
	// clear stencil buffer to 0 (not drawable)
	uint64 glStateMinusStencil = GL_GetCurrentStateMinusStencil();
	GL_State( glStateMinusStencil | GLS_STENCIL_FUNC_ALWAYS | GLS_STENCIL_MAKE_REF( STENCIL_SHADOW_TEST_VALUE ) | GLS_STENCIL_MAKE_MASK( STENCIL_SHADOW_MASK_VALUE ) );	// make sure stencil mask passes for the clear
	GL_Clear( false, false, true, 0, 0.0f, 0.0f, 0.0f, 0.0f, false );	// clear to 0 for stencil select
	
	// set the depthbounds
	GL_DepthBoundsTest( vLight->scissorRect.zmin, vLight->scissorRect.zmax );
	
	
	GL_State(
		GLS_COLORMASK |
		GLS_ALPHAMASK |
		GLS_CULL_TWOSIDED |
		GLS_DEPTHMASK |
		GLS_DEPTHFUNC_LESS |
		GLS_STENCIL_FUNC_ALWAYS |
		GLS_STENCIL_MAKE_REF( STENCIL_SHADOW_TEST_VALUE ) |
		GLS_STENCIL_MAKE_MASK( STENCIL_SHADOW_MASK_VALUE ) );
		
	renderProgManager.BindShader_Depth();
	
	// set the matrix for deforming the 'zeroOneCubeModel' into the frustum to exactly cover the light volume
	idRenderMatrix invProjectMVPMatrix;
	idRenderMatrix::Multiply( viewDef->worldSpace.mvp, vLight->inverseBaseLightProject, invProjectMVPMatrix );
	RB_SetMVP( invProjectMVPMatrix );
	
#if !defined(USE_VULKAN)
	// two-sided stencil test
	glStencilOpSeparate( GL_FRONT, GL_KEEP, GL_REPLACE, GL_ZERO );
	glStencilOpSeparate( GL_BACK, GL_KEEP, GL_ZERO, GL_REPLACE );
#endif
	
	DrawElementsWithCounters( &zeroOneCubeSurface );
	
	// reset stencil state
	GL_State( glStateBits & ~( GLS_CULL_MASK ) | GLS_CULL_FRONTSIDED );
	
	renderProgManager.Unbind();
	
	
	// unset the depthbounds
	GL_DepthBoundsTest( 0.0f, 0.0f );
	
	renderLog.CloseBlock();
}

/*
==============================================================================================

SHADOW MAPS RENDERING

==============================================================================================
*/

/*
same as D3DXMatrixOrthoOffCenterRH

http://msdn.microsoft.com/en-us/library/bb205348(VS.85).aspx
*/
static void MatrixOrthogonalProjectionRH( float m[16], float left, float right, float bottom, float top, float zNear, float zFar )
{
	m[0] = 2 / ( right - left );
	m[4] = 0;
	m[8] = 0;
	m[12] = ( left + right ) / ( left - right );
	m[1] = 0;
	m[5] = 2 / ( top - bottom );
	m[9] = 0;
	m[13] = ( top + bottom ) / ( bottom - top );
	m[2] = 0;
	m[6] = 0;
	m[10] = 1 / ( zNear - zFar );
	m[14] = zNear / ( zNear - zFar );
	m[3] = 0;
	m[7] = 0;
	m[11] = 0;
	m[15] = 1;
}

void MatrixCrop( float m[16], const idVec3 mins, const idVec3 maxs )
{
	float			scaleX, scaleY, scaleZ;
	float			offsetX, offsetY, offsetZ;
	
	scaleX = 2.0f / ( maxs[0] - mins[0] );
	scaleY = 2.0f / ( maxs[1] - mins[1] );
	
	offsetX = -0.5f * ( maxs[0] + mins[0] ) * scaleX;
	offsetY = -0.5f * ( maxs[1] + mins[1] ) * scaleY;
	
	scaleZ = 1.0f / ( maxs[2] - mins[2] );
	offsetZ = -mins[2] * scaleZ;
	
	m[ 0] = scaleX;
	m[ 4] = 0;
	m[ 8] = 0;
	m[12] = offsetX;
	m[ 1] = 0;
	m[ 5] = scaleY;
	m[ 9] = 0;
	m[13] = offsetY;
	m[ 2] = 0;
	m[ 6] = 0;
	m[10] = scaleZ;
	m[14] = offsetZ;
	m[ 3] = 0;
	m[ 7] = 0;
	m[11] = 0;
	m[15] = 1;
}

void MatrixLookAtRH( float m[16], const idVec3& eye, const idVec3& dir, const idVec3& up )
{
	idVec3 dirN;
	idVec3 upN;
	idVec3 sideN;
	
	sideN = dir.Cross( up );
	sideN.Normalize();
	
	upN = sideN.Cross( dir );
	upN.Normalize();
	
	dirN = dir;
	dirN.Normalize();
	
	m[ 0] = sideN[0];
	m[ 4] = sideN[1];
	m[ 8] = sideN[2];
	m[12] = -( sideN * eye );
	m[ 1] = upN[0];
	m[ 5] = upN[1];
	m[ 9] = upN[2];
	m[13] = -( upN * eye );
	m[ 2] = -dirN[0];
	m[ 6] = -dirN[1];
	m[10] = -dirN[2];
	m[14] = ( dirN * eye );
	m[ 3] = 0;
	m[ 7] = 0;
	m[11] = 0;
	m[15] = 1;
}

/*
=====================
idRenderBackend::BeginShadowMapSide

Depth only state every shadow map side starts with
=====================
*/
void idRenderBackend::BeginShadowMapSide()
{
	renderProgManager.BindShader_Depth();

	GL_SelectTexture( 0 );

	uint64 glState = 0;

	// the actual stencil func will be set in the draw code, but we need to make sure it isn't
	// disabled here, and that the value will get reset for the interactions without looking
	// like a no-change-required
	GL_State( glState | GLS_POLYGON_OFFSET );

	switch( r_shadowMapOccluderFacing.GetInteger() )
	{
		case 0:
			GL_State( glStateBits & ~( GLS_CULL_MASK ) | GLS_CULL_FRONTSIDED );
			GL_PolygonOffset( r_shadowMapPolygonFactor.GetFloat(), r_shadowMapPolygonOffset.GetFloat() );
			break;

		case 1:
			GL_State( glStateBits & ~( GLS_CULL_MASK ) | GLS_CULL_BACKSIDED );
			GL_PolygonOffset( -r_shadowMapPolygonFactor.GetFloat(), -r_shadowMapPolygonOffset.GetFloat() );
			break;

		default:
			GL_State( glStateBits & ~( GLS_CULL_MASK ) | GLS_CULL_TWOSIDED );
			GL_PolygonOffset( r_shadowMapPolygonFactor.GetFloat(), r_shadowMapPolygonOffset.GetFloat() );
			break;
	}
}

/*
=====================
idRenderBackend::SetupShadowMapMatrices

Light view and projection of one shadow map side, also kept in shadowV / shadowP
=====================
*/
void idRenderBackend::SetupShadowMapMatrices( const viewLight_t* vLight, int side, idRenderMatrix& lightViewRenderMatrix, idRenderMatrix& lightProjectionRenderMatrix )
{
	if( vLight->parallel && side >= 0 )
	{
		assert( side >= 0 && side < 6 );
		
		// original light direction is from surface to light origin
		idVec3 lightDir = -vLight->lightCenter;
		if( lightDir.Normalize() == 0.0f )
		{
			lightDir[2] = -1.0f;
		}
		
		idMat3 rotation = lightDir.ToMat3();
		//idAngles angles = lightDir.ToAngles();
		//idMat3 rotation = angles.ToMat3();
		
		const idVec3 viewDir = viewDef->renderView.viewaxis[0];
		const idVec3 viewPos = viewDef->renderView.vieworg;
		
#if 1
		idRenderMatrix::CreateViewMatrix( viewDef->renderView.vieworg, rotation, lightViewRenderMatrix );
#else
		float lightViewMatrix[16];
		MatrixLookAtRH( lightViewMatrix, viewPos, lightDir, viewDir );
		idRenderMatrix::Transpose( *( idRenderMatrix* )lightViewMatrix, lightViewRenderMatrix );
#endif
		
		idBounds lightBounds;
		lightBounds.Clear();
		
		ALIGNTYPE16 frustumCorners_t corners;
		idRenderMatrix::GetFrustumCorners( corners, vLight->inverseBaseLightProject, bounds_zeroOneCube );
		
		idVec4 point, transf;
		for( int j = 0; j < 8; j++ )
		{
			point[0] = corners.x[j];
			point[1] = corners.y[j];
			point[2] = corners.z[j];
			point[3] = 1;
			
			lightViewRenderMatrix.TransformPoint( point, transf );
			transf[0] /= transf[3];
			transf[1] /= transf[3];
			transf[2] /= transf[3];
			
			lightBounds.AddPoint( transf.ToVec3() );
		}
		
		float lightProjectionMatrix[16];
		MatrixOrthogonalProjectionRH( lightProjectionMatrix, lightBounds[0][0], lightBounds[1][0], lightBounds[0][1], lightBounds[1][1], -lightBounds[1][2], -lightBounds[0][2] );
		idRenderMatrix::Transpose( *( idRenderMatrix* )lightProjectionMatrix, lightProjectionRenderMatrix );
		
		
		// 	'frustumMVP' goes from global space -> camera local space -> camera projective space
		// invert the MVP projection so we can deform zero-to-one cubes into the frustum pyramid shape and calculate global bounds
		
		idRenderMatrix splitFrustumInverse;
		if( !idRenderMatrix::Inverse( viewDef->frustumMVPs[FRUSTUM_CASCADE1 + side], splitFrustumInverse ) )
		{
			idLib::Warning( "splitFrustumMVP invert failed" );
		}
		
		// splitFrustumCorners in global space
		ALIGNTYPE16 frustumCorners_t splitFrustumCorners;
		idRenderMatrix::GetFrustumCorners( splitFrustumCorners, splitFrustumInverse, bounds_unitCube );
		
#if 0
		idBounds splitFrustumBounds;
		splitFrustumBounds.Clear();
		for( int j = 0; j < 8; j++ )
		{
			point[0] = splitFrustumCorners.x[j];
			point[1] = splitFrustumCorners.y[j];
			point[2] = splitFrustumCorners.z[j];
			
			splitFrustumBounds.AddPoint( point.ToVec3() );
		}
		
		idVec3 center = splitFrustumBounds.GetCenter();
		float radius = splitFrustumBounds.GetRadius( center );
		
		//ALIGNTYPE16 frustumCorners_t splitFrustumCorners;
		splitFrustumBounds[0] = idVec3( -radius, -radius, -radius );
		splitFrustumBounds[1] = idVec3( radius, radius, radius );
		splitFrustumBounds.TranslateSelf( viewPos );
		idVec3 splitFrustumCorners2[8];
		splitFrustumBounds.ToPoints( splitFrustumCorners2 );
		
		for( int j = 0; j < 8; j++ )
		{
			splitFrustumCorners.x[j] = splitFrustumCorners2[j].x;
			splitFrustumCorners.y[j] = splitFrustumCorners2[j].y;
			splitFrustumCorners.z[j] = splitFrustumCorners2[j].z;
		}
#endif
		
		
		idRenderMatrix lightViewProjectionRenderMatrix;
		idRenderMatrix::Multiply( lightProjectionRenderMatrix, lightViewRenderMatrix, lightViewProjectionRenderMatrix );
		
		// find the bounding box of the current split in the light's clip space
		idBounds cropBounds;
		cropBounds.Clear();
		for( int j = 0; j < 8; j++ )
		{
			point[0] = splitFrustumCorners.x[j];
			point[1] = splitFrustumCorners.y[j];
			point[2] = splitFrustumCorners.z[j];
			point[3] = 1;
			
			lightViewRenderMatrix.TransformPoint( point, transf );
			transf[0] /= transf[3];
			transf[1] /= transf[3];
			transf[2] /= transf[3];
			
			cropBounds.AddPoint( transf.ToVec3() );
		}
		
		// don't let the frustum AABB be bigger than the light AABB
		if( cropBounds[0][0] < lightBounds[0][0] )
		{
			cropBounds[0][0] = lightBounds[0][0];
		}
		
		if( cropBounds[0][1] < lightBounds[0][1] )
		{
			cropBounds[0][1] = lightBounds[0][1];
		}
		
		if( cropBounds[1][0] > lightBounds[1][0] )
		{
			cropBounds[1][0] = lightBounds[1][0];
		}
		
		if( cropBounds[1][1] > lightBounds[1][1] )
		{
			cropBounds[1][1] = lightBounds[1][1];
		}
		
		cropBounds[0][2] = lightBounds[0][2];
		cropBounds[1][2] = lightBounds[1][2];
		
		//float cropMatrix[16];
		//MatrixCrop(cropMatrix, cropBounds[0], cropBounds[1]);
		
		//idRenderMatrix cropRenderMatrix;
		//idRenderMatrix::Transpose( *( idRenderMatrix* )cropMatrix, cropRenderMatrix );
		
		//idRenderMatrix tmp = lightProjectionRenderMatrix;
		//idRenderMatrix::Multiply( cropRenderMatrix, tmp, lightProjectionRenderMatrix );
		
		MatrixOrthogonalProjectionRH( lightProjectionMatrix, cropBounds[0][0], cropBounds[1][0], cropBounds[0][1], cropBounds[1][1], -cropBounds[1][2], -cropBounds[0][2] );
		idRenderMatrix::Transpose( *( idRenderMatrix* )lightProjectionMatrix, lightProjectionRenderMatrix );
		
		shadowV[side] = lightViewRenderMatrix;
		shadowP[side] = lightProjectionRenderMatrix;
	}
	else if( vLight->pointLight && side >= 0 )
	{
		assert( side >= 0 && side < 6 );
		
		// FIXME OPTIMIZE no memset
		
		float	viewMatrix[16];
		
		idVec3	vec;
		idVec3	origin = vLight->globalLightOrigin;
		
		// side of a point light
		memset( viewMatrix, 0, sizeof( viewMatrix ) );
		switch( side )
		{
			case 0:
				viewMatrix[0] = 1;
				viewMatrix[9] = 1;
				viewMatrix[6] = -1;
				break;
			case 1:
				viewMatrix[0] = -1;
				viewMatrix[9] = -1;
				viewMatrix[6] = -1;
				break;
			case 2:
				viewMatrix[4] = 1;
				viewMatrix[1] = -1;
				viewMatrix[10] = 1;
				break;
			case 3:
				viewMatrix[4] = -1;
				viewMatrix[1] = -1;
				viewMatrix[10] = -1;
				break;
			case 4:
				viewMatrix[8] = 1;
				viewMatrix[1] = -1;
				viewMatrix[6] = -1;
				break;
			case 5:
				viewMatrix[8] = -1;
				viewMatrix[1] = 1;
				viewMatrix[6] = -1;
				break;
		}
		
		viewMatrix[12] = -origin[0] * viewMatrix[0] + -origin[1] * viewMatrix[4] + -origin[2] * viewMatrix[8];
		viewMatrix[13] = -origin[0] * viewMatrix[1] + -origin[1] * viewMatrix[5] + -origin[2] * viewMatrix[9];
		viewMatrix[14] = -origin[0] * viewMatrix[2] + -origin[1] * viewMatrix[6] + -origin[2] * viewMatrix[10];
		
		viewMatrix[3] = 0;
		viewMatrix[7] = 0;
		viewMatrix[11] = 0;
		viewMatrix[15] = 1;
		
		// from world space to light origin, looking down the X axis
		float	unflippedLightViewMatrix[16];
		
		// from world space to OpenGL view space, looking down the negative Z axis
		float	lightViewMatrix[16];
		
		static float	s_flipMatrix[16] =
		{
			// convert from our coordinate system (looking down X)
			// to OpenGL's coordinate system (looking down -Z)
			0, 0, -1, 0,
			-1, 0, 0, 0,
			0, 1, 0, 0,
			0, 0, 0, 1
		};
		
		memcpy( unflippedLightViewMatrix, viewMatrix, sizeof( unflippedLightViewMatrix ) );
		R_MatrixMultiply( viewMatrix, s_flipMatrix, lightViewMatrix );
		
		idRenderMatrix::Transpose( *( idRenderMatrix* )lightViewMatrix, lightViewRenderMatrix );
		
		// set up 90 degree projection matrix
		const float zNear = 4;
		const float	fov = r_shadowMapFrustumFOV.GetFloat();
		
		float ymax = zNear * tan( fov * idMath::PI / 360.0f );
		float ymin = -ymax;
		
		float xmax = zNear * tan( fov * idMath::PI / 360.0f );
		float xmin = -xmax;
		
		const float width = xmax - xmin;
		const float height = ymax - ymin;
		
		// from OpenGL view space to OpenGL NDC ( -1 : 1 in XYZ )
		float lightProjectionMatrix[16];
		
		lightProjectionMatrix[0 * 4 + 0] = 2.0f * zNear / width;
		lightProjectionMatrix[1 * 4 + 0] = 0.0f;
		lightProjectionMatrix[2 * 4 + 0] = ( xmax + xmin ) / width;	// normally 0
		lightProjectionMatrix[3 * 4 + 0] = 0.0f;
		
		lightProjectionMatrix[0 * 4 + 1] = 0.0f;
		lightProjectionMatrix[1 * 4 + 1] = 2.0f * zNear / height;
		lightProjectionMatrix[2 * 4 + 1] = ( ymax + ymin ) / height;	// normally 0
		lightProjectionMatrix[3 * 4 + 1] = 0.0f;
		
		// this is the far-plane-at-infinity formulation, and
		// crunches the Z range slightly so w=0 vertexes do not
		// rasterize right at the wraparound point
		lightProjectionMatrix[0 * 4 + 2] = 0.0f;
		lightProjectionMatrix[1 * 4 + 2] = 0.0f;
		lightProjectionMatrix[2 * 4 + 2] = -0.999f; // adjust value to prevent imprecision issues
		lightProjectionMatrix[3 * 4 + 2] = -2.0f * zNear;
		
		lightProjectionMatrix[0 * 4 + 3] = 0.0f;
		lightProjectionMatrix[1 * 4 + 3] = 0.0f;
		lightProjectionMatrix[2 * 4 + 3] = -1.0f;
		lightProjectionMatrix[3 * 4 + 3] = 0.0f;
		
		idRenderMatrix::Transpose( *( idRenderMatrix* )lightProjectionMatrix, lightProjectionRenderMatrix );
		
		shadowV[side] = lightViewRenderMatrix;
		shadowP[side] = lightProjectionRenderMatrix;
	}
	else
	{
		lightViewRenderMatrix.Identity();
		lightProjectionRenderMatrix = vLight->baseLightProject;
		
		shadowV[0] = lightViewRenderMatrix;
		shadowP[0] = lightProjectionRenderMatrix;
	}
}

/*
=====================
idRenderBackend::DrawShadowMapCasters

Draws the casters of one shadow map side into the bound target and viewport
=====================
*/
void idRenderBackend::DrawShadowMapCasters( const drawSurf_t* drawSurfs, const viewLight_t* vLight, int side,
		const idRenderMatrix& lightViewRenderMatrix, const idRenderMatrix& lightProjectionRenderMatrix )
{
	// the front end flags the point light sides a caster can't reach
	const int culledSideBit = ( vLight->pointLight && !vLight->parallel && side >= 0 ) ? ( 1 << side ) : 0;

	// process the chain of shadows with the current rendering state
	currentSpace = NULL;

#if defined(ANDROID) && !defined(USE_VULKAN)
	// the opaque world casters of this side with one call, in the identity transform of the world areas
	const worldMergeCasters_t* mergedCasters = R_FindWorldMergeCasters( vLight );
	const int mergedSide = ( side < 0 ) ? 0 : side;
	if( mergedCasters != NULL && mergedSide < 6 && mergedCasters->numIndexes[mergedSide] > 0 )
	{
		idRenderMatrix clipMVP;
		idRenderMatrix::Multiply( lightProjectionRenderMatrix, lightViewRenderMatrix, clipMVP );
		if( !vLight->parallel && side < 0 )
		{
			// from OpenGL view space to OpenGL NDC ( -1 : 1 in XYZ )
			idRenderMatrix MVP;
			idRenderMatrix::Multiply( renderMatrix_windowSpaceToClipSpace, clipMVP, MVP );
			RB_SetMVP( MVP );
		}
		else
		{
			RB_SetMVP( clipMVP );
		}
		
		renderProgManager.BindShader_Depth();
		DrawWorldMergeRange( mergedCasters->firstIndex[mergedSide], mergedCasters->numIndexes[mergedSide] );
	}
	
	int numShadowDrawSurfs = 0;
	for( const drawSurf_t* scan = drawSurfs; scan != NULL; scan = scan->nextOnLight )
	{
		if( !( scan->shadowMapCulledSides & culledSideBit ) && scan->mergedGroup != WORLD_MERGE_SHADOW_CASTER )
		{
			++numShadowDrawSurfs;
		}
	}
	if( numShadowDrawSurfs == 0 )
	{
		return;
	}
	const drawSurf_t** shadowDrawSurfs = (const drawSurf_t**)_alloca( numShadowDrawSurfs * sizeof( const drawSurf_t* ) );
	bool* gpuShadowConsumed = (bool*)_alloca( numShadowDrawSurfs * sizeof( bool ) );
	int shadowArrayIndex = 0;
	for( const drawSurf_t* scan = drawSurfs; scan != NULL; scan = scan->nextOnLight )
	{
		if( !( scan->shadowMapCulledSides & culledSideBit ) && scan->mergedGroup != WORLD_MERGE_SHADOW_CASTER )
		{
			shadowDrawSurfs[shadowArrayIndex++] = scan;
		}
	}
	memset( gpuShadowConsumed, 0, numShadowDrawSurfs * sizeof( bool ) );
	int gpuShadowBatchCount = 0;
	int gpuShadowDrawCallsSaved = 0;
	for( int shadowIndex = 0; shadowIndex < numShadowDrawSurfs; ++shadowIndex )
	{
		if( gpuShadowConsumed[shadowIndex] )
		{
			continue;
		}
		const drawSurf_t* drawSurf = shadowDrawSurfs[shadowIndex];
#else
	for( const drawSurf_t* drawSurf = drawSurfs; drawSurf != NULL; drawSurf = drawSurf->nextOnLight )
	{
		if( drawSurf->shadowMapCulledSides & culledSideBit )
		{
			continue;
		}
#endif

#if 1
		// make sure the shadow occluder geometry is done
		if( drawSurf->shadowVolumeState != SHADOWVOLUME_DONE )
		{
			assert( drawSurf->shadowVolumeState == SHADOWVOLUME_UNFINISHED || drawSurf->shadowVolumeState == SHADOWVOLUME_DONE );

			uint64 start = Sys_Microseconds();
			while( drawSurf->shadowVolumeState == SHADOWVOLUME_UNFINISHED )
			{
				Sys_Yield();
			}
			uint64 end = Sys_Microseconds();

			pc.shadowMicroSec += end - start;
		}
#endif

		if( drawSurf->numIndexes == 0 )
		{
			continue;	// a job may have created an empty shadow geometry
		}

#if defined(ANDROID) && !defined(USE_VULKAN)
		gpuInstanceData_t gpuShadowInstances[MAX_GPU_INSTANCES_PER_DRAW];
		const gpuInstanceData_t* gpuShadowInstanceData = NULL;
		int gpuShadowInstanceCount = 1;
		if( glConfig.glVersion >= 3.2f )
		{
			gpuShadowInstanceCount = R_BuildShadowGPUInstanceBatch( shadowDrawSurfs, numShadowDrawSurfs,
					shadowIndex, viewDef->renderView.vieworg, gpuShadowConsumed, gpuShadowInstances,
					lightViewRenderMatrix, lightProjectionRenderMatrix, vLight, side );
			if( gpuShadowInstanceCount > 1 )
			{
				gpuShadowInstanceData = gpuShadowInstances;
				++gpuShadowBatchCount;
			}
		}
#endif
		
		if( drawSurf->space != currentSpace )
		{
			idRenderMatrix modelRenderMatrix;
			idRenderMatrix::Transpose( *( idRenderMatrix* )drawSurf->space->modelMatrix, modelRenderMatrix );
			
			idRenderMatrix modelToLightRenderMatrix;
			idRenderMatrix::Multiply( lightViewRenderMatrix, modelRenderMatrix, modelToLightRenderMatrix );
			
			idRenderMatrix clipMVP;
			idRenderMatrix::Multiply( lightProjectionRenderMatrix, modelToLightRenderMatrix, clipMVP );
			
			if( vLight->parallel )
			{
				idRenderMatrix MVP;
				idRenderMatrix::Multiply( renderMatrix_clipSpaceToWindowSpace, clipMVP, MVP );
				
				RB_SetMVP( clipMVP );
			}
			else if( side < 0 )
			{
				// from OpenGL view space to OpenGL NDC ( -1 : 1 in XYZ )
				idRenderMatrix MVP;
				idRenderMatrix::Multiply( renderMatrix_windowSpaceToClipSpace, clipMVP, MVP );
				
				RB_SetMVP( MVP );
			}
			else
			{
				RB_SetMVP( clipMVP );
			}
			
			// set the local light position to allow the vertex program to project the shadow volume end cap to infinity
			/*
			idVec4 localLight( 0.0f );
			R_GlobalPointToLocal( drawSurf->space->modelMatrix, vLight->globalLightOrigin, localLight.ToVec3() );
			SetVertexParm( RENDERPARM_LOCALLIGHTORIGIN, localLight.ToFloatPtr() );
			*/
			
			currentSpace = drawSurf->space;
		}
		
		bool didDraw = false;
		
		const idMaterial* shader = drawSurf->material;
		
		// get the expressions for conditionals / color / texcoords
		const float* regs = drawSurf->shaderRegisters;
		idVec4 color( 0, 0, 0, 1 );
		
		uint64 surfGLState = 0;
		
		// set polygon offset if necessary
		if( shader && shader->TestMaterialFlag( MF_POLYGONOFFSET ) )
		{
			surfGLState |= GLS_POLYGON_OFFSET;
			GL_PolygonOffset( r_offsetFactor.GetFloat(), r_offsetUnits.GetFloat() * shader->GetPolygonOffset() );
		}
		
#if 1
		if( shader && shader->Coverage() == MC_PERFORATED )
		{
			// perforated surfaces may have multiple alpha tested stages
			for( int stage = 0; stage < shader->GetNumStages(); stage++ )
			{
				const shaderStage_t* pStage = shader->GetStage( stage );
				
				if( !pStage->hasAlphaTest )
				{
					continue;
				}
				
				// check the stage enable condition
				if( regs[ pStage->conditionRegister ] == 0 )
				{
					continue;
				}
				
				// if we at least tried to draw an alpha tested stage,
				// we won't draw the opaque surface
				didDraw = true;
				
				// set the alpha modulate
				color[3] = regs[ pStage->color.registers[3] ];
				
				// skip the entire stage if alpha would be black
				if( color[3] <= 0.0f )
				{
					continue;
				}
				
				uint64 stageGLState = surfGLState;
				
				// set privatePolygonOffset if necessary
				if( pStage->privatePolygonOffset )
				{
					GL_PolygonOffset( r_offsetFactor.GetFloat(), r_offsetUnits.GetFloat() * pStage->privatePolygonOffset );
					stageGLState |= GLS_POLYGON_OFFSET;
				}
				
				GL_Color( color );
				
				GL_State( stageGLState );
				idVec4 alphaTestValue( regs[ pStage->alphaTestRegister ] );
				SetFragmentParm( RENDERPARM_ALPHA_TEST, alphaTestValue.ToFloatPtr() );
				
				if( drawSurf->jointCache )
				{
					renderProgManager.BindShader_TextureVertexColorSkinned();
				}
				else
				{
#if defined(ANDROID) && !defined(USE_VULKAN)
					if( gpuShadowInstanceCount > 1 )
					{
						renderProgManager.BindShader_TextureVertexColorInstanced();
					}
					else
#endif
					{
						renderProgManager.BindShader_TextureVertexColor();
					}
				}
				
				RB_SetVertexColorParms( SVC_IGNORE );
				
				// bind the texture
				GL_SelectTexture( 0 );
				pStage->texture.image->Bind();
				
				// set texture matrix and texGens
				PrepareStageTexturing( pStage, drawSurf );
				
				// must render with less-equal for Z-Cull to work properly
				assert( ( GL_GetCurrentState() & GLS_DEPTHFUNC_BITS ) == GLS_DEPTHFUNC_LESS );
				
				// draw it
#if defined(ANDROID) && !defined(USE_VULKAN)
				DrawElementsWithCounters( drawSurf, gpuShadowInstanceData, gpuShadowInstanceCount );
				if( gpuShadowInstanceCount > 1 )
				{
					gpuShadowDrawCallsSaved += gpuShadowInstanceCount - 1;
				}
#else
				DrawElementsWithCounters( drawSurf );
#endif
				
				// clean up
				FinishStageTexturing( pStage, drawSurf );
				
				// unset privatePolygonOffset if necessary
				if( pStage->privatePolygonOffset )
				{
					GL_PolygonOffset( r_offsetFactor.GetFloat(), r_offsetUnits.GetFloat() * shader->GetPolygonOffset() );
				}
			}
		}
#endif
		
		if( !didDraw )
		{
			if( drawSurf->jointCache )
			{
				renderProgManager.BindShader_DepthSkinned();
			}
			else
			{
#if defined(ANDROID) && !defined(USE_VULKAN)
				if( gpuShadowInstanceCount > 1 )
				{
					renderProgManager.BindShader_DepthInstanced();
				}
				else
#endif
				{
					renderProgManager.BindShader_Depth();
				}
			}
			
#if defined(ANDROID) && !defined(USE_VULKAN)
			DrawElementsWithCounters( drawSurf, gpuShadowInstanceData, gpuShadowInstanceCount );
			if( gpuShadowInstanceCount > 1 )
			{
				gpuShadowDrawCallsSaved += gpuShadowInstanceCount - 1;
			}
#else
			DrawElementsWithCounters( drawSurf );
#endif
		}
	}

#if defined(ANDROID) && !defined(USE_VULKAN)
	if( r_gpuInstancingDebug.GetBool() && gpuShadowBatchCount > 0 )
	{
		idLib::Printf( "GPU instancing shadow-map pass (side %d): %d batches, %d draw calls saved\n",
				side, gpuShadowBatchCount, gpuShadowDrawCallsSaved );
	}
#endif
}

/*
=====================
idRenderBackend::ShadowMapPass

Renders one side into the per resolution shadow map array right before the
light's interactions. Only used for lights that didn't get a place in the
shadow atlas, every call interrupts the main render pass on tile based GPUs.
=====================
*/
void idRenderBackend::ShadowMapPass( const drawSurf_t* drawSurfs, const viewLight_t* vLight, int side, bool restoreFramebuffer )
{
	if( r_skipShadows.GetBool() )
	{
		return;
	}

	if( drawSurfs == NULL )
	{
		return;
	}

	RENDERLOG_PRINTF( "---------- RB_ShadowMapPass( side = %i ) ----------\n", side );

	BeginShadowMapSide();

	idRenderMatrix lightProjectionRenderMatrix;
	idRenderMatrix lightViewRenderMatrix;
	SetupShadowMapMatrices( vLight, side, lightViewRenderMatrix, lightProjectionRenderMatrix );

	// FIXME
#if !defined(USE_VULKAN)
	globalFramebuffers.shadowFBO[vLight->shadowLOD]->Bind();

	if( side < 0 )
	{
		globalFramebuffers.shadowFBO[vLight->shadowLOD]->AttachImageDepthLayer( globalImages->shadowImage[vLight->shadowLOD], 0 );
	}
	else
	{
		globalFramebuffers.shadowFBO[vLight->shadowLOD]->AttachImageDepthLayer( globalImages->shadowImage[vLight->shadowLOD], side );
	}

	globalFramebuffers.shadowFBO[vLight->shadowLOD]->Check();

	GL_ViewportAndScissor( 0, 0, shadowMapResolutions[vLight->shadowLOD], shadowMapResolutions[vLight->shadowLOD] );


	glClear( GL_DEPTH_BUFFER_BIT );
#endif

	DrawShadowMapCasters( drawSurfs, vLight, side, lightViewRenderMatrix, lightProjectionRenderMatrix );

	// Restore the main target only when requested. For cubemap/directional shadow
	// sets, the caller keeps the shadow FBO active across all faces/splits and
	// restores the main target once after the final pass.
	if( restoreFramebuffer )
	{
		if( UseSceneFramebuffer() /*&& !viewDef->is2Dgui*/ )
		{
			globalFramebuffers.hdrFBO->Bind();
		}
		else
		{
			Framebuffer::Unbind();
		}
	}
	renderProgManager.Unbind();

	GL_State( GLS_DEFAULT );

	SetFragmentParm( RENDERPARM_ALPHA_TEST, vec4_zero.ToFloatPtr() );
}

#if !defined( USE_VULKAN )

/*
==============================================================================================

SHADOW ATLAS

A tile based GPU keeps the color, depth and stencil of the render pass in tile
memory and writes them out only when the render pass ends. Rendering a light's
shadow maps right before its interactions ends the main render pass for every
shadowed light: the whole frame is written to memory, the shadow map rendered
and the frame read back again.

Instead, the shadow maps of all lights of all views of a frame are rendered
first, into one depth atlas, in one render pass. Every side of every light is a
square tile of the light's shadowMapResolutions size. The interaction shaders
need no change: the atlas is a 2D array with a single layer, so the layer they
pick from the side index is clamped to it, and each side's matrix already maps
into that side's tile.

Lights that don't fit keep the per light shadow maps.

==============================================================================================
*/

idCVar r_shadowAtlasSize( "r_shadowAtlasSize", "2048", CVAR_RENDERER | CVAR_INTEGER, "size of the atlas the shadow maps of all lights are rendered into before the views, so they don't interrupt the main render pass of tile based GPUs; 0 renders every light's shadow maps right before its interactions", 0, 8192 );

// guard texels around every side, the PCF taps of one side never reach the next tile
static const int SHADOW_ATLAS_TILE_BORDER = 4;

// tiles are allocated in Morton order, which packs squares of descending power of two sizes without gaps
static int R_MortonCompactBits( int v )
{
	v &= 0x55555555;
	v = ( v ^ ( v >> 1 ) ) & 0x33333333;
	v = ( v ^ ( v >> 2 ) ) & 0x0f0f0f0f;
	v = ( v ^ ( v >> 4 ) ) & 0x00ff00ff;
	v = ( v ^ ( v >> 8 ) ) & 0x0000ffff;
	return v;
}

/*
=====================
idRenderBackend::FindShadowAtlasLight
=====================
*/
const idRenderBackend::shadowAtlasLight_t* idRenderBackend::FindShadowAtlasLight( const viewLight_t* vLight ) const
{
	for( int i = 0; i < numShadowAtlasLights; i++ )
	{
		if( shadowAtlasLights[i].vLight == vLight )
		{
			return &shadowAtlasLights[i];
		}
	}
	return NULL;
}

/*
=====================
idRenderBackend::ShadowAtlasPass

Runs before the first command of the frame, while nothing is bound that a
render pass switch would have to write out.
=====================
*/
void idRenderBackend::ShadowAtlasPass( const emptyCommand_t* cmds )
{
	numShadowAtlasLights = 0;

	if( !r_useShadowMapping.GetBool() || r_skipShadows.GetBool() || r_skipInteractions.GetBool() || r_skipRender.GetBool() )
	{
		return;
	}

	// the debug single side mode leaves the other sides as they are, keep it on the per light path
	if( r_shadowMapSingleSide.GetInteger() != -1 )
	{
		return;
	}

	int atlasSize = r_shadowAtlasSize.GetInteger();
	if( glConfig.maxTextureSize > 0 )
	{
		atlasSize = Min( atlasSize, glConfig.maxTextureSize );
	}
	if( atlasSize < shadowMapResolutions[MAX_SHADOWMAP_RESOLUTIONS - 1] )
	{
		return;
	}
	atlasSize = idMath::FloorPowerOfTwo( atlasSize );

	//-------------------------------------------------
	// gather the lights that DrawInteractions renders shadow maps for
	//-------------------------------------------------
	struct atlasRequest_t
	{
		const viewDef_t*	view;
		const viewLight_t*	vLight;
		int					size;
		int					numSides;
	};
	atlasRequest_t requests[MAX_SHADOW_ATLAS_LIGHTS];
	int numRequests = 0;
	int minSize = shadowMapResolutions[0];

	for( const emptyCommand_t* cmd = cmds; cmd != NULL && numRequests < MAX_SHADOW_ATLAS_LIGHTS; cmd = ( const emptyCommand_t* )cmd->next )
	{
		if( cmd->commandId != RC_DRAW_VIEW_3D )
		{
			continue;
		}

		// DrawView skips views without surfaces, and guis don't have lights
		const viewDef_t* view = ( ( const drawSurfsCommand_t* )cmd )->viewDef;
		if( view == NULL || view->numDrawSurfs == 0 || view->viewEntitys == NULL )
		{
			continue;
		}

		for( const viewLight_t* vLight = view->viewLights; vLight != NULL && numRequests < MAX_SHADOW_ATLAS_LIGHTS; vLight = vLight->next )
		{
			if( vLight->lightShader->IsFogLight() || vLight->lightShader->IsBlendLight() )
			{
				continue;
			}
			if( vLight->localInteractions == NULL && vLight->globalInteractions == NULL && vLight->translucentInteractions == NULL )
			{
				continue;
			}
			// ambient lights are drawn without shadows
			if( vLight->globalShadows == NULL || vLight->lightShader->IsAmbientLight() )
			{
				continue;
			}

			atlasRequest_t& request = requests[numRequests++];
			request.view = view;
			request.vLight = vLight;
			request.size = shadowMapResolutions[vLight->shadowLOD];
			if( vLight->parallel )
			{
				request.numSides = r_shadowMapSplits.GetInteger() + 1;
			}
			else if( vLight->pointLight )
			{
				request.numSides = 6;
			}
			else
			{
				request.numSides = 1;
			}
			minSize = Min( minSize, request.size );
		}
	}
	if( numRequests == 0 )
	{
		return;
	}

	// largest sides first, so every tile lands aligned to its own size
	for( int i = 1; i < numRequests; i++ )
	{
		const atlasRequest_t request = requests[i];
		int j = i - 1;
		for( ; j >= 0 && requests[j].size < request.size; j-- )
		{
			requests[j + 1] = requests[j];
		}
		requests[j + 1] = request;
	}

	//-------------------------------------------------
	// place the tiles
	//-------------------------------------------------
	const atlasRequest_t* atlasRequests[MAX_SHADOW_ATLAS_LIGHTS];
	const int cellsPerRow = atlasSize / minSize;
	const int totalCells = cellsPerRow * cellsPerRow;
	int nextCell = 0;
	int usedWidth = 0;
	int usedHeight = 0;

	for( int i = 0; i < numRequests; i++ )
	{
		const atlasRequest_t& request = requests[i];
		if( request.size > atlasSize || request.size <= SHADOW_ATLAS_TILE_BORDER * 2 )
		{
			continue;
		}

		const int cellsPerSide = ( request.size / minSize ) * ( request.size / minSize );
		const int firstCell = ( ( nextCell + cellsPerSide - 1 ) / cellsPerSide ) * cellsPerSide;
		if( firstCell + cellsPerSide * request.numSides > totalCells )
		{
			// no room, this light renders its shadow maps the old way
			continue;
		}
		nextCell = firstCell + cellsPerSide * request.numSides;

		atlasRequests[numShadowAtlasLights] = &request;
		shadowAtlasLight_t& atlasLight = shadowAtlasLights[numShadowAtlasLights++];
		atlasLight.vLight = request.vLight;
		atlasLight.numSides = request.numSides;
		atlasLight.tileSize = request.size;
		for( int side = 0; side < request.numSides; side++ )
		{
			const int cell = firstCell + side * cellsPerSide;
			atlasLight.tileX[side] = R_MortonCompactBits( cell ) * minSize;
			atlasLight.tileY[side] = R_MortonCompactBits( cell >> 1 ) * minSize;
			usedWidth = Max( usedWidth, atlasLight.tileX[side] + request.size );
			usedHeight = Max( usedHeight, atlasLight.tileY[side] + request.size );
		}
	}
	if( numShadowAtlasLights == 0 )
	{
		return;
	}

	renderLog.OpenBlock( "RB_ShadowAtlasPass" );

	idImage* atlasImage = globalImages->shadowAtlasImage;
	if( !atlasImage->IsLoaded() || atlasImage->GetUploadWidth() != atlasSize )
	{
		atlasImage->GenerateShadowArray( atlasSize, atlasSize, TF_LINEAR, TR_CLAMP_TO_ZERO_ALPHA, TD_SHADOW_ATLAS );
	}
	shadowAtlasSize = atlasSize;

	// one render pass for every shadow map of the frame
	globalFramebuffers.shadowAtlasFBO->Bind();
	globalFramebuffers.shadowAtlasFBO->AttachImageDepthLayer( atlasImage, 0 );
	globalFramebuffers.shadowAtlasFBO->Check();

	// nothing of the previous frame is needed, so nothing gets loaded into the tiles
	const GLenum depthAttachment[] = { GL_DEPTH_ATTACHMENT };
	globalFramebuffers.shadowAtlasFBO->InvalidateAttachments( depthAttachment, 1 );

	glBindVertexArray( glConfig.global_vao );

	// depth writes must be on for the clear, the guard borders keep the cleared depth
	GL_State( GLS_DEFAULT );
	GL_ViewportAndScissor( 0, 0, usedWidth, usedHeight );
	glClear( GL_DEPTH_BUFFER_BIT );

	const viewDef_t* savedViewDef = viewDef;

	for( int i = 0; i < numShadowAtlasLights; i++ )
	{
		shadowAtlasLight_t& atlasLight = shadowAtlasLights[i];
		const viewLight_t* vLight = atlasLight.vLight;

		// parallel light cascades follow the camera of their view
		viewDef = atlasRequests[i]->view;

		renderLog.OpenBlock( vLight->lightShader->GetName() );

		const int innerSize = atlasLight.tileSize - SHADOW_ATLAS_TILE_BORDER * 2;
		const float tileScale = ( float )innerSize / ( float )atlasSize;

		for( int s = 0; s < atlasLight.numSides; s++ )
		{
			// projected lights have a single side that ShadowMapPass calls -1
			const int side = ( vLight->parallel || vLight->pointLight ) ? s : -1;
			const int innerX = atlasLight.tileX[s] + SHADOW_ATLAS_TILE_BORDER;
			const int innerY = atlasLight.tileY[s] + SHADOW_ATLAS_TILE_BORDER;

			BeginShadowMapSide();

			idRenderMatrix lightViewRenderMatrix;
			idRenderMatrix lightProjectionRenderMatrix;
			SetupShadowMapMatrices( vLight, side, lightViewRenderMatrix, lightProjectionRenderMatrix );

			GL_ViewportAndScissor( innerX, innerY, innerSize, innerSize );

			DrawShadowMapCasters( vLight->globalShadows, vLight, side, lightViewRenderMatrix, lightProjectionRenderMatrix );

			// world space -> 0..1 shadow map space, like RenderInteractions builds it
			idRenderMatrix lightViewProjection;
			idRenderMatrix::Multiply( lightProjectionRenderMatrix, lightViewRenderMatrix, lightViewProjection );

			idRenderMatrix worldToWindow;
			if( side < 0 )
			{
				// the base light projection of a projected light is 0..1 already
				worldToWindow = lightViewProjection;
			}
			else
			{
				idRenderMatrix::Multiply( renderMatrix_clipSpaceToWindowSpace, lightViewProjection, worldToWindow );
			}

			// and 0..1 into this side's tile, before the divide by w
			const idRenderMatrix tileMatrix(
				tileScale, 0.0f, 0.0f, ( float )innerX / ( float )atlasSize,
				0.0f, tileScale, 0.0f, ( float )innerY / ( float )atlasSize,
				0.0f, 0.0f, 1.0f, 0.0f,
				0.0f, 0.0f, 0.0f, 1.0f );
			idRenderMatrix::Multiply( tileMatrix, worldToWindow, atlasLight.worldToShadow[s] );
		}

		renderLog.CloseBlock();
	}

	viewDef = savedViewDef;

	renderProgManager.Unbind();
	GL_State( GLS_DEFAULT );
	SetFragmentParm( RENDERPARM_ALPHA_TEST, vec4_zero.ToFloatPtr() );

	Framebuffer::Unbind();

	renderLog.CloseBlock();
}

#endif // !defined( USE_VULKAN )

#if defined(ANDROID) && !defined(USE_VULKAN)
/*
==============================================================================================

TBDR: ON-TILE DEFERRED SHADING

Every light draws all the surfaces it lights again, with all their textures, in the forward
interactions. A tile based GPU keeps the render pass of the view in its tile memory, so a
G-buffer next to the scene color costs no memory traffic there: DeferredGBufferPass draws
the surfaces once into it, and every light is then one draw of its volume that reads the
G-buffer of its pixels with GL_EXT_shader_framebuffer_fetch (DrawDeferredLight). The
G-buffer is cleared when the view starts and invalidated when the lights are done, it never
leaves the tile memory. See DeferredShaders_GL.h for its layout.

A light from the G-buffer lights the same surfaces as its interactions:
- the front end only lets a light do it when every area its volume reaches holds its
  interactions (viewLight_t::deferredVolumeOk), a light behind a closed door or one that
  flows through the portals keeps its interactions
- the surfaces of more than one interaction (several bump stages), of an invisible ink
  spectrum or translucent stay out of the G-buffer and keep their interactions
- lights with local stencil shadows, which mustn't darken the noSelfShadow surfaces, keep
  their interactions
The stencil shadows and the shadow maps work as before: the light volume is drawn with the
stencil test of the interactions, or reads the shadow maps like interactionSM.

Without HDR the views draw into the HDR target too, it holds the G-buffer, and
DeferredResolve copies the view to the back buffer like the tonemap does.

==============================================================================================
*/

idCVar r_useDeferredShading( "r_useDeferredShading", "1", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_BOOL, "TBDR: light the views from a G-buffer in the tile memory, read with GL_EXT_shader_framebuffer_fetch (GLES 3.2), instead of drawing the lit surfaces again for every light. Off without the extension and with MSAA" );
idCVar r_showDeferredShading( "r_showDeferredShading", "0", CVAR_RENDERER | CVAR_BOOL, "print every frame how many lights the on-tile deferred shading drew from the G-buffer and how many as interactions" );

/*
==================
idRenderBackend::DeferredBeginFrame
==================
*/
void idRenderBackend::DeferredBeginFrame( const emptyCommand_t* cmds )
{
	if( r_showDeferredShading.GetBool() && ( deferredLightsDeferred + deferredLightsForward ) > 0 )
	{
		idLib::Printf( "on-tile deferred shading: %d lights from the G-buffer, %d as interactions\n", deferredLightsDeferred, deferredLightsForward );
	}
	deferredLightsDeferred = 0;
	deferredLightsForward = 0;

	deferredFrame = false;
	deferredView = false;

	if( !r_useDeferredShading.GetBool() || glConfig.multisamples > 0 || deferredSelfTestResult < 0 )
	{
		// the scene color alone again
		if( deferredGBufferAttached )
		{
			DeferredReleaseTargets();
		}
		return;
	}

	// a frame without 3D views keeps what is attached
	bool any3DView = false;
	for( const emptyCommand_t* cmd = cmds; cmd != NULL && !any3DView; cmd = ( const emptyCommand_t* )cmd->next )
	{
		if( cmd->commandId == RC_DRAW_VIEW_3D )
		{
			const viewDef_t* view = ( ( const drawSurfsCommand_t* )cmd )->viewDef;
			any3DView = ( view != NULL && view->viewEntitys != NULL && view->numDrawSurfs > 0 && !view->is2Dgui );
		}
	}
	if( !any3DView || r_skipRender.GetBool() )
	{
		return;
	}

	// the first time: the programs, the extension and the test of the driver,
	// later reloadShaders may have lost a program
	if( !DeferredSelfTest() || !renderProgManager.DeferredShadersReady() )
	{
		if( deferredGBufferAttached )
		{
			DeferredReleaseTargets();
		}
		return;
	}

	deferredFrame = DeferredUpdateTargets();
}

/*
==================
R_DeferredFinishInteraction

What DrawSingleInteraction does to the interaction before it draws it, false when it wouldn't draw it
==================
*/
static bool R_DeferredFinishInteraction( drawInteraction_t* din )
{
	if( din->bumpImage == NULL )
	{
		// stage wasn't actually an interaction
		return false;
	}

	if( din->diffuseImage == NULL || r_skipDiffuse.GetBool() )
	{
		din->diffuseImage = globalImages->blackImage;
	}
	if( din->specularImage == NULL || r_skipSpecular.GetBool() )
	{
		din->specularImage = globalImages->blackImage;
	}
	if( r_skipBump.GetBool() )
	{
		din->bumpImage = globalImages->flatNormalMap;
	}

	const bool diffuseIsBlack = ( din->diffuseImage == globalImages->blackImage )
								|| ( ( din->diffuseColor[0] <= 0 ) && ( din->diffuseColor[1] <= 0 ) && ( din->diffuseColor[2] <= 0 ) );
	const bool specularIsBlack = ( din->specularImage == globalImages->blackImage )
								 || ( ( din->specularColor[0] <= 0 ) && ( din->specularColor[1] <= 0 ) && ( din->specularColor[2] <= 0 ) );
	return !( diffuseIsBlack && specularIsBlack );
}

/*
==================
idRenderBackend::DeferredSurfaceInteraction

Goes through the stages like RenderInteractions does, for a light of color 1: -1 when the G-buffer
can't hold the surface, else the number of interactions it draws, 0 or 1, the one in inter.
The view surface and the light surfaces of a model surface share the material and the registers.
==================
*/
int idRenderBackend::DeferredSurfaceInteraction( const drawSurf_t* surf, drawInteraction_t* inter )
{
	const idMaterial* material = surf->material;
	if( material == NULL || surf->shaderRegisters == NULL || material->Coverage() == MC_TRANSLUCENT
			|| !material->ReceivesLighting() || material->Spectrum() != 0 )
	{
		return -1;
	}

	// identity texture matrices, a stage the material doesn't have samples a black image
	const idVec4 sMatrix( 1, 0, 0, 0 );
	const idVec4 tMatrix( 0, 1, 0, 0 );

	drawInteraction_t din = {};
	din.surf = surf;
	din.bumpImage = NULL;
	din.diffuseImage = NULL;
	din.specularImage = NULL;
	din.vertexColor = SVC_IGNORE;
	din.ambientLight = 0;
	din.bumpMatrix[0] = sMatrix;
	din.bumpMatrix[1] = tMatrix;
	din.diffuseMatrix[0] = sMatrix;
	din.diffuseMatrix[1] = tMatrix;
	din.specularMatrix[0] = sMatrix;
	din.specularMatrix[1] = tMatrix;

	// see RB_SetupForFastPathInteractions
	if( material->GetFastPathBumpImage() && !r_skipInteractionFastPath.GetBool() )
	{
		din.bumpImage = material->GetFastPathBumpImage();
		din.specularImage = material->GetFastPathSpecularImage();
		din.diffuseImage = material->GetFastPathDiffuseImage();
		din.diffuseColor = idVec4( 1.0f, 1.0f, 1.0f, 1.0f );
		din.specularColor = idVec4( 1.0f, 1.0f, 1.0f, 1.0f );
		if( inter != NULL )
		{
			*inter = din;
		}
		return 1;
	}

	static const float lightColorOne[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
	const float* surfaceRegs = surf->shaderRegisters;

	din.diffuseColor = idVec4( 0.0f, 0.0f, 0.0f, 0.0f );
	din.specularColor = idVec4( 0.0f, 0.0f, 0.0f, 0.0f );

	int numInteractions = 0;
	for( int surfaceStageNum = 0; surfaceStageNum < material->GetNumStages(); surfaceStageNum++ )
	{
		const shaderStage_t* surfaceStage = material->GetStage( surfaceStageNum );

		bool previousDone = false;
		switch( surfaceStage->lighting )
		{
			case SL_BUMP:
				if( !surfaceRegs[ surfaceStage->conditionRegister ] )
				{
					break;
				}
				previousDone = ( din.bumpImage != NULL );
				break;
			case SL_DIFFUSE:
				if( !surfaceRegs[ surfaceStage->conditionRegister ] )
				{
					break;
				}
				previousDone = ( din.diffuseImage != NULL );
				break;
			case SL_SPECULAR:
				if( !surfaceRegs[ surfaceStage->conditionRegister ] )
				{
					break;
				}
				previousDone = ( din.specularImage != NULL );
				break;
			default:
				// coverage and ambient stages are no interactions
				continue;
		}
		if( !surfaceRegs[ surfaceStage->conditionRegister ] )
		{
			continue;
		}

		// RenderInteractions draws the previous interaction
		if( previousDone && R_DeferredFinishInteraction( &din ) )
		{
			if( ++numInteractions > 1 )
			{
				return -1;
			}
			if( inter != NULL )
			{
				*inter = din;
			}
		}

		switch( surfaceStage->lighting )
		{
			case SL_BUMP:
				din.bumpImage = surfaceStage->texture.image;
				din.diffuseImage = NULL;
				din.specularImage = NULL;
				SetupInteractionStage( surfaceStage, surfaceRegs, NULL, din.bumpMatrix, NULL );
				break;
			case SL_DIFFUSE:
				din.diffuseImage = surfaceStage->texture.image;
				din.vertexColor = surfaceStage->vertexColor;
				SetupInteractionStage( surfaceStage, surfaceRegs, lightColorOne, din.diffuseMatrix, din.diffuseColor.ToFloatPtr() );
				break;
			case SL_SPECULAR:
				din.specularImage = surfaceStage->texture.image;
				din.vertexColor = surfaceStage->vertexColor;
				SetupInteractionStage( surfaceStage, surfaceRegs, lightColorOne, din.specularMatrix, din.specularColor.ToFloatPtr() );
				break;
			default:
				break;
		}
	}

	// the final interaction
	if( R_DeferredFinishInteraction( &din ) )
	{
		if( ++numInteractions > 1 )
		{
			return -1;
		}
		if( inter != NULL )
		{
			*inter = din;
		}
	}

	return numInteractions;
}

/*
==================
idRenderBackend::DeferredSurface

The surface is in the G-buffer, or it has no interaction at all
==================
*/
bool idRenderBackend::DeferredSurface( const drawSurf_t* surf )
{
	return DeferredSurfaceInteraction( surf, NULL ) >= 0;
}

/*
==================
idRenderBackend::DeferredLight

The light is drawn from the G-buffer, see the top of this section
==================
*/
bool idRenderBackend::DeferredLight( const viewLight_t* vLight ) const
{
	if( !deferredView || !vLight->deferredVolumeOk )
	{
		return false;
	}

	const idMaterial* lightShader = vLight->lightShader;
	if( lightShader->IsFogLight() || lightShader->IsBlendLight() || lightShader->Spectrum() != 0 )
	{
		return false;
	}

	// the stencil test of the volume can't keep the local shadows off the noSelfShadow surfaces
	if( !r_useShadowMapping.GetBool() && vLight->localShadows != NULL )
	{
		return false;
	}

	// no opaque surface gets the light from its interactions either
	return ( vLight->localInteractions != NULL || vLight->globalInteractions != NULL );
}

/*
==================
idRenderBackend::DeferredGBufferPass

AmbientPass for the G-buffer: the surfaces of the depth pass with one interaction write it,
and the ambient pass into the scene color, which AmbientPass skips for them then.
==================
*/
void idRenderBackend::DeferredGBufferPass( const drawSurf_t* const* drawSurfs, int numDrawSurfs )
{
	if( numDrawSurfs == 0 || viewDef->viewEntitys == NULL )
	{
		return;
	}

	renderLog.OpenBlock( "RB_DeferredGBufferPass" );

	bool* gpuInstanceConsumed = ( bool* )_alloca( numDrawSurfs * sizeof( bool ) );
	memset( gpuInstanceConsumed, 0, numDrawSurfs * sizeof( bool ) );

	// every merged world group is drawn once in this pass
	++s_worldMergeStamp;

	// force MVP change on first surface
	currentSpace = NULL;

	// the ambient of AmbientPass
	const bool writeAmbient = ( r_forceAmbient.GetFloat() > 0 && !r_skipAmbient.GetBool() );
	float ambientBoost = 1.0f;
	ambientBoost += r_useSSAO.GetBool() ? 0.2f : 0.0f;
	ambientBoost *= r_useHDR.GetBool() ? 1.1f : 1.0f;
	const idVec4 ambientColor( r_forceAmbient.GetFloat() * ambientBoost, r_forceAmbient.GetFloat() * ambientBoost, r_forceAmbient.GetFloat() * ambientBoost, 1.0f );
	renderProgManager.SetRenderParm( RENDERPARM_AMBIENT_COLOR, ambientColor.ToFloatPtr() );

	const float gbufferParms[4] = { r_lightScale.GetFloat(), writeAmbient ? 1.0f : 0.0f, 0.0f, 0.0f };
	SetFragmentParm( RENDERPARM_USER3, gbufferParms );

	// without the ambient the scene color keeps the black of the depth pass
	const uint64 sceneColorMask = writeAmbient ? 0 : ( GLS_COLORMASK | GLS_ALPHAMASK );
	DeferredSetGBufferWrites( true );

	int gpuInstanceBatchCount = 0;
	int numDrawn = 0;

	for( int i = 0; i < numDrawSurfs; i++ )
	{
		if( gpuInstanceConsumed[i] )
		{
			continue;
		}

		const drawSurf_t* drawSurf = drawSurfs[i];
		const idMaterial* surfaceMaterial = drawSurf->material;

		// translucent surfaces don't put anything in the depth buffer
		if( surfaceMaterial->Coverage() == MC_TRANSLUCENT )
		{
			continue;
		}

		// if all stages of a material have been conditioned off, don't do anything
		const float* surfaceRegs = drawSurf->shaderRegisters;
		int stage = 0;
		for( ; stage < surfaceMaterial->GetNumStages(); stage++ )
		{
			if( surfaceRegs[ surfaceMaterial->GetStage( stage )->conditionRegister ] != 0 )
			{
				break;
			}
		}
		if( stage == surfaceMaterial->GetNumStages() )
		{
			continue;
		}

		// the surfaces of more than one interaction are lit by their interactions
		drawInteraction_t inter;
		if( DeferredSurfaceInteraction( drawSurf, &inter ) != 1 )
		{
			continue;
		}

		// a merged world surface draws its whole material group, the rest of the group is done then
		worldMergeSurf = NULL;
		if( R_WorldMergeSkip( drawSurf ) )
		{
			continue;
		}
		if( R_IsWorldMergeGroupMember( drawSurf ) )
		{
			BeginWorldMergeGroup( drawSurf );
		}

		const gpuInstanceData_t* gpuInstanceData = NULL;
		int gpuInstanceCount = 1;
		gpuInstanceData_t gpuInstances[MAX_GPU_INSTANCES_PER_DRAW];
		if( glConfig.glVersion >= 3.2f )
		{
			gpuInstanceCount = R_BuildGPUInstanceBatch( drawSurfs, numDrawSurfs, i, viewDef->renderView.vieworg, gpuInstanceConsumed, gpuInstances, surfaceMaterial->Coverage() == MC_PERFORATED );
			if( gpuInstanceCount > 1 )
			{
				gpuInstanceData = gpuInstances;
				gpuInstanceBatchCount++;
			}
		}

		if( drawSurf->jointCache )
		{
			renderProgManager.BindShader_DeferredGBufferSkinned();
		}
		else if( gpuInstanceCount > 1 )
		{
			renderProgManager.BindShader_DeferredGBufferInstanced();
		}
		else
		{
			renderProgManager.BindShader_DeferredGBuffer();
		}

		// change the matrix if needed
		if( drawSurf->space != currentSpace )
		{
			currentSpace = drawSurf->space;

			RB_SetMVP( drawSurf->space->mvp );

			// tranform the view origin into model local space
			idVec4 localViewOrigin( 1.0f );
			R_GlobalPointToLocal( drawSurf->space->modelMatrix, viewDef->renderView.vieworg, localViewOrigin.ToVec3() );
			SetVertexParm( RENDERPARM_LOCALVIEWORIGIN, localViewOrigin.ToFloatPtr() );

			// the G-buffer is in view space
			float modelViewMatrixTranspose[16];
			R_MatrixTranspose( drawSurf->space->modelViewMatrix, modelViewMatrixTranspose );
			SetVertexParms( RENDERPARM_MODELVIEWMATRIX_X, modelViewMatrixTranspose, 4 );
		}

		SetVertexParm( RENDERPARM_BUMPMATRIX_S, inter.bumpMatrix[0].ToFloatPtr() );
		SetVertexParm( RENDERPARM_BUMPMATRIX_T, inter.bumpMatrix[1].ToFloatPtr() );
		SetVertexParm( RENDERPARM_DIFFUSEMATRIX_S, inter.diffuseMatrix[0].ToFloatPtr() );
		SetVertexParm( RENDERPARM_DIFFUSEMATRIX_T, inter.diffuseMatrix[1].ToFloatPtr() );
		SetVertexParm( RENDERPARM_SPECULARMATRIX_S, inter.specularMatrix[0].ToFloatPtr() );
		SetVertexParm( RENDERPARM_SPECULARMATRIX_T, inter.specularMatrix[1].ToFloatPtr() );
		RB_SetVertexColorParms( inter.vertexColor );

		// the colors of the material stages, the light pass multiplies the light color with them
		SetFragmentParm( RENDERPARM_DIFFUSEMODIFIER, inter.diffuseColor.ToFloatPtr() );
		SetFragmentParm( RENDERPARM_SPECULARMODIFIER, inter.specularColor.ToFloatPtr() );

		// AmbientPass adds the ambient of vertex colored stages to the black of the depth pass
		uint64 surfGLState = GLS_DEPTHMASK | GLS_DEPTHFUNC_EQUAL | sceneColorMask;
		if( writeAmbient && inter.vertexColor != SVC_IGNORE )
		{
			surfGLState |= GLS_SRCBLEND_ONE | GLS_DSTBLEND_ONE;
		}
		else
		{
			surfGLState |= GLS_SRCBLEND_ONE | GLS_DSTBLEND_ZERO;
		}
		GL_State( surfGLState );

		GL_SelectTexture( INTERACTION_TEXUNIT_BUMP );
		inter.bumpImage->Bind();

		GL_SelectTexture( INTERACTION_TEXUNIT_SPECULARMIX );
		inter.specularImage->Bind();

		GL_SelectTexture( INTERACTION_TEXUNIT_BASECOLOR );
		inter.diffuseImage->Bind();

		renderLog.OpenBlock( surfaceMaterial->GetName() );
		DrawElementsWithCounters( drawSurf, gpuInstanceData, gpuInstanceCount );
		renderLog.CloseBlock();
		numDrawn++;
	}

	DeferredSetGBufferWrites( false );
	worldMergeSurf = NULL;

	if( r_gpuInstancingDebug.GetBool() )
	{
		idLib::Printf( "on-tile deferred shading G-buffer: %d draws, %d instanced batches\n", numDrawn, gpuInstanceBatchCount );
	}

	GL_SelectTexture( 0 );
	renderProgManager.Unbind();

	renderLog.CloseBlock();
}

/*
==================
idRenderBackend::DrawDeferredLight

Adds the light to the G-buffer pixels of its volume, what its interactions add to them: draws the
volume with depth greater equal, so only the pixels of the surfaces in front of its back sides, and
the program reads their G-buffer and the scene color with the framebuffer fetch.
The interactions are culled to the light volume, the program skips the pixels outside of it.
Everything is in view space, the G-buffer has the view depth.
False when the light must be drawn as interactions.
==================
*/
bool idRenderBackend::DrawDeferredLight( const viewLight_t* vLight, bool performStencilTest )
{
	// view space -> world space
	idRenderMatrix worldToView;
	idRenderMatrix::Transpose( *( const idRenderMatrix* )viewDef->worldSpace.modelViewMatrix, worldToView );
	idRenderMatrix viewToWorld;
	if( !idRenderMatrix::Inverse( worldToView, viewToWorld ) )
	{
		return false;
	}

	// the plane a dotted with a world space x y z 1, for a view space x y z 1
	auto planeToView = [&viewToWorld]( const float* plane, float* out )
	{
		for( int j = 0; j < 4; j++ )
		{
			out[j] = plane[0] * viewToWorld[0][j] + plane[1] * viewToWorld[1][j] + plane[2] * viewToWorld[2][j] + plane[3] * viewToWorld[3][j];
		}
	};

	const idMaterial* lightShader = vLight->lightShader;
	const float* lightRegs = vLight->shaderRegisters;
	const bool ambientLight = lightShader->IsAmbientLight();
	const bool useShadowMaps = r_useShadowMapping.GetBool() && vLight->globalShadows != NULL && !ambientLight;

	renderLog.OpenBlock( "Deferred Light" );

	// the scissor of the interactions
	if( !currentScissor.Equals( vLight->scissorRect ) && r_useScissor.GetBool() )
	{
		GL_Scissor( viewDef->viewport.x1 + vLight->scissorRect.x1,
					viewDef->viewport.y1 + vLight->scissorRect.y1,
					vLight->scissorRect.x2 + 1 - vLight->scissorRect.x1,
					vLight->scissorRect.y2 + 1 - vLight->scissorRect.y1 );

		currentScissor = vLight->scissorRect;
	}

	// The sides of the volume behind the surfaces: inside of it only its back side passes. Both
	// facings are drawn, a mirror and a projection turn the winding around, and in front of the
	// volume where both pass the program finds the pixels outside of it. The program adds the
	// light to the fetched scene color itself.
	uint64 glState = GLS_SRCBLEND_ONE | GLS_DSTBLEND_ZERO | GLS_DEPTHMASK | GLS_DEPTHFUNC_GREATER | GLS_CULL_TWOSIDED;
	if( performStencilTest )
	{
		glState |= GLS_STENCIL_FUNC_EQUAL | GLS_STENCIL_MAKE_REF( STENCIL_SHADOW_TEST_VALUE ) | GLS_STENCIL_MAKE_MASK( STENCIL_SHADOW_MASK_VALUE );
	}
	else
	{
		glState |= GLS_STENCIL_FUNC_ALWAYS;
	}
	GL_State( glState );

	if( ambientLight )
	{
		renderProgManager.BindShader_DeferredLight( idRenderProgManager::DEFERRED_LIGHT_AMBIENT );
	}
	else if( useShadowMaps )
	{
		if( vLight->parallel )
		{
			renderProgManager.BindShader_DeferredLight( idRenderProgManager::DEFERRED_LIGHT_PARALLEL );
		}
		else if( vLight->pointLight )
		{
			renderProgManager.BindShader_DeferredLight( idRenderProgManager::DEFERRED_LIGHT_POINT );
		}
		else
		{
			renderProgManager.BindShader_DeferredLight( idRenderProgManager::DEFERRED_LIGHT_SPOT );
		}
	}
	else
	{
		renderProgManager.BindShader_DeferredLight( idRenderProgManager::DEFERRED_LIGHT_PLAIN );
	}

	// window coordinates -> view space position on the view depth
	const idScreenRect& viewport = viewDef->viewport;
	const float viewportWidth = ( float )( viewport.x2 + 1 - viewport.x1 );
	const float viewportHeight = ( float )( viewport.y2 + 1 - viewport.y1 );
	const float windowToNDC[4] = { 2.0f / viewportWidth, 2.0f / viewportHeight,
								   -1.0f - 2.0f * ( float )viewport.x1 / viewportWidth, -1.0f - 2.0f * ( float )viewport.y1 / viewportHeight
								 };
	SetFragmentParm( RENDERPARM_USER0, windowToNDC );

	const idRenderMatrix& projection = viewDef->projectionRenderMatrix;
	const float projectionX[4] = { 1.0f / projection[0][0], projection[0][2], projection[0][3], projection[3][2] };
	const float projectionY[4] = { 1.0f / projection[1][1], projection[1][2], projection[1][3], projection[3][3] };
	SetFragmentParm( RENDERPARM_USER1, projectionX );
	SetFragmentParm( RENDERPARM_USER2, projectionY );

	// the light in view space
	const idVec3& lightOrigin = vLight->globalLightOrigin;
	float viewLightOrigin[4];
	for( int i = 0; i < 4; i++ )
	{
		viewLightOrigin[i] = worldToView[i][0] * lightOrigin.x + worldToView[i][1] * lightOrigin.y + worldToView[i][2] * lightOrigin.z + worldToView[i][3];
	}
	SetFragmentParm( RENDERPARM_LOCALLIGHTORIGIN, viewLightOrigin );

	// the volume: 0 < x, y, z < w in the zero to one cube, see idRenderMatrix::CullPointToMVP
	for( int i = 0; i < 4; i++ )
	{
		float row[4];
		planeToView( vLight->baseLightProject[i], row );
		SetFragmentParm( ( renderParm_t )( RENDERPARM_TEXGEN_0_S + i ), row );
	}

	if( useShadowMaps )
	{
		// what RenderInteractions sets up for the shadow map interactions
		const shadowAtlasLight_t* atlasLight = FindShadowAtlasLight( vLight );

		idRenderMatrix lightWorldToShadow[6];
		int numShadowSides = 0;
		if( atlasLight != NULL )
		{
			numShadowSides = atlasLight->numSides;
			for( int i = 0; i < numShadowSides; i++ )
			{
				lightWorldToShadow[i] = atlasLight->worldToShadow[i];
			}
		}
		else
		{
			// ShadowMapPass left the matrices of this light in shadowV / shadowP
			if( vLight->parallel )
			{
				numShadowSides = r_shadowMapSplits.GetInteger() + 1;
			}
			else if( vLight->pointLight )
			{
				numShadowSides = 6;
			}
			else
			{
				numShadowSides = 1;
			}
			numShadowSides = Min( numShadowSides, 6 );
			for( int i = 0; i < numShadowSides; i++ )
			{
				idRenderMatrix lightViewProjection;
				idRenderMatrix::Multiply( shadowP[i], shadowV[i], lightViewProjection );
				if( vLight->parallel || vLight->pointLight )
				{
					idRenderMatrix::Multiply( renderMatrix_clipSpaceToWindowSpace, lightViewProjection, lightWorldToShadow[i] );
				}
				else
				{
					// the base light projection of a projected light is 0..1 already
					lightWorldToShadow[i] = lightViewProjection;
				}
			}
		}

		for( int i = 0; i < numShadowSides; i++ )
		{
			idRenderMatrix viewToShadow;
			idRenderMatrix::Multiply( lightWorldToShadow[i], viewToWorld, viewToShadow );
			renderProgManager.SetRenderParms( ( renderParm_t )( RENDERPARM_SHADOW_MATRIX_0_X + i * 4 ), viewToShadow[0], 4 );
		}

		// the side of a point light is the world axis closest to the direction to the light
		SetFragmentParm( RENDERPARM_MODELMATRIX_X, viewToWorld[0] );
		SetFragmentParm( RENDERPARM_MODELMATRIX_Y, viewToWorld[1] );
		SetFragmentParm( RENDERPARM_MODELMATRIX_Z, viewToWorld[2] );

		const static int JITTER_SIZE = 128;

		// default high quality
		float jitterSampleScale = 1.0f;
		float shadowMapSamples = r_shadowMapSamples.GetInteger();

		// screen power of two correction factor
		float screenCorrectionParm[4];
		screenCorrectionParm[0] = 1.0f / ( JITTER_SIZE * shadowMapSamples ) ;
		screenCorrectionParm[1] = 1.0f / JITTER_SIZE;
		// texel size for the PCF taps, the atlas texel is as big as a texel of the light's own resolution
		screenCorrectionParm[2] = ( atlasLight != NULL ) ? 1.0f / shadowAtlasSize : 1.0f / shadowMapResolutions[vLight->shadowLOD];
		screenCorrectionParm[3] = vLight->parallel ? r_shadowMapSunDepthBiasScale.GetFloat() : r_shadowMapRegularDepthBiasScale.GetFloat();
		SetFragmentParm( RENDERPARM_SCREENCORRECTIONFACTOR, screenCorrectionParm ); // rpScreenCorrectionFactor

		float jitterTexScale[4];
		jitterTexScale[0] = r_shadowMapJitterScale.GetFloat() * jitterSampleScale;
		jitterTexScale[1] = r_shadowMapJitterScale.GetFloat() * jitterSampleScale;
		jitterTexScale[2] = -r_shadowMapBiasScale.GetFloat();
		jitterTexScale[3] = 0.0f;
		SetFragmentParm( RENDERPARM_JITTERTEXSCALE, jitterTexScale ); // rpJitterTexScale

		float jitterTexOffset[4];
		if( r_shadowMapRandomizeJitter.GetBool() )
		{
			jitterTexOffset[0] = ( rand() & 255 ) / 255.0;
			jitterTexOffset[1] = ( rand() & 255 ) / 255.0;
		}
		else
		{
			jitterTexOffset[0] = 0;
			jitterTexOffset[1] = 0;
		}
		jitterTexOffset[2] = 0.0f;
		jitterTexOffset[3] = 0.0f;
		SetFragmentParm( RENDERPARM_JITTERTEXOFFSET, jitterTexOffset ); // rpJitterTexOffset

		if( vLight->parallel )
		{
			float cascadeDistances[4];
			cascadeDistances[0] = viewDef->frustumSplitDistances[0];
			cascadeDistances[1] = viewDef->frustumSplitDistances[1];
			cascadeDistances[2] = viewDef->frustumSplitDistances[2];
			cascadeDistances[3] = viewDef->frustumSplitDistances[3];
			SetFragmentParm( RENDERPARM_CASCADEDISTANCES, cascadeDistances ); // rpCascadeDistances
		}

		// texture 5 will be the shadow maps array
		GL_SelectTexture( INTERACTION_TEXUNIT_SHADOWMAPS );
		if( atlasLight != NULL )
		{
			globalImages->shadowAtlasImage->Bind();
		}
		else
		{
			globalImages->shadowImage[vLight->shadowLOD]->Bind();
		}

		// texture 6 will be the jitter texture for soft shadowing
		GL_SelectTexture( INTERACTION_TEXUNIT_JITTER );
		if( r_shadowMapSamples.GetInteger() == 16 )
		{
			globalImages->jitterImage16->Bind();
		}
		else if( r_shadowMapSamples.GetInteger() == 4 )
		{
			globalImages->jitterImage4->Bind();
		}
		else
		{
			globalImages->jitterImage1->Bind();
		}
	}

	// the zero to one cube deformed into the light volume
	idRenderMatrix lightVolumeMVP;
	idRenderMatrix::Multiply( viewDef->worldSpace.mvp, vLight->inverseBaseLightProject, lightVolumeMVP );
	RB_SetMVP( lightVolumeMVP );

	const float lightScale = r_lightScale.GetFloat();

	// some rare lights have multiple animating stages
	for( int lightStageNum = 0; lightStageNum < lightShader->GetNumStages(); lightStageNum++ )
	{
		const shaderStage_t* lightStage = lightShader->GetStage( lightStageNum );

		// ignore stages that fail the condition
		if( !lightRegs[ lightStage->conditionRegister ] )
		{
			continue;
		}

		const idVec4 lightColor(
			lightScale * lightRegs[ lightStage->color.registers[0] ],
			lightScale * lightRegs[ lightStage->color.registers[1] ],
			lightScale * lightRegs[ lightStage->color.registers[2] ],
			lightRegs[ lightStage->color.registers[3] ] );
		if( lightColor[0] <= 0.0f && lightColor[1] <= 0.0f && lightColor[2] <= 0.0f )
		{
			// the interactions would draw nothing
			continue;
		}

		// apply the world-global overbright and the 2x factor for specular
		const idVec4 specularColor = lightColor * 2.0f;
		SetFragmentParm( RENDERPARM_DIFFUSEMODIFIER, lightColor.ToFloatPtr() );
		SetFragmentParm( RENDERPARM_SPECULARMODIFIER, specularColor.ToFloatPtr() );

		// the light projection with the texture matrix of the stage, in view space
		idPlane lightProjection[4];
		for( int i = 0; i < 4; i++ )
		{
			lightProjection[i] = vLight->lightProject[i];
		}
		if( lightStage->texture.hasMatrix )
		{
			float lightTextureMatrix[16];
			RB_GetShaderTextureMatrix( lightRegs, &lightStage->texture, lightTextureMatrix );
			RB_BakeTextureMatrixIntoTexgen( lightProjection, lightTextureMatrix );
		}
		for( int i = 0; i < 4; i++ )
		{
			float plane[4];
			planeToView( lightProjection[i].ToFloatPtr(), plane );
			SetFragmentParm( ( renderParm_t )( RENDERPARM_LIGHTPROJECTION_S + i ), plane );
		}

		// texture 3 will be the light falloff texture
		GL_SelectTexture( INTERACTION_TEXUNIT_FALLOFF );
		vLight->falloffImage->Bind();

		// texture 4 will be the light projection texture
		GL_SelectTexture( INTERACTION_TEXUNIT_PROJECTION );
		lightStage->texture.image->Bind();

		DrawElementsWithCounters( &zeroOneCubeSurface );
	}

	GL_SelectTexture( 0 );

	renderLog.CloseBlock();
	return true;
}

/*
==================
idRenderBackend::DeferredResolve

Without HDR: the view from the scene target to the back buffer, where the tonemap would draw it.
Only the view's part of the back buffer, a gui may have drawn the rest.
==================
*/
void idRenderBackend::DeferredResolve( const viewDef_t* _viewDef )
{
	renderLog.OpenBlock( "RB_DeferredResolve" );

	const int screenWidth = renderSystem->GetWidth();
	const int screenHeight = renderSystem->GetHeight();
	const idScreenRect& viewport = _viewDef->viewport;
	const bool fullScreen = ( viewport.x1 <= 0 && viewport.y1 <= 0 && viewport.x2 + 1 >= screenWidth && viewport.y2 + 1 >= screenHeight );

	if( fullScreen && !backBufferDrawnThisFrame )
	{
		// every pixel of the back buffer is replaced, nothing is loaded into the tiles
		Framebuffer::InvalidateDefaultFramebufferForOverwrite();
	}
	else
	{
		Framebuffer::Unbind();
	}
	backBufferDrawnThisFrame = true;

	GL_State( GLS_SRCBLEND_ONE | GLS_DSTBLEND_ZERO | GLS_DEPTHMASK | GLS_DEPTHFUNC_ALWAYS | GLS_CULL_TWOSIDED );
	GL_Viewport( 0, 0, screenWidth, screenHeight );
	GL_Scissor( viewport.x1, viewport.y1, viewport.x2 + 1 - viewport.x1, viewport.y2 + 1 - viewport.y1 );

	GL_SelectTexture( 0 );
	globalImages->currentRenderHDRImage->Bind();

	renderProgManager.BindShader_DeferredResolve();
	DrawElementsWithCounters( &unitSquareSurface );

	renderProgManager.Unbind();
	GL_State( GLS_DEFAULT );

	renderLog.CloseBlock();
}
#endif // defined(ANDROID) && !defined(USE_VULKAN)

/*
==============================================================================================

DRAW INTERACTIONS

==============================================================================================
*/
/*
==================
idRenderBackend::DrawInteractions
==================
*/
void idRenderBackend::DrawInteractions( const viewDef_t* _viewDef )
{
	if( r_skipInteractions.GetBool() )
	{
		return;
	}
	
	renderLog.OpenMainBlock( MRB_DRAW_INTERACTIONS );
	renderLog.OpenBlock( "RB_DrawInteractions" );
	
	GL_SelectTexture( 0 );
	
	
	const bool useLightDepthBounds = r_useLightDepthBounds.GetBool() && !r_useShadowMapping.GetBool();
	
	//
	// for each light, perform shadowing and adding
	//
	for( const viewLight_t* vLight = viewDef->viewLights; vLight != NULL; vLight = vLight->next )
	{
		// do fogging later
		if( vLight->lightShader->IsFogLight() )
		{
			continue;
		}
		if( vLight->lightShader->IsBlendLight() )
		{
			continue;
		}
		
		if( vLight->localInteractions == NULL && vLight->globalInteractions == NULL && vLight->translucentInteractions == NULL )
		{
			continue;
		}
		
		const idMaterial* lightShader = vLight->lightShader;
		renderLog.OpenBlock( lightShader->GetName() );

#if defined(ANDROID) && !defined(USE_VULKAN)
		// TBDR: the surfaces in the G-buffer get the light from its volume in one draw,
		// the others from their interactions as before
		const bool deferredLight = DeferredLight( vLight );
		if( deferredView )
		{
			if( deferredLight )
			{
				deferredLightsDeferred++;
			}
			else
			{
				deferredLightsForward++;
			}
		}
#endif

		// set the depth bounds for the whole light
		if( useLightDepthBounds )
		{
			GL_DepthBoundsTest( vLight->scissorRect.zmin, vLight->scissorRect.zmax );
		}
		
		// RB: shadow mapping
		if( r_useShadowMapping.GetBool() )
		{
#if !defined(USE_VULKAN)
			// lights in the shadow atlas had their shadow maps rendered before the frame
			const bool shadowMapsInAtlas = ( FindShadowAtlasLight( vLight ) != NULL );
#else
			const bool shadowMapsInAtlas = false;
#endif
			// ambient lights are drawn without shadows, their shadow maps were never read
			if( !shadowMapsInAtlas && vLight->globalShadows != NULL && !lightShader->IsAmbientLight() )
			{
				int	side, sideStop;

				if( vLight->parallel )
				{
					side = 0;
					sideStop = r_shadowMapSplits.GetInteger() + 1;
				}
				else if( vLight->pointLight )
				{
					if( r_shadowMapSingleSide.GetInteger() != -1 )
					{
						side = r_shadowMapSingleSide.GetInteger();
						sideStop = side + 1;
					}
					else
					{
						side = 0;
						sideStop = 6;
					}
				}
				else
				{
					side = -1;
					sideStop = 0;
				}

				for( ; side < sideStop ; side++ )
				{
					// Keep the shadow framebuffer bound while rendering all faces/splits for
					// this light. Switching back to the main target after every face can
					// force tile GPUs to flush and reload the main framebuffer repeatedly.
					ShadowMapPass( vLight->globalShadows, vLight, side, false );
				}

#if !defined(USE_VULKAN)
				// Restore the main render target once, after the complete shadow set.
				if( UseSceneFramebuffer() )
				{
					globalFramebuffers.hdrFBO->Bind();
				}
				else
				{
					Framebuffer::Unbind();
				}
#endif

				// go back from light view to default camera view
				ResetViewportAndScissorToDefaultCamera( _viewDef );
			}

#if defined(ANDROID) && !defined(USE_VULKAN)
			// local and global interactions get the same shadow maps
			const bool skipDeferredSurfaces = deferredLight && DrawDeferredLight( vLight, false );
#else
			const bool skipDeferredSurfaces = false;
#endif

			if( vLight->localInteractions != NULL )
			{
				renderLog.OpenBlock( "Local Light Interactions" );
				RenderInteractions( vLight->localInteractions, vLight, GLS_DEPTHFUNC_EQUAL, false, useLightDepthBounds, skipDeferredSurfaces );
				renderLog.CloseBlock();
			}

			if( vLight->globalInteractions != NULL )
			{
				renderLog.OpenBlock( "Global Light Interactions" );
				RenderInteractions( vLight->globalInteractions, vLight, GLS_DEPTHFUNC_EQUAL, false, useLightDepthBounds, skipDeferredSurfaces );
				renderLog.CloseBlock();
			}
		}
		else
		{
			// only need to clear the stencil buffer and perform stencil testing if there are shadows
			const bool performStencilTest = ( vLight->globalShadows != NULL || vLight->localShadows != NULL ) && !r_useShadowMapping.GetBool();
			
			// mirror flips the sense of the stencil select, and I don't want to risk accidentally breaking it
			// in the normal case, so simply disable the stencil select in the mirror case
			const bool useLightStencilSelect = ( r_useLightStencilSelect.GetBool() && viewDef->isMirror == false );
			
			if( performStencilTest )
			{
				if( useLightStencilSelect )
				{
					// write a stencil mask for the visible light bounds to hi-stencil
					StencilSelectLight( vLight );
				}
				else
				{
					// always clear whole S-Cull tiles
					idScreenRect rect;
					rect.x1 = ( vLight->scissorRect.x1 +  0 ) & ~15;
					rect.y1 = ( vLight->scissorRect.y1 +  0 ) & ~15;
					rect.x2 = ( vLight->scissorRect.x2 + 15 ) & ~15;
					rect.y2 = ( vLight->scissorRect.y2 + 15 ) & ~15;
					
					if( !currentScissor.Equals( rect ) && r_useScissor.GetBool() )
					{
						GL_Scissor( viewDef->viewport.x1 + rect.x1,
									viewDef->viewport.y1 + rect.y1,
									rect.x2 + 1 - rect.x1,
									rect.y2 + 1 - rect.y1 );
									
						currentScissor = rect;
					}
					GL_State( GLS_DEFAULT );	// make sure stencil mask passes for the clear
					GL_Clear( false, false, true, STENCIL_SHADOW_TEST_VALUE, 0.0f, 0.0f, 0.0f, 0.0f, false );
				}
			}
			
			if( vLight->globalShadows != NULL )
			{
				renderLog.OpenBlock( "Global Light Shadows" );
				StencilShadowPass( vLight->globalShadows, vLight );
				renderLog.CloseBlock();
			}

#if defined(ANDROID) && !defined(USE_VULKAN)
			// DeferredLight keeps the lights with local shadows on the interactions:
			// the global shadows are all the stencil has
			const bool skipDeferredSurfaces = deferredLight && DrawDeferredLight( vLight, performStencilTest );
#else
			const bool skipDeferredSurfaces = false;
#endif

			if( vLight->localInteractions != NULL )
			{
				renderLog.OpenBlock( "Local Light Interactions" );
				RenderInteractions( vLight->localInteractions, vLight, GLS_DEPTHFUNC_EQUAL, performStencilTest, useLightDepthBounds, skipDeferredSurfaces );
				renderLog.CloseBlock();
			}

			if( vLight->localShadows != NULL )
			{
				renderLog.OpenBlock( "Local Light Shadows" );
				StencilShadowPass( vLight->localShadows, vLight );
				renderLog.CloseBlock();
			}

			if( vLight->globalInteractions != NULL )
			{
				renderLog.OpenBlock( "Global Light Interactions" );
				RenderInteractions( vLight->globalInteractions, vLight, GLS_DEPTHFUNC_EQUAL, performStencilTest, useLightDepthBounds, skipDeferredSurfaces );
				renderLog.CloseBlock();
			}
		}
		// RB end
		
		if( vLight->translucentInteractions != NULL && !r_skipTranslucent.GetBool() )
		{
			renderLog.OpenBlock( "Translucent Interactions" );
			
			// Disable the depth bounds test because translucent surfaces don't work with
			// the depth bounds tests since they did not write depth during the depth pass.
			if( useLightDepthBounds )
			{
				GL_DepthBoundsTest( 0.0f, 0.0f );
			}
			
			// The depth buffer wasn't filled in for translucent surfaces, so they
			// can never be constrained to perforated surfaces with the depthfunc equal.
			
			// Translucent surfaces do not receive shadows. This is a case where a
			// shadow buffer solution would work but stencil shadows do not because
			// stencil shadows only affect surfaces that contribute to the view depth
			// buffer and translucent surfaces do not contribute to the view depth buffer.
			
			RenderInteractions( vLight->translucentInteractions, vLight, GLS_DEPTHFUNC_LESS, false, false );
			
			renderLog.CloseBlock();
		}
		
		renderLog.CloseBlock();
	}
	
	// disable stencil shadow test
	GL_State( GLS_DEFAULT );
	
	// unbind texture units
	GL_SelectTexture( 0 );
	
	// reset depth bounds
	if( useLightDepthBounds )
	{
		GL_DepthBoundsTest( 0.0f, 0.0f );
	}
	
	renderLog.CloseBlock();
	renderLog.CloseMainBlock();
}

/*
=============================================================================================

NON-INTERACTION SHADER PASSES

=============================================================================================
*/

/*
=====================
idRenderBackend::DrawShaderPasses

Draw non-light dependent passes

If we are rendering Guis, the drawSurf_t::sort value is a depth offset that can
be multiplied by guiEye for polarity and screenSeparation for scale.
=====================
*/
int idRenderBackend::DrawShaderPasses( const drawSurf_t* const* const drawSurfs, const int numDrawSurfs,
									   const float guiStereoScreenOffset, const int stereoEye )
{
	// only obey skipAmbient if we are rendering a view
	if( viewDef->viewEntitys && r_skipAmbient.GetBool() )
	{
		return numDrawSurfs;
	}
	
	renderLog.OpenBlock( "RB_DrawShaderPasses" );
	
	GL_SelectTexture( 0 );
	
	currentSpace = ( const viewEntity_t* )1;	// using NULL makes /analyze think surf->space needs to be checked...
	float currentGuiStereoOffset = 0.0f;

#if defined(ANDROID) && !defined(USE_VULKAN)
	bool* gpuShaderPassConsumed = (bool*)_alloca( numDrawSurfs * sizeof( bool ) );
	memset( gpuShaderPassConsumed, 0, numDrawSurfs * sizeof( bool ) );
	int gpuShaderPassBatchCount = 0;
	int gpuShaderPassDrawCallsSaved = 0;
#endif
	
	int i = 0;
	for( ; i < numDrawSurfs; i++ )
	{
#if defined(ANDROID) && !defined(USE_VULKAN)
		if( gpuShaderPassConsumed[i] )
		{
			continue;
		}
		gpuInstanceData_t gpuShaderPassInstances[MAX_GPU_INSTANCES_PER_DRAW];
		const gpuInstanceData_t* gpuShaderPassInstanceData = NULL;
		int gpuShaderPassInstanceCount = 1;
#endif
		const drawSurf_t* surf = drawSurfs[i];
		const idMaterial* shader = surf->material;
		
		if( !shader->HasAmbient() )
		{
			continue;
		}
		
		if( shader->IsPortalSky() )
		{
			continue;
		}
		
		// some deforms may disable themselves by setting numIndexes = 0
		if( surf->numIndexes == 0 )
		{
			continue;
		}
		
		if( shader->SuppressInSubview() )
		{
			continue;
		}
		
		if( viewDef->isXraySubview && surf->space->entityDef )
		{
			if( surf->space->entityDef->parms.xrayIndex != 2 )
			{
				continue;
			}
		}
		
		// we need to draw the post process shaders after we have drawn the fog lights
		if( shader->GetSort() >= SS_POST_PROCESS && !currentRenderCopied )
		{
			break;
		}
		
		// if we are rendering a 3D view and the surface's eye index doesn't match
		// the current view's eye index then we skip the surface
		// if the stereoEye value of a surface is 0 then we need to draw it for both eyes.
		const int shaderStereoEye = shader->GetStereoEye();
		const bool isEyeValid = game->GetCVarBool("stereoRender_swapEyes") ? ( shaderStereoEye == stereoEye ) : ( shaderStereoEye != stereoEye );
		if( ( stereoEye != 0 ) && ( shaderStereoEye != 0 ) && ( isEyeValid ) )
		{
			continue;
		}

#if defined(ANDROID) && !defined(USE_VULKAN)
		if( glConfig.glVersion >= 3.2f && !viewDef->isXraySubview
			&& ( surf->extraGLState & ( GLS_SRCBLEND_BITS | GLS_DSTBLEND_BITS ) ) == 0
			&& R_GPUShaderPassMaterialInstanceable( shader ) )
		{
			gpuShaderPassInstanceCount = R_BuildGPUInstanceBatch( drawSurfs, numDrawSurfs, i,
					viewDef->renderView.vieworg, gpuShaderPassConsumed, gpuShaderPassInstances,
					shader->Coverage() == MC_PERFORATED );
			if( gpuShaderPassInstanceCount > 1 )
			{
				gpuShaderPassInstanceData = gpuShaderPassInstances;
				++gpuShaderPassBatchCount;
				gpuShaderPassDrawCallsSaved += gpuShaderPassInstanceCount - 1;
			}
		}
#endif
		
		renderLog.OpenBlock( shader->GetName() );
		
		// determine the stereoDepth offset
		// guiStereoScreenOffset will always be zero for 3D views, so the !=
		// check will never force an update due to the current sort value.
		const float thisGuiStereoOffset = guiStereoScreenOffset * surf->sort;
		
		// change the matrix and other space related vars if needed
		if( surf->space != currentSpace || thisGuiStereoOffset != currentGuiStereoOffset )
		{
			currentSpace = surf->space;
			currentGuiStereoOffset = thisGuiStereoOffset;
			
			const viewEntity_t* space = currentSpace;
			
			if( guiStereoScreenOffset != 0.0f )
			{
				RB_SetMVPWithStereoOffset( space->mvp, currentGuiStereoOffset );
			}
			else
			{
				RB_SetMVP( space->mvp );
			}
			
			// set eye position in local space
			idVec4 localViewOrigin( 1.0f );
			R_GlobalPointToLocal( space->modelMatrix, viewDef->renderView.vieworg, localViewOrigin.ToVec3() );
			SetVertexParm( RENDERPARM_LOCALVIEWORIGIN, localViewOrigin.ToFloatPtr() );
			
			// set model Matrix
			float modelMatrixTranspose[16];
			R_MatrixTranspose( space->modelMatrix, modelMatrixTranspose );
			SetVertexParms( RENDERPARM_MODELMATRIX_X, modelMatrixTranspose, 4 );
			
			// Set ModelView Matrix
			float modelViewMatrixTranspose[16];
			R_MatrixTranspose( space->modelViewMatrix, modelViewMatrixTranspose );
			SetVertexParms( RENDERPARM_MODELVIEWMATRIX_X, modelViewMatrixTranspose, 4 );
		}
		
		// change the scissor if needed
		if( !currentScissor.Equals( surf->scissorRect ) && r_useScissor.GetBool() )
		{
			GL_Scissor( viewDef->viewport.x1 + surf->scissorRect.x1,
						viewDef->viewport.y1 + surf->scissorRect.y1,
						surf->scissorRect.x2 + 1 - surf->scissorRect.x1,
						surf->scissorRect.y2 + 1 - surf->scissorRect.y1 );
						
			currentScissor = surf->scissorRect;
		}
		
		// get the expressions for conditionals / color / texcoords
		const float*	regs = surf->shaderRegisters;
		
		// set face culling appropriately
		uint64 cullMode;
		if( surf->space->isGuiSurface )
		{
			cullMode = GLS_CULL_TWOSIDED;
		}
		else
		{
			switch( shader->GetCullType() )
			{
				case CT_TWO_SIDED:
					cullMode = GLS_CULL_TWOSIDED;
					break;
					
				case CT_BACK_SIDED:
					cullMode = GLS_CULL_BACKSIDED;
					break;
					
				case CT_FRONT_SIDED:
				default:
					cullMode = GLS_CULL_FRONTSIDED;
					break;
			}
		}
		
		uint64 surfGLState = surf->extraGLState | cullMode;
		
		// set polygon offset if necessary
		
		if( shader->TestMaterialFlag( MF_POLYGONOFFSET ) )
		{
			surfGLState = GLS_POLYGON_OFFSET;
			
			// RB: make sure the pipeline of the current shader has dynamic polygon offset enabled
			//renderProgManager.CommitUniforms( surfGLState );
			
#if !defined( USE_VULKAN )
			GL_PolygonOffset( r_offsetFactor.GetFloat(), r_offsetUnits.GetFloat() * shader->GetPolygonOffset() );
#endif
		}
		
		for( int stage = 0; stage < shader->GetNumStages(); stage++ )
		{
			const shaderStage_t* pStage = shader->GetStage( stage );
			
			// check the enable condition
			if( regs[ pStage->conditionRegister ] == 0 )
			{
				continue;
			}
			
			// skip the stages involved in lighting
			if( pStage->lighting != SL_AMBIENT )
			{
				continue;
			}
			
			uint64 stageGLState = surfGLState;
			if( ( surfGLState & GLS_OVERRIDE ) == 0 )
			{
				stageGLState |= pStage->drawStateBits;
			}
			
			// skip if the stage is ( GL_ZERO, GL_ONE ), which is used for some alpha masks
			if( ( stageGLState & ( GLS_SRCBLEND_BITS | GLS_DSTBLEND_BITS ) ) == ( GLS_SRCBLEND_ZERO | GLS_DSTBLEND_ONE ) )
			{
				continue;
			}
			
			
			// see if we are a new-style stage
			newShaderStage_t* newStage = pStage->newStage;
			if( newStage != NULL )
			{
				//--------------------------
				//
				// new style stages
				//
				//--------------------------
				if( r_skipNewAmbient.GetBool() )
				{
					continue;
				}
				renderLog.OpenBlock( "New Shader Stage" );
				
				GL_State( stageGLState );
				
				renderProgManager.BindProgram( newStage->glslProgram );
				
				for( int j = 0; j < newStage->numVertexParms; j++ )
				{
					float parm[4];
					parm[0] = regs[ newStage->vertexParms[j][0] ];
					parm[1] = regs[ newStage->vertexParms[j][1] ];
					parm[2] = regs[ newStage->vertexParms[j][2] ];
					parm[3] = regs[ newStage->vertexParms[j][3] ];
					SetVertexParm( ( renderParm_t )( RENDERPARM_USER0 + j ), parm );
				}
				
				// set rpEnableSkinning if the shader has optional support for skinning
				if( surf->jointCache && renderProgManager.ShaderHasOptionalSkinning() )
				{
					const idVec4 skinningParm( 1.0f );
					SetVertexParm( RENDERPARM_ENABLE_SKINNING, skinningParm.ToFloatPtr() );
				}
				
				// bind texture units
				for( int j = 0; j < newStage->numFragmentProgramImages; j++ )
				{
					idImage* image = newStage->fragmentProgramImages[j];
					if( image != NULL )
					{
						GL_SelectTexture( j );
						image->Bind();
					}
				}
				
				// draw it
				DrawElementsWithCounters( surf );
				
				// clear rpEnableSkinning if it was set
				if( surf->jointCache && renderProgManager.ShaderHasOptionalSkinning() )
				{
					const idVec4 skinningParm( 0.0f );
					SetVertexParm( RENDERPARM_ENABLE_SKINNING, skinningParm.ToFloatPtr() );
				}
				
				GL_SelectTexture( 0 );
				renderProgManager.Unbind();
				
				renderLog.CloseBlock();
				continue;
			}
			
			//--------------------------
			//
			// old style stages
			//
			//--------------------------
			
			// set the color
			idVec4 color;
			color[0] = regs[ pStage->color.registers[0] ];
			color[1] = regs[ pStage->color.registers[1] ];
			color[2] = regs[ pStage->color.registers[2] ];
			color[3] = regs[ pStage->color.registers[3] ];
			
			// skip the entire stage if an add would be black
			if( ( stageGLState & ( GLS_SRCBLEND_BITS | GLS_DSTBLEND_BITS ) ) == ( GLS_SRCBLEND_ONE | GLS_DSTBLEND_ONE )
					&& color[0] <= 0 && color[1] <= 0 && color[2] <= 0 )
			{
				continue;
			}
			
			// skip the entire stage if a blend would be completely transparent
			if( ( stageGLState & ( GLS_SRCBLEND_BITS | GLS_DSTBLEND_BITS ) ) == ( GLS_SRCBLEND_SRC_ALPHA | GLS_DSTBLEND_ONE_MINUS_SRC_ALPHA )
					&& color[3] <= 0 )
			{
				continue;
			}
			
			stageVertexColor_t svc = pStage->vertexColor;
			
			renderLog.OpenBlock( "Old Shader Stage" );
			GL_Color( color );
			//GK: On Classic DOOM apply it's own builtin shader if the user want to, otherwise apply the shader that was applied in the first place (vertex_color)
			if (pStage->texture.image && r_useClShader.GetBool() && !idStr::Cmp(pStage->texture.image->GetName(), "_doomClassic")) {
				renderProgManager.BindShader_Classic();
			} else
			if( surf->space->isGuiSurface )
			{
				// Force gui surfaces to always be SVC_MODULATE
				svc = SVC_MODULATE;
				
				// use special shaders for bink cinematics
				if( pStage->texture.cinematic )
				{
					if( ( stageGLState & GLS_OVERRIDE ) != 0 )
					{
						// This is a hack... Only SWF Guis set GLS_OVERRIDE
						// Old style guis do not, and we don't want them to use the new GUI renederProg
						renderProgManager.BindShader_TextureVertexColor_sRGB();
					}
					else
					{
						renderProgManager.BindShader_TextureVertexColor();
					}
				}
				else
				{
					if( ( stageGLState & GLS_OVERRIDE ) != 0 )
					{
						// This is a hack... Only SWF Guis set GLS_OVERRIDE
						// Old style guis do not, and we don't want them to use the new GUI renderProg
						renderProgManager.BindShader_GUI();
					}
					else
					{
						if( surf->jointCache )
						{
							renderProgManager.BindShader_TextureVertexColorSkinned();
						}
						else
						{
							if( viewDef->is2Dgui && r_useHDR.GetBool())
							{
								// RB: 2D fullscreen drawing like warp or damage blend effects
								renderProgManager.BindShader_TextureVertexColor_sRGB();
							}
							else
							{
								renderProgManager.BindShader_TextureVertexColor();
							}
						}
					}
				}
			}
			else if( ( pStage->texture.texgen == TG_SCREEN ) || ( pStage->texture.texgen == TG_SCREEN2 ) )
			{
				renderProgManager.BindShader_TextureTexGenVertexColor();
			}
			else if( pStage->texture.cinematic )
			{
				renderProgManager.BindShader_Bink();
			}
			else
			{
				if( surf->jointCache )
				{
					renderProgManager.BindShader_TextureVertexColorSkinned();
				}
				else
				{
					renderProgManager.BindShader_TextureVertexColor();
				}
			}
			
			RB_SetVertexColorParms( svc );
			
			// bind the texture
			BindVariableStageImage( &pStage->texture, regs );
			
			if( pStage->privatePolygonOffset )
			{
				stageGLState |= GLS_POLYGON_OFFSET;
			}
			
			// set the state
			GL_State( stageGLState );
			
			// set privatePolygonOffset if necessary
#if defined( USE_VULKAN )
			if( shader->TestMaterialFlag( MF_POLYGONOFFSET ) || pStage->privatePolygonOffset )
			{
				// RB: make sure the pipeline of the current shader has dynamic polygon offset enabled
				renderProgManager.CommitUniforms( stageGLState );
				
				if( shader->TestMaterialFlag( MF_POLYGONOFFSET ) )
				{
					GL_PolygonOffset( r_offsetFactor.GetFloat(), r_offsetUnits.GetFloat() * shader->GetPolygonOffset() );
				}
				else
				{
					GL_PolygonOffset( r_offsetFactor.GetFloat(), r_offsetUnits.GetFloat() * pStage->privatePolygonOffset );
				}
			}
#else
			if( pStage->privatePolygonOffset )
			{
				GL_PolygonOffset( r_offsetFactor.GetFloat(), r_offsetUnits.GetFloat() * pStage->privatePolygonOffset );
			}
#endif
			
			PrepareStageTexturing( pStage, surf );
			
			// Draw standard UV stages with the per-instance MVP when possible.
#if defined(ANDROID) && !defined(USE_VULKAN)
			if( gpuShaderPassInstanceCount > 1 )
			{
				renderProgManager.BindShader_TextureVertexColorInstanced();
				DrawElementsWithCounters( surf, gpuShaderPassInstanceData, gpuShaderPassInstanceCount );
			}
			else
#endif
			{
				DrawElementsWithCounters( surf );
			}
			
			FinishStageTexturing( pStage, surf );
			
			// unset privatePolygonOffset if necessary
			if( pStage->privatePolygonOffset )
			{
				GL_PolygonOffset( r_offsetFactor.GetFloat(), r_offsetUnits.GetFloat() * shader->GetPolygonOffset() );
			}
			
			renderLog.CloseBlock();
		}
		
		renderLog.CloseBlock();
	}
	
	GL_Color( 1.0f, 1.0f, 1.0f );
	
	// disable stencil shadow test
	GL_State( GLS_DEFAULT );
	
	GL_SelectTexture( 0 );

#if defined(ANDROID) && !defined(USE_VULKAN)
	if( r_gpuInstancingDebug.GetBool() && gpuShaderPassBatchCount > 0 )
	{
		idLib::Printf( "GPU instancing shader pass: %d batches, %d draw calls saved\n",
				gpuShaderPassBatchCount, gpuShaderPassDrawCallsSaved );
	}
#endif
	
	renderLog.CloseBlock();
	return i;
}

/*
=============================================================================================

BLEND LIGHT PROJECTION

=============================================================================================
*/

/*
=====================
idRenderBackend::T_BlendLight
=====================
*/
void idRenderBackend::T_BlendLight( const drawSurf_t* drawSurfs, const viewLight_t* vLight )
{
	currentSpace = NULL;

#if defined(ANDROID) && !defined(USE_VULKAN)
	int numBlendSurfs = 0;
	for( const drawSurf_t* scan = drawSurfs; scan != NULL; scan = scan->nextOnLight )
	{
		++numBlendSurfs;
	}
	const int blendAllocCount = numBlendSurfs > 0 ? numBlendSurfs : 1;
	const drawSurf_t** blendBatchSurfs = (const drawSurf_t**)_alloca( blendAllocCount * sizeof( const drawSurf_t* ) );
	bool* blendConsumed = (bool*)_alloca( blendAllocCount * sizeof( bool ) );
	int blendArrayIndex = 0;
	for( const drawSurf_t* scan = drawSurfs; scan != NULL; scan = scan->nextOnLight )
	{
		blendBatchSurfs[blendArrayIndex++] = scan;
	}
	memset( blendConsumed, 0, blendAllocCount * sizeof( bool ) );
	int blendBatchCount = 0;
	int blendDrawCallsSaved = 0;
	for( int blendIndex = 0; blendIndex < numBlendSurfs; ++blendIndex )
	{
		if( blendConsumed[blendIndex] )
		{
			continue;
		}
		const drawSurf_t* drawSurf = blendBatchSurfs[blendIndex];
		gpuInstanceData_t blendInstances[MAX_GPU_INSTANCES_PER_DRAW];
		const drawSurf_t* blendInstanceSurfs[MAX_GPU_INSTANCES_PER_DRAW];
		const gpuInstanceData_t* blendInstanceData = NULL;
		int blendInstanceCount = 1;
#else
	for( const drawSurf_t* drawSurf = drawSurfs; drawSurf != NULL; drawSurf = drawSurf->nextOnLight )
	{
#endif
		if( drawSurf->scissorRect.IsEmpty() )
		{
			continue;	// !@# FIXME: find out why this is sometimes being hit!
			// temporarily jump over the scissor and draw so the gl error callback doesn't get hit
		}
		
#if defined(ANDROID) && !defined(USE_VULKAN)
		// a merged world surface draws all world surfaces of the light, whatever their material
		worldMergeSurf = NULL;
		if( R_WorldMergeSkip( drawSurf ) )
		{
			continue;
		}
		if( R_IsWorldMergeGroupMember( drawSurf ) )
		{
			BeginWorldMergeGroup( drawSurf );
		}
#endif
		
#if defined(ANDROID) && !defined(USE_VULKAN)
		if( glConfig.glVersion >= 3.2f && !drawSurf->jointCache )
		{
			blendInstanceCount = R_BuildGPUInstanceBatch( blendBatchSurfs, numBlendSurfs, blendIndex,
					viewDef->renderView.vieworg, blendConsumed, blendInstances, true, blendInstanceSurfs );
			if( blendInstanceCount > 1 )
			{
				for( int instanceIndex = 0; instanceIndex < blendInstanceCount; ++instanceIndex )
				{
					R_FillTexGenGPUInstanceData( blendInstances[instanceIndex], blendInstanceSurfs[instanceIndex],
							viewDef->renderView.vieworg, vLight->lightProject );
				}
				blendInstanceData = blendInstances;
				++blendBatchCount;
				blendDrawCallsSaved += blendInstanceCount - 1;
			}
		}
#endif
		if( !currentScissor.Equals( drawSurf->scissorRect ) && r_useScissor.GetBool() )
		{
			// change the scissor
			GL_Scissor( viewDef->viewport.x1 + drawSurf->scissorRect.x1,
						viewDef->viewport.y1 + drawSurf->scissorRect.y1,
						drawSurf->scissorRect.x2 + 1 - drawSurf->scissorRect.x1,
						drawSurf->scissorRect.y2 + 1 - drawSurf->scissorRect.y1 );
						
			currentScissor = drawSurf->scissorRect;
		}
		
		if( drawSurf->space != currentSpace )
		{
			// change the matrix
			RB_SetMVP( drawSurf->space->mvp );
			
			// change the light projection matrix
			idPlane	lightProjectInCurrentSpace[4];
			for( int i = 0; i < 4; i++ )
			{
				R_GlobalPlaneToLocal( drawSurf->space->modelMatrix, vLight->lightProject[i], lightProjectInCurrentSpace[i] );
			}
			
			SetVertexParm( RENDERPARM_TEXGEN_0_S, lightProjectInCurrentSpace[0].ToFloatPtr() );
			SetVertexParm( RENDERPARM_TEXGEN_0_T, lightProjectInCurrentSpace[1].ToFloatPtr() );
			SetVertexParm( RENDERPARM_TEXGEN_0_Q, lightProjectInCurrentSpace[2].ToFloatPtr() );
			SetVertexParm( RENDERPARM_TEXGEN_1_S, lightProjectInCurrentSpace[3].ToFloatPtr() );	// falloff
			
			currentSpace = drawSurf->space;
		}
		
#if defined(ANDROID) && !defined(USE_VULKAN)
		if( blendInstanceCount > 1 )
		{
			renderProgManager.BindShader_BlendLightInstanced();
			DrawElementsWithCounters( drawSurf, blendInstanceData, blendInstanceCount );
		}
		else
		{
			renderProgManager.BindShader_BlendLight();
			DrawElementsWithCounters( drawSurf );
		}
#else
		DrawElementsWithCounters( drawSurf );
#endif
	}

#if defined(ANDROID) && !defined(USE_VULKAN)
	if( r_gpuInstancingDebug.GetBool() && blendBatchCount > 0 )
	{
		idLib::Printf( "GPU instancing blend-light pass: %d batches, %d draw calls saved\n",
				blendBatchCount, blendDrawCallsSaved );
	}
#endif
}

/*
=====================
idRenderBackend::BlendLight

Dual texture together the falloff and projection texture with a blend
mode to the framebuffer, instead of interacting with the surface texture
=====================
*/
void idRenderBackend::BlendLight( const drawSurf_t* drawSurfs, const drawSurf_t* drawSurfs2, const viewLight_t* vLight )
{
	if( drawSurfs == NULL )
	{
		return;
	}
	if( r_skipBlendLights.GetBool() )
	{
		return;
	}
	renderLog.OpenBlock( vLight->lightShader->GetName() );
	
	const idMaterial* lightShader = vLight->lightShader;
	const float*	 regs = vLight->shaderRegisters;
	
	// texture 1 will get the falloff texture
	GL_SelectTexture( 1 );
	vLight->falloffImage->Bind();
	
	// texture 0 will get the projected texture
	GL_SelectTexture( 0 );
	
	renderProgManager.BindShader_BlendLight();
	
	for( int i = 0; i < lightShader->GetNumStages(); i++ )
	{
		const shaderStage_t*	stage = lightShader->GetStage( i );
		
		if( !regs[ stage->conditionRegister ] )
		{
			continue;
		}
		
		GL_State( GLS_DEPTHMASK | stage->drawStateBits | GLS_DEPTHFUNC_EQUAL );
		
		GL_SelectTexture( 0 );
		stage->texture.image->Bind();
		
		if( stage->texture.hasMatrix )
		{
			RB_LoadShaderTextureMatrix( regs, &stage->texture );
		}
		
		// get the modulate values from the light, including alpha, unlike normal lights
		idVec4 lightColor;
		lightColor[0] = regs[ stage->color.registers[0] ];
		lightColor[1] = regs[ stage->color.registers[1] ];
		lightColor[2] = regs[ stage->color.registers[2] ];
		lightColor[3] = regs[ stage->color.registers[3] ];
		GL_Color( lightColor );
		
#if defined(ANDROID) && !defined(USE_VULKAN)
		// a merged group spans both chains, it is drawn once per stage
		++s_worldMergeStamp;
#endif
		T_BlendLight( drawSurfs, vLight );
		T_BlendLight( drawSurfs2, vLight );
	}
	
#if defined(ANDROID) && !defined(USE_VULKAN)
	worldMergeSurf = NULL;
#endif
	GL_SelectTexture( 0 );
	
	renderProgManager.Unbind();
	renderLog.CloseBlock();
}

/*
=========================================================================================================

FOG LIGHTS

=========================================================================================================
*/

/*
=====================
idRenderBackend::T_BasicFog
=====================
*/
void idRenderBackend::T_BasicFog( const drawSurf_t* drawSurfs, const idPlane fogPlanes[4], const idRenderMatrix* inverseBaseLightProject )
{
	currentSpace = NULL;

#if defined(ANDROID) && !defined(USE_VULKAN)
	int numFogSurfs = 0;
	for( const drawSurf_t* scan = drawSurfs; scan != NULL; scan = scan->nextOnLight )
	{
		++numFogSurfs;
	}
	const int fogAllocCount = numFogSurfs > 0 ? numFogSurfs : 1;
	const drawSurf_t** fogBatchSurfs = (const drawSurf_t**)_alloca( fogAllocCount * sizeof( const drawSurf_t* ) );
	bool* fogConsumed = (bool*)_alloca( fogAllocCount * sizeof( bool ) );
	int fogArrayIndex = 0;
	for( const drawSurf_t* scan = drawSurfs; scan != NULL; scan = scan->nextOnLight )
	{
		fogBatchSurfs[fogArrayIndex++] = scan;
	}
	memset( fogConsumed, 0, fogAllocCount * sizeof( bool ) );
	int fogBatchCount = 0;
	int fogDrawCallsSaved = 0;
	for( int fogIndex = 0; fogIndex < numFogSurfs; ++fogIndex )
	{
		if( fogConsumed[fogIndex] )
		{
			continue;
		}
		const drawSurf_t* drawSurf = fogBatchSurfs[fogIndex];
		gpuInstanceData_t fogInstances[MAX_GPU_INSTANCES_PER_DRAW];
		const drawSurf_t* fogInstanceSurfs[MAX_GPU_INSTANCES_PER_DRAW];
		const gpuInstanceData_t* fogInstanceData = NULL;
		int fogInstanceCount = 1;
#else
	for( const drawSurf_t* drawSurf = drawSurfs; drawSurf != NULL; drawSurf = drawSurf->nextOnLight )
	{
#endif
		if( drawSurf->scissorRect.IsEmpty() )
		{
			continue;	// !@# FIXME: find out why this is sometimes being hit!
			// temporarily jump over the scissor and draw so the gl error callback doesn't get hit
		}
		
#if defined(ANDROID) && !defined(USE_VULKAN)
		// a merged world surface draws all world surfaces of the light, whatever their material
		worldMergeSurf = NULL;
		if( R_WorldMergeSkip( drawSurf ) )
		{
			continue;
		}
		if( R_IsWorldMergeGroupMember( drawSurf ) )
		{
			BeginWorldMergeGroup( drawSurf );
		}
#endif
		
#if defined(ANDROID) && !defined(USE_VULKAN)
		if( inverseBaseLightProject == NULL && glConfig.glVersion >= 3.2f && !drawSurf->jointCache )
		{
			fogInstanceCount = R_BuildGPUInstanceBatch( fogBatchSurfs, numFogSurfs, fogIndex,
					viewDef->renderView.vieworg, fogConsumed, fogInstances, true, fogInstanceSurfs );
			if( fogInstanceCount > 1 )
			{
				for( int instanceIndex = 0; instanceIndex < fogInstanceCount; ++instanceIndex )
				{
					R_FillTexGenGPUInstanceData( fogInstances[instanceIndex], fogInstanceSurfs[instanceIndex],
							viewDef->renderView.vieworg, fogPlanes );
				}
				fogInstanceData = fogInstances;
				++fogBatchCount;
				fogDrawCallsSaved += fogInstanceCount - 1;
			}
		}
#endif
		if( !currentScissor.Equals( drawSurf->scissorRect ) && r_useScissor.GetBool() )
		{
			// change the scissor
			GL_Scissor( viewDef->viewport.x1 + drawSurf->scissorRect.x1,
						viewDef->viewport.y1 + drawSurf->scissorRect.y1,
						drawSurf->scissorRect.x2 + 1 - drawSurf->scissorRect.x1,
						drawSurf->scissorRect.y2 + 1 - drawSurf->scissorRect.y1 );
						
			currentScissor = drawSurf->scissorRect;
		}
		
		if( drawSurf->space != currentSpace )
		{
			idPlane localFogPlanes[4];
			if( inverseBaseLightProject == NULL )
			{
				RB_SetMVP( drawSurf->space->mvp );
				for( int i = 0; i < 4; i++ )
				{
					R_GlobalPlaneToLocal( drawSurf->space->modelMatrix, fogPlanes[i], localFogPlanes[i] );
				}
			}
			else
			{
				idRenderMatrix invProjectMVPMatrix;
				idRenderMatrix::Multiply( viewDef->worldSpace.mvp, *inverseBaseLightProject, invProjectMVPMatrix );
				RB_SetMVP( invProjectMVPMatrix );
				for( int i = 0; i < 4; i++ )
				{
					inverseBaseLightProject->InverseTransformPlane( fogPlanes[i], localFogPlanes[i], false );
				}
			}
			
			SetVertexParm( RENDERPARM_TEXGEN_0_S, localFogPlanes[0].ToFloatPtr() );
			SetVertexParm( RENDERPARM_TEXGEN_0_T, localFogPlanes[1].ToFloatPtr() );
			SetVertexParm( RENDERPARM_TEXGEN_1_T, localFogPlanes[2].ToFloatPtr() );
			SetVertexParm( RENDERPARM_TEXGEN_1_S, localFogPlanes[3].ToFloatPtr() );
			
			currentSpace = ( inverseBaseLightProject == NULL ) ? drawSurf->space : NULL;
		}
		
		if( drawSurf->jointCache )
		{
			renderProgManager.BindShader_FogSkinned();
		}
		else
		{
#if defined(ANDROID) && !defined(USE_VULKAN)
			if( fogInstanceCount > 1 )
			{
				renderProgManager.BindShader_FogInstanced();
			}
			else
#endif
			{
				renderProgManager.BindShader_Fog();
			}
		}
		
#if defined(ANDROID) && !defined(USE_VULKAN)
		DrawElementsWithCounters( drawSurf, fogInstanceData, fogInstanceCount );
#else
		DrawElementsWithCounters( drawSurf );
#endif
	}

#if defined(ANDROID) && !defined(USE_VULKAN)
	if( r_gpuInstancingDebug.GetBool() && fogBatchCount > 0 )
	{
		idLib::Printf( "GPU instancing fog pass: %d batches, %d draw calls saved\n",
				fogBatchCount, fogDrawCallsSaved );
	}
#endif
}

/*
==================
idRenderBackend::FogPass
==================
*/
void idRenderBackend::FogPass( const drawSurf_t* drawSurfs,  const drawSurf_t* drawSurfs2, const viewLight_t* vLight )
{
	renderLog.OpenBlock( vLight->lightShader->GetName() );
	
	// find the current color and density of the fog
	const idMaterial* lightShader = vLight->lightShader;
	const float* regs = vLight->shaderRegisters;
	// assume fog shaders have only a single stage
	const shaderStage_t* stage = lightShader->GetStage( 0 );
	
	idVec4 lightColor;
	lightColor[0] = regs[ stage->color.registers[0] ];
	lightColor[1] = regs[ stage->color.registers[1] ];
	lightColor[2] = regs[ stage->color.registers[2] ];
	lightColor[3] = regs[ stage->color.registers[3] ];
	
	GL_Color( lightColor );
	
	// calculate the falloff planes
	float a;
	
	// if they left the default value on, set a fog distance of 500
	if( lightColor[3] <= 1.0f )
	{
		a = -0.5f / DEFAULT_FOG_DISTANCE;
	}
	else
	{
		// otherwise, distance = alpha color
		a = -0.5f / lightColor[3];
	}
	
	// texture 0 is the falloff image
	GL_SelectTexture( 0 );
	globalImages->fogImage->Bind();
	
	// texture 1 is the entering plane fade correction
	GL_SelectTexture( 1 );
	globalImages->fogEnterImage->Bind();
	
	// S is based on the view origin
	const float s = vLight->fogPlane.Distance( viewDef->renderView.vieworg );
	
	const float FOG_SCALE = 0.001f;
	
	idPlane fogPlanes[4];
	
	// S-0
	fogPlanes[0][0] = a * viewDef->worldSpace.modelViewMatrix[0 * 4 + 2];
	fogPlanes[0][1] = a * viewDef->worldSpace.modelViewMatrix[1 * 4 + 2];
	fogPlanes[0][2] = a * viewDef->worldSpace.modelViewMatrix[2 * 4 + 2];
	fogPlanes[0][3] = a * viewDef->worldSpace.modelViewMatrix[3 * 4 + 2] + 0.5f;
	
	// T-0
	fogPlanes[1][0] = 0.0f;//a * backEnd.viewDef->worldSpace.modelViewMatrix[0*4+0];
	fogPlanes[1][1] = 0.0f;//a * backEnd.viewDef->worldSpace.modelViewMatrix[1*4+0];
	fogPlanes[1][2] = 0.0f;//a * backEnd.viewDef->worldSpace.modelViewMatrix[2*4+0];
	fogPlanes[1][3] = 0.5f;//a * backEnd.viewDef->worldSpace.modelViewMatrix[3*4+0] + 0.5f;
	
	// T-1 will get a texgen for the fade plane, which is always the "top" plane on unrotated lights
	fogPlanes[2][0] = FOG_SCALE * vLight->fogPlane[0];
	fogPlanes[2][1] = FOG_SCALE * vLight->fogPlane[1];
	fogPlanes[2][2] = FOG_SCALE * vLight->fogPlane[2];
	fogPlanes[2][3] = FOG_SCALE * vLight->fogPlane[3] + FOG_ENTER;
	
	// S-1
	fogPlanes[3][0] = 0.0f;
	fogPlanes[3][1] = 0.0f;
	fogPlanes[3][2] = 0.0f;
	fogPlanes[3][3] = FOG_SCALE * s + FOG_ENTER;
	
	// draw it
	GL_State( GLS_DEPTHMASK | GLS_SRCBLEND_SRC_ALPHA | GLS_DSTBLEND_ONE_MINUS_SRC_ALPHA | GLS_DEPTHFUNC_EQUAL );
#if defined(ANDROID) && !defined(USE_VULKAN)
	// a merged group spans both chains, it is drawn once
	++s_worldMergeStamp;
#endif
	T_BasicFog( drawSurfs, fogPlanes, NULL );
	T_BasicFog( drawSurfs2, fogPlanes, NULL );
#if defined(ANDROID) && !defined(USE_VULKAN)
	worldMergeSurf = NULL;
#endif
	
	// the light frustum bounding planes aren't in the depth buffer, so use depthfunc_less instead
	// of depthfunc_equal
	GL_State( GLS_DEPTHMASK | GLS_SRCBLEND_SRC_ALPHA | GLS_DSTBLEND_ONE_MINUS_SRC_ALPHA | GLS_DEPTHFUNC_LESS | GLS_CULL_BACKSIDED );
	
	zeroOneCubeSurface.space = &viewDef->worldSpace;
	zeroOneCubeSurface.scissorRect = viewDef->scissor;
	T_BasicFog( &zeroOneCubeSurface, fogPlanes, &vLight->inverseBaseLightProject );
	
	GL_State( glStateBits & ~( GLS_CULL_MASK ) | GLS_CULL_FRONTSIDED );
	
	GL_SelectTexture( 0 );
	
	renderProgManager.Unbind();
	
	renderLog.CloseBlock();
}

/*
==================
idRenderBackend::FogAllLights
==================
*/
void idRenderBackend::FogAllLights()
{
	if( r_skipFogLights.GetBool() || r_showOverDraw.GetInteger() != 0
			|| viewDef->isXraySubview /* don't fog in xray mode*/ )
	{
		return;
	}
	renderLog.OpenMainBlock( MRB_FOG_ALL_LIGHTS );
	renderLog.OpenBlock( "RB_FogAllLights" );
	
	// force fog plane to recalculate
	currentSpace = NULL;
	
	for( viewLight_t* vLight = viewDef->viewLights; vLight != NULL; vLight = vLight->next )
	{
		if( vLight->lightShader->IsFogLight() )
		{
			FogPass( vLight->globalInteractions, vLight->localInteractions, vLight );
		}
		else if( vLight->lightShader->IsBlendLight() )
		{
			BlendLight( vLight->globalInteractions, vLight->localInteractions, vLight );
		}
	}
	
	renderLog.CloseBlock();
	renderLog.CloseMainBlock();
}

// RB begin
void idRenderBackend::CalculateAutomaticExposure()
{
	int				i;
	static float	image[64 * 64 * 4];
	float           curTime;
	float			deltaTime;
	float           luminance;
	float			avgLuminance;
	float			maxLuminance;
	double			sum;
	const idVec3    LUMINANCE_SRGB( 0.2125f, 0.7154f, 0.0721f ); // be careful wether this should be linear RGB or sRGB
	idVec4			color;
	float			newAdaptation;
	float			newMaximum;
	
	if( !r_hdrAutoExposure.GetBool() )
	{
		// no dynamic exposure
		
		hdrKey = r_hdrKey.GetFloat();
		hdrAverageLuminance = r_hdrMinLuminance.GetFloat();
		hdrMaxLuminance = 1;
	}
	else
	{
		curTime = Sys_Milliseconds() / 1000.0f;
		
		// calculate the average scene luminance
		globalFramebuffers.hdr64FBO->Bind();
		
		bool hasLuminanceSample = true;
#if !defined(USE_VULKAN)
		// Android reads into a three-buffer PBO ring and only maps a buffer after
		// its fence signals. Desktop retains the original direct readback path.
#if defined(ANDROID)
		hasLuminanceSample = RB_ReadExposurePixelsAsync( image );
#else
		glReadPixels( 0, 0, 64, 64, GL_RGBA, GL_FLOAT, image );
#endif
		// Do not invalidate hdr64FBO here: on Android the PBO readback is
		// asynchronous, and the GPU may still be reading this attachment.
		// Discarding it before the readback fence signals can produce unstable
		// luminance samples on tile-based GPUs.
#endif
		
		if ( hasLuminanceSample )
		{
			sum = 0.0f;
			maxLuminance = 0.0f;
			for( i = 0; i < ( 64 * 64 ); i += 4 )
			{
				color[0] = image[i * 4 + 0];
				color[1] = image[i * 4 + 1];
				color[2] = image[i * 4 + 2];
				color[3] = image[i * 4 + 3];

				luminance = ( color.x * LUMINANCE_SRGB.x + color.y * LUMINANCE_SRGB.y + color.z * LUMINANCE_SRGB.z ) + 0.0001f;
				if( luminance > maxLuminance )
				{
					maxLuminance = luminance;
				}

				//float logLuminance = log( luminance + 1 );
				//if( logLuminance > 0 )
				{
					sum += luminance;
				}
			}
#if 0
			sum /= ( 64.0f * 64.0f );
			avgLuminance = exp( sum );
#else
			avgLuminance = sum / ( 64.0f * 64.0f );
#endif

			// The user's adapted luminance level is simulated by closing the gap between
			// adapted luminance and current luminance by 2% every frame, based on a
			// 30 fps rate. Only update adaptation time when a GPU sample is available.
			if( hdrTime > curTime )
			{
				hdrTime = curTime;
			}

			deltaTime = curTime - hdrTime;

			//if(r_hdrMaxLuminance->value)
			{
				hdrAverageLuminance = idMath::ClampFloat( r_hdrMinLuminance.GetFloat(), r_hdrMaxLuminance.GetFloat(), hdrAverageLuminance );
				avgLuminance = idMath::ClampFloat( r_hdrMinLuminance.GetFloat(), r_hdrMaxLuminance.GetFloat(), avgLuminance );

				hdrMaxLuminance = idMath::ClampFloat( r_hdrMinLuminance.GetFloat(), r_hdrMaxLuminance.GetFloat(), hdrMaxLuminance );
				maxLuminance = idMath::ClampFloat( r_hdrMinLuminance.GetFloat(), r_hdrMaxLuminance.GetFloat(), maxLuminance );
			}

			newAdaptation = hdrAverageLuminance + ( avgLuminance - hdrAverageLuminance ) * ( 1.0f - powf( 0.98f, 30.0f * deltaTime ) );
			newMaximum = hdrMaxLuminance + ( maxLuminance - hdrMaxLuminance ) * ( 1.0f - powf( 0.98f, 30.0f * deltaTime ) );

			if( !IsNAN( newAdaptation ) && !IsNAN( newMaximum ) )
			{
#if 1
				hdrAverageLuminance = newAdaptation;
				hdrMaxLuminance = newMaximum;
#else
				hdrAverageLuminance = avgLuminance;
				hdrMaxLuminance = maxLuminance;
#endif
			}

			hdrTime = curTime;
		}

		// calculate HDR image key
#if 0
		// RB: this never worked :/
		if( r_hdrAutoExposure.GetBool() )
		{
			// calculation from: Perceptual Effects in Real-time Tone Mapping - Krawczyk et al.
			hdrKey = 1.03 - ( 2.0 / ( 2.0 + ( hdrAverageLuminance + 1.0f ) ) );
		}
		else
#endif
		{
			hdrKey = r_hdrKey.GetFloat();
		}
	}
	
	if( r_hdrDebug.GetBool() )
	{
		idLib::Printf( "HDR luminance avg = %f, max = %f, key = %f\n", hdrAverageLuminance, hdrMaxLuminance, hdrKey );
	}
	
	//GL_CheckErrors();
}


void idRenderBackend::Tonemap( const viewDef_t* _viewDef )
{
	RENDERLOG_PRINTF( "---------- RB_Tonemap( avg = %f, max = %f, key = %f, is2Dgui = %i ) ----------\n", hdrAverageLuminance, hdrMaxLuminance, hdrKey, ( int )viewDef->is2Dgui );
	
	//postProcessCommand_t* cmd = ( postProcessCommand_t* )data;
	//const idScreenRect& viewport = cmd->viewDef->viewport;
	//globalImages->currentRenderImage->CopyFramebuffer( viewport.x1, viewport.y1, viewport.GetWidth(), viewport.GetHeight() );

	// the full screen quad below replaces every pixel of the back buffer
	Framebuffer::InvalidateDefaultFramebufferForOverwrite();

	GL_State( GLS_SRCBLEND_ONE | GLS_DSTBLEND_ZERO | GLS_DEPTHMASK | GLS_DEPTHFUNC_ALWAYS | GLS_CULL_TWOSIDED );
	
	int screenWidth = renderSystem->GetWidth();
	int screenHeight = renderSystem->GetHeight();
	
	// set the window clipping
	GL_Viewport( 0, 0, screenWidth, screenHeight );
	GL_Scissor( 0, 0, screenWidth, screenHeight );
	
	GL_SelectTexture( 0 );
	
#if defined(USE_HDR_MSAA)
	if( glConfig.multisamples > 0 )
	{
		globalImages->currentRenderHDRImageNoMSAA->Bind();
	}
	else
#endif
	{
		globalImages->currentRenderHDRImage->Bind();
	}
	
	GL_SelectTexture( 1 );
	globalImages->heatmap7Image->Bind();
	
	if( r_hdrDebug.GetBool() )
	{
		renderProgManager.BindShader_HDRDebug();
	}
	else
	{
		renderProgManager.BindShader_Tonemap();
	}
	
	float screenCorrectionParm[4];
	if( _viewDef->is2Dgui )
	{
		screenCorrectionParm[0] = 2.0f;
		screenCorrectionParm[1] = 1.0f;
		screenCorrectionParm[2] = 1.0f;
	}
	else
	{
		if( r_hdrAutoExposure.GetBool() )
		{
			float exposureOffset = Lerp( -0.01f, 0.02f, idMath::ClampFloat( 0.0, 1.0, r_exposure.GetFloat() ) );
			
			screenCorrectionParm[0] = hdrKey + exposureOffset;
			screenCorrectionParm[1] = hdrAverageLuminance;
			screenCorrectionParm[2] = hdrMaxLuminance;
			screenCorrectionParm[3] = exposureOffset;
			//screenCorrectionParm[3] = Lerp( -1, 5, idMath::ClampFloat( 0.0, 1.0, r_exposure.GetFloat() ) );
		}
		else
		{
			//float exposureOffset = ( idMath::ClampFloat( 0.0, 1.0, r_exposure.GetFloat() ) * 2.0f - 1.0f ) * 0.01f;
			
			float exposureOffset = Lerp( -0.01f, 0.01f, idMath::ClampFloat( 0.0, 1.0, r_exposure.GetFloat() ) );
			
			screenCorrectionParm[0] = 0.015f + exposureOffset;
			screenCorrectionParm[1] = 0.005f;
			screenCorrectionParm[2] = 1;
			
			// RB: this gives a nice exposure curve in Scilab when using
			// log2( max( 3 + 0..10, 0.001 ) ) as input for exp2
			//float exposureOffset = r_exposure.GetFloat() * 10.0f;
			//screenCorrectionParm[3] = exposureOffset;
		}
	}
	
	SetFragmentParm( RENDERPARM_SCREENCORRECTIONFACTOR, screenCorrectionParm ); // rpScreenCorrectionFactor
	
	// Draw
	DrawElementsWithCounters( &unitSquareSurface );
	
	GL_SelectTexture( 0 );
	renderProgManager.Unbind();
	
	GL_State( GLS_DEFAULT );
}


void idRenderBackend::Bloom( const viewDef_t* _viewDef )
{
	bloomResultImage = NULL;

	if( _viewDef->is2Dgui || !r_useHDR.GetBool() )
	{
		return;
	}
	
	const GLenum tileColorAttachment[] = { GL_COLOR_ATTACHMENT0 };
	RENDERLOG_PRINTF( "---------- RB_Bloom( avg = %f, max = %f, key = %f ) ----------\n", hdrAverageLuminance, hdrMaxLuminance, hdrKey );
	
	// BRIGHTPASS
	
	//GL_CheckErrors();
	
	//Framebuffer::Unbind();
	//globalFramebuffers.hdrQuarterFBO->Bind();
	
	// FIXME
#if !defined(USE_VULKAN)
	glClearColor( 0, 0, 0, 1 );
//	glClear( GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT );
#endif

	GL_State( /*GLS_SRCBLEND_ONE | GLS_DSTBLEND_ZERO |*/ GLS_DEPTHMASK | GLS_DEPTHFUNC_ALWAYS | GLS_CULL_TWOSIDED );
	
	
	// set the window clipping
	GL_Viewport( 0, 0, globalFramebuffers.bloomRenderFBO[0]->GetWidth(), globalFramebuffers.bloomRenderFBO[0]->GetHeight() );
	GL_Scissor( 0, 0, globalFramebuffers.bloomRenderFBO[0]->GetWidth(), globalFramebuffers.bloomRenderFBO[0]->GetHeight() );
	
	globalFramebuffers.bloomRenderFBO[ 0 ]->Bind();
	globalFramebuffers.bloomRenderFBO[ 0 ]->InvalidateAttachments(tileColorAttachment, 1);
	
	GL_SelectTexture( 0 );
	
	if( r_useHDR.GetBool() )
	{
		globalImages->currentRenderHDRImage->Bind();
		
		renderProgManager.BindShader_Brightpass();
	}
	else
	{
		int x = viewDef->viewport.x1;
		int y = viewDef->viewport.y1;
		int	w = viewDef->viewport.x2 - viewDef->viewport.x1 + 1;
		int	h = viewDef->viewport.y2 - viewDef->viewport.y1 + 1;
		
		RENDERLOG_PRINTF( "Resolve to %i x %i buffer\n", w, h );
		
		// resolve the screen
		globalImages->currentRenderImage->CopyFramebuffer( x, y, w, h );
		
		renderProgManager.BindShader_Brightpass();
	}
	
	float screenCorrectionParm[4];
	screenCorrectionParm[0] = hdrKey;
	screenCorrectionParm[1] = hdrAverageLuminance;
	screenCorrectionParm[2] = hdrMaxLuminance;
	screenCorrectionParm[3] = 1.0f;
	SetFragmentParm( RENDERPARM_SCREENCORRECTIONFACTOR, screenCorrectionParm ); // rpScreenCorrectionFactor
	
	float overbright[4];
	if( r_useHDR.GetBool() )
	{
		if( r_hdrAutoExposure.GetBool() )
		{
			overbright[0] = r_hdrContrastDynamicThreshold.GetFloat();
		}
		else
		{
			overbright[0] = r_hdrContrastStaticThreshold.GetFloat();
		}
		overbright[1] = r_hdrContrastOffset.GetFloat();
		overbright[2] = 0;
		overbright[3] = 0;
	}
	else
	{
		overbright[0] = r_ldrContrastThreshold.GetFloat();
		overbright[1] = r_ldrContrastOffset.GetFloat();
		overbright[2] = 0;
		overbright[3] = 0;
	}
	SetFragmentParm( RENDERPARM_OVERBRIGHT, overbright ); // rpOverbright
	
	// Draw
	DrawElementsWithCounters( &unitSquareSurface );
	
	
	// BLOOM PING PONG rendering
	renderProgManager.BindShader_HDRGlareChromatic();
	
	// The old loop rendered one extra blur pass whose destination was immediately
	// invalidated and never sampled by the composite. Skip that wasted full-screen
	// draw, and composite the last pass that actually contributes to the image.
	int bloomPassCount = r_hdrGlarePasses.GetInteger();
	if ( bloomPassCount > 0 )
	{
		--bloomPassCount;
	}
	else
	{
		bloomPassCount = 0;
	}

	int j;
	for( j = 0; j < bloomPassCount; j++ )
	{
		Framebuffer* bloomTarget = globalFramebuffers.bloomRenderFBO[( j + 1 ) % 2 ];
		bloomTarget->Bind();
		GL_Viewport( 0, 0, bloomTarget->GetWidth(), bloomTarget->GetHeight() );
		GL_Scissor( 0, 0, bloomTarget->GetWidth(), bloomTarget->GetHeight() );
		if ( !bloomTarget->InvalidateAttachments( tileColorAttachment, 1 ) )
		{
#if !defined(USE_VULKAN)
			glClear( GL_COLOR_BUFFER_BIT );
#endif
		}

		globalImages->bloomRenderImage[j % 2]->Bind();

		DrawElementsWithCounters( &unitSquareSurface );
	}

	// BloomComposite adds it to the back buffer after the tonemap
	bloomResultImage = globalImages->bloomRenderImage[j % 2];

	renderProgManager.Unbind();

	GL_State( GLS_DEFAULT );
}

/*
==================
idRenderBackend::BloomComposite

Adds the glare Bloom blurred to the back buffer
==================
*/
void idRenderBackend::BloomComposite( const viewDef_t* _viewDef )
{
	if( bloomResultImage == NULL )
	{
		return;
	}

	// add filtered glare back to main context
	Framebuffer::Unbind();

	ResetViewportAndScissorToDefaultCamera( _viewDef );

	GL_State( GLS_SRCBLEND_ONE | GLS_DSTBLEND_ONE | GLS_DEPTHMASK | GLS_DEPTHFUNC_ALWAYS );

	renderProgManager.BindShader_Screen();

	GL_SelectTexture( 0 );
	bloomResultImage->Bind();
	bloomResultImage = NULL;

	DrawElementsWithCounters( &unitSquareSurface );

	renderProgManager.Unbind();

	GL_State( GLS_DEFAULT );
}


void idRenderBackend::DrawScreenSpaceAmbientOcclusion( const viewDef_t* _viewDef )
{
#if !defined(USE_VULKAN)
	if( !_viewDef->viewEntitys || _viewDef->is2Dgui )
	{
		// 3D views only
		return;
	}
	
	if( r_useSSAO.GetInteger() <= 0 )
	{
		return;
	}
	
	// FIXME very expensive to enable this in subviews
	if( _viewDef->isSubview )
	{
		return;
	}
	
	RENDERLOG_PRINTF( "---------- RB_SSAO() ----------\n" );
	
#if 0
	//GL_CheckErrors();
	
	// clear the alpha buffer and draw only the hands + weapon into it so
	// we can avoid blurring them
	glClearColor( 0, 0, 0, 1 );
	GL_State( GLS_COLORMASK | GLS_DEPTHMASK );
	glClear( GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT );
	GL_Color( 0, 0, 0, 0 );
	
	
	GL_SelectTexture( 0 );
	globalImages->blackImage->Bind();
	backEnd.currentSpace = NULL;
	
	drawSurf_t** drawSurfs = ( drawSurf_t** )&backEnd.viewDef->drawSurfs[0];
	for( int surfNum = 0; surfNum < backEnd.viewDef->numDrawSurfs; surfNum++ )
	{
		const drawSurf_t* surf = drawSurfs[ surfNum ];
		
		if( !surf->space->weaponDepthHack && !surf->space->skipMotionBlur && !surf->material->HasSubview() )
		{
			// Apply motion blur to this object
			continue;
		}
		
		const idMaterial* shader = surf->material;
		if( shader->Coverage() == MC_TRANSLUCENT )
		{
			// muzzle flash, etc
			continue;
		}
		
		// set mvp matrix
		if( surf->space != backEnd.currentSpace )
		{
			RB_SetMVP( surf->space->mvp );
			backEnd.currentSpace = surf->space;
		}
		
		// this could just be a color, but we don't have a skinned color-only prog
		if( surf->jointCache )
		{
			renderProgManager.BindShader_TextureVertexColorSkinned();
		}
		else
		{
			renderProgManager.BindShader_TextureVertexColor();
		}
		
		// draw it solid
		RB_DrawElementsWithCounters( surf );
	}
	GL_State( GLS_DEPTHFUNC_ALWAYS );
	
	// copy off the color buffer and the depth buffer for the motion blur prog
	// we use the viewport dimensions for copying the buffers in case resolution scaling is enabled.
	const idScreenRect& viewport = backEnd.viewDef->viewport;
	globalImages->currentRenderImage->CopyFramebuffer( viewport.x1, viewport.y1, viewport.GetWidth(), viewport.GetHeight() );
	
	// in stereo rendering, each eye needs to get a separate previous frame mvp
	int mvpIndex = ( backEnd.viewDef->renderView.viewEyeBuffer == 1 ) ? 1 : 0;
	
	// derive the matrix to go from current pixels to previous frame pixels
	idRenderMatrix	inverseMVP;
	idRenderMatrix::Inverse( backEnd.viewDef->worldSpace.mvp, inverseMVP );
	
	idRenderMatrix	motionMatrix;
	idRenderMatrix::Multiply( backEnd.prevMVP[mvpIndex], inverseMVP, motionMatrix );
	
	backEnd.prevMVP[mvpIndex] = backEnd.viewDef->worldSpace.mvp;
	
	RB_SetMVP( motionMatrix );
#endif
	
	currentSpace = &viewDef->worldSpace;
	RB_SetMVP( viewDef->worldSpace.mvp );
	
	const bool hdrIsActive = ( globalFramebuffers.hdrFBO != NULL && globalFramebuffers.hdrFBO->IsBound() );
	
	int screenWidth = renderSystem->GetWidth();
	int screenHeight = renderSystem->GetHeight();
	const GLenum tileColorAttachment[] = { GL_COLOR_ATTACHMENT0 };
	
	// build hierarchical depth buffer
	if( r_useHierarchicalDepthBuffer.GetBool() )
	{
		renderProgManager.BindShader_AmbientOcclusionMinify();
		
		glClearColor( 0, 0, 0, 1 );
		
		GL_SelectTexture( 0 );
		//globalImages->currentDepthImage->Bind();
		
		for( int i = 0; i < MAX_HIERARCHICAL_ZBUFFERS; i++ )
		{
			int width = globalFramebuffers.csDepthFBO[i]->GetWidth();
			int height = globalFramebuffers.csDepthFBO[i]->GetHeight();
			
			GL_Viewport( 0, 0, width, height );
			GL_Scissor( 0, 0, width, height );
			
			globalFramebuffers.csDepthFBO[i]->Bind();
			if (!globalFramebuffers.csDepthFBO[i]->InvalidateAttachments(tileColorAttachment, 1))
			{
				glClear( GL_COLOR_BUFFER_BIT );
			}
			
			if( i == 0 )
			{
				renderProgManager.BindShader_AmbientOcclusionReconstructCSZ();
				
				globalImages->currentDepthImage->Bind();
			}
			else
			{
				renderProgManager.BindShader_AmbientOcclusionMinify();
				
				GL_SelectTexture( 0 );
				globalImages->hierarchicalZbufferImage->Bind();
			}
			
			float jitterTexScale[4];
			jitterTexScale[0] = i - 1;
			jitterTexScale[1] = 0;
			jitterTexScale[2] = 0;
			jitterTexScale[3] = 0;
			SetFragmentParm( RENDERPARM_JITTERTEXSCALE, jitterTexScale ); // rpJitterTexScale
#if 1
			float screenCorrectionParm[4];
			screenCorrectionParm[0] = 1.0f / width;
			screenCorrectionParm[1] = 1.0f / height;
			screenCorrectionParm[2] = width;
			screenCorrectionParm[3] = height;
			SetFragmentParm( RENDERPARM_SCREENCORRECTIONFACTOR, screenCorrectionParm ); // rpScreenCorrectionFactor
#endif
			
			DrawElementsWithCounters( &unitSquareSurface );
		}
	}
	
	// set the window clipping
	GL_Viewport( 0, 0, screenWidth, screenHeight );
	GL_Scissor( 0, 0, screenWidth, screenHeight );
	
	GL_State( GLS_SRCBLEND_ONE | GLS_DSTBLEND_ZERO | GLS_DEPTHMASK | GLS_DEPTHFUNC_ALWAYS | GLS_CULL_TWOSIDED );
	
	if( r_ssaoFiltering.GetBool() )
	{
		globalFramebuffers.ambientOcclusionFBO[0]->Bind();
		GL_Viewport( 0, 0, globalFramebuffers.ambientOcclusionFBO[0]->GetWidth(), globalFramebuffers.ambientOcclusionFBO[0]->GetHeight() );
		GL_Scissor( 0, 0, globalFramebuffers.ambientOcclusionFBO[0]->GetWidth(), globalFramebuffers.ambientOcclusionFBO[0]->GetHeight() );
		if (!globalFramebuffers.ambientOcclusionFBO[0]->InvalidateAttachments(tileColorAttachment, 1))
		{
			glClearColor( 0, 0, 0, 0 );
			glClear( GL_COLOR_BUFFER_BIT );
		}
		
		renderProgManager.BindShader_AmbientOcclusion();
	}
	else
	{
		if( r_ssaoDebug.GetInteger() <= 0 )
		{
			GL_State( GLS_SRCBLEND_DST_COLOR | GLS_DSTBLEND_ZERO | GLS_ALPHAMASK | GLS_DEPTHMASK | GLS_DEPTHFUNC_ALWAYS );
		}
		
		if( hdrIsActive )
		{
			globalFramebuffers.hdrFBO->Bind();
		}
		else
		{
			Framebuffer::Unbind();
		}
		
		renderProgManager.BindShader_AmbientOcclusionAndOutput();
	}
	
	float screenCorrectionParm[4];
	screenCorrectionParm[0] = 1.0f / screenWidth;
	screenCorrectionParm[1] = 1.0f / screenHeight;
	screenCorrectionParm[2] = screenWidth;
	screenCorrectionParm[3] = screenHeight;
	SetFragmentParm( RENDERPARM_SCREENCORRECTIONFACTOR, screenCorrectionParm ); // rpScreenCorrectionFactor
	
#if 0
	// RB: set unprojection matrices so we can convert zbuffer values back to camera and world spaces
	idRenderMatrix modelViewMatrix;
	idRenderMatrix::Transpose( *( idRenderMatrix* )backEnd.viewDef->worldSpace.modelViewMatrix, modelViewMatrix );
	idRenderMatrix cameraToWorldMatrix;
	if( !idRenderMatrix::Inverse( modelViewMatrix, cameraToWorldMatrix ) )
	{
		idLib::Warning( "cameraToWorldMatrix invert failed" );
	}
	
	SetVertexParms( RENDERPARM_MODELMATRIX_X, cameraToWorldMatrix[0], 4 );
	//SetVertexParms( RENDERPARM_MODELMATRIX_X, viewDef->unprojectionToWorldRenderMatrix[0], 4 );
#endif
	SetVertexParms( RENDERPARM_MODELMATRIX_X, viewDef->unprojectionToCameraRenderMatrix[0], 4 );
	
	
	float jitterTexOffset[4];
	if( r_shadowMapRandomizeJitter.GetBool() )
	{
		jitterTexOffset[0] = ( rand() & 255 ) / 255.0;
		jitterTexOffset[1] = ( rand() & 255 ) / 255.0;
	}
	else
	{
		jitterTexOffset[0] = 0;
		jitterTexOffset[1] = 0;
	}
	jitterTexOffset[2] = viewDef->renderView.time[0] * 0.001f;
	jitterTexOffset[3] = 0.0f;
	SetFragmentParm( RENDERPARM_JITTERTEXOFFSET, jitterTexOffset ); // rpJitterTexOffset
	
	GL_SelectTexture( 0 );
	globalImages->currentNormalsImage->Bind();
	
	GL_SelectTexture( 1 );
	if( r_useHierarchicalDepthBuffer.GetBool() )
	{
		globalImages->hierarchicalZbufferImage->Bind();
	}
	else
	{
		globalImages->currentDepthImage->Bind();
	}
	
	DrawElementsWithCounters( &unitSquareSurface );
	
	if( r_ssaoFiltering.GetBool() )
	{
		float jitterTexScale[4];
		
		// AO blur X
#if 1
		globalFramebuffers.ambientOcclusionFBO[1]->Bind();
		GL_Viewport( 0, 0, globalFramebuffers.ambientOcclusionFBO[1]->GetWidth(), globalFramebuffers.ambientOcclusionFBO[1]->GetHeight() );
		GL_Scissor( 0, 0, globalFramebuffers.ambientOcclusionFBO[1]->GetWidth(), globalFramebuffers.ambientOcclusionFBO[1]->GetHeight() );
		globalFramebuffers.ambientOcclusionFBO[1]->InvalidateAttachments(tileColorAttachment, 1);
		
		renderProgManager.BindShader_AmbientOcclusionBlur();
		
		// set axis parameter
		jitterTexScale[0] = 1;
		jitterTexScale[1] = 0;
		jitterTexScale[2] = 0;
		jitterTexScale[3] = 0;
		SetFragmentParm( RENDERPARM_JITTERTEXSCALE, jitterTexScale ); // rpJitterTexScale
		
		GL_SelectTexture( 2 );
		globalImages->ambientOcclusionImage[0]->Bind();
		
		DrawElementsWithCounters( &unitSquareSurface );
#endif
		
		// AO blur Y
		GL_Viewport( 0, 0, screenWidth, screenHeight );
		GL_Scissor( 0, 0, screenWidth, screenHeight );
		if( hdrIsActive )
		{
			globalFramebuffers.hdrFBO->Bind();
		}
		else
		{
			Framebuffer::Unbind();
		}
		
		if( r_ssaoDebug.GetInteger() <= 0 )
		{
			GL_State( GLS_SRCBLEND_DST_COLOR | GLS_DSTBLEND_ZERO | GLS_DEPTHMASK | GLS_DEPTHFUNC_ALWAYS );
		}
		
		renderProgManager.BindShader_AmbientOcclusionBlurAndOutput();
		
		// set axis parameter
		jitterTexScale[0] = 0;
		jitterTexScale[1] = 1;
		jitterTexScale[2] = 0;
		jitterTexScale[3] = 0;
		SetFragmentParm( RENDERPARM_JITTERTEXSCALE, jitterTexScale ); // rpJitterTexScale
		
		GL_SelectTexture( 2 );
		globalImages->ambientOcclusionImage[1]->Bind();
		
		DrawElementsWithCounters( &unitSquareSurface );
	}
	
	renderProgManager.Unbind();
	
	GL_State( GLS_DEFAULT );
	
	//GL_CheckErrors();
#endif
}

void idRenderBackend::DrawScreenSpaceGlobalIllumination( const viewDef_t* _viewDef )
{
#if !defined(USE_VULKAN)
	if( !_viewDef->viewEntitys || _viewDef->is2Dgui )
	{
		// 3D views only
		return;
	}
	
	if( r_useSSGI.GetInteger() <= 0 )
	{
		return;
	}
	
	// FIXME very expensive to enable this in subviews
	if( _viewDef->isSubview )
	{
		return;
	}
	
	RENDERLOG_PRINTF( "---------- RB_SSGI() ----------\n" );
	
	currentSpace = &viewDef->worldSpace;
	RB_SetMVP( viewDef->worldSpace.mvp );
	
	const bool hdrIsActive = ( globalFramebuffers.hdrFBO != NULL && globalFramebuffers.hdrFBO->IsBound() );
	
	int screenWidth = renderSystem->GetWidth();
	int screenHeight = renderSystem->GetHeight();
	const GLenum tileColorAttachment[] = { GL_COLOR_ATTACHMENT0 };
	
	// set the window clipping
	GL_Viewport( 0, 0, screenWidth, screenHeight );
	GL_Scissor( 0, 0, screenWidth, screenHeight );
	
	if( !hdrIsActive )
	{
		const idScreenRect& viewport = viewDef->viewport;
		globalImages->currentRenderImage->CopyFramebuffer( viewport.x1, viewport.y1, viewport.GetWidth(), viewport.GetHeight() );
	}
	
	// build hierarchical depth buffer
	if( r_useHierarchicalDepthBuffer.GetBool() )
	{
		renderProgManager.BindShader_AmbientOcclusionMinify();
		
		glClearColor( 0, 0, 0, 1 );
		
		GL_SelectTexture( 0 );
		//globalImages->currentDepthImage->Bind();
		
		for( int i = 0; i < MAX_HIERARCHICAL_ZBUFFERS; i++ )
		{
			int width = globalFramebuffers.csDepthFBO[i]->GetWidth();
			int height = globalFramebuffers.csDepthFBO[i]->GetHeight();
			
			GL_Viewport( 0, 0, width, height );
			GL_Scissor( 0, 0, width, height );
			
			globalFramebuffers.csDepthFBO[i]->Bind();
			if (!globalFramebuffers.csDepthFBO[i]->InvalidateAttachments(tileColorAttachment, 1))
			{
				glClear( GL_COLOR_BUFFER_BIT );
			}
			
			if( i == 0 )
			{
				renderProgManager.BindShader_AmbientOcclusionReconstructCSZ();
				
				globalImages->currentDepthImage->Bind();
			}
			else
			{
				renderProgManager.BindShader_AmbientOcclusionMinify();
				
				GL_SelectTexture( 0 );
				globalImages->hierarchicalZbufferImage->Bind();
			}
			
			float jitterTexScale[4];
			jitterTexScale[0] = i - 1;
			jitterTexScale[1] = 0;
			jitterTexScale[2] = 0;
			jitterTexScale[3] = 0;
			SetFragmentParm( RENDERPARM_JITTERTEXSCALE, jitterTexScale ); // rpJitterTexScale
#if 1
			float screenCorrectionParm[4];
			screenCorrectionParm[0] = 1.0f / width;
			screenCorrectionParm[1] = 1.0f / height;
			screenCorrectionParm[2] = width;
			screenCorrectionParm[3] = height;
			SetFragmentParm( RENDERPARM_SCREENCORRECTIONFACTOR, screenCorrectionParm ); // rpScreenCorrectionFactor
#endif
			
			DrawElementsWithCounters( &unitSquareSurface );
		}
	}
	
	// set the window clipping
	GL_Viewport( 0, 0, screenWidth, screenHeight );
	GL_Scissor( 0, 0, screenWidth, screenHeight );
	
	GL_State( GLS_SRCBLEND_ONE | GLS_DSTBLEND_ZERO | GLS_DEPTHMASK | GLS_DEPTHFUNC_ALWAYS | GLS_CULL_TWOSIDED );
	
	if( r_ssgiFiltering.GetBool() )
	{
		globalFramebuffers.ambientOcclusionFBO[0]->Bind();
		GL_Viewport( 0, 0, globalFramebuffers.ambientOcclusionFBO[0]->GetWidth(), globalFramebuffers.ambientOcclusionFBO[0]->GetHeight() );
		GL_Scissor( 0, 0, globalFramebuffers.ambientOcclusionFBO[0]->GetWidth(), globalFramebuffers.ambientOcclusionFBO[0]->GetHeight() );
		if (!globalFramebuffers.ambientOcclusionFBO[0]->InvalidateAttachments(tileColorAttachment, 1))
		{
			// FIXME remove and mix with color from previous frame
			glClearColor( 0, 0, 0, 0 );
			glClear( GL_COLOR_BUFFER_BIT );
		}
		
		renderProgManager.BindShader_DeepGBufferRadiosity();
	}
	else
	{
		if( r_ssgiDebug.GetInteger() > 0 )
		{
			// replace current
			GL_State( GLS_SRCBLEND_ONE | GLS_DSTBLEND_ZERO | GLS_DEPTHMASK | GLS_DEPTHFUNC_ALWAYS );
		}
		else
		{
			// add result to main color
			GL_State( GLS_SRCBLEND_ONE | GLS_DSTBLEND_ONE | GLS_DEPTHMASK | GLS_DEPTHFUNC_ALWAYS );
		}
		
		if( hdrIsActive )
		{
			globalFramebuffers.hdrFBO->Bind();
		}
		else
		{
			Framebuffer::Unbind();
		}
		
		renderProgManager.BindShader_DeepGBufferRadiosity();
	}
	
	float screenCorrectionParm[4];
	screenCorrectionParm[0] = 1.0f / screenWidth;
	screenCorrectionParm[1] = 1.0f / screenHeight;
	screenCorrectionParm[2] = screenWidth;
	screenCorrectionParm[3] = screenHeight;
	SetFragmentParm( RENDERPARM_SCREENCORRECTIONFACTOR, screenCorrectionParm ); // rpScreenCorrectionFactor
	
#if 0
	// RB: set unprojection matrices so we can convert zbuffer values back to camera and world spaces
	idRenderMatrix modelViewMatrix;
	idRenderMatrix::Transpose( *( idRenderMatrix* )backEnd.viewDef->worldSpace.modelViewMatrix, modelViewMatrix );
	idRenderMatrix cameraToWorldMatrix;
	if( !idRenderMatrix::Inverse( modelViewMatrix, cameraToWorldMatrix ) )
	{
		idLib::Warning( "cameraToWorldMatrix invert failed" );
	}
	
	SetVertexParms( RENDERPARM_MODELMATRIX_X, cameraToWorldMatrix[0], 4 );
	//SetVertexParms( RENDERPARM_MODELMATRIX_X, viewDef->unprojectionToWorldRenderMatrix[0], 4 );
#endif
	SetVertexParms( RENDERPARM_MODELMATRIX_X, viewDef->unprojectionToCameraRenderMatrix[0], 4 );
	
	
	float jitterTexOffset[4];
	if( r_shadowMapRandomizeJitter.GetBool() )
	{
		jitterTexOffset[0] = ( rand() & 255 ) / 255.0;
		jitterTexOffset[1] = ( rand() & 255 ) / 255.0;
	}
	else
	{
		jitterTexOffset[0] = 0;
		jitterTexOffset[1] = 0;
	}
	jitterTexOffset[2] = viewDef->renderView.time[0] * 0.001f;
	jitterTexOffset[3] = 0.0f;
	SetFragmentParm( RENDERPARM_JITTERTEXOFFSET, jitterTexOffset ); // rpJitterTexOffset
	
	GL_SelectTexture( 0 );
	globalImages->currentNormalsImage->Bind();
	
	GL_SelectTexture( 1 );
	if( r_useHierarchicalDepthBuffer.GetBool() )
	{
		globalImages->hierarchicalZbufferImage->Bind();
	}
	else
	{
		globalImages->currentDepthImage->Bind();
	}
	
	GL_SelectTexture( 2 );
	if( hdrIsActive )
	{
		globalImages->currentRenderHDRImage->Bind();
	}
	else
	{
		globalImages->currentRenderImage->Bind();
	}
	
	DrawElementsWithCounters( &unitSquareSurface );
	
	if( r_ssgiFiltering.GetBool() )
	{
		float jitterTexScale[4];
		
		// AO blur X
#if 1
		globalFramebuffers.ambientOcclusionFBO[1]->Bind();
		GL_Viewport( 0, 0, globalFramebuffers.ambientOcclusionFBO[1]->GetWidth(), globalFramebuffers.ambientOcclusionFBO[1]->GetHeight() );
		GL_Scissor( 0, 0, globalFramebuffers.ambientOcclusionFBO[1]->GetWidth(), globalFramebuffers.ambientOcclusionFBO[1]->GetHeight() );
		globalFramebuffers.ambientOcclusionFBO[1]->InvalidateAttachments(tileColorAttachment, 1);
		
		renderProgManager.BindShader_DeepGBufferRadiosityBlur();
		
		// set axis parameter
		jitterTexScale[0] = 1;
		jitterTexScale[1] = 0;
		jitterTexScale[2] = 0;
		jitterTexScale[3] = 0;
		SetFragmentParm( RENDERPARM_JITTERTEXSCALE, jitterTexScale ); // rpJitterTexScale
		
		GL_SelectTexture( 2 );
		globalImages->ambientOcclusionImage[0]->Bind();
		
		DrawElementsWithCounters( &unitSquareSurface );
#endif
		
		// AO blur Y
		GL_Viewport( 0, 0, screenWidth, screenHeight );
		GL_Scissor( 0, 0, screenWidth, screenHeight );
		if( hdrIsActive )
		{
			globalFramebuffers.hdrFBO->Bind();
		}
		else
		{
			Framebuffer::Unbind();
		}
		
		if( r_ssgiDebug.GetInteger() > 0 )
		{
			// replace current
			GL_State( GLS_SRCBLEND_ONE | GLS_DSTBLEND_ZERO | GLS_DEPTHMASK | GLS_DEPTHFUNC_ALWAYS );
		}
		else
		{
			// add result to main color
			GL_State( GLS_SRCBLEND_ONE | GLS_DSTBLEND_ONE | GLS_DEPTHMASK | GLS_DEPTHFUNC_ALWAYS );
		}
		
		renderProgManager.BindShader_DeepGBufferRadiosityBlurAndOutput();
		
		// set axis parameter
		jitterTexScale[0] = 0;
		jitterTexScale[1] = 1;
		jitterTexScale[2] = 0;
		jitterTexScale[3] = 0;
		SetFragmentParm( RENDERPARM_JITTERTEXSCALE, jitterTexScale ); // rpJitterTexScale
		
		GL_SelectTexture( 2 );
		globalImages->ambientOcclusionImage[1]->Bind();
		
		DrawElementsWithCounters( &unitSquareSurface );
	}
	
	renderProgManager.Unbind();
	
	GL_State( GLS_DEFAULT );
	
	//GL_CheckErrors();
#endif
}
// RB end

/*
=========================================================================================================

BACKEND COMMANDS

=========================================================================================================
*/


/*
====================
RB_ExecuteBackEndCommands

This function will be called syncronously if running without
smp extensions, or asyncronously by another thread.
====================
*/
void idRenderBackend::ExecuteBackEndCommands( const emptyCommand_t* cmds )
{
	// r_debugRenderToTexture
	int c_draw3d = 0;
	int c_draw2d = 0;
	int c_setBuffers = 0;
	int c_copyRenders = 0;
	
	resolutionScale.SetCurrentGPUFrameTime( commonLocal.GetRendererGPUMicroseconds() );
	
	ResizeImages();
	
	renderLog.StartFrame();
	GL_StartFrame();
	g_frameFences.BeginFrame();
	Framebuffer::BeginFrame();

#if !defined( USE_VULKAN )
	// viewLight_t memory is reused from frame to frame, never match last frame's atlas
	numShadowAtlasLights = 0;
	hdrTargetClearedThisFrame = false;
	skipBackBufferClear = false;
#endif
	
#if defined(ANDROID) && !defined( USE_VULKAN )
	R_ResetWorldMerge();
	worldMergeSurf = NULL;
	worldMergeIndexBuffer = 0;

	// DeferredBeginFrame decides, the stereo path draws without it
	deferredFrame = false;
	deferredView = false;
	backBufferDrawnThisFrame = false;
#endif

	if( cmds->commandId == RC_NOP && !cmds->next )
	{
		Framebuffer::EndFrame();
		g_frameFences.EndFrame();
		return;
	}
	
	if( renderSystem->GetStereo3DMode() != STEREO3D_OFF )
	{
		StereoRenderExecuteBackEndCommands( cmds );
		Framebuffer::InvalidateDefaultFramebufferAttachments();
		Framebuffer::EndFrame();
		g_frameFences.EndFrame();
		renderLog.EndFrame();
		return;
	}
	
	uint64 backEndStartTime = Sys_Microseconds();
	
	// needed for editor rendering
	GL_SetDefaultState();

#if defined(ANDROID) && !defined( USE_VULKAN )
	// whether the 3D views of the frame get the G-buffer of the on-tile deferred shading
	DeferredBeginFrame( cmds );

	// the merged world surfaces of all views, the shadow atlas draws merged casters already
	WorldMergePass( cmds );
#endif

#if !defined( USE_VULKAN )
	// all shadow maps of the frame, before anything is drawn into the views' targets
	ShadowAtlasPass( cmds );

	// the tonemap of a HDR view draws every pixel of the back buffer, see SetBuffer
	if( r_useHDR.GetBool() )
	{
		for( const emptyCommand_t* cmd = cmds; cmd != NULL; cmd = ( const emptyCommand_t* )cmd->next )
		{
			if( cmd->commandId != RC_DRAW_VIEW_3D )
			{
				continue;
			}

			// the views DrawView renders and DrawViewInternal tonemaps
			const viewDef_t* view = ( ( const drawSurfsCommand_t* )cmd )->viewDef;
			if( view != NULL && view->numDrawSurfs > 0 && !view->is2Dgui && !( r_skipRender.GetBool() && view->viewEntitys != NULL ) )
			{
				skipBackBufferClear = true;
				break;
			}
		}
	}
#endif

	for( ; cmds != NULL; cmds = ( const emptyCommand_t* )cmds->next )
	{
		switch( cmds->commandId )
		{
			case RC_NOP:
				break;
				
			case RC_DRAW_VIEW_3D:
			case RC_DRAW_VIEW_GUI:
				DrawView( cmds, 0 );
				if( ( ( const drawSurfsCommand_t* )cmds )->viewDef->viewEntitys )
				{
					c_draw3d++;
				}
				else
				{
					c_draw2d++;
				}
				break;
				
			case RC_SET_BUFFER:
				SetBuffer( cmds );
				c_setBuffers++;
				break;
				
			case RC_COPY_RENDER:
				CopyRender( cmds );
				c_copyRenders++;
				break;
				
			case RC_POST_PROCESS:
				PostProcess( cmds );
				break;
				
			default:
				common->Error( "RB_ExecuteBackEndCommands: bad commandId" );
				break;
		}
	}
	
	DrawFlickerBox();
	Framebuffer::InvalidateDefaultFramebufferAttachments();
	Framebuffer::EndFrame();
	g_frameFences.EndFrame();
	GL_EndFrame();
	
	// stop rendering on this thread
	uint64 backEndFinishTime = Sys_Microseconds();
	pc.totalMicroSec = backEndFinishTime - backEndStartTime;
	
	if( r_debugRenderToTexture.GetInteger() == 1 )
	{
		common->Printf( "3d: %i, 2d: %i, SetBuf: %i, CpyRenders: %i, CpyFrameBuf: %i\n", c_draw3d, c_draw2d, c_setBuffers, c_copyRenders, pc.c_copyFrameBuffer );
		pc.c_copyFrameBuffer = 0;
	}
	
	renderLog.EndFrame();
}


/*
==================
idRenderBackend::DrawViewInternal
==================
*/
void idRenderBackend::DrawViewInternal( const viewDef_t* _viewDef, const int stereoEye )
{
	renderLog.OpenBlock( "RB_DrawViewInternal" );
	
	//-------------------------------------------------
	// guis can wind up referencing purged images that need to be loaded.
	// this used to be in the gui emit code, but now that it can be running
	// in a separate thread, it must not try to load images, so do it here.
	//-------------------------------------------------
	drawSurf_t** drawSurfs = ( drawSurf_t** )&_viewDef->drawSurfs[0];
	const int numDrawSurfs = _viewDef->numDrawSurfs;
	
	for( int i = 0; i < numDrawSurfs; i++ )
	{
		const drawSurf_t* ds = _viewDef->drawSurfs[ i ];
		if( ds->material != NULL )
		{
			const_cast<idMaterial*>( ds->material )->EnsureNotPurged();
		}
	}
	
	//-------------------------------------------------
	// RB_BeginDrawingView
	//
	// Any mirrored or portaled views have already been drawn, so prepare
	// to actually render the visible surfaces for this view
	//
	// clear the z buffer, set the projection matrix, etc
	//-------------------------------------------------
	ResetViewportAndScissorToDefaultCamera( _viewDef );
	
	// ensures that depth writes are enabled for the depth clear
	GL_State( GLS_DEFAULT | GLS_CULL_FRONTSIDED, true );
	
	//GL_CheckErrors();
	
	// RB begin
	bool useHDR = r_useHDR.GetBool() && !_viewDef->is2Dgui;

#if !defined(USE_VULKAN)
	// the HDR target, or without HDR the scene target of the deferred shading
	const bool useSceneFBO = UseSceneFramebuffer() && !_viewDef->is2Dgui;

#if defined(ANDROID)
	// the G-buffer and the lights from it, see DrawDeferredLight
	deferredView = deferredFrame && useSceneFBO && _viewDef->viewEntitys != NULL && !_viewDef->isXraySubview && !r_skipInteractions.GetBool();
	if( !useSceneFBO )
	{
		backBufferDrawnThisFrame = true;
	}
#endif

	// Bind the target first and clear the whole of it in one go: a tile based GPU starts
	// the render pass from the clear values then, instead of reading the target back in.
	if( useSceneFBO )
	{
		globalFramebuffers.hdrFBO->Bind();
	}
	else
	{
		Framebuffer::Unbind();
	}

	// SetBuffer only clears the back buffer, the first HDR view of the frame clears the HDR color.
	// Later views keep it, a mirror's subview leaves its image there.
	const bool clearColor = useSceneFBO && !hdrTargetClearedThisFrame;
	if( useSceneFBO )
	{
		hdrTargetClearedThisFrame = true;
	}

	// Clear the depth buffer and clear the stencil to 128 for stencil shadows as well as gui masking.
	// No view uses the depth or stencil of another view, so the whole target can be cleared.
	GL_Scissor( 0, 0, renderSystem->GetWidth(), renderSystem->GetHeight() );
	GL_Clear( clearColor, true, true, STENCIL_SHADOW_TEST_VALUE, 0.0f, 0.0f, 0.0f, 1.0f, false );
#if defined(ANDROID)
	// every view of the scene target clears the G-buffer, it is never read back into the tiles
	if( useSceneFBO )
	{
		DeferredClearGBuffer();
	}
#endif
	ResetViewportAndScissorToDefaultCamera( _viewDef );
#else
	// Clear the depth buffer and clear the stencil to 128 for stencil shadows as well as gui masking
	GL_Clear( false, true, true, STENCIL_SHADOW_TEST_VALUE, 0.0f, 0.0f, 0.0f, 0.0f, useHDR );

	if( useHDR )
	{
		globalFramebuffers.hdrFBO->Bind();
	}
	else
	{
		Framebuffer::Unbind();
	}
#endif
	// RB end
	
	//GL_CheckErrors();
	
#if !defined(USE_VULKAN)
	// bind one global Vertex Array Object (VAO)
	glBindVertexArray( glConfig.global_vao );
#endif
	
	//------------------------------------
	// sets variables that can be used by all programs
	//------------------------------------
	{
		//
		// set eye position in global space
		//
		float parm[4];
		parm[0] = viewDef->renderView.vieworg[0];
		parm[1] = viewDef->renderView.vieworg[1];
		parm[2] = viewDef->renderView.vieworg[2];
		parm[3] = 1.0f;
		
		SetVertexParm( RENDERPARM_GLOBALEYEPOS, parm ); // rpGlobalEyePos
		
		// sets overbright to make world brighter
		// This value is baked into the specularScale and diffuseScale values so
		// the interaction programs don't need to perform the extra multiply,
		// but any other renderprogs that want to obey the brightness value
		// can reference this.
		float overbright = r_lightScale.GetFloat() * 0.5f;
		parm[0] = overbright;
		parm[1] = overbright;
		parm[2] = overbright;
		parm[3] = overbright;
		SetFragmentParm( RENDERPARM_OVERBRIGHT, parm );
		
		// Set Projection Matrix
		float projMatrixTranspose[16];
		R_MatrixTranspose( viewDef->projectionMatrix, projMatrixTranspose );
		SetVertexParms( RENDERPARM_PROJMATRIX_X, projMatrixTranspose, 4 );
	}
	
	//-------------------------------------------------
	// fill the depth buffer and clear color buffer to black except on subviews
	//-------------------------------------------------
	FillDepthBufferFast( drawSurfs, numDrawSurfs );
	
	//-------------------------------------------------
	// FIXME, OPTIMIZE: merge this with FillDepthBufferFast like in a light prepass deferred renderer
	//
	// fill the geometric buffer with normals and roughness
	//-------------------------------------------------
	AmbientPass( drawSurfs, numDrawSurfs, true );

#if defined(ANDROID) && !defined(USE_VULKAN)
	if( deferredView )
	{
		//-------------------------------------------------
		// TBDR: the G-buffer in the tile memory, and the ambient of its surfaces
		//-------------------------------------------------
		DeferredGBufferPass( drawSurfs, numDrawSurfs );

		// the surfaces of more than one interaction keep the old ambient and interactions
		AmbientPass( drawSurfs, numDrawSurfs, false, true );
	}
	else
#endif
	{
		//-------------------------------------------------
		// fill the depth buffer and the color buffer with precomputed Q3A style lighting
		//-------------------------------------------------
		AmbientPass( drawSurfs, numDrawSurfs, false );
	}

	//-------------------------------------------------
	// main light renderer
	//-------------------------------------------------
	DrawInteractions( _viewDef );

#if defined(ANDROID) && !defined(USE_VULKAN)
	// nothing reads the G-buffer after the lights, a render pass end must not write it to memory
	if( useSceneFBO )
	{
		DeferredInvalidateGBuffer();
	}
	deferredView = false;
#endif

	//-------------------------------------------------
	// capture the depth for the motion blur before rendering any post process surfaces that may contribute to the depth
	//-------------------------------------------------
	if( ( r_motionBlur.GetInteger() > 0 ||  r_useSSAO.GetBool() || r_useSSGI.GetBool() ) && !r_useHDR.GetBool() )
	{
		const idScreenRect& viewport = viewDef->viewport;
		globalImages->currentDepthImage->CopyDepthbuffer( viewport.x1, viewport.y1, viewport.GetWidth(), viewport.GetHeight() );
	}
	
	//-------------------------------------------------
	// darken the scene using the screen space ambient occlusion
	//-------------------------------------------------
	DrawScreenSpaceAmbientOcclusion( _viewDef );
	//RB_SSGI( _viewDef );
	
	//-------------------------------------------------
	// now draw any non-light dependent shading passes
	//-------------------------------------------------
	int processed = 0;
	if( !r_skipShaderPasses.GetBool() )
	{
		renderLog.OpenMainBlock( MRB_DRAW_SHADER_PASSES );
		float guiScreenOffset;
		if( _viewDef->viewEntitys != NULL )
		{
			// guiScreenOffset will be 0 in non-gui views
			guiScreenOffset = 0.0f;
		}
		else
		{
			guiScreenOffset = stereoEye * _viewDef->renderView.stereoScreenSeparation;
		}
		//GK: Set rpWindowCoord to the Virtual resolution for Classic DOOM Screen Shaders
		float windowCoordParm[4];
		float clSWidth = 320.0f * r_clShaderScaler.GetFloat();
		float clSHeight = 240.0f * r_clShaderScaler.GetFloat();
		windowCoordParm[0] = 1.0f / clSWidth;
		windowCoordParm[1] = 1.0f / clSHeight;
		windowCoordParm[2] = clSWidth;
		windowCoordParm[3] = clSHeight;
		SetFragmentParm( RENDERPARM_WINDOWCOORD, windowCoordParm ); // rpWindowCoord
		processed = DrawShaderPasses( drawSurfs, numDrawSurfs, guiScreenOffset, stereoEye );
		renderLog.CloseMainBlock();
	}
	
	//-------------------------------------------------
	// use direct light and emissive light contributions to add indirect screen space light
	//-------------------------------------------------
	//RB_SSGI( viewDef );
	
	//-------------------------------------------------
	// fog and blend lights, drawn after emissive surfaces
	// so they are properly dimmed down
	//-------------------------------------------------
	FogAllLights();
	
	//-------------------------------------------------
	// now draw any screen warping post-process effects using _currentRender
	//-------------------------------------------------
	if( processed < numDrawSurfs && !r_skipPostProcess.GetBool() )
	{
		int x = viewDef->viewport.x1;
		int y = viewDef->viewport.y1;
		int	w = viewDef->viewport.x2 - viewDef->viewport.x1 + 1;
		int	h = viewDef->viewport.y2 - viewDef->viewport.y1 + 1;
		
		RENDERLOG_PRINTF( "Resolve to %i x %i buffer\n", w, h );
		
		GL_SelectTexture( 0 );
		
		// resolve the screen
		globalImages->currentRenderImage->CopyFramebuffer( x, y, w, h );
		currentRenderCopied = true;
		
		// RENDERPARM_SCREENCORRECTIONFACTOR amd RENDERPARM_WINDOWCOORD overlap
		// diffuseScale and specularScale
		
		// screen power of two correction factor (no longer relevant now)
		float screenCorrectionParm[4];
		screenCorrectionParm[0] = 1.0f;
		screenCorrectionParm[1] = 1.0f;
		screenCorrectionParm[2] = 0.0f;
		screenCorrectionParm[3] = 1.0f;
		SetFragmentParm( RENDERPARM_SCREENCORRECTIONFACTOR, screenCorrectionParm ); // rpScreenCorrectionFactor
		
		// window coord to 0.0 to 1.0 conversion
		float windowCoordParm[4];
		windowCoordParm[0] = 1.0f / w;
		windowCoordParm[1] = 1.0f / h;
		windowCoordParm[2] = 0.0f;
		windowCoordParm[3] = 1.0f;
		SetFragmentParm( RENDERPARM_WINDOWCOORD, windowCoordParm ); // rpWindowCoord
		
		// render the remaining surfaces
		renderLog.OpenMainBlock( MRB_DRAW_SHADER_PASSES_POST );
		DrawShaderPasses( drawSurfs + processed, numDrawSurfs - processed, 0.0f /* definitely not a gui */, stereoEye );
		renderLog.CloseMainBlock();
	}
	
	//-------------------------------------------------
	// render debug tools
	//-------------------------------------------------
#ifndef ANDROID
    DBG_RenderDebugTools( drawSurfs, numDrawSurfs );
#endif
#if !defined(USE_VULKAN)
#if defined(ANDROID)
	// without HDR the scene target of the deferred shading is copied to the back buffer
	if( useSceneFBO && !useHDR )
	{
		// Nothing reads the depth and stencil of the view once it is drawn,
		// don't let the end of the render pass write them out to memory.
		globalFramebuffers.hdrFBO->Bind();
		const GLenum sceneDepthStencilAttachment[] = { GL_DEPTH_STENCIL_ATTACHMENT };
		globalFramebuffers.hdrFBO->InvalidateAttachments( sceneDepthStencilAttachment, 1 );

		DeferredResolve( _viewDef );
	}
#endif

	// RB: convert back from HDR to LDR range
	if( useHDR )
	{
		// Nothing reads the depth and stencil of the view once it is drawn,
		// don't let the end of the render pass write them out to memory.
		globalFramebuffers.hdrFBO->Bind();
		const GLenum hdrDepthStencilAttachment[] = { GL_DEPTH_STENCIL_ATTACHMENT };
		globalFramebuffers.hdrFBO->InvalidateAttachments( hdrDepthStencilAttachment, 1 );

		/*
		int x = backEnd.viewDef->viewport.x1;
		int y = backEnd.viewDef->viewport.y1;
		int	w = backEnd.viewDef->viewport.x2 - backEnd.viewDef->viewport.x1 + 1;
		int	h = backEnd.viewDef->viewport.y2 - backEnd.viewDef->viewport.y1 + 1;
		
		GL_Viewport( viewDef->viewport.x1,
				viewDef->viewport.y1,
				viewDef->viewport.x2 + 1 - viewDef->viewport.x1,
				viewDef->viewport.y2 + 1 - viewDef->viewport.y1 );
		*/
		
		/*
		glBindFramebuffer( GL_READ_FRAMEBUFFER, globalFramebuffers.hdrFBO->GetFramebuffer() );
		glBindFramebuffer( GL_DRAW_FRAMEBUFFER, globalFramebuffers.hdrQuarterFBO->GetFramebuffer() );
		glBlitFramebuffer( 0, 0, renderSystem->GetWidth(), renderSystem->GetHeight(),
						   0, 0, renderSystem->GetWidth() * 0.25f, renderSystem->GetHeight() * 0.25f,
						   GL_COLOR_BUFFER_BIT,
						   GL_LINEAR );
		*/
		
#if defined(USE_HDR_MSAA)
		if( glConfig.multisamples > 0 )
		{
			glBindFramebuffer( GL_READ_FRAMEBUFFER, globalFramebuffers.hdrFBO->GetFramebuffer() );
			glBindFramebuffer( GL_DRAW_FRAMEBUFFER, globalFramebuffers.hdrNonMSAAFBO->GetFramebuffer() );
			const GLenum hdrColorAttachment[] = { GL_COLOR_ATTACHMENT0 };
			Framebuffer::InvalidateDrawFramebufferAttachments( hdrColorAttachment, 1 );
			glBlitFramebuffer( 0, 0, renderSystem->GetWidth(), renderSystem->GetHeight(),
							   0, 0, renderSystem->GetWidth(), renderSystem->GetHeight(),
							   GL_COLOR_BUFFER_BIT,
							   GL_LINEAR );
							   
			// TODO resolve to 1x1
			glBindFramebuffer( GL_READ_FRAMEBUFFER_EXT, globalFramebuffers.hdrNonMSAAFBO->GetFramebuffer() );
			glBindFramebuffer( GL_DRAW_FRAMEBUFFER_EXT, globalFramebuffers.hdr64FBO->GetFramebuffer() );
			const GLenum hdr64ColorAttachment[] = { GL_COLOR_ATTACHMENT0 };
			Framebuffer::InvalidateDrawFramebufferAttachments( hdr64ColorAttachment, 1 );
			glBlitFramebuffer( 0, 0, renderSystem->GetWidth(), renderSystem->GetHeight(),
							   0, 0, 64, 64,
							   GL_COLOR_BUFFER_BIT,
							   GL_LINEAR );
		}
		else
#endif
		{
#ifndef ANDROID
			if (glConfig.directStateAccess) {
				glBlitNamedFramebuffer(globalFramebuffers.hdrFBO->GetFramebuffer(), globalFramebuffers.hdr64FBO->GetFramebuffer(),
					0, 0, renderSystem->GetWidth(), renderSystem->GetHeight(),
					0, 0, 64, 64,
					GL_COLOR_BUFFER_BIT,
					GL_LINEAR);
			}
			else
#endif
			{
				glBindFramebuffer(GL_READ_FRAMEBUFFER_EXT, globalFramebuffers.hdrFBO->GetFramebuffer());
				glBindFramebuffer(GL_DRAW_FRAMEBUFFER_EXT, globalFramebuffers.hdr64FBO->GetFramebuffer());
				const GLenum hdr64ColorAttachment[] = { GL_COLOR_ATTACHMENT0 };
				Framebuffer::InvalidateDrawFramebufferAttachments( hdr64ColorAttachment, 1 );
				glBlitFramebuffer(0, 0, renderSystem->GetWidth(), renderSystem->GetHeight(),
					0, 0, 64, 64,
					GL_COLOR_BUFFER_BIT,
					GL_LINEAR);
			}
		}
		
		CalculateAutomaticExposure();

		// The glare is blurred in its own small targets before the tonemap, so the
		// tonemap and adding the glare share one render pass on the back buffer.
		// Adding it after the tonemap the old way wrote the back buffer out and read it back.
		Bloom( _viewDef );

		Tonemap( _viewDef );

		BloomComposite( _viewDef );
	}
#endif

	renderLog.CloseBlock();
}

/*
==================
RB_MotionBlur

Experimental feature
==================
*/
void idRenderBackend::MotionBlur()
{
	if( !viewDef->viewEntitys )
	{
		// 3D views only
		return;
	}
	if( r_motionBlur.GetInteger() <= 0 )
	{
		return;
	}
	if( viewDef->isSubview )
	{
		return;
	}
	
	//GL_CheckErrors();
	
	
	// clear the alpha buffer and draw only the hands + weapon into it so
	// we can avoid blurring them
	GL_State( GLS_COLORMASK | GLS_DEPTHMASK );
	
// FIXME
#if !defined(USE_VULKAN)
	glClearColor( 0, 0, 0, 1 );
	glClear( GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT );
	
#endif
	
	GL_Color( 0, 0, 0, 0 );
	GL_SelectTexture( 0 );
	globalImages->blackImage->Bind();
	currentSpace = NULL;
	
	drawSurf_t** drawSurfs = ( drawSurf_t** )&viewDef->drawSurfs[0];
#if defined(ANDROID) && !defined(USE_VULKAN)
	const int numMotionBlurSurfs = viewDef->numDrawSurfs;
	const drawSurf_t** motionBlurBatchSurfs = (const drawSurf_t**)_alloca(
			numMotionBlurSurfs * sizeof( const drawSurf_t* ) );
	bool* motionBlurConsumed = (bool*)_alloca( numMotionBlurSurfs * sizeof( bool ) );
	memset( motionBlurConsumed, 0, numMotionBlurSurfs * sizeof( bool ) );
	int motionBlurBatchCount = 0;
	int motionBlurDrawCallsSaved = 0;
	for( int i = 0; i < numMotionBlurSurfs; ++i )
	{
		const drawSurf_t* candidate = drawSurfs[i];
		const bool selected = candidate->space->weaponDepthHack
			|| candidate->space->skipMotionBlur || candidate->material->HasSubview();
		motionBlurBatchSurfs[i] = selected && candidate->material->Coverage() != MC_TRANSLUCENT
			? candidate : NULL;
	}
#endif
	for( int surfNum = 0; surfNum < viewDef->numDrawSurfs; surfNum++ )
	{
#if defined(ANDROID) && !defined(USE_VULKAN)
		if( motionBlurConsumed[surfNum] )
		{
			continue;
		}
		gpuInstanceData_t motionBlurInstances[MAX_GPU_INSTANCES_PER_DRAW];
		const gpuInstanceData_t* motionBlurInstanceData = NULL;
		int motionBlurInstanceCount = 1;
#endif
		const drawSurf_t* surf = drawSurfs[ surfNum ];
		
		if( !surf->space->weaponDepthHack && !surf->space->skipMotionBlur && !surf->material->HasSubview() )
		{
			// Apply motion blur to this object
			continue;
		}
		
		const idMaterial* shader = surf->material;
		if( shader->Coverage() == MC_TRANSLUCENT )
		{
			// muzzle flash, etc
			continue;
		}

#if defined(ANDROID) && !defined(USE_VULKAN)
		if( glConfig.glVersion >= 3.2f )
		{
			const int instanceCount = R_BuildGPUInstanceBatch( motionBlurBatchSurfs, numMotionBlurSurfs,
					surfNum, viewDef->renderView.vieworg, motionBlurConsumed, motionBlurInstances );
			if( instanceCount > 1 )
			{
				motionBlurInstanceCount = instanceCount;
				motionBlurInstanceData = motionBlurInstances;
				++motionBlurBatchCount;
				motionBlurDrawCallsSaved += instanceCount - 1;
			}
		}
#endif
		
		// set mvp matrix
		if( surf->space != currentSpace )
		{
			RB_SetMVP( surf->space->mvp );
			currentSpace = surf->space;
		}
		
		// this could just be a color, but we don't have a skinned color-only prog
		if( surf->jointCache )
		{
			renderProgManager.BindShader_TextureVertexColorSkinned();
		}
		else
		{
#if defined(ANDROID) && !defined(USE_VULKAN)
			if( motionBlurInstanceCount > 1 )
			{
				renderProgManager.BindShader_TextureVertexColorInstanced();
			}
			else
#endif
			{
				renderProgManager.BindShader_TextureVertexColor();
			}
		}
		
		// draw it solid
#if defined(ANDROID) && !defined(USE_VULKAN)
		DrawElementsWithCounters( surf, motionBlurInstanceData, motionBlurInstanceCount );
#else
		DrawElementsWithCounters( surf );
#endif
	}

#if defined(ANDROID) && !defined(USE_VULKAN)
	if( r_gpuInstancingDebug.GetBool() && motionBlurBatchCount > 0 )
	{
		idLib::Printf( "GPU instancing motion-blur mask: %d batches, %d draw calls saved\n",
				motionBlurBatchCount, motionBlurDrawCallsSaved );
	}
#endif
	
	GL_State( GLS_DEPTHFUNC_ALWAYS );
	
	// copy off the color buffer and the depth buffer for the motion blur prog
	// we use the viewport dimensions for copying the buffers in case resolution scaling is enabled.
	const idScreenRect& viewport = viewDef->viewport;
	globalImages->currentRenderImage->CopyFramebuffer( viewport.x1, viewport.y1, viewport.GetWidth(), viewport.GetHeight() );
	
	// in stereo rendering, each eye needs to get a separate previous frame mvp
	int mvpIndex = ( viewDef->renderView.viewEyeBuffer == 1 ) ? 1 : 0;
	
	// derive the matrix to go from current pixels to previous frame pixels
	idRenderMatrix	inverseMVP;
	idRenderMatrix::Inverse( viewDef->worldSpace.mvp, inverseMVP );
	
	idRenderMatrix	motionMatrix;
	idRenderMatrix::Multiply( prevMVP[mvpIndex], inverseMVP, motionMatrix );
	
	prevMVP[mvpIndex] = viewDef->worldSpace.mvp;
	
	RB_SetMVP( motionMatrix );
	
	GL_State( GLS_DEPTHFUNC_ALWAYS | GLS_CULL_TWOSIDED );
	
	renderProgManager.BindShader_MotionBlur();
	
	// let the fragment program know how many samples we are going to use
	idVec4 samples( ( float )( 1 << r_motionBlur.GetInteger() ) );
	SetFragmentParm( RENDERPARM_OVERBRIGHT, samples.ToFloatPtr() );
	
	GL_SelectTexture( 0 );
	globalImages->currentRenderImage->Bind();
	GL_SelectTexture( 1 );
	globalImages->currentDepthImage->Bind();
	
	DrawElementsWithCounters( &unitSquareSurface );
	//GL_CheckErrors();
}

/*
==================
idRenderBackend::DrawView

StereoEye will always be 0 in mono modes, or -1 / 1 in stereo modes.
If the view is a GUI view that is repeated for both eyes, the viewDef.stereoEye value
is 0, so the stereoEye parameter is not always the same as that.
==================
*/
void idRenderBackend::DrawView( const void* data, const int stereoEye )
{
	const drawSurfsCommand_t* cmd = ( const drawSurfsCommand_t* )data;
	
	viewDef = cmd->viewDef;
	
	// we will need to do a new copyTexSubImage of the screen
	// when a SS_POST_PROCESS material is used
	currentRenderCopied = false;
	
	// if there aren't any drawsurfs, do nothing
	if( !viewDef->numDrawSurfs )
	{
		return;
	}
	
	// skip render bypasses everything that has models, assuming
	// them to be 3D views, but leaves 2D rendering visible
	if( r_skipRender.GetBool() && viewDef->viewEntitys )
	{
		return;
	}
	
	// skip render context sets the wgl context to NULL,
	// which should factor out the API cost, under the assumption
	// that all gl calls just return if the context isn't valid
	
	// RB: not really needed
	//if( r_skipRenderContext.GetBool() && backEnd.viewDef->viewEntitys )
	//{
	//	GLimp_DeactivateContext();
	//}
	// RB end
	
	pc.c_surfaces += viewDef->numDrawSurfs;
#ifndef ANDROID
	DBG_ShowOverdraw();
#endif
	// render the scene
	DrawViewInternal( cmd->viewDef, stereoEye );
	
	MotionBlur();
	
	// restore the context for 2D drawing if we were stubbing it out
	// RB: not really needed
	//if( r_skipRenderContext.GetBool() && backEnd.viewDef->viewEntitys )
	//{
	//	GLimp_ActivateContext();
	//	GL_SetDefaultState();
	//}
	// RB end
	
	// optionally draw a box colored based on the eye number
	if( r_drawEyeColor.GetBool() )
	{
		const idScreenRect& r = viewDef->viewport;
		GL_Scissor( ( r.x1 + r.x2 ) / 2, ( r.y1 + r.y2 ) / 2, 32, 32 );
		switch( stereoEye )
		{
			case -1:
				GL_Clear( true, false, false, 0, 1.0f, 0.0f, 0.0f, 1.0f );
				break;
			case 1:
				GL_Clear( true, false, false, 0, 0.0f, 1.0f, 0.0f, 1.0f );
				break;
			default:
				GL_Clear( true, false, false, 0, 0.5f, 0.5f, 0.5f, 1.0f );
				break;
		}
	}
}

/*
==================
idRenderBackend::CopyRender

Copy part of the current framebuffer to an image
==================
*/
void idRenderBackend::CopyRender( const void* data )
{
	const copyRenderCommand_t* cmd = ( const copyRenderCommand_t* )data;
	
	if( r_skipCopyTexture.GetBool() )
	{
		return;
	}
	
	RENDERLOG_PRINTF( "***************** RB_CopyRender *****************\n" );
	
	if( cmd->image )
	{
		cmd->image->CopyFramebuffer( cmd->x, cmd->y, cmd->imageWidth, cmd->imageHeight );
	}
	
	if( cmd->clearColorAfterCopy )
	{
		GL_Clear( true, false, false, STENCIL_SHADOW_TEST_VALUE, 0, 0, 0, 0 );
	}
}

/*
==================
idRenderBackend::PostProcess

==================
*/
extern idCVar rs_enable;
void idRenderBackend::PostProcess( const void* data )
{
	// only do the post process step if resolution scaling is enabled. Prevents the unnecessary copying of the framebuffer and
	// corresponding full screen quad pass.
	if( rs_enable.GetInteger() == 0 && !r_useFilmicPostProcessEffects.GetBool() && r_antiAliasing.GetInteger() == 0 )
	{
		return;
	}
	
	if( ( r_ssaoDebug.GetInteger() > 0 ) || ( r_ssgiDebug.GetInteger() > 0 ) )
	{
		return;
	}
	
	RENDERLOG_PRINTF( "---------- RB_PostProcess() ----------\n" );
	
// FIXME
#if !defined(USE_VULKAN)

	// resolve the scaled rendering to a temporary texture
	postProcessCommand_t* cmd = ( postProcessCommand_t* )data;
	const idScreenRect& viewport = cmd->viewDef->viewport;
	
	GL_State( GLS_SRCBLEND_ONE | GLS_DSTBLEND_ZERO | GLS_DEPTHMASK | GLS_DEPTHFUNC_ALWAYS |  GLS_CULL_TWOSIDED );
	
	int screenWidth = renderSystem->GetWidth();
	int screenHeight = renderSystem->GetHeight();
	
	// set the window clipping
	GL_Viewport( 0, 0, screenWidth, screenHeight );
	GL_Scissor( 0, 0, screenWidth, screenHeight );
	
	// SMAA
	int aaMode = r_antiAliasing.GetInteger();
	if( aaMode == ANTI_ALIASING_SMAA_1X )
	{
		/*
		 * The shader has three passes, chained together as follows:
		 *
		 *                           |input|------------------�
		 *                              v                     |
		 *                    [ SMAA*EdgeDetection ]          |
		 *                              v                     |
		 *                          |edgesTex|                |
		 *                              v                     |
		 *              [ SMAABlendingWeightCalculation ]     |
		 *                              v                     |
		 *                          |blendTex|                |
		 *                              v                     |
		 *                [ SMAANeighborhoodBlending ] <------�
		 *                              v
		 *                           |output|
		*/
		
		globalImages->smaaInputImage->CopyFramebuffer( 0, 0, screenWidth, screenHeight );
		
		// set SMAA_RT_METRICS = rpScreenCorrectionFactor
		float screenCorrectionParm[4];
		screenCorrectionParm[0] = 1.0f / screenWidth;
		screenCorrectionParm[1] = 1.0f / screenHeight;
		screenCorrectionParm[2] = screenWidth;
		screenCorrectionParm[3] = screenHeight;
		SetFragmentParm( RENDERPARM_SCREENCORRECTIONFACTOR, screenCorrectionParm ); // rpScreenCorrectionFactor
		
		const GLenum tileColorAttachment[] = { GL_COLOR_ATTACHMENT0 };
		globalFramebuffers.smaaEdgesFBO->Bind();
		if (!globalFramebuffers.smaaEdgesFBO->InvalidateAttachments(tileColorAttachment, 1))
		{
			glClearColor( 0, 0, 0, 0 );
			glClear( GL_COLOR_BUFFER_BIT );
		}
		
		GL_SelectTexture( 0 );
		globalImages->smaaInputImage->Bind();
		
		renderProgManager.BindShader_SMAA_EdgeDetection();
		DrawElementsWithCounters( &unitSquareSurface );
		
#if 1
		//globalImages->smaaEdgesImage->CopyFramebuffer( viewport.x1, viewport.y1, viewport.GetWidth(), viewport.GetHeight() );
		
		globalFramebuffers.smaaBlendFBO->Bind();
		//Framebuffer::Unbind();
		if (!globalFramebuffers.smaaBlendFBO->InvalidateAttachments(tileColorAttachment, 1))
		{
			glClear( GL_COLOR_BUFFER_BIT );
		}
		
		GL_SelectTexture( 0 );
		globalImages->smaaEdgesImage->Bind();
		
		GL_SelectTexture( 1 );
		globalImages->smaaAreaImage->Bind();
		
		GL_SelectTexture( 2 );
		globalImages->smaaSearchImage->Bind();
		
		renderProgManager.BindShader_SMAA_BlendingWeightCalculation();
		DrawElementsWithCounters( &unitSquareSurface );
		
		Framebuffer::Unbind();
#endif
		
#if 1
		//GL_SelectTexture( 0 );
		//globalImages->smaaBlendImage->CopyFramebuffer( viewport.x1, viewport.y1, viewport.GetWidth(), viewport.GetHeight() );
		
		GL_SelectTexture( 0 );
		globalImages->smaaInputImage->Bind();
		
		GL_SelectTexture( 1 );
		globalImages->smaaBlendImage->Bind();
		
		renderProgManager.BindShader_SMAA_NeighborhoodBlending();
		DrawElementsWithCounters( &unitSquareSurface );
#endif
	}
	
#if 1
	if( r_useFilmicPostProcessEffects.GetBool() )
	{
		globalImages->currentRenderImage->CopyFramebuffer( viewport.x1, viewport.y1, viewport.GetWidth(), viewport.GetHeight() );
		
		GL_SelectTexture( 0 );
		globalImages->currentRenderImage->Bind();
		
		GL_SelectTexture( 1 );
		globalImages->grainImage1->Bind();
		
		renderProgManager.BindShader_PostProcess();
		
		const static int GRAIN_SIZE = 128;
		
		// screen power of two correction factor
		float screenCorrectionParm[4];
		screenCorrectionParm[0] = 1.0f / GRAIN_SIZE;
		screenCorrectionParm[1] = 1.0f / GRAIN_SIZE;
		screenCorrectionParm[2] = 1.0f;
		screenCorrectionParm[3] = 1.0f;
		SetFragmentParm( RENDERPARM_SCREENCORRECTIONFACTOR, screenCorrectionParm ); // rpScreenCorrectionFactor
		
		float jitterTexOffset[4];
		if( r_shadowMapRandomizeJitter.GetBool() )
		{
			jitterTexOffset[0] = ( rand() & 255 ) / 255.0;
			jitterTexOffset[1] = ( rand() & 255 ) / 255.0;
		}
		else
		{
			jitterTexOffset[0] = 0;
			jitterTexOffset[1] = 0;
		}
		jitterTexOffset[2] = 0.0f;
		jitterTexOffset[3] = 0.0f;
		SetFragmentParm( RENDERPARM_JITTERTEXOFFSET, jitterTexOffset ); // rpJitterTexOffset
		
		// Draw
		DrawElementsWithCounters( &unitSquareSurface );
	}
#endif
	
	GL_SelectTexture( 0 );
	renderProgManager.Unbind();
	
#endif
	
	renderLog.CloseBlock();
}
