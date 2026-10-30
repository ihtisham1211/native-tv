#!/usr/bin/env python3
"""Runs the TV client through fixed scenarios against the Home fixture and reports frame times,
input-to-swap latency, CPU, resident memory and context switches, each as the median over runs.

  perf_bench.py BUILD/nativetv-tv [--runs 3] [--scenario NAME ...] [--json out.json]

Host only (it needs python3 and the fixture). CPU, memory and context switches come from the
kernel's per-child accounting (wait4), so they are the client's own, not the fixture's. A run is
real time, not the fixed clock of --time. What the numbers mean on a TV is in
docs/development/testing.md; the same client flags run there through jailer.
"""
import argparse
import json
import os
import statistics
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import home_fixture  # noqa: E402


def keys(names, start, gap):
    return [{"at": start + i * gap, "key": name} for i, name in enumerate(names)]


def build_scenarios():
    right_left = ["right", "left"] * 20
    return {
        # A: the launcher doing nothing; the loop should sleep, not spin.
        "idle-home": {"ms": 10000, "script": []},
        # A screen that never animates: what the loop costs while it only waits.
        "idle-static": {"ms": 10000, "screen": "kit:keyboard", "script": []},
        # The dashboard screen has no ambient animation, so this one is idle in the loop's sense.
        "idle-dashboard": {"ms": 10000, "screen": "dashboard", "script": []},
        # B: rapid directional navigation, a key every 70 ms (faster than the card slide).
        "nav-rapid": {"ms": 4800, "script": keys(right_left, 1500, 70)},
        # F: focus moving between rows and back.
        "nav-rows": {"ms": 4800, "script": keys(["down", "up"] * 20, 1500, 70)},
        # C: the Quick drawer opened, a row down and back, closed.
        "quick-drawer": {"ms": 4600, "script": keys(
            ["up", "right", "right", "right", "ok", "down", "down", "up", "back"], 1500, 400)},
        # D: the Apps row entered and scrolled.
        "apps-scroll": {"ms": 5200, "script": keys(
            ["up", "right", "ok"] + ["right"] * 14 + ["left"] * 6, 1500, 120)},
    }


def run_once(client, origin, scenario, workdir):
    log = os.path.join(workdir, "perf.jsonl")
    steps = os.path.join(workdir, "steps.json")
    with open(steps, "w") as handle:
        json.dump(scenario["script"], handle)
    command = [client, "--server", origin, "--screen", scenario.get("screen", "home"), "--script", steps,
               "--duration-ms", str(scenario["ms"]), "--perf", log]
    started = time.monotonic()
    process = subprocess.Popen(command, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    _, status, usage = os.wait4(process.pid, 0)
    wall = time.monotonic() - started
    if os.WEXITSTATUS(status) != 0:
        raise RuntimeError("client exited %d" % os.WEXITSTATUS(status))
    with open(log) as handle:
        records = [json.loads(line) for line in handle]
    summary = records[-1]["summary"]
    rss = usage.ru_maxrss / (1024 * 1024 if sys.platform == "darwin" else 1024)  # MB
    cpu = usage.ru_utime + usage.ru_stime
    return {
        "wallS": wall, "cpuPct": 100 * cpu / wall, "rssMB": rss,
        "voluntarySwitchesPerS": usage.ru_nvcsw / wall,
        "involuntarySwitchesPerS": usage.ru_nivcsw / wall,
        "frames": summary["frames"], "ambientFrames": summary["ambientFrames"],
        "p50": summary["p50Ms"], "p95": summary["p95Ms"], "p99": summary["p99Ms"],
        "max": summary["maxMs"], "late": summary["lateFrames"],
        "draws": summary["draws"], "textureMB": summary["textureMB"],
        "inputFrames": summary["inputFrames"], "inputP50": summary["inputP50Ms"],
        "inputP95": summary["inputP95Ms"], "inputMax": summary["inputMaxMs"],
    }


def median_of(runs):
    return {key: round(statistics.median(run[key] for run in runs), 2) for key in runs[0]}


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("client")
    parser.add_argument("--runs", type=int, default=3)
    parser.add_argument("--scenario", action="append")
    parser.add_argument("--json")
    args = parser.parse_args()
    scenarios = build_scenarios()
    names = args.scenario or list(scenarios)
    results = {}
    with home_fixture.serve() as origin, tempfile.TemporaryDirectory() as workdir:
        for name in names:
            runs = [run_once(os.path.abspath(args.client), origin, scenarios[name], workdir)
                    for _ in range(args.runs)]
            results[name] = {"median": median_of(runs), "runs": runs}
    columns = ["cpuPct", "rssMB", "voluntarySwitchesPerS", "frames", "p50", "p95", "p99", "max",
               "late", "draws", "inputFrames", "inputP50", "inputP95", "inputMax"]
    print("| scenario | " + " | ".join(columns) + " |")
    print("|---|" + "---|" * len(columns))
    for name in names:
        print("| %s | %s |" % (name, " | ".join(str(results[name]["median"][c]) for c in columns)))
    if args.json:
        with open(args.json, "w") as handle:
            json.dump(results, handle, indent=2)


if __name__ == "__main__":
    main()
