#!/usr/bin/env bash
#
# Everything, in the order that fails fastest.
#
# Each step answers a question none of the others can:
#
#   source        GLSL reserved words declared as identifiers, M_PI, the
#                 built-ins GLSL ES 3.00 lacks, and the FFGL SDK still at the
#                 fleet's pin
#   shaders       do the four shaders (the vertex shader, the convert, the
#                 prefix and the screen passes) compile through a real GLSL
#                 compiler (tools/glslc.sh), before a host has to find out.
#                 glslc is REQUIRED here: a skipped compile is not a pass.
#   build         a fresh universal Release build, which is what would ship;
#                 no warning in this repo's own code
#   checks        the plugin's claims, measured at TWO rasters (640x360 and
#                 320x180), each with negative controls built into the real
#                 plugin, and one character of the shipped GLSL mutated
#   software      the same checks on Apple's software renderer, which is
#                 what a GPU-less runner gets: a check calibrated on this
#                 Mac's GPU fails here before it fails anywhere else
#   pipe          the filming mode: N frames in, N out, two inputs; a
#                 standard parameter ramps and an option steps; bad cues
#                 and bad --set values refused; a closed stdout is exit 1
#   bundle        plugMain exported (the OBJECT-library trap), lipo
#                 (universal, or did CMake latch the architecture list
#                 early and report success anyway), the plist, an ad-hoc
#                 signature on a copy, the version in two places
#   oxbow         the name, id, TYPE and input count a host reads, parameter
#                 0 (which Arena hides on a mixer), the Opacity name, and the
#                 refusal of the single input an effect host hands it.
#                 REQUIRED: set OXBOW= to a built oxbow; without one this
#                 FAILS, it does not skip.
#   mutants       tools/mutate.sh: 29 one-character mutants, each caught
#   sweep         does every control change the picture (tools/sweep.py)
#   bench         the render cost at 720p, 1080p and 4K, for the record. Not
#                 pass/fail.
#
#     OXBOW=/path/to/oxbow tools/verify.sh [BUILD_DIR]
#
set -uo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO"

BUILD="${1:-$REPO/build-verify}"
OXBOW="${OXBOW:-$HOME/Projects/resolume/oxbow/build/oxbow}"
passes=0
failures=0
LOGS="$( mktemp -d "${TMPDIR:-/tmp}/puverify.XXXXXX" )"

# Every harness run instantiates the real plugin, and the plugin writes a
# log. Keep those lines out of the log an Arena session would be read from.
export PENUMBRA_LOG_DIR="${PENUMBRA_LOG_DIR:-$LOGS/penumbra-logs}"

step() { printf '\n\033[1m== %s\033[0m\n' "$1"; }
pass() { printf '   \033[32mok\033[0m   %s\n' "$1"; passes=$(( passes + 1 )); }
fail() { printf '   \033[31mFAIL\033[0m %s\n' "$1"; failures=$(( failures + 1 )); }

#---------------------------------------------------------------------------
step "source"
#---------------------------------------------------------------------------
# GLSL 4.10's reserved words, declared as identifiers: a declaration of one
# compiles on some drivers and not on others (`packed` passed Apple's compiler
# and glslc and failed only Mesa: atrac; noise1-4 are built-ins a user
# function is silently shadowed by: dice). Only declarations -- a type, then
# the word -- since calling a built-in such as round() is what it is for.
if python3 -I - <<'PY'
import re, sys, pathlib
words = ( "patch sample input output filter common active half layout flat packed smooth round "
          "noise1 noise2 noise3 noise4 superp partition resource namespace using cast external "
          "interface long short fixed unsigned inline goto asm class union enum typedef template "
          "this volatile public static extern sizeof near far precise noperspective" ).split()
