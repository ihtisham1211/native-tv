#!/usr/bin/env python3
"""Check the QR code on a capture of the handoff page is the code qrcodegen makes for the link.

No QR decoder is available to the tests, so this reads the pixels instead: every module of the
code, sampled at its centre, must be what qrcodegen's matrix says, on whole-pixel modules with
the quiet zone around them. Skips (77) where the machine has no GL, like the golden tests.

  qr_capture.py CLIENT QR_MATRIX
"""
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, "golden"))
import golden  # noqa: E402
import setup_fixture  # noqa: E402


def main():
    client, matrix_tool = sys.argv[1], sys.argv[2]
    env = golden.client_environment()
    if not golden.has_gl(client, env):
        print("skipped: no GL context on this machine")
        return golden.SKIP
    text = subprocess.run([matrix_tool, setup_fixture.HANDOFF_URL], capture_output=True, text=True, check=True).stdout
    header, *rows = text.split("\n")
    fits, modules, scale, left, top = map(int, header.split())
    rows = [row for row in rows if row]
    assert fits and len(rows) == modules and scale >= 3, (fits, modules, scale)
    with tempfile.TemporaryDirectory() as directory:
        output = os.path.join(directory, "handoff.ppm")
        case = {"name": "qr", "time": 2600, "fixture": "setup-settled", "args": ["--screen", "setup:handoff"]}
        golden.render(client, case, output, env)
        width, height, pixels = golden.read_ppm(output)

    def at(x, y):
        offset = (y * width + x) * 3
        return pixels[offset:offset + 3]

    def accent(colour):  # the scan line crossing the code
        r, g, b = colour
        return b > r + 40 and b > 150

    def dark(x, y):
        # A module spans `scale` rows; the 3 px scan line can hide some of them, not all.
        for row in range(scale):
            colour = at(x, y + row)
            if not accent(colour):
                return sum(colour) < 3 * 128
        raise AssertionError("the scan line hides a whole module")

    # The four quiet modules, and the tile around them, are white.
    quiet = 4 * scale
    for x, y in ((left, top), (left + quiet - 1, top + quiet - 1), (left + (modules + 8) * scale - 1, top + 3)):
        assert sum(at(x, y)) > 3 * 240, "the quiet zone is white at %d,%d" % (x, y)
    wrong = 0
    for y in range(modules):
        for x in range(modules):
            px, py = left + quiet + x * scale + scale // 2, top + quiet + y * scale
            if dark(px, py) != (rows[y][x] == "1"):
                wrong += 1
    assert wrong == 0, "%d of %d modules differ from qrcodegen's matrix" % (wrong, modules * modules)
    # Crisp: the module edges are whole pixels, so the pixel before and after a run is the other colour.
    for y in range(0, modules, 7):
        for x in range(1, modules):
            if rows[y][x] != rows[y][x - 1]:
                edge = left + quiet + x * scale
                py = top + quiet + y * scale + scale // 2
                if accent(at(edge, py)):
                    continue
                assert (sum(at(edge - 1, py)) < 3 * 128) == (rows[y][x - 1] == "1") and \
                       (sum(at(edge, py)) < 3 * 128) == (rows[y][x] == "1"), "soft module edge at %d,%d" % (edge, py)
    print("qr_capture: %d x %d modules of %d px match qrcodegen's matrix on the capture" % (modules, modules, scale))
    return 0


if __name__ == "__main__":
    sys.exit(main())
