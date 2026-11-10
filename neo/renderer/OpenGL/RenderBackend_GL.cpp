/*
===========================================================================

Doom 3 BFG Edition GPL Source Code
Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company.
Copyright (C) 2013-2015 Robert Beckebans
Copyright (C) 2016-2017 Dustin Land

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
#include <string>
#include <vector>
#endif
#pragma hdrstop

#include "../RenderCommon.h"
#include "../RenderBackend.h"
#include "../../framework/Common_local.h"

#if defined(ANDROID) && !defined(USE_VULKAN)
extern void RB_InitExposureReadback();
extern void RB_ShutdownExposureReadback();
extern void RB_ShutdownWorldMerge();
static void R_ShutdownInstanceStream();
#endif
#if ANDROID

#define GL_MULTISAMPLE                    0x809D
#define GL_TEXTURE_MAX_ANISOTROPY_EXT     0x84FE
#define GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT 0x84FF
#endif

#ifdef USE_OPENXR
#include "../OpenXR/XRCommon.h"
#endif

idCVar r_drawFlickerBox( "r_drawFlickerBox", "0", CVAR_RENDERER | CVAR_BOOL, "visual test for dropping frames" );
idCVar stereoRender_warp( "stereoRender_warp", "1", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_BOOL, "use the optical warping renderprog instead of stereoDeGhost" );
idCVar stereoRender_warpStrength( "stereoRender_warpStrength", "1.45", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "amount of pre-distortion" );

idCVar stereoRender_warpCenterX( "stereoRender_warpCenterX", "0.5", CVAR_RENDERER | CVAR_FLOAT | CVAR_ARCHIVE, "center for left eye, right eye will be 1.0 - this" );
idCVar stereoRender_warpCenterY( "stereoRender_warpCenterY", "0.5", CVAR_RENDERER | CVAR_FLOAT | CVAR_ARCHIVE, "center for both eyes" );
idCVar stereoRender_warpParmZ( "stereoRender_warpParmZ", "0", CVAR_RENDERER | CVAR_FLOAT | CVAR_ARCHIVE, "development parm" );
idCVar stereoRender_warpParmW( "stereoRender_warpParmW", "0", CVAR_RENDERER | CVAR_FLOAT | CVAR_ARCHIVE, "development parm" );
idCVar stereoRender_warpTargetFraction( "stereoRender_warpTargetFraction", "1.0", CVAR_RENDERER | CVAR_FLOAT | CVAR_ARCHIVE, "fraction of half-width the through-lens view covers" );
idCVar r_useOpenGLDSA("r_useOpenGLDSA", "0", CVAR_RENDERER | CVAR_INTEGER | CVAR_ARCHIVE, "Enable/Disable OpenGL DSA. -1 = Enable based on system detection, 0 = Force to Disable, 1 = Force to Enable", -1, 1);
#if defined(ANDROID) && !defined(USE_VULKAN)
idCVar r_useMultiDrawIndirect("r_useMultiDrawIndirect", "1", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_BOOL,
    "Batch compatible GLES indexed draws with GL_EXT_multi_draw_indirect");
idCVar r_showMultiDrawIndirectStats("r_showMultiDrawIndirectStats", "0", CVAR_RENDERER | CVAR_BOOL,
    "Print GLES multi-draw-indirect batching statistics each frame");
#endif

//#ifdef _DEBUG
//idCVar r_showSwapBuffers("r_showSwapBuffers", "1", CVAR_BOOL, "Show timings from GL_BlockingSwapBuffers");
//#else
idCVar r_showSwapBuffers( "r_showSwapBuffers", "0", CVAR_BOOL, "Show timings from GL_BlockingSwapBuffers" );
//#endif
idCVar r_syncEveryFrame( "r_syncEveryFrame", "0", CVAR_BOOL, "Don't let the GPU buffer execution past swapbuffers" );

//GK: Begin
idCVar r_showGLExt("r_showGLExt", "0", CVAR_RENDERER | CVAR_BOOL, "Shows the OpenGL Extensions on the logfile");
//GK: End

extern idCVar r_oldGLSLVersion;
extern idCVar r_motionBlur;
extern idCVar g_fov;
void GLimp_SwapBuffers();
void RB_SetMVP( const idRenderMatrix& mvp );

glContext_t glcontext;
static int		swapIndex;		// 0 or 1 into renderSync
static GLsync	renderSync[2];
static uintptr_t currentVertOffset = ~0;
#if defined(ANDROID) && !defined(USE_VULKAN)
struct drawElementsIndirectCommand_t
{
    GLuint count;
    GLuint instanceCount;
    GLuint firstIndex;
    GLint baseVertex;
    GLuint baseInstance;
};
static std::vector<drawElementsIndirectCommand_t> g_multiDrawCommands;
static GLuint g_multiDrawIndirectBuffer = 0;
static GLuint g_multiDrawVertexBuffer = 0;
static GLuint g_multiDrawIndexBuffer = 0;
static bool g_multiDrawIndirectAvailable = false;
static int g_multiDrawIndirectBatchCount = 0;
static int g_multiDrawIndirectDrawCallsSaved = 0;
static int g_multiDrawIndirectSubmittedCommands = 0;
static const size_t MAX_GLES_MDI_BATCH_COMMANDS = 256;
enum multiDrawLayoutKey_t
{
    MDI_LAYOUT_NONE = 0,
    MDI_LAYOUT_DRAW_VERT,
    MDI_LAYOUT_SHADOW_VERT
};
static multiDrawLayoutKey_t g_multiDrawBatchLayout = MDI_LAYOUT_NONE;
#endif
static bool gEnableGLSynchronization = false;

#if ANDROID
const int DEFAULT_GLES_VERSION = 320;
int glesVersion = DEFAULT_GLES_VERSION;
static bool g_enableDXTSupport = false;

extern "C"{
__attribute__((used)) __attribute__((visibility("default")))
void setHardwareDXTSupport(const bool enableDXTSupport) {
	g_enableDXTSupport = enableDXTSupport;
}

__attribute__((used)) __attribute__((visibility("default")))
void setGLESVersion(const int targetGLESVersion) {
	glesVersion = targetGLESVersion;
}

__attribute__((used)) __attribute__((visibility("default")))
void updateGLSynchronizationState(const bool enableGLSynchronization) {
	gEnableGLSynchronization = enableGLSynchronization;
}
}

bool isGLES32Version (){
    return glesVersion == DEFAULT_GLES_VERSION;
}

static inline bool IsDXTBCSupported() {
	GLint numFormats = 0;
	glGetIntegerv(GL_NUM_COMPRESSED_TEXTURE_FORMATS, &numFormats);
	if (numFormats <= 0) {
		return false;
	}
	std::vector<GLint> formats(numFormats);
	glGetIntegerv(GL_COMPRESSED_TEXTURE_FORMATS, formats.data());

	for (GLint i = 0; i < numFormats; ++i) {
		switch (formats[i]) {
			case GL_COMPRESSED_RGB_S3TC_DXT1_EXT:
			case GL_COMPRESSED_RGBA_S3TC_DXT1_EXT:
			case GL_COMPRESSED_RGBA_S3TC_DXT3_EXT:
			case GL_COMPRESSED_RGBA_S3TC_DXT5_EXT:
			case GL_COMPRESSED_RED_RGTC1_EXT:
			case GL_COMPRESSED_SIGNED_RED_RGTC1_EXT:
				return true;
		}
	}

	return false;
}
#else
bool isGLES32Version (){
    return true;
}
#endif


/*
==================
GL_CheckErrors
==================
*/
// RB: added filename, line parms
bool GL_CheckErrors_( const char* filename, int line )
{
	int		err;
	char	s[64];
	int		i;
	
	if( r_ignoreGLErrors.GetBool() )
	{
		return false;
	}
	
	// check for up to 10 errors pending
	bool error = false;
	for( i = 0 ; i < 10 ; i++ )
	{
		err = glGetError();
		if( err == GL_NO_ERROR )
		{
			break;
		}
		
		error = true;
		switch( err )
		{
			case GL_INVALID_ENUM:
				strcpy( s, "GL_INVALID_ENUM" );
				break;
			case GL_INVALID_VALUE:
				strcpy( s, "GL_INVALID_VALUE" );
				break;
			case GL_INVALID_OPERATION:
				strcpy( s, "GL_INVALID_OPERATION" );
				break;
#if !defined(USE_GLES2) && !defined(USE_GLES3) && !ANDROID
			case GL_STACK_OVERFLOW:
				strcpy( s, "GL_STACK_OVERFLOW" );
				break;
			case GL_STACK_UNDERFLOW:
				strcpy( s, "GL_STACK_UNDERFLOW" );
				break;
#endif
			case GL_OUT_OF_MEMORY:
				strcpy( s, "GL_OUT_OF_MEMORY" );
				break;
			default:
				idStr::snPrintf( s, sizeof( s ), "%i", err );
				break;
		}
		//strcpy(s, (const char*)glErrorStringREGAL(err)); //GK: Better ?
		common->Printf( "caught OpenGL error: %d-%s in file %s line %i\n", err, s, filename, line );
	}
	
	return error;
}

/*
========================
DebugCallback

For ARB_debug_output
========================
*/
// RB: added const to userParam
static void CALLBACK DebugCallback( unsigned int source, unsigned int type,
									unsigned int id, unsigned int severity, int length, const char* message, const void* userParam )
{
#ifndef ANDROID
	// it probably isn't safe to do an idLib::Printf at this point
	idStr sourceStr = "";
	switch (source) {
	case GL_DEBUG_SOURCE_API:
		sourceStr = "OpenGL C/C++ API call";
		break;
	case GL_DEBUG_SOURCE_WINDOW_SYSTEM:
		sourceStr = "Window-system API call";
		break;
	case GL_DEBUG_SOURCE_SHADER_COMPILER:
		sourceStr = "Shader compilation";
		break;
	case GL_DEBUG_SOURCE_THIRD_PARTY:
		sourceStr = "Associate OpenGL Application";
		break;
	case GL_DEBUG_SOURCE_APPLICATION:
		sourceStr = "User Error";
		break;
	default:
		sourceStr = "Unknown";
		break;
	}

	idStr typeStr = "";
	switch (type) {
	case GL_DEBUG_TYPE_ERROR:
		typeStr = "OpenGL Error";
		break;
	case GL_DEBUG_TYPE_DEPRECATED_BEHAVIOR:
		typeStr = "Deprecated call(s) in use";
		break;
	case GL_DEBUG_TYPE_UNDEFINED_BEHAVIOR:
		typeStr = "Undefined Behavior";
		break;
	case GL_DEBUG_TYPE_PORTABILITY:
		typeStr = "Non portable functionallity reliance";
		break;
	case GL_DEBUG_TYPE_PERFORMANCE:
		typeStr = "Possible Performance issue";
		break;
	case GL_DEBUG_TYPE_MARKER:
		typeStr = "Marker";
		break;
	case GL_DEBUG_TYPE_PUSH_GROUP:
		typeStr = "Group Pushing";
		break;
	case GL_DEBUG_TYPE_POP_GROUP:
		typeStr = "Group Poping";
		break;
	default:
		typeStr = "Unknown";
		break;
	}

	idStr severityStr = "";
	switch (severity) {
	case GL_DEBUG_SEVERITY_HIGH:
		severityStr = "High";
		break;
	case GL_DEBUG_SEVERITY_MEDIUM:
		severityStr = "Medium";
		break;
	case GL_DEBUG_SEVERITY_LOW:
		severityStr = "Low";
		break;
	default:
		severityStr = "Notification";
		break;
	}
	// RB: printf should be thread safe on Linux
	idLib::Printf("caught OpenGL Error:\n\tSource:%s\n\tType: %s\n\tSeverity: %s\n\tMessage: %s\n%s", sourceStr.c_str(), typeStr.c_str(), severityStr.c_str(), message, callstack.c_str());
	// RB end
#endif
}

static void R_PrintExtensionStatus(bool extBool, const char* extName) {
	if (extBool) {
		common->Printf("...using %s\n", extName);
	}
	else
	{
		common->Printf("X..%s not found\n", extName);
	}
}

/*
==================
R_CheckPortableExtensions
==================
*/
// RB: replaced QGL with GLEW
static void R_CheckPortableExtensions()
{
	glConfig.glVersion = atof( glConfig.version_string );
	const char* badVideoCard = idLocalization::GetString( "#str_06780" );
	if( glConfig.glVersion < 2.0f )
	{
		idLib::FatalError( "%s", badVideoCard );
	}
	
	if( idStr::Icmpn( glConfig.vendor_string, "ATI ", 4 ) == 0 || idStr::Icmpn( glConfig.vendor_string, "AMD ", 4 ) == 0 )
	{
		glConfig.vendor = VENDOR_AMD;
	}
	else if( idStr::Icmpn( glConfig.vendor_string, "NVIDIA", 6 ) == 0 )
	{
		glConfig.vendor = VENDOR_NVIDIA;
	}
	else if( idStr::Icmpn( glConfig.vendor_string, "Intel", 5 ) == 0 )
	{
		glConfig.vendor = VENDOR_INTEL;
	}

#ifndef ANDROID
	// RB: Mesa support
	if( idStr::Icmpn( glConfig.renderer_string, "Mesa", 4 ) == 0 || idStr::Icmpn( glConfig.renderer_string, "X.org", 5 ) == 0 || idStr::Icmpn( glConfig.renderer_string, "Gallium", 7 ) == 0 ||
			strcmp( glConfig.vendor_string, "X.Org" ) == 0 ||
			idStr::Icmpn( glConfig.renderer_string, "llvmpipe", 8 ) == 0 /*|| idStr::Icmpn( glConfig.renderer_string, "SVGA3D", 6 ) == 0*/ )
	{
		if( glConfig.driverType == GLDRV_OPENGL32_CORE_PROFILE )
		{
			glConfig.driverType = GLDRV_OPENGL_MESA_CORE_PROFILE;
		}
		else
		{
			glConfig.driverType = GLDRV_OPENGL_MESA;
		}
	}
#else
    glConfig.driverType = GLDRV_OPENGL_MESA;
#endif
	// RB end
#ifndef ANDROID
	// GL_ARB_multitexture
	if( glConfig.driverType != GLDRV_OPENGL3X )
	{
		glConfig.multitextureAvailable = true;
	}
	else
	{
		glConfig.multitextureAvailable = GLEW_ARB_multitexture != 0;
	}

	// GL_ARB_texture_compression + GL_S3_s3tc
	// DRI drivers may have GL_ARB_texture_compression but no GL_EXT_texture_compression_s3tc
	if( glConfig.driverType == GLDRV_OPENGL_MESA_CORE_PROFILE )
	{
		glConfig.textureCompressionAvailable = true;
	}
	else
	{
		glConfig.textureCompressionAvailable = GLEW_ARB_texture_compression != 0 && GLEW_EXT_texture_compression_s3tc != 0;
	}
	// GL_EXT_texture_filter_anisotropic
	glConfig.anisotropicFilterAvailable = (glConfig.glVersion > 4.5) ? GLEW_ARB_texture_filter_anisotropic != 0 : GLEW_EXT_texture_filter_anisotropic != 0;
#else
    glGetIntegerv(GL_MAX_SAMPLES, &glConfig.maxSupportedSamples);
	glConfig.hasMSAAEXT = glFramebufferTexture2DMultisampleEXT != nullptr;
    glConfig.textureCompressionAvailable = g_enableDXTSupport && IsDXTBCSupported();
    glConfig.multitextureAvailable = true;
	glConfig.anisotropicFilterAvailable = GLAD_GL_EXT_texture_filter_anisotropic;
#endif
	if( glConfig.anisotropicFilterAvailable )
	{
#ifndef ANDROID
		glGetFloatv((glConfig.glVersion > 4.5) ? GL_MAX_TEXTURE_MAX_ANISOTROPY : GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT, &glConfig.maxTextureAnisotropy );
#else
		glGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT, &glConfig.maxTextureAnisotropy );
#endif
		common->Printf( "   maxTextureAnisotropy: %f\n", glConfig.maxTextureAnisotropy );
	}
	else
	{
		glConfig.maxTextureAnisotropy = 1;
	}

	// GL_ARB_direct_state_access
	//GK: Use the core direct State Access intead (what purpose the EXT has?)
#ifndef ANDROID
	glConfig.directStateAccess = (r_useOpenGLDSA.GetInteger() < 0) ? GLEW_ARB_direct_state_access != 0 && glConfig.glVersion >= 4.5 : r_useOpenGLDSA.GetBool();
#else
	glConfig.directStateAccess = false;
#endif
	R_PrintExtensionStatus(glConfig.directStateAccess, "GL_ARB_direct_state_access");
	
	// GL_EXT_texture_lod_bias
	// The actual extension is broken as specificed, storing the state in the texture unit instead
	// of the texture object.  The behavior in GL 1.4 is the behavior we use.
#ifndef ANDROID
	glConfig.textureLODBiasAvailable = ( glConfig.glVersion >= 1.4 || GLEW_EXT_texture_lod_bias != 0 );
#else
	glConfig.textureLODBiasAvailable = false;
#endif
	R_PrintExtensionStatus(glConfig.textureLODBiasAvailable, "GL_EXT_texture_lod_bias");

#ifdef ANDROID //karin: force setup glConfig's extension support
    glConfig.vertexBufferObjectAvailable = true;
    glConfig.mapBufferRangeAvailable = true;
    glConfig.vertexArrayObjectAvailable = true;
    glConfig.drawElementsBaseVertexAvailable = GLAD_GL_EXT_draw_elements_base_vertex!=0;
    glConfig.uniformBufferAvailable = true;
    glConfig.gpuSkinningAvailable = true;
    glConfig.fragmentProgramAvailable = true;
#if !defined(USE_VULKAN)
    // GLAD defines generated symbols even for extensions the active driver lacks.
    g_multiDrawIndirectAvailable = false;
    R_PrintExtensionStatus(g_multiDrawIndirectAvailable, "GL_EXT_multi_draw_indirect");
#endif
#endif

	// GL_ARB_seamless_cube_map
#ifndef ANDROID
	glConfig.seamlessCubeMapAvailable = GLEW_ARB_seamless_cube_map != 0;
#else
	glConfig.seamlessCubeMapAvailable = false;
#endif
	R_PrintExtensionStatus(glConfig.seamlessCubeMapAvailable, "GL_ARB_seamless_cube_map");
	r_useSeamlessCubeMap.SetModified();		// the CheckCvars() next frame will enable / disable it
	
	// GL_ARB_framebuffer_sRGB
#ifndef ANDROID
	glConfig.sRGBFramebufferAvailable = GLEW_ARB_framebuffer_sRGB != 0;
