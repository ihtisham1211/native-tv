#!/usr/bin/env python3
"""End-to-end cases for the native TV client on a real TV.

  run.py --list                     the cases
  run.py --show E2E-030             one case, with the commands it runs
  run.py --check                    the registry against its own rules (no TV needed)
  run.py --case E2E-030 [--case ..] run cases
  run.py --all [--quick]            run every case that is allowed (--quick: without the sweeps and runs over 45 s)
  options: --report FILE (.json or .md), --stage, --allow takeover|reboot|control|fresh|stage

The cases (cases.json) need a real TV, so nothing here runs in CI: with NATIVETV_TV_HOST
unset the run options exit 77 (skipped). The environment:

  NATIVETV_TV_HOST             the TV, "ADDRESS" or "USER@ADDRESS" (user defaults to root)
  NATIVETV_SSH_KEY             private key to use (default: ssh's own choice)
  NATIVETV_SSH_KNOWN_HOSTS     known-hosts file to use (default: ssh's own)
  NATIVETV_BUILD               build folder of the client for --stage (default: ../build-webos)

What a run does on the TV, and nothing else: one bounded client run (at most 60 s) through the
app's jailer, started the way deploy.py starts it, one at a time under a lock folder in /tmp.
It never reboots, never deploys or restarts the daemon, never touches a TV setting (but for the
cases marked `control`, which hold a key on the Quick drawer's volume or brightness and put it
back), and navigate-only cases are checked to hold no OK, text or click. It reads the server's
port from the TV as deploy.py does and never reads or prints the access token. Cases that need
the takeover, a reboot or to change a TV setting (`control`) are marked in `requires` and run
only with the matching --allow, and so are those that need a TV that has not done first-run setup
(`fresh`: its first-run marker is present, which the runner never creates).

The one exception is the flow cases (kind "flow", which all require takeover): they switch Native Tv
Home on and off and press the Home and Back keys through the server's own routes, from the TV
itself (scripts/home-flow.js under the TV's Node, loopback needing no token), and watch which app
is in front. Their steps are a closed vocabulary, checked by --check; a flow that fails leaves
the TV as it stopped, and the "off" case puts LG's Home back.

The api cases (kind "api") are the regression cases for what testing the dashboard on a TV found:
GET requests to the server's own routes, from the TV itself like a flow, and the reply's fields
compared (`expect` a value, `same` two paths of one reply). A case that must change a setting
(POST) requires `control`, and puts the setting back.

A reboot case (kind "reboot", which requires `reboot`) sends the server's own reboot action, waits for
the TV to go down and come back, gives the services time to start, and then runs the case's flow (GET
requests) on the TV, to see that its settings survived.
"""
import argparse
import json
import os
import re
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
SKIP = 77
# The Native Tv tile's app: the one native app sam is told of, which Native Tv Home opens too.
APP_ID = "com.tvweb.dashboard"
APP = "/media/developer/apps/usr/palm/applications/" + APP_ID
SERVICE = "/var/lib/tvweb/native/nativetv-service"
DATA_ROOT = "/var/lib/tvweb"
LOCK = "/tmp/nativetv-e2e.lock"
MAX_DURATION_MS = 60000
# A sweep of a Settings screen waits for a write to the TV after each press, which takes seconds a row.
SWEEP_MAX_DURATION_MS = 300000
KNOWN_REQUIRES = {"takeover", "reboot", "control", "fresh"}
KNOWN_ALLOW = KNOWN_REQUIRES | {"stage"}
ENV_NAMES = {"NATIVETV_PROBE_FAULT", "NATIVETV_AMBIENT"}
# What a flow may ask of the TV: the server's Home routes and its remote, launch and close controls for
# an input or the Native Tv app, nothing else.
FLOW_API = {"GET /api/home", "POST /api/home/enable", "POST /api/home/disable"}
FLOW_APP = re.compile(r"^(com\.webos\.app\.(hdmi[1-4]|livetv)|com\.tvweb\.dashboard)$")
FLOW_KEYS = {"home", "back"}
# What a reboot case asks first. The server answers, and the TV goes down.
REBOOT_STEP = {"api": "POST /api/control", "body": {"action": "reboot"}, "expect": {"ok": True}}
REBOOT_DOWN_S, REBOOT_UP_S, REBOOT_SETTLE_S = 120, 420, 40
API_GET = re.compile(r"^GET /api/[A-Za-z0-9_/?=&,.-]*$")
API_POST = re.compile(r"^POST /api/[A-Za-z0-9_/-]*$")
CASE_ID = re.compile(r"^E2E-\d{3}$")
SCREEN = re.compile(r"^[a-z]+(:[a-z0-9]+)?$")
VALUE = re.compile(r"^[A-Za-z0-9_,=.-]*$")
# What the registry and the scripts must never hold: a personal path, an address, a token
# (a long run of hex digits); the loopback address is the one address the cases use.
PRIVATE = re.compile(r"/Users/|(?<!/api)/home/[a-z]|\b(?!127\.0\.0\.1\b)\d{1,3}\.\d{1,3}\.\d{1,3}\.\d{1,3}\b|[A-Fa-f0-9]{24,}")
FORBIDDEN_IN_NAVIGATE_ONLY = ("text", "click", "activate", "longPress", "sweep")
# What a sweep of a Settings screen reports for a row that was pressed and did not behave.
SWEEP_FAILURES = ("unchanged by OK", "the write did not come back", "not back after the most presses", "out of time")


