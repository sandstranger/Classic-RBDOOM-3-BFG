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

#ifndef __ENGINESHADERS_GL_H__
#define __ENGINESHADERS_GL_H__

/*
================================================================================================

GLSL ES PROGRAMS OF THE ENGINE

Programs written here as GLSL ES 3.20, not HLSL in the game data: the renderprogs on the device
can be older than the engine. Their names start with _engine, idRenderProgManager::LoadShader
takes the source from R_EngineShaderSource.

================================================================================================
*/

static const char* const engineShaderCommon =
	"precision highp float;\n"
	"precision highp int;\n"
	"\n"
	"#define PI 3.14159265358979323846\n"
	"\n";

static const char* const engineShaderColorFunctions =
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

/*
================================================================================================

FORWARD MULTI LIGHT INTERACTIONS

A surface is drawn once for up to LB_SLOTS light stages, blended one one like the interactions,
into the target of the view. The surface values (bump, diffuse and specular maps with their
stage colors and the vertex color) are computed once, every slot adds what interaction.ps or
interactionAmbient.ps would add for its light stage.

The slots are in lightbatch_ubo (binding 1), LB_VEC4S vec4 per slot, in view space
(see idRenderBackend::LightBatchAddEntries):
	0		origin xyz, w type: 0 no shadows, 1 ambient, 2 spot, 3 point, 4 parallel shadow mapped
	1..4	the volume, view space to the 0..1 cube of the light
	5..8	projection s t q and falloff s, with the texture matrix of the stage
	9		light color rgb, the specular is twice it like the interactions
	10		x shadow atlas texel size, y depth bias scale, z 1 when the cache atlas holds the maps,
			w 1 when the triangles facing away from the light stay dark, as the light tris of
			the interaction leave them out
	11		parallel light cascade distances
	12..35	view space to shadow atlas, 4 rows per side
An unused slot has a zero volume and adds nothing.

LB_SHADOWS builds the shadow map code in (interactionSM with the atlases), the programs without
it are used for the draws whose lights have no shadow maps.

================================================================================================
*/

static const char* const forwardLightsCommon =
	"#define LB_VEC4S 36\n"
	"layout( std140 ) uniform lightbatch_ubo\n"
	"{\n"
	"	vec4 lightBatch[ LB_SLOTS * LB_VEC4S ];\n"
	"};\n"
	"\n"
	"bool LB_Inside( int b, vec4 position )\n"
	"{\n"
	"	vec4 volume = vec4( dot( position, lightBatch[ b + 1 ] ), dot( position, lightBatch[ b + 2 ] ), dot( position, lightBatch[ b + 3 ] ), dot( position, lightBatch[ b + 4 ] ) );\n"
	"	return !( volume.w <= 0.0 || any( lessThan( volume.xyz, vec3( 0.0 ) ) ) || any( greaterThan( volume.xyz, vec3( volume.w ) ) ) );\n"
	"}\n"
	"\n"
	"#if defined( LB_SHADOWS )\n"
	"uniform mediump sampler2DArrayShadow samp3;	// the shadow atlas of the frame\n"
	"uniform mediump sampler2DArrayShadow samp4;	// the shadow cache\n"
	"uniform mediump sampler2D samp5;				// jitter\n"
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
	// LB_Light may have returned for some pixels already, implicit derivatives are undefined here
	"	vec2 jitterTC = gl_FragCoord.xy * rpScreenCorrectionFactor.xy + rpJitterTexOffset.xy;\n"
	"	float randomAngle = textureLod( samp5, jitterTC, 0.0 ).x * PI;\n"
	"	vec2 rot = vec2( cos( randomAngle ), sin( randomAngle ) );\n"
	"	float shadowTexelSize = shadowParms.x * rpJitterTexScale.x;\n"
	"	if( shadowParms.z > 0.5 )\n"
	"	{\n"
	"		return LB_PCF( samp4, shadowTexcoord, shadowTexelSize, rot );\n"
	"	}\n"
	"	return LB_PCF( samp3, shadowTexcoord, shadowTexelSize, rot );\n"
	"}\n"
	"#endif\n"
	"\n"
	// what the light stage of slot b adds to the pixel
	"vec3 LB_Light( int b, mediump sampler2D falloffMap, mediump sampler2D projectionMap, vec4 position, vec3 normal, vec3 viewVector,\n"
	"			   vec3 albedo, vec3 specular, float viewDepth, vec3 faceNormal )\n"
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
	// the interaction left the triangles facing away from the light out, 10.w is 1 then
	"	if( lightBatch[ b + 10 ].w > 0.5 && type != 1.0 && dot( faceNormal, toLight ) < 0.0 )\n"
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
	"#if defined( LB_SHADOWS )\n"
	"	if( type >= 2.0 )\n"
	"	{\n"
	"		color *= LB_Shadow( b, type, position, viewDepth, toLight );\n"
	"	}\n"
	"#endif\n"
	"	return color;\n"
	"}\n"
	"\n";

