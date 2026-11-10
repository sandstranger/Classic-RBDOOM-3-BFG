/*
===========================================================================

Doom 3 BFG Edition GPL Source Code
Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company.
Copyright (C) 2016-2017 Dustin Land
Copyright (C) 2017 Robert Beckebans

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

#ifndef __RENDERER_BACKEND_H__
#define __RENDERER_BACKEND_H__



bool			GL_CheckErrors_( const char* filename, int line );
#if 1 // !defined(RETAIL)
#define         GL_CheckErrors()	GL_CheckErrors_(__FILE__, __LINE__)
#else
#define         GL_CheckErrors()	false
#endif
// RB end

struct tmu_t
{
	unsigned int	current2DMap;
	unsigned int	current2DArray;
	unsigned int	currentCubeMap;
};

const int MAX_MULTITEXTURE_UNITS =	8;

enum stencilFace_t
{
	STENCIL_FACE_FRONT,
	STENCIL_FACE_BACK,
	STENCIL_FACE_NUM
};

struct backEndCounters_t
{
	int		c_surfaces;
	int		c_shaders;
	
	int		c_drawElements;
	int		c_drawIndexes;
	
	int		c_shadowElements;
	int		c_shadowIndexes;
	
	int		c_copyFrameBuffer;

	float	c_overDraw;

	int		totalMicroSec;		// total microseconds for backend run
	int		shadowMicroSec;

	// r_showRenderStats: what the frame cost, by what it was drawn for (draw calls, see drawCategory_t)
	int		c_drawsByCategory[4];
	int		c_indexesByCategory[4];
	int		c_framebufferBinds;
	int		c_programBinds;

	// lights
	int		c_visibleLights;		// lights with surfaces to light
	int		c_batchedLights;		// lights the multi light draws took
	int		c_forwardBatchDraws;	// multi light draws of model surfaces
	int		c_forwardWorldDraws;	// multi light draws of the world surfaces of a material
	int		c_forwardBatchPairs;	// light stage / surface pairs they lit
	int		c_interactionDraws;		// single light interaction draws

	// shadows
	int		c_shadowedLights;		// lights drawn with shadows
	int		c_shadowMapSides;		// shadow map sides rendered this frame
	int		c_shadowCasterDraws;	// caster draws into shadow maps
	int		c_shadowCacheHits;		// lights sampled from the shadow cache, nothing rendered
	int		c_shadowCacheCopies;	// lights whose cached static casters were copied under their dynamic casters
	int		c_shadowCacheMisses;	// lights whose cache was built this frame
	int		c_shadowCacheInvalidations;
	int		c_shadowAtlasLights;
	int		c_shadowAtlasOverflow;	// lights that fell back to the per light shadow maps
	int		c_shadowAtlasDowngrades;// sides that got a smaller tile than their LOD so everything fits
	int		c_shadowAtlasUsedTexels;
	int		c_shadowAtlasTexels;
	int		shadowStaticMicroSec;	// CPU time recording the shadow cache
	int		shadowDynamicMicroSec;	// CPU time recording the frame atlas
};

// what a draw call is for, see backEndCounters_t::c_drawsByCategory
enum drawCategory_t
{
	DRAW_CATEGORY_SCENE,		// depth, ambient, shader passes, fog
	DRAW_CATEGORY_LIGHTING,		// interactions, single and multi light
	DRAW_CATEGORY_SHADOW,		// shadow maps and stencil shadow volumes
	DRAW_CATEGORY_POST,			// post processing and resolves
};

struct gfxImpParms_t
{
	int		x;				// ignored in fullscreen
	int		y;				// ignored in fullscreen
	int		width;
	int		height;
	int		fullScreen;		// 0 = windowed, otherwise 1 based monitor number to go full screen on
	// -1 = borderless window for spanning multiple displays
	int		displayHz;
	int		multiSamples;
};

#define MAX_DEBUG_LINES		16384
#define MAX_DEBUG_TEXT		512
#define MAX_DEBUG_POLYGONS	8192

struct debugLine_t
{
	idVec4		rgb;
	idVec3		start;
	idVec3		end;
	bool		depthTest;
	int			lifeTime;
};

struct debugText_t
{
	idStr		text;
	idVec3		origin;
	float		scale;
	idVec4		color;
	idMat3		viewAxis;
	int			align;
	int			lifeTime;
	bool		depthTest;
};

struct debugPolygon_t
{
	idVec4		rgb;
	idWinding	winding;
	bool		depthTest;
	int			lifeTime;
};

struct gpuInstanceData_t
{
	float mvp[16];
	float modelView[16];
	float localViewOrigin[4];
	float localLightOrigin[4];
};

void RB_SetMVP( const idRenderMatrix& mvp );
void RB_SetVertexColorParms( stageVertexColor_t svc );
//void RB_GetShaderTextureMatrix( const float* shaderRegisters, const textureStage_t* texture, float matrix[16] );
//void RB_LoadShaderTextureMatrix( const float* shaderRegisters, const textureStage_t* texture );
//void RB_BakeTextureMatrixIntoTexgen( idPlane lightProject[3], const float* textureMatrix );

//bool ChangeDisplaySettingsIfNeeded( gfxImpParms_t parms );
//bool CreateGameWindow( gfxImpParms_t parms );

#if defined( USE_VULKAN )

struct gpuInfo_t
{
	VkPhysicalDevice					device;
	VkPhysicalDeviceProperties			props;
	VkPhysicalDeviceMemoryProperties	memProps;
	VkSurfaceCapabilitiesKHR			surfaceCaps;
	idList< VkSurfaceFormatKHR >		surfaceFormats;
	idList< VkPresentModeKHR >			presentModes;
	idList< VkQueueFamilyProperties >	queueFamilyProps;
	idList< VkExtensionProperties >		extensionProps;
};

struct vulkanContext_t
{
	uint64							frameCounter;
	uint32							frameParity;
	
	vertCacheHandle_t				jointCacheHandle;
	uint64							stencilOperations[ STENCIL_FACE_NUM ];
	
	VkInstance						instance;
	
	// selected physical device
	VkPhysicalDevice				physicalDevice;
	VkPhysicalDeviceFeatures		physicalDeviceFeatures;
	
	// logical device
	VkDevice						device;
	
	VkQueue							graphicsQueue;
	VkQueue							presentQueue;
	int								graphicsFamilyIdx;
	int								presentFamilyIdx;
	VkDebugReportCallbackEXT		callback;
	
	idList< const char* >			instanceExtensions;
	idList< const char* >			deviceExtensions;
	idList< const char* >			validationLayers;
	
	// selected GPU
	gpuInfo_t* 						gpu;
	
	// all GPUs found on the system
	idList< gpuInfo_t >				gpus;
	
	VkCommandPool					commandPool;
	idArray< VkCommandBuffer, NUM_FRAME_DATA >	commandBuffer;
	idArray< VkFence, NUM_FRAME_DATA >			commandBufferFences;
	idArray< bool, NUM_FRAME_DATA >				commandBufferRecorded;
	
	VkSurfaceKHR					surface;
	VkPresentModeKHR				presentMode;
	VkFormat						depthFormat;
	VkRenderPass					renderPass;
	VkPipelineCache					pipelineCache;
	VkSampleCountFlagBits			sampleCount;
	bool							supersampling;
	
	int								fullscreen;
	VkSwapchainKHR					swapchain;
	VkFormat						swapchainFormat;
	VkExtent2D						swapchainExtent;
	uint32							currentSwapIndex;
	VkImage							msaaImage;
	VkImageView						msaaImageView;
#if defined( USE_AMD_ALLOCATOR )
	VmaAllocation					msaaVmaAllocation;
	VmaAllocationInfo				msaaAllocation;
#else
	vulkanAllocation_t				msaaAllocation;
#endif
	idArray< VkImage, NUM_FRAME_DATA >			swapchainImages;
	idArray< VkImageView, NUM_FRAME_DATA >		swapchainViews;
	
	idArray< VkFramebuffer, NUM_FRAME_DATA >	frameBuffers;
	idArray< VkSemaphore, NUM_FRAME_DATA >		acquireSemaphores;
	idArray< VkSemaphore, NUM_FRAME_DATA >		renderCompleteSemaphores;
	
	int											currentImageParm;
	idArray< idImage*, MAX_IMAGE_PARMS >		imageParms;
};

extern vulkanContext_t vkcontext;

#else //if defined( ID_OPENGL )

struct glContext_t
{
//	bool		bAnisotropicFilterAvailable;
//	bool		bTextureLODBiasAvailable;

//	float		maxTextureAnisotropy;

	tmu_t		tmu[ MAX_MULTITEXTURE_UNITS ];
	uint64		stencilOperations[ STENCIL_FACE_NUM ];
	int			activeTextureUnit;		// the unit glActiveTexture selected last, -1 when unknown
};

extern glContext_t glcontext;

// idImage::Bind skips binding a texture the unit has bound already. Texture binds and
// deletes outside of it go through these, they drop what Bind knows about the units.
void GL_InvalidateTextureBindings();
void R_DirectBindTexture( unsigned int target, unsigned int texture );
void R_DirectDeleteTextures( int count, const unsigned int* textures );

#endif

/*
===========================================================================

idRenderBackend

all state modified by the back end is separated from the front end state

===========================================================================
*/
class idRenderBackend
{
	friend class Framebuffer;
	
public:
	idRenderBackend();
	~idRenderBackend();
	
