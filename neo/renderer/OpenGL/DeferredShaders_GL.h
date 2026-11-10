/*
===========================================================================

Doom 3 BFG Edition GPL Source Code
Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company.

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

===========================================================================
*/

#ifndef __DEFERREDSHADERS_GL_H__
#define __DEFERREDSHADERS_GL_H__

/*
================================================================================================

TBDR: ON-TILE DEFERRED SHADING

The programs of the on-tile deferred shading are GLSL ES 3.20 written here, not HLSL in the game
data: the light programs read the G-buffer with GL_EXT_shader_framebuffer_fetch, which the HLSL
conversion can't express, and the renderprogs on the device can be older than the engine.

The G-buffer lives next to the scene color in the render pass of the view and never leaves the
tile memory, all RGBA8:
	color 0		scene color, the ambient pass and the lights add to it
	color 1		rgb albedo, a specular red			sqrt encoded
	color 2		rgb view space normal, octahedral 12 + 12 bits, a specular green
	color 3		rgb linear view depth, 24 bits, a specular blue

The albedo and the specular color are what the interaction programs multiply the light with:
the diffuse and specular maps, the color of the material stage and the vertex color. A light
adds the same as its interactions add, for the pixels of the light volume.

================================================================================================
*/

// the linear view depth the 24 bits of the G-buffer cover, beyond any map
#define DEFERRED_DEPTH_RANGE_STRING		"131072.0"

static const char* const deferredShaderCommon =
	"precision highp float;\n"
	"precision highp int;\n"
	"\n"
	"#define PI 3.14159265358979323846\n"
	"#define DEFERRED_DEPTH_RANGE " DEFERRED_DEPTH_RANGE_STRING "\n"
	"\n";

static const char* const deferredShaderColorFunctions =
	"float LinearValue( float u )\n"
	"{\n"
	"	return ( u <= 0.04045 ) ? u / 12.92 : pow( ( u + 0.055 ) / 1.055, 2.4 );\n"
	"}\n"
	"\n"
	"vec3 sRGBToLinearRGB( vec3 rgb )\n"
	"{\n"
	"#if defined( USE_LINEAR_RGB )\n"
	"	return vec3( LinearValue( rgb.r ), LinearValue( rgb.g ), LinearValue( rgb.b ) );\n"
	"#else\n"
	"	return rgb;\n"
	"#endif\n"
	"}\n"
	"\n";

static const char* const deferredShaderPackFunctions =
	"vec2 OctahedronSigns( vec2 v )\n"
	"{\n"
	"	return vec2( v.x >= 0.0 ? 1.0 : -1.0, v.y >= 0.0 ? 1.0 : -1.0 );\n"
	"}\n"
	"\n"
	// a unit vector to 12 + 12 bits in three bytes
	"vec3 PackNormal( vec3 n )\n"
	"{\n"
	"	n /= ( abs( n.x ) + abs( n.y ) + abs( n.z ) );\n"
	"	vec2 e = ( n.z >= 0.0 ) ? n.xy : ( 1.0 - abs( n.yx ) ) * OctahedronSigns( n.xy );\n"
	"	uvec2 q = uvec2( clamp( e * 0.5 + 0.5, 0.0, 1.0 ) * 4095.0 + 0.5 );\n"
	"	q = min( q, uvec2( 4095u ) );\n"
	"	uint b0 = q.x >> 4u;\n"
	"	uint b1 = ( ( q.x & 15u ) << 4u ) | ( q.y >> 8u );\n"
	"	uint b2 = q.y & 255u;\n"
	"	return vec3( float( b0 ), float( b1 ), float( b2 ) ) * ( 1.0 / 255.0 );\n"
	"}\n"
	"\n"
	"vec3 UnpackNormal( vec3 p )\n"
	"{\n"
	"	uvec3 b = uvec3( clamp( p, 0.0, 1.0 ) * 255.0 + 0.5 );\n"
	"	vec2 e = vec2( float( ( b.x << 4u ) | ( b.y >> 4u ) ), float( ( ( b.y & 15u ) << 8u ) | b.z ) ) * ( 2.0 / 4095.0 ) - 1.0;\n"
	"	vec3 n = vec3( e, 1.0 - abs( e.x ) - abs( e.y ) );\n"
	"	float t = max( -n.z, 0.0 );\n"
	"	n.x += ( n.x >= 0.0 ) ? -t : t;\n"
	"	n.y += ( n.y >= 0.0 ) ? -t : t;\n"
	"	return normalize( n );\n"
	"}\n"
	"\n"
	// linear view depth to 24 bits in three bytes
	"vec3 PackDepth( float viewDepth )\n"
	"{\n"
	"	uint u = uint( clamp( viewDepth * ( 1.0 / DEFERRED_DEPTH_RANGE ), 0.0, 1.0 ) * 16777215.0 + 0.5 );\n"
	"	u = min( u, 16777215u );\n"
	"	return vec3( float( u >> 16u ), float( ( u >> 8u ) & 255u ), float( u & 255u ) ) * ( 1.0 / 255.0 );\n"
	"}\n"
	"\n"
	"float UnpackDepth( vec3 p )\n"
	"{\n"
	"	uvec3 b = uvec3( clamp( p, 0.0, 1.0 ) * 255.0 + 0.5 );\n"
	"	return float( ( b.x << 16u ) | ( b.y << 8u ) | b.z ) * ( DEFERRED_DEPTH_RANGE / 16777215.0 );\n"
	"}\n"
	"\n";

/*
================================================================================================
G-buffer

Draws the surfaces of the depth pass with depth equal, one interaction each, and writes the
ambient pass (AmbientPass, ambient_lighting) of the same surfaces into the scene color.
================================================================================================
*/

static const renderParm_t deferredGBufferVertexParms[] =
{
	RENDERPARM_MVPMATRIX_X,
	RENDERPARM_MVPMATRIX_Y,
	RENDERPARM_MVPMATRIX_Z,
	RENDERPARM_MVPMATRIX_W,
	RENDERPARM_MODELVIEWMATRIX_X,
	RENDERPARM_MODELVIEWMATRIX_Y,
	RENDERPARM_MODELVIEWMATRIX_Z,
	RENDERPARM_MODELVIEWMATRIX_W,
	RENDERPARM_LOCALVIEWORIGIN,
	RENDERPARM_BUMPMATRIX_S,
	RENDERPARM_BUMPMATRIX_T,
	RENDERPARM_DIFFUSEMATRIX_S,
	RENDERPARM_DIFFUSEMATRIX_T,
	RENDERPARM_SPECULARMATRIX_S,
	RENDERPARM_SPECULARMATRIX_T,
	RENDERPARM_VERTEXCOLOR_MODULATE,
	RENDERPARM_VERTEXCOLOR_ADD,
};