#else
	glConfig.sRGBFramebufferAvailable = false;
#endif
	r_useSRGB.SetModified();		// the CheckCvars() next frame will enable / disable it

#ifndef ANDROID
	// GL_ARB_vertex_buffer_object
	if( glConfig.driverType == GLDRV_OPENGL_MESA_CORE_PROFILE )
	{
		glConfig.vertexBufferObjectAvailable = true;
	}
	else
	{
		glConfig.vertexBufferObjectAvailable = GLEW_ARB_vertex_buffer_object != 0;
	}
#endif
	
	// GL_ARB_map_buffer_range, map a section of a buffer object's data store
	//if( glConfig.driverType == GLDRV_OPENGL_MESA_CORE_PROFILE )
	//{
	//    glConfig.mapBufferRangeAvailable = true;
	//}
	//else
	{
#ifndef ANDROID
		glConfig.mapBufferRangeAvailable = GLEW_ARB_map_buffer_range != 0;
#endif
	}
	
	// GL_ARB_vertex_array_object
	//if( glConfig.driverType == GLDRV_OPENGL_MESA_CORE_PROFILE )
	//{
	//    glConfig.vertexArrayObjectAvailable = true;
	//}
	//else
#ifndef ANDROID
	{
		glConfig.vertexArrayObjectAvailable = GLEW_ARB_vertex_array_object != 0;
	}
	
	// GL_ARB_draw_elements_base_vertex
	glConfig.drawElementsBaseVertexAvailable = GLEW_ARB_draw_elements_base_vertex != 0;
	
	// GL_ARB_vertex_program / GL_ARB_fragment_program
	glConfig.fragmentProgramAvailable = GLEW_ARB_fragment_program != 0;
#endif
//	if( glConfig.fragmentProgramAvailable )
//	{
		//glGetInteger64v( GL_MAX_TEXTURE_COORDS, ( GLint64* )&glConfig.maxTextureCoords ); //DEPRECATED
		glGetIntegerv( GL_MAX_TEXTURE_IMAGE_UNITS, ( GLint* )&glConfig.maxTextureImageUnits );
//	}

    // GLSL, core in OpenGL > 2.0
	glConfig.glslAvailable = ( glConfig.glVersion >= 2.0f );
	
	// GL_ARB_uniform_buffer_object
#ifndef ANDROID
	glConfig.uniformBufferAvailable = GLEW_ARB_uniform_buffer_object != 0;
#endif
	if( glConfig.uniformBufferAvailable )
	{
		glGetIntegerv( GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT, ( GLint* )&glConfig.uniformBufferOffsetAlignment );
		if( glConfig.uniformBufferOffsetAlignment < 256 )
		{
			glConfig.uniformBufferOffsetAlignment = 256;
		}
	}
#ifndef ANDROID
	// RB: make GPU skinning optional for weak OpenGL drivers
	glConfig.gpuSkinningAvailable = glConfig.uniformBufferAvailable && ( glConfig.driverType == GLDRV_OPENGL3X || glConfig.driverType == GLDRV_OPENGL32_CORE_PROFILE || glConfig.driverType == GLDRV_OPENGL32_COMPATIBILITY_PROFILE );
#endif
	// ATI_separate_stencil / OpenGL 2.0 separate stencil
#ifndef ANDROID
	glConfig.twoSidedStencilAvailable = ( glConfig.glVersion >= 2.0f ) || GLEW_ATI_separate_stencil != 0;
	// GL_EXT_depth_bounds_test
	glConfig.depthBoundsTestAvailable = GLEW_EXT_depth_bounds_test != 0;
#else
	glConfig.twoSidedStencilAvailable = false;
	glConfig.depthBoundsTestAvailable = false;
#endif

	// GL_ARB_sync
#ifndef ANDROID
	glConfig.syncAvailable = GLEW_ARB_sync &&
							 // as of 5/24/2012 (driver version 15.26.12.64.2761) sync objects
							 // do not appear to work for the Intel HD 4000 graphics
							 ( glConfig.vendor != VENDOR_INTEL || r_skipIntelWorkarounds.GetBool() );
							 
	// GL_ARB_occlusion_query
	glConfig.occlusionQueryAvailable = GLEW_ARB_occlusion_query != 0;

	// GL_ARB_timer_query
	glConfig.timerQueryAvailable = ( GLEW_ARB_timer_query != 0 || GLEW_EXT_timer_query != 0 ) && ( glConfig.vendor != VENDOR_INTEL || r_skipIntelWorkarounds.GetBool() ) && glConfig.driverType != GLDRV_OPENGL_MESA;
	
	// GREMEDY_string_marker
	glConfig.gremedyStringMarkerAvailable = GLEW_GREMEDY_string_marker != 0;
#else
	glConfig.occlusionQueryAvailable = false;
	glConfig.timerQueryAvailable = false;
	glConfig.gremedyStringMarkerAvailable = false;
#endif
	R_PrintExtensionStatus(glConfig.gremedyStringMarkerAvailable, "GL_GREMEDY_string_marker");
	
	// GL_ARB_framebuffer_object
#ifndef ANDROID
	glConfig.framebufferObjectAvailable = GLEW_ARB_framebuffer_object != 0;
#else
    glConfig.framebufferObjectAvailable = true;
    glConfig.syncAvailable = true;
#endif

	R_PrintExtensionStatus(glConfig.framebufferObjectAvailable, "GL_ARB_framebuffer_object");
	if( glConfig.framebufferObjectAvailable )
	{
		glGetIntegerv( GL_MAX_RENDERBUFFER_SIZE, &glConfig.maxRenderbufferSize );
		glGetIntegerv( GL_MAX_COLOR_ATTACHMENTS, &glConfig.maxColorAttachments );
	}
	
	// GL_EXT_framebuffer_blit
#ifndef ANDROID
	glConfig.framebufferBlitAvailable = GLEW_EXT_framebuffer_blit != 0;
#else
	glConfig.framebufferBlitAvailable = true;
#endif
	R_PrintExtensionStatus(glConfig.framebufferBlitAvailable, "GL_EXT_framebuffer_blit");
	
	// GL_ARB_debug_output
#ifndef ANDROID
	glConfig.debugOutputAvailable = GLEW_ARB_debug_output != 0 && glConfig.glVersion >= 4.3;
	if( glConfig.debugOutputAvailable )
	{
		if( r_debugContext.GetInteger() >= 1 )
		{
			if (r_debugContext.GetInteger() < 2) {
				glEnable(GL_DEBUG_OUTPUT);
			}
			glDebugMessageCallbackARB( ( GLDEBUGPROCARB ) DebugCallback, NULL );
		}
		if( r_debugContext.GetInteger() >= 2 )
		{
			// force everything to happen in the main thread instead of in a separate driver thread
			glEnable( GL_DEBUG_OUTPUT_SYNCHRONOUS_ARB );
		}
		if( r_debugContext.GetInteger() >= 3 )
		{
			// enable all the low priority messages
			glDebugMessageControlARB( GL_DONT_CARE,
									  GL_DONT_CARE,
									  GL_DONT_CARE,
									  0, NULL, true );
		}
	}
	// GL_ARB_multitexture
	if( !glConfig.multitextureAvailable )
	{
		idLib::Error( "GL_ARB_multitexture not available" );
	}
	// GL_ARB_texture_compression + GL_EXT_texture_compression_s3tc
	if( !glConfig.textureCompressionAvailable )
	{
		idLib::Error( "GL_ARB_texture_compression or GL_EXT_texture_compression_s3tc not available" );
	}
	// GL_ARB_vertex_buffer_object
	if( !glConfig.vertexBufferObjectAvailable )
	{
		idLib::Error( "GL_ARB_vertex_buffer_object not available" );
	}
	// GL_ARB_map_buffer_range
	if( !glConfig.mapBufferRangeAvailable )
	{
		idLib::Error( "GL_ARB_map_buffer_range not available" );
	}
	// GL_ARB_vertex_array_object
	if( !glConfig.vertexArrayObjectAvailable )
	{
		idLib::Error( "GL_ARB_vertex_array_object not available" );
	}
	// GL_ARB_draw_elements_base_vertex
	if( !glConfig.drawElementsBaseVertexAvailable )
	{
		idLib::Error( "GL_ARB_draw_elements_base_vertex not available" );
	}
	// GL_ARB_vertex_program / GL_ARB_fragment_program
	//if( !glConfig.fragmentProgramAvailable )
	//{
	//	idLib::Warning( "GL_ARB_fragment_program not available" );
	//}
	// GLSL
	if( !glConfig.glslAvailable )
	{
		idLib::Error( "GLSL not available" );
	}
	// GL_ARB_uniform_buffer_object
	if( !glConfig.uniformBufferAvailable )
	{
		idLib::Error( "GL_ARB_uniform_buffer_object not available" );
	}
	// GL_EXT_stencil_two_side
	if( !glConfig.twoSidedStencilAvailable )
	{
		idLib::Error( "GL_ATI_separate_stencil not available" );
	}

	// generate one global Vertex Array Object (VAO)
	if (glConfig.directStateAccess) {
		glCreateVertexArrays(1, &glConfig.global_vao);
	}
	else
#else
	glConfig.debugOutputAvailable = false;
#endif
	{
		glGenVertexArrays(1, &glConfig.global_vao);
		glBindVertexArray(glConfig.global_vao);
	}
}
// RB end

idStr extensions_string;
/*
==================
R_InitOpenGL

This function is responsible for initializing a valid OpenGL subsystem
for rendering.  This is done by calling the system specific GLimp_Init,
which gives us a working OGL subsystem, then setting all necessary openGL
state, including images, vertex programs, and display lists.

Changes to the vertex cache size or smp state require a vid_restart.

If R_IsInitialized() is false, no rendering can take place, but
all renderSystem functions will still operate properly, notably the material
and model information functions.
==================
*/

void idRenderBackend::Init()
{
	common->Printf( "----- R_InitOpenGL -----\n" );
	
	if( tr.IsInitialized() )
	{
		common->FatalError( "R_InitOpenGL called while active" );
	}
	
	// DG: make sure SDL has setup video so getting supported modes in R_SetNewMode() works
	GLimp_PreInit();
	// DG end
	
	R_SetNewMode( true );
	
	// input and sound systems need to be tied to the new window
	Sys_InitInput();
	
	glConfig.forceGLSLGeneration = false;
	// get our config strings
	glConfig.vendor_string = ( const char* )glGetString( GL_VENDOR );
	glConfig.renderer_string = ( const char* )glGetString( GL_RENDERER );
	glConfig.version_string = ( const char* )glGetString( GL_VERSION );
	glConfig.shading_language_string = ( const char* )glGetString( GL_SHADING_LANGUAGE_VERSION );
	//glConfig.extensions_string = ( const char* )glGetString( GL_EXTENSIONS );
	//GL_CheckErrors();

#ifdef ANDROID //karin: force setup OpenGLES3.2
    glConfig.version_string = "320 es";
	glConfig.shading_language_string = "320";
#endif

	float glVersion = atof( idStr(glConfig.version_string).SubStr(0, 3) );
	float glslVersion = atof( glConfig.shading_language_string );

	idLib::Printf( "OpenGL Version   : %1.1f\n", glVersion );
	idLib::Printf( "OpenGL Vendor    : %s\n", glConfig.vendor_string );
	idLib::Printf( "OpenGL Renderer  : %s\n", glConfig.renderer_string );
	idLib::Printf( "OpenGL GLSL      : %1.1f\n", glslVersion );
	if (r_showGLExt.GetBool()) {
		idLib::Printf("OpenGL Extensions: ");
		//GK: The number of extensions is ridiculusly long the idLib::Print can't output all of it
		// therefor print it one by one.

		// As of OpenGL 3.2, glGetStringi is required to obtain the available extensions
		//glGetStringi = ( PFNGLGETSTRINGIPROC )GLimp_ExtensionPointer( "glGetStringi" );

		// Build the extensions string
		GLint numExtensions;
		glGetIntegerv(GL_NUM_EXTENSIONS, &numExtensions);
		//extensions_string.Clear();
		for (int i = 0; i < numExtensions; i++)
		{
			//extensions_string.Append((const char*)glGetStringi(GL_EXTENSIONS, i));
			idLib::Printf("%s", (const char*)glGetStringi(GL_EXTENSIONS, i));
			// the now deprecated glGetString method usaed to create a single string with each extension separated by a space
			if (i < numExtensions - 1)
			{
				idLib::Printf(" ");
				//extensions_string.Append(' ');
			}
		}
		idLib::Printf("\n");
	}
	if (glslVersion < 3.0f) { //GK: GLSL Version HACK. After GLSL 1.50 (GL 3.2) GLSL and GL Versions come in sync so there is no GLSL 3.00 but there is 3.00 es
		idLib::FatalError("System doesn't support minimum required OpenGL Version 3.3 or OpenGL ES 3.0");
	}
	if (r_oldGLSLVersion.GetFloat() != glslVersion) {
		idFileList* listOfGLSLProgs = fileSystem->ListFilesTree("renderprogs/glsl", "*");
		for (int i = 0; i < listOfGLSLProgs->GetNumFiles(); i++) {
			fileSystem->RemoveFile(listOfGLSLProgs->GetFile(i));
		}
	}
	r_oldGLSLVersion.SetFloat(glslVersion);
	// OpenGL driver constants
	GLint temp;
	glGetIntegerv( GL_MAX_TEXTURE_SIZE, &temp );
	glConfig.maxTextureSize = temp;

	
	
	// stubbed or broken drivers may have reported 0...
	if( glConfig.maxTextureSize <= 0 )
	{
		glConfig.maxTextureSize = 256;
	}
	
	// recheck all the extensions (FIXME: this might be dangerous)
	R_CheckPortableExtensions();

	//GL_CheckErrors();
	
	renderProgManager.Init();

#if defined(ANDROID) && !defined(USE_VULKAN)
	// Prepare asynchronous HDR exposure readback after the GLES context exists.
	RB_InitExposureReadback();
#endif

	//GL_CheckErrors();
	
	tr.SetInitialized();

	//GL_CheckErrors();
	
	// allocate the vertex array range or vertex objects
	vertexCache.Init( glConfig.uniformBufferOffsetAlignment );
	
	// allocate the frame data, which may be more if smp is enabled
	R_InitFrameData();
	cmdBufferPool.Init(24 * 1024 * 1024);
	currentCmdBuffer = cmdBufferPool.GetWriteBuffer();
	frameData->cmdHead = (emptyCommand_t *) currentCmdBuffer->Alloc(sizeof(emptyCommand_t));
	// Reset our gamma
	R_SetColorMappings();
#ifndef _WIN32
	//GK: I don't know why SDL2 on linux does this,
	// but apparently the engine has to re-set the mode (without initialization) after the rest of the rendering initialization proccess is done
	R_SetNewMode( false );
#endif
}

void idRenderBackend::Shutdown()
{
	cmdBufferPool.Shutdown();
#if defined(ANDROID) && !defined(USE_VULKAN)
    FlushMultiDrawBatch();
    if (gpuInstanceBuffer != 0)
    {
        GLuint buffer = (GLuint)gpuInstanceBuffer;
        glDeleteBuffers(1, &buffer);
        gpuInstanceBuffer = 0;
    }
    if (g_multiDrawIndirectBuffer != 0)
    {
        glDeleteBuffers(1, &g_multiDrawIndirectBuffer);
        g_multiDrawIndirectBuffer = 0;
    }
    g_multiDrawIndirectAvailable = false;
    g_multiDrawCommands.clear();
	// Delete PBOs and fences while the GLES context is still alive.
	RB_ShutdownExposureReadback();
	// the same for the streaming rings, they must not outlive the context in static destructors
	RB_ShutdownWorldMerge();
	R_ShutdownInstanceStream();
	// Framebuffer::Shutdown deleted hdrFBO already, only the renderbuffers are left
	deferredGBufferAttached = false;
	DeferredReleaseTargets();
#endif
	GLimp_Shutdown();
}

#if defined(ANDROID) && !defined(USE_VULKAN)
/*
==============================================================================================

TBDR: ON-TILE DEFERRED SHADING, the render targets

The G-buffer is colors 1..3 of hdrFBO next to the scene color, all of the view in one render
pass: it is cleared when the view starts and invalidated when the lights are done, so a tile
based GPU never writes it to memory or reads it back. Renderbuffers, nothing samples it.

==============================================================================================
*/

static const GLenum deferredGBufferAttachments[3] = { GL_COLOR_ATTACHMENT1, GL_COLOR_ATTACHMENT2, GL_COLOR_ATTACHMENT3 };

/*
=============================
idRenderBackend::DeferredReleaseTargets

hdrFBO back to the scene color alone
=============================
*/
void idRenderBackend::DeferredReleaseTargets()
{
	// a vid_restart makes a new hdrFBO, it has nothing attached
	if( deferredGBufferAttached && globalFramebuffers.hdrFBO != NULL && globalFramebuffers.hdrFBO->GetFramebuffer() == deferredAttachedFBO )
	{
		globalFramebuffers.hdrFBO->Bind();
		for( int i = 0; i < 3; i++ )
		{
			glFramebufferRenderbuffer( GL_FRAMEBUFFER, deferredGBufferAttachments[i], GL_RENDERBUFFER, 0 );
		}
		const GLenum drawBuffers[1] = { GL_COLOR_ATTACHMENT0 };
		glDrawBuffers( 1, drawBuffers );
		Framebuffer::Unbind();

		// GL_State sets the mask of all colors again
		glColorMask( ( glStateBits & GLS_REDMASK ) ? GL_FALSE : GL_TRUE, ( glStateBits & GLS_GREENMASK ) ? GL_FALSE : GL_TRUE,
					 ( glStateBits & GLS_BLUEMASK ) ? GL_FALSE : GL_TRUE, ( glStateBits & GLS_ALPHAMASK ) ? GL_FALSE : GL_TRUE );
	}
	deferredGBufferAttached = false;
	deferredAttachedColor = 0;
	deferredAttachedFBO = 0;

	if( deferredRenderbuffers[0] != 0 )
	{
		const GLuint renderbuffers[3] = { deferredRenderbuffers[0], deferredRenderbuffers[1], deferredRenderbuffers[2] };
		glDeleteRenderbuffers( 3, renderbuffers );
		deferredRenderbuffers[0] = deferredRenderbuffers[1] = deferredRenderbuffers[2] = 0;
	}
	deferredTargetWidth = 0;
	deferredTargetHeight = 0;
}

