"""Exercise native Live TV picture details and legacy forwarding for other kinds."""
import json
from http.server import BaseHTTPRequestHandler, HTTPServer
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import threading
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
        import socket
        first = socket.socket()
        second = socket.socket()
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


def request(port, path, token):
    req = Request("http://127.0.0.1:%d%s" % (port, path),
                  headers={"Authorization": "Bearer " + token})
    try:
        with urlopen(req, timeout=5) as response:
            return response.status, response.read()
    except HTTPError as error:
        return error.code, error.read()


class NodeFallback(BaseHTTPRequestHandler):
    def do_GET(self):
        payload = json.dumps({"fallback": self.path}).encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(payload)))
        self.end_headers()
        self.wfile.write(payload)

    def log_message(self, _format, *_args):
        pass


def main():
    binary = Path(sys.argv[1]).resolve()
    fake_luna = Path(sys.argv[2]).resolve()
    token = "home-picture-runtime-token"
    port, _ = unused_port_pair()
    node_port, _ = unused_port_pair((port, port + 1))
    fallback = HTTPServer(("127.0.0.1", node_port), NodeFallback)
    fallback_thread = threading.Thread(target=fallback.serve_forever, daemon=True)
    fallback_thread.start()

    with tempfile.TemporaryDirectory(prefix="nativetv-home-picture-") as folder:
        root = Path(folder)
        assets = root / "assets"
        assets.mkdir()
        config = root / "config.json"
        config.write_text(json.dumps({
            "host": "127.0.0.1", "port": port, "token": token,
            "allowControl": True, "panel": "OLED"
        }))
        (root / "home.json").write_text(json.dumps({
            "enabled": True, "picture": True
        }))
        details = root / "luna-details.jsonl"
        environment = os.environ.copy()
        environment["TVWEB_LUNA_SEND"] = str(fake_luna)
        environment["TVWEB_DATA_DIR"] = str(root)
        environment["TVWEB_PROXY_SECRET"] = "home-picture-proxy-secret-0123456789"
        environment["NATIVETV_FAKE_LUNA_MODE"] = "home-picture"
        environment["NATIVETV_FAKE_LUNA_DETAILS"] = str(details)
        video_fixture = Path(__file__).resolve().parents[2] / \
            "native-tv/tests/fixtures/tv/videooutput-g3-netflix-dv.json"
        environment["NATIVETV_FAKE_HOME_VIDEO"] = video_fixture.read_text()
        system = root / "system"
        receiver = system / "proc/lg/hdmi20/port0"
        receiver.mkdir(parents=True)
        fixtures = Path(__file__).resolve().parents[2] / "native-tv/tests/fixtures/tv/hdmi20-g3"
        for name in ("port0.status", "port0.pkt"):
            (receiver / name.split(".", 1)[1]).write_bytes(
                (fixtures / name).read_bytes())
        process = subprocess.Popen(
            [str(binary), "--config", str(config), "--heartbeat",
             str(root / "tvweb.beat"), "--workers", "4", "--listen",
             "--node-port", str(node_port), "--assets-root", str(assets),
             "--system-root", str(system)],
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
            env=environment)
        try:
            deadline = time.monotonic() + 8
            ready = False
            while time.monotonic() < deadline and process.poll() is None:
                try:
                    status, _ = request(port, "/api/caps", token)
                    if status == 200:
                        ready = True
                        break
                except (OSError, URLError):
                    pass
                time.sleep(0.03)
            if not ready:
                output = process.communicate(timeout=5)[0] if process.poll() is not None else ""
                raise AssertionError("native daemon did not start:\n" + output[-4000:])

            for _ in range(2):
                status, body = request(port, "/api/home/picture?kind=channel", token)
                assert status == 200
                assert json.loads(body) == {
                    "ok": True, "input": "dtv", "signal": None, "range": "sdr",
                    "mode": {"raw": "standard", "display": "Standard"},
                    "light": 47, "oled": True
                }
            for _ in range(2):
                status, body = request(
                    port, "/api/home/picture?kind=input&port=4", token)
                assert status == 200
                assert json.loads(body) == {
                    "ok": True, "input": "hdmi4",
                    "signal": {
                        "width": 3840, "height": 2160, "hz": 24,
                        "chroma": "YCbCr 4:4:4", "bits": 10, "hdr": "hdr10",
                        "vrr": False, "allm": False
                    },
                    "range": "hdr",
                    "mode": {"raw": "hdrFilmMaker", "display": "HDR Filmmaker"},
                    "light": 100, "oled": True
                }
            calls = [json.loads(line) for line in details.read_text().splitlines()]
            picture_calls = [call for call in calls
                             if call["uri"] == "com.webos.service.settings/getSystemSettings"]
            assert len(picture_calls) == 4
            assert picture_calls[0]["payload"]["dimension"] == {
                "input": "dtv", "dynamicRange": "sdr"
            }
            assert picture_calls[1]["payload"]["dimension"] == {
                "input": "dtv", "pictureMode": "standard"
            }
            assert picture_calls[2]["payload"]["dimension"] == {
                "input": "hdmi4", "dynamicRange": "hdr"
            }
            assert picture_calls[3]["payload"]["dimension"] == {
                "input": "hdmi4", "pictureMode": "hdrFilmMaker"
            }

            status, body = request(port, "/api/home/picture?kind=input&port=2", token)
            assert status == 200
            assert json.loads(body) == {
                "ok": True, "input": "hdmi2", "signal": None, "range": None,
                "mode": None, "light": None, "oled": True
            }
            expected_app = {
                "ok": True, "input": "default",
                "signal": {
                    "width": 3840, "height": 2160, "hz": 23.98,
                    "chroma": None, "bits": None, "hdr": "dolby_vision",
                    "vrr": False, "allm": False, "played": True
                },
                "range": "dolbyHdr",
                "mode": {"raw": "dolbyHdrCinemaBright",
                         "display": "Dolby Vision Cinema Bright"},
                "light": 47, "oled": True
            }
            deadline = time.monotonic() + 4
            while time.monotonic() < deadline:
                status, body = request(
                    port, "/api/home/picture?kind=app&app=netflix", token)
                if status == 200 and json.loads(body) == expected_app:
                    break
                time.sleep(0.05)
            assert json.loads(body) == expected_app, \
                "native videooutput tracking must retain the last played signal"
            status, body = request(
                port, "/api/home/picture?kind=app&app=netflix", token)
            assert status == 200 and json.loads(body) == expected_app

            # An app with no signal seen is answered here, as an input with none is.
            status, body = request(
                port, "/api/home/picture?kind=app&app=youtube", token)
            assert status == 200
            assert json.loads(body) == {
                "ok": True, "input": "default", "signal": None, "range": None,
                "mode": None, "light": None, "oled": True
            }
            status, body = request(
                port, "/api/home/picture?kind=bogus", token)
            assert status == 400
            assert json.loads(body) == {
                "ok": False, "error": "kind input, channel or app expected"}
            status, body = request(
                port, "/api/home/picture?kind=input&port=nine", token)
            assert status == 400
            assert json.loads(body) == {"ok": False, "error": "port 1 to 4 expected"}

            calls = [json.loads(line) for line in details.read_text().splitlines()]
            picture_calls = [call for call in calls
                             if call["uri"] == "com.webos.service.settings/getSystemSettings"]
            assert len(picture_calls) == 6
            assert picture_calls[4]["payload"]["dimension"] == {
                "input": "default", "dynamicRange": "dolbyHdr"
            }
            assert picture_calls[5]["payload"]["dimension"] == {
                "input": "default", "pictureMode": "dolbyHdrCinemaBright"
            }
        finally:
            if process.poll() is None:
                process.send_signal(signal.SIGTERM)
                try:
                    process.communicate(timeout=6)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=3)
            fallback.shutdown()
            fallback.server_close()
            fallback_thread.join(timeout=2)


if __name__ == "__main__":
    main()
