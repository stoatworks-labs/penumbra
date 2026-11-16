#!/usr/bin/env bash
#
# Mutation testing: change ONE character of the shipped code and require that a
# check fails. A check that still passes against a mutant was not looking at
# the code it claims to cover -- and a harness that compiled its own copy of
# the shaders would pass every GLSL mutant here.
#
# One copy of the tree in a temporary directory (the FFGL SDK is symlinked,
# not copied), configured once, arm64-only. For each mutant the pristine file
# is put back, the one character changed, putest rebuilt incrementally, and
# only the named check run (it runs at 640x360 and 320x180), under a time
# limit (a mutant can hang: honeydew's lesson; a timeout counts as caught).
#
#     tools/mutate.sh
#
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# The scratch tree goes under TMPDIR when it is set (a heredoc into python
# hung once with the system's /var/folders TMPDIR: the fleet's trap).
WORK="$(mktemp -d "${TMPDIR:-/tmp}/pumutate.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT

# file | the exact text | its one-character mutant | the check that must fail | what it is
# The fields are split on '|', so neither a target nor a mutant may contain one
# (conway's trap) -- which is why no mutant here touches a GLSL '||'.
MUTANTS=(
	"source/Shaders.cpp|vec4 t  = texelFetch( Occluder, p - ivec2( 1 ), 0 );|vec4 t  = texelFetch( Occluder, p - ivec2( 0 ), 0 );|--mixer|GLSL convert: the occluder read one texel late, against the table's leading zeros, 1 -> 0"
	"source/Shaders.cpp|float o = Luma != 0 ? dot( t.rgb, vec3( 0.2126, 0.7152, 0.0722 ) ) : t.a;|float o = Luma == 0 ? dot( t.rgb, vec3( 0.2126, 0.7152, 0.0722 ) ) : t.a;|--alpha|GLSL convert: Luma and Alpha swapped, ! -> ="
	"source/Shaders.cpp|ivec2 q = p - Step * t;|ivec2 q = p + Step * t;|--penumbra|GLSL prefix: the sum taken forwards, - -> +"
	"source/Shaders.cpp|w       = i0 == i1 ? vec3( b - a, 0.0, 0.0 ) : vec3( float( i0 + 1 ) - a, 1.0, b - float( i1 ) );|w       = i0 == i1 ? vec3( b - a, 0.0, 0.0 ) : vec3( float( i0 + 1 ) - a, 1.0, b + float( i1 ) );|--penumbra|GLSL axisOf: the last part-texel's weight b + i1, - -> +"
	"source/Shaders.cpp|len     = hi - lo;|len     = hi + lo;|--penumbra|GLSL axisOf: the box's length hi + lo, - -> +"
	"source/Shaders.cpp|inside  = lo >= 0.0 && hi <= float( n );|inside  = lo <= 0.0 && hi <= float( n );|--penumbra|GLSL axisOf: a box hanging off the low corner taken as wholly on the frame, > -> <"
	"source/Shaders.cpp|		return 1.0;|		return 0.0;|--colour|GLSL occluded: the exact umbra branch returning 0, 1 -> 0"
	"source/Shaders.cpp|uint run = t[ ( j + 1 ) * 4 + i + 1 ] - t[ ( j + 1 ) * 4 + i ] - t[ j * 4 + i + 1 ] + t[ j * 4 + i ];|uint run = t[ ( j + 1 ) * 4 + i + 1 ] - t[ ( j + 1 ) * 4 + i ] + t[ j * 4 + i + 1 ] + t[ j * 4 + i ];|--penumbra|GLSL occluded: a run's box sum with + for its top edge, - -> +"
	"source/Shaders.cpp|return sum / ( lx * ly * float( Levels ) );|return sum / ( lx + ly * float( Levels ) );|--penumbra|GLSL occluded: the box's area lx + ly, * -> +"
	"source/Shaders.cpp|vec2 base = ( vec2( px ) + ( faulty( kFaultHalfTexel ) ? 0.0 : 0.5 ) ) * Scale;|vec2 base = ( vec2( px ) + ( faulty( kFaultHalfTexel ) ? 0.0 : 0.6 ) ) * Scale;|--parting|GLSL screen: the pixel's centre a tenth of a pixel off, 5 -> 6"
	"source/Shaders.cpp|float o = opacity * occluded( base + Offset[ i ], HalfSize[ i ] );|float o = opacity + occluded( base + Offset[ i ], HalfSize[ i ] );|--opacity|GLSL screen: Opacity added to the blocking, * -> +"
	"source/Shaders.cpp|shade += Share[ i ] * o;|shade -= Share[ i ] * o;|--colour|GLSL screen: a blocked lamp adding light, + -> -"
	"source/Shaders.cpp|fragColor = vec4( Linear != 0 ? encode( L ) : L, 1.0 );|fragColor = vec4( Linear == 0 ? encode( L ) : L, 1.0 );|--linear|GLSL screen: Light view encoded only with Linear Light off, ! -> ="
	"source/Shaders.cpp|lit = encode( decode( clamp( rgb / a, 0.0, 1.0 ) ) * L ) * a;|lit = encode( decode( clamp( rgb * a, 0.0, 1.0 ) ) * L ) * a;|--alpha|GLSL screen: Dest premultiplied twice instead of unpremultiplied, / -> *"
	"source/Shaders.cpp|lit = mix( lit, rgb, equal( L, vec3( 1.0 ) ) );|lit = mix( lit, rgb, equal( L, vec3( 0.0 ) ) );|--unchanged|GLSL screen: the pass-through at light 0, not 1, 1 -> 0"
	"source/Shaders.cpp|rgb = d.rgb + ( 1.0 - d.a );|rgb = d.rgb - ( 1.0 - d.a );|--unchanged|GLSL screen: On White subtracting the white, + -> -"
	"source/Shaders.cpp|		a   = 1.0;|		a   = 0.0;|--alpha|GLSL screen: On White transparent, 1 -> 0"
	"source/Shaders.cpp|return mix( c / 12.92, pow( ( c + 0.055 ) / 1.055, vec3( 2.4 ) ), greaterThan( c, vec3( 0.04045 ) ) );|return mix( c / 12.92, pow( ( c + 0.055 ) / 1.055, vec3( 2.4 ) ), greaterThan( c, vec3( 0.14045 ) ) );|--linear|GLSL decode: the knee at 0.14, 0 -> 1"
	"source/Shaders.cpp|return mix( x * 12.92, 1.055 * pow( x, vec3( 1.0 / 2.4 ) ) - 0.055, greaterThan( x, vec3( 0.0031308 ) ) );|return mix( x * 12.92, 1.055 * pow( x, vec3( 1.0 / 2.2 ) ) - 0.055, greaterThan( x, vec3( 0.0031308 ) ) );|--linear|GLSL encode: gamma 1 / 2.2, 4 -> 2"
	"source/Shaders.cpp|p      = clamp( p, HalfTexelScreen, vec2( 1.0 ) - HalfTexelScreen ) * MaxUVScreen;|p      = clamp( p, HalfTexelScreen, vec2( 1.0 ) - HalfTexelScreen ) / MaxUVScreen;|--mixer|GLSL screenAt: Dest's MaxUV divided, not multiplied, * -> /"
	"source/Shaders.cpp|	if( Opacity > 0.0 )|	if( Opacity > 1.0 )|--colour|GLSL screen: no shadow at all unless Opacity exceeds 1, 0 -> 1"
	"source/Model.cpp|s.g            = k / ( 1.0 - k );|s.g            = k / ( 1.0 + k );|--penumbra|C++: g = k / (1 + k), - -> +"
	"source/Model.cpp|l.offset[ 1 ]     = sign * asked.lamp[ i ].y * s.g * perHeight[ 1 ];|l.offset[ 1 ]     = sign * asked.lamp[ i ].y * s.g * perHeight[ 0 ];|--penumbra|C++: a lamp's vertical offset at the occluder's horizontal scale, 1 -> 0"
	"source/Model.cpp|l.half[ 1 ] = 0.5 * side * perHeight[ 1 ];|l.half[ 1 ] = 0.5 * side * perHeight[ 0 ];|--penumbra|C++: a box's height at the occluder's horizontal scale, 1 -> 0"
	"source/Model.cpp|s.total[ c ] += s.lamp[ i ].linear[ c ];|s.total[ c ] -= s.lamp[ i ].linear[ c ];|--colour|C++: the lamps subtracted from the total light, + -> -"
	"source/Controls.cpp|return c <= 0.04045 ? c / 12.92 : std::pow( ( c + 0.055 ) / 1.055, 2.4 );|return c <= 0.04045 ? c / 12.92 : std::pow( ( c + 0.055 ) / 1.055, 2.5 );|--linear|C++: a lamp's colour decoded at gamma 2.5, 4 -> 5"
	"source/Controls.cpp|return kLampSizeMax * p * p;|return kLampSizeMax * p + p;|--names|C++: Lamp Size's mapping no longer squared, * -> +"
	"source/Penumbra.cpp|for( long stride = 1; stride < n; stride *= kRadix )|for( long stride = 1; stride > n; stride *= kRadix )|--penumbra|C++: no prefix pass at all, the table left as raw levels, < -> >"
	"source/Penumbra.cpp|const int taps = static_cast< int >( std::min< long >( kRadix, ( n - 1 ) / stride + 1 ) );|const int taps = static_cast< int >( std::min< long >( kRadix, ( n - 1 ) / stride - 1 ) );|--penumbra|C++: the last prefix pass two taps short, + -> -"
)

