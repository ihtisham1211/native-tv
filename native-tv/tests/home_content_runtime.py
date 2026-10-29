"""Exercise native Home metadata routes with TMDB and Netflix disabled."""
import json
import os
from pathlib import Path
import signal
import socket
import subprocess
import sys
import tempfile
import time
from urllib.error import HTTPError, URLError
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


def get(port, path, token):
    req = Request("http://127.0.0.1:%d%s" % (port, path),
                  headers={"Authorization": "Bearer " + token})
    try:
        with urlopen(req, timeout=5) as response:
            return response.status, response.read()
    except HTTPError as error:
        return error.code, error.read()


def main():
    binary = Path(sys.argv[1]).resolve()
    fake_luna = Path(sys.argv[2]).resolve()
    token = "home-content-runtime-token"
    port, _ = unused_port_pair()
    node_port, _ = unused_port_pair((port, port + 1))
    with tempfile.TemporaryDirectory(prefix="nativetv-home-content-") as folder:
        root = Path(folder)
        assets = root / "assets"
        assets.mkdir()
        config = root / "config.json"
        config.write_text(json.dumps({
            "host": "127.0.0.1", "port": port, "token": token,
            "allowControl": True
        }))
        environment = os.environ.copy()
        environment["TVWEB_LUNA_SEND"] = str(fake_luna)
        environment["TVWEB_DATA_DIR"] = str(root)
        environment["TVWEB_PROXY_SECRET"] = "home-content-proxy-secret-0123456789"
        process = subprocess.Popen(
            [str(binary), "--config", str(config), "--heartbeat",
             str(root / "tvweb.beat"), "--workers", "4", "--listen",
             "--node-port", str(node_port), "--assets-root", str(assets)],
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
            env=environment)
        try:
            deadline = time.monotonic() + 8
            ready = False
            while time.monotonic() < deadline and process.poll() is None:
                try:
                    status, _ = get(port, "/api/caps", token)
                    if status == 200:
                        ready = True
                        break
                except (OSError, URLError):
                    pass
                time.sleep(0.03)
            if not ready:
                output = process.communicate(timeout=5)[0] if process.poll() is not None else ""
                raise AssertionError("native daemon did not start:\n" + output[-4000:])

            paths = [
                "/api/home/details?type=movie&id=7",
                "/api/home/search?q=film",
                "/api/home/netflix?type=movie&id=7",
            ]
            for path in paths:
                status, body = get(port, path, token)
                assert status == 200
                assert json.loads(body) == {"ok": True, "enabled": False}

            status, body = get(port, "/api/home/discover", token)
            assert status == 200
            assert json.loads(body) == {
                "ok": True, "enabled": False, "rows": []
            }

            status, body = get(
                port,
                "/api/home/image?u=https%3A%2F%2Fevil.example%2Fimage.jpg",
                token)
            assert status == 404
            assert json.loads(body) == {"ok": False, "error": "no picture"}
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
