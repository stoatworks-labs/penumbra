/**
 * SW Penumbra (PU01, an FFGL mixer), in a browser.
 *
 * Three coloured lamps light the layer below, and this layer stands between
 * them and the screen. Each point of the screen shows the sum of the lamps it
 * can see, so red, green and blue lamps throw cyan, magenta and yellow
 * shadows, black only where all three are blocked.
 *
 * What runs here, and what is a port:
 *
 * - **The shaders are the plugin's.** `shaders.js` is spliced from
 *   source/Shaders.cpp by demo/tools/check_shaders.py --write, unedited, and
 *   each pass is assembled as Penumbra.cpp's InitGL assembles it: kVersion +
 *   kCommon + the pass, over kVersion + kVertex. The kit's port() changes the
 *   version line and adds precision qualifiers (highp int, so the table's
 *   32-bit unsigned sums wrap as GLSL 4.10 says they do); nothing else.
 *   tools/verify.sh fails if a character drifts.
 * - **The CPU half is a port**, between the PORT and FRAME markers below:
 *   Controls.cpp (every conversion from the host's 0..1), Model.cpp (LevelsFor
 *   and Plan: the lamps' shares, offsets and boxes, in double) and the CPU
 *   half of Penumbra.cpp -- its constructor's defaults, asked(), and
 *   ProcessOpenGL, which is the summed-area table's whole pass orchestration:
 *   two R32UI tables one texel bigger than Src each way, NEAREST, the Convert
 *   pass into the first, radix-16 Prefix passes along x then y ping-ponging
 *   the two, then the Screen pass into the canvas, and every uniform.
 *   demo/tools/crosscheck/ compares this page's pictures with putest's; past
 *   that, only a reader checks it.
 * - Not ported: the log (Diag.cpp), the About block, the input guards' log
 *   lines, Plan's fault branches and the harness's negative controls (the
 *   Fault uniform is 0, as shipped).
 *
 * The person (Src) is the page's, not the plugin's: putest's stick figure
 * (tools/putest/Harness.h, figureCover and figureCard), ported to a small
 * GLSL shader below. The white screen is the page's too.
 */
import { mountDemo } from './vendor/demo.js';
import { PassBuffer, Program, bindTexture } from './vendor/gl.js';
import * as S from './shaders.js';

const f32 = Math.fround;

// ==== PORT: Controls.h and Controls.cpp =====================================

const kLamps = 3;
const kRoomMax = 1.0;
const kDistanceMax = 0.9;
const kDistanceDefault = 0.35;
const kLampXMax = 2.0;
const kLampYMax = 1.0;
const kLampSizeMax = 0.5;
const kLampSizeDefault = 0.05;

/** The three lamps' defaults: red, green and blue, in a triangle. */
const kLampDefaults = [
  { r: 1.0, g: 0.0, b: 0.0, x: -0.45, y: -0.15 },
  { r: 0.0, g: 1.0, b: 0.0, x: 0.0, y: 0.3 },
  { r: 0.0, g: 0.0, b: 1.0, x: 0.45, y: -0.15 },
];

const OccluderKind = { Alpha: 0, Luma: 1, Count: 2 };
const ScreenKind = { AsIs: 0, OnWhite: 1, Count: 2 };
const ViewKind = { Picture: 0, Light: 1, Count: 2 };

const clamp = (value, lo, hi) => Math.min(hi, Math.max(lo, value));
/** Controls.cpp's unit(): the host's float, as a double, clamped to 0..1. */
const unit = (value) => clamp(value, 0.0, 1.0);
/** Controls.cpp's toParam(): back to the host's float. */
const toParam = (p) => f32(clamp(p, 0.0, 1.0));
/** std::lround: half away from zero, which Math.round is not. */
const lround = (v) => (v < 0 ? -Math.floor(-v + 0.5) : Math.floor(v + 0.5));

function OptionIndex(value, count) {
  return clamp(lround(value), 0, count - 1);
}

const RoomFromParam = (value) => kRoomMax * unit(value);
const ParamForRoom = (room) => toParam(room / kRoomMax);
const DistanceFromParam = (value) => kDistanceMax * unit(value);
const ParamForDistance = (k) => toParam(k / kDistanceMax);
const LampXFromParam = (value) => kLampXMax * (2.0 * unit(value) - 1.0);
const ParamForLampX = (x) => toParam(0.5 + (0.5 * x) / kLampXMax);
const LampYFromParam = (value) => kLampYMax * (2.0 * unit(value) - 1.0);
const ParamForLampY = (y) => toParam(0.5 + (0.5 * y) / kLampYMax);
function LampSizeFromParam(value) {
  const p = unit(value);
  return kLampSizeMax * p * p;
}
const ParamForLampSize = (size) => toParam(Math.sqrt(Math.max(size, 0.0) / kLampSizeMax));

/** IEC 61966-2-1:1999 (amended 2003), in double, for the CPU's half. */
const DecodeSrgb = (c) => (c <= 0.04045 ? c / 12.92 : Math.pow((c + 0.055) / 1.055, 2.4));

// ==== PORT: Model.cpp =========================================================

/** Q: 255 levels, or fewer when the whole table would not fit 32 bits. */
function LevelsFor(tableW, tableH) {
  const texels = Math.max(1.0, tableW * tableH);
  const fit = Math.floor(4294967295.0 / texels);
  return clamp(fit, 1.0, 255.0);
}

/**
 * The frame's numbers, in double as in the C++ (the shader gets floats: the
 * uniform upload rounds them as static_cast< float > does). The fault
 * branches are not ported: the shipped plugin's fault is 0.
 */
