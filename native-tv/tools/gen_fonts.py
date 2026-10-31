#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.9"
# dependencies = ["fonttools>=4.50"]
# ///
"""Cut the TV client's fonts from Google Sans Flex into native-tv/assets/fonts.

The client draws text with FreeType from static TTFs (src/graphics/glyph_atlas.cpp), so the
variable font Google Fonts publishes is instanced here into three Latin cuts on the type scale
the kit uses at 1080p (design/tokens.json, dp x 2): a display cut at optical size 36 for the
titles of 48 px and more, and regular and medium text cuts at optical size 18. Each cut is
about 58 KB. The source is the google/fonts repository at a pinned commit, with its licence.

    tools/gen_fonts.py            # writes the three TTFs and LICENSE-GoogleSansFlex-OFL.txt
"""
import sys
import tempfile
import urllib.request
from pathlib import Path

from fontTools.subset import Options, Subsetter
from fontTools.ttLib import TTFont
from fontTools.varLib.instancer import instantiateVariableFont

sys.path.insert(0, str(Path(__file__).resolve().parent))
from woff_to_ttf import relayout_ttf  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
OUTPUT = ROOT / "assets" / "fonts"
COMMIT = "bd8f81ddb5c74d5c8897b36ad88b440266245103"
SOURCE = "https://raw.githubusercontent.com/google/fonts/%s/ofl/googlesansflex/" % COMMIT
VARIABLE = "GoogleSansFlex[ROND,opsz,slnt,wdth,wght].ttf"
CUTS = {
    "GoogleSansFlex-Display.ttf": {"opsz": 36, "wght": 400},
    "GoogleSansFlex-Regular.ttf": {"opsz": 18, "wght": 400},
    "GoogleSansFlex-Medium.ttf": {"opsz": 18, "wght": 500},
}
# Latin, its punctuation and the few symbols the screens use (middle dot, ellipsis, arrows,
# the euro and trademark signs).
LATIN = ("U+0000-00FF,U+0131,U+0152-0153,U+02BB-02BC,U+02C6,U+02DA,U+02DC,U+0300-0301,"
         "U+0303-0304,U+0308-0309,U+0323,U+0329,U+2000-206F,U+2074,U+20AC,U+2122,U+2191,"
         "U+2193,U+2212,U+2215,U+FEFF,U+FFFD")


def codepoints(ranges):
    for item in ranges.split(","):
        first, _, last = item[2:].partition("-")
        for value in range(int(first, 16), int(last or first, 16) + 1):
            yield value


def fetch(name, folder):
    path = folder / name
    with urllib.request.urlopen(SOURCE + urllib.request.quote(name), timeout=60) as response:
        path.write_bytes(response.read())
    return path


def main():
    OUTPUT.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory() as temporary:
        folder = Path(temporary)
        source = fetch(VARIABLE, folder)
        (OUTPUT / "LICENSE-GoogleSansFlex-OFL.txt").write_bytes(fetch("OFL.txt", folder).read_bytes())
        for name, location in CUTS.items():
            font = TTFont(source)
            axes = {"opsz": location["opsz"], "wght": location["wght"], "wdth": 100, "ROND": 0, "slnt": 0}
            font = instantiateVariableFont(font, axes, inplace=False, updateFontNames=True)
            options = Options()
            options.layout_features = ["kern", "liga", "calt", "ccmp", "mark", "mkmk", "locl", "tnum", "pnum"]
            options.name_IDs = ["*"]
            options.notdef_outline = True
            subsetter = Subsetter(options)
            subsetter.populate(unicodes=list(codepoints(LATIN)))
            subsetter.subset(font)
            cut = folder / name
            font.save(cut)
            # The layout tests/woff_roundtrip.py expects, which fontTools does not write.
            (OUTPUT / name).write_bytes(relayout_ttf(cut.read_bytes()))
            print("%s %d KB" % (name, (OUTPUT / name).stat().st_size // 1024))
    return 0


if __name__ == "__main__":
    sys.exit(main())