static const char* const deferredGBufferVertexShader =
	"in vec4 in_Position;\n"
	"in vec2 in_TexCoord;\n"
	"in vec4 in_Normal;\n"
	"in vec4 in_Tangent;\n"
	"in vec4 in_Color;\n"
	"in vec4 in_Color2;\n"
	"#if defined( USE_GPU_INSTANCING )\n"
	"in vec4 in_InstanceMVPX;\n"
	"in vec4 in_InstanceMVPY;\n"
	"in vec4 in_InstanceMVPZ;\n"
	"in vec4 in_InstanceMVPW;\n"
	"in vec4 in_InstanceModelViewX;\n"
	"in vec4 in_InstanceModelViewY;\n"
	"in vec4 in_InstanceModelViewZ;\n"
	"in vec4 in_InstanceModelViewW;\n"
	"in vec4 in_InstanceLocalViewOrigin;\n"
	"#endif\n"
	"#if defined( USE_GPU_SKINNING )\n"
	"uniform matrices_ubo { vec4 matrices[408]; };\n"
	"#endif\n"
	"\n"
	"out highp vec4 vofi_TexCoordBumpDiffuse;\n"
	"out highp vec2 vofi_TexCoordSpecular;\n"
	"out mediump vec3 vofi_Tangent;\n"
	"out mediump vec3 vofi_Bitangent;\n"
	"out mediump vec3 vofi_Normal;\n"
	"out highp float vofi_ViewDepth;\n"
	"out mediump vec4 vofi_Color;\n"
	"out mediump vec3 vofi_AmbientHalfAngle;\n"
	"out mediump vec3 vofi_AmbientLight;\n"
	"#if defined( FORWARD_LIGHTS )\n"
	"out highp vec3 vofi_ViewPosition;\n"
	"#endif\n"
	"\n"
	"void main()\n"
	"{\n"
	"	vec4 vNormal = in_Normal * 2.0 - 1.0;\n"
	"	vec4 vTangent = in_Tangent * 2.0 - 1.0;\n"
	"	vec3 vBitangent = cross( vNormal.xyz, vTangent.xyz ) * vTangent.w;\n"
	"\n"
	"#if defined( USE_GPU_SKINNING )\n"
	"	float w0 = in_Color2.x;\n"
	"	float w1 = in_Color2.y;\n"
	"	float w2 = in_Color2.z;\n"
	"	float w3 = in_Color2.w;\n"
	"\n"
	"	vec4 matX, matY, matZ;\n"
	"	int joint = int( in_Color.x * 255.1 * 3.0 );\n"
	"	matX = matrices[ joint + 0 ] * w0;\n"
	"	matY = matrices[ joint + 1 ] * w0;\n"
	"	matZ = matrices[ joint + 2 ] * w0;\n"
	"\n"
	"	joint = int( in_Color.y * 255.1 * 3.0 );\n"
	"	matX += matrices[ joint + 0 ] * w1;\n"
	"	matY += matrices[ joint + 1 ] * w1;\n"
	"	matZ += matrices[ joint + 2 ] * w1;\n"
	"\n"
	"	joint = int( in_Color.z * 255.1 * 3.0 );\n"
	"	matX += matrices[ joint + 0 ] * w2;\n"
	"	matY += matrices[ joint + 1 ] * w2;\n"
	"	matZ += matrices[ joint + 2 ] * w2;\n"
	"\n"
	"	joint = int( in_Color.w * 255.1 * 3.0 );\n"
	"	matX += matrices[ joint + 0 ] * w3;\n"
	"	matY += matrices[ joint + 1 ] * w3;\n"
	"	matZ += matrices[ joint + 2 ] * w3;\n"
	"\n"
	"	vec3 normal = normalize( vec3( dot( matX.xyz, vNormal.xyz ), dot( matY.xyz, vNormal.xyz ), dot( matZ.xyz, vNormal.xyz ) ) );\n"
	"	vec3 tangent = normalize( vec3( dot( matX.xyz, vTangent.xyz ), dot( matY.xyz, vTangent.xyz ), dot( matZ.xyz, vTangent.xyz ) ) );\n"
	"	vec3 bitangent = normalize( vec3( dot( matX.xyz, vBitangent ), dot( matY.xyz, vBitangent ), dot( matZ.xyz, vBitangent ) ) );\n"
	"	vec4 modelPosition = vec4( dot( matX, in_Position ), dot( matY, in_Position ), dot( matZ, in_Position ), 1.0 );\n"
	"#else\n"
	"	vec4 modelPosition = in_Position;\n"
	"	vec3 normal = vNormal.xyz;\n"
	"	vec3 tangent = vTangent.xyz;\n"
	"	vec3 bitangent = vBitangent;\n"
	"#endif\n"
	"\n"
	"#if defined( USE_GPU_INSTANCING )\n"
	"	gl_Position = vec4( dot( modelPosition, in_InstanceMVPX ), dot( modelPosition, in_InstanceMVPY ), dot( modelPosition, in_InstanceMVPZ ), dot( modelPosition, in_InstanceMVPW ) );\n"
	"	vec4 modelViewX = in_InstanceModelViewX;\n"
	"	vec4 modelViewY = in_InstanceModelViewY;\n"
	"	vec4 modelViewZ = in_InstanceModelViewZ;\n"
	"	vec4 localViewOrigin = in_InstanceLocalViewOrigin;\n"
	"#else\n"
	"	gl_Position = vec4( dot( modelPosition, rpMVPmatrixX ), dot( modelPosition, rpMVPmatrixY ), dot( modelPosition, rpMVPmatrixZ ), dot( modelPosition, rpMVPmatrixW ) );\n"
	"	vec4 modelViewX = rpModelViewMatrixX;\n"
	"	vec4 modelViewY = rpModelViewMatrixY;\n"
	"	vec4 modelViewZ = rpModelViewMatrixZ;\n"
	"	vec4 localViewOrigin = rpLocalViewOrigin;\n"
	"#endif\n"
	"\n"
	"	vec4 texCoord = vec4( in_TexCoord.xy, 0.0, 1.0 );\n"
	"	vofi_TexCoordBumpDiffuse = vec4( dot( texCoord, rpBumpMatrixS ), dot( texCoord, rpBumpMatrixT ), dot( texCoord, rpDiffuseMatrixS ), dot( texCoord, rpDiffuseMatrixT ) );\n"
	"	vofi_TexCoordSpecular = vec2( dot( texCoord, rpSpecularMatrixS ), dot( texCoord, rpSpecularMatrixT ) );\n"
	"\n"
	"	// the tangent space in view space, the light programs shade in view space\n"
	"	vofi_Tangent = vec3( dot( modelViewX.xyz, tangent ), dot( modelViewY.xyz, tangent ), dot( modelViewZ.xyz, tangent ) );\n"
	"	vofi_Bitangent = vec3( dot( modelViewX.xyz, bitangent ), dot( modelViewY.xyz, bitangent ), dot( modelViewZ.xyz, bitangent ) );\n"
	"	vofi_Normal = vec3( dot( modelViewX.xyz, normal ), dot( modelViewY.xyz, normal ), dot( modelViewZ.xyz, normal ) );\n"
	"\n"
	"	// the depth hacks of the weapon change the depth buffer, not this\n"
	"	vofi_ViewDepth = -dot( modelPosition, modelViewZ );\n"
	"#if defined( FORWARD_LIGHTS )\n"
	"	vofi_ViewPosition = vec3( dot( modelPosition, modelViewX ), dot( modelPosition, modelViewY ), dot( modelPosition, modelViewZ ) );\n"
	"#endif\n"
	"\n"
	"	// the ambient pass lights from a fixed direction in model space, see ambient_lighting.vs\n"
	"	const vec3 ambientLight = vec3( 0.0, 0.5, 1.0 ) / 1.5;\n"
	"	vec3 toView = normalize( localViewOrigin.xyz - modelPosition.xyz );\n"
	"	vec3 halfAngle = ambientLight + toView;\n"
	"	vofi_AmbientHalfAngle = vec3( dot( tangent, halfAngle ), dot( bitangent, halfAngle ), dot( normal, halfAngle ) );\n"
	"	vofi_AmbientLight = vec3( dot( tangent, ambientLight ), dot( bitangent, ambientLight ), dot( normal, ambientLight ) );\n"
	"\n"
	"#if defined( USE_GPU_SKINNING )\n"
	"	vofi_Color = vec4( 1.0 );\n"
	"#else\n"
	"	vofi_Color = in_Color * rpVertexColorModulate + rpVertexColorAdd;\n"
	"#endif\n"
	"}\n";

static const renderParm_t deferredGBufferFragmentParms[] =
{
	RENDERPARM_DIFFUSEMODIFIER,		// color of the diffuse stage
	RENDERPARM_SPECULARMODIFIER,	// color of the specular stage
	RENDERPARM_AMBIENT_COLOR,
	RENDERPARM_USER3,				// x r_lightScale, y 1 when the ambient pass is written
};

