#pragma once

/**
    putest's plumbing: checks, pictures, a headless GL context, the two-input
    rig, and the harness's OWN model of the room.

    The model (`Room`) never calls the plugin's geometry. It stands the lamps
    at an arbitrary height D (7.3 screen heights: D drops out of the plugin's
    arithmetic, and an odd value shows it does), casts a ray from each screen
    point to two opposite corners of each square lamp, crosses the occluder's
    plane at d = k D, and carries the crossing back to the screen through the
    reference point -- where the occluder's picture is defined. The lamp's
    image on the picture is the rectangle between the two corners' images,
    and the fraction of the lamp a point cannot see is the occluder's exact
    area inside that rectangle over its area, in double, against occluders
    made of rectangles whose edges are texel edges (so the rasterised Src is
    exactly the shape the model integrates). It reads back from the plugin
    only what it asked for: the parameters' physical values, through
    Controls, because a slider's float is what the plugin really got.
*/

#include "Controls.h"
#include "Model.h"
#include "Names.h"
#include "Penumbra.h"
#include "Shaders.h"

#include <OpenGL/OpenGL.h>
#include <OpenGL/gl3.h>
#include <zlib.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <functional>
#include <map>
#include <sstream>
#include <string>
#include <unistd.h>
#include <vector>

using namespace penumbra;