function Plan(asked, outW, outH, tableW, tableH, scaleW, scaleH) {
  const s = {
    asked,
    g: 0.0,
    lamp: [],
    unblocked: [1.0, 1.0, 1.0],
    total: [0.0, 0.0, 0.0],
    outW,
    outH,
    tableW,
    tableH,
    scale: [0.0, 0.0],
    levels: LevelsFor(tableW, tableH),
  };

  const k = clamp(asked.k, 0.0, kDistanceMax);
  s.g = k / (1.0 - k);

  // Occluder texels per output pixel, and per screen height.
  s.scale[0] = scaleW / outW;
  s.scale[1] = scaleH / outH;
  const perHeight = [s.scale[0] * outH, scaleH];

  for (let c = 0; c < 3; c += 1) s.total[c] = asked.room;
  for (let i = 0; i < kLamps; i += 1) {
    s.lamp.push({ linear: [0, 0, 0], share: [0, 0, 0], offset: [0, 0], half: [0, 0], lit: false });
    for (let c = 0; c < 3; c += 1) {
      const picked = clamp(asked.lamp[i].colour[c], 0.0, 1.0);
      s.lamp[i].linear[c] = asked.linear ? DecodeSrgb(picked) : picked;
      s.total[c] += s.lamp[i].linear[c];
    }
  }
  for (let i = 0; i < kLamps; i += 1) {
    const l = s.lamp[i];
    for (let c = 0; c < 3; c += 1) {
      l.share[c] = s.total[c] > 0.0 ? l.linear[c] / s.total[c] : 0.0;
      l.lit = l.lit || l.share[c] > 0.0;
    }
    // Lamp i reads the occluder's picture at P + L_i g.
    l.offset[0] = asked.lamp[i].x * s.g * perHeight[0];
    l.offset[1] = asked.lamp[i].y * s.g * perHeight[1];
    // The lamp, of side s, seen through the occluder's plane: s g.
    const side = asked.lamp[i].size * s.g;
    l.half[0] = 0.5 * side * perHeight[0];
    l.half[1] = 0.5 * side * perHeight[1];
  }
  return s;
}

// ==== END PORT ================================================================

// ==== FRAME: Penumbra.cpp, the CPU half =======================================

const kRadix = 16; // Shaders.h

const LampField = { RED: 0, GREEN: 1, BLUE: 2, X: 3, Y: 4, SIZE: 5, FIELDS: 6 };

const PT = {
  LINEAR_LIGHT: 0,
  ROOM_LIGHT: 1,
  OPACITY: 2,
  OCCLUDER: 3,
  DISTANCE: 4,
  LAMP_FIRST: 5,
  SCREEN: 5 + kLamps * LampField.FIELDS,
  VIEW: 6 + kLamps * LampField.FIELDS,
  ABOUT_FIRST: 7 + kLamps * LampField.FIELDS, // 25; the About block follows in the plugin and is not here
};

const LampParam = (lamp, field) => PT.LAMP_FIRST + lamp * LampField.FIELDS + field;

/** The constructor's defaults, as the host is told them: floats. */
function PenumbraDefaults() {
  const p = new Float32Array(PT.ABOUT_FIRST);
  p[PT.LINEAR_LIGHT] = 1.0;
  p[PT.ROOM_LIGHT] = ParamForRoom(0.0);
  p[PT.OPACITY] = 1.0;
  p[PT.OCCLUDER] = OccluderKind.Alpha;
  p[PT.DISTANCE] = ParamForDistance(kDistanceDefault);
  for (let i = 0; i < kLamps; i += 1) {
    p[LampParam(i, LampField.RED)] = kLampDefaults[i].r;
    p[LampParam(i, LampField.GREEN)] = kLampDefaults[i].g;
    p[LampParam(i, LampField.BLUE)] = kLampDefaults[i].b;
    p[LampParam(i, LampField.X)] = ParamForLampX(kLampDefaults[i].x);
    p[LampParam(i, LampField.Y)] = ParamForLampY(kLampDefaults[i].y);
    p[LampParam(i, LampField.SIZE)] = ParamForLampSize(kLampSizeDefault);
  }
  p[PT.SCREEN] = ScreenKind.AsIs;
  p[PT.VIEW] = ViewKind.Picture;
  return p;
}

/** The constructor's declaration, in index order: name, FF type, elements, group. */
const PENUMBRA_TABLE = (() => {
  const rows = [
    [PT.LINEAR_LIGHT, 'Linear Light', 'boolean', null, 'Light'],
    [PT.ROOM_LIGHT, 'Room Light', 'standard', null, 'Light'],
    [PT.OPACITY, 'Opacity', 'standard', null, 'Occluder'],
    [PT.OCCLUDER, 'Occluder', 'option', ['Alpha', 'Luma'], 'Occluder'],
    [PT.DISTANCE, 'Distance', 'standard', null, 'Occluder'],
  ];
  for (let i = 0; i < kLamps; i += 1) {
    const lamp = `Lamp ${i + 1}`;
    rows.push(
      [LampParam(i, LampField.RED), lamp, 'red', null, lamp],
      [LampParam(i, LampField.GREEN), `${lamp}_Green`, 'green', null, lamp],
      [LampParam(i, LampField.BLUE), `${lamp}_Blue`, 'blue', null, lamp],
      [LampParam(i, LampField.X), `${lamp} X`, 'standard', null, lamp],
      [LampParam(i, LampField.Y), `${lamp} Y`, 'standard', null, lamp],
      [LampParam(i, LampField.SIZE), `${lamp} Size`, 'standard', null, lamp],
    );
  }
  rows.push(
    [PT.SCREEN, 'Screen', 'option', ['As Is', 'On White'], 'Screen'],
    [PT.VIEW, 'View', 'option', ['Picture', 'Light'], 'Screen'],
  );
  return rows.map(([slot, name, kind, elements, group]) => ({ slot, name, kind, elements, group }));
})();

if (PENUMBRA_TABLE.length !== PT.ABOUT_FIRST || PENUMBRA_TABLE.some((row, i) => row.slot !== i)) {
  throw new Error('the parameter table is not in index order');
}

const OFFSET_NAMES = ['Offset[0]', 'Offset[1]', 'Offset[2]'];
const HALF_NAMES = ['HalfSize[0]', 'HalfSize[1]', 'HalfSize[2]'];
const SHARE_NAMES = ['Share[0]', 'Share[1]', 'Share[2]'];

/** glUniform2i by name, counted as missing the way the kit's setters count. */
function setIvec2(program, name, a, b) {
  const location = program.location(name);
  if (location === null) {
    program.missing.add(name);
    return;
  }
  program.gl.uniform2i(location, a | 0, b | 0);
}

/** GLState.h's unbindTextureUnits. */
function unbindTextureUnits(gl, count) {
  for (let u = count - 1; u >= 0; u -= 1) {
    gl.activeTexture(gl.TEXTURE0 + u);
    gl.bindTexture(gl.TEXTURE_2D, null);
  }
}

