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

static idCVar r_postProcessScale("r_postProcessScale", "0.25", CVAR_FLOAT | CVAR_ARCHIVE,
                                 "Resolution scale for post-process effects (bloom, SSAO). Range: 0.25 to 1.0");


const int shadowMapResolutions[MAX_SHADOWMAP_RESOLUTIONS] = { 512, 256, 256, 128, 64 };

idList<Framebuffer*> Framebuffer::framebuffers;
RenderTargetPool Framebuffer::renderTargetPool;
Framebuffer* Framebuffer::currentBoundFramebuffer = nullptr;
globalFramebuffers_t globalFramebuffers;

RenderTargetPool::RenderTargetPool() : _currentFrame(0)
{
}

RenderTargetPool::~RenderTargetPool()
{
    Shutdown();
}

GLuint RenderTargetPool::Acquire(const std::string& name, const RenderTargetDesc& desc)
{
    auto it = _nameToIndex.find(name);
    if (it != _nameToIndex.end())
    {
        RenderTarget& rt = _pool[it->second];
        if (CanReuse(rt, desc))
        {
            rt.inUse = true;
            rt.lastUsedFrame = _currentFrame;
            return rt.textureID;
        }
        else
        {
            DestroyRenderTarget(rt);
            rt = CreateRenderTarget(desc, name);
            rt.inUse = true;
            rt.lastUsedFrame = _currentFrame;
            return rt.textureID;
        }
    }

    for (size_t i = 0; i < _pool.size(); ++i)
    {
        RenderTarget& rt = _pool[i];
        if (!rt.inUse && CanReuse(rt, desc))
        {
            rt.inUse = true;
            rt.lastUsedFrame = _currentFrame;
            rt.debugName = name;
            _nameToIndex[name] = i;
            return rt.textureID;
        }
    }

    RenderTarget rt = CreateRenderTarget(desc, name);
    rt.inUse = true;
    rt.lastUsedFrame = _currentFrame;
    _pool.push_back(rt);
    _nameToIndex[name] = _pool.size() - 1;
    return rt.textureID;
}

void RenderTargetPool::Release(const std::string& name)
{
    auto it = _nameToIndex.find(name);
    if (it != _nameToIndex.end())
    {
        _pool[it->second].inUse = false;
    }
}

GLuint RenderTargetPool::GetFBO(const std::string& name) const
{
    auto it = _nameToIndex.find(name);
    if (it != _nameToIndex.end())
    {
        return _pool[it->second].fboID;
    }
    return 0;
}

GLuint RenderTargetPool::GetTexture(const std::string& name) const
{
    auto it = _nameToIndex.find(name);
    if (it != _nameToIndex.end())
    {
        return _pool[it->second].textureID;
    }
    return 0;
}

void RenderTargetPool::BeginFrame()
{
    _currentFrame++;
    _nameToIndex.clear();
}

void RenderTargetPool::EndFrame()
{
    for (RenderTarget& rt : _pool)
    {
        rt.inUse = false;
    }
    _nameToIndex.clear();
}

void RenderTargetPool::Shutdown()
{
    for (RenderTarget& rt : _pool)
    {
        DestroyRenderTarget(rt);
    }
    _pool.clear();
    _nameToIndex.clear();
}

size_t RenderTargetPool::GetActiveCount() const
{
    size_t count = 0;
    for (const RenderTarget& rt : _pool)
    {
        if (rt.inUse)
        {
            count++;
        }
    }
    return count;
}