types = r"(?:float|int|uint|bool|[iuhdf]?vec[234]|mat[234]|u?sampler2D|void|const)"
pattern = re.compile( r"\b" + types + r"\s+(" + "|".join( words ) + r")\b" )
text = pathlib.Path( "source/Shaders.cpp" ).read_text()
glsl = "".join( re.findall( r'R"\((.*?)\)"', text, re.S ) )
glsl = re.sub( r"//[^\n]*", "", glsl )# comments are free to use any word
hits = sorted( set( m.group( 1 ) for m in pattern.finditer( glsl ) ) )
if hits:
	print( "   declared in GLSL: " + ", ".join( hits ) )
	sys.exit( 1 )
if "M_PI" in glsl:
	print( "   M_PI in GLSL: no GLSL defines it" )
	sys.exit( 1 )
# The check must be able to see one.
if not pattern.search( "float sample = 1.0;" ):
	sys.exit( "   the reserved-word pattern matches nothing" )
PY
then
	pass "no GLSL reserved word declared as an identifier; no M_PI"
else
	fail "a GLSL reserved word is declared as an identifier, or M_PI is used"
fi
# Built-ins that arrived in GLSL 4.00 / ES 3.10: they compile everywhere here
# but not in WebGL2, where the fleet's browser demos run the shaders unedited.
# Penumbra has no demo; the rule is kept so one can be made.
if grep -nE '(^|[^A-Za-z_])(findMSB|findLSB|bitCount|bitfieldExtract|bitfieldInsert|bitfieldReverse|uaddCarry|usubBorrow|umulExtended|imulExtended|frexp|ldexp|fma|textureGather|textureQueryLevels|packUnorm4x8|unpackUnorm4x8)[[:space:]]*\(' source/Shaders.cpp | grep -vE '^[0-9]+:[[:space:]]*//' >"$LOGS/webgl.txt"; then
	sed 's/^/   /' "$LOGS/webgl.txt"
	fail "a built-in GLSL ES 3.00 lacks is in the GLSL"
else
	pass "no built-in GLSL ES 3.00 lacks"
fi
pin=$( git -C external/ffgl rev-parse --short=7 HEAD 2>/dev/null )
if [ "$pin" = "b1afaf9" ]; then
	pass "external/ffgl at the fleet's pin, b1afaf9"
else
	fail "external/ffgl is at '$pin', not the fleet's b1afaf9"
fi

#---------------------------------------------------------------------------
step "shaders"
#---------------------------------------------------------------------------
if GLSLC_REQUIRED=1 tools/glslc.sh; then
	pass "every shader compiles through glslc"
else
	fail "a shader does not compile, or glslc is missing (brew install shaderc)"
fi

#---------------------------------------------------------------------------
step "build"
#---------------------------------------------------------------------------
# A fresh universal Release build -- the one that would ship. The dev build
# in build/ is arm64 only and is not what any of the binary checks below
# should be looking at.
# Fresh only when it is verify's own directory: a BUILD_DIR passed in is
# someone's, and lipo below catches one that is not universal.
[ "$BUILD" = "$REPO/build-verify" ] && rm -rf "$BUILD"
if cmake -B "$BUILD" -DCMAKE_BUILD_TYPE=Release >"$LOGS/configure.txt" 2>&1 \
	&& cmake --build "$BUILD" --parallel >"$LOGS/build.txt" 2>&1; then
	pass "universal Release build in $( basename "$BUILD" )"
else
	tail -20 "$LOGS/build.txt" "$LOGS/configure.txt" 2>/dev/null | sed 's/^/   /'
	fail "build failed"
	printf '\n\033[31mverify: stopped at the build\033[0m\n'
	exit 1
fi
# Our own warnings only: the SDK's FFGLUtilities.cpp warns about RAND_MAX.
if grep -E "warning:" "$LOGS/build.txt" | grep -v "external/ffgl" >"$LOGS/warnings.txt"; then
	sed 's/^/   /' "$LOGS/warnings.txt" | head -10
	fail "the build warns in this repo's own code"