/** Penumbra.cpp's drawInto: one pass into one half of the table, its whole extent. */
function drawInto(gl, target, quad) {
  gl.bindFramebuffer(gl.FRAMEBUFFER, target.fbo);
  gl.viewport(0, 0, target.width, target.height);
  quad.draw();
}

/**
 * InitGL: AssembleVertex() = kVersion + kVertex for every pass, each fragment
 * stage AssembleFragment( pass ) = kVersion + kCommon + pass.
 */
function compilePrograms(gl) {
  const vertex = S.VERSION + S.VERTEX;
  const assemble = (pass) => S.VERSION + S.COMMON + pass;
  return {
    convert: new Program(gl, vertex, assemble(S.CONVERT_FRAGMENT), 'convert'),
    prefix: new Program(gl, vertex, assemble(S.PREFIX_FRAGMENT), 'prefix'),
    screen: new Program(gl, vertex, assemble(S.SCREEN_FRAGMENT), 'screen'),
  };
}

/**
 * One SW Penumbra instance. `dest` and `src` are { texture, width, height },
 * unpadded, so each one's hardware size is its used size and MaxUV is 1.
 */
class PenumbraInstance {
  constructor(gl, quad, programs) {
    this.gl = gl;
    this.quad = quad;
    this.programs = programs;
    // The plugin's float params[]: a Float32Array rounds as the host's float does.
    this.params = PenumbraDefaults();
    this.table = [new PassBuffer(gl, { filter: 'nearest' }), new PassBuffer(gl, { filter: 'nearest' })];
    this.lastSetup = null;
    this.lastPasses = null;
    this.lastFrom = 0;
  }

  /** The About block's indices are not here: a web page has links of its own. */
  SetFloatParameter(index, value) {
    if (index < 0 || index >= PT.ABOUT_FIRST) return false;
    this.params[index] = value;
    return true;
  }

  asked() {
    const p = this.params;
    const a = {
      linear: p[PT.LINEAR_LIGHT] > 0.5,
      room: RoomFromParam(p[PT.ROOM_LIGHT]),
      opacity: clamp(p[PT.OPACITY], 0.0, 1.0),
      k: DistanceFromParam(p[PT.DISTANCE]),
      occluder: OptionIndex(p[PT.OCCLUDER], OccluderKind.Count),
      screen: OptionIndex(p[PT.SCREEN], ScreenKind.Count),
      view: OptionIndex(p[PT.VIEW], ViewKind.Count),
      lamp: [],
    };
    for (let i = 0; i < kLamps; i += 1) {
      a.lamp.push({
        colour: [p[LampParam(i, LampField.RED)], p[LampParam(i, LampField.GREEN)], p[LampParam(i, LampField.BLUE)]],
        x: LampXFromParam(p[LampParam(i, LampField.X)]),
        y: LampYFromParam(p[LampParam(i, LampField.Y)]),
        size: LampSizeFromParam(p[LampParam(i, LampField.SIZE)]),
      });
    }
    return a;
  }

  /**
   * ProcessOpenGL. The host's framebuffer is `host` (null: the canvas) at the
   * output's viewport. Returns false where the plugin returns FF_FAIL.
   */
  ProcessOpenGL(dest, src, outW, outH, host = null) {
    const gl = this.gl;
    const { convert, prefix, screen } = this.programs;
    if (!dest || !src) return false;
    if (!dest.width || !dest.height || !src.width || !src.height) return false;
    if (outW <= 0 || outH <= 0) return false;

    // The room, as numbers. The table covers Src's used area.
    const tableW = src.width;
    const tableH = src.height;
    const s = Plan(this.asked(), outW, outH, tableW, tableH, tableW, tableH);
    this.lastSetup = s;

    gl.disable(gl.BLEND);

    // The buffers, before anything is bound for the frame.
    this.table[0].ensure(tableW + 1, tableH + 1, gl.R32UI);
    this.table[1].ensure(tableW + 1, tableH + 1, gl.R32UI);

    // 1. Convert: the occluder as integers, into table 0.
    convert.use();
    bindTexture(gl, 0, src.texture);
    convert.setSampler('Occluder', 0);
    convert.setInt('Luma', s.asked.occluder === OccluderKind.Luma ? 1 : 0);
    convert.setInt('Levels', s.levels);
    drawInto(gl, this.table[0], this.quad);

    // 2. Prefix sums, along x then along y, ping-ponging the two tables.
    prefix.use();
    prefix.setSampler('Table', 0);
    const passes = [];
    let from = 0;
    for (let axis = 0; axis < 2; axis += 1) {
      const n = axis === 0 ? tableW + 1 : tableH + 1;
      for (let stride = 1; stride < n; stride *= kRadix) {
        const taps = Math.min(kRadix, Math.floor((n - 1) / stride) + 1);
        bindTexture(gl, 0, this.table[from].texture);
        setIvec2(prefix, 'Step', axis === 0 ? stride : 0, axis === 1 ? stride : 0);
        prefix.setInt('Taps', taps);
        drawInto(gl, this.table[1 - from], this.quad);
        passes.push({ axis, stride, taps });
        from = 1 - from;
      }
    }
    this.lastPasses = passes;
    this.lastFrom = from;

    // 3. The screen, into the host's framebuffer at the host's viewport.
    gl.bindFramebuffer(gl.FRAMEBUFFER, host);
    gl.viewport(0, 0, outW, outH);
    screen.use();
    bindTexture(gl, 0, dest.texture);
    bindTexture(gl, 1, this.table[from].texture);
    screen.setSampler('Screen', 0);
    screen.setSampler('Table', 1);

    // GetMaxGLTexCoords: the used size over the hardware size. The page's
    // textures are unpadded, so their hardware size is their used size.
    const hwWidth = dest.hwWidth ?? dest.width;
    const hwHeight = dest.hwHeight ?? dest.height;
    screen.set('MaxUVScreen', dest.width / hwWidth, dest.height / hwHeight);
    screen.set('HalfTexelScreen', f32(0.5 / dest.width), f32(0.5 / dest.height));
    screen.setInt('ScreenExact', dest.width === outW && dest.height === outH ? 1 : 0);
    setIvec2(screen, 'OutSize', outW, outH);
    setIvec2(screen, 'TableSize', tableW, tableH);
    screen.set('Scale', s.scale[0], s.scale[1]);
    for (let i = 0; i < kLamps; i += 1) {
      const l = s.lamp[i];
      screen.set(OFFSET_NAMES[i], l.offset[0], l.offset[1]);
      screen.set(HALF_NAMES[i], l.half[0], l.half[1]);
      screen.set(SHARE_NAMES[i], l.share[0], l.share[1], l.share[2]);
    }
    screen.set('Unblocked', s.unblocked[0], s.unblocked[1], s.unblocked[2]);
    screen.set('Opacity', s.asked.opacity);
    screen.setInt('Levels', s.levels);
    screen.setInt('Linear', s.asked.linear ? 1 : 0);
    screen.setInt('OnWhite', s.asked.screen === ScreenKind.OnWhite ? 1 : 0);
    screen.setInt('ShowLight', s.asked.view === ViewKind.Light ? 1 : 0);
    screen.setInt('Fault', 0);

    this.quad.draw();

    // Leave nothing bound.
    unbindTextureUnits(gl, 2);
    gl.useProgram(null);
    return true;
  }

