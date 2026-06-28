/*
===========================================================================

Doom 3 BFG Edition GPL Source Code
Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company.

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
/*
================================================================================================
Contains the DxtDecoder implementation.
================================================================================================
*/

#pragma hdrstop
#include "DXTCodec_local.h"
#include "DXTCodec.h"
#if defined(__ARM_NEON) || defined(__ARM_NEON__) || defined(__aarch64__) || defined(_M_ARM64)
#include <arm_neon.h>
#include <cstdint>
#endif
/*
========================
idDxtDecoder::EmitBlock
========================
*/
#if defined(__ARM_NEON) || defined(__ARM_NEON__) || defined(__aarch64__) || defined(_M_ARM64)
void idDxtDecoder::EmitBlock( byte* outPtr, int x, int y, const byte* colorBlock )
{
    outPtr += ( y * width + x ) * 4;
    const int stride = width * 4;
    uint8x16_t row0 = vld1q_u8( colorBlock );
    uint8x16_t row1 = vld1q_u8( colorBlock + 16 );
    uint8x16_t row2 = vld1q_u8( colorBlock + 32 );
    uint8x16_t row3 = vld1q_u8( colorBlock + 48 );
    vst1q_u8( outPtr, row0 ); outPtr += stride;
    vst1q_u8( outPtr, row1 ); outPtr += stride;
    vst1q_u8( outPtr, row2 ); outPtr += stride;
    vst1q_u8( outPtr, row3 );
}
#else
void idDxtDecoder::EmitBlock( byte* outPtr, int x, int y, const byte* colorBlock )
{
	outPtr += ( y * width + x ) * 4;
	for( int j = 0; j < 4; j++ )
	{
		memcpy( outPtr, &colorBlock[j * 4 * 4], 4 * 4 );
		outPtr += width * 4;
	}
}
#endif

