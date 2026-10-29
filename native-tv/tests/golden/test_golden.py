#!/usr/bin/env python3
"""Tests for golden.py's image reading and comparison (no GL needed)."""
import os
import struct
import sys
import tempfile
import unittest
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import golden  # noqa: E402


def gradient(width, height):
    return bytes((x * 7 + y * 3 + c * 50) & 255 for y in range(height) for x in range(width) for c in range(3))


def filtered_png(path, width, height, pixels, kind):
    """A PNG whose rows all use one filter type, to test the reader against."""
    stride, rows, previous = width * 3, [], bytearray(width * 3)
    for y in range(height):
        row = pixels[y * stride:(y + 1) * stride]
        out = bytearray()
        for i in range(stride):
            a = row[i - 3] if i >= 3 else 0
            b, c = previous[i], (previous[i - 3] if i >= 3 else 0)
            p = a + b - c
            paeth = a if abs(p - a) <= abs(p - b) and abs(p - a) <= abs(p - c) else b if abs(p - b) <= abs(p - c) else c
            predictor = [0, a, b, (a + b) // 2, paeth][kind]
            out.append((row[i] - predictor) & 255)
        rows.append(bytes([kind]) + bytes(out))
        previous = row
    with open(path, "wb") as file:
        file.write(b"\x89PNG\r\n\x1a\n")
        file.write(golden.png_chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)))
        file.write(golden.png_chunk(b"IDAT", zlib.compress(b"".join(rows))))
        file.write(golden.png_chunk(b"IEND", b""))


class GoldenTools(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        self.addCleanup(self.dir.cleanup)

    def path(self, name):
        return os.path.join(self.dir.name, name)

    def test_png_round_trip(self):
        pixels = gradient(37, 11)
        golden.write_png(self.path("a.png"), 37, 11, pixels)
        self.assertEqual(golden.read_png(self.path("a.png")), (37, 11, pixels))

    def test_every_png_filter_reads_back(self):
        pixels = gradient(29, 9)
        for kind in range(5):
            filtered_png(self.path("f.png"), 29, 9, pixels, kind)
            self.assertEqual(golden.read_png(self.path("f.png")), (29, 9, pixels), "filter %d" % kind)

    def test_ppm_reads_back(self):
        pixels = gradient(5, 4)
        with open(self.path("a.ppm"), "wb") as file:
            file.write(b"P6\n5 4\n255\n" + pixels)
        self.assertEqual(golden.read_image(self.path("a.ppm")), (5, 4, pixels))

    def test_identical_images_match(self):
        image = (16, 8, gradient(16, 8))
        self.assertTrue(golden.compare(image, image, 0, 0)[0])

    def test_tolerance_and_share_of_bad_pixels(self):
        width, height = 20, 10
        base = bytearray(gradient(width, height))
        near, far = bytearray(base), bytearray(base)
        near[1] = (near[1] + 5) & 255 if near[1] < 250 else near[1] - 5
        far[30:33] = bytes((v + 60) & 255 for v in far[30:33])  # one pixel, far off
        expected = (width, height, bytes(base))
        self.assertTrue(golden.compare(expected, (width, height, bytes(near)), 5, 0)[0])
        self.assertFalse(golden.compare(expected, (width, height, bytes(near)), 4, 0)[0])
        self.assertFalse(golden.compare(expected, (width, height, bytes(far)), 8, 0)[0])
        # One bad pixel of 200 is 0.5%.
        self.assertTrue(golden.compare(expected, (width, height, bytes(far)), 8, 0.006)[0])

    def test_size_mismatch_fails(self):
        ok, message = golden.compare((4, 4, gradient(4, 4)), (4, 5, gradient(4, 5)), 255, 1)
        self.assertFalse(ok)
        self.assertIn("size 4x5", message)

    def test_failure_writes_a_diff_image_with_the_bad_pixel_red(self):
        width, height = 8, 4
        base = gradient(width, height)
        changed = bytearray(base)
        changed[3 * (2 * width + 5):3 * (2 * width + 5) + 3] = b"\x00\x00\x00"
        diff = self.path("diff.png")
        ok, _ = golden.compare((width, height, base), (width, height, bytes(changed)), 0, 0, diff)
        self.assertFalse(ok)
        w, h, pixels = golden.read_png(diff)
        self.assertEqual((w, h), (width, height))
        offset = 3 * (2 * width + 5)
        self.assertEqual(pixels[offset:offset + 3], b"\xff\x00\x00")
        self.assertEqual(pixels[:3], bytes(b >> 2 for b in base[:3]))

    def test_a_pass_writes_no_diff(self):
        image = (4, 4, gradient(4, 4))
        diff = self.path("none.png")
        self.assertTrue(golden.compare(image, image, 0, 0, diff)[0])
        self.assertFalse(os.path.exists(diff))


if __name__ == "__main__":
    unittest.main()