/*
=============================
idRenderBackend::DeferredUpdateTargets

The G-buffer as big as the scene color, attached to hdrFBO. False when hdrFBO can't take it.
=============================
*/
bool idRenderBackend::DeferredUpdateTargets()
{
	idImage* colorImage = globalImages->currentRenderHDRImage;
	if( globalFramebuffers.hdrFBO == NULL || colorImage == NULL || !colorImage->IsLoaded() || colorImage->texnum == 0xFFFFFFFF )
	{
		return false;
	}

	const int width = colorImage->GetUploadWidth();
	const int height = colorImage->GetUploadHeight();
	if( width <= 0 || height <= 0 )
	{
		return false;
	}

	// the scene color is resized or reallocated with the screen, a vid_restart makes a new hdrFBO
	if( deferredGBufferAttached && width == deferredTargetWidth && height == deferredTargetHeight && colorImage->texnum == deferredAttachedColor
			&& globalFramebuffers.hdrFBO->GetFramebuffer() == deferredAttachedFBO )
	{
		return true;
	}
	if( deferredGBufferAttached && globalFramebuffers.hdrFBO->GetFramebuffer() != deferredAttachedFBO )
	{
		// the old hdrFBO is gone with its attachments
		deferredGBufferAttached = false;
	}

	if( width != deferredTargetWidth || height != deferredTargetHeight || deferredRenderbuffers[0] == 0 )
	{
		DeferredReleaseTargets();

		GLuint renderbuffers[3] = { 0, 0, 0 };
		glGenRenderbuffers( 3, renderbuffers );
		for( int i = 0; i < 3; i++ )
		{
			glBindRenderbuffer( GL_RENDERBUFFER, renderbuffers[i] );
			glRenderbufferStorage( GL_RENDERBUFFER, GL_RGBA8, width, height );
			deferredRenderbuffers[i] = renderbuffers[i];
		}
		glBindRenderbuffer( GL_RENDERBUFFER, 0 );
		deferredTargetWidth = width;
		deferredTargetHeight = height;
	}

	globalFramebuffers.hdrFBO->Bind();
	for( int i = 0; i < 3; i++ )
	{
		glFramebufferRenderbuffer( GL_FRAMEBUFFER, deferredGBufferAttachments[i], GL_RENDERBUFFER, deferredRenderbuffers[i] );
	}
	const GLenum drawBuffers[4] = { GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1, GL_COLOR_ATTACHMENT2, GL_COLOR_ATTACHMENT3 };
	glDrawBuffers( 4, drawBuffers );

	const GLenum status = glCheckFramebufferStatus( GL_FRAMEBUFFER );
	deferredGBufferAttached = true;
	deferredAttachedColor = colorImage->texnum;
	deferredAttachedFBO = globalFramebuffers.hdrFBO->GetFramebuffer();
	if( status != GL_FRAMEBUFFER_COMPLETE )
	{
		common->Printf( "on-tile deferred shading: the G-buffer makes the scene target incomplete (0x%X), off\n", status );
		DeferredReleaseTargets();
		return false;
	}

	// nothing but the deferred shading writes the G-buffer
	DeferredSetGBufferWrites( false );
	Framebuffer::Unbind();

	common->Printf( "on-tile deferred shading: G-buffer %i x %i\n", width, height );
	return true;
}

/*
=============================
idRenderBackend::DeferredSetGBufferWrites

The masks of colors 1..3, GL_State keeps color 0 only while the G-buffer is attached
=============================
*/
void idRenderBackend::DeferredSetGBufferWrites( bool enable )
{
	if( !deferredGBufferAttached )
	{
		return;
	}
	FlushMultiDrawBatch();
	const GLboolean mask = enable ? GL_TRUE : GL_FALSE;
	for( int i = 1; i <= 3; i++ )
	{
		glColorMaski( i, mask, mask, mask, mask );
	}
}