static const char* const deferredGBufferFragmentShader =
	"uniform mediump sampler2D samp0;	// bump map\n"
	"uniform mediump sampler2D samp1;	// specular map\n"
	"uniform mediump sampler2D samp2;	// diffuse map, YCoCg\n"
	"uniform float " TEXTURE_LOD_BIAS_UNIFORM_NAME ";\n"
	"\n"
	"in highp vec4 vofi_TexCoordBumpDiffuse;\n"
	"in highp vec2 vofi_TexCoordSpecular;\n"
	"in mediump vec3 vofi_Tangent;\n"
	"in mediump vec3 vofi_Bitangent;\n"
	"in mediump vec3 vofi_Normal;\n"
	"in highp float vofi_ViewDepth;\n"
	"in mediump vec4 vofi_Color;\n"
	"in mediump vec3 vofi_AmbientHalfAngle;\n"
	"in mediump vec3 vofi_AmbientLight;\n"
	"\n"
	"layout( location = 0 ) out mediump vec4 fo_Ambient;\n"
	"layout( location = 1 ) out mediump vec4 fo_GBuffer0;\n"
	"layout( location = 2 ) out mediump vec4 fo_GBuffer1;\n"
	"layout( location = 3 ) out mediump vec4 fo_GBuffer2;\n"
	"\n"
	"vec3 ConvertYCoCgToRGB( vec4 YCoCg )\n"
	"{\n"
	"	YCoCg.z = ( YCoCg.z * 31.875 ) + 1.0;\n"
	"	YCoCg.z = 1.0 / YCoCg.z;\n"
	"	YCoCg.xy *= YCoCg.z;\n"
	"	vec3 rgbColor;\n"
	"	rgbColor.x = dot( YCoCg, vec4( 1.0, -1.0, 0.0, 1.0 ) );\n"
	"	rgbColor.y = dot( YCoCg, vec4( 0.0, 1.0, -0.50196078, 1.0 ) );\n"
	"	rgbColor.z = dot( YCoCg, vec4( -1.0, -1.0, 1.00392156, 1.0 ) );\n"
	"	return rgbColor;\n"
	"}\n"
	"\n"
	"void main()\n"
	"{\n"
	"	mediump vec4 bumpMap = texture( samp0, vofi_TexCoordBumpDiffuse.xy, " TEXTURE_LOD_BIAS_UNIFORM_NAME " );\n"
	"	mediump vec4 YCoCG = texture( samp2, vofi_TexCoordBumpDiffuse.zw, " TEXTURE_LOD_BIAS_UNIFORM_NAME " );\n"
	"	mediump vec4 specMapSRGB = texture( samp1, vofi_TexCoordSpecular, " TEXTURE_LOD_BIAS_UNIFORM_NAME " );\n"
	"\n"
	"	vec3 diffuseMap = sRGBToLinearRGB( ConvertYCoCgToRGB( YCoCG ) );\n"
	"	vec3 specMap = sRGBToLinearRGB( specMapSRGB.rgb );\n"
	"\n"
	"	vec3 localNormal;\n"
	"	localNormal.xy = bumpMap.wy - 0.5;\n"
	"	localNormal.z = sqrt( abs( dot( localNormal.xy, localNormal.xy ) - 0.25 ) );\n"
	"	localNormal = normalize( localNormal );\n"
	"\n"
	"	vec3 normal = localNormal.x * vofi_Tangent + localNormal.y * vofi_Bitangent + localNormal.z * vofi_Normal;\n"
	"	float normalLengthSqr = dot( normal, normal );\n"
	"	normal = ( normalLengthSqr > 1e-12 ) ? normal * inversesqrt( normalLengthSqr ) : vec3( 0.0, 0.0, 1.0 );\n"
	"\n"
	"	// everything the interaction programs multiply the light with but the light itself\n"
	"	vec3 albedo = clamp( diffuseMap * sRGBToLinearRGB( rpDiffuseModifier.rgb ) * vofi_Color.rgb, 0.0, 1.0 );\n"
	"	vec3 specular = clamp( specMap * sRGBToLinearRGB( rpSpecularModifier.rgb ) * vofi_Color.rgb, 0.0, 1.0 );\n"
	"	specular = sqrt( specular );\n"
	"\n"
	"	fo_GBuffer0 = vec4( sqrt( albedo ), specular.r );\n"
	"	fo_GBuffer1 = vec4( PackNormal( normal ), specular.g );\n"
	"	fo_GBuffer2 = vec4( PackDepth( vofi_ViewDepth ), specular.b );\n"
	"\n"
	"	fo_Ambient = vec4( 0.0 );\n"
	"	if( rpUser3.y > 0.0 )\n"
	"	{\n"
	"		// AmbientPass, see ambient_lighting.ps\n"
	"		float hDotN = dot( normalize( vofi_AmbientHalfAngle ), localNormal );\n"
	"		float specularBase = abs( hDotN );\n"
	"		float specularBase2 = specularBase * specularBase;\n"
	"		float specularBase4 = specularBase2 * specularBase2;\n"
	"		float specularBase8 = specularBase4 * specularBase4;\n"
	"		vec3 specularContribution = vec3( specularBase8 * specularBase2 );\n"
	"\n"
	"		vec3 diffuseColor = diffuseMap * ( rpDiffuseModifier.rgb * rpUser3.x ) * 1.5;\n"
	"		vec3 specularColor = specMap * specularContribution * ( rpSpecularModifier.rgb * ( rpUser3.x * 2.0 ) );\n"
	"\n"
	"		float halfLdotN = 1.0;\n"
	"#if defined( USE_HALF_LAMBERT )\n"
	"		halfLdotN = dot( localNormal, normalize( vofi_AmbientLight ) ) * 0.5 + 0.5;\n"
	"		halfLdotN *= halfLdotN;\n"
	"#endif\n"
	"\n"
	"		vec3 lightColor = sRGBToLinearRGB( rpAmbientColor.rgb );\n"
	"\n"
	"		float rim = 1.0 - clamp( hDotN, 0.0, 1.0 );\n"
	"		float rim2 = rim * rim;\n"
	"		float rim4 = rim2 * rim2;\n"
	"		float rim8 = rim4 * rim4;\n"
	"		vec3 rimColor = sRGBToLinearRGB( vec3( 0.125 * 1.2 ) ) * lightColor * rim8;\n"
	"\n"
	"		fo_Ambient.rgb = ( ( diffuseColor + specularColor ) * halfLdotN * lightColor + rimColor ) * vofi_Color.rgb;\n"
	"		fo_Ambient.a = vofi_Color.a;\n"
	"	}\n"
	"}\n";

/*
================================================================================================
Lights

The light volume is drawn with depth greater equal: only the pixels of the surfaces in front
of its back side. Everything is in view space, DrawDeferredLight moves the light there.
================================================================================================
*/

static const renderParm_t deferredLightVertexParms[] =
{
	RENDERPARM_MVPMATRIX_X,
	RENDERPARM_MVPMATRIX_Y,
	RENDERPARM_MVPMATRIX_Z,
	RENDERPARM_MVPMATRIX_W,
};

static const char* const deferredLightVertexShader =
	"in vec4 in_Position;\n"
	"\n"
	"void main()\n"
	"{\n"
	"	gl_Position = vec4( dot( in_Position, rpMVPmatrixX ), dot( in_Position, rpMVPmatrixY ), dot( in_Position, rpMVPmatrixZ ), dot( in_Position, rpMVPmatrixW ) );\n"
	"}\n";

static const renderParm_t deferredLightFragmentParms[] =
{
	RENDERPARM_USER0,				// window coordinates to normalized device coordinates
	RENDERPARM_USER1,				// 1 / P00, P02, P03, P32 of the projection
	RENDERPARM_USER2,				// 1 / P11, P12, P13, P33 of the projection
	RENDERPARM_LOCALLIGHTORIGIN,	// view space
	RENDERPARM_LIGHTPROJECTION_S,	// view space, with the texture matrix of the light stage
	RENDERPARM_LIGHTPROJECTION_T,
	RENDERPARM_LIGHTPROJECTION_Q,
	RENDERPARM_LIGHTFALLOFF_S,
	RENDERPARM_TEXGEN_0_S,			// view space to the 0..1 cube of the light volume
	RENDERPARM_TEXGEN_0_T,
	RENDERPARM_TEXGEN_0_Q,
	RENDERPARM_TEXGEN_0_ENABLED,
	RENDERPARM_DIFFUSEMODIFIER,		// light color
	RENDERPARM_SPECULARMODIFIER,	// light color * 2
};

static const renderParm_t deferredLightShadowFragmentParms[] =
{
	RENDERPARM_USER0,
	RENDERPARM_USER1,
	RENDERPARM_USER2,
	RENDERPARM_LOCALLIGHTORIGIN,
	RENDERPARM_LIGHTPROJECTION_S,
	RENDERPARM_LIGHTPROJECTION_T,
	RENDERPARM_LIGHTPROJECTION_Q,
	RENDERPARM_LIGHTFALLOFF_S,
	RENDERPARM_TEXGEN_0_S,
	RENDERPARM_TEXGEN_0_T,
	RENDERPARM_TEXGEN_0_Q,
	RENDERPARM_TEXGEN_0_ENABLED,
	RENDERPARM_DIFFUSEMODIFIER,
	RENDERPARM_SPECULARMODIFIER,
	RENDERPARM_SCREENCORRECTIONFACTOR,
	RENDERPARM_JITTERTEXSCALE,
	RENDERPARM_JITTERTEXOFFSET,
	RENDERPARM_CASCADEDISTANCES,
	RENDERPARM_MODELMATRIX_X,		// view space to world space
	RENDERPARM_MODELMATRIX_Y,
	RENDERPARM_MODELMATRIX_Z,
	RENDERPARM_SHADOW_MATRIX_0_X,	// rpShadowMatrices[6 * 4], view space to the shadow maps, must be the last
};

