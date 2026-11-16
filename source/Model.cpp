#include "Model.h"

#include "Shaders.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace penumbra
{

unsigned LevelsFor( int tableW, int tableH )
{
	const double texels = std::max( 1.0, static_cast< double >( tableW ) * static_cast< double >( tableH ) );
	const double fit    = std::floor( 4294967295.0 / texels );
	return static_cast< unsigned >( std::clamp( fit, 1.0, 255.0 ) );
}

Setup Plan( const Asked& asked, int outW, int outH, int tableW, int tableH, int scaleW, int scaleH, int fault )
{
	Setup s;
	s.asked  = asked;
	s.outW   = outW;
	s.outH   = outH;
	s.tableW = tableW;
	s.tableH = tableH;
	s.levels = LevelsFor( tableW, tableH );

	const double k = std::clamp( asked.k, 0.0, kDistanceMax );
	s.g            = k / ( 1.0 - k );

	//Occluder texels per output pixel, and per screen height: the occluder
	//is stretched over the frame, so a square lamp is a rectangle in texels
	//whenever the occluder's aspect is not the output's.
	s.scale[ 0 ]                = static_cast< double >( scaleW ) / outW;
	s.scale[ 1 ]                = static_cast< double >( scaleH ) / outH;
	const double perHeight[ 2 ] = { s.scale[ 0 ] * outH, static_cast< double >( scaleH ) };

	//The unblocked light, per channel, and each lamp's share of it.
	for( int c = 0; c < 3; ++c )
		s.total[ c ] = asked.room;
	for( int i = 0; i < kLamps; ++i )
		for( int c = 0; c < 3; ++c )
		{
			const double picked     = std::clamp( asked.lamp[ i ].colour[ c ], 0.0, 1.0 );
			const bool decode       = asked.linear && !( fault & kFaultGammaShares );
			s.lamp[ i ].linear[ c ] = decode ? DecodeSrgb( picked ) : picked;
			s.total[ c ] += s.lamp[ i ].linear[ c ];
		}
	for( int i = 0; i < kLamps; ++i )
	{
		Setup::Lamp& l = s.lamp[ i ];
		for( int c = 0; c < 3; ++c )
		{
			//A channel nothing lights has nothing to balance: its shares are 0
			//and its light stays exactly 1, the clip as it is.
			if( fault & kFaultUnbalanced )
				l.share[ c ] = l.linear[ c ];
			else
				l.share[ c ] = s.total[ c ] > 0.0 ? l.linear[ c ] / s.total[ c ] : 0.0;
			l.lit = l.lit || l.share[ c ] > 0.0;
		}

		//Lamp i reads the occluder's picture at P + L_i g: a lamp to the
		//right throws its shadow left.
		const double sign = ( fault & kFaultTowardLamp ) ? -1.0 : 1.0;
		l.offset[ 0 ]     = sign * asked.lamp[ i ].x * s.g * perHeight[ 0 ];
		l.offset[ 1 ]     = sign * asked.lamp[ i ].y * s.g * perHeight[ 1 ];

		//The lamp, of side s, seen through the occluder's plane from the
		//screen: a square of side s g on the picture (similar triangles).
		double side = asked.lamp[ i ].size * s.g;
		if( fault & kFaultOccluderPlane )
			side = asked.lamp[ i ].size * k;
		if( fault & kFaultPointLamps )
			side = 0.0;
		l.half[ 0 ] = 0.5 * side * perHeight[ 0 ];
		l.half[ 1 ] = 0.5 * side * perHeight[ 1 ];
	}

	if( fault & kFaultUnbalanced )
		for( int c = 0; c < 3; ++c )
			s.unblocked[ c ] = s.total[ c ];
	return s;
}

} // namespace penumbra
