/*
===========================================================================
Doom 3 BFG Edition GPL Source Code
Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company.
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
#ifndef __BUFFEROBJECT_H__
#define __BUFFEROBJECT_H__

#if defined( USE_VULKAN )
#include "Vulkan/Allocator_VK.h"
#endif
#include <cstdint>
#define RING_BUFFER_SIZE 4

enum bufferMapType_t
{
    BM_READ,
    BM_WRITE
};

enum bufferUsageType_t
{
    BU_STATIC,
    BU_DYNAMIC
};

class idGLBufferRing;
class idGLFrameFenceRing;
extern idGLFrameFenceRing g_frameFences;

class idGLFrameFenceRing {
public:
    static constexpr int SLOT_COUNT = RING_BUFFER_SIZE;
    static constexpr GLuint64 WAIT_TIMEOUT_NS = 16'666'666;
    static constexpr GLuint64 EMERGENCY_TIMEOUT_NS = 50'000'000;

    idGLFrameFenceRing() = default;
    ~idGLFrameFenceRing() { Shutdown(); }

    void Shutdown();
    int  CurrentSlot() const;
    void BeginFrame();
    void EndFrame();

    // the backend frame that runs next: EndFrame fences its commands and counts on
    uint64_t FrameIndex() const { return frameIndex; }
    // until the GPU is done with the commands of a backend frame, returns at once for one it is done with
    void WaitForFrame(uint64_t frame);

private:
    GLsync fences[SLOT_COUNT]{};
    uint64_t fenceFrames[SLOT_COUNT]{};		// the frame of each fence
    uint64_t frameIndex = 0;
};

class idGLBufferRing {
public:
    static constexpr int SLOT_COUNT = RING_BUFFER_SIZE;

    idGLBufferRing() = default;
    ~idGLBufferRing() { Destroy(); }

    // a slot per frame of g_frameFences, mapped every frame or persistently
    bool Create(GLenum target, GLsizeiptr size, bool persistent);

    // the dynamic buffers of the vertex cache, written by the CPU over a frame and drawn by the next
    // backend frame. Persistently mapped (allowPersistent and EXT_buffer_storage): two slots, a slot
    // waits for the frame that drew it last before it is written again. Otherwise CPU memory, of which
    // SubmitSlot gives only the written bytes to fresh GL storage: the GPU memory is what the frames use,
    // not the whole size, and the driver keeps the storage of the frames still drawing.
    bool CreateStreaming(GLenum target, GLsizeiptr size, bool allowPersistent);
    bool IsStreaming() const { return streaming_; }
    // the slot to write next, see CreateStreaming; the slot of the frame for the rings of Create
    int  AcquireSlot();
    // writtenBytes from the start of the slot are written, the next backend frame draws them
    void SubmitSlot(int slot, GLsizeiptr writtenBytes);

    void Destroy();

    GLuint Buffer(int slot) const;
    void*  MapForWrite(int slot);
    void   Unmap(int slot);

    // only writtenBytes from the start of the slot were written: flush just those
    void   Unmap(int slot, GLsizeiptr writtenBytes);
    // persistent rings stay mapped, a range written between draws is flushed with this
    void   FlushRange(int slot, GLsizeiptr offset, GLsizeiptr length);
    bool   IsPersistent() const { return persistent_; }
    void*  PersistentPointer(int slot) const { return persistent_ ? mapped[slot] : nullptr; }
    GLsizeiptr Size() const { return size_; }

private:
    GLenum      target_ = GL_ARRAY_BUFFER;
    GLsizeiptr  size_ = 0;
    bool        persistent_ = false;
    bool coherent_ = false;
    GLuint      buffers[SLOT_COUNT]{};
    void*       mapped[SLOT_COUNT]{};

    // CreateStreaming
    bool        streaming_ = false;
    int         numSlots_ = SLOT_COUNT;
    int         lastSlot_ = -1;
    bool        slotSubmitted_[SLOT_COUNT]{};
    uint64_t    slotDrawFrame_[SLOT_COUNT]{};	// the backend frame that draws what was submitted
    byte*       staging_ = nullptr;				// the CPU copy when not persistently mapped
};

void UnbindBufferObjects();
bool IsWriteCombined(void* base);
void CopyBuffer(byte* dst, const byte* src, int numBytes);
bool R_HasBufferStorage();

class idBufferObject
{
public:
    idBufferObject();

    int                 GetSize() const { return (size & ~MAPPED_FLAG); }
    int                 GetAllocedSize() const { return ((size & ~MAPPED_FLAG) + 15) & ~15; }
    bufferUsageType_t   GetUsage() const { return usage; }
#if defined( USE_VULKAN )
    VkBuffer            GetAPIObject() const { return apiObject; }
#else
    GLuint              GetAPIObject() const { return apiObject; }
#endif
    int                 GetOffset() const { return (offsetInOtherBuffer & ~OWNS_BUFFER_FLAG); }

    bool                IsMapped() const { return (size & MAPPED_FLAG) != 0; }
    bool                IsRingBuffer() const { return isRingBuffer_; }

protected:
    void                SetMapped() const { const_cast<int&>(size) |= MAPPED_FLAG; }
    void                SetUnmapped() const { const_cast<int&>(size) &= ~MAPPED_FLAG; }
    bool                OwnsBuffer() const { return ((offsetInOtherBuffer & OWNS_BUFFER_FLAG) != 0); }

protected:
    int                 size;
    int                 offsetInOtherBuffer;
    bufferUsageType_t   usage;

#if defined( USE_VULKAN )
    VkBuffer            apiObject;
#if defined( USE_AMD_ALLOCATOR )
    VmaAllocation       vmaAllocation;
    VmaAllocationInfo   allocation;
#else
    vulkanAllocation_t  allocation;
#endif
#else

    GLuint              apiObject;
    void*               buffer;

#ifdef ANDROID
    bool                isRingBuffer_;
    int                 currentSlot_;
#endif
#endif

    static const int    MAPPED_FLAG         = 1 << (4 * 8 - 1);
    static const int    OWNS_BUFFER_FLAG    = 1 << (4 * 8 - 1);
};

class idVertexBuffer : public idBufferObject
{
public:
    idVertexBuffer();
    ~idVertexBuffer();

    bool                AllocBufferObject(const void* data, int allocSize, bufferUsageType_t usage);
    void                FreeBufferObject();

    void                Reference(const idVertexBuffer& other);
    void                Reference(const idVertexBuffer& other, int refOffset, int refSize);

    void                Update(const void* data, int size, int offset = 0) const;

    void*               MapBuffer(bufferMapType_t mapType);
    idDrawVert*         MapVertexBuffer(bufferMapType_t mapType) {
        return static_cast<idDrawVert*>(MapBuffer(mapType));
    }
    void                UnmapBuffer();
#ifdef ANDROID
    // a dynamic buffer of which writtenBytes from the start were written, see idGLBufferRing::SubmitSlot
    void                UnmapBuffer(int writtenBytes);
    // a static buffer gets larger storage that keeps the first keepBytes, see idVertexCache::GrowStatic
    bool                GrowStatic(int newAllocSize, int keepBytes);
#endif

private:
    void                ClearWithoutFreeing();

#ifdef ANDROID
    idGLBufferRing      ring_;
#endif

DISALLOW_COPY_AND_ASSIGN(idVertexBuffer);
};

class idIndexBuffer : public idBufferObject
{
public:
    idIndexBuffer();
    ~idIndexBuffer();

    bool                AllocBufferObject(const void* data, int allocSize, bufferUsageType_t usage);
    void                FreeBufferObject();

    void                Reference(const idIndexBuffer& other);
    void                Reference(const idIndexBuffer& other, int refOffset, int refSize);

    void                Update(const void* data, int size, int offset = 0) const;

    void*               MapBuffer(bufferMapType_t mapType);
    triIndex_t*         MapIndexBuffer(bufferMapType_t mapType) {
        return static_cast<triIndex_t*>(MapBuffer(mapType));
    }
    void                UnmapBuffer();
#ifdef ANDROID
    void                UnmapBuffer(int writtenBytes);
    bool                GrowStatic(int newAllocSize, int keepBytes);
#endif

private:
    void                ClearWithoutFreeing();

#ifdef ANDROID
    idGLBufferRing      ring_;
#endif

DISALLOW_COPY_AND_ASSIGN(idIndexBuffer);
};

class idUniformBuffer : public idBufferObject
{
public:
    idUniformBuffer();
    ~idUniformBuffer();

    bool                AllocBufferObject(const void* data, int allocSize, bufferUsageType_t usage);
    void                FreeBufferObject();

    void                Reference(const idUniformBuffer& other);
    void                Reference(const idUniformBuffer& other, int refOffset, int refSize);

    void                Update(const void* data, int size, int offset = 0) const;

    void*               MapBuffer(bufferMapType_t mapType);
    void                UnmapBuffer();
#ifdef ANDROID
    void                UnmapBuffer(int writtenBytes);
#endif

private:
    void                ClearWithoutFreeing();

#ifdef ANDROID
    idGLBufferRing      ring_;
#endif

DISALLOW_COPY_AND_ASSIGN(idUniformBuffer);
};

#endif