else
	pass "no warnings in this repo's own code"
fi
PUTEST="$BUILD/putest"

#---------------------------------------------------------------------------
step "checks (this Mac's GPU, 640x360 and 320x180)"
#---------------------------------------------------------------------------
# Every claim the README makes. Each GL check runs at both rasters itself and
# carries its own negative controls; --names needs no GL.
CHECKS="names mixer unchanged colour penumbra parting alpha linear opacity resize mutation"
negatives=0
for check in $CHECKS; do
	if out=$( "$PUTEST" --$check 2>&1 ); then
		n=$( printf '%s\n' "$out" | grep -c '^  ok' )
		neg=$( printf '%s\n' "$out" | grep -c '^  ok    negative control' )
		negatives=$(( negatives + neg ))
		pass "putest --$check: $n assertions, $neg of them negative controls"
	else
		printf '%s\n' "$out" | grep -E 'FAIL' | sed 's/^/      /'
		fail "putest --$check"
	fi
done

#---------------------------------------------------------------------------
step "software renderer (both rasters)"
#---------------------------------------------------------------------------
# All of them, on Apple's software renderer: about a minute and a half.
for check in $CHECKS; do
	[ "$check" = names ] && continue
	if out=$( PUTEST_RENDERER=software "$PUTEST" --$check 2>&1 ); then
		pass "putest --$check (software): $( printf '%s\n' "$out" | grep -c '^  ok' ) assertions"
	else
		printf '%s\n' "$out" | grep -E 'FAIL' | sed 's/^/      /'
		fail "putest --$check on the software renderer -- run: PUTEST_RENDERER=software $PUTEST --$check"
	fi
done

#---------------------------------------------------------------------------
step "pipe"
#---------------------------------------------------------------------------
# The fleet's --pipe frame format, with the mixer's second input: stdin is
# Dest (the screen), --pipe-src is Src (the occluder), raw RGBA, top row first.
W=64; H=36; FRAME=$(( W * H * 4 ))
tmp="$LOGS/pipe"; mkdir -p "$tmp"
# frames N R G B A > file: N flat frames.
frames() {
	python3 -I -c "import sys; sys.stdout.buffer.write(bytes([$2, $3, $4, $5]) * ($W * $H * $1))"
}
frames 3 255 255 255 255 > "$tmp/white3.rgba"
frames 3 0 0 0 0 > "$tmp/clear3.rgba"
frames 3 0 0 0 255 > "$tmp/opaque3.rgba"
frames 2 0 0 0 255 > "$tmp/opaque2.rgba"
head -c $(( FRAME * 5 / 2 )) "$tmp/white3.rgba" > "$tmp/white2half.rgba"

status=0
"$PUTEST" --pipe --size ${W}x${H} --pipe-src "$tmp/clear3.rgba" < "$tmp/white3.rgba" > "$tmp/out.rgba" 2>/dev/null || status=$?
if [ "$status" -eq 0 ] && cmp -s "$tmp/out.rgba" "$tmp/white3.rgba"; then
	pass "3 frames in, 3 out; a clear Src gives Dest back, byte for byte"
else
	fail "3 frames with a clear Src: exit $status, $( wc -c < "$tmp/out.rgba" | tr -d ' ' ) bytes, or not Dest"
fi
status=0
"$PUTEST" --pipe --size ${W}x${H} --pipe-src "$tmp/opaque3.rgba" < "$tmp/white3.rgba" > "$tmp/out.rgba" 2>/dev/null || status=$?
if [ "$status" -eq 0 ] && [ "$( wc -c < "$tmp/out.rgba" | tr -d ' ' )" -eq $(( FRAME * 3 )) ] && ! cmp -s "$tmp/out.rgba" "$tmp/white3.rgba"; then
	pass "an opaque Src from --pipe-src shades the screen"
else
	fail "an opaque Src from --pipe-src: exit $status, or the screen came back unshaded"
