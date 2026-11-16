# Attributions

Penumbra is built on other people's work. This file lists what that work is, who did
it, and what it is doing here.

It is a provisional hand copy: the master lists live in the `stoatworks-backend` repo and
are pushed out by `scripts/sync-attributions.py` once a project is registered there.
Penumbra is not registered yet, so this file was written by hand on 2026-11-16, adapted
from haze's, and will be replaced by the generated one.

## Code we derived from other people's work

Someone else solved this first, and this project would not exist in its current form without their work.

### The mixer's shape, the harness's rig, the pipe and the log — Stoatworks pepper

<https://github.com/stoatworks-labs/pepper>  
Licence: MIT  
Copyright: Stoatworks Labs

How an FFGL mixer takes its two inputs here is pepper's (and lenticular's, which shares it): the guards on the input count, the pointers and zero sizes, each input with its own MaxUV, index 0 as a sacrificial parameter, `Opacity` named so Arena binds the layer fader, and the per-instance Opacity log. The harness's two-input rig, its `--pipe`/`--pipe-src` mode (SIGPIPE ignored, a closed stdout exits 1), its cue sheets and the shape of tools/verify.sh are pepper's, adapted. source/Diag.{h,cpp} is pepper's, unchanged but for the name.

### What a mixer is in Arena — Stoatworks genlock

<https://github.com/stoatworks-labs/genlock>  
Licence: MIT  
Copyright: Stoatworks Labs

genlock's AGENTS.md is the measured account of how Resolume Arena drives an FFGL mixer (which input is Dest, the padding, the hidden first parameter, MaxUV folded into the vertex shader as the trap to avoid), and `putest --mixer`'s magenta-padding sentinel is genlock's test, adapted. No code beyond the test's shape.

### The tooling, the workflows and the About block — Stoatworks haze

Not yet published.  
Licence: MIT  
Copyright: Stoatworks Labs

tools/glslc.sh, tools/mutate.sh (one character at a time, each under perl's alarm), tools/sweep.py (with pepper's), the CI and release workflows, scripts/release-lib.sh (verbatim), and source/StoatworksAbout.h, StoatworksAboutLinks.h and StoatworksAboutParams.h (provisional hand copies, with `guide` empty: no user guide exists) are haze's, adapted.

### The GL state guard — Stoatworks vectrix, from resolume-scopes

<https://github.com/stoatworks-labs/vectrix>  
Licence: MIT  
Copyright: Stoatworks Labs

source/GLState.h is haze's, which is radar's, which is vectrix's, which lifted it from `resolume-scopes`' Scopes.cpp: the host's GL state captured and put back. `ScopedGLState` is penumbra's addition.

## Third-party code this project uses

Libraries, SDKs and frameworks the project is built on or bundles.

### Resolume FFGL SDK

<https://github.com/resolume/ffgl>  
Licence: BSD-3-Clause  
Copyright: FreeFrame

Vendored as a git submodule at external/ffgl.

The plugin ABI itself. An FFGL mixer is defined by this SDK's headers — there is no other way to be loadable by Resolume Arena and Avenue.

### GLEW — the OpenGL Extension Wrangler Library

<https://github.com/nigels-com/glew>  
Licence: BSD-3-Clause (with Mesa 3-D and Khronos components)  
Copyright: Milan Ikits, Marcelo E. Magallon and Lev Povalahev

Arrives inside the FFGL submodule at external/ffgl/deps/glew-2.1.0. Not fetched separately.

Resolves OpenGL entry points on Windows, where the system headers stop at OpenGL 1.1.

### libpng

<http://www.libpng.org/pub/png/libpng.html>  
Licence: PNG Reference Library License (libpng)  
Copyright: the PNG Reference Library authors

Arrives inside the FFGL submodule, under the SDK's CustomThumbnail sample.

Part of the upstream SDK tree rather than something this plugin calls directly — listed because it is present in the checkout.

## Work we checked ourselves against

No code was taken from these — but they were how we knew we had it right, and that is worth saying out loud.

### Soler and Sillion on soft shadows as a convolution

C. Soler and F. X. Sillion, "Fast calculation of soft shadow textures using convolution", SIGGRAPH 98, pp. 321–332, doi:10.1145/280814.280927, §3.1, read from the paper's text on 2026-11-16. For a light source, an occluder and a receiver in parallel planes, the part of the source a receiver point can see is the source's shape, scaled by α = d/(D − d), convolved with the occluder's. That identity is the whole plugin: a square lamp makes the convolution a box, and a summed-area table evaluates it exactly. `putest --penumbra` holds the picture to the similar-triangles width it implies, and to the harness's own 3D ray cast.

## Standards and published specifications

What the implementation is measured against.

- **OpenGL Shading Language 4.10** (revision 6, 24 July 2010) — §4.1.3: unsigned integer arithmetic wraps to the low 32 bits, so every box sum taken from the integer table is exact; §4.5.1: the precision of `+ − ×` (correctly rounded), division (2.5 ULP), `exp2`, `log2` and so `pow`, and int-to-float conversion, from which every tolerance in `putest` is built.
- **OpenGL 4.1 core profile** (25 July 2010) — §3.8.3: R32UI is a required texture and renderbuffer format; §3.8.14: an integer texture is incomplete unless its filters are NEAREST.
- **IEC 61966-2-1:1999 (sRGB)**, amended 2003 — the decode and encode in source/Shaders.cpp and source/Controls.cpp (constants checked against Wikipedia's sRGB article, not against the paid standard itself), and the harness's own separate copy.
- **ITU-R BT.709-6** (06/2015), item 3.2 — the luma weights 0.2126, 0.7152, 0.0722 for `Occluder = Luma`.
- **Porter and Duff**, "Compositing digital images", Computer Graphics 18(3), 253–259 (1984), doi:10.1145/800031.808606 — the premultiplied "over" behind `Screen = On White`.

## Inspirations

What this set out to be. No code, assets or binaries from any of these were used or examined — the debt is to the idea.

### The Exploratorium's Colored Shadows

<https://annex.exploratorium.edu/xref/exhibits/colored_shadows.html>

Red, green and blue spotlights that add to white on a wall, and a person between them whose shadows, close enough to the wall, make "six different colors, plus black and white" (the exhibit page, read on 2026-11-16). Built from how the light works; nothing is copied from the exhibit or anyone's software.

## Getting this wrong

If your work is here and the description is inaccurate, the licence is wrong, or you would rather not be listed — open an issue and it will be fixed.
