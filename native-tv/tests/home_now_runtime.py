"""Exercise the native Home current-source endpoint and state tracker."""
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
        with urlopen(request, timeout=15) as response:
            return response.status, json.loads(response.read().decode("utf-8"))
    except HTTPError as error:
        return error.code, json.loads(error.read().decode("utf-8"))


def run_case(binary, fake_luna, app_id, expected_kind):
    token = "home-now-runtime-token"
    port, _ = unused_port_pair()
    node_port, _ = unused_port_pair((port, port + 1))

    with tempfile.TemporaryDirectory(prefix="nativetv-home-now-") as folder:
        root = Path(folder)
        assets = root / "assets"
        assets.mkdir()
        config = root / "config.json"
        config.write_text(json.dumps({
            "host": "127.0.0.1", "port": port, "token": token,
            "allowControl": True
        }))
        (root / "home.json").write_text(json.dumps({"enabled": True}))
        details = root / "luna-details.jsonl"
        state_ready = root / "foreground-ready"
        heartbeat = root / "tvweb.beat"
        environment = os.environ.copy()
        environment["TVWEB_LUNA_SEND"] = str(fake_luna)
        environment["TVWEB_DATA_DIR"] = str(root)
        environment["TVWEB_PROXY_SECRET"] = "home-now-proxy-secret-0123456789"
        environment["TVWEB_NATIVE_STATE"] = "1"
        environment["NATIVETV_FAKE_LUNA_MODE"] = "home-now"
        environment["NATIVETV_FAKE_HOME_NOW_APP"] = app_id
        environment["NATIVETV_FAKE_HOME_NOW_READY"] = str(state_ready)
        environment["NATIVETV_FAKE_LUNA_DETAILS"] = str(details)
        process = subprocess.Popen(
            [str(binary), "--config", str(config), "--heartbeat", str(heartbeat),
             "--workers", "4", "--listen", "--node-port", str(node_port),
             "--assets-root", str(assets)],
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
            env=environment)
        try:
            start_deadline = time.monotonic() + 8
            ready = False
            while time.monotonic() < start_deadline and process.poll() is None:
                try:
                    status, _ = get_json(port, "/api/caps", token)
                    if status == 200:
                        ready = True
                        break
                except (OSError, URLError, ValueError):
                    pass
                time.sleep(0.03)
            assert ready, "native daemon did not start"
            state_deadline = time.monotonic() + 8
            while time.monotonic() < state_deadline and not state_ready.exists():
                time.sleep(0.03)
            assert state_ready.exists(), "foreground subscription did not emit its snapshot"
            time.sleep(0.1)
            deadline = time.monotonic() + 8
            last = None
            while time.monotonic() < deadline and process.poll() is None:
                try:
                    status, last = get_json(port, "/api/home/now", token)
                    if status == 200 and last.get("kind") == expected_kind:
                        break
                except (OSError, URLError, ValueError):
                    pass
                time.sleep(0.2)
            if not last or last.get("kind") != expected_kind:
                if process.poll() is None:
                    process.send_signal(signal.SIGTERM)
                    output = process.communicate(timeout=6)[0]
                else:
                    output = process.communicate(timeout=3)[0]
                calls = details.read_text() if details.exists() else "<no Luna calls>"
                raise AssertionError(
                    "unexpected HomeNow response for %s: %r\nLuna calls:\n%s\nDaemon:\n%s" %
                    (app_id, last, calls, output[-5000:]))
            return last, details.read_text() if details.exists() else ""
        finally:
            if process.poll() is None:
                process.send_signal(signal.SIGTERM)
                try:
                    process.communicate(timeout=6)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=3)


def main():
    binary = Path(sys.argv[1]).resolve()
    fake_luna = Path(sys.argv[2]).resolve()

    app, app_calls = run_case(binary, fake_luna, "com.webos.app.netflix", "app")
    assert app["appId"] == "com.webos.app.netflix"
    assert app.get("title") in (None, "Netflix")
    assert app["since"] > 0
    calls = [json.loads(line)["uri"] for line in app_calls.splitlines()]
    assert "com.webos.applicationManager/getForegroundAppInfo" in calls

if __name__ == "__main__":
    main()