fi
status=0
"$PUTEST" --pipe --size ${W}x${H} --pipe-src "$tmp/opaque2.rgba" < "$tmp/white3.rgba" > "$tmp/out.rgba" 2>/dev/null || status=$?
got=$( wc -c < "$tmp/out.rgba" | tr -d ' ' )
if [ "$status" -eq 0 ] && [ "$got" -eq $(( FRAME * 2 )) ]; then
	pass "3 Dest frames over 2 Src frames: 2 out -- the stream ends with either input"
else
	fail "3 Dest frames over 2 Src frames gave $got bytes, exit $status"
fi
status=0
"$PUTEST" --pipe --size ${W}x${H} --input-b clear < "$tmp/white2half.rgba" > "$tmp/out.rgba" 2>/dev/null || status=$?
got=$( wc -c < "$tmp/out.rgba" | tr -d ' ' )
if [ "$status" -eq 0 ] && [ "$got" -eq $(( FRAME * 2 )) ]; then
	pass "2.5 frames in, exactly 2 out: a partial frame ends the stream"
else
	fail "2.5 frames in gave $got bytes (want $(( FRAME * 2 ))), exit $status"
fi
# Cues: Opacity ramps 0 -> 1 over frames 0..2; View steps Picture -> Light at
# frame 4. A grey screen under an opaque occluder, so Picture and Light differ:
#   frames 0 > 1 > 2   the ramp (a STEPPED Opacity would be 0, 0, 1)
#   frames 2 = 3       View still Picture (a RAMPED View is 0.5 at 2: Light)
#   frames 3 != 4 = 5  the step
frames 6 128 128 128 255 > "$tmp/grey6.rgba"
frames 6 0 0 0 255 > "$tmp/opaque6.rgba"
printf '0 Opacity 0\n2 Opacity 1\n0 View 0\n4 View 1\n' > "$tmp/cues.txt"
status=0
"$PUTEST" --pipe --size ${W}x${H} --pipe-src "$tmp/opaque6.rgba" --script "$tmp/cues.txt" < "$tmp/grey6.rgba" > "$tmp/cued.rgba" 2>/dev/null || status=$?
verdict=$( python3 -I - "$tmp/cued.rgba" $FRAME <<'PY'
import sys
data, size = open( sys.argv[ 1 ], "rb" ).read(), int( sys.argv[ 2 ] )
f = [ data[ i * size:( i + 1 ) * size ] for i in range( len( data ) // size ) ]
if len( f ) != 6:
	print( f"bad {len( f )} frames" ); sys.exit()
m = [ sum( x[ 0::4 ] ) / ( size / 4 ) for x in f ]
ok = m[ 0 ] > m[ 1 ] > m[ 2 ] and f[ 2 ] == f[ 3 ] and f[ 3 ] != f[ 4 ] and f[ 4 ] == f[ 5 ]
print( ( "ok " if ok else "bad " ) + " ".join( f"{v:.1f}" for v in m ) )
PY
)
case "$status:$verdict" in
	0:ok*) pass "cues: Opacity ramps, View steps (mean red ${verdict#ok })" ;;
	*) fail "cues gave exit $status, '${verdict#bad }': want a falling ramp over 0..2, equal 2 and 3, a step at 4" ;;
esac
printf '0 No Such Control 0.5\n' > "$tmp/bad.txt"
status=0
"$PUTEST" --pipe --size ${W}x${H} --script "$tmp/bad.txt" < "$tmp/white3.rgba" >/dev/null 2>&1 || status=$?
[ "$status" -eq 2 ] && pass "a cue naming no parameter is refused (exit 2)" || fail "a cue naming no parameter gave exit $status, not 2"
status=0
"$PUTEST" --out "$tmp/x.png" --size ${W}x${H} --set "View=Sideways" >/dev/null 2>&1 || status=$?
s2=0
"$PUTEST" --out "$tmp/x.png" --size ${W}x${H} --set "Distance=0.5x" >/dev/null 2>&1 || s2=$?
s3=0
"$PUTEST" --out "$tmp/x.png" --size ${W}x${H} --set "View=Light" >/dev/null 2>&1 || s3=$?
if [ "$status" -eq 2 ] && [ "$s2" -eq 2 ] && [ "$s3" -eq 0 ]; then
	pass "--set refuses 'View=Sideways' and 'Distance=0.5x' (exit 2), takes 'View=Light'"
