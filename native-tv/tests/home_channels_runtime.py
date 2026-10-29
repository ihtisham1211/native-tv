"""Exercise the native Home channel route through the public HTTP front door."""
import json
import os
from pathlib import Path
import signal
import socket
import subprocess
import sys
import tempfile
import time
from urllib.error import URLError
from urllib.request import Request, urlopen


def unused_port_pair(excluded=()):
    excluded = set(excluded)
    first_port = 20000 + ((os.getpid() * 7919) % 39000)
    for offset in range(39000):
        port = 20000 + ((first_port - 20000 + offset) % 39000)
        if any(port + index in excluded for index in range(3)):
            continue
        first = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        second = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        try:
            first.bind(("127.0.0.1", port))
            second.bind(("127.0.0.1", port + 1))
            return port, port + 1
        except OSError:
            pass
        finally:
            first.close()
            second.close()
    raise RuntimeError("could not reserve consecutive test ports")


def request(port, token=None):
    headers = {}
    if token:
        headers["Authorization"] = "Bearer " + token
    req = Request("http://127.0.0.1:%d/api/home/channels" % port, headers=headers)
    with urlopen(req, timeout=5) as response:
        return response.status, json.loads(response.read().decode("utf-8"))


def main():
    binary = Path(sys.argv[1]).resolve()
    fake_luna = Path(sys.argv[2]).resolve()
    token = "home-channels-runtime-token"
    port, _ = unused_port_pair()
    node_port, _ = unused_port_pair((port, port + 1))

    with tempfile.TemporaryDirectory(prefix="nativetv-home-channels-") as folder:
        root = Path(folder)
        assets = root / "assets"
        assets.mkdir()
        config = root / "config.json"
        heartbeat = root / "tvweb.beat"
        details = root / "luna-details.jsonl"
        config.write_text(json.dumps({
            "host": "127.0.0.1", "port": port, "token": token,
            "allowControl": True
        }))
        environment = os.environ.copy()
        environment["TVWEB_LUNA_SEND"] = str(fake_luna)
        environment["TVWEB_DATA_DIR"] = str(root)
        environment["TVWEB_PROXY_SECRET"] = "home-channels-proxy-secret-0123456789"
        environment["NATIVETV_FAKE_LUNA_MODE"] = "home-channels"
        environment["NATIVETV_FAKE_LUNA_DETAILS"] = str(details)
        process = subprocess.Popen(
            [str(binary), "--config", str(config), "--heartbeat", str(heartbeat),
             "--workers", "4", "--listen", "--node-port", str(node_port),
             "--assets-root", str(assets)],
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
            env=environment)
        try:
            deadline = time.monotonic() + 8
            ready = False
            while time.monotonic() < deadline and process.poll() is None:
                try:
                    req = Request("http://127.0.0.1:%d/api/caps" % port,
                                  headers={"Authorization": "Bearer " + token})
                    with urlopen(req, timeout=2):
                        ready = True
                        break
                except (OSError, URLError):
                    time.sleep(0.03)
            if not ready:
                output = process.communicate(timeout=5)[0] if process.poll() is not None else ""
                raise AssertionError("native daemon did not start:\n" + output[-4000:])

            status, first = request(port, token)
            assert status == 200
            assert first == {
                "ok": True,
                "tuner": True,
                "channels": [
                    {"id": "1_1_1_0_0_0_0", "number": "1",
                     "name": "BBC One HD", "radio": False},
                    {"id": "radio-2", "number": "2",
                     "name": "Radio 2", "radio": True}
                ]
            }

            status, second = request(port, token)
            assert status == 200 and second == first
            calls = [json.loads(line) for line in details.read_text().splitlines()]
            channel_reads = [item for item in calls
                             if item["uri"] ==
                             "com.webos.service.apiadapter/tv/getChannelList"]
            assert len(channel_reads) == 1, "channel catalogue should use its 30 second cache"
        finally:
            if process.poll() is None:
                process.send_signal(signal.SIGTERM)
                try:
                    process.communicate(timeout=6)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=3)


if __name__ == "__main__":
    main()