static const renderParm_t forwardLightsVertexParms[] =
{
	RENDERPARM_MVPMATRIX_X,
	RENDERPARM_MVPMATRIX_Y,
	RENDERPARM_MVPMATRIX_Z,
	RENDERPARM_MVPMATRIX_W,
	RENDERPARM_MODELVIEWMATRIX_X,
	RENDERPARM_MODELVIEWMATRIX_Y,
	RENDERPARM_MODELVIEWMATRIX_Z,
	RENDERPARM_BUMPMATRIX_S,
	RENDERPARM_BUMPMATRIX_T,
	RENDERPARM_DIFFUSEMATRIX_S,
	RENDERPARM_DIFFUSEMATRIX_T,
	RENDERPARM_SPECULARMATRIX_S,
	RENDERPARM_SPECULARMATRIX_T,
	RENDERPARM_VERTEXCOLOR_MODULATE,
	RENDERPARM_VERTEXCOLOR_ADD,
};

// the position the same way as the depth pass, the draws test depth equal against it
static const char* const forwardLightsVertexShader =
	"in vec4 in_Position;\n"
	"in vec2 in_TexCoord;\n"
	"in vec4 in_Normal;\n"
	"in vec4 in_Tangent;\n"
	"in vec4 in_Color;\n"
	"in vec4 in_Color2;\n"
	"#if defined( USE_GPU_SKINNING )\n"
	"uniform matrices_ubo { vec4 matrices[408]; };\n"
	"#endif\n"
	"\n"
	"out highp vec4 vofi_TexCoordBumpDiffuse;\n"
	"out highp vec2 vofi_TexCoordSpecular;\n"
	"out mediump vec3 vofi_Tangent;\n"
	"out mediump vec3 vofi_Bitangent;\n"
	"out mediump vec3 vofi_Normal;\n"
	"out highp vec3 vofi_ViewPosition;\n"
	"out mediump vec4 vofi_Color;\n"
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
	"	gl_Position = vec4( dot( modelPosition, rpMVPmatrixX ), dot( modelPosition, rpMVPmatrixY ), dot( modelPosition, rpMVPmatrixZ ), dot( modelPosition, rpMVPmatrixW ) );\n"
	"\n"
	"	vec4 texCoord = vec4( in_TexCoord.xy, 0.0, 1.0 );\n"
	"	vofi_TexCoordBumpDiffuse = vec4( dot( texCoord, rpBumpMatrixS ), dot( texCoord, rpBumpMatrixT ), dot( texCoord, rpDiffuseMatrixS ), dot( texCoord, rpDiffuseMatrixT ) );\n"
	"	vofi_TexCoordSpecular = vec2( dot( texCoord, rpSpecularMatrixS ), dot( texCoord, rpSpecularMatrixT ) );\n"
	"\n"
	// the tangent space and the position in view space, the lights are in view space
	"	vofi_Tangent = vec3( dot( rpModelViewMatrixX.xyz, tangent ), dot( rpModelViewMatrixY.xyz, tangent ), dot( rpModelViewMatrixZ.xyz, tangent ) );\n"
	"	vofi_Bitangent = vec3( dot( rpModelViewMatrixX.xyz, bitangent ), dot( rpModelViewMatrixY.xyz, bitangent ), dot( rpModelViewMatrixZ.xyz, bitangent ) );\n"
	"	vofi_Normal = vec3( dot( rpModelViewMatrixX.xyz, normal ), dot( rpModelViewMatrixY.xyz, normal ), dot( rpModelViewMatrixZ.xyz, normal ) );\n"
	"	vofi_ViewPosition = vec3( dot( modelPosition, rpModelViewMatrixX ), dot( modelPosition, rpModelViewMatrixY ), dot( modelPosition, rpModelViewMatrixZ ) );\n"
	"\n"
	// interaction.vs: the skinned models have no vertex color
	"#if defined( USE_GPU_SKINNING )\n"
	"	vofi_Color = vec4( 1.0 );\n"
	"#else\n"
	"	vofi_Color = in_Color * rpVertexColorModulate + rpVertexColorAdd;\n"
	"#endif\n"
	"}\n";