else
	fail "--set: no such option gave $status, a half number gave $s2, a real option gave $s3"
fi
# Read from a file, not a pipe: a writer killed by SIGPIPE would fail the
# pipeline whatever putest did.
frames 20 128 128 128 255 > "$tmp/grey20.rgba"
"$PUTEST" --pipe --size ${W}x${H} < "$tmp/grey20.rgba" 2>/dev/null | head -c 1 >/dev/null
status=${PIPESTATUS[0]}
if [ "$status" -eq 1 ]; then
	pass "putest --pipe | head -c 1 exits 1 (SIGPIPE ignored), not 141"
else
	fail "a closed stdout gave exit $status, expected 1"
fi

#---------------------------------------------------------------------------
step "Penumbra.bundle"
#---------------------------------------------------------------------------
bundle="$BUILD/Penumbra.bundle"
bin="$bundle/Contents/MacOS/Penumbra"
declared=$( sed -n 's/^[[:space:]]*VERSION \([0-9.]*\)$/\1/p' CMakeLists.txt | head -1 )
if grep -q "versionFallback = \"v$declared\"" source/StoatworksAbout.h; then
	pass "CMakeLists and StoatworksAbout.h agree: v$declared"
else
	fail "StoatworksAbout.h's versionFallback is not v$declared"
fi
# `nm ... | grep -q X` FAILS when grep FINDS its match under pipefail: grep
# exits at once, nm takes SIGPIPE, and the pipeline reports that failure.
syms=$( nm -gU "$bin" 2>/dev/null )
case "$syms" in
	*_plugMain*) pass "exports plugMain" ;;
	*) fail "no plugMain -- the bundle contains no plugin" ;;
esac
archs=$( lipo -archs "$bin" 2>/dev/null )
case "$archs" in *arm64*x86_64*|*x86_64*arm64*) pass "universal: $archs" ;; *) fail "not universal (got: $archs)" ;; esac
plist="$bundle/Contents/Info.plist"
read_plist() { /usr/libexec/PlistBuddy -c "Print :$1" "$plist" 2>/dev/null; }
exe=$( read_plist CFBundleExecutable )
if [ -n "$exe" ] && [ -f "$bundle/Contents/MacOS/$exe" ] && [ "$( read_plist CFBundleIdentifier )" = "com.stoatworks.ffgl.penumbra" ] \
	&& [ "$( read_plist CFBundlePackageType )" = "BNDL" ] && [ "$( read_plist CFBundleVersion )" = "$declared" ]; then
	pass "plist: CFBundleExecutable $exe on disk, com.stoatworks.ffgl.penumbra, BNDL, v$declared"
else
	fail "plist: executable '$exe', id '$( read_plist CFBundleIdentifier )', type '$( read_plist CFBundlePackageType )', version '$( read_plist CFBundleVersion )'"
fi
copy="$LOGS/sign"; mkdir -p "$copy"; cp -R "$bundle" "$copy/"
if codesign --force --sign - --timestamp=none "$copy/Penumbra.bundle" >/dev/null 2>&1 \
	&& codesign --verify --deep --strict "$copy/Penumbra.bundle" >/dev/null 2>&1; then
	pass "ad-hoc signs and verifies (on a copy)"
else
	fail "ad-hoc signing failed"
fi

