# AGENTS.md: Penumbra

Why the code is the way it is. `CLAUDE.md` is the command reference; the design is
`~/Projects/resolume/specs/SPEC-penumbra.md`, whose "As built" section lists where this repo
departs from it. Built 2026-11-16 from the spec, in one session, local only; public at
`stoatworks-labs/penumbra` since 2026-11-18.

## The idea

**Each point of the screen shows the sum of the lamps it can see.**

Three planes, parallel: the screen, the occluder at d, three square lamps at D. Src is the
occluder's silhouette **seen from a reference point C in the lamps' plane** (where the hub's
camera stands, or where one white point lamp would cast exactly that picture). Lamp i is at
offset Lᵢ from C, side sᵢ, in screen heights. With k = d/D and g = k/(1 − k):

- a screen point P sees lamp point L′ through the silhouette at P + (L′ − C)·g, so
- lamp i's shadow is the silhouette **translated by −Lᵢ·g**, and
- **box-filtered by a square of side sᵢ·g**, the lamp through similar triangles.

That is Soler and Sillion's §3.1 identity (SIGGRAPH 98, doi:10.1145/280814.280927, read on
2026-11-16): for source, occluder and receiver in parallel planes, visibility is the scaled
source convolved with the occluder. D drops out. `source/Model.cpp` turns that into texels in
double on the CPU, once a frame: the offset Lᵢ·g and half-size sᵢ·g/2 per axis, each times
`perHeight = { scale[0]·outH, tableH }` (occluder texels per screen height; the occluder is
stretched over the frame, so a square lamp is a rectangle in texels when Src's aspect isn't
the output's).

Per channel c, with each lamp's picked colour decoded to linear light and R the room light:
`total_c = R + Σ linᵢ,c`, `shareᵢ,c = linᵢ,c / total_c`, and the light is
`L_c = 1 − Σ shareᵢ,c · Opacity · oᵢ`, oᵢ the occluder's mean over lamp i's box. The picture
is white-balanced to the unblocked screen: L is exactly 1 where nothing is blocked. A channel
nothing lights has shares 0 and L = 1. A black lamp (all shares 0) is skipped.

**The table.** Three passes a frame (`source/Penumbra.cpp`):

1. **Convert**: Src's alpha (or BT.709 luma of its rgb as received) quantised to Q levels
   into an R32UI texture one texel bigger each way, with a leading row and column of zeros
   so the prefix sums are exclusive. Q = min(255, ⌊(2³² − 1)/(W·H)⌋): the whole table's sum
   fits 32 bits, so every box sum is exact under GLSL 4.10 §4.1.3's wrap-around. 255 through
   4096×2160; 129 at 7680×4320.
2. **Prefix**: radix-16 passes, along x then y, ping-ponging two buffers: after passes at
   strides 1, 16, 256, … each texel holds the sum of the 16ⁿ texels at and before it. A
   1080p table is 3 passes an axis.
3. **Screen**: per lamp, `axisOf` cuts the box [c − h, c + h] at the frame on each axis and
   returns four table indices (the part texel at each end and the run of whole texels
   between) and each run's weight. `occluded` fetches the 4×4 corners (16 `texelFetch`es)
   and forms nine exact integer box sums, weighted and summed in float, over the box's
   length × height × Q. Two branches are exact by construction: every texel the box touches
   summing to 0 returns 0; the box wholly on the frame and every texel under it at Q returns
   1. A box narrower than 1/256 texel is a point: the texel under its centre.

A box that hangs off the frame is cut there (no occluder lies outside it) but keeps its
**full length as the divisor**: the part of a lamp seen past the frame's edge is seen
through nothing.

**The output.** `Screen = As Is`: Dest is taken as premultiplied, un-premultiplied, clamped,
decoded, times L, encoded, premultiplied, alpha = Dest.a; a transparent texel passes through.
`On White`: rgb + (1 − a), alpha 1, then the same. `View = Light`: encode(L) (L itself with
Linear Light off), alpha 1. Then, per channel, `mix(lit, rgb, equal(L, 1))`: a channel no
shadow reaches is the input's value to the bit. When Dest's used size is the output's it is
`texelFetch`ed, otherwise sampled bilinearly inside its MaxUV with a half-texel clamp.

## Decisions taken without asking (the brief says decide and write it down)

- **Distance defaults to 0.35 (g 0.54), not the spec's 0.2.** Six of Resolume's demo clips
  with alpha were paired as occluders over the three brightest opaque clips through `putest
  --pipe`, and four of them over two screens at 0.2, 0.35 and 0.5, onto contact sheets. At
  0.2 a half-frame occluder throws one black hole with thin coloured rims and the effect
  reads as a black shadow. At 0.35 all seven colours show around a smaller core. At 0.5 a
  thin outline (Trinity_09's rings) blurs out. `Controls.h` says this beside the constant.
- **Lamp Size stays at the spec's 0.05**, the lamps in the spec's triangle, red, green and
  blue. At 0.35 a 0.05 lamp gives a 0.027-screen-height edge: soft enough to read as light,
  sharp enough to keep a hand's fingers.
- **Occluder defaults to Alpha.** Any layer with alpha works without the hub. The hub's mask
  format is the hub's to define; a white-on-black mask with no alpha needs Luma, and with
  Alpha it blocks everything (an opaque layer is a wall). Both are said in the README.
- **The output alpha is the screen's** (As Is), On White is opaque, Light is opaque. A shadow
  is less light, not less screen; the occluder's alpha is consumed, never output. Checked on
  partial-alpha Dest by `--alpha` and `--unchanged`.
- **Depth is out of scope for v0.1.0**, for the spec's three reasons: a mixer's two inputs are
  both spoken for and the hub's distance is a separate stream; an occluder that isn't planar
  isn't a convolution, and the closed forms the harness holds the plugin to hold only for a
  plane; one Distance (which the hub can drive over OSC) covers stepping towards the lamps.
  The alpha path is the only path, so it keeps working.
- **Q's bound is 2³² − 1, unsigned.** The table is R32UI and GLSL's unsigned arithmetic wraps
  mod 2³², so a box sum is exact whenever the true sum is below 2³²; 2³¹ − 1 (a signed
  bound) would halve Q at 4K for nothing.
- **A box off the frame keeps its length as the divisor**, and the exact umbra branch needs
  the box wholly on the frame (`inX && inY`): outside the frame is clear, so a box half off
  it can't be fully blocked. `--penumbra`'s corner blocks test both.
- **Index 0 is Linear Light, on.** Arena hides a mixer's first parameter (measured on five
  other mixers), so it must be a control whose default is right for ever.
- **Luma is taken on the rgb as received**, gamma-encoded and premultiplied. For a grey mask
  every weighting gives the mask, and an opaque mask is unaffected by premultiplication.
- **Light view encodes L**, so it reads as the light looks; with Linear Light off it is L.
- **Negative controls the spec didn't list**: `kFaultKeepTable` (the table never reallocated
  after the first frame) so `--resize` can fail. `kFaultReencode` is in `--unchanged` only:
  it changes only channels at L = 1, which `--linear` never measures.