  dispose() {
    this.table[0].dispose();
    this.table[1].dispose();
  }
}

// ==== END FRAME ===============================================================

//===========================================================================
// The page's own inputs: the person (Src) and a white screen (Dest). Not the
// plugin's, which is handed whatever layers the operator gives it.
//===========================================================================

const PERSON_VERTEX = `#version 410 core
layout( location = 0 ) in vec4 vPosition;
void main()
{
	gl_Position = vPosition;
}
`;

/**
 * The person: putest's stick figure (tools/putest/Harness.h, figureCover and
 * figureCard) in GLSL. A head, a body, two arms and two legs, as capsules in
 * screen heights from the frame's centre; the alpha is the pixel's coverage,
 * 0.5 - distance / pixel, and the rgb a dark premultiplied grey-blue, so its
 * luma is not its alpha. Kind 1 is the harness's `mask` card (white on black,
 * opaque), Kind 2 is nobody (clear). Lean turns the figure about its feet and
 * Reach lifts its right arm about the shoulder; both 0 is the harness's
 * figure, and the swaying person moves them slowly on the page's clock.
 */
const PERSON_FRAGMENT = `#version 410 core
uniform vec2 Resolution;
uniform float Lean;
uniform float Reach;
uniform int Kind;

out vec4 fragColor;

float capsule( vec2 p, vec2 a, vec2 b, float r )
{
	vec2 v  = b - a;
	float t = clamp( dot( p - a, v ) / dot( v, v ), 0.0, 1.0 );
	return length( p - ( a + t * v ) ) - r;
}

vec2 turn( vec2 p, vec2 about, float angle )
{
	float c = cos( angle );
	float s = sin( angle );
	vec2 d  = p - about;
	return about + vec2( c * d.x - s * d.y, s * d.x + c * d.y );
}

float figureCover( vec2 p )
{
	vec2 hand = Reach == 0.0 ? vec2( 0.22, 0.16 ) : turn( vec2( 0.22, 0.16 ), vec2( 0.08, 0.08 ), Reach );
	float d   = length( p - vec2( 0.0, 0.22 ) ) - 0.075;
	d         = min( d, capsule( p, vec2( 0.0, 0.1 ), vec2( 0.0, -0.12 ), 0.085 ) );
	d         = min( d, capsule( p, vec2( -0.08, 0.08 ), vec2( -0.24, -0.06 ), 0.028 ) );
	d         = min( d, capsule( p, vec2( 0.08, 0.08 ), hand, 0.028 ) );
	d         = min( d, capsule( p, vec2( -0.04, -0.14 ), vec2( -0.08, -0.46 ), 0.036 ) );
	d         = min( d, capsule( p, vec2( 0.04, -0.14 ), vec2( 0.09, -0.46 ), 0.036 ) );
	return d;
}

void main()
{
	if( Kind == 2 )
	{
		fragColor = vec4( 0.0 );
		return;
	}
	vec2 px = floor( gl_FragCoord.xy );
	vec2 s  = ( px + 0.5 - 0.5 * Resolution ) / Resolution.y;
	if( Lean != 0.0 )
		s = turn( s, vec2( 0.005, -0.46 ), -Lean );
	float a = clamp( 0.5 - figureCover( s ) * Resolution.y, 0.0, 1.0 );
	if( Kind == 1 )
		fragColor = vec4( vec3( a ), 1.0 );
	else
		fragColor = vec4( 0.15 * a, 0.2 * a, 0.3 * a, a );
}
`;

/**
 * The person's pose at `time` seconds: a lean of up to 0.06 rad (3.4°) about
 * the feet, once every 14 s, and the right arm lifted by up to 0.35 rad (20°)
 * and lowered again every 9 s. Slow on purpose; both are 0 at time 0.
 */
const SWAY_LEAN = 0.06;
const SWAY_LEAN_PERIOD = 14.0;
const SWAY_REACH = 0.35;
const SWAY_REACH_PERIOD = 9.0;
function poseAt(time) {
  return {
    lean: SWAY_LEAN * Math.sin((2.0 * Math.PI * time) / SWAY_LEAN_PERIOD),
    reach: SWAY_REACH * (0.5 - 0.5 * Math.cos((2.0 * Math.PI * time) / SWAY_REACH_PERIOD)),
  };
}

const PEOPLE = [
  { id: 'sway', name: 'Stick figure, swaying', kind: 0, sways: true, hint: 'putest’s stick figure in alpha, leaning slowly about its feet and lifting one arm, so the shadows move. Generated in this page: not a keyed performer.' },
  { id: 'still', name: 'Stick figure, still', kind: 0, sways: false, hint: 'putest’s figure card exactly as the harness draws it: the figure in alpha, its rgb a dark grey-blue that is never drawn.' },
  { id: 'mask', name: 'White-on-black mask', kind: 1, sways: false, hint: 'putest’s mask card: the figure white on black, opaque, no alpha — the shape a person mask arrives in over NDI. Set Occluder to Luma; on Alpha an opaque layer is a wall.' },
  { id: 'nobody', name: 'Nobody (a clear layer)', kind: 2, sways: false, hint: 'A clear layer: nothing blocks any lamp, so the screen comes back untouched, to the bit.' },
];
const PERSON_DEFAULT = 'sway';

class PersonRenderer {
  constructor(gl, quad) {
    this.gl = gl;
    this.quad = quad;
    this.program = new Program(gl, PERSON_VERTEX, PERSON_FRAGMENT, 'person');
    this.buffer = new PassBuffer(gl, { filter: 'linear' });
  }

