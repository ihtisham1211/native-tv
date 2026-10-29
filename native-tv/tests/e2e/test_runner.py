#!/usr/bin/env python3
"""The end-to-end runner's own rules, checked without a TV (run.py has the cases that need one)."""
import json
import os
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import run  # noqa: E402


def frames(intervals, swap=1, start=2000):
    rows = [{"frame": i, "atMs": start + i * 17, "intervalMs": value, "swap": swap, "draws": 12,
             "vertices": 3000, "textureMB": 20.0} for i, value in enumerate(intervals)]
    return rows + [dict(rows[-1], frame=len(rows), intervalMs=80.0)]  # the captured last frame


class Registry(unittest.TestCase):
    def test_the_registry_keeps_its_own_rules(self):
        self.assertEqual(run.check_registry(run.load_registry()), [])

    def test_the_ids_cover_what_the_owner_asked_for(self):
        titles = " ".join(case["title"] for case in run.load_registry()["cases"])
        for word in ("probe", "Kit gallery", "Splash", "Home navigation", "Live", "Settings", "Catalog",
                     "Setup", "idle dim"):
            self.assertIn(word, titles)

    def test_a_navigate_only_script_may_not_press_ok(self):
        with tempfile.TemporaryDirectory() as folder:
            os.mkdir(os.path.join(folder, "scripts"))
            with open(os.path.join(folder, "scripts", "bad.json"), "w") as file:
                json.dump([{"at": 100, "key": "ok"}], file)
            case = dict(run.load_registry()["cases"][-1], script="bad.json", navigateOnly=True)
            real, run.HERE = run.HERE, folder
            try:
                problems = run.check_registry({"cases": [case]})
            finally:
                run.HERE = real
            self.assertTrue(any("navigate-only" in problem for problem in problems), problems)

    def test_a_run_is_bounded_and_the_environment_is_closed(self):
        case = dict(run.load_registry()["cases"][-1], durationMs=61000, env={"LD_PRELOAD": "x"})
        problems = run.check_registry({"cases": [case]})
        self.assertTrue(any("durationMs" in problem for problem in problems), problems)
        self.assertTrue(any("LD_PRELOAD" in problem for problem in problems), problems)

    def test_nothing_personal_is_in_the_folder(self):
        for name in ("cases.json",) + tuple(os.path.join("scripts", n) for n in os.listdir(os.path.join(HERE, "scripts"))):
            with open(os.path.join(HERE, name)) as file:
                text = file.read()
            self.assertNotIn("/Users/", text, name)
            self.assertIsNone(run.re.search(r"\b(?!127\.0\.0\.1\b)\d{1,3}(\.\d{1,3}){3}\b", text), name)


class Commands(unittest.TestCase):
    def test_only_the_runner_s_own_verbs_go_to_the_tv(self):
        allowed = ("mkdir /tmp/nativetv-e2e.lock", "cat ", "cd " + run.APP, "ls -l /tmp/nativetv-e2e", "rm -f ")
        forbidden = ("reboot", "tvwebctl", "luna-send", "poweroff", "nativetv-daemon", "systemctl", "deploy",
                     "/var/lib/tvweb/home", "access", "token")
        for case in run.load_registry()["cases"]:
            for line in run.commands(case):
                self.assertTrue(line.startswith(allowed), line)
                for word in forbidden:
                    self.assertNotIn(word, line)

    def test_the_target_and_the_ssh_options_come_from_the_environment(self):
        tv = run.Tv({"NATIVETV_TV_HOST": "tv.example", "NATIVETV_SSH_KEY": "key", "NATIVETV_SSH_KNOWN_HOSTS": "hosts"})
        self.assertEqual(tv.target, "root@tv.example")
        self.assertIn("IdentitiesOnly=yes", tv.command)
        self.assertIn("UserKnownHostsFile=hosts", tv.command)
        self.assertEqual(run.Tv({"NATIVETV_TV_HOST": "me@tv.example"}).target, "me@tv.example")
        with self.assertRaises(ValueError):
            run.Tv({"NATIVETV_TV_HOST": "tv; reboot"})

    def test_without_a_tv_the_run_options_skip(self):
        environment = {key: value for key, value in os.environ.items() if key != "NATIVETV_TV_HOST"}
        for option in (["--all"], ["--case", "E2E-001"]):
            done = subprocess.run([sys.executable, os.path.join(HERE, "run.py")] + option, env=environment,
                                  capture_output=True, text=True)
            self.assertEqual(done.returncode, run.SKIP, done.stdout)

    def test_staging_needs_its_own_permission(self):
        environment = dict(os.environ, NATIVETV_TV_HOST="tv.example")
        done = subprocess.run([sys.executable, os.path.join(HERE, "run.py"), "--case", "E2E-001", "--stage"],
                              env=environment, capture_output=True, text=True)
        self.assertEqual(done.returncode, 1)
        self.assertIn("--allow stage", done.stderr)


class Judging(unittest.TestCase):
    case = {"criteria": {"exitCode": 0, "p95Max": 16.8, "over33Max": 0, "drawsMax": 40, "capture": True},
            "floor": {"p95Max": 20.0, "over33Max": 3}}

    def metrics(self, intervals, **extra):
        metrics = run.perf_metrics(frames(intervals), 1500)
        metrics.update(exitCode=0, captureBytes=10, **extra)
        return metrics

    def test_the_intervals_exclude_loading_and_ambient_frames(self):
        rows = frames([16.7] * 20)[:-1] + frames([60.0], start=100)[:-1] + frames([33.4] * 5, swap=2)
        metrics = run.perf_metrics(rows, 1500)
        self.assertEqual((metrics["intervals"], metrics["over33"]), (20, 0), metrics)

    def test_a_clean_run_passes(self):
        self.assertEqual(run.verdict(self.metrics([16.7] * 100), self.case)[0], "pass")

    def test_missing_only_the_platform_s_limits_is_the_floor(self):
        result, notes = run.verdict(self.metrics([16.7] * 90 + [18.5] * 8 + [36.0] * 2), self.case)
        self.assertEqual(result, "floor", notes)

    def test_anything_else_fails(self):
        self.assertEqual(run.verdict(self.metrics([16.7] * 90 + [25.0] * 10), self.case)[0], "fail")
        self.assertEqual(run.verdict(self.metrics([16.7] * 90 + [36.0] * 10), self.case)[0], "fail")
        bad = self.metrics([16.7] * 100)
        bad["exitCode"] = 1
        self.assertEqual(run.verdict(bad, self.case)[0], "fail")
        bad = self.metrics([16.7] * 100)
        bad["draws"] = 41
        self.assertEqual(run.verdict(bad, self.case)[0], "fail")

    def test_checkpoints_and_probe_rules(self):
        rules = {"criteria": {"checkpoints": [{"name": "dimmed", "path": "idle", "equals": True}],
                              "probe": {"stage": "gl"}}}
        good = {"checkpoints": {"dimmed": {"idle": True}}, "probe": {"stage": "gl"}}
        self.assertEqual(run.verdict(good, rules)[0], "pass")
        self.assertEqual(run.verdict({"checkpoints": {"dimmed": {"idle": False}}, "probe": {}}, rules)[0], "fail")


if __name__ == "__main__":
    unittest.main()
