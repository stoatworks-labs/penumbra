# penumbra

Coloured shadows, for Resolume Arena/Avenue, as one FFGL **mixer**: three coloured area
lamps light a white screen with the clip printed on it, and someone stands between them and
the screen. Each screen point shows the sum of the lamps it can see (Soler & Sillion 1998
§3.1: for parallel planes, the visible part of a lamp is a convolution), evaluated exactly
by an integer summed-area table. C++/GLSL, CMake MODULE → universal `.bundle` (macOS) +
Windows `.dll` (never built). MIT.

| id | display name | bundle | type | inputs |
| --- | --- | --- | --- | --- |
| `PU01` | `SW Penumbra` | `Penumbra.bundle`, `com.stoatworks.ffgl.penumbra` | mixer | Dest = the layer below (the screen), Src = this layer (the occluder) |

Read `AGENTS.md` before touching the table's arithmetic (`source/Shaders.cpp`'s `axisOf`
and `occluded`), the lamps' shares (`source/Model.cpp`), the output alpha, index 0, or any
tolerance in `tools/putest/main.cpp`. Read genlock's AGENTS.md (mixer section) before
touching `ProcessOpenGL`'s inputs. The design is
`~/Projects/resolume/specs/SPEC-penumbra.md`; its "As built" section says where the code
departs from it.

