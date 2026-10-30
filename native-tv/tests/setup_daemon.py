#!/usr/bin/env python3
"""First-run setup against the real nativetv-daemon, without Node, over a data root that starts wiped.

  setup_daemon.py DAEMON SETUP_FLOW_BINARY

The daemon runs as tvwebctl starts it on the TV (TVWEB_NODE_FREE=1) with its stand-in luna-send, so
the TV is an OLED65B3PUA and a restart is a script that notes it. The client's first run goes through
the name, the admin PIN (the default 0000), the network choice and the phone link to the broker typed
on the TV, which the daemon takes only under the PIN's session; what the daemon wrote (config.json,
home.json, admin-pin.json) is then read back, and a launch after "Open dashboard" opens the dashboard.
"""
import json
import os
import signal
import socket
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request


def free_pair():
    """Two ports in a row: the server's and the phone link's."""
    while True:
        first = socket.socket()
        first.bind(("127.0.0.1", 0))
        port = first.getsockname()[1]
        second = socket.socket()
        try:
            second.bind(("127.0.0.1", port + 1))
        except OSError:
            continue
        finally:
            first.close()
            second.close()
        return port


def get(port, path):
    try:
        with urllib.request.urlopen("http://127.0.0.1:%d%s" % (port, path), timeout=10) as response:
            return response.status, json.loads(response.read() or b"{}")
    except urllib.error.HTTPError as error:
        return error.code, {}


def run(binary, origin, *arguments):
    done = subprocess.run([binary, origin] + list(arguments), capture_output=True, text=True, timeout=120)
    sys.stdout.write(done.stdout)
    if done.returncode != 0:
        sys.stderr.write(done.stderr)
        raise AssertionError("%s %s failed" % (os.path.basename(binary), " ".join(arguments)))


def launches(binary, origin, cases):
    """Opens the client once for each (screen, expected) at the same time and checks where each ends."""
    runs = [(screen, subprocess.Popen([binary, origin, screen, expected], stdout=subprocess.PIPE,
                                      stderr=subprocess.PIPE, text=True)) for screen, expected in cases]
    for screen, process in runs:
        out, err = process.communicate(timeout=60)
        sys.stdout.write(out)
        if process.returncode != 0:
            sys.stderr.write(err)
            raise AssertionError("launch %s failed" % screen)


def main():
    daemon, flow = os.path.abspath(sys.argv[1]), os.path.abspath(sys.argv[2])
    fake_luna = os.path.join(os.path.dirname(daemon), "native-fake-luna-send")
    port = free_pair()
    node_port = free_pair()
    with tempfile.TemporaryDirectory(prefix="nativetv-setup-daemon-") as folder:
        root = os.path.join(folder, "root")
        data = os.path.join(root, "data")
        for directory in ("assets/screensavers", "proc/net", "system/etc", "data"):
            os.makedirs(os.path.join(root, directory))
        open(os.path.join(data, ".setup-pending"), "w").close()
        restarts = os.path.join(root, "restarts")
        with open(os.path.join(data, "tvwebctl"), "w") as file:
            file.write('#!/bin/sh\necho "$@" >> "%s"\n' % restarts)
        os.chmod(os.path.join(data, "tvwebctl"), 0o755)
        config = os.path.join(root, "config.json")
        with open(config, "w") as file:
            json.dump({"host": "127.0.0.1", "port": port, "allowControl": True}, file)
        environment = dict(os.environ, TVWEB_NODE_FREE="1", TVWEB_LUNA_SEND=fake_luna, TVWEB_DATA_DIR=data,
                       NATIVETV_FAKE_LUNA_MODE="mqtt-discovery")  # the stand-in that knows the model
        environment.pop("TVWEB_PROXY_SECRET", None)
        process = subprocess.Popen(
            [daemon, "--config", config, "--heartbeat", os.path.join(root, "tvweb.beat"), "--workers", "4",
             "--listen", "--node-port", str(node_port), "--assets-root", os.path.join(root, "assets"),
             "--proc-root", os.path.join(root, "proc"), "--system-root", os.path.join(root, "system")],
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, env=environment)
        origin = "http://127.0.0.1:%d" % port
        try:
            deadline = time.monotonic() + 10
            while time.monotonic() < deadline and process.poll() is None:
                try:
                    if get(port, "/api/caps")[0] == 200:
                        break
                except OSError:
                    time.sleep(0.05)
            assert process.poll() is None, "the daemon exited early"
            status, caps = get(port, "/api/caps")
            assert caps["setupNeeded"], caps
            status, setup = get(port, "/api/setup")
            assert setup["model"] == "OLED65B3PUA", setup

            # A fresh install: the tile, Home and Settings open setup; Settings' own entry stays.
            launches(flow, origin, [("dashboard", "setup:welcome"), ("home", "setup:welcome"),
                                    ("dashboard:settings", "setup:welcome"), ("setup:handoff", "setup:handoff")])

            run(flow, origin, "real")
            with open(config) as file:
                written = json.load(file)
            assert written["host"] in ("0.0.0.0", "::"), "the network choice was written: %r" % written.get("host")
            mqtt = written.get("mqtt", {})
            assert (mqtt.get("enabled"), mqtt.get("host"), mqtt.get("port"), mqtt.get("username"),
                    mqtt.get("password")) == (True, "192.168.1.10", 1883, "lg", "secret"), mqtt
            with open(os.path.join(data, "home.json")) as file:
                profiles = json.load(file)["profiles"]
            assert profiles[0]["id"] == "main" and profiles[0]["name"] == "Den", profiles
            with open(restarts) as file:
                assert len([line for line in file if line.strip()]) >= 2, "a restart for the network and one for the broker"
            assert os.path.exists(os.path.join(data, "admin-pin.json")), "the PIN step set the admin PIN"
            status, setup = get(port, "/api/setup")
            assert setup["adminPin"] is True, setup
            status, admin = get(port, "/api/admin")
            assert admin["set"] is True and admin["defaultPin"] is True and admin["unlocked"] is False, admin
            assert os.path.exists(os.path.join(data, ".setup-pending")), "setup is not done yet"
            print("setup_daemon: the daemon wrote the network, the profile's name and the broker")

            run(flow, origin, "finish")
            assert not os.path.exists(os.path.join(data, ".setup-pending")), "Open dashboard marks setup done"
            launches(flow, origin, [("dashboard", "!setup:"), ("home", "home"), ("setup:handoff", "setup:handoff")])
        finally:
            if process.poll() is None:
                process.send_signal(signal.SIGTERM)
                try:
                    process.communicate(timeout=8)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.communicate(timeout=3)
    return 0


if __name__ == "__main__":
    sys.exit(main())
