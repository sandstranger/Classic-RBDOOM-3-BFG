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


static const GLenum bufferUsage = GL_DYNAMIC_DRAW;
#ifndef ANDROID
static const GLenum bufferStorageFlags = GL_DYNAMIC_STORAGE_BIT | GL_MAP_WRITE_BIT | GL_MAP_READ_BIT;
#else
static const GLenum bufferStorageFlags = GL_MAP_WRITE_BIT | GL_MAP_READ_BIT;
#endif

/*
================================================================================================
Buffer Objects
================================================================================================
*/

void UnbindBufferObjects()
{
    if (!glConfig.directStateAccess) {
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    }
}

/*
================================================================================================
idVertexBuffer
================================================================================================
*/

idVertexBuffer::idVertexBuffer()
{
    size = 0;
    offsetInOtherBuffer = OWNS_BUFFER_FLAG;
    apiObject = 0xFFFF;
    SetUnmapped();
}

bool idVertexBuffer::AllocBufferObject( const void* data, int allocSize, bufferUsageType_t _usage )
{
    assert( apiObject == 0xFFFF );
    assert_16_byte_aligned( data );

    if( allocSize <= 0 )
    {
        idLib::Error( "idVertexBuffer::AllocBufferObject: allocSize = %i", allocSize );
    }

    size = allocSize;
    usage = _usage;

    bool allocationFailed = false;

    int numBytes = GetAllocedSize();

    if (usage == BU_DYNAMIC)
    {
#ifndef ANDROID
        if (!glConfig.directStateAccess)
#endif
        {
            glGenBuffers(1, (GLuint*)&apiObject);
            if (apiObject == 0xFFFF)
                idLib::FatalError("idVertexBuffer::AllocBufferObject: failed");
            glBindBuffer(GL_ARRAY_BUFFER, apiObject);
            glBufferData(GL_ARRAY_BUFFER, numBytes, NULL, bufferUsage);
        }
#ifndef ANDROID
        else {
			glCreateBuffers(1, (GLuint*)&apiObject);
			if (apiObject == 0xFFFF)
				idLib::FatalError("idVertexBuffer::AllocBufferObject: failed");
			glNamedBufferStorage(apiObject, numBytes, NULL, bufferStorageFlags);
		}
#endif
    }
    else
    {
#ifndef ANDROID
        if (!glConfig.directStateAccess)
#endif
        {
            glGenBuffers(1, (GLuint*)&apiObject);
            if (apiObject == 0xFFFF)
                idLib::FatalError("idVertexBuffer::AllocBufferObject: failed");
            glBindBuffer(GL_ARRAY_BUFFER, apiObject);
            glBufferData(GL_ARRAY_BUFFER, numBytes, data, bufferUsage);
        }
#ifndef ANDROID
        else {
			glCreateBuffers(1, (GLuint*)&apiObject);
			if (apiObject == 0xFFFF)
				idLib::FatalError("idVertexBuffer::AllocBufferObject: failed");
			glNamedBufferStorage(apiObject, numBytes, data, bufferStorageFlags);
		}
#endif
    }

    GLenum err = glGetError();
    if( err == GL_OUT_OF_MEMORY )
    {
        idLib::Warning( "idVertexBuffer::AllocBufferObject: allocation failed" );
        allocationFailed = true;
    }

    if( r_showBuffers.GetBool() )
    {
        idLib::Printf( "vertex buffer alloc %p, api %p (%i bytes)\n", this, ( GLuint* )&apiObject, GetSize() );
    }

    if( data != NULL && usage == BU_DYNAMIC )
    {
        Update( data, allocSize );
    }

    return !allocationFailed;
}

void idVertexBuffer::FreeBufferObject()
{
    if( IsMapped() )
    {
        UnmapBuffer();
    }

    if( OwnsBuffer() == false )
    {
        ClearWithoutFreeing();
        return;
    }

    if( apiObject == 0xFFFF )
        return;

    if( r_showBuffers.GetBool() )
    {
        idLib::Printf( "vertex buffer free %p, api %p (%i bytes)\n", this, ( GLuint* )&apiObject, GetSize() );
    }

    glDeleteBuffers( 1, ( GLuint* )&apiObject );
    ClearWithoutFreeing();
}

