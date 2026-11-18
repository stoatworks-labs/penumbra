"""The browser demo against the plugin: the same frames, two renderers, one picture.

    python3 demo/tools/crosscheck/crosscheck.py [--putest build-verify/putest] [--size 640x360] [--keep DIR]

Each case below is rendered twice:

  - by the PLUGIN: `putest --out` (the real Penumbra class in a GL 4.1
    context), with one of the harness's own cards as Dest (the screen) and
    one as Src (the person). Build putest first; tools/verify.sh leaves one
    in build-verify/.
  - by the PAGE: demo/ served from here and loaded in headless Chrome,
    through window.penumbraDemo.renderCase, which makes a fresh instance and
    renders the same two pictures with the same settings.

The pictures are the harness's, taken from putest itself rather than rebuilt
here: a card put through the plugin as Dest over a clear Src comes back to
the bit (the plugin's --unchanged check), so `putest --out --input-a <card>
--input-b clear` IS the card. The same route gives the figure and mask
cards, which the page's own person shader (plugin.js) is compared with too.

The GPU half is the plugin's GLSL (check_shaders.py), so what this mostly
tests is the PORT: Controls.cpp's conversions, Model.cpp's Plan, and the pass
orchestration and every uniform of Penumbra.cpp's ProcessOpenGL -- the R32UI
tables, the radix-16 prefix passes, the screen pass. A difference is a port
bug, or a difference between GL 4.1 on this machine and WebGL2 through ANGLE
on the same GPU. Before any picture, the page's table of controls (names,
kinds, defaults, elements) is compared with `putest --list`.

A case passes when no pixel is more than --max levels of 255 apart and
--exact % are identical. Negative controls -- the page with one control one
slider step (0.001) off -- are compared with the plugin's picture as it
should be, and each must fail that test, so the comparison is shown to see a
slip of that size.

Measured on an M4 Max (GL 4.1 against Chrome's ANGLE on Metal), 18 November
2026: see AGENTS.md, "Browser demo", for the figures.

Not run by tools/verify.sh: it needs a GPU and Chrome. Exit 1 if a table
differs, a uniform is missing or unset, a case fails or a negative control
passes.
"""
import argparse
import functools
import http.server
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import threading
import zlib

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
DEMO = os.path.abspath(os.path.join(HERE, "..", ".."))
REPO = os.path.abspath(os.path.join(DEMO, ".."))
sys.dont_write_bytecode = True

# name, settings as `putest --set` takes them (host values; options by element
# name), Dest card, Src card, and Src's size as a share of the output's.
CASES = [
    ("defaults-white", {}, "white", "figure", 1),
    ("defaults-screen", {}, "screen", "figure", 1),
    ("as-is-alpha", {}, "screen-alpha", "figure", 1),
    ("on-white", {"Screen": "On White"}, "screen-alpha", "figure", 1),
    ("view-light", {"View": "Light"}, "screen", "figure", 1),
    ("linear-off", {"Linear Light": "0"}, "screen", "figure", 1),
    ("at-the-screen", {"Distance": "0"}, "white", "figure", 1),
    ("distance-max", {"Distance": "1"}, "white", "figure", 1),
    ("point-lamps", {"Lamp 1 Size": "0", "Lamp 2 Size": "0", "Lamp 3 Size": "0", "Distance": "0.6"}, "white", "figure", 1),
    ("big-lamps", {"Lamp 1 Size": "1", "Lamp 2 Size": "0.7", "Lamp 3 Size": "0.4", "Distance": "0.6"}, "screen", "figure", 1),
    ("lamps-moved", {"Lamp 1 X": "0.1", "Lamp 2 Y": "0.95", "Lamp 3 X": "0.9", "Lamp 3 Y": "0.2"}, "white", "figure", 1),
    ("amber-teal-violet-room", {"Lamp 1": "1", "Lamp 1_Green": "0.75", "Lamp 1_Blue": "0",
                                "Lamp 2": "0", "Lamp 2_Green": "0.5", "Lamp 2_Blue": "0.5",
                                "Lamp 3": "0.5", "Lamp 3_Green": "0", "Lamp 3_Blue": "1",
                                "Room Light": "0.3", "Opacity": "0.6"}, "screen", "figure", 1),
    ("two-whites-one-black", {"Lamp 1_Green": "1", "Lamp 1_Blue": "1", "Lamp 2_Green": "0",
                              "Lamp 3": "1", "Lamp 3_Green": "1"}, "white", "figure", 1),
    ("luma-mask", {"Occluder": "Luma"}, "white", "mask", 1),
    ("alpha-mask-wall", {}, "screen", "mask", 1),
    ("nobody", {}, "screen-alpha", "clear", 1),
    ("src-half-size", {"Distance": "0.5"}, "screen", "figure", 0.5),
]
# The negative controls: a case rendered by the page with one setting one
# slider step off, compared with the plugin's render of the case as it should
# be. Each is the size of a plausible port slip.
NEGATIVES = [
    ("defaults-white", "distance-1-step", {"Distance": "0.3899"}, "Distance one slider step (0.001) off"),
    ("defaults-white", "lamp-x-1-step", {"Lamp 1 X": "0.3885"}, "Lamp 1 X one step off"),
    ("defaults-white", "lamp-size-1-step", {"Lamp 2 Size": "0.3172"}, "Lamp 2 Size one step off"),
    # One Room Light step (0.001) moves no pixel more than one level: inside
    # the tolerance by design, so this control is ten steps.
    ("amber-teal-violet-room", "room-10-steps", {"Room Light": "0.31"}, "Room Light ten steps (0.01) off"),
    ("defaults-screen", "opacity-1-step", {"Opacity": "0.999"}, "Opacity one step off"),
]
CARDS_NEEDED = sorted({c[2] for c in CASES} | {c[3] for c in CASES})