  render(id, width, height, time) {
    const gl = this.gl;
    const person = PEOPLE.find((p) => p.id === id) ?? PEOPLE[0];
    const pose = person.sways ? poseAt(time) : { lean: 0, reach: 0 };
    this.buffer.ensure(width, height, gl.RGBA8);
    this.buffer.bind();
    gl.disable(gl.BLEND);
    this.program.use();
    this.program.set('Resolution', width, height);
    this.program.set('Lean', pose.lean);
    this.program.set('Reach', pose.reach);
    this.program.setInt('Kind', person.kind);
    this.quad.draw();
    gl.bindFramebuffer(gl.FRAMEBUFFER, null);
    return { texture: this.buffer.texture, width, height, pose };
  }
}

/** A white screen at the output's size: putest's `white` card. */
class WhiteScreen {
  constructor(gl) {
    this.gl = gl;
    this.buffer = new PassBuffer(gl, { filter: 'linear' });
    this.size = '';
  }

  render(width, height) {
    const key = `${width}x${height}`;
    if (this.size !== key) {
      this.buffer.ensure(width, height, this.gl.RGBA8);
      this.buffer.clearTo(1, 1, 1, 1);
      this.gl.bindFramebuffer(this.gl.FRAMEBUFFER, null);
      this.size = key;
    }
    return { texture: this.buffer.texture, width, height };
  }
}

//===========================================================================
// For demo/tools/crosscheck/ and headless checks.
//===========================================================================

/** An RGBA8 picture given top row first, uploaded as a host hands one over. */
function uploadFrame(gl, rgba, width, height) {
  const texture = gl.createTexture();
  gl.bindTexture(gl.TEXTURE_2D, texture);
  gl.texStorage2D(gl.TEXTURE_2D, 1, gl.RGBA8, width, height);
  gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.LINEAR);
  gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.LINEAR);
  gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
  gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
  const row = width * 4;
  const flipped = new Uint8Array(row * height);
  for (let y = 0; y < height; y += 1) flipped.set(rgba.subarray(y * row, (y + 1) * row), (height - 1 - y) * row);
  gl.pixelStorei(gl.UNPACK_ALIGNMENT, 1);
  gl.pixelStorei(gl.UNPACK_FLIP_Y_WEBGL, false);
  gl.pixelStorei(gl.UNPACK_PREMULTIPLY_ALPHA_WEBGL, false);
  gl.texSubImage2D(gl.TEXTURE_2D, 0, 0, 0, width, height, gl.RGBA, gl.UNSIGNED_BYTE, flipped);
  gl.bindTexture(gl.TEXTURE_2D, null);
  return { texture, width, height };
}

/** RGBA8 out of a framebuffer, top row first. */
function readFrame(gl, fbo, width, height) {
  gl.bindFramebuffer(gl.FRAMEBUFFER, fbo);
  const row = width * 4;
  const read = new Uint8Array(row * height);
  gl.pixelStorei(gl.PACK_ALIGNMENT, 1);
  gl.readPixels(0, 0, width, height, gl.RGBA, gl.UNSIGNED_BYTE, read);
  const pixels = new Uint8Array(row * height);
  for (let y = 0; y < height; y += 1) pixels.set(read.subarray(y * row, (y + 1) * row), (height - 1 - y) * row);
  gl.bindFramebuffer(gl.FRAMEBUFFER, null);
  return pixels;
}

/** The uniforms a program declares that nothing has set: each would sit at 0. */
function unsetUniforms(gl, program) {
  const set = new Set([...program.locations].filter(([, loc]) => loc !== null).map(([name]) => name));
  const out = [];
  const count = gl.getProgramParameter(program.program, gl.ACTIVE_UNIFORMS);
  for (let i = 0; i < count; i += 1) {
    const name = gl.getActiveUniform(program.program, i).name;
    if (!set.has(name) && !set.has(name.replace(/\[0\]$/, ''))) out.push(name);
  }
  return out;
}

function installHook(gl, quad, programs, person) {
  window.penumbraDemo = {
    table: PENUMBRA_TABLE.map((row) => ({
      name: row.name,
      kind: row.kind,
      value: PenumbraDefaults()[row.slot],
      elements: row.elements ?? [],
    })),

    /**
     * A FRESH instance (putest's is one too), the settings by the plugin's
     * display name (an option by its element's name), one frame of the given
     * RGBA8 pictures (top row first), read back the same way. The draw and
     * the read are one task, so the page's own frames cannot interleave.
     */
    renderCase({ settings = {}, width, height, dest, src, srcWidth, srcHeight }) {
      const run = new PenumbraInstance(gl, quad, programs);
      const made = [];
      try {
        for (const [name, value] of Object.entries(settings)) {
          const info = PENUMBRA_TABLE.find((row) => row.name === name);
          if (!info) throw new Error(`no parameter called '${name}'`);
          const index = info.elements ? info.elements.indexOf(value) : -1;
          const number = index >= 0 ? index : Number(value);
          if (!Number.isFinite(number)) throw new Error(`'${value}' is not a number or an option of ${name}`);
          run.SetFloatParameter(info.slot, number);
        }
        const destTex = uploadFrame(gl, dest, width, height);
        const srcTex = uploadFrame(gl, src, srcWidth ?? width, srcHeight ?? height);
        made.push(destTex.texture, srcTex.texture);
        gl.canvas.width = width;
        gl.canvas.height = height;
        gl.bindFramebuffer(gl.FRAMEBUFFER, null);
        gl.viewport(0, 0, width, height);
        gl.disable(gl.BLEND);
        gl.clearColor(0, 0, 0, 0);
        gl.clear(gl.COLOR_BUFFER_BIT);
        const ok = run.ProcessOpenGL(destTex, srcTex, width, height, null);
        const pixels = readFrame(gl, null, width, height);
        const unset = Object.entries(programs).flatMap(([k, p]) => unsetUniforms(gl, p).map((u) => `${k}: ${u}`));
        const missing = Object.entries(programs).flatMap(([k, p]) => [...p.missing].map((u) => `${k}: ${u}`));
        return { ok, pixels, missing, unset, error: gl.getError(), passes: run.lastPasses, levels: run.lastSetup?.levels };
      } finally {
        for (const t of made) gl.deleteTexture(t);
        run.dispose();
      }
    },

    /** The page's person, RGBA8 top row first, for comparison with putest's cards. */
    person(id, width, height, time = 0) {
      const out = person.render(id, width, height, time);
      return readFrame(gl, person.buffer.fbo, out.width, out.height);
    },

    /**
     * The table the page's own last frame built, as RGBA_INTEGER /
     * UNSIGNED_INT (the readback WebGL2 guarantees for an R32UI target),
     * red channel only, bottom row first as GL stores it.
     */
    readTable() {
      const inst = window.penumbraPage?.instance;
      if (!inst?.lastSetup) return null;
      const t = inst.table[inst.lastFrom];
      gl.bindFramebuffer(gl.FRAMEBUFFER, t.fbo);
      const raw = new Uint32Array(t.width * t.height * 4);
      gl.readPixels(0, 0, t.width, t.height, gl.RGBA_INTEGER, gl.UNSIGNED_INT, raw);
      gl.bindFramebuffer(gl.FRAMEBUFFER, null);
      const red = new Uint32Array(t.width * t.height);
      for (let i = 0; i < red.length; i += 1) red[i] = raw[i * 4];
      return { width: t.width, height: t.height, levels: inst.lastSetup.levels, red, error: gl.getError() };
    },
  };
}