namespace
{
int failures = 0;
bool quiet   = false;

void Check( bool ok, const std::string& what )
{
	if( !quiet )
		std::printf( "  %s  %s\n", ok ? "ok  " : "FAIL", what.c_str() );
	if( !ok )
		++failures;
}

/// A negative control: something the check must be able to REJECT. Passes
/// when `rejected` is true.
void Negative( bool rejected, const std::string& what )
{
	if( !quiet )
		std::printf( "  %s  negative control: %s\n", rejected ? "ok  " : "FAIL", what.c_str() );
	if( !rejected )
		++failures;
}

/// EVERY conversion must be a floating-point one.
__attribute__( ( format( printf, 1, 0 ) ) ) std::string fmt( const char* format, double a, double b = 0.0, double c = 0.0, double d = 0.0 )
{
	char buffer[ 320 ];
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wformat-nonliteral"
	std::snprintf( buffer, sizeof( buffer ), format, a, b, c, d );
#pragma clang diagnostic pop
	return buffer;
}

/// Run a check silently and return how many of its assertions failed,
/// leaving the global count as it was: how a negative control asks "did the
/// check reject this?".
template< typename F >
int failuresOf( F&& run )
{
	const int before = failures;
	const bool was   = quiet;
	quiet            = true;
	run();
	quiet            = was;
	const int caught = failures - before;
	failures         = before;
	return caught;
}

constexpr double kFloatUlp = 1.0 / 16777216.0;///< 2^-24, float32's unit roundoff at 1

/// One float ULP at the magnitude of x.
double ulpAt( double x )
{
	const double m = std::max( std::fabs( x ), 1e-30 );
	return std::ldexp( 1.0, static_cast< int >( std::floor( std::log2( m ) ) ) - 23 );
}

/// The sRGB pair's budget, absolute, on values in 0..1 -- a bound, derived in
/// AGENTS.md ("Tolerances") from GLSL 4.10 4.5.1: pow is exp2( y log2 x ),
/// exp2 good to ( 3 + 2|x| ) ULP and log2 to 3 ULP (absolute 2^-21 in
/// [0.5, 2]), + - * correctly rounded, / 2.5 ULP. decode's relative error is
/// at most 5.1e-6 (at its knee, where exp2's argument is largest) and
/// encode's absolute error at most 6.8e-7; encode scales a relative error in
/// its input by y encode'(y) = 0.44 y^(1/2.4), which is 0.04 at the knee, so
/// encode( decode( rgb / a ) L ) a is good to 1.9e-6 absolute over the whole
/// range (summed worst cases, evaluated on a grid on 2026-11-16). 4e-6 is
/// used; the tolerance is three times it.
constexpr double kPowBound = 4e-6;
constexpr double kPowTol   = 3.0 * kPowBound;

/// The sRGB pair, IEC 61966-2-1:1999 (amended 2003): the harness's own copy.
/// The plugin's (Controls.cpp) decodes the lamps' colours, so it must not
/// also be the yardstick -- a mutant there would move both sides at once.
double srgbToLinear( double v )
{
	if( v <= 0.04045 )
		return v / 12.92;
	return std::pow( ( v + 0.055 ) / 1.055, 2.4 );
}

double linearToSrgb( double v )
{
	if( v <= 0.0031308 )
		return v * 12.92;
	return 1.055 * std::pow( v, 1.0 / 2.4 ) - 0.055;
}

/// A light level from shares that are exactly 0 or 1 per region, or from
/// shares rounded once to float: 1 - sum of three products, six roundings.
constexpr double kColourBound = 6.0 * kFloatUlp;
constexpr double kColourTol   = 3.0 * kColourBound;

//---------------------------------------------------------------------------
// PNG writer. zlib ships with the OS. Rows BOTTOM-UP in, top-down out.
//---------------------------------------------------------------------------
using Image  = std::vector< unsigned char >;
using ImageF = std::vector< float >;

void putU32( std::vector< unsigned char >& out, uint32_t value )
{
	out.push_back( static_cast< unsigned char >( value >> 24 ) );
	out.push_back( static_cast< unsigned char >( value >> 16 ) );
	out.push_back( static_cast< unsigned char >( value >> 8 ) );
	out.push_back( static_cast< unsigned char >( value ) );
}

void putChunk( std::vector< unsigned char >& out, const char* type, const std::vector< unsigned char >& data )
{
	putU32( out, static_cast< uint32_t >( data.size() ) );
	const size_t start = out.size();
	out.insert( out.end(), type, type + 4 );
	out.insert( out.end(), data.begin(), data.end() );
	uLong crc = crc32( 0L, Z_NULL, 0 );
	crc       = crc32( crc, out.data() + start, static_cast< uInt >( 4 + data.size() ) );
	putU32( out, static_cast< uint32_t >( crc ) );
}

bool writePng( const std::string& path, int width, int height, const Image& bottomUp )
{
	std::vector< unsigned char > raw;
	raw.reserve( static_cast< size_t >( height ) * ( 1 + static_cast< size_t >( width ) * 4 ) );
	for( int y = height - 1; y >= 0; --y )
	{
		raw.push_back( 0 );
		const unsigned char* row = bottomUp.data() + static_cast< size_t >( y ) * width * 4;
		raw.insert( raw.end(), row, row + static_cast< size_t >( width ) * 4 );
	}
	uLongf compressedSize = compressBound( static_cast< uLong >( raw.size() ) );
	std::vector< unsigned char > compressed( compressedSize );
	if( compress2( compressed.data(), &compressedSize, raw.data(), static_cast< uLong >( raw.size() ), 6 ) != Z_OK )
		return false;
	compressed.resize( compressedSize );

	std::vector< unsigned char > png = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
	std::vector< unsigned char > ihdr;
	putU32( ihdr, static_cast< uint32_t >( width ) );
	putU32( ihdr, static_cast< uint32_t >( height ) );
	ihdr.push_back( 8 );
	ihdr.push_back( 6 );
	ihdr.push_back( 0 );
	ihdr.push_back( 0 );
	ihdr.push_back( 0 );
	putChunk( png, "IHDR", ihdr );
	putChunk( png, "IDAT", compressed );
	putChunk( png, "IEND", {} );

	FILE* file = fopen( path.c_str(), "wb" );
	if( file == nullptr )
		return false;
	const size_t written = fwrite( png.data(), 1, png.size(), file );
	fclose( file );
	return written == png.size();
}

//---------------------------------------------------------------------------
// Pictures. Bottom-up, GL's orientation: row 0 is the BOTTOM. RGBA,
// premultiplied (the fleet's assumption about what a host hands a mixer).
//---------------------------------------------------------------------------
struct Rgb
{
	unsigned char r, g, b;
};

unsigned char toByte( double v )
{
	return static_cast< unsigned char >( std::lround( std::clamp( v, 0.0, 1.0 ) * 255.0 ) );
}

constexpr Rgb kBlack    = { 0, 0, 0 };
constexpr Rgb kWhite    = { 255, 255, 255 };
constexpr Rgb kSentinel = { 0xFF, 0x00, 0xFF };///< padding: a colour that appears nowhere else
constexpr int kSentinelNear = 135;

size_t at4( int width, int x, int y )
{
	return ( static_cast< size_t >( y ) * width + x ) * 4;
}

void put( Image& image, int width, int x, int y, Rgb c, unsigned char a = 255 )
{
	const size_t at = at4( width, x, y );
	image[ at + 0 ] = c.r;
	image[ at + 1 ] = c.g;
	image[ at + 2 ] = c.b;
	image[ at + 3 ] = a;
}

Rgb pixelAt( const Image& image, int width, int x, int y )
{
	const size_t at = at4( width, x, y );
	return { image[ at + 0 ], image[ at + 1 ], image[ at + 2 ] };
}

int l1( Rgb a, Rgb b )
{
	return std::abs( a.r - b.r ) + std::abs( a.g - b.g ) + std::abs( a.b - b.b );
}

Image flatField( int width, int height, Rgb c, unsigned char alpha = 255 )
{
	Image image( static_cast< size_t >( width ) * height * 4 );
	for( int y = 0; y < height; ++y )
		for( int x = 0; x < width; ++x )
			put( image, width, x, y, c, alpha );
	return image;
}

void hsv( double h, double s, double v, double& r, double& g, double& b )
{
	h           = h - std::floor( h );
	const int i = static_cast< int >( h * 6.0 ) % 6;
	const double f = h * 6.0 - std::floor( h * 6.0 );
	const double p = v * ( 1 - s ), q = v * ( 1 - f * s ), t = v * ( 1 - ( 1 - f ) * s );
	switch( i )
	{
	case 0: r = v, g = t, b = p; break;
	case 1: r = q, g = v, b = p; break;
	case 2: r = p, g = v, b = t; break;
	case 3: r = p, g = q, b = v; break;
	case 4: r = t, g = p, b = v; break;
	default: r = v, g = p, b = q; break;
	}
}

/// The screen: a clip, light enough that a shadow on it reads. Hue across,
/// a soft brightness wave down, a grid of thin lines.
Image screenCard( int width, int height, bool withAlpha = false )
{
	Image image( static_cast< size_t >( width ) * height * 4 );
	for( int y = 0; y < height; ++y )
		for( int x = 0; x < width; ++x )
		{
			const double u = ( x + 0.5 ) / width, v = ( y + 0.5 ) / height;
			double r, g, b;
			hsv( u * 0.9, 0.35, 0.75 + 0.25 * std::cos( v * 6.0 ), r, g, b );
			const bool line = ( x % std::max( 8, width / 16 ) ) == 0 || ( y % std::max( 8, height / 9 ) ) == 0;
			if( line )
				r = g = b = 0.95;
			double a = 1.0;
			if( withAlpha )
			{
				//A band at half alpha and a hole, as the demo clips have.
				if( v > 0.62 && v < 0.82 )
					a = 0.5;
				if( u > 0.7 && u < 0.92 && v > 0.15 && v < 0.45 )
					a = 0.0;
			}
			put( image, width, x, y, { toByte( r * a ), toByte( g * a ), toByte( b * a ) }, toByte( a ) );
		}
	return image;
}

double capsule( double px, double py, double ax, double ay, double bx, double by, double r )
{
	const double vx = bx - ax, vy = by - ay;
	const double t  = std::clamp( ( ( px - ax ) * vx + ( py - ay ) * vy ) / ( vx * vx + vy * vy ), 0.0, 1.0 );
	const double dx = px - ( ax + t * vx ), dy = py - ( ay + t * vy );
	return std::sqrt( dx * dx + dy * dy ) - r;
}

/// How much of the pixel a standing figure covers, 0..1, in screen heights
/// from the frame's centre: a head, a body, two arms, two legs.
double figureCover( double x, double y )
{
	double d = std::hypot( x - 0.0, y - 0.22 ) - 0.075;
	d        = std::min( d, capsule( x, y, 0.0, 0.1, 0.0, -0.12, 0.085 ) );
	d        = std::min( d, capsule( x, y, -0.08, 0.08, -0.24, -0.06, 0.028 ) );
	d        = std::min( d, capsule( x, y, 0.08, 0.08, 0.22, 0.16, 0.028 ) );
	d        = std::min( d, capsule( x, y, -0.04, -0.14, -0.08, -0.46, 0.036 ) );
	d        = std::min( d, capsule( x, y, 0.04, -0.14, 0.09, -0.46, 0.036 ) );
	return d;
}

/// The occluder: a figure in alpha, its rgb a dark, premultiplied grey-blue
/// (so its luma is not its alpha, and the Occluder option shows).
Image figureCard( int width, int height, bool lumaMask = false )
{
	Image image( static_cast< size_t >( width ) * height * 4 );
	const double px = 1.0 / height;
	for( int y = 0; y < height; ++y )
		for( int x = 0; x < width; ++x )
		{
			const double sx = ( x + 0.5 - 0.5 * width ) / height, sy = ( y + 0.5 - 0.5 * height ) / height;
			const double a  = std::clamp( 0.5 - figureCover( sx, sy ) / px, 0.0, 1.0 );
			if( lumaMask )
				put( image, width, x, y, { toByte( a ), toByte( a ), toByte( a ) }, 255 );
			else
				put( image, width, x, y, { toByte( 0.15 * a ), toByte( 0.2 * a ), toByte( 0.3 * a ) }, toByte( a ) );
		}
	return image;
}

/// Four flat quadrants and a marker, for --mixer.
struct QuadCard
{
	Image image;
	Rgb quadrant[ 4 ];
};

QuadCard quadCard( int width, int height )
{
	QuadCard c;
	c.quadrant[ 0 ] = { 0xC0, 0x30, 0x30 };
	c.quadrant[ 1 ] = { 0x30, 0xC0, 0x30 };
	c.quadrant[ 2 ] = { 0x30, 0x30, 0xC0 };
	c.quadrant[ 3 ] = { 0xC0, 0xC0, 0x30 };
	c.image         = Image( static_cast< size_t >( width ) * height * 4 );
	for( int y = 0; y < height; ++y )
		for( int x = 0; x < width; ++x )
			put( c.image, width, x, y, c.quadrant[ ( y >= height / 2 ? 2 : 0 ) + ( x >= width / 2 ? 1 : 0 ) ] );
	return c;
}

/// An occluder made of rectangles on the texel grid, the shape the model
/// integrates exactly. [x0, x1) x [y0, y1), alpha a / 255.
struct Block
{
	int x0, y0, x1, y1;
	int a;
};

/// In alpha (rgb premultiplied grey, so Alpha and Luma read the same), or
/// as an opaque grey-on-black mask with no alpha (Luma's input).
Image blocksImage( int width, int height, const std::vector< Block >& blocks, bool opaqueMask = false )
{
	Image image = opaqueMask ? flatField( width, height, kBlack, 255 ) : flatField( width, height, kBlack, 0 );
	for( const Block& b : blocks )
		for( int y = std::max( 0, b.y0 ); y < std::min( height, b.y1 ); ++y )
			for( int x = std::max( 0, b.x0 ); x < std::min( width, b.x1 ); ++x )
			{
				const unsigned char a = static_cast< unsigned char >( b.a );
				put( image, width, x, y, { a, a, a }, opaqueMask ? 255 : a );
			}
	return image;
}

Image generate( const std::string& name, int width, int height )
{
	if( name == "screen" )
		return screenCard( width, height );
	if( name == "screen-alpha" )
		return screenCard( width, height, true );
	if( name == "figure" )
		return figureCard( width, height );
	if( name == "mask" )
		return figureCard( width, height, true );
	if( name == "quads" )
		return quadCard( width, height ).image;
	if( name == "black" )
		return flatField( width, height, kBlack );
	if( name == "white" )
		return flatField( width, height, kWhite );
	if( name == "clear" )
		return flatField( width, height, kBlack, 0 );
	return flatField( width, height, { 0x80, 0x80, 0x80 } );//"grey"
}

const char* const kCardNames = "screen | screen-alpha | figure | mask | quads | black | white | clear | grey";

bool isCard( const std::string& name )
{
	static const char* names[] = { "screen", "screen-alpha", "figure", "mask", "quads", "black", "white", "clear", "grey" };
	for( const char* n : names )
		if( name == n )
			return true;
	return false;
}

/// A reproducible pseudo-random float in [0, 1): PCG's output function over
/// a counter, so a card is the same on every run and every machine.
float hashUnit( uint32_t n )
{
	uint32_t state = n * 747796405u + 2891336453u;
	uint32_t word  = ( ( state >> ( ( state >> 28u ) + 4u ) ) ^ state ) * 277803737u;
	word           = ( word >> 22u ) ^ word;
	return static_cast< float >( word >> 8 ) / 16777216.0f;
}

/// Random premultiplied RGBA floats: rgb <= a, a anywhere in 0..1.
ImageF randomPremultiplied( int width, int height, uint32_t seed )
{
	ImageF image( static_cast< size_t >( width ) * height * 4 );
	for( size_t i = 0; i < image.size(); i += 4 )
	{
		const uint32_t n = seed * 7919u + static_cast< uint32_t >( i );
		const float a    = hashUnit( n + 3 );
		image[ i + 0 ]   = hashUnit( n + 0 ) * a;
		image[ i + 1 ]   = hashUnit( n + 1 ) * a;
		image[ i + 2 ]   = hashUnit( n + 2 ) * a;
		image[ i + 3 ]   = a;
	}
	return image;
}

//---------------------------------------------------------------------------
// GL plumbing.
//---------------------------------------------------------------------------
bool softwareRenderer()
{
	const char* renderer = std::getenv( "PUTEST_RENDERER" );
	return renderer != nullptr && std::strcmp( renderer, "software" ) == 0;
}

CGLContextObj createContext()
{
	const CGLPixelFormatAttribute accelerated[] = {
		kCGLPFAOpenGLProfile, static_cast< CGLPixelFormatAttribute >( kCGLOGLPVersion_GL4_Core ),
		kCGLPFAAccelerated,
		kCGLPFAColorSize, static_cast< CGLPixelFormatAttribute >( 24 ),
		kCGLPFAAlphaSize, static_cast< CGLPixelFormatAttribute >( 8 ),
		static_cast< CGLPixelFormatAttribute >( 0 )
	};
	const CGLPixelFormatAttribute software[] = {
		kCGLPFAOpenGLProfile, static_cast< CGLPixelFormatAttribute >( kCGLOGLPVersion_GL4_Core ),
		kCGLPFAColorSize, static_cast< CGLPixelFormatAttribute >( 24 ),
		kCGLPFAAlphaSize, static_cast< CGLPixelFormatAttribute >( 8 ),
		static_cast< CGLPixelFormatAttribute >( 0 )
	};
	//PUTEST_RENDERER=software asks for Apple's software renderer by id, on a
	//Mac that has a GPU: what a GPU-less runner falls back to.
	const CGLPixelFormatAttribute generic[] = {
		kCGLPFAOpenGLProfile, static_cast< CGLPixelFormatAttribute >( kCGLOGLPVersion_GL4_Core ),
		kCGLPFARendererID, static_cast< CGLPixelFormatAttribute >( kCGLRendererGenericFloatID ),
		kCGLPFAColorSize, static_cast< CGLPixelFormatAttribute >( 24 ),
		kCGLPFAAlphaSize, static_cast< CGLPixelFormatAttribute >( 8 ),
		static_cast< CGLPixelFormatAttribute >( 0 )
	};
	CGLPixelFormatObj format = nullptr;
	GLint formatCount        = 0;
	if( softwareRenderer() )
	{
		if( CGLChoosePixelFormat( generic, &format, &formatCount ) != kCGLNoError || format == nullptr )
			return nullptr;
		std::fprintf( stderr, "putest: PUTEST_RENDERER=software, Apple's software renderer\n" );
	}
	else if( CGLChoosePixelFormat( accelerated, &format, &formatCount ) != kCGLNoError || format == nullptr )
		if( CGLChoosePixelFormat( software, &format, &formatCount ) != kCGLNoError || format == nullptr )
			return nullptr;
	CGLContextObj context = nullptr;
	const CGLError error  = CGLCreateContext( format, nullptr, &context );
	CGLDestroyPixelFormat( format );
	if( error != kCGLNoError )
		return nullptr;
	CGLSetCurrentContext( context );
	return context;
}

GLuint makeTexture( int width, int height, bool floatFormat, const void* pixels )
{
	GLuint texture = 0;
	glGenTextures( 1, &texture );
	glBindTexture( GL_TEXTURE_2D, texture );
	if( floatFormat )
		glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA32F, width, height, 0, GL_RGBA, GL_FLOAT, pixels );
	else
		glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
	glBindTexture( GL_TEXTURE_2D, 0 );
	return texture;
}