RenderTarget RenderTargetPool::CreateRenderTarget(const RenderTargetDesc& desc, const std::string& name)
{
    RenderTarget rt;
    rt.desc = desc;
    rt.debugName = name;

    glGenTextures(1, &rt.textureID);

    if (desc.samples > 0)
    {
        glBindTexture(GL_TEXTURE_2D_MULTISAMPLE, rt.textureID);
        glTexStorage2DMultisample(GL_TEXTURE_2D_MULTISAMPLE, desc.samples, desc.internalFormat, desc.width, desc.height, GL_FALSE);
    }
    else
    {
        glBindTexture(GL_TEXTURE_2D, rt.textureID);
        glTexStorage2D(GL_TEXTURE_2D, 1, desc.internalFormat, desc.width, desc.height);


        if (desc.isDepth)
        {
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        }
        else
        {
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        }

        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);


        if (desc.isDepth)
        {
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL);
        }
    }

    glGenFramebuffers(1, &rt.fboID);
    glBindFramebuffer(GL_FRAMEBUFFER, rt.fboID);

    GLenum target = (desc.samples > 0) ? GL_TEXTURE_2D_MULTISAMPLE : GL_TEXTURE_2D;

    if (desc.isDepth)
    {
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, target, rt.textureID, 0);


        GLenum drawBuffers[] = { GL_NONE };
        glDrawBuffers(1, drawBuffers);


        glReadBuffer(GL_NONE);
    }
    else
    {
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, target, rt.textureID, 0);
    }

    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE)
    {
        common->Error("RenderTargetPool::CreateRenderTarget(%s): Framebuffer incomplete (0x%X)", name.c_str(), status);
    }

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glBindTexture(GL_TEXTURE_2D, 0);

    return rt;
}

bool RenderTargetPool::CanReuse(const RenderTarget& rt, const RenderTargetDesc& desc) const
{
    return rt.desc.width == desc.width &&
           rt.desc.height == desc.height &&
           rt.desc.internalFormat == desc.internalFormat &&
           rt.desc.samples == desc.samples &&
           rt.desc.isDepth == desc.isDepth;
}

void RenderTargetPool::DestroyRenderTarget(RenderTarget& rt)
{
    if (rt.textureID != 0)
    {
        glDeleteTextures(1, &rt.textureID);
        rt.textureID = 0;
    }
    if (rt.fboID != 0)
    {
        glDeleteFramebuffers(1, &rt.fboID);
        rt.fboID = 0;
    }
}


static void R_ListFramebuffers_f(const idCmdArgs& args)
{
    if (!glConfig.framebufferObjectAvailable)
    {
        common->Printf("GL_EXT_framebuffer_object is not available.\n");
        return;
    }

    common->Printf("Render Target Pool Statistics:\n");
    common->Printf("  Total pool size: %zu\n", Framebuffer::renderTargetPool.GetPoolSize());
    common->Printf("  Active render targets: %zu\n", Framebuffer::renderTargetPool.GetActiveCount());
}

Framebuffer::Framebuffer(const char* name, int w, int h)
{
    fboName = name;
    frameBuffer = 0;
    colorFormat = 0;

    depthBuffer = 0;
    depthFormat = 0;

    stencilBuffer = 0;
    stencilFormat = 0;

    width = w;
    height = h;

    msaaSamples = false;

    glGenFramebuffers(1, &frameBuffer);

    framebuffers.Append(this);
}

Framebuffer::~Framebuffer()
{
    if (frameBuffer != 0)
    {
        glDeleteFramebuffers(1, &frameBuffer);
    }
}

