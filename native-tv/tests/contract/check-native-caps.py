#!/usr/bin/env python3
"""Keep /api/caps native and the release version well formed."""
import json
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[2]
manifest = json.loads((ROOT / "contract/routes.json").read_text())
groups = [group for group in manifest["routes"] if "/api/caps" in group.get("paths", [])]
if len(groups) != 1 or groups[0].get("owner") != "native":
    print("native caps contract: /api/caps must have one native owner", file=sys.stderr)
    sys.exit(1)

frontdoor = (ROOT / "src/daemon/http_frontdoor.cpp").read_text()
if 'request.uri == "/api/caps"' not in frontdoor:
    print("native caps contract: front door does not dispatch /api/caps", file=sys.stderr)
    sys.exit(1)

version = (ROOT / "VERSION").read_text().strip()
if not re.fullmatch(r"\d+\.\d+\.\d+", version):
    print("native caps contract: VERSION must be semantic version text", file=sys.stderr)
    sys.exit(1)

print("native caps contract: native ownership and version verified")