/*
=============================
idRenderBackend::DeferredClearGBuffer

At the start of a view's render pass on hdrFBO, with the whole target in the scissor: the G-buffer is
zero where no surface writes it, which no light lights, and a tile based GPU loads nothing for it
=============================
*/
void idRenderBackend::DeferredClearGBuffer()
{
	if( !deferredGBufferAttached )
	{
		return;
	}
	FlushMultiDrawBatch();

	static const GLfloat zero[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
	DeferredSetGBufferWrites( true );
	for( int i = 1; i <= 3; i++ )
	{
		glClearBufferfv( GL_COLOR, i, zero );
	}
	DeferredSetGBufferWrites( false );

	// glClearBuffer keeps the mask of color 0, GL_EndFrame may have left all colors writable
	glColorMaski( 0, ( glStateBits & GLS_REDMASK ) ? GL_FALSE : GL_TRUE, ( glStateBits & GLS_GREENMASK ) ? GL_FALSE : GL_TRUE,
				  ( glStateBits & GLS_BLUEMASK ) ? GL_FALSE : GL_TRUE, ( glStateBits & GLS_ALPHAMASK ) ? GL_FALSE : GL_TRUE );
}

/*
=============================
idRenderBackend::DeferredInvalidateGBuffer

Once the lights are done: whatever ends the render pass later doesn't write the G-buffer out
=============================
*/
void idRenderBackend::DeferredInvalidateGBuffer()
{
	if( !deferredGBufferAttached || !globalFramebuffers.hdrFBO->IsBound() )
	{
		return;
	}
	FlushMultiDrawBatch();
	DeferredSetGBufferWrites( false );
	globalFramebuffers.hdrFBO->InvalidateAttachments( deferredGBufferAttachments, 3 );
}

/*
=============================
idRenderBackend::DeferredSelfTest

Some drivers list GL_EXT_shader_framebuffer_fetch but don't fetch every color attachment right.
Writes four colors, fetches them all and reads the result back, once.
=============================
*/
bool idRenderBackend::DeferredSelfTest()
{
	if( deferredSelfTestResult != 0 )
	{
		return deferredSelfTestResult > 0;
	}
	deferredSelfTestResult = -1;

	if( !renderProgManager.DeferredShadersReady() )
	{
		common->Printf( "on-tile deferred shading: needs GLES 3.2 with GL_EXT_shader_framebuffer_fetch, off\n" );
		return false;
	}

	static const int TEST_SIZE = 8;

	FlushMultiDrawBatch();

	// a framebuffer of its own, bound around Framebuffer's back: the default framebuffer is bound before and after
	Framebuffer::Unbind();
	GLuint testFramebuffer = 0;
	glGenFramebuffers( 1, &testFramebuffer );
	glBindFramebuffer( GL_FRAMEBUFFER, testFramebuffer );

	GLuint renderbuffers[4] = { 0, 0, 0, 0 };
	glGenRenderbuffers( 4, renderbuffers );
	for( int i = 0; i < 4; i++ )
	{
		glBindRenderbuffer( GL_RENDERBUFFER, renderbuffers[i] );
		glRenderbufferStorage( GL_RENDERBUFFER, GL_RGBA8, TEST_SIZE, TEST_SIZE );
		glFramebufferRenderbuffer( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0 + i, GL_RENDERBUFFER, renderbuffers[i] );
	}
	glBindRenderbuffer( GL_RENDERBUFFER, 0 );
	const GLenum drawBuffers[4] = { GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1, GL_COLOR_ATTACHMENT2, GL_COLOR_ATTACHMENT3 };
	glDrawBuffers( 4, drawBuffers );

	// what _deferred_screen_testfetch makes of what _deferred_screen_testwrite wrote:
	// 0.2 + 0.3, 0.5 + 0.25, 0.7 - 0.2, 1
	static const int expected[4] = { 128, 191, 128, 255 };

	byte pixels[TEST_SIZE * TEST_SIZE * 4];
	memset( pixels, 0, sizeof( pixels ) );

	// earlier errors must not fail the test
	for( int i = 0; i < 16 && glGetError() != GL_NO_ERROR; i++ )
	{
	}

	bool passed = false;
	if( glCheckFramebufferStatus( GL_FRAMEBUFFER ) == GL_FRAMEBUFFER_COMPLETE )
	{
		glBindVertexArray( glConfig.global_vao );
		GL_State( GLS_SRCBLEND_ONE | GLS_DSTBLEND_ZERO | GLS_DEPTHMASK | GLS_DEPTHFUNC_ALWAYS | GLS_CULL_TWOSIDED, true );
		GL_ViewportAndScissor( 0, 0, TEST_SIZE, TEST_SIZE );
		glColorMask( GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE );

		renderProgManager.BindShader_DeferredTestWrite();
		DrawElementsWithCounters( &unitSquareSurface );

		// only color 0 is written, the fetch of the others must still work
		for( int i = 1; i < 4; i++ )
		{
			glColorMaski( i, GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE );
		}
		renderProgManager.BindShader_DeferredTestFetch();
		DrawElementsWithCounters( &unitSquareSurface );
		FlushMultiDrawBatch();

		glReadBuffer( GL_COLOR_ATTACHMENT0 );
		glReadPixels( 0, 0, TEST_SIZE, TEST_SIZE, GL_RGBA, GL_UNSIGNED_BYTE, pixels );

		passed = ( glGetError() == GL_NO_ERROR );
		for( int i = 0; i < TEST_SIZE * TEST_SIZE * 4 && passed; i++ )
		{
			const int difference = ( int )pixels[i] - expected[i & 3];
			passed = ( difference >= -3 && difference <= 3 );
		}

		glColorMask( GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE );
		renderProgManager.Unbind();
	}

	glBindFramebuffer( GL_FRAMEBUFFER, 0 );
	glDeleteFramebuffers( 1, &testFramebuffer );
	glDeleteRenderbuffers( 4, renderbuffers );

	// GL_State and the masks back to what the frame expects
	GL_State( GLS_DEFAULT, true );

	common->Printf( "on-tile deferred shading self test %s (%d %d %d %d)\n", passed ? "passed" : "failed, off",
					pixels[0], pixels[1], pixels[2], pixels[3] );
	deferredSelfTestResult = passed ? 1 : -1;
	return passed;
}
#endif

#if defined(ANDROID) && !defined(USE_VULKAN)
/*
=============================
idRenderBackend::FlushMultiDrawBatch
=============================
*/
void idRenderBackend::FlushMultiDrawBatch()
{
    const size_t commandCount = g_multiDrawCommands.size();
    if (commandCount == 0)
    {
        return;
    }

    if (commandCount == 1 || !g_multiDrawIndirectAvailable
        || glMultiDrawElementsIndirectEXT == NULL)
    {
        // A one-command batch is cheaper as a normal draw; retain the safe fallback.
        for (size_t i = 0; i < commandCount; ++i)
        {
            const drawElementsIndirectCommand_t& command = g_multiDrawCommands[i];
            glDrawElementsBaseVertex(GL_TRIANGLES, (GLsizei)command.count, GL_INDEX_TYPE,
                (const void*)(uintptr_t)(command.firstIndex * sizeof(triIndex_t)), command.baseVertex);
        }
    }
    else
    {
        if (g_multiDrawIndirectBuffer == 0)
        {
            glGenBuffers(1, &g_multiDrawIndirectBuffer);
        }
        glBindBuffer(GL_DRAW_INDIRECT_BUFFER, g_multiDrawIndirectBuffer);
        // Orphan old storage so queued commands don't overwrite data the GPU is reading.
        glBufferData(GL_DRAW_INDIRECT_BUFFER,
            (GLsizeiptr)(commandCount * sizeof(drawElementsIndirectCommand_t)),
            g_multiDrawCommands.data(), GL_STREAM_DRAW);
        glMultiDrawElementsIndirectEXT(GL_TRIANGLES, GL_INDEX_TYPE, NULL,
            (GLsizei)commandCount, 0);

        ++g_multiDrawIndirectBatchCount;
        g_multiDrawIndirectDrawCallsSaved += (int)commandCount - 1;
        g_multiDrawIndirectSubmittedCommands += (int)commandCount;
    }

    g_multiDrawCommands.clear();
    g_multiDrawVertexBuffer = 0;
    g_multiDrawIndexBuffer = 0;
    g_multiDrawBatchLayout = MDI_LAYOUT_NONE;
}
#endif

/*
=============
idRenderBackend::DrawElementsWithCounters
=============
*/
#if defined(ANDROID) && !defined(USE_VULKAN)
/*
=============
idRenderBackend::DrawWorldMergeRange

Draws merged world surfaces: 32 bit indexes from this frame's merge buffer
into the static vertex buffer, with whatever program and state is set up
=============
*/
void idRenderBackend::DrawWorldMergeRange( int firstIndex, int numIndexes )
{
	if( numIndexes <= 0 || worldMergeIndexBuffer == 0 )
	{
		return;
	}

	FlushMultiDrawBatch();

	idVertexBuffer* vertexBuffer = &vertexCache.staticData.vertexBuffer;

	renderProgManager.CommitUniforms( glStateBits );

	if( ( GLintptr )currentIndexBuffer != ( GLintptr )worldMergeIndexBuffer || !r_useStateCaching.GetBool() )
	{
		glBindBuffer( GL_ELEMENT_ARRAY_BUFFER, worldMergeIndexBuffer );
		currentIndexBuffer = worldMergeIndexBuffer;
	}

	// the indexes count from the first vertex of the static buffer
	if( ( vertexLayout != LAYOUT_DRAW_VERT ) || ( ( GLintptr )currentVertexBuffer != ( GLintptr )vertexBuffer->GetAPIObject() ) ||
			!r_useStateCaching.GetBool() || ( !isGLES32Version() && currentVertOffset != 0 ) )
	{
		glBindBuffer( GL_ARRAY_BUFFER, ( GLintptr )vertexBuffer->GetAPIObject() );
		currentVertexBuffer = ( GLintptr )vertexBuffer->GetAPIObject();

		glEnableVertexAttribArray( PC_ATTRIB_INDEX_VERTEX );
		glEnableVertexAttribArray( PC_ATTRIB_INDEX_NORMAL );
		glEnableVertexAttribArray( PC_ATTRIB_INDEX_COLOR );
		glEnableVertexAttribArray( PC_ATTRIB_INDEX_COLOR2 );
		glEnableVertexAttribArray( PC_ATTRIB_INDEX_ST );
		glEnableVertexAttribArray( PC_ATTRIB_INDEX_TANGENT );

		glVertexAttribPointer( PC_ATTRIB_INDEX_VERTEX, 3, GL_FLOAT, GL_FALSE, sizeof( idDrawVert ), ( void* )( DRAWVERT_XYZ_OFFSET ) );
		glVertexAttribPointer( PC_ATTRIB_INDEX_NORMAL, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof( idDrawVert ), ( void* )( DRAWVERT_NORMAL_OFFSET ) );
		glVertexAttribPointer( PC_ATTRIB_INDEX_COLOR, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof( idDrawVert ), ( void* )( DRAWVERT_COLOR_OFFSET ) );
		glVertexAttribPointer( PC_ATTRIB_INDEX_COLOR2, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof( idDrawVert ), ( void* )( DRAWVERT_COLOR2_OFFSET ) );
		glVertexAttribPointer( PC_ATTRIB_INDEX_ST, 2, GL_HALF_FLOAT, GL_TRUE, sizeof( idDrawVert ), ( void* )( DRAWVERT_ST_OFFSET ) );
		glVertexAttribPointer( PC_ATTRIB_INDEX_TANGENT, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof( idDrawVert ), ( void* )( DRAWVERT_TANGENT_OFFSET ) );

		vertexLayout = LAYOUT_DRAW_VERT;
		if( !isGLES32Version() )
		{
			currentVertOffset = 0;
		}
	}

	glDrawElements( GL_TRIANGLES, numIndexes, GL_UNSIGNED_INT, ( const void* )( ( uintptr_t )firstIndex * sizeof( uint32 ) ) );

	pc.c_drawElements++;
	pc.c_drawIndexes += numIndexes;
}

/*
=============
R_StreamInstanceData

Per instance data goes into a persistently mapped ring for the frame, where the
GPU is done with the slot, instead of a new glBufferData allocation per draw.
=============
*/
static idGLBufferRing*	s_instanceRing = NULL;		// destroyed in idRenderBackend::Shutdown
static int				s_instanceRingSlot = -1;
static GLsizeiptr		s_instanceRingUsed = 0;
static const GLsizeiptr	INSTANCE_RING_SIZE = 1024 * 1024;

static bool R_StreamInstanceData( const void* data, GLsizeiptr bytes, GLuint& buffer, GLintptr& offset )
{
	if( !R_HasBufferStorage() || bytes > INSTANCE_RING_SIZE )
	{
		return false;
	}
	if( s_instanceRing == NULL )
	{
		s_instanceRing = new idGLBufferRing;
		if( !s_instanceRing->Create( GL_COPY_WRITE_BUFFER, INSTANCE_RING_SIZE, true ) || !s_instanceRing->IsPersistent() )
		{
			s_instanceRing->Destroy();
			// keep the object, so the check above fails fast from now on
			return false;
		}
	}
	if( !s_instanceRing->IsPersistent() )
	{
		return false;
	}

	const int slot = g_frameFences.CurrentSlot();
	if( slot != s_instanceRingSlot )
	{
		s_instanceRingSlot = slot;
		s_instanceRingUsed = 0;
	}
	if( s_instanceRingUsed + bytes > INSTANCE_RING_SIZE )
	{
		return false;
	}

	byte* mapped = ( byte* )s_instanceRing->PersistentPointer( slot );
	if( mapped == NULL )
	{
		return false;
	}
	memcpy( mapped + s_instanceRingUsed, data, bytes );
	s_instanceRing->FlushRange( slot, s_instanceRingUsed, bytes );

	buffer = s_instanceRing->Buffer( slot );
	offset = s_instanceRingUsed;
	s_instanceRingUsed += ( bytes + 255 ) & ~( GLsizeiptr )255;
	return true;
}

static void R_ShutdownInstanceStream()
{
	if( s_instanceRing != NULL )
	{
		s_instanceRing->Destroy();
		delete s_instanceRing;
		s_instanceRing = NULL;
	}
	s_instanceRingSlot = -1;
	s_instanceRingUsed = 0;
}
#endif

#if defined(ANDROID) && !defined(USE_VULKAN)
void idRenderBackend::DrawElementsWithCounters( const drawSurf_t* surf, const gpuInstanceData_t* instances, int instanceCount )
#else
void idRenderBackend::DrawElementsWithCounters( const drawSurf_t* surf )
#endif
{
#if defined(ANDROID) && !defined(USE_VULKAN)
	// the first surface of a merged world group draws the whole group
	if( worldMergeSurf != NULL && surf == worldMergeSurf )
	{
		DrawWorldMergeRange( worldMergeFirstIndex, worldMergeNumIndexes );
		return;
	}

    const bool useInstancing = (instances != NULL && instanceCount > 1);
    const bool useMultiDraw = g_multiDrawIndirectAvailable
        && r_useMultiDrawIndirect.GetBool()
        && isGLES32Version()
        && !r_singleTriangle.GetBool()
        && !surf->jointCache
        && !useInstancing;
    if (!useMultiDraw)
    {
        FlushMultiDrawBatch();
    }
#endif
	// get vertex buffer
	const vertCacheHandle_t vbHandle = surf->ambientCache;
	idVertexBuffer* vertexBuffer;
	if( vertexCache.CacheIsStatic( vbHandle ) )
	{
		vertexBuffer = &vertexCache.staticData.vertexBuffer;
	}
	else
	{
		const uint64 frameNum = ( int )( vbHandle >> VERTCACHE_FRAME_SHIFT ) & VERTCACHE_FRAME_MASK;
		if( frameNum != ( ( vertexCache.currentFrame - 1 ) & VERTCACHE_FRAME_MASK ) )
		{
			idLib::Warning( "RB_DrawElementsWithCounters, vertexBuffer == NULL" );
			return;
		}
		vertexBuffer = &vertexCache.frameData[vertexCache.drawListNum].vertexBuffer;
	}
	const int vertOffset = ( int )( vbHandle >> VERTCACHE_OFFSET_SHIFT ) & VERTCACHE_OFFSET_MASK;
	
	// get index buffer
	const vertCacheHandle_t ibHandle = surf->indexCache;
	idIndexBuffer* indexBuffer;
	if( vertexCache.CacheIsStatic( ibHandle ) )
	{
		indexBuffer = &vertexCache.staticData.indexBuffer;
	}
	else
	{
		const uint64 frameNum = ( int )( ibHandle >> VERTCACHE_FRAME_SHIFT ) & VERTCACHE_FRAME_MASK;
		if( frameNum != ( ( vertexCache.currentFrame - 1 ) & VERTCACHE_FRAME_MASK ) )
		{
			idLib::Warning( "RB_DrawElementsWithCounters, indexBuffer == NULL" );
			return;
		}
		indexBuffer = &vertexCache.frameData[vertexCache.drawListNum].indexBuffer;
	}
	// RB: 64 bit fixes, changed int to GLintptr
	const GLintptr indexOffset = ( GLintptr )( ibHandle >> VERTCACHE_OFFSET_SHIFT ) & VERTCACHE_OFFSET_MASK;
	// RB end
#if defined(ANDROID) && !defined(USE_VULKAN)
    if (useMultiDraw && !g_multiDrawCommands.empty()
        && (g_multiDrawVertexBuffer != (GLuint)vertexBuffer->GetAPIObject()
            || g_multiDrawIndexBuffer != (GLuint)indexBuffer->GetAPIObject()
            || g_multiDrawBatchLayout != MDI_LAYOUT_DRAW_VERT
            || vertexLayout != LAYOUT_DRAW_VERT))
    {
        FlushMultiDrawBatch();
    }
#endif
	
	RENDERLOG_PRINTF( "Binding Buffers: %p:%i %p:%i\n", vertexBuffer, vertOffset, indexBuffer, indexOffset );
	
	if( surf->jointCache )
	{
		// DG: this happens all the time in the erebus1 map with blendlight.vfp,
		// so don't call assert (through verify) here until it's fixed (if fixable)
		// else the game crashes on linux when using debug builds
		
		// FIXME: fix this properly if possible?
		// RB: yes but it would require an additional blend light skinned shader
		//if( !verify( renderProgManager.ShaderUsesJoints() ) )
		if( !renderProgManager.ShaderUsesJoints() )
			// DG end
		{
			return;
		}
	}
	else
	{
		if( !verify( !renderProgManager.ShaderUsesJoints() || renderProgManager.ShaderHasOptionalSkinning() ) )
		{
			return;
		}
	}
	
	if (surf->jointCache)
	{
		idUniformBuffer jointBuffer;
		if (!vertexCache.GetJointBuffer(surf->jointCache, &jointBuffer))
		{
			idLib::Warning("RB_DrawElementsWithCounters, jointBuffer == NULL");
			return;
		}
		assert((jointBuffer.GetOffset() & (glConfig.uniformBufferOffsetAlignment - 1)) == 0);

		// RB: 64 bit fixes, changed GLuint to GLintptr
		const GLintptr ubo = jointBuffer.GetAPIObject();
		// RB end

		glBindBufferRange(GL_UNIFORM_BUFFER, 0, ubo, jointBuffer.GetOffset(), jointBuffer.GetSize());
	}

#if defined(ANDROID) && !defined(USE_VULKAN)
    // Uniform writes flush before changing values, so unchanged uniforms can be
    // uploaded once and shared by all commands in the pending batch.
    if (!(useMultiDraw && !g_multiDrawCommands.empty()))
    {
        renderProgManager.CommitUniforms(glStateBits);
    }
#else
	renderProgManager.CommitUniforms(glStateBits);
#endif
#ifndef ANDROID
	if (glConfig.directStateAccess) {		
		// RB: 64 bit fixes, changed GLuint to GLintptr
		if ((GLintptr)currentIndexBuffer != (GLintptr)indexBuffer->GetAPIObject() || !r_useStateCaching.GetBool())
		{
			glVertexArrayElementBuffer(glConfig.global_vao, indexBuffer->GetAPIObject());
			currentIndexBuffer = (GLintptr)indexBuffer->GetAPIObject();
		}

		if ((vertexLayout != LAYOUT_DRAW_VERT) || ((GLintptr)currentVertexBuffer != (GLintptr)vertexBuffer->GetAPIObject()) || !r_useStateCaching.GetBool())
		{
			glVertexArrayVertexBuffer(glConfig.global_vao, 0, vertexBuffer->GetAPIObject(), 0, sizeof(idDrawVert));
			currentVertexBuffer = (GLintptr)vertexBuffer->GetAPIObject();

			glEnableVertexArrayAttrib(glConfig.global_vao, PC_ATTRIB_INDEX_VERTEX);
			glEnableVertexArrayAttrib(glConfig.global_vao, PC_ATTRIB_INDEX_NORMAL);
			glEnableVertexArrayAttrib(glConfig.global_vao, PC_ATTRIB_INDEX_COLOR);
			glEnableVertexArrayAttrib(glConfig.global_vao, PC_ATTRIB_INDEX_COLOR2);
			glEnableVertexArrayAttrib(glConfig.global_vao, PC_ATTRIB_INDEX_ST);
			glEnableVertexArrayAttrib(glConfig.global_vao, PC_ATTRIB_INDEX_TANGENT);

			glVertexArrayAttribFormat(glConfig.global_vao, PC_ATTRIB_INDEX_VERTEX, 3, GL_FLOAT, GL_FALSE, DRAWVERT_XYZ_OFFSET);
			glVertexArrayAttribFormat(glConfig.global_vao, PC_ATTRIB_INDEX_NORMAL, 4, GL_UNSIGNED_BYTE, GL_TRUE, DRAWVERT_NORMAL_OFFSET);
			glVertexArrayAttribFormat(glConfig.global_vao, PC_ATTRIB_INDEX_COLOR, 4, GL_UNSIGNED_BYTE, GL_TRUE, DRAWVERT_COLOR_OFFSET);
			glVertexArrayAttribFormat(glConfig.global_vao, PC_ATTRIB_INDEX_COLOR2, 4, GL_UNSIGNED_BYTE, GL_TRUE, DRAWVERT_COLOR2_OFFSET);
			glVertexArrayAttribFormat(glConfig.global_vao, PC_ATTRIB_INDEX_ST, 2, GL_HALF_FLOAT, GL_TRUE, DRAWVERT_ST_OFFSET);
			glVertexArrayAttribFormat(glConfig.global_vao, PC_ATTRIB_INDEX_TANGENT, 4, GL_UNSIGNED_BYTE, GL_TRUE, DRAWVERT_TANGENT_OFFSET);

			glVertexArrayAttribBinding(glConfig.global_vao, PC_ATTRIB_INDEX_VERTEX, 0);
			glVertexArrayAttribBinding(glConfig.global_vao, PC_ATTRIB_INDEX_NORMAL, 0);
			glVertexArrayAttribBinding(glConfig.global_vao, PC_ATTRIB_INDEX_COLOR, 0);
			glVertexArrayAttribBinding(glConfig.global_vao, PC_ATTRIB_INDEX_COLOR2, 0);
			glVertexArrayAttribBinding(glConfig.global_vao, PC_ATTRIB_INDEX_ST, 0);
			glVertexArrayAttribBinding(glConfig.global_vao, PC_ATTRIB_INDEX_TANGENT, 0);

			vertexLayout = LAYOUT_DRAW_VERT;
		}
		glBindVertexArray(glConfig.global_vao);
	}
	else
#endif
	{
		// RB: 64 bit fixes, changed GLuint to GLintptr
		if ((GLintptr)currentIndexBuffer != (GLintptr)indexBuffer->GetAPIObject() ||
        !r_useStateCaching.GetBool() || (!isGLES32Version() && currentVertOffset != vertOffset))
		{
			glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, (GLintptr)indexBuffer->GetAPIObject());
			currentIndexBuffer = (GLintptr)indexBuffer->GetAPIObject();
		}

		if ((vertexLayout != LAYOUT_DRAW_VERT) || ((GLintptr)currentVertexBuffer != (GLintptr)vertexBuffer->GetAPIObject()) ||
        !r_useStateCaching.GetBool() || (!isGLES32Version() && currentVertOffset != vertOffset))
		{
            glBindBuffer(GL_ARRAY_BUFFER, (GLintptr)vertexBuffer->GetAPIObject());
			currentVertexBuffer = (GLintptr)vertexBuffer->GetAPIObject();

			glEnableVertexAttribArray(PC_ATTRIB_INDEX_VERTEX);
			glEnableVertexAttribArray(PC_ATTRIB_INDEX_NORMAL);
			glEnableVertexAttribArray(PC_ATTRIB_INDEX_COLOR);
			glEnableVertexAttribArray(PC_ATTRIB_INDEX_COLOR2);
			glEnableVertexAttribArray(PC_ATTRIB_INDEX_ST);
			glEnableVertexAttribArray(PC_ATTRIB_INDEX_TANGENT);

			if (!isGLES32Version()){
				glVertexAttribPointer(PC_ATTRIB_INDEX_VERTEX, 3, GL_FLOAT, GL_FALSE, sizeof(idDrawVert), (void*)(vertOffset + DRAWVERT_XYZ_OFFSET));
				glVertexAttribPointer(PC_ATTRIB_INDEX_NORMAL, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(idDrawVert), (void*)(vertOffset + DRAWVERT_NORMAL_OFFSET));
				glVertexAttribPointer(PC_ATTRIB_INDEX_COLOR, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(idDrawVert), (void*)(vertOffset + DRAWVERT_COLOR_OFFSET));
				glVertexAttribPointer(PC_ATTRIB_INDEX_COLOR2, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(idDrawVert), (void*)(vertOffset + DRAWVERT_COLOR2_OFFSET));
				glVertexAttribPointer(PC_ATTRIB_INDEX_ST, 2, GL_HALF_FLOAT, GL_TRUE, sizeof(idDrawVert), (void*)(vertOffset + DRAWVERT_ST_OFFSET));
				glVertexAttribPointer(PC_ATTRIB_INDEX_TANGENT, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(idDrawVert), (void*)(vertOffset + DRAWVERT_TANGENT_OFFSET));
			} else {
				glVertexAttribPointer(PC_ATTRIB_INDEX_VERTEX, 3, GL_FLOAT, GL_FALSE,
									  sizeof(idDrawVert), (void *) (DRAWVERT_XYZ_OFFSET));
				glVertexAttribPointer(PC_ATTRIB_INDEX_NORMAL, 4, GL_UNSIGNED_BYTE, GL_TRUE,
									  sizeof(idDrawVert), (void *) (DRAWVERT_NORMAL_OFFSET));
				glVertexAttribPointer(PC_ATTRIB_INDEX_COLOR, 4, GL_UNSIGNED_BYTE, GL_TRUE,
									  sizeof(idDrawVert), (void *) (DRAWVERT_COLOR_OFFSET));
				glVertexAttribPointer(PC_ATTRIB_INDEX_COLOR2, 4, GL_UNSIGNED_BYTE, GL_TRUE,
									  sizeof(idDrawVert), (void *) (DRAWVERT_COLOR2_OFFSET));
				glVertexAttribPointer(PC_ATTRIB_INDEX_ST, 2, GL_HALF_FLOAT, GL_TRUE,
									  sizeof(idDrawVert), (void *) (DRAWVERT_ST_OFFSET));
				glVertexAttribPointer(PC_ATTRIB_INDEX_TANGENT, 4, GL_UNSIGNED_BYTE, GL_TRUE,
									  sizeof(idDrawVert), (void *) (DRAWVERT_TANGENT_OFFSET));
			}
			vertexLayout = LAYOUT_DRAW_VERT;
            if (!isGLES32Version()) {
                currentVertOffset = vertOffset;
            }
        }
	}
	// RB end
#if defined(ANDROID) && !defined(USE_VULKAN)
	const GLuint mvpAttribs[4] = { 1, 5, 6, 7 };
	const GLuint modelViewAttribs[4] = { 10, 11, 12, 13 };
	const GLuint instanceAttribs[10] = { 1, 5, 6, 7, 10, 11, 12, 13, 14, 15 };
	if (useInstancing)
	{
		const GLsizeiptr instanceBytes = sizeof(gpuInstanceData_t) * instanceCount;
		GLuint instanceBuffer = 0;
		GLintptr instanceOffset = 0;
		if (!R_StreamInstanceData(instances, instanceBytes, instanceBuffer, instanceOffset))
		{
			// no persistent mapping: a fresh allocation per draw, as before
			if (gpuInstanceBuffer == 0)
			{
				GLuint buffer = 0;
				glGenBuffers(1, &buffer);
				gpuInstanceBuffer = buffer;
			}
			instanceBuffer = (GLuint)gpuInstanceBuffer;
			instanceOffset = 0;
			glBindBuffer(GL_ARRAY_BUFFER, instanceBuffer);
			glBufferData(GL_ARRAY_BUFFER, instanceBytes, instances, GL_STREAM_DRAW);
		}
		glBindBuffer(GL_ARRAY_BUFFER, instanceBuffer);
		const GLsizei stride = sizeof(gpuInstanceData_t);
		for (int row = 0; row < 4; ++row)
		{
			const GLuint mvpAttrib = mvpAttribs[row];
			const GLuint modelViewAttrib = modelViewAttribs[row];
			glEnableVertexAttribArray(mvpAttrib);
			glVertexAttribPointer(mvpAttrib, 4, GL_FLOAT, GL_FALSE, stride, (const void*)(instanceOffset + row * 4 * sizeof(float)));
			glVertexAttribDivisor(mvpAttrib, 1);
			glEnableVertexAttribArray(modelViewAttrib);
			glVertexAttribPointer(modelViewAttrib, 4, GL_FLOAT, GL_FALSE, stride, (const void*)(instanceOffset + 16 * sizeof(float) + row * 4 * sizeof(float)));
			glVertexAttribDivisor(modelViewAttrib, 1);
		}
		glEnableVertexAttribArray(14);
		glVertexAttribPointer(14, 4, GL_FLOAT, GL_FALSE, stride, (const void*)(instanceOffset + 32 * sizeof(float)));
		glVertexAttribDivisor(14, 1);
		glEnableVertexAttribArray(15);
		glVertexAttribPointer(15, 4, GL_FLOAT, GL_FALSE, stride, (const void*)(instanceOffset + 36 * sizeof(float)));
		glVertexAttribDivisor(15, 1);
	}
    if (useMultiDraw)
    {
        drawElementsIndirectCommand_t command;
        command.count = (GLuint)surf->numIndexes;
        command.instanceCount = 1;
        command.firstIndex = (GLuint)(indexOffset / sizeof(triIndex_t));
        command.baseVertex = (GLint)(vertOffset / sizeof(idDrawVert));
        // EXT_base_instance is optional, so the reserved field must remain zero.
        command.baseInstance = 0;
        g_multiDrawCommands.push_back(command);
        if (g_multiDrawCommands.size() == 1)
        {
            g_multiDrawVertexBuffer = (GLuint)vertexBuffer->GetAPIObject();
            g_multiDrawIndexBuffer = (GLuint)indexBuffer->GetAPIObject();
            g_multiDrawBatchLayout = MDI_LAYOUT_DRAW_VERT;
        }
        pc.c_drawElements++;
        pc.c_drawIndexes += surf->numIndexes;
        if (g_multiDrawCommands.size() >= MAX_GLES_MDI_BATCH_COMMANDS)
        {
            FlushMultiDrawBatch();
        }
        return;
    }
	if (useInstancing)
	{
		glDrawElementsInstancedBaseVertex(GL_TRIANGLES, r_singleTriangle.GetBool() ? 3 : surf->numIndexes,
			GL_INDEX_TYPE, (triIndex_t*)indexOffset, instanceCount, vertOffset / sizeof(idDrawVert));
	}
	else if (!isGLES32Version())
	{
		glDrawElements(GL_TRIANGLES, r_singleTriangle.GetBool() ? 3 : surf->numIndexes, GL_INDEX_TYPE, (triIndex_t*)indexOffset);
	}
	else
	{
		glDrawElementsBaseVertex(GL_TRIANGLES, r_singleTriangle.GetBool() ? 3 : surf->numIndexes,
			GL_INDEX_TYPE, (triIndex_t*)indexOffset, vertOffset / sizeof(idDrawVert));
	}
	if (useInstancing)
	{
		for (int i = 0; i < 10; ++i)
		{
			const GLuint attrib = instanceAttribs[i];
			glVertexAttribDivisor(attrib, 0);
			glDisableVertexAttribArray(attrib);
		}
		glBindBuffer(GL_ARRAY_BUFFER, (GLuint)vertexBuffer->GetAPIObject());
	}
	pc.c_drawElements++;
	pc.c_drawIndexes += surf->numIndexes * (useInstancing ? instanceCount : 1);
#else
	if (!isGLES32Version())
	{
		glDrawElements(GL_TRIANGLES, r_singleTriangle.GetBool() ? 3 : surf->numIndexes,
			GL_INDEX_TYPE, (triIndex_t*)indexOffset);
	}
	else
	{
		glDrawElementsBaseVertex(GL_TRIANGLES, r_singleTriangle.GetBool() ? 3 : surf->numIndexes,
			GL_INDEX_TYPE, (triIndex_t*)indexOffset, vertOffset / sizeof(idDrawVert));
	}
	pc.c_drawElements++;
	pc.c_drawIndexes += surf->numIndexes;
#endif
}


