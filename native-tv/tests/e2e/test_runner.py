#!/usr/bin/env python3
"""The end-to-end runner's own rules, checked without a TV (run.py has the cases that need one)."""
import json
import os
import shutil
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


def last_run_case():
    return [case for case in run.load_registry()["cases"] if case["kind"] == "run"][-1]


class Registry(unittest.TestCase):
    def test_the_registry_keeps_its_own_rules(self):
        self.assertEqual(run.check_registry(run.load_registry()), [])

    def test_the_ids_cover_what_the_owner_asked_for(self):
        titles = " ".join(case["title"] for case in run.load_registry()["cases"])
        for word in ("probe", "Kit gallery", "Splash", "Home navigation", "Live", "Settings", "Catalog",
                     "Setup", "idle dim", "Quick drawer navigation", "hold volume", "hold brightness",
                     "closes with Back"):
            self.assertIn(word, titles)

    def test_cases_that_change_a_tv_setting_need_the_control_permission(self):
        for case in run.load_registry()["cases"]:
            script = run.script_path(case)
            if not script:
                continue
            with open(script) as file:
                steps = json.load(file)
            holds = sum(1 for step in steps if step.get("key") in ("left", "right")) > 10
            if case["screen"] == "quick" and holds:
                self.assertIn("control", case.get("requires", []), case["id"])
        skipped = [case["id"] for case in run.load_registry()["cases"] if "control" in case.get("requires", [])]
        self.assertEqual(skipped, ["E2E-091", "E2E-092", "E2E-101", "E2E-102", "E2E-103"])

    def test_a_navigate_only_script_may_not_press_ok(self):
        with tempfile.TemporaryDirectory() as folder:
            os.mkdir(os.path.join(folder, "scripts"))
            with open(os.path.join(folder, "scripts", "bad.json"), "w") as file:
                json.dump([{"at": 100, "key": "ok"}], file)
            case = dict(last_run_case(), script="bad.json", navigateOnly=True)
            real, run.HERE = run.HERE, folder
            try:
                problems = run.check_registry({"cases": [case]})
            finally:
                run.HERE = real
            self.assertTrue(any("navigate-only" in problem for problem in problems), problems)

    def test_a_flow_may_close_only_the_native_tv_app(self):
        case = next(c for c in run.load_registry()["cases"] if c["id"] == "E2E-083")
        close = {"api": "POST /api/control", "body": {"action": "closeApp", "value": run.APP_ID}}
        self.assertIn(close, case["flow"])
        self.assertEqual(run.check_registry({"cases": [case]}), [])
        other = dict(case, flow=case["flow"][:-1] + [dict(close, body={"action": "closeApp", "value": "com.webos.app.home"})])
        self.assertTrue(any("may not" in problem for problem in run.check_registry({"cases": [other]})))

    def test_a_run_is_bounded_and_the_environment_is_closed(self):
        case = dict(last_run_case(), durationMs=61000, env={"LD_PRELOAD": "x"})
        problems = run.check_registry({"cases": [case]})
        self.assertTrue(any("durationMs" in problem for problem in problems), problems)
        self.assertTrue(any("LD_PRELOAD" in problem for problem in problems), problems)

    def test_an_api_case_reads_the_server_and_only_control_may_write(self):
        read = {"id": "E2E-900", "title": "t", "purpose": "p", "kind": "api", "steps": ["s"], "criteria": {"flowOk": True},
                "flow": [{"api": "GET /api/lgsettings?section=sound", "same": {"a[id=x].v": "b.c"}}]}
        self.assertEqual(run.check_registry({"cases": [read]}), [])
        post = dict(read, flow=[{"api": "POST /api/control", "body": {"action": "mute"}}])
        self.assertTrue(any("POST needs control" in problem for problem in run.check_registry({"cases": [post]})))
        self.assertEqual(run.check_registry({"cases": [dict(post, requires=["control"])]}), [])
        outside = dict(read, flow=[{"api": "GET /etc/passwd"}])
        self.assertTrue(run.check_registry({"cases": [outside]}))
        bodyless = dict(post, requires=["control"], flow=[{"api": "POST /api/control"}])
        self.assertTrue(any("body" in problem for problem in run.check_registry({"cases": [bodyless]})))

    def test_nothing_personal_is_in_the_folder(self):
        for name in ("cases.json",) + tuple(os.path.join("scripts", n) for n in os.listdir(os.path.join(HERE, "scripts"))):
            with open(os.path.join(HERE, name)) as file:
                text = file.read()
            self.assertNotIn("/Users/", text, name)
            self.assertIsNone(run.re.search(r"\b(?!127\.0\.0\.1\b)\d{1,3}(\.\d{1,3}){3}\b", text), name)


