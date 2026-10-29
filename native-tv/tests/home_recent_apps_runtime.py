"""Exercise native Home recent-app listing and screenshot reads."""
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
from urllib.parse import quote
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
    return request(port, "/api/home/recents", token, request_headers, data, "POST")


def app_record(app_id, app_type="native"):
    return {"id": app_id, "appType": app_type}


def recent_record(app_id, title=None):
    result = {"appId": app_id, "title": title or app_id}
    return result


def main():
    binary = Path(sys.argv[1]).resolve()
    fake_luna = Path(sys.argv[2]).resolve()
    token = "home-recent-apps-runtime-token"
    port, _ = unused_port_pair()
    node_port, _ = unused_port_pair((port, port + 1))

    with tempfile.TemporaryDirectory(prefix="nativetv-home-recents-") as folder:
        root = Path(folder)
        assets = root / "assets"
        assets.mkdir()
        screenshots = root / "screenshots"
        screenshots.mkdir()
        png = b"\x89PNG\r\n\x1a\nrecent-app-screenshot"
        jpeg = b"\xff\xd8recent-jpeg-screenshot\xff\xd9"
        (screenshots / "first.PNG").write_bytes(png)
        (screenshots / "second.jpeg").write_bytes(jpeg)
        (screenshots / "linked.png").symlink_to(screenshots / "first.PNG")
        recent = [
            {"appId": "com.example.first", "title": "First app",
             "customImageFilePath": str(screenshots / "first.PNG")},
            {"appId": "com.example.second", "title": "Second app",
             "cardSnapShotFilePath": str(screenshots / "second.jpeg")},
            {"appId": "com.example.second", "title": "Duplicate",
             "cardSnapShotFilePath": str(screenshots / "first.PNG")},
            {"appId": "com.example.plain", "title": "No image",
             "splashBackground": str(screenshots / "plain.gif")},
            {"appId": "com.example.link", "title": "Symlink image",
             "cardSnapShotFilePath": str(screenshots / "linked.png")},
            {"appId": "com.webos.app.home", "title": "LG Home",
             "cardSnapShotFilePath": str(screenshots / "first.PNG")},
            {"appId": "com.tvweb.native", "title": "nativeTv Home",
             "cardSnapShotFilePath": str(screenshots / "first.PNG")},
            {"title": "Missing app id"}
        ]
        details = root / "luna-details.jsonl"
        state_file = root / "recent-state.json"
        state_file.write_text(json.dumps({"cards": recent, "running": []}))
        config = root / "config.json"
        config.write_text(json.dumps({
            "host": "127.0.0.1", "port": port, "token": token,
            "allowControl": True
        }))
        heartbeat = root / "tvweb.beat"
        environment = os.environ.copy()
        environment["TVWEB_LUNA_SEND"] = str(fake_luna)
        environment["TVWEB_DATA_DIR"] = str(root)
        environment["TVWEB_PROXY_SECRET"] = "home-recent-apps-proxy-secret-0123456789"
        environment["NATIVETV_FAKE_LUNA_MODE"] = "home-recents"
        environment["NATIVETV_FAKE_HOME_RECENTS"] = json.dumps(recent)
        environment["NATIVETV_FAKE_HOME_RECENTS_STATE"] = str(state_file)
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

            image_path = "/api/home/recents/image?id=" + quote("com.example.first")
            status, _, body = request(port, image_path, token)
            assert status == 404
            assert json.loads(body.decode("utf-8"))["error"] == "no picture"

            status, _, body = request(port, "/api/home/recents", token)
            listing = json.loads(body.decode("utf-8"))
            assert status == 200 and listing["ok"] is True
            assert listing["apps"] == [
                {"id": "com.example.first", "title": "First app", "image": True},
                {"id": "com.example.second", "title": "Second app", "image": True},
                {"id": "com.example.plain", "title": "No image", "image": False},
                {"id": "com.example.link", "title": "Symlink image", "image": True}
            ]

            status, headers, body = request(port, image_path, token)
            assert status == 200 and body == png
            assert headers.get("content-type") == "image/png"
            assert headers.get("cache-control") == "private, no-store"
            etag = headers["etag"]
            assert etag.startswith('"') and etag.endswith('"')
            status, _, body = request(port, image_path, token, {"If-None-Match": etag})
            assert status == 304 and not body

            status, headers, body = request(
                port, "/api/home/recents/image?id=com.example.second", token)
            assert status == 200 and body == jpeg
            assert headers.get("content-type") == "image/jpeg"

            status, _, body = request(
                port, "/api/home/recents/image?id=com.example.unknown", token)
            assert status == 404
            assert json.loads(body.decode("utf-8")) == {
                "ok": False, "error": "no picture"}
            status, _, body = request(
                port, "/api/home/recents/image?id=com.example.link", token)
            assert status == 404
            assert json.loads(body.decode("utf-8"))["ok"] is False

            calls = [json.loads(line)["uri"] for line in details.read_text().splitlines()]
            assert calls.count("com.webos.service.recent/listRecents") == 1

            status, headers, body = post(
                port, token, {"action": "open", "id": "com.example.first"})
            assert status == 200 and json.loads(body.decode("utf-8")) == {"ok": True}
            assert headers.get("content-type", "").startswith("application/json")

            status, _, body = post(port, token, {"action": "other"})
            assert status == 400
            assert json.loads(body.decode("utf-8"))["error"] == \
                "action must be open, close or closeAll"
            status, _, body = post(port, token, {"action": "open", "id": "missing"})
            assert status == 400
            assert json.loads(body.decode("utf-8"))["error"] == \
                "that app is not in recent apps"

            status, _, body = post(
                port, token, {"action": "closeAll"},
                {"Origin": "http://attacker.example"})
            assert status == 403
            assert json.loads(body.decode("utf-8"))["error"] == \
                "cross-origin request refused"
            status, _, body = post(
                port, token, {"action": "closeAll"},
                {"Content-Type": "text/plain"})
            assert status == 415

            cards = [recent_record("netflix"), recent_record("web.app"),
                     recent_record("com.webos.app.home"),
                     recent_record("com.tvweb.native")]
            running = [app_record("netflix"), app_record("web.app", "web"),
                       app_record("com.webos.app.home"),
                       app_record("com.tvweb.native"), app_record("background.app")]
            state_file.write_text(json.dumps({"cards": cards, "running": running}))
            status, _, body = post(port, token, {"action": "closeAll"})
            closed = json.loads(body.decode("utf-8"))
            assert status == 200 and closed["ok"] is True and closed["apps"] == []
            state = json.loads(state_file.read_text())
            remaining = [app["id"] for app in state["running"]]
            assert remaining == ["com.webos.app.home", "com.tvweb.native", "background.app"]
            assert [app["appId"] for app in state["cards"]] == [
                "com.webos.app.home", "com.tvweb.native"]

            netflix = [recent_record("netflix")]
            netflix_running = [app_record("netflix")]
            state_file.write_text(json.dumps({
                "cards": netflix, "running": netflix_running, "refused": True}))
            status, _, body = post(port, token, {"action": "close", "id": "netflix"})
            assert status == 400 and json.loads(body.decode("utf-8"))["ok"] is False
            assert json.loads(state_file.read_text())["cards"] == netflix

            state_file.write_text(json.dumps({
                "cards": netflix, "running": netflix_running, "ackOnly": True}))
            status, _, body = post(port, token, {"action": "close", "id": "netflix"})
            assert status == 400 and json.loads(body.decode("utf-8"))["ok"] is False
            assert json.loads(state_file.read_text())["cards"] == netflix

            state_file.write_text(json.dumps({"cards": netflix, "running": netflix_running}))
            status, _, body = post(port, token, {"action": "close", "id": "netflix"})
            assert status == 200 and json.loads(body.decode("utf-8")) == {
                "ok": True, "apps": []}
            assert json.loads(state_file.read_text())["running"] == []
            assert json.loads(state_file.read_text())["cards"] == []

            cards = [recent_record("netflix"), recent_record("web.app"),
                     recent_record("com.webos.app.home")]
            running = [app_record("netflix"), app_record("web.app", "web"),
                       app_record("com.webos.app.home"), app_record("background.app")]
            state_file.write_text(json.dumps({
                "cards": cards, "running": running, "newCard": True}))
            status, _, body = post(port, token, {"action": "closeAll"})
            updated = json.loads(body.decode("utf-8"))
            assert status == 200 and updated["ok"] is True
            assert [app["id"] for app in updated["apps"]] == ["new.app"]
            state = json.loads(state_file.read_text())
            assert [app["id"] for app in state["running"]] == [
                "com.webos.app.home", "background.app", "new.app"]
            assert [app["appId"] for app in state["cards"]] == [
                "com.webos.app.home", "new.app"]

            state_file.write_text(json.dumps({
                "cards": netflix, "running": netflix_running,
                "runningUnavailable": True}))
            status, _, body = post(port, token, {"action": "closeAll"})
            assert status == 400
            assert json.loads(body.decode("utf-8"))["error"] == \
                "the TV would not close it"
            assert json.loads(state_file.read_text())["cards"] == netflix

            calls = [json.loads(line) for line in details.read_text().splitlines()]
            assert any(call["uri"] == "com.webos.applicationManager/launch" and
                       call["payload"]["id"] == "com.example.first" for call in calls)
            assert sum(call["uri"] == "com.webos.applicationManager/closeByAppId"
                       for call in calls) >= 1
            assert sum(call["uri"] == "com.palm.webappmanager/killApp"
                       for call in calls) >= 1
            assert sum(call["uri"] == "com.webos.service.recent/removeRecent"
                       for call in calls) >= 1
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
