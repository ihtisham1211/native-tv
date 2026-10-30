#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.9"
# dependencies = ["pillow", "resvg-py"]
# ///
"""Generate native-tv/assets/app-icons/app-icons.pack from design/app-icons.json.

Every icon is drawn on one tile (same background, same padding), so apps look
alike. Brand logos come from Dashboard Icons at the pinned commit; the glyphs
for LG's own apps are the Material Design Icons in assets/icons/material.
The daemon looks an app up by its id, then by its title, and serves the app's
own icon when the pack has neither (src/service/app_icons.cpp).

Pack format: "NTVICONS 1\\n", one "key offset length\\n" line per icon with the
offset counted from the first data byte, an empty line, then the PNGs.
"""
import io
import json
import re
import sys
import tempfile
import urllib.request
from pathlib import Path

from PIL import Image
import resvg_py

ROOT = Path(__file__).resolve().parents[1]
CONFIG = ROOT / "design" / "app-icons.json"
GLYPHS = ROOT / "assets" / "icons" / "material"
OUTPUT = ROOT / "assets" / "app-icons" / "app-icons.pack"
LOGO_URL = "https://raw.githubusercontent.com/{repo}/{commit}/svg/{name}.svg"
# Rendered large and shrunk, so edges stay clean at the tile's size.
RENDER_PX = 512


def fetch_logo(source, name, cache):
    path = cache / (name + ".svg")
    if not path.exists():
        url = LOGO_URL.format(name=name, **source)
        with urllib.request.urlopen(url, timeout=30) as response:
            path.write_bytes(response.read())
    return path.read_text(encoding="utf-8")


def render(svg):
    return Image.open(io.BytesIO(bytes(resvg_py.svg_to_bytes(svg_string=svg, width=RENDER_PX)))).convert("RGBA")


def tile(art, spec, fill):
    size = spec["size"]
    canvas = Image.new("RGBA", (size, size), spec["background"])
    box = round(size * fill)
    art.thumbnail((box, box), Image.LANCZOS)
    canvas.alpha_composite(art, ((size - art.width) // 2, (size - art.height) // 2))
    out = io.BytesIO()
    canvas.convert("RGB").save(out, "PNG", optimize=True)
    return out.getvalue()


def main():
    config = json.loads(CONFIG.read_text())
    spec = config["tile"]
    cache = Path(tempfile.gettempdir()) / "nativetv-app-icon-sources"
    cache.mkdir(exist_ok=True)

    blobs = {}  # ("logo"|"glyph", source name) -> png bytes, so aliases share one
    for key, name in config["logos"].items():
        blobs.setdefault(("logo", name), tile(render(fetch_logo(config["logoSource"], name, cache)), spec, spec["logoFill"]))
    for key, name in config["glyphs"].items():
        svg = (GLYPHS / (name + ".svg")).read_text()
        svg = re.sub(r'fill="#fff"', 'fill="%s"' % spec["glyph"], svg)
        blobs.setdefault(("glyph", name), tile(render(svg), spec, spec["glyphFill"]))

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