	void				Init();
	void				Shutdown();
#if defined(ANDROID) && !defined(USE_VULKAN)
    // Submit pending MDI commands before any draw state changes.
    void            FlushMultiDrawBatch();
#endif
	
	void				ExecuteBackEndCommands( const emptyCommand_t* cmds );
	void				StereoRenderExecuteBackEndCommands( const emptyCommand_t* const allCmds );
	void				GL_BlockingSwapBuffers();
	
	void				Print();
	void				CheckCVars();
	
private:
	void				DrawFlickerBox();
	
#if defined(ANDROID) && !defined(USE_VULKAN)
	void				DrawElementsWithCounters( const drawSurf_t* surf, const gpuInstanceData_t* instances = NULL, int instanceCount = 1 );
#else
	void				DrawElementsWithCounters( const drawSurf_t* surf );
#endif
#if defined(ANDROID) && !defined(USE_VULKAN)
	void				DrawStencilShadowPass( const drawSurf_t* drawSurf, const bool renderZPass,
					const gpuInstanceData_t* instances = NULL, int instanceCount = 1 );
#else
	void				DrawStencilShadowPass( const drawSurf_t* drawSurf, const bool renderZPass );
#endif
	
	void				SetColorMappings();
	void				ResizeImages();
	
	void				DrawViewInternal( const viewDef_t* viewDef, const int stereoEye );
	void				DrawView( const void* data, const int stereoEye );
	void				CopyRender( const void* data );
	