/*
=========================================================================================================

GL COMMANDS

=========================================================================================================
*/

/*
==================
idRenderBackend::GL_StartFrame
==================
*/
void idRenderBackend::GL_StartFrame()
{
#if defined(ANDROID) && !defined(USE_VULKAN)
    FlushMultiDrawBatch();
    g_multiDrawIndirectBatchCount = 0;
    g_multiDrawIndirectDrawCallsSaved = 0;
    g_multiDrawIndirectSubmittedCommands = 0;
#endif
	if (gEnableGLSynchronization && glConfig.syncAvailable && glIsSync(renderSync[swapIndex]))
	{
		const auto glSync = renderSync[swapIndex];
		const auto status = glClientWaitSync(glSync, 0, 1000 * 1000 * 16);
		if (status == GL_TIMEOUT_EXPIRED) {
			glClientWaitSync(glSync, 0, 1000 * 1000 * 50);
		}
		glDeleteSync(glSync);
		renderSync[swapIndex] = nullptr;
	}

	// If we have a stereo pixel format, this will draw to both
	// the back left and back right buffers, which will have a
	// performance penalty.
#ifndef ANDROID
	if (glConfig.directStateAccess) {
		glNamedFramebufferDrawBuffer(0, GL_BACK);
	}
	else
	{
		glDrawBuffer(GL_BACK);
	}
#endif
}

/*
==================
idRenderBackend::GL_EndFrame
==================
*/
void idRenderBackend::GL_EndFrame()
{
#if defined(ANDROID) && !defined(USE_VULKAN)
    FlushMultiDrawBatch();
    if (r_showMultiDrawIndirectStats.GetBool())
    {
        idLib::Printf("GLES MDI: available=%d, batches=%d, submitted draws=%d, draw calls saved=%d\n",
            g_multiDrawIndirectAvailable ? 1 : 0,
            g_multiDrawIndirectBatchCount,
            g_multiDrawIndirectSubmittedCommands,
            g_multiDrawIndirectDrawCallsSaved);
    }
#endif
	// Fix for the steam overlay not showing up while in game without Shell/Debug/Console/Menu also rendering
	glColorMask( 1, 1, 1, 1 );
	
	//glFlush();
}

/*
========================
GL_SetDefaultState

This should initialize all GL state that any part of the entire program
may touch, including the editor.
========================
*/
void idRenderBackend::GL_SetDefaultState()
{
#if defined(ANDROID) && !defined(USE_VULKAN)
    FlushMultiDrawBatch();
#endif
	RENDERLOG_PRINTF( "--- GL_SetDefaultState ---\n" );

#ifndef ANDROID
	glClearDepth( 1.0f );
#else
    glClearDepthf( 1.0f );
#endif
	// make sure our GL state vector is set correctly
	memset( &glcontext.tmu, 0, sizeof( glcontext.tmu ) );
	glcontext.activeTextureUnit = -1;
	currenttmu = 0;
	currentVertexBuffer = 0;
	currentIndexBuffer = 0;
	currentFramebuffer = 0;
	vertexLayout = LAYOUT_UNKNOWN;
	polyOfsScale = 0.0f;
	polyOfsBias = 0.0f;
	glStateBits = 0;
	
	hdrAverageLuminance = 0;
	hdrMaxLuminance = 0;
	hdrTime = 0;
	hdrKey = 0;
	
	GL_State( 0, true );
	
	// RB begin
	Framebuffer::Unbind();
	// RB end
	
#if 0
	// These are changed by GL_Cull
	glCullFace( GL_FRONT_AND_BACK );
	glEnable( GL_CULL_FACE );
	
	// These are changed by GL_State
	glColorMask( GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE );
	glBlendFunc( GL_ONE, GL_ZERO );
	glDepthMask( GL_TRUE );
	glDepthFunc( GL_LESS );
	glDisable( GL_STENCIL_TEST );
	glDisable( GL_POLYGON_OFFSET_FILL );
	glDisable( GL_POLYGON_OFFSET_LINE );
	glPolygonMode( GL_FRONT_AND_BACK, GL_FILL );
#endif
	
	// These should never be changed
	// DG: deprecated in opengl 3.2 and not needed because we don't do fixed function pipeline
	// glShadeModel( GL_SMOOTH );
	// DG end
	glEnable( GL_DEPTH_TEST );
	glEnable( GL_BLEND );
	glEnable( GL_SCISSOR_TEST );
#ifndef ANDROID
	if (glConfig.directStateAccess) {
		glNamedFramebufferDrawBuffer(0, GL_BACK);
		glNamedFramebufferReadBuffer(0, GL_BACK);
	}
	else
#endif
	{
#ifndef ANDROID
		glDrawBuffer(GL_BACK);
#endif
		glReadBuffer(GL_BACK);
	}
	
	if( r_useScissor.GetBool() )
	{
		glScissor( 0, 0, renderSystem->GetWidth(), renderSystem->GetHeight() );
	}
	
	// RB: don't keep renderprogs that were enabled during level load
	renderProgManager.Unbind();
	// RB end
}

/*
====================
idRenderBackend::GL_State

This routine is responsible for setting the most commonly changed state
====================
*/
void idRenderBackend::GL_State( uint64 stateBits, bool forceGlState )
{
	uint64 diff = stateBits ^ glStateBits;
	
	if( !r_useStateCaching.GetBool() || forceGlState )
	{
		// make sure everything is set all the time, so we
		// can see if our delta checking is screwing up
		diff = 0xFFFFFFFFFFFFFFFF;
	}
	else if( diff == 0 )
	{
		return;
	}
#if defined(ANDROID) && !defined(USE_VULKAN)
    FlushMultiDrawBatch();
#endif
	
	//
	// culling
	//
	if( diff & ( GLS_CULL_BITS ) )//| GLS_MIRROR_VIEW ) )
	{
		switch( stateBits & GLS_CULL_BITS )
		{
			case GLS_CULL_TWOSIDED:
				glDisable( GL_CULL_FACE );
				break;
				
			case GLS_CULL_BACKSIDED:
				glEnable( GL_CULL_FACE );
				if( viewDef != NULL && viewDef->isMirror )
				{
					stateBits |= GLS_MIRROR_VIEW;
					glCullFace( GL_FRONT );
				}
				else
				{
					glCullFace( GL_BACK );
				}
				break;
				
			case GLS_CULL_FRONTSIDED:
			default:
				glEnable( GL_CULL_FACE );
				if( viewDef != NULL && viewDef->isMirror )
				{
					stateBits |= GLS_MIRROR_VIEW;
					glCullFace( GL_BACK );
				}
				else
				{
					glCullFace( GL_FRONT );
				}
				break;
		}
	}
	
	//
	// check depthFunc bits
	//
	if( diff & GLS_DEPTHFUNC_BITS )
	{
		switch( stateBits & GLS_DEPTHFUNC_BITS )
		{
			case GLS_DEPTHFUNC_EQUAL:
				glDepthFunc( GL_EQUAL );
				break;
			case GLS_DEPTHFUNC_ALWAYS:
				glDepthFunc( GL_ALWAYS );
				break;
			case GLS_DEPTHFUNC_LESS:
				glDepthFunc( GL_LEQUAL );
				break;
			case GLS_DEPTHFUNC_GREATER:
				glDepthFunc( GL_GEQUAL );
				break;
		}
	}
	
	//
	// check blend bits
	//
	if( diff & ( GLS_SRCBLEND_BITS | GLS_DSTBLEND_BITS ) )
	{
		GLenum srcFactor = GL_ONE;
		GLenum dstFactor = GL_ZERO;
		
		switch( stateBits & GLS_SRCBLEND_BITS )
		{
			case GLS_SRCBLEND_ZERO:
				srcFactor = GL_ZERO;
				break;
			case GLS_SRCBLEND_ONE:
				srcFactor = GL_ONE;
				break;
			case GLS_SRCBLEND_DST_COLOR:
				srcFactor = GL_DST_COLOR;
				break;
			case GLS_SRCBLEND_ONE_MINUS_DST_COLOR:
				srcFactor = GL_ONE_MINUS_DST_COLOR;
				break;
			case GLS_SRCBLEND_SRC_ALPHA:
				srcFactor = GL_SRC_ALPHA;
				break;
			case GLS_SRCBLEND_ONE_MINUS_SRC_ALPHA:
				srcFactor = GL_ONE_MINUS_SRC_ALPHA;
				break;
			case GLS_SRCBLEND_DST_ALPHA:
				srcFactor = GL_DST_ALPHA;
				break;
			case GLS_SRCBLEND_ONE_MINUS_DST_ALPHA:
				srcFactor = GL_ONE_MINUS_DST_ALPHA;
				break;
			default:
				assert( !"GL_State: invalid src blend state bits\n" );
				break;
		}
		
		switch( stateBits & GLS_DSTBLEND_BITS )
		{
			case GLS_DSTBLEND_ZERO:
				dstFactor = GL_ZERO;
				break;
			case GLS_DSTBLEND_ONE:
				dstFactor = GL_ONE;
				break;
			case GLS_DSTBLEND_SRC_COLOR:
				dstFactor = GL_SRC_COLOR;
				break;
			case GLS_DSTBLEND_ONE_MINUS_SRC_COLOR:
				dstFactor = GL_ONE_MINUS_SRC_COLOR;
				break;
			case GLS_DSTBLEND_SRC_ALPHA:
				dstFactor = GL_SRC_ALPHA;
				break;
			case GLS_DSTBLEND_ONE_MINUS_SRC_ALPHA:
				dstFactor = GL_ONE_MINUS_SRC_ALPHA;
				break;
			case GLS_DSTBLEND_DST_ALPHA:
				dstFactor = GL_DST_ALPHA;
				break;
			case GLS_DSTBLEND_ONE_MINUS_DST_ALPHA:
				dstFactor = GL_ONE_MINUS_DST_ALPHA;
				break;
			default:
				assert( !"GL_State: invalid dst blend state bits\n" );
				break;
		}
		
		// Only actually update GL's blend func if blending is enabled.
		if( srcFactor == GL_ONE && dstFactor == GL_ZERO )
		{
			glDisable( GL_BLEND );
		}
		else
		{
			glEnable( GL_BLEND );
			glBlendFunc( srcFactor, dstFactor );
		}
	}
	
	//
	// check depthmask
	//
	if( diff & GLS_DEPTHMASK )
	{
		if( stateBits & GLS_DEPTHMASK )
		{
			glDepthMask( GL_FALSE );
		}
		else
		{
			glDepthMask( GL_TRUE );
		}
	}
	
	//
	// check colormask
	//
	if( diff & ( GLS_REDMASK | GLS_GREENMASK | GLS_BLUEMASK | GLS_ALPHAMASK ) )
	{
		GLboolean r = ( stateBits & GLS_REDMASK ) ? GL_FALSE : GL_TRUE;
		GLboolean g = ( stateBits & GLS_GREENMASK ) ? GL_FALSE : GL_TRUE;
		GLboolean b = ( stateBits & GLS_BLUEMASK ) ? GL_FALSE : GL_TRUE;
		GLboolean a = ( stateBits & GLS_ALPHAMASK ) ? GL_FALSE : GL_TRUE;
#if defined(ANDROID)
		// the masks of the G-buffer colors belong to the deferred shading, see DeferredSetGBufferWrites
		if( deferredGBufferAttached )
		{
			glColorMaski( 0, r, g, b, a );
		}
		else
#endif
		{
			glColorMask( r, g, b, a );
		}
	}
	
	//
	// fill/line mode
	//
#ifndef ANDROID
	if( diff & GLS_POLYMODE_LINE )
	{
		if( stateBits & GLS_POLYMODE_LINE )
		{
			glPolygonMode( GL_FRONT_AND_BACK, GL_LINE );
		}
		else
		{
			glPolygonMode( GL_FRONT_AND_BACK, GL_FILL );
		}
	}
#endif

	//
	// polygon offset
	//

	if( diff & GLS_POLYGON_OFFSET )
	{
		if( stateBits & GLS_POLYGON_OFFSET )
		{
			glPolygonOffset( polyOfsScale, polyOfsBias );
			glEnable( GL_POLYGON_OFFSET_FILL );
#ifndef ANDROID
			glEnable( GL_POLYGON_OFFSET_LINE );
#endif
		}
		else
		{
			glDisable( GL_POLYGON_OFFSET_FILL );
#ifndef ANDROID
            glDisable( GL_POLYGON_OFFSET_LINE );
#endif
		}
	}
	
	//
	// stencil
	//
	if( diff & ( GLS_STENCIL_FUNC_BITS | GLS_STENCIL_OP_BITS ) )
	{
		if( ( stateBits & ( GLS_STENCIL_FUNC_BITS | GLS_STENCIL_OP_BITS ) ) != 0 )
		{
			glEnable( GL_STENCIL_TEST );
		}
		else
		{
			glDisable( GL_STENCIL_TEST );
		}
	}
	if( diff & ( GLS_STENCIL_FUNC_BITS | GLS_STENCIL_FUNC_REF_BITS | GLS_STENCIL_FUNC_MASK_BITS ) )
	{
		GLuint ref = GLuint( ( stateBits & GLS_STENCIL_FUNC_REF_BITS ) >> GLS_STENCIL_FUNC_REF_SHIFT );
		GLuint mask = GLuint( ( stateBits & GLS_STENCIL_FUNC_MASK_BITS ) >> GLS_STENCIL_FUNC_MASK_SHIFT );
		GLenum func = 0;
		
		switch( stateBits & GLS_STENCIL_FUNC_BITS )
		{
			case GLS_STENCIL_FUNC_NEVER:
				func = GL_NEVER;
				break;
			case GLS_STENCIL_FUNC_LESS:
				func = GL_LESS;
				break;
			case GLS_STENCIL_FUNC_EQUAL:
				func = GL_EQUAL;
				break;
			case GLS_STENCIL_FUNC_LEQUAL:
				func = GL_LEQUAL;
				break;
			case GLS_STENCIL_FUNC_GREATER:
				func = GL_GREATER;
				break;
			case GLS_STENCIL_FUNC_NOTEQUAL:
				func = GL_NOTEQUAL;
				break;
			case GLS_STENCIL_FUNC_GEQUAL:
				func = GL_GEQUAL;
				break;
			case GLS_STENCIL_FUNC_ALWAYS:
				func = GL_ALWAYS;
				break;
		}
		glStencilFunc( func, ref, mask );
	}
	if( diff & ( GLS_STENCIL_OP_FAIL_BITS | GLS_STENCIL_OP_ZFAIL_BITS | GLS_STENCIL_OP_PASS_BITS ) )
	{
		GLenum sFail = 0;
		GLenum zFail = 0;
		GLenum pass = 0;
		
		switch( stateBits & GLS_STENCIL_OP_FAIL_BITS )
		{
			case GLS_STENCIL_OP_FAIL_KEEP:
				sFail = GL_KEEP;
				break;
			case GLS_STENCIL_OP_FAIL_ZERO:
				sFail = GL_ZERO;
				break;
			case GLS_STENCIL_OP_FAIL_REPLACE:
				sFail = GL_REPLACE;
				break;
			case GLS_STENCIL_OP_FAIL_INCR:
				sFail = GL_INCR;
				break;
			case GLS_STENCIL_OP_FAIL_DECR:
				sFail = GL_DECR;
				break;
			case GLS_STENCIL_OP_FAIL_INVERT:
				sFail = GL_INVERT;
				break;
			case GLS_STENCIL_OP_FAIL_INCR_WRAP:
				sFail = GL_INCR_WRAP;
				break;
			case GLS_STENCIL_OP_FAIL_DECR_WRAP:
				sFail = GL_DECR_WRAP;
				break;
		}
		switch( stateBits & GLS_STENCIL_OP_ZFAIL_BITS )
		{
			case GLS_STENCIL_OP_ZFAIL_KEEP:
				zFail = GL_KEEP;
				break;
			case GLS_STENCIL_OP_ZFAIL_ZERO:
				zFail = GL_ZERO;
				break;
			case GLS_STENCIL_OP_ZFAIL_REPLACE:
				zFail = GL_REPLACE;
				break;
			case GLS_STENCIL_OP_ZFAIL_INCR:
				zFail = GL_INCR;
				break;
			case GLS_STENCIL_OP_ZFAIL_DECR:
				zFail = GL_DECR;
				break;
			case GLS_STENCIL_OP_ZFAIL_INVERT:
				zFail = GL_INVERT;
				break;
			case GLS_STENCIL_OP_ZFAIL_INCR_WRAP:
				zFail = GL_INCR_WRAP;
				break;
			case GLS_STENCIL_OP_ZFAIL_DECR_WRAP:
				zFail = GL_DECR_WRAP;
				break;
		}
		switch( stateBits & GLS_STENCIL_OP_PASS_BITS )
		{
			case GLS_STENCIL_OP_PASS_KEEP:
				pass = GL_KEEP;
				break;
			case GLS_STENCIL_OP_PASS_ZERO:
				pass = GL_ZERO;
				break;
			case GLS_STENCIL_OP_PASS_REPLACE:
				pass = GL_REPLACE;
				break;
			case GLS_STENCIL_OP_PASS_INCR:
				pass = GL_INCR;
				break;
			case GLS_STENCIL_OP_PASS_DECR:
				pass = GL_DECR;
				break;
			case GLS_STENCIL_OP_PASS_INVERT:
				pass = GL_INVERT;
				break;
			case GLS_STENCIL_OP_PASS_INCR_WRAP:
				pass = GL_INCR_WRAP;
				break;
			case GLS_STENCIL_OP_PASS_DECR_WRAP:
				pass = GL_DECR_WRAP;
				break;
		}
		glStencilOp( sFail, zFail, pass );
	}
	
	glStateBits = stateBits;
}

/*
====================
GL_InvalidateTextureBindings
====================
*/
void GL_InvalidateTextureBindings()
{
	// no texture is named 0, so nothing matches until bound again
	memset( glcontext.tmu, 0, sizeof( glcontext.tmu ) );
}

void R_DirectBindTexture( unsigned int target, unsigned int texture )
{
	glBindTexture( target, texture );
	GL_InvalidateTextureBindings();
}

void R_DirectDeleteTextures( int count, const unsigned int* textures )
{
	// a deleted texture is unbound from every unit, and its name comes back for new textures
	glDeleteTextures( count, textures );
	GL_InvalidateTextureBindings();
}

