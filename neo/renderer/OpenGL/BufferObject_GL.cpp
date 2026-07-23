/*
===========================================================================

Doom 3 BFG Edition GPL Source Code
Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company.
Copyright (C) 2013 Robert Beckebans
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
#pragma hdrstop
#include "precompiled.h"
#include "../RenderCommon.h"

extern idCVar r_showBuffers;

idGLFrameFenceRing g_frameFences;

void idGLFrameFenceRing::Shutdown() {
    for (int i = 0; i < SLOT_COUNT; ++i) {
        if (fences[i]) {
            glDeleteSync(fences[i]);
            fences[i] = nullptr;
        }
    }
}

int idGLFrameFenceRing::CurrentSlot() const {
    return static_cast<int>(frameIndex % SLOT_COUNT);
}

void idGLFrameFenceRing::BeginFrame() {
    const int slot = CurrentSlot();
    GLsync& fence = fences[slot];

    if (!fence) return;


    GLenum status = glClientWaitSync(fence, 0, WAIT_TIMEOUT_NS);

    if (status == GL_ALREADY_SIGNALED || status == GL_CONDITION_SATISFIED) {
        glDeleteSync(fence);
        fence = nullptr;
        return;
    }

    if (status == GL_TIMEOUT_EXPIRED) {
        idLib::Warning("idGLFrameFenceRing: GPU fence timeout, waiting longer");
        status = glClientWaitSync(fence, 0, EMERGENCY_TIMEOUT_NS);
    }

    if (status == GL_ALREADY_SIGNALED || status == GL_CONDITION_SATISFIED) {
        glDeleteSync(fence);
        fence = nullptr;
        return;
    }

    idLib::Warning("idGLFrameFenceRing: fence wait failed or timed out");
    glDeleteSync(fence);
    fence = nullptr;
}

void idGLFrameFenceRing::EndFrame() {
    const int slot = CurrentSlot();
    GLsync& fence = fences[slot];

    if (fence) {
        glDeleteSync(fence);
        fence = nullptr;
    }

    fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    ++frameIndex;
}


bool idGLBufferRing::Create(GLenum target, GLsizeiptr size, bool persistent) {
    target_ = target;
    size_ = size;
    persistent_ = false;
    coherent_ = false;

    glGenBuffers(SLOT_COUNT, buffers);


    bool canPersist = persistent && R_HasBufferStorage();

    for (int i = 0; i < SLOT_COUNT; ++i) {
        if (buffers[i] == 0) {
            Destroy();
            return false;
        }

        glBindBuffer(target_, buffers[i]);

        if (canPersist) {

            GLbitfield coherentFlags = GL_MAP_WRITE_BIT |
                                       GL_MAP_PERSISTENT_BIT_EXT |
                                       GL_MAP_COHERENT_BIT_EXT;
            glBufferStorageEXT(target_, size_, nullptr, coherentFlags);
            mapped[i] = glMapBufferRange(target_, 0, size_, coherentFlags);

            if (mapped[i] != nullptr) {

                persistent_ = true;
                coherent_ = true;
                continue;
            }

            glBindBuffer(target_, buffers[i]);
            glUnmapBuffer(target_);
            glDeleteBuffers(1, &buffers[i]);
            glGenBuffers(1, &buffers[i]);
            glBindBuffer(target_, buffers[i]);

            GLbitfield nonCoherentFlags = GL_MAP_WRITE_BIT |
                                          GL_MAP_PERSISTENT_BIT_EXT |
                                          GL_MAP_FLUSH_EXPLICIT_BIT_EXT;
            glBufferStorageEXT(target_, size_, nullptr, nonCoherentFlags);
            mapped[i] = glMapBufferRange(target_, 0, size_, nonCoherentFlags);

            if (mapped[i] != nullptr) {

                persistent_ = true;
                coherent_ = false;
                continue;
            }


            glDeleteBuffers(1, &buffers[i]);
            glGenBuffers(1, &buffers[i]);
            glBindBuffer(target_, buffers[i]);
            glBufferData(target_, size_, nullptr, GL_STREAM_DRAW);
            mapped[i] = nullptr;
            canPersist = false;
            persistent_ = false;
        } else {

            glBufferData(target_, size_, nullptr, GL_STREAM_DRAW);
            mapped[i] = nullptr;
        }
    }

    glBindBuffer(target_, 0);
    return true;
}

void idGLBufferRing::Destroy() {
    if (persistent_) {
        for (int i = 0; i < SLOT_COUNT; ++i) {
            if (mapped[i]) {
                glBindBuffer(target_, buffers[i]);
                glUnmapBuffer(target_);
                mapped[i] = nullptr;
            }
        }
    }
    glDeleteBuffers(SLOT_COUNT, buffers);
    memset(buffers, 0, sizeof(buffers));
    persistent_ = false;
    coherent_ = false;
}

void* idGLBufferRing::MapForWrite(int slot) {
    if (persistent_) {
        return mapped[slot];
    }

    glBindBuffer(target_, buffers[slot]);
    return glMapBufferRange(target_, 0, size_,
                            GL_MAP_WRITE_BIT | GL_MAP_UNSYNCHRONIZED_BIT | GL_MAP_INVALIDATE_BUFFER_BIT);
}

void idGLBufferRing::Unmap(int slot) {
    if (persistent_) {
        if (!coherent_) {

            glFlushMappedBufferRange(target_, 0, size_);
        }
        return;
    }
    glBindBuffer(target_, buffers[slot]);
    glUnmapBuffer(target_);
}

GLuint idGLBufferRing::Buffer(int slot) const {
    return buffers[slot];
}

bool R_HasBufferStorage() {
    return GLAD_GL_EXT_buffer_storage !=0;
}

void UnbindBufferObjects() {
    if (!glConfig.directStateAccess) {
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    }
}

idVertexBuffer::idVertexBuffer() : idBufferObject() {
}

bool idVertexBuffer::AllocBufferObject(const void* data, int allocSize, bufferUsageType_t _usage) {
    assert(apiObject == 0);
    assert_16_byte_aligned(data);

    if (allocSize <= 0) {
        idLib::Error("idVertexBuffer::AllocBufferObject: allocSize = %i", allocSize);
    }

    size = allocSize;
    usage = _usage;
    int numBytes = GetAllocedSize();

#ifdef ANDROID
    if (usage == BU_DYNAMIC) {
        isRingBuffer_ = true;
        if (!ring_.Create(GL_ARRAY_BUFFER, numBytes, R_HasBufferStorage())) {
            idLib::FatalError("idVertexBuffer::AllocBufferObject: ring create failed");
        }
        currentSlot_ = g_frameFences.CurrentSlot();
        apiObject = ring_.Buffer(currentSlot_);
    } else {
        isRingBuffer_ = false;
        glGenBuffers(1, (GLuint*)&apiObject);
        if (apiObject == 0) {
            idLib::FatalError("idVertexBuffer::AllocBufferObject: failed");
        }
        glBindBuffer(GL_ARRAY_BUFFER, apiObject);
        glBufferData(GL_ARRAY_BUFFER, numBytes, NULL, GL_STATIC_DRAW);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
    }
#else

    if (!glConfig.directStateAccess) {
        glGenBuffers(1, (GLuint*)&apiObject);
        if (apiObject == 0) {
            idLib::FatalError("idVertexBuffer::AllocBufferObject: failed");
        }
        glBindBuffer(GL_ARRAY_BUFFER, apiObject);
        glBufferDataARB(GL_ARRAY_BUFFER, numBytes, NULL, GL_STREAM_DRAW);
    } else {
        glCreateBuffers(1, (GLuint*)&apiObject);
        if (apiObject == 0) {
            idLib::FatalError("idVertexBuffer::AllocBufferObject: failed");
        }
        glNamedBufferStorage(apiObject, numBytes, NULL, GL_MAP_WRITE_BIT | GL_MAP_READ_BIT);
    }
#endif

    GLenum err = glGetError();
    if (err == GL_OUT_OF_MEMORY) {
        idLib::Warning("idVertexBuffer::AllocBufferObject: allocation failed");
        return false;
    }

    if (r_showBuffers.GetBool()) {
        idLib::Printf("vertex buffer alloc %p, api %p (%i bytes)\n", this, (void*)(uintptr_t)apiObject, GetSize());
    }

    if (data != NULL) {
        Update(data, allocSize);
    }

    return true;
}

void idVertexBuffer::FreeBufferObject() {
    if (IsMapped()) {
        UnmapBuffer();
    }

    if (!OwnsBuffer()) {
        ClearWithoutFreeing();
        return;
    }

    if (apiObject == 0) {
        return;
    }

    if (r_showBuffers.GetBool()) {
        idLib::Printf("vertex buffer free %p, api %p (%i bytes)\n", this, (void*)(uintptr_t)apiObject, GetSize());
    }

#ifdef ANDROID
    if (isRingBuffer_) {
        ring_.Destroy();
    } else {
        glDeleteBuffers(1, (GLuint*)&apiObject);
    }
#else
    glDeleteBuffers(1, (GLuint*)&apiObject);
#endif

    ClearWithoutFreeing();
}

void idVertexBuffer::Update(const void* data, int updateSize, int offset) const {
    assert(apiObject != 0);
    assert_16_byte_aligned(data);
    assert((GetOffset() & 15) == 0);

    if (updateSize > GetSize()) {
        idLib::FatalError("idVertexBuffer::Update: size overrun, %i > %i\n", updateSize, GetSize());
    }

    int numBytes = (updateSize + 15) & ~15;

    if (usage == BU_DYNAMIC) {
        CopyBuffer((byte*)buffer + offset, (const byte*)data, numBytes);
    } else {
#ifndef ANDROID
        if (!glConfig.directStateAccess)
#endif
        {
            glBindBuffer(GL_ARRAY_BUFFER, apiObject);
            glBufferSubData(GL_ARRAY_BUFFER, GetOffset() + offset, (GLsizeiptr)numBytes, data);
        }
#ifndef ANDROID
        else {
            glNamedBufferSubData(apiObject, GetOffset() + offset, (GLsizeiptr)numBytes, data);
        }
#endif
    }
}

void* idVertexBuffer::MapBuffer(bufferMapType_t mapType) {
    assert(apiObject != 0);
    assert(IsMapped() == false);

    buffer = NULL;

#ifdef ANDROID
    if (isRingBuffer_ && mapType == BM_WRITE) {
        currentSlot_ = g_frameFences.CurrentSlot();
        apiObject = ring_.Buffer(currentSlot_);

        void* base = ring_.MapForWrite(currentSlot_);
        if (base == NULL) {
            idLib::FatalError("idVertexBuffer::MapBuffer: ring map failed");
        }

        buffer = (byte*)base + GetOffset();
        SetMapped();
        return buffer;
    }
#endif

#ifndef ANDROID
    if (!glConfig.directStateAccess)
#endif
    {
        glBindBuffer(GL_ARRAY_BUFFER, apiObject);

        if (mapType == BM_READ) {
#ifndef ANDROID
            buffer = glMapBufferRange(GL_ARRAY_BUFFER_ARB, 0, GetAllocedSize(),
                GL_MAP_READ_BIT | GL_MAP_UNSYNCHRONIZED_BIT);
#else
            buffer = glMapBufferRange(GL_ARRAY_BUFFER, 0, GetAllocedSize(),
                                      GL_MAP_READ_BIT | GL_MAP_UNSYNCHRONIZED_BIT);
#endif
        } else if (mapType == BM_WRITE) {
#ifdef ANDROID
            buffer = glMapBufferRange(GL_ARRAY_BUFFER, 0, GetAllocedSize(),
                                      GL_MAP_WRITE_BIT | GL_MAP_UNSYNCHRONIZED_BIT | GL_MAP_INVALIDATE_BUFFER_BIT);
#else
            buffer = glMapBufferRange(GL_ARRAY_BUFFER, 0, GetAllocedSize(),
                GL_MAP_WRITE_BIT | GL_MAP_UNSYNCHRONIZED_BIT);
#endif
        } else {
            assert(false);
        }

        if (buffer != NULL) {
            buffer = (byte*)buffer + GetOffset();
        }
    }
#ifndef ANDROID
    else {
        switch (mapType) {
        case BM_READ:
            buffer = glMapNamedBufferRange(apiObject, 0, GetAllocedSize(),
                GL_MAP_READ_BIT | GL_MAP_UNSYNCHRONIZED_BIT);
            break;
        case BM_WRITE:
            buffer = glMapNamedBufferRange(apiObject, 0, GetAllocedSize(),
                GL_MAP_WRITE_BIT | GL_MAP_UNSYNCHRONIZED_BIT);
            break;
        default:
            assert(false);
        }
        if (buffer != NULL) {
            buffer = (byte*)buffer + GetOffset();
        }
    }
#endif

    SetMapped();

    if (buffer == NULL) {
        idLib::FatalError("idVertexBuffer::MapBuffer: failed");
    }

    return buffer;
}

void idVertexBuffer::UnmapBuffer() {
    assert(apiObject != 0);
    assert(IsMapped());

#ifdef ANDROID
    if (isRingBuffer_) {
        ring_.Unmap(currentSlot_);
        buffer = NULL;
        SetUnmapped();
        return;
    }
#endif

#ifndef ANDROID
    if (!glConfig.directStateAccess)
#endif
    {
        glBindBuffer(GL_ARRAY_BUFFER, apiObject);
        if (!glUnmapBuffer(GL_ARRAY_BUFFER)) {
            idLib::Printf("idVertexBuffer::UnmapBuffer failed\n");
        }
    }
#ifndef ANDROID
    else {
        if (!glUnmapNamedBuffer(apiObject)) {
            idLib::Printf("idVertexBuffer::UnmapBuffer failed\n");
        }
    }
#endif

    buffer = NULL;
    SetUnmapped();
}

void idVertexBuffer::ClearWithoutFreeing() {
    size = 0;
    offsetInOtherBuffer = OWNS_BUFFER_FLAG;
    apiObject = 0;
    buffer = NULL;
#ifdef ANDROID
    isRingBuffer_ = false;
    currentSlot_ = 0;
#endif
}

idIndexBuffer::idIndexBuffer() : idBufferObject() {}

bool idIndexBuffer::AllocBufferObject(const void* data, int allocSize, bufferUsageType_t _usage) {
    assert(apiObject == 0);
    assert_16_byte_aligned(data);

    if (allocSize <= 0) {
        idLib::Error("idIndexBuffer::AllocBufferObject: allocSize = %i", allocSize);
    }

    size = allocSize;
    usage = _usage;
    int numBytes = GetAllocedSize();

#ifdef ANDROID
    if (usage == BU_DYNAMIC) {
        isRingBuffer_ = true;
        if (!ring_.Create(GL_ELEMENT_ARRAY_BUFFER, numBytes, R_HasBufferStorage())) {
            idLib::FatalError("idIndexBuffer::AllocBufferObject: ring create failed");
        }
        currentSlot_ = g_frameFences.CurrentSlot();
        apiObject = ring_.Buffer(currentSlot_);
    } else {
        isRingBuffer_ = false;
        glGenBuffers(1, (GLuint*)&apiObject);
        if (apiObject == 0) {
            idLib::FatalError("idIndexBuffer::AllocBufferObject: failed");
        }
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, apiObject);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, numBytes, NULL, GL_STATIC_DRAW);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    }
#else

    if (!glConfig.directStateAccess) {
        glGenBuffersARB(1, (GLuint*)&apiObject);
        if (apiObject == 0) {
            idLib::FatalError("idIndexBuffer::AllocBufferObject: failed");
        }
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, apiObject);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, numBytes, NULL, GL_STREAM_DRAW);
    } else {
        glCreateBuffers(1, (GLuint*)&apiObject);
        if (apiObject == 0) {
            idLib::FatalError("idIndexBuffer::AllocBufferObject: failed");
        }
        glNamedBufferStorage(apiObject, numBytes, NULL, GL_MAP_WRITE_BIT | GL_MAP_READ_BIT);
    }
#endif

    GLenum err = glGetError();
    if (err == GL_OUT_OF_MEMORY) {
        idLib::Warning("idIndexBuffer::AllocBufferObject: allocation failed");
        return false;
    }

    if (r_showBuffers.GetBool()) {
        idLib::Printf("index buffer alloc %p, api %p (%i bytes)\n", this, (void*)(uintptr_t)apiObject, GetSize());
    }

    if (data != NULL) {
        Update(data, allocSize);
    }

    return true;
}

void idIndexBuffer::FreeBufferObject() {
    if (IsMapped()) {
        UnmapBuffer();
    }

    if (!OwnsBuffer()) {
        ClearWithoutFreeing();
        return;
    }

    if (apiObject == 0) {
        return;
    }

    if (r_showBuffers.GetBool()) {
        idLib::Printf("index buffer free %p, api %p (%i bytes)\n", this, (void*)(uintptr_t)apiObject, GetSize());
    }

#ifdef ANDROID
    if (isRingBuffer_) {
        ring_.Destroy();
    } else {
        glDeleteBuffers(1, (GLuint*)&apiObject);
    }
#else
    glDeleteBuffers(1, (GLuint*)&apiObject);
#endif

    ClearWithoutFreeing();
}

void idIndexBuffer::Update(const void* data, int updateSize, int offset) const {
    assert(apiObject != 0);
    assert_16_byte_aligned(data);
    assert((GetOffset() & 15) == 0);

    if (updateSize > GetSize()) {
        idLib::FatalError("idIndexBuffer::Update: size overrun, %i > %i\n", updateSize, GetSize());
    }

    int numBytes = (updateSize + 15) & ~15;

    if (usage == BU_DYNAMIC) {
        CopyBuffer((byte*)buffer + offset, (const byte*)data, numBytes);
    } else {
#ifndef ANDROID
        if (!glConfig.directStateAccess)
#endif
        {
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, apiObject);
            glBufferSubData(GL_ELEMENT_ARRAY_BUFFER, GetOffset() + offset, (GLsizeiptr)numBytes, data);
        }
#ifndef ANDROID
        else {
            glNamedBufferSubData(apiObject, GetOffset() + offset, (GLsizeiptr)numBytes, data);
        }
#endif
    }
}

void* idIndexBuffer::MapBuffer(bufferMapType_t mapType) {
    assert(apiObject != 0);
    assert(IsMapped() == false);

    buffer = NULL;

#ifdef ANDROID
    if (isRingBuffer_ && mapType == BM_WRITE) {
        currentSlot_ = g_frameFences.CurrentSlot();
        apiObject = ring_.Buffer(currentSlot_);

        void* base = ring_.MapForWrite(currentSlot_);
        if (base == NULL) {
            idLib::FatalError("idIndexBuffer::MapBuffer: ring map failed");
        }

        buffer = (byte*)base + GetOffset();
        SetMapped();
        return buffer;
    }
#endif

#ifndef ANDROID
    if (!glConfig.directStateAccess)
#endif
    {
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, apiObject);

        if (mapType == BM_READ) {
            buffer = glMapBufferRange(GL_ELEMENT_ARRAY_BUFFER, 0, GetAllocedSize(),
                                      GL_MAP_READ_BIT | GL_MAP_UNSYNCHRONIZED_BIT);
        } else if (mapType == BM_WRITE) {
#ifdef ANDROID
            buffer = glMapBufferRange(GL_ELEMENT_ARRAY_BUFFER, 0, GetAllocedSize(),
                                      GL_MAP_WRITE_BIT | GL_MAP_UNSYNCHRONIZED_BIT | GL_MAP_INVALIDATE_BUFFER_BIT);
#else
            buffer = glMapBufferRange(GL_ELEMENT_ARRAY_BUFFER, 0, GetAllocedSize(),
                GL_MAP_WRITE_BIT | GL_MAP_UNSYNCHRONIZED_BIT);
#endif
        } else {
            assert(false);
        }

        if (buffer != NULL) {
            buffer = (byte*)buffer + GetOffset();
        }
    }
#ifndef ANDROID
    else {
        switch (mapType) {
        case BM_READ:
            buffer = glMapNamedBufferRange(apiObject, 0, GetAllocedSize(),
                GL_MAP_READ_BIT | GL_MAP_UNSYNCHRONIZED_BIT);
            break;
        case BM_WRITE:
            buffer = glMapNamedBufferRange(apiObject, 0, GetAllocedSize(),
                GL_MAP_WRITE_BIT | GL_MAP_UNSYNCHRONIZED_BIT);
            break;
        default:
            assert(false);
        }
        if (buffer != NULL) {
            buffer = (byte*)buffer + GetOffset();
        }
    }
#endif

    SetMapped();

    if (buffer == NULL) {
        idLib::FatalError("idIndexBuffer::MapBuffer: failed");
    }

    return buffer;
}

void idIndexBuffer::UnmapBuffer() {
    assert(apiObject != 0);
    assert(IsMapped());

#ifdef ANDROID
    if (isRingBuffer_) {
        ring_.Unmap(currentSlot_);
        buffer = NULL;
        SetUnmapped();
        return;
    }
#endif

#ifndef ANDROID
    if (!glConfig.directStateAccess)
#endif
    {
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, apiObject);
        if (!glUnmapBuffer(GL_ELEMENT_ARRAY_BUFFER)) {
            idLib::Printf("idIndexBuffer::UnmapBuffer failed\n");
        }
    }
#ifndef ANDROID
    else {
        if (!glUnmapNamedBuffer(apiObject)) {
            idLib::Printf("idIndexBuffer::UnmapBuffer failed\n");
        }
    }
#endif

    buffer = NULL;
    SetUnmapped();
}

void idIndexBuffer::ClearWithoutFreeing() {
    size = 0;
    offsetInOtherBuffer = OWNS_BUFFER_FLAG;
    apiObject = 0;
    buffer = NULL;
#ifdef ANDROID
    isRingBuffer_ = false;
    currentSlot_ = 0;
#endif
}

idUniformBuffer::idUniformBuffer() : idBufferObject() {}

bool idUniformBuffer::AllocBufferObject(const void* data, int allocSize, bufferUsageType_t _usage) {
    assert(apiObject == 0);
    assert_16_byte_aligned(data);

    if (allocSize <= 0) {
        idLib::Error("idUniformBuffer::AllocBufferObject: allocSize = %i", allocSize);
    }

    size = allocSize;
    usage = _usage;
    int numBytes = GetAllocedSize();

#ifdef ANDROID
    if (usage == BU_DYNAMIC) {
        isRingBuffer_ = true;
        if (!ring_.Create(GL_UNIFORM_BUFFER, numBytes, R_HasBufferStorage())) {
            idLib::FatalError("idUniformBuffer::AllocBufferObject: ring create failed");
        }
        currentSlot_ = g_frameFences.CurrentSlot();
        apiObject = ring_.Buffer(currentSlot_);
    } else {
        isRingBuffer_ = false;
        glGenBuffers(1, (GLuint*)&apiObject);
        if (apiObject == 0) {
            idLib::FatalError("idUniformBuffer::AllocBufferObject: failed");
        }
        glBindBuffer(GL_UNIFORM_BUFFER, apiObject);
        glBufferData(GL_UNIFORM_BUFFER, numBytes, NULL, GL_STATIC_DRAW);
        glBindBuffer(GL_UNIFORM_BUFFER, 0);
    }
#else

    if (!glConfig.directStateAccess) {
        glGenBuffers(1, (GLuint*)&apiObject);
        if (apiObject == 0) {
            idLib::FatalError("idUniformBuffer::AllocBufferObject: failed");
        }
        glBindBuffer(GL_UNIFORM_BUFFER, apiObject);
        glBufferData(GL_UNIFORM_BUFFER, numBytes, NULL, GL_STREAM_DRAW);
        glBindBuffer(GL_UNIFORM_BUFFER, 0);
    } else {
        glCreateBuffers(1, (GLuint*)&apiObject);
        if (apiObject == 0) {
            idLib::FatalError("idUniformBuffer::AllocBufferObject: failed");
        }
        glNamedBufferStorage(apiObject, numBytes, NULL, GL_MAP_WRITE_BIT | GL_MAP_READ_BIT);
    }
#endif

    if (r_showBuffers.GetBool()) {
        idLib::Printf("uniform buffer alloc %p, api %p (%i bytes)\n", this, (void*)(uintptr_t)apiObject, GetSize());
    }

    if (data != NULL) {
        Update(data, allocSize);
    }

    return true;
}

void idUniformBuffer::FreeBufferObject() {
    if (IsMapped()) {
        UnmapBuffer();
    }

    if (!OwnsBuffer()) {
        ClearWithoutFreeing();
        return;
    }

    if (apiObject == 0) {
        return;
    }

    if (r_showBuffers.GetBool()) {
        idLib::Printf("uniform buffer free %p, api %p (%i bytes)\n", this, (void*)(uintptr_t)apiObject, GetSize());
    }

    if (!glConfig.directStateAccess) {
        glBindBuffer(GL_UNIFORM_BUFFER, 0);
    }

#ifdef ANDROID
    if (isRingBuffer_) {
        ring_.Destroy();
    } else {
        glDeleteBuffers(1, (GLuint*)&apiObject);
    }
#else
    glDeleteBuffers(1, (GLuint*)&apiObject);
#endif

    ClearWithoutFreeing();
}

void idUniformBuffer::Update(const void* data, int updateSize, int offset) const {
    assert(apiObject != 0);
    assert_16_byte_aligned(data);
    assert((GetOffset() & 15) == 0);

    if (updateSize > GetSize()) {
        idLib::FatalError("idUniformBuffer::Update: size overrun, %i > %i\n", updateSize, GetSize());
    }

    int numBytes = (updateSize + 15) & ~15;

    if (usage == BU_DYNAMIC) {
        CopyBuffer((byte*)buffer + offset, (const byte*)data, numBytes);
    } else {
#ifndef ANDROID
        if (!glConfig.directStateAccess)
#endif
        {
            glBindBuffer(GL_UNIFORM_BUFFER, apiObject);
            glBufferSubData(GL_UNIFORM_BUFFER, GetOffset() + offset, (GLsizeiptr)numBytes, data);
            glBindBuffer(GL_UNIFORM_BUFFER, 0);
        }
#ifndef ANDROID
        else {
            glNamedBufferSubData(apiObject, GetOffset() + offset, (GLsizeiptr)numBytes, data);
        }
#endif
    }
}

void* idUniformBuffer::MapBuffer(bufferMapType_t mapType) {
    assert(IsMapped() == false);
    assert(mapType == BM_WRITE);
    assert(apiObject != 0);

    buffer = NULL;

#ifdef ANDROID
    if (isRingBuffer_) {
        currentSlot_ = g_frameFences.CurrentSlot();
        apiObject = ring_.Buffer(currentSlot_);

        void* base = ring_.MapForWrite(currentSlot_);
        if (base == NULL) {
            idLib::FatalError("idUniformBuffer::MapBuffer: ring map failed");
        }

        buffer = (byte*)base + GetOffset();
        SetMapped();
        return buffer;
    }
#endif

#ifndef ANDROID
    if (!glConfig.directStateAccess)
#endif
    {
        glBindBuffer(GL_UNIFORM_BUFFER, apiObject);
        assert(GetOffset() == 0);

#ifdef ANDROID
        buffer = glMapBufferRange(GL_UNIFORM_BUFFER, 0, GetAllocedSize(),
                                  GL_MAP_WRITE_BIT | GL_MAP_UNSYNCHRONIZED_BIT | GL_MAP_INVALIDATE_BUFFER_BIT);
#else
        buffer = glMapBufferRange(GL_UNIFORM_BUFFER, 0, GetAllocedSize(),
            GL_MAP_WRITE_BIT | GL_MAP_UNSYNCHRONIZED_BIT);
#endif
    }
#ifndef ANDROID
    else {
        assert(GetOffset() == 0);
        buffer = glMapNamedBufferRange(apiObject, 0, GetAllocedSize(),
            GL_MAP_WRITE_BIT | GL_MAP_UNSYNCHRONIZED_BIT);
    }
#endif

    if (buffer != NULL) {
        buffer = (byte*)buffer + GetOffset();
    }

    SetMapped();

    if (buffer == NULL) {
        idLib::FatalError("idUniformBuffer::MapBuffer: failed");
    }

    return (float*)buffer;
}

void idUniformBuffer::UnmapBuffer() {
    assert(apiObject != 0);
    assert(IsMapped());

#ifdef ANDROID
    if (isRingBuffer_) {
        ring_.Unmap(currentSlot_);
        buffer = NULL;
        SetUnmapped();
        return;
    }
#endif

#ifndef ANDROID
    if (!glConfig.directStateAccess)
#endif
    {
        glBindBuffer(GL_UNIFORM_BUFFER, apiObject);
        if (!glUnmapBuffer(GL_UNIFORM_BUFFER)) {
            idLib::Printf("idUniformBuffer::UnmapBuffer failed\n");
        }
    }
#ifndef ANDROID
    else {
        if (!glUnmapNamedBuffer(apiObject)) {
            idLib::Printf("idUniformBuffer::UnmapBuffer failed\n");
        }
    }
#endif

    buffer = NULL;
    SetUnmapped();
}

void idUniformBuffer::ClearWithoutFreeing() {
    size = 0;
    offsetInOtherBuffer = OWNS_BUFFER_FLAG;
    apiObject = 0;
    buffer = NULL;
#ifdef ANDROID
    isRingBuffer_ = false;
    currentSlot_ = 0;
#endif
}