void idVertexBuffer::Update( const void* data, int updateSize, int offset ) const
{
    assert( apiObject != 0xFFFF );
    assert_16_byte_aligned( data );
    assert( ( GetOffset() & 15 ) == 0 );

    if( updateSize > GetSize() )
    {
        idLib::FatalError( "idVertexBuffer::Update: size overrun, %i > %i\n", updateSize, GetSize() );
    }

    int numBytes = ( updateSize + 15 ) & ~15;

    if( usage == BU_DYNAMIC )
    {

        CopyBuffer( ( byte* )buffer + offset, ( const byte* )data, numBytes );
    }
    else
    {
#ifndef ANDROID
        if (!glConfig.directStateAccess)
#endif
        {
            glBindBuffer(GL_ARRAY_BUFFER, apiObject);
            glBufferSubData(GL_ARRAY_BUFFER, GetOffset() + offset, (GLsizeiptr)numBytes, data);
        }
#ifndef ANDROID
        else {
			glNamedBufferSubData(apiObject, GetOffset() + offset, (GLsizeiptrARB)numBytes, data);
		}
#endif
    }
}

void* idVertexBuffer::MapBuffer( bufferMapType_t mapType )
{
    assert( apiObject != 0xFFFF );
    assert( IsMapped() == false );

    buffer = NULL;
#ifndef ANDROID
    if (!glConfig.directStateAccess)
#endif
    {
        glBindBuffer(GL_ARRAY_BUFFER, apiObject);

        if (mapType == BM_READ)
        {
            buffer = glMapBufferRange(GL_ARRAY_BUFFER, GetOffset(), GetAllocedSize(),
                                      GL_MAP_READ_BIT | GL_MAP_UNSYNCHRONIZED_BIT);
        }
        else if (mapType == BM_WRITE)
        {


            GLbitfield flags = GL_MAP_WRITE_BIT;
            if (usage == BU_DYNAMIC)
            {
                flags |= GL_MAP_INVALIDATE_BUFFER_BIT;
            }
            else
            {
                flags |= GL_MAP_UNSYNCHRONIZED_BIT;
            }
            buffer = glMapBufferRange(GL_ARRAY_BUFFER, GetOffset(), GetAllocedSize(), flags);
        }
        else
        {
            assert(false);
        }
    }
#ifndef ANDROID
    else {
		if (mapType == BM_READ)
		{
			buffer = glMapNamedBufferRange(apiObject, 0, GetAllocedSize(),
				GL_MAP_READ_BIT | GL_MAP_UNSYNCHRONIZED_BIT);
		}
		else if (mapType == BM_WRITE)
		{
			GLbitfield flags = GL_MAP_WRITE_BIT;
			if (usage == BU_DYNAMIC)
				flags |= GL_MAP_INVALIDATE_BUFFER_BIT;
			else
				flags |= GL_MAP_UNSYNCHRONIZED_BIT;
			buffer = glMapNamedBufferRange(apiObject, 0, GetAllocedSize(), flags);
		}
		else
		{
			assert(false);
		}
		if (buffer != NULL)
			buffer = (byte*)buffer + GetOffset();
	}
#endif
    SetMapped();

    if( buffer == NULL )
    {
        idLib::FatalError( "idVertexBuffer::MapBuffer: failed" );
    }
    return buffer;
}

void idVertexBuffer::UnmapBuffer()
{
    assert( apiObject != 0xFFFF );
    assert( IsMapped() );
#ifndef ANDROID
    if (!glConfig.directStateAccess)
#endif
    {
        glBindBuffer(GL_ARRAY_BUFFER, apiObject);
        if (!glUnmapBuffer(GL_ARRAY_BUFFER))
            idLib::Printf("idVertexBuffer::UnmapBuffer failed\n");
    }
#ifndef ANDROID
    else {
		if (!glUnmapNamedBuffer(apiObject))
			idLib::Printf("idVertexBuffer::UnmapBuffer failed\n");
	}
#endif
    SetUnmapped();
}

void idVertexBuffer::ClearWithoutFreeing()
{
    size = 0;
    offsetInOtherBuffer = OWNS_BUFFER_FLAG;
    apiObject = 0xFFFF;
}

/*
================================================================================================
idIndexBuffer
================================================================================================
*/

idIndexBuffer::idIndexBuffer()
{
    size = 0;
    offsetInOtherBuffer = OWNS_BUFFER_FLAG;
    apiObject = 0xFFFF;
    SetUnmapped();
}

