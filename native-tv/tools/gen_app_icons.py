#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.9"
# dependencies = ["pillow", "resvg-py"]
# ///
"""Generate native-tv/assets/app-icons/app-icons.pack from design/app-icons.json.

Every icon sits on one dark rounded-square tile, the same size and padding, so
the apps read as a set on the TV's dark UI. A brand's logo is drawn as its
owner publishes it, in its own colours, never recoloured: where the plain logo
is dark (X, Apple TV, Prime Video, TikTok, Peacock) the brand's own variant for
dark surfaces is used. The logos are Dashboard Icons'. LG's own apps get a
Material Design glyph (assets/icons/material) in the kit's accent colour on the
same tile. The squares have square corners: the screens that show them round
them. The daemon looks an app up by its id, then by its title, and serves the
app's own icon when the pack has neither (src/service/app_icons.cpp).

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


def background(spec):
    """The tile: a dark square, a little lighter at the top."""
    size = spec["size"]
    fade = Image.linear_gradient("L").resize((size, size))  # black at the top, white at the bottom
    return Image.composite(Image.new("RGB", (size, size), spec["bottom"]),
                           Image.new("RGB", (size, size), spec["top"]), fade).convert("RGBA")


def tile(art, spec, fill, color=None, mask=None):
    """The art, fitted to `fill` of the tile and centred. Its colours are kept unless `color`
    (or `mask`, which keeps only a logo's marks, in white) asks for one flat colour."""
    if mask:
        art.putalpha(shape(art, mask))
        color = color or "#FFFFFF"
    art = art.crop(art.getchannel("A").getbbox())
    if color:
        flat = Image.new("RGBA", art.size, color)
        flat.putalpha(art.getchannel("A"))
        art = flat
    box = round(spec["size"] * fill)
    art.thumbnail((box, box), Image.LANCZOS)
    canvas = background(spec)
    canvas.alpha_composite(art, ((spec["size"] - art.width) // 2, (spec["size"] - art.height) // 2))
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
        art = render(fetch_logo(source, brand["name"], folder))
        blobs[("logo", name)] = tile(art, spec, brand.get("fill", spec["fill"]), brand.get("color"), brand.get("mask"))
    for name in set(config["glyphs"].values()):
        art = render((GLYPHS / (name + ".svg")).read_text())
        blobs[("glyph", name)] = tile(art, spec, spec["glyphFill"], spec["glyphColor"])

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