	void				BindVariableStageImage( const textureStage_t* texture, const float* shaderRegisters );
	void				PrepareStageTexturing( const shaderStage_t* pStage, const drawSurf_t* surf );
	void				FinishStageTexturing( const shaderStage_t* pStage, const drawSurf_t* surf );
	
	void				ResetViewportAndScissorToDefaultCamera( const viewDef_t* _viewDef );
	
	void				FillDepthBufferGeneric( const drawSurf_t* const* drawSurfs, int numDrawSurfs );
	void				FillDepthBufferFast( drawSurf_t** drawSurfs, int numDrawSurfs );
	
	void				T_BlendLight( const drawSurf_t* drawSurfs, const viewLight_t* vLight );
	void				BlendLight( const drawSurf_t* drawSurfs, const drawSurf_t* drawSurfs2, const viewLight_t* vLight );
	void				T_BasicFog( const drawSurf_t* drawSurfs, const idPlane fogPlanes[ 4 ], const idRenderMatrix* inverseBaseLightProject );
	void				FogPass( const drawSurf_t* drawSurfs,  const drawSurf_t* drawSurfs2, const viewLight_t* vLight );
	void				FogAllLights();
	
	void				SetupInteractionStage( const shaderStage_t* surfaceStage, const float* surfaceRegs, const float lightColor[4],
			idVec4 matrix[2], float color[4] );
			
	void				DrawInteractions( const viewDef_t* _viewDef );
	void				DrawSingleInteraction( drawInteraction_t* din, const gpuInstanceData_t* instances = NULL, int instanceCount = 1 );
	int					DrawShaderPasses( const drawSurf_t* const* const drawSurfs, const int numDrawSurfs,
										  const float guiStereoScreenOffset, const int stereoEye );
										  
