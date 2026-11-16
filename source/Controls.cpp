#include "Controls.h"

#include <algorithm>
#include <cmath>

namespace penumbra
{
namespace
{
double unit( float value )
{
	return std::clamp( static_cast< double >( value ), 0.0, 1.0 );
}

float toParam( double p )
{
	return static_cast< float >( std::clamp( p, 0.0, 1.0 ) );
}
} // namespace

int OptionIndex( float value, int count )
{
	return std::clamp( static_cast< int >( std::lround( value ) ), 0, count - 1 );
}

double RoomFromParam( float value )
{
	return kRoomMax * unit( value );
}

float ParamForRoom( double room )
{
	return toParam( room / kRoomMax );
}

double DistanceFromParam( float value )
{
	return kDistanceMax * unit( value );
}

float ParamForDistance( double k )
{
	return toParam( k / kDistanceMax );
}

double LampXFromParam( float value )
{
	return kLampXMax * ( 2.0 * unit( value ) - 1.0 );
}

float ParamForLampX( double x )
{
	return toParam( 0.5 + 0.5 * x / kLampXMax );
}

double LampYFromParam( float value )
{
	return kLampYMax * ( 2.0 * unit( value ) - 1.0 );
}

float ParamForLampY( double y )
{
	return toParam( 0.5 + 0.5 * y / kLampYMax );
}

double LampSizeFromParam( float value )
{
	const double p = unit( value );
	return kLampSizeMax * p * p;
}

float ParamForLampSize( double size )
{
	return toParam( std::sqrt( std::max( size, 0.0 ) / kLampSizeMax ) );
}

double DecodeSrgb( double c )
{
	return c <= 0.04045 ? c / 12.92 : std::pow( ( c + 0.055 ) / 1.055, 2.4 );
}

double EncodeSrgb( double x )
{
	return x <= 0.0031308 ? 12.92 * x : 1.055 * std::pow( x, 1.0 / 2.4 ) - 0.055;
}

} // namespace penumbra
