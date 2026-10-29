"""Exercise native Home icon routing and app-icon response compatibility."""
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


def request(port, path, token, headers=None):
    request_headers = {"Authorization": "Bearer " + token}
    if headers:
        request_headers.update(headers)
    req = Request("http://127.0.0.1:%d%s" % (port, path), headers=request_headers)
    try:
        with urlopen(req, timeout=5) as response:
            return response.status, {key.lower(): value for key, value in response.headers.items()}, response.read()
    except HTTPError as error:
        return error.code, {key.lower(): value for key, value in error.headers.items()}, error.read()


def main():
    binary = Path(sys.argv[1]).resolve()
    fake_luna = Path(sys.argv[2]).resolve()
    token = "home-icon-runtime-token"
    port, _ = unused_port_pair()
    node_port, _ = unused_port_pair((port, port + 1))

    with tempfile.TemporaryDirectory(prefix="nativetv-home-icon-") as folder:
        root = Path(folder)
        assets = root / "assets"
        assets.mkdir()
        system_root = root / "system"
        app = (system_root / "media/developer/apps/usr/palm/applications/"
               "com.example.app")
        (app / "assets").mkdir(parents=True)
        icon = b"\x89PNG\r\n\x1a\nfixture-icon"
        (app / "appinfo.json").write_text(json.dumps({
            "largeIcon": "assets/icon.png"
        }))
        (app / "assets/icon.png").write_bytes(icon)
        config = root / "config.json"
        config.write_text(json.dumps({
            "host": "127.0.0.1", "port": port, "token": token,
            "allowControl": True
        }))
        heartbeat = root / "tvweb.beat"
        environment = os.environ.copy()
        environment["TVWEB_LUNA_SEND"] = str(fake_luna)
        environment["TVWEB_DATA_DIR"] = str(root)
        environment["TVWEB_PROXY_SECRET"] = "home-icon-proxy-secret-0123456789abcdef"
        process = subprocess.Popen(
            [str(binary), "--config", str(config), "--heartbeat", str(heartbeat),
             "--workers", "4", "--listen", "--node-port", str(node_port),
             "--assets-root", str(assets), "--system-root", str(system_root)],
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
            env=environment)
        try:
            deadline = time.monotonic() + 8
            ready = False
            while time.monotonic() < deadline and process.poll() is None:
                try:
                    status, _, _ = request(port, "/api/caps", token)
                    if status == 200:
                        ready = True
                        break
                except (OSError, URLError):
                    pass
                time.sleep(0.03)
            if not ready:
                output = process.communicate(timeout=5)[0] if process.poll() is not None else ""
                raise AssertionError("native daemon did not start:\n" + output[-4000:])

            path = "/api/home/icon?id=com.example.app"
            status, headers, body = request(port, path, token)
            assert status == 200 and body == icon
            assert headers.get("content-type") == "image/png"
            assert headers.get("cache-control") == "public, max-age=86400"
            etag = headers["etag"]
            assert etag.startswith('"') and etag.endswith('"')

            status, _, body = request(port, path, token, {"If-None-Match": etag})
            assert status == 304 and not body
            status, _, app_icon = request(port, "/api/apps/icon?id=com.example.app", token)
            assert status == 200 and app_icon == icon

            status, _, body = request(port, "/api/home/icon?id=com.example.missing", token)
            assert status == 404
            assert json.loads(body) == {"ok": False, "error": "icon not found"}
            status, _, body = request(port, "/api/home/icon?id=..%2F..%2Fsecret", token)
            assert status == 404
            assert json.loads(body) == {"ok": False, "error": "icon not found"}
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