# ---- The registry ---------------------------------------------------------------------------

def load_registry(path=None):
    with open(path or os.path.join(HERE, "cases.json")) as file:
        return json.load(file)


def script_path(case):
    return os.path.join(HERE, "scripts", case["script"]) if case.get("script") else None


def flow_problems(case):
    """A flow only asks what FLOW_API and the control routes allow, and always needs the takeover."""
    name = case.get("id", "?")
    problems = []
    if "takeover" not in case.get("requires", []):
        problems.append("%s: a flow switches Home on or off and must require takeover" % name)
    flow = case.get("flow")
    if not isinstance(flow, list) or not flow:
        return problems + ["%s: a flow has steps" % name]
    for index, step in enumerate(flow):
        where = "%s: flow step %d" % (name, index)
        if not isinstance(step, dict):
            problems.append(where + " is not an object")
        elif "api" in step:
            body = step.get("body")
            allowed = step["api"] in FLOW_API and body is None
            if step["api"] == "POST /api/control" and isinstance(body, dict):
                action, value = body.get("action"), body.get("value")
                allowed = (action == "launchApp" and isinstance(value, str) and bool(FLOW_APP.match(value))) or \
                          (action == "closeApp" and value == APP_ID) or \
                          (action == "rcu" and value in FLOW_KEYS)
                allowed = allowed and set(body) == {"action", "value"}
                # Waking the panel, which a flow's keys need: the first key on a dark one only wakes it.
                allowed = allowed or body == {"action": "screenOn"}
            if not allowed:
                problems.append(where + " asks for " + json.dumps(step) + ", which a flow may not")
        elif "foreground" in step:
            if not FLOW_APP.match(step["foreground"]) and step["foreground"] != "com.webos.app.home" or \
                    not 1 <= step.get("withinMs", 0) <= 30000:
                problems.append(where + " needs an app and a withinMs up to 30000")
        elif "stays" in step:
            if not FLOW_APP.match(step["stays"]) and step["stays"] != "com.webos.app.home" or \
                    not 1 <= step.get("forMs", 0) <= 30000:
                problems.append(where + " needs an app and a forMs up to 30000")
        elif "wait" in step:
            if not 1 <= step["wait"] <= 30000:
                problems.append(where + " waits up to 30000 ms")
        else:
            problems.append(where + " is not a step")
    return problems


def api_problems(case, field="flow"):
    """An api case asks GET routes of the server; a POST needs `control`, since it changes a setting."""
    name = case.get("id", "?")
    flow = case.get(field)
    if not isinstance(flow, list) or not flow:
        return ["%s: an api case has steps" % name]
    problems = []
    for index, step in enumerate(flow):
        where = "%s: step %d" % (name, index)
        if not isinstance(step, dict):
            problems.append(where + " is not an object")
        elif "api" in step:
            call = step["api"]
            if not (isinstance(call, str) and (API_GET.match(call) or
                                               (API_POST.match(call) and "control" in case.get("requires", [])))):
                problems.append(where + " asks for " + json.dumps(call) + ", which an api case may not (POST needs control)")
            if "body" in step and not (isinstance(call, str) and call.startswith("POST ")):
                problems.append(where + ": only a POST has a body")
            for field in ("expect", "same"):
                if field in step and not isinstance(step[field], dict):
                    problems.append("%s: %s is not an object" % (where, field))
            if "status" in step and not (isinstance(step["status"], int) and 100 <= step["status"] <= 599):
                problems.append(where + ": status is not an HTTP status")
            if "save" in step and not (isinstance(step["save"], dict) and all(isinstance(v, str) for v in step["save"].values())):
                problems.append(where + ": save maps names to reply paths")
            for field in ("keepSession", "withSession"):
                if field in step and step[field] is not True:
                    problems.append("%s: %s is true or left out" % (where, field))
        elif "wait" in step:
            if not isinstance(step["wait"], int) or not 1 <= step["wait"] <= 30000:
                problems.append(where + " waits up to 30000 ms")
        else:
            problems.append(where + " is not a step")
    return problems