//===========================================================================
// The parameters: the plugin's own, in its declaration's order and groups,
// with its names, element lists and defaults. Hints from the README.
//===========================================================================

const signed = (n, d) => `${n >= 0 ? '+' : '−'}${Math.abs(n).toFixed(d)}`;
const V = (v) => f32(v);

const LAMP_COLOUR_HINT = 'The lamp’s colour as picked (sRGB, decoded to linear light with Linear Light on); its level is the lamp’s brightness. Black turns the lamp off, shadow and all.';

const SPEC = [
  ['linearLight', PT.LINEAR_LIGHT, null, 'On: the lamps add, and the clip is lit, in linear light: the picked colours and the clip are decoded from sRGB, multiplied by the light and encoded again. Off does the same arithmetic on the gamma-encoded values. Parameter 0: Resolume Arena does not show a mixer’s first parameter (measured on genlock, wipe, relay, lenticular and pepper, not yet on SW Penumbra), so in Arena this stays on. A browser does not hide it.'],
  ['roomLight', PT.ROOM_LIGHT, (v) => `${RoomFromParam(V(v)).toFixed(2)} white lamp`, 'White light nobody blocks, 0 to 1 of one full white lamp, in linear light: the floor under every shadow. At 0 a point that sees no lamp is black.'],
  ['opacity', PT.OPACITY, (v) => `×${unit(V(v)).toFixed(2)}`, 'The person’s opacity: it scales how much of each lamp is blocked, in linear light. In Resolume Arena a mixer parameter named Opacity is bound to the layer’s opacity fader (measured on other mixers, not yet on SW Penumbra), so fading the person’s layer brings the shadows in. Here it is a slider.'],
  ['occluder', PT.OCCLUDER, null, 'Which part of this layer is the person: Alpha (a keyed performer, any clip with alpha), or Luma (the BT.709 luma of its rgb, for a white-on-black mask with no alpha). With Alpha an opaque mask is a wall and blocks every lamp. The person’s colours are never drawn.'],
  ['distance', PT.DISTANCE, (v) => { const k = DistanceFromParam(V(v)); return `k ${k.toFixed(2)}, g ${(k / (1 - k)).toFixed(2)}`; }, 'How far from the screen the person stands, as k, the fraction of the way to the lamps, 0 to 0.9. With g = k / (1 − k) the shadows part by the lamps’ spacing times g and each edge is its lamp’s size times g. At 0 all three shadows are the silhouette, hard and black.'],
];
for (let i = 0; i < kLamps; i += 1) {
  const n = i + 1;
  SPEC.push(
    [`lamp${n}`, LampParam(i, LampField.RED), null, LAMP_COLOUR_HINT],
    [`lamp${n}Green`, LampParam(i, LampField.GREEN), null, ''],
    [`lamp${n}Blue`, LampParam(i, LampField.BLUE), null, ''],
    [`lamp${n}X`, LampParam(i, LampField.X), (v) => `${signed(LampXFromParam(V(v)), 3)} heights`, 'Where the lamp stands in its plane, −2 to 2 screen heights across from the reference point. A lamp to the right throws its shadow left.'],
    [`lamp${n}Y`, LampParam(i, LampField.Y), (v) => `${signed(LampYFromParam(V(v)), 3)} heights`, '−1 to 1 screen heights up from the reference point. A lamp above throws its shadow down.'],
    [`lamp${n}Size`, LampParam(i, LampField.SIZE), (v) => { const size = LampSizeFromParam(V(v)); return size > 0 ? `${size.toFixed(4)} heights` : 'a point'; }, 'The square lamp’s side, 0 to 0.5 screen heights, as 0.5 p², so the small sizes, where an edge goes from hard to soft, get most of the slider. 0 is a point lamp and a hard shadow.'],
  );
}
SPEC.push(
  ['screen', PT.SCREEN, null, 'As Is: the output’s alpha is the clip’s, so where the clip is transparent no shadow shows. On White: the clip printed on a white screen (premultiplied over white, in gamma space), opaque, so the shadows fall on white where the clip is clear. Pick the “Shape on transparency” screen clip to see the difference.'],
  ['view', PT.VIEW, null, 'Picture: the clip, lit. Light: the light on the screen alone, as if the clip were white, for setting up.'],
);

const PARAMS = SPEC.map(([id, slot, display, hint]) => {
  const types = { boolean: 'boolean', standard: 'standard', option: 'option', red: 'colour', green: 'colour', blue: 'colour' };
  const row = PENUMBRA_TABLE[slot];
  const p = { id, slot, name: row.name, type: types[row.kind], default: PenumbraDefaults()[slot], group: row.group, hint };
  if (row.elements) p.elements = row.elements;
  if (display) p.display = display;
  return p;
});

//===========================================================================
// The page.
//===========================================================================

const query = new URLSearchParams(window.location.search);
const REPO = 'https://github.com/stoatworks-labs/penumbra';
const WHITE = 'white';
// The screen clips: the white screen first (the page's, the harness's
// `white` card), then the kit's. The kit's own clip picker lists only its
// SOURCES, so the white screen is added to it after mounting (below).
const SCREEN_CLIPS = [WHITE, 'scene', 'bars100', 'grid', 'ramp', 'alpha', 'spot', 'detail'];

