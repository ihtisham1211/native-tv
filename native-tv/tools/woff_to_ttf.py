#!/usr/bin/env python3
"""Unpack a WOFF1 font into the plain TrueType file FreeType opens on every webOS TV.

The TV's FreeType is not guaranteed to be built with WOFF support, so the TV
client ships TTF: each table is stored zlib-compressed in a WOFF and only needs
inflating, the glyph data is unchanged. The TTF is written in one canonical
layout (tables in tag order, each padded to four bytes), which `relayout_ttf`
also applies to a TTF written by another tool, so that packing a shipped font
as WOFF and unpacking it returns the same bytes (tests/woff_roundtrip.py).

    woff_to_ttf.py some-font.woff assets/fonts/SomeFont-Regular.ttf
"""
import struct
import sys
import zlib


def write_ttf(flavor, tables):
    """A TrueType file from (tag, checksum, body) tables, in tag order."""
    tables = sorted(tables, key=lambda table: table[0])
    count = len(tables)
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
    return bytes(out)


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
    out = write_ttf(flavor, tables)
    if len(out) != total_size:
        raise ValueError("unpacked size %d differs from the header's %d" % (len(out), total_size))
    return out


def relayout_ttf(data):
    """The same TrueType tables in the canonical layout."""
    flavor, count = struct.unpack(">IH", data[:6])
    tables = []
    for index in range(count):
        tag, checksum, offset, length = struct.unpack(">4sIII", data[12 + 16 * index:28 + 16 * index])
        tables.append((tag, checksum, data[offset:offset + length]))
    return write_ttf(flavor, tables)


if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    with open(sys.argv[1], "rb") as source:
        converted = woff_to_ttf(source.read())
    with open(sys.argv[2], "wb") as target:
        target.write(converted)