static const char* const deferredLightFragmentShader =
	"layout( location = 0 ) inout mediump vec4 fb_Color;\n"
	"layout( location = 1 ) inout highp vec4 fb_GBuffer0;\n"
	"layout( location = 2 ) inout highp vec4 fb_GBuffer1;\n"
	"layout( location = 3 ) inout highp vec4 fb_GBuffer2;\n"
	"\n"
	"uniform mediump sampler2D samp3;	// light falloff\n"
	"uniform mediump sampler2D samp4;	// light projection\n"
	"#if defined( SHADOW_MAPPING )\n"
	"uniform mediump sampler2DArrayShadow samp5;	// shadow maps\n"
	"uniform mediump sampler2D samp6;	// jitter\n"
	"\n"
	// interactionSM.ps with TBR_MOBILE
	"float Shadow( vec4 position, float viewDepth, vec3 toLight )\n"
	"{\n"
	"	int shadowIndex = 0;\n"
	"#if defined( LIGHT_POINT )\n"
	"	vec3 toLightGlobal = vec3( dot( rpModelMatrixX.xyz, toLight ), dot( rpModelMatrixY.xyz, toLight ), dot( rpModelMatrixZ.xyz, toLight ) );\n"
	"	vec3 absToLightGlobal = abs( toLightGlobal );\n"
	"	if( absToLightGlobal.x >= absToLightGlobal.y && absToLightGlobal.x >= absToLightGlobal.z )\n"
	"	{\n"
	"		shadowIndex = ( toLightGlobal.x > 0.0 ) ? 1 : 0;\n"
	"	}\n"
	"	else if( absToLightGlobal.y >= absToLightGlobal.z )\n"
	"	{\n"
	"		shadowIndex = ( toLightGlobal.y > 0.0 ) ? 3 : 2;\n"
	"	}\n"
	"	else\n"
	"	{\n"
	"		shadowIndex = ( toLightGlobal.z > 0.0 ) ? 5 : 4;\n"
	"	}\n"
	"#elif defined( LIGHT_PARALLEL )\n"
	"	shadowIndex = int( viewDepth >= rpCascadeDistances.x ) + int( viewDepth >= rpCascadeDistances.y )\n"
	"				+ int( viewDepth >= rpCascadeDistances.z ) + int( viewDepth >= rpCascadeDistances.w );\n"
	"#endif\n"
	"\n"
	"	vec4 shadowTexcoord;\n"
	"	shadowTexcoord.x = dot( position, rpShadowMatrices( shadowIndex * 4 + 0 ) );\n"
	"	shadowTexcoord.y = dot( position, rpShadowMatrices( shadowIndex * 4 + 1 ) );\n"
	"	shadowTexcoord.z = dot( position, rpShadowMatrices( shadowIndex * 4 + 2 ) );\n"
	"	shadowTexcoord.w = dot( position, rpShadowMatrices( shadowIndex * 4 + 3 ) );\n"
	"	shadowTexcoord.xyz /= shadowTexcoord.w;\n"
	"	shadowTexcoord.z = shadowTexcoord.z * rpScreenCorrectionFactor.w;\n"
	"	shadowTexcoord.w = float( shadowIndex );\n"
	"\n"
	"	const vec2 poissonDisk[4] = vec2[](\n"
	"		vec2( 0.6111618, 0.1050905 ),\n"
	"		vec2( 0.3030421, -0.6292974 ),\n"
	"		vec2( -0.6411021, -0.4748057 ),\n"
	"		vec2( -0.1314865, 0.8404058 ) );\n"
	"\n"
	// main returned for some pixels already, implicit derivatives are undefined here
	"	float shadow = 4.0 / 12.0;\n"
	"	vec2 jitterTC = gl_FragCoord.xy * rpScreenCorrectionFactor.xy + rpJitterTexOffset.xy;\n"
	"	float randomAngle = textureLod( samp6, jitterTC, 0.0 ).x * PI;\n"
	"	vec2 rot = vec2( cos( randomAngle ), sin( randomAngle ) );\n"
	"	float shadowTexelSize = rpScreenCorrectionFactor.z * rpJitterTexScale.x;\n"
	"	for( int i = 0; i < 4; i++ )\n"
	"	{\n"
	"		vec2 jitter = poissonDisk[i];\n"
	"		vec2 jitterRotated = vec2( jitter.x * rot.x - jitter.y * rot.y, jitter.x * rot.y + jitter.y * rot.x );\n"
	"		shadow += textureGrad( samp5, vec4( shadowTexcoord.xy + jitterRotated * shadowTexelSize, shadowTexcoord.w, shadowTexcoord.z ), vec2( 0.0 ), vec2( 0.0 ) );\n"
	"	}\n"
	"	return shadow * 0.25;\n"
	"}\n"
	"#endif\n"
	"\n"
	"void main()\n"
	"{\n"
	"	vec4 gbuffer0 = fb_GBuffer0;\n"
	"	vec4 gbuffer1 = fb_GBuffer1;\n"
	"	vec4 gbuffer2 = fb_GBuffer2;\n"
	"\n"
	"	vec3 albedo = gbuffer0.rgb * gbuffer0.rgb;\n"
	"	vec3 specular = vec3( gbuffer0.a, gbuffer1.a, gbuffer2.a );\n"
	"	specular *= specular;\n"
	"\n"
	"	// sky, surfaces without lighting and the ones the interactions light\n"
	"	if( max( max( max( albedo.r, albedo.g ), max( albedo.b, specular.r ) ), max( specular.g, specular.b ) ) <= 0.0 )\n"
	"	{\n"
	"		return;\n"
	"	}\n"
	"\n"
	"	float viewDepth = UnpackDepth( gbuffer2.rgb );\n"
	"	vec2 ndc = gl_FragCoord.xy * rpUser0.xy + rpUser0.zw;\n"
	"	float z = -viewDepth;\n"
	"	float w = rpUser1.w * z + rpUser2.w;\n"
	"	vec4 position = vec4( ( ndc.x * w - rpUser1.y * z - rpUser1.z ) * rpUser1.x,\n"
	"						  ( ndc.y * w - rpUser2.y * z - rpUser2.z ) * rpUser2.x, z, 1.0 );\n"
	"\n"
	"	// the interactions are culled to the light volume\n"
	"	vec4 volume = vec4( dot( position, rpTexGen0S ), dot( position, rpTexGen0T ), dot( position, rpTexGen0Q ), dot( position, rpTexGen0Enabled ) );\n"
	"	if( volume.w <= 0.0 || any( lessThan( volume.xyz, vec3( 0.0 ) ) ) || any( greaterThan( volume.xyz, vec3( volume.w ) ) ) )\n"
	"	{\n"
	"		return;\n"
	"	}\n"
	"\n"
	"	vec3 lightProjection = vec3( dot( position, rpLightProjectionS ), dot( position, rpLightProjectionT ), dot( position, rpLightProjectionQ ) );\n"
	"	float falloff = dot( position, rpLightFalloffS );\n"
	"	mediump vec4 lightFalloff = textureLod( samp3, vec2( falloff, 0.5 ), 0.0 );\n"
	"	mediump vec4 lightProj = textureLod( samp4, lightProjection.xy / lightProjection.z, 0.0 );\n"
	"\n"
	"#if defined( LIGHT_AMBIENT )\n"
	"	// interactionAmbient.ps, the specular map of an ambient light is black\n"
	"	vec3 color = albedo * sRGBToLinearRGB( rpDiffuseModifier.rgb ) * lightProj.rgb * lightFalloff.rgb;\n"
	"#else\n"
	"	vec3 normal = UnpackNormal( gbuffer1.rgb );\n"
	"	vec3 toLight = rpLocalLightOrigin.xyz - position.xyz;\n"
	"	vec3 lightVector = normalize( toLight );\n"
	"	vec3 viewVector = normalize( -position.xyz );\n"
	"\n"
	"	// interaction.ps\n"
	"	float ldotN = clamp( dot( normal, lightVector ), 0.0, 1.0 );\n"
	"#if defined( USE_HALF_LAMBERT )\n"
	"	float halfLdotN = dot( normal, lightVector ) * 0.5 + 0.5;\n"
	"	halfLdotN *= halfLdotN;\n"
	"	float lambert = mix( ldotN, halfLdotN, 0.5 );\n"
	"#else\n"
	"	float lambert = ldotN;\n"
	"#endif\n"
	"\n"
	"	vec3 halfAngleVector = normalize( lightVector + viewVector );\n"
	"	float hdotN = clamp( dot( halfAngleVector, normal ), 0.0, 1.0 );\n"
	"	float specularBase2 = hdotN * hdotN;\n"
	"	float specularBase4 = specularBase2 * specularBase2;\n"
	"	float specularBase8 = specularBase4 * specularBase4;\n"
	"\n"
	"	vec3 diffuseColor = albedo * sRGBToLinearRGB( rpDiffuseModifier.rgb );\n"
	"	vec3 specularColor = specular * ( specularBase8 * specularBase2 ) * sRGBToLinearRGB( rpSpecularModifier.rgb );\n"
	"	vec3 lightColor = sRGBToLinearRGB( lightProj.rgb * lightFalloff.rgb );\n"
	"\n"
	"	vec3 color = ( diffuseColor + specularColor ) * lambert * lightColor;\n"
	"#if defined( SHADOW_MAPPING )\n"
	"	color *= Shadow( position, viewDepth, toLight );\n"
	"#endif\n"
	"#endif\n"
	"\n"
	"	fb_Color = vec4( fb_Color.rgb + color, 1.0 );\n"
	"}\n";

