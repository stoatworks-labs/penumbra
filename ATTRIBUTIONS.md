# Attributions

Penumbra is built on other people's work. This file lists what that work is, who did
it, and what it is doing here.

It is generated — the master lists live in the `stoatworks-backend` repo and are
pushed out by `scripts/sync-attributions.py`. Edit it there, not here.

## Code we derived from other people's work

Someone else solved this first, and this project would not exist in its current form without their work.

### The mixer's shape, the two-input harness, the pipe and the log — Stoatworks pepper

<https://github.com/stoatworks-labs/pepper>  
Licence: MIT  
Copyright: Stoatworks Labs

How the mixer takes its two inputs is pepper's (and lenticular's, which shares it): the guards on the input count, the pointers and zero sizes, each input with its own MaxUV, index 0 as a sacrificial parameter, Opacity named so Arena binds the layer fader, and the per-instance Opacity log. The harness's two-input rig, its --pipe/--pipe-src mode (SIGPIPE ignored, a closed stdout exits 1), its cue sheets and the shape of tools/verify.sh are pepper's, adapted; so is tools/wincheck.ps1, which loads the Windows DLL as a host does and knows a mixer. source/Diag.{h,cpp} is pepper's, unchanged but for the name.

### What a mixer is in Arena — Stoatworks genlock

<https://github.com/stoatworks-labs/genlock>  
Licence: MIT  
Copyright: Stoatworks Labs

genlock's AGENTS.md is the measured account of how Resolume Arena drives an FFGL mixer (which input is Dest, the padding, the hidden first parameter, MaxUV folded into the vertex shader as the trap to avoid), and putest --mixer's magenta-padding sentinel is genlock's test, adapted. No code beyond the test's shape.

### The tooling and the About block — Stoatworks haze

<https://github.com/stoatworks-labs/haze>  
Licence: MIT  
Copyright: Stoatworks Labs

tools/glslc.sh, tools/mutate.sh (one character at a time, each under perl's alarm), tools/sweep.py (with pepper's) and the CI workflow are haze's, adapted, as was the first draft of the About headers the generated ones replaced.

### The release workflow — Stoatworks xenon

<https://github.com/stoatworks-labs/xenon>  
Licence: MIT  
Copyright: Stoatworks Labs

.github/workflows/release.yml is xenon's (slitscan's): a universal bundle and a Windows DLL, the DLL loaded as a host loads it, ATTRIBUTIONS.md in each zip, and the Windows installer per user into Resolume's Extra Effects. Adapted.

### The GL state guard — Stoatworks vectrix, from resolume-scopes

<https://github.com/stoatworks-labs/vectrix>  
Licence: MIT  
Copyright: Stoatworks Labs

source/GLState.h is haze's, which is radar's, which is vectrix's, which lifted it from resolume-scopes' Scopes.cpp: the host's GL state captured and put back. ScopedGLState is penumbra's addition.

## Third-party code this project uses

Libraries, SDKs and frameworks the project is built on or bundles.

### Resolume FFGL SDK

<https://github.com/resolume/ffgl>  
Licence: BSD-3-Clause  
Copyright: FreeFrame

Vendored as a git submodule at external/ffgl (third_party/ffgl in oxbow).

The plugin ABI itself. An FFGL effect or source is defined by this SDK's headers — there is no other way to be loadable by Resolume Arena and Avenue.

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

Part of the upstream SDK tree rather than something these plugins call directly — listed because it is present in the checkout.

## Work we checked ourselves against

No code was taken from these — but they were how we knew we had it right, and that is worth saying out loud.

### Soler and Sillion, "Fast calculation of soft shadow textures using convolution" — Cyril Soler and François X. Sillion, SIGGRAPH 98, pp. 321–332

<https://doi.org/10.1145/280814.280927>

§3.1, eq. 1–3: with source, occluder and receiver in parallel planes, the part of a lamp a receiver point can see is the lamp's shape, scaled by similar triangles, convolved with the occluder. That identity is the whole plugin: each lamp's shadow is the silhouette moved by −Lᵢ·g and box-filtered by a square of side sᵢ·g. Read against the paper's text on 2026-11-16; nothing is copied from it but the mathematics.

### The summed-area table

A box over the silhouette as four lookups in a table of prefix sums, held as unsigned integers so every box sum is exact (GLSL 4.10 §4.1.3's wrap-around). A long-standing graphics technique; the radix-16 prefix passes and the part-texel weights in source/Shaders.cpp are written for this plugin.

## Inspirations

What this set out to be. No code, assets or binaries from any of these were used or examined — the debt is to the idea.

### Coloured shadows — the Exploratorium's Colored Shadows exhibit

<https://www.exploratorium.edu/exhibits/colored-shadows>

Three coloured lamps on a white wall, and a person in front making cyan, magenta and yellow shadows, black only where all three are blocked. The plugin is that exhibit as a blend mode; nothing is copied from it but the idea.

## Standards and published specifications

What the implementation is measured against.

- **IEC 61966-2-1 (sRGB)** — The lamps' picked colours and the clip are decoded to linear light so the lamps' light adds, then encoded again (source/Shaders.cpp, source/Model.cpp).
- **OpenGL 4.1 and GLSL 4.10** — R32UI integer textures with NEAREST filters (GL 4.1 §3.8.14), unsigned wrap-around (GLSL 4.10 §4.1.3) for exact box sums, and the precision of exp2 and log2 (§4.5.1) that the harness's sRGB tolerance is derived from.

## Getting this wrong

If your work is here and the description is inaccurate, the licence is wrong, or you would rather not be listed — open an issue and it will be fixed.
