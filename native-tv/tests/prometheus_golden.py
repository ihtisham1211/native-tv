"""The native Prometheus renderer against the renderings recorded from the legacy one.

  prometheus_golden.py RENDERER     against fixtures/prometheus-golden.json
"""
import copy
import json
from pathlib import Path
import subprocess
import sys


RENDERER = str(Path(sys.argv[1]).resolve())
GOLDEN_FILE = Path(__file__).resolve().parent / "fixtures/prometheus-golden.json"
golden = json.loads(GOLDEN_FILE.read_text())
recorded = {}
FIXTURE_DIR = Path(__file__).resolve().parent / "fixtures/tv"
VERSION = "0.80.1"

def run(binary, payload, arguments=()):
    body = json.dumps(payload, separators=(",", ":")).encode()
    return subprocess.run([binary] + list(arguments), input=body, capture_output=True,
                          check=True, timeout=5).stdout


def compare(name, payload):
    source = golden[name].encode()
    recorded[name] = source.decode()
    native = run(RENDERER, payload)
    if source != native:
        limit = min(len(source), len(native))
        offset = next((i for i in range(limit) if source[i] != native[i]), limit)
        raise AssertionError(
            "%s renderer differs at byte %d (golden=%d bytes, native=%d bytes)\n"
            "golden: %r\nnative: %r" %
            (name, offset, len(source), len(native), source[offset:offset + 120],
             native[offset:offset + 120]))
    if source and not source.endswith(b"\n"):
        raise AssertionError(name + " output does not end with a newline")


cases = []
fixture_names = [
    "stats-b8-webos4.json", "stats-c4-webos9.json", "stats-c4-webos9-hdr.json",
    "stats-c4-webos9-standby.json", "stats-cx-webos5.json",
    "stats-cx-webos5-hdr.json", "stats-g4-webos9.json"
]
fixtures = {}
for filename in fixture_names:
    stats = json.loads((FIXTURE_DIR / filename).read_text())
    fixtures[filename] = stats
    cases.append((filename, {"stats": stats, "version": VERSION}))

c4 = copy.deepcopy(fixtures["stats-c4-webos9.json"])
c4_ntp = {"at": 1791576408500, "offsetMs": -606.25, "roundTripMs": 1.5, "stratum": 4}
cases.append(("c4-with-syslog-ntp-watch", {
    "stats": c4, "version": VERSION,
    "syslog": {"messages": {"system": 12}, "errors": 0},
    "ntp": {"sets": 1, "errors": 0, "last": c4_ntp},
    "watch": {"netflix": 5400, "hdmi2": 1800.5}
}))

playing = copy.deepcopy(c4)
playing.setdefault("sound", {})["input"] = {
    "codec": "Dolby Digital Plus", "codec_raw": "eac3",
    "immersive": "Dolby Atmos", "immersive_raw": "atmos"
}
cases.append(("c4-playing", {"stats": playing, "version": VERSION}))
cases.append(("empty", {"stats": {}, "version": VERSION}))
cases.append(("failed", {"stats": {"ok": False, "error": "timeout"}, "version": VERSION}))

labels = copy.deepcopy(c4)
labels["device"]["model"] = 'TV "\n\\ model'
labels["system"]["firmware"] = 'fw "\n\\ rev'
cases.append(("escaped-labels", {"stats": labels, "version": VERSION}))

for case_name, case_payload in cases:
    compare(case_name, case_payload)
assert set(golden) == set(recorded), "golden file and cases differ"
print("%d Prometheus renderings match byte-for-byte" % len(cases))
