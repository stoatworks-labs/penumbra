#include "Shaders.h"

namespace penumbra
{

const char* const kVersion = R"(#version 410 core
)";

//---------------------------------------------------------------------------
// One vertex shader for every pass. It passes nothing but the position:
// every pass works from the fragment's integer position.
//---------------------------------------------------------------------------
const char* const kVertex = R"(
layout( location = 0 ) in vec4 vPosition;

void main()
{
	gl_Position = vPosition;
}
)";

//---------------------------------------------------------------------------
// The sRGB pair, IEC 61966-2-1:1999 (amended 2003). mix() with a boolean
// selector picks a component without arithmetic, so pow() of a value outside
// its branch (pow of a negative is undefined) never reaches the result
// (GLSL 4.10 8.3, mix with genBType).
//---------------------------------------------------------------------------
const char* const kCommon = R"(
vec3 decode( vec3 c )
{
	return mix( c / 12.92, pow( ( c + 0.055 ) / 1.055, vec3( 2.4 ) ), greaterThan( c, vec3( 0.04045 ) ) );
}

vec3 encode( vec3 x )
{
	return mix( x * 12.92, 1.055 * pow( x, vec3( 1.0 / 2.4 ) ) - 0.055, greaterThan( x, vec3( 0.0031308 ) ) );
}
)";

//---------------------------------------------------------------------------
// 1. Convert: Src's occluder, as integers, with a leading row and column of
// zeros so the prefix sums are exclusive.
//---------------------------------------------------------------------------
const char* const kConvertFragment = R"(
//inputTextures[1] -- Src, THIS layer: the occluder.
uniform sampler2D Occluder;
uniform int Luma;   //1: BT.709 luma of the (premultiplied) rgb; 0: alpha
uniform int Levels; //Q: the occluder's full value

out uint Level;

void main()
{
	ivec2 p = ivec2( floor( gl_FragCoord.xy ) );
	if( p.x == 0 || p.y == 0 )
	{
		Level = 0u;
		return;
	}
	vec4 t  = texelFetch( Occluder, p - ivec2( 1 ), 0 );
	//ITU-R BT.709-6 item 3.2, on the gamma-encoded values as received.
	float o = Luma != 0 ? dot( t.rgb, vec3( 0.2126, 0.7152, 0.0722 ) ) : t.a;
	Level   = uint( floor( clamp( o, 0.0, 1.0 ) * float( Levels ) + 0.5 ) );
}
)";

//---------------------------------------------------------------------------
// 2. Prefix: one radix-16 pass of the summed-area table, along one axis.
//---------------------------------------------------------------------------
const char* const kPrefixFragment = R"(
uniform usampler2D Table;
uniform ivec2 Step; //( s, 0 ) or ( 0, s )
uniform int Taps;   //16, or fewer on the last pass

out uint Sum;

void main()
{
	ivec2 p  = ivec2( floor( gl_FragCoord.xy ) );
	uint sum = 0u;
	for( int t = 0; t < Taps; ++t )
	{
		ivec2 q = p - Step * t;
		if( q.x < 0 || q.y < 0 )
			break;
		sum += texelFetch( Table, q, 0 ).r;
	}
	Sum = sum;
}
)";

//---------------------------------------------------------------------------
// 3. Screen: the light each pixel gets from the three lamps, and the clip
// times it.
//---------------------------------------------------------------------------
const char* const kScreenFragment = R"(
//inputTextures[0] -- Dest, the layer BELOW: the clip, printed on the screen.
uniform sampler2D Screen;
//The occluder's exclusive summed-area table, ( TableSize + 1 ) texels.
uniform usampler2D Table;