def check_registry(registry):
    """Every rule a case must keep; returns the problems as strings."""
    problems = []
    seen = set()
    for case in registry["cases"]:
        name = case.get("id", "?")
        if not CASE_ID.match(name) or name in seen:
            problems.append("%s: the id must be E2E-nnn and unique" % name)
        seen.add(name)
        for field in ("title", "purpose", "kind", "steps", "criteria"):
            if not case.get(field):
                problems.append("%s: no %s" % (name, field))
        if case.get("kind") not in ("probe", "run", "flow", "api", "reboot"):
            problems.append("%s: kind must be probe, run, flow, api or reboot" % name)
        if case.get("kind") == "reboot":
            problems += api_problems(case)
            if "reboot" not in case.get("requires", []):
                problems.append("%s: a reboot case must require reboot" % name)
            if any(step.get("api", "").startswith("POST ") for step in case.get("flow", []) if isinstance(step, dict)):
                problems.append("%s: what a reboot case checks after the boot is GET requests" % name)
        if case.get("kind") == "flow":
            problems += flow_problems(case)
        if case.get("kind") == "api":
            problems += api_problems(case)
        if not set(case.get("requires", [])) <= KNOWN_REQUIRES:
            problems.append("%s: requires holds %s" % (name, sorted(set(case["requires"]) - KNOWN_REQUIRES)))
        if case.get("kind") == "run":
            if "cleanup" in case:
                problems += api_problems(case, "cleanup")
            if "after" in case:
                problems += api_problems(case, "after")
            if not SCREEN.match(case.get("screen", "")):
                problems.append("%s: the screen is not a route" % name)
            duration = case.get("durationMs", 0)
            sweeps = False
            if case.get("script") and os.path.isfile(script_path(case)):
                with open(script_path(case)) as file:
                    sweeps = any("sweep" in step for step in json.load(file))
            limit = SWEEP_MAX_DURATION_MS if sweeps else MAX_DURATION_MS
            if not 1000 <= duration <= limit:
                problems.append("%s: durationMs must be 1000 to %d" % (name, limit))
        for key, value in case.get("env", {}).items():
            if key not in ENV_NAMES or not VALUE.match(value):
                problems.append("%s: environment %s is not allowed" % (name, key))
        path = script_path(case)
        if path:
            if not os.path.isfile(path):
                problems.append("%s: no script %s" % (name, case["script"]))
            else:
                with open(path) as file:
                    steps = json.load(file)
                text = json.dumps(steps)
                if PRIVATE.search(text):
                    problems.append("%s: the script holds a path, address or token" % name)
                if any("capture" in step or "wallClock" in step for step in steps):
                    problems.append("%s: a script may not capture or set the clock" % name)
                if any(isinstance(step.get("sweep"), dict) and step["sweep"].get("press", True) for step in steps) \
                        and "control" not in case.get("requires", []):
                    problems.append("%s: a script that presses a sweep's rows changes settings, so the case requires control" % name)
                if case.get("navigateOnly") and any(
                        step.get("key") in ("ok", "back", "red", "delete") or
                        any(field in step for field in FORBIDDEN_IN_NAVIGATE_ONLY) for step in steps):
                    problems.append("%s: a navigate-only script may only move the focus" % name)
                if steps and max(step["at"] for step in steps) > case.get("durationMs", 0):
                    problems.append("%s: the script runs past the duration" % name)
        if PRIVATE.search(json.dumps(case)):
            problems.append("%s: the case holds a path, address or token" % name)
    return problems


# ---- Metrics and criteria -------------------------------------------------------------------

def percentile(values, fraction):
    return values[min(len(values) - 1, int(fraction * len(values)))] if values else 0.0


