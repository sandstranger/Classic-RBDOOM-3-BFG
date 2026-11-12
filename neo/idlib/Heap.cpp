/*
===========================================================================

Doom 3 BFG Edition GPL Source Code
Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company.
Copyright (C) 2012 Robert Beckebans
Copyright (C) 2012 Daniel Gibson

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

//===============================================================
//
//	memory allocation all in one place
//
//===============================================================
#include <stdlib.h>
#undef new

/*
==================
Mem_SetOutOfMemoryHandler

The engine frees what it can do without (caches, scratch buffers) when an allocation fails, the
allocation is tried once more after that. See Mem_Alloc16.
==================
*/
static memOutOfMemoryHandler_t	mem_outOfMemoryHandler = NULL;

void Mem_SetOutOfMemoryHandler( memOutOfMemoryHandler_t handler )
{
	mem_outOfMemoryHandler = handler;
}

static void* Mem_TryAlloc16( const size_t paddedSize )
{
#ifdef _WIN32
	// this should work with MSVC and mingw, as long as __MSVCRT_VERSION__ >= 0x0700
	return _aligned_malloc( paddedSize, 16 );
#else // not _WIN32
	// DG: the POSIX solution for linux etc
	// a failed posix_memalign leaves ret as it was: without the check the caller got garbage
	void* ret = NULL;
	if( posix_memalign( &ret, 16, paddedSize ) != 0 )
	{
		return NULL;
	}
	return ret;
	// DG end
#endif // _WIN32
}

/*
==================
Mem_Alloc16
==================
*/
// RB: 64 bit fixes, changed int to size_t
void* Mem_Alloc16( const size_t size, const memTag_t tag )
// RB end
{
	if( !size )
	{
		return NULL;
	}
	const size_t paddedSize = ( size + 15 ) & ~15;
	if( paddedSize < size )
	{
		idLib::FatalError( "Mem_Alloc16: size overflow, %zu bytes (tag %d)", size, ( int )tag );
	}

	void* ret = Mem_TryAlloc16( paddedSize );
	if( ret != NULL )
	{
		return ret;
	}

	// out of memory (or of address space in a 32 bit process): free what the engine can do without and
	// try again, a NULL would only crash somewhere else
	static volatile bool inHandler = false;
	if( mem_outOfMemoryHandler != NULL && !inHandler )
	{
		inHandler = true;
		const bool freed = mem_outOfMemoryHandler( paddedSize );
		inHandler = false;
		if( freed )
		{
			ret = Mem_TryAlloc16( paddedSize );
			if( ret != NULL )
			{
				return ret;
			}
		}
	}

	idLib::FatalError( "Out of memory: %zu bytes could not be allocated (tag %d)", paddedSize, ( int )tag );
	return NULL;
}

/*
==================
Mem_Free16
==================
*/
void Mem_Free16( void* ptr )
{
	if( ptr == NULL )
	{
		return;
	}
#ifdef _WIN32
	_aligned_free( ptr );
#else // not _WIN32
	// DG: Linux/POSIX compatibility
	// can use normal free() for aligned memory
	free( ptr );
	// DG end
#endif // _WIN32
}

/*
==================
Mem_ClearedAlloc
==================
*/
void* Mem_ClearedAlloc( const size_t size, const memTag_t tag )
{
	void* mem = Mem_Alloc( size, tag );
	SIMDProcessor->Memset( mem, 0, size );
	return mem;
}

/*
==================
Mem_CopyString
==================
*/
char* Mem_CopyString( const char* in )
{
	char* out = ( char* )Mem_Alloc( strlen( in ) + 1, TAG_STRING );
	strcpy( out, in );
	return out;
}