## Commands (CMake)
- Configure: `cmake -B build -DCMAKE_BUILD_TYPE=Release`
- Fast dev build: add `-DCMAKE_OSX_ARCHITECTURES=arm64` (iteration builds here are arm64)
- Build: `cmake --build build -j8`
- Install to Resolume: `cmake --install build` (into Arena's Extra Effects). Not run on
  this machine: the plugin has never been loaded into Resolume.
- Render offline: `./build/putest --out /tmp/penumbra.png` (the `screen` card as Dest, the
  `figure` card's alpha as the occluder, on the defaults); `--input-a white` for the
  classic exhibit look
- Inputs: `--input-a NAME` (Dest) and `--input-b NAME` (Src), each one of `screen
  screen-alpha figure mask quads black white clear grey`; `--src-size WxH` for a Src of
  another size; `--size WxH` for the output
- List parameters: `./build/putest --list`
- Set anything by name: `./build/putest --set "Distance=0.5" --set "View=Light"` (a
  standard control takes the host's 0..1; an option takes its element name; anything
  unparsable, or a name that doesn't exist, exits 2)
- A clip through it: `ffmpeg -i screen.mov -f rawvideo -pix_fmt rgba - | ./build/putest
  --pipe --size WxH --pipe-src occluder.rgba | ffmpeg -f rawvideo -pix_fmt rgba -s WxH -i -
  out.mp4`. stdin is Dest, `--pipe-src` (a file or FIFO) is Src; without it, `--input-b`
  is held. The run ends with the shorter input; a partial last frame is dropped; a reader
  that hangs up ends it with exit 1 (SIGPIPE ignored).
- Cue sheets for `--pipe`: `--script cues`, `frame Name value` per line; standard and
  colour parameters ramp between keys, options step; a bad line exits 2.
- Render cost: `./build/putest --bench` (720p, 1080p and 4K, at the defaults and at the
  biggest boxes the controls allow).

## Verify
- Everything: `tools/verify.sh [BUILD_DIR]` (source greps, the four shaders through
  `glslc`, a fresh universal build in `build-verify` with no warnings outside the SDK, every
  check below on the GPU and again on Apple's software renderer, the pipe, the bundle
  through lipo, nm, plist, version and an ad-hoc signature, `oxbow probe` and `selftest`,
  29 mutants, the sweep, the bench): a few minutes on this Mac. It ends with one line,
  `verify: all N checks passed (M negative controls; logs in …)`, or the failures.
- **Set `TMPDIR`** to a writable scratch directory in a sandboxed Claude session, or the
  heredocs in verify.sh, glslc.sh and mutate.sh hang.
- **Set `OXBOW`** to a built `oxbow` binary. The default,
  `~/Projects/resolume/oxbow/build/oxbow`, isn't built on this Mac, and verify **fails**
  without one (it never skips). Build it out of tree: `cmake -S ~/Projects/resolume/oxbow
  -B <scratch>/oxbow-build && cmake --build <scratch>/oxbow-build` (never inside
  ~/Projects). oxbow is an effect host and hands a mixer one input, so its `selftest`
  must be refused on frame 0; verify requires that.
- **The checks** (GL, each at 640×360 and 320×180, each with its negative controls):
  `--mixer` (the guards; two inputs of two sizes with padding; the padding never casts a
  shadow; the host's GL state comes back), `--unchanged` (nothing blocked gives Dest to the
  bit), `--colour` (each shadow is exactly the sum of the lamps it can see), `--penumbra`
  (the ramp is s·d/(D − d) wide; umbra; corners; the 3D ray cast pixel by pixel),
  `--parting` (each shadow sits at the occluder minus Lᵢ·g), `--alpha`, `--linear`,
  `--opacity`, `--resize`, `--mutation` (one character of the shipped GLSL).
  `--names` needs no GL.
- `PUTEST_RENDERER=software` runs any of it on Apple's software renderer; verify runs all
  of them there.
- The checks can fail: `tools/mutate.sh` (29 one-character mutants of the GLSL and the C++,
  each built arm64-only in a temporary tree and run under a 300 s perl alarm; about 40 s).
- No dead controls: `python3 tools/sweep.py --build build` (`--bare` drops the per-control
  contexts and shows Screen dead without a clip that has alpha).

## Notes
- **Units.** Lengths are in screen heights: the output frame is one unit tall, centred on
  the reference point's foot. Lamps sit in a plane parallel to the screen; Distance is
  k = d/D, the occluder's fraction of the way from the screen to the lamps; g = k/(1 − k).
  D itself drops out.
- **The occluder is integers.** Src's alpha (or BT.709 luma) is quantised to Q levels
  (255, or fewer when the table would overflow 32 bits) into an R32UI texture with a leading
  row and column of zeros, then summed by radix-16 prefix passes along x and y. Box sums
  are exact because unsigned arithmetic wraps (GLSL 4.10 §4.1.3).
- **Integer textures need NEAREST filters** or they are incomplete (GL 4.1 §3.8.14);
  `FFGLFBO` can't make an R32UI texture at all, hence `source/TableBuffer.*`.
- **`ScopedFBOBinding` does not restore the viewport.** The screen pass sets the host's
  viewport back by hand.
- All host parameters are 0..1 and mapped in `source/Controls.cpp`; options hold the
  element index.
- **Index 0 is sacrificial**: Arena hides a mixer's first parameter, so it is Linear Light
  (on). Arena binds a parameter named `Opacity` to the layer fader.
- GLSL reserved words must not be identifiers; `verify.sh` greps for them.
- Override `SetTextParameter` to return `FF_SUCCESS` for the About block, or no host can
  instantiate the plugin.
- The macOS build must be universal. Check with `lipo`, never the build log.
- **macOS make compares whole-second mtimes.** A file restored within the same second as
  its object looks up to date. mutate.sh deletes the touched objects and the binary before
  every build; never hand-mutate a source file in a shared build directory.
- `putest`'s `fmt()` takes doubles only: never pass it a `%s` or `%d` format.
- **Generated, never edit here:** `source/StoatworksAbout.h`, `ATTRIBUTIONS.md` (and the
  README's attributions block), the issue forms and `.github/FUNDING.yml` come from
  stoatworks-backend's sync scripts (registered 2026-11-18). Edit them there and re-sync.
- **The workflows.** The repo is public (`stoatworks-labs/penumbra`, since 2026-11-18),
  which is what allows Actions. `ci.yml` builds on macOS and Windows on every push and pull
  request; `release.yml` (xenon's shape, with pepper's `tools/wincheck.ps1`) builds the
  universal bundle and the Windows DLL, loads the DLL as a host would, and on a `v*` tag
  publishes the zips, the dmg and the installer. A `workflow_dispatch` run builds without
  publishing. `scripts/release-lib.sh` is stoatworks-backend's `release/release-lib.sh`,
  verbatim.
- The fleet checkout is `~/Projects/resolume/penumbra`; work on it in a worktree.

## Not done yet
- Never loaded in Resolume (either platform); never built on Windows; the hub's mask has
  never been fed to it.
- No user guide, no OpenFX port, no browser demo, no presets.

## Diagnostics

`source/Diag.{h,cpp}`: log file only, no crash handler (this runs inside Resolume).

    ~/Library/Logs/penumbra/penumbra.YYYY-MM-DD.log
    %LOCALAPPDATA%\penumbra\logs\penumbra.YYYY-MM-DD.log

`PENUMBRA_LOG_DIR` overrides the folder. It logs the first frame's sizes, the table's size
and Q, each change of Opacity, up to 400 lines an instance (the only evidence a host gives of what drives a mixer's fader) and every guard
that refused a frame.
