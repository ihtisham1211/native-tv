"""Exercise the native Home open route: tile launches, channel tuning,
recent-history stamps and the forwarded Node kinds."""
import json
import os
from http.server import BaseHTTPRequestHandler, HTTPServer
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import threading
import time
from urllib.error import HTTPError, URLError
from urllib.request import Request, urlopen


def unused_port_pair(excluded=()):
    excluded = set(excluded)
    first_port = 20000 + ((os.getpid() * 4241) % 39000)
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


def request(port, path, token, headers=None, data=None, method=None):
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


def post(port, token, payload, headers=None):
    request_headers = {"Content-Type": "application/json"}
    if headers:
        request_headers.update(headers)
    data = json.dumps(payload).encode("utf-8")
    return request(port, "/api/home/open", token, request_headers, data, "POST")


def open_recent(path):
    with open(str(path), "r", encoding="utf-8") as handle:
        return json.load(handle)


class FakeNode(BaseHTTPRequestHandler):
    received = []

    def do_POST(self):
        length = int(self.headers.get("Content-Length", "0"))
        body = self.rfile.read(length).decode("utf-8") if length else ""
        FakeNode.received.append({
            "path": self.path,
            "secret": self.headers.get("X-NativeTv-Proxy-Secret"),
            "body": body,
        })
        payload = json.dumps({"ok": True, "from": "node"}).encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(payload)))
        self.end_headers()
        self.wfile.write(payload)

    def log_message(self, *args):
        pass


def start_daemon(binary, fake_luna, root, token, port, node_port, mode, home):
    assets = root / "assets"
    assets.mkdir(exist_ok=True)
    config = root / "config.json"
    config.write_text(json.dumps({
        "host": "127.0.0.1", "port": port, "token": token,
        "allowControl": True
    }))
    home_path = root / "home.json"
    home_path.write_text(json.dumps(home))
    recent_path = root / "home-recent.json"
    recent_path.write_text(json.dumps({"app": {}, "dev": {}, "ch": {}}))
    details = root / "luna-details.jsonl"
    if details.exists():
        details.write_text("")
    heartbeat = root / "tvweb.beat"
    environment = os.environ.copy()
    environment["TVWEB_LUNA_SEND"] = str(fake_luna)
    environment["TVWEB_DATA_DIR"] = str(root)
    environment["TVWEB_PROXY_SECRET"] = "home-open-proxy-secret-0123456789"
    environment["NATIVETV_FAKE_LUNA_MODE"] = mode
    environment["NATIVETV_FAKE_LUNA_DETAILS"] = str(details)
    process = subprocess.Popen(
        [str(binary), "--config", str(config), "--heartbeat", str(heartbeat),
         "--workers", "4", "--listen", "--node-port", str(node_port),
         "--assets-root", str(assets)],
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
        env=environment)
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
    return process, recent_path, details


def calls(details):
    return [json.loads(line) for line in details.read_text().splitlines()]