/*
================================================================================================
Multi light shading

Up to LB_SLOTS light stages per draw, their parameters in lightbatch_ubo, LB_VEC4S vec4s each,
in the view space of the G-buffer (see idRenderBackend::LightBatchAddEntries):
	0		origin xyz, w type: 0 no shadows, 1 ambient, 2 spot, 3 point, 4 parallel shadow mapped
	1..4	the volume, view space to the 0..1 cube of the light (texgen 0 of DrawDeferredLight)
	5..8	projection s t q and falloff s, with the texture matrix of the stage
	9		light color rgb, the specular is twice it like the interactions
	10		x shadow atlas texel size, y depth bias scale, z 1 when the cache atlas holds the maps,
			w 1 when the triangles facing away from the light stay dark (forward draws)
	11		parallel light cascade distances
	12..35	view space to shadow atlas, 4 rows per side
An unused slot has a zero volume and lights nothing. The math is DrawDeferredLight's, a light of
the batch adds what it adds alone.
================================================================================================
*/

static const char* const deferredLightBatchCommon =
	"#define LB_VEC4S 36\n"
	"layout( std140 ) uniform lightbatch_ubo\n"
	"{\n"
	"	vec4 lightBatch[ LB_SLOTS * LB_VEC4S ];\n"
	"};\n"
	"\n"
	"uniform mediump sampler2DArrayShadow LB_FRAME_ATLAS;	// the shadow atlas of the frame\n"
	"uniform mediump sampler2DArrayShadow LB_CACHE_ATLAS;	// the shadow cache\n"
	"uniform mediump sampler2D LB_JITTER;\n"
	"\n"
	"bool LB_Inside( int b, vec4 position )\n"
	"{\n"
	"	vec4 volume = vec4( dot( position, lightBatch[ b + 1 ] ), dot( position, lightBatch[ b + 2 ] ), dot( position, lightBatch[ b + 3 ] ), dot( position, lightBatch[ b + 4 ] ) );\n"
	"	return !( volume.w <= 0.0 || any( lessThan( volume.xyz, vec3( 0.0 ) ) ) || any( greaterThan( volume.xyz, vec3( volume.w ) ) ) );\n"
	"}\n"
	"\n"
	// interactionSM.ps with TBR_MOBILE, four Poisson taps
	"float LB_PCF( mediump sampler2DArrayShadow atlas, vec4 shadowTexcoord, float shadowTexelSize, vec2 rot )\n"
	"{\n"
	"	const vec2 poissonDisk[4] = vec2[](\n"
	"		vec2( 0.6111618, 0.1050905 ),\n"
	"		vec2( 0.3030421, -0.6292974 ),\n"
	"		vec2( -0.6411021, -0.4748057 ),\n"
	"		vec2( -0.1314865, 0.8404058 ) );\n"
	"	float shadow = 4.0 / 12.0;\n"
	"	for( int i = 0; i < 4; i++ )\n"
	"	{\n"
	"		vec2 jitter = poissonDisk[i];\n"
	"		vec2 jitterRotated = vec2( jitter.x * rot.x - jitter.y * rot.y, jitter.x * rot.y + jitter.y * rot.x );\n"
	"		shadow += textureGrad( atlas, vec4( shadowTexcoord.xy + jitterRotated * shadowTexelSize, shadowTexcoord.w, shadowTexcoord.z ), vec2( 0.0 ), vec2( 0.0 ) );\n"
	"	}\n"
	"	return shadow * 0.25;\n"
	"}\n"
	"\n"
	"float LB_Shadow( int b, float type, vec4 position, float viewDepth, vec3 toLight )\n"
	"{\n"
	"	int shadowIndex = 0;\n"
	"	if( type == 3.0 )\n"
	"	{\n"
	"		vec3 toLightGlobal = vec3( dot( rpModelMatrixX.xyz, toLight ), dot( rpModelMatrixY.xyz, toLight ), dot( rpModelMatrixZ.xyz, toLight ) );\n"
	"		vec3 absToLightGlobal = abs( toLightGlobal );\n"
	"		if( absToLightGlobal.x >= absToLightGlobal.y && absToLightGlobal.x >= absToLightGlobal.z )\n"
	"		{\n"
	"			shadowIndex = ( toLightGlobal.x > 0.0 ) ? 1 : 0;\n"
	"		}\n"
	"		else if( absToLightGlobal.y >= absToLightGlobal.z )\n"
	"		{\n"
	"			shadowIndex = ( toLightGlobal.y > 0.0 ) ? 3 : 2;\n"
	"		}\n"
	"		else\n"
	"		{\n"
	"			shadowIndex = ( toLightGlobal.z > 0.0 ) ? 5 : 4;\n"
	"		}\n"
	"	}\n"
	"	else if( type == 4.0 )\n"
	"	{\n"
	"		vec4 cascades = lightBatch[ b + 11 ];\n"
	"		shadowIndex = int( viewDepth >= cascades.x ) + int( viewDepth >= cascades.y ) + int( viewDepth >= cascades.z ) + int( viewDepth >= cascades.w );\n"
	"	}\n"
	"\n"
	"	int m = b + 12 + shadowIndex * 4;\n"
	"	vec4 shadowParms = lightBatch[ b + 10 ];\n"
	"	vec4 shadowTexcoord = vec4( dot( position, lightBatch[ m ] ), dot( position, lightBatch[ m + 1 ] ), dot( position, lightBatch[ m + 2 ] ), dot( position, lightBatch[ m + 3 ] ) );\n"
	"	shadowTexcoord.xyz /= shadowTexcoord.w;\n"
	"	shadowTexcoord.z = shadowTexcoord.z * shadowParms.y;\n"
	// the atlases are a single layer
	"	shadowTexcoord.w = 0.0;\n"
	"\n"
	// a main may have returned for some pixels already, implicit derivatives are undefined here
	"	vec2 jitterTC = gl_FragCoord.xy * rpScreenCorrectionFactor.xy + rpJitterTexOffset.xy;\n"
	"	float randomAngle = textureLod( LB_JITTER, jitterTC, 0.0 ).x * PI;\n"
	"	vec2 rot = vec2( cos( randomAngle ), sin( randomAngle ) );\n"
	"	float shadowTexelSize = shadowParms.x * rpJitterTexScale.x;\n"
	"	if( shadowParms.z > 0.5 )\n"
	"	{\n"
	"		return LB_PCF( LB_CACHE_ATLAS, shadowTexcoord, shadowTexelSize, rot );\n"
	"	}\n"
	"	return LB_PCF( LB_FRAME_ATLAS, shadowTexcoord, shadowTexelSize, rot );\n"
	"}\n"
	"\n"
	// what the light stage of slot b adds to the pixel, see DrawDeferredLight
	"vec3 LB_Light( int b, mediump sampler2D falloffMap, mediump sampler2D projectionMap, vec4 position, vec3 normal, vec3 viewVector,\n"
	"			   vec3 albedo, vec3 specular, float viewDepth, vec3 facingNormal )\n"
	"{\n"
	"	if( !LB_Inside( b, position ) )\n"
	"	{\n"
	"		return vec3( 0.0 );\n"
	"	}\n"
	"\n"
	"	vec4 origin = lightBatch[ b ];\n"
	"	float type = origin.w;\n"
	"	vec3 toLight = origin.xyz - position.xyz;\n"
	"\n"
	// the interaction culled the triangles facing away from the light, 10.w is 1 then
	"	if( lightBatch[ b + 10 ].w > 0.5 && type != 1.0 && dot( facingNormal, toLight ) < 0.0 )\n"
	"	{\n"
	"		return vec3( 0.0 );\n"
	"	}\n"
	"\n"
	"	vec3 lightProjection = vec3( dot( position, lightBatch[ b + 5 ] ), dot( position, lightBatch[ b + 6 ] ), dot( position, lightBatch[ b + 7 ] ) );\n"
	"	float falloff = dot( position, lightBatch[ b + 8 ] );\n"
	"	mediump vec4 lightFalloff = textureLod( falloffMap, vec2( falloff, 0.5 ), 0.0 );\n"
	"	mediump vec4 lightProj = textureLod( projectionMap, lightProjection.xy / lightProjection.z, 0.0 );\n"
	"	vec3 lightColorParm = lightBatch[ b + 9 ].rgb;\n"
	"\n"
	"	if( type == 1.0 )\n"
	"	{\n"
	// interactionAmbient.ps, the specular map of an ambient light is black
	"		return albedo * sRGBToLinearRGB( lightColorParm ) * lightProj.rgb * lightFalloff.rgb;\n"
	"	}\n"
	"\n"
	"	vec3 lightVector = normalize( toLight );\n"
	"	float ldotN = clamp( dot( normal, lightVector ), 0.0, 1.0 );\n"
	"#if defined( USE_HALF_LAMBERT )\n"
	"	float halfLdotN = dot( normal, lightVector ) * 0.5 + 0.5;\n"
	"	halfLdotN *= halfLdotN;\n"
	"	float lambert = mix( ldotN, halfLdotN, 0.5 );\n"
	"#else\n"
	"	float lambert = ldotN;\n"
	"#endif\n"
	"\n"
	"	vec3 halfAngleVector = normalize( lightVector + viewVector );\n"
	"	float hdotN = clamp( dot( halfAngleVector, normal ), 0.0, 1.0 );\n"
	"	float specularBase2 = hdotN * hdotN;\n"
	"	float specularBase4 = specularBase2 * specularBase2;\n"
	"	float specularBase8 = specularBase4 * specularBase4;\n"
	"\n"
	"	vec3 diffuseColor = albedo * sRGBToLinearRGB( lightColorParm );\n"
	"	vec3 specularColor = specular * ( specularBase8 * specularBase2 ) * sRGBToLinearRGB( lightColorParm * 2.0 );\n"
	"	vec3 lightColor = sRGBToLinearRGB( lightProj.rgb * lightFalloff.rgb );\n"
	"\n"
	"	vec3 color = ( diffuseColor + specularColor ) * lambert * lightColor;\n"
	"	if( type >= 2.0 )\n"
	"	{\n"
	"		color *= LB_Shadow( b, type, position, viewDepth, toLight );\n"
	"	}\n"
	"	return color;\n"
	"}\n"
	"\n";