const page = { mounted: null, instance: null, stats: '' };

function createRenderer(gl, quad) {
  const programs = compilePrograms(gl);
  const instance = new PenumbraInstance(gl, quad, programs);
  const person = new PersonRenderer(gl, quad);
  const white = new WhiteScreen(gl);
  page.instance = instance;
  installHook(gl, quad, programs, person);

  return {
    render({ input, params, width, height, time, variant }) {
      // The host restates every parameter each frame.
      for (const p of PARAMS) instance.SetFloatParameter(p.slot, params.get(p.id));

      // Dest, the layer below: the screen. The kit has already drawn its clip
      // into `input`; the white screen is the page's own texture.
      const dest = page.mounted?.state?.sourceId === WHITE ? white.render(width, height) : input;
      // Src, this layer: the person, at the output's raster.
      const src = person.render(variant ?? PERSON_DEFAULT, width, height, time);

      gl.bindFramebuffer(gl.FRAMEBUFFER, null);
      gl.viewport(0, 0, width, height);
      gl.disable(gl.BLEND);

      instance.ProcessOpenGL(dest, src, width, height, null);

      const s = instance.lastSetup;
      if (!s) return;
      const perAxis = [0, 1].map((a) => instance.lastPasses.filter((p) => p.axis === a).map((p) => p.stride).join(', '));
      const lamps = s.lamp.map((l, i) => {
        if (!l.lit) return `lamp ${i + 1} off`;
        const a = s.asked.lamp[i];
        // The shadow moves by −L g and its edge is s g, in output pixels.
        return `lamp ${i + 1}’s shadow moved (${signed(-a.x * s.g * height, 0)}, ${signed(-a.y * s.g * height, 0)}) px, edge ${(a.size * s.g * height).toFixed(1)} px`;
      });
      const pose = src.pose.lean !== 0 || src.pose.reach !== 0
        ? ` · the person leaning ${signed((src.pose.lean * 180) / Math.PI, 1)}°, arm up ${((src.pose.reach * 180) / Math.PI).toFixed(1)}°`
        : '';
      page.stats = [
        `table ${s.tableW + 1} × ${s.tableH + 1} R32UI ×2, Q ${s.levels}; convert, prefix x at strides ${perAxis[0]}, prefix y at ${perAxis[1]}, screen`,
        `g ${s.g.toFixed(3)}`,
        ...lamps,
      ].join(' · ') + pose;
    },
  };
}

const COMMON_GAPS = {
  es: 'GLSL ES 3.00 in WebGL2, not desktop GL 4.1 core. The shader text is the plugin’s; only the version line and precision qualifiers are changed by the kit. The table is R32UI, written by the Convert and Prefix passes and read with texelFetch through a highp usampler2D: WebGL2 guarantees 32-bit highp integers in the fragment stage, so the box sums wrap exactly as GLSL 4.10 says they do, and are the same integers. The float weights over each box’s part texels are the driver’s rounding, which on another GPU can move a shadow’s edge by a level.',
  presets: 'The presets are the page’s. The plugin declares none; each is only a combination of the plugin’s own parameters.',
  about: 'The About block — a text line and link buttons for the host’s panel — is absent: a web page has links of its own. So are the plugin’s log file and the harness’s negative controls (the Fault uniform is 0, as shipped). There is no audio path, so nothing here is missing for want of one.',
};

