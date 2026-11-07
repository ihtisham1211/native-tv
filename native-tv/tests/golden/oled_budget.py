#!/usr/bin/env python3
"""The light budget of n-oled-care, checked on the committed `oled-*` goldens (no GL needed).

Light output is the mean linear luminance of a frame as a share of a full-white screen. Bright is the
share of pixels whose Rec. 709 luma (of the displayed values) is above 80%: what a white pill or a
bright title puts on the panel. A static screen of the theme stays under both limits, so a change
that brings back a wash, a picture or a filled white pill fails here.

  oled_budget.py            check every oled-*.png beside this file
  oled_budget.py --report   print the figures
"""
import collections
import glob
import os
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import golden  # noqa: E402

LIGHT_OUTPUT_LIMIT = 1.6  # percent of a full-white screen
BRIGHT_LIMIT = 1.0  # percent of the pixels above 80% luma
LINEAR = [(v / 255 / 12.92 if v / 255 <= 0.04045 else ((v / 255 + 0.055) / 1.055) ** 2.4)
          for v in range(256)]


def measure(path):
    """(light output %, bright %) of a golden."""
    width, height, pixels = golden.read_png(path)
    count = width * height
    red, green, blue = pixels[0::3], pixels[1::3], pixels[2::3]
    mean = lambda channel: sum(LINEAR[value] * n for value, n in collections.Counter(channel).items()) / count
    light = 0.2126 * mean(red) + 0.7152 * mean(green) + 0.0722 * mean(blue)
    bright = sum(1 for r, g, b in zip(red, green, blue) if 0.2126 * r + 0.7152 * g + 0.0722 * b > 204)
    return light * 100, bright / count * 100


def oled_goldens():
    return sorted(glob.glob(os.path.join(HERE, "oled-*.png")))


class LightBudget(unittest.TestCase):
    def test_there_are_oled_goldens(self):
        self.assertTrue(oled_goldens(), "no oled-*.png beside oled_budget.py")

    def test_each_oled_golden_is_inside_the_budget(self):
        for path in oled_goldens():
            light, bright = measure(path)
            name = os.path.basename(path)
            self.assertLessEqual(light, LIGHT_OUTPUT_LIMIT, "%s: light output %.2f%%" % (name, light))
            self.assertLessEqual(bright, BRIGHT_LIMIT, "%s: %.2f%% of pixels above 80%% luma" % (name, bright))

    def test_the_budget_can_fail(self):
        # Google TV's Home is far over it; a limit that nothing could exceed would prove nothing.
        light, bright = measure(os.path.join(HERE, "home-rest.png"))
        self.assertGreater(light, LIGHT_OUTPUT_LIMIT)
        self.assertGreater(bright, BRIGHT_LIMIT)


if __name__ == "__main__":
    if "--report" in sys.argv:
        for path in oled_goldens():
            print("%-28s light output %.2f%%, above 80%% luma %.2f%%" % ((os.path.basename(path),) + measure(path)))
    else:
        unittest.main()
