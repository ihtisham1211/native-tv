#!/usr/bin/env python3
"""Packing each shipped Google Sans Flex TTF as WOFF1 and unpacking it returns the same bytes."""
import struct
import sys
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from woff_to_ttf import woff_to_ttf  # noqa: E402


def pack_woff(ttf):
    flavor, count = struct.unpack(">IH", ttf[:6])
    tables = []
    for index in range(count):
        tag, checksum, offset, length = struct.unpack(">4sIII", ttf[12 + 16 * index:28 + 16 * index])
        tables.append((tag, checksum, ttf[offset:offset + length]))
    body = bytearray()
    entries = []
    start = 44 + 20 * count
    for tag, checksum, data in tables:
        packed = zlib.compress(data)
        if len(packed) >= len(data):
            packed = data
        entries.append(struct.pack(">4sIIII", tag, start + len(body), len(packed), len(data), checksum))
        body += packed + b"\0" * (-len(packed) & 3)
    total = 44 + 20 * count + len(body)
    header = struct.pack(">4sIIHHIHHIIIII", b"wOFF", flavor, total, count, 0, len(ttf), 1, 0, 0, 0, 0, 0, 0)
    return header + b"".join(entries) + bytes(body)


def main():
    fonts = sorted((ROOT / "assets" / "fonts").glob("GoogleSansFlex-*.ttf"))
    assert len(fonts) == 3, "expected the three Google Sans Flex cuts"
    for font in fonts:
        original = font.read_bytes()
        assert struct.unpack(">I", original[:4])[0] == 0x00010000, font.name + " is not TrueType"
        assert woff_to_ttf(pack_woff(original)) == original, font.name + " does not round-trip"
    print("%d fonts round-trip" % len(fonts))


if __name__ == "__main__":
    main()