limit() {
	# macOS has no timeout(1): perl's alarm is enough.
	perl -e 'alarm shift; exec @ARGV' "$@"
}

tree="$WORK/tree"
mkdir -p "$tree/external"
cp -R "$REPO/source" "$REPO/tools" "$REPO/cmake" "$REPO/CMakeLists.txt" "$tree/"
ln -s "$REPO/external/ffgl" "$tree/external/ffgl"
cmake -S "$tree" -B "$tree/build" -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES=arm64 >/dev/null
cmake --build "$tree/build" --target putest -j6 >/dev/null 2>&1

caught=0
for entry in "${MUTANTS[@]}"; do
	IFS='|' read -r file original mutant check what <<<"$entry"
	# Every other mutant's file back to the shipped text first. Each file
	# touched loses its object too: make on macOS compares whole-second
	# mtimes, and a file put back in the same second the last mutant's
	# object was written looks up to date -- the previous mutant then runs
	# again under this one's name (seen here: Lamp Size's mutant reached the
	# next two runs).
	for f in source/Shaders.cpp source/Model.cpp source/Controls.cpp source/Penumbra.cpp "$file"; do
		cmp -s "$REPO/$f" "$tree/$f" || cp "$REPO/$f" "$tree/$f"
		find "$tree/build" -name "$(basename "$f").o" -delete
	done
	# And the binary, so it is relinked whatever the clock says.
	rm -f "$tree/build/putest"

	python3 - "$tree/$file" "$original" "$mutant" <<'PY'
