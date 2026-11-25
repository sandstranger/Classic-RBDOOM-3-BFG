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
#ifndef __VERTEXCACHE_H__
#define __VERTEXCACHE_H__

// On GLES these are what a frame may use: the GL storage is what the frames really write, see
// idGLBufferRing::CreateStreaming, the rest is address space of a CPU copy or of a persistent map.
const int VERTCACHE_INDEX_MEMORY_PER_FRAME = 31 * 1024 * 1024;
const int VERTCACHE_VERTEX_MEMORY_PER_FRAME = 31 * 1024 * 1024;
const int VERTCACHE_JOINT_MEMORY_PER_FRAME = 256 * 1024;

#if defined( ANDROID )
// The most a map may use, as before. The GL storage was all of it from the start, 248 MB, while the maps
// use a fraction: the static buffers start smaller now and grow by copying when a map needs more, see
// idVertexCache::GrowStatic.
const int STATIC_INDEX_MEMORY = 4 * 31 * 1024 * 1024;
const int STATIC_VERTEX_MEMORY = 4 * 31 * 1024 * 1024;
#if defined( ID_LOW_MEMORY )
const int STATIC_INDEX_MEMORY_START = 6 * 1024 * 1024;
const int STATIC_VERTEX_MEMORY_START = 16 * 1024 * 1024;
#else
const int STATIC_INDEX_MEMORY_START = 16 * 1024 * 1024;
const int STATIC_VERTEX_MEMORY_START = 32 * 1024 * 1024;
#endif
#else
// there are a lot more static indexes than vertexes, because interactions are just new
// index lists that reference existing vertexes
const int STATIC_INDEX_MEMORY = 4 * 31 * 1024 * 1024;
const int STATIC_VERTEX_MEMORY = 4 * 31 * 1024 * 1024;
const int STATIC_INDEX_MEMORY_START = STATIC_INDEX_MEMORY;
const int STATIC_VERTEX_MEMORY_START = STATIC_VERTEX_MEMORY;
#endif

// vertCacheHandle_t packs size, offset, and frame number into 64 bits.
// 0 is no allocation (ActuallyAlloc never makes it: a static one has VERTCACHE_STATIC set, a per-frame one a size) and is
// never current. Its frame number, 0, is the current one every VERTCACHE_FRAME_MASK + 1 frames: the surfaces whose caches
// were not made yet - a particle system's, new every frame - were taken as cached then and drawn from the start of the
// frame's buffers, the white flashes every 8192 frames (~2:16 at 60 fps) of the 13 bit frame number. A wider frame number
// only made them rarer, and cost the offset (32 MB, the static index buffer is 124 MB) and the size (1 MB, the GUI's
// vertex block is 2.5 MB) their range. Only a handle unused for exactly a multiple of 16384 frames can still look current.
typedef uint64 vertCacheHandle_t;
const int VERTCACHE_STATIC = 1;                 // in the static set, not the per-frame set
const int VERTCACHE_SIZE_SHIFT = 1;
const int VERTCACHE_SIZE_MASK = 0x3FFFFF;		// 22 bits: 4 MB
const int VERTCACHE_OFFSET_SHIFT = 23;
const int VERTCACHE_OFFSET_MASK = 0x7FFFFFF;	// 27 bits: 128 MB
const int VERTCACHE_FRAME_SHIFT = 50;
const int VERTCACHE_FRAME_MASK = 0x3FFF;		// 14 bits: 16384 frames to wrap around

static_assert( STATIC_INDEX_MEMORY <= VERTCACHE_OFFSET_MASK + 1 && STATIC_VERTEX_MEMORY <= VERTCACHE_OFFSET_MASK + 1 &&
			   VERTCACHE_INDEX_MEMORY_PER_FRAME <= VERTCACHE_OFFSET_MASK + 1 && VERTCACHE_VERTEX_MEMORY_PER_FRAME <= VERTCACHE_OFFSET_MASK + 1,
			   "a handle's offset must address every buffer" );