def parse_size(text):
    w, h = text.lower().split("x")
    return int(w), int(h)


def run_putest(putest, args, env):
    done = subprocess.run([putest, *args], capture_output=True, check=False, env=env)
    if done.returncode != 0:
        sys.exit(f"putest {' '.join(args)} failed: {done.stderr.decode(errors='replace').strip()}")
    return done.stdout


def read_png(path):
    """putest's own PNGs: 8-bit RGBA, filter 0 on every row, top row first."""
    with open(path, "rb") as handle:
        data = handle.read()
    pos, idat, width, height = 8, b"", 0, 0
    while pos < len(data):
        length = int.from_bytes(data[pos:pos + 4], "big")
        kind = data[pos + 4:pos + 8]
        body = data[pos + 8:pos + 8 + length]
        if kind == b"IHDR":
            width, height = int.from_bytes(body[0:4], "big"), int.from_bytes(body[4:8], "big")
            if body[8:10] != b"\x08\x06":
                sys.exit(f"{path}: not 8-bit RGBA")
        elif kind == b"IDAT":
            idat += body
        pos += 12 + length
    raw = np.frombuffer(zlib.decompress(idat), np.uint8).reshape(height, 1 + width * 4)
    if raw[:, 0].any():
        sys.exit(f"{path}: a row with a PNG filter; putest writes filter 0")
    return raw[:, 1:].tobytes(), width, height


def putest_frame(putest, work, env, name, size, sets=(), dest="white", src="clear", src_size=None):
    png = os.path.join(work, f"{name}.png")
    args = ["--out", png, "--input-a", dest, "--input-b", src, "--size", size, *sets]
    if src_size:
        args += ["--src-size", src_size]
    run_putest(putest, args, env)
    pixels, w, h = read_png(png)
    if f"{w}x{h}" != size:
        sys.exit(f"putest --out for {name} wrote {w}x{h}, not {size}")
    return pixels


# ---------------------------------------------------------------- the server
class Handler(http.server.SimpleHTTPRequestHandler):
    """demo/ as it is, plus /xc/ for the job, the frames and the page's answers."""

    def __init__(self, *args, state, **kwargs):
        self.state = state
        super().__init__(*args, directory=DEMO, **kwargs)

    def log_message(self, *args):
        pass

    def do_GET(self):
        if self.path == "/xc/job.json":
            return self.reply(json.dumps(self.state["job"]).encode(), "application/json")
        if self.path.startswith("/xc/frame/"):
            return self.reply(self.state["frames"][self.path[len("/xc/frame/"):]], "application/octet-stream")
        return super().do_GET()

    def do_POST(self):
        body = self.rfile.read(int(self.headers.get("Content-Length", 0)))
        if self.path.startswith("/xc/out/"):
            self.state["page"][self.path[len("/xc/out/"):]] = body
        elif self.path == "/xc/table":
            self.state["table"] = json.loads(body)
        elif self.path == "/xc/missing":
            self.state["missing"] = json.loads(body)
        elif self.path == "/xc/error":
            self.state["error"] = body.decode(errors="replace")
            self.state["finished"].set()
        elif self.path == "/xc/done":
            self.state["finished"].set()
        self.reply(b"ok", "text/plain")

    def reply(self, body, kind):
        self.send_response(200)
        self.send_header("Content-Type", kind)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)