class FlowScript(unittest.TestCase):
    """scripts/home-flow.js under node, against a stand-in for the server (which answers an empty JSON body
    with 400, as the daemon does)."""

    def run_flow(self, steps, replies):
        import http.server
        import threading

        class Handler(http.server.BaseHTTPRequestHandler):
            def log_message(self, *args):
                pass

            def reply(self, code, body):
                data = json.dumps(body).encode()
                self.send_response(code)
                self.send_header("Content-Length", str(len(data)))
                self.end_headers()
                self.wfile.write(data)

            def do_GET(self):
                self.reply(200, replies[self.path])

            def do_POST(self):
                body = self.rfile.read(int(self.headers.get("Content-Length", 0)))
                if not body:
                    return self.reply(400, {"ok": False, "error": "malformed JSON"})
                self.reply(200, {"ok": True})

        server = http.server.HTTPServer(("127.0.0.1", 0), Handler)
        threading.Thread(target=server.serve_forever, daemon=True).start()
        try:
            program = ("var PORT = %d;\nvar STEPS = %s;\n" % (server.server_port, json.dumps(steps))).encode()
            with open(os.path.join(HERE, "scripts", "home-flow.js"), "rb") as file:
                program += file.read()
            done = subprocess.run(["node", "-"], input=program, capture_output=True, timeout=60)
        finally:
            server.shutdown()
        return done.returncode, [json.loads(line) for line in done.stdout.decode().splitlines() if line.startswith("{")]

    @unittest.skipUnless(shutil.which("node"), "needs node")
    def test_a_post_with_no_body_sends_an_empty_object(self):
        code, rows = self.run_flow([{"api": "POST /api/home/enable", "expect": {"ok": True}}], {})
        self.assertEqual(code, 0, rows)

    @unittest.skipUnless(shutil.which("node"), "needs node")
    def test_same_compares_two_paths_of_one_reply(self):
        reply = {"rows": [{"id": "a", "value": 1}, {"id": "x", "value": "tv_speaker"}],
                 "dimensions": {"sound": {"x": "tv_speaker"}}}
        agree = {"api": "GET /r", "same": {"rows[id=x].value": "dimensions.sound.x"}}
        self.assertEqual(self.run_flow([agree], {"/r": reply})[0], 0)
        stale = {"rows": [{"id": "x", "value": "external_arc"}], "dimensions": {"sound": {"x": "tv_speaker"}}}
        code, rows = self.run_flow([agree], {"/r": stale})
        self.assertEqual(code, 1)
        self.assertIn("external_arc", rows[-2]["note"])
        # a field that is missing on both sides is not agreement
        self.assertEqual(self.run_flow([agree], {"/r": {"rows": [], "dimensions": {}}})[0], 1)


class Commands(unittest.TestCase):
    def test_only_the_runner_s_own_verbs_go_to_the_tv(self):
        allowed = ("mkdir /tmp/nativetv-e2e.lock", "cat ", "cd " + run.APP, "ls -l /tmp/nativetv-e2e", "rm -f ", "node - ")
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