static const renderParm_t forwardLightsFragmentParms[] =
{
	RENDERPARM_DIFFUSEMODIFIER,		// color of the diffuse stage
	RENDERPARM_SPECULARMODIFIER,	// color of the specular stage
	RENDERPARM_SCREENCORRECTIONFACTOR,
	RENDERPARM_JITTERTEXSCALE,
	RENDERPARM_JITTERTEXOFFSET,
	RENDERPARM_MODELMATRIX_X,		// view space to world space
	RENDERPARM_MODELMATRIX_Y,
	RENDERPARM_MODELMATRIX_Z,
};

static const char* const forwardLightsFragmentShader =
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
	"in highp vec3 vofi_ViewPosition;\n"
	"in mediump vec4 vofi_Color;\n"
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
	// everything with implicit derivatives first, LB_Light returns early for some pixels.
	// The triangle's plane, turned to the viewer: what the light tris culling of the
	// interactions tests against the light origin.
	"	vec3 faceNormal = cross( dFdx( vofi_ViewPosition ), dFdy( vofi_ViewPosition ) );\n"
	"	faceNormal = ( dot( faceNormal, vofi_ViewPosition ) > 0.0 ) ? -faceNormal : faceNormal;\n"
	"\n"
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
	// interaction.ps multiplies the sum with the vertex color, the same as both of them
	"	vec3 albedo = diffuseMap * sRGBToLinearRGB( rpDiffuseModifier.rgb ) * vofi_Color.rgb;\n"
	"	vec3 specular = specMap * sRGBToLinearRGB( rpSpecularModifier.rgb ) * vofi_Color.rgb;\n"
	"\n"
	"	vec3 viewVector = normalize( -position.xyz );\n"
	"	float viewDepth = -vofi_ViewPosition.z;\n"
	"\n"
	"	vec3 color = LB_Light( 0 * LB_VEC4S, samp6, samp7, position, normal, viewVector, albedo, specular, viewDepth, faceNormal );\n"
	"	color += LB_Light( 1 * LB_VEC4S, samp8, samp9, position, normal, viewVector, albedo, specular, viewDepth, faceNormal );\n"
	"	color += LB_Light( 2 * LB_VEC4S, samp10, samp11, position, normal, viewVector, albedo, specular, viewDepth, faceNormal );\n"
	"	color += LB_Light( 3 * LB_VEC4S, samp12, samp13, position, normal, viewVector, albedo, specular, viewDepth, faceNormal );\n"
	"\n"
	"	fo_Color = vec4( color, 1.0 );\n"
	"}\n";

/*
================================================================================================

SHADOW CACHE COPY

The static casters of a light from the shadow cache into its frame atlas tile, under the dynamic
casters drawn next. The viewport is the destination tile, rpUser0.xy the offset to the source.

================================================================================================
*/

static const renderParm_t shadowCopyFragmentParms[] =
{
	RENDERPARM_USER0,
};

static const char* const screenQuadVertexShader =
	"in vec4 in_Position;\n"
	"\n"
	"void main()\n"
	"{\n"
	"	gl_Position = vec4( in_Position.xy, 0.0, 1.0 );\n"
	"}\n";

static const char* const shadowCopyFragmentShader =
	"uniform highp sampler2DArray samp0;	// the shadow cache, bound with a sampler without depth compare\n"
	"\n"
	"void main()\n"
	"{\n"
	"	ivec2 texel = ivec2( gl_FragCoord.xy ) + ivec2( rpUser0.xy );\n"
	"	gl_FragDepth = texelFetch( samp0, ivec3( texel, 0 ), 0 ).x;\n"
	"}\n";