/*
========================
idDxtDecoder::DecodeAlphaValues
========================
*/
#if defined(__ARM_NEON) || defined(__ARM_NEON__) || defined(__aarch64__) || defined(_M_ARM64)
void idDxtDecoder::DecodeAlphaValues( byte* colorBlock, const int offset )
{
    uint8_t alphas[16] = {0};
    alphas[0] = ReadByte();
    alphas[1] = ReadByte();

    if( alphas[0] > alphas[1] ) {
        alphas[2] = ( 6 * alphas[0] + 1 * alphas[1] ) / 7;
        alphas[3] = ( 5 * alphas[0] + 2 * alphas[1] ) / 7;
        alphas[4] = ( 4 * alphas[0] + 3 * alphas[1] ) / 7;
        alphas[5] = ( 3 * alphas[0] + 4 * alphas[1] ) / 7;
        alphas[6] = ( 2 * alphas[0] + 5 * alphas[1] ) / 7;
        alphas[7] = ( 1 * alphas[0] + 6 * alphas[1] ) / 7;
    } else {
        alphas[2] = ( 4 * alphas[0] + 1 * alphas[1] ) / 5;
        alphas[3] = ( 3 * alphas[0] + 2 * alphas[1] ) / 5;
        alphas[4] = ( 2 * alphas[0] + 3 * alphas[1] ) / 5;
        alphas[5] = ( 1 * alphas[0] + 4 * alphas[1] ) / 5;
        alphas[6] = 0;
        alphas[7] = 255;
    }

    uint8x16_t aPalette = vld1q_u8(alphas);
    uint8_t rawIndices[8];
    for(int i = 0; i < 6; ++i) rawIndices[i] = ReadByte();
    uint64_t bits = *(uint64_t*)rawIndices;
    alignas(16) uint8_t idx[16];

    for(int i = 0; i < 16; ++i) {
        idx[i] = (uint8_t)((bits >> (i * 3)) & 7);
    }
    uint8x16_t indices = vld1q_u8(idx);
    uint8x16_t finalAlphas = vqtbl1q_u8(aPalette, indices);
    uint8x16x4_t rgba = vld4q_u8(colorBlock);
    rgba.val[offset] = finalAlphas;
    vst4q_u8(colorBlock, rgba);
}
#else
void idDxtDecoder::DecodeAlphaValues( byte* colorBlock, const int offset )
{
	int i;
	unsigned int indexes;
	byte alphas[8];
	
	alphas[0] = ReadByte();
	alphas[1] = ReadByte();
	
	if( alphas[0] > alphas[1] )
	{
		alphas[2] = ( 6 * alphas[0] + 1 * alphas[1] ) / 7;
		alphas[3] = ( 5 * alphas[0] + 2 * alphas[1] ) / 7;
		alphas[4] = ( 4 * alphas[0] + 3 * alphas[1] ) / 7;
		alphas[5] = ( 3 * alphas[0] + 4 * alphas[1] ) / 7;
		alphas[6] = ( 2 * alphas[0] + 5 * alphas[1] ) / 7;
		alphas[7] = ( 1 * alphas[0] + 6 * alphas[1] ) / 7;
	}
	else
	{
		alphas[2] = ( 4 * alphas[0] + 1 * alphas[1] ) / 5;
		alphas[3] = ( 3 * alphas[0] + 2 * alphas[1] ) / 5;
		alphas[4] = ( 2 * alphas[0] + 3 * alphas[1] ) / 5;
		alphas[5] = ( 1 * alphas[0] + 4 * alphas[1] ) / 5;
		alphas[6] = 0;
		alphas[7] = 255;
	}
	
	colorBlock += offset;
	
	indexes = ( int )ReadByte() | ( ( int )ReadByte() << 8 ) | ( ( int )ReadByte() << 16 );
	for( i = 0; i < 8; i++ )
	{
		colorBlock[i * 4] = alphas[indexes & 7];
		indexes >>= 3;
	}
	
	indexes = ( int )ReadByte() | ( ( int )ReadByte() << 8 ) | ( ( int )ReadByte() << 16 );
	for( i = 8; i < 16; i++ )
	{
		colorBlock[i * 4] = alphas[indexes & 7];
		indexes >>= 3;
	}
}
#endif
/*
========================
idDxtDecoder::DecodeColorValues
========================
*/
#if defined(__ARM_NEON) || defined(__ARM_NEON__) || defined(__aarch64__) || defined(_M_ARM64)
void idDxtDecoder::DecodeColorValues( byte* colorBlock, bool noBlack, bool writeAlpha )
{
    alignas(16) uint8_t colors[16];
    unsigned short color0 = ReadUShort();
    unsigned short color1 = ReadUShort();

    ColorFrom565( color0, &colors[0] );
    ColorFrom565( color1, &colors[4] );
    colors[3] = 255;
    colors[7] = 255;

    if( noBlack || color0 > color1 ) {
        for(int c=0; c<3; ++c) {
            colors[8+c]  = ( 2 * colors[0+c] + colors[4+c] ) / 3;
            colors[12+c] = ( colors[0+c] + 2 * colors[4+c] ) / 3;
        }
        colors[11] = 255;
        colors[15] = 255;
    } else {
        for(int c=0; c<3; ++c) {
            colors[8+c]  = ( colors[0+c] + colors[4+c] ) >> 1;
            colors[12+c] = 0;
        }
        colors[11] = 255;
        colors[15] = 0;
    }

    uint8x16_t palette = vld1q_u8(colors);
    uint32_t indexes = ReadUInt();
    alignas(16) uint8_t lookup_idx[64];
    for( int i = 0; i < 16; i++ ) {
        uint8_t color_idx = ((indexes >> (i * 2)) & 3) * 4;
        lookup_idx[i*4 + 0] = color_idx + 0; // R
        lookup_idx[i*4 + 1] = color_idx + 1; // G
        lookup_idx[i*4 + 2] = color_idx + 2; // B
        lookup_idx[i*4 + 3] = color_idx + 3; // A
    }

    uint32x4_t rgbMask = vdupq_n_u32(0x00FFFFFF);
    for( int i = 0; i < 4; i++ ) {
        uint8x16_t pIdx = vld1q_u8(&lookup_idx[i * 16]);
        uint8x16_t newPixels = vqtbl1q_u8(palette, pIdx);
        uint8_t* target = colorBlock + i * 16;

        if( writeAlpha ) {
            vst1q_u8(target, newPixels);
        } else {
            uint32x4_t oldP = vld1q_u32((uint32_t*)target);
            uint32x4_t result = vbslq_u32(rgbMask, vreinterpretq_u32_u8(newPixels), oldP);
            vst1q_u32((uint32_t*)target, result);
        }
    }
}
#else
void idDxtDecoder::DecodeColorValues( byte* colorBlock, bool noBlack, bool writeAlpha )
{
	byte colors[4][4];
	
	unsigned short color0 = ReadUShort();
	unsigned short color1 = ReadUShort();
	
	ColorFrom565( color0, colors[0] );
	ColorFrom565( color1, colors[1] );
	
	colors[0][3] = 255;
	colors[1][3] = 255;
	
	if( noBlack || color0 > color1 )
	{
		colors[2][0] = ( 2 * colors[0][0] + 1 * colors[1][0] ) / 3;
		colors[2][1] = ( 2 * colors[0][1] + 1 * colors[1][1] ) / 3;
		colors[2][2] = ( 2 * colors[0][2] + 1 * colors[1][2] ) / 3;
		colors[2][3] = 255;
		
		colors[3][0] = ( 1 * colors[0][0] + 2 * colors[1][0] ) / 3;
		colors[3][1] = ( 1 * colors[0][1] + 2 * colors[1][1] ) / 3;
		colors[3][2] = ( 1 * colors[0][2] + 2 * colors[1][2] ) / 3;
		colors[3][3] = 255;
	}
	else
	{
		colors[2][0] = ( 1 * colors[0][0] + 1 * colors[1][0] ) / 2;
		colors[2][1] = ( 1 * colors[0][1] + 1 * colors[1][1] ) / 2;
		colors[2][2] = ( 1 * colors[0][2] + 1 * colors[1][2] ) / 2;
		colors[2][3] = 255;
		
		colors[3][0] = 0;
		colors[3][1] = 0;
		colors[3][2] = 0;
		colors[3][3] = 0;
	}
	
	unsigned int indexes = ReadUInt();
	for( int i = 0; i < 16; i++ )
	{
		colorBlock[i * 4 + 0] = colors[indexes & 3][0];
		colorBlock[i * 4 + 1] = colors[indexes & 3][1];
		colorBlock[i * 4 + 2] = colors[indexes & 3][2];
		if( writeAlpha )
		{
			colorBlock[i * 4 + 3] = colors[indexes & 3][3];
		}
		indexes >>= 2;
	}
}
#endif
/*
========================
idDxtDecoder::DecodeCTX1Values
========================
*/
void idDxtDecoder::DecodeCTX1Values( byte* colorBlock )
{
	byte colors[4][2];
	
	colors[0][0] = ReadByte();
	colors[0][1] = ReadByte();
	colors[1][0] = ReadByte();
	colors[1][1] = ReadByte();
	
	colors[2][0] = ( 2 * colors[0][0] + 1 * colors[1][0] ) / 3;
	colors[2][1] = ( 2 * colors[0][1] + 1 * colors[1][1] ) / 3;
	colors[3][0] = ( 1 * colors[0][0] + 2 * colors[1][0] ) / 3;
	colors[3][1] = ( 1 * colors[0][1] + 2 * colors[1][1] ) / 3;
	
	unsigned int indexes = ReadUInt();
	for( int i = 0; i < 16; i++ )
	{
		colorBlock[i * 4 + 0] = colors[indexes & 3][0];
		colorBlock[i * 4 + 1] = colors[indexes & 3][1];
		indexes >>= 2;
	}
}

/*
========================
idDxtDecoder::DecompressImageDXT1
========================
*/
void idDxtDecoder::DecompressImageDXT1( const byte* inBuf, byte* outBuf, int _width, int _height )
{
	byte block[64];
	
	this->width = _width;
	this->height = _height;
	this->inData = inBuf;
	
	for( int j = 0; j < _height; j += 4 )
	{
		for( int i = 0; i < _width; i += 4 )
		{
			DecodeColorValues( block, false, true );
			EmitBlock( outBuf, i, j, block );
		}
	}
}