/*
====================
idRenderBackend::GL_ActivateTextureUnit
====================
*/
void idRenderBackend::GL_ActivateTextureUnit()
{
	if( glcontext.activeTextureUnit != currenttmu )
	{
		glActiveTexture( GL_TEXTURE0 + currenttmu );
		glcontext.activeTextureUnit = currenttmu;
	}
}

/*
====================
idRenderBackend::SelectTexture
====================
*/
void idRenderBackend::GL_SelectTexture( int unit )
{
	if( currenttmu == unit )
	{
		return;
	}
	
	if( unit < 0 || unit >= glConfig.maxTextureImageUnits )
	{
		common->Warning( "GL_SelectTexture: unit = %i", unit );
		return;
	}
	
	RENDERLOG_PRINTF( "GL_SelectTexture( %i );\n", unit );
	
	currenttmu = unit;
}



/*
====================
idRenderBackend::GL_Scissor
====================
*/
void idRenderBackend::GL_Scissor( int x /* left*/, int y /* bottom */, int w, int h )
{
#if defined(ANDROID) && !defined(USE_VULKAN)
    FlushMultiDrawBatch();
#endif
	glScissor( x, y, w, h );
}

/*
====================
idRenderBackend::GL_Viewport
====================
*/
void idRenderBackend::GL_Viewport( int x /* left */, int y /* bottom */, int w, int h )
{
#if defined(ANDROID) && !defined(USE_VULKAN)
    FlushMultiDrawBatch();
#endif
	glViewport( x, y, w, h );
}

/*
====================
idRenderBackend::GL_PolygonOffset
====================
*/
void idRenderBackend::GL_PolygonOffset( float scale, float bias )
{
#if defined(ANDROID) && !defined(USE_VULKAN)
    if (polyOfsScale != scale || polyOfsBias != bias)
    {
        FlushMultiDrawBatch();
    }
#endif
	polyOfsScale = scale;
	polyOfsBias = bias;
	
	if( glStateBits & GLS_POLYGON_OFFSET )
	{
		glPolygonOffset( scale, bias );
	}
}

/*
========================
idRenderBackend::GL_DepthBoundsTest
========================
*/
void idRenderBackend::GL_DepthBoundsTest( const float zmin, const float zmax )
{
#ifndef ANDROID
	if( !glConfig.depthBoundsTestAvailable || zmin > zmax )
	{
		return;
	}
	
	if( zmin == 0.0f && zmax == 0.0f )
	{
		glDisable( GL_DEPTH_BOUNDS_TEST_EXT );
	}
	else
	{
		glEnable( GL_DEPTH_BOUNDS_TEST_EXT );
		glDepthBoundsEXT( zmin, zmax );
	}
#endif
}

/*
====================
idRenderBackend::GL_Color
====================
*/
void idRenderBackend::GL_Color( float r, float g, float b, float a )
{
	float parm[4];
	parm[0] = idMath::ClampFloat( 0.0f, 1.0f, r );
	parm[1] = idMath::ClampFloat( 0.0f, 1.0f, g );
	parm[2] = idMath::ClampFloat( 0.0f, 1.0f, b );
	parm[3] = idMath::ClampFloat( 0.0f, 1.0f, a );
	renderProgManager.SetRenderParm( RENDERPARM_COLOR, parm );
}

/*
========================
idRenderBackend::GL_Clear
========================
*/
void idRenderBackend::GL_Clear( bool color, bool depth, bool stencil, byte stencilValue, float r, float g, float b, float a, bool clearHDR )
{
#if defined(ANDROID) && !defined(USE_VULKAN)
    FlushMultiDrawBatch();
#endif
	int clearFlags = 0;
	if( color )
	{
		glClearColor( r, g, b, a );
		clearFlags |= GL_COLOR_BUFFER_BIT;
	}
	if( depth )
	{
		clearFlags |= GL_DEPTH_BUFFER_BIT;
	}
	if( stencil )
	{
		glClearStencil( stencilValue );
		clearFlags |= GL_STENCIL_BUFFER_BIT;
	}
	glClear( clearFlags );
	
	// RB begin
	if( UseSceneFramebuffer() && clearHDR && globalFramebuffers.hdrFBO != NULL )
	{
		bool isDefaultFramebufferActive = Framebuffer::IsDefaultFramebufferActive();
		
		globalFramebuffers.hdrFBO->Bind();
		glClear( clearFlags );
		
		if( isDefaultFramebufferActive )
		{
			Framebuffer::Unbind();
		}
	}
	// RB end
}


/*
=================
idRenderBackend::GL_GetCurrentState
=================
*/
uint64 idRenderBackend::GL_GetCurrentState() const
{
	return glStateBits;
}

/*
========================
idRenderBackend::GL_GetCurrentStateMinusStencil
========================
*/
uint64 idRenderBackend::GL_GetCurrentStateMinusStencil() const
{
	return GL_GetCurrentState() & ~( GLS_STENCIL_OP_BITS | GLS_STENCIL_FUNC_BITS | GLS_STENCIL_FUNC_REF_BITS | GLS_STENCIL_FUNC_MASK_BITS );
}


/*
=============
idRenderBackend::CheckCVars

See if some cvars that we watch have changed
=============
*/
void idRenderBackend::CheckCVars()
{
	// gamma stuff
	if( r_gamma.IsModified() || r_brightness.IsModified() )
	{
		r_gamma.ClearModified();
		r_brightness.ClearModified();
		R_SetColorMappings();
	}
	
	// filtering
	if( r_maxAnisotropicFiltering.IsModified() || r_useTrilinearFiltering.IsModified() || r_lodBias.IsModified() )
	{
		idLib::Printf( "Updating texture filter parameters.\n" );
#ifdef ANDROID
		// r_lodBias is a shader uniform on gles (CommitUniforms), only filtering changes concern the textures
		const bool filteringChanged = r_maxAnisotropicFiltering.IsModified() || r_useTrilinearFiltering.IsModified();
#endif
		r_maxAnisotropicFiltering.ClearModified();
		r_useTrilinearFiltering.ClearModified();
		r_lodBias.ClearModified();
		
		for( int i = 0 ; i < globalImages->images.Num() ; i++ )
		{
			if( globalImages->images[i] )
			{
#ifdef ANDROID
				// Bind() loads images that aren't resident, i.e. every texture this session ever referenced, at once.
				// an image loaded later gets the current filtering from AllocImage() anyway.
				if( !filteringChanged || !globalImages->images[i]->IsLoaded() )
				{
					continue;
				}
#endif
				//GK: It sucks but some textures needs to be bound when altering their parameters (so much for DSA)
				globalImages->images[i]->Bind();
				GL_ActivateTextureUnit();
				globalImages->images[i]->SetTexParameters();
			}
		}
	}
	
	if( r_useSeamlessCubeMap.IsModified() )
	{
		r_useSeamlessCubeMap.ClearModified();
#ifndef ANDROID
		if( glConfig.seamlessCubeMapAvailable )
		{
			if( r_useSeamlessCubeMap.GetBool() )
			{
				glEnable( GL_TEXTURE_CUBE_MAP_SEAMLESS );
			}
			else
			{
				glDisable( GL_TEXTURE_CUBE_MAP_SEAMLESS );
			}
		}
#endif
	}
	
	if( r_useSRGB.IsModified() )
	{
		r_useSRGB.ClearModified();
		if( glConfig.sRGBFramebufferAvailable )
		{
#ifndef ANDROID
			if( r_useSRGB.GetBool() && r_useSRGB.GetInteger() != 3 )
			{
				glEnable( GL_FRAMEBUFFER_SRGB );
			}
			else
			{
				glDisable( GL_FRAMEBUFFER_SRGB );
			}
#endif
		}
	}

#ifndef ANDROID
	if( r_antiAliasing.IsModified() )
#else
	if( r_antiAliasing.IsModified() && (glConfig.hasMSAAEXT || isGLES32Version()))
#endif
	{
		switch( r_antiAliasing.GetInteger() )
		{
			case ANTI_ALIASING_MSAA_2X:
			case ANTI_ALIASING_MSAA_4X:
			case ANTI_ALIASING_MSAA_8X:
				if( r_antiAliasing.GetInteger() > 0 )
				{
					glEnable( GL_MULTISAMPLE );
				}
				break;
				
			default:
				glDisable( GL_MULTISAMPLE );
				break;
		}
	}
	
	if( r_useHDR.IsModified() || r_useHalfLambertLighting.IsModified() )
	{
		glConfig.forceGLSLGeneration = true;
		r_useHDR.ClearModified();
		r_useHalfLambertLighting.ClearModified();
		renderProgManager.KillAllShaders();
		renderProgManager.LoadAllShaders();
		glConfig.forceGLSLGeneration = false;
	}

#ifndef ANDROID
	// RB: turn off shadow mapping for OpenGL drivers that are too slow
	switch( glConfig.driverType )
	{
#if ANDROID
		case GLDRV_OPENGL_MESA:
			r_fullscreen.SetInteger( 1 );
			r_useSSAO.SetInteger(0);
			com_engineHz.SetInteger(r_displayRefresh.GetInteger());
            break;
#endif
		case GLDRV_OPENGL_ES2:
		case GLDRV_OPENGL_ES3:
			r_fullscreen.SetInteger( 1 );
			r_useSSAO.SetInteger(0);
			com_engineHz.SetInteger(r_displayRefresh.GetInteger());
			break;
			
		default:
			break;
	}
#else
	r_fullscreen.SetInteger( 1 );
	r_useSSAO.SetInteger(0);
	r_motionBlur.SetInteger(0);
	com_engineHz.SetInteger(r_displayRefresh.GetInteger());
    static const float targetFovValue = 86.0f;
    const auto currentFovValue = g_fov.GetFloat();

    if (currentFovValue < targetFovValue){
        g_fov.SetFloat(targetFovValue);
    }
#endif
	// RB end
}


/*
==============================================================================================

STENCIL SHADOW RENDERING

==============================================================================================
*/
extern idCVar r_useStencilShadowPreload;

/*
==================
idRenderBackend::DrawStencilShadowPass
==================
*/
#if defined(ANDROID) && !defined(USE_VULKAN)
void idRenderBackend::DrawStencilShadowPass( const drawSurf_t* drawSurf, const bool renderZPass,
		const gpuInstanceData_t* instances, int instanceCount )
#else
void idRenderBackend::DrawStencilShadowPass( const drawSurf_t* drawSurf, const bool renderZPass )
#endif
{
#if defined(ANDROID) && !defined(USE_VULKAN)
    const bool useInstancing = instances != NULL && instanceCount > 1 && !drawSurf->jointCache;
    const bool useMultiDrawShadow = g_multiDrawIndirectAvailable
        && r_useMultiDrawIndirect.GetBool()
        && isGLES32Version()
        && !r_singleTriangle.GetBool()
        && !drawSurf->jointCache
        && !useInstancing
        && (renderZPass || !r_useStencilShadowPreload.GetBool());
    if (!useMultiDrawShadow)
    {
        FlushMultiDrawBatch();
    }
#endif
	// get vertex buffer
	const vertCacheHandle_t vbHandle = drawSurf->shadowCache;
	idVertexBuffer* vertexBuffer;
	if( vertexCache.CacheIsStatic( vbHandle ) )
	{
		vertexBuffer = &vertexCache.staticData.vertexBuffer;
	}
	else
	{
		const uint64 frameNum = ( int )( vbHandle >> VERTCACHE_FRAME_SHIFT ) & VERTCACHE_FRAME_MASK;
		if( frameNum != ( ( vertexCache.currentFrame - 1 ) & VERTCACHE_FRAME_MASK ) )
		{
			idLib::Warning( "DrawStencilShadowPass, vertexBuffer == NULL" );
			return;
		}
		vertexBuffer = &vertexCache.frameData[vertexCache.drawListNum].vertexBuffer;
	}
	const int vertOffset = ( int )( vbHandle >> VERTCACHE_OFFSET_SHIFT ) & VERTCACHE_OFFSET_MASK;
	
	// get index buffer
	const vertCacheHandle_t ibHandle = drawSurf->indexCache;
	idIndexBuffer* indexBuffer;
	if( vertexCache.CacheIsStatic( ibHandle ) )
	{
		indexBuffer = &vertexCache.staticData.indexBuffer;
	}
	else
	{
		const uint64 frameNum = ( int )( ibHandle >> VERTCACHE_FRAME_SHIFT ) & VERTCACHE_FRAME_MASK;
		if( frameNum != ( ( vertexCache.currentFrame - 1 ) & VERTCACHE_FRAME_MASK ) )
		{
			idLib::Warning( "DrawStencilShadowPass, indexBuffer == NULL" );
			return;
		}
		indexBuffer = &vertexCache.frameData[vertexCache.drawListNum].indexBuffer;
	}
	const uint64 indexOffset = ( int )( ibHandle >> VERTCACHE_OFFSET_SHIFT ) & VERTCACHE_OFFSET_MASK;
#if defined(ANDROID) && !defined(USE_VULKAN)
    if (useMultiDrawShadow && !g_multiDrawCommands.empty()
        && (g_multiDrawVertexBuffer != (GLuint)vertexBuffer->GetAPIObject()
            || g_multiDrawIndexBuffer != (GLuint)indexBuffer->GetAPIObject()
            || g_multiDrawBatchLayout != MDI_LAYOUT_SHADOW_VERT
            || vertexLayout != LAYOUT_DRAW_SHADOW_VERT))
    {
        FlushMultiDrawBatch();
    }
#endif
	
	RENDERLOG_PRINTF( "Binding Buffers: %p %p\n", vertexBuffer, indexBuffer );
	
	// RB: 64 bit fixes, changed GLuint to GLintptr
	if((GLintptr)currentIndexBuffer != ( GLintptr )indexBuffer->GetAPIObject()
            || (!isGLES32Version() && currentVertOffset != vertOffset) || !r_useStateCaching.GetBool() )
	{
#ifndef ANDROID
		if (glConfig.directStateAccess) {
			glVertexArrayElementBuffer(glConfig.global_vao, indexBuffer->GetAPIObject());
		}
		else
#endif
		{
			glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, (GLintptr)indexBuffer->GetAPIObject());
		}
		currentIndexBuffer = ( GLintptr )indexBuffer->GetAPIObject();
	}
	
	if( drawSurf->jointCache )
	{
		assert( renderProgManager.ShaderUsesJoints() );
		
		idUniformBuffer jointBuffer;
		if( !vertexCache.GetJointBuffer( drawSurf->jointCache, &jointBuffer ) )
		{
			idLib::Warning( "DrawStencilShadowPass, jointBuffer == NULL" );
			return;
		}
		assert( ( jointBuffer.GetOffset() & ( glConfig.uniformBufferOffsetAlignment - 1 ) ) == 0 );
		
		const GLintptr ubo = jointBuffer.GetAPIObject();
		glBindBufferRange( GL_UNIFORM_BUFFER, 0, ubo, jointBuffer.GetOffset(), jointBuffer.GetSize() );
		
		if( ( vertexLayout != LAYOUT_DRAW_SHADOW_VERT_SKINNED ) || ((GLintptr)currentVertexBuffer != ( GLintptr )vertexBuffer->GetAPIObject() )
        || !r_useStateCaching.GetBool() || (!isGLES32Version() && currentVertOffset != vertOffset) )
		{
#ifndef ANDROID
			if (!glConfig.directStateAccess)
#endif
			{
				glBindBuffer(GL_ARRAY_BUFFER, (GLintptr)vertexBuffer->GetAPIObject());
				currentVertexBuffer = (GLintptr)vertexBuffer->GetAPIObject();

				glEnableVertexAttribArray(PC_ATTRIB_INDEX_VERTEX);
				glDisableVertexAttribArray(PC_ATTRIB_INDEX_NORMAL);
				glEnableVertexAttribArray(PC_ATTRIB_INDEX_COLOR);
				glEnableVertexAttribArray(PC_ATTRIB_INDEX_COLOR2);
				glDisableVertexAttribArray(PC_ATTRIB_INDEX_ST);
				glDisableVertexAttribArray(PC_ATTRIB_INDEX_TANGENT);

if (!isGLES32Version()) {
    glVertexAttribPointer(PC_ATTRIB_INDEX_VERTEX, 4, GL_FLOAT, GL_FALSE,
                          sizeof(idShadowVertSkinned),
                          (void *) (vertOffset + SHADOWVERTSKINNED_XYZW_OFFSET));
    glVertexAttribPointer(PC_ATTRIB_INDEX_COLOR, 4, GL_UNSIGNED_BYTE, GL_TRUE,
                          sizeof(idShadowVertSkinned),
                          (void *) (vertOffset + SHADOWVERTSKINNED_COLOR_OFFSET));
    glVertexAttribPointer(PC_ATTRIB_INDEX_COLOR2, 4, GL_UNSIGNED_BYTE, GL_TRUE,
                          sizeof(idShadowVertSkinned),
                          (void *) (vertOffset + SHADOWVERTSKINNED_COLOR2_OFFSET));
} else {
    glVertexAttribPointer(PC_ATTRIB_INDEX_VERTEX, 4, GL_FLOAT, GL_FALSE,
                          sizeof(idShadowVertSkinned), (void *) (SHADOWVERTSKINNED_XYZW_OFFSET));
    glVertexAttribPointer(PC_ATTRIB_INDEX_COLOR, 4, GL_UNSIGNED_BYTE, GL_TRUE,
                          sizeof(idShadowVertSkinned), (void *) (SHADOWVERTSKINNED_COLOR_OFFSET));
    glVertexAttribPointer(PC_ATTRIB_INDEX_COLOR2, 4, GL_UNSIGNED_BYTE, GL_TRUE,
                          sizeof(idShadowVertSkinned), (void *) (SHADOWVERTSKINNED_COLOR2_OFFSET));
}
			}
#ifndef ANDROID
			else 
			{
				glVertexArrayVertexBuffer(glConfig.global_vao, 0, vertexBuffer->GetAPIObject(), 0, sizeof(idShadowVertSkinned));
				currentVertexBuffer = (GLintptr)vertexBuffer->GetAPIObject();

				glEnableVertexArrayAttrib(glConfig.global_vao, PC_ATTRIB_INDEX_VERTEX);
				glEnableVertexArrayAttrib(glConfig.global_vao, PC_ATTRIB_INDEX_NORMAL);
				glEnableVertexArrayAttrib(glConfig.global_vao, PC_ATTRIB_INDEX_COLOR);
				glEnableVertexArrayAttrib(glConfig.global_vao, PC_ATTRIB_INDEX_COLOR2);
				glEnableVertexArrayAttrib(glConfig.global_vao, PC_ATTRIB_INDEX_ST);
				glEnableVertexArrayAttrib(glConfig.global_vao, PC_ATTRIB_INDEX_TANGENT);

				glVertexArrayAttribFormat(glConfig.global_vao, PC_ATTRIB_INDEX_VERTEX, 4, GL_FLOAT, GL_FALSE, SHADOWVERTSKINNED_XYZW_OFFSET);
				glVertexArrayAttribFormat(glConfig.global_vao, PC_ATTRIB_INDEX_COLOR, 4, GL_UNSIGNED_BYTE, GL_TRUE, SHADOWVERTSKINNED_COLOR_OFFSET);
				glVertexArrayAttribFormat(glConfig.global_vao, PC_ATTRIB_INDEX_COLOR2, 4, GL_UNSIGNED_BYTE, GL_TRUE, SHADOWVERTSKINNED_COLOR2_OFFSET);

				glVertexArrayAttribBinding(glConfig.global_vao, PC_ATTRIB_INDEX_VERTEX, 0);
				glVertexArrayAttribBinding(glConfig.global_vao, PC_ATTRIB_INDEX_COLOR, 0);
				glVertexArrayAttribBinding(glConfig.global_vao, PC_ATTRIB_INDEX_COLOR2, 0);
			}
#endif
			vertexLayout = LAYOUT_DRAW_SHADOW_VERT_SKINNED;
            if (!isGLES32Version()) {
                currentVertOffset = vertOffset;
            }
		}
		glBindVertexArray(glConfig.global_vao);
	}
	else
	{
		if( ( vertexLayout != LAYOUT_DRAW_SHADOW_VERT ) || ((GLintptr)currentVertexBuffer != ( GLintptr )vertexBuffer->GetAPIObject() ) ||
        (!isGLES32Version() && currentVertOffset != vertOffset) || !r_useStateCaching.GetBool() )
		{
#ifndef ANDROID
			if (!glConfig.directStateAccess) {
				glBindBuffer(GL_ARRAY_BUFFER, (GLintptr)vertexBuffer->GetAPIObject());
				currentVertexBuffer = (GLintptr)vertexBuffer->GetAPIObject();

				glEnableVertexAttribArray(PC_ATTRIB_INDEX_VERTEX);
				glDisableVertexAttribArray(PC_ATTRIB_INDEX_NORMAL);
				glDisableVertexAttribArray(PC_ATTRIB_INDEX_COLOR);
				glDisableVertexAttribArray(PC_ATTRIB_INDEX_COLOR2);
				glDisableVertexAttribArray(PC_ATTRIB_INDEX_ST);
				glDisableVertexAttribArray(PC_ATTRIB_INDEX_TANGENT);

#if defined(USE_GLES2) || defined(USE_GLES3)
				glVertexAttribPointer(PC_ATTRIB_INDEX_VERTEX, 4, GL_FLOAT, GL_FALSE, sizeof(idShadowVert), (void*)(vertOffset + SHADOWVERT_XYZW_OFFSET));
#else
				glVertexAttribPointer(PC_ATTRIB_INDEX_VERTEX, 4, GL_FLOAT, GL_FALSE, sizeof(idShadowVert), (void*)(SHADOWVERT_XYZW_OFFSET));
#endif
			}
			else 
			{
				glVertexArrayVertexBuffer(glConfig.global_vao, 0, vertexBuffer->GetAPIObject(), 0, sizeof(idShadowVert));
				currentVertexBuffer = (GLintptr)vertexBuffer->GetAPIObject();

				glEnableVertexArrayAttrib(glConfig.global_vao, PC_ATTRIB_INDEX_VERTEX);
				glEnableVertexArrayAttrib(glConfig.global_vao, PC_ATTRIB_INDEX_NORMAL);
				glEnableVertexArrayAttrib(glConfig.global_vao, PC_ATTRIB_INDEX_COLOR);
				glEnableVertexArrayAttrib(glConfig.global_vao, PC_ATTRIB_INDEX_COLOR2);
				glEnableVertexArrayAttrib(glConfig.global_vao, PC_ATTRIB_INDEX_ST);
				glEnableVertexArrayAttrib(glConfig.global_vao, PC_ATTRIB_INDEX_TANGENT);

				glVertexArrayAttribFormat(glConfig.global_vao, PC_ATTRIB_INDEX_VERTEX, 4, GL_FLOAT, GL_FALSE, SHADOWVERT_XYZW_OFFSET);

				glVertexArrayAttribBinding(glConfig.global_vao, PC_ATTRIB_INDEX_VERTEX, 0);
			}
#else
			glBindBuffer(GL_ARRAY_BUFFER, (GLintptr)vertexBuffer->GetAPIObject());
			currentVertexBuffer = (GLintptr)vertexBuffer->GetAPIObject();

			glEnableVertexAttribArray(PC_ATTRIB_INDEX_VERTEX);
			glDisableVertexAttribArray(PC_ATTRIB_INDEX_NORMAL);
			glDisableVertexAttribArray(PC_ATTRIB_INDEX_COLOR);
			glDisableVertexAttribArray(PC_ATTRIB_INDEX_COLOR2);
			glDisableVertexAttribArray(PC_ATTRIB_INDEX_ST);
			glDisableVertexAttribArray(PC_ATTRIB_INDEX_TANGENT);

            if (!isGLES32Version()) {
                glVertexAttribPointer(PC_ATTRIB_INDEX_VERTEX, 4, GL_FLOAT, GL_FALSE,
                                      sizeof(idShadowVert),
                                      (void *) (vertOffset + SHADOWVERT_XYZW_OFFSET));
            }
            else {
                glVertexAttribPointer(PC_ATTRIB_INDEX_VERTEX, 4, GL_FLOAT, GL_FALSE,
                                      sizeof(idShadowVert), (void *) (SHADOWVERT_XYZW_OFFSET));
            }
#endif
			vertexLayout = LAYOUT_DRAW_SHADOW_VERT;
            if (!isGLES32Version()) {
                currentVertOffset = vertOffset;
            }
		}
	}
	// RB end
	
