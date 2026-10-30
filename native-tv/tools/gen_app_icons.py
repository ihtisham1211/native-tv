#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.9"
# dependencies = ["pillow", "resvg-py"]
# ///
"""Generate native-tv/assets/app-icons/app-icons.pack from design/app-icons.json.

Every icon is a full-bleed colour square with one white glyph, centred, the
look of the Homebrew Channel's icon, so apps read as one family. A brand's
glyph is its monochrome logo from Simple Icons (or, for the brands it dropped,
Dashboard Icons) on the brand's colour; LG's own apps get a Material Design
glyph (assets/icons/material) on one shared blue. Only the glyph's shape is
kept; its colours are replaced. The squares have square corners: the screens
that show them round them. The daemon looks an app up by its id, then by its
title, and serves the app's own icon when the pack has neither
(src/service/app_icons.cpp).

Pack format: "NTVICONS 1\\n", one "key offset length\\n" line per icon with the
offset counted from the first data byte, an empty line, then the PNGs.
"""
import io
import json
import sys
import tempfile
import urllib.request
from pathlib import Path

from PIL import Image, ImageChops
import resvg_py

ROOT = Path(__file__).resolve().parents[1]
CONFIG = ROOT / "design" / "app-icons.json"
GLYPHS = ROOT / "assets" / "icons" / "material"
OUTPUT = ROOT / "assets" / "app-icons" / "app-icons.pack"
SOURCE_URL = "https://raw.githubusercontent.com/{repo}/{commit}/{path}"
# Rendered large and shrunk, so edges stay clean at the tile's size.
RENDER_PX = 512


def fetch_logo(source, name, cache):
    path = cache / (name + ".svg")
    if not path.exists():
        url = SOURCE_URL.format(repo=source["repo"], commit=source["commit"], path=source["path"].format(name=name))
        with urllib.request.urlopen(url, timeout=30) as response:
            path.write_bytes(response.read())
    return path.read_text(encoding="utf-8")


def render(svg):
    return Image.open(io.BytesIO(bytes(resvg_py.svg_to_bytes(svg_string=svg, width=RENDER_PX)))).convert("RGBA")


def shape(art, mask):
    """Where the glyph is: the logo's alpha, or, for a logo drawn on its own tile, the
    light marks on it ("light") or everything but its white marks ("dark")."""
    alpha = art.getchannel("A")
    if mask == "light":
        # Stretched so the logo's own dark tile drops out entirely.
        return ImageChops.multiply(alpha, art.convert("L").point(lambda v: max(0, (v - 80) * 255 // 175)))
    if mask == "dark":
        red, green, blue = art.convert("RGB").split()
        return ImageChops.multiply(alpha, ImageChops.invert(ImageChops.darker(ImageChops.darker(red, green), blue)))
    return alpha


def tile(art, size, background, foreground, fill, mask=None):
    """The glyph's shape in `foreground`, fitted to `fill` of a square of `background`."""
    art.putalpha(shape(art, mask))
    art = art.crop(art.getchannel("A").getbbox())
    glyph = Image.new("RGBA", art.size, foreground)
    glyph.putalpha(art.getchannel("A"))
    box = round(size * fill)
    glyph.thumbnail((box, box), Image.LANCZOS)
    canvas = Image.new("RGBA", (size, size), background)
    canvas.alpha_composite(glyph, ((size - glyph.width) // 2, (size - glyph.height) // 2))
    out = io.BytesIO()
    canvas.convert("RGB").save(out, "PNG", optimize=True)
    return out.getvalue()


def main():
    config = json.loads(CONFIG.read_text())
    spec = config["tile"]
    cache = Path(tempfile.gettempdir()) / "nativetv-app-icon-sources"

    blobs = {}  # ("logo"|"glyph", source name) -> png bytes, so aliases share one
    for name in set(config["logos"].values()):
        brand = config["brands"][name]
        source = config["sources"][brand["source"]]
        folder = cache / brand["source"]
        folder.mkdir(parents=True, exist_ok=True)
        art = render(fetch_logo(source, name, folder))
        blobs[("logo", name)] = tile(art, spec["size"], brand["background"], brand.get("foreground", spec["foreground"]),
                                     brand.get("fill", spec["fill"]), brand.get("mask"))
    for name in set(config["glyphs"].values()):
        art = render((GLYPHS / (name + ".svg")).read_text())
        blobs[("glyph", name)] = tile(art, spec["size"], spec["systemBackground"], spec["foreground"], spec["glyphFill"])

    keys = {}
    for kind, table in (("logo", config["logos"]), ("glyph", config["glyphs"])):
        for key, name in table.items():
            if key in keys:
                sys.exit("duplicate key " + key)
            keys[key] = (kind, name)

    data = bytearray()
    placed = {}
    lines = []
    for key in sorted(keys):
        source = keys[key]
        if source not in placed:
            placed[source] = (len(data), len(blobs[source]))
            data += blobs[source]
        lines.append("%s %d %d\n" % ((key,) + placed[source]))
    OUTPUT.parent.mkdir(parents=True, exist_ok=True)
    OUTPUT.write_bytes(b"NTVICONS 1\n" + "".join(lines).encode() + b"\n" + bytes(data))
    print("%d keys, %d icons, %d bytes" % (len(keys), len(placed), OUTPUT.stat().st_size))


if __name__ == "__main__":
    main()