def perf_metrics(lines, warmup_ms):
    """What the perf log says, with the first `warmup_ms` (loading) and the captured last frame left out of the intervals."""
    frames = [row for row in lines if "frame" in row]
    # The last frame of a run is the one that is captured, and the readback is in its interval.
    intervals = sorted(row["intervalMs"] for row in frames[:-1]
                       if row["atMs"] >= warmup_ms and row.get("intervalMs") is not None and row["swap"] == 1)
    return {
        "frames": len(frames),
        "intervals": len(intervals),
        "p50": percentile(intervals, .5), "p95": percentile(intervals, .95),
        "p99": percentile(intervals, .99), "max": intervals[-1] if intervals else 0.0,
        "over33": sum(1 for value in intervals if value > 33),
        "draws": max([row["draws"] for row in frames] or [0]),
        "vertices": max([row["vertices"] for row in frames] or [0]),
        "textureMb": max([row["textureMB"] for row in frames] or [0]),
    }


def judge(metrics, rules):
    """The rules that the metrics break, as {rule key: message}."""
    broken = {}

    def over(key, value, limit, unit=""):
        if value > limit:
            broken[key] = "%s %.1f%s is over %s%s" % (key, value, unit, limit, unit)

    if "exitCode" in rules and metrics.get("exitCode") != rules["exitCode"]:
        broken["exitCode"] = "exit code %s, wanted %s" % (metrics.get("exitCode"), rules["exitCode"])
    if "framesMin" in rules and metrics.get("frames", 0) < rules["framesMin"]:
        broken["framesMin"] = "%s frames, wanted at least %s" % (metrics.get("frames", 0), rules["framesMin"])
    if "intervalsMin" in rules and metrics.get("intervals", 0) < rules["intervalsMin"]:
        broken["intervalsMin"] = "%s intervals measured, wanted at least %s" % (metrics.get("intervals", 0), rules["intervalsMin"])
    if "p50Max" in rules:
        over("p50Max", metrics.get("p50", 0), rules["p50Max"], " ms")
    if "p95Max" in rules:
        over("p95Max", metrics.get("p95", 0), rules["p95Max"], " ms")
    if "p99Max" in rules:
        over("p99Max", metrics.get("p99", 0), rules["p99Max"], " ms")
    if "maxIntervalMax" in rules:
        over("maxIntervalMax", metrics.get("max", 0), rules["maxIntervalMax"], " ms")
    if "over33Max" in rules:
        over("over33Max", metrics.get("over33", 0), rules["over33Max"], " intervals")
    if "drawsMax" in rules:
        over("drawsMax", metrics.get("draws", 0), rules["drawsMax"])
    if "verticesMax" in rules:
        over("verticesMax", metrics.get("vertices", 0), rules["verticesMax"])
    if "textureMbMax" in rules:
        over("textureMbMax", metrics.get("textureMb", 0), rules["textureMbMax"], " MB")
    if rules.get("capture") and not metrics.get("captureBytes"):
        broken["capture"] = "no capture was produced"
    for expected in rules.get("checkpoints", []):
        point = metrics.get("checkpoints", {}).get(expected["name"])
        value = None if point is None else point.get(expected["path"])
        if "atLeast" in expected:
            # A count that has to have grown to this much (the readings Live has taken in so many seconds).
            if point is None or not isinstance(value, (int, float)) or value < expected["atLeast"]:
                broken["checkpoint " + expected["name"] + "." + expected["path"]] = \
                    "checkpoint %s: %s is %r, wanted at least %r" % (expected["name"], expected["path"], value, expected["atLeast"])
        elif point is None or value != expected["equals"]:
            broken["checkpoint " + expected["name"] + "." + expected["path"]] = \
                "checkpoint %s: %s is %r, wanted %r" % (expected["name"], expected["path"], value, expected["equals"])
    if "sweep" in rules:
        rows = metrics.get("sweep", [])
        pressed = [row for row in rows if "restored" in row]
        failed = [row for row in rows if row.get("restored") is False or row.get("skipped") in SWEEP_FAILURES]
        if len(pressed) < rules["sweep"].get("pressedMin", 0):
            broken["sweep.pressedMin"] = "%d rows were pressed and put back, wanted at least %d" % (
                len(pressed), rules["sweep"]["pressedMin"])
        acted = [row for row in rows if row.get("acted")]
        if len(acted) < rules["sweep"].get("actedMin", 0):
            broken["sweep.actedMin"] = "%d action rows were pressed, wanted at least %d" % (
                len(acted), rules["sweep"]["actedMin"])
        if failed:
            broken["sweep"] = "; ".join("%s / %s / %s: %s" % (row.get("group"), row.get("page"), row.get("row"),
                                                              row.get("skipped") or "not put back") for row in failed)
    if rules.get("flowOk"):
        bad = [row for row in metrics.get("flow", []) if "step" in row and not row["ok"]]
        if bad or not metrics.get("flowDone"):
            broken["flowOk"] = "; ".join("step %d: %s" % (row["step"], row["note"]) for row in bad) or "the flow did not finish"
    for key, wanted in rules.get("probe", {}).items():
        if metrics.get("probe", {}).get(key) != wanted:
            broken["probe " + key] = "probe %s is %r, wanted %r" % (key, metrics.get("probe", {}).get(key), wanted)
    return broken