#if defined(ANDROID) && !defined(USE_VULKAN)
	const GLuint mvpAttribs[4] = { 1, 5, 6, 7 };
	const GLuint lightOriginAttrib = 15;
	if( useInstancing )
	{
		const GLsizeiptr instanceBytes = sizeof( gpuInstanceData_t ) * instanceCount;
		GLuint instanceBuffer = 0;
		GLintptr instanceOffset = 0;
		if( !R_StreamInstanceData( instances, instanceBytes, instanceBuffer, instanceOffset ) )
		{
			if( gpuInstanceBuffer == 0 )
			{
				GLuint buffer = 0;
				glGenBuffers( 1, &buffer );
				gpuInstanceBuffer = buffer;
			}
			instanceBuffer = (GLuint)gpuInstanceBuffer;
			instanceOffset = 0;
			glBindBuffer( GL_ARRAY_BUFFER, instanceBuffer );
			glBufferData( GL_ARRAY_BUFFER, instanceBytes, instances, GL_STREAM_DRAW );
		}
		glBindBuffer( GL_ARRAY_BUFFER, instanceBuffer );
		const GLsizei stride = sizeof( gpuInstanceData_t );
		for( int row = 0; row < 4; ++row )
		{
			const GLuint attrib = mvpAttribs[row];
			glEnableVertexAttribArray( attrib );
			glVertexAttribPointer( attrib, 4, GL_FLOAT, GL_FALSE, stride, (const void*)( instanceOffset + row * 4 * sizeof( float ) ) );
			glVertexAttribDivisor( attrib, 1 );
		}
		glEnableVertexAttribArray( lightOriginAttrib );
		glVertexAttribPointer( lightOriginAttrib, 4, GL_FLOAT, GL_FALSE, stride, (const void*)( instanceOffset + 36 * sizeof( float ) ) );
		glVertexAttribDivisor( lightOriginAttrib, 1 );
	}
#endif

#if defined(ANDROID) && !defined(USE_VULKAN)
    if (!(useMultiDrawShadow && !g_multiDrawCommands.empty()))
    {
        renderProgManager.CommitUniforms(glStateBits);
    }
#else
	renderProgManager.CommitUniforms( glStateBits );
#endif
	
#if defined(ANDROID) && !defined(USE_VULKAN)
    if (useMultiDrawShadow)
    {
        drawElementsIndirectCommand_t command;
        command.count = (GLuint)drawSurf->numIndexes;
        command.instanceCount = 1;
        command.firstIndex = (GLuint)(indexOffset / sizeof(triIndex_t));
        command.baseVertex = (GLint)(vertOffset / sizeof(idShadowVert));
        command.baseInstance = 0;
        g_multiDrawCommands.push_back(command);
        if (g_multiDrawCommands.size() == 1)
        {
            g_multiDrawVertexBuffer = (GLuint)vertexBuffer->GetAPIObject();
            g_multiDrawIndexBuffer = (GLuint)indexBuffer->GetAPIObject();
            g_multiDrawBatchLayout = MDI_LAYOUT_SHADOW_VERT;
        }
        pc.c_shadowElements++;
        pc.c_shadowIndexes += drawSurf->numIndexes;
        if (g_multiDrawCommands.size() >= MAX_GLES_MDI_BATCH_COMMANDS)
        {
            FlushMultiDrawBatch();
        }
        return;
    }
#endif
	if( drawSurf->jointCache )
	{
        if (!isGLES32Version()) {
            glDrawElements(GL_TRIANGLES, r_singleTriangle.GetBool() ? 3 : drawSurf->numIndexes,
                           GL_INDEX_TYPE, (triIndex_t *) indexOffset);
        }
        else {
            glDrawElementsBaseVertex(GL_TRIANGLES,
                                     r_singleTriangle.GetBool() ? 3 : drawSurf->numIndexes,
                                     GL_INDEX_TYPE, (triIndex_t *) indexOffset,
                                     vertOffset / sizeof(idShadowVertSkinned));
        }
	}
	else
	{
#if defined(ANDROID) && !defined(USE_VULKAN)
		if( useInstancing )
		{
			glDrawElementsInstancedBaseVertex( GL_TRIANGLES, r_singleTriangle.GetBool() ? 3 : drawSurf->numIndexes,
					GL_INDEX_TYPE, (triIndex_t*)indexOffset, instanceCount, vertOffset / sizeof( idShadowVert ) );
		}
		else
#endif
		if( !isGLES32Version() )
		{
			glDrawElements( GL_TRIANGLES, r_singleTriangle.GetBool() ? 3 : drawSurf->numIndexes,
					GL_INDEX_TYPE, (triIndex_t*)indexOffset );
		}
		else
		{
			glDrawElementsBaseVertex( GL_TRIANGLES, r_singleTriangle.GetBool() ? 3 : drawSurf->numIndexes,
				GL_INDEX_TYPE, (triIndex_t*)indexOffset, vertOffset / sizeof( idShadowVert ) );
		}
	}
	
	// RB: added stats
	pc.c_shadowElements++;
	#if defined(ANDROID) && !defined(USE_VULKAN)
	pc.c_shadowIndexes += drawSurf->numIndexes * ( useInstancing ? instanceCount : 1 );
#else
	pc.c_shadowIndexes += drawSurf->numIndexes;
#endif
	// RB end
	
	if( !renderZPass && r_useStencilShadowPreload.GetBool() )
	{
		// render again with Z-pass
		glStencilOpSeparate( GL_FRONT, GL_KEEP, GL_KEEP, GL_INCR );
		glStencilOpSeparate( GL_BACK, GL_KEEP, GL_KEEP, GL_DECR );
		
		if( drawSurf->jointCache )
		{
            if (!isGLES32Version()) {
                glDrawElements(GL_TRIANGLES, r_singleTriangle.GetBool() ? 3 : drawSurf->numIndexes,
                               GL_INDEX_TYPE, (triIndex_t *) indexOffset);
            } else {
                glDrawElementsBaseVertex(GL_TRIANGLES,
                                         r_singleTriangle.GetBool() ? 3 : drawSurf->numIndexes,
                                         GL_INDEX_TYPE, (triIndex_t *) indexOffset,
                                         vertOffset / sizeof(idShadowVertSkinned));
            }
		}
		else
		{
#if defined(ANDROID) && !defined(USE_VULKAN)
		if( useInstancing )
		{
			glDrawElementsInstancedBaseVertex( GL_TRIANGLES, r_singleTriangle.GetBool() ? 3 : drawSurf->numIndexes,
					GL_INDEX_TYPE, (triIndex_t*)indexOffset, instanceCount, vertOffset / sizeof( idShadowVert ) );
		}
		else
#endif
		if( !isGLES32Version() )
		{
			glDrawElements( GL_TRIANGLES, r_singleTriangle.GetBool() ? 3 : drawSurf->numIndexes,
				GL_INDEX_TYPE, (triIndex_t*)indexOffset );
		}
		else
		{
			glDrawElementsBaseVertex( GL_TRIANGLES, r_singleTriangle.GetBool() ? 3 : drawSurf->numIndexes,
				GL_INDEX_TYPE, (triIndex_t*)indexOffset, vertOffset / sizeof( idShadowVert ) );
		}
    }
		
		// RB: added stats
		pc.c_shadowElements++;
		#if defined(ANDROID) && !defined(USE_VULKAN)
	pc.c_shadowIndexes += drawSurf->numIndexes * ( useInstancing ? instanceCount : 1 );
#else
	pc.c_shadowIndexes += drawSurf->numIndexes;
#endif
		// RB end
	}

#if defined(ANDROID) && !defined(USE_VULKAN)
	if( useInstancing )
	{
		for( int row = 0; row < 4; ++row )
		{
			glVertexAttribDivisor( mvpAttribs[row], 0 );
			glDisableVertexAttribArray( mvpAttribs[row] );
		}
		glVertexAttribDivisor( lightOriginAttrib, 0 );
		glDisableVertexAttribArray( lightOriginAttrib );
	}
#endif
}

/*
============================================================================

RENDER BACK END THREAD FUNCTIONS

============================================================================
*/

/*
=============
idRenderBackend::DrawFlickerBox
=============
*/
void idRenderBackend::DrawFlickerBox()
{
	if( !r_drawFlickerBox.GetBool() )
	{
		return;
	}
	if( tr.frameCount & 1 )
	{
		glClearColor( 1, 0, 0, 1 );
	}
	else
	{
		glClearColor( 0, 1, 0, 1 );
	}
	glScissor( 0, 0, 256, 256 );
	glClear( GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT );
}

/*
=============
idRenderBackend::SetBuffer
=============
*/
void idRenderBackend::SetBuffer( const void* data )
{
	// see which draw buffer we want to render the frame to
	
	const setBufferCommand_t* cmd = ( const setBufferCommand_t* )data;
	
	RENDERLOG_PRINTF( "---------- RB_SetBuffer ---------- to buffer # %d\n", cmd->buffer );
	
	GL_Scissor( 0, 0, tr.GetWidth(), tr.GetHeight() );

	// The HDR color target is cleared by the first HDR view of the frame, when it starts its
	// render pass. Clearing it here too would be a render pass of its own: on a tile based GPU
	// that writes the cleared target out, only for the view to read it back in.
	// The same goes for the back buffer when a HDR view's tonemap replaces all of it.
	if( skipBackBufferClear )
	{
		return;
	}

	// clear screen for debugging
	// automatically enable this with several other debug tools
	// that might leave unrendered portions of the screen
	if( r_clear.GetFloat() || idStr::Length( r_clear.GetString() ) != 1 || r_singleArea.GetBool() || r_showOverDraw.GetBool() )
	{
		float c[3];
		if( sscanf( r_clear.GetString(), "%f %f %f", &c[0], &c[1], &c[2] ) == 3 )
		{
			if (c[0] > 1.0f || c[1] > 1.0f || c[2] > 1.0f) {
				c[0] = c[0] / 255.0f;
				c[1] = c[1] / 255.0f;
				c[2] = c[2] / 255.0f;
			}
			GL_Clear( true, false, false, 0, c[0], c[1], c[2], 1.0f, false );
		}
		else if( r_clear.GetInteger() == 2 )
		{
			GL_Clear( true, false, false, 0, 0.0f, 0.0f,  0.0f, 1.0f, false );
		}
		else if( r_showOverDraw.GetBool() )
		{
			GL_Clear( true, false, false, 0, 1.0f, 1.0f, 1.0f, 1.0f, false );
		}
		else
		{
			GL_Clear( true, false, false, 0, 0.4f, 0.0f, 0.25f, 1.0f, false );
		}
	}
}

/*
=============
GL_BlockingSwapBuffers

We want to exit this with the GPU idle, right at vsync
=============
*/
void idRenderBackend::GL_BlockingSwapBuffers()
{
	GLimp_SwapBuffers();
	if (gEnableGLSynchronization && glConfig.syncAvailable) {
		int nextIndex = swapIndex ^ 1;
		if (glIsSync(renderSync[nextIndex])) {
			glDeleteSync(renderSync[nextIndex]);
			renderSync[nextIndex] = nullptr;
		}

		renderSync[nextIndex] = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
		swapIndex = nextIndex;
	}
}
/*
=============
idRenderBackend::idRenderBackend
=============
*/
idRenderBackend::idRenderBackend()
{
	memset( glcontext.tmu, 0, sizeof( glcontext.tmu ) );
	memset( glcontext.stencilOperations, 0, sizeof( glcontext.stencilOperations ) );
	glcontext.activeTextureUnit = -1;

	numShadowAtlasLights = 0;
	shadowAtlasSize = 0;
	hdrTargetClearedThisFrame = false;
	skipBackBufferClear = false;
	bloomResultImage = NULL;
#if defined(ANDROID)
	worldMergeSurf = NULL;
	worldMergeFirstIndex = 0;
	worldMergeNumIndexes = 0;
	worldMergeIndexBuffer = 0;

	deferredFrame = false;
	deferredView = false;
	deferredGBufferAttached = false;
	backBufferDrawnThisFrame = false;
	deferredSelfTestResult = 0;
	deferredRenderbuffers[0] = deferredRenderbuffers[1] = deferredRenderbuffers[2] = 0;
	deferredTargetWidth = 0;
	deferredTargetHeight = 0;
	deferredAttachedColor = 0;
	deferredAttachedFBO = 0;
	deferredLightsDeferred = 0;
	deferredLightsForward = 0;
#endif
}

/*
=============
idRenderBackend::~idRenderBackend
=============
*/
idRenderBackend::~idRenderBackend()
{

}

/*
====================
R_MakeStereoRenderImage
====================
*/
static void R_MakeStereoRenderImage( idImage* image )
{
	idImageOpts	opts;
	opts.width = renderSystem->GetWidth();
	opts.height = renderSystem->GetHeight();
	opts.numLevels = 1;
	opts.format = FMT_RGBA8;
	image->AllocImage( opts, TF_LINEAR, TR_CLAMP );
}