// globals of the batch programs, the per light data is in lightbatch_ubo
static const renderParm_t deferredLightBatchFragmentParms[] =
{
	RENDERPARM_USER0,				// window coordinates to normalized device coordinates
	RENDERPARM_USER1,				// 1 / P00, P02, P03, P32 of the projection
	RENDERPARM_USER2,				// 1 / P11, P12, P13, P33 of the projection
	RENDERPARM_SCREENCORRECTIONFACTOR,	// xy the jitter texture coordinates of a pixel
	RENDERPARM_JITTERTEXSCALE,
	RENDERPARM_JITTERTEXOFFSET,
	RENDERPARM_MODELMATRIX_X,		// view space to world space
	RENDERPARM_MODELMATRIX_Y,
	RENDERPARM_MODELMATRIX_Z,
};

/*
------------------------------------------------
From the G-buffer: an instance per screen tile rectangle, with the mask of the batch's lights
that reach it. Every pixel fetches its G-buffer once for all of them.
------------------------------------------------
*/

static const char* const deferredLightBatchVertexShader =
	"in vec4 in_Position;\n"
	"in vec4 in_InstanceMVPX;	// the tiles: x1 y1 x2 y2 in normalized device coordinates\n"
	"in vec4 in_InstanceMVPY;	// x: the mask of the slots that reach them\n"
	"\n"
	"flat out highp uint vofi_LightMask;\n"
	"\n"
	"void main()\n"
	"{\n"
	"	vec2 corner = in_Position.xy * 0.5 + 0.5;\n"
	"	gl_Position = vec4( mix( in_InstanceMVPX.xy, in_InstanceMVPX.zw, corner ), 0.0, 1.0 );\n"
	"	vofi_LightMask = uint( in_InstanceMVPY.x + 0.5 );\n"
	"}\n";

static const char* const deferredLightBatchFragmentShader =
	"layout( location = 0 ) inout mediump vec4 fb_Color;\n"
	"layout( location = 1 ) inout highp vec4 fb_GBuffer0;\n"
	"layout( location = 2 ) inout highp vec4 fb_GBuffer1;\n"
	"layout( location = 3 ) inout highp vec4 fb_GBuffer2;\n"
	"\n"
	"uniform mediump sampler2D samp3;	// falloff and projection of slot 0\n"
	"uniform mediump sampler2D samp4;\n"
	"uniform mediump sampler2D samp5;\n"
	"uniform mediump sampler2D samp6;\n"
	"uniform mediump sampler2D samp7;\n"
	"uniform mediump sampler2D samp8;\n"
	"uniform mediump sampler2D samp9;\n"
	"uniform mediump sampler2D samp10;\n"
	"uniform mediump sampler2D samp11;\n"
	"uniform mediump sampler2D samp12;\n"
	"uniform mediump sampler2D samp13;\n"
	"uniform mediump sampler2D samp14;\n"
	"\n"
	"flat in highp uint vofi_LightMask;\n"
	"\n"
	"void main()\n"
	"{\n"
	"	vec4 gbuffer0 = fb_GBuffer0;\n"
	"	vec4 gbuffer1 = fb_GBuffer1;\n"
	"	vec4 gbuffer2 = fb_GBuffer2;\n"
	"\n"
	"	vec3 albedo = gbuffer0.rgb * gbuffer0.rgb;\n"
	"	vec3 specular = vec3( gbuffer0.a, gbuffer1.a, gbuffer2.a );\n"
	"	specular *= specular;\n"
	"\n"
	// sky, surfaces without lighting and the ones the interactions light
	"	if( max( max( max( albedo.r, albedo.g ), max( albedo.b, specular.r ) ), max( specular.g, specular.b ) ) <= 0.0 )\n"
	"	{\n"
	"		return;\n"
	"	}\n"
	"\n"
	"	float viewDepth = UnpackDepth( gbuffer2.rgb );\n"
	"	vec2 ndc = gl_FragCoord.xy * rpUser0.xy + rpUser0.zw;\n"
	"	float z = -viewDepth;\n"
	"	float w = rpUser1.w * z + rpUser2.w;\n"
	"	vec4 position = vec4( ( ndc.x * w - rpUser1.y * z - rpUser1.z ) * rpUser1.x,\n"
	"						  ( ndc.y * w - rpUser2.y * z - rpUser2.z ) * rpUser2.x, z, 1.0 );\n"
	"	vec3 normal = UnpackNormal( gbuffer1.rgb );\n"
	"	vec3 viewVector = normalize( -position.xyz );\n"
	"\n"
	"	uint mask = vofi_LightMask;\n"
	"	vec3 color = vec3( 0.0 );\n"
	"	if( ( mask & 1u ) != 0u ) color += LB_Light( 0 * LB_VEC4S, samp3, samp4, position, normal, viewVector, albedo, specular, viewDepth, normal );\n"
	"	if( ( mask & 2u ) != 0u ) color += LB_Light( 1 * LB_VEC4S, samp5, samp6, position, normal, viewVector, albedo, specular, viewDepth, normal );\n"
	"	if( ( mask & 4u ) != 0u ) color += LB_Light( 2 * LB_VEC4S, samp7, samp8, position, normal, viewVector, albedo, specular, viewDepth, normal );\n"
	"	if( ( mask & 8u ) != 0u ) color += LB_Light( 3 * LB_VEC4S, samp9, samp10, position, normal, viewVector, albedo, specular, viewDepth, normal );\n"
	"	if( ( mask & 16u ) != 0u ) color += LB_Light( 4 * LB_VEC4S, samp11, samp12, position, normal, viewVector, albedo, specular, viewDepth, normal );\n"
	"	if( ( mask & 32u ) != 0u ) color += LB_Light( 5 * LB_VEC4S, samp13, samp14, position, normal, viewVector, albedo, specular, viewDepth, normal );\n"
	"\n"
	"	fb_Color = vec4( fb_Color.rgb + color, 1.0 );\n"
	"}\n";

/*
------------------------------------------------
A surface outside the G-buffer with several lights: what the G-buffer pass would have stored is
computed in place, then every light of the draw adds to it, blended one one like the interactions.
------------------------------------------------
*/

static const renderParm_t deferredForwardFragmentParms[] =
{
	RENDERPARM_DIFFUSEMODIFIER,		// color of the diffuse stage
	RENDERPARM_SPECULARMODIFIER,	// color of the specular stage
	RENDERPARM_SCREENCORRECTIONFACTOR,
	RENDERPARM_JITTERTEXSCALE,
	RENDERPARM_JITTERTEXOFFSET,
	RENDERPARM_MODELMATRIX_X,
	RENDERPARM_MODELMATRIX_Y,
	RENDERPARM_MODELMATRIX_Z,
};