	// the surfaces a multi light draw lit (drawSurf_t::forwardBatched) are skipped
	void				RenderInteractions( const drawSurf_t* surfList, const viewLight_t* vLight, int depthFunc, bool performStencilTest, bool useLightDepthBounds );

	// RB
	void				AmbientPass( const drawSurf_t* const* drawSurfs, int numDrawSurfs, bool fillGbuffer );
	void				ShadowMapPass( const drawSurf_t* drawSurfs, const viewLight_t* vLight, int side, bool restoreFramebuffer = true );

	// shadow map helpers shared by ShadowMapPass and the shadow atlas
	void				SetupShadowMapMatrices( const viewLight_t* vLight, int side, idRenderMatrix& lightViewRenderMatrix, idRenderMatrix& lightProjectionRenderMatrix );
	void				BeginShadowMapSide();
	// which casters DrawShadowMapCasters draws: the shadow cache holds the static ones
	enum shadowCasterFilter_t
	{
		SHADOW_CASTERS_ALL,
		SHADOW_CASTERS_STATIC,
		SHADOW_CASTERS_DYNAMIC,
	};

	// casterFilter: shadowCasterFilter_t, which casters of the chain are drawn
	// cachedCasters: with SHADOW_CASTERS_DYNAMIC, the static casters that aren't in this sorted list are drawn too
	void				DrawShadowMapCasters( const drawSurf_t* drawSurfs, const viewLight_t* vLight, int side,
											  const idRenderMatrix& lightViewRenderMatrix, const idRenderMatrix& lightProjectionRenderMatrix, int casterFilter = 0,
											  const idList<uint32>* cachedCasters = NULL );

#if !defined( USE_VULKAN )
	// tile based GPUs: render the shadow maps of all lights of the frame into one atlas
	// before any view, so that shadow maps never interrupt the main render pass
	struct shadowAtlasLight_t
	{
		const viewLight_t*	vLight;
		int					numSides;
		int					tileSize;			// texels of one side, including the guard border
		int					tileX[6];
		int					tileY[6];
		idRenderMatrix		worldToShadow[6];	// world space -> atlas texture coordinates and depth
		idImage*			image;				// the atlas that holds the sides: the frame atlas or the shadow cache
		int					atlasSize;			// texels of that atlas, for the PCF taps
	};

	static const int	MAX_SHADOW_ATLAS_LIGHTS = 64;

	void				ShadowAtlasPass( const emptyCommand_t* cmds );
	const shadowAtlasLight_t* FindShadowAtlasLight( const viewLight_t* vLight ) const;

	shadowAtlasLight_t	shadowAtlasLights[MAX_SHADOW_ATLAS_LIGHTS];
	int					numShadowAtlasLights;
	int					shadowAtlasSize;
	bool				hdrTargetClearedThisFrame;	// the first HDR view of a frame clears the HDR color
	bool				skipBackBufferClear;		// a HDR view's tonemap replaces the whole back buffer this frame
	int					drawCategory;				// drawCategory_t of the draws now, for the counters
#endif

#if defined(ANDROID) && !defined(USE_VULKAN)
	/*
	Shadow cache: the static casters of a stationary light are rendered once into tiles of a
	persistent atlas. A light without dynamic casters is sampled straight from its cache tiles,
	one with dynamic casters copies its cached depth into its frame atlas tiles and draws only the
	dynamic casters on top. See the SHADOW CACHE section of RenderBackend.cpp for the rules.
	*/
	static const int	MAX_SHADOW_CACHE_ENTRIES = 64;
	static const int	MAX_SHADOW_CACHE_LEVELS = 8;	// buddy levels: the cache size down to 1/128 of it

	struct shadowCacheEntry_t
	{
		int					generation;			// viewLight_t::shadowCacheGeneration it was built for, 0 = free
		int					tileSize;
		int					numSides;
		int					level;				// buddy level of its blocks
		int					blockX[6];
		int					blockY[6];
		int					lastUsedFrame;
		int					missingFrames;		// frames in a row a view needed static casters it doesn't hold
		idList<uint32>*		casters;			// sorted drawSurf_t::shadowCacheId of the static casters it holds, allocated once
	};