struct InputSpec
{
	int usedW = 0, usedH = 0;
	int hwW = 0, hwH = 0;
	static InputSpec Exact( int w, int h )
	{
		return { w, h, w, h };
	}
	static InputSpec Padded( int w, int h, int hw, int hh )
	{
		return { w, h, hw, hh };
	}
};

/// Set a parameter by display name, in host units.
bool setByName( Penumbra& plugin, const std::string& name, float value )
{
	for( unsigned int i = 0; i < Penumbra::PT_ABOUT_FIRST; ++i )
	{
		const char* declared = plugin.GetParamName( i );
		if( declared != nullptr && name == declared )
		{
			plugin.SetFloatParameter( i, value );
			return true;
		}
	}
	std::fprintf( stderr, "no parameter called '%s'\n", name.c_str() );
	return false;
}

/// The output framebuffer, its texture, and reading it back.
struct Target
{
	int width = 0, height = 0;
	GLuint texture = 0, fbo = 0;

	void Init( int w, int h, bool floatOutput )
	{
		width   = w;
		height  = h;
		texture = makeTexture( w, h, floatOutput, nullptr );
		glGenFramebuffers( 1, &fbo );
		glBindFramebuffer( GL_FRAMEBUFFER, fbo );
		glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0 );
	}

	void Begin()
	{
		glBindFramebuffer( GL_FRAMEBUFFER, fbo );
		glViewport( 0, 0, width, height );
		glClearColor( 0.0f, 0.0f, 0.0f, 0.0f );
		glClear( GL_COLOR_BUFFER_BIT );
	}

	Image Pixels()
	{
		Image pixels( static_cast< size_t >( width ) * height * 4 );
		glBindFramebuffer( GL_FRAMEBUFFER, fbo );
		glPixelStorei( GL_PACK_ALIGNMENT, 1 );
		glReadPixels( 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data() );
		return pixels;
	}

	ImageF PixelsF()
	{
		ImageF pixels( static_cast< size_t >( width ) * height * 4 );
		glBindFramebuffer( GL_FRAMEBUFFER, fbo );
		glPixelStorei( GL_PACK_ALIGNMENT, 1 );
		glReadPixels( 0, 0, width, height, GL_RGBA, GL_FLOAT, pixels.data() );
		return pixels;
	}

	void Release()
	{
		if( fbo )
			glDeleteFramebuffers( 1, &fbo );
		if( texture )
			glDeleteTextures( 1, &texture );
		fbo = texture = 0;
	}
};