static const char* const deferredForwardFragmentShader =
	"uniform mediump sampler2D samp0;	// bump map\n"
	"uniform mediump sampler2D samp1;	// specular map\n"
	"uniform mediump sampler2D samp2;	// diffuse map, YCoCg\n"
	"uniform mediump sampler2D samp6;	// falloff and projection of slot 0\n"
	"uniform mediump sampler2D samp7;\n"
	"uniform mediump sampler2D samp8;\n"
	"uniform mediump sampler2D samp9;\n"
	"uniform mediump sampler2D samp10;\n"
	"uniform mediump sampler2D samp11;\n"
	"uniform mediump sampler2D samp12;\n"
	"uniform mediump sampler2D samp13;\n"
	"uniform float " TEXTURE_LOD_BIAS_UNIFORM_NAME ";\n"
	"\n"
	"in highp vec4 vofi_TexCoordBumpDiffuse;\n"
	"in highp vec2 vofi_TexCoordSpecular;\n"
	"in mediump vec3 vofi_Tangent;\n"
	"in mediump vec3 vofi_Bitangent;\n"
	"in mediump vec3 vofi_Normal;\n"
	"in highp float vofi_ViewDepth;\n"
	"in mediump vec4 vofi_Color;\n"
	"in mediump vec3 vofi_AmbientHalfAngle;\n"
	"in mediump vec3 vofi_AmbientLight;\n"
	"in highp vec3 vofi_ViewPosition;\n"
	"\n"
	"layout( location = 0 ) out mediump vec4 fo_Color;\n"
	"\n"
	"vec3 ConvertYCoCgToRGB( vec4 YCoCg )\n"
	"{\n"
	"	YCoCg.z = ( YCoCg.z * 31.875 ) + 1.0;\n"
	"	YCoCg.z = 1.0 / YCoCg.z;\n"
	"	YCoCg.xy *= YCoCg.z;\n"
	"	vec3 rgbColor;\n"
	"	rgbColor.x = dot( YCoCg, vec4( 1.0, -1.0, 0.0, 1.0 ) );\n"
	"	rgbColor.y = dot( YCoCg, vec4( 0.0, 1.0, -0.50196078, 1.0 ) );\n"
	"	rgbColor.z = dot( YCoCg, vec4( -1.0, -1.0, 1.00392156, 1.0 ) );\n"
	"	return rgbColor;\n"
	"}\n"
	"\n"
	"void main()\n"
	"{\n"
	"	vec4 position = vec4( vofi_ViewPosition, 1.0 );\n"
	"\n"
	// nothing to sample where none of the lights reach
	"	if( !LB_Inside( 0 * LB_VEC4S, position ) && !LB_Inside( 1 * LB_VEC4S, position ) && !LB_Inside( 2 * LB_VEC4S, position ) && !LB_Inside( 3 * LB_VEC4S, position ) )\n"
	"	{\n"
	"		fo_Color = vec4( 0.0 );\n"
	"		return;\n"
	"	}\n"
	"\n"
	// the G-buffer values of the surface, see deferredGBufferFragmentShader
	"	mediump vec4 bumpMap = texture( samp0, vofi_TexCoordBumpDiffuse.xy, " TEXTURE_LOD_BIAS_UNIFORM_NAME " );\n"
	"	mediump vec4 YCoCG = texture( samp2, vofi_TexCoordBumpDiffuse.zw, " TEXTURE_LOD_BIAS_UNIFORM_NAME " );\n"
	"	mediump vec4 specMapSRGB = texture( samp1, vofi_TexCoordSpecular, " TEXTURE_LOD_BIAS_UNIFORM_NAME " );\n"
	"\n"
	"	vec3 diffuseMap = sRGBToLinearRGB( ConvertYCoCgToRGB( YCoCG ) );\n"
	"	vec3 specMap = sRGBToLinearRGB( specMapSRGB.rgb );\n"
	"\n"
	"	vec3 localNormal;\n"
	"	localNormal.xy = bumpMap.wy - 0.5;\n"
	"	localNormal.z = sqrt( abs( dot( localNormal.xy, localNormal.xy ) - 0.25 ) );\n"
	"	localNormal = normalize( localNormal );\n"
	"\n"
	"	vec3 normal = localNormal.x * vofi_Tangent + localNormal.y * vofi_Bitangent + localNormal.z * vofi_Normal;\n"
	"	float normalLengthSqr = dot( normal, normal );\n"
	"	normal = ( normalLengthSqr > 1e-12 ) ? normal * inversesqrt( normalLengthSqr ) : vec3( 0.0, 0.0, 1.0 );\n"
	"\n"
	"	vec3 albedo = clamp( diffuseMap * sRGBToLinearRGB( rpDiffuseModifier.rgb ) * vofi_Color.rgb, 0.0, 1.0 );\n"
	"	vec3 specular = clamp( specMap * sRGBToLinearRGB( rpSpecularModifier.rgb ) * vofi_Color.rgb, 0.0, 1.0 );\n"
	"\n"
	"	vec3 viewVector = normalize( -position.xyz );\n"
	"	vec3 facingNormal = vofi_Normal;\n"
	"	float viewDepth = vofi_ViewDepth;\n"
	"\n"
	"	vec3 color = LB_Light( 0 * LB_VEC4S, samp6, samp7, position, normal, viewVector, albedo, specular, viewDepth, facingNormal );\n"
	"	color += LB_Light( 1 * LB_VEC4S, samp8, samp9, position, normal, viewVector, albedo, specular, viewDepth, facingNormal );\n"
	"	color += LB_Light( 2 * LB_VEC4S, samp10, samp11, position, normal, viewVector, albedo, specular, viewDepth, facingNormal );\n"
	"	color += LB_Light( 3 * LB_VEC4S, samp12, samp13, position, normal, viewVector, albedo, specular, viewDepth, facingNormal );\n"
	"\n"
	"	fo_Color = vec4( color, 1.0 );\n"
	"}\n";

/*
------------------------------------------------
The static casters of a light from the shadow cache into its frame atlas tile, under the dynamic
casters drawn next. The viewport is the destination tile, rpUser0.xy the offset to the source.
------------------------------------------------
*/

static const renderParm_t deferredShadowCopyFragmentParms[] =
{
	RENDERPARM_USER0,
};

static const char* const deferredShadowCopyFragmentShader =
	"uniform highp sampler2DArray samp0;	// the shadow cache, bound with a sampler without depth compare\n"
	"\n"
	"void main()\n"
	"{\n"
	"	ivec2 texel = ivec2( gl_FragCoord.xy ) + ivec2( rpUser0.xy );\n"
	"	gl_FragDepth = texelFetch( samp0, ivec3( texel, 0 ), 0 ).x;\n"
	"}\n";

/*
================================================================================================
Full screen programs: the scene color to the back buffer without HDR, and the start up test
of the framebuffer fetch with four color attachments
================================================================================================
*/

static const char* const deferredScreenVertexShader =
	"in vec4 in_Position;\n"
	"\n"
	"void main()\n"
	"{\n"
	"	gl_Position = vec4( in_Position.xy, 0.0, 1.0 );\n"
	"}\n";

static const char* const deferredResolveFragmentShader =
	"uniform mediump sampler2D samp0;\n"
	"\n"
	"layout( location = 0 ) out mediump vec4 fo_FragColor;\n"
	"\n"
	"void main()\n"
	"{\n"
	"	fo_FragColor = texelFetch( samp0, ivec2( gl_FragCoord.xy ), 0 );\n"
	"}\n";

static const char* const deferredTestWriteFragmentShader =
	"layout( location = 0 ) out mediump vec4 fo_Color0;\n"
	"layout( location = 1 ) out mediump vec4 fo_Color1;\n"
	"layout( location = 2 ) out mediump vec4 fo_Color2;\n"
	"layout( location = 3 ) out mediump vec4 fo_Color3;\n"
	"\n"
	"void main()\n"
	"{\n"
	"	fo_Color0 = vec4( 0.25, 0.5, 0.75, 1.0 );\n"
	"	fo_Color1 = vec4( 0.2, 0.4, 0.6, 0.8 );\n"
	"	fo_Color2 = vec4( 0.1, 0.3, 0.5, 0.7 );\n"
	"	fo_Color3 = vec4( 0.9, 0.7, 0.5, 0.3 );\n"
	"}\n";

// idRenderBackend::DeferredSelfTest expects 0.2 + 0.3, 0.5 + 0.25, 0.7 - 0.2, 1
static const char* const deferredTestFetchFragmentShader =
	"layout( location = 0 ) inout mediump vec4 fb_Color;\n"
	"layout( location = 1 ) inout highp vec4 fb_GBuffer0;\n"
	"layout( location = 2 ) inout highp vec4 fb_GBuffer1;\n"
	"layout( location = 3 ) inout highp vec4 fb_GBuffer2;\n"
	"\n"
	"void main()\n"
	"{\n"
	"	fb_Color = vec4( fb_GBuffer0.r + fb_GBuffer1.g, fb_GBuffer2.b + fb_Color.r, fb_GBuffer1.a - fb_GBuffer0.r, 1.0 );\n"
	"}\n";

/*
==================
R_IsDeferredShader
==================
*/
static bool R_IsDeferredShader( const char* name )
{
	return idStr::Icmpn( name, "_deferred", 9 ) == 0;
}

