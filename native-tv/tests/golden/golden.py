#!/usr/bin/env python3
"""Golden-image tests for the TV client.

  golden.py check   --client BIN [--name NAME] [--out DIR]
  golden.py update  --client BIN [--name NAME]
  golden.py compare EXPECTED ACTUAL [--tolerance N] [--max-bad FRACTION] [--diff OUT.png]

`check` renders every case in cases.json with `--time` (a fixed clock, so the
animation is at the same phase every run), then compares the PPM the client
captured with <name>.png beside this file. A pixel is bad when a channel
differs by more than the tolerance; a case passes when the share of bad pixels
is at most maxBad. On failure it writes <name>.actual.png and <name>.diff.png
(bad pixels red, the rest the expected image at a quarter brightness) to --out.

Exit codes: 0 pass, 1 mismatch or error, 77 skipped (no GL on this machine;
NATIVETV_REQUIRE_GL=1 turns that into a failure).
"""
import argparse
import contextlib
import json
import os
import struct
import subprocess
import sys
import tempfile
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
SKIP = 77
NO_GL_EXITS = (10, 11)  # the client's --probe: no display, no context


# ---- images -----------------------------------------------------------------

def read_ppm(path):
    with open(path, "rb") as file:
        data = file.read()
    fields, position = [], 0
    while len(fields) < 4:
        while data[position:position + 1].isspace():
            position += 1
        end = position
        while not data[end:end + 1].isspace():
            end += 1
        fields.append(data[position:end])
        position = end
    magic, width, height, maximum = fields
    if magic != b"P6" or maximum != b"255":
        raise ValueError(path + ": not an 8-bit binary PPM")
    width, height = int(width), int(height)
    pixels = data[position + 1:]
    if len(pixels) != width * height * 3:
        raise ValueError(path + ": truncated PPM")
    return width, height, pixels


def png_chunk(kind, body):
    return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body))


def write_png(path, width, height, pixels):
    stride = width * 3
    raw = b"".join(b"\0" + pixels[y * stride:(y + 1) * stride] for y in range(height))
    with open(path, "wb") as file:
        file.write(b"\x89PNG\r\n\x1a\n")
        file.write(png_chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)))
        file.write(png_chunk(b"IDAT", zlib.compress(raw, 9)))
        file.write(png_chunk(b"IEND", b""))


