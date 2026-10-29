#!/usr/bin/env python3
"""Runs the TV client binary: --probe exit codes, --time captures, --perf, SIGTERM.

  client_platform.py PATH/TO/nativetv-tv

The tests that need a GL context skip when the machine cannot give one (a CI
runner with no Mesa), unless NATIVETV_REQUIRE_GL=1.
"""
import json
import os
import signal
import subprocess
import sys
import tempfile
import time
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "golden"))
import golden  # noqa: E402

CLIENT = sys.argv.pop(1)
# Keep in step with platform/probe.hpp.
OK, NO_DISPLAY, NO_CONTEXT, GL_ERROR, READBACK = 0, 10, 11, 12, 13
QUIET = ["--windowed", "--server", "http://127.0.0.1:9"]


def run(args, env=None, **extra):
    environment = golden.client_environment()
    environment.update(env or {})
    return subprocess.run([CLIENT] + args, env=environment, capture_output=True, text=True, timeout=60, **extra)


def read(path, mode="r"):
    with open(path, mode) as file:
        return file.read()


def probe(env=None):
    done = run(["--probe", "--windowed"], env)
    return done.returncode, json.loads(done.stdout.strip().splitlines()[-1])


class ProbeExitCodes(unittest.TestCase):
    def test_no_display(self):
        code, report = probe({"SDL_VIDEODRIVER": "no-such-video-driver"})
        self.assertEqual(code, NO_DISPLAY)
        self.assertEqual((report["ok"], report["stage"]), (False, "display"))

    def test_a_window_without_gl_is_no_context(self):
        code, report = probe({"SDL_VIDEODRIVER": "dummy"})
        self.assertEqual(code, NO_CONTEXT)
        self.assertEqual((report["ok"], report["stage"]), (False, "context"))
        self.assertTrue(report["error"])

    def test_the_codes_are_distinct_and_documented(self):
        self.assertEqual(len({OK, NO_DISPLAY, NO_CONTEXT, GL_ERROR, READBACK, 1}), 6)
        header = read(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "src", "platform", "probe.hpp"))
        for name, value in (("kProbeNoDisplay", NO_DISPLAY), ("kProbeNoContext", NO_CONTEXT),
                            ("kProbeGlError", GL_ERROR), ("kProbeReadback", READBACK)):
            self.assertIn("%s = %d" % (name, value), header)