//---------------------------------------------------------------------------
// The mixer's rig: SW Penumbra, TWO input textures and an output, each at
// its own size. inputTextures[0] is Dest (the screen, `--input-a`), [1] is
// Src (the occluder, `--input-b`). Padding is the sentinel, opaque.
//---------------------------------------------------------------------------
struct MixRig
{
	Penumbra plugin;
	Target out;
	InputSpec destSpec, srcSpec;
	bool destFloat               = false;
	GLuint destTexture = 0, srcTexture = 0;
	FFGLTextureStruct destStruct = {}, srcStruct = {};
	FFGLTextureStruct* inputs[ 2 ] = { nullptr, nullptr };
	ProcessOpenGLStruct process    = {};
	bool ready                     = false;

	bool Init( int outW, int outH, InputSpec dest, InputSpec src, bool floatOutput = false, int fault = 0, const char* fragment = nullptr,
	           bool floatDest = false )
	{
		plugin.SetFaultForTest( fault );
		plugin.SetFragmentShaderForTest( fragment );
		FFGLViewportStruct viewport = {};
		viewport.width              = static_cast< FFUInt32 >( outW );
		viewport.height             = static_cast< FFUInt32 >( outH );
		if( plugin.InitGL( &viewport ) != FF_SUCCESS )
		{
			std::fprintf( stderr, "InitGL failed -- see the diagnostics log for the shader\n" );
			return false;
		}
		destFloat = floatDest;
		MakeInputs( dest, src );
		out.Init( outW, outH, floatOutput );
		process.numInputTextures = 2;
		process.inputTextures    = inputs;
		process.HostFBO          = out.fbo;
		ready                    = true;
		return true;
	}