bool idIndexBuffer::AllocBufferObject( const void* data, int allocSize, bufferUsageType_t _usage )
{
    assert( apiObject == 0xFFFF );
    assert_16_byte_aligned( data );

    if( allocSize <= 0 )
    {
        idLib::Error( "idIndexBuffer::AllocBufferObject: allocSize = %i", allocSize );
    }

    size = allocSize;
    usage = _usage;

    bool allocationFailed = false;

    int numBytes = GetAllocedSize();


    if (usage == BU_DYNAMIC)
    {
#ifndef ANDROID
        if (!glConfig.directStateAccess)
#endif
        {
            glGenBuffers(1, (GLuint*)&apiObject);
            if (apiObject == 0xFFFF)
                idLib::FatalError("idIndexBuffer::AllocBufferObject: failed");
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, apiObject);
            glBufferData(GL_ELEMENT_ARRAY_BUFFER, numBytes, NULL, bufferUsage);
        }
#ifndef ANDROID
        else {
			glCreateBuffers(1, (GLuint*)&apiObject);
			if (apiObject == 0xFFFF)
				idLib::FatalError("idIndexBuffer::AllocBufferObject: failed");
			glNamedBufferStorage(apiObject, numBytes, NULL, bufferStorageFlags);
		}
#endif
    }
    else
    {
#ifndef ANDROID
        if (!glConfig.directStateAccess)
#endif
        {
            glGenBuffers(1, (GLuint*)&apiObject);
            if (apiObject == 0xFFFF)
                idLib::FatalError("idIndexBuffer::AllocBufferObject: failed");
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, apiObject);
            glBufferData(GL_ELEMENT_ARRAY_BUFFER, numBytes, data, bufferUsage);
        }
#ifndef ANDROID
        else {
			glCreateBuffers(1, (GLuint*)&apiObject);
			if (apiObject == 0xFFFF)
				idLib::FatalError("idIndexBuffer::AllocBufferObject: failed");
			glNamedBufferStorage(apiObject, numBytes, data, bufferStorageFlags);
		}
#endif
    }

    GLenum err = glGetError();
    if( err == GL_OUT_OF_MEMORY )
    {
        idLib::Warning( "idIndexBuffer:AllocBufferObject: allocation failed" );
        allocationFailed = true;
    }

    if( r_showBuffers.GetBool() )
    {
        idLib::Printf( "index buffer alloc %p, api %p (%i bytes)\n", this, ( GLuint* )&apiObject, GetSize() );
    }

    if( data != NULL && usage == BU_DYNAMIC )
    {
        Update( data, allocSize );
    }

    return !allocationFailed;
}

void idIndexBuffer::FreeBufferObject()
{
    if( IsMapped() )
    {
        UnmapBuffer();
    }

    if( OwnsBuffer() == false )
    {
        ClearWithoutFreeing();
        return;
    }

    if( apiObject == 0xFFFF )
        return;

    if( r_showBuffers.GetBool() )
    {
        idLib::Printf( "index buffer free %p, api %p (%i bytes)\n", this, ( GLuint* )&apiObject, GetSize() );
    }

    glDeleteBuffers( 1, ( GLuint* )&apiObject );
    ClearWithoutFreeing();
}

void idIndexBuffer::Update( const void* data, int updateSize, int offset ) const
{
    assert( apiObject != 0xFFFF );
    assert_16_byte_aligned( data );
    assert( ( GetOffset() & 15 ) == 0 );

    if( updateSize > GetSize() )
    {
        idLib::FatalError( "idIndexBuffer::Update: size overrun, %i > %i\n", updateSize, GetSize() );
    }

    int numBytes = ( updateSize + 15 ) & ~15;

    if( usage == BU_DYNAMIC )
    {
        CopyBuffer( ( byte* )buffer + offset, ( const byte* )data, numBytes );
    }
    else
    {
#ifndef ANDROID
        if (!glConfig.directStateAccess)
#endif
        {
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, apiObject);
            glBufferSubData(GL_ELEMENT_ARRAY_BUFFER, GetOffset() + offset, (GLsizeiptr)numBytes, data);
        }
#ifndef ANDROID
        else {
			glNamedBufferSubData(apiObject, GetOffset() + offset, (GLsizeiptrARB)numBytes, data);
		}
#endif
    }
}

void* idIndexBuffer::MapBuffer( bufferMapType_t mapType )
{
    assert( apiObject != 0xFFFF );
    assert( IsMapped() == false );

    buffer = NULL;
#ifndef ANDROID
    if (!glConfig.directStateAccess)
#endif
    {
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, apiObject);

        if (mapType == BM_READ)
        {
            buffer = glMapBufferRange(GL_ELEMENT_ARRAY_BUFFER, 0, GetAllocedSize(),
                                      GL_MAP_READ_BIT | GL_MAP_UNSYNCHRONIZED_BIT);
            if (buffer != NULL)
                buffer = (byte*)buffer + GetOffset();
        }
        else if (mapType == BM_WRITE)
        {
            GLbitfield flags = GL_MAP_WRITE_BIT;
            if (usage == BU_DYNAMIC)
                flags |= GL_MAP_INVALIDATE_BUFFER_BIT;
            else
                flags |= GL_MAP_UNSYNCHRONIZED_BIT;
            buffer = glMapBufferRange(GL_ELEMENT_ARRAY_BUFFER, 0, GetAllocedSize(), flags);
            if (buffer != NULL)
                buffer = (byte*)buffer + GetOffset();
        }
        else
        {
            assert(false);
        }
    }