/*
==================
R_DeferredUniforms

The parms go into _va_ / _fa_ like the converted HLSL ones, see ConvertCG2GLSL
==================
*/
static void R_DeferredUniforms( const renderParm_t* parms, int numParms, rpStage_t stage, idStr& glsl, idStr& layout )
{
	const char* arrayName = ( stage == SHADER_STAGE_VERTEX ) ? VERTEX_UNIFORM_ARRAY_NAME : FRAGMENT_UNIFORM_ARRAY_NAME;

	layout = "uniforms [\n";
	if( numParms > 0 )
	{
		int extraSize = 0;
		for( int i = 0; i < numParms; i++ )
		{
			if( parms[i] == RENDERPARM_SHADOW_MATRIX_0_X )
			{
				extraSize = 6 * 4;
			}
		}
		glsl += va( "uniform vec4 %s[%d];\n", arrayName, numParms + extraSize );

		for( int i = 0; i < numParms; i++ )
		{
			const char* parmName = renderProgManager.GetGLSLParmName( parms[i] );
			if( parms[i] == RENDERPARM_SHADOW_MATRIX_0_X )
			{
				glsl += va( "#define %s( index ) %s[%d + ( index )]\n", parmName, arrayName, i );
			}
			else
			{
				glsl += va( "#define %s %s[%d]\n", parmName, arrayName, i );
			}
			layout += "\t";
			layout += parmName;
			layout += "\n";
		}
		glsl += "\n";
	}
	layout += "]\n";
}

/*
==================
R_DeferredShaderSource

The source and the uniform layout of a program of the deferred shading, by the name and suffix
idRenderProgManager::Init gives it
==================
*/
static bool R_DeferredShaderSource( const char* name, const char* suffix, rpStage_t stage, idStr& glsl, idStr& layout, bool& usesFetch )
{
	const char* body = NULL;
	const char* lightBatchFunctions = NULL;
	const renderParm_t* parms = NULL;
	int numParms = 0;
	bool fetch = false;
	bool colorFunctions = false;
	bool packFunctions = false;
	idStr defines;

	if( idStr::Icmp( name, "_deferred_lightbatch" ) == 0 )
	{
		if( stage == SHADER_STAGE_VERTEX )
		{
			body = deferredLightBatchVertexShader;
		}
		else
		{
			body = deferredLightBatchFragmentShader;
			parms = deferredLightBatchFragmentParms;
			numParms = sizeof( deferredLightBatchFragmentParms ) / sizeof( deferredLightBatchFragmentParms[0] );
			fetch = true;
			colorFunctions = true;
			packFunctions = true;
			lightBatchFunctions = deferredLightBatchCommon;
			defines += "#define LB_SLOTS 6\n#define LB_FRAME_ATLAS samp0\n#define LB_CACHE_ATLAS samp1\n#define LB_JITTER samp2\n";
		}
	}
	else if( idStr::Icmp( name, "_deferred_forward" ) == 0 )
	{
		if( stage == SHADER_STAGE_VERTEX )
		{
			body = deferredGBufferVertexShader;
			parms = deferredGBufferVertexParms;
			numParms = sizeof( deferredGBufferVertexParms ) / sizeof( deferredGBufferVertexParms[0] );
			defines += "#define FORWARD_LIGHTS\n";
			if( idStr::Icmp( suffix, "_skinned" ) == 0 )
			{
				defines += "#define USE_GPU_SKINNING\n";
			}
		}
		else
		{
			body = deferredForwardFragmentShader;
			parms = deferredForwardFragmentParms;
			numParms = sizeof( deferredForwardFragmentParms ) / sizeof( deferredForwardFragmentParms[0] );
			colorFunctions = true;
			lightBatchFunctions = deferredLightBatchCommon;
			defines += "#define LB_SLOTS 4\n#define LB_FRAME_ATLAS samp3\n#define LB_CACHE_ATLAS samp4\n#define LB_JITTER samp5\n";
		}
	}
	else if( idStr::Icmp( name, "_deferred_shadowcopy" ) == 0 )
	{
		if( stage == SHADER_STAGE_VERTEX )
		{
			body = deferredScreenVertexShader;
		}
		else
		{
			body = deferredShadowCopyFragmentShader;
			parms = deferredShadowCopyFragmentParms;
			numParms = sizeof( deferredShadowCopyFragmentParms ) / sizeof( deferredShadowCopyFragmentParms[0] );
		}
	}
	else if( idStr::Icmp( name, "_deferred_gbuffer" ) == 0 )
	{
		if( stage == SHADER_STAGE_VERTEX )
		{
			body = deferredGBufferVertexShader;
			parms = deferredGBufferVertexParms;
			numParms = sizeof( deferredGBufferVertexParms ) / sizeof( deferredGBufferVertexParms[0] );
			if( idStr::Icmp( suffix, "_skinned" ) == 0 )
			{
				defines += "#define USE_GPU_SKINNING\n";
			}
			else if( idStr::Icmp( suffix, "_instanced" ) == 0 )
			{
				defines += "#define USE_GPU_INSTANCING\n";
			}
		}
		else
		{
			body = deferredGBufferFragmentShader;
			parms = deferredGBufferFragmentParms;
			numParms = sizeof( deferredGBufferFragmentParms ) / sizeof( deferredGBufferFragmentParms[0] );
			colorFunctions = true;
			packFunctions = true;
		}
	}
	else if( idStr::Icmp( name, "_deferred_light" ) == 0 )
	{
		if( stage == SHADER_STAGE_VERTEX )
		{
			body = deferredLightVertexShader;
			parms = deferredLightVertexParms;
			numParms = sizeof( deferredLightVertexParms ) / sizeof( deferredLightVertexParms[0] );
		}
		else
		{
			body = deferredLightFragmentShader;
			fetch = true;
			colorFunctions = true;
			packFunctions = true;

			bool shadowMapping = true;
			if( idStr::Icmp( suffix, "_point" ) == 0 )
			{
				defines += "#define LIGHT_POINT\n";
			}
			else if( idStr::Icmp( suffix, "_parallel" ) == 0 )
			{
				defines += "#define LIGHT_PARALLEL\n";
			}
			else if( idStr::Icmp( suffix, "_spot" ) != 0 )
			{
				shadowMapping = false;
				if( idStr::Icmp( suffix, "_ambient" ) == 0 )
				{
					defines += "#define LIGHT_AMBIENT\n";
				}
			}

			if( shadowMapping )
			{
				defines += "#define SHADOW_MAPPING\n";
				parms = deferredLightShadowFragmentParms;
				numParms = sizeof( deferredLightShadowFragmentParms ) / sizeof( deferredLightShadowFragmentParms[0] );
			}
			else
			{
				parms = deferredLightFragmentParms;
				numParms = sizeof( deferredLightFragmentParms ) / sizeof( deferredLightFragmentParms[0] );
			}
		}
	}
	else if( idStr::Icmp( name, "_deferred_screen" ) == 0 )
	{
		if( stage == SHADER_STAGE_VERTEX )
		{
			body = deferredScreenVertexShader;
		}
		else if( idStr::Icmp( suffix, "_resolve" ) == 0 )
		{
			body = deferredResolveFragmentShader;
		}
		else if( idStr::Icmp( suffix, "_testwrite" ) == 0 )
		{
			body = deferredTestWriteFragmentShader;
		}
		else if( idStr::Icmp( suffix, "_testfetch" ) == 0 )
		{
			body = deferredTestFetchFragmentShader;
			fetch = true;
		}
	}

	if( body == NULL )
	{
		idLib::Warning( "no deferred shading program %s%s", name, suffix );
		return false;
	}

	glsl = "#version 320 es\n";
	if( fetch )
	{
		glsl += "#extension GL_EXT_shader_framebuffer_fetch : require\n";
	}
	glsl += va( "// %s%s, on-tile deferred shading\n", name, suffix );

	if( stage == SHADER_STAGE_FRAGMENT )
	{
		if( r_useHDR.GetBool() )
		{
			defines += "#define USE_LINEAR_RGB\n";
		}
		if( r_useHalfLambertLighting.GetBool() )
		{
			defines += "#define USE_HALF_LAMBERT\n";
		}
	}
	glsl += defines;
	glsl += deferredShaderCommon;

	R_DeferredUniforms( parms, numParms, stage, glsl, layout );

	if( colorFunctions )
	{
		glsl += deferredShaderColorFunctions;
	}
	if( packFunctions )
	{
		glsl += deferredShaderPackFunctions;
	}
	if( lightBatchFunctions != NULL )
	{
		glsl += lightBatchFunctions;
	}
	glsl += body;

	usesFetch = fetch;
	return true;
}

#endif // __DEFERREDSHADERS_GL_H__