uniform vec2 MaxUVScreen;
uniform vec2 HalfTexelScreen;
uniform int ScreenExact;   //1: Dest's used size is the output's, so it is fetched, not sampled
uniform ivec2 OutSize;
uniform ivec2 TableSize;   //occluder texels the table covers
uniform vec2 Scale;        //occluder texels per output pixel
uniform vec2 Offset[ 3 ];  //each lamp's box centre minus the pixel's, texels
uniform vec2 HalfSize[ 3 ];//each lamp's box half-size, texels
uniform vec3 Share[ 3 ];   //each lamp's share of the unblocked light, per channel
uniform vec3 Unblocked;    //the light where nothing is blocked: 1, white-balanced
uniform float Opacity;     //the occluder's opacity: the layer fader
uniform int Levels;        //Q: the occluder's full value in the table
uniform int Linear;
uniform int OnWhite;
uniform int ShowLight;

//The harness's negative controls. 0 in the shipped plugin.
uniform int Fault;

out vec4 fragColor;

const int kFaultFoldedMaxUV  = 1;
const int kFaultReencode     = 16;
const int kFaultProduct      = 64;
const int kFaultHalfTexel    = 1024;
const int kFaultAlphaFromSrc = 2048;
const int kFaultStraight     = 4096;
const int kFaultCrossfade    = 16384;

bool faulty( int bit )
{
	return ( Fault & bit ) != 0;
}

//A box narrower than this, in texels, is a point: the texel under its
//centre. At 4K a coordinate's float ULP is 2^-11 texel, so a box this
//narrow has edges a few ULPs apart and no width worth dividing by.
const float kPointWidth = 1.0 / 256.0;

vec4 screenAt( ivec2 px )
{
	if( ScreenExact != 0 )
		return texelFetch( Screen, px, 0 );
	vec2 p = ( vec2( px ) + 0.5 ) / vec2( OutSize );
	p      = clamp( p, HalfTexelScreen, vec2( 1.0 ) - HalfTexelScreen ) * MaxUVScreen;
	if( faulty( kFaultFoldedMaxUV ) )
		p *= MaxUVScreen;
	return texture( Screen, p );
}

//One axis of the box [ c - h, c + h ] on n texels: the four table indices
//its three runs of whole and part texels start and end at, and each run's
//weight -- the length of the box over each texel of it. A box hanging off
//the frame is cut there (no occluder lies outside it) but keeps its length
//as the divisor. False: the box misses the occluder entirely.
bool axisOf( float c, float h, int n, out ivec4 at, out vec3 w, out float len, out bool inside )
{
	float lo = c - h;
	float hi = c + h;
	if( hi - lo < kPointWidth )
	{
		int i  = int( floor( c ) );
		at     = ivec4( i, i + 1, i, i + 1 );
		w      = vec3( 1.0, 0.0, 0.0 );
		len    = 1.0;
		inside = true;
		return i >= 0 && i < n;
	}
	float a = clamp( lo, 0.0, float( n ) );
	float b = clamp( hi, 0.0, float( n ) );
	int i0  = min( int( floor( a ) ), n - 1 );
	int i1  = min( int( floor( b ) ), n - 1 );
	at      = ivec4( i0, i0 + 1, i1, i1 + 1 );
	w       = i0 == i1 ? vec3( b - a, 0.0, 0.0 ) : vec3( float( i0 + 1 ) - a, 1.0, b - float( i1 ) );
	len     = hi - lo;
	inside  = lo >= 0.0 && hi <= float( n );
	return b > a;
}