const int VERTEX_CACHE_ALIGN		= 32;
const int INDEX_CACHE_ALIGN			= 16;
const int JOINT_CACHE_ALIGN			= 16;

enum cacheType_t
{
	CACHE_VERTEX,
	CACHE_INDEX,
	CACHE_JOINT
};

struct geoBufferSet_t
{
	idIndexBuffer			indexBuffer;
	idVertexBuffer			vertexBuffer;
	idUniformBuffer			jointBuffer;
	byte* 					mappedVertexBase;
	byte* 					mappedIndexBase;
	byte* 					mappedJointBase;
	idSysInterlockedInteger	indexMemUsed;
	idSysInterlockedInteger	vertexMemUsed;
	idSysInterlockedInteger	jointMemUsed;
	int						allocations;	// number of index and vertex allocations combined
};

class idVertexCache
{
public:
	void			Init( int uniformBufferOffsetAlignment );
	void			Shutdown();
	void			PurgeAll();
	
	// call on loading a new map
	void			FreeStaticData();
	
	// this data is only valid for one frame of rendering
	vertCacheHandle_t	AllocVertex( const void* data, int num, size_t size = sizeof( idDrawVert ) );
	vertCacheHandle_t	AllocIndex( const void* data, int num, size_t size = sizeof( triIndex_t ) );
	vertCacheHandle_t	AllocJoint( const void* data, int num, size_t size = sizeof( idJointMat ) );
	
	// this data is valid until the next map load
	vertCacheHandle_t	AllocStaticVertex( const void* data, int bytes );
	vertCacheHandle_t	AllocStaticIndex( const void* data, int bytes );
	
	byte* 			MappedVertexBuffer( vertCacheHandle_t handle );
	byte* 			MappedIndexBuffer( vertCacheHandle_t handle );
	
	// Returns false if it's been purged
	// This can only be called by the front end, the back end should only be looking at
	// vertCacheHandle_t that are already validated.
	bool			CacheIsCurrent( const vertCacheHandle_t handle );
	static bool		CacheIsStatic( const vertCacheHandle_t handle )
	{
		return ( handle & VERTCACHE_STATIC ) != 0;
	}
	
	// vb/ib is a temporary reference -- don't store it
	bool			GetVertexBuffer( vertCacheHandle_t handle, idVertexBuffer* vb );
	bool			GetIndexBuffer( vertCacheHandle_t handle, idIndexBuffer* ib );
	bool			GetJointBuffer( vertCacheHandle_t handle, idUniformBuffer* jb );
	
	void			BeginBackEnd();
	
public:
	int				currentFrame;	// for determining the active buffers
	int				listNum;		// currentFrame % NUM_FRAME_DATA
	int				drawListNum;	// (currentFrame-1) % NUM_FRAME_DATA
	
	geoBufferSet_t	staticData;
	geoBufferSet_t	frameData[ NUM_FRAME_DATA ];
	
	int				uniformBufferOffsetAlignment;
	
	// High water marks for the per-frame buffers
	int				mostUsedVertex;
	int				mostUsedIndex;
	int				mostUsedJoint;
	
	// Try to make room for <bytes> bytes
	// dataBytes: how much of data is copied (-1: bytes). The allocations are rounded up to the cache's alignment, the
	// data is not: copying the rounded size read past its end
	vertCacheHandle_t	ActuallyAlloc( geoBufferSet_t& vcs, const void* data, int bytes, cacheType_t type, int dataBytes = -1 );

#if defined( ANDROID ) && !defined( USE_VULKAN )
	// larger static storage when the map needs it, the handles stay valid
	void				GrowStatic( cacheType_t type, int neededBytes );
#endif
};

// platform specific code to memcpy into vertex buffers efficiently
// 16 byte alignment is guaranteed
void CopyBuffer( byte* dst, const byte* src, int numBytes );

extern	idVertexCache	vertexCache;

#endif // __VERTEXCACHE_H__
