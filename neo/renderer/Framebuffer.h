/*
===========================================================================

Doom 3 BFG Edition GPL Source Code
Copyright (C) 2014-2016 Robert Beckebans

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

#ifndef __FRAMEBUFFER_GL_H__
#define __FRAMEBUFFER_GL_H__

#include <unordered_map>
#include <vector>
#include <string>


constexpr int MAX_SHADOWMAP_RESOLUTIONS = 5;
constexpr int MAX_BLOOM_BUFFERS = 2;
constexpr int MAX_SSAO_BUFFERS = 2;
constexpr int MAX_HIERARCHICAL_ZBUFFERS = 6;
constexpr int MAX_COLOR_ATTACHMENTS = 16;


extern const int shadowMapResolutions[MAX_SHADOWMAP_RESOLUTIONS];


class idImage;
class idCmdArgs;





/*
================================================================================================

RENDER TARGET POOL

Every 2D render target texture of an idImage goes through the pool on Android (idImage::AllocImage).
A physical texture is described by RenderTargetDesc and held by:
	- logical references: the idImages that use it as their texnum
	- attachment references: the framebuffers it is attached to
It is only handed to another image, or deleted, when both are zero. A texture nobody holds stays
cached for maxFrameAge frames, so a resize or a toggled feature finds it again, then it is deleted.

Aliasing: transient targets whose lifetimes never overlap within a frame share one texture when
their images carry the same alias key (idImage::SetRenderTargetAliasKey) and the same description.
The keys are only given to targets whose passes are known not to overlap, see renderTargetAlias_t.

GL keeps the order of the commands that read and write a texture, so a texture can be handed to
another image or deleted while the GPU still has commands for it in flight: no fence is needed.

================================================================================================
*/

// the alias keys, each groups targets that are written and read in passes that never overlap
enum renderTargetAlias_t
{
	RT_ALIAS_NONE = 0,
	RT_ALIAS_QUARTER_A,		// _bloomRender0 (Bloom -> BloomComposite) and _ao0 (SSAO / SSGI of a view)
	RT_ALIAS_QUARTER_B,		// _bloomRender1 and _ao1
	RT_ALIAS_FULL_A,		// _currentNormals (ambient pass -> SSAO / SSGI of a view) and _smaaEdges (PostProcess)
};

struct RenderTargetDesc
{
    GLenum target;
    int width;
    int height;
    int layers;
    GLenum internalFormat;
    GLenum format;
    GLenum type;
    int samples;
    int levels;
    bool isDepth;
    bool linearFilter;

    RenderTargetDesc() : target(0x0DE1 /* GL_TEXTURE_2D */), width(0), height(0), layers(1), internalFormat(0), format(0), type(0), samples(0), levels(1), isDepth(false), linearFilter(true) {}

    bool operator==(const RenderTargetDesc& other) const
    {
        return target == other.target && width == other.width && height == other.height && layers == other.layers &&
               internalFormat == other.internalFormat && format == other.format &&
               type == other.type && samples == other.samples &&
               levels == other.levels && isDepth == other.isDepth && linearFilter == other.linearFilter;
    }
};

struct RenderTarget
{
    GLuint textureID;
    RenderTargetDesc desc;
    int aliasKey;
    int logicalRefs;			// idImages using the texture
    int attachmentRefs;			// framebuffer attachments of the texture
    int lastUsedFrame;
    int64_t bytes;
    std::vector<std::string> owners;	// names of the images holding it

    RenderTarget() : textureID(0), aliasKey(0), logicalRefs(0), attachmentRefs(0), lastUsedFrame(0), bytes(0) {}
};

class RenderTargetPool
{
public:

    struct PoolConfig
    {
        size_t maxPoolSize;
        size_t preallocateCount;
        bool enableLRUEviction;
        int maxFrameAge;

        PoolConfig()
                : maxPoolSize(64)
                , preallocateCount(16)
                , enableLRUEviction(true)
                , maxFrameAge(10)
        {
        }
    };