//The mean of the occluder over a box, 0..1: the fraction of a lamp the
//pixel cannot see.
float occluded( vec2 c, vec2 h )
{
	ivec4 ax, ay;
	vec3 wx, wy;
	float lx, ly;
	bool inX, inY;
	if( !axisOf( c.x, h.x, TableSize.x, ax, wx, lx, inX ) || !axisOf( c.y, h.y, TableSize.y, ay, wy, ly, inY ) )
		return 0.0;
	uint t[ 16 ];
	for( int j = 0; j < 4; ++j )
		for( int i = 0; i < 4; ++i )
			t[ j * 4 + i ] = texelFetch( Table, ivec2( ax[ i ], ay[ j ] ), 0 ).r;
	//Every texel the box touches, as one integer: nothing there, or the box
	//wholly on the frame and every texel under it fully opaque. Both exact.
	uint whole = t[ 15 ] - t[ 12 ] - t[ 3 ] + t[ 0 ];
	if( whole == 0u )
		return 0.0;
	uint count = uint( ( ax.w - ax.x ) * ( ay.w - ay.x ) );
	if( inX && inY && whole == uint( Levels ) * count )
		return 1.0;
	//The nine runs: each an exact integer box sum (unsigned arithmetic wraps,
	//GLSL 4.10 4.1.3), weighted by the box's length over its texels.
	float sum = 0.0;
	for( int j = 0; j < 3; ++j )
		for( int i = 0; i < 3; ++i )
		{
			uint run = t[ ( j + 1 ) * 4 + i + 1 ] - t[ ( j + 1 ) * 4 + i ] - t[ j * 4 + i + 1 ] + t[ j * 4 + i ];
			sum += wx[ i ] * wy[ j ] * float( run );
		}
	return sum / ( lx * ly * float( Levels ) );
}

void main()
{
	ivec2 px = ivec2( floor( gl_FragCoord.xy ) );
	vec4 d   = screenAt( px );

	//The light on the screen, per channel, as a fraction of the unblocked
	//screen's: 1 - sum of each lamp's share times the fraction of it blocked.
	vec3 shade    = vec3( 0.0 );
	vec3 kept     = vec3( 1.0 );
	float opacity = faulty( kFaultCrossfade ) ? 1.0 : Opacity;
	if( Opacity > 0.0 )
	{
		vec2 base = ( vec2( px ) + ( faulty( kFaultHalfTexel ) ? 0.0 : 0.5 ) ) * Scale;
		for( int i = 0; i < 3; ++i )
		{
			//A black lamp casts nothing.
			if( Share[ i ] == vec3( 0.0 ) )
				continue;
			float o = opacity * occluded( base + Offset[ i ], HalfSize[ i ] );
			shade += Share[ i ] * o;
			kept *= vec3( 1.0 ) - Share[ i ] * o;
		}
	}
	vec3 L = faulty( kFaultProduct ) ? kept : max( Unblocked - shade, vec3( 0.0 ) );

	if( ShowLight != 0 )
	{
		fragColor = vec4( Linear != 0 ? encode( L ) : L, 1.0 );
		return;
	}

	vec3 rgb = d.rgb;
	float a  = d.a;
	if( OnWhite != 0 )
	{
		//The clip printed on a white screen: premultiplied over white, in
		//gamma space as Arena composites (Porter & Duff 1984).
		rgb = d.rgb + ( 1.0 - d.a );
		a   = 1.0;
	}
	vec3 lit;
	if( Linear == 0 )
		lit = rgb * L;
	else if( faulty( kFaultStraight ) )
		lit = encode( decode( clamp( rgb, 0.0, 1.0 ) ) * L );
	else if( a > 0.0 )
		lit = encode( decode( clamp( rgb / a, 0.0, 1.0 ) ) * L ) * a;
	else
		lit = rgb;
	//A channel no shadow reaches is the input's own value, to the bit -- not
	//a decode and a re-encode of it.
	if( !faulty( kFaultReencode ) )
		lit = mix( lit, rgb, equal( L, vec3( 1.0 ) ) );
	if( faulty( kFaultCrossfade ) )
		lit = mix( rgb, lit, Opacity );
	//A shadow is less light on the screen, not less screen: the alpha is the
	//screen's.
	float alpha = faulty( kFaultAlphaFromSrc ) ? occluded( ( vec2( px ) + 0.5 ) * Scale, vec2( 0.0 ) ) : a;
	fragColor   = vec4( lit, alpha );
}
)";

std::string AssembleFragment( const char* pass )
{
	return std::string( kVersion ) + kCommon + pass;
}

std::string AssembleVertex()
{
	return std::string( kVersion ) + kVertex;
}

} // namespace penumbra