	int					ShadowCacheAllocBlock( int level, int& x, int& y );
	void				ShadowCacheFreeBlock( int level, int x, int y );
	void				ShadowCacheFreeEntry( shadowCacheEntry_t& entry );
	void				ShadowCacheReset( int size );
	shadowCacheEntry_t*	ShadowCacheFind( int generation );
	shadowCacheEntry_t*	ShadowCacheAlloc( int generation, int tileSize, int numSides );
	void				DrawShadowCacheCopy( int cacheX, int cacheY, int frameX, int frameY, int size );

	shadowCacheEntry_t	shadowCacheEntries[MAX_SHADOW_CACHE_ENTRIES];
	idList<int>*		shadowCacheFreeBlocks[MAX_SHADOW_CACHE_LEVELS];	// packed x << 16 | y; pointers, no destructor at exit()
	int					shadowCacheSize;		// texels of the cache atlas the entries live in, 0 = no entries
	int					shadowCacheFrame;
	unsigned int		shadowCacheSampler;		// sampler object without depth compare, for the copy

	/*
	Forward multi light interactions: lights whose shadows don't need the stencil buffer (no
	shadows, or shadow maps in the atlas or the shadow cache) light a surface several at once, one
	draw per interaction of the surface for up to LIGHT_BATCH_SLOTS light stages, into the target
	of the view like the interactions. The world surfaces of a material that the same lights reach
	are drawn together. See FORWARD MULTI LIGHT INTERACTIONS in RenderBackend.cpp.
	The light data goes into a uniform buffer, LIGHT_BATCH_VEC4S per light stage.
	*/
	static const int	LIGHT_BATCH_SLOTS = 4;			// light stages per draw, the surface's maps take three units
	static const int	LIGHT_BATCH_VEC4S = 36;			// vec4s per light stage in the uniform buffer
	static const int	MAX_LIGHT_BATCH_ENTRIES = 256;

	struct lightBatchEntry_t
	{
		const viewLight_t*		vLight;
		const shaderStage_t*	stage;
		idImage*				falloffImage;
		idImage*				projectionImage;
		idScreenRect			rect;				// in the viewport
		bool					shadowMapped;		// reads a shadow atlas
		float					data[LIGHT_BATCH_VEC4S * 4];
	};

	void				LightBatchBeginView( const viewDef_t* _viewDef );
	bool				LightBatchable( const viewLight_t* vLight ) const;
	int					LightBatchAddEntries( const viewLight_t* vLight );
	void				LightBatchViewParms( idRenderMatrix& worldToView, idRenderMatrix& viewToWorld );
	int					ShadowSideMatrices( const viewLight_t* vLight, const shadowAtlasLight_t* atlasLight, idRenderMatrix worldToShadow[6] );
	void				LightBatchSetGlobals();
	void				LightBatchBindSlot( int slot, int firstUnit, const lightBatchEntry_t& entry );
	void				LightBatchBindShadowImages( int frameAtlasUnit, int cacheAtlasUnit, int jitterUnit );
	void				DrawForwardLightBatches();
	int					CollectSurfaceInteractions( const drawSurf_t* surf, bool ambientLight, drawInteraction_t* out, int maxInteractions );
	bool				UploadLightBatchData( const float* data, int numVec4s, unsigned int& buffer, int& offset );
	// 32 bit indexes into the static vertex buffer for this frame, see DrawStaticIndexRange
	bool				UploadLightBatchIndexes( const uint32* indexes, int numIndexes, unsigned int& buffer, int& firstIndex );
	void				DrawStaticIndexRange( unsigned int indexBuffer, int firstIndex, int numIndexes );

	lightBatchEntry_t	lightBatchEntries[MAX_LIGHT_BATCH_ENTRIES];	// the batchable light stages of the view
	int					numLightBatchEntries;
#endif

#if defined(ANDROID) && !defined( USE_VULKAN )
	// world surfaces of several areas drawn with one call, see WorldMergePass
	void				WorldMergePass( const emptyCommand_t* cmds );
	void				DrawWorldMergeRange( int firstIndex, int numIndexes );
	void				BeginWorldMergeGroup( const drawSurf_t* surf );