/*
========================
idDxtDecoder::DecompressImageDXT5
========================
*/
void idDxtDecoder::DecompressImageDXT5( const byte* inBuf, byte* outBuf, int _width, int _height )
{
	byte block[64];
	
	this->width = _width;
	this->height = _height;
	this->inData = inBuf;
	
	for( int j = 0; j < _height; j += 4 )
	{
		for( int i = 0; i < _width; i += 4 )
		{
			DecodeAlphaValues( block, 3 );
			DecodeColorValues( block, true, false );
			EmitBlock( outBuf, i, j, block );
		}
	}
}

/*
========================
idDxtDecoder::DecompressImageDXT5_nVidia7x
========================
*/
void idDxtDecoder::DecompressImageDXT5_nVidia7x( const byte* inBuf, byte* outBuf, int _width, int _height )
{
	byte block[64];
	
	this->width = _width;
	this->height = _height;
	this->inData = inBuf;
	
	for( int j = 0; j < _height; j += 4 )
	{
		for( int i = 0; i < _width; i += 4 )
		{
			DecodeAlphaValues( block, 3 );
			DecodeColorValues( block, false, false );
			EmitBlock( outBuf, i, j, block );
		}
	}
}

/*
========================
idDxtDecoder::DecompressYCoCgDXT5
========================
*/

#if defined(__ARM_NEON) || defined(__ARM_NEON__) || defined(__aarch64__) || defined(_M_ARM64)
void idDxtDecoder::DecompressYCoCgDXT5( const byte* inBuf, byte* outBuf, int _width, int _height )
{
    DecompressImageDXT5_nVidia7x( inBuf, outBuf, _width, _height );

    const int pixelCount = _width * _height;
    int i = 0;

    const uint8x8_t v_128_u8 = vdup_n_u8( 128 );
    const int16x8_t v_128_s16 = vdupq_n_s16( 128 );

    for( ; i + 8 <= pixelCount; i += 8 )
    {
        uint8x8x4_t px = vld4_u8( outBuf + i * 4 );

        uint8x8_t scale8 = vadd_u8( vshr_n_u8( px.val[2], 3 ), vdup_n_u8( 1 ) );
        uint16x8_t scale16 = vmovl_u8( scale8 );

        float32x4_t scale_low_f = vcvtq_f32_u32( vmovl_u16( vget_low_u16( scale16 ) ) );
        float32x4_t scale_high_f = vcvtq_f32_u32( vmovl_u16( vget_high_u16( scale16 ) ) );

        int16x8_t r16 = vreinterpretq_s16_u16( vsubl_u8( px.val[0], v_128_u8 ) );
        float32x4_t r_low_f  = vcvtq_f32_s32( vmovl_s16( vget_low_s16( r16 ) ) );
        float32x4_t r_high_f = vcvtq_f32_s32( vmovl_s16( vget_high_s16( r16 ) ) );

        r_low_f = vdivq_f32( r_low_f, scale_low_f );
        r_high_f = vdivq_f32( r_high_f, scale_high_f );

        int16x8_t r_res16 = vcombine_s16( vmovn_s32( vcvtq_s32_f32( r_low_f ) ),
                                          vmovn_s32( vcvtq_s32_f32( r_high_f ) ) );
        px.val[0] = vqmovun_s16( vaddq_s16( r_res16, v_128_s16 ) );

        int16x8_t g16 = vreinterpretq_s16_u16( vsubl_u8( px.val[1], v_128_u8 ) );
        float32x4_t g_low_f  = vcvtq_f32_s32( vmovl_s16( vget_low_s16( g16 ) ) );
        float32x4_t g_high_f = vcvtq_f32_s32( vmovl_s16( vget_high_s16( g16 ) ) );

        g_low_f = vdivq_f32( g_low_f, scale_low_f );
        g_high_f = vdivq_f32( g_high_f, scale_high_f );

        int16x8_t g_res16 = vcombine_s16( vmovn_s32( vcvtq_s32_f32( g_low_f ) ),
                                          vmovn_s32( vcvtq_s32_f32( g_high_f ) ) );
        px.val[1] = vqmovun_s16( vaddq_s16( g_res16, v_128_s16 ) );

        px.val[2] = vdup_n_u8( 0 );

        vst4_u8( outBuf + i * 4, px );
    }

    for( ; i < pixelCount; ++i )
    {
        const int scale = ( outBuf[i * 4 + 2] >> 3 ) + 1;
        outBuf[i * 4 + 0] = byte( ( int( outBuf[i * 4 + 0] ) - 128 ) / scale + 128 );
        outBuf[i * 4 + 1] = byte( ( int( outBuf[i * 4 + 1] ) - 128 ) / scale + 128 );
        outBuf[i * 4 + 2] = 0;
    }
}
#else
void idDxtDecoder::DecompressYCoCgDXT5( const byte* inBuf, byte* outBuf, int _width, int _height )
{
	DecompressImageDXT5_nVidia7x( inBuf, outBuf, _width, _height );
	// descale the CoCg values and set the scale factor effectively to 1
	for( int i = 0; i < _width * _height; i++ )
	{
		int scale = ( outBuf[i * 4 + 2] >> 3 ) + 1;
		outBuf[i * 4 + 0] = byte( ( outBuf[i * 4 + 0] - 128 ) / scale + 128 );
		outBuf[i * 4 + 1] = byte( ( outBuf[i * 4 + 1] - 128 ) / scale + 128 );
		outBuf[i * 4 + 2] = 0;	// this translates to a scale factor of 1 for uncompressed
	}
}
#endif

/*
========================
idDxtDecoder::DecompressYCoCgCTX1DXT5A
========================
*/
void idDxtDecoder::DecompressYCoCgCTX1DXT5A( const byte* inBuf, byte* outBuf, int _width, int _height )
{
	byte block[64];
	
	this->width = _width;
	this->height = _height;
	this->inData = inBuf;
	
	for( int j = 0; j < _height; j += 4 )
	{
		for( int i = 0; i < _width; i += 4 )
		{
			DecodeAlphaValues( block, 3 );
			DecodeCTX1Values( block );
			EmitBlock( outBuf, i, j, block );
		}
	}
}

