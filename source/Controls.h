#pragma once

/**
    Host parameters are 0..1; these are what they mean.

    Every ranged parameter this plugin declares is a plain FF_TYPE_STANDARD
    float in 0..1, including the ones that stand for a distance as a fraction
    of the way to the lamps, a position in screen heights or a lamp's size.
    That is not a style preference: `SetParamInfo` clamps a standard default
    into 0..1 *before* returning, and `SetParamRange` can only be called
    afterwards -- so a parameter declared in screen heights cannot declare a
    default in screen heights. The conversions live here, in one file the
    plugin and the harness both use, so there is only ever one answer to what
    a slider position means.

    **Lengths are in screen heights.** The screen is the output frame, one
    unit tall and W/H wide, centred on the reference point's foot. A lamp at
    X 0.45 stands 0.45 screen heights right of the reference point, in the
    lamps' plane.
*/
namespace penumbra
{

/// MSVC has no M_PI.
inline constexpr double kPi = 3.14159265358979323846;

inline constexpr int kLamps = 3;

/// Room Light: white light nobody blocks, in units of one full white lamp,
/// 0 to 1, linear. It is the floor under every shadow.
inline constexpr double kRoomMax = 1.0;

/// Distance: how far from the screen the occluder stands, as k = d / D, the
/// fraction of the way to the lamps' plane, 0 to 0.9, linear. Linear in k
/// because that is where the person stands; the shadows' parting and their
/// edges go as g = k / ( 1 - k ), 0 at the screen, 0.25 at 0.2, 9 at 0.9.
/// 0.9 is a person at a tenth of the way from the lamps: past it g runs off
/// to infinity, and every shadow is a smear the size of the frame.
///
/// The default, 0.35 (g 0.54), was chosen on Resolume's demo clips, paired
/// through `putest --pipe` (AGENTS.md, "Defaults"): at 0.2 a half-frame
/// occluder throws one black hole with thin coloured rims; at 0.35 all
/// seven coloured combinations show and the black core shrinks; at 0.5 a
/// thin outline (Trinity's rings) blurs out.
inline constexpr double kDistanceMax     = 0.9;
inline constexpr double kDistanceDefault = 0.35;

/// Lamp X and Y: a lamp's offset from the reference point in the lamps'
/// plane, X -2 to 2 and Y -1 to 1 screen heights, linear.
inline constexpr double kLampXMax = 2.0;
inline constexpr double kLampYMax = 1.0;

/// Lamp Size: a square lamp's side, 0 to 0.5 screen heights, as 0.5 p^2:
/// the small sizes, where a shadow's edge goes from hard to soft, get most
/// of the slider. 0 is a point lamp.
inline constexpr double kLampSizeMax     = 0.5;
inline constexpr double kLampSizeDefault = 0.05;

/// The three lamps' defaults: red, green and blue, in a triangle, so the
/// three shadows part in three directions.
struct LampDefault
{
	double r, g, b;
	double x, y;
};
inline constexpr LampDefault kLampDefaults[ kLamps ] = {
	{ 1.0, 0.0, 0.0, -0.45, -0.15 },
	{ 0.0, 1.0, 0.0, 0.0, 0.3 },
	{ 0.0, 0.0, 1.0, 0.45, -0.15 },
};

enum class OccluderKind : int
{
	Alpha,///< Src's alpha is the occluder: any layer, a keyed person, a clip with alpha
	Luma, ///< Src's BT.709 luma: a white-on-black mask that arrives without alpha (NDI)
	Count
};

enum class ScreenKind : int
{
	AsIs,   ///< the clip's own alpha: the screen is where the clip is
	OnWhite,///< the clip printed on a white screen: opaque, shadows fall on white where the clip is clear
	Count
};

enum class ViewKind : int
{
	Picture,///< the clip, lit
	Light,  ///< the light on the screen, as if the clip were white: for setting up
	Count
};

/// An option parameter's value is its element index, but its declared range
/// reads back 0..1 whatever the element count (the fleet's trap): map by
/// index, rounded and clamped.
int OptionIndex( float value, int count );

double RoomFromParam( float value );
float ParamForRoom( double room );

double DistanceFromParam( float value );///< k = d / D
float ParamForDistance( double k );

double LampXFromParam( float value );///< screen heights
float ParamForLampX( double x );
double LampYFromParam( float value );///< screen heights
float ParamForLampY( double y );
double LampSizeFromParam( float value );///< screen heights
float ParamForLampSize( double size );

/// IEC 61966-2-1:1999 (amended 2003), in double, for the CPU's half.
double DecodeSrgb( double c );
double EncodeSrgb( double x );

} // namespace penumbra
