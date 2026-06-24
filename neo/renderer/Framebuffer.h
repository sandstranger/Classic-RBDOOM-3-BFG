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





struct RenderTargetDesc
{
    int width;
    int height;
    GLenum internalFormat;
    GLenum format;
    GLenum type;
    int samples;
    bool isDepth;

    bool operator==(const RenderTargetDesc& other) const
    {
        return width == other.width &&
               height == other.height &&
               internalFormat == other.internalFormat &&
               format == other.format &&
               type == other.type &&
               samples == other.samples &&
               isDepth == other.isDepth;
    }
};

struct RenderTarget
{
    GLuint textureID;
    GLuint fboID;
    RenderTargetDesc desc;
    bool inUse;
    std::string debugName;
    int lastUsedFrame;

    RenderTarget() : textureID(0), fboID(0), inUse(false), lastUsedFrame(0) {}
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

private:
    std::vector<RenderTarget> _pool;
    std::unordered_map<std::string, size_t> _nameToIndex;
    int _currentFrame;
    PoolConfig _config;

public:
    RenderTargetPool();
    ~RenderTargetPool();

    void Initialize(const PoolConfig& config);

    RenderTargetPool(const RenderTargetPool&) = delete;
    RenderTargetPool& operator=(const RenderTargetPool&) = delete;


    GLuint Acquire(const std::string& name, const RenderTargetDesc& desc);


    void Release(const std::string& name);


    GLuint GetFBO(const std::string& name) const;


    GLuint GetTexture(const std::string& name) const;


    void BeginFrame();


    void EndFrame();


    void Shutdown();


    size_t GetPoolSize() const { return _pool.size(); }
    size_t GetActiveCount() const;

private:
    RenderTarget CreateRenderTarget(const RenderTargetDesc& desc, const std::string& name);
    bool CanReuse(const RenderTarget& rt, const RenderTargetDesc& desc) const;
    void DestroyRenderTarget(RenderTarget& rt);
};





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


    void Bind();
    bool IsBound() const;
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
    static idList<Framebuffer*> framebuffers;
    static RenderTargetPool renderTargetPool;
    static Framebuffer* currentBoundFramebuffer;

private:
    idStr fboName;


    uint32_t frameBuffer;

    GLuint colorTextures[MAX_COLOR_ATTACHMENTS] = {};
    int colorFormat;
    uint32_t depthBuffer;
    GLuint depthTexture;
    int depthFormat;

    uint32_t stencilBuffer;
    int stencilFormat;

    int width;
    int height;

    bool msaaSamples;
};





struct globalFramebuffers_t
{
    Framebuffer* shadowFBO[MAX_SHADOWMAP_RESOLUTIONS];
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