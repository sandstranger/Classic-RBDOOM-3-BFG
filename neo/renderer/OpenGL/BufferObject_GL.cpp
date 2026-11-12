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
    fenceFrames[slot] = frameIndex;
    ++frameIndex;
}

void idGLFrameFenceRing::WaitForFrame(uint64_t frame) {
    // a frame that hasn't run yet has nothing on the GPU
    if (frame >= frameIndex) {
        return;
    }
    // BeginFrame waited for the fence and deleted it before EndFrame reused the slot: that frame is done
    const int slot = static_cast<int>(frame % SLOT_COUNT);
    GLsync& fence = fences[slot];
    if (fence == nullptr || fenceFrames[slot] != frame) {
        return;
    }

    GLenum status = glClientWaitSync(fence, GL_SYNC_FLUSH_COMMANDS_BIT, WAIT_TIMEOUT_NS);
    if (status == GL_TIMEOUT_EXPIRED) {
        status = glClientWaitSync(fence, GL_SYNC_FLUSH_COMMANDS_BIT, EMERGENCY_TIMEOUT_NS);
    }
    if (status == GL_ALREADY_SIGNALED || status == GL_CONDITION_SATISFIED) {
        glDeleteSync(fence);
        fence = nullptr;
    } else {
        idLib::Warning("idGLFrameFenceRing: wait for frame %llu failed or timed out", (unsigned long long)frame);
    }
}