/*
========================
idDxtDecoder::DecodeNormalYValues
========================
*/
#if defined(__ARM_NEON) || defined(__ARM_NEON__) || defined(__aarch64__) || defined(_M_ARM64)
void idDxtDecoder::DecodeNormalYValues( byte* normalBlock, const int offsetY, byte& c0, byte& c1 )
{
    unsigned short normal0 = ReadUShort();
    unsigned short normal1 = ReadUShort();

    assert( normal0 >= normal1 );

    const byte ny0 = NormalYFrom565( normal0 );
    const byte ny1 = NormalYFrom565( normal1 );
    const byte ny2 = byte( ( 2 * ny0 + ny1 ) / 3 );
    const byte ny3 = byte( ( ny0 + 2 * ny1 ) / 3 );

    c0 = NormalBiasFrom565( normal0 );
    c1 = NormalScaleFrom565( normal0 );

    alignas(16) const uint8_t yTableBytes[16] = { ny0, ny1, ny2, ny3, 0 };
    const uint8x16_t yTable = vld1q_u8( yTableBytes );

    unsigned int indexes = ReadUInt();

    alignas(16) uint8_t idxBytes[16];
    for( int i = 0; i < 16; ++i ) {
        idxBytes[i] = ( uint8_t )( (indexes >> (i * 2)) & 3 );
    }

    const uint8x16_t idx = vld1q_u8( idxBytes );

    const uint8x16_t yOut = vqtbl1q_u8( yTable, idx );

    uint8x16x4_t rgba = vld4q_u8( normalBlock );
    rgba.val[offsetY] = yOut;
    vst4q_u8( normalBlock, rgba );
}
#else
void idDxtDecoder::DecodeNormalYValues( byte* normalBlock, const int offsetY, byte& c0, byte& c1 )
{
	int i;
	unsigned int indexes;
	unsigned short normal0, normal1;
	byte normalsY[4];
	
	normal0 = ReadUShort();
	normal1 = ReadUShort();
	
	assert( normal0 >= normal1 );
	
	normalsY[0] = NormalYFrom565( normal0 );
	normalsY[1] = NormalYFrom565( normal1 );
	normalsY[2] = ( 2 * normalsY[0] + 1 * normalsY[1] ) / 3;
	normalsY[3] = ( 1 * normalsY[0] + 2 * normalsY[1] ) / 3;
	
	c0 = NormalBiasFrom565( normal0 );
	c1 = NormalScaleFrom565( normal0 );
	
	byte* normalYPtr = normalBlock + offsetY;
	
	indexes = ReadUInt();
	for( i = 0; i < 16; i++ )
	{
		normalYPtr[i * 4] = normalsY[indexes & 3];
		indexes >>= 2;
	}
}
#endif
/*
========================
UShortSqrt
========================
*/
byte UShortSqrt( unsigned short s )
{
#if 1
	int t, b, r, x;
	
	r = 0;
	for( b = 0x10000000; b != 0; b >>= 2 )
	{
		t = r + b;
		r >>= 1;
		x = -( t <= s );
		s = s - ( unsigned short )( t & x );
		r += b & x;
	}
	return byte( r );
#else
	int t, b, r;
	
	r = 0;
	for( b = 0x10000000; b != 0; b >>= 2 )
	{
		t = r + b;
		r >>= 1;
		if( t <= s )
		{
			s -= t;
			r += b;
		}
	}
	return r;
#endif
}

/*
========================
idDxtDecoder::DeriveNormalZValues
========================
*/
void idDxtDecoder::DeriveNormalZValues( byte* normalBlock )
{
	int i;
	
	for( i = 0; i < 16; i++ )
	{
		int x = normalBlock[i * 4 + 0] - 127;
		int y = normalBlock[i * 4 + 1] - 127;
		normalBlock[i * 4 + 2] = 128 + UShortSqrt( ( unsigned short )( 16383 - x * x - y * y ) );
	}
}