class NeedsGl(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        code, _ = probe()
        if code in (NO_DISPLAY, NO_CONTEXT) and not os.environ.get("NATIVETV_REQUIRE_GL"):
            raise unittest.SkipTest("no GL context on this machine")


class ProbeWithGl(NeedsGl):
    def test_success_reports_the_context(self):
        code, report = probe()
        self.assertEqual(code, OK, report)
        self.assertTrue(report["ok"])
        self.assertTrue(report["renderer"] and report["version"])
        self.assertGreaterEqual(report["maxTextureSize"], 2048)
        self.assertEqual(len(report["pixel"]), 4)
        for got, want in zip(report["pixel"], (51, 153, 204, 255)):
            self.assertLessEqual(abs(got - want), 3)

    def test_gl_error_exit_code(self):
        code, report = probe({"NATIVETV_PROBE_FAULT": "gl-error"})
        self.assertEqual(code, GL_ERROR)
        self.assertEqual((report["ok"], report["stage"]), (False, "gl"))

    def test_readback_mismatch_exit_code(self):
        code, report = probe({"NATIVETV_PROBE_FAULT": "readback"})
        self.assertEqual(code, READBACK)
        self.assertEqual((report["ok"], report["stage"]), (False, "readback"))


class FixedClock(NeedsGl):
    def capture(self, directory, name, time_ms, *extra):
        path = os.path.join(directory, name)
        done = run(QUIET + ["--screen", "splash", "--time=%d" % time_ms, "--capture", path] + list(extra))
        self.assertEqual(done.returncode, 0, done.stderr)
        return read(path, "rb")

    def test_two_runs_write_identical_files(self):
        with tempfile.TemporaryDirectory() as directory:
            first = self.capture(directory, "a.ppm", 500)
            second = self.capture(directory, "b.ppm", 500)
            self.assertEqual(first, second)
            self.assertTrue(first.startswith(b"P6\n1280 720\n255\n"))
            # The clock decides the picture: a later frame is a different one.
            self.assertNotEqual(first, self.capture(directory, "c.ppm", 700))

    def test_perf_log_and_summary(self):
        with tempfile.TemporaryDirectory() as directory:
            log = os.path.join(directory, "perf.jsonl")
            done = run(QUIET + ["--screen", "splash", "--time=500", "--perf=" + log])
            self.assertEqual(done.returncode, 0, done.stderr)
            line = done.stdout.strip().splitlines()[-1]
            # Frames 0..30 are at 0..500 ms, so the count is exact.
            self.assertRegex(line, r"^perf: frames=31 ambient=0 p50=\d+\.\d p95=\d+\.\d p99=\d+\.\d "
                                   r"max=\d+\.\d late=\d+ build=\d+\.\d\d submit=\d+\.\d\d "
                                   r"draws=\d+ vertices=\d+ textureMB=\d+\.\d$")
            records = [json.loads(text) for text in read(log).splitlines()]
            frames, summary = records[:-1], records[-1]["summary"]
            self.assertEqual([f["frame"] for f in frames], list(range(31)))
            self.assertIsNone(frames[0]["intervalMs"])
            self.assertEqual(frames[30]["atMs"], 500.0)
            self.assertEqual(summary["frames"], 31)
            self.assertEqual(sum(summary["histogram"]), 30)
            self.assertEqual(len(summary["histogram"]), len(summary["histogramEdgesMs"]) + 1)
            self.assertGreater(frames[1]["draws"], 0)

    def test_equals_and_space_forms_of_an_option_agree(self):
        with tempfile.TemporaryDirectory() as directory:
            a, b = os.path.join(directory, "a.ppm"), os.path.join(directory, "b.ppm")
            run(QUIET + ["--screen", "splash", "--time", "300", "--capture", a])
            run(QUIET + ["--screen=splash", "--time=300", "--capture=" + b])
            self.assertEqual(read(a, "rb"), read(b, "rb"))


class FrameRate(NeedsGl):
    def frames(self, env=None):
        with tempfile.TemporaryDirectory() as directory:
            log = os.path.join(directory, "perf.jsonl")
            done = run(QUIET + ["--screen", "splash", "--duration-ms=1200", "--perf=" + log], env)
            self.assertEqual(done.returncode, 0, done.stderr)
            records = [json.loads(text) for text in read(log).splitlines()]
        return done.stdout, records[:-1]

    @staticmethod
    def median(frames):
        intervals = sorted(f["intervalMs"] for f in frames if f["intervalMs"] is not None)
        return intervals[len(intervals) // 2]

    def test_animation_draws_continuously_at_swap_interval_one(self):
        out, frames = self.frames()
        self.assertEqual({f["swap"] for f in frames}, {1})
        # The display's refresh (60 or 120 Hz), or the loop's own 8 ms floor when
        # the context does not wait for it; never a busy loop and never slow.
        self.assertTrue(6 <= self.median(frames) <= 25, self.median(frames))

    def test_ambient_animation_runs_at_30_fps(self):
        out, frames = self.frames({"NATIVETV_AMBIENT": "1"})
        self.assertEqual({f["swap"] for f in frames}, {2})
        self.assertIn("ambient=%d " % len(frames), out)
        # The loop's own pacing: the swap interval stays 1, and "swap" in the log is the pace.
        self.assertTrue(25 <= self.median(frames) <= 50, self.median(frames))


def reap(process, timeout):
    """Waits for the process; returns (exit code, CPU seconds it used)."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        pid, status, usage = os.wait4(process.pid, os.WNOHANG)
        if pid:
            process.returncode = os.waitstatus_to_exitcode(status)
            return process.returncode, usage.ru_utime + usage.ru_stime
        time.sleep(0.05)
    raise AssertionError("the client did not exit within %d s of SIGTERM" % timeout)


class Termination(NeedsGl):
    def start(self, screen):
        environment = golden.client_environment()
        process = subprocess.Popen([CLIENT] + QUIET + ["--screen", screen], env=environment,
                                   stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)
        self.addCleanup(lambda: process.poll() is None and process.kill())
        self.addCleanup(process.stderr.close)
        # Printed just before the loop starts.
        self.assertIn("Native Tv native:", process.stderr.readline())
        return process

    def test_sigterm_while_idle_exits_zero_without_spinning(self):
        process = self.start("kit:keyboard")
        time.sleep(3)
        process.send_signal(signal.SIGTERM)
        code, cpu = reap(process, 5)
        self.assertEqual(code, 0)
        # Start-up costs a few tenths of a second; a busy loop would use all 3.
        self.assertLess(cpu, 1.5)

    def test_sigterm_while_animating_exits_zero(self):
        process = self.start("splash")
        time.sleep(1)
        process.send_signal(signal.SIGTERM)
        self.assertEqual(reap(process, 5)[0], 0)


if __name__ == "__main__":
    unittest.main()