    struct Stats
    {
        int cacheHits;			// an unused texture of the description was handed out again
        int cacheMisses;		// a new texture had to be created
        int aliasHits;			// a texture held by another image of the alias key was shared
        int allocations;		// textures created
        int deallocations;		// textures deleted
        int evictions;			// deleted because nobody held them for maxFrameAge frames, or for room
        int allocationFailures;
        int peakTextures;
        int64_t peakBytes;

        Stats() { memset( this, 0, sizeof( *this ) ); }
    };

private:
    std::vector<RenderTarget> _pool;
    int _currentFrame;
    PoolConfig _config;
    bool _initialized;
    Stats _stats;

public:
    RenderTargetPool();
    ~RenderTargetPool();

    void Initialize(const PoolConfig& config);

    RenderTargetPool(const RenderTargetPool&) = delete;
    RenderTargetPool& operator=(const RenderTargetPool&) = delete;

    // a texture for the image called name, shared with the holders of aliasKey when it isn't 0
    GLuint Acquire(const std::string& name, const RenderTargetDesc& desc, int aliasKey = RT_ALIAS_NONE);

    // the image called name doesn't use textureID any more
    void Release(GLuint textureID, const std::string& name);
    bool RetainTexture(GLuint textureID);
    void ReleaseAttachmentRef(GLuint textureID);
    bool IsInitialized() const { return _initialized; }

    const RenderTarget* FindTexture(GLuint textureID) const;

    void BeginFrame();
    void EndFrame();
    void Shutdown();

    size_t GetPoolSize() const { return _pool.size(); }
    size_t GetActiveCount() const;
    int64_t GetAllocatedBytes() const;
    int64_t GetIdleBytes() const;		// cached textures nobody holds
    const Stats& GetStats() const { return _stats; }
    void Print() const;

    static int64_t TextureBytes(const RenderTargetDesc& desc);

private:
    bool CreateRenderTarget(const RenderTargetDesc& desc, const std::string& name, int aliasKey, RenderTarget& rt);
    bool CanReuse(const RenderTarget& rt, const RenderTargetDesc& desc) const;
    void DestroyRenderTarget(RenderTarget& rt);
    static void RemoveOwner(RenderTarget& rt, const std::string& name);
};

// bytes per pixel of a GL internal format, a logical estimate: drivers may pad or compress
int R_InternalFormatBytes( GLenum internalFormat );
const char* R_InternalFormatName( GLenum internalFormat );





class Framebuffer
{
public:
    Framebuffer(const char* name, int width, int height);
    ~Framebuffer();

    static void Init();
    static void Shutdown();

    static void CheckFramebuffers();
    static void ResizeFramebuffers();

    static Framebuffer* Find(const char* name);


    static void InitializePool();
    static void BeginFrame();
    static void EndFrame();

    // Optional render targets get their storage when their feature runs and give it back when
    // the feature is turned off (Android, see the top of Framebuffer_GL.cpp). The other builds
    // allocate everything in Init, these only report that the targets are there.
    static bool EnsureSceneTargets();					// hdrFBO: scene color and depth stencil of HDR
    static bool EnsureExposureTargets();				// hdr64FBO, the HDR luminance
    static bool EnsureBloomTargets();					// bloomRenderFBO[]
    static bool EnsureAmbientOcclusionTargets();		// ambientOcclusionFBO[], SSAO / SSGI filtering
    static bool EnsureHierarchicalDepthTargets();		// csDepthFBO[], SSAO / SSGI
    static bool EnsureGeometryBufferTargets();			// geometryBufferFBO, the normals of SSAO / SSGI
    static bool EnsureSMAATargets();					// smaaEdgesFBO, smaaBlendFBO
    static bool EnsureShadowMapTargets( int lod );		// shadowFBO[lod], the per light shadow maps
    static bool EnsureShadowAtlasTarget( int size );	// shadowAtlasFBO
    static bool EnsureShadowCacheTarget( int size );	// shadowCacheFBO
    static void ReleaseShadowCacheTarget();