/*
==================
R_IsEngineShader
==================
*/
static bool R_IsEngineShader( const char* name )
{
	return idStr::Icmpn( name, "_engine", 7 ) == 0;
}

/*
==================
R_EngineUniforms

The parms go into _va_ / _fa_ like the converted HLSL ones, see ConvertCG2GLSL
==================
*/
static void R_EngineUniforms( const renderParm_t* parms, int numParms, rpStage_t stage, idStr& glsl, idStr& layout )
{
	const char* arrayName = ( stage == SHADER_STAGE_VERTEX ) ? VERTEX_UNIFORM_ARRAY_NAME : FRAGMENT_UNIFORM_ARRAY_NAME;

	layout = "uniforms [\n";
	if( numParms > 0 )
	{
		glsl += va( "uniform vec4 %s[%d];\n", arrayName, numParms );
		for( int i = 0; i < numParms; i++ )
		{
			const char* parmName = renderProgManager.GetGLSLParmName( parms[i] );
			glsl += va( "#define %s %s[%d]\n", parmName, arrayName, i );
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
R_EngineShaderSource

The source and the uniform layout of an engine program, by the name and suffix
idRenderProgManager::Init gives it
==================
*/
static bool R_EngineShaderSource( const char* name, const char* suffix, rpStage_t stage, idStr& glsl, idStr& layout )
{
	const char* body = NULL;
	const char* forwardFunctions = NULL;
	const renderParm_t* parms = NULL;
	int numParms = 0;
	bool colorFunctions = false;
	idStr defines;

	if( idStr::Icmp( name, "_engine_forward" ) == 0 )
	{
		const bool skinned = ( idStr::Icmp( suffix, "_skinned" ) == 0 || idStr::Icmp( suffix, "_shadows_skinned" ) == 0 );
		const bool shadows = ( idStr::Icmp( suffix, "_shadows" ) == 0 || idStr::Icmp( suffix, "_shadows_skinned" ) == 0 );
		if( !skinned && !shadows && suffix[0] != '\0' )
		{
			idLib::Warning( "no engine program %s%s", name, suffix );
			return false;
		}
		if( stage == SHADER_STAGE_VERTEX )
		{
			body = forwardLightsVertexShader;
			parms = forwardLightsVertexParms;
			numParms = sizeof( forwardLightsVertexParms ) / sizeof( forwardLightsVertexParms[0] );
			if( skinned )
			{
				defines += "#define USE_GPU_SKINNING\n";
			}
		}
		else
		{
			body = forwardLightsFragmentShader;
			parms = forwardLightsFragmentParms;
			numParms = sizeof( forwardLightsFragmentParms ) / sizeof( forwardLightsFragmentParms[0] );
			colorFunctions = true;
			forwardFunctions = forwardLightsCommon;
			defines += "#define LB_SLOTS 4\n";
			if( shadows )
			{
				defines += "#define LB_SHADOWS\n";
			}
		}
	}
	else if( idStr::Icmp( name, "_engine_shadowcopy" ) == 0 )
	{
		if( stage == SHADER_STAGE_VERTEX )
		{
			body = screenQuadVertexShader;
		}
		else
		{
			body = shadowCopyFragmentShader;
			parms = shadowCopyFragmentParms;
			numParms = sizeof( shadowCopyFragmentParms ) / sizeof( shadowCopyFragmentParms[0] );
		}
	}

	if( body == NULL )
	{
		idLib::Warning( "no engine program %s%s", name, suffix );
		return false;
	}

	glsl = "#version 320 es\n";
	glsl += va( "// %s%s, engine program\n", name, suffix );

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
	glsl += engineShaderCommon;

	R_EngineUniforms( parms, numParms, stage, glsl, layout );

	if( colorFunctions )
	{
		glsl += engineShaderColorFunctions;
	}
	if( forwardFunctions != NULL )
	{
		glsl += forwardFunctions;
	}
	glsl += body;
	return true;
}

#endif // __ENGINESHADERS_GL_H__