	// while set, DrawElementsWithCounters draws the merged group of worldMergeSurf in place of worldMergeSurf
	const drawSurf_t*	worldMergeSurf;
	int					worldMergeFirstIndex;
	int					worldMergeNumIndexes;
	unsigned int		worldMergeIndexBuffer;		// this frame's 32 bit indexes into the static vertex buffer

#endif

	// the 3D views draw into hdrFBO: HDR
	bool				UseSceneFramebuffer() const;

	void				StencilShadowPass( const drawSurf_t* drawSurfs, const viewLight_t* vLight );
	void				StencilSelectLight( const viewLight_t* vLight );
	
	// RB: HDR stuff
	
	// TODO optimize and replace with compute shader
	void				CalculateAutomaticExposure();
	void				Tonemap( const viewDef_t* viewDef );
	void				Bloom( const viewDef_t* viewDef );
	void				BloomComposite( const viewDef_t* viewDef );
	idImage*			bloomResultImage;		// Bloom's blurred glare, added by BloomComposite
	
	void				DrawScreenSpaceAmbientOcclusion( const viewDef_t* _viewDef );
	void				DrawScreenSpaceGlobalIllumination( const viewDef_t* _viewDef );
	
	// Experimental feature
	void				MotionBlur();
	void				PostProcess( const void* data );
	
private:
	void				GL_StartFrame();
	void				GL_EndFrame();
	
public:
	uint64				GL_GetCurrentState() const;
private:
	uint64				GL_GetCurrentStateMinusStencil() const;
	void				GL_SetDefaultState();
	
	void				GL_State( uint64 stateBits, bool forceGlState = false );
	void				GL_SeparateStencil( stencilFace_t face, uint64 stencilBits );
	
	void				GL_SelectTexture( int unit );
public:
	// glActiveTexture for the unit GL_SelectTexture picked, for texture calls of its own after a Bind
	void				GL_ActivateTextureUnit();
private:
//	void				GL_BindTexture( idImage* image );

//	void				GL_CopyFrameBuffer( idImage* image, int x, int y, int imageWidth, int imageHeight );
//	void				GL_CopyDepthBuffer( idImage* image, int x, int y, int imageWidth, int imageHeight );

	// RB: HDR parm
	void				GL_Clear( bool color, bool depth, bool stencil, byte stencilValue, float r, float g, float b, float a, bool clearHDR = true );
	
	void				GL_DepthBoundsTest( const float zmin, const float zmax );
	void				GL_PolygonOffset( float scale, float bias );
	
	void				GL_Scissor( int x /* left*/, int y /* bottom */, int w, int h );
	void				GL_Viewport( int x /* left */, int y /* bottom */, int w, int h );
	
	ID_INLINE void		GL_Scissor( const idScreenRect& rect )
	{
		GL_Scissor( rect.x1, rect.y1, rect.x2 - rect.x1 + 1, rect.y2 - rect.y1 + 1 );
	}
	ID_INLINE void		GL_Viewport( const idScreenRect& rect )
	{
		GL_Viewport( rect.x1, rect.y1, rect.x2 - rect.x1 + 1, rect.y2 - rect.y1 + 1 );
	}
	
	ID_INLINE void	GL_ViewportAndScissor( int x, int y, int w, int h )
	{
		GL_Viewport( x, y, w, h );
		GL_Scissor( x, y, w, h );
	}
	
	ID_INLINE void	GL_ViewportAndScissor( const idScreenRect& rect )
	{
		GL_Viewport( rect );
		GL_Scissor( rect );
	}
	
	void				GL_Color( float r, float g, float b, float a );
	ID_INLINE void		GL_Color( float r, float g, float b )
	{
		GL_Color( r, g, b, 1.0f );
	}
	
	ID_INLINE void		GL_Color( const idVec3& color )
	{
		GL_Color( color[0], color[1], color[2], 1.0f );
	}
	
	ID_INLINE void		GL_Color( const idVec4& color )
	{
		GL_Color( color[0], color[1], color[2], color[3] );
	}
	
//	void				GL_Color( float* color );