    // once per frame after the back end: frees the targets of features that are off
    static void UpdateOptionalTargets();

    // listFramebuffers
    static void PrintRenderTargets( bool verbose );

    // drop all attachments, the references to the pool go with them
    void DetachAll();


    void Bind();
    bool IsBound() const;
    bool InvalidateAttachments(const GLenum* attachments, GLsizei count);
    static bool InvalidateDrawFramebufferAttachments(const GLenum* attachments, GLsizei count);
    static void InvalidateDefaultFramebufferAttachments();
    // binds the back buffer and drops all of it, for a pass that overwrites every pixel
    static void InvalidateDefaultFramebufferForOverwrite();
    static void Unbind();
    static bool IsDefaultFramebufferActive();


    void AddColorBuffer(int format, int index, int multiSamples = 0);
    void AddDepthBuffer(int format, int multiSamples = 0);
    void AddStencilBuffer(int format, int multiSamples = 0);


    void AttachImage2D(int target, const idImage* image, int index, int mipmapLod = 0);
    void AttachImage3D(const idImage* image);
    void AttachImageDepth(int target, const idImage* image);
    void AttachImageDepthLayer(const idImage* image, int layer);


    void Check();


    void PurgeFramebuffer();


    uint32_t GetFramebuffer() const { return frameBuffer; }
    int GetWidth() const { return width; }
    int GetHeight() const { return height; }
    bool IsMultiSampled() const { return msaaSamples; }

    void Resize(int width_, int height_)
    {
        width = width_;
        height = height_;
    }
    const char* GetName() const { return fboName.c_str(); }
    GLuint GetColorAttachment( int index ) const { return ( index >= 0 && index < MAX_COLOR_ATTACHMENTS ) ? attachedColor[index] : 0; }
    GLuint GetDepthAttachment() const { return attachedDepth; }
    int GetNumAttachments() const;

    static idList<Framebuffer*> framebuffers;
    static RenderTargetPool renderTargetPool;
    static Framebuffer* currentBoundFramebuffer;

private:
    void TrackColorAttachment( int index, GLuint texture );
    void TrackDepthAttachment( GLuint texture, GLenum attachmentPoint );

    idStr fboName;


    uint32_t frameBuffer = 0;

    GLuint colorTextures[MAX_COLOR_ATTACHMENTS] = {};		// textures AddColorBuffer created for this framebuffer
    GLuint attachedColor[MAX_COLOR_ATTACHMENTS] = {};		// what is attached at each color, any texture
    bool attachedColorRetained[MAX_COLOR_ATTACHMENTS] = {};	// the pool counts that attachment
    GLuint attachedDepth = 0;
    GLenum attachedDepthPoint = 0;
    bool attachedDepthRetained = false;
    int colorFormat = 0;
    uint32_t depthBuffer = 0;
    GLuint depthTexture = 0;
    int depthFormat = 0;

    uint32_t stencilBuffer = 0;
    int stencilFormat = 0;

    int width = 0;
    int height = 0;

    bool msaaSamples = false;
};





struct globalFramebuffers_t
{
    Framebuffer* shadowFBO[MAX_SHADOWMAP_RESOLUTIONS];
    Framebuffer* shadowAtlasFBO;
    Framebuffer* shadowCacheFBO;		// the static casters of stationary lights, see ShadowAtlasPass
    Framebuffer* hdrFBO;
#if defined(USE_HDR_MSAA)
    Framebuffer* hdrNonMSAAFBO;
#endif
    Framebuffer* hdr64FBO;
    Framebuffer* bloomRenderFBO[MAX_BLOOM_BUFFERS];
    Framebuffer* ambientOcclusionFBO[MAX_SSAO_BUFFERS];
    Framebuffer* csDepthFBO[MAX_HIERARCHICAL_ZBUFFERS];
    Framebuffer* geometryBufferFBO;
    Framebuffer* smaaEdgesFBO;
    Framebuffer* smaaBlendFBO;
};

extern globalFramebuffers_t globalFramebuffers;

#endif