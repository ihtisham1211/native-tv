#!/usr/bin/env python3
"""Run the client's first-run setup against the stand-in server and check what it wrote.

  setup_flow.py TEST_BINARY
"""
import json
import os
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import setup_fixture  # noqa: E402


def main():
    with tempfile.TemporaryDirectory() as root, setup_fixture.serve(os.path.join(root, "data")) as (origin, state):
        marker = os.path.join(root, "data", ".setup-pending")
        assert os.path.exists(marker), "the data root starts wiped: first-run marker present"
        run = subprocess.run([sys.argv[1], origin], capture_output=True, text=True, timeout=120)
        sys.stdout.write(run.stdout)
        if run.returncode != 0:
            sys.stderr.write(run.stderr)
            return 1
        assert not os.path.exists(marker), "the first-run marker is removed once setup is done"
        assert state.config()["host"] == "0.0.0.0", "'Yes, on phones and computers' opened the dashboard"
        assert state.always is True, "Always-on was switched on"
        with open(os.path.join(root, "data", "settings.json")) as file:
            saved = json.load(file)
        mqtt = saved["mqtt"]
        assert (mqtt["enabled"], mqtt["host"], mqtt["port"], mqtt["username"], mqtt["password"]) == \
            (True, "192.168.1.10", 1883, "lg", "secret"), mqtt
        assert saved["device"]["id"] == "lg_tv", saved
        writes = [call.split(" ", 2)[1] for call in state.calls if call.startswith("POST")]
        assert writes == ["/api/setup/network", "/api/setup/handoff", "/api/settings",
                          "/api/setup/always-ready", "/api/setup/done"], writes
        print("setup_flow: the server wrote what was chosen, in order: " + ", ".join(writes))
    return 0


if __name__ == "__main__":
    sys.exit(main())