def verdict(metrics, case):
    """pass: the criteria hold. floor: only the interval limits the platform cannot always keep
    are missed, by less than the floor. fail: anything else."""
    broken = judge(metrics, case["criteria"])
    if not broken:
        return "pass", []
    floor = case.get("floor", {})
    uncovered = [key for key in broken if key not in floor]
    if not uncovered and not judge(metrics, floor):
        return "floor", list(broken.values())
    return "fail", list(broken.values())


# ---- The TV ---------------------------------------------------------------------------------

class Tv:
    def __init__(self, environment=None):
        environment = os.environ if environment is None else environment
        host = environment.get("NATIVETV_TV_HOST", "").strip()
        if not re.match(r"^([A-Za-z0-9_.-]+@)?[A-Za-z0-9_.:-]+$", host):
            raise ValueError("NATIVETV_TV_HOST is not an address")
        self.target = host if "@" in host else "root@" + host
        self.command = ["ssh", "-o", "BatchMode=yes", "-o", "ConnectTimeout=8", "-o", "LogLevel=ERROR"]
        if environment.get("NATIVETV_SSH_KEY"):
            self.command += ["-i", environment["NATIVETV_SSH_KEY"], "-o", "IdentitiesOnly=yes"]
        if environment.get("NATIVETV_SSH_KNOWN_HOSTS"):
            self.command += ["-o", "UserKnownHostsFile=" + environment["NATIVETV_SSH_KNOWN_HOSTS"]]
        self.command.append(self.target)

    def run(self, remote, data=None, timeout=60):
        done = subprocess.run(self.command + [remote], input=data, capture_output=True, timeout=timeout)
        return done.returncode, done.stdout.decode("utf-8", "replace"), done.stderr.decode("utf-8", "replace")

    def port(self):
        """The server's port, read the way deploy.py reads it; the token is never asked for."""
        code, out, _ = self.run("%s port --data-root %s" % (SERVICE, DATA_ROOT))
        port = out.strip()
        if code != 0 or not port.isdecimal() or not 1 <= int(port) <= 65535:
            raise RuntimeError("the TV did not give the server's port")
        return int(port)


def jailer(args, env=None, timeout_s=60, redirect=""):
    """The command line deploy.py's smoke run uses, as one remote shell line."""
    environment = " ".join("%s=%s" % pair for pair in sorted((env or {}).items()))
    launch = ["-t", "native_devmode", "-i", APP_ID, "-p", APP, APP + "/nativetv-tv"]
    return "cd %s && env XDG_RUNTIME_DIR=/tmp/xdg %s timeout %d /usr/bin/jailer %s %s%s" % (
        APP, environment, timeout_s, " ".join(launch), " ".join(args), redirect)


def client_arguments(case, port, remote):
    """The client's own arguments for a run case (the launch parameter names the app)."""
    launch = "'" + json.dumps({"@system_native_app": True, "nid": APP_ID}, separators=(",", ":")) + "'"
    arguments = [launch, "--server", "http://127.0.0.1:%s" % port, "--screen", case["screen"],
                 "--duration-ms", str(case["durationMs"])]
    if case.get("script"):
        arguments += ["--script", remote["script"]]
    arguments += ["--perf", remote["perf"], "--capture", remote["capture"], "--report", remote["report"]]
    return arguments


def remote_files(case):
    base = "/tmp/nativetv-e2e-" + case["id"]
    return {"script": "%s/e2e-%s.json" % (APP, case["id"]), "perf": base + ".perf.jsonl",
            "capture": base + ".ppm", "report": base + ".report.json", "out": base + ".out"}


