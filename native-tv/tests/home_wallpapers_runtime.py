"""Exercise the native uploaded Home wallpaper read endpoint and ETag."""
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


def request(port, path, token, headers=None, method=None, data=None):
    request_headers = {"Authorization": "Bearer " + token}
    if headers:
        request_headers.update(headers)
    req = Request("http://127.0.0.1:%d%s" % (port, path), data=data,
                  headers=request_headers, method=method)
    try:
        with urlopen(req, timeout=5) as response:
            return response.status, {key.lower(): value for key, value in response.headers.items()}, response.read()
    except HTTPError as error:
        return error.code, {key.lower(): value for key, value in error.headers.items()}, error.read()


def main():
    binary = Path(sys.argv[1]).resolve()
    fake_luna = Path(sys.argv[2]).resolve()
    token = "home-wallpapers-runtime-token"
    jpeg = (Path(__file__).resolve().parents[2] /
            "native-tv/tests/fixtures/tv/home-wallpaper.jpg").read_bytes()
    port, _ = unused_port_pair()
    node_port, _ = unused_port_pair((port, port + 1))

    with tempfile.TemporaryDirectory(prefix="nativetv-home-wallpapers-") as folder:
        root = Path(folder)
        assets = root / "assets"
        assets.mkdir()
        uploads = root / "home-wallpaper-uploads"
        uploads.mkdir()
        wallpaper = b"\xff\xd8\xffnative-wallpaper-fixture\xff\xd9"
        (uploads / "evening.jpg").write_bytes(wallpaper)
        config = root / "config.json"
        config.write_text(json.dumps({
            "host": "127.0.0.1", "port": port, "token": token,
            "allowControl": True
        }))
        heartbeat = root / "tvweb.beat"
        environment = os.environ.copy()
        environment["TVWEB_LUNA_SEND"] = str(fake_luna)
        environment["TVWEB_DATA_DIR"] = str(root)
        environment["TVWEB_PROXY_SECRET"] = "home-wallpapers-proxy-secret-0123456789"
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

            status, headers, body = request(port, "/api/home/wallpaper?slot=evening", token)
            assert status == 200 and body == wallpaper
            assert headers.get("content-type") == "image/jpeg"
            assert headers.get("cache-control") == "public, max-age=86400"
            etag = headers["etag"]
            assert etag.startswith('"') and etag.endswith('"')

            status, _, body = request(port, "/api/home/wallpaper?slot=evening", token,
                                     {"If-None-Match": etag})
            assert status == 304 and not body

            status, _, body = request(port, "/api/home/wallpaper?slot=../config.json", token)
            assert status == 404
            assert json.loads(body) == {"ok": False, "error": "no wallpaper uploaded"}

            status, _, body = request(port, "/api/home/wallpaper?slot=morning", token)
            assert status == 404
            assert json.loads(body)["ok"] is False

            status, _, body = request(
                port, "/api/home/wallpapers/upload?slot=morning", token,
                {"Content-Type": "image/jpeg"}, "POST", jpeg)
            assert status == 200
            uploaded = json.loads(body)
            assert uploaded["ok"] is True
            assert uploaded["wallpapers"]["slots"]["morning"] is True
            status, _, body = request(port, "/api/home/wallpaper?slot=morning", token)
            assert status == 200 and body == jpeg

            status, _, body = request(
                port, "/api/home/wallpapers/upload?slot=morning", token,
                {"Content-Type": "image/jpeg"}, "POST", b"not a JPEG")
            assert status == 400
            assert json.loads(body)["ok"] is False
            status, _, body = request(port, "/api/home/wallpaper?slot=morning", token)
            assert status == 200 and body == jpeg

            status, _, body = request(
                port, "/api/home/wallpapers/upload?slot=morning", token,
                {"Content-Type": "application/octet-stream"}, "POST", jpeg)
            assert status == 415
            status, _, body = request(
                port, "/api/home/wallpapers/upload?slot=morning", token,
                {"Content-Type": "image/jpeg", "Origin": "http://elsewhere.example"},
                "POST", jpeg)
            assert status == 403
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
