#!/usr/bin/env python3
"""Run the client's first-run setup against the stand-in server and check what it wrote.

  setup_flow.py TEST_BINARY

On a data root that starts wiped it checks that a launch of the dashboard (the TV app's tile),
Home or Settings opens setup first, and that the Settings entry (`setup:handoff`) is left where it
is; then it walks the whole first run; then it checks that the same launches open what they were
asked for.
"""
import json
import os
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import setup_fixture  # noqa: E402


def launches(binary, origin, cases):
    """Opens the client once for each (screen, expected) at the same time and checks where each ends."""
    runs = [(screen, expected, subprocess.Popen([binary, origin, screen, expected], stdout=subprocess.PIPE,
                                                stderr=subprocess.PIPE, text=True))
            for screen, expected in cases]
    for screen, expected, process in runs:
        out, err = process.communicate(timeout=60)
        sys.stdout.write(out)
        if process.returncode != 0:
            sys.stderr.write(err)
            raise AssertionError("launch %s did not end on %s" % (screen, expected))


def main():
    with tempfile.TemporaryDirectory() as root, setup_fixture.serve(os.path.join(root, "data")) as (origin, state):
        marker = os.path.join(root, "data", ".setup-pending")
        assert os.path.exists(marker), "the data root starts wiped: first-run marker present"
        launches(sys.argv[1], origin, [("dashboard", "setup:welcome"), ("home", "setup:welcome"),
                                       ("dashboard:settings", "setup:welcome"),
                                       ("dashboard:system", "setup:welcome"),
                                       ("setup:handoff", "setup:handoff")])  # the last: Settings' own entry
        with state.lock:
            del state.calls[:]
        run = subprocess.run([sys.argv[1], origin], capture_output=True, text=True, timeout=120)
        sys.stdout.write(run.stdout)
        if run.returncode != 0:
            sys.stderr.write(run.stderr)
            return 1
        assert not os.path.exists(marker), "the first-run marker is removed once setup is done"
        assert state.config()["host"] == "0.0.0.0", "'Yes, on phones and computers' opened the dashboard"
        assert state.always is True, "Always-on was switched on"
        assert state.profile == "Den", "the TV's name was written as the main profile's: %r" % state.profile
        with open(os.path.join(root, "data", "settings.json")) as file:
            saved = json.load(file)
        mqtt = saved["mqtt"]
        assert (mqtt["enabled"], mqtt["host"], mqtt["port"], mqtt["username"], mqtt["password"]) == \
            (True, "192.168.1.10", 1883, "lg", "secret"), mqtt
        assert saved["device"]["id"] == "lg_tv", saved
        writes = [call.split(" ", 2)[1] for call in state.calls if call.startswith("POST")]
        assert writes == ["/api/home/profile", "/api/setup/network", "/api/setup/handoff", "/api/settings",
                          "/api/setup/always-ready", "/api/setup/done"], writes
        print("setup_flow: the server wrote what was chosen, in order: " + ", ".join(writes))
        # Setup is done: the tile opens the dashboard, Home opens Home, Settings opens Settings.
        launches(sys.argv[1], origin, [("dashboard", "!setup:"), ("home", "home"),
                                       ("dashboard:settings", "!setup:"),
                                       ("setup:handoff", "setup:handoff")])
    return 0


if __name__ == "__main__":
    sys.exit(main())