	bool Init( int outW, int outH, bool floatOutput = false, int fault = 0, const char* fragment = nullptr, bool floatDest = false )
	{
		return Init( outW, outH, InputSpec::Exact( outW, outH ), InputSpec::Exact( outW, outH ), floatOutput, fault, fragment, floatDest );
	}

	void MakeInputs( InputSpec dest, InputSpec src )
	{
		if( destTexture )
			glDeleteTextures( 1, &destTexture );
		if( srcTexture )
			glDeleteTextures( 1, &srcTexture );
		destSpec = dest;
		srcSpec  = src;
		if( destFloat )
		{
			ImageF pad( static_cast< size_t >( dest.hwW ) * dest.hwH * 4 );
			for( size_t i = 0; i < pad.size(); i += 4 )
				pad[ i ] = pad[ i + 2 ] = pad[ i + 3 ] = 1.0f;
			destTexture = makeTexture( dest.hwW, dest.hwH, true, pad.data() );
		}
		else
			destTexture = makeTexture( dest.hwW, dest.hwH, false, flatField( dest.hwW, dest.hwH, kSentinel ).data() );
		srcTexture = makeTexture( src.hwW, src.hwH, false, flatField( src.hwW, src.hwH, kSentinel ).data() );
		auto fill  = []( FFGLTextureStruct& t, const InputSpec& s, GLuint handle ) {
            t.Width          = static_cast< FFUInt32 >( s.usedW );
            t.Height         = static_cast< FFUInt32 >( s.usedH );
            t.HardwareWidth  = static_cast< FFUInt32 >( s.hwW );
            t.HardwareHeight = static_cast< FFUInt32 >( s.hwH );
            t.Handle         = handle;
		};
		fill( destStruct, dest, destTexture );
		fill( srcStruct, src, srcTexture );
		inputs[ 0 ] = &destStruct;
		inputs[ 1 ] = &srcStruct;
	}