void Framebuffer::Init()
{
    cmdSystem->AddCommand("listFramebuffers", R_ListFramebuffers_f, CMD_FL_RENDERER, "lists framebuffers");
    tr.backend.currentFramebuffer = NULL;
    int width, height;
    width = height = r_shadowMapImageSize.GetInteger();

    for (int i = 0; i < MAX_SHADOWMAP_RESOLUTIONS; i++)
    {
        width = height = shadowMapResolutions[i];

        globalFramebuffers.shadowFBO[i] = new Framebuffer(va("_shadowMap%i", i), width, height);

#ifndef ANDROID
        if (!glConfig.directStateAccess)
        {
            globalFramebuffers.shadowFBO[i]->Bind();
            glDrawBuffers(0, NULL);
        }
        else
        {
            glNamedFramebufferDrawBuffers(globalFramebuffers.shadowFBO[i]->frameBuffer, 0, NULL);
        }
#else
        globalFramebuffers.shadowFBO[i]->Bind();
        GLenum drawBuffers[] = { GL_NONE };
        glDrawBuffers(1, drawBuffers);
        glReadBuffer(GL_NONE);
#endif
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

#ifndef ANDROID
    if (!glConfig.directStateAccess)
    {
        globalFramebuffers.hdrFBO->Bind();
    }
#else
    globalFramebuffers.hdrFBO->Bind();
#endif

#if defined(USE_HDR_MSAA)
    if (glConfig.multisamples)
    {
        globalFramebuffers.hdrFBO->AddColorBuffer(GL_RGBA16F, 0, glConfig.multisamples);
        globalFramebuffers.hdrFBO->AddDepthBuffer(GL_DEPTH24_STENCIL8, glConfig.multisamples);

#ifndef ANDROID
        globalFramebuffers.hdrFBO->AttachImage2D(GL_TEXTURE_2D_MULTISAMPLE, globalImages->currentRenderHDRImage, 0);
        globalFramebuffers.hdrFBO->AttachImageDepth(GL_TEXTURE_2D_MULTISAMPLE, globalImages->currentDepthImage);
#else
        // Для Android MSAA не поддерживается так же, используем обычный путь
        globalFramebuffers.hdrFBO->AddColorBuffer(GL_RGBA16F, 0);
        globalFramebuffers.hdrFBO->AddDepthBuffer(GL_DEPTH24_STENCIL8);
        globalFramebuffers.hdrFBO->AttachImage2D(GL_TEXTURE_2D, globalImages->currentRenderHDRImage, 0);
        globalFramebuffers.hdrFBO->AttachImageDepth(GL_TEXTURE_2D, globalImages->currentDepthImage);
#endif
    }
    else
#endif
    {
        globalFramebuffers.hdrFBO->AddColorBuffer(GL_RGB10_A2, 0);
#ifndef ANDROID
        globalFramebuffers.hdrFBO->AddDepthBuffer(GL_DEPTH_COMPONENT24);
#endif
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

#ifndef ANDROID
    if (!glConfig.directStateAccess)
    {
        globalFramebuffers.hdr64FBO->Bind();
    }
#else
    globalFramebuffers.hdr64FBO->Bind();
#endif

    globalFramebuffers.hdr64FBO->AddColorBuffer(GL_RGB10_A2, 0);
    globalFramebuffers.hdr64FBO->AttachImage2D(GL_TEXTURE_2D, globalImages->currentRenderHDRImage64, 0);

    globalFramebuffers.hdr64FBO->Check();

    for (int i = 0; i < MAX_BLOOM_BUFFERS; i++)
    {
        int bloomWidth = screenWidth / 4;
        int bloomHeight = screenHeight / 4;

        globalFramebuffers.bloomRenderFBO[i] = new Framebuffer(va("_bloomRender%i", i), bloomWidth, bloomHeight);

#ifndef ANDROID
        if (!glConfig.directStateAccess)
        {
            globalFramebuffers.bloomRenderFBO[i]->Bind();
        }
#else
        globalFramebuffers.bloomRenderFBO[i]->Bind();
#endif

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

#ifndef ANDROID
            if (!glConfig.directStateAccess)
            {
                globalFramebuffers.ambientOcclusionFBO[i]->Bind();
            }
#else
            globalFramebuffers.ambientOcclusionFBO[i]->Bind();
#endif

            globalFramebuffers.ambientOcclusionFBO[i]->AddColorBuffer(GL_RGBA8, 0);
            globalFramebuffers.ambientOcclusionFBO[i]->AttachImage2D(GL_TEXTURE_2D, globalImages->ambientOcclusionImage[i], 0);
            globalFramebuffers.ambientOcclusionFBO[i]->Check();
        }
    }

    for (int i = 0; i < MAX_HIERARCHICAL_ZBUFFERS; i++)
    {
        globalFramebuffers.csDepthFBO[i] = new Framebuffer(va("_csz%i", i), screenWidth / (1 << i), screenHeight / (1 << i));

#ifndef ANDROID
        if (!glConfig.directStateAccess)
        {
            globalFramebuffers.csDepthFBO[i]->Bind();
        }
#else
        globalFramebuffers.csDepthFBO[i]->Bind();
#endif

        globalFramebuffers.csDepthFBO[i]->AddColorBuffer(GL_R16F, 0);
        globalFramebuffers.csDepthFBO[i]->AttachImage2D(GL_TEXTURE_2D, globalImages->hierarchicalZbufferImage, 0, i);
        globalFramebuffers.csDepthFBO[i]->Check();
    }

    globalFramebuffers.geometryBufferFBO = new Framebuffer("_gbuffer", screenWidth, screenHeight);

#ifndef ANDROID
    if (!glConfig.directStateAccess)
    {
        globalFramebuffers.geometryBufferFBO->Bind();
    }
#else
    globalFramebuffers.geometryBufferFBO->Bind();
#endif

    globalFramebuffers.geometryBufferFBO->AddColorBuffer(GL_RGB10_A2, 0);
#ifndef ANDROID
    globalFramebuffers.geometryBufferFBO->AddStencilBuffer(GL_DEPTH24_STENCIL8);
#endif
    globalFramebuffers.geometryBufferFBO->AttachImage2D(GL_TEXTURE_2D, globalImages->currentNormalsImage, 0);
    globalFramebuffers.geometryBufferFBO->AttachImageDepth(GL_TEXTURE_2D, globalImages->currentDepthImage);
    globalFramebuffers.geometryBufferFBO->Check();

    globalFramebuffers.smaaEdgesFBO = new Framebuffer("_smaaEdges", screenWidth, screenHeight);

#ifndef ANDROID
    if (!glConfig.directStateAccess)
    {
        globalFramebuffers.smaaEdgesFBO->Bind();
    }
#else
    globalFramebuffers.smaaEdgesFBO->Bind();
#endif

    globalFramebuffers.smaaEdgesFBO->AddColorBuffer(GL_RGBA8, 0);
    globalFramebuffers.smaaEdgesFBO->AttachImage2D(GL_TEXTURE_2D, globalImages->smaaEdgesImage, 0);
    globalFramebuffers.smaaEdgesFBO->Check();

    globalFramebuffers.smaaBlendFBO = new Framebuffer("_smaaBlend", screenWidth, screenHeight);

#ifndef ANDROID
    if (!glConfig.directStateAccess)
    {
        globalFramebuffers.smaaBlendFBO->Bind();
    }
#else
    globalFramebuffers.smaaBlendFBO->Bind();
#endif

    globalFramebuffers.smaaBlendFBO->AddColorBuffer(GL_RGBA8, 0);
    globalFramebuffers.smaaBlendFBO->AttachImage2D(GL_TEXTURE_2D, globalImages->smaaBlendImage, 0);
    globalFramebuffers.smaaBlendFBO->Check();

#ifndef ANDROID
    if (!glConfig.directStateAccess)
    {
        Unbind();
    }
#else
    Unbind();
#endif
}

void RenderTargetPool::Initialize(const PoolConfig& config)
{
    _config = config;
    _pool.reserve(config.preallocateCount);

    common->Printf("RenderTargetPool initialized:\n");
    common->Printf("  Max pool size: %zu\n", _config.maxPoolSize);
    common->Printf("  Pre-allocated: %zu\n", _config.preallocateCount);
    common->Printf("  LRU eviction: %s\n", _config.enableLRUEviction ? "enabled" : "disabled");
}


void Framebuffer::InitializePool()
{
    RenderTargetPool::PoolConfig config;
    config.maxPoolSize = 64;
    config.preallocateCount = 32;
    config.enableLRUEviction = true;
    config.maxFrameAge = 10;

    renderTargetPool.Initialize(config);


    int screenWidth = renderSystem->GetWidth() > 0 ? renderSystem->GetWidth() : 1280;
    int screenHeight = renderSystem->GetHeight() > 0 ? renderSystem->GetHeight() : 720;

    float postProcessScale = r_postProcessScale.GetFloat();
    int bloomWidth = static_cast<int>(screenWidth * postProcessScale);
    int bloomHeight = static_cast<int>(screenHeight * postProcessScale);
    int ssaoWidth = screenWidth / 4;
    int ssaoHeight = screenHeight / 4;


    RenderTargetDesc hdrDesc;
    hdrDesc.width = screenWidth;
    hdrDesc.height = screenHeight;
    hdrDesc.internalFormat = GL_RGBA16F;
    hdrDesc.format = GL_RGBA;
    hdrDesc.type = GL_FLOAT;
    hdrDesc.samples = 0;
    hdrDesc.isDepth = false;

    renderTargetPool.Acquire("_hdrColor_prealloc", hdrDesc);

    RenderTargetDesc hdrDepthDesc;
    hdrDepthDesc.width = screenWidth;
    hdrDepthDesc.height = screenHeight;
    hdrDepthDesc.internalFormat = GL_DEPTH24_STENCIL8;
    hdrDepthDesc.format = GL_DEPTH_STENCIL;
    hdrDepthDesc.type = GL_UNSIGNED_INT_24_8;
    hdrDepthDesc.samples = 0;
    hdrDepthDesc.isDepth = true;

    renderTargetPool.Acquire("_hdrDepth_prealloc", hdrDepthDesc);


    for (int i = 0; i < MAX_BLOOM_BUFFERS; i++)
    {
        RenderTargetDesc bloomDesc;
        bloomDesc.width = bloomWidth;
        bloomDesc.height = bloomHeight;
        bloomDesc.internalFormat = GL_RGBA8;
        bloomDesc.format = GL_RGBA;
        bloomDesc.type = GL_UNSIGNED_BYTE;
        bloomDesc.samples = 0;
        bloomDesc.isDepth = false;

        std::string bloomName = va("_bloomTex_prealloc%i", i);
        renderTargetPool.Acquire(bloomName, bloomDesc);
    }


    for (int i = 0; i < MAX_SSAO_BUFFERS; i++)
    {
        RenderTargetDesc ssaoDesc;
        ssaoDesc.width = ssaoWidth;
        ssaoDesc.height = ssaoHeight;
        ssaoDesc.internalFormat = GL_R8;
        ssaoDesc.format = GL_RED;
        ssaoDesc.type = GL_UNSIGNED_BYTE;
        ssaoDesc.samples = 0;
        ssaoDesc.isDepth = false;

        std::string ssaoName = va("_ssaoTex_prealloc%i", i);
        renderTargetPool.Acquire(ssaoName, ssaoDesc);
    }

    renderTargetPool.EndFrame();

    common->Printf("RenderTargetPool pre-allocated %zu render targets\n", renderTargetPool.GetPoolSize());
}

void Framebuffer::BeginFrame()
{
    //renderTargetPool.BeginFrame();
}

void Framebuffer::EndFrame()
{
//    renderTargetPool.EndFrame();
}

void Framebuffer::Shutdown()
{
  //  renderTargetPool.Shutdown();
    framebuffers.DeleteContents(true);
}

void Framebuffer::Bind()
{
    RENDERLOG_PRINTF("Framebuffer::Bind( %s )\n", fboName.c_str());

    if (currentBoundFramebuffer != this)
    {
        glBindFramebuffer(GL_FRAMEBUFFER, frameBuffer);
        currentBoundFramebuffer = this;
    }
}

bool Framebuffer::IsBound() const
{
    return (currentBoundFramebuffer == this);
}

void Framebuffer::Unbind()
{
    RENDERLOG_PRINTF("Framebuffer::Unbind()\n");

    if (currentBoundFramebuffer != nullptr)
    {
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glBindRenderbuffer(GL_RENDERBUFFER, 0);
        currentBoundFramebuffer = nullptr;
    }
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

    glBindTexture(GL_TEXTURE_2D, colorTextures[index]);
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

    glBindTexture(GL_TEXTURE_2D, depthTexture);

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
}

void Framebuffer::AttachImage3D(const idImage* image)
{
    glFramebufferTexture(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, image->texnum, 0);
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
}

void Framebuffer::AttachImageDepthLayer(const idImage* image, int layer)
{
    glFramebufferTextureLayer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, image->texnum, 0, layer);
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
#ifndef ANDROID
        case GL_FRAMEBUFFER_INCOMPLETE_DRAW_BUFFER:
            common->Error("Framebuffer::Check( %s ): Framebuffer incomplete, missing draw buffer", fboName.c_str());
            break;

        case GL_FRAMEBUFFER_INCOMPLETE_READ_BUFFER:
            common->Error("Framebuffer::Check( %s ): Framebuffer incomplete, missing read buffer", fboName.c_str());
            break;
#endif
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


        if (r_ssaoFiltering.GetBool() || r_ssgiFiltering.GetBool())
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

void Framebuffer::ResizeFramebuffers()
{
    CheckFramebuffers();
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
    if (frameBuffer != 0)
    {
        glDeleteFramebuffers(1, &frameBuffer);
        frameBuffer = 0;
    }
}

#endif