#---------------------------------------------------------------------------
step "oxbow"
#---------------------------------------------------------------------------
if [ ! -x "$OXBOW" ]; then
	fail "no oxbow at $OXBOW -- set OXBOW= to a built oxbow; this step does not skip"
else
	probe=$( "$OXBOW" probe "$bundle" 2>&1 )
	printf '%s\n' "$probe" > "$LOGS/probe.txt"
	case "$probe" in *"name:        SW Penumbra"$'\n'*) pass "a host reads the name SW Penumbra" ;; *) fail "wrong or missing name -- see: $OXBOW probe $bundle" ;; esac
	case "$probe" in *"id:          PU01"*) pass "a host reads the id PU01" ;; *) fail "wrong or missing id (want PU01)" ;; esac
	case "$probe" in *"type:        mixer"*) pass "a host reads the type mixer" ;; *) fail "wrong plugin type (want mixer) -- the 8th argument of CFFGLPluginInfo" ;; esac
	case "$probe" in *"inputs:      2..2"*) pass "a host is told 2..2 inputs" ;; *) fail "wrong input count (want 2..2) -- SetMinInputs/SetMaxInputs are separate from the type" ;; esac
	# Arena does not expose a mixer's parameter 0 (genlock, wipe, relay,
	# lenticular, pepper), so index 0 is a control whose default is right for
	# ever: Linear Light, on.
	case "$probe" in
		*"[ 0] Linear Light     type=0   default=1 "*) pass "parameter 0, which Arena hides, is Linear Light, a boolean, on" ;;
		*) fail "parameter 0 is not Linear Light (boolean, on) -- Arena hides a mixer's first parameter" ;;
	esac
	# Arena binds a mixer parameter named Opacity to the layer's fader.
	case "$probe" in
		*"] Opacity          type=10"*) pass "the occluder's opacity is a standard parameter named Opacity" ;;
		*) fail "no standard parameter named Opacity -- Arena binds the layer fader by that name" ;;
	esac
	# oxbow hosts effects: it hands a plugin one input. A mixer must refuse
	# that (its guard, FF_FAIL), not draw from an unbound texture unit.
	selftest=$( "$OXBOW" selftest "$bundle" 2>&1 )
	case "$selftest" in
		*"FF_PROCESS_OPENGL failed on frame 0"*) pass "one input, as an effect host gives, is refused (FF_FAIL)" ;;
		*) fail "the mixer did not refuse a single input" ;;
	esac
fi

#---------------------------------------------------------------------------
step "mutants"
#---------------------------------------------------------------------------
if tools/mutate.sh >"$LOGS/mutate.txt" 2>&1; then
	pass "$( tail -1 "$LOGS/mutate.txt" )"
else
	grep -E '^== mutant|FAIL' "$LOGS/mutate.txt" | grep -B1 'FAIL' | sed 's/^/   /'
	fail "a mutant survived: $( tail -1 "$LOGS/mutate.txt" )"
fi

#---------------------------------------------------------------------------
step "sweep"
#---------------------------------------------------------------------------
# The only thing that catches a uniform whose name does not match the C++.
if out=$( python3 -I tools/sweep.py --build "$BUILD" 2>&1 ); then
	pass "$( printf '%s\n' "$out" | tail -1 )"
else
	printf '%s\n' "$out" | sed 's/^/   /'
	fail "a dead control"
fi

#---------------------------------------------------------------------------
step "bench"
#---------------------------------------------------------------------------
"$PUTEST" --bench --frames 30 2>&1 | sed -n '4,7p' | sed 's/^/   /'

printf '\n'
if [ "$failures" -eq 0 ]; then
	printf '\033[32mverify: all %d checks passed\033[0m (%d negative controls; logs in %s)\n' "$passes" "$negatives" "$LOGS"
else
	printf '\033[31mverify: %d of %d checks failed\033[0m (logs in %s)\n' "$failures" $(( passes + failures )) "$LOGS"
fi
exit $(( failures > 0 ? 1 : 0 ))
