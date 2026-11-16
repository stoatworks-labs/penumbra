#pragma once

#include "Controls.h"

/**
    The room, as numbers: what the shader is told, worked out in double once a
    frame from what the operator asked for.

    Three planes, parallel: the screen, the occluder at d, the lamps at D. The
    occluder's picture (Src) is its silhouette seen from a REFERENCE POINT C in
    the lamps' plane, projected onto the screen -- where the hub's camera
    stands, or, for an ordinary layer, where a single white point lamp would
    cast exactly that picture. Lamp positions are offsets from C. With
    k = d / D and g = k / ( 1 - k ):

    - a screen point P sees lamp i's point L' through the occluder's picture
      at P + ( L' - C ) g, so
    - lamp i's shadow is the picture translated by -L_i g, and
    - box-filtered by a square of side s_i g: the lamp's own shape, through
      similar triangles.

    The fraction of lamp i that P can see is one minus the mean of the
    occluder over that square: exactly, for planar source, occluder and
    receiver in parallel planes (Soler & Sillion 1998, section 3.1). D drops
    out; only k matters. See SPEC-penumbra.md and AGENTS.md.

    Each lamp is assumed to light the unblocked screen evenly (no falloff, no
    form factor), and the picture is white-balanced to the unblocked screen:
    per channel, each lamp's share is its linear colour over the sum of all
    of them and the room.
*/
namespace penumbra
{

struct LampAsked
{
	double colour[ 3 ] = { 0.0, 0.0, 0.0 };///< as picked: sRGB-encoded 0..1
	double x = 0.0, y = 0.0;              ///< screen heights from the reference point
	double size = 0.0;                    ///< the square's side, screen heights
};

struct Asked
{
	bool linear    = true;
	double room    = 0.0;
	double opacity = 1.0;
	double k       = kDistanceDefault;
	OccluderKind occluder = OccluderKind::Alpha;
	ScreenKind screen     = ScreenKind::AsIs;
	ViewKind view         = ViewKind::Picture;
	LampAsked lamp[ kLamps ];
};

/// What one frame's shader is given. Positions are in OCCLUDER texels: the
/// table covers tableW x tableH of them, stretched over the output frame.
struct Setup
{
	Asked asked;
	double g = 0.0;///< k / ( 1 - k )

	struct Lamp
	{
		double linear[ 3 ] = { 0.0, 0.0, 0.0 };///< the colour in linear light (or as picked, Linear Light off)
		double share[ 3 ]  = { 0.0, 0.0, 0.0 };///< of the unblocked light, per channel
		double offset[ 2 ] = { 0.0, 0.0 };     ///< the box's centre minus the pixel's, texels
		double half[ 2 ]   = { 0.0, 0.0 };     ///< the box's half-size, texels
		bool lit           = false;            ///< any share at all: a black lamp casts nothing
	} lamp[ kLamps ];

	double unblocked[ 3 ] = { 1.0, 1.0, 1.0 };///< the light where nothing is blocked: 1, white-balanced
	double total[ 3 ]     = { 0.0, 0.0, 0.0 };///< R + the lamps' linear colours, per channel

	int outW = 0, outH = 0;
	int tableW = 0, tableH = 0;      ///< occluder texels the table covers
	double scale[ 2 ] = { 0.0, 0.0 };///< occluder texels per output pixel
	unsigned levels   = 0;           ///< Q: the occluder's full value in the table
};

/// The occluder's full value Q in the integer table: 255 levels (the input is
/// 8-bit), or fewer when the whole table would not fit 32 bits. Every box sum
/// is then exact (GLSL 4.10 4.1.3: unsigned arithmetic wraps mod 2^32, and the
/// true value is below it). 255 through 4096 x 2160; 129 at 7680 x 4320.
unsigned LevelsFor( int tableW, int tableH );

/// The frame's numbers. `scaleW` x `scaleH` is the size the occluder is
/// mapped at -- the table's own, except under kFaultSharedScale.
Setup Plan( const Asked& asked, int outW, int outH, int tableW, int tableH, int scaleW, int scaleH, int fault );

} // namespace penumbra