class Flows(unittest.TestCase):
    def flow_cases(self):
        return [case for case in run.load_registry()["cases"] if case["kind"] == "flow"]

    def test_the_takeover_cases_are_there_and_need_permission(self):
        titles = [case["title"] for case in self.flow_cases()]
        for word in ("takeover on", "HDMI", "Back", "takeover off"):
            self.assertTrue(any(word in title for title in titles), word)
        for case in self.flow_cases():
            self.assertEqual(case["requires"], ["takeover"], case["id"])

    def test_a_flow_asks_only_what_the_vocabulary_allows(self):
        case = dict(self.flow_cases()[0])
        for step in ({"api": "POST /api/settings"}, {"api": "POST /api/control", "body": {"action": "powerOff"}},
                     {"api": "POST /api/control", "body": {"action": "launchApp", "value": "com.example.x"}},
                     {"api": "POST /api/control", "body": {"action": "rcu", "value": "home", "extra": 1}},
                     {"foreground": "com.webos.app.home", "withinMs": 99999}, {"run": "reboot"}):
            case["flow"] = [step]
            self.assertTrue(run.check_registry({"cases": [case]}), step)
        case["flow"] = [{"api": "POST /api/control", "body": {"action": "launchApp", "value": "com.webos.app.hdmi2"}}]
        self.assertEqual(run.check_registry({"cases": [case]}), [])
        case["requires"] = []
        self.assertTrue(any("must require takeover" in problem for problem in run.check_registry({"cases": [case]})))

    def test_the_verdict_names_the_step_that_failed(self):
        case = self.flow_cases()[1]
        good = {"exitCode": 0, "flowDone": True, "flow": [{"step": 0, "ok": True}, {"done": True, "ok": True}]}
        self.assertEqual(run.verdict(good, case)[0], "pass")
        bad = {"exitCode": 1, "flowDone": True, "flow": [{"step": 0, "ok": True}, {"step": 1, "ok": False, "note": "in front: hdmi"}]}
        result, notes = run.verdict(bad, case)
        self.assertEqual(result, "fail")
        self.assertIn("step 1: in front: hdmi", " ".join(notes))
        self.assertEqual(run.verdict({"exitCode": 1, "flowDone": False, "flow": []}, case)[0], "fail")

    def test_the_script_runs_under_node_against_a_server_and_an_app_manager(self):
        node = shutil.which("node")
        if not node:
            self.skipTest("no node on this computer")
        import http.server
        import threading

        class Handler(http.server.BaseHTTPRequestHandler):
            seen = []

            def respond(self, body):
                data = json.dumps(body).encode()
                self.send_response(200)
                self.send_header("Content-Length", str(len(data)))
                self.end_headers()
                self.wfile.write(data)

            def do_GET(self):
                Handler.seen.append("GET " + self.path)
                self.respond({"enabled": True, "nativeHome": {"halted": False}})

            def do_POST(self):
                Handler.seen.append("POST " + self.path + " " + self.rfile.read(int(self.headers["Content-Length"])).decode())
                self.respond({"ok": True})

            def log_message(self, *args):
                pass

        server = http.server.HTTPServer(("127.0.0.1", 0), Handler)
        threading.Thread(target=server.serve_forever, daemon=True).start()
        with tempfile.TemporaryDirectory() as folder:
            front = os.path.join(folder, "front")
            with open(front, "w") as file:
                file.write("com.tvweb.dashboard")
            fake = os.path.join(folder, "luna-send")
            with open(fake, "w") as file:
                file.write('#!/bin/sh\nprintf \'{"returnValue":true,"appId":"%%s"}\' "$(cat %s)"\n' % front)
            os.chmod(fake, 0o755)
            case = {"id": "E2E-999", "flow": [
                {"api": "GET /api/home", "expect": {"enabled": True, "nativeHome.halted": False}},
                {"api": "POST /api/control", "body": {"action": "rcu", "value": "home"}, "expect": {"ok": True}},
                {"foreground": "com.tvweb.dashboard", "withinMs": 2000},
                {"stays": "com.tvweb.dashboard", "forMs": 500}, {"wait": 10}]}
            environment = dict(os.environ, PATH=folder + os.pathsep + os.environ["PATH"])
            done = subprocess.run([node, "-"], input=run.flow_program(case, server.server_port), env=environment,
                                  capture_output=True, timeout=30)
            rows = [json.loads(line) for line in done.stdout.decode().splitlines()]
            self.assertEqual(done.returncode, 0, done.stdout + done.stderr)
            self.assertEqual([row["ok"] for row in rows if "step" in row], [True] * 5, rows)
            self.assertTrue(rows[-1]["done"] and rows[-1]["ok"])
            self.assertEqual(Handler.seen, ["GET /api/home", 'POST /api/control {"action":"rcu","value":"home"}'])
            # An app that never comes to the front ends the flow at that step, with the exit code 1.
            with open(front, "w") as file:
                file.write("com.webos.app.hdmi1")
            case["flow"] = [{"foreground": "com.tvweb.dashboard", "withinMs": 600}, {"wait": 10}]
            done = subprocess.run([node, "-"], input=run.flow_program(case, server.server_port), env=environment,
                                  capture_output=True, timeout=30)
            rows = [json.loads(line) for line in done.stdout.decode().splitlines()]
            self.assertEqual(done.returncode, 1)
            self.assertFalse(rows[-1]["ok"])
            self.assertIn("still com.webos.app.hdmi1", rows[-2]["note"])
        server.shutdown()
        server.server_close()


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
