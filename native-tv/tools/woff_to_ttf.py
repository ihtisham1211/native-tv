#!/usr/bin/env python3
"""Unpack a WOFF1 font into the plain TrueType file FreeType opens on every webOS TV.

The TV's FreeType is not guaranteed to be built with WOFF support, so the TV
client ships TTF. Figtree comes from the `@fontsource/figtree` npm package
(`figtree-latin-<weight>-normal.woff`); each table is stored zlib-compressed
and only needs inflating, the glyph data is unchanged.

    woff_to_ttf.py figtree-latin-400-normal.woff assets/fonts/Figtree-Regular.ttf
"""
import struct
import sys
import zlib


def woff_to_ttf(data):
    signature, flavor, length, count, _, total_size = struct.unpack(">4sIIHHI", data[:20])
    if signature != b"wOFF" or length != len(data):
        raise ValueError("not a WOFF1 file")
    tables = []
    for index in range(count):
        tag, offset, compressed, original, checksum = struct.unpack(
            ">4sIIII", data[44 + 20 * index:64 + 20 * index])
        body = data[offset:offset + compressed]
        if compressed < original:
            body = zlib.decompress(body)
        if len(body) != original:
            raise ValueError("table %s has the wrong length" % tag.decode("latin1"))
        tables.append((tag, checksum, body))
    tables.sort(key=lambda table: table[0])
    search_range = 16
    entry_selector = 0
    while search_range * 2 <= count * 16:
        search_range *= 2
        entry_selector += 1
    out = bytearray(struct.pack(">IHHHH", flavor, count, search_range, entry_selector,
                                count * 16 - search_range))
    offset = 12 + 16 * count
    for tag, checksum, body in tables:
        out += struct.pack(">4sIII", tag, checksum, offset, len(body))
        offset += (len(body) + 3) & ~3
    for _, _, body in tables:
        out += body + b"\0" * (-len(body) & 3)
    if len(out) != total_size:
        raise ValueError("unpacked size %d differs from the header's %d" % (len(out), total_size))
    return bytes(out)


if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    with open(sys.argv[1], "rb") as source:
        converted = woff_to_ttf(source.read())
    with open(sys.argv[2], "wb") as target:
        target.write(converted)