bool idGLBufferRing::Create(GLenum target, GLsizeiptr size, bool persistent) {
    target_ = target;
    size_ = size;
    persistent_ = false;
    coherent_ = false;
    streaming_ = false;
    numSlots_ = SLOT_COUNT;
    lastSlot_ = -1;

    glGenBuffers(SLOT_COUNT, buffers);


    bool canPersist = persistent && R_HasBufferStorage();

    for (int i = 0; i < SLOT_COUNT; ++i) {
        if (buffers[i] == 0) {
            Destroy();
            return false;
        }

        glBindBuffer(target_, buffers[i]);

        if (canPersist) {
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
    memset(mapped, 0, sizeof(mapped));
    persistent_ = false;
    coherent_ = false;

    if (staging_ != nullptr) {
        Mem_Free16(staging_);
        staging_ = nullptr;
    }
    streaming_ = false;
    numSlots_ = SLOT_COUNT;
    lastSlot_ = -1;
    memset(slotSubmitted_, 0, sizeof(slotSubmitted_));
    memset(slotDrawFrame_, 0, sizeof(slotDrawFrame_));
}

/*
========================
idGLBufferRing::CreateStreaming

The four slots of Create for every dynamic buffer of the vertex cache, which double buffers them
already, reserved eight full size copies of each: 31 MB vertexes and 31 MB indexes made almost
500 MB of GL storage, enough to run a 32 bit process out of memory on its own.
========================
*/
bool idGLBufferRing::CreateStreaming(GLenum target, GLsizeiptr size, bool allowPersistent) {
    Destroy();

    target_ = target;
    size_ = size;
    streaming_ = true;

    // the buffers are created and filled through the copy target, binding an element buffer would change the VAO
    if (allowPersistent && R_HasBufferStorage()) {
        numSlots_ = 2;
        glGenBuffers(numSlots_, buffers);

        const GLbitfield flags = GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT_EXT | GL_MAP_FLUSH_EXPLICIT_BIT_EXT;
        bool ok = true;
        for (int i = 0; i < numSlots_ && ok; ++i) {
            if (buffers[i] == 0) {
                ok = false;
                break;
            }
            glBindBuffer(GL_COPY_WRITE_BUFFER, buffers[i]);
            glBufferStorageEXT(GL_COPY_WRITE_BUFFER, size_, nullptr, flags);
            mapped[i] = glMapBufferRange(GL_COPY_WRITE_BUFFER, 0, size_, flags);
            ok = (mapped[i] != nullptr);
        }
        glBindBuffer(GL_COPY_WRITE_BUFFER, 0);

        if (ok) {
            persistent_ = true;
            return true;
        }

        // no persistent mapping after all: the CPU copy below
        for (int i = 0; i < numSlots_; ++i) {
            if (mapped[i] != nullptr) {
                glBindBuffer(GL_COPY_WRITE_BUFFER, buffers[i]);
                glUnmapBuffer(GL_COPY_WRITE_BUFFER);
                mapped[i] = nullptr;
            }
        }
        glBindBuffer(GL_COPY_WRITE_BUFFER, 0);
        glDeleteBuffers(SLOT_COUNT, buffers);
        memset(buffers, 0, sizeof(buffers));
    }

    numSlots_ = 1;
    glGenBuffers(1, buffers);
    if (buffers[0] == 0) {
        streaming_ = false;
        return false;
    }
    glBindBuffer(GL_COPY_WRITE_BUFFER, buffers[0]);
    glBufferData(GL_COPY_WRITE_BUFFER, 16, nullptr, GL_STREAM_DRAW);
    glBindBuffer(GL_COPY_WRITE_BUFFER, 0);

    // only the pages the frames write become resident
    staging_ = (byte*)Mem_Alloc16(size_, TAG_RENDER);
    return true;
}

int idGLBufferRing::AcquireSlot() {
    if (!streaming_) {
        return g_frameFences.CurrentSlot();
    }
    if (numSlots_ <= 1) {
        return 0;
    }

    const int slot = (lastSlot_ + 1) % numSlots_;
    if (slotSubmitted_[slot]) {
        // the GPU may still draw what this slot got two maps ago
        g_frameFences.WaitForFrame(slotDrawFrame_[slot]);
        slotSubmitted_[slot] = false;
    }
    lastSlot_ = slot;
    return slot;
}

void idGLBufferRing::SubmitSlot(int slot, GLsizeiptr writtenBytes) {
    GLsizeiptr written = writtenBytes;
    if (written < 0) {
        written = 0;
    }
    if (written > size_) {
        written = size_;
    }

    if (persistent_) {
        FlushRange(slot, 0, written);
        if (streaming_) {
            slotDrawFrame_[slot] = g_frameFences.FrameIndex();
            slotSubmitted_[slot] = true;
        }
        return;
    }

    if (staging_ != nullptr) {
        // fresh storage of the written size: the driver keeps the old one while earlier frames draw it
        const GLsizeiptr uploadBytes = (written + 15) & ~(GLsizeiptr)15;
        glBindBuffer(GL_COPY_WRITE_BUFFER, buffers[0]);
        glBufferData(GL_COPY_WRITE_BUFFER, uploadBytes > 0 ? uploadBytes : 16, uploadBytes > 0 ? staging_ : nullptr, GL_STREAM_DRAW);
        glBindBuffer(GL_COPY_WRITE_BUFFER, 0);
        return;
    }

    glBindBuffer(target_, buffers[slot]);
    glUnmapBuffer(target_);
}

void* idGLBufferRing::MapForWrite(int slot) {
    if (staging_ != nullptr) {
        return staging_;
    }
    if (persistent_) {
        return mapped[slot];
    }

    glBindBuffer(target_, buffers[slot]);
    glBufferData(target_, size_, nullptr, GL_STREAM_DRAW);
    return glMapBufferRange(target_, 0, size_,GL_MAP_WRITE_BIT | GL_MAP_UNSYNCHRONIZED_BIT);
}

void idGLBufferRing::Unmap(int slot) {
    if (streaming_) {
        SubmitSlot(slot, size_);
        return;
    }
    if (persistent_) {
        // the slot's buffer must be bound for the flush, the previous code flushed whatever was bound
        FlushRange(slot, 0, size_);
        return;
    }
    glBindBuffer(target_, buffers[slot]);
    glUnmapBuffer(target_);
}

void idGLBufferRing::Unmap(int slot, GLsizeiptr writtenBytes) {
    if (streaming_) {
        SubmitSlot(slot, writtenBytes);
        return;
    }
    if (persistent_) {
        FlushRange(slot, 0, writtenBytes);
        return;
    }
    glBindBuffer(target_, buffers[slot]);
    glUnmapBuffer(target_);
}

void idGLBufferRing::FlushRange(int slot, GLsizeiptr offset, GLsizeiptr length) {
    if (!persistent_ || coherent_ || length <= 0) {
        return;
    }
    // the streaming rings are vertex, index and uniform buffers: the copy target leaves the VAO alone
    const GLenum flushTarget = streaming_ ? GL_COPY_WRITE_BUFFER : target_;
    glBindBuffer(flushTarget, buffers[slot]);
    glFlushMappedBufferRange(flushTarget, offset, length);
    if (streaming_) {
        glBindBuffer(flushTarget, 0);
    }
}

/*
========================
R_DynamicBuffersPersistent

The dynamic vertex cache buffers stay mapped with EXT_buffer_storage, but not in a 32 bit process:
two full size slots of every buffer cost GL memory and address space it doesn't have.
========================
*/
static bool R_DynamicBuffersPersistent() {
#if defined(ID_LOW_MEMORY)
    return false;
#else
    return true;
#endif
}

/*
========================
R_GrowStaticBuffer

Larger storage for a static vertex or index buffer, the first keepBytes copied over: GLES 3.0 has
glCopyBufferSubData. The copy targets leave the VAO's bindings alone.
========================
*/
static bool R_GrowStaticBuffer(GLuint& apiObject, int newBytes, int keepBytes) {
    GLuint newBuffer = 0;
    glGenBuffers(1, &newBuffer);
    if (newBuffer == 0) {
        return false;
    }
    glBindBuffer(GL_COPY_WRITE_BUFFER, newBuffer);
    glBufferData(GL_COPY_WRITE_BUFFER, newBytes, NULL, GL_STATIC_DRAW);
    if (keepBytes > 0 && apiObject != 0) {
        glBindBuffer(GL_COPY_READ_BUFFER, apiObject);
        glCopyBufferSubData(GL_COPY_READ_BUFFER, GL_COPY_WRITE_BUFFER, 0, 0, keepBytes);
        glBindBuffer(GL_COPY_READ_BUFFER, 0);
    }
    glBindBuffer(GL_COPY_WRITE_BUFFER, 0);
    if (apiObject != 0) {
        // the GL keeps the storage until the frames that draw from it are done
        glDeleteBuffers(1, &apiObject);
    }
    apiObject = newBuffer;
    return true;
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
        if (!ring_.CreateStreaming(GL_ARRAY_BUFFER, numBytes, R_DynamicBuffersPersistent())) {
            idLib::FatalError("idVertexBuffer::AllocBufferObject: ring create failed");
        }
        currentSlot_ = 0;
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
#ifdef ANDROID
        if (isRingBuffer_) {
            // the ring is destroyed below, uploading what was written would be wasted
            buffer = NULL;
            SetUnmapped();
        } else
#endif
        {
            UnmapBuffer();
        }
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
        currentSlot_ = ring_.AcquireSlot();
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
            glBufferData(GL_ARRAY_BUFFER, GetAllocedSize(), nullptr, GL_STREAM_DRAW);
            buffer = glMapBufferRange(GL_ARRAY_BUFFER, 0, GetAllocedSize(),GL_MAP_WRITE_BIT | GL_MAP_UNSYNCHRONIZED_BIT );
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

#ifdef ANDROID
void idVertexBuffer::UnmapBuffer(int writtenBytes) {
    if (isRingBuffer_ && IsMapped()) {
        ring_.Unmap(currentSlot_, writtenBytes);
        buffer = NULL;
        SetUnmapped();
        return;
    }
    UnmapBuffer();
}

bool idVertexBuffer::GrowStatic(int newAllocSize, int keepBytes) {
    assert(usage == BU_STATIC && !isRingBuffer_ && OwnsBuffer() && !IsMapped());
    const int newBytes = (newAllocSize + 15) & ~15;
    if (!R_GrowStaticBuffer(apiObject, newBytes, keepBytes)) {
        return false;
    }
    size = newAllocSize;
    if (r_showBuffers.GetBool()) {
        idLib::Printf("vertex buffer grow %p, api %p (%i bytes)\n", this, (void*)(uintptr_t)apiObject, GetSize());
    }
    return true;
}
#endif

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
        if (!ring_.CreateStreaming(GL_ELEMENT_ARRAY_BUFFER, numBytes, R_DynamicBuffersPersistent())) {
            idLib::FatalError("idIndexBuffer::AllocBufferObject: ring create failed");
        }
        currentSlot_ = 0;
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
#ifdef ANDROID
        if (isRingBuffer_) {
            // the ring is destroyed below, uploading what was written would be wasted
            buffer = NULL;
            SetUnmapped();
        } else
#endif
        {
            UnmapBuffer();
        }
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
        currentSlot_ = ring_.AcquireSlot();
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
            glBufferData(GL_ELEMENT_ARRAY_BUFFER, GetAllocedSize(), nullptr, GL_STREAM_DRAW);
            buffer = glMapBufferRange(GL_ELEMENT_ARRAY_BUFFER, 0, GetAllocedSize(), GL_MAP_WRITE_BIT | GL_MAP_UNSYNCHRONIZED_BIT);
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

#ifdef ANDROID
void idIndexBuffer::UnmapBuffer(int writtenBytes) {
    if (isRingBuffer_ && IsMapped()) {
        ring_.Unmap(currentSlot_, writtenBytes);
        buffer = NULL;
        SetUnmapped();
        return;
    }
    UnmapBuffer();
}

bool idIndexBuffer::GrowStatic(int newAllocSize, int keepBytes) {
    assert(usage == BU_STATIC && !isRingBuffer_ && OwnsBuffer() && !IsMapped());
    const int newBytes = (newAllocSize + 15) & ~15;
    if (!R_GrowStaticBuffer(apiObject, newBytes, keepBytes)) {
        return false;
    }
    size = newAllocSize;
    if (r_showBuffers.GetBool()) {
        idLib::Printf("index buffer grow %p, api %p (%i bytes)\n", this, (void*)(uintptr_t)apiObject, GetSize());
    }
    return true;
}
#endif

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
        if (!ring_.CreateStreaming(GL_UNIFORM_BUFFER, numBytes, R_DynamicBuffersPersistent())) {
            idLib::FatalError("idUniformBuffer::AllocBufferObject: ring create failed");
        }
        currentSlot_ = 0;
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
#ifdef ANDROID
        if (isRingBuffer_) {
            // the ring is destroyed below, uploading what was written would be wasted
            buffer = NULL;
            SetUnmapped();
        } else
#endif
        {
            UnmapBuffer();
        }
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
        currentSlot_ = ring_.AcquireSlot();
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
        glBufferData(GL_UNIFORM_BUFFER, GetAllocedSize(), nullptr, GL_STREAM_DRAW);
        buffer = glMapBufferRange(GL_UNIFORM_BUFFER, 0, GetAllocedSize(),GL_MAP_WRITE_BIT | GL_MAP_UNSYNCHRONIZED_BIT);
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

#ifdef ANDROID
void idUniformBuffer::UnmapBuffer(int writtenBytes) {
    if (isRingBuffer_ && IsMapped()) {
        ring_.Unmap(currentSlot_, writtenBytes);
        buffer = NULL;
        SetUnmapped();
        return;
    }
    UnmapBuffer();
}
#endif

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