def read_png(path):
    """8-bit RGB or RGBA, not interlaced; any filter. Returns width, height, RGB bytes."""
    with open(path, "rb") as file:
        data = file.read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError(path + ": not a PNG")
    position, header, compressed = 8, None, []
    while position < len(data):
        length, kind = struct.unpack(">I4s", data[position:position + 8])
        body = data[position + 8:position + 8 + length]
        position += 12 + length
        if kind == b"IHDR":
            header = struct.unpack(">IIBBBBB", body)
        elif kind == b"IDAT":
            compressed.append(body)
    width, height, depth, color, _, _, interlace = header
    if depth != 8 or color not in (2, 6) or interlace:
        raise ValueError(path + ": only 8-bit RGB/RGBA PNGs that are not interlaced are read")
    step = 3 if color == 2 else 4
    stride = width * step
    raw = zlib.decompress(b"".join(compressed))
    rows, previous = [], bytearray(stride)
    for y in range(height):
        kind = raw[y * (stride + 1)]
        row = bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        if kind == 1:
            for i in range(step, stride):
                row[i] = (row[i] + row[i - step]) & 255
        elif kind == 2:
            row = bytearray((a + b) & 255 for a, b in zip(row, previous))
        elif kind == 3:
            for i in range(stride):
                left = row[i - step] if i >= step else 0
                row[i] = (row[i] + (left + previous[i]) // 2) & 255
        elif kind == 4:
            for i in range(stride):
                a = row[i - step] if i >= step else 0
                b, c = previous[i], (previous[i - step] if i >= step else 0)
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                predictor = a if pa <= pb and pa <= pc else b if pb <= pc else c
                row[i] = (row[i] + predictor) & 255
        elif kind != 0:
            raise ValueError(path + ": unknown PNG filter")
        rows.append(row)
        previous = row
    if step == 4:
        rows = [bytearray(b for i, b in enumerate(row) if i % 4 != 3) for row in rows]
    return width, height, b"".join(rows)


def read_image(path):
    return read_png(path) if path.endswith(".png") else read_ppm(path)


# ---- comparison -------------------------------------------------------------

def compare(expected, actual, tolerance, max_bad, diff_path=None):
    """Returns (ok, message). expected and actual are (width, height, RGB bytes)."""
    (width, height, want), (actual_width, actual_height, got) = expected, actual
    if (width, height) != (actual_width, actual_height):
        return False, "size %dx%d, expected %dx%d" % (actual_width, actual_height, width, height)
    stride, bad, worst = width * 3, 0, 0
    diff = bytearray()
    for y in range(height):
        want_row, got_row = want[y * stride:(y + 1) * stride], got[y * stride:(y + 1) * stride]
        if want_row == got_row:
            diff += bytes(b >> 2 for b in want_row) if diff_path else b""
            continue
        row = bytearray(b >> 2 for b in want_row) if diff_path else None
        for x in range(0, stride, 3):
            largest = max(abs(want_row[x] - got_row[x]), abs(want_row[x + 1] - got_row[x + 1]),
                          abs(want_row[x + 2] - got_row[x + 2]))
            worst = max(worst, largest)
            if largest > tolerance:
                bad += 1
                if row is not None:
                    row[x:x + 3] = b"\xff\x00\x00"
        diff += row if row is not None else b""
    share = bad / float(width * height)
    ok = share <= max_bad
    if not ok and diff_path:
        write_png(diff_path, width, height, bytes(diff))
    return ok, "%d of %d pixels differ by more than %d (%.4f%%, allowed %.4f%%), largest difference %d" % (
        bad, width * height, tolerance, share * 100, max_bad * 100, worst)


# ---- running the client -----------------------------------------------------

def client_environment():
    env = dict(os.environ)
    # A Linux machine without a display renders through Mesa's offscreen EGL.
    if sys.platform.startswith("linux") and not (env.get("DISPLAY") or env.get("WAYLAND_DISPLAY")):
        env.setdefault("SDL_VIDEODRIVER", "offscreen")
        # SDL's offscreen window leaves an EGL allocation inside Mesa at exit, which
        # an ASan build reports as the client's leak; heap errors are still caught.
        env.setdefault("ASAN_OPTIONS", "detect_leaks=0")
    return env


def has_gl(client, env):
    """True when the client can open a context here; False means skip."""
    probe = subprocess.run([client, "--probe", "--windowed"], env=env, capture_output=True, text=True)
    if probe.returncode == 0:
        return True
    if probe.returncode in NO_GL_EXITS and not os.environ.get("NATIVETV_REQUIRE_GL"):
        return False
    sys.exit("the client's --probe failed (%d): %s" % (probe.returncode, probe.stdout.strip()))


def render(client, case, output, env):
    """A case may name a `fixture` (a stand-in daemon from tests/: "home", "dashboard", "setup", "setup-settled") and a `script` of timed keys."""
    server = "http://127.0.0.1:9"
    with contextlib.ExitStack() as stack:
        if case.get("fixture") == "home":
            sys.path.insert(0, os.path.join(HERE, ".."))
            import home_fixture
            server = stack.enter_context(home_fixture.serve(profiles=case.get("profiles", 1)))
        elif case.get("fixture") == "dashboard":
            sys.path.insert(0, os.path.join(HERE, ".."))
            import dashboard_fixture
            server = stack.enter_context(dashboard_fixture.serve())
        elif case.get("fixture", "").startswith("setup"):
            # The first-run routes over a wiped data root; "setup-settled" is a TV that has done it.
            sys.path.insert(0, os.path.join(HERE, ".."))
            import setup_fixture
            root = stack.enter_context(tempfile.TemporaryDirectory())
            server, _ = stack.enter_context(setup_fixture.serve(
                os.path.join(root, "data"), pending=case["fixture"] != "setup-settled"))
        extra = []
        if case.get("script"):
            directory = stack.enter_context(tempfile.TemporaryDirectory())
            path = os.path.join(directory, "script.json")
            with open(path, "w") as file:
                json.dump(case["script"], file)
            extra = ["--script", path]
        # Not --windowed: the TV draws 1920x1080, and a minified texture (the splash
        # mark at 1280x720) samples differently on every GPU.
        command = [client, "--server", server, "--time=%d" % case["time"],
                   "--capture", output] + extra + case.get("args", [])
        done = subprocess.run(command, env=env, capture_output=True, text=True, timeout=120)
    if done.returncode != 0:
        raise RuntimeError("%s: the client exited %d: %s" % (case["name"], done.returncode, done.stderr.strip()))


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("command", choices=["check", "update", "compare"])
    parser.add_argument("files", nargs="*", help="compare: EXPECTED ACTUAL")
    parser.add_argument("--client")
    parser.add_argument("--name", help="only this case")
    parser.add_argument("--out", help="where captures and diffs go (default: a temporary directory)")
    parser.add_argument("--cases", default=os.path.join(HERE, "cases.json"))
    parser.add_argument("--tolerance", type=int)
    parser.add_argument("--max-bad", type=float)
    parser.add_argument("--diff")
    args = parser.parse_args()

    with open(args.cases) as file:
        config = json.load(file)
    tolerance = args.tolerance if args.tolerance is not None else config["tolerance"]
    max_bad = args.max_bad if args.max_bad is not None else config["maxBad"]

    if args.command == "compare":
        ok, message = compare(read_image(args.files[0]), read_image(args.files[1]), tolerance, max_bad, args.diff)
        print(("same: " if ok else "different: ") + message)
        return 0 if ok else 1

    if not args.client:
        parser.error("--client is required")
    env = client_environment()
    if not has_gl(args.client, env):
        print("skipped: no GL context on this machine")
        return SKIP
    out = args.out or tempfile.mkdtemp(prefix="nativetv-golden-")
    os.makedirs(out, exist_ok=True)
    failed = 0
    for case in config["cases"]:
        if args.name and case["name"] != args.name:
            continue
        golden = os.path.join(HERE, case["name"] + ".png")
        capture = os.path.join(out, case["name"] + ".ppm")
        render(args.client, case, capture, env)
        actual = read_ppm(capture)
        if args.command == "update":
            write_png(golden, *actual)
            print("updated " + golden)
            continue
        diff = os.path.join(out, case["name"] + ".diff.png")
        ok, message = compare(read_png(golden), actual, case.get("tolerance", tolerance),
                              case.get("maxBad", max_bad), diff)
        print("%s %s: %s" % ("ok  " if ok else "FAIL", case["name"], message))
        if not ok:
            failed += 1
            write_png(os.path.join(out, case["name"] + ".actual.png"), *actual)
            print("     actual and diff images in " + out)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