import sys, pathlib
path, original, mutant = pathlib.Path(sys.argv[1]), sys.argv[2], sys.argv[3]
text = path.read_text()
if text.count(original) != 1:
    sys.exit(f"mutation target found {text.count(original)} times in {path}: '{original}'")
if sum(a != b for a, b in zip(original, mutant)) != 1 or len(original) != len(mutant):
    sys.exit("a mutant must differ by exactly one character")
path.write_text(text.replace(original, mutant))
PY

	printf '\n== mutant: %s\n' "$what"
	if ! cmake --build "$tree/build" --target putest -j6 >"$WORK/build.log" 2>&1; then
		printf '   FAIL  the mutant does not build -- a mutant must compile to mean anything\n'
		continue
	fi
	# A GLSL mutant is a string literal in the binary: if the binary does not
	# carry it, whatever runs next is not this mutant.
	if [[ "$file" == source/Shaders.cpp ]] && ! grep -qF -- "$mutant" "$tree/build/putest"; then
		printf '   FAIL  the built putest does not carry the mutant -- a stale build\n'
		continue
	fi
	status=0
	limit 300 "$tree/build/putest" "$check" >"$WORK/log" 2>&1 || status=$?
	if [[ "$status" -eq 0 ]]; then
		printf '   FAIL  %s still PASSES -- the check does not cover this code\n' "$check"
	else
		printf '   ok    %s fails against the mutant (exit %d):\n' "$check" "$status"
		# A mutant that timed out printed no FAIL: grep finding none must not end the run.
		{ grep -E '^  FAIL' "$WORK/log" || true; } | head -2 | cut -c1-160 | sed 's/^/        /'
		caught=$(( caught + 1 ))
	fi
done

printf '\nmutants: %d, caught: %d\n' "${#MUTANTS[@]}" "$caught"
[[ "$caught" -eq "${#MUTANTS[@]}" ]]