- **The harness has its own sRGB pair** (`Harness.h`), never the plugin's `DecodeSrgb`: a
  model that borrows the plugin's arithmetic can't catch it.
- **`--bench` warms up for 60 frames.** With 5, the first measurement (720p, defaults) read
  slower than 720p's biggest boxes.

## The traps actually hit

- **`fmt()` takes doubles only.** `fmt("Linear Light %s …", linear)` segfaulted `--colour`
  (exit 139). Strings are concatenated outside it now.
- **Integer textures need NEAREST filters.** An integer texture whose minification filter is
  anything else, including the default NEAREST_MIPMAP_LINEAR, is incomplete (OpenGL 4.1 core
  §3.8.14) and fetches return nothing useful. `TableBuffer` sets both.
- **`FFGLFBO` can't make an R32UI texture**: it allocates with `GL_RGBA` and
  `GL_UNSIGNED_BYTE`/`GL_FLOAT`, and an integer format needs `GL_RED_INTEGER` with
  `GL_UNSIGNED_INT` (GL_INVALID_OPERATION otherwise). Hence `source/TableBuffer.*`.
- **`ScopedFBOBinding` doesn't restore the viewport.** The screen pass puts the host's
  viewport back by hand; `--mixer` checks it.
- **macOS make (GNU make 3.81) compares whole-second mtimes**, three times over:
  - mutate.sh restored a file within the second its object was built, the object looked up
    to date, and Lamp Size's mutant leaked into the next mutants' runs ("lamp 0.95"). It now
    deletes the object of every file it touches.
  - The first mutant wasn't caught because putest wasn't relinked. It now deletes putest
    before every build, and for GLSL mutants requires the mutant's text in the binary
    (`grep -qF`), or it reports a stale build.
  - A mutant applied by hand with sed to the shared tree and undone with `git checkout` left
    the mutated object in `build/`: the corner test failed by 0.5 and the shader was debugged
    for an hour before the mutant text turned up in the binary. Never hand-mutate a shared
    build; rebuild with `--clean-first` if you did.
