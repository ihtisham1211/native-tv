"""Exercise the native Home catalogue read path with fake Luna sources."""
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


def get_json(port, path, token):
    request = Request("http://127.0.0.1:%d%s" % (port, path),
                      headers={"Authorization": "Bearer " + token})
    try:
        with urlopen(request, timeout=5) as response:
            return response.status, json.loads(response.read().decode("utf-8"))
    except HTTPError as error:
        return error.code, json.loads(error.read().decode("utf-8"))


def main():
    binary = Path(sys.argv[1]).resolve()
    fake_luna = Path(sys.argv[2]).resolve()
    token = "home-catalogue-runtime-token"
    port, _ = unused_port_pair()
    node_port, _ = unused_port_pair((port, port + 1))

    with tempfile.TemporaryDirectory(prefix="nativetv-home-catalogue-") as folder:
        root = Path(folder)
        assets = root / "assets"
        assets.mkdir()
        config = root / "config.json"
        config.write_text(json.dumps({
            "host": "127.0.0.1", "port": port, "token": token,
            "allowControl": True
        }))
        home_file = root / "home.json"
        home_file.write_text(json.dumps({
            "enabled": True, "showLive": False,
            "channels": ["channel-1"],
            "sort": {"app": "recent", "dev": "recent", "ch": "custom"},
            "profiles": [{"id": "p1", "name": "Kids", "color": "#f2b8b5"}],
            "profile": "p1"
        }))
        original_home = home_file.read_text()
        (root / "home-recent.json").write_text(json.dumps({
            "app": {"com.webos.app.netflix": 100, "com.webos.app.youtube": 200},
            "dev": {"hdmi2": 1, "hdmi2:game-console": 2}
        }))
        details = root / "luna-details.jsonl"
        heartbeat = root / "tvweb.beat"
        environment = os.environ.copy()
        environment["TVWEB_LUNA_SEND"] = str(fake_luna)
        environment["TVWEB_DATA_DIR"] = str(root)
        environment["TVWEB_PROXY_SECRET"] = "home-catalogue-proxy-secret-0123456789"
        environment["NATIVETV_FAKE_LUNA_MODE"] = "home-catalogue"
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
                    status, _ = get_json(port, "/api/caps", token)
                    if status == 200:
                        ready = True
                        break
                except (OSError, URLError, ValueError):
                    pass
                time.sleep(0.03)
            if not ready:
                output = process.communicate(timeout=5)[0] if process.poll() is not None else ""
                raise AssertionError("native daemon did not start:\n" + output[-4000:])

            status, catalogue = get_json(port, "/api/home/catalogue?view=1", token)
            assert status == 200 and catalogue["ok"] is True
            assert [app["id"] for app in catalogue["apps"]] == [
                "com.webos.app.netflix", "com.webos.app.youtube",
                "com.webos.app.notificationcenter"]
            assert catalogue["apps"][0]["icon"] == "/api/home/icon?id=com.webos.app.netflix"
            assert [device["id"] for device in catalogue["devices"]] == [
                "hdmi1", "hdmi2:av-receiver", "hdmi2:game-console"]
            assert catalogue["order"]["app"][:3] == [
                "com.webos.app.youtube", "com.webos.app.netflix",
                "com.webos.app.notificationcenter"]
            assert catalogue["order"]["dev"] == [
                "hdmi2:game-console", "hdmi2:av-receiver", "hdmi1"]
            assert catalogue["channels"] == []
            assert catalogue["sources"] == {
                "apps": True, "devices": True, "channels": None}
            assert catalogue["profile"] == "p1"
            assert catalogue["profiles"] == [{
                "id": "p1", "name": "Kids", "color": "#f2b8b5", "active": True}]
            assert catalogue["enabled"] is True
            assert catalogue["wallpapers"]["available"] is False
            assert int(catalogue["layoutVersion"]) > 0
            assert home_file.read_text() == original_home, "GET must not write home.json"

            status, second = get_json(port, "/api/home/catalogue?view=1", token)
            assert status == 200 and second == catalogue
            calls = [json.loads(line)["uri"] for line in details.read_text().splitlines()]
            assert calls.count("com.webos.applicationManager/listLaunchPoints") == 1
            assert calls.count("com.webos.applicationManager/getAppInfo") == 2
            assert calls.count("com.webos.service.eim/getAllInputStatus") == 1
            assert "com.webos.service.apiadapter/tv/getChannelList" not in calls
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