	/// A new output size, as a host's FF_RESIZE.
	void ResizeOutput( int outW, int outH, bool floatOutput = false )
	{
		FFGLViewportStruct viewport = {};
		viewport.width              = static_cast< FFUInt32 >( outW );
		viewport.height             = static_cast< FFUInt32 >( outH );
		plugin.Resize( &viewport );
		out.Release();
		out.Init( outW, outH, floatOutput );
		process.HostFBO = out.fbo;
	}

	void UploadDest( const Image& bottomUp )
	{
		glBindTexture( GL_TEXTURE_2D, destTexture );
		glTexSubImage2D( GL_TEXTURE_2D, 0, 0, 0, destSpec.usedW, destSpec.usedH, GL_RGBA, GL_UNSIGNED_BYTE, bottomUp.data() );
		glBindTexture( GL_TEXTURE_2D, 0 );
	}

	void UploadDestF( const ImageF& bottomUp )
	{
		glBindTexture( GL_TEXTURE_2D, destTexture );
		glTexSubImage2D( GL_TEXTURE_2D, 0, 0, 0, destSpec.usedW, destSpec.usedH, GL_RGBA, GL_FLOAT, bottomUp.data() );
		glBindTexture( GL_TEXTURE_2D, 0 );
	}

	void UploadSrc( const Image& bottomUp )
	{
		glBindTexture( GL_TEXTURE_2D, srcTexture );
		glTexSubImage2D( GL_TEXTURE_2D, 0, 0, 0, srcSpec.usedW, srcSpec.usedH, GL_RGBA, GL_UNSIGNED_BYTE, bottomUp.data() );
		glBindTexture( GL_TEXTURE_2D, 0 );
	}

	bool Set( const std::string& name, float value )
	{
		return setByName( plugin, name, value );
	}

	void SetIndex( unsigned id, float value )
	{
		plugin.SetFloatParameter( id, value );
	}

	bool Render()
	{
		out.Begin();
		return plugin.ProcessOpenGL( &process ) == FF_SUCCESS;
	}

	FFResult RenderBroken( int numInputs, int nullIndex, bool zeroSize = false, bool zeroViewport = false )
	{
		FFGLTextureStruct* saved[ 2 ]     = { inputs[ 0 ], inputs[ 1 ] };
		const FFGLTextureStruct savedDest = destStruct;
		if( nullIndex >= 0 && nullIndex < 2 )
			inputs[ nullIndex ] = nullptr;
		if( nullIndex == -2 )
			process.inputTextures = nullptr;
		if( zeroSize )
			destStruct.HardwareWidth = 0;
		FFGLViewportStruct viewport = {};
		if( zeroViewport )
			plugin.Resize( &viewport );
		process.numInputTextures = static_cast< FFUInt32 >( numInputs );
		out.Begin();
		const FFResult result = plugin.ProcessOpenGL( &process );
		if( zeroViewport )
		{
			viewport.width  = static_cast< FFUInt32 >( out.width );
			viewport.height = static_cast< FFUInt32 >( out.height );
			plugin.Resize( &viewport );
		}
		inputs[ 0 ]              = saved[ 0 ];
		inputs[ 1 ]              = saved[ 1 ];
		destStruct               = savedDest;
		process.inputTextures    = inputs;
		process.numInputTextures = 2;
		return result;
	}

	Image Pixels()
	{
		return out.Pixels();
	}

	ImageF PixelsF()
	{
		return out.PixelsF();
	}

	~MixRig()
	{
		if( !ready )
			return;
		plugin.DeInitGL();
		out.Release();
		glDeleteTextures( 1, &srcTexture );
		glDeleteTextures( 1, &destTexture );
	}
};

double channel( const ImageF& img, int width, int x, int y, int c )
{
	return img[ at4( width, x, y ) + static_cast< size_t >( c ) ];
}

