#!/usr/bin/env python3
"""List the manifest routes the daemon cannot answer without Node.

Starts nativetv-daemon on fixture roots with nothing listening on its Node
upstream, requests every route in contract/routes.json, and reports the ones
that come back as the proxy's "native service unavailable" 502. Those are the
routes (or the methods of a route) that reach a Node that is not there; with
every switch on (--flags=all) the list must be empty (--fail-on-node).

    node-free-probe.py BUILD/nativetv-daemon [--fail-on-node] [--flags=STATS,INSTALL,...]
                       [--timeout=SECONDS]

--flags adds TVWEB_NATIVE_<name>=1 to the ones the front door always sets; "all"
is every switch the daemon takes over from Node (STATS, INSTALL, SERVICES,
HOME_ORCH, MQTT_PRODUCTION) and the one that starts it with no Node at all
(NODE_FREE, which is TVWEB_NODE_FREE=1).
"""
import json
import os
import signal
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path
from urllib.error import HTTPError, URLError
from urllib.request import Request, urlopen

ROOT = Path(__file__).resolve().parents[2]
SAMPLES = {
    "/assets/*": "/assets/ui/app.js",
    "/assets/google-tv/*": "/assets/google-tv/pages/home/home.js",
    # The icon listener is the main port plus two, and only with iconServer set.
    "/icons/{appId}.png": ("/icons/com.webos.app.home.png", 2),
}
BODY_METHODS = ("POST",)
ALL_FLAGS = ("STATS", "INSTALL", "SERVICES", "HOME_ORCH", "MQTT_PRODUCTION", "NODE_FREE")
# What the proxy answers when it has no Node to forward to.
UNAVAILABLE = '"native service unavailable"'


def free_port_pair():
    while True:
        with socket.socket() as first:
            first.bind(("127.0.0.1", 0))
            port = first.getsockname()[1]
        if port > 1024 and port < 65000:
            with socket.socket() as second:
                try:
                    second.bind(("127.0.0.1", port + 1))
                except OSError:
                    continue
            return port


def option(name, default):
    for argument in sys.argv[2:]:
        if argument.startswith(name + "="):
            return argument.split("=", 1)[1]
    return default


def methods_for(route):
    declared = route["methods"]
    methods = []
    for method in declared:
        if method in ("ANY", "non-GET", "non-POST"):
            methods += ["GET", "POST"]
        else:
            methods.append(method)
    return sorted(set(methods))


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    binary = Path(sys.argv[1]).resolve()
    manifest = json.loads((ROOT / "contract/routes.json").read_text())
    port = free_port_pair()
    nodePort = free_port_pair()
    while abs(nodePort - port) < 4:
        nodePort = free_port_pair()
    token = "probe-token"
    with tempfile.TemporaryDirectory(prefix="nativetv-node-free-") as folder:
        root = Path(folder)
        (root / "proc/net").mkdir(parents=True)
        (root / "system").mkdir()
        (root / "assets").mkdir()
        config = root / "config.json"
        config.write_text(json.dumps({"host": "127.0.0.1", "port": port, "token": token,
                                      "allowControl": True, "allowPower": False,
                                      "iconServer": True}))
        environment = os.environ.copy()
        environment["TVWEB_DATA_DIR"] = str(root / "data")
        # The flags native-frontdoor.js sets when it launches the daemon.
        for flag in ("HISTORY", "NTP", "REMOTE_INPUT", "SCREENTIME", "STATE", "SYSLOG"):
            environment["TVWEB_NATIVE_" + flag] = "1"
        for argument in sys.argv[2:]:
            if argument.startswith("--flags="):
                names = argument.split("=", 1)[1]
                for flag in (ALL_FLAGS if names == "all" else names.split(",")):
                    if flag == "NODE_FREE":
                        environment["TVWEB_NODE_FREE"] = "1"
                    elif flag:
                        environment["TVWEB_NATIVE_" + flag] = "1"
        environment["TVWEB_INSTALL_STAGING"] = str(root / "staging")
        fake = binary.parent / "native-fake-luna-send"
        if fake.exists():
            environment["TVWEB_LUNA_SEND"] = str(fake)
        environment["TVWEB_PROXY_SECRET"] = "probe-secret-0123456789abcdef012345"
        process = subprocess.Popen(
            [str(binary), "--config", str(config), "--heartbeat", str(root / "beat"),
             "--workers", "4", "--listen", "--node-port", str(nodePort),
             "--assets-root", str(ROOT / "assets"), "--proc-root", str(root / "proc"),
             "--system-root", str(root / "system")],
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, env=environment)
        needs_node = []
        no_answer = []
        try:
            def request(method, path, offset=0):
                data = b"{}" if method in BODY_METHODS else None
                headers = {"Authorization": "Bearer " + token}
                if data:
                    headers["Content-Type"] = "application/json"
                req = Request("http://127.0.0.1:%d%s" % (port + offset, path), data=data,
                              method=method, headers=headers)
                try:
                    with urlopen(req, timeout=float(option("--timeout", "8"))) as response:
                        return response.status, response.read().decode("utf-8", "replace")
                except HTTPError as error:
                    return error.code, error.read().decode("utf-8", "replace")
                except (OSError, URLError) as error:
                    return 0, "no answer: %s" % error

            deadline = time.monotonic() + 10
            while time.monotonic() < deadline and process.poll() is None:
                try:
                    if request("GET", "/api/caps")[0] == 200:
                        break
                except (OSError, URLError):
                    time.sleep(0.05)
            if process.poll() is not None:
                sys.exit("daemon did not start:\n" + (process.stdout.read() or "")[-2000:])
            for route in manifest["routes"]:
                for path in route["paths"]:
                    sample = SAMPLES.get(path, path)
                    offset = 0
                    if isinstance(sample, tuple):
                        sample, offset = sample
                    for method in methods_for(route):
                        status, body = request(method, sample, offset)
                        if status == 502 and UNAVAILABLE in body:
                            needs_node.append("%s %s" % (method, path))
                        elif status == 0:
                            # The fixture bus answers little, so a route that waits on
                            # Luna can outlast the probe's timeout. Not a Node dependency.
                            no_answer.append("%s %s" % (method, path))
        finally:
            if process.poll() is None:
                process.send_signal(signal.SIGTERM)
                try:
                    process.wait(timeout=8)
                except subprocess.TimeoutExpired:
                    process.kill()
    total = sum(len(methods_for(r)) * len(r["paths"]) for r in manifest["routes"])
    for entry in sorted(set(needs_node)):
        print("needs Node:", entry)
    for entry in sorted(set(no_answer)):
        print("no answer from the fixture bus:", entry)
    print("%d of %d probed method/path pairs still reach Node" % (len(set(needs_node)), total))
    if needs_node and "--fail-on-node" in sys.argv:
        sys.exit(1)


if __name__ == "__main__":
    main()