def commands(case, port="PORT"):
    """What a case runs on the TV, in order, for --show and the report."""
    files = remote_files(case)
    if case["kind"] == "probe":
        return [jailer(["--probe"], case.get("env"), 40)]
    if case["kind"] == "reboot":
        return ["mkdir " + LOCK,
                "node - %s   # scripts/home-flow.js with POST /api/control reboot" % port,
                "wait for ssh to stop answering (at most %d s), then to answer again (at most %d s)" % (REBOOT_DOWN_S, REBOOT_UP_S),
                "wait for the server's port, then %d s for the services" % REBOOT_SETTLE_S,
                "node - PORT   # scripts/home-flow.js with this case's flow",
                "rmdir " + LOCK]
    if case["kind"] in ("flow", "api"):
        return ["mkdir " + LOCK,
                "node - %s   # scripts/home-flow.js with this case's flow, over ssh" % port,
                "rm -f /tmp/nativetv-e2e-%s* && rmdir %s" % (case["id"], LOCK)]
    steps = ["mkdir " + LOCK]
    if case.get("script"):
        steps.append("cat > " + files["script"] + "   # the script file, over ssh")
    steps.append(jailer(client_arguments(case, port, files), case.get("env"),
                        case["durationMs"] // 1000 + 15, " > %s 2>&1" % files["out"]))
    steps += ["cat %s %s %s" % (files["perf"], files["report"], files["out"]),
              "ls -l " + files["capture"],
              "rm -f %s %s* && rmdir %s" % (files["script"] if case.get("script") else "/dev/null",
                                            "/tmp/nativetv-e2e-" + case["id"], LOCK)]
    return steps


def flow_program(case, port):
    """scripts/home-flow.js with the port and the case's steps in front of it."""
    with open(os.path.join(HERE, "scripts", "home-flow.js"), "rb") as file:
        body = file.read()
    return ("var PORT = %d;\nvar STEPS = %s;\n" % (port, json.dumps(case["flow"]))).encode() + body


def wait_for(condition, seconds, every):
    """Whether `condition` becomes true within `seconds`, tried every `every` seconds."""
    deadline = time.time() + seconds
    while time.time() < deadline:
        try:
            if condition():
                return True
        except (subprocess.TimeoutExpired, OSError):
            pass
        time.sleep(every)
    return False


def execute(tv, case, port):
    """Runs one case on the TV and returns its metrics."""
    files = remote_files(case)
    if case["kind"] == "probe":
        code, out, _ = tv.run(jailer(["--probe"], case.get("env"), 40) + "; echo exit=$?", timeout=60)
        lines = [line for line in out.splitlines() if line.startswith("{")]
        exit_line = [line for line in out.splitlines() if line.startswith("exit=")]
        metrics = {"exitCode": int(exit_line[-1][5:]) if exit_line else -1, "probe": {}}
        if lines:
            try:
                metrics["probe"] = json.loads(lines[-1])
            except ValueError:
                pass
        return metrics
    code, _, _ = tv.run("mkdir " + LOCK)
    if code != 0:
        raise RuntimeError("another run holds " + LOCK + "; remove it only when no client is running")
    if case["kind"] == "reboot":
        try:
            started = time.time()
            tv.run("node - %d" % port, flow_program(dict(case, flow=[REBOOT_STEP]), port), timeout=60)
            tv.run("rmdir " + LOCK)          # /tmp does not survive the reboot, and the lock goes with it
            if not wait_for(lambda: tv.run("true", timeout=12)[0] != 0, REBOOT_DOWN_S, 3):
                raise RuntimeError("the TV did not go down within %d s of the reboot" % REBOOT_DOWN_S)
            if not wait_for(lambda: tv.run("true", timeout=12)[0] == 0, REBOOT_UP_S, 5):
                raise RuntimeError("the TV did not answer over ssh within %d s" % REBOOT_UP_S)
            booted = round(time.time() - started, 1)
            serving = []

            def serves():
                try:
                    serving.append(tv.port())
                    return True
                except (RuntimeError, ValueError, subprocess.TimeoutExpired, OSError):
                    return False
            if not wait_for(serves, REBOOT_UP_S, 5):
                raise RuntimeError("the TV is back but the server's port was not read")
            time.sleep(REBOOT_SETTLE_S)
            code, out, _ = tv.run("node - %d" % serving[-1], flow_program(case, serving[-1]), timeout=180)
            rows = []
            for line in out.splitlines():
                try:
                    rows.append(json.loads(line))
                except ValueError:
                    pass
            return {"exitCode": code, "flow": rows, "flowDone": any(row.get("done") for row in rows),
                    "wallSeconds": round(time.time() - started, 1), "backAfterSeconds": booted}
        finally:
            tv.run("rmdir " + LOCK, timeout=15)
    if case["kind"] in ("flow", "api"):
        try:
            program = flow_program(case, port)
            started = time.time()
            code, out, _ = tv.run("node - %d" % port, program, timeout=180)
            rows = []
            for line in out.splitlines():
                try:
                    rows.append(json.loads(line))
                except ValueError:
                    pass
            return {"exitCode": code, "flow": rows, "flowDone": any(row.get("done") for row in rows),
                    "wallSeconds": round(time.time() - started, 1)}
        finally:
            tv.run("rmdir " + LOCK)
    try:
        code, out, _ = tv.run("pidof nativetv-tv")
        if out.strip():
            raise RuntimeError("a nativetv-tv is already running on the TV")
        data = None
        if case.get("script"):
            with open(script_path(case), "rb") as file:
                data = file.read()
            if tv.run("cat > " + files["script"], data)[0] != 0:
                raise RuntimeError("could not stage the script")
        started = time.time()
        _, out, _ = tv.run(jailer(client_arguments(case, port, files), case.get("env"),
                                  case["durationMs"] // 1000 + 15, " > %s 2>&1; echo exit=$?" % files["out"]),
                           timeout=case["durationMs"] / 1000 + 45)
        exit_line = [line for line in out.splitlines() if line.startswith("exit=")]
        metrics = {"exitCode": int(exit_line[-1][5:]) if exit_line else -1,
                   "wallSeconds": round(time.time() - started, 1)}
        _, perf, _ = tv.run("cat " + files["perf"])
        rows = [json.loads(line) for line in perf.splitlines() if line.startswith("{")]
        metrics.update(perf_metrics(rows, case.get("warmupMs", 1500)))
        _, report, _ = tv.run("cat " + files["report"])
        try:
            parsed = json.loads(report)
            metrics["checkpoints"] = {point["name"]: point["state"] for point in parsed.get("checkpoints", [])}
            metrics["sweep"] = parsed.get("sweep", [])
            metrics["renderer"] = parsed.get("renderer", "")
        except ValueError:
            metrics["checkpoints"] = {}
        _, listing, _ = tv.run("ls -l " + files["capture"])
        fields = listing.split()
        metrics["captureBytes"] = int(fields[4]) if len(fields) > 4 and fields[4].isdigit() else 0
        if case.get("after"):
            # What the client run did to the TV, read back through the server and judged by `flowOk`.
            code, out, _ = tv.run("node - %d" % port, flow_program(dict(case, flow=case["after"]), port), timeout=120)
            flow = []
            for line in out.splitlines():
                try:
                    flow.append(json.loads(line))
                except ValueError:
                    pass
            metrics["flow"] = flow
            metrics["flowDone"] = any(row.get("done") for row in flow)
        return metrics
    finally:
        if case.get("cleanup"):
            # What the client run changed on the server and cannot undo itself, put back even when the run failed.
            tv.run("node - %d" % port, flow_program(dict(case, flow=case["cleanup"]), port), timeout=60)
        tv.run("rm -f %s %s* ; rmdir %s" % (files["script"], "/tmp/nativetv-e2e-" + case["id"], LOCK))


# ---- Reports --------------------------------------------------------------------------------

def markdown(results):
    lines = ["| id | case | result | p50 | p95 | p99 | max | late | draws | vertices | MB | notes |",
             "|---|---|---|---|---|---|---|---|---|---|---|---|"]
    for row in results:
        m = row.get("metrics", {})
        lines.append("| %s | %s | %s | %s | %s | %s | %s | %s | %s | %s | %s | %s |" % (
            row["id"], row["title"], row["result"],
            *[("%.1f" % m[k]) if k in m and m.get("intervals") else "" for k in ("p50", "p95", "p99", "max")],
            m.get("over33", "") if m.get("intervals") else "", m.get("draws", ""), m.get("vertices", ""),
            ("%.1f" % m["textureMb"]) if "textureMb" in m else "", "; ".join(row.get("notes", []))))
    return "\n".join(lines) + "\n"


def summary(results):
    width = max([len(row["id"] + " " + row["title"]) for row in results] or [10])
    out = []
    for row in results:
        m = row.get("metrics", {})
        stats = ""
        if m.get("intervals"):
            stats = "p50 %.1f p95 %.1f p99 %.1f max %.1f late %d draws %d verts %d" % (
                m["p50"], m["p95"], m["p99"], m["max"], m["over33"], m["draws"], m["vertices"])
        out.append("%-*s  %-7s %s" % (width, row["id"] + " " + row["title"], row["result"], stats))
        for note in row.get("notes", []):
            out.append("    " + note)
    return "\n".join(out)


# ---- Main -----------------------------------------------------------------------------------

def is_slow(case):
    """A Settings sweep, or a client run of more than 45 s: what --quick leaves out."""
    return str(case.get("script", "")).startswith("sweep-") or (
        case.get("kind") == "run" and case.get("durationMs", 0) > 45000)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--list", action="store_true")
    parser.add_argument("--show", metavar="ID")
    parser.add_argument("--check", action="store_true", help="check the registry; needs no TV")
    parser.add_argument("--case", action="append", default=[], metavar="ID")
    parser.add_argument("--all", action="store_true")
    parser.add_argument("--quick", action="store_true",
                        help="with --all, leave out the Settings sweeps and the client runs over 45 s (about three "
                             "quarters of the time): what a change needs between releases")
    parser.add_argument("--allow", action="append", default=[], choices=sorted(KNOWN_ALLOW),
                        help="let cases that require takeover, reboot, control or a fresh install run; stage allows --stage")
    parser.add_argument("--stage", action="store_true",
                        help="first stage the client with deploy.py (which also stages the managed runtime "
                             "under /var/lib/tvweb/native, so it needs --allow stage)")
    parser.add_argument("--report", metavar="FILE", help="write the results as .json, or as a table for .md")
    args = parser.parse_args(argv)
    registry = load_registry()
    by_id = {case["id"]: case for case in registry["cases"]}

    if args.check:
        problems = check_registry(registry)
        print("\n".join(problems) if problems else "%d cases, no problems" % len(by_id))
        return 1 if problems else 0
    if args.list:
        for case in registry["cases"]:
            print("%s  %-9s %s" % (case["id"], "/".join(case.get("requires", [])) or "-", case["title"]))
        return 0
    if args.show:
        case = by_id.get(args.show)
        if not case:
            print("no such case: " + args.show, file=sys.stderr)
            return 1
        print(json.dumps(case, indent=2))
        print("\nOn the TV, in order (PORT is read from the TV):")
        for line in commands(case):
            print("  " + line)
        return 0
    if not args.case and not args.all:
        parser.print_help()
        return 1
    if not os.environ.get("NATIVETV_TV_HOST"):
        print("skipped: NATIVETV_TV_HOST is not set; these cases need a real TV")
        return SKIP

    problems = check_registry(registry)
    if problems:
        print("\n".join(problems), file=sys.stderr)
        return 1
    chosen = registry["cases"] if args.all else [by_id[name] for name in args.case if name in by_id]
    if args.all and args.quick:
        chosen = [case for case in chosen if not is_slow(case)]
    if not args.all and len(chosen) != len(args.case):
        print("no such case in --case", file=sys.stderr)
        return 1
    tv = Tv()
    if args.stage:
        if "stage" not in args.allow:
            print("--stage needs --allow stage: deploy.py also stages the managed runtime", file=sys.stderr)
            return 1
        build = os.environ.get("NATIVETV_BUILD", os.path.join(HERE, "..", "..", "build-webos"))
        command = [sys.executable, os.path.join(HERE, "..", "..", "deploy.py"), "--tv", tv.target, "--build", build]
        if os.environ.get("NATIVETV_SSH_KEY"):
            command += ["--identity", os.environ["NATIVETV_SSH_KEY"]]
        if os.environ.get("NATIVETV_SSH_KNOWN_HOSTS"):
            command += ["--known-hosts", os.environ["NATIVETV_SSH_KNOWN_HOSTS"]]
        if subprocess.run(command).returncode != 0:
            print("staging failed", file=sys.stderr)
            return 1
    port = None
    results = []
    for case in chosen:
        row = {"id": case["id"], "title": case["title"], "screen": case.get("screen", ""), "notes": []}
        missing = set(case.get("requires", [])) - set(args.allow)
        if missing:
            row.update(result="skipped", notes=["needs --allow " + " --allow ".join(sorted(missing))])
        else:
            try:
                if port is None and case["kind"] in ("run", "flow", "api", "reboot"):
                    port = tv.port()
                row["metrics"] = execute(tv, case, port)
                row["result"], row["notes"] = verdict(row["metrics"], case)
            except (RuntimeError, ValueError, subprocess.TimeoutExpired, OSError) as error:
                row.update(result="error", notes=[str(error)])
        print("%s %s: %s" % (row["id"], row["title"], row["result"]), flush=True)
        results.append(row)
    print()
    print(summary(results))
    if args.report:
        with open(args.report, "w") as file:
            file.write(markdown(results) if args.report.endswith(".md") else json.dumps(results, indent=2) + "\n")
    return 1 if any(row["result"] in ("fail", "error") for row in results) else 0


if __name__ == "__main__":
    sys.exit(main())