def main():
    binary = Path(sys.argv[1]).resolve()
    fake_luna = Path(sys.argv[2]).resolve()
    token = "home-open-runtime-token"

    fake_node = HTTPServer(("127.0.0.1", 0), FakeNode)
    node_port = fake_node.server_address[1]
    threading.Thread(target=fake_node.serve_forever, daemon=True).start()

    home = {
        "enabled": True,
        "sort": {"app": "recent", "dev": "recent", "ch": "recent"}
    }

    with tempfile.TemporaryDirectory(prefix="nativetv-home-open-") as folder:
        root = Path(folder)
        port, _ = unused_port_pair()
        process, recent_path, details = start_daemon(
            binary, fake_luna, root, token, port, node_port, "home-open", home)
        try:
            # An app tile launches its launch point and stamps it as opened.
            status, headers, body = post(
                port, token, {"kind": "app", "id": "com.webos.app.netflix"})
            assert status == 200 and json.loads(body.decode("utf-8")) == {"ok": True}
            assert headers.get("content-type", "").startswith("application/json")
            assert headers.get("cache-control") == "no-store"
            opened = [call for call in calls(details)
                      if call["uri"] == "com.webos.applicationManager/launch"]
            assert opened and opened[-1]["payload"] == {"id": "com.webos.app.netflix"}
            stamped = open_recent(recent_path)
            assert "com.webos.app.netflix" in stamped["app"]
            assert stamped["app"]["com.webos.app.netflix"] > 0

            # Only what the catalogue lists can be opened.
            status, _, body = post(
                port, token, {"kind": "app", "id": "com.example.unknown"})
            assert status == 400
            assert json.loads(body.decode("utf-8")) == {
                "ok": False, "error": "that is not on Native Tv Home"}

            # A device opens its HDMI input, stamping the tile's own id.
            status, _, body = post(
                port, token, {"kind": "dev", "id": "hdmi2:game-console"})
            assert status == 200 and json.loads(body.decode("utf-8")) == {"ok": True}
            opened = [call for call in calls(details)
                      if call["uri"] == "com.webos.applicationManager/launch"]
            assert opened[-1]["payload"] == {"id": "com.webos.app.hdmi2"}
            stamped = open_recent(recent_path)
            assert "hdmi2:game-console" in stamped["dev"]

            status, _, body = post(port, token, {"kind": "dev", "id": "hdmi1"})
            assert status == 200 and json.loads(body.decode("utf-8")) == {"ok": True}
            opened = [call for call in calls(details)
                      if call["uri"] == "com.webos.applicationManager/launch"]
            assert opened[-1]["payload"] == {"id": "com.webos.app.hdmi1"}
            stamped = open_recent(recent_path)
            assert "hdmi1" in stamped["dev"]

            status, _, body = post(port, token, {"kind": "dev", "id": "hdmi3"})
            assert status == 400
            assert json.loads(body.decode("utf-8"))["error"] == \
                "that is not on Native Tv Home"

            # A channel tunes through the API adapter.
            status, _, body = post(port, token, {"kind": "ch", "id": "channel-1"})
            assert status == 200 and json.loads(body.decode("utf-8")) == {"ok": True}
            tunes = [call for call in calls(details)
                     if call["uri"] == "com.webos.service.apiadapter/tv/openChannel"]
            assert tunes and tunes[-1]["payload"] == {"channelId": "channel-1"}
            stamped = open_recent(recent_path)
            assert "channel-1" in stamped["ch"]

            # A feed title has no deep link of its own: only YouTube and Netflix do.
            before = len([call for call in calls(details)
                          if call["uri"] == "com.webos.applicationManager/launch"])
            status, _, body = post(
                port, token, {"kind": "title", "type": "movie", "id": 42})
            assert status == 400 and json.loads(body.decode("utf-8"))["error"] == \
                "kind must be app, dev, ch or lg"
            assert len([call for call in calls(details)
                        if call["uri"] == "com.webos.applicationManager/launch"]) == before

            # YouTube feed videos use the webOS TV app and contentTarget.
            status, _, body = post(
                port, token, {"kind": "youtube", "id": "abcdefghijk"})
            assert status == 200 and json.loads(body.decode("utf-8")) == {"ok": True}
            opened = [call for call in calls(details)
                      if call["uri"] == "com.webos.applicationManager/launch"]
            assert opened[-1]["payload"] == {
                "id": "youtube.leanback.v4",
                "params": {"contentTarget": "https://www.youtube.com/tv?v=abcdefghijk"}}

            # A no-key Netflix lookup refuses without launching or making a request.
            status, _, body = post(
                port, token, {"kind": "netflix", "type": "movie", "id": "42"})
            assert status == 400
            assert json.loads(body.decode("utf-8")) == {
                "ok": False, "error": "No Netflix link is available for this title"}

            # Native releases the LG Home takeover and launches LG Home; its
            # secret bridge updates Node's remaining foreground-state owner.
            FakeNode.received = []
            status, _, body = post(port, token, {"kind": "lg", "id": "com.webos.app.home"})
            assert status == 200
            assert json.loads(body.decode("utf-8")) == {"ok": True}
            assert len(FakeNode.received) == 1
            assert FakeNode.received[0]["path"] == "/_internal/home-escape"
            assert json.loads(FakeNode.received[0]["body"]) == {"escaped": True}
            assert FakeNode.received[0]["secret"] == "home-open-proxy-secret-0123456789"
            opened = [call for call in calls(details)
                      if call["uri"] == "com.webos.applicationManager/launch"]
            assert opened[-1]["payload"] == {"id": "com.webos.app.home"}

            status, _, body = post(port, token, {"kind": "app", "id": "lg-home"})
            assert status == 200
            assert json.loads(body.decode("utf-8")) == {"ok": True}
            assert len(FakeNode.received) == 2
            assert FakeNode.received[-1]["path"] == "/_internal/home-escape"

            status, _, body = post(port, token, {"kind": "other", "id": "x"})
            assert status == 400
            assert json.loads(body.decode("utf-8")) == {
                "ok": False, "error": "kind must be app, dev, ch or lg"}
            assert len(FakeNode.received) == 2

            # The same-origin guard protects the native tiles too.
            status, _, body = post(
                port, token, {"kind": "app", "id": "com.webos.app.netflix"},
                {"Origin": "http://attacker.example"})
            assert status == 403

            # A row sorting by Last opened only records while Home is on.
            home["enabled"] = False
            (root / "home.json").write_text(json.dumps(home))
            recent_path.write_text(json.dumps({"app": {}, "dev": {}, "ch": {}}))
            status, _, body = post(
                port, token, {"kind": "app", "id": "com.webos.app.netflix"})
            assert status == 200 and json.loads(body.decode("utf-8")) == {"ok": True}
            assert open_recent(recent_path)["app"] == {}
        finally:
            process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()

    with tempfile.TemporaryDirectory(prefix="nativetv-home-open-refused-") as folder:
        root = Path(folder)
        port, _ = unused_port_pair()
        process, recent_path, details = start_daemon(
            binary, fake_luna, root, token, port, node_port,
            "home-open-refused", home)
        try:
            # A refused launch keeps the card's stamp off and answers like Node.
            status, _, body = post(
                port, token, {"kind": "dev", "id": "hdmi2:game-console"})
            assert status == 400
            assert json.loads(body.decode("utf-8")) == {
                "ok": False, "error": "the TV would not open it"}
            assert open_recent(recent_path)["dev"] == {}

            status, _, body = post(port, token, {"kind": "ch", "id": "channel-1"})
            assert status == 400
            assert json.loads(body.decode("utf-8"))["error"] == \
                "the TV would not open it"
            assert open_recent(recent_path)["ch"] == {}
        finally:
            process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()

    fake_node.shutdown()
    print("Native Home open runtime passed.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