//---------------------------------------------------------------------------
// The harness's own room: a 3D ray cast through the occluder's plane.
//---------------------------------------------------------------------------
struct RoomLamp
{
	double colour[ 3 ] = { 0.0, 0.0, 0.0 };///< as picked
	double x = 0.0, y = 0.0, size = 0.0;   ///< screen heights
};

struct Room
{
	int W = 0, H = 0;  ///< output pixels
	int mw = 0, mh = 0;///< occluder texels
	double D       = 7.3;///< the lamps' height, screen heights: it drops out, so any value will do
	double k       = 0.0;///< d / D
	double room    = 0.0;
	double opacity = 1.0;
	bool linear    = false;
	RoomLamp lamp[ kLamps ];
	std::vector< Block > blocks;

	/// Lamp i's square as the screen point at pixel coordinates ( px, py )
	/// sees it through the occluder's plane, in occluder texels.
	void boxAt( int i, double px, double py, double& x0, double& y0, double& x1, double& y1 ) const
	{
		const double P[ 3 ] = { ( px - 0.5 * W ) / H, ( py - 0.5 * H ) / H, 0.0 };
		const double C[ 3 ] = { 0.0, 0.0, D };
		const double d      = k * D;
		double tx[ 2 ], ty[ 2 ];
		for( int corner = 0; corner < 2; ++corner )
		{
			const double sgn    = corner == 0 ? -1.0 : 1.0;
			const double L[ 3 ] = { lamp[ i ].x + sgn * 0.5 * lamp[ i ].size, lamp[ i ].y + sgn * 0.5 * lamp[ i ].size, D };
			//The segment from P to the lamp's corner, where it crosses z = d.
			const double t      = ( d - P[ 2 ] ) / ( L[ 2 ] - P[ 2 ] );
			const double X[ 3 ] = { P[ 0 ] + ( L[ 0 ] - P[ 0 ] ) * t, P[ 1 ] + ( L[ 1 ] - P[ 1 ] ) * t, d };
			//Seen from C, that point lies in front of this one on the screen.
			const double s      = C[ 2 ] / ( C[ 2 ] - X[ 2 ] );
			const double M[ 2 ] = { C[ 0 ] + ( X[ 0 ] - C[ 0 ] ) * s, C[ 1 ] + ( X[ 1 ] - C[ 1 ] ) * s };
			//The occluder's picture fills the frame.
			tx[ corner ] = ( M[ 0 ] * H / W + 0.5 ) * mw;
			ty[ corner ] = ( M[ 1 ] + 0.5 ) * mh;
		}
		x0 = std::min( tx[ 0 ], tx[ 1 ] );
		x1 = std::max( tx[ 0 ], tx[ 1 ] );
		y0 = std::min( ty[ 0 ], ty[ 1 ] );
		y1 = std::max( ty[ 0 ], ty[ 1 ] );
	}

	/// The fraction of lamp i the point cannot see, before Opacity.
	double blocked( int i, double px, double py ) const
	{
		double x0, y0, x1, y1;
		boxAt( i, px, py, x0, y0, x1, y1 );
		const double area = ( x1 - x0 ) * ( y1 - y0 );
		double covered    = 0.0;
		if( x1 - x0 < 1e-9 && y1 - y0 < 1e-9 )
		{
			for( const Block& b : blocks )
				if( x0 >= b.x0 && x0 < b.x1 && y0 >= b.y0 && y0 < b.y1 )
					covered += b.a / 255.0;
			return covered;
		}
		for( const Block& b : blocks )
		{
			const double ox = std::min( x1, static_cast< double >( b.x1 ) ) - std::max( x0, static_cast< double >( b.x0 ) );
			const double oy = std::min( y1, static_cast< double >( b.y1 ) ) - std::max( y0, static_cast< double >( b.y0 ) );
			if( ox > 0.0 && oy > 0.0 )
				covered += b.a / 255.0 * ox * oy;
		}
		return covered / area;
	}

	/// How close the point's box edges come to a block edge, in texels: a
	/// point lamp right on a texel edge could round either way.
	double edgeMargin( int i, double px, double py ) const
	{
		double x0, y0, x1, y1;
		boxAt( i, px, py, x0, y0, x1, y1 );
		double m = 1e9;
		for( const Block& b : blocks )
			for( double e : { x0, x1 } )
				m = std::min( { m, std::fabs( e - b.x0 ), std::fabs( e - b.x1 ) } );
		for( const Block& b : blocks )
			for( double e : { y0, y1 } )
				m = std::min( { m, std::fabs( e - b.y0 ), std::fabs( e - b.y1 ) } );
		return m;
	}

	double linearOf( int i, int c ) const
	{
		const double picked = std::clamp( lamp[ i ].colour[ c ], 0.0, 1.0 );
		return linear ? srgbToLinear( picked ) : picked;
	}

	/// The light on the screen in channel c, as a fraction of the unblocked
	/// screen's: the sum of the lamps it can see, over the sum of them all.
	double light( int c, double px, double py ) const
	{
		double total = room, seen = room;
		for( int i = 0; i < kLamps; ++i )
		{
			const double l = linearOf( i, c );
			total += l;
			if( l > 0.0 )
				seen += l * ( 1.0 - opacity * blocked( i, px, py ) );
		}
		return total > 0.0 ? seen / total : 1.0;
	}

