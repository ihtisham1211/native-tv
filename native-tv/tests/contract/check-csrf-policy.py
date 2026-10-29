#!/usr/bin/env python3
"""Keep the native front-door CSRF table aligned with the route contract."""
import json
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[2]
MANIFEST = ROOT / "contract/routes.json"
POLICY = ROOT / "src/daemon/http_csrf.cpp"
EXCEPTIONS = {
    "/mqtt": "native listener validates its local code; Node keeps the legacy fallback",
    "/api/setup": "native setup dispatcher preserves the loopback-only no-Origin-comparison contract",
    "/api/setup/handoff": "TV-only setup action preserves the no-Origin-comparison contract",
    "/api/setup/handoff-stop": "TV-only setup action preserves the no-Origin-comparison contract",
    "/api/history": "native history POST validates JSON, loopback and same-origin when Origin is present; legacy fallback is unchanged",
    "/api/backup/apps/clear": "manifest explicitly has no content-type/CSRF guard",
}


def expected_policy(routes):
    expected = {}
    for route in routes:
        if "POST" not in route["methods"]:
            continue
        csrf = route["csrf"]
        if csrf == "not applicable" or "no explicit CSRF/content-type guard" in csrf:
            continue
        if "/mqtt" in route["paths"]:
            kind = "JsonPrefix"
        elif "Exact application/octet-stream" in csrf:
            kind = "OctetStreamExact"
        elif "Exact image/jpeg" in csrf:
            kind = "JpegExact"
        elif "json" in csrf.lower():
            kind = "JsonPrefix"
        else:
            raise ValueError("Unclassified CSRF policy: " + csrf)
        for path in route["paths"]:
            if path in EXCEPTIONS:
                continue
            prior = expected.get(path)
            if prior and prior != kind:
                raise ValueError("Conflicting CSRF policies for " + path)
            expected[path] = kind
    return expected


def native_policy(source):
    return dict(re.findall(
        r'\{"([^"\n]+)", ContentTypeRule::([A-Za-z]+)\}', source))


def main():
    try:
        routes = json.loads(MANIFEST.read_text())["routes"]
        expected = expected_policy(routes)
        actual = native_policy(POLICY.read_text())
    except (OSError, ValueError, KeyError) as error:
        print("CSRF policy check: " + str(error), file=sys.stderr)
        return 1

    errors = []
    for path in sorted(set(expected) - set(actual)):
        errors.append("missing native guard: " + path)
    for path in sorted(set(actual) - set(expected)):
        errors.append("native guard has no manifest rule: " + path)
    for path in sorted(set(expected) & set(actual)):
        if expected[path] != actual[path]:
            errors.append("wrong native content-type rule for " + path +
                          ": expected " + expected[path] + ", found " + actual[path])
    for path, reason in EXCEPTIONS.items():
        if not any(path in route["paths"] for route in routes):
            errors.append("documented exception is not in manifest: " + path)
    if errors:
        print("CSRF policy check failed:", file=sys.stderr)
        print("\n".join("- " + error for error in errors), file=sys.stderr)
        return 1
    print("CSRF policy check: %d native routes and %d documented policy exceptions" %
          (len(actual), len(EXCEPTIONS)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