const mounted = mountDemo({
  name: 'SW Penumbra',
  kind: 'mixer',
  pluginId: 'PU01',
  tagline:
    'Coloured shadows as an FFGL mixer: three coloured lamps light the layer below and this layer stands in front of them, the way the museum exhibit does. Red, green and blue lamps throw cyan, magenta and yellow shadows, black only where all three are blocked; each shadow is the part of its lamp a point of the screen can see, by similar triangles, so the shadows part and soften as the person steps towards the lamps, and wherever nobody stands the clip comes back untouched.',
  repo: REPO,
  blurb:
    'It is SW Penumbra’s own GLSL — its three passes, ported from the repository to WebGL2 unedited — run by a JavaScript port of the plugin’s CPU half: the summed-area table’s pass orchestration, the lamps’ shares and boxes, and every conversion. SW Penumbra is a mixer, so it needs two pictures: the screen (Dest, the layer below: a white screen, the kit’s clips or a picture of your own) and the person (Src, this layer). The person is a stick figure generated in this page, the one the plugin’s test harness uses, not a keyed performer. In Resolume the person’s layer fader would be Opacity; here it is a slider.',
  sources: SCREEN_CLIPS,
  variants: {
    label: 'Person',
    default: PERSON_DEFAULT,
    options: PEOPLE.map((p) => ({ id: p.id, name: p.name, hint: `Src, this layer: ${p.hint}` })),
  },
  params: PARAMS,
  // The plugin declares no presets. These are the page's: combinations of
  // the plugin's own parameters and nothing else.
  presets: {
    'At the screen (Distance 0): one hard black shadow': { distance: ParamForDistance(0.0) },
    'Towards the lamps (Distance 0.6): the shadows part': { distance: ParamForDistance(0.6) },
    'Point lamps: hard edges': { lamp1Size: 0, lamp2Size: 0, lamp3Size: 0 },
    'Big lamps: soft edges, pale arms': { lamp1Size: ParamForLampSize(0.2), lamp2Size: ParamForLampSize(0.2), lamp3Size: ParamForLampSize(0.2) },
    'Amber, teal and violet lamps': { lamp1: 1, lamp1Green: 0.75, lamp1Blue: 0, lamp2: 0, lamp2Green: 0.5, lamp2Blue: 0.5, lamp3: 0.5, lamp3Green: 0, lamp3Blue: 1 },
    'Two white lamps: grey shadows': { lamp1: 1, lamp1Green: 1, lamp1Blue: 1, lamp2: 0, lamp2Green: 0, lamp2Blue: 0, lamp3: 1, lamp3Green: 1, lamp3Blue: 1 },
    'Room Light 0.3: never quite black': { roomLight: ParamForRoom(0.3) },
    'The light alone (View: Light)': { view: ViewKind.Light },
    'Gamma-space arithmetic (Linear Light off)': { linearLight: 0 },
  },
  differences: [
    'Two inputs, both generated here. The kit this page is built on hands a demo one input; SW Penumbra is a mixer. The screen (Dest, the layer below) is the kit’s clip picker, renamed Screen, with a plain white screen added first — the page’s own white texture at the output’s size, the harness’s `white` card — and “Use my own…” replaces it with a picture of yours, decoded in the page and never uploaded. The person (Src, this layer) is the Person dropdown.',
    'The person is a generated figure, not a keyed performer. It is the stick figure of the plugin’s offline harness (putest’s figure card: a head, a body, two arms and two legs, drawn in alpha), ported to a small GLSL shader of the page’s own: still, gently swaying (a lean of up to 3.4° about its feet every 14 s and one arm lifting by up to 20° every 9 s, on the page’s clock), as the harness’s white-on-black mask, or nobody at all. No camera, keyer or hub person mask is anywhere in this page, and the person cannot be a picture of your own.',
    'The person’s colours are never drawn: the plugin reads only its alpha, or with Occluder on Luma the BT.709 luma of its rgb. The mask with Occluder on Alpha blocks every lamp everywhere — an opaque layer is a wall — which is what the plugin does with it.',
    'Opacity and Linear Light are on the panel. In Resolume Arena a mixer parameter named Opacity is bound to the layer’s opacity fader, and a mixer’s first parameter — Linear Light here, which is why it is first and on — is hidden. That is the fleet’s mixer behaviour measured on genlock, wipe, relay, lenticular and pepper; SW Penumbra itself has never been loaded into Resolume, so it is not confirmed for this plugin.',
    'The pass orchestration is a port. The three shaders are the plugin’s, unedited. What runs them is translated to JavaScript by hand in demo/plugin.js from Penumbra.cpp’s ProcessOpenGL: two R32UI tables one texel bigger than the person each way, with NEAREST filters; the Convert pass into the first; radix-16 Prefix passes along x then along y (strides 1, 16, 256 …, each with its own tap count), ping-ponging the two; then the Screen pass, with every uniform. So are Controls.cpp’s conversions and Model.cpp’s Plan — the lamps’ shares, offsets and boxes, in double, and Q. demo/tools/crosscheck/ rendered the page and putest, the plugin’s own harness, from the same frames in the cases AGENTS.md lists; outside those, only a reader checks it. demo/tools/check_shaders.py fails the repository’s verify script if a character of the shaders drifts, or if the C++ lines the orchestration was ported from change.',
    'MaxUV is 1. Resolume hands a mixer padded textures, and the shader applies Dest’s MaxUV and half-texel clamp when Dest is not the output’s size. Here the screen, the person and the output are always one size and unpadded (a picture of your own sets the output to its size), so the screen is always fetched texel for texel, the sampled path is never taken, and the person’s table is never cut from a padded texture — the arrangement the plugin’s --mixer check exists for.',
    'The clock is the page’s, and only the inputs use it. The plugin has no time-driven behaviour at all; Pause, Step and Restart move the kit’s clips and the swaying figure, never the shadows’ maths.',
    'The lamps’ colour swatches are 8 bits a channel, as an HTML colour input is; the plugin takes any float the host sends.',
    COMMON_GAPS.es,
    'The line under the picture is not the plugin. It reports what the port computed for this frame — the table’s size, Q and passes, g, and where each lamp’s shadow lands and how wide its edge is — and the figure’s pose. The plugin draws no such thing.',
    COMMON_GAPS.presets,
    COMMON_GAPS.about,
    'Nothing here is measured. The plugin’s numerical proof — each shadow exactly the sum of the lamps it can see (0 channel values wrong in 223,096 pixels), each edge the width similar triangles give to 0.018 px, every pixel within 2.1e-6 of a 3D ray cast, the screen back bit for bit where nothing is blocked, 37 wrong versions and 29 one-character mutants each caught — is tools/putest in the repository, and that harness, not this page, is the reason to believe the shadows.',
  ],
  showBackdrop: true,
  createRenderer,
});

page.mounted = mounted ?? null;

//---------------------------------------------------------------------------
// The kit labels its clip picker "Clip". With two inputs that is ambiguous,
// so it is renamed for what it feeds, the screen (Dest), and the white screen
// is added to it: the kit lists only its own SOURCES, skips an id it does not
// know, and accepts any id in `sources` from ?clip=, so the page's id rides
// through state.sourceId and the renderer swaps in the white texture (the
// kit still draws its first clip into `input` underneath, unused). The
// variant dropdown is the person (Src); `?person=<id>` picks it, the way the
// kit's `?clip=` picks the screen.
//---------------------------------------------------------------------------
for (const label of document.querySelectorAll('.transport__label')) {
  if (label.textContent === 'Clip') {
    label.textContent = 'Screen';
    label.title = 'Dest, the layer below: the clip printed on the screen the lamps light.';
    const select = label.closest('.transport__field')?.querySelector('select');
    if (select) {
      const option = new Option('White screen', WHITE);
      option.title = 'A plain white screen, the museum exhibit’s wall: the harness’s white card.';
      select.prepend(option);
      select.value = mounted?.state?.sourceId ?? WHITE;
    }
    const personField = [...document.querySelectorAll('.transport__label')]
      .find((l) => l.textContent === 'Person')?.closest('.transport__field');
    const screenField = label.closest('.transport__field');
    if (personField && screenField) personField.before(screenField);
  }
}
const fileLabel = document.querySelector('.transport__file');
if (fileLabel) fileLabel.title = 'A picture or a video from this machine as the screen (Dest). It is decoded in the page and never uploaded anywhere.';

const wantedPerson = query.get('person');
if (mounted?.state && wantedPerson && PEOPLE.some((p) => p.id === wantedPerson)) {
  mounted.state.variant = wantedPerson;
  const select = [...document.querySelectorAll('.transport__label')]
    .find((l) => l.textContent === 'Person')?.closest('.transport__field')?.querySelector('select');
  if (select) select.value = wantedPerson;
}

// The line under the picture.
const statLine = document.createElement('p');
statLine.className = 'stage__status';
statLine.setAttribute('aria-live', 'off');
statLine.dataset.role = 'penumbra-stats';
document.querySelector('.stage')?.append(statLine);
if (statLine.isConnected) {
  const tick = () => {
    if (statLine.textContent !== page.stats) statLine.textContent = page.stats;
    requestAnimationFrame(tick);
  };
  requestAnimationFrame(tick);
}

// For a headless check: the mounted page.
window.penumbraPage = page;