/*
========================
idDxtDecoder::UnRotateNormals
========================
*/
#if defined(__ARM_NEON) || defined(__ARM_NEON__) || defined(__aarch64__) || defined(_M_ARM64)
void UnRotateNormals( const byte* block, float* normals, byte c0, byte c1 )
{
    const float angle = -( float( c0 ) / 255.0f ) * idMath::PI;
    const float s = sinf( angle );
    const float c = cosf( angle );

    const int scale = ( c1 >> 3 ) + 1;

    const float32x4_t sVec      = vdupq_n_f32( s );
    const float32x4_t cVec      = vdupq_n_f32( c );
    const float32x4_t mulVec    = vdupq_n_f32( 2.0f / 255.0f );
    const float32x4_t minusOne  = vdupq_n_f32( -1.0f );
    const float32x4_t biasVec   = vdupq_n_f32( 128.0f );
    const float32x4_t invScale  = vdupq_n_f32( 1.0f / float( scale ) );
    const float32x4_t zeroVec   = vdupq_n_f32( 0.0f );

    for( int i = 0; i < 16; i += 8 )
    {
        const uint8x8x4_t px = vld4_u8( block + i * 4 );

        uint16x8_t r16 = vmovl_u8( px.val[0] );
        float32x4_t x0 = vcvtq_f32_u32( vmovl_u16( vget_low_u16( r16 ) ) );
        float32x4_t x1 = vcvtq_f32_u32( vmovl_u16( vget_high_u16( r16 ) ) );

        uint16x8_t g16 = vmovl_u8( px.val[1] );
        float32x4_t y0 = vcvtq_f32_u32( vmovl_u16( vget_low_u16( g16 ) ) );
        float32x4_t y1 = vcvtq_f32_u32( vmovl_u16( vget_high_u16( g16 ) ) );

        x0 = vmlaq_f32( minusOne, x0, mulVec );
        x1 = vmlaq_f32( minusOne, x1, mulVec );

        y0 = vaddq_f32( vmulq_f32( vsubq_f32( y0, biasVec ), invScale ), biasVec );
        y1 = vaddq_f32( vmulq_f32( vsubq_f32( y1, biasVec ), invScale ), biasVec );
        y0 = vmlaq_f32( minusOne, y0, mulVec );
        y1 = vmlaq_f32( minusOne, y1, mulVec );

        float32x4_t rx0 = vmlsq_f32( vmulq_f32( cVec, x0 ), sVec, y0 );
        float32x4_t ry0 = vmlaq_f32( vmulq_f32( sVec, x0 ), cVec, y0 );

        float32x4_t rx1 = vmlsq_f32( vmulq_f32( cVec, x1 ), sVec, y1 );
        float32x4_t ry1 = vmlaq_f32( vmulq_f32( sVec, x1 ), cVec, y1 );

        float32x4x4_t out0 = { rx0, ry0, zeroVec, zeroVec };
        vst4q_f32( normals + i * 4, out0 );

        float32x4x4_t out1 = { rx1, ry1, zeroVec, zeroVec };
        vst4q_f32( normals + (i + 4) * 4, out1 );
    }
}
#else
void UnRotateNormals( const byte* block, float* normals, byte c0, byte c1 )
{
	int rotation = c0;
	float angle = -( rotation / 255.0f ) * idMath::PI;
	float s = sin( angle );
	float c = cos( angle );
	
	int scale = ( c1 >> 3 ) + 1;
	for( int i = 0; i < 16; i++ )
	{
		float x = block[i * 4 + 0] / 255.0f * 2.0f - 1.0f;
		float y = ( ( block[i * 4 + 1] - 128 ) / scale + 128 ) / 255.0f * 2.0f - 1.0f;
		float rx = c * x - s * y;
		float ry = s * x + c * y;
		normals[i * 4 + 0] = rx;
		normals[i * 4 + 1] = ry;
	}
}
#endif
/*
========================
idDxtDecoder::DecompressNormalMapDXT1
========================
*/
void idDxtDecoder::DecompressNormalMapDXT1( const byte* inBuf, byte* outBuf, int _width, int _height )
{
	byte block[64];
	
	this->width = _width;
	this->height = _height;
	this->inData = inBuf;
	
	for( int j = 0; j < _height; j += 4 )
	{
		for( int i = 0; i < _width; i += 4 )
		{
			DecodeColorValues( block, false, true );
#if 1
			float normals[16 * 4];
			/*
			for ( int k = 0; k < 16; k++ ) {
				normals[k*4+0] = block[k*4+0] / 255.0f * 2.0f - 1.0f;
				normals[k*4+1] = block[k*4+1] / 255.0f * 2.0f - 1.0f;
			}
			*/
			UnRotateNormals( block, normals, block[0 * 4 + 2], 0 );
			for( int k = 0; k < 16; k++ )
			{
				float x = normals[k * 4 + 0];
				float y = normals[k * 4 + 1];
				float z = 1.0f - x * x - y * y;
				if( z < 0.0f ) z = 0.0f;
				normals[k * 4 + 2] = sqrt( z );
			}
			for( int k = 0; k < 16; k++ )
			{
				block[k * 4 + 0] = idMath::Ftob( ( normals[k * 4 + 0] + 1.0f ) / 2.0f * 255.0f );
				block[k * 4 + 1] = idMath::Ftob( ( normals[k * 4 + 1] + 1.0f ) / 2.0f * 255.0f );
				block[k * 4 + 2] = idMath::Ftob( ( normals[k * 4 + 2] + 1.0f ) / 2.0f * 255.0f );
			}
#else
			DeriveNormalZValues( block );
#endif
			EmitBlock( outBuf, i, j, block );
		}
	}
}

/*
========================
idDxtDecoder::DecompressNormalMapDXT1Renormalize
========================
*/
void idDxtDecoder::DecompressNormalMapDXT1Renormalize( const byte* inBuf, byte* outBuf, int _width, int _height )
{
	byte block[64];
	
	this->width = _width;
	this->height = _height;
	this->inData = inBuf;
	
	for( int j = 0; j < _height; j += 4 )
	{
		for( int i = 0; i < _width; i += 4 )
		{
			DecodeColorValues( block, false, true );
			
			for( int k = 0; k < 16; k++ )
			{
				float normal[3];
				normal[0] = block[k * 4 + 0] / 255.0f * 2.0f - 1.0f;
				normal[1] = block[k * 4 + 1] / 255.0f * 2.0f - 1.0f;
				normal[2] = block[k * 4 + 2] / 255.0f * 2.0f - 1.0f;
				float rsq = idMath::InvSqrt( normal[0] * normal[0] + normal[1] * normal[1] + normal[2] * normal[2] );
				normal[0] *= rsq;
				normal[1] *= rsq;
				normal[2] *= rsq;
				block[k * 4 + 0] = idMath::Ftob( ( normal[0] + 1.0f ) / 2.0f * 255.0f + 0.5f );
				block[k * 4 + 1] = idMath::Ftob( ( normal[1] + 1.0f ) / 2.0f * 255.0f + 0.5f );
				block[k * 4 + 2] = idMath::Ftob( ( normal[2] + 1.0f ) / 2.0f * 255.0f + 0.5f );
			}
			
			EmitBlock( outBuf, i, j, block );
		}
	}
}