#ifndef ANDROID
    else {
		if (mapType == BM_READ)
		{
			buffer = glMapNamedBufferRange(apiObject, 0, GetAllocedSize(),
				GL_MAP_READ_BIT | GL_MAP_UNSYNCHRONIZED_BIT);
		}
		else if (mapType == BM_WRITE)
		{
			GLbitfield flags = GL_MAP_WRITE_BIT;
			if (usage == BU_DYNAMIC)
				flags |= GL_MAP_INVALIDATE_BUFFER_BIT;
			else
				flags |= GL_MAP_UNSYNCHRONIZED_BIT;
			buffer = glMapNamedBufferRange(apiObject, 0, GetAllocedSize(), flags);
		}
		else
		{
			assert(false);
		}
		if (buffer != NULL)
			buffer = (byte*)buffer + GetOffset();
	}
#endif
    SetMapped();

    if( buffer == NULL )
    {
        idLib::FatalError( "idIndexBuffer::MapBuffer: failed" );
    }
    return buffer;
}

void idIndexBuffer::UnmapBuffer()
{
    assert( apiObject != 0xFFFF );
    assert( IsMapped() );
#ifndef ANDROID
    if (!glConfig.directStateAccess)
#endif
    {
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, apiObject);
        if (!glUnmapBuffer(GL_ELEMENT_ARRAY_BUFFER))
            idLib::Printf("idIndexBuffer::UnmapBuffer failed\n");
    }
#ifndef ANDROID
    else {
		if (!glUnmapNamedBuffer(apiObject))
			idLib::Printf("idIndexBuffer::UnmapBuffer failed\n");
	}
#endif
    buffer = NULL;
    SetUnmapped();
}

void idIndexBuffer::ClearWithoutFreeing()
{
    size = 0;
    offsetInOtherBuffer = OWNS_BUFFER_FLAG;
    apiObject = 0xFFFF;
}

/*
================================================================================================
idUniformBuffer
================================================================================================
*/

idUniformBuffer::idUniformBuffer()
{
    size = 0;
    offsetInOtherBuffer = OWNS_BUFFER_FLAG;
    apiObject = 0xFFFF;
    SetUnmapped();
}

bool idUniformBuffer::AllocBufferObject( const void* data, int allocSize, bufferUsageType_t _usage )
{
    assert( apiObject == 0xFFFF );
    assert_16_byte_aligned( data );

    if( allocSize <= 0 )
    {
        idLib::Error( "idUniformBuffer::AllocBufferObject: allocSize = %i", allocSize );
    }

    size = allocSize;
    usage = _usage;

    bool allocationFailed = false;

    const int numBytes = GetAllocedSize();


    if (usage == BU_DYNAMIC)
    {
#ifndef ANDROID
        if (!glConfig.directStateAccess)
#endif
        {
            glGenBuffers(1, (GLuint*)&apiObject);
            glBindBuffer(GL_UNIFORM_BUFFER, apiObject);
            glBufferData(GL_UNIFORM_BUFFER, numBytes, NULL, GL_STREAM_DRAW);
            glBindBuffer(GL_UNIFORM_BUFFER, 0);
        }
#ifndef ANDROID
        else {
			glCreateBuffers(1, (GLuint*)&apiObject);
			glNamedBufferStorage(apiObject, numBytes, NULL, bufferStorageFlags);
		}
#endif
    }
    else
    {
#ifndef ANDROID
        if (!glConfig.directStateAccess)
#endif
        {
            glGenBuffers(1, (GLuint*)&apiObject);
            glBindBuffer(GL_UNIFORM_BUFFER, apiObject);
            glBufferData(GL_UNIFORM_BUFFER, numBytes, data, GL_STATIC_DRAW);
            glBindBuffer(GL_UNIFORM_BUFFER, 0);
        }
#ifndef ANDROID
        else {
			glCreateBuffers(1, (GLuint*)&apiObject);
			glNamedBufferStorage(apiObject, numBytes, data, bufferStorageFlags);
		}
#endif
    }

    if( r_showBuffers.GetBool() )
    {
        idLib::Printf( "uniform buffer alloc %p, api %p (%i bytes)\n", this, ( GLuint* )&apiObject, GetSize() );
    }

    if( data != NULL && usage == BU_DYNAMIC )
    {
        Update( data, allocSize );
    }

    return !allocationFailed;
}