	void				SetBuffer( const void* data );
	
private:
	void				DBG_SimpleSurfaceSetup( const drawSurf_t* drawSurf );
	void				DBG_SimpleWorldSetup();
	void				DBG_PolygonClear();
	void				DBG_ShowDestinationAlpha();
	void				DBG_ScanStencilBuffer();
	void				DBG_CountStencilBuffer();
	void				DBG_ColorByStencilBuffer();
	void				DBG_ShowOverdraw();
	void				DBG_ShowIntensity();
	void				DBG_ShowDepthBuffer();
	void				DBG_ShowLightCount();
//	void				DBG_EnterWeaponDepthHack();
//	void				DBG_EnterModelDepthHack( float depth );
//	void				DBG_LeaveDepthHack();
	void				DBG_RenderDrawSurfListWithFunction( drawSurf_t** drawSurfs, int numDrawSurfs );
	void				DBG_ShowSilhouette();
	void				DBG_ShowTris( drawSurf_t** drawSurfs, int numDrawSurfs );
	void				DBG_ShowSurfaceInfo( drawSurf_t** drawSurfs, int numDrawSurfs );
	void				DBG_ShowViewEntitys( viewEntity_t* vModels );
	void				DBG_ShowTexturePolarity( drawSurf_t** drawSurfs, int numDrawSurfs );
	void				DBG_ShowUnsmoothedTangents( drawSurf_t** drawSurfs, int numDrawSurfs );
	void				DBG_ShowTangentSpace( drawSurf_t** drawSurfs, int numDrawSurfs );
	void				DBG_ShowVertexColor( drawSurf_t** drawSurfs, int numDrawSurfs );
	void				DBG_ShowNormals( drawSurf_t** drawSurfs, int numDrawSurfs );
	void				DBG_ShowTextureVectors( drawSurf_t** drawSurfs, int numDrawSurfs );
	void				DBG_ShowDominantTris( drawSurf_t** drawSurfs, int numDrawSurfs );
	void				DBG_ShowEdges( drawSurf_t** drawSurfs, int numDrawSurfs );
	void				DBG_ShowLights();
	void				DBG_ShowShadowMapLODs(); // RB
	void				DBG_ShowPortals();
	
	void				DBG_ShowDebugText();
	void				DBG_ShowDebugLines();
	void				DBG_ShowDebugPolygons();
	
	void				DBG_ShowCenterOfProjection();
	void				DBG_ShowLines();
	
	void				DBG_TestGamma();
	void				DBG_TestGammaBias();
	void				DBG_TestImage();
	void				DBG_ShowShadowMaps(); // RB
	void				DBG_ShowTrace( drawSurf_t** drawSurfs, int numDrawSurfs );
	void				DBG_RenderDebugTools( drawSurf_t** drawSurfs, int numDrawSurfs );
	
public:
	backEndCounters_t	pc;
	
	// surfaces used for code-based drawing
	drawSurf_t			unitSquareSurface;
	drawSurf_t			zeroOneCubeSurface;
	drawSurf_t			testImageSurface;
#if defined(ANDROID) && !defined(USE_VULKAN)
	unsigned int		gpuInstanceBuffer = 0;
#endif
	
private:
	uint64				glStateBits;
	
	const viewDef_t* 	viewDef;
	
	const viewEntity_t*	currentSpace;			// for detecting when a matrix must change
	idScreenRect		currentScissor;			// for scissor clipping, local inside renderView viewport
	
	bool				currentRenderCopied;	// true if any material has already referenced _currentRender
	
	idRenderMatrix		prevMVP[2];				// world MVP from previous frame for motion blur
	
	// RB begin
	idRenderMatrix		shadowV[6];				// shadow depth view matrix
	idRenderMatrix		shadowP[6];				// shadow depth projection matrix
	
	float				hdrAverageLuminance;
	float				hdrMaxLuminance;
	float				hdrTime;
	float				hdrKey;
	// RB end
	
private:
#if !defined( USE_VULKAN )
	int					currenttmu;
	
	unsigned int		currentVertexBuffer;
	unsigned int		currentIndexBuffer;
	Framebuffer*		currentFramebuffer;		// RB: for offscreen rendering
	
	vertexLayoutType_t	vertexLayout;
	
	float				polyOfsScale;
	float				polyOfsBias;
	
public:
	int					GetCurrentTextureUnit() const
	{
		return currenttmu;
	}
	
#endif // !defined( USE_VULKAN )
};

#endif
