#pragma once

#include <string>

/**
    The passes, as GLSL. Three of them, and the second runs several times.

    1. **Convert.** Src's used area, as integers: each texel's occluder value
       (its alpha, or its BT.709 luma) times Q, rounded, into an R32UI buffer
       one texel larger each way, with a row and a column of zeros at the
       start -- so that the prefix sums below come out EXCLUSIVE: T( i, j ) is
       the occluder summed over the texels left of column i and below row j.
       Read by texelFetch at integer positions, so Src's padding is never
       touched and no MaxUV is involved.
    2. **Prefix.** The summed-area table, in radix-16 passes: each pass adds
       16 taps `Step` apart, so after passes at strides 1, 16, 256, ... every
       texel holds the sum of the r^n texels at and before it. Along x, then
       along y. All in 32-bit unsigned integers, whose wrap-around is exact
       (GLSL 4.10 4.1.3), so the table is the same bits on every conforming
       implementation.
    3. **Screen.** Per output pixel and per lamp, the mean of the occluder
       over the lamp's box -- sixteen fetches of the table and nine integer
       box sums, weighted by the box's fractional edges -- then the light,
       1 - sum of share x occluded, and the clip times the light.

    Every position comes from `floor( gl_FragCoord.xy )`, an integer, never
    from an interpolated uv (wipe's CI lesson). The vertex shader passes
    nothing but the position.
*/

namespace penumbra
{

extern const char* const kVersion;
extern const char* const kVertex;
extern const char* const kCommon;
extern const char* const kConvertFragment;
extern const char* const kPrefixFragment;
extern const char* const kScreenFragment;

/// kVersion + kCommon + the pass, the order every fragment shader is built in.
std::string AssembleFragment( const char* pass );
std::string AssembleVertex();

/// The prefix passes' radix: taps per pass. Three passes cover 4096 texels.
inline constexpr int kRadix = 16;

/// The negative controls' bitmask, shared with the CPU side. The shipped
/// plugin always carries 0; only the harness sets any of these. The bits the
/// shader reads are repeated there as GLSL constants.
enum Fault : int
{
	kFaultNone          = 0,
	kFaultFoldedMaxUV   = 1 << 0, ///< shader: Dest's MaxUV applied twice (the genlock trap) -- --mixer
	kFaultSharedScale   = 1 << 1, ///< the occluder mapped at Dest's size, not Src's -- --mixer
	kFaultPaddedTable   = 1 << 2, ///< the table built over Src's hardware size: the padding occludes -- --mixer
	kFaultLeaveBound    = 1 << 3, ///< the table left bound on unit 1 -- --mixer
	kFaultReencode      = 1 << 4, ///< shader: every channel decoded and re-encoded, light 1 or not -- --unchanged
	kFaultUnbalanced    = 1 << 5, ///< raw light, R + sum colour x visibility, not white-balanced -- --unchanged, --colour
	kFaultProduct       = 1 << 6, ///< shader: the shadows multiplied, not summed -- --colour
	kFaultOccluderPlane = 1 << 7, ///< the box s k wide, the lamp scaled to the occluder plane -- --penumbra
	kFaultPointLamps    = 1 << 8, ///< every lamp a point -- --penumbra
	kFaultTowardLamp    = 1 << 9, ///< the shadow thrown towards its lamp -- --parting
	kFaultHalfTexel     = 1 << 10,///< shader: the pixel's corner, not its centre, mapped into the occluder -- --parting
	kFaultAlphaFromSrc  = 1 << 11,///< shader: out.a from the occluder -- --alpha
	kFaultStraight      = 1 << 12,///< shader: Dest decoded without un-premultiplying -- --alpha
	kFaultGammaShares   = 1 << 13,///< lamp colours used as picked, not decoded -- --linear
	kFaultCrossfade     = 1 << 14,///< shader: Opacity as a gamma-space crossfade -- --opacity
	kFaultKeepTable     = 1 << 15,///< the table never reallocated after the first frame -- --resize
};

} // namespace penumbra
