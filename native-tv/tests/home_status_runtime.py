"""Exercise native Home status and combined status/catalogue reads."""
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
        body = error.read().decode("utf-8")
        try:
            body = json.loads(body)
        except ValueError:
            pass
        return error.code, body


def run_case(binary, fake_luna, foreground, mounted):
    token = "home-status-runtime-token"
    port, _ = unused_port_pair()
    node_port, _ = unused_port_pair((port, port + 1))

    with tempfile.TemporaryDirectory(prefix="nativetv-home-status-") as folder:
        root = Path(folder)
        assets = root / "assets"
        assets.mkdir()
        system = root / "system"
        (system / "etc").mkdir(parents=True)
        (system / "etc" / "issue").write_text("webOS TV 10.3.1\n")
        config = root / "config.json"
        config.write_text(json.dumps({
            "host": "127.0.0.1", "port": port, "token": token,
            "allowControl": False, "history": {"enabled": True}
        }))
        data = root / "home.json"
        original = json.dumps({"enabled": True, "openOnPowerOn": True})
        data.write_text(original)
        stage = root / "home-takeover"
        app_directory = system / "usr" / "palm" / "applications" / "com.webos.app.home"
        if mounted:
            stage.mkdir()
            app_directory.parent.mkdir(parents=True)
            app_directory.symlink_to(stage, target_is_directory=True)
            assert app_directory.stat().st_ino == stage.stat().st_ino
        details = root / "luna-details.jsonl"
        heartbeat = root / "tvweb.beat"
        environment = os.environ.copy()
        environment["TVWEB_LUNA_SEND"] = str(fake_luna)
        environment["TVWEB_DATA_DIR"] = str(root)
        environment["TVWEB_PROXY_SECRET"] = "home-status-proxy-secret-0123456789"
        environment["NATIVETV_FAKE_LUNA_MODE"] = "home-status"
        environment["NATIVETV_FAKE_HOME_STATUS_APP"] = foreground
        environment["NATIVETV_FAKE_LUNA_DETAILS"] = str(details)
        process = subprocess.Popen(
            [str(binary), "--config", str(config), "--heartbeat", str(heartbeat),
             "--workers", "4", "--listen", "--node-port", str(node_port),
             "--assets-root", str(assets), "--system-root", str(system)],
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
            status, home = get_json(port, "/api/home", token)
            assert status == 200, "native /api/home status was not served: %r" % home
            assert home["ok"] is True and home["enabled"] is True
            assert home["openOnPowerOn"] is True and home["writable"] is True
            assert home["supported"]["ok"] is True
            assert home["supported"]["webos"] == "10.3.1"
            assert home["supported"]["experimental"] is True
            assert home["available"] is True
            assert home["active"] is mounted, \
                "unexpected active state %r; Luna calls: %s" % (
                    home, details.read_text() if details.exists() else "<none>")
            assert home["lgHomeOpen"] is (foreground == "com.webos.app.home" and not mounted)
            assert home["history"] is True
            assert home["profiles"][0]["id"] == "main"
            assert json.loads(data.read_text()) == json.loads(original), \
                "status reads must not rewrite home.json"

            combined_status, combined = get_json(
                port, "/api/home?with=catalogue,channels", token)
            assert combined_status == 200, \
                "the combined query should be served by the native front door"
            assert combined["ok"] is True and combined["catalogue"]["ok"] is True
            assert "channels" in combined
            assert json.loads(data.read_text()) == json.loads(original), \
                "combined reads must not rewrite home.json"
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
    run_case(binary, fake_luna, "com.webos.app.netflix", True)
    run_case(binary, fake_luna, "com.webos.app.home", False)


if __name__ == "__main__":
    main()
