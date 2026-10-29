"""The native Home Assistant catalogue, capability filter and discovery payloads
against the answers recorded from the legacy server's ha.js."""
import json
from pathlib import Path
import subprocess
import sys

CATALOGUE, DISCOVERY = sys.argv[1], sys.argv[2]
FIXTURES = Path(__file__).resolve().parent / "fixtures"
golden = json.loads((FIXTURES / "ha-golden.json").read_text())


def run(*arguments):
    return json.loads(subprocess.check_output(list(arguments), text=True, timeout=20))


def first_difference(actual, expected, label):
    assert len(actual) == len(expected), "%s: %d entries, golden has %d" % (
        label, len(actual), len(expected))
    for index, (a, b) in enumerate(zip(actual, expected)):
        assert a == b, "%s differs at %d:\n native=%s\n golden=%s" % (
            label, index, json.dumps(a, ensure_ascii=False)[:300],
            json.dumps(b, ensure_ascii=False)[:300])


first_difference(run(CATALOGUE, "--dump"), golden["catalogue"], "catalogue")
for name, case in golden["filters"].items():
    first_difference(run(CATALOGUE, "--filter", json.dumps(case["capabilities"], separators=(",", ":"))),
                     case["expected"], "filter " + name)
first_difference(run(DISCOVERY, "--dump"), golden["discoveryBaseline"], "discovery baseline")
first_difference(run(DISCOVERY, "--dump-dynamic", str(FIXTURES / "ha-discovery-dynamic.json")),
                 golden["discoveryDynamic"], "discovery dynamic")
print("HA: %d catalogue entries, %d filters, %d + %d discovery payloads match the recording" % (
    len(golden["catalogue"]), len(golden["filters"]), len(golden["discoveryBaseline"]),
    len(golden["discoveryDynamic"])))