/*
========================
idDxtDecoder::DecompressNormalMapDXT5Renormalize
========================
*/
#if defined(__ARM_NEON) || defined(__ARM_NEON__) || defined(__aarch64__) || defined(_M_ARM64)
void idDxtDecoder::DecompressNormalMapDXT5Renormalize( const byte* inBuf, byte* outBuf, int _width, int _height )
{
    byte block[64];

    this->width = _width;
    this->height = _height;
    this->inData = inBuf;

    const float32x4_t v_scale = vdupq_n_f32(2.0f / 255.0f);
    const float32x4_t v_minus_one = vdupq_n_f32(-1.0f);
    const float32x4_t v_half = vdupq_n_f32(0.5f);
    const float32x4_t v_255 = vdupq_n_f32(255.0f);

    for( int j = 0; j < _height; j += 4 )
    {
        for( int i = 0; i < _width; i += 4 )
        {
            DecodeAlphaValues( block, 3 );
            DecodeColorValues( block, false, false );

            for( int k = 0; k < 64; k += 32 )
            {
                uint8x8x4_t px = vld4_u8( block + k );

                uint16x8_t cx = vmovl_u8( px.val[3] );
                uint16x8_t cy = vmovl_u8( px.val[1] );
                uint16x8_t cz = vmovl_u8( px.val[2] );

                float32x4_t x_low = vcvtq_f32_u32( vmovl_u16( vget_low_u16( cx ) ) );
                float32x4_t x_high = vcvtq_f32_u32( vmovl_u16( vget_high_u16( cx ) ) );

                float32x4_t y_low = vcvtq_f32_u32( vmovl_u16( vget_low_u16( cy ) ) );
                float32x4_t y_high = vcvtq_f32_u32( vmovl_u16( vget_high_u16( cy ) ) );

                float32x4_t z_low = vcvtq_f32_u32( vmovl_u16( vget_low_u16( cz ) ) );
                float32x4_t z_high = vcvtq_f32_u32( vmovl_u16( vget_high_u16( cz ) ) );

                auto renormalize = [&](float32x4_t& vx, float32x4_t& vy, float32x4_t& vz) {
                    vx = vmlaq_f32(v_minus_one, vx, v_scale);
                    vy = vmlaq_f32(v_minus_one, vy, v_scale);
                    vz = vmlaq_f32(v_minus_one, vz, v_scale);

                    float32x4_t dot = vmulq_f32(vx, vx);
                    dot = vmlaq_f32(dot, vy, vy);
                    dot = vmlaq_f32(dot, vz, vz);

                    float32x4_t rsq = vrsqrteq_f32(dot);
                    rsq = vmulq_f32(rsq, vrsqrtsq_f32(dot, vmulq_f32(rsq, rsq)));

                    vx = vmulq_f32(vx, rsq);
                    vy = vmulq_f32(vy, rsq);
                    vz = vmulq_f32(vz, rsq);

                    vx = vmlaq_f32(v_half, vmlaq_f32(v_half, vx, v_half), v_255);
                    vy = vmlaq_f32(v_half, vmlaq_f32(v_half, vy, v_half), v_255);
                    vz = vmlaq_f32(v_half, vmlaq_f32(v_half, vz, v_half), v_255);
                };

                renormalize(x_low, y_low, z_low);
                renormalize(x_high, y_high, z_high);

                uint16x4_t rx_l = vmovn_u32( vcvtaq_u32_f32( x_low ) );
                uint16x4_t rx_h = vmovn_u32( vcvtaq_u32_f32( x_high ) );
                px.val[0] = vmovn_u16( vcombine_u16( rx_l, rx_h ) );

                uint16x4_t ry_l = vmovn_u32( vcvtaq_u32_f32( y_low ) );
                uint16x4_t ry_h = vmovn_u32( vcvtaq_u32_f32( y_high ) );
                px.val[1] = vmovn_u16( vcombine_u16( ry_l, ry_h ) );

                uint16x4_t rz_l = vmovn_u32( vcvtaq_u32_f32( z_low ) );
                uint16x4_t rz_h = vmovn_u32( vcvtaq_u32_f32( z_high ) );
                px.val[2] = vmovn_u16( vcombine_u16( rz_l, rz_h ) );

                vst4_u8( block + k, px );
            }

            EmitBlock( outBuf, i, j, block );
        }
    }
}
#else
void idDxtDecoder::DecompressNormalMapDXT5Renormalize( const byte* inBuf, byte* outBuf, int _width, int _height )
{
	byte block[64];
	
	this->width = _width;
	this->height = _height;
	this->inData = inBuf;
	
	for( int j = 0; j < _height; j += 4 )
	{
		for( int i = 0; i < _width; i += 4 )
		{
			DecodeAlphaValues( block, 3 );
			DecodeColorValues( block, false, false );
			
			for( int k = 0; k < 16; k++ )
			{
				float normal[3];
#if 0 // object-space
				normal[0] = block[k * 4 + 0] / 255.0f * 2.0f - 1.0f;
				normal[1] = block[k * 4 + 1] / 255.0f * 2.0f - 1.0f;
				normal[2] = block[k * 4 + 3] / 255.0f * 2.0f - 1.0f;
#else
				normal[0] = block[k * 4 + 3] / 255.0f * 2.0f - 1.0f;
				normal[1] = block[k * 4 + 1] / 255.0f * 2.0f - 1.0f;
				normal[2] = block[k * 4 + 2] / 255.0f * 2.0f - 1.0f;
#endif
				float rsq = idMath::InvSqrt( normal[0] * normal[0] + normal[1] * normal[1] + normal[2] * normal[2] );
				normal[0] *= rsq;
				normal[1] *= rsq;
				normal[2] *= rsq;
				block[k * 4 + 0] = idMath::Ftob( ( normal[0] + 1.0f ) / 2.0f * 255.0f + 0.5f );
				block[k * 4 + 1] = idMath::Ftob( ( normal[1] + 1.0f ) / 2.0f * 255.0f + 0.5f );
				block[k * 4 + 2] = idMath::Ftob( ( normal[2] + 1.0f ) / 2.0f * 255.0f + 0.5f );
			}
			
			EmitBlock( outBuf, i, j, block );
		}
	}
}
#endif
/*
========================
idDxtDecoder::BiasScaleNormalY
========================
*/
void BiasScaleNormalY( byte* normals, const int offsetY, const byte c0, const byte c1 )
{
	int bias = c0 - 4;
	int scale = ( c1 >> 3 ) + 1;
	for( int i = 0; i < 16; i++ )
	{
		normals[i * 4 + offsetY] = byte( ( normals[i * 4 + offsetY] - 128 ) / scale + bias );
	}
}