# ---------------------------------------------------------------- the judging
def compare(a, b, width, height):
    x = np.frombuffer(a, np.uint8).reshape(height, width, 4).astype(np.int16)
    y = np.frombuffer(b, np.uint8).reshape(height, width, 4).astype(np.int16)
    worst = np.abs(x - y).max(axis=2)
    return {
        "exact": float((worst == 0).mean() * 100.0),
        "within1": float((worst <= 1).mean() * 100.0),
        "within4": float((worst <= 4).mean() * 100.0),
        "max": int(worst.max()),
        "mean": float(np.abs(x - y)[..., :3].mean()),
    }


def passes(s, args):
    return s["max"] <= args.max and s["exact"] >= args.exact


LIST_ROW = re.compile(r"^(\d+)\s+(.+?)\s{2,}(boolean|standard|option|red|green|blue|about)\s+(\S+)\s+(\[[^\]]*\]|-)\s*(.*)$")


def compare_table(putest, page_table, env):
    """`putest --list` against the page's table: name, kind, default (4 dp), elements, in order."""
    rows = []
    for line in run_putest(putest, ["--list"], env).decode().splitlines()[1:]:
        m = LIST_ROW.match(line.rstrip())
        if m is None:
            continue
        index, name, kind, value, _, elements = m.groups()
        if kind == "about":
            continue
        rows.append((int(index), name, kind, float(value), [e.strip() for e in elements.split("|")] if elements else []))
    problems = []
    if len(rows) != len(page_table):
        problems.append(f"putest lists {len(rows)} controls before the About block, the page {len(page_table)}")
    for (index, name, kind, value, elements), mine in zip(rows, page_table):
        if (name, kind) != (mine["name"], mine["kind"]) or abs(value - mine["value"]) > 0.00005 or elements != mine["elements"]:
            problems.append(f"#{index}: putest says {name} {kind} {value:.4f} {elements}, the page "
                            f"{mine['name']} {mine['kind']} {mine['value']:.4f} {mine['elements']}")
    return len(rows), problems