/*
====================
idRenderBackend::StereoRenderExecuteBackEndCommands

Renders the draw list twice, with slight modifications for left eye / right eye
====================
*/
void idRenderBackend::StereoRenderExecuteBackEndCommands( const emptyCommand_t* const allCmds )
{
	uint64 backEndStartTime = Sys_Microseconds();
	
	// If we are in a monoscopic context, this draws to the only buffer, and is
	// the same as GL_BACK.  In a quad-buffer stereo context, this is necessary
	// to prevent GL from forcing the rendering to go to both BACK_LEFT and
	// BACK_RIGHT at a performance penalty.
	// To allow stereo deghost processing, the views have to be copied to separate
	// textures anyway, so there isn't any benefit to rendering to BACK_RIGHT for
	// that eye.
#ifndef ANDROID
	glDrawBuffer( GL_BACK_LEFT );
#endif
	// create the stereoRenderImage if we haven't already
	static idImage* stereoRenderImages[2];
	for( int i = 0; i < 2; i++ )
	{
		if( stereoRenderImages[i] == NULL )
		{
			stereoRenderImages[i] = globalImages->ImageFromFunction( va( "_stereoRender%i", i ), R_MakeStereoRenderImage );
		}
		
		// resize the stereo render image if the main window has changed size
		if( stereoRenderImages[i]->GetUploadWidth() != renderSystem->GetWidth() ||
				stereoRenderImages[i]->GetUploadHeight() != renderSystem->GetHeight() )
		{
			stereoRenderImages[i]->Resize( renderSystem->GetWidth(), renderSystem->GetHeight() );
		}
	}
	
	// In stereoRender mode, the front end has generated two RC_DRAW_VIEW commands
	// with slightly different origins for each eye.
	
	// TODO: only do the copy after the final view has been rendered, not mirror subviews?
	
	// Render the 3D draw views from the screen origin so all the screen relative
	// texture mapping works properly, then copy the portion we are going to use
	// off to a texture.
	bool foundEye[2] = { false, false };
	
	for( int stereoEye = 1; stereoEye >= -1; stereoEye -= 2 )
	{
		// set up the target texture we will draw to
		const int targetEye = ( stereoEye == 1 ) ? 1 : 0;
		
		// Set the back end into a known default state to fix any stale render state issues
		GL_SetDefaultState();
		
		renderProgManager.Unbind();
		renderProgManager.ZeroUniforms();
		
		for( const emptyCommand_t* cmds = allCmds; cmds != NULL; cmds = ( const emptyCommand_t* )cmds->next )
		{
			switch( cmds->commandId )
			{
				case RC_NOP:
					break;
				case RC_DRAW_VIEW_GUI:
				case RC_DRAW_VIEW_3D:
				{
					const drawSurfsCommand_t* const dsc = ( const drawSurfsCommand_t* )cmds;
					const viewDef_t&			eyeViewDef = *dsc->viewDef;
					
					if( eyeViewDef.renderView.viewEyeBuffer && eyeViewDef.renderView.viewEyeBuffer != stereoEye )
					{
						// this is the render view for the other eye
						continue;
					}
					
					foundEye[ targetEye ] = true;
					DrawView( dsc, stereoEye );
					if( cmds->commandId == RC_DRAW_VIEW_GUI )
					{
					}
				}
				break;
				case RC_SET_BUFFER:
					SetBuffer( cmds );
					break;
				case RC_COPY_RENDER:
					CopyRender( cmds );
					break;
				case RC_POST_PROCESS:
				{
					postProcessCommand_t* cmd = ( postProcessCommand_t* )cmds;
					if( cmd->viewDef->renderView.viewEyeBuffer != stereoEye )
					{
						break;
					}
					PostProcess( cmds );
				}
				break;
				default:
					common->Error( "RB_ExecuteBackEndCommands: bad commandId" );
					break;
			}
		}
		
		// copy to the target
		stereoRenderImages[ targetEye ]->CopyFramebuffer( 0, 0, renderSystem->GetWidth(), renderSystem->GetHeight() );
	}
	
	// perform the final compositing / warping / deghosting to the actual framebuffer(s)
	assert( foundEye[0] && foundEye[1] );
	
	GL_SetDefaultState();
	
	RB_SetMVP( renderMatrix_identity );
	
	// If we are in quad-buffer pixel format but testing another 3D mode,
	// make sure we draw to both eyes.  This is likely to be sub-optimal
	// performance on most cards and drivers, but it is better than getting
	// a confusing, half-ghosted view.
#ifndef ANDROID
	if( renderSystem->GetStereo3DMode() != STEREO3D_QUAD_BUFFER )
	{
		glDrawBuffer( GL_BACK );
	}
#endif
	
	GL_State( GLS_DEPTHFUNC_ALWAYS | GLS_CULL_TWOSIDED );
	
	// We just want to do a quad pass - so make sure we disable any texgen and
	// set the texture matrix to the identity so we don't get anomalies from
	// any stale uniform data being present from a previous draw call
	const float texS[4] = { 1.0f, 0.0f, 0.0f, 0.0f };
	const float texT[4] = { 0.0f, 1.0f, 0.0f, 0.0f };
	renderProgManager.SetRenderParm( RENDERPARM_TEXTUREMATRIX_S, texS );
	renderProgManager.SetRenderParm( RENDERPARM_TEXTUREMATRIX_T, texT );
	
	// disable any texgen
	const float texGenEnabled[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
	renderProgManager.SetRenderParm( RENDERPARM_TEXGEN_0_ENABLED, texGenEnabled );
	
	renderProgManager.BindShader_Texture();
	GL_Color( 1, 1, 1, 1 );
	
	switch( renderSystem->GetStereo3DMode() )
	{
		case STEREO3D_QUAD_BUFFER:
#ifndef ANDROID
			glDrawBuffer( GL_BACK_RIGHT );
#endif
			GL_SelectTexture( 0 );
			stereoRenderImages[1]->Bind();
			GL_ActivateTextureUnit();
			GL_SelectTexture( 1 );
			stereoRenderImages[0]->Bind();
			GL_ActivateTextureUnit();
			DrawElementsWithCounters( &unitSquareSurface );
#ifndef ANDROID
			glDrawBuffer( GL_BACK_LEFT );
#endif
			GL_SelectTexture( 1 );
			stereoRenderImages[1]->Bind();
			GL_ActivateTextureUnit();
			GL_SelectTexture( 0 );
			stereoRenderImages[0]->Bind();
			GL_ActivateTextureUnit();
			DrawElementsWithCounters( &unitSquareSurface );
			break;
			
		case STEREO3D_HDMI_720:
			// HDMI 720P 3D
			GL_SelectTexture( 0 );
			stereoRenderImages[1]->Bind();
			GL_ActivateTextureUnit();
			GL_SelectTexture( 1 );
			stereoRenderImages[0]->Bind();
			GL_ActivateTextureUnit();
			GL_ViewportAndScissor( 0, 0, 1280, 720 );
			DrawElementsWithCounters( &unitSquareSurface );
			
			GL_SelectTexture( 0 );
			stereoRenderImages[0]->Bind();
			GL_ActivateTextureUnit();
			GL_SelectTexture( 1 );
			stereoRenderImages[1]->Bind();
			GL_ActivateTextureUnit();
			GL_ViewportAndScissor( 0, 750, 1280, 720 );
			DrawElementsWithCounters( &unitSquareSurface );
			
			// force the HDMI 720P 3D guard band to a constant color
			glScissor( 0, 720, 1280, 30 );
			glClear( GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT );
			break;
			
		default:
#ifdef USE_OPENXR
		case STEREO3D_VR: {
			if (xrSystem->IsInitialized()) {
				xrSystem->PollXREvents();
				
				renderProgManager.BindShader_StereoVRWarp();
				glScissor(0, 0, renderSystem->GetWidth(), renderSystem->GetHeight());
				glClearColor(0, 0, 0, 0);
				glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
				idVec4	color(stereoRender_warpCenterX.GetFloat(), stereoRender_warpCenterY.GetFloat(), stereoRender_warpParmZ.GetFloat(), stereoRender_warpParmW.GetFloat());
				renderProgManager.SetRenderParm(RENDERPARM_COLOR, color.ToFloatPtr());
				GL_SelectTexture(0);
				stereoRenderImages[0]->Bind();
				GL_ActivateTextureUnit();
				GL_SelectTexture(1);
				stereoRenderImages[1]->Bind();
				GL_ActivateTextureUnit();
				GLint xrFBO;
				xrSystem->StartFrame();
				glGetIntegerv(GL_FRAMEBUFFER_BINDING, &xrFBO);
				xrSystem->BindSwapchainImage(0);
				GL_ViewportAndScissor(0, 0, xrSystem->GetWidth(), xrSystem->GetHeight());
				DrawElementsWithCounters(&unitSquareSurface);

				xrSystem->RenderFrame(0, 0, xrSystem->GetWidth(), xrSystem->GetHeight());
				xrSystem->ReleaseSwapchainImage();
				if (glConfig.directStateAccess) {
					glBlitNamedFramebuffer(xrFBO, 0, 0, 0, xrSystem->GetWidth(), xrSystem->GetHeight(), 0, 0, renderSystem->GetWidth() / 2, renderSystem->GetHeight(), GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT, GL_NEAREST);
				}
				else {
					glBindFramebuffer(GL_READ_FRAMEBUFFER, xrFBO);
					glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
					glBlitFramebuffer(0, 0, xrSystem->GetWidth(), xrSystem->GetHeight(), 0, 0, renderSystem->GetWidth() / 2, renderSystem->GetHeight(), GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT, GL_NEAREST);
				}
				idVec4	color2(stereoRender_warpCenterX.GetFloat(), stereoRender_warpCenterY.GetFloat(), stereoRender_warpParmZ.GetFloat(), stereoRender_warpParmW.GetFloat());
				renderProgManager.SetRenderParm(RENDERPARM_COLOR, color2.ToFloatPtr());
				glScissor(0, 0, xrSystem->GetWidth(), xrSystem->GetHeight());
				glClearColor(0, 0, 0, 0);
				glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);

				GL_SelectTexture(0);
				stereoRenderImages[1]->Bind();
				GL_ActivateTextureUnit();
				GL_SelectTexture(1);
				stereoRenderImages[0]->Bind();
				GL_ActivateTextureUnit();
				xrSystem->BindSwapchainImage(1);
				GL_ViewportAndScissor(0, 0, xrSystem->GetWidth(), xrSystem->GetHeight());
				DrawElementsWithCounters(&unitSquareSurface);
				xrSystem->RenderFrame(0, 0, xrSystem->GetWidth(), xrSystem->GetHeight());
				xrSystem->ReleaseSwapchainImage();
				if (glConfig.directStateAccess) {
					glBlitNamedFramebuffer(xrFBO, 0, 0, 0, xrSystem->GetWidth(), xrSystem->GetHeight(), renderSystem->GetWidth() / 2, 0, renderSystem->GetWidth(), renderSystem->GetHeight(), GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT, GL_NEAREST);
				}
				else {
					glBindFramebuffer(GL_READ_FRAMEBUFFER, xrFBO);
					glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
					glBlitFramebuffer(0, 0, xrSystem->GetWidth(), xrSystem->GetHeight(), renderSystem->GetWidth() / 2, 0, renderSystem->GetWidth() , renderSystem->GetHeight(), GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT, GL_NEAREST);
				}
				glScissor(0, 0, xrSystem->GetWidth(), xrSystem->GetHeight());
				glClearColor(0, 0, 0, 0);
				glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
				xrSystem->EndFrame();
			}
			break;

		}
#endif
		case STEREO3D_SIDE_BY_SIDE:
			if( stereoRender_warp.GetBool() )
			{
				// this is the Rift warp
				// renderSystem->GetWidth() / GetHeight() have returned equal values (640 for initial Rift)
				// and we are going to warp them onto a symetric square region of each half of the screen
				
				renderProgManager.BindShader_StereoWarp();
				
				// clear the entire screen to black
				// we could be smart and only clear the areas we aren't going to draw on, but
				// clears are fast...
				glScissor( 0, 0, glConfig.nativeScreenWidth, glConfig.nativeScreenHeight );
				glClearColor( 0, 0, 0, 0 );
				glClear( GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT );
				
				// the size of the box that will get the warped pixels
				// With the 7" displays, this will be less than half the screen width
				const int pixelDimensions = ( glConfig.nativeScreenWidth >> 1 ) * stereoRender_warpTargetFraction.GetFloat();
				// Always scissor to the half-screen boundary, but the viewports
				// might cross that boundary if the lenses can be adjusted closer
				// together.
				glViewport( ( glConfig.nativeScreenWidth >> 1 ) - pixelDimensions,
							( glConfig.nativeScreenHeight >> 1 ) - ( pixelDimensions >> 1 ),
							pixelDimensions, pixelDimensions );
				glScissor( 0, 0, glConfig.nativeScreenWidth >> 1, glConfig.nativeScreenHeight );
				
				idVec4	color( stereoRender_warpCenterX.GetFloat(), stereoRender_warpCenterY.GetFloat(), stereoRender_warpParmZ.GetFloat(), stereoRender_warpParmW.GetFloat() );
				// don't use GL_Color(), because we don't want to clamp
				renderProgManager.SetRenderParm( RENDERPARM_COLOR, color.ToFloatPtr() );
				

				GL_SelectTexture( 0 );
				stereoRenderImages[0]->Bind();
				GL_ActivateTextureUnit();
#ifdef ANDROID //karin: using GL_CLAMP_TO_EDGE instead of GL_CLAMP_TO_BORDER in OpenGLES
                glTexParameterf( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
				glTexParameterf( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
#else
				glTexParameterf( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER );
				glTexParameterf( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER );
#endif
				DrawElementsWithCounters( &unitSquareSurface );
				
				idVec4	color2( stereoRender_warpCenterX.GetFloat(), stereoRender_warpCenterY.GetFloat(), stereoRender_warpParmZ.GetFloat(), stereoRender_warpParmW.GetFloat() );
				// don't use GL_Color(), because we don't want to clamp
				renderProgManager.SetRenderParm( RENDERPARM_COLOR, color2.ToFloatPtr() );
				
				glViewport( ( glConfig.nativeScreenWidth >> 1 ),
							( glConfig.nativeScreenHeight >> 1 ) - ( pixelDimensions >> 1 ),
							pixelDimensions, pixelDimensions );
				glScissor( glConfig.nativeScreenWidth >> 1, 0, glConfig.nativeScreenWidth >> 1, glConfig.nativeScreenHeight );
				
				GL_SelectTexture( 0 );
				stereoRenderImages[1]->Bind();
				GL_ActivateTextureUnit();
#ifdef ANDROID //karin: using GL_CLAMP_TO_EDGE instead of GL_CLAMP_TO_BORDER in OpenGLES
                glTexParameterf( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
				glTexParameterf( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
#else
				glTexParameterf( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER );
				glTexParameterf( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER );
#endif
				DrawElementsWithCounters( &unitSquareSurface );
				break;
			}
			
		// a non-warped side-by-side-uncompressed (dual input cable) is rendered
		// just like STEREO3D_SIDE_BY_SIDE_COMPRESSED, so fall through.
		case STEREO3D_SIDE_BY_SIDE_COMPRESSED:

			GL_SelectTexture( 0 );
			stereoRenderImages[0]->Bind();
			GL_ActivateTextureUnit();
			GL_SelectTexture( 1 );
			stereoRenderImages[1]->Bind();
			GL_ActivateTextureUnit();
			GL_ViewportAndScissor( 0, 0, renderSystem->GetWidth(), renderSystem->GetHeight() );
			DrawElementsWithCounters( &unitSquareSurface );

			GL_SelectTexture( 0 );
			stereoRenderImages[1]->Bind();
			GL_ActivateTextureUnit();
			GL_SelectTexture( 1 );
			stereoRenderImages[0]->Bind();
			GL_ActivateTextureUnit();
			GL_ViewportAndScissor( renderSystem->GetWidth(), 0, renderSystem->GetWidth(), renderSystem->GetHeight() );
			DrawElementsWithCounters( &unitSquareSurface );
			break;
			
		case STEREO3D_TOP_AND_BOTTOM_COMPRESSED:
			GL_SelectTexture( 1 );
			stereoRenderImages[0]->Bind();
			GL_ActivateTextureUnit();
			GL_SelectTexture( 0 );
			stereoRenderImages[1]->Bind();
			GL_ActivateTextureUnit();
			GL_ViewportAndScissor( 0, 0, renderSystem->GetWidth(), renderSystem->GetHeight() );
			DrawElementsWithCounters( &unitSquareSurface );
			
			GL_SelectTexture( 1 );
			stereoRenderImages[1]->Bind();
			GL_ActivateTextureUnit();
			GL_SelectTexture( 0 );
			stereoRenderImages[0]->Bind();
			GL_ActivateTextureUnit();
			GL_ViewportAndScissor( 0, renderSystem->GetHeight(), renderSystem->GetWidth(), renderSystem->GetHeight() );
			DrawElementsWithCounters( &unitSquareSurface );
			break;
			
		case STEREO3D_INTERLACED:
			// every other scanline
			GL_SelectTexture( 0 );
			stereoRenderImages[0]->Bind();
			GL_ActivateTextureUnit();
			glTexParameterf( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST );
			glTexParameterf( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST );
			
			GL_SelectTexture( 1 );
			stereoRenderImages[1]->Bind();
			GL_ActivateTextureUnit();
			glTexParameterf( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST );
			glTexParameterf( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST );
			
			GL_ViewportAndScissor( 0, 0, renderSystem->GetWidth(), renderSystem->GetHeight() * 2 );
			renderProgManager.BindShader_StereoInterlace();
			DrawElementsWithCounters( &unitSquareSurface );
			
			GL_SelectTexture( 0 );
			GL_ActivateTextureUnit();
			glTexParameterf( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
			glTexParameterf( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
			
			GL_SelectTexture( 1 );
			GL_ActivateTextureUnit();
			glTexParameterf( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
			glTexParameterf( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
			
			break;
	}
	
	// debug tool
	DrawFlickerBox();

#ifndef ANDROID
	// make sure the drawing is actually started
	glFlush();
#endif
	// we may choose to sync to the swapbuffers before the next frame
	
	// stop rendering on this thread
	uint64 backEndFinishTime = Sys_Microseconds();
	pc.totalMicroSec = backEndFinishTime - backEndStartTime;
}