/*
========================
idDxtDecoder::BiasScaleNormals
========================
*/
void BiasScaleNormals( const byte* block, float* normals, const byte c0, const byte c1 )
{
	int bias = c0 - 4;
	int scale = ( c1 >> 3 ) + 1;
	for( int i = 0; i < 16; i++ )
	{
		normals[i * 4 + 0] = block[i * 4 + 0] / 255.0f * 2.0f - 1.0f;
		normals[i * 4 + 1] = ( ( block[i * 4 + 1] - 128.0f ) / scale + bias ) / 255.0f * 2.0f - 1.0f;
	}
}

/*
========================
idDxtDecoder::DecompressNormalMapDXT5
========================
*/
#if defined(__ARM_NEON) || defined(__ARM_NEON__) || defined(__aarch64__) || defined(_M_ARM64)
void idDxtDecoder::DecompressNormalMapDXT5( const byte* inBuf, byte* outBuf, int _width, int _height ) {
    byte block[64];
    byte c0, c1;

    this->width = _width;
    this->height = _height;
    this->inData = inBuf;

    const float32x4_t one = vdupq_n_f32(1.0f);
    const float32x4_t zero = vdupq_n_f32(0.0f);
    const float32x4_t scale = vdupq_n_f32(127.5f);

    for (int j = 0; j < _height; j += 4) {
        for (int i = 0; i < _width; i += 4) {
            DecodeAlphaValues(block, 0);
            DecodeNormalYValues(block, 1, c0, c1);

            alignas(16) float normals[16 * 4];
            UnRotateNormals(block, normals, c0, c1);

            for (int k = 0; k < 16; k += 8) {
                float32x4x4_t n0 = vld4q_f32(&normals[k * 4]);
                float32x4x4_t n1 = vld4q_f32(&normals[(k + 4) * 4]);

                auto process_normals = [&](float32x4_t x, float32x4_t y) -> float32x4x3_t {
                    float32x4_t xx = vmulq_f32(x, x);
                    float32x4_t yy = vmulq_f32(y, y);
                    float32x4_t z = vsubq_f32(one, vaddq_f32(xx, yy));
                    z = vmaxq_f32(z, zero);

                    z = vsqrtq_f32(z);

                    x = vmulq_f32(vaddq_f32(x, one), scale);
                    y = vmulq_f32(vaddq_f32(y, one), scale);
                    z = vmulq_f32(vaddq_f32(z, one), scale);

                    return {x, y, z};
                };

                float32x4x3_t res0 = process_normals(n0.val[0], n0.val[1]);
                float32x4x3_t res1 = process_normals(n1.val[0], n1.val[1]);

                uint8x8x4_t out_px;

                uint16x4_t rx0 = vmovn_u32(vcvtaq_u32_f32(res0.val[0]));
                uint16x4_t rx1 = vmovn_u32(vcvtaq_u32_f32(res1.val[0]));
                out_px.val[0] = vmovn_u16(vcombine_u16(rx0, rx1));

                uint16x4_t ry0 = vmovn_u32(vcvtaq_u32_f32(res0.val[1]));
                uint16x4_t ry1 = vmovn_u32(vcvtaq_u32_f32(res1.val[1]));
                out_px.val[1] = vmovn_u16(vcombine_u16(ry0, ry1));

                uint16x4_t rz0 = vmovn_u32(vcvtaq_u32_f32(res0.val[2]));
                uint16x4_t rz1 = vmovn_u32(vcvtaq_u32_f32(res1.val[2]));
                out_px.val[2] = vmovn_u16(vcombine_u16(rz0, rz1));

                out_px.val[3] = vdup_n_u8(255);

                vst4_u8(block + k * 4, out_px);
            }

            EmitBlock(outBuf, i, j, block);
        }
    }
}
#else
void idDxtDecoder::DecompressNormalMapDXT5( const byte* inBuf, byte* outBuf, int _width, int _height )
{
	byte block[64];
	byte c0, c1;
	
	this->width = _width;
	this->height = _height;
	this->inData = inBuf;
	
	for( int j = 0; j < _height; j += 4 )
	{
		for( int i = 0; i < _width; i += 4 )
		{
			DecodeAlphaValues( block, 0 );
			DecodeNormalYValues( block, 1, c0, c1 );
#if 1
			float normals[16 * 4];
			//BiasScaleNormals( block, normals, c0, c1 );
			UnRotateNormals( block, normals, c0, c1 );
			for( int k = 0; k < 16; k++ )
			{
				float x = normals[k * 4 + 0];
				float y = normals[k * 4 + 1];
				float z = 1.0f - x * x - y * y;
				if( z < 0.0f ) z = 0.0f;
				normals[k * 4 + 2] = sqrt( z );
			}
			for( int k = 0; k < 16; k++ )
			{
				block[k * 4 + 0] = idMath::Ftob( ( normals[k * 4 + 0] + 1.0f ) / 2.0f * 255.0f );
				block[k * 4 + 1] = idMath::Ftob( ( normals[k * 4 + 1] + 1.0f ) / 2.0f * 255.0f );
				block[k * 4 + 2] = idMath::Ftob( ( normals[k * 4 + 2] + 1.0f ) / 2.0f * 255.0f );
			}
#else
			BiasScaleNormalY( block, 1, c0, c1 );
			DeriveNormalZValues( block );
#endif
			
			EmitBlock( outBuf, i, j, block );
		}
	}
}
#endif
/*
========================
idDxtDecoder::DecompressNormalMapDXN2
========================
*/
void idDxtDecoder::DecompressNormalMapDXN2( const byte* inBuf, byte* outBuf, int _width, int _height )
{
	byte block[64];
	
	this->width = _width;
	this->height = _height;
	this->inData = inBuf;
	
	for( int j = 0; j < _height; j += 4 )
	{
		for( int i = 0; i < _width; i += 4 )
		{
			DecodeAlphaValues( block, 0 );
			DecodeAlphaValues( block, 1 );
#if 1
			float normals[16 * 4];
			for( int k = 0; k < 16; k++ )
			{
				normals[k * 4 + 0] = block[k * 4 + 0] / 255.0f * 2.0f - 1.0f;
				normals[k * 4 + 1] = block[k * 4 + 1] / 255.0f * 2.0f - 1.0f;
			}
			for( int k = 0; k < 16; k++ )
			{
				float x = normals[k * 4 + 0];
				float y = normals[k * 4 + 1];
				float z = 1.0f - x * x - y * y;
				if( z < 0.0f ) z = 0.0f;
				normals[k * 4 + 2] = sqrt( z );
			}
			for( int k = 0; k < 16; k++ )
			{
				block[k * 4 + 0] = idMath::Ftob( ( normals[k * 4 + 0] + 1.0f ) / 2.0f * 255.0f );
				block[k * 4 + 1] = idMath::Ftob( ( normals[k * 4 + 1] + 1.0f ) / 2.0f * 255.0f );
				block[k * 4 + 2] = idMath::Ftob( ( normals[k * 4 + 2] + 1.0f ) / 2.0f * 255.0f );
			}
#else
			DeriveNormalZValues( block );
#endif
			EmitBlock( outBuf, i, j, block );
		}
	}
}

