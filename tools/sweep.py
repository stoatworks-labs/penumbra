"""Render every parameter at both ends of its range and fail if any made no
difference.

**This is the only thing in the repo that catches a dead control.** A GLSL
uniform whose name does not match the C++ is silently ignored --
`glGetUniformLocation` returns -1 and `glUniform` on -1 is a documented no-op
-- so a slider can be stone dead while everything compiles, links, loads and
renders.

Each control is swept where it can act: one frame (the plugin has no clock)
at 320x180, CI's raster, through the real plugin, on the defaults: putest's
`screen` card as Dest and its `figure` card (a person-shaped alpha, dark rgb)
as Src. An option is set by its element name. Screen acts only on a Dest
with alpha, so its context is the `screen-alpha` card. A control whose ends
differ by less than `--floor` (mean 8-bit difference per channel) is barely
alive: vectrix's lesson.

Usage::

    tools/sweep.py [--build BUILD_DIR] [--verbose] [--jobs N] [--bare]

`--bare` drops every per-control context (keeping only the base), the
addendum's honesty check: what goes dead there is the list of controls that
only act with another one set, which AGENTS.md records.
"""

import argparse
import concurrent.futures
import pathlib
import re
import subprocess
import sys
import tempfile
import zlib

REPO = pathlib.Path(__file__).resolve().parent.parent

BASE = []

# name -> (low settings, high settings, context settings). A setting is a
# "Name=value" string (passed as --set) or a list of raw arguments.
SWEEP = {
    "Linear Light": (["Linear Light=0"], ["Linear Light=1"], []),
    "Room Light": (["Room Light=0"], ["Room Light=1"], []),
    "Opacity": (["Opacity=0"], ["Opacity=1"], []),
    "Occluder": (["Occluder=Alpha"], ["Occluder=Luma"], []),
    "Distance": (["Distance=0.1"], ["Distance=0.9"], []),
    "Screen": (["Screen=As Is"], ["Screen=On White"], [["--input-a", "screen-alpha"]]),
    "View": (["View=Picture"], ["View=Light"], []),
}
for lamp in (1, 2, 3):
    for suffix in ("", "_Green", "_Blue"):
        name = f"Lamp {lamp}{suffix}"
        SWEEP[name] = ([f"{name}=0"], [f"{name}=1"], [])
    for field in ("X", "Y"):
        name = f"Lamp {lamp} {field}"
        SWEEP[name] = ([f"{name}=0.3"], [f"{name}=0.7"], [])
    name = f"Lamp {lamp} Size"
    SWEEP[name] = ([f"{name}=0"], [f"{name}=1"], [])

# The About block: a text line and browser buttons, with no pixel to sweep.
SKIP = {"About", "Project page", "Source on GitHub", "Support the work", "User guide"}


def read_png(path):
    """Enough of PNG for putest's own writer: 8-bit RGBA, filter 0 rows."""
    data = path.read_bytes()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("not a PNG")
    pos, width, height, idat = 8, 0, 0, b""
    while pos < len(data):
        length = int.from_bytes(data[pos:pos + 4], "big")
        kind = data[pos + 4:pos + 8]
        body = data[pos + 8:pos + 8 + length]
        if kind == b"IHDR":
            width = int.from_bytes(body[0:4], "big")
            height = int.from_bytes(body[4:8], "big")
        elif kind == b"IDAT":
            idat += body
        pos += 12 + length
    raw = zlib.decompress(idat)
    stride = width * 4
    out = bytearray()
    for y in range(height):
        start = y * (stride + 1)
        out += raw[start + 1:start + 1 + stride]
    return bytes(out)


def difference(a, b):
    if len(a) != len(b):
        return 255.0
    return sum(abs(x - y) for x, y in zip(a, b)) / len(a)


def render(putest, out, settings):
    args = [str(putest), "--out", str(out), "--size", "320x180", "--frames", "1"]
    for setting in settings:
        args += list(setting) if isinstance(setting, list) else ["--set", setting]
    result = subprocess.run(args, capture_output=True, text=True)
    if result.returncode != 0:
        raise RuntimeError(f"putest failed: {' '.join(args)}\n{result.stderr.strip()}")
    return read_png(out)


def parameters(putest):
    result = subprocess.run([str(putest), "--list"], capture_output=True, text=True)
    if result.returncode != 0:
        raise RuntimeError(f"putest --list failed: {result.stderr.strip()}")
    names = []
    for line in result.stdout.splitlines()[1:]:
        parts = re.split(r"\s{2,}", line.strip())
        if len(parts) >= 3 and parts[0].isdigit():
            names.append(parts[1].strip())
    return names


def sweep_one(putest, scratch, name, bare):
    low, high, context = SWEEP[name]
    context = BASE + ([] if bare else context)
    tag = f"p{abs(hash(name))}"
    before = render(putest, scratch / f"{tag}-a.png", context + low)
    after = render(putest, scratch / f"{tag}-b.png", context + high)
    return name, difference(before, after)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--build", type=pathlib.Path, default=REPO / "build")
    parser.add_argument("--verbose", action="store_true")
    parser.add_argument("--bare", action="store_true")
    parser.add_argument("--jobs", type=int, default=4)
    parser.add_argument("--floor", type=float, default=0.05,
                        help="mean 8-bit difference below which a control is 'barely alive'")
    args = parser.parse_args()

    build = args.build if args.build.is_absolute() else REPO / args.build
    putest = build / "putest"
    if not putest.exists():
        print(f"{putest} not found", file=sys.stderr)
        return 1

    declared = parameters(putest)
    unknown = [n for n in declared if n not in SWEEP and n not in SKIP]
    if unknown:
        # A new parameter with no sweep is a hole, not a pass.
        print(f"no sweep defined for: {', '.join(unknown)}", file=sys.stderr)
        return 1
    missing = [n for n in SWEEP if n not in declared]
    if missing:
        # A sweep for a parameter that no longer exists is a stale table.
        print(f"swept but not declared: {', '.join(missing)}", file=sys.stderr)
        return 1
    dead, weak = [], []
    with tempfile.TemporaryDirectory() as scratch:
        scratch = pathlib.Path(scratch)
        with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
            names = [n for n in declared if n in SWEEP]
            results = list(pool.map(lambda n: sweep_one(putest, scratch, n, args.bare), names))
    for name, delta in results:
        if delta == 0.0:
            dead.append(name)
            print(f"  DEAD {name:16s} both ends identical")
        elif delta < args.floor:
            weak.append(name)
            print(f"  WEAK {name:16s} mean delta {delta:.4f}")
        elif args.verbose:
            print(f"  ok   {name:16s} mean delta {delta:.3f}")
    print(f"SW Penumbra: {len(results)} parameters swept{' (bare: base context only)' if args.bare else ''}, "
          f"{len(dead)} dead, {len(weak)} barely alive")
    if dead or weak:
        if not args.bare:
            print("\nA parameter that changes nothing is usually a uniform name that does not match\n"
                  "the C++, or a setting nothing reads. Both are silent everywhere else.", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