- **A box-edge mutant needs a corner.** `lo >= 0.0` → `lo <= 0.0` (a box off the low edge
  taken as wholly on the frame) survived until the ray-cast scene got a block in the frame's
  corner, `{0, 0, mw/10, mh/10}`: a box cut on both low edges.
- **The parting tolerance carried an extra factor of the width.** Re-derived from the fit's
  mean (below); the corner-not-centre fault is now caught at k = 0.2 and 0.5.
- **The sweep needs a Dest with alpha for Screen.** On an opaque screen As Is and On White
  are the same picture.
- **Under `set -o pipefail`, `nm | grep -q` fails when it succeeds**: grep exits at the first
  match, nm takes SIGPIPE. verify.sh captures nm's output first.
- **The SDK's FFGLUtilities.cpp warns about RAND_MAX**; verify.sh's no-warnings check filters
  `external/ffgl` only.
- **oxbow is an effect host**: it hands a mixer one input, so `selftest` must fail on frame 0,
  and verify requires exactly that, plus `probe`'s inputs 2..2.
- **The shell.** zsh treats `=====` as a command (`=word` expansion); the contact-sheet
  scripts are bash. macOS has no `timeout` (perl's alarm) and no `cat -A` (`cat -vet`).
  Heredocs hang without a writable `TMPDIR`.

## Verified, and assumed

Verified on this Mac's GPU and Apple's software renderer, at 640×360 and 320×180, by
`putest` (numbers in the README's table): the primaries' shadows exact, other lamp sets to
4.2e-8; the penumbra widths equal to the closed form to the fourth decimal; the 3D ray cast
to 2.1e-6; the parting to 6.9e-6 px; nothing blocked bit-exact; the alpha bit-exact;
linear-light sums; Opacity; resizes; the guards and the padding; the host's GL state.

Assumed, and said so in the README:

- **Each lamp lights the screen evenly**: no falloff, no form factor. Soler and Sillion's
  eq. 1 separates visibility from the form factor as an approximation exact for a small lamp;
  here the form factor is taken as constant and only visibility varies.
- **Square lamps, square to the screen.** The box is what makes the table exact.
- **A flat occluder at one distance**, the silhouette as seen from the reference point.
- **Dest is premultiplied** (the fleet's assumption, from genlock's AGENTS.md).
- **Arena's behaviour** with mixers (index 0 hidden, `Opacity` on the fader, Dest and Src) is
  measured on genlock, wipe, relay, lenticular and pepper, not on this plugin.
- **The hub's mask format**: not defined yet.

## Tolerances, and where they come from

Every numeric tolerance is three times a stated bound. The bounds:

| quantity | bound | from |
| --- | --- | --- |
| a level from shares exactly 0 or 1, or rounded once (`kColourBound`) | 6·2⁻²⁴ (tolerance 1.1e-6) | 1 − a sum of three products: six roundings of values ≤ 1 |
| the sRGB pair (`kPowBound`) | 4e-6 absolute (tolerance 1.2e-5) | below |
| a texel coordinate (`coordinateBound`) | 4 ULPs of the largest coordinate a lamp's box reaches | the pixel centre scaled, plus the offset, plus or minus the half-size, each rounded once or twice |
| a light level over a box (`levelBound`) | 8e/len + 24·2⁻²⁴ | each of four edges off by e moves the mean by e/len and the divisor by 2e/len each way; the nine-term sum, the area and the division (16); 1 − Σ shares (8). A point lamp has no edges to round; its pixels near a texel edge are skipped instead |
| a ramp's width | 3·w²·slopeBound·level | a least-squares line through the ramp; a per-point error e moves its slope by slopeBound·e, slopeBound = Σ\|x − x̄\| / Σ(x − x̄)²; width = 1/slope |
| the ramp's straightness | 3·level·(2 + \|x − x̄\|·slopeBound) | the point's own e plus the fitted line's error there: e at the mean, slopeBound·e per pixel away from it |
| a shadow edge's half-light crossing | 3·level·w·(1 + reach·slopeBound) | the fit's mean off by e moves the crossing e·w; its slope off by slopeBound·e moves it \|crossing − x̄\|·slopeBound·e·w |
| the ray cast, every pixel | Σ over lamps of 3·levelBound | each lamp's level independently |
| bit-exact claims | 0 | a branch and a fetch, no arithmetic: `--unchanged`, `--alpha`'s alpha, `--resize`, the hard shadow in `--mixer` |

**The sRGB pair.** GLSL 4.10 §4.5.1: `+ − ×` correctly rounded, `/` 2.5 ULP, `pow(x, y)` as
`exp2(y·log2 x)`, `exp2` good to (3 + 2|x|) ULP, `log2` to 3 ULP outside [0.5, 2] and
2⁻²¹ absolute inside. Summing worst cases on a grid over 0..1 (2026-11-16): decode's relative
error is at most **5.1e-6**, at its knee, where exp2's argument (2.4·log2 0.0905 = −8.3) is
largest; encode's absolute error at most **6.8e-7**. Encode scales a relative error in its
input y by y·encode′(y) = 0.44·y^(1/2.4), only 0.04 at the knee, so
`encode(decode(rgb/a)·L)·a`, with the division and both multiplies, is good to **1.9e-6**
absolute everywhere. `kPowBound` is 4e-6; the tolerance three times it.

## Would this hold on another rasteriser, at another raster?

Every picture check runs at 640×360 and 320×180 on this Mac's GPU and, in verify, again on
Apple's software renderer (all ten GL checks, about a minute and a half).

- **The table is the same bits on every conforming implementation.** Positions come from
  `floor(gl_FragCoord.xy)`, integers, never interpolated uv; the occluder is read with
  `texelFetch` and summed in `uint`, whose wrap-around GLSL 4.10 §4.1.3 defines. R32UI is a
  required format (GL 4.1 §3.8.3).
- **The float work in a box is bounded in ULPs of its coordinates**, not fitted: converting
  nine integer sums (exact below 2²⁴, correctly rounded above), weighting them by edge
  lengths formed from float coordinates, dividing once. The tolerances scale with the raster
  through `coordinateBound` and `levelBound`, so 4K's larger coordinates get proportionally
  looser bounds automatically.
- `--colour`: the primaries are exact because each share is exactly 0 or 1 and each region's
  interior has o exactly 0 or 1 from the exact branches. Holds anywhere.
- `--penumbra`, `--parting`: fits over many pixels against the bounds above; both rasters,
  software renderer. Widths at 320×180 are half those at 640×360, as they must be.
- `--unchanged`, `--alpha`'s alpha, `--resize`: a branch, a fetch, a `mix` with a boolean
  selector (GLSL 4.10 §8.3: no arithmetic). True on any rasteriser; `--resize` needs only a
  deterministic one within one context.
- `--mixer`'s hard shadow is exact on the pixels whose centres map into the block, and the
  check asserts that no pixel centre falls within 1e-3 texel of a texel edge (nearest 0.014
  at 640×360), so a rasteriser's rounding of a centre can't flip one.
- `--linear`, `--opacity`, `--alpha`'s colour: the sRGB pair's derived budget.
- Not run: llvmpipe, other GPUs, Windows drivers.

## The negative controls and mutants

`SetFaultForTest` builds the wrong answer into the real plugin; 37 negatives in all, each
caught at both rasters (the counts are assertions failed at 640×360):

| check | the wrong answer | failed |
| --- | --- | --- |
| mixer (state) | the table left bound on unit 1 | 1 |
| mixer | Dest's MaxUV applied twice (genlock's trap) | 1 |
| mixer | the occluder mapped at Dest's size, not Src's | 2 |
| mixer | the table built over Src's hardware size: the padding occludes | 3 |
| mixer, parting | the pixel's corner, not its centre, mapped into the occluder | 1, 2 |
| unchanged | every channel decoded and re-encoded even at light 1 | 5 |
| unchanged, colour | the light not white-balanced | 2, 3 |
| colour | the shadows multiplied, Π(1 − wᵢoᵢ), not summed | 3 |
| colour, parting | the shadows thrown towards their lamps | 5, 2 |
| penumbra | the box s·k wide: the lamp scaled to the occluder's plane | 7 |
| penumbra | every lamp a point | 7 |
| alpha | the output alpha taken from the occluder | 2 |
| alpha | Dest decoded without un-premultiplying | 1 |
| linear | lamp colours used as picked, not decoded | 1 |
| opacity | Opacity as a gamma-space crossfade | 2 |
| resize | the table never reallocated after the first frame | 3 |
| mutation | one character of the screen shader (`i0 + 1` → `i0 - 1`) through the shader hook | 7 |

`tools/mutate.sh`: 29 one-character mutants, 29 caught, each in a temporary tree, arm64,
under a 300 s alarm (about 40 s in all). 21 are GLSL: the convert pass reading one texel
late and Luma/Alpha swapped; the prefix summing forwards; `axisOf`'s last part-texel weight,
its length `hi + lo`, and the off-frame test; `occluded`'s exact umbra branch returning 0, a
run's sign, and its area `lx + ly`; the pixel centre at 0.6; Opacity added, not multiplied; a
blocked lamp adding light; the Light view's encode inverted; Dest premultiplied twice; the
pass-through at L = 0; On White subtracting white and On White transparent; decode's knee at
0.14; encode's gamma 1/2.2; Dest's MaxUV divided; no shadow unless Opacity > 1. 8 are C++:
g = k/(1 + k); a lamp's vertical offset and a box's height at the horizontal scale; the lamps
subtracted from the total; a lamp decoded at gamma 2.5; Lamp Size's mapping unsquared; no
prefix pass at all; the last prefix pass two taps short.

## The sweep's honest list

`python3 tools/sweep.py` sweeps all 25 controls with pixels (the About block's four have
none) at 320×180 and finds every one live, none barely. `--bare` drops the per-control
contexts and finds one dead, for the reason its context exists:

- **Screen**: As Is and On White differ only where the clip is transparent, and the default
  `screen` card is opaque. Its context is the `screen-alpha` card.

## Demo clips and the stills

Frames at 2 s (`ffmpeg -ss 2`) from the clips in `/Applications/Resolume Arena/media`
(Shop74's and the AV set, DXV, 1280×720). Alpha at 2 s: BattleWeapon_Tank_09 61%
transparent, Ethnik2 51%, Metalive_01 68%, Galactucity_21 88%, Trinity_09 93%,
OrganicMotions_06 77%; IntoTheGlow, FogAndDust, NeonRoom, Beat, Bass and Synth opaque.
Most are dark: Bass, Synth and NeonRoom near black; the brightest opaque are
NoHopeJustFear_44 (mean red 130), Beat_001 (53) and IntoTheGlow_21 (43).

Judged by eye on contact sheets (18 pairs at the shipped defaults: the six occluders over
the three bright screens), not by a metric. The coloured shadows read in every pair. Around a
large occluder (the tank, Metalive_01, Galactucity_21) a black core remains with the six
colours ringing it; Trinity_09's thin rings give faint coloured outlines and no black at all;
on IntoTheGlow_21, the darkest of the three screens, everything is dimmer. A shadow on a dark
screen is dark (the light multiplies the clip); a strongly coloured screen shows only the
shadows of the lamps it reflects (on a red screen, only the red lamp's). The stills and
sheets are in the session's scratch directory, not the repo. The README's two images are
`putest --out … --input-a white --size 1280x720` and the Metalive_01-over-NoHopeJustFear_44
pair at the defaults.

## Render cost

`putest --bench`: wall-clock with `glFinish` either side, the mean of 30 frames after 60 of
warm-up, Dest and Src at the output size, verify's run on 2026-11-16 on an M4 Max shared with
other builds:

| | 720p | 1080p | 4K |
| --- | --- | --- | --- |
| defaults (Distance 0.35, lamps 0.05) | 0.204 ms | 0.407 ms | 2.379 ms |
| biggest boxes (Distance 0.9, lamps 0.5) | 0.205 | 0.412 | 2.394 |

1080p is 2.4% of a 60 fps frame. Box size costs nothing (16 fetches whatever the size). 4K is
5.8× 1080p for 4× the pixels; the table's passes are the obvious suspect (a 4K table is
3841×2161 R32UI, 33 MB, written seven times a frame), not measured. Earlier runs the same day
with a 5-frame warm-up and a load average near 11 gave 0.54/0.61/2.45 ms.

## Open questions

- **Depth, in v0.2**: `Distance From: Control / Occluder Red`, opt-in, once the hub defines
  how it packs distance into its mask.
- **The hub's mask: alpha or white on black?** If the hub sends no alpha, Luma should be the
  default when it's in use, or the hub should send alpha.
- **Dark clips.** Most demo clips are dark and shadows on them are faint. On White helps
  only where the clip is transparent; a gain or a "screen brightness" control would be a new
  idea, not physics.
- **Arena must confirm** it hides parameter 0 on this mixer too, and that `Opacity` lands on
  the layer fader.
- **Falloff and the form factor** (even wash assumed): a 1/cos³-style term per lamp would be
  cheap but breaks the "exactly 1 where unblocked" pass-through unless it is normalised.
- **Round lamps**: the same width, a different profile; a stepped disc costs k times the
  fetches.
- **Never in Resolume, never on Windows.**

## Generated copies

`source/StoatworksAbout.h` and `ATTRIBUTIONS.md` (and the README's attributions block) are
generated by stoatworks-backend's `sync-about.py` and `sync-attributions.py` since penumbra
was registered on 2026-11-18; edits belong there. sync-about added the User guide button, so
the plugin declares 30 parameters. `StoatworksAboutLinks.h` and `StoatworksAboutParams.h` are
the FFGL renderer, which is this repo's own (haze's shape). The issue forms are
`sync-issue-templates.py`'s plugin variant and `.github/FUNDING.yml` is `sync-funding.sh`'s.