void idUniformBuffer::FreeBufferObject()
{
    if( IsMapped() )
    {
        UnmapBuffer();
    }

    if( OwnsBuffer() == false )
    {
        ClearWithoutFreeing();
        return;
    }

    if( apiObject == 0xFFFF )
        return;

    if( r_showBuffers.GetBool() )
    {
        idLib::Printf( "uniform buffer free %p, api %p (%i size)\n", this, ( GLuint* )&apiObject, GetSize() );
    }

    glDeleteBuffers( 1, ( GLuint* )&apiObject );
    ClearWithoutFreeing();
}

void idUniformBuffer::Update( const void* data, int updateSize, int offset ) const
{
    assert( apiObject != 0xFFFF );
    assert_16_byte_aligned( data );
    assert( ( GetOffset() & 15 ) == 0 );

    if( updateSize > GetSize() )
    {
        idLib::FatalError( "idUniformBuffer::Update: size overrun, %i > %i\n", updateSize, GetSize() );
    }

    const int numBytes = ( updateSize + 15 ) & ~15;

    if( usage == BU_DYNAMIC )
    {
        CopyBuffer( ( byte* )buffer + offset, ( const byte* )data, numBytes );
    }
    else
    {
#ifndef ANDROID
        if (!glConfig.directStateAccess)
#endif
        {
            glBindBuffer(GL_UNIFORM_BUFFER, apiObject);
            glBufferSubData(GL_UNIFORM_BUFFER, GetOffset() + offset, (GLsizeiptr)numBytes, data);
        }
#ifndef ANDROID
        else {
			glNamedBufferSubData(apiObject, GetOffset() + offset, (GLsizeiptr)numBytes, data);
		}
#endif
    }
}

void* idUniformBuffer::MapBuffer( bufferMapType_t mapType )
{
    assert( IsMapped() == false );
    assert( mapType == BM_WRITE );
    assert( apiObject != 0xFFFF );

    buffer = NULL;
#ifndef ANDROID
    if (!glConfig.directStateAccess)
#endif
    {
        glBindBuffer(GL_UNIFORM_BUFFER, apiObject);
        assert(GetOffset() == 0);

        GLbitfield flags = GL_MAP_WRITE_BIT;
        if (usage == BU_DYNAMIC)
            flags |= GL_MAP_INVALIDATE_BUFFER_BIT;
        else
            flags |= GL_MAP_UNSYNCHRONIZED_BIT;

        buffer = glMapBufferRange(GL_UNIFORM_BUFFER, 0, GetAllocedSize(), flags);
    }
#ifndef ANDROID
    else {
		assert(GetOffset() == 0);
		GLbitfield flags = GL_MAP_WRITE_BIT;
		if (usage == BU_DYNAMIC)
			flags |= GL_MAP_INVALIDATE_BUFFER_BIT;
		else
			flags |= GL_MAP_UNSYNCHRONIZED_BIT;
		buffer = glMapNamedBufferRange(apiObject, 0, GetAllocedSize(), flags);
	}
#endif
    if( buffer != NULL )
    {
        buffer = ( byte* )buffer + GetOffset();
    }

    SetMapped();

    if( buffer == NULL )
    {
        idLib::FatalError( "idUniformBuffer::MapBuffer: failed" );
    }
    return ( float* ) buffer;
}

void idUniformBuffer::UnmapBuffer()
{
    assert( apiObject != 0xFFFF );
    assert( IsMapped() );
#ifndef ANDROID
    if (!glConfig.directStateAccess)
#endif
    {
        glBindBuffer(GL_UNIFORM_BUFFER, apiObject);
        if (!glUnmapBuffer(GL_UNIFORM_BUFFER))
            idLib::Printf("idUniformBuffer::UnmapBuffer failed\n");
    }
#ifndef ANDROID
    else {
		if (!glUnmapNamedBuffer(apiObject))
			idLib::Printf("idUniformBuffer::UnmapBuffer failed\n");
	}
#endif
    buffer = NULL;
    SetUnmapped();
}

void idUniformBuffer::ClearWithoutFreeing()
{
    size = 0;
    offsetInOtherBuffer = OWNS_BUFFER_FLAG;
    apiObject = 0xFFFF;
}