/*
========================
idDxtDecoder::DecomposeColorBlock
========================
*/
void idDxtDecoder::DecomposeColorBlock( byte colors[2][4], byte colorIndices[16], bool noBlack )
{
	int i;
	unsigned int indices;
	unsigned short color0, color1;
	int colorRemap1[] = { 3, 0, 2, 1 };
	int colorRemap2[] = { 1, 3, 2, 0 };
	int* crm;
	
	color0 = ReadUShort();
	color1 = ReadUShort();
	
	ColorFrom565( color0, colors[0] );
	ColorFrom565( color1, colors[1] );
	
	if( noBlack || color0 > color1 )
	{
		crm = colorRemap1;
	}
	else
	{
		crm = colorRemap2;
	}
	
	indices = ReadUInt();
	for( i = 0; i < 16; i++ )
	{
		colorIndices[i] = ( byte )crm[ indices & 3 ];
		indices >>= 2;
	}
}

/*
========================
idDxtDecoder::DecomposeAlphaBlock
========================
*/
void idDxtDecoder::DecomposeAlphaBlock( byte colors[2][4], byte alphaIndices[16] )
{
	int i;
	unsigned char alpha0, alpha1;
	unsigned int indices;
	int alphaRemap1[] = { 7, 0, 6, 5, 4, 3, 2, 1 };
	int alphaRemap2[] = { 1, 6, 2, 3, 4, 5, 0, 7 };
	int* arm;
	
	alpha0 = ReadByte();
	alpha1 = ReadByte();
	
	colors[0][3] = alpha0;
	colors[1][3] = alpha1;
	
	if( alpha0 > alpha1 )
	{
		arm = alphaRemap1;
	}
	else
	{
		arm = alphaRemap2;
	}
	
	indices = ( int )ReadByte() | ( ( int )ReadByte() << 8 ) | ( ( int )ReadByte() << 16 );
	for( i = 0; i < 8; i++ )
	{
		alphaIndices[i] = ( byte )arm[ indices & 7 ];
		indices >>= 3;
	}
	
	indices = ( int )ReadByte() | ( ( int )ReadByte() << 8 ) | ( ( int )ReadByte() << 16 );
	for( i = 8; i < 16; i++ )
	{
		alphaIndices[i] = ( byte )arm[ indices & 7 ];
		indices >>= 3;
	}
}

/*
========================
idDxtDecoder::DecomposeImageDXT1
========================
*/
void idDxtDecoder::DecomposeImageDXT1( const byte* inBuf, byte* colorIndices, byte* pic1, byte* pic2, int _width, int _height )
{
	byte colors[2][4];
	byte indices[16];
	
	this->width = _width;
	this->height = _height;
	this->inData = inBuf;
	
	// extract the colors from the DXT
	for( int j = 0; j < _height; j += 4 )
	{
		for( int i = 0; i < _width; i += 4 )
		{
			DecomposeColorBlock( colors, indices, false );
			
			memcpy( colorIndices + ( j + 0 ) * _width + i, indices + 0, 4 );
			memcpy( colorIndices + ( j + 1 ) * _width + i, indices + 4, 4 );
			memcpy( colorIndices + ( j + 2 ) * _width + i, indices + 8, 4 );
			memcpy( colorIndices + ( j + 3 ) * _width + i, indices + 12, 4 );
			
			memcpy( pic1 + j * _width / 4 + i, colors[0], 4 );
			
			memcpy( pic2 + j * _width / 4 + i, colors[1], 4 );
		}
	}
}

/*
========================
idDxtDecoder::DecomposeImageDXT5
========================
*/
void idDxtDecoder::DecomposeImageDXT5( const byte* inBuf, byte* colorIndices, byte* alphaIndices, byte* pic1, byte* pic2, int _width, int _height )
{
	byte colors[2][4];
	byte colorInd[16];
	byte alphaInd[16];
	
	this->width = _width;
	this->height = _height;
	this->inData = inBuf;
	
	// extract the colors from the DXT
	for( int j = 0; j < _height; j += 4 )
	{
		for( int i = 0; i < _width; i += 4 )
		{
			DecomposeAlphaBlock( colors, alphaInd );
			DecomposeColorBlock( colors, colorInd, true );
			
			memcpy( colorIndices + ( j + 0 ) * _width + i, colorInd + 0, 4 );
			memcpy( colorIndices + ( j + 1 ) * _width + i, colorInd + 4, 4 );
			memcpy( colorIndices + ( j + 2 ) * _width + i, colorInd + 8, 4 );
			memcpy( colorIndices + ( j + 3 ) * _width + i, colorInd + 12, 4 );
			
			memcpy( colorIndices + ( j + 0 ) * _width + i, alphaInd + 0, 4 );
			memcpy( colorIndices + ( j + 1 ) * _width + i, alphaInd + 4, 4 );
			memcpy( colorIndices + ( j + 2 ) * _width + i, alphaInd + 8, 4 );
			memcpy( colorIndices + ( j + 3 ) * _width + i, alphaInd + 12, 4 );
			
			memcpy( pic1 + j * _width / 4 + i, colors[0], 4 );
			
			memcpy( pic2 + j * _width / 4 + i, colors[1], 4 );
		}
	}
}

