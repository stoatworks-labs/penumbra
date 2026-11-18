"""The demo's shaders must be the plugin's shaders, character for character.

    python3 demo/tools/check_shaders.py           compare; exit 1 on any drift
    python3 demo/tools/check_shaders.py --write   regenerate demo/shaders.js

Called from `tools/verify.sh`. Exit code 1 means a copy has drifted.

------------------------------------------------------------------- why

`demo/shaders.js` holds the GLSL pieces of `source/Shaders.cpp`: kVersion,
kVertex, kCommon (the sRGB pair), and the three passes -- Convert, Prefix and
Screen. That is two copies of the same text, and two copies drift --
quietly, because a coloured shadow cast by a *plausible* shader looks exactly
like one cast by the right one. The whole claim of the page is that it runs
the plugin's own shaders, so the claim needs something enforcing it. `putest`
drives the real plugin class and has never heard of this page, and
`tools/glslc.sh` compiles the C++ copies and never looks at the JS one.

------------------------------------------------------------------- what it does

Pulls each piece out of `source/Shaders.cpp` and the matching backtick literal
out of `demo/shaders.js`, and compares them exactly -- no whitespace
normalisation, no comment stripping: a comment updated on one side only is
exactly the drift worth catching. A piece may be several adjacent raw strings
(MSVC refuses one literal over about 16 KB); this joins them as the compiler
does, with nothing between.

It fails when Shaders.cpp grows a piece this script does not list (a pass the
page would silently lack), and it checks the JOIN as well as the pieces:
AssembleFragment() is kVersion + kCommon + the pass and AssembleVertex() is
kVersion + kVertex, Penumbra.cpp's InitGL compiles those for all three passes,
and plugin.js must assemble them the same way.

The page is more than the shaders: the orchestration around them -- the table
one texel bigger than Src each way, the radix-16 prefix loop along x then y,
each pass's tap count, the ping-pong -- is Penumbra.cpp's ProcessOpenGL
translated by hand. This cannot check a translation, but it can trip when its
source moves: ORCHESTRATION lists the C++ lines the port was made from, and a
change to any of them fails here and needs a reader to carry it across.

And it fails when a piece declares a uniform that plugin.js never names (a
uniform the page does not set sits at 0, a control quietly dead on the page and
live in the plugin), or when plugin.js hands an integer uniform -- int, ivec,
uint or a sampler -- to the kit's `.set()`, which always uploads a float and so
is refused by GL without a word. Every piece is checked, not only the
fragments.

The one transformation is a decode, not a normalisation: a backtick cannot
appear raw inside a template literal, so --write would escape one as \\`.
Shaders.cpp has none today. This undoes that escape and REJECTS any other
backslash or any `${` on the JS side; there are none in the C++ bodies, so
either could only be somebody hiding a difference.

`--write` produces shaders.js from the C++ by exactly that escape, so the copy
is spliced by this script and never typed.

------------------------------------------------------------------- what it cannot

Nothing here checks the PORTED half: Controls.cpp (every conversion),
Model.cpp's Plan (the lamps' shares, offsets and boxes, Q) and the CPU half of
Penumbra.cpp's ProcessOpenGL (the passes and every uniform) in demo/plugin.js
are a hand translation. demo/tools/crosscheck/ renders putest and the page from
the same frames and compares the pictures; it needs Chrome and a GPU, so it is
run by hand, not by verify.sh. When you change one of those, change the port
too.
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))
sys.dont_write_bytecode = True

# JS constant, C++ symbol (all in source/Shaders.cpp), in the order they are
# declared there.
SHADERS = [
    ("VERSION", "kVersion"),
    ("VERTEX", "kVertex"),
    ("COMMON", "kCommon"),
    ("CONVERT_FRAGMENT", "kConvertFragment"),
    ("PREFIX_FRAGMENT", "kPrefixFragment"),
    ("SCREEN_FRAGMENT", "kScreenFragment"),
]

# The pieces whose uniforms plugin.js must set (by name, quoted).
UNIFORM_PIECES = ["kVertex", "kCommon", "kConvertFragment", "kPrefixFragment", "kScreenFragment"]

# How the pieces are joined, on each side. Each entry is (file, text that must
# appear in it verbatim). The C++ lines are the plugin's assembly; the JS lines
# are plugin.js's copy of it. A change to either is a change to what runs.
ASSEMBLY = [
    ("source/Shaders.cpp",
     "std::string AssembleFragment( const char* pass )\n{\n"
     "\treturn std::string( kVersion ) + kCommon + pass;\n}"),
    ("source/Shaders.cpp",
     "std::string AssembleVertex()\n{\n"
     "\treturn std::string( kVersion ) + kVertex;\n}"),
    ("source/Penumbra.cpp", "if( shader.Compile( AssembleVertex(), fragment ) )"),
    ("source/Penumbra.cpp",
     "const std::string screen = fragmentOverride ? std::string( fragmentOverride ) : AssembleFragment( kScreenFragment );"),
    ("source/Penumbra.cpp", "if( !compile( convertShader, AssembleFragment( kConvertFragment ), \"convert\" )"),
    ("source/Penumbra.cpp",
     "|| !compile( prefixShader, AssembleFragment( kPrefixFragment ), \"prefix\" ) || !compile( screenShader, screen, \"screen\" ) )"),
    ("demo/plugin.js", "const vertex = S.VERSION + S.VERTEX;"),
    ("demo/plugin.js", "const assemble = (pass) => S.VERSION + S.COMMON + pass;"),
    ("demo/plugin.js", "convert: new Program(gl, vertex, assemble(S.CONVERT_FRAGMENT), 'convert'),"),
    ("demo/plugin.js", "prefix: new Program(gl, vertex, assemble(S.PREFIX_FRAGMENT), 'prefix'),"),
    ("demo/plugin.js", "screen: new Program(gl, vertex, assemble(S.SCREEN_FRAGMENT), 'screen'),"),
]

# The C++ the page's pass orchestration (plugin.js, ProcessOpenGL) was ported
# from, line by line. Not a proof of the port: a tripwire for its source.
ORCHESTRATION = [
    ("source/Shaders.h", "inline constexpr int kRadix = 16;"),
    ("source/Penumbra.cpp", "const int tableW  = static_cast< int >( padded ? src.HardwareWidth : src.Width );"),
    ("source/Penumbra.cpp", "const int tableH  = static_cast< int >( padded ? src.HardwareHeight : src.Height );"),
    ("source/Penumbra.cpp", "!table[ 0 ].Ensure( tableW + 1, tableH + 1 ) || !table[ 1 ].Ensure( tableW + 1, tableH + 1 )"),
    ("source/Penumbra.cpp", "drawInto( table[ 0 ], quad );"),
    ("source/Penumbra.cpp", "for( int axis = 0; axis < 2; ++axis )"),
    ("source/Penumbra.cpp", "const long n = axis == 0 ? tableW + 1 : tableH + 1;"),
    ("source/Penumbra.cpp", "for( long stride = 1; stride < n; stride *= kRadix )"),
    ("source/Penumbra.cpp", "const int taps = static_cast< int >( std::min< long >( kRadix, ( n - 1 ) / stride + 1 ) );"),
    ("source/Penumbra.cpp", "drawInto( table[ 1 - from ], quad );"),
    ("source/Penumbra.cpp", "bindUnit( 1, table[ from ].Texture() );"),
    ("source/TableBuffer.cpp",
     "glTexImage2D( GL_TEXTURE_2D, 0, GL_R32UI, w, h, 0, GL_RED_INTEGER, GL_UNSIGNED_INT, nullptr );"),
    ("source/TableBuffer.cpp", "glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST );"),
]

HEADER = """// GENERATED from source/Shaders.cpp by demo/tools/check_shaders.py --write.
// Do not edit: tools/verify.sh fails if a character of this differs from the
// plugin's. The one escape allowed is \\` for a backtick inside a comment.
"""


def read(*parts):
    with open(os.path.join(REPO, *parts)) as handle:
        return handle.read()


def from_cpp(source, symbol):
    """The piece's text: its raw strings, joined as the compiler joins them.

    Returns (text, count of raw strings), or (None, 0) if the symbol is not
    there or what follows its `=` is not a run of adjacent raw strings ending
    in `;`.
    """
    match = re.search(r'const char\* const ' + symbol + r' = ((?:R"\(.*?\)"\s*)+);', source, re.S)
    if match is None:
        return None, 0
    parts = re.findall(r'R"\((.*?)\)"', match.group(1), re.S)
    rebuilt = re.sub(r'R"\(.*?\)"', "", match.group(1), flags=re.S)
    if rebuilt.strip() != "":
        return None, 0
    return "".join(parts), len(parts)


def cpp_pieces(source):
    """Every raw-string piece Shaders.cpp defines, by symbol."""
    return re.findall(r'const char\* const (k\w+) = R"\(', source)


def from_js(source, name):
    match = re.search(r'^export const ' + name + r' = `(.*?)`;$', source, re.S | re.M)
    if match is None:
        return None, None
    body = match.group(1)
    stray = re.search(r"\\(?!`)", body)
    if stray is not None:
        line = body[: stray.start()].count("\n") + 1
        return None, f"backslash that is not an escaped backtick, at line {line}"
    if "${" in body:
        return None, "template substitution"
    return body.replace("\\`", "`"), None


def uniforms(glsl):
    """Every uniform a piece declares, as (type, name), comments stripped
    first. An array (`uniform vec2 Offset[ 3 ];`) counts by its name."""
    code = re.sub(r"//[^\n]*", "", glsl)
    return re.findall(r"^\s*uniform\s+(\w+)\s+(\w+)\s*(?:\[\s*\d+\s*\])?\s*;", code, re.M)


INTEGER_TYPES = re.compile(r"^(int|uint|ivec[234]|uvec[234]|bool|bvec[234]|[iu]?sampler\w+)$")


def write(cpp):
    out = [HEADER]
    for name, symbol in SHADERS:
        body, count = from_cpp(cpp, symbol)
        if body is None:
            print(f"FAIL  {symbol} not found in source/Shaders.cpp (or not a run of adjacent raw strings)")
            return 1
        if "\\" in body or "${" in body:
            print(f"FAIL  {symbol} holds a backslash or ${{; the escape scheme cannot carry it")
            return 1
        joined = f", {count} adjacent raw strings joined" if count > 1 else ""
        out.append(f"\n// {symbol}, source/Shaders.cpp{joined}\nexport const {name} = `{body.replace('`', chr(92) + '`')}`;\n")
    with open(os.path.join(REPO, "demo", "shaders.js"), "w") as handle:
        handle.write("".join(out))
    print("wrote demo/shaders.js")
    return 0


def first_difference(a, b):
    left, right = a.splitlines(), b.splitlines()
    for i in range(max(len(left), len(right))):
        x = left[i] if i < len(left) else "<missing>"
        y = right[i] if i < len(right) else "<missing>"
        if x != y:
            return i + 1, x, y
    return None


def verbatim(entries, what):
    problems = 0
    for path, text in entries:
        try:
            body = read(*path.split("/"))
        except FileNotFoundError:
            print(f"FAIL  {path} is missing")
            problems += 1
            continue
        if text in body:
            print(f"ok    {what}: {path} still says {text.splitlines()[0][:64]}")
        else:
            print(f"FAIL  {what}: {path} no longer says: {text.splitlines()[0]}")
            problems += 1
    return problems


def check(cpp, js):
    problems = 0
    listed = {symbol for _, symbol in SHADERS}
    for symbol in cpp_pieces(cpp):
        if symbol not in listed:
            print(f"FAIL  source/Shaders.cpp defines {symbol}, which this check does not list -- "
                  "add it to SHADERS (and to the page, or say on the page why not)")
            problems += 1

    for name, symbol in SHADERS:
        cpp_text, count = from_cpp(cpp, symbol)
        js_text, complaint = from_js(js, name)
        if cpp_text is None:
            print(f"FAIL  {symbol} not found in source/Shaders.cpp (or not a run of adjacent raw strings)")
            problems += 1
        elif complaint is not None:
            print(f"FAIL  {name} in demo/shaders.js has a {complaint}")
            problems += 1
        elif js_text is None:
            print(f"FAIL  {name} not found in demo/shaders.js")
            problems += 1
        elif cpp_text == js_text:
            joined = f", {count} raw strings joined" if count > 1 else ""
            print(f"ok    {name:<20} matches {symbol} ({len(cpp_text)} chars{joined})")
        else:
            problems += 1
            print(f"FAIL  {name} has drifted from {symbol}")
            where = first_difference(cpp_text, js_text)
            if where:
                print(f"        first difference at line {where[0]}")
                print(f"          C++: {where[1]}")
                print(f"          js : {where[2]}")

    problems += verbatim(ASSEMBLY, "assembly")
    problems += verbatim(ORCHESTRATION, "orchestration")

    try:
        plugin = read("demo", "plugin.js")
    except FileNotFoundError:
        plugin = ""

    for symbol in UNIFORM_PIECES:
        text, _ = from_cpp(cpp, symbol)
        if text is None:
            continue
        declared = uniforms(text)
        # Named as 'Name' or, for an array, element by element as 'Name[0]'.
        unset = [n for _, n in declared if f"'{n}'" not in plugin and f"'{n}[" not in plugin]
        floated = [n for t, n in declared
                   if INTEGER_TYPES.match(t) and re.search(r"\.set\(\s*'" + n + r"[\[']", plugin)]
        if unset:
            print(f"FAIL  {symbol} declares {', '.join(unset)}, which demo/plugin.js never names -- "
                  "the page would leave it at 0")
            problems += 1
        elif floated:
            print(f"FAIL  {symbol}'s integer uniform(s) {', '.join(floated)} go through .set(), which uploads a "
                  "float that GL refuses -- use setInt, setSampler or gl.uniform2i")
            problems += 1
        else:
            print(f"ok    uniforms: all {len(declared)} of {symbol}'s are named in demo/plugin.js, integers as integers")
    return problems


def main(argv):
    cpp = read("source", "Shaders.cpp")
    if "--write" in argv:
        return write(cpp)
    try:
        js = read("demo", "shaders.js")
    except FileNotFoundError:
        print("FAIL  demo/shaders.js is missing -- run demo/tools/check_shaders.py --write")
        return 1
    problems = check(cpp, js)
    print()
    if problems:
        print(f"{problems} problem(s) -- rerun demo/tools/check_shaders.py --write for a drifted copy "
              "(never edit shaders.js by hand); an assembly, orchestration, uniform or unlisted-piece "
              "failure needs a reader")
        return 1
    print(f"all {len(SHADERS)} shader pieces, their assembly, the orchestration's C++ and the uniforms "
          "are the plugin's")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