	/// Push this room's settings into the plugin, as a host would, and read
	/// back the physical values the slider floats really stand for.
	void Apply( MixRig& rig )
	{
		Penumbra& p = rig.plugin;
		p.SetFloatParameter( Penumbra::PT_LINEAR_LIGHT, linear ? 1.0f : 0.0f );
		p.SetFloatParameter( Penumbra::PT_ROOM_LIGHT, ParamForRoom( room ) );
		p.SetFloatParameter( Penumbra::PT_OPACITY, static_cast< float >( opacity ) );
		p.SetFloatParameter( Penumbra::PT_DISTANCE, ParamForDistance( k ) );
		for( int i = 0; i < kLamps; ++i )
		{
			p.SetFloatParameter( Penumbra::LampParam( i, Penumbra::LAMP_RED ), static_cast< float >( lamp[ i ].colour[ 0 ] ) );
			p.SetFloatParameter( Penumbra::LampParam( i, Penumbra::LAMP_GREEN ), static_cast< float >( lamp[ i ].colour[ 1 ] ) );
			p.SetFloatParameter( Penumbra::LampParam( i, Penumbra::LAMP_BLUE ), static_cast< float >( lamp[ i ].colour[ 2 ] ) );
			p.SetFloatParameter( Penumbra::LampParam( i, Penumbra::LAMP_X ), ParamForLampX( lamp[ i ].x ) );
			p.SetFloatParameter( Penumbra::LampParam( i, Penumbra::LAMP_Y ), ParamForLampY( lamp[ i ].y ) );
			p.SetFloatParameter( Penumbra::LampParam( i, Penumbra::LAMP_SIZE ), ParamForLampSize( lamp[ i ].size ) );
		}
		const Asked a = p.AskedForTest();
		room          = a.room;
		opacity       = a.opacity;
		k             = a.k;
		for( int i = 0; i < kLamps; ++i )
		{
			for( int c = 0; c < 3; ++c )
				lamp[ i ].colour[ c ] = a.lamp[ i ].colour[ c ];
			lamp[ i ].x    = a.lamp[ i ].x;
			lamp[ i ].y    = a.lamp[ i ].y;
			lamp[ i ].size = a.lamp[ i ].size;
		}
	}

	/// The coordinate bound: four float ULPs of the largest texel coordinate
	/// the shader forms for lamp i (the pixel's centre scaled, plus the
	/// offset, plus and minus the half-size: each rounded once or twice).
	double coordinateBound( int i ) const
	{
		double x0, y0, x1, y1, m = 0.0;
		for( double px : { 0.0, static_cast< double >( W ) } )
			for( double py : { 0.0, static_cast< double >( H ) } )
			{
				boxAt( i, px, py, x0, y0, x1, y1 );
				m = std::max( { m, std::fabs( x0 ), std::fabs( x1 ), std::fabs( y0 ), std::fabs( y1 ) } );
			}
		return 4.0 * ulpAt( m );
	}

	/// The light-level bound for lamp i's box: each of the box's four edges
	/// off by the coordinate bound e moves the covered area by at most e
	/// times the other side, so the mean by e / len; the divisor len x len
	/// by 2e / len each way; then the nine-term sum and the division (16
	/// roundings), and 1 - sum of shares (8). A point lamp has no edges to
	/// round: its pixels near a texel edge are skipped instead.
	double levelBound( int i ) const
	{
		double x0, y0, x1, y1;
		boxAt( i, 0.5 * W, 0.5 * H, x0, y0, x1, y1 );
		const double len = std::min( x1 - x0, y1 - y0 );
		const double e   = coordinateBound( i );
		return ( len < 1.0 / 256.0 ? 0.0 : 8.0 * e / len ) + 24.0 * kFloatUlp;
	}
};

/// The default triangle of lamps, at a given size, with given colours.
void triangle( Room& r, double size, const double colours[ kLamps ][ 3 ] )
{
	for( int i = 0; i < kLamps; ++i )
	{
		r.lamp[ i ].x    = kLampDefaults[ i ].x;
		r.lamp[ i ].y    = kLampDefaults[ i ].y;
		r.lamp[ i ].size = size;
		for( int c = 0; c < 3; ++c )
			r.lamp[ i ].colour[ c ] = colours[ i ][ c ];
	}
}

constexpr double kPrimaries[ kLamps ][ 3 ] = { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } };

/// View = Light, Linear Light as the room says, Picture otherwise: what the
/// physics checks read.
void showLight( MixRig& rig, bool on )
{
	rig.plugin.SetFloatParameter( Penumbra::PT_VIEW, static_cast< float >( on ? ViewKind::Light : ViewKind::Picture ) );
}

size_t differingBytes( const Image& a, const Image& b )
{
	if( a.size() != b.size() )
		return std::max( a.size(), b.size() );
	size_t n = 0;
	for( size_t i = 0; i < a.size(); ++i )
		n += a[ i ] != b[ i ];
	return n;
}

size_t differingFloats( const ImageF& a, const ImageF& b )
{
	if( a.size() != b.size() )
		return std::max( a.size(), b.size() );
	size_t n = 0;
	for( size_t i = 0; i < a.size(); ++i )
		n += std::memcmp( &a[ i ], &b[ i ], sizeof( float ) ) != 0;
	return n;
}

} // namespace
