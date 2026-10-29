#!/usr/bin/env python3
"""End-to-end cases for the native TV client on a real TV.

  run.py --list                     the cases
  run.py --show E2E-030             one case, with the commands it runs
  run.py --check                    the registry against its own rules (no TV needed)
  run.py --case E2E-030 [--case ..] run cases
  run.py --all                      run every case that is allowed
  options: --report FILE (.json or .md), --stage, --allow takeover|reboot|stage

The cases (cases.json) need a real TV, so nothing here runs in CI: with NATIVETV_TV_HOST
unset the run options exit 77 (skipped). The environment:

  NATIVETV_TV_HOST             the TV, "ADDRESS" or "USER@ADDRESS" (user defaults to root)
  NATIVETV_SSH_KEY             private key to use (default: ssh's own choice)
  NATIVETV_SSH_KNOWN_HOSTS     known-hosts file to use (default: ssh's own)
  NATIVETV_BUILD               build folder of the client for --stage (default: ../build-webos)

What a run does on the TV, and nothing else: one bounded client run (at most 60 s) through the
app's jailer, started the way deploy.py starts it, one at a time under a lock folder in /tmp.
It never reboots, never deploys or restarts the daemon, never touches the Home takeover or a TV
setting, and navigate-only cases are checked to hold no OK, text or click. It reads the server's
port from the TV as deploy.py does and never reads or prints the access token. Cases that need
the takeover or a reboot are marked in `requires` and run only with the matching --allow.
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
APP_ID = "com.tvweb.native"
APP = "/media/developer/apps/usr/palm/applications/" + APP_ID
SERVICE = "/var/lib/tvweb/native/nativetv-service"
DATA_ROOT = "/var/lib/tvweb"
LOCK = "/tmp/nativetv-e2e.lock"
MAX_DURATION_MS = 60000
KNOWN_REQUIRES = {"takeover", "reboot"}
KNOWN_ALLOW = KNOWN_REQUIRES | {"stage"}
ENV_NAMES = {"NATIVETV_PROBE_FAULT", "NATIVETV_AMBIENT"}
CASE_ID = re.compile(r"^E2E-\d{3}$")
SCREEN = re.compile(r"^[a-z]+(:[a-z0-9]+)?$")
VALUE = re.compile(r"^[A-Za-z0-9_,=.-]*$")
# What the registry and the scripts must never hold: a personal path, an address, a token
# (a long run of hex digits); the loopback address is the one address the cases use.
PRIVATE = re.compile(r"/Users/|/home/[a-z]|\b(?!127\.0\.0\.1\b)\d{1,3}\.\d{1,3}\.\d{1,3}\.\d{1,3}\b|[A-Fa-f0-9]{24,}")
FORBIDDEN_IN_NAVIGATE_ONLY = ("text", "click", "activate")


# ---- The registry ---------------------------------------------------------------------------

def load_registry(path=None):
    with open(path or os.path.join(HERE, "cases.json")) as file:
        return json.load(file)


def script_path(case):
    return os.path.join(HERE, "scripts", case["script"]) if case.get("script") else None


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
        if case.get("kind") not in ("probe", "run"):
            problems.append("%s: kind must be probe or run" % name)
        if not set(case.get("requires", [])) <= KNOWN_REQUIRES:
            problems.append("%s: requires holds %s" % (name, sorted(set(case["requires"]) - KNOWN_REQUIRES)))
        if case.get("kind") == "run":
            if not SCREEN.match(case.get("screen", "")):
                problems.append("%s: the screen is not a route" % name)
            duration = case.get("durationMs", 0)
            if not 1000 <= duration <= MAX_DURATION_MS:
                problems.append("%s: durationMs must be 1000 to %d" % (name, MAX_DURATION_MS))
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
        if point is None or value != expected["equals"]:
            broken["checkpoint " + expected["name"] + "." + expected["path"]] = \
                "checkpoint %s: %s is %r, wanted %r" % (expected["name"], expected["path"], value, expected["equals"])
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
            metrics["renderer"] = parsed.get("renderer", "")
        except ValueError:
            metrics["checkpoints"] = {}
        _, listing, _ = tv.run("ls -l " + files["capture"])
        fields = listing.split()
        metrics["captureBytes"] = int(fields[4]) if len(fields) > 4 and fields[4].isdigit() else 0
        return metrics
    finally:
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

def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--list", action="store_true")
    parser.add_argument("--show", metavar="ID")
    parser.add_argument("--check", action="store_true", help="check the registry; needs no TV")
    parser.add_argument("--case", action="append", default=[], metavar="ID")
    parser.add_argument("--all", action="store_true")
    parser.add_argument("--allow", action="append", default=[], choices=sorted(KNOWN_ALLOW),
                        help="let cases that require takeover or reboot run; stage allows --stage")
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
                if port is None and case["kind"] == "run":
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