def main(argv):
    parser = argparse.ArgumentParser()
    parser.add_argument("--putest", default=os.path.join(REPO, "build-verify", "putest"))
    parser.add_argument("--size", default="640x360")
    parser.add_argument("--keep", help="write each case's two pictures here as PNGs")
    parser.add_argument("--max", type=int, default=1,
                        help="the largest difference, in levels of 255, allowed anywhere (default 1)")
    parser.add_argument("--exact", type=float, default=99.0,
                        help="share of pixels, %%, that must be identical (default 99)")
    parser.add_argument("--chrome", default=os.environ.get(
        "CHROME", "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome"))
    parser.add_argument("--port", type=int, default=0, help="the local server's port (default: any free one)")
    args = parser.parse_args(argv)
    width, height = parse_size(args.size)
    if not os.access(args.putest, os.X_OK):
        sys.exit(f"no putest at {args.putest}: build it (tools/verify.sh, or cmake --build build --target putest)")

    work = tempfile.mkdtemp(prefix="penumbra-xc-")
    # The harness writes the plugin's log; keep it out of ~/Library/Logs.
    env = {**os.environ, "PENUMBRA_LOG_DIR": os.path.join(work, "log")}
    size = f"{width}x{height}"
    frame_bytes = width * height * 4

    # The harness's cards, through the plugin over a clear Src: the card itself.
    frames = {}
    for share in sorted({c[4] for c in CASES}):
        sw, sh = round(width * share), round(height * share)
        for card in CARDS_NEEDED:
            frames[f"{card}-{sw}x{sh}"] = putest_frame(args.putest, work, env, f"card-{card}-{sw}x{sh}", f"{sw}x{sh}", dest=card)

    plugin, cases = {}, []
    for name, settings, dest, src, share in CASES:
        sw, sh = round(width * share), round(height * share)
        sets = [x for k, v in settings.items() for x in ("--set", f"{k}={v}")]
        plugin[name] = putest_frame(args.putest, work, env, name, size, sets, dest=dest, src=src,
                                    src_size=f"{sw}x{sh}")
        cases.append({"name": name, "settings": settings, "width": width, "height": height,
                      "dest": f"{dest}-{width}x{height}", "src": f"{src}-{sw}x{sh}", "srcWidth": sw, "srcHeight": sh})
    by_name = {c["name"]: c for c in cases}
    for base, name, change, _ in NEGATIVES:
        cases.append({**by_name[base], "name": name, "settings": {**by_name[base]["settings"], **change}})

    # The page's own person against the harness's cards it was ported from.
    people = [("still", "figure"), ("mask", "mask"), ("nobody", "clear")]
    job = {"cases": cases, "people": [{"id": pid, "width": width, "height": height} for pid, _ in people]}

    state = {"job": job, "frames": frames, "page": {}, "table": None, "missing": None,
             "error": None, "finished": threading.Event()}
    server = http.server.ThreadingHTTPServer(("127.0.0.1", args.port), functools.partial(Handler, state=state))
    port = server.server_address[1]
    threading.Thread(target=server.serve_forever, daemon=True).start()
    profile = tempfile.mkdtemp(prefix="penumbra-chrome-")
    chrome = subprocess.Popen([
        args.chrome, "--headless=new", "--no-first-run", "--no-default-browser-check",
        f"--user-data-dir={profile}", "--use-angle=metal", "--enable-gpu", "--ignore-gpu-blocklist",
        "--window-size=1600,1200", f"http://127.0.0.1:{port}/tools/crosscheck/",
    ], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        if not state["finished"].wait(600):
            sys.exit("the page never finished (600 s)")
    finally:
        chrome.terminate()
        try:
            chrome.wait(10)
        except subprocess.TimeoutExpired:
            chrome.kill()
        server.shutdown()
        shutil.rmtree(profile, ignore_errors=True)
    if state["error"]:
        shutil.rmtree(work, ignore_errors=True)
        sys.exit(f"the page failed:\n{state['error']}")

    failures = 0
    count, table_problems = compare_table(args.putest, state["table"] or [], env)
    if table_problems:
        failures += len(table_problems)
        print("the page's controls differ from putest --list:")
        for line in table_problems:
            print(f"  {line}")
    else:
        print(f"controls: all {count} of putest --list's before the About block, same names, kinds, defaults and elements, in order")
    gaps = state["missing"] or {}
    for label, key in (("uniforms the page set that a program lacks", "missing"),
                       ("uniforms a program has that the page never set", "unset"), ("GL errors", "errors"),
                       ("frames the instance refused", "refused")):
        if gaps.get(key):
            failures += 1
            print(f"{label}: {', '.join(sorted(set(gaps[key])))}")
    if not any(gaps.get(k) for k in ("missing", "unset", "errors", "refused")):
        print("every uniform set, none missing, no GL error, no frame refused")
    if gaps.get("passes"):
        print(f"prefix passes at {size}: {gaps['passes']}")

    if args.keep:
        from PIL import Image
        os.makedirs(args.keep, exist_ok=True)

    print(f"\n{size}, one frame each\n")
    print(f"{'case':<26} {'==':>9} {'<=1':>9} {'<=4':>9} {'max':>4} {'mean':>7}")
    for name in [c[0] for c in CASES]:
        got = state["page"].get(name)
        if got is None or len(got) != frame_bytes:
            print(f"{name:<26} no picture from the page")
            failures += 1
            continue
        s = compare(plugin[name], got, width, height)
        ok = passes(s, args)
        failures += 0 if ok else 1
        print(f"{name:<26} {s['exact']:8.3f}% {s['within1']:8.3f}% {s['within4']:8.3f}% {s['max']:>4} {s['mean']:7.4f}  {'ok' if ok else 'FAIL'}")
        if args.keep:
            for label, data in (("plugin", plugin[name]), ("page", got)):
                Image.frombytes("RGBA", (width, height), data).save(os.path.join(args.keep, f"{name}-{label}.png"))

    print("\nthe page's person against the harness's card it was ported from")
    for pid, card in people:
        got = state["page"].get(f"person-{pid}")
        s = compare(frames[f"{card}-{width}x{height}"], got, width, height) if got else None
        ok = s is not None and passes(s, args)
        failures += 0 if ok else 1
        print(f"  {pid:<8} vs {card:<8} " + (f"{s['exact']:8.3f}% {s['within1']:8.3f}% {s['max']:>4} {s['mean']:7.4f}  "
                                             f"{'ok' if ok else 'FAIL'}" if s else "no picture"))

    print("\nnegative controls: the page a little off, against the plugin as it should be")
    for base, name, _, label in NEGATIVES:
        late = state["page"].get(name)
        s = compare(plugin[base], late, width, height) if late else None
        caught = s is not None and not passes(s, args)
        failures += 0 if caught else 1
        print(f"  {label:<36} " + (f"{s['exact']:8.3f}% {s['within1']:8.3f}% {s['max']:>4} {s['mean']:7.4f}  "
                                     f"{'caught' if caught else 'NOT CAUGHT'}" if s else "no picture"))
    print(f"\n{'every case passes, every negative control is caught' if not failures else f'{failures} problem(s)'}"
          f" (pass: no pixel more than {args.max} level(s) apart and {args.exact}% identical)")
    shutil.rmtree(work, ignore_errors=True)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
