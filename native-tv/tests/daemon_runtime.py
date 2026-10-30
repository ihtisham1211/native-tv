"""Exercise startup, heartbeat permissions and graceful daemon shutdown."""
import gzip
import hashlib
import http.client
import io
import json
from http.server import BaseHTTPRequestHandler, HTTPServer
import os
from pathlib import Path
import signal
import shutil
import shlex
import socket
import stat
import struct
import subprocess
import sys
import threading
import tempfile
import tarfile
import time
from urllib.error import HTTPError, URLError
from urllib.parse import urlsplit
from urllib.request import Request, urlopen
import unittest


def unused_port_pair(excluded=()):
    excluded = set(excluded)
    # Keep server ports below the OS ephemeral-source range. CTest runs this
    # integration suite alongside HTTP clients, whose temporary source ports
    # can otherwise steal a just-probed ephemeral port before the fixture binds.
    base = 20000 + ((os.getpid() * 37) % 9000)
    for attempt in range(100):
        port = base + attempt * 2
        if port >= 65535 or port in excluded or port + 1 in excluded:
            continue
        first = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        second = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        try:
            first.bind(("127.0.0.1", port))
            second.bind(("127.0.0.1", port + 1))
            return port
        except OSError:
            pass
        finally:
            first.close()
            second.close()
    raise RuntimeError("could not reserve consecutive test ports")


def without_country_learning(calls):
    """Keep route assertions focused on their requested Luna operation."""
    return [call for call in calls
            if not (call.get("uri") == "com.webos.settingsservice/getSystemSettings" and
                    call.get("payload", {}).get("category") == "option" and
                    "smartServiceCountryCode2" in
                    call.get("payload", {}).get("keys", []))]


def app_install_ipk():
    def tar_gzip(entries):
        buffer = io.BytesIO()
        with tarfile.open(fileobj=buffer, mode="w:gz", format=tarfile.USTAR_FORMAT) as archive:
            for name, contents in entries:
                info = tarfile.TarInfo(name)
                if contents is None:
                    info.type = tarfile.DIRTYPE
                    archive.addfile(info)
                else:
                    info.size = len(contents)
                    archive.addfile(info, io.BytesIO(contents))
        return buffer.getvalue()

    app_id = "com.example.app"
    control = tar_gzip([("./control", b"Package: com.example.app\nVersion: 1.2.3\nArchitecture: all\n")])
    elf = bytearray(64)
    elf[:7] = b"\x7fELF\x01\x01\x01"
    elf[18:20] = struct.pack("<H", 40)
    appinfo = json.dumps({"id": app_id, "title": "Example", "version": "1.2.3",
                          "icon": "icon.png"}).encode("utf-8")
    data = tar_gzip([
        ("./usr/", None),
        ("./usr/palm/applications/", None),
        ("./usr/palm/applications/%s/" % app_id, None),
        ("./usr/palm/applications/%s/bin/" % app_id, None),
        ("./usr/palm/applications/%s/appinfo.json" % app_id, appinfo),
        ("./usr/palm/applications/%s/bin/run" % app_id, bytes(elf)),
        ("./usr/palm/services/", None),
        ("./usr/palm/services/%s.service/" % app_id, None),
        ("./usr/palm/services/%s.service/run" % app_id, bytes(elf)),
    ])

    def ar_member(name, body):
        fields = [name + "/", "0", "0", "0", "100644", str(len(body))]
        widths = [16, 12, 6, 6, 8, 10]
        header = "".join(value.ljust(width) for value, width in zip(fields, widths)) + "`\n"
        encoded = header.encode("ascii")
        if len(encoded) != 60:
            raise AssertionError("invalid ar header length")
        return encoded + body + (b"\n" if len(body) % 2 else b"")

    return (b"!<arch>\n" + ar_member("debian-binary", b"2.0\n") +
            ar_member("control.tar.gz", control) + ar_member("data.tar.gz", data))


def probe_stand_in(folder, exit_code=0):
    """A stand-in for `nativetv-tv`: --probe prints the probe's JSON line and exits as `probe-exit` says."""
    folder = Path(folder)
    exit_file = folder / "probe-exit"
    exit_file.write_text("%d\n" % exit_code)
    client = folder / "nativetv-tv"
    client.write_text(
        "#!/bin/sh\n"
        "echo \"$@\" >> \"%s\"\n"
        "echo '{\"ok\":true,\"renderer\":\"Stand-in GPU\",\"maxTextureSize\":8192}'\n"
        "exit $(cat \"%s\")\n" % (folder / "probe-calls", exit_file))
    client.chmod(0o755)
    return client


class DaemonRuntimeTests(unittest.TestCase):
    def test_help_and_runtime_lifecycle(self):
        binary = Path(sys.argv[1]).resolve()
        help_result = subprocess.run([str(binary), "--help"], capture_output=True, text=True, timeout=3)
        self.assertEqual(help_result.returncode, 0)
        self.assertIn("--heartbeat PATH", help_result.stdout)
        self.assertIn("--show-home-starting-notice", help_result.stdout)
        self.assertIn("--restore-nativetv-home", help_result.stdout)

        with tempfile.TemporaryDirectory(prefix="nativetv-daemon-") as folder:
            root = Path(folder)
            config = root / "config.json"
            heartbeat = root / "tvweb.beat"
            config.write_text(json.dumps({"port": 8177, "logging": {"level": "debug"}}))
            config.chmod(0o644)
            process = subprocess.Popen(
                [str(binary), "--config", str(config), "--heartbeat", str(heartbeat), "--workers", "2"],
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
            try:
                deadline = time.monotonic() + 5
                while time.monotonic() < deadline and not heartbeat.exists() and process.poll() is None:
                    time.sleep(0.02)
                self.assertIsNone(process.poll(), "daemon exited before the first heartbeat")
                self.assertTrue(heartbeat.is_file(), "daemon did not write its heartbeat")
                self.assertEqual(stat.S_IMODE(heartbeat.stat().st_mode), 0o600)
                self.assertTrue(heartbeat.read_text().strip().isdigit())
                self.assertEqual(stat.S_IMODE(config.stat().st_mode), 0o600)
                process.send_signal(signal.SIGTERM)
                output, _ = process.communicate(timeout=5)
                self.assertEqual(process.returncode, 0, output)
                self.assertIn("port=8177", output)
                self.assertIn("HTTP engine=CivetWeb/1.16", output)
                self.assertIn("resident runtime stopped", output)
            finally:
                if process.poll() is None:
                    process.kill()
                    process.wait(timeout=3)

    def test_invalid_worker_limit(self):
        binary = Path(sys.argv[1]).resolve()
        result = subprocess.run([str(binary), "--workers", "0"], capture_output=True,
                                text=True, timeout=3)
        self.assertEqual(result.returncode, 2)
        self.assertIn("between 1 and 64", result.stderr)

    def test_native_start_migrates_the_old_piccap_enabled_key(self):
        # Node's start-up migration is gone with Node: an old piccap.enabled is
        # saved as the PicCap Capture entity setting when the daemon starts.
        binary = Path(sys.argv[1]).resolve()
        fakeLuna = binary.parent / "native-fake-luna-send"
        publicPort = unused_port_pair()
        nodePort = unused_port_pair((publicPort, publicPort + 1))
        with tempfile.TemporaryDirectory(prefix="nativetv-piccap-migration-") as folder:
            root = Path(folder)
            config = root / "config.json"
            assets = root / "assets"
            assets.mkdir()
            config.write_text(json.dumps({
                "host": "127.0.0.1", "port": publicPort, "token": "migration-token",
                "piccap": {"enabled": False, "pollIntervalMs": 30000},
                "mqtt": {"host": "", "entities": {"disabled": ["mute"]}}}))
            environment = os.environ.copy()
            environment["TVWEB_LUNA_SEND"] = str(fakeLuna)
            environment["TVWEB_DATA_DIR"] = str(root)
            environment["TVWEB_PROXY_SECRET"] = "piccap-migration-secret-0123456789abcdef"
            process = subprocess.Popen(
                [str(binary), "--config", str(config), "--heartbeat", str(root / "tvweb.beat"),
                 "--workers", "4", "--listen", "--node-port", str(nodePort),
                 "--assets-root", str(assets)],
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                env=environment)
            try:
                deadline = time.monotonic() + 8
                while time.monotonic() < deadline and process.poll() is None:
                    try:
                        urlopen(Request("http://127.0.0.1:%d/api/caps" % publicPort,
                                        headers={"Authorization": "Bearer migration-token"}),
                                timeout=2)
                        break
                    except (OSError, URLError):
                        time.sleep(0.03)
                self.assertIsNone(process.poll(), "daemon exited early")
                saved = json.loads(config.read_text())
                self.assertEqual(saved["piccap"], {"pollIntervalMs": 30000})
                self.assertEqual(saved["mqtt"]["entities"]["disabled"], ["mute", "piccap"])
                self.assertEqual(saved["token"], "migration-token")
            finally:
                if process.poll() is None:
                    process.send_signal(signal.SIGTERM)
                    try:
                        process.communicate(timeout=6)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.communicate(timeout=3)

    def test_native_game_without_the_service_is_not_asked_again(self):
        # test-game.js: each retry starts a luna-send, which is what can freeze a TV,
        # so a TV that says it has no pipeline service is asked once.
        binary = Path(sys.argv[1]).resolve()
        fakeLuna = binary.parent / "native-fake-luna-send"
        token = "game-nosvc-test-token"
        publicPort = unused_port_pair()
        nodePort = unused_port_pair((publicPort, publicPort + 1))
        with tempfile.TemporaryDirectory(prefix="nativetv-game-nosvc-") as folder:
            root = Path(folder)
            config = root / "config.json"
            assets = root / "assets"
            assets.mkdir()
            details = root / "luna-details.jsonl"
            config.write_text(json.dumps({
                "host": "127.0.0.1", "port": publicPort, "token": token}))
            environment = os.environ.copy()
            environment["TVWEB_LUNA_SEND"] = str(fakeLuna)
            environment["TVWEB_DATA_DIR"] = str(root)
            environment["TVWEB_PROXY_SECRET"] = "game-nosvc-proxy-secret-0123456789abcdef"
            environment["NATIVETV_FAKE_LUNA_MODE"] = "game-fps"
            environment["NATIVETV_FAKE_LUNA_FPS_FORMAT"] = "nosvc"
            environment["NATIVETV_FAKE_LUNA_FPS_FOREGROUND"] = "onscreen"
            environment["NATIVETV_FAKE_LUNA_DETAILS"] = str(details)
            process = subprocess.Popen(
                [str(binary), "--config", str(config), "--heartbeat", str(root / "tvweb.beat"),
                 "--workers", "4", "--listen", "--node-port", str(nodePort),
                 "--assets-root", str(assets)],
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                env=environment)

            def get(path):
                request = Request("http://127.0.0.1:%d%s" % (publicPort, path),
                                  headers={"Authorization": "Bearer " + token})
                with urlopen(request, timeout=4) as response:
                    return response.status, response.read()

            def binds():
                if not details.exists():
                    return 0
                return len([line for line in details.read_text().splitlines()
                            if json.loads(line)["uri"] ==
                            "com.webos.service.utp.extinputs/bind"])

            try:
                deadline = time.monotonic() + 8
                while time.monotonic() < deadline and process.poll() is None:
                    try:
                        get("/api/caps")
                        break
                    except (OSError, URLError):
                        time.sleep(0.03)
                self.assertIsNone(process.poll(), "daemon exited early")
                for _ in range(8):
                    status, body = get("/api/game/fps")
                    self.assertEqual(status, 200)
                    self.assertEqual(json.loads(body)["fps"]["frameRate"], 0)
                    time.sleep(0.3)
                self.assertEqual(binds(), 1,
                                 "a TV without the service must be asked to bind once")
            finally:
                if process.poll() is None:
                    process.send_signal(signal.SIGTERM)
                    try:
                        process.communicate(timeout=6)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.communicate(timeout=3)

    def test_native_game_fps_stream_contract(self):
        binary = Path(sys.argv[1]).resolve()
        fakeLuna = binary.parent / "native-fake-luna-send"
        token = "game-fps-test-token"
        secret = "game-fps-proxy-secret-0123456789abcdef"
        scenarios = [
            ("", "onscreen", {"frameRate": 119.88, "vrrType": "VRR",
                               "port": "HDMI_2"}),
            ("legacy", "onscreen", {"frameRate": 60, "vrrType": "off",
                                     "port": "HDMI_2"}),
            ("false", "onscreen", {"frameRate": 59, "vrrType": "off",
                                    "port": "HDMI_2"}),
            ("", "offscreen", {"frameRate": 0, "vrrType": "off", "port": None}),
        ]

        for fpsFormat, foreground, expected in scenarios:
            publicPort = unused_port_pair()
            nodePort = unused_port_pair((publicPort, publicPort + 1))
            with tempfile.TemporaryDirectory(prefix="nativetv-game-fps-") as folder:
                root = Path(folder)
                config = root / "config.json"
                heartbeat = root / "tvweb.beat"
                assets = root / "assets"
                assets.mkdir()
                details = root / "luna-details.jsonl"
                config.write_text(json.dumps({
                    "host": "127.0.0.1", "port": publicPort, "token": token
                }))
                environment = os.environ.copy()
                environment["TVWEB_LUNA_SEND"] = str(fakeLuna)
                environment["TVWEB_DATA_DIR"] = str(root)
                environment["TVWEB_PROXY_SECRET"] = secret
                environment["NATIVETV_FAKE_LUNA_MODE"] = "game-fps"
                environment["NATIVETV_FAKE_LUNA_FPS_FORMAT"] = fpsFormat
                environment["NATIVETV_FAKE_LUNA_FPS_FOREGROUND"] = foreground
                environment["NATIVETV_FAKE_LUNA_DETAILS"] = str(details)
                process = subprocess.Popen(
                    [str(binary), "--config", str(config), "--heartbeat", str(heartbeat),
                     "--workers", "4", "--listen", "--node-port", str(nodePort),
                     "--assets-root", str(assets)],
                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                    env=environment)

                def get(path, method="GET"):
                    request = Request(
                        "http://127.0.0.1:%d%s" % (publicPort, path), method=method,
                        headers={"Authorization": "Bearer " + token})
                    with urlopen(request, timeout=4) as response:
                        return response.status, response.read()

                try:
                    deadline = time.monotonic() + 8
                    ready = False
                    while time.monotonic() < deadline and process.poll() is None:
                        try:
                            get("/api/caps")
                            ready = True
                            break
                        except (OSError, URLError):
                            time.sleep(0.03)
                    if not ready:
                        if process.poll() is None:
                            process.send_signal(signal.SIGTERM)
                        output, _ = process.communicate(timeout=6)
                        self.fail("native daemon did not start:\n" + output[-4000:])

                    observed = None
                    deadline = time.monotonic() + 8
                    while time.monotonic() < deadline:
                        status, body = get("/api/game/fps")
                        self.assertEqual(status, 200)
                        observed = json.loads(body)["fps"]
                        records = []
                        if details.exists():
                            for line in details.read_text().splitlines():
                                try:
                                    records.append(json.loads(line))
                                except ValueError:
                                    continue
                        reading = [item for item in records
                                   if item["uri"] ==
                                   "com.webos.service.utp.extinputs/getVRRInfo" and
                                   item["payload"].get("subscribe") is True]
                        if reading and observed == expected:
                            break
                        time.sleep(0.05)
                    self.assertTrue(reading,
                                    "native FPS route did not start its Game Optimizer stream")
                    self.assertEqual(observed, expected)
                    self.assertEqual(reading[-1]["payload"], {
                        "subscribe": True, "pipelineId": "pipeline-hdmi2",
                        "mode": "GameOptimizer", "interval": 1000})
                    bind = [item for item in records
                            if item["uri"] ==
                            "com.webos.service.utp.extinputs/bind" and
                            item["payload"].get("subscribe") is True]
                    self.assertTrue(bind, "FPS polling must bind the HDMI input pipeline")

                    status, body = get("/api/game/fps", method="HEAD")
                    self.assertEqual(status, 200)
                    self.assertEqual(body, b"")
                finally:
                    if process.poll() is None:
                        process.send_signal(signal.SIGTERM)
                        try:
                            process.communicate(timeout=6)
                        except subprocess.TimeoutExpired:
                            process.kill()
                            process.wait(timeout=3)

    def test_native_piccap_control_gates_and_refreshes_state(self):
        binary = Path(sys.argv[1]).resolve()
        fakeLuna = binary.parent / "native-fake-luna-send"
        token = "piccap-test-token"
        secret = "piccap-proxy-secret-0123456789abcdef"
        scenarios = [
            {"name": "missing-app", "mode": "piccap", "version": "4.4.3",
             "installed": False, "values": ["on"], "expected": [
                 {"ok": False, "error": "PicCap is not available"}], "calls": []},
            {"name": "webos-two", "mode": "piccap", "version": "2.2.3",
             "installed": True, "values": ["on"], "expected": [
                 {"ok": False, "error": "PicCap is not available"}], "calls": []},
            {"name": "missing-service", "mode": "piccap-unknown", "version": "4.4.3",
             "installed": True, "values": ["on"], "expected": [
                 {"ok": False, "error": "PicCap is not available"}], "calls": [
                     "org.webosbrew.piccap.service/status"]},
            {"name": "available", "mode": "piccap", "version": "4.4.3",
             "installed": True, "values": ["on", "OFF"], "expected": [
                 {"ok": True}, {"ok": True}], "calls": [
                     "org.webosbrew.piccap.service/status",
                     "org.webosbrew.piccap.service/start",
                     "org.webosbrew.piccap.service/status",
                     "org.webosbrew.piccap.service/status",
                     "org.webosbrew.piccap.service/stop",
                     "org.webosbrew.piccap.service/status"]},
        ]

        for scenario in scenarios:
            publicPort = unused_port_pair()
            nodePort = unused_port_pair((publicPort, publicPort + 1))
            with tempfile.TemporaryDirectory(prefix="nativetv-piccap-") as folder:
                root = Path(folder)
                app = root / "media/developer/apps/usr/palm/applications/org.webosbrew.piccap"
                if scenario["installed"]:
                    app.mkdir(parents=True)
                etc = root / "etc"
                etc.mkdir()
                (etc / "issue").write_text("webOS TV " + scenario["version"] + "\n")
                config = root / "config.json"
                heartbeat = root / "tvweb.beat"
                assets = root / "assets"
                assets.mkdir()
                details = root / "luna-details.jsonl"
                config.write_text(json.dumps({
                    "host": "127.0.0.1", "port": publicPort, "token": token,
                    "allowControl": True
                }))
                environment = os.environ.copy()
                environment["TVWEB_LUNA_SEND"] = str(fakeLuna)
                environment["TVWEB_DATA_DIR"] = str(root)
                environment["TVWEB_PROXY_SECRET"] = secret
                environment["NATIVETV_FAKE_LUNA_MODE"] = scenario["mode"]
                environment["NATIVETV_FAKE_LUNA_DETAILS"] = str(details)
                process = subprocess.Popen(
                    [str(binary), "--config", str(config), "--heartbeat", str(heartbeat),
                     "--workers", "4", "--listen", "--node-port", str(nodePort),
                     "--assets-root", str(assets), "--system-root", str(root)],
                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                    env=environment)

                def request(path, method="GET", body=None):
                    data = None if body is None else json.dumps(body).encode("utf-8")
                    headers = {"Authorization": "Bearer " + token}
                    if data is not None:
                        headers["Content-Type"] = "application/json"
                    req = Request("http://127.0.0.1:%d%s" % (publicPort, path),
                                  data=data, method=method, headers=headers)
                    try:
                        with urlopen(req, timeout=8) as response:
                            return response.status, response.read()
                    except HTTPError as error:
                        return error.code, error.read()

                try:
                    deadline = time.monotonic() + 8
                    ready = False
                    while time.monotonic() < deadline and process.poll() is None:
                        try:
                            request("/api/caps")
                            ready = True
                            break
                        except (OSError, URLError):
                            time.sleep(0.03)
                    if not ready:
                        if process.poll() is None:
                            process.send_signal(signal.SIGTERM)
                        output, _ = process.communicate(timeout=6)
                        self.fail("native daemon did not start:\n" + output[-4000:])

                    responses = []
                    for value in scenario["values"]:
                        status, body = request(
                            "/api/control", method="POST",
                            body={"action": "piccap", "value": value})
                        self.assertEqual(status, 200,
                                         scenario["name"] + ": " +
                                         body.decode("utf-8", "replace"))
                        responses.append(json.loads(body))
                    self.assertEqual(responses, scenario["expected"], scenario["name"])
                    recorded = ([json.loads(line)
                                 for line in details.read_text().splitlines()]
                                if details.exists() else [])
                    calls = [call["uri"] for call in without_country_learning(recorded)]
                    self.assertEqual(calls, scenario["calls"], scenario["name"])
                    if scenario["name"] == "available":
                        process.send_signal(signal.SIGTERM)
                        output, _ = process.communicate(timeout=6)
                        self.assertEqual(process.returncode, 0, output)
                        self.assertIn(
                            "command-policy: action=control.piccap.start result=success",
                            output)
                        self.assertIn(
                            "command-policy: action=control.piccap.stop result=success",
                            output)
                finally:
                    if process.poll() is None:
                        process.send_signal(signal.SIGTERM)
                        try:
                            process.communicate(timeout=6)
                        except subprocess.TimeoutExpired:
                            process.kill()
                            process.wait(timeout=3)

    def test_native_quick_access_write_preserves_legacy_contract(self):
        binary = Path(sys.argv[1]).resolve()
        fakeLuna = binary.parent / "native-fake-luna-send"
        publicPort = unused_port_pair()
        nodePort = unused_port_pair((publicPort, publicPort + 1))
        token = "quick-access-test-token"
        secret = "quick-access-proxy-secret-0123456789abcdef"
        nodeRequests = []

        class NodeCaptureHandler(BaseHTTPRequestHandler):
            def do_POST(self):
                length = int(self.headers.get("Content-Length", "0"))
                body = self.rfile.read(length)
                nodeRequests.append({
                    "path": self.path,
                    "body": body.decode("utf-8", "replace"),
                    "secret": self.headers.get("X-NativeTv-Proxy-Secret"),
                    "originalRemote": self.headers.get("X-NativeTv-Original-Remote"),
                })
                if self.path == "/_internal/native-state":
                    self.send_response(204)
                    self.send_header("Content-Length", "0")
                    self.end_headers()
                    return
                response = b'{"ok":true,"forwarded":true}'
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(response)))
                self.end_headers()
                self.wfile.write(response)

            def log_message(self, *_args):
                pass

        node = HTTPServer(("127.0.0.1", nodePort), NodeCaptureHandler)
        nodeThread = threading.Thread(target=node.serve_forever, daemon=True)
        nodeThread.start()

        with tempfile.TemporaryDirectory(prefix="nativetv-native-quick-access-") as folder:
            root = Path(folder)
            config = root / "config.json"
            heartbeat = root / "tvweb.beat"
            assets = root / "assets"
            assets.mkdir()
            details = root / "luna-details.jsonl"
            config.write_text(json.dumps({
                "host": "127.0.0.1", "port": publicPort, "token": token,
                "allowControl": True
            }))
            environment = os.environ.copy()
            environment["TVWEB_LUNA_SEND"] = str(fakeLuna)
            environment["TVWEB_DATA_DIR"] = str(root)
            environment["TVWEB_PROXY_SECRET"] = secret
            environment["NATIVETV_FAKE_LUNA_MODE"] = "quick-access"
            environment["NATIVETV_FAKE_LUNA_DETAILS"] = str(details)
            process = subprocess.Popen(
                [str(binary), "--config", str(config), "--heartbeat", str(heartbeat),
                 "--workers", "4", "--listen", "--node-port", str(nodePort),
                 "--assets-root", str(assets)],
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, env=environment)
            try:
                deadline = time.monotonic() + 8
                ready = False
                while time.monotonic() < deadline and process.poll() is None:
                    try:
                        request = Request(
                            "http://127.0.0.1:%d/api/caps" % publicPort,
                            headers={"Authorization": "Bearer " + token})
                        response = urlopen(request, timeout=1)
                        response.read()
                        ready = True
                        break
                    except (OSError, URLError):
                        time.sleep(0.03)
                if not ready:
                    if process.poll() is None:
                        process.send_signal(signal.SIGTERM)
                    output, _ = process.communicate(timeout=6)
                    self.fail("native daemon did not start:\n" + output[-4000:])

                def post(body, authorization=token, ensure_ascii=True):
                    request = Request(
                        "http://127.0.0.1:%d/api/quickaccess" % publicPort,
                        data=json.dumps(body, ensure_ascii=ensure_ascii).encode("utf-8"), method="POST",
                        headers={"Content-Type": "application/json",
                                 "Authorization": "Bearer " + authorization})
                    try:
                        response = urlopen(request, timeout=8)
                        return response.status, response.read()
                    except HTTPError as error:
                        return error.code, error.read()

                def postControl(raw):
                    request = Request(
                        "http://127.0.0.1:%d/api/control" % publicPort,
                        data=raw.encode("utf-8"), method="POST",
                        headers={"Content-Type": "application/json",
                                 "Authorization": "Bearer " + token})
                    try:
                        response = urlopen(request, timeout=8)
                        return response.status, response.read()
                    except HTTPError as error:
                        return error.code, error.read()

                def getServiceMenu(method="GET"):
                    request = Request(
                        "http://127.0.0.1:%d/api/servicemenu" % publicPort,
                        method=method,
                        headers={"Authorization": "Bearer " + token})
                    try:
                        with urlopen(request, timeout=8) as response:
                            return response.status, response.read()
                    except HTTPError as error:
                        return error.code, error.read()

                def getApps():
                    req = Request("http://127.0.0.1:%d/api/apps" % publicPort,
                                  headers={"Authorization": "Bearer " + token})
                    with urlopen(req, timeout=30) as response:
                        return json.loads(response.read())

                def postPreload(body):
                    request = Request(
                        "http://127.0.0.1:%d/api/preload/toggle" % publicPort,
                        data=json.dumps(body).encode("utf-8"), method="POST",
                        headers={"Content-Type": "application/json",
                                 "Authorization": "Bearer " + token})
                    try:
                        response = urlopen(request, timeout=8)
                        return response.status, response.read()
                    except HTTPError as error:
                        return error.code, error.read()

                def postHdmiSeen(diag, proxySecret=None, originalRemote=None):
                    headers = {"Content-Type": "application/json"}
                    if proxySecret is not None:
                        headers["X-NativeTv-Proxy-Secret"] = proxySecret
                    if originalRemote is not None:
                        headers["X-NativeTv-Original-Remote"] = originalRemote
                    request = Request(
                        "http://127.0.0.1:%d/_internal/hdmi-seen" % publicPort,
                        data=json.dumps({"diag": diag}).encode("utf-8"),
                        method="POST", headers=headers)
                    try:
                        response = urlopen(request, timeout=5)
                        return response.status, response.read()
                    except HTTPError as error:
                        return error.code, error.read()

                status, _body = postHdmiSeen({"port": 2, "allm": False})
                self.assertEqual(status, 403,
                                 "HDMI observations require the native loopback secret")
                status, _body = postHdmiSeen(
                    {"port": 2, "allm": False}, "wrong-secret")
                self.assertEqual(status, 403,
                                 "an invalid native loopback secret is rejected")
                status, _body = postHdmiSeen(
                    {"port": 2, "allm": False}, secret, "192.0.2.20")
                self.assertEqual(status, 403,
                                 "a forwarded external peer cannot write HDMI markers")
                self.assertFalse((root / "hdmi_seen").exists(),
                                 "refused observations must not touch the marker")
                status, body = postHdmiSeen(
                    {"port": 2, "phy_mode": "FRL 48 Gbps", "allm": False,
                     "vrr": None}, secret)
                self.assertEqual(status, 204, body.decode("utf-8", "replace"))
                self.assertEqual((root / "hdmi_seen").read_text(),
                                 "allm\nphy_mode\n",
                                 "the native route persists non-null fields except port")
                status, _body = postHdmiSeen(
                    {"port": 1, "hdcp": "HDCP 2.3", "allm": False}, secret)
                self.assertEqual(status, 204)
                self.assertEqual((root / "hdmi_seen").read_text(),
                                 "allm\nhdcp\nphy_mode\n",
                                 "the native route merges new fields in sorted order")

                status, body = postControl(
                    '{"action":"volume","value":"118percent"}')
                self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                self.assertEqual(json.loads(body), {"ok": True})
                calls = [json.loads(line) for line in details.read_text().splitlines()]
                volumeWrites = [call for call in calls
                                if call["uri"] == "com.webos.audio/setVolume"]
                self.assertEqual(volumeWrites[-1]["payload"], {"volume": 100},
                                 "native volume preserves parseInt and clamp behavior")
                invalidations = [item for item in nodeRequests
                                 if item["path"] == "/_internal/native-state"]
                self.assertEqual(len(invalidations), 1,
                                 "a successful native volume change invalidates Node caches")
                self.assertEqual(json.loads(invalidations[0]["body"]),
                                 {"type": "invalidate", "group": "control"})
                self.assertEqual(invalidations[0]["secret"], secret)
                self.assertIsNone(invalidations[0]["originalRemote"])

                status, body = postControl('{"action":"mute","value":"ON"}')
                self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                self.assertEqual(json.loads(body), {"ok": True})
                calls = [json.loads(line) for line in details.read_text().splitlines()]
                muteWrites = [call for call in calls
                              if call["uri"] == "com.webos.audio/setMuted"]
                self.assertEqual(muteWrites[-1]["payload"], {"muted": True},
                                 "native mute preserves legacy true-value matching")
                invalidations = [item for item in nodeRequests
                                 if item["path"] == "/_internal/native-state"]
                self.assertEqual(len(invalidations), 2,
                                 "successful mute changes also invalidate Node caches")

                for action, expectedUri in (
                        ("screenOff", "com.webos.service.tvpower/power/turnOffScreen"),
                        ("screenOn", "com.webos.service.tvpower/power/turnOnScreen")):
                    status, body = postControl('{"action":"' + action + '"}')
                    self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                    self.assertEqual(json.loads(body), {"ok": True})
                    calls = [json.loads(line) for line in details.read_text().splitlines()]
                    self.assertTrue(any(call["uri"] == expectedUri and
                                        call["payload"] == {}
                                        for call in calls),
                                    action + " uses the matching TV power service")
                    invalidations = [item for item in nodeRequests
                                     if item["path"] == "/_internal/native-state"]
                    self.assertEqual(len(invalidations), 2 +
                                     (1 if action == "screenOff" else 2),
                                     action + " invalidates Node's control caches")

                inputStart = len(calls)
                status, body = postControl('{"action":"input","value":"hdmi2"}')
                self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                self.assertEqual(json.loads(body), {"ok": True})
                calls = [json.loads(line) for line in details.read_text().splitlines()]
                inputWrites = [call for call in calls[inputStart:]
                               if call["uri"] == "com.webos.applicationManager/launch"]
                self.assertEqual(inputWrites[-1]["payload"],
                                 {"id": "com.webos.app.hdmi2"},
                                 "native input control launches the legacy input app id")
                invalidations = [item for item in nodeRequests
                                 if item["path"] == "/_internal/native-state"]
                self.assertEqual(len(invalidations), 5,
                                 "a successful input change invalidates Node caches")

                status, body = postControl('{"action":"input","value":"hdmi9"}')
                self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                self.assertEqual(json.loads(body),
                                 {"ok": False, "error": "unknown input"})
                self.assertEqual(len([item for item in nodeRequests
                                      if item["path"] == "/_internal/native-state"]), 5,
                                 "an invalid input does not invalidate state")

                pictureStart = len(calls)
                status, body = postControl(
                    '{"action":"pictureMode","value":"  Cinema  "}')
                self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                self.assertEqual(json.loads(body), {"ok": True})
                calls = [json.loads(line) for line in details.read_text().splitlines()]
                pictureCalls = calls[pictureStart:]
                self.assertEqual([call["uri"] for call in pictureCalls], [
                    "com.webos.service.settings/getSystemSettings",
                    "com.webos.service.settings/setSystemSettings"],
                    "picture mode reads the active input dimension before writing")
                self.assertEqual(pictureCalls[-1]["payload"], {
                    "category": "picture",
                    "settings": {"pictureMode": "Cinema"},
                    "dimension": {"input": "hdmi2"}},
                    "picture mode preserves the active LG settings dimension")
                self.assertEqual(len([item for item in nodeRequests
                                      if item["path"] == "/_internal/native-state"]), 6,
                                 "successful picture mode invalidates Node caches")

                status, body = postControl(
                    '{"action":"picture_mode","value":"sports"}')
                self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                self.assertEqual(json.loads(body), {
                    "ok": False, "error": "picture mode is locked while in Gallery"})
                calls = [json.loads(line) for line in details.read_text().splitlines()]
                self.assertEqual(calls[-1]["uri"],
                                 "com.webos.service.settings/getSystemSettings",
                                 "Gallery guard refuses before writing a picture mode")
                self.assertEqual(len([item for item in nodeRequests
                                      if item["path"] == "/_internal/native-state"]), 6,
                                 "a Gallery refusal does not invalidate Node caches")

                status, body = postControl(
                    '{"action":"energySaving","value":" MAX "}')
                self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                self.assertEqual(json.loads(body), {"ok": True})
                calls = [json.loads(line) for line in details.read_text().splitlines()]
                self.assertEqual(calls[-1]["payload"], {
                    "category": "picture",
                    "settings": {"energySaving": "max", "energySavingModified": "true"}},
                    "energy saving normalizes the accepted setting and modified marker")
                self.assertEqual(len([item for item in nodeRequests
                                      if item["path"] == "/_internal/native-state"]), 7)

                callsBeforeInvalidEnergy = len(calls)
                status, body = postControl(
                    '{"action":"energySaving","value":"maximum"}')
                self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                self.assertEqual(json.loads(body), {
                    "ok": False,
                    "error": "energy saving must be one of auto, off, min, med, max, screen_off"})
                calls = [json.loads(line) for line in details.read_text().splitlines()]
                self.assertEqual(len(calls), callsBeforeInvalidEnergy,
                                 "invalid energy-saving values do not reach Luna")
                self.assertEqual(len([item for item in nodeRequests
                                      if item["path"] == "/_internal/native-state"]), 7)

                status, body = postControl(
                    '{"action":"backlight","value":" 40.6percent"}')
                self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                self.assertEqual(json.loads(body), {"ok": True})
                calls = [json.loads(line) for line in details.read_text().splitlines()]
                self.assertEqual(calls[-1]["payload"], {
                    "category": "picture", "settings": {"backlight": "41"}},
                    "backlight preserves parseFloat rounding and string setting values")
                self.assertEqual(len([item for item in nodeRequests
                                      if item["path"] == "/_internal/native-state"]), 8)

                status, body = postControl(
                    '{"action":"standbyLight","value":true}')
                self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                self.assertEqual(json.loads(body), {"ok": True})
                calls = [json.loads(line) for line in details.read_text().splitlines()]
                self.assertEqual(calls[-1]["payload"], {
                    "category": "option", "settings": {"standByLight": "on"}},
                    "standby light keeps the legacy system-setting name and value")
                self.assertEqual(len([item for item in nodeRequests
                                      if item["path"] == "/_internal/native-state"]), 9)

                status, body = postControl(
                    '{"action":"logoLight","value":"1"}')
                self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                self.assertEqual(json.loads(body), {"ok": True})
                calls = [json.loads(line) for line in details.read_text().splitlines()]
                self.assertEqual(calls[-1]["payload"], {
                    "category": "option", "settings": {"logoLight": "off"}},
                    "logo light retains exact truth-value matching")
                self.assertEqual(len([item for item in nodeRequests
                                      if item["path"] == "/_internal/native-state"]), 10)

                status, body = postControl(
                    '{"action":"launchUrl","value":" HTTPS://example.com/watch "}')
                self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                self.assertEqual(json.loads(body), {"ok": True})
                calls = [json.loads(line) for line in details.read_text().splitlines()]
                self.assertEqual(calls[-1]["uri"], "com.webos.applicationManager/launch")
                self.assertEqual(calls[-1]["payload"], {
                    "id": "com.webos.app.browser",
                    "params": {"target": "HTTPS://example.com/watch"}},
                    "launch URL uses the browser app with the trimmed target")
                self.assertEqual(len([item for item in nodeRequests
                                      if item["path"] == "/_internal/native-state"]), 11)

                callsBeforeInvalidUrl = len(calls)
                status, body = postControl(
                    '{"action":"launch_url","value":"file:///etc/passwd"}')
                self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                self.assertEqual(json.loads(body), {
                    "ok": False, "error": "url must start with http:// or https://"})
                calls = [json.loads(line) for line in details.read_text().splitlines()]
                self.assertEqual(len(calls), callsBeforeInvalidUrl,
                                 "unsafe URL schemes do not reach Luna")

                status, body = postControl(
                    '{"action":"launchApp","value":"Netflix"}')
                self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                self.assertEqual(json.loads(body), {"ok": True})
                calls = [json.loads(line) for line in details.read_text().splitlines()]
                self.assertEqual(calls[-2]["uri"],
                                 "com.webos.applicationManager/listApps")
                self.assertEqual(calls[-1]["uri"], "com.webos.applicationManager/launch")
                self.assertEqual(calls[-1]["payload"], {"id": "netflix"},
                                 "launch-app resolves a case-insensitive installed title")
                self.assertEqual(len([item for item in nodeRequests
                                      if item["path"] == "/_internal/native-state"]), 12)

                status, body = postControl(
                    '{"action":"closeApp","value":"  com.webos.app.netflix  "}')
                self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                self.assertEqual(json.loads(body), {"ok": True})
                calls = [json.loads(line) for line in details.read_text().splitlines()]
                self.assertEqual(calls[-1]["uri"],
                                 "com.webos.applicationManager/closeByAppId")
                self.assertEqual(calls[-1]["payload"], {"id": "com.webos.app.netflix"},
                                 "close-app trims and forwards the target app id")
                self.assertEqual(len([item for item in nodeRequests
                                      if item["path"] == "/_internal/native-state"]), 13)

                callsBeforeMissingApp = len(calls)
                status, body = postControl('{"action":"close_app","value":0}')
                self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                self.assertEqual(json.loads(body), {
                    "ok": False, "error": "missing app id"})
                calls = [json.loads(line) for line in details.read_text().splitlines()]
                self.assertEqual(len(calls), callsBeforeMissingApp,
                                 "a missing close-app id does not reach Luna")

                callsBeforeInvalidBacklight = len(calls)
                status, body = postControl('{"action":"backlight","value":101}')
                self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                self.assertEqual(json.loads(body), {
                    "ok": False, "error": "backlight must be between 0 and 100"})
                calls = [json.loads(line) for line in details.read_text().splitlines()]
                self.assertEqual(len(calls), callsBeforeInvalidBacklight,
                                 "out-of-range backlight values do not reach Luna")
                self.assertEqual(len([item for item in nodeRequests
                                      if item["path"] == "/_internal/native-state"]), 13)

                stepStart = len(calls)
                status, body = postControl('{"action":"volumeStep","value":1}')
                self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                self.assertEqual(json.loads(body), {"ok": True})
                calls = [json.loads(line) for line in details.read_text().splitlines()]
                stepCalls = [call["uri"] for call in calls[stepStart:]]
                self.assertEqual(stepCalls, [
                    "com.webos.service.networkinput/test/sendKeyCode",
                    "com.webos.audio/volumeUp"],
                    "a refused remote key falls back to the TV audio volume service")
                invalidations = [item for item in nodeRequests
                                 if item["path"] == "/_internal/native-state"]
                self.assertEqual(len(invalidations), 14)

                stepStart = len(calls)
                status, body = postControl('{"action":"volumeStep","value":25}')
                self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                self.assertEqual(json.loads(body), {"ok": True})
                calls = [json.loads(line) for line in details.read_text().splitlines()]
                steppedWrite = [call for call in calls[stepStart:]
                                if call["uri"] == "com.webos.audio/setVolume"]
                self.assertEqual(steppedWrite[-1]["payload"], {"volume": 35},
                                 "large volume steps use the legacy default read level")
                self.assertEqual([call["uri"] for call in calls[stepStart:]], [
                    "com.webos.service.audio/master/getVolume",
                    "com.webos.audio/getVolume",
                    "com.webos.audio/setVolume"],
                    "large steps probe the new and legacy volume services in order")
                invalidations = [item for item in nodeRequests
                                 if item["path"] == "/_internal/native-state"]
                self.assertEqual(len(invalidations), 15)

                refusedBody = '{"action":"volume","value":0}'
                status, body = postControl(refusedBody)
                self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                self.assertEqual(json.loads(body), {"ok": True, "forwarded": True},
                                 "unsupported output scenarios retain Node's refusal side effect")
                self.assertEqual(nodeRequests[-1]["path"], "/api/control")
                self.assertEqual(nodeRequests[-1]["body"], refusedBody,
                                 "native fallback forwards the exact buffered request body")

                fallbackBody = '{"action":"privacyAllOff"}'
                status, body = postControl(fallbackBody)
                self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                aggregate = json.loads(body)
                self.assertFalse(aggregate["ok"])
                self.assertEqual(aggregate["done"], 0)
                self.assertIn("could not switch off", aggregate["error"])
                self.assertNotEqual(nodeRequests[-1].get("body"), fallbackBody,
                                    "the aggregate privacy action runs in the daemon")

                malformedBody = '{"action":"volume"'
                nodeSeen = len(nodeRequests)
                status, body = postControl(malformedBody)
                self.assertEqual(status, 400, body.decode("utf-8", "replace"))
                self.assertEqual(json.loads(body), {"ok": False, "error": "malformed JSON"})
                self.assertEqual(len(nodeRequests), nodeSeen,
                                 "malformed control requests are refused by the daemon")

                oversizedBody = '{"action":"mute","value":"' + "😀" * 2040 + '"}'
                status, body = postControl(oversizedBody)
                self.assertEqual(status, 413, body.decode("utf-8", "replace"))
                self.assertEqual(len(nodeRequests), nodeSeen,
                                 "the 4096 UTF-16 code unit cap rejects before forwarding")

                calls = [json.loads(line) for line in details.read_text().splitlines()]
                soundStart = len(calls)
                status, body = postControl(
                    '{"action":"sound_output","value":" optical "}')
                self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                self.assertEqual(json.loads(body), {"ok": True}, details.read_text())
                calls = [json.loads(line) for line in details.read_text().splitlines()]
                soundCalls = calls[soundStart:]
                soundWrites = [call["payload"]["settings"]["soundOutput"]
                               for call in soundCalls
                               if call["uri"] ==
                               "com.webos.service.settings/setSystemSettings" and
                               call["payload"].get("category") == "sound"]
                self.assertEqual(soundWrites, ["optical", "external_optical"],
                                 "a refused optical alias retries the supported firmware name")
                self.assertEqual(
                    [call["uri"] for call in soundCalls
                     if call["uri"] == "com.webos.service.audio/master/getVolume"],
                    ["com.webos.service.audio/master/getVolume"] * 2,
                    "sound output waits until the audio service reports the new route")
                self.assertEqual(len([item for item in nodeRequests
                                      if item["path"] == "/_internal/native-state"]), 16,
                                 "a successful sound-output change invalidates Node caches")

                powerStart = len(calls)
                status, body = postControl('{"action":"powerOff"}')
                self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                self.assertEqual(json.loads(body), {"ok": True}, details.read_text())
                calls = [json.loads(line) for line in details.read_text().splitlines()]
                powerCalls = calls[powerStart:]
                self.assertEqual([call["uri"] for call in powerCalls], [
                    "com.webos.service.tvpower/power/powerOff",
                    "com.webos.service.tvpower/power/getPowerState",
                    "com.webos.service.tvpower/power/powerOff"],
                    "power-off checks state before retrying LG's local-key fallback")
                self.assertEqual([call["payload"] for call in powerCalls], [
                    {"reason": "remoteKey"}, {}, {"reason": "localKey"}])
                self.assertEqual(len([item for item in nodeRequests
                                      if item["path"] == "/_internal/native-state"]), 17,
                                 "a busy retry counts as an accepted power-off transition")

                powerOnStart = len(calls)
                status, body = postControl('{"action":"powerOn"}')
                self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                self.assertEqual(json.loads(body), {"ok": True})
                calls = [json.loads(line) for line in details.read_text().splitlines()]
                powerOnCalls = calls[powerOnStart:]
                self.assertEqual([call["uri"] for call in powerOnCalls], [
                    "com.webos.service.tvpower/power/powerOn",
                    "com.webos.service.tv.power/cancelPowerOff",
                    "com.webos.service.tvpower/power/getPowerState"],
                    "power-on cancels a pending off and confirms Active state")
                self.assertEqual(powerOnCalls[0]["payload"], {"reason": "remoteKey"})
                self.assertEqual(len([item for item in nodeRequests
                                      if item["path"] == "/_internal/native-state"]), 18,
                                 "successful power-on recovery invalidates Node caches")

                started = time.monotonic()
                status, body = post({"key": 4, "id": "netflix"})
                elapsed = time.monotonic() - started
                self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                self.assertEqual(json.loads(body), {"ok": True})
                self.assertGreaterEqual(elapsed, 0.48,
                                        "replacing a set key must preserve LG's clear delay")
                calls = [json.loads(line) for line in details.read_text().splitlines()]
                writes = [call for call in calls
                          if call["uri"].endswith("/setSystemSettings") and
                          "magicNum4" in call["payload"].get("settings", {})]
                self.assertEqual(len(writes), 2, "replacing a set key must clear then set")
                self.assertEqual(
                    writes[0]["payload"]["settings"]["magicNum4"],
                    {"id": "", "params": {}})
                self.assertEqual(
                    writes[1]["payload"]["settings"]["magicNum4"],
                    {"id": "netflix", "params": {}})
                self.assertGreaterEqual(
                    calls.index(writes[1]) - calls.index(writes[0]), 1,
                    "the clear and set operations must be separately issued")
                self.assertTrue(any(call["uri"].endswith("/getAllInputStatus")
                                    for call in calls),
                                "target validation must include named TV inputs")

                before = len(calls)
                status, body = post({"key": 5, "id": ""})
                self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                calls = [json.loads(line) for line in details.read_text().splitlines()]
                clearWrites = [call for call in calls[before:]
                               if call["uri"].endswith("/setSystemSettings") and
                               "magicNum5" in call["payload"].get("settings", {})]
                self.assertEqual(len(clearWrites), 1,
                                 "clearing an empty key should write one empty value")
                self.assertEqual(
                    clearWrites[0]["payload"]["settings"]["magicNum5"],
                    {"id": "", "params": {}})

                status, body = post({"key": 6, "id": ["netflix"]})
                self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                calls = [json.loads(line) for line in details.read_text().splitlines()]
                arrayTargetWrite = [call for call in calls
                                    if call["uri"].endswith("/setSystemSettings") and
                                    "magicNum6" in call["payload"]["settings"]]
                self.assertEqual(
                    arrayTargetWrite[-1]["payload"]["settings"]["magicNum6"],
                    {"id": "netflix", "params": {}},
                    "legacy JavaScript string coercion must not turn an array target into a clear")

                status, body = post({"key": 7, "id": "界" * 3000}, ensure_ascii=False)
                self.assertEqual(status, 400,
                                 "the body limit follows JS characters, not UTF-8 byte count: " +
                                 body.decode("utf-8", "replace"))

                # The apps list carries the Quick Access keys as Node's read model gave them.
                quick = getApps()["quickAccess"]
                self.assertTrue(quick["supported"])
                self.assertEqual([key["key"] for key in quick["keys"]], list(range(1, 10)))
                self.assertEqual(quick["keys"][3]["id"], "youtube")
                self.assertEqual(quick["keys"][0]["title"], "")
                self.assertEqual(quick["choices"][-1]["kind"] == "app" or
                                 quick["choices"][-1]["kind"] == "input", True)
                self.assertIn("com.webos.app.livetv", [c["id"] for c in quick["choices"]])

                before = len(calls)
                status, body = post({"key": 2, "id": "com.webos.app.self-diagnosis"})
                self.assertEqual(status, 400, body.decode("utf-8", "replace"))
                calls = [json.loads(line) for line in details.read_text().splitlines()]
                self.assertFalse(any(call["uri"].endswith("/setSystemSettings")
                                     and call["payload"].get("category") == "option"
                                     for call in calls[before:]),
                                 "hidden apps must be rejected without a TV write")

                before = len(calls)
                status, _body = post({"key": 0, "id": "netflix"})
                self.assertEqual(status, 400)
                calls = [json.loads(line) for line in details.read_text().splitlines()]
                self.assertEqual(len(calls), before,
                                 "invalid keys must not call Luna")

                before = len(calls)
                status, body = postPreload({"id": "netflix", "off": True})
                self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                self.assertEqual(json.loads(body), {"ok": True})
                calls = [json.loads(line) for line in details.read_text().splitlines()]
                preloadCalls = calls[before:]
                self.assertEqual([call["uri"] for call in preloadCalls], [
                    "com.webos.service.config/getConfigs",
                    "com.webos.service.config/setConfigs",
                    "com.webos.service.preloadmanager/setPreloadPolicy"],
                    "preload change reads, persists, then applies the live policy")
                self.assertEqual(
                    preloadCalls[1]["payload"]["configs"]
                    ["com.webos.service.preloadmanager.supportAppList"][2]["isEnabled"],
                    False, "configd stores the owner-disabled value")
                self.assertEqual(preloadCalls[2]["payload"], {
                    "application": {"id": "netflix", "isEnabled": False}})
                preloadState = json.loads((root / "preload.json").read_text())
                self.assertEqual(preloadState["off"], ["netflix"])
                self.assertEqual(len(preloadState["lg"]), 4,
                                 "native state keeps the exact LG baseline list")

                before = len(calls)
                status, body = postPreload({"id": "netflix", "off": False})
                self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                self.assertEqual(json.loads(body), {"ok": True})
                calls = [json.loads(line) for line in details.read_text().splitlines()]
                restoreCalls = calls[before:]
                restoredList = restoreCalls[1]["payload"]["configs"][
                    "com.webos.service.preloadmanager.supportAppList"]
                self.assertTrue(restoredList[2]["isEnabled"],
                                "restoring an app uses the saved LG baseline")
                self.assertTrue(restoreCalls[2]["payload"]["application"]["isEnabled"])
                self.assertEqual(json.loads((root / "preload.json").read_text())["off"], [])

                before = len(calls)
                status, body = postPreload({"id": "com.webos.app.home", "off": True})
                self.assertEqual(status, 400, body.decode("utf-8", "replace"))
                self.assertFalse(json.loads(body)["ok"])
                calls = [json.loads(line) for line in details.read_text().splitlines()]
                self.assertEqual(
                    len([call for call in calls[before:]
                         if call["uri"] == "com.webos.service.config/setConfigs"]),
                    0, "essential apps are refused without a persistent write")

                before = len(calls)
                status, body = postControl('{"action":"sleepTimer","value":30}')
                self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                self.assertEqual(json.loads(body), {"ok": True})
                calls = [json.loads(line) for line in details.read_text().splitlines()]
                sleepWrite = [call for call in calls[before:]
                              if call["uri"] == "com.webos.service.settings/setSystemSettings"]
                self.assertEqual(sleepWrite[-1]["payload"], {
                    "category": "time", "settings": {"sleepTimer": "30"}})

                before = len(calls)
                status, body = postControl('{"action":"sleepTimer","value":15}')
                self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                self.assertEqual(json.loads(body), {
                    "ok": False,
                    "error": "sleep timer must be one of off, 10, 30, 60, 90, 120"})
                calls = [json.loads(line) for line in details.read_text().splitlines()]
                self.assertFalse(any(
                    call["uri"] == "com.webos.service.settings/setSystemSettings"
                    for call in calls[before:]),
                    "an unsupported sleep-timer value must not call Luna")

                status, body = postControl(
                    '{"action":"quickBoot","value":"ON"}')
                self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                self.assertEqual(json.loads(body), {"ok": True})
                calls = [json.loads(line) for line in details.read_text().splitlines()]
                self.assertEqual(calls[-1]["payload"], {
                    "category": "option", "settings": {"quickStartMode": "on"}})

                status, body = postControl(
                    '{"action":"deviceDetection","value":true}')
                self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                self.assertEqual(json.loads(body), {"ok": True})
                calls = [json.loads(line) for line in details.read_text().splitlines()]
                self.assertEqual(calls[-1]["payload"], {
                    "category": "other", "settings": {"ueiEnable": "on"}})

                before = len(calls)
                status, body = postControl(
                    '{"action":"powerTimer","value":{"timer":"on","time":"06:45"}}')
                self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                self.assertEqual(json.loads(body), {"ok": True})
                calls = [json.loads(line) for line in details.read_text().splitlines()]
                timerCalls = calls[before:]
                self.assertEqual([call["uri"] for call in timerCalls], [
                    "com.webos.service.settings/getSystemSettings",
                    "com.webos.applicationManager/getAppInfo",
                    "com.webos.service.settings/setSystemSettings",
                    "com.webos.service.settings/setSystemSettings",
                    "com.webos.service.settings/setSystemSettings"],
                    "an active On Timer is read, re-armed, updated, and re-enabled")
                self.assertEqual([call["payload"]["settings"] for call in timerCalls[2:]], [
                    {"onTimerEnable": "off"},
                    {"onTimerHour": "6", "onTimerMinute": "45",
                     "onTimerAppId": "com.webos.app.home"},
                    {"onTimerEnable": "on"}])

                before = len(calls)
                status, body = postControl(
                    '{"action":"powerTimer","value":{"timer":"on","days":null}}')
                self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                self.assertEqual(json.loads(body), {
                    "ok": False,
                    "error": "days must be a list of 0 (Sunday) to 6 (Saturday)"})
                calls = [json.loads(line) for line in details.read_text().splitlines()]
                self.assertEqual(len(calls), before,
                                 "invalid timer days are rejected before Luna")

                before = len(details.read_text().splitlines())
                invalidations = len([item for item in nodeRequests
                                     if item["path"] == "/_internal/native-state"])
                status, body = postControl(
                    '{"action":"screenShift","value":"true"}')
                self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                self.assertEqual(json.loads(body), {"ok": True})
                calls = [json.loads(line) for line in details.read_text().splitlines()]
                screenShiftWrite = [call for call in calls[before:]
                                    if call["uri"] ==
                                    "com.webos.service.settings/setSystemSettings"]
                self.assertEqual(len(screenShiftWrite), 1)
                self.assertEqual(screenShiftWrite[0]["payload"], {
                    "category": "picture", "settings": {"screenShift": "on"}})
                self.assertEqual(
                    len([item for item in nodeRequests
                         if item["path"] == "/_internal/native-state"]),
                    invalidations + 1,
                    "screen-shift writes invalidate Node's cached control state")

                before = len(calls)
                status, body = postControl(
                    '{"action":"logoDimming","value":" STRONG "}')
                self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                self.assertEqual(json.loads(body), {"ok": True})
                calls = [json.loads(line) for line in details.read_text().splitlines()]
                logoDimmingWrite = [call for call in calls[before:]
                                    if call["uri"] ==
                                    "com.webos.service.settings/setSystemSettings"]
                self.assertEqual(len(logoDimmingWrite), 1)
                self.assertEqual(logoDimmingWrite[0]["payload"], {
                    "category": "picture",
                    "settings": {"logoLuminanceAdjust": "strong"}})

                before = len(calls)
                status, body = postControl(
                    '{"action":"logoDimming","value":"High"}')
                self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                self.assertEqual(json.loads(body), {
                    "ok": False,
                    "error": "logo dimming takes off, light, strong"})
                calls = [json.loads(line) for line in details.read_text().splitlines()]
                self.assertEqual(len(calls), before,
                                 "invalid logo-dimming labels are rejected before Luna")

                standbyInvalidations = len([item for item in nodeRequests
                                             if item["path"] ==
                                             "/_internal/native-state"])
                status, body = postControl(
                    '{"action":"alwaysReady","value":"ON"}')
                self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                self.assertEqual(json.loads(body), {"ok": True})
                calls = [json.loads(line) for line in details.read_text().splitlines()]
                self.assertEqual(calls[-1]["payload"], {
                    "category": "general", "settings": {"alwaysOn": "on"}})

                status, body = postControl(
                    '{"action":"alwaysReadyScreen","value":true}')
                self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                self.assertEqual(json.loads(body), {"ok": True})
                calls = [json.loads(line) for line in details.read_text().splitlines()]
                self.assertEqual(calls[-1]["payload"], {
                    "category": "general", "settings": {"lifeOnScreenMode": "allEnabled"}})

                status, body = postControl(
                    '{"action":"alwaysReadyOffStart","value":"22:00"}')
                self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                self.assertEqual(json.loads(body), {"ok": True})
                calls = [json.loads(line) for line in details.read_text().splitlines()]
                self.assertEqual(calls[-1]["payload"], {
                    "category": "general", "settings": {
                        "alwaysOnDisableStartHour": "22",
                        "alwaysOnDisableStartMinute": "0",
                        "alwaysOnDisableEndHour": "3",
                        "alwaysOnDisableEndMinute": "0"}})

                before = len(calls)
                status, body = postControl(
                    '{"action":"alwaysReadyOffStart","value":"25:00"}')
                self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                self.assertEqual(json.loads(body), {
                    "ok": False, "error": "the start must be an hour from 0 to 23"})
                calls = [json.loads(line) for line in details.read_text().splitlines()]
                self.assertEqual(len(calls), before,
                                 "invalid nightly start hours are rejected before Luna")

                status, body = postControl(
                    '{"action":"wakeOnLan","value":true}')
                self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                self.assertEqual(json.loads(body), {"ok": True})
                calls = [json.loads(line) for line in details.read_text().splitlines()]
                self.assertEqual(calls[-1]["payload"], {
                    "category": "network", "settings": {"wolwowlOnOff": "true"}})

                status, body = postControl(
                    '{"action":"lgLogo","value":"true"}')
                self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                self.assertEqual(json.loads(body), {"ok": True})
                calls = [json.loads(line) for line in details.read_text().splitlines()]
                self.assertEqual(calls[-1]["payload"], {
                    "category": "other", "settings": {"lgLogoDisplay": "on"}})

                status, body = postControl(
                    '{"action":"lgLogo","value":"1"}')
                self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                self.assertEqual(json.loads(body), {"ok": True})
                calls = [json.loads(line) for line in details.read_text().splitlines()]
                self.assertEqual(calls[-1]["payload"], {
                    "category": "other", "settings": {"lgLogoDisplay": "off"}},
                    "LG logo toggle keeps the exact legacy truth-value set")
                self.assertEqual(
                    len([item for item in nodeRequests
                         if item["path"] == "/_internal/native-state"]),
                    standbyInvalidations + 6,
                    "successful standby setting writes invalidate Node's cache")

                before = len(calls)
                status, body = postControl(
                    '{"action":"lgSetting","value":{"id":"gameGenre","value":"FPS"}}')
                self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                self.assertEqual(json.loads(body), {"ok": True})
                calls = [json.loads(line) for line in details.read_text().splitlines()]
                gameGenreCalls = calls[before:]
                self.assertEqual([call["uri"] for call in gameGenreCalls], [
                    "com.webos.service.settings/getSystemSettingDesc",
                    "com.webos.service.settings/setSystemSettings"])
                self.assertEqual(gameGenreCalls[0]["payload"], {
                    "category": "other", "keys": ["gameGenre"]})
                self.assertEqual(gameGenreCalls[1]["payload"], {
                    "category": "other", "settings": {"gameGenre": "FPS"}})

                before = len(calls)
                status, body = postControl(
                    '{"action":"lgSetting","value":{"id":"blackStabilizer","value":20}}')
                self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                self.assertEqual(json.loads(body), {"ok": True})
                calls = [json.loads(line) for line in details.read_text().splitlines()]
                self.assertEqual(calls[-1]["payload"], {
                    "category": "other", "settings": {"blackStabilizer": 20}})

                before = len(calls)
                status, body = postControl(
                    '{"action":"lgSetting","value":{"id":"gameGenre","value":"Racing"}}')
                self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                self.assertEqual(json.loads(body), {
                    "ok": False, "error": "That value is not one this setting takes"})
                calls = [json.loads(line) for line in details.read_text().splitlines()]
                self.assertEqual(len(calls), before,
                                 "invalid Game Optimizer choices are rejected before Luna")

                # Dashboard rows beyond Game Optimizer are written natively too; a
                # switch takes `on`, and a value that is not one the row takes is
                # refused before Luna.
                before = len(calls)
                nodeBefore = len(nodeRequests)
                status, body = postControl(
                    '{"action":"lgSetting","value":{"id":"homePromotion","on":false}}')
                self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                self.assertEqual(json.loads(body), {"ok": True})
                calls = [json.loads(line) for line in details.read_text().splitlines()]
                promotionCalls = calls[before:]
                self.assertEqual([call["uri"] for call in promotionCalls], [
                    "com.webos.service.settings/getSystemSettingDesc",
                    "com.webos.service.settings/setSystemSettings"])
                self.assertEqual(promotionCalls[1]["payload"], {
                    "category": "general", "settings": {"homePromotion": "off"}})
                self.assertEqual(
                    [item["path"] for item in nodeRequests[nodeBefore:]
                     if item["path"] == "/api/control"], [],
                    "dashboard LG settings no longer reach Node")
                before = len(calls)
                status, body = postControl(
                    '{"action":"lgSetting","value":{"id":"soundOutput","value":"nonsense"}}')
                self.assertEqual(json.loads(body), {
                    "ok": False, "error": "That value is not one this setting takes"})
                calls = [json.loads(line) for line in details.read_text().splitlines()]
                self.assertEqual(len(calls), before)

                before = len(calls)
                invalidations = len([item for item in nodeRequests
                                     if item["path"] == "/_internal/native-state"])
                status, body = postControl(
                    '{"action":"rcu","value":" RIGHT "}')
                self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                self.assertEqual(json.loads(body), {"ok": True})
                calls = [json.loads(line) for line in details.read_text().splitlines()]
                remoteCalls = calls[before:]
                self.assertEqual(len(remoteCalls), 1, remoteCalls)
                self.assertEqual(remoteCalls[0]["uri"],
                                 "com.webos.service.networkinput/test/sendKeyCode")
                self.assertEqual(remoteCalls[0]["payload"], {"keyCode": 106})
                self.assertEqual(len([item for item in nodeRequests
                                      if item["path"] == "/_internal/native-state"]),
                                 invalidations + 1,
                                 "successful native remote keys invalidate Node's cached state")

                homeBody = '{"action":"rcu","value":"home"}'
                before = len(calls)
                status, body = postControl(homeBody)
                self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                self.assertEqual(json.loads(body), {"ok": True})
                calls = [json.loads(line) for line in details.read_text().splitlines()]
                homeCalls = calls[before:]
                self.assertEqual(len(homeCalls), 1, homeCalls)
                self.assertEqual(homeCalls[0]["uri"],
                                 "com.webos.applicationManager/launch")
                self.assertEqual(homeCalls[0]["payload"],
                                 {"id": "com.webos.app.home"})
                self.assertEqual(nodeRequests[-1]["path"], "/_internal/native-state")

                serviceMenuReads = len(calls)
                nodeRequestCount = len(nodeRequests)
                status, body = getServiceMenu()
                self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                state = json.loads(body)
                self.assertTrue(state["ok"], state)
                self.assertFalse(state["lockable"], state)
                self.assertIsNone(state["locked"], state)
                self.assertTrue(state["writable"], state)
                self.assertIsInstance(state["app"], bool)
                calls = [json.loads(line) for line in details.read_text().splitlines()]
                readCalls = calls[serviceMenuReads:]
                self.assertEqual(len(readCalls), 1, readCalls)
                self.assertEqual(readCalls[0]["uri"],
                                 "com.webos.settingsservice/getSystemSettings")
                self.assertEqual(readCalls[0]["payload"], {
                    "category": "other", "keys": ["svcMenuFlag"]})
                self.assertEqual(len(nodeRequests), nodeRequestCount,
                                 "native service-menu reads must not proxy to Node")
                status, body = getServiceMenu("HEAD")
                self.assertEqual(status, 200)
                self.assertEqual(body, b"")
                calls = [json.loads(line) for line in details.read_text().splitlines()]

                toastMessage = "x" * 130
                toastBody = json.dumps({"action": "toast", "value": toastMessage})
                before = len(calls)
                status, body = postControl(toastBody)
                self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                self.assertEqual(json.loads(body), {"ok": True})
                calls = [json.loads(line) for line in details.read_text().splitlines()]
                toastCalls = calls[before:]
                self.assertEqual(len(toastCalls), 1, toastCalls)
                self.assertEqual(toastCalls[0]["uri"],
                                 "com.webos.notification/createToast")
                self.assertEqual(toastCalls[0]["payload"], {
                    "sourceId": "com.webos.app.home", "message": "x" * 120})
                self.assertEqual(toastCalls[0]["applicationId"],
                                 "com.webos.app.home")
                self.assertEqual(nodeRequests[-1]["path"], "/_internal/native-state")

                before = len(calls)
                status, body = postControl(
                    '{"action":"serviceMenuOpen","value":{"menu":"inStart"}}')
                self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                self.assertEqual(json.loads(body), {"ok": True, "menu": "inStart"})
                calls = [json.loads(line) for line in details.read_text().splitlines()]
                serviceMenuCalls = calls[before:]
                self.assertEqual(len(serviceMenuCalls), 1, serviceMenuCalls)
                self.assertEqual(serviceMenuCalls[0]["uri"],
                                 "com.webos.applicationManager/launch")
                self.assertEqual(serviceMenuCalls[0]["payload"], {
                    "id": "com.webos.app.factorywin",
                    "params": {"irKey": "inStart"}})
                self.assertEqual(nodeRequests[-1]["path"], "/_internal/native-state")

                before = len(calls)
                status, body = postControl(
                    '{"action":"serviceMenuLock","value":{"locked":true}}')
                self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                lockResponse = json.loads(body)
                self.assertTrue(lockResponse["ok"], lockResponse)
                self.assertEqual(lockResponse["state"]["locked"], True)
                self.assertTrue(lockResponse["state"]["lockable"])
                calls = [json.loads(line) for line in details.read_text().splitlines()]
                serviceMenuLockCalls = calls[before:]
                self.assertEqual([call["uri"] for call in serviceMenuLockCalls], [
                    "com.webos.settingsservice/setSystemSettings",
                    "com.webos.settingsservice/getSystemSettings"])
                self.assertEqual(serviceMenuLockCalls[0]["payload"], {
                    "category": "other", "settings": {"svcMenuFlag": True}})
                self.assertEqual(serviceMenuLockCalls[1]["payload"], {
                    "category": "other", "keys": ["svcMenuFlag"]})
                self.assertEqual(nodeRequests[-1]["path"], "/_internal/native-state")

                nodeRequestCount = len(nodeRequests)
                status, body = getServiceMenu()
                self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                state = json.loads(body)
                self.assertTrue(state["lockable"], state)
                self.assertTrue(state["locked"], state)
                self.assertEqual(len(nodeRequests), nodeRequestCount,
                                 "native service-menu reads must not proxy to Node")
                calls = [json.loads(line) for line in details.read_text().splitlines()]

                before = len(calls)
                status, body = postControl(
                    '{"action":"oledProtection","value":'
                    '{"key":"gsr","enabled":true}}')
                self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                oledResponse = json.loads(body)
                self.assertTrue(oledResponse["ok"], oledResponse)
                self.assertEqual(oledResponse["state"]["gsr"], True)
                self.assertEqual(oledResponse["state"]["gsrStressCount"], 17)
                calls = [json.loads(line) for line in details.read_text().splitlines()]
                oledCalls = calls[before:]
                self.assertEqual([call["uri"] for call in oledCalls], [
                    "com.webos.service.oledepl/getGlobalStressReduction",
                    "com.webos.service.oledepl/getTemporalPeakControl",
                    "com.webos.service.oledepl/setGlobalStressReduction",
                    "com.webos.service.oledepl/getGlobalStressReduction",
                    "com.webos.service.oledepl/getTemporalPeakControl"])
                self.assertEqual(oledCalls[2]["payload"], {"enable": True})
                self.assertEqual(nodeRequests[-1]["path"], "/_internal/native-state")

                before = len(calls)
                status, body = postControl(
                    '{"action":"oledProtection","value":'
                    '{"key":"tpc","enabled":true}}')
                self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                tpcResponse = json.loads(body)
                self.assertTrue(tpcResponse["ok"], tpcResponse)
                self.assertTrue(tpcResponse["state"]["tpc"])
                calls = [json.loads(line) for line in details.read_text().splitlines()]
                tpcCalls = calls[before:]
                self.assertEqual([call["uri"] for call in tpcCalls], [
                    "com.webos.service.oledepl/getGlobalStressReduction",
                    "com.webos.service.oledepl/getTemporalPeakControl",
                    "com.webos.service.oledepl/setTemporalPeakControl",
                    "com.webos.service.oledepl/getGlobalStressReduction",
                    "com.webos.service.oledepl/getTemporalPeakControl"])
                self.assertEqual(tpcCalls[2]["payload"], {"enable": True})

                for action, mode in (("refresherSchedule", "schedule"),
                                     ("refresherCancel", "cancel_schedule")):
                    before = len(calls)
                    status, body = postControl(json.dumps({"action": action}))
                    self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                    self.assertEqual(json.loads(body), {"ok": True})
                    calls = [json.loads(line) for line in details.read_text().splitlines()]
                    refresherCalls = calls[before:]
                    self.assertEqual(len(refresherCalls), 1, refresherCalls)
                    self.assertEqual(refresherCalls[0]["uri"],
                                     "com.webos.service.tv.display/requestClearPanelNoise")
                    self.assertEqual(refresherCalls[0]["payload"], {"mode": mode})
                    self.assertEqual(nodeRequests[-1]["path"], "/_internal/native-state")

                before = len(calls)
                status, body = postControl(
                    '{"action":"oledProtection","value":{"key":"other"}}')
                self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                self.assertEqual(json.loads(body), {
                    "ok": False, "error": "unknown protection: other"})
                calls = [json.loads(line) for line in details.read_text().splitlines()]
                self.assertEqual(len(calls), before,
                                 "unknown OLED protection keys are rejected before Luna")

                unknownKey = '{"action":"rcu","value":"eject_disc"}'
                status, body = postControl(unknownKey)
                self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                self.assertEqual(json.loads(body), {
                    "ok": False, "error": "unknown key: eject_disc"})
                self.assertNotEqual(nodeRequests[-1].get("body"), unknownKey,
                                    "unrecognized RCU values are refused by the daemon")

                process.send_signal(signal.SIGTERM)
                output, _ = process.communicate(timeout=6)
                self.assertIn("quickaccess: key 4 opens netflix", output,
                              "successful changes must be recorded in the native audit log")
                self.assertIn("control: power off applied", output,
                              "power transition decisions are written to the native audit log")
                self.assertIn("control: power on applied", output,
                              "power-on recovery decisions are written to the native audit log")
                self.assertIn("preload: netflix back to LG's setting", output,
                              "preload changes are recorded by the native daemon")
                self.assertIn("control: sleep timer 30 applied", output)
                self.assertIn("control: quick boot on applied", output)
                self.assertIn("control: device detection on applied", output)
                self.assertIn("control: power timer on applied", output)
                self.assertIn("control: screen shift on applied", output)
                self.assertIn("control: OLED pixel refresher schedule applied", output)
                self.assertIn("control: OLED pixel refresher cancel applied", output)
                self.assertIn("command-policy: action=control.refresher-schedule result=success", output)
                self.assertIn("command-policy: action=control.refresher-cancel result=success", output)
                self.assertIn("control: logo dimming strong applied", output)
                self.assertIn("control: always-on on applied", output)
                self.assertIn("control: always-ready screen on applied", output)
                self.assertIn("control: always-on nightly start 22 applied", output)
                self.assertIn("control: wake-on-LAN on applied", output)
                self.assertIn("control: LG logo display on applied", output)
                self.assertIn("control: LG logo display off applied", output)
                self.assertIn("control: LG setting gameGenre applied", output)
                self.assertIn("control: LG setting blackStabilizer applied", output)
                self.assertIn("command-policy: action=control.remote-key result=success", output)
                self.assertIn("command-policy: action=control.toast result=success", output)
                self.assertIn(
                    "command-policy: action=control.service-menu-open result=success",
                    output)
                self.assertIn(
                    "command-policy: action=control.service-menu-lock result=success",
                    output)
                self.assertIn(
                    "command-policy: action=control.oled-protection result=success",
                    output)
                self.assertIn("control: remote key code 106 applied", output)
            finally:
                if process.poll() is None:
                    process.send_signal(signal.SIGTERM)
                    try:
                        process.communicate(timeout=6)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        output, _ = process.communicate(timeout=3)
                        self.fail("native daemon did not stop after SIGTERM:\n" + output[-4000:])
                node.shutdown()
                node.server_close()
                nodeThread.join(timeout=3)

    def test_native_hdmi_receiver_map_is_asked_until_configd_answers(self):
        # test-hdmi-map.js: no answer is asked again rather than remembered as no
        # map; an answer or a refusal is final.
        binary = Path(sys.argv[1]).resolve()
        fakeLuna = binary.parent / "native-fake-luna-send"
        for behaviour, expectedAsks in (("silent", 2), ("refused", 1), ("answered", 1)):
            publicPort = unused_port_pair()
            nodePort = unused_port_pair((publicPort, publicPort + 1))
            token = "hdmi-map-token"
            with tempfile.TemporaryDirectory(prefix="nativetv-native-hdmi-map-") as folder:
                root = Path(folder)
                (root / "assets").mkdir()
                (root / "proc").mkdir()
                (root / "system").mkdir()
                (root / "config.json").write_text(json.dumps({
                    "host": "127.0.0.1", "port": publicPort, "token": token}))
                details = root / "luna-details"
                environment = os.environ.copy()
                environment["TVWEB_LUNA_SEND"] = str(fakeLuna)
                environment["TVWEB_DATA_DIR"] = str(root / "data")
                environment["TVWEB_PROXY_SECRET"] = "hdmi-map-proxy-secret-0123456789abcdef"
                environment["NATIVETV_FAKE_HDMI_MAP"] = behaviour
                environment["NATIVETV_FAKE_LUNA_DETAILS"] = str(details)
                process = subprocess.Popen(
                    [str(binary), "--config", str(root / "config.json"),
                     "--heartbeat", str(root / "tvweb.beat"), "--workers", "4", "--listen",
                     "--node-port", str(nodePort), "--assets-root", str(root / "assets"),
                     "--proc-root", str(root / "proc"), "--system-root", str(root / "system")],
                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, env=environment)

                def request(path):
                    req = Request("http://127.0.0.1:%d%s" % (publicPort, path),
                                  headers={"Authorization": "Bearer " + token})
                    try:
                        with urlopen(req, timeout=20) as response:
                            return response.status, response.read()
                    except HTTPError as error:
                        return error.code, error.read()

                try:
                    deadline = time.monotonic() + 8
                    while time.monotonic() < deadline and process.poll() is None:
                        try:
                            request("/api/caps")
                            break
                        except (OSError, URLError):
                            time.sleep(0.03)
                    self.assertIsNone(process.poll(), "daemon exited early")
                    for _ in range(2):
                        status, body = request("/api/hdmi")
                        self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                    asks = len([line for line in details.read_text().splitlines()
                                if "inputMap." in line])
                    self.assertEqual(asks, expectedAsks, behaviour)
                finally:
                    if process.poll() is None:
                        process.send_signal(signal.SIGTERM)
                        try:
                            process.communicate(timeout=6)
                        except subprocess.TimeoutExpired:
                            process.kill()
                            process.communicate(timeout=3)

    def test_native_oled_care_hours_come_from_the_panel_controller(self):
        # webOS 10.3.2 (B3) refuses the system property keys and answers the
        # panel controller only when the request says `subscribe`; without it
        # the hours fell back to a file that is hours behind.
        binary = Path(sys.argv[1]).resolve()
        fakeLuna = binary.parent / "native-fake-luna-send"
        publicPort = unused_port_pair()
        nodePort = unused_port_pair((publicPort, publicPort + 1))
        token = "oled-panelcontroller-token"
        with tempfile.TemporaryDirectory(prefix="nativetv-native-oled-pc-") as folder:
            root = Path(folder)
            config = root / "config.json"
            assets = root / "assets"
            systemRoot = root / "system"
            pnwash = systemRoot / "mnt/lg/cmn_data/pnwash"
            assets.mkdir()
            pnwash.mkdir(parents=True)
            (pnwash / "autoOffRsLastTime").write_text("5731\n")
            (pnwash / "autoOffRsInterval").write_text("4\n")
            config.write_text(json.dumps({
                "host": "127.0.0.1", "port": publicPort, "token": token,
                "allowControl": True}))
            environment = os.environ.copy()
            environment["TVWEB_LUNA_SEND"] = str(fakeLuna)
            environment["TVWEB_DATA_DIR"] = str(root / "data")
            environment["TVWEB_PROXY_SECRET"] = "oled-pc-proxy-secret-0123456789abcdef"
            environment["NATIVETV_FAKE_LUNA_MODE"] = "oled-panelcontroller"
            process = subprocess.Popen(
                [str(binary), "--config", str(config), "--heartbeat", str(root / "tvweb.beat"),
                 "--workers", "4", "--listen", "--node-port", str(nodePort),
                 "--assets-root", str(assets), "--system-root", str(systemRoot)],
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                env=environment)

            def request(path):
                req = Request("http://127.0.0.1:%d%s" % (publicPort, path),
                              headers={"Authorization": "Bearer " + token})
                try:
                    with urlopen(req, timeout=8) as response:
                        return response.status, response.read()
                except HTTPError as error:
                    return error.code, error.read()

            try:
                deadline = time.monotonic() + 8
                while time.monotonic() < deadline and process.poll() is None:
                    try:
                        request("/api/caps")
                        break
                    except (OSError, URLError):
                        time.sleep(0.03)
                self.assertIsNone(process.poll(), "daemon exited early")
                status, body = request("/api/oledcare")
                self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                result = json.loads(body)
                self.assertEqual(result["panelHours"], 5736,
                                 "34421 units from the panel controller, not the file's 5731 hours")
            finally:
                if process.poll() is None:
                    process.send_signal(signal.SIGTERM)
                    try:
                        process.communicate(timeout=6)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.communicate(timeout=3)

    def test_native_oled_care_read_route_preserves_legacy_projection(self):
        binary = Path(sys.argv[1]).resolve()
        fakeLuna = binary.parent / "native-fake-luna-send"
        publicPort = unused_port_pair()
        nodePort = unused_port_pair((publicPort, publicPort + 1))
        token = "oled-care-test-token"
        secret = "oled-care-proxy-secret-0123456789abcdef"
        nodeRequests = []

        class NodeCaptureHandler(BaseHTTPRequestHandler):
            def do_POST(self):
                length = int(self.headers.get("Content-Length", "0"))
                body = self.rfile.read(length)
                nodeRequests.append({"path": self.path, "body": body})
                self.send_response(204)
                self.send_header("Content-Length", "0")
                self.end_headers()

            def log_message(self, *_args):
                pass

        node = HTTPServer(("127.0.0.1", nodePort), NodeCaptureHandler)
        nodeThread = threading.Thread(target=node.serve_forever, daemon=True)
        nodeThread.start()

        with tempfile.TemporaryDirectory(prefix="nativetv-native-oled-care-") as folder:
            root = Path(folder)
            config = root / "config.json"
            heartbeat = root / "tvweb.beat"
            assets = root / "assets"
            systemRoot = root / "system"
            procRoot = root / "proc"
            pnwash = systemRoot / "mnt/lg/cmn_data/pnwash"
            assets.mkdir()
            pnwash.mkdir(parents=True)
            (pnwash / "autoPnwashTime").write_text("800\n")
            (pnwash / "autoOffRsIntervalHomeMode").write_text("24\n")
            (pnwash / "autoJbInterval").write_text("2000\n")
            (pnwash / "completedOffRsCount").write_text("23\n")
            (pnwash / "completedJbCount").write_text("9\n")
            (pnwash / "failAlertCount").write_text("2\n")
            (pnwash / "socTpcStatus").write_text("0\n")
            (pnwash / "state").write_text("2\n")
            details = root / "luna-details.jsonl"
            config.write_text(json.dumps({
                "host": "127.0.0.1", "port": publicPort, "token": token,
                "allowControl": True
            }))
            environment = os.environ.copy()
            environment["TVWEB_LUNA_SEND"] = str(fakeLuna)
            environment["TVWEB_DATA_DIR"] = str(root / "data")
            environment["TVWEB_PROXY_SECRET"] = secret
            environment["NATIVETV_FAKE_LUNA_MODE"] = "oled-care"
            environment["NATIVETV_FAKE_LUNA_DETAILS"] = str(details)
            process = subprocess.Popen(
                [str(binary), "--config", str(config), "--heartbeat", str(heartbeat),
                 "--workers", "8", "--listen", "--node-port", str(nodePort),
                 "--assets-root", str(assets), "--system-root", str(systemRoot)],
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                env=environment)

            def request(path, method="GET", body=None):
                data = None if body is None else json.dumps(body).encode("utf-8")
                headers = {"Authorization": "Bearer " + token}
                if data is not None:
                    headers["Content-Type"] = "application/json"
                req = Request("http://127.0.0.1:%d%s" % (publicPort, path),
                              data=data, method=method, headers=headers)
                try:
                    with urlopen(req, timeout=8) as response:
                        return response.status, response.read()
                except HTTPError as error:
                    return error.code, error.read()

            try:
                deadline = time.monotonic() + 8
                ready = False
                while time.monotonic() < deadline and process.poll() is None:
                    try:
                        request("/api/caps")
                        ready = True
                        break
                    except (OSError, URLError):
                        time.sleep(0.03)
                self.assertTrue(ready, "native daemon did not start")

                status, body = request("/api/oledcare")
                self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                result = json.loads(body)
                self.assertEqual(result["isOled"], True)
                self.assertEqual(result["available"], True)
                self.assertEqual(result["serviceControls"], True)
                self.assertEqual(result["writable"], True)
                self.assertEqual((result["gsr"], result["tpc"],
                                  result["gsrStressCount"]), (True, False, 17))
                self.assertEqual((result["screenShift"], result["logoDimming"]),
                                 ("on", "strong"))
                self.assertEqual((result["panelHours"], result["hoursUntilComp"],
                                  result["compInterval"]), (1000, 3, 4))
                self.assertEqual((result["hoursUntilRefresher"],
                                  result["refresherInterval"]), (1800, 2000))
                self.assertEqual((result["compStatus"], result["refresherStatus"],
                                  result["compCycles"], result["refresherCycles"],
                                  result["failureAlerts"]),
                                 ("Running", "Running", 23, 9, 2))
                calls = without_country_learning(
                    [json.loads(line) for line in details.read_text().splitlines()])
                self.assertEqual([call["uri"] for call in calls], [
                    "com.webos.service.tv.systemproperty/getSystemProperties",
                    "com.webos.service.tv.systemproperty/getSystemProperties",
                    "com.webos.service.tv.display/getClearPanelNoiseStatus",
                    "com.webos.service.oledepl/getGlobalStressReduction",
                    "com.webos.service.oledepl/getTemporalPeakControl",
                    "com.webos.service.settings/getSystemSettings"])
                self.assertEqual(nodeRequests, [],
                                 "available native panel data must not be proxied")

                status, body = request("/api/oledcare", "HEAD")
                self.assertEqual(status, 200)
                self.assertEqual(body, b"")
                recorded = [json.loads(line) for line in details.read_text().splitlines()]
                self.assertEqual(len(without_country_learning(recorded)), 6,
                                 "GET and HEAD share the 30-second panel cache")

                status, body = request("/api/control", "POST", {
                    "action": "refresherSchedule"})
                self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                self.assertEqual(json.loads(body), {"ok": True})
                status, body = request("/api/oledcare")
                self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                recorded = [json.loads(line) for line in details.read_text().splitlines()]
                self.assertEqual(len(without_country_learning(recorded)), 13,
                                 "refresher writes invalidate the OLED care cache")
            finally:
                if process.poll() is None:
                    process.send_signal(signal.SIGTERM)
                    try:
                        process.communicate(timeout=6)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.communicate(timeout=3)
                node.shutdown()
                node.server_close()
                nodeThread.join(timeout=3)

    def test_native_background_services_read_projection(self):
        binary = Path(sys.argv[1]).resolve()
        fakeLuna = binary.parent / "native-fake-luna-send"
        token = "background-services-test-token"
        reservedPorts = set()

        def next_port():
            port = unused_port_pair(reservedPorts)
            reservedPorts.update((port, port + 1))
            return port

        with tempfile.TemporaryDirectory(prefix="nativetv-native-services-") as folder:
            root = Path(folder)
            config = root / "config.json"
            heartbeat = root / "tvweb.beat"
            assets = root / "assets"
            systemRoot = root / "system"
            procRoot = root / "proc"
            dataRoot = root / "data"
            assets.mkdir()
            procRoot.mkdir()
            dataRoot.mkdir()
            (systemRoot / "bin").mkdir(parents=True)
            systemctl = systemRoot / "bin/systemctl"
            systemctlLog = root / "systemctl.log"
            systemctl.write_text(
                "#!/bin/sh\n"
                "echo \"$*\" >> \"" + str(systemctlLog) + "\"\n"
                "if [ \"$1\" = is-active ] && "
                "[ \"$2\" = com.webos.service.camera.service ]; then\n"
                "  echo active\n"
                "else\n"
                "  echo inactive\n"
                "  exit 0\n"
                "fi\n")
            systemctl.chmod(0o755)
            unitDirectory = systemRoot / "lib/systemd/system"
            unitDirectory.mkdir(parents=True)
            (unitDirectory / "com.webos.service.camera.service").write_text(
                "[Unit]\nExecStart=/usr/sbin/camera --service\n")
            cameraProgram = systemRoot / "usr/sbin/camera"
            cameraProgram.parent.mkdir(parents=True)
            cameraProgram.write_text("fixture\n")
            cameraProgram.chmod(0o755)
            mountLog = root / "mount.log"
            for utility in ("mount", "umount"):
                command = systemRoot / "bin" / utility
                command.write_text(
                    "#!/bin/sh\n"
                    "echo \"" + utility + " $*\" >> \"" + str(mountLog) + "\"\n"
                    "exit 0\n")
                command.chmod(0o755)
            program = systemRoot / "usr/sbin/admanager"
            program.parent.mkdir(parents=True, exist_ok=True)
            program.write_text("fixture\n")
            (dataRoot / "disabled_services.json").write_text(
                json.dumps(["camera", "alwaysready"]))
            (procRoot / "101").mkdir()
            (procRoot / "101/exe").symlink_to(program)
            publicPort = next_port()
            nodePort = next_port()
            config.write_text(json.dumps({
                "host": "127.0.0.1", "port": publicPort, "token": token,
                "allowControl": True
            }))
            environment = os.environ.copy()
            environment["TVWEB_LUNA_SEND"] = str(fakeLuna)
            environment["TVWEB_DATA_DIR"] = str(dataRoot)
            environment["TVWEB_PROXY_SECRET"] = "background-services-proxy-secret-0123456789abcdef"
            process = subprocess.Popen(
                [str(binary), "--config", str(config), "--heartbeat", str(heartbeat),
                 "--workers", "4", "--listen", "--node-port", str(nodePort),
                 "--assets-root", str(assets), "--system-root", str(systemRoot),
                 "--proc-root", str(procRoot)],
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                env=environment)

            def request(path, method="GET"):
                req = Request("http://127.0.0.1:%d%s" % (publicPort, path),
                              method=method,
                              headers={"Authorization": "Bearer " + token})
                try:
                    with urlopen(req, timeout=12) as response:
                        return response.status, response.read()
                except HTTPError as error:
                    return error.code, error.read()

            def toggle(id, disabled, origin=None):
                headers = {"Content-Type": "application/json",
                           "Authorization": "Bearer " + token}
                if origin is not None:
                    headers["Origin"] = origin
                req = Request("http://127.0.0.1:%d/api/services/toggle" % publicPort,
                              data=json.dumps({"id": id, "disabled": disabled}).encode(),
                              method="POST", headers=headers)
                try:
                    with urlopen(req, timeout=12) as response:
                        return response.status, response.read()
                except HTTPError as error:
                    return error.code, error.read()

            try:
                deadline = time.monotonic() + 8
                ready = False
                while time.monotonic() < deadline and process.poll() is None:
                    try:
                        request("/api/caps")
                        ready = True
                        break
                    except (OSError, URLError):
                        time.sleep(0.03)
                if not ready:
                    if process.poll() is None:
                        process.kill()
                    output, _ = process.communicate(timeout=3)
                    self.fail("native service status daemon did not start: " + output)
                status, body = request("/api/services")
                self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                result = json.loads(body)
                self.assertTrue(result["ok"])
                self.assertTrue(result["supported"])
                self.assertEqual([row["id"] for row in result["services"]],
                                 ["camera", "admanager"])
                camera, admanager = result["services"]
                self.assertEqual(camera["title"], "USB Camera Listener")
                self.assertEqual(camera["unit"], "com.webos.service.camera.service")
                self.assertTrue(camera["disabled"])
                self.assertTrue(camera["running"], "systemd status is included")
                self.assertTrue(admanager["running"], "on-demand process state is included")
                self.assertTrue(admanager["disabled"] is False)
                self.assertIsNone(admanager["unit"], "a service with no unit lists null, as the server does")
                status, body = request("/api/services", "HEAD")
                self.assertEqual(status, 200)
                self.assertEqual(body, b"")
                self.assertEqual(json.loads((dataRoot / "disabled_services.json").read_text()),
                                 ["camera", "alwaysready"],
                                 "a read must not mutate Node-owned service state")

                status, body = toggle("camera", False)
                self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                self.assertEqual(json.loads(body), {
                    "ok": True, "id": "camera", "disabled": False, "running": True})
                self.assertEqual(json.loads((dataRoot / "disabled_services.json").read_text()), [])
                self.assertIn("start com.webos.service.camera.service",
                              systemctlLog.read_text())

                status, body = toggle("camera", True)
                self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                self.assertEqual(json.loads(body), {
                    "ok": True, "id": "camera", "disabled": True, "running": False})
                self.assertEqual(json.loads((dataRoot / "disabled_services.json").read_text()),
                                 ["camera"])
                self.assertEqual((dataRoot / "service-off").stat().st_mode & 0o777, 0o755)
                bootHook = systemRoot / "var/lib/webosbrew/init.d/20-tvweb-services"
                self.assertTrue(bootHook.exists())
                self.assertIn("off \"/usr/sbin/camera\"", bootHook.read_text())
                self.assertIn("stop com.webos.service.camera.service", systemctlLog.read_text())
                self.assertIn("mount --bind", mountLog.read_text())

                mountTarget = str(cameraProgram)
                (procRoot / "mounts").write_text(
                    "/var/lib/tvweb/service-off " + mountTarget + " none rw 0 0\n")
                status, body = toggle("camera", False)
                self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                self.assertEqual(json.loads((dataRoot / "disabled_services.json").read_text()), [])
                self.assertFalse(bootHook.exists())
                self.assertIn("umount " + mountTarget, mountLog.read_text())

                status, body = toggle("voiceconductor", True)
                self.assertEqual(status, 400)
                self.assertFalse(json.loads(body)["ok"])
                status, body = toggle("camera", True, "http://evil.example")
                self.assertEqual(status, 403)
            finally:
                if process.poll() is None:
                    process.send_signal(signal.SIGTERM)
                    try:
                        process.communicate(timeout=6)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.communicate(timeout=3)

    def test_native_ssh_keys_route_preserves_token_and_file_contract(self):
        binary = Path(sys.argv[1]).resolve()
        fakeLuna = binary.parent / "native-fake-luna-send"
        token = "ssh-keys-test-token"
        secret = "ssh-keys-proxy-secret-0123456789abcdef"
        reservedPorts = set()

        def next_port():
            port = unused_port_pair(reservedPorts)
            reservedPorts.update((port, port + 1))
            return port

        nodePort = next_port()
        ed = ("ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIHNWUlvIOI3rP9HRKBnPo43I42IfLCo105Go4U7x4Vef "
              "ed@test")
        ec = ("ecdsa-sha2-nistp256 AAAAE2VjZHNhLXNoYTItbmlzdHAyNTYAAAAIbmlzdHAyNTYAAABBBOjDvPcUSd/g8kmInR3WZYXM+kJHSfnV0P/dEmnHENV/V3qJ+vZCMTqAxgX6zJWMwyjIfJYAI0KPTECjGTuC22Y= "
              "ec@test")
        edFingerprint = "SHA256:2zyRteKuXNx0xachsViK0oWVjR5mFrpcCEQwrjhb7mM"
        ecFingerprint = "SHA256:slxSqMW+W9tmAUsZZPzeYNhCuxTcAQzSq3YbVP2ofsc"

        with tempfile.TemporaryDirectory(prefix="nativetv-native-ssh-") as folder:
            root = Path(folder)
            assets = root / "assets"
            systemRoot = root / "system"
            procRoot = root / "proc"
            dataRoot = root / "data"
            assets.mkdir()
            procRoot.mkdir()
            dataRoot.mkdir()
            (systemRoot / "etc").mkdir(parents=True)
            (systemRoot / "etc/shadow").write_text(
                "root:xGVw8H4GqkKg6:19000:0:99999:7:::\nnobody:*:19000::::::\n")
            (systemRoot / "home/root/.ssh").mkdir(parents=True)
            keyFile = systemRoot / "home/root/.ssh/authorized_keys"
            keyFile.write_text("# keep\nfrom=\"10.0.0.1, 10.0.0.2\",no-pty " + ed + "\n")
            (procRoot / "44").mkdir()
            (procRoot / "44/comm").write_text("dropbear\n")
            heartbeat = root / "tvweb.beat"
            config = root / "config.json"

            def start_server(port, control, configuredToken):
                config.write_text(json.dumps({
                    "host": "127.0.0.1", "port": port, "token": configuredToken,
                    "allowControl": control
                }))
                environment = os.environ.copy()
                environment["TVWEB_LUNA_SEND"] = str(fakeLuna)
                environment["TVWEB_DATA_DIR"] = str(dataRoot)
                environment["TVWEB_PROXY_SECRET"] = secret
                process = subprocess.Popen(
                    [str(binary), "--config", str(config), "--heartbeat", str(heartbeat),
                     "--workers", "4", "--listen", "--node-port", str(nodePort),
                     "--assets-root", str(assets), "--system-root", str(systemRoot),
                     "--proc-root", str(procRoot)],
                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                    env=environment)
                deadline = time.monotonic() + 8
                while time.monotonic() < deadline and process.poll() is None:
                    try:
                        request(port, "/api/caps", "GET", None,
                                configuredToken or None)
                        return process
                    except (OSError, URLError):
                        time.sleep(0.03)
                output, _ = process.communicate(timeout=3)
                self.fail("native SSH daemon did not start: " + output)

            def stop_server(process):
                if process.poll() is None:
                    process.send_signal(signal.SIGTERM)
                    try:
                        process.communicate(timeout=6)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.communicate(timeout=3)

            def request(port, path, method="GET", payload=None, requestToken=None):
                data = None if payload is None else json.dumps(payload).encode("utf-8")
                headers = {}
                if requestToken:
                    headers["Authorization"] = "Bearer " + requestToken
                if data is not None:
                    headers["Content-Type"] = "application/json"
                req = Request("http://127.0.0.1:%d%s" % (port, path),
                              data=data, method=method, headers=headers)
                try:
                    with urlopen(req, timeout=12) as response:
                        return response.status, response.read()
                except HTTPError as error:
                    return error.code, error.read()

            process = None
            try:
                port = next_port()
                process = start_server(port, True, token)
                status, body = request(port, "/api/ssh", requestToken=token)
                self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                result = json.loads(body)
                self.assertTrue(result["running"])
                self.assertTrue(result["fileExists"])
                self.assertTrue(result["passwordNow"])
                self.assertFalse(result["passwordAfterReboot"])
                self.assertTrue(result["writable"])
                self.assertFalse(result["needsToken"])
                self.assertEqual(result["keys"][0]["fingerprint"], edFingerprint)
                self.assertEqual(result["keys"][0]["comment"], "ed@test")
                self.assertEqual(result["keys"][0]["options"],
                                 'from="10.0.0.1, 10.0.0.2",no-pty')
                status, body = request(port, "/api/ssh", "HEAD", requestToken=token)
                self.assertEqual(status, 200)
                self.assertEqual(body, b"")
                status, body = request(port, "/api/ssh", "GET", requestToken="wrong")
                self.assertEqual(status, 200,
                                 "loopback requests retain the legacy on-TV auth exemption")

                status, body = request(port, "/api/ssh", "POST",
                                       {"action": "add", "key": ec}, token)
                self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                self.assertTrue(json.loads(body)["writable"])
                self.assertEqual(json.loads(body)["keys"][1]["fingerprint"], ecFingerprint)
                self.assertEqual(keyFile.read_text().count("\n"), 3)
                self.assertEqual(keyFile.stat().st_mode & 0o777, 0o600)
                self.assertEqual(keyFile.parent.stat().st_mode & 0o777, 0o700)

                status, body = request(port, "/api/ssh", "POST",
                                       {"action": "add", "key": ec.replace("ec@test", "another")}, token)
                self.assertEqual(status, 400)
                self.assertIn("already there", json.loads(body)["error"])
                status, body = request(port, "/api/ssh", "POST",
                                       {"action": "remove", "fingerprint": edFingerprint}, token)
                self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                self.assertEqual(json.loads(body)["keys"][0]["fingerprint"], ecFingerprint)
                status, body = request(port, "/api/ssh", "POST",
                                       {"action": "deleteFile"}, token)
                self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                self.assertFalse(json.loads(body)["fileExists"])
                self.assertTrue(json.loads(body)["passwordAfterReboot"])
                self.assertFalse(keyFile.exists())
            finally:
                if process is not None:
                    stop_server(process)

            keyFile.write_text(ed + "\n")
            missingTokenPort = next_port()
            process = start_server(missingTokenPort, True, "")
            try:
                status, body = request(missingTokenPort, "/api/ssh")
                self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                result = json.loads(body)
                self.assertFalse(result["writable"])
                self.assertTrue(result["needsToken"])
                self.assertNotIn("comment", result["keys"][0])
                self.assertNotIn("options", result["keys"][0])
                status, body = request(missingTokenPort, "/api/ssh", "POST",
                                       {"action": "add", "key": ec})
                self.assertEqual(status, 403)
                self.assertIn("needs a token", json.loads(body)["error"])
            finally:
                stop_server(process)

            noControlPort = next_port()
            process = start_server(noControlPort, False, token)
            try:
                status, body = request(noControlPort, "/api/ssh", "POST",
                                       {"action": "deleteFile"}, token)
                self.assertEqual(status, 403)
                self.assertIn("controls disabled", json.loads(body)["error"])
                self.assertTrue(keyFile.exists())
            finally:
                stop_server(process)

    def test_native_privacy_read_projection_preserves_legacy_contract(self):
        binary = Path(sys.argv[1]).resolve()
        fakeLuna = binary.parent / "native-fake-luna-send"
        publicPort = unused_port_pair()
        nodePort = unused_port_pair((publicPort, publicPort + 1))
        token = "privacy-read-test-token"
        secret = "privacy-read-proxy-secret-0123456789abcdef"
        nodeRequests = []

        class NodeCaptureHandler(BaseHTTPRequestHandler):
            def do_POST(self):
                length = int(self.headers.get("Content-Length", "0"))
                nodeRequests.append({"path": self.path, "body": self.rfile.read(length)})
                self.send_response(204)
                self.send_header("Content-Length", "0")
                self.end_headers()

            def log_message(self, *_args):
                pass

        node = HTTPServer(("127.0.0.1", nodePort), NodeCaptureHandler)
        nodeThread = threading.Thread(target=node.serve_forever, daemon=True)
        nodeThread.start()

        with tempfile.TemporaryDirectory(prefix="nativetv-native-privacy-read-") as folder:
            root = Path(folder)
            config = root / "config.json"
            heartbeat = root / "tvweb.beat"
            assets = root / "assets"
            systemRoot = root / "system"
            procRoot = root / "proc"
            dataRoot = root / "data"
            assets.mkdir()
            procRoot.mkdir()
            dataRoot.mkdir()
            (systemRoot / "etc").mkdir(parents=True)
            (systemRoot / "etc/hosts").write_text("127.0.0.1 localhost\n")
            (systemRoot / "var/luna/preferences").mkdir(parents=True)
            # As on a TV: the flags sit under eulaStatus, beside the agreement list.
            (systemRoot / "var/luna/preferences/eula").write_text(json.dumps({
                "eulaInfo": {"eulaList": []},
                "eulaStatus": {
                    "acrAllowed": True, "acrAdAllowed": True,
                    "generalTermsAllowed": False, "voiceAllowed": True}
            }))
            licenseDirectory = systemRoot / "var/palm/license"
            licenseDirectory.mkdir(parents=True)
            (licenseDirectory / "eulaInfoNetwork.json").write_text(json.dumps({
                "eulaMappingList": {"eulaInfo": [
                    {"settingKey": "acrAllowed", "mandatory": ["acr-doc"]},
                    {"settingKey": "acrAdAllowed", "mandatory": ["acr-doc"]},
                    {"settingKey": "generalTermsAllowed", "mandatory": ["terms-doc"]}
                ]}
            }))
            serviceDirectory = systemRoot / "usr/share/dbus-1/system-services"
            serviceDirectory.mkdir(parents=True)
            for filename, serviceName, program in (
                    ("acr.service", "com.webos.service.acr", "/usr/sbin/acr2"),
                    ("admanager.service", "com.webos.service.admanager", "/usr/sbin/admanager"),
                    ("voice.service", "com.webos.service.voiceinput.sound",
                     "/usr/sbin/voiceinput.sound")):
                (serviceDirectory / filename).write_text(
                    "[D-BUS Service]\nName=%s\nType=dynamic\nExec=%s\n" %
                    (serviceName, program))
            details = root / "luna-details.jsonl"
            config.write_text(json.dumps({
                "host": "127.0.0.1", "port": publicPort, "token": token,
                "allowControl": True
            }))
            environment = os.environ.copy()
            environment["TVWEB_LUNA_SEND"] = str(fakeLuna)
            environment["TVWEB_DATA_DIR"] = str(dataRoot)
            environment["TVWEB_PROXY_SECRET"] = secret
            environment["NATIVETV_FAKE_LUNA_MODE"] = "privacy-read"
            environment["NATIVETV_FAKE_LUNA_DETAILS"] = str(details)
            process = subprocess.Popen(
                [str(binary), "--config", str(config), "--heartbeat", str(heartbeat),
                 "--workers", "4", "--listen", "--node-port", str(nodePort),
                 "--assets-root", str(assets), "--system-root", str(systemRoot),
                 "--proc-root", str(procRoot)],
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                env=environment)

            def request(path, method="GET", payload=None):
                data = None if payload is None else json.dumps(payload).encode("utf-8")
                headers = {"Authorization": "Bearer " + token}
                if data is not None:
                    headers["Content-Type"] = "application/json"
                req = Request("http://127.0.0.1:%d%s" % (publicPort, path),
                              data=data, method=method, headers=headers)
                try:
                    with urlopen(req, timeout=12) as response:
                        return response.status, response.read()
                except HTTPError as error:
                    return error.code, error.read()

            try:
                deadline = time.monotonic() + 8
                ready = False
                while time.monotonic() < deadline and process.poll() is None:
                    try:
                        request("/api/caps")
                        ready = True
                        break
                    except (OSError, URLError):
                        time.sleep(0.03)
                self.assertTrue(ready, "native daemon did not start")

                status, body = request("/api/privacy")
                self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                result = json.loads(body)
                self.assertTrue(result["ok"])
                self.assertEqual(result["consentWritable"], True)
                self.assertEqual(result["consentGroups"][0], ["advertising", "Advertising"])
                known = {row["key"]: row for row in result["consent"]["known"]}
                self.assertEqual(known["acrAllowed"]["agreements"],
                                 ["Screen recognition agreement"])
                self.assertEqual(known["acrAllowed"]["sharesWith"],
                                 ["Ads based on what you watch"])
                self.assertEqual(known["voiceAllowed"]["enabled"], True)
                self.assertEqual(result["acr"]["stopped"], True)
                self.assertEqual(result["acr"]["captureState"], "stopped")
                self.assertEqual(result["acr"]["consentOn"], True)
                self.assertEqual(result["advertisingId"]["stopped"], True)
                self.assertEqual(result["advertisingId"]["limitTracking"], True)
                self.assertEqual(result["advertisingId"]["lmtKnown"], True)
                self.assertEqual(result["adblock"]["mode"], "off")
                self.assertGreater(result["adblock"]["count"], 0)
                self.assertGreater(result["adblock"]["adCount"], 0)
                self.assertEqual(result["tvUpdates"]["blocked"], False)
                self.assertEqual(result["simple"]["total"], 3)

                recorded = [json.loads(line) for line in details.read_text().splitlines()]
                routeCalls = without_country_learning(recorded)
                uris = [call["uri"] for call in routeCalls]
                self.assertFalse(any(uri.startswith("com.webos.service.acr/") or
                                     uri.startswith("com.webos.service.admanager/") or
                                     uri.startswith("com.webos.service.voiceinput.sound/")
                                     for uri in uris),
                                 "privacy reads must not wake on-demand LG services")
                callCount = len(routeCalls)
                status, body = request("/api/privacy", "HEAD")
                self.assertEqual(status, 200)
                self.assertEqual(body, b"")
                recorded = [json.loads(line) for line in details.read_text().splitlines()]
                self.assertEqual(len(without_country_learning(recorded)), callCount,
                                 "GET and HEAD share the privacy response cache")
                self.assertEqual(nodeRequests, [],
                                 "native privacy projection must not proxy to Node")

                (procRoot / "101").mkdir()
                (procRoot / "101/cmdline").write_bytes(b"/usr/sbin/acr2\0--dbus\0")
                (procRoot / "102").mkdir()
                (procRoot / "102/cmdline").write_bytes(b"/usr/sbin/admanager\0--dbus\0")
                status, body = request("/api/control", "POST", {
                    "action": "volume", "value": 22
                })
                self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                self.assertEqual(json.loads(body)["ok"], True)
                self.assertEqual(len(nodeRequests), 1,
                                 "a successful native control invalidates the local privacy cache")

                status, body = request("/api/privacy")
                self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                active = json.loads(body)
                self.assertEqual(active["acr"]["running"], True)
                self.assertEqual(active["acr"]["active"], True)
                self.assertEqual(active["acr"]["capturing"], True)
                self.assertNotIn("stopped", active["acr"])
                self.assertEqual(active["advertisingId"]["available"], True)
                self.assertEqual(active["advertisingId"]["present"], True)
                self.assertNotIn("stopped", active["advertisingId"])
                recorded = [json.loads(line) for line in details.read_text().splitlines()]
                uris = [call["uri"] for call in without_country_learning(recorded)]
                self.assertIn("com.webos.service.acr/getACRSolutionStatus", uris)
                self.assertIn("com.webos.service.acr/getVideoCaptureStatus", uris)
                self.assertIn("com.webos.service.admanager/getAdid", uris)
                self.assertFalse(any(uri.startswith("com.webos.service.voiceinput.sound/")
                                     for uri in uris),
                                 "the microphone probe also leaves an idle dynamic service stopped")
            finally:
                if process.poll() is None:
                    process.send_signal(signal.SIGTERM)
                    try:
                        process.communicate(timeout=6)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.communicate(timeout=3)
                node.shutdown()
                node.server_close()
                nodeThread.join(timeout=3)

    def test_native_consent_control_preserves_lg_agreement_rules(self):
        binary = Path(sys.argv[1]).resolve()
        fakeLuna = binary.parent / "native-fake-luna-send"
        publicPort = unused_port_pair()
        nodePort = unused_port_pair((publicPort, publicPort + 1))
        token = "privacy-consent-test-token"
        secret = "privacy-consent-proxy-secret-0123456789abcdef"
        nodeRequests = []

        class NodeCaptureHandler(BaseHTTPRequestHandler):
            def do_POST(self):
                length = int(self.headers.get("Content-Length", "0"))
                nodeRequests.append({"path": self.path, "body": self.rfile.read(length)})
                self.send_response(204)
                self.send_header("Content-Length", "0")
                self.end_headers()

            def log_message(self, *_args):
                pass

        node = HTTPServer(("127.0.0.1", nodePort), NodeCaptureHandler)
        nodeThread = threading.Thread(target=node.serve_forever, daemon=True)
        nodeThread.start()

        with tempfile.TemporaryDirectory(prefix="nativetv-native-consent-") as folder:
            root = Path(folder)
            config = root / "config.json"
            heartbeat = root / "tvweb.beat"
            assets = root / "assets"
            systemRoot = root / "system"
            licenseDirectory = systemRoot / "var/palm/license"
            assets.mkdir()
            licenseDirectory.mkdir(parents=True)
            (licenseDirectory / "eulaInfoNetwork.json").write_text(json.dumps({
                "eulaMappingList": {"eulaInfo": [
                    {"settingKey": "acrAllowed", "mandatory": ["acr-doc"]},
                    {"settingKey": "acrAdAllowed", "mandatory": ["acr-doc"]},
                    {"settingKey": "generalTermsAllowed", "mandatory": ["terms-doc"]}
                ]}
            }))
            details = root / "luna-details.jsonl"
            config.write_text(json.dumps({
                "host": "127.0.0.1", "port": publicPort, "token": token,
                "allowControl": True
            }))
            environment = os.environ.copy()
            environment["TVWEB_LUNA_SEND"] = str(fakeLuna)
            environment["TVWEB_DATA_DIR"] = str(root / "data")
            environment["TVWEB_PROXY_SECRET"] = secret
            environment["NATIVETV_FAKE_LUNA_MODE"] = "privacy-consent"
            environment["NATIVETV_FAKE_LUNA_DETAILS"] = str(details)
            process = subprocess.Popen(
                [str(binary), "--config", str(config), "--heartbeat", str(heartbeat),
                 "--workers", "4", "--listen", "--node-port", str(nodePort),
                 "--assets-root", str(assets), "--system-root", str(systemRoot)],
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                env=environment)
            output = ""

            def request(path, body=None):
                data = None if body is None else json.dumps(body).encode("utf-8")
                headers = {"Authorization": "Bearer " + token}
                if data is not None:
                    headers["Content-Type"] = "application/json"
                req = Request("http://127.0.0.1:%d%s" % (publicPort, path),
                              data=data, method="POST", headers=headers)
                try:
                    with urlopen(req, timeout=8) as response:
                        return response.status, response.read()
                except HTTPError as error:
                    return error.code, error.read()

            try:
                deadline = time.monotonic() + 8
                ready = False
                while time.monotonic() < deadline and process.poll() is None:
                    try:
                        with urlopen(Request(
                                "http://127.0.0.1:%d/api/caps" % publicPort,
                                headers={"Authorization": "Bearer " + token}),
                                timeout=1):
                            pass
                        ready = True
                        break
                    except (OSError, URLError):
                        time.sleep(0.03)
                self.assertTrue(ready, "native daemon did not start")

                status, body = request("/api/control", {
                    "action": "consent",
                    "value": {"key": "acrAllowed", "enabled": False}
                })
                self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                self.assertEqual(json.loads(body), {
                    "ok": True, "key": "acrAllowed", "enabled": False,
                    "changed": True, "alsoChanged": 1
                })
                calls = without_country_learning(
                    [json.loads(line) for line in details.read_text().splitlines()])
                consentCalls = [call for call in calls
                                if call["uri"].startswith("com.webos.settingsservice/")]
                self.assertEqual([call["uri"] for call in consentCalls], [
                    "com.webos.settingsservice/getSystemSettings",
                    "com.webos.settingsservice/getSystemSettings",
                    "com.webos.settingsservice/setSystemSettings",
                    "com.webos.settingsservice/getSystemSettings"
                ])
                write = consentCalls[2]["payload"]["settings"]
                self.assertEqual(write["eulaStatus"]["acrAllowed"], False)
                self.assertEqual(write["eulaStatus"]["acrAdAllowed"], False)
                self.assertEqual(write["eulaInfoNetwork"]["region"], "GB")
                accepted = {doc["id"]: doc["accepted"]
                            for doc in write["eulaInfoNetwork"]["eulaList"]}
                self.assertEqual(accepted, {"acr-doc": False, "other-doc": True})
                self.assertEqual(nodeRequests[-1]["path"], "/_internal/native-state",
                                 "successful consent writes invalidate Node's privacy cache")

                before = len(calls)
                status, body = request("/api/control", {
                    "action": "consent",
                    "value": {"key": "chpAllowed", "enabled": False}
                })
                self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                self.assertEqual(json.loads(body), {
                    "ok": False,
                    "error": "chpAllowed is not changeable from here"
                })
                calls = without_country_learning(
                    [json.loads(line) for line in details.read_text().splitlines()])
                self.assertEqual(len(calls), before,
                                 "hidden LG Channels consent is rejected before Luna")
            finally:
                if process.poll() is None:
                    process.send_signal(signal.SIGTERM)
                try:
                    output, _ = process.communicate(timeout=6)
                except subprocess.TimeoutExpired:
                    process.kill()
                    output, _ = process.communicate(timeout=3)
                    self.fail("native daemon did not stop after SIGTERM:\n" + output[-4000:])
                node.shutdown()
                node.server_close()
                nodeThread.join(timeout=3)
            self.assertIn("command-policy: action=control.consent result=success", output)

    def test_native_acr_toggle_follows_binary_consents_and_fallback(self):
        binary = Path(sys.argv[1]).resolve()
        fakeLuna = binary.parent / "native-fake-luna-send"
        publicPort = unused_port_pair()
        nodePort = unused_port_pair((publicPort, publicPort + 1))
        token = "privacy-acr-test-token"
        secret = "privacy-acr-proxy-secret-0123456789abcdef"
        nodeRequests = []

        class NodeCaptureHandler(BaseHTTPRequestHandler):
            def do_POST(self):
                length = int(self.headers.get("Content-Length", "0"))
                nodeRequests.append({"path": self.path, "body": self.rfile.read(length)})
                self.send_response(204)
                self.send_header("Content-Length", "0")
                self.end_headers()

            def log_message(self, *_args):
                pass

        node = HTTPServer(("127.0.0.1", nodePort), NodeCaptureHandler)
        nodeThread = threading.Thread(target=node.serve_forever, daemon=True)
        nodeThread.start()

        with tempfile.TemporaryDirectory(prefix="nativetv-native-acr-") as folder:
            root = Path(folder)
            config = root / "config.json"
            heartbeat = root / "tvweb.beat"
            assets = root / "assets"
            systemRoot = root / "system"
            licenseDirectory = systemRoot / "var/palm/license"
            binaryDirectory = systemRoot / "usr/sbin"
            assets.mkdir()
            licenseDirectory.mkdir(parents=True)
            binaryDirectory.mkdir(parents=True)
            mapping = {
                "eulaMappingList": {"eulaInfo": [
                    {"settingKey": "acrAllowed", "mandatory": ["acr-doc"]},
                    {"settingKey": "acrOnAllowed", "mandatory": ["acr-doc"]},
                    {"settingKey": "acrAdAllowed", "mandatory": ["acr-doc"]}
                ]}
            }
            (licenseDirectory / "eulaInfoNetwork.json").write_text(json.dumps(mapping))
            acrBinary = binaryDirectory / "acr2"
            acrBinary.write_bytes(b"\x7fELF\0acrOnAllowed\0")
            details = root / "luna-details.jsonl"
            config.write_text(json.dumps({
                "host": "127.0.0.1", "port": publicPort, "token": token,
                "allowControl": True
            }))
            environment = os.environ.copy()
            environment["TVWEB_LUNA_SEND"] = str(fakeLuna)
            environment["TVWEB_DATA_DIR"] = str(root / "data")
            environment["TVWEB_PROXY_SECRET"] = secret
            environment["NATIVETV_FAKE_LUNA_MODE"] = "privacy-acr"
            environment["NATIVETV_FAKE_LUNA_DETAILS"] = str(details)
            process = subprocess.Popen(
                [str(binary), "--config", str(config), "--heartbeat", str(heartbeat),
                 "--workers", "4", "--listen", "--node-port", str(nodePort),
                 "--assets-root", str(assets), "--system-root", str(systemRoot)],
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                env=environment)
            output = ""

            def request(value):
                data = json.dumps({"action": "acr", "value": value}).encode("utf-8")
                req = Request("http://127.0.0.1:%d/api/control" % publicPort,
                              data=data, method="POST", headers={
                                  "Authorization": "Bearer " + token,
                                  "Content-Type": "application/json"
                              })
                try:
                    with urlopen(req, timeout=8) as response:
                        return response.status, response.read()
                except HTTPError as error:
                    return error.code, error.read()

            try:
                deadline = time.monotonic() + 8
                ready = False
                while time.monotonic() < deadline and process.poll() is None:
                    try:
                        with urlopen(Request(
                                "http://127.0.0.1:%d/api/caps" % publicPort,
                                headers={"Authorization": "Bearer " + token}),
                                timeout=1):
                            pass
                        ready = True
                        break
                    except (OSError, URLError):
                        time.sleep(0.03)
                self.assertTrue(ready, "native daemon did not start")

                status, body = request("on")
                self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                self.assertEqual(json.loads(body), {"ok": True, "alsoChanged": 0})
                calls = without_country_learning(
                    [json.loads(line) for line in details.read_text().splitlines()])
                self.assertEqual([call["uri"] for call in calls], [
                    "com.webos.service.settings/getSystemSettings",
                    "com.webos.service.settings/setSystemSettings",
                    "com.webos.settingsservice/getSystemSettings",
                    "com.webos.settingsservice/getSystemSettings",
                    "com.webos.settingsservice/setSystemSettings",
                    "com.webos.settingsservice/getSystemSettings"
                ])
                self.assertEqual(calls[1]["payload"], {
                    "category": "option", "settings": {"livePlus": "on"}
                })
                firstConsentWrite = calls[4]["payload"]["settings"]
                self.assertFalse(firstConsentWrite["eulaStatus"]["acrAllowed"])
                self.assertTrue(firstConsentWrite["eulaStatus"]["acrOnAllowed"])
                self.assertEqual(nodeRequests[0]["path"], "/_internal/native-state")

                # An unreadable/unrecognized acr2 binary retains the legacy fallback.
                acrBinary.write_bytes(b"\x7fELF\0unknownConsentSymbol\0")
                status, body = request(False)
                self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                self.assertEqual(json.loads(body), {"ok": True, "alsoChanged": 1})
                calls = without_country_learning(
                    [json.loads(line) for line in details.read_text().splitlines()])
                self.assertEqual([call["uri"] for call in calls[6:]], [
                    "com.webos.service.settings/getSystemSettings",
                    "com.webos.service.settings/setSystemSettings",
                    "com.webos.settingsservice/getSystemSettings",
                    "com.webos.settingsservice/getSystemSettings",
                    "com.webos.settingsservice/setSystemSettings",
                    "com.webos.settingsservice/getSystemSettings"
                ])
                self.assertEqual(calls[7]["payload"], {
                    "category": "option", "settings": {"livePlus": "off"}
                })
                secondConsentWrite = calls[10]["payload"]["settings"]
                self.assertFalse(secondConsentWrite["eulaStatus"]["acrAllowed"])
                self.assertFalse(secondConsentWrite["eulaStatus"]["acrOnAllowed"])
                self.assertFalse(secondConsentWrite["eulaStatus"]["acrAdAllowed"])
                self.assertEqual([item["path"] for item in nodeRequests], [
                    "/_internal/native-state", "/_internal/native-state"
                ])
            finally:
                if process.poll() is None:
                    process.send_signal(signal.SIGTERM)
                try:
                    output, _ = process.communicate(timeout=6)
                except subprocess.TimeoutExpired:
                    process.kill()
                    output, _ = process.communicate(timeout=3)
                    self.fail("native daemon did not stop after SIGTERM:\n" + output[-4000:])
                node.shutdown()
                node.server_close()
                nodeThread.join(timeout=3)
            self.assertEqual(output.count(
                "command-policy: action=control.acr-live-plus result=success"), 2)
            self.assertEqual(output.count(
                "command-policy: action=control.consent result=success"), 2)

    def test_native_limit_ad_tracking_avoids_starting_dynamic_service(self):
        binary = Path(sys.argv[1]).resolve()
        fakeLuna = binary.parent / "native-fake-luna-send"
        publicPort = unused_port_pair()
        nodePort = unused_port_pair((publicPort, publicPort + 1))
        token = "privacy-lmt-test-token"
        secret = "privacy-lmt-proxy-secret-0123456789abcdef"
        nodeRequests = []

        class NodeCaptureHandler(BaseHTTPRequestHandler):
            def do_POST(self):
                length = int(self.headers.get("Content-Length", "0"))
                nodeRequests.append({"path": self.path, "body": self.rfile.read(length)})
                self.send_response(204)
                self.send_header("Content-Length", "0")
                self.end_headers()

            def log_message(self, *_args):
                pass

        node = HTTPServer(("127.0.0.1", nodePort), NodeCaptureHandler)
        nodeThread = threading.Thread(target=node.serve_forever, daemon=True)
        nodeThread.start()

        with tempfile.TemporaryDirectory(prefix="nativetv-native-lmt-") as folder:
            root = Path(folder)
            config = root / "config.json"
            heartbeat = root / "tvweb.beat"
            assets = root / "assets"
            systemRoot = root / "system"
            procRoot = root / "proc"
            serviceDirectory = systemRoot / "usr/share/dbus-1/system-services"
            assets.mkdir()
            serviceDirectory.mkdir(parents=True)
            (serviceDirectory / "admanager.service").write_text(
                "[D-BUS Service]\n"
                "Name=com.webos.service.admanager;\n"
                "Exec=/usr/sbin/admanager\n"
                "Type=dynamic\n")
            details = root / "luna-details.jsonl"
            config.write_text(json.dumps({
                "host": "127.0.0.1", "port": publicPort, "token": token,
                "allowControl": True
            }))
            environment = os.environ.copy()
            environment["TVWEB_LUNA_SEND"] = str(fakeLuna)
            environment["TVWEB_DATA_DIR"] = str(root / "data")
            environment["TVWEB_PROXY_SECRET"] = secret
            environment["NATIVETV_FAKE_LUNA_MODE"] = "privacy-lmt"
            environment["NATIVETV_FAKE_LUNA_DETAILS"] = str(details)
            process = subprocess.Popen(
                [str(binary), "--config", str(config), "--heartbeat", str(heartbeat),
                 "--workers", "4", "--listen", "--node-port", str(nodePort),
                 "--assets-root", str(assets), "--proc-root", str(procRoot),
                 "--system-root", str(systemRoot)],
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                env=environment)
            output = ""

            def request(body):
                data = json.dumps(body).encode("utf-8")
                req = Request("http://127.0.0.1:%d/api/control" % publicPort,
                              data=data, method="POST", headers={
                                  "Authorization": "Bearer " + token,
                                  "Content-Type": "application/json"
                              })
                try:
                    with urlopen(req, timeout=8) as response:
                        return response.status, response.read()
                except HTTPError as error:
                    return error.code, error.read()

            try:
                deadline = time.monotonic() + 8
                ready = False
                while time.monotonic() < deadline and process.poll() is None:
                    try:
                        with urlopen(Request(
                                "http://127.0.0.1:%d/api/caps" % publicPort,
                                headers={"Authorization": "Bearer " + token}),
                                timeout=1):
                            pass
                        ready = True
                        break
                    except (OSError, URLError):
                        time.sleep(0.03)
                self.assertTrue(ready, "native daemon did not start")

                status, body = request({
                    "action": "limitAdTracking", "value": True
                })
                self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                self.assertEqual(json.loads(body), {"ok": True, "limitTracking": True})
                calls = without_country_learning(
                    [json.loads(line) for line in details.read_text().splitlines()])
                self.assertEqual([call["uri"] for call in calls], [
                    "com.webos.settingsservice/getSystemSettings",
                    "com.webos.settingsservice/setSystemSettings",
                    "com.webos.settingsservice/getSystemSettings"
                ], "a stopped dynamic ad manager must never be queried and started")
                self.assertEqual(calls[1]["payload"], {
                    "category": "general", "settings": {"lmt": "on"}
                })

                blockedCalls = len(calls)
                status, body = request({"action": "clearAdCookies"})
                self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                self.assertEqual(json.loads(body), {
                    "ok": False,
                    "error": "LG’s advertising service is not running, and this would start it."
                })
                calls = without_country_learning(
                    [json.loads(line) for line in details.read_text().splitlines()])
                self.assertEqual(len(calls), blockedCalls,
                                 "clearing cookies must not start a stopped dynamic service")
                self.assertEqual(len(nodeRequests), 1,
                                 "a refused cookie clear does not invalidate successful state")

                (procRoot / "1234").mkdir(parents=True)
                (procRoot / "1234/cmdline").write_bytes(
                    b"/usr/sbin/admanager\0--dbus\0")
                status, body = request({
                    "action": "limitAdTracking", "value": "off"
                })
                self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                self.assertEqual(json.loads(body), {"ok": True, "limitTracking": False})
                calls = without_country_learning(
                    [json.loads(line) for line in details.read_text().splitlines()])
                self.assertEqual([call["uri"] for call in calls[3:]], [
                    "com.webos.settingsservice/getSystemSettings",
                    "com.webos.settingsservice/setSystemSettings",
                    "com.webos.service.admanager/getAdid"
                ], "a running ad manager supplies the verified value apps receive")
                self.assertEqual(calls[4]["payload"], {
                    "category": "general", "settings": {"lmt": "off"}
                })
                status, body = request({"action": "clearAdCookies"})
                self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                self.assertEqual(json.loads(body), {"ok": True})
                calls = without_country_learning(
                    [json.loads(line) for line in details.read_text().splitlines()])
                self.assertEqual(calls[-1]["uri"],
                                 "com.webos.service.admanager/inactivateCookies")
                self.assertEqual(calls[-1]["payload"], {})
                self.assertEqual([item["path"] for item in nodeRequests], [
                    "/_internal/native-state", "/_internal/native-state",
                    "/_internal/native-state"
                ], "successful privacy changes clear Node's cached projection")
            finally:
                if process.poll() is None:
                    process.send_signal(signal.SIGTERM)
                try:
                    output, _ = process.communicate(timeout=6)
                except subprocess.TimeoutExpired:
                    process.kill()
                    output, _ = process.communicate(timeout=3)
                    self.fail("native daemon did not stop after SIGTERM:\n" + output[-4000:])
                node.shutdown()
                node.server_close()
                nodeThread.join(timeout=3)
            self.assertEqual(output.count(
                "command-policy: action=control.limit-ad-tracking result=success"), 2)
            self.assertEqual(output.count(
                "command-policy: action=control.clear-ad-cookies result=success"), 1)

    def test_native_reset_ad_id_avoids_starting_dynamic_service(self):
        binary = Path(sys.argv[1]).resolve()
        fakeLuna = binary.parent / "native-fake-luna-send"
        publicPort = unused_port_pair()
        nodePort = unused_port_pair((publicPort, publicPort + 1))
        token = "privacy-reset-adid-test-token"
        secret = "privacy-reset-adid-proxy-secret-0123456789abcdef"
        nodeRequests = []

        class NodeCaptureHandler(BaseHTTPRequestHandler):
            def do_POST(self):
                length = int(self.headers.get("Content-Length", "0"))
                nodeRequests.append({"path": self.path, "body": self.rfile.read(length)})
                self.send_response(204)
                self.send_header("Content-Length", "0")
                self.end_headers()

            def log_message(self, *_args):
                pass

        node = HTTPServer(("127.0.0.1", nodePort), NodeCaptureHandler)
        nodeThread = threading.Thread(target=node.serve_forever, daemon=True)
        nodeThread.start()

        with tempfile.TemporaryDirectory(prefix="nativetv-native-reset-adid-") as folder:
            root = Path(folder)
            config = root / "config.json"
            heartbeat = root / "tvweb.beat"
            assets = root / "assets"
            systemRoot = root / "system"
            procRoot = root / "proc"
            serviceDirectory = systemRoot / "usr/share/dbus-1/system-services"
            assets.mkdir()
            serviceDirectory.mkdir(parents=True)
            (serviceDirectory / "admanager.service").write_text(
                "[D-BUS Service]\n"
                "Name=com.webos.service.admanager;\n"
                "Exec=/usr/sbin/admanager\n"
                "Type=dynamic\n")
            details = root / "luna-details.jsonl"
            config.write_text(json.dumps({
                "host": "127.0.0.1", "port": publicPort, "token": token,
                "allowControl": True
            }))
            environment = os.environ.copy()
            environment["TVWEB_LUNA_SEND"] = str(fakeLuna)
            environment["TVWEB_DATA_DIR"] = str(root / "data")
            environment["TVWEB_PROXY_SECRET"] = secret
            environment["NATIVETV_FAKE_LUNA_MODE"] = "privacy-reset-adid"
            environment["NATIVETV_FAKE_LUNA_DETAILS"] = str(details)
            process = subprocess.Popen(
                [str(binary), "--config", str(config), "--heartbeat", str(heartbeat),
                 "--workers", "4", "--listen", "--node-port", str(nodePort),
                 "--assets-root", str(assets), "--proc-root", str(procRoot),
                 "--system-root", str(systemRoot)],
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                env=environment)
            output = ""

            def request():
                req = Request("http://127.0.0.1:%d/api/control" % publicPort,
                              data=json.dumps({"action": "resetAdId"}).encode("utf-8"),
                              method="POST", headers={
                                  "Authorization": "Bearer " + token,
                                  "Content-Type": "application/json"
                              })
                try:
                    with urlopen(req, timeout=8) as response:
                        return response.status, response.read()
                except HTTPError as error:
                    return error.code, error.read()

            try:
                deadline = time.monotonic() + 8
                ready = False
                while time.monotonic() < deadline and process.poll() is None:
                    try:
                        with urlopen(Request(
                                "http://127.0.0.1:%d/api/caps" % publicPort,
                                headers={"Authorization": "Bearer " + token}),
                                timeout=1):
                            pass
                        ready = True
                        break
                    except (OSError, URLError):
                        time.sleep(0.03)
                self.assertTrue(ready, "native daemon did not start")

                status, body = request()
                self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                self.assertEqual(json.loads(body), {
                    "ok": False,
                    "error": "LG’s advertising service is not running, and this would start it."
                })
                startupCalls = ([json.loads(line) for line in details.read_text().splitlines()]
                                if details.exists() else [])
                self.assertFalse(any(call["uri"].startswith(
                    "com.webos.service.admanager/") for call in startupCalls),
                    "the stopped dynamic service must not be queried")

                (procRoot / "1234").mkdir(parents=True)
                (procRoot / "1234/cmdline").write_bytes(
                    b"/usr/sbin/admanager\0--dbus\0")
                status, body = request()
                self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                self.assertEqual(json.loads(body), {"ok": True, "changed": True})
                calls = without_country_learning(
                    [json.loads(line) for line in details.read_text().splitlines()])
                self.assertEqual([call["uri"] for call in calls], [
                    "com.webos.service.admanager/getAdid",
                    "com.webos.service.admanager/resetIFA",
                    "com.webos.service.admanager/getAdid"
                ])
                self.assertEqual(calls[1]["payload"], {})
                self.assertEqual([item["path"] for item in nodeRequests], [
                    "/_internal/native-state"
                ], "a successful reset invalidates Node's cached privacy projection")
            finally:
                if process.poll() is None:
                    process.send_signal(signal.SIGTERM)
                try:
                    output, _ = process.communicate(timeout=6)
                except subprocess.TimeoutExpired:
                    process.kill()
                    output, _ = process.communicate(timeout=3)
                    self.fail("native daemon did not stop after SIGTERM:\n" + output[-4000:])
                node.shutdown()
                node.server_close()
                nodeThread.join(timeout=3)
            self.assertEqual(output.count(
                "command-policy: action=control.reset-ad-id result=success"), 1)

    def test_native_oled_protection_uses_sysprop_fallback(self):
        binary = Path(sys.argv[1]).resolve()
        fakeLuna = binary.parent / "native-fake-luna-send"
        publicPort = unused_port_pair()
        nodePort = unused_port_pair((publicPort, publicPort + 1))
        token = "oled-sysprop-test-token"
        secret = "oled-sysprop-proxy-secret-0123456789abcdef"
        nodeRequests = []

        class NodeCaptureHandler(BaseHTTPRequestHandler):
            def do_POST(self):
                length = int(self.headers.get("Content-Length", "0"))
                body = self.rfile.read(length)
                nodeRequests.append({"path": self.path, "body": body})
                self.send_response(204)
                self.send_header("Content-Length", "0")
                self.end_headers()

            def log_message(self, *_args):
                pass

        node = HTTPServer(("127.0.0.1", nodePort), NodeCaptureHandler)
        nodeThread = threading.Thread(target=node.serve_forever, daemon=True)
        nodeThread.start()

        with tempfile.TemporaryDirectory(prefix="nativetv-native-oled-sysprop-") as folder:
            root = Path(folder)
            config = root / "config.json"
            heartbeat = root / "tvweb.beat"
            assets = root / "assets"
            assets.mkdir()
            details = root / "luna-details.jsonl"
            config.write_text(json.dumps({
                "host": "127.0.0.1", "port": publicPort, "token": token,
                "allowControl": True
            }))
            environment = os.environ.copy()
            environment["TVWEB_LUNA_SEND"] = str(fakeLuna)
            environment["TVWEB_DATA_DIR"] = str(root)
            environment["TVWEB_PROXY_SECRET"] = secret
            environment["NATIVETV_FAKE_LUNA_MODE"] = "oled-sysprop"
            environment["NATIVETV_FAKE_LUNA_DETAILS"] = str(details)
            process = subprocess.Popen(
                [str(binary), "--config", str(config), "--heartbeat", str(heartbeat),
                 "--workers", "4", "--listen", "--node-port", str(nodePort),
                 "--assets-root", str(assets)],
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                env=environment)

            def request(path, method="GET", body=None):
                data = None if body is None else json.dumps(body).encode("utf-8")
                headers = {"Authorization": "Bearer " + token}
                if data is not None:
                    headers["Content-Type"] = "application/json"
                req = Request("http://127.0.0.1:%d%s" % (publicPort, path),
                              data=data, method=method, headers=headers)
                try:
                    with urlopen(req, timeout=8) as response:
                        return response.status, response.read()
                except HTTPError as error:
                    return error.code, error.read()

            try:
                deadline = time.monotonic() + 8
                ready = False
                while time.monotonic() < deadline and process.poll() is None:
                    try:
                        request("/api/caps")
                        ready = True
                        break
                    except (OSError, URLError):
                        time.sleep(0.03)
                if not ready:
                    if process.poll() is None:
                        process.send_signal(signal.SIGTERM)
                    output, _ = process.communicate(timeout=6)
                    self.fail("native daemon did not start:\n" + output[-4000:])

                status, body = request("/api/control", "POST", {
                    "action": "oledProtection",
                    "value": {"key": "gsr", "enabled": True}})
                self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                result = json.loads(body)
                self.assertTrue(result["ok"], result)
                self.assertEqual(result["state"]["gsr"], True)
                self.assertEqual(result["state"]["tpc"], True)
                calls = without_country_learning(
                    [json.loads(line) for line in details.read_text().splitlines()])
                self.assertEqual([call["uri"] for call in calls], [
                    "com.webos.service.oledepl/getGlobalStressReduction",
                    "com.webos.service.tv.systemproperty/getProperties",
                    "com.webos.service.tv.systemproperty/setProperties",
                    "com.webos.service.tv.systemproperty/getProperties"])
                self.assertEqual(calls[2]["payload"], {"OledGSR": "true"})
                self.assertEqual(calls[1]["payload"], {
                    "keys": ["OledTPC", "OledGSR"]})
                self.assertEqual(nodeRequests[-1]["path"], "/_internal/native-state")
                process.send_signal(signal.SIGTERM)
                output, _ = process.communicate(timeout=6)
                self.assertIn(
                    "command-policy: action=control.oled-protection result=success",
                    output)
            finally:
                if process.poll() is None:
                    process.send_signal(signal.SIGTERM)
                    try:
                        process.communicate(timeout=6)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait(timeout=3)
                node.shutdown()
                node.server_close()
                nodeThread.join(timeout=3)

    def test_native_playback_controls_keep_media_and_input_contracts(self):
        binary = Path(sys.argv[1]).resolve()
        fakeLuna = binary.parent / "native-fake-luna-send"
        scenarios = [
            ("media-controls", "MSE", "playback", "playPause", 119, True, 1, 0),
            ("media-controls", "external input", "media", "playpause", 207, False, 1, 0),
            ("media-acb-absent", "", "media", "playPause", 207, False, 1, 1),
            ("home-unavailable", "", "playback", "playPause", 207, False, 1, 1),
        ]
        for (mode, playerType, action, value, expectedCode, useNativeState,
             expectedAcbReads, expectedMediaReads) in scenarios:
            publicPort = unused_port_pair()
            nodePort = unused_port_pair((publicPort, publicPort + 1))
            token = "media-control-test-token"
            secret = "media-control-proxy-secret-0123456789abcdef"
            nodeRequests = []

            class NodeCaptureHandler(BaseHTTPRequestHandler):
                def do_POST(self):
                    length = int(self.headers.get("Content-Length", "0"))
                    body = self.rfile.read(length)
                    nodeRequests.append({
                        "path": self.path,
                        "body": body.decode("utf-8", "replace"),
                        "secret": self.headers.get("X-NativeTv-Proxy-Secret"),
                    })
                    self.send_response(204)
                    self.send_header("Content-Length", "0")
                    self.end_headers()

                def log_message(self, *_args):
                    pass

            node = HTTPServer(("127.0.0.1", nodePort), NodeCaptureHandler)
            nodeThread = threading.Thread(target=node.serve_forever, daemon=True)
            nodeThread.start()

            with tempfile.TemporaryDirectory(prefix="nativetv-native-media-control-") as folder:
                root = Path(folder)
                config = root / "config.json"
                heartbeat = root / "tvweb.beat"
                assets = root / "assets"
                procRoot = root / "proc"
                systemRoot = root / "system"
                inputDir = systemRoot / "dev/input"
                details = root / "luna-details.jsonl"
                assets.mkdir()
                (procRoot / "bus/input").mkdir(parents=True)
                inputDir.mkdir(parents=True)
                (procRoot / "bus/input/devices").write_text(
                    'I: Bus=0003 Vendor=0000 Product=0000 Version=0000\n'
                    'N: Name="LGE RCU"\n'
                    'H: Handlers=kbd event7\n')
                eventPath = inputDir / "event7"
                eventPath.touch()
                config.write_text(json.dumps({
                    "host": "127.0.0.1", "port": publicPort, "token": token,
                    "allowControl": True
                }))
                environment = os.environ.copy()
                environment["TVWEB_LUNA_SEND"] = str(fakeLuna)
                environment["TVWEB_DATA_DIR"] = str(root / "data")
                environment["TVWEB_PROXY_SECRET"] = secret
                environment["NATIVETV_FAKE_LUNA_MODE"] = mode
                environment["NATIVETV_FAKE_LUNA_DETAILS"] = str(details)
                environment["NATIVETV_FAKE_LUNA_PLAYER_TYPE"] = playerType
                if useNativeState:
                    environment["TVWEB_NATIVE_STATE"] = "1"
                else:
                    environment.pop("TVWEB_NATIVE_STATE", None)
                process = subprocess.Popen(
                    [str(binary), "--config", str(config), "--heartbeat", str(heartbeat),
                     "--workers", "8", "--listen", "--node-port", str(nodePort),
                     "--assets-root", str(assets), "--proc-root", str(procRoot),
                     "--system-root", str(systemRoot)],
                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, env=environment)
                try:
                    deadline = time.monotonic() + 8
                    ready = False
                    while time.monotonic() < deadline and process.poll() is None:
                        try:
                            request = Request(
                                "http://127.0.0.1:%d/api/caps" % publicPort,
                                headers={"Authorization": "Bearer " + token})
                            response = urlopen(request, timeout=1)
                            response.read()
                            ready = True
                            break
                        except (OSError, URLError):
                            time.sleep(0.03)
                    self.assertTrue(ready, "native daemon did not start")

                    if useNativeState:
                        mediaSnapshotReady = False
                        deadline = time.monotonic() + 5
                        while time.monotonic() < deadline:
                            mediaSnapshotReady = any(
                                call["path"] == "/_internal/native-state" and
                                json.loads(call["body"]).get("type") == "state" and
                                json.loads(call["body"]).get("group") == "media" and
                                json.loads(call["body"]).get("key") == "snapshot"
                                for call in nodeRequests)
                            if mediaSnapshotReady:
                                break
                            time.sleep(0.01)
                        self.assertTrue(mediaSnapshotReady,
                                        "managed state source did not publish media snapshot")

                    requestBody = json.dumps({"action": action, "value": value}).encode("utf-8")
                    request = Request(
                        "http://127.0.0.1:%d/api/control" % publicPort,
                        data=requestBody, method="POST",
                        headers={"Content-Type": "application/json",
                                 "Authorization": "Bearer " + token})
                    response = urlopen(request, timeout=8)
                    self.assertEqual(json.loads(response.read()), {"ok": True})

                    data = eventPath.read_bytes()
                    self.assertEqual(len(data), 64,
                                     "media key sends press/sync/release/sync input events")
                    self.assertEqual(struct.unpack_from("<HHI", data, 8),
                                     (1, expectedCode, 1))
                    self.assertEqual(struct.unpack_from("<HHI", data, 24), (0, 0, 0))
                    self.assertEqual(struct.unpack_from("<HHI", data, 40),
                                     (1, expectedCode, 0))
                    self.assertEqual(struct.unpack_from("<HHI", data, 56), (0, 0, 0))

                    calls = [json.loads(line) for line in details.read_text().splitlines()]
                    acbReads = [call for call in calls
                                if call["uri"] ==
                                "com.webos.service.acb/getForegroundAppInfo"]
                    self.assertEqual(len(acbReads), expectedAcbReads,
                                     "play/pause uses native state or reads fresh ACB state")
                    if useNativeState:
                        self.assertTrue(any(call["payload"].get("subscribe") is True
                                            for call in acbReads),
                                        "managed mode should use the live ACB subscription snapshot")
                    mediaReads = [call for call in calls
                                  if call["uri"] ==
                                  "com.webos.media/getForegroundAppInfo"]
                    self.assertEqual(len(mediaReads), expectedMediaReads,
                                     "missing ACB service uses the legacy media fallback")
                    invalidations = [
                        call for call in nodeRequests
                        if call["path"] == "/_internal/native-state" and
                        json.loads(call["body"]).get("type") == "invalidate" and
                        json.loads(call["body"]).get("group") == "control"]
                    self.assertEqual(len(invalidations), 1,
                                     "successful media keys invalidate the control cache")
                    self.assertEqual(invalidations[0]["secret"], secret)

                    before = list(data)
                    invalid = Request(
                        "http://127.0.0.1:%d/api/control" % publicPort,
                        data=b'{"action":"media","value":"Pause"}', method="POST",
                        headers={"Content-Type": "application/json",
                                 "Authorization": "Bearer " + token})
                    invalidResponse = urlopen(invalid, timeout=8)
                    self.assertEqual(json.loads(invalidResponse.read()), {"ok": False})
                    self.assertEqual(eventPath.read_bytes(), bytes(before),
                                     "unknown media keys must not write to the input device")
                    afterInvalidations = [
                        call for call in nodeRequests
                        if call["path"] == "/_internal/native-state" and
                        json.loads(call["body"]).get("type") == "invalidate" and
                        json.loads(call["body"]).get("group") == "control"]
                    self.assertEqual(len(afterInvalidations), len(invalidations),
                                     "failed media keys do not invalidate native state")

                    if mode == "home-unavailable":
                        home = Request(
                            "http://127.0.0.1:%d/api/control" % publicPort,
                            data=b'{"action":"rcu","value":"home"}',
                            method="POST",
                            headers={"Content-Type": "application/json",
                                     "Authorization": "Bearer " + token})
                        homeResponse = urlopen(home, timeout=8)
                        self.assertEqual(json.loads(homeResponse.read()), {"ok": True})
                        homeEvents = eventPath.read_bytes()
                        self.assertEqual(len(homeEvents), 64)
                        self.assertEqual(struct.unpack_from("<HHI", homeEvents, 8),
                                         (1, 125, 1))
                        self.assertEqual(struct.unpack_from("<HHI", homeEvents, 40),
                                         (1, 125, 0))
                        calls = [json.loads(line) for line in details.read_text().splitlines()]
                        appLaunches = [call for call in calls
                                       if call["uri"] ==
                                       "com.webos.applicationManager/launch"]
                        self.assertEqual(appLaunches[-1]["payload"],
                                         {"id": "com.webos.app.home"})

                    process.send_signal(signal.SIGTERM)
                    output, _ = process.communicate(timeout=6)
                    self.assertIn("command-policy: action=control.media-key result=success",
                                  output)
                    if mode == "home-unavailable":
                        self.assertIn("command-policy: action=control.remote-home-key result=success",
                                      output)
                finally:
                    if process.poll() is None:
                        process.send_signal(signal.SIGTERM)
                        try:
                            process.communicate(timeout=6)
                        except subprocess.TimeoutExpired:
                            process.kill()
                            output, _ = process.communicate(timeout=3)
                            self.fail("native daemon did not stop after SIGTERM:\n" + output[-4000:])
                    node.shutdown()
                    node.server_close()
                    nodeThread.join(timeout=3)

    def test_native_remote_battery_gatt_scope_and_cache(self):
        binary = Path(sys.argv[1]).resolve()
        fakeLuna = binary.parent / "native-fake-luna-send"
        scenarios = [
            ("webOS TV 9.2.0\n", True, False, None),
            ("webOS TV 9.2.0\n", True, True, None),
            ("webOS TV 5.5.0\n", False, False, None),
            ("webOS TV 4.4.3\n", False, False, 61),
        ]
        for issue, expectGatt, failDiscovery, deviceLevel in scenarios:
            publicPort = unused_port_pair()
            nodePort = unused_port_pair((publicPort, publicPort + 1))
            with tempfile.TemporaryDirectory(prefix="nativetv-native-remote-battery-") as folder:
                root = Path(folder)
                config = root / "config.json"
                heartbeat = root / "tvweb.beat"
                assets = root / "assets"
                systemRoot = root / "system"
                details = root / "luna-details.jsonl"
                assets.mkdir()
                (systemRoot / "etc").mkdir(parents=True)
                (systemRoot / "etc/issue").write_text(issue)
                config.write_text(json.dumps({
                    "host": "127.0.0.1", "port": publicPort, "token": "remote-battery-test"
                }))
                environment = os.environ.copy()
                environment["TVWEB_LUNA_SEND"] = str(fakeLuna)
                environment["TVWEB_DATA_DIR"] = str(root / "data")
                environment["TVWEB_PROXY_SECRET"] = "remote-battery-proxy-secret-0123456789abcdef"
                environment["TVWEB_NATIVE_STATE"] = "1"
                environment["NATIVETV_FAKE_LUNA_MODE"] = "remote-battery"
                environment["NATIVETV_FAKE_LUNA_DETAILS"] = str(details)
                environment["NATIVETV_FAKE_LUNA_DISCOVER_FAIL"] = "1" if failDiscovery else "0"
                if deviceLevel is not None:
                    environment["NATIVETV_FAKE_LUNA_BATTERY_LEVEL"] = str(deviceLevel)
                else:
                    environment.pop("NATIVETV_FAKE_LUNA_BATTERY_LEVEL", None)
                process = subprocess.Popen(
                    [str(binary), "--config", str(config), "--heartbeat", str(heartbeat),
                     "--workers", "8", "--listen", "--node-port", str(nodePort),
                     "--assets-root", str(assets), "--system-root", str(systemRoot)],
                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, env=environment)
                try:
                    deadline = time.monotonic() + 12
                    ready = False
                    while time.monotonic() < deadline and process.poll() is None:
                        try:
                            response = urlopen("http://127.0.0.1:%d/api/caps" % publicPort,
                                               timeout=1)
                            response.read()
                            ready = True
                            break
                        except (OSError, URLError):
                            time.sleep(0.03)
                    self.assertTrue(ready, "native daemon did not start")

                    statePath = root / "data/remote-battery.json"
                    deadline = time.monotonic() + 12
                    while time.monotonic() < deadline:
                        records = ([json.loads(line) for line in details.read_text().splitlines()]
                                   if details.exists() else [])
                        deviceReads = sum(record["uri"] ==
                                          "com.webos.service.mrcu/getDeviceInfo"
                                          for record in records)
                        if expectGatt and failDiscovery:
                            if any(json.loads(line)["uri"].endswith("/gatt/disconnect")
                                   for line in details.read_text().splitlines()) if details.exists() else False:
                                break
                        elif statePath.exists() and (expectGatt or deviceLevel is not None):
                            saved = json.loads(statePath.read_text())
                            if saved.get("level") == (deviceLevel if deviceLevel is not None else 77):
                                break
                        elif not expectGatt and deviceReads >= 2:
                            break
                        time.sleep(0.05)
                    self.assertTrue(statePath.exists(), "native battery cache was not created")
                    saved = json.loads(statePath.read_text())
                    records = ([json.loads(line) for line in details.read_text().splitlines()]
                               if details.exists() else [])
                    calls = [record["uri"] for record in records]
                    output = ""
                    if process.poll() is None:
                        process.send_signal(signal.SIGTERM)
                        try:
                            output, _ = process.communicate(timeout=6)
                        except subprocess.TimeoutExpired:
                            process.kill()
                            output, _ = process.communicate(timeout=3)
                    self.assertEqual(any(uri.endswith("/gatt/connect") for uri in calls), expectGatt,
                                     "unexpected GATT path for %s: %r; cache=%r" %
                                     (issue.strip(), calls, saved) + "\n" + output[-4000:])
                    if expectGatt:
                        self.assertEqual(saved.get("level"), None if failDiscovery else 77,
                                         "battery result did not match the GATT scenario; cache=%r calls=%r" %
                                         (saved, calls))
                        self.assertEqual(saved.get("mac"),
                                         None if failDiscovery else "00:00:5e:00:53:02")
                        self.assertFalse(saved.get("lowStrength"))
                        self.assertIn("com.webos.service.bluetooth2/gatt/disconnect", calls)
                    elif deviceLevel is not None:
                        self.assertEqual(saved.get("level"), deviceLevel)
                        self.assertEqual(saved.get("mac"), "00:00:5e:00:53:02")
                        self.assertNotIn("com.webos.service.bluetooth2/gatt/connect", calls)
                    else:
                        self.assertNotIn("com.webos.service.bluetooth2/gatt/readCharacteristicValue",
                                         calls)
                finally:
                    if process.poll() is None:
                        process.send_signal(signal.SIGTERM)
                        try:
                            process.communicate(timeout=6)
                        except subprocess.TimeoutExpired:
                            process.kill()
                            output, _ = process.communicate(timeout=3)
                            self.fail("native daemon did not stop after SIGTERM:\n" + output[-4000:])

    def test_native_power_controls_respect_allow_power(self):
        binary = Path(sys.argv[1]).resolve()
        fakeLuna = binary.parent / "native-fake-luna-send"
        publicPort = unused_port_pair()
        nodePort = unused_port_pair((publicPort, publicPort + 1))
        token = "power-policy-test-token"
        secret = "power-policy-proxy-secret-0123456789abcdef"
        forwarded = []

        class NodeCaptureHandler(BaseHTTPRequestHandler):
            def do_POST(self):
                length = int(self.headers.get("Content-Length", "0"))
                body = self.rfile.read(length)
                forwarded.append({"path": self.path, "body": body.decode("utf-8")})
                response = b'{"ok":true,"forwarded":true}'
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(response)))
                self.end_headers()
                self.wfile.write(response)

            def log_message(self, *_args):
                pass

        node = HTTPServer(("127.0.0.1", nodePort), NodeCaptureHandler)
        nodeThread = threading.Thread(target=node.serve_forever, daemon=True)
        nodeThread.start()

        with tempfile.TemporaryDirectory(prefix="nativetv-native-power-policy-") as folder:
            root = Path(folder)
            config = root / "config.json"
            details = root / "luna-details.jsonl"
            heartbeat = root / "tvweb.beat"
            assets = root / "assets"
            assets.mkdir()
            config.write_text(json.dumps({
                "host": "127.0.0.1", "port": publicPort, "token": token,
                "allowControl": True, "allowPower": False
            }))
            environment = os.environ.copy()
            for key in ("TVWEB_NATIVE_STATE", "TVWEB_NATIVE_HISTORY",
                        "TVWEB_NATIVE_SCREENTIME"):
                environment.pop(key, None)
            environment["TVWEB_LUNA_SEND"] = str(fakeLuna)
            environment["TVWEB_DATA_DIR"] = str(root)
            environment["TVWEB_PROXY_SECRET"] = secret
            environment["NATIVETV_FAKE_LUNA_MODE"] = "quick-access"
            environment["NATIVETV_FAKE_LUNA_DETAILS"] = str(details)
            process = subprocess.Popen(
                [str(binary), "--config", str(config), "--heartbeat", str(heartbeat),
                 "--workers", "4", "--listen", "--node-port", str(nodePort),
                 "--assets-root", str(assets)],
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, env=environment)
            try:
                deadline = time.monotonic() + 8
                ready = False
                while time.monotonic() < deadline and process.poll() is None:
                    try:
                        request = Request(
                            "http://127.0.0.1:%d/api/caps" % publicPort,
                            headers={"Authorization": "Bearer " + token})
                        response = urlopen(request, timeout=1)
                        response.read()
                        ready = True
                        break
                    except (OSError, URLError):
                        time.sleep(0.03)
                if not ready:
                    self.fail("native daemon did not start")

                for action in ("powerOff", "powerOn"):
                    body = json.dumps({"action": action}).encode("utf-8")
                    request = Request(
                        "http://127.0.0.1:%d/api/control" % publicPort,
                        data=body, method="POST",
                        headers={"Content-Type": "application/json",
                                 "Authorization": "Bearer " + token})
                    with urlopen(request, timeout=5) as response:
                        result = json.loads(response.read().decode("utf-8"))
                    self.assertEqual(result, {
                        "ok": False, "error": "power actions disabled (set allowPower)"})
                    self.assertEqual(forwarded, [],
                                     "the refusal is the daemon's, not Node's")
                calls = ([json.loads(line) for line in details.read_text().splitlines()]
                         if details.exists() else [])
                self.assertFalse(any(call["uri"].endswith("/powerOff") for call in calls),
                                 "allowPower=false must block native power writes")
            finally:
                if process.poll() is None:
                    process.send_signal(signal.SIGTERM)
                    try:
                        process.communicate(timeout=6)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        output, _ = process.communicate(timeout=3)
                        self.fail("native daemon did not stop after SIGTERM:\n" + output[-4000:])
                node.shutdown()
                node.server_close()
                nodeThread.join(timeout=3)

    def test_native_remote_key_dismisses_nativetv_screen_saver(self):
        binary = Path(sys.argv[1]).resolve()
        fakeLuna = binary.parent / "native-fake-luna-send"
        self.assertTrue(fakeLuna.is_file(), "fake Luna executable was not built")
        publicPort = unused_port_pair()
        secret = "native-screen-saver-key-test-secret-0123456789abcdef"

        with tempfile.TemporaryDirectory(prefix="nativetv-native-saver-key-") as folder:
            root = Path(folder)
            inputDirectory = root / "input"
            runtimeDirectory = root / "var" / "run"
            inputDirectory.mkdir()
            runtimeDirectory.mkdir(parents=True)
            marker = runtimeDirectory / "tvweb-screensaver-launched"
            marker.write_text("1\n")
            (inputDirectory / "event1").write_bytes(
                struct.pack("<qqHHi", 0, 0, 1, 103, 1))
            config = root / "config.json"
            config.write_text(json.dumps({"host": "127.0.0.1", "port": publicPort}))
            heartbeat = root / "tvweb.beat"
            details = root / "luna-details.jsonl"
            environment = os.environ.copy()
            environment["TVWEB_PROXY_SECRET"] = secret
            environment["TVWEB_DATA_DIR"] = str(root)
            environment["TVWEB_LUNA_SEND"] = str(fakeLuna)
            environment["TVWEB_NATIVE_STATE"] = "1"
            environment["TVWEB_NATIVE_REMOTE_INPUT"] = "1"
            environment["TVWEB_REMOTE_INPUT_DIRECTORY"] = str(inputDirectory)
            environment["TVWEB_REMOTE_INPUT_DEVICES"] = str(root / "input-devices")
            environment["TVWEB_REMOTE_INPUT_LOG"] = ""
            environment["NATIVETV_FAKE_LUNA_MODE"] = "state-groups"
            environment["NATIVETV_FAKE_LUNA_DETAILS"] = str(details)
            process = subprocess.Popen(
                [str(binary), "--config", str(config), "--heartbeat", str(heartbeat),
                 "--workers", "12", "--listen", "--node-port",
                 str(unused_port_pair((publicPort, publicPort + 1))),
                 "--assets-root", str(root), "--system-root", str(root)],
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, env=environment)
            try:
                deadline = time.monotonic() + 8
                ready = False
                while time.monotonic() < deadline and process.poll() is None:
                    try:
                        response = urlopen(
                            "http://127.0.0.1:%d/api/caps" % publicPort, timeout=1)
                        response.read()
                        ready = True
                        break
                    except (OSError, URLError):
                        time.sleep(0.03)
                self.assertTrue(ready, "native daemon did not start")

                deadline = time.monotonic() + 8
                calls = []
                while time.monotonic() < deadline and process.poll() is None:
                    if details.exists():
                        calls = [json.loads(line) for line in details.read_text().splitlines()]
                    if any(call["uri"] ==
                           "com.webos.applicationManager/closeByAppId" for call in calls):
                        break
                    time.sleep(0.03)
                self.assertTrue(any(
                    call["uri"] == "com.webos.service.tvpower/power/getPowerState"
                    for call in calls), "native remote input did not check tvpower state")
                self.assertTrue(any(
                    call["uri"] == "com.webos.applicationManager/running"
                    for call in calls), "native remote input did not confirm the saver app")
                self.assertTrue(any(
                    call["uri"] == "com.webos.applicationManager/closeByAppId" and
                    call["payload"].get("id") == "com.webos.app.screensaver"
                    for call in calls), "native remote input did not close the Native Tv saver")
                self.assertTrue(marker.is_file(),
                                "the existing Node screen-saver lifecycle owns the marker")
            finally:
                if process.poll() is None:
                    process.send_signal(signal.SIGTERM)
                    try:
                        output, _ = process.communicate(timeout=6)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        output, _ = process.communicate(timeout=3)
                        self.fail("native daemon did not stop:\n" + output[-4000:])
                    self.assertEqual(process.returncode, 0, output)

    def test_native_state_bridge_retries_until_node_is_ready(self):
        binary = Path(sys.argv[1]).resolve()
        fakeLuna = binary.parent / "native-fake-luna-send"
        self.assertTrue(fakeLuna.is_file(), "fake Luna executable was not built")
        publicPort = unused_port_pair()
        nodePort = unused_port_pair((publicPort, publicPort + 1))
        secret = "native-state-bridge-test-secret-0123456789abcdef"
        received = []
        receivedLock = threading.Lock()

        class StateCaptureHandler(BaseHTTPRequestHandler):
            def do_POST(self):
                length = int(self.headers.get("Content-Length", "0"))
                try:
                    body = self.rfile.read(length).decode("utf-8")
                    event = json.loads(body) if body else {}
                except (ValueError, UnicodeDecodeError):
                    self.send_error(400)
                    return
                with receivedLock:
                    received.append({
                        "event": event,
                        "secret": self.headers.get("X-NativeTv-Proxy-Secret"),
                        "peer": self.headers.get("X-NativeTv-Original-Remote"),
                        "path": self.path,
                    })
                if self.path == "/_internal/native-history":
                    body = json.dumps({"enabled": True, "running": True,
                                       "readPersisted": True, "clockSet": True,
                                       "hours": {}, "titles": {}}).encode("utf-8")
                    self.send_response(200)
                    self.send_header("Content-Type", "application/json")
                    self.send_header("Content-Length", str(len(body)))
                    self.end_headers()
                    self.wfile.write(body)
                    return
                if self.path == "/_internal/reload-config":
                    body = json.dumps({"ok": True}).encode("utf-8")
                    self.send_response(200)
                    self.send_header("Content-Type", "application/json")
                    self.send_header("Content-Length", str(len(body)))
                    self.end_headers()
                    self.wfile.write(body)
                    return
                if self.path == "/_internal/home-escape":
                    body = json.dumps({"ok": True}).encode("utf-8")
                    self.send_response(200)
                    self.send_header("Content-Type", "application/json")
                    self.send_header("Content-Length", str(len(body)))
                    self.end_headers()
                    self.wfile.write(body)
                    return
                self.send_response(204)
                self.end_headers()

            def log_message(self, *_args):
                pass

        node = HTTPServer(("127.0.0.1", nodePort), StateCaptureHandler)
        nodeThread = None
        process = None
        with tempfile.TemporaryDirectory(prefix="nativetv-native-state-bridge-") as folder:
            root = Path(folder)
            config = root / "config.json"
            homeConfig = root / "home.json"
            heartbeat = root / "tvweb.beat"
            lunaDetails = root / "luna-details.jsonl"
            foregroundFile = root / "foreground.json"
            assets = root / "assets"
            inputDirectory = root / "input"
            assets.mkdir()
            inputDirectory.mkdir()
            inputDevice = inputDirectory / "event1"
            inputRecords = b"".join(
                struct.pack("<qqHHi", 0, 0, 1, code, value)
                for code, value in ((773, 1), (398, 1), (103, 1)))
            inputDevice.write_bytes(inputRecords)
            homeConfig.write_text(json.dumps({"enabled": False}))
            foregroundFile.write_text(json.dumps({"appId": "com.webos.app.netflix"}))
            config.write_text(json.dumps({
                "host": "127.0.0.1", "port": publicPort,
                "mqtt": {"enabled": True, "host": "broker.local"},
                "history": {"enabled": True},
                "screenTime": {"enabled": True, "dailyMinutes": 0,
                               "bedtimeStart": "", "bedtimeEnd": "",
                               "action": "off", "appAction": "home", "apps": {}}
            }))
            environment = os.environ.copy()
            environment["TVWEB_PROXY_SECRET"] = secret
            environment["TVWEB_DATA_DIR"] = str(root)
            environment["TVWEB_LUNA_SEND"] = str(fakeLuna)
            environment["TVWEB_NATIVE_STATE"] = "1"
            environment["TVWEB_NATIVE_HISTORY"] = "1"
            environment["TVWEB_NATIVE_SCREENTIME"] = "1"
            environment["TVWEB_NATIVE_REMOTE_INPUT"] = "1"
            environment["TVWEB_REMOTE_INPUT_DIRECTORY"] = str(inputDirectory)
            environment["TVWEB_REMOTE_INPUT_DEVICES"] = str(root / "input-devices")
            environment["TVWEB_REMOTE_INPUT_LOG"] = ""
            environment["NATIVETV_FAKE_LUNA_DETAILS"] = str(lunaDetails)
            environment["NATIVETV_FAKE_LUNA_MODE"] = "state-groups"
            environment["NATIVETV_FAKE_LUNA_FOREGROUND_FILE"] = str(foregroundFile)
            process = subprocess.Popen(
                [str(binary), "--config", str(config), "--heartbeat", str(heartbeat),
                 "--workers", "12", "--listen", "--node-port", str(nodePort),
                 "--assets-root", str(assets)],
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, env=environment)
            try:
                deadline = time.monotonic() + 8
                ready = False
                while time.monotonic() < deadline and process.poll() is None:
                    try:
                        response = urlopen("http://127.0.0.1:%d/api/caps" % publicPort, timeout=1)
                        response.read()
                        ready = True
                        break
                    except (OSError, URLError):
                        time.sleep(0.03)
                self.assertTrue(ready, "native daemon did not start before Node was available")

                # Let the daemon receive Luna snapshots while its Node peer is
                # absent. The bridge must retain them and retry after Node starts.
                time.sleep(0.7)
                nodeThread = threading.Thread(target=node.serve_forever, daemon=True)
                nodeThread.start()
                time.sleep(1.3)
                lunaCalls = ([json.loads(line) for line in lunaDetails.read_text().splitlines()]
                             if lunaDetails.exists() else [])
                self.assertFalse(any(
                    call["uri"] == "com.webos.applicationManager/launch" and
                    call["payload"].get("id") == "com.webos.app.home"
                    for call in lunaCalls),
                    "holding Home must not open LG Home while Native Tv Home is disabled")
                with inputDevice.open("ab") as device:
                    device.write(struct.pack("<qqHHi", 0, 0, 1, 773, 0))
                homeConfig.write_text(json.dumps({"enabled": True}))
                with inputDevice.open("ab") as device:
                    device.write(struct.pack("<qqHHi", 0, 0, 1, 773, 1))
                deadline = time.monotonic() + 10
                firstHomeReleaseSent = False
                foregroundAwaySent = False
                secondHomeHoldSent = False
                shortPressSent = False
                finalForegroundSent = False
                while time.monotonic() < deadline and process.poll() is None:
                    with receivedLock:
                        events = [item["event"] for item in received]
                    lunaCalls = ([json.loads(line) for line in lunaDetails.read_text().splitlines()]
                                 if lunaDetails.exists() else [])
                    homeLaunches = [
                        call["uri"] == "com.webos.applicationManager/launch" and
                        call["payload"].get("id") == "com.webos.app.home"
                        for call in lunaCalls]
                    homeLaunchCount = sum(homeLaunches)
                    homeLaunched = homeLaunchCount > 0
                    if not firstHomeReleaseSent and homeLaunched:
                        foregroundFile.write_text(json.dumps({"appId": "com.webos.app.home"}))
                        with inputDevice.open("ab") as device:
                            device.write(struct.pack("<qqHHi", 0, 0, 1, 773, 0))
                        firstHomeReleaseSent = True
                    hasApplication = any(
                        event.get("type") == "state" and event.get("group") == "application" and
                        event.get("key") == "app_id" and
                        event.get("value") == "com.webos.app.netflix" for event in events)
                    hasHomeApplication = any(
                        event.get("type") == "state" and event.get("group") == "application" and
                        event.get("key") == "app_id" and
                        event.get("value") == "com.webos.app.home" for event in events)
                    hasApplicationLive = any(
                        event.get("type") == "status" and event.get("group") == "application" and
                        event.get("live") is True for event in events)
                    hasWindowState = all(any(
                        event.get("type") == "state" and event.get("group") == group and
                        event.get("key") == "control_app_id" for event in events)
                        for group in ("surface", "focus"))
                    hasMediaState = any(
                        event.get("type") == "state" and event.get("group") == "media" and
                        event.get("key") == "snapshot" and
                        json.loads(event.get("value", "{}")).get("state") == "playing"
                        for event in events)
                    remoteInputEvents = [event for event in events
                                         if event.get("type") == "remote-input"]
                    hasRemoteInput = all(any(
                        event.get("event") == expectedEvent and
                        event.get("code") == expectedCode and
                        event.get("pressed") == expectedPressed
                        for event in remoteInputEvents)
                        for expectedEvent, expectedCode, expectedPressed in (
                            ("home", 773, True), ("home", 773, False),
                            ("button", 398, True)))
                    hasHomeShortPress = any(
                        event.get("event") == "home-short" and
                        event.get("code") == 773 and event.get("pressed") is True
                        for event in remoteInputEvents)
                    if (firstHomeReleaseSent and hasHomeApplication and
                            not foregroundAwaySent):
                        foregroundFile.write_text(json.dumps({"appId": "com.webos.app.netflix"}))
                        foregroundAwaySent = True
                    with receivedLock:
                        captured = list(received)
                    hasForegroundFallback = any(
                        item["path"] == "/_internal/home-escape" and
                        item["event"] == {
                            "foregroundFallback": True,
                            "appId": "com.webos.app.netflix"}
                        for item in captured)
                    if hasForegroundFallback and not secondHomeHoldSent:
                        with inputDevice.open("ab") as device:
                            device.write(struct.pack("<qqHHi", 0, 0, 1, 773, 1))
                        time.sleep(1.35)
                        with inputDevice.open("ab") as device:
                            device.write(struct.pack("<qqHHi", 0, 0, 1, 773, 0))
                        secondHomeHoldSent = True
                    if homeLaunchCount >= 2 and secondHomeHoldSent and not shortPressSent:
                        foregroundFile.write_text(json.dumps({"appId": "com.webos.app.home"}))
                        with inputDevice.open("ab") as device:
                            device.write(struct.pack("<qqHHi", 0, 0, 1, 773, 1))
                        time.sleep(0.05)
                        with inputDevice.open("ab") as device:
                            device.write(struct.pack("<qqHHi", 0, 0, 1, 773, 0))
                        shortPressSent = True
                    if hasHomeShortPress and not finalForegroundSent:
                        foregroundFile.write_text(json.dumps({"appId": "com.webos.app.netflix"}))
                        finalForegroundSent = True
                    applicationValues = [
                        item["event"].get("value") for item in captured
                        if item["event"].get("type") == "state" and
                        item["event"].get("group") == "application" and
                        item["event"].get("key") == "app_id"]
                    distinctApplicationValues = []
                    for value in applicationValues:
                        if not distinctApplicationValues or distinctApplicationValues[-1] != value:
                            distinctApplicationValues.append(value)
                    hasFinalNetflixForeground = (
                        len(distinctApplicationValues) >= 3 and
                        distinctApplicationValues[-2:] == [
                            "com.webos.app.home", "com.webos.app.netflix"])
                    if (hasApplication and hasApplicationLive and hasWindowState and
                            hasMediaState and hasRemoteInput and homeLaunched and
                            homeLaunchCount >= 2 and hasHomeShortPress and
                            hasForegroundFallback and hasFinalNetflixForeground):
                        break
                    time.sleep(0.03)
                with receivedLock:
                    captured = list(received)
                self.assertTrue(any(
                    item["event"].get("type") == "state" and
                    item["event"].get("group") == "application" and
                    item["event"].get("key") == "app_id" for item in captured),
                    "native application state was not retried to Node")
                self.assertTrue(any(
                    item["event"].get("type") == "status" and
                    item["event"].get("group") == "application" and
                    item["event"].get("live") is True for item in captured),
                    "native subscription liveness was not delivered to Node")
                self.assertTrue(all(any(
                    item["event"].get("type") == "state" and
                    item["event"].get("group") == group and
                    item["event"].get("key") == "control_app_id" for item in captured)
                    for group in ("surface", "focus")),
                    "configured MQTT window groups were not started and bridged")
                self.assertTrue(any(
                    item["event"].get("type") == "state" and
                    item["event"].get("group") == "media" and
                    json.loads(item["event"].get("value", "{}")).get("state") == "playing"
                    for item in captured),
                    "native play state was not sent over the event bridge")
                for expectedEvent, expectedCode, expectedPressed in (
                        ("home", 773, True), ("home", 773, False),
                        ("button", 398, True)):
                    self.assertTrue(any(
                        item["event"].get("type") == "remote-input" and
                        item["event"].get("event") == expectedEvent and
                        item["event"].get("code") == expectedCode and
                        item["event"].get("pressed") == expectedPressed
                        for item in captured),
                        "native remote input was not bridged: %r" %
                        ((expectedEvent, expectedCode, expectedPressed),))
                self.assertFalse(any(
                    item["event"].get("type") == "remote-input" and
                    item["event"].get("event") == "home-held"
                    for item in captured),
                    "native Home hold actions should no longer be delegated to Node")
                self.assertTrue(any(
                    item["event"].get("type") == "remote-input" and
                    item["event"].get("event") == "home-short" and
                    item["event"].get("code") == 773 and
                    item["event"].get("pressed") is True
                    for item in captured),
                    "a rejected native short-press return should use the Node recovery path: %r" %
                    [item["event"] for item in captured
                     if item["event"].get("type") == "remote-input"])
                self.assertTrue(any(
                    item["path"] == "/_internal/home-escape" and
                    item["event"] == {"escaped": True}
                    for item in captured),
                    "native Home hold should update the managed Node escape observer")
                self.assertTrue(any(
                    call["uri"] == "com.webos.applicationManager/launch" and
                    call["payload"].get("id") == "com.webos.app.home"
                    for call in lunaCalls),
                    "native Home hold should launch LG Home")
                self.assertFalse(any(
                    item["event"].get("type") == "remote-input" and
                    item["event"].get("event") == "key"
                    for item in captured),
                    "generic remote keys should be handled by the native screen-saver consumer")
                self.assertTrue(all(item["path"] in ("/_internal/native-state",
                                                       "/_internal/home-escape") and
                                    item["secret"] == secret and item["peer"] is None
                                    for item in captured),
                                "native bridges must use private authenticated Node endpoints")
                # The fixture checks that the live segment is rounded to a
                # persisted tenth of a second, so leave it time to accumulate.
                time.sleep(0.2)
                response = urlopen("http://127.0.0.1:%d/api/history?days=1" % publicPort,
                                   timeout=5)
                history = json.loads(response.read().decode("utf-8"))
                self.assertTrue(history["enabled"])
                applicationEvents = [
                    item["event"].get("value") for item in captured
                    if item["event"].get("type") == "state" and
                    item["event"].get("group") == "application" and
                    item["event"].get("key") == "app_id"]
                self.assertEqual(history["current"], "netflix",
                                 "current app history should follow foreground changes: %r" %
                                 applicationEvents)
                self.assertTrue(any("netflix" in hour for hour in history["hours"].values()),
                                "native history queries must include the daemon's live segment")

                screenTime = json.loads(urlopen(
                    "http://127.0.0.1:%d/api/screentime" % publicPort,
                    timeout=5).read().decode("utf-8"))
                self.assertTrue(screenTime["ok"])
                self.assertTrue(screenTime["historyOn"])
                self.assertEqual(screenTime["settings"]["dailyMinutes"], 0)
                self.assertTrue(any(choice["key"] == "netflix"
                                    for choice in screenTime["choices"]),
                                "native screen-time choices should use the native history query")
                with receivedLock:
                    screenTimeBridgeCalls = sum(
                        1 for item in received
                        if item["path"] == "/_internal/reload-config")
                self.assertEqual(screenTimeBridgeCalls, 0,
                                 "native screen-time status should not use the Node policy engine")

                request = Request(
                    "http://127.0.0.1:%d/api/screentime" % publicPort,
                    data=b'{"dailyMinutes":45}',
                    headers={"Content-Type": "application/json"}, method="POST")
                with urlopen(request, timeout=5) as response:
                    updatedScreenTime = json.loads(response.read().decode("utf-8"))
                self.assertEqual(updatedScreenTime["settings"]["dailyMinutes"], 45)
                savedConfig = json.loads(config.read_text())
                self.assertEqual(savedConfig["screenTime"]["dailyMinutes"], 45,
                                 "native screen-time updates should persist directly to config.json")
                self.assertEqual(savedConfig["mqtt"]["host"], "broker.local",
                                 "screen-time writes must preserve unrelated config sections")
                with receivedLock:
                    screenTimeBridgeCalls = sum(
                        1 for item in received
                        if item["path"] == "/_internal/reload-config")
                self.assertEqual(screenTimeBridgeCalls, 1,
                                 "native screen-time writes should refresh Node's in-memory config; requests: %r" %
                                 [item["path"] for item in received])

                request = Request(
                    "http://127.0.0.1:%d/api/screentime" % publicPort,
                    data=b'{"dailyMinutes":60}',
                    headers={"Content-Type": "text/plain"}, method="POST")
                with self.assertRaises(HTTPError) as wrongScreenTimeType:
                    urlopen(request, timeout=5)
                self.assertEqual(wrongScreenTimeType.exception.code, 415,
                                 "screen-time writes must require application/json")
                self.assertEqual(json.loads(config.read_text())["screenTime"]["dailyMinutes"], 45,
                                 "wrong-content-type screen-time requests must not change config.json")

                request = Request(
                    "http://127.0.0.1:%d/api/screentime" % publicPort,
                    data=b'{"unknownSetting":true}',
                    headers={"Content-Type": "application/json"}, method="POST")
                with self.assertRaises(HTTPError) as invalidScreenTime:
                    urlopen(request, timeout=5)
                self.assertEqual(invalidScreenTime.exception.code, 400,
                                 "unknown screen-time settings must be rejected")
                self.assertEqual(json.loads(config.read_text())["screenTime"]["dailyMinutes"], 45,
                                 "invalid screen-time updates must leave config.json unchanged")

                request = Request(
                    "http://127.0.0.1:%d/api/screentime" % publicPort,
                    data=b'{"dailyMinutes":60}',
                    headers={"Content-Type": "application/json",
                             "Origin": "http://attacker.example"}, method="POST")
                with self.assertRaises(HTTPError) as crossSiteScreenTime:
                    urlopen(request, timeout=5)
                self.assertEqual(crossSiteScreenTime.exception.code, 403,
                                 "screen-time writes must reject cross-origin requests")
                self.assertEqual(json.loads(config.read_text())["screenTime"]["dailyMinutes"], 45,
                                 "cross-origin screen-time requests must not change config.json")
                with receivedLock:
                    self.assertEqual(sum(
                        1 for item in received
                        if item["path"] == "/_internal/reload-config"), 1,
                        "refused screen-time writes must not reload Node config")

                request = Request(
                    "http://127.0.0.1:%d/api/screentime" % publicPort,
                    data=b'{"bedtimeStart":"22:30"}',
                    headers={"Content-Type": "application/json"}, method="POST")
                with urlopen(request, timeout=5) as response:
                    partialScreenTime = json.loads(response.read().decode("utf-8"))
                self.assertEqual(partialScreenTime["settings"]["dailyMinutes"], 45,
                                 "partial screen-time updates must retain saved fields")
                self.assertEqual(partialScreenTime["settings"]["bedtimeStart"], "22:30")
                savedConfig = json.loads(config.read_text())
                self.assertEqual(savedConfig["screenTime"]["dailyMinutes"], 45)
                self.assertEqual(savedConfig["screenTime"]["bedtimeStart"], "22:30")
                with receivedLock:
                    self.assertEqual(sum(
                        1 for item in received
                        if item["path"] == "/_internal/reload-config"), 2,
                        "each accepted screen-time patch should reload Node config once")

                updatedScreenTime = json.loads(urlopen(
                    "http://127.0.0.1:%d/api/screentime" % publicPort,
                    timeout=5).read().decode("utf-8"))
                self.assertEqual(updatedScreenTime["settings"]["dailyMinutes"], 45,
                                 "native status should read the settings returned by the apply bridge")
                self.assertEqual(updatedScreenTime["settings"]["bedtimeStart"], "22:30")
                with receivedLock:
                    self.assertEqual(sum(
                        1 for item in received
                        if item["path"] == "/_internal/reload-config"), 2,
                        "subsequent native status reads should not trigger config reloads")

                request = Request(
                    "http://127.0.0.1:%d/api/history" % publicPort,
                    data=b'{"action":"enabled","enabled":false}',
                    headers={"Content-Type": "application/json",
                             "Origin": "http://attacker.example"}, method="POST")
                with self.assertRaises(HTTPError) as denied:
                    urlopen(request, timeout=5)
                self.assertEqual(denied.exception.code, 403,
                                 "native history controls must reject cross-origin requests")

                request = Request(
                    "http://127.0.0.1:%d/api/history" % publicPort,
                    data=json.dumps({"action": "enabled", "enabled": False}).encode("utf-8"),
                    headers={"Content-Type": "application/json"}, method="POST")
                with urlopen(request, timeout=5) as response:
                    historyStatus = json.loads(response.read().decode("utf-8"))
                self.assertTrue(historyStatus["ok"])
                self.assertFalse(historyStatus["running"],
                                 "the native recorder control must stop the active segment")
                request = Request(
                    "http://127.0.0.1:%d/api/history" % publicPort,
                    data=b'{"action":"clear"}',
                    headers={"Content-Type": "application/json"}, method="POST")
                with urlopen(request, timeout=5) as response:
                    cleared = json.loads(response.read().decode("utf-8"))
                self.assertTrue(cleared["ok"] and not cleared["kept"],
                                "native Clear must remove its durable history files")
            finally:
                if process.poll() is None:
                    process.send_signal(signal.SIGTERM)
                    try:
                        output, _ = process.communicate(timeout=6)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        output, _ = process.communicate(timeout=3)
                        self.fail("native daemon did not stop after SIGTERM:\n" + output[-4000:])
                    self.assertIn("screentime: limits on", output,
                                  "managed startup should start the native policy runtime")
                node.shutdown()
                node.server_close()
                if nodeThread:
                    nodeThread.join(timeout=3)

    def test_supervised_daemon_exits_with_its_parent(self):
        binary = Path(sys.argv[1]).resolve()
        with tempfile.TemporaryDirectory(prefix="nativetv-supervised-daemon-") as folder:
            root = Path(folder)
            config = root / "config.json"
            heartbeat = root / "tvweb.beat"
            pidfile = root / "daemon.pid"
            config.write_text(json.dumps({"port": 8177}))
            launch = (
                "import os, subprocess, sys, time\n"
                "env = os.environ.copy()\n"
                "env['TVWEB_SUPERVISED'] = '1'\n"
                "child = subprocess.Popen([sys.argv[1], '--config', sys.argv[2], "
                "'--heartbeat', sys.argv[3], '--workers', '2'], env=env, "
                "stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)\n"
                "deadline = time.monotonic() + 4\n"
                "while time.monotonic() < deadline and not os.path.exists(sys.argv[3]):\n"
                "    time.sleep(0.02)\n"
                "if not os.path.exists(sys.argv[3]):\n"
                "    child.kill()\n"
                "    sys.exit('daemon did not initialize before supervisor exit')\n"
                "open(sys.argv[4], 'w').write(str(child.pid))\n"
            )
            parent = subprocess.run([sys.executable, "-c", launch, str(binary),
                                     str(config), str(heartbeat), str(pidfile)],
                                    capture_output=True, text=True, timeout=5)
            self.assertEqual(parent.returncode, 0, parent.stderr)
            self.assertTrue(pidfile.is_file(), "supervisor did not record the daemon PID")
            daemonPid = int(pidfile.read_text())

            def daemonIsRunning():
                try:
                    state = subprocess.check_output(
                        ["ps", "-p", str(daemonPid), "-o", "stat=,command="],
                        stderr=subprocess.DEVNULL, text=True).strip()
                except subprocess.CalledProcessError:
                    return False
                fields = state.split(None, 1)
                return (len(fields) == 2 and not fields[0].startswith("Z") and
                        "nativetv-daemon" in fields[1])

            deadline = time.monotonic() + 5
            while time.monotonic() < deadline and daemonIsRunning():
                time.sleep(0.05)
            if daemonIsRunning():
                try:
                    os.kill(daemonPid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
                self.fail("supervised daemon stayed alive after its parent exited")

    def test_setup_always_ready_calls_luna(self):
        binary = Path(sys.argv[1]).resolve()
        with tempfile.TemporaryDirectory(prefix="nativetv-setup-luna-") as folder:
            root = Path(folder)
            config = root / "config.json"
            heartbeat = root / "tvweb.beat"
            assets = root / "assets"
            assets.mkdir()
            port = unused_port_pair()
            upstreamPort = unused_port_pair((port, port + 1))

            capture = root / "luna-call.json"
            fakeLuna = root / "fake-luna-send"
            fakeLuna.write_text(
                "#!/usr/bin/env python3\n"
                "import json, os, sys\n"
                "with open(os.environ['TVWEB_LUNA_CAPTURE'], 'a') as output:\n"
                "    output.write(json.dumps(sys.argv[1:]) + '\\n')\n"
                "print('{\"returnValue\":true}')\n")
            fakeLuna.chmod(0o700)
            config.write_text(json.dumps({"host": "127.0.0.1", "port": port,
                                          "allowControl": True}))
            environment = os.environ.copy()
            environment["TVWEB_PROXY_SECRET"] = "runtime-proxy-secret-for-tests-0123456789abcdef"
            environment["TVWEB_DATA_DIR"] = str(root)
            environment["TVWEB_LUNA_SEND"] = str(fakeLuna)
            environment["TVWEB_LUNA_CAPTURE"] = str(capture)
            process = subprocess.Popen(
                [str(binary), "--config", str(config), "--heartbeat", str(heartbeat),
                 "--workers", "2", "--listen", "--node-port", str(upstreamPort),
                 "--assets-root", str(assets)],
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, env=environment)
            try:
                deadline = time.monotonic() + 5
                response = None
                last_error = None
                while time.monotonic() < deadline and process.poll() is None:
                    try:
                        request = Request(
                            "http://127.0.0.1:%d/api/setup/always-ready" % port,
                            data=b"{\"action\":\"alwaysReady\",\"on\":true}",
                            headers={"Content-Type": "application/json"}, method="POST")
                        response = urlopen(request, timeout=1)
                        break
                    except HTTPError as error:
                        self.fail("native Always Ready action failed with HTTP %d: %s" %
                                  (error.code, error.read().decode("utf-8", errors="replace")))
                    except (OSError, URLError) as error:
                        last_error = error
                        time.sleep(0.02)
                if response is None:
                    output = "daemon still running"
                    if process.poll() is not None:
                        output, _ = process.communicate(timeout=2)
                    self.fail("native setup action did not answer: %r\n%s" %
                              (last_error, output))
                self.assertEqual(response.status, 200)
                self.assertEqual(json.loads(response.read().decode("utf-8")), {"ok": True})
                self.assertTrue(capture.is_file(), "native setup action did not call Luna")

                # The daemon's ad-block manager also reads the TV country at
                # startup, so pick the setup write out of an append-only log.
                def lastAlwaysReadyWrite():
                    for line in reversed(capture.read_text().splitlines()):
                        try:
                            arguments = json.loads(line)
                        except ValueError:
                            continue
                        if "luna://com.webos.service.settings/setSystemSettings" in arguments:
                            return arguments
                    return []

                arguments = lastAlwaysReadyWrite()
                self.assertTrue(arguments, "native setup action did not write the setting")
                self.assertEqual(json.loads(arguments[-1]),
                                 {"category": "general", "settings": {"alwaysOn": "on"}})

                legacyAction = Request(
                    "http://127.0.0.1:%d/api/setup" % port,
                    data=b'{"action":"alwaysReady","on":"false"}',
                    headers={"Content-Type": "application/json"}, method="POST")
                legacyResponse = urlopen(legacyAction, timeout=3)
                self.assertEqual(legacyResponse.status, 200)
                self.assertEqual(json.loads(legacyResponse.read().decode("utf-8")), {"ok": True})
                legacyArguments = lastAlwaysReadyWrite()
                self.assertEqual(json.loads(legacyArguments[-1]),
                                 {"category": "general", "settings": {"alwaysOn": "on"}},
                                 "legacy setup actions preserve JavaScript truthiness")
            finally:
                if process.poll() is None:
                    process.send_signal(signal.SIGTERM)
                    process.communicate(timeout=5)

    def test_setup_network_writes_config_and_restarts_node(self):
        binary = Path(sys.argv[1]).resolve()
        secret = "setup-network-proxy-secret-for-tests-0123456789"
        restartRequests = []
        restartState = {"busy": True}

        class RestartCaptureHandler(BaseHTTPRequestHandler):
            def do_POST(self):
                length = int(self.headers.get("Content-Length", "0"))
                self.rfile.read(length)
                restartRequests.append({
                    "path": self.path,
                    "secret": self.headers.get("X-NativeTv-Proxy-Secret", ""),
                    "peer": self.headers.get("X-NativeTv-Original-Remote"),
                })
                if self.path == "/_internal/restart?check=1":
                    status = 409 if restartState["busy"] else 200
                    body = ({"ok": False, "error": "an install is in progress"}
                            if restartState["busy"] else {"ok": True})
                elif self.path == "/_internal/restart":
                    status = 200
                    body = {"ok": True, "restarting": True}
                else:
                    status = 404
                    body = {"ok": False}
                payload = json.dumps(body).encode("utf-8")
                self.send_response(status)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(payload)))
                self.end_headers()
                self.wfile.write(payload)

            def log_message(self, *_args):
                pass

        port = unused_port_pair()
        upstreamPort = unused_port_pair((port, port + 1))
        upstream = HTTPServer(("127.0.0.1", upstreamPort), RestartCaptureHandler)
        thread = threading.Thread(target=upstream.serve_forever, daemon=True)
        thread.start()
        with tempfile.TemporaryDirectory(prefix="nativetv-setup-network-") as folder:
            root = Path(folder)
            config = root / "config.json"
            heartbeat = root / "tvweb.beat"
            assets = root / "assets"
            assets.mkdir()
            config.write_text(json.dumps({
                "host": "127.0.0.1", "port": port, "allowControl": True,
                "token": "preserve-me", "mqtt": {"host": "broker.local", "password": "keep"}
            }))
            environment = os.environ.copy()
            environment["TVWEB_PROXY_SECRET"] = secret
            environment["TVWEB_DATA_DIR"] = str(root)
            process = subprocess.Popen(
                [str(binary), "--config", str(config), "--heartbeat", str(heartbeat),
                 "--workers", "2", "--listen", "--node-port", str(upstreamPort),
                 "--assets-root", str(assets)],
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, env=environment)
            try:
                deadline = time.monotonic() + 5
                ready = False
                while time.monotonic() < deadline and process.poll() is None:
                    try:
                        urlopen("http://127.0.0.1:%d/api/caps" % port, timeout=1).read()
                        ready = True
                        break
                    except (OSError, URLError):
                        time.sleep(0.02)
                self.assertTrue(ready, "native front door did not start for network setup")

                def post(origin):
                    request = Request(
                        "http://127.0.0.1:%d/api/setup/network" % port,
                        data=b'{"action":"network","open":true}',
                        headers={"Content-Type": "application/json", "Origin": origin},
                        method="POST")
                    return urlopen(request, timeout=4)

                try:
                    post("http://evil.example")
                    self.fail("network setup should reject a cross-origin request")
                except HTTPError as error:
                    self.assertEqual(error.code, 403)
                self.assertEqual(restartRequests, [], "CSRF refusal must not call Node")
                self.assertEqual(json.loads(config.read_text())["host"], "127.0.0.1")

                origin = "http://127.0.0.1:%d" % port
                try:
                    post(origin)
                    self.fail("network setup must refuse a restart during an install")
                except HTTPError as error:
                    self.assertEqual(error.code, 409)
                    error.read()
                self.assertEqual(restartRequests[-1]["path"], "/_internal/restart?check=1")
                self.assertEqual(json.loads(config.read_text())["host"], "127.0.0.1",
                                 "busy restart preflight must happen before config writes")

                restartState["busy"] = False
                with post(origin) as response:
                    self.assertEqual(response.status, 200)
                    self.assertEqual(json.loads(response.read().decode("utf-8")),
                                     {"ok": True, "restarting": True})
                saved = json.loads(config.read_text())
                self.assertEqual(saved["host"], "::")
                self.assertEqual(saved["token"], "preserve-me")
                self.assertEqual(saved["mqtt"]["password"], "keep")
                self.assertEqual(stat.S_IMODE(config.stat().st_mode), 0o600)
                self.assertEqual([item["path"] for item in restartRequests], [
                    "/_internal/restart?check=1", "/_internal/restart?check=1",
                    "/_internal/restart"])
                self.assertTrue(all(item["secret"] == secret and item["peer"] is None
                                    for item in restartRequests),
                                "restart bridge must send only its shared secret")
            finally:
                if process.poll() is None:
                    process.send_signal(signal.SIGTERM)
                    process.communicate(timeout=5)
        upstream.shutdown()
        upstream.server_close()
        thread.join(timeout=2)

    def test_setup_network_writes_config_and_restarts_node(self):
        binary = Path(sys.argv[1]).resolve()
        secret = "setup-network-proxy-secret-for-tests-0123456789"
        restartRequests = []
        restartState = {"busy": True}

        class RestartCaptureHandler(BaseHTTPRequestHandler):
            def do_POST(self):
                length = int(self.headers.get("Content-Length", "0"))
                self.rfile.read(length)
                restartRequests.append({
                    "path": self.path,
                    "secret": self.headers.get("X-NativeTv-Proxy-Secret", ""),
                    "peer": self.headers.get("X-NativeTv-Original-Remote"),
                })
                if self.path == "/_internal/restart?check=1":
                    status = 409 if restartState["busy"] else 200
                    body = ({"ok": False, "error": "an install is in progress"}
                            if restartState["busy"] else {"ok": True})
                elif self.path == "/_internal/restart":
                    status = 200
                    body = {"ok": True, "restarting": True}
                else:
                    status = 404
                    body = {"ok": False}
                payload = json.dumps(body).encode("utf-8")
                self.send_response(status)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(payload)))
                self.end_headers()
                self.wfile.write(payload)

            def log_message(self, *_args):
                pass

        port = unused_port_pair()
        upstreamPort = unused_port_pair((port, port + 1))
        upstream = HTTPServer(("127.0.0.1", upstreamPort), RestartCaptureHandler)
        thread = threading.Thread(target=upstream.serve_forever, daemon=True)
        thread.start()
        with tempfile.TemporaryDirectory(prefix="nativetv-setup-network-") as folder:
            root = Path(folder)
            config = root / "config.json"
            heartbeat = root / "tvweb.beat"
            assets = root / "assets"
            assets.mkdir()
            config.write_text(json.dumps({
                "host": "127.0.0.1", "port": port, "allowControl": True,
                "token": "preserve-me", "mqtt": {"host": "broker.local", "password": "keep"}
            }))
            environment = os.environ.copy()
            environment["TVWEB_PROXY_SECRET"] = secret
            environment["TVWEB_DATA_DIR"] = str(root)
            process = subprocess.Popen(
                [str(binary), "--config", str(config), "--heartbeat", str(heartbeat),
                 "--workers", "2", "--listen", "--node-port", str(upstreamPort),
                 "--assets-root", str(assets)],
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, env=environment)
            try:
                deadline = time.monotonic() + 5
                ready = False
                while time.monotonic() < deadline and process.poll() is None:
                    try:
                        urlopen("http://127.0.0.1:%d/api/caps" % port, timeout=1).read()
                        ready = True
                        break
                    except (OSError, URLError):
                        time.sleep(0.02)
                self.assertTrue(ready, "native front door did not start for network setup")

                def post(origin):
                    request = Request(
                        "http://127.0.0.1:%d/api/setup/network" % port,
                        data=b'{"action":"network","open":true}',
                        headers={"Content-Type": "application/json", "Origin": origin},
                        method="POST")
                    return urlopen(request, timeout=4)

                try:
                    post("http://evil.example")
                    self.fail("network setup should reject a cross-origin request")
                except HTTPError as error:
                    self.assertEqual(error.code, 403)
                self.assertEqual(restartRequests, [], "CSRF refusal must not call Node")
                self.assertEqual(json.loads(config.read_text())["host"], "127.0.0.1")

                origin = "http://127.0.0.1:%d" % port
                try:
                    post(origin)
                    self.fail("network setup must refuse a restart during an install")
                except HTTPError as error:
                    self.assertEqual(error.code, 409)
                    error.read()
                self.assertEqual(restartRequests[-1]["path"], "/_internal/restart?check=1")
                self.assertEqual(json.loads(config.read_text())["host"], "127.0.0.1",
                                 "busy restart preflight must happen before config writes")

                restartState["busy"] = False
                with post(origin) as response:
                    self.assertEqual(response.status, 200)
                    self.assertEqual(json.loads(response.read().decode("utf-8")),
                                     {"ok": True, "restarting": True})
                saved = json.loads(config.read_text())
                self.assertEqual(saved["host"], "::")
                self.assertEqual(saved["token"], "preserve-me")
                self.assertEqual(saved["mqtt"]["password"], "keep")
                self.assertEqual(stat.S_IMODE(config.stat().st_mode), 0o600)
                self.assertEqual([item["path"] for item in restartRequests], [
                    "/_internal/restart?check=1", "/_internal/restart?check=1",
                    "/_internal/restart"])
                self.assertTrue(all(item["secret"] == secret and item["peer"] is None
                                    for item in restartRequests),
                                "restart bridge must send only its shared secret")
            finally:
                if process.poll() is None:
                    process.send_signal(signal.SIGTERM)
                    process.communicate(timeout=5)
        upstream.shutdown()
        upstream.server_close()
        thread.join(timeout=2)

    def test_native_dashboard_settings_write_and_restart(self):
        binary = Path(sys.argv[1]).resolve()
        secret = "dashboard-settings-proxy-secret-0123456789"
        restartRequests = []
        restartState = {"busy": True}

        class RestartCaptureHandler(BaseHTTPRequestHandler):
            def do_POST(self):
                self.rfile.read(int(self.headers.get("Content-Length", "0")))
                restartRequests.append({
                    "path": self.path,
                    "secret": self.headers.get("X-NativeTv-Proxy-Secret", ""),
                    "peer": self.headers.get("X-NativeTv-Original-Remote"),
                })
                if self.path == "/_internal/restart?check=1":
                    status = 409 if restartState["busy"] else 200
                    response = ({"ok": False, "error": "an install is in progress"}
                                if restartState["busy"] else {"ok": True})
                elif self.path == "/_internal/restart":
                    status, response = 200, {"ok": True, "restarting": True}
                else:
                    status, response = 404, {"ok": False}
                payload = json.dumps(response).encode("utf-8")
                self.send_response(status)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(payload)))
                self.end_headers()
                self.wfile.write(payload)

            def log_message(self, *_args):
                pass

        port = unused_port_pair()
        nodePort = unused_port_pair((port, port + 1))
        node = HTTPServer(("127.0.0.1", nodePort), RestartCaptureHandler)
        thread = threading.Thread(target=node.serve_forever, daemon=True)
        thread.start()
        with tempfile.TemporaryDirectory(prefix="nativetv-dashboard-settings-") as folder:
            root = Path(folder)
            config = root / "config.json"
            heartbeat = root / "tvweb.beat"
            assets = root / "assets"
            assets.mkdir()
            original = {
                "host": "127.0.0.1", "port": port, "allowControl": True,
                "allowPower": False, "token": "route-token", "preserved": {"key": 7},
                "mqtt": {"host": "old-broker", "password": "keep-secret", "legacy": True},
                "device": {"legacy": "keep-device"}
            }
            config.write_text(json.dumps(original))
            environment = os.environ.copy()
            environment["TVWEB_PROXY_SECRET"] = secret
            environment["TVWEB_DATA_DIR"] = str(root)
            process = subprocess.Popen(
                [str(binary), "--config", str(config), "--heartbeat", str(heartbeat),
                 "--workers", "4", "--listen", "--node-port", str(nodePort),
                 "--assets-root", str(assets)], stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT, text=True, env=environment)
            try:
                deadline = time.monotonic() + 6
                ready = False
                while time.monotonic() < deadline and process.poll() is None:
                    try:
                        urlopen("http://127.0.0.1:%d/api/caps" % port, timeout=1).read()
                        ready = True
                        break
                    except (OSError, URLError):
                        time.sleep(0.02)
                self.assertTrue(ready, "native front door did not start for dashboard settings")

                url = "http://127.0.0.1:%d/api/settings" % port
                headers = {"Content-Type": "application/json",
                           "Authorization": "Bearer route-token"}
                body = json.dumps({
                    "mqtt": {"enabled": True, "host": " broker.local ",
                             "port": "8883tail", "tls": True,
                             "tlsRejectUnauthorized": False, "username": " user ",
                             "topicPrefix": " livingroom ", "discoveryPrefix": "homeassistant",
                             "telemetryIntervalMs": "20000ms",
                             "entities": {"controls": False, "disabled": ["cpu.temp", "bad/id"]}},
                    "device": {"id": " living_room ", "name": " Living Room "},
                    "iconServer": True, "host": "0.0.0.0", "port": 1,
                    "allowControl": False, "token": "attacker"
                }).encode("utf-8")

                def post(data, extra=None):
                    requestHeaders = dict(headers)
                    if extra:
                        requestHeaders.update(extra)
                    request = Request(url, data=data, headers=requestHeaders, method="POST")
                    try:
                        with urlopen(request, timeout=5) as response:
                            return response.status, response.read()
                    except HTTPError as error:
                        return error.code, error.read()

                # A loopback peer is the TV and bypasses token auth by design,
                # matching Node; remote token enforcement is covered by the
                # native HTTP unit tests.
                status, _ = post(body, {"Origin": "http://attacker.example"})
                self.assertEqual(status, 403)
                self.assertEqual(restartRequests, [], "auth/CSRF refusals must not reach Node")
                status, response = post(body)
                self.assertEqual(status, 409, response.decode("utf-8", "replace"))
                self.assertEqual(json.loads(config.read_text()), original,
                                 "install preflight must occur before config write")
                self.assertEqual([item["path"] for item in restartRequests],
                                 ["/_internal/restart?check=1"])

                invalid = json.dumps({"mqtt": {"enabled": True},
                                      "device": {"id": "bad id"}}).encode("utf-8")
                status, response = post(invalid)
                self.assertEqual(status, 400, response.decode("utf-8", "replace"))
                self.assertEqual(len(restartRequests), 1,
                                 "invalid settings must be rejected before restart preflight")

                restartState["busy"] = False
                status, response = post(body)
                self.assertEqual(status, 200, response.decode("utf-8", "replace"))
                self.assertEqual(json.loads(response), {"ok": True, "restarting": True})
                saved = json.loads(config.read_text())
                self.assertEqual(saved["preserved"], {"key": 7})
                self.assertEqual(saved["host"], "127.0.0.1")
                self.assertEqual(saved["port"], port)
                self.assertTrue(saved["allowControl"])
                self.assertFalse(saved["allowPower"])
                self.assertEqual(saved["token"], "route-token")
                self.assertEqual(saved["mqtt"]["host"], "broker.local")
                self.assertEqual(saved["mqtt"]["port"], 8883)
                self.assertEqual(saved["mqtt"]["password"], "keep-secret")
                self.assertTrue(saved["mqtt"]["legacy"])
                self.assertEqual(saved["device"]["id"], "living_room")
                self.assertEqual(saved["device"]["legacy"], "keep-device")
                self.assertTrue(saved["iconServer"])
                self.assertEqual(stat.S_IMODE(config.stat().st_mode), 0o600)
                self.assertEqual([item["path"] for item in restartRequests], [
                    "/_internal/restart?check=1", "/_internal/restart?check=1",
                    "/_internal/restart"])
                self.assertTrue(all(item["secret"] == secret and item["peer"] is None
                                    for item in restartRequests))
            finally:
                if process.poll() is None:
                    process.send_signal(signal.SIGTERM)
                    process.communicate(timeout=5)
        node.shutdown()
        node.server_close()
        thread.join(timeout=2)

    def test_native_dashboard_settings_read_projection(self):
        binary = Path(sys.argv[1]).resolve()
        secret = "dashboard-settings-read-secret-0123456789"
        contextRequests = []
        forwardedGets = []
        state = {"fail": False}
        context = {
            "status": {
                "state": "connected", "broker": "broker.local:8883",
                "tls": True, "detail": "", "forMs": 1234,
                "lastPublishMs": 567, "latencyMs": 42
            },
            "categories": [{"id": "controls", "name": "Controls & Media"}],
            "entityCatalogue": [{"id": "switch.living_room", "type": "switch",
                                 "name": "Living Room", "cat": "controls"}]
        }

        class SettingsContextHandler(BaseHTTPRequestHandler):
            def do_POST(self):
                length = int(self.headers.get("Content-Length", "0"))
                body = self.rfile.read(length)
                contextRequests.append({
                    "path": self.path,
                    "secret": self.headers.get("X-NativeTv-Proxy-Secret", ""),
                    "peer": self.headers.get("X-NativeTv-Original-Remote"),
                    "body": body,
                })
                if self.path != "/_internal/settings-context":
                    payload = b'{"ok":false,"error":"not found"}'
                    self.send_response(404)
                elif state["fail"]:
                    payload = b'{"ok":false,"error":"unavailable"}'
                    self.send_response(500)
                else:
                    payload = json.dumps(context).encode("utf-8")
                    self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(payload)))
                self.end_headers()
                self.wfile.write(payload)

            def do_GET(self):
                forwardedGets.append(self.path)
                payload = b'{"ok":true,"fallback":true}'
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(payload)))
                self.end_headers()
                self.wfile.write(payload)

            def log_message(self, *_args):
                pass

        port = unused_port_pair()
        nodePort = unused_port_pair((port, port + 1, port + 2))
        node = HTTPServer(("127.0.0.1", nodePort), SettingsContextHandler)
        thread = threading.Thread(target=node.serve_forever, daemon=True)
        thread.start()
        with tempfile.TemporaryDirectory(prefix="nativetv-dashboard-settings-read-") as folder:
            root = Path(folder)
            config = root / "config.json"
            heartbeat = root / "tvweb.beat"
            assets = root / "assets"
            assets.mkdir()
            config.write_text(json.dumps({
                "host": "127.0.0.1", "port": port, "allowControl": True,
                "token": "settings-read-token", "iconServer": True,
                "mqtt": {
                    "enabled": True, "host": "broker.local", "port": 8883,
                    "tls": True, "tlsRejectUnauthorized": False,
                    "username": "mqtt-user", "password": "keep-secret",
                    "topicPrefix": "livingroom", "discoveryPrefix": "ha",
                    "telemetryIntervalMs": 0,
                    "entities": {"controls": False, "oled": True,
                                 "disabled": ["cpu.temp", "bad/id"]}
                },
                "device": {"id": "living_room", "name": "Living Room"}
            }))
            environment = os.environ.copy()
            environment["TVWEB_PROXY_SECRET"] = secret
            environment["TVWEB_DATA_DIR"] = str(root)
            process = subprocess.Popen(
                [str(binary), "--config", str(config), "--heartbeat", str(heartbeat),
                 "--workers", "4", "--listen", "--node-port", str(nodePort),
                 "--assets-root", str(assets)], stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT, text=True, env=environment)
            try:
                deadline = time.monotonic() + 6
                ready = False
                while time.monotonic() < deadline and process.poll() is None:
                    try:
                        urlopen("http://127.0.0.1:%d/api/caps" % port, timeout=1).read()
                        ready = True
                        break
                    except (OSError, URLError):
                        time.sleep(0.02)
                if not ready:
                    if process.poll() is None:
                        process.send_signal(signal.SIGTERM)
                    output, _ = process.communicate(timeout=5)
                    self.fail("native front door did not start for dashboard settings read:\n" +
                              output[-4000:])

                url = "http://127.0.0.1:%d/api/settings" % port
                request = Request(url, headers={
                    "Authorization": "Bearer settings-read-token",
                    "Connection": "close"})
                with urlopen(request, timeout=5) as response:
                    self.assertEqual(response.status, 200)
                    self.assertEqual(response.headers.get("Content-Type"), "application/json")
                    self.assertEqual(response.headers.get("Cache-Control"), "no-store")
                    self.assertEqual(response.headers.get("X-Content-Type-Options"), "nosniff")
                    self.assertEqual(response.headers.get("Referrer-Policy"), "no-referrer")
                    settings = json.loads(response.read().decode("utf-8"))

                self.assertEqual(settings["ok"], True)
                self.assertEqual(settings["writable"], True)
                self.assertEqual(settings["configFile"], str(config))
                self.assertEqual(settings["iconServer"], True)
                self.assertEqual(settings["device"],
                                 {"id": "living_room", "name": "Living Room"})
                self.assertEqual(settings["status"], context["status"])
                mqtt = settings["mqtt"]
                self.assertEqual(mqtt["enabled"], True)
                self.assertEqual(mqtt["host"], "broker.local")
                self.assertEqual(mqtt["port"], 8883)
                self.assertEqual(mqtt["tls"], True)
                self.assertEqual(mqtt["tlsRejectUnauthorized"], False)
                self.assertEqual(mqtt["username"], "mqtt-user")
                self.assertEqual(mqtt["passwordSet"], True)
                self.assertNotIn("password", mqtt)
                self.assertEqual(mqtt["topicPrefix"], "livingroom")
                self.assertEqual(mqtt["discoveryPrefix"], "ha")
                self.assertEqual(mqtt["telemetryIntervalMs"], 30000, "the server's default")
                self.assertEqual(mqtt["entities"], {
                    "controls": False, "oled": True, "video": True,
                    "system": True, "diagnostics": True,
                    "disabled": ["cpu.temp", "bad/id"]
                })
                categories = mqtt["categories"]
                self.assertEqual([item["id"] for item in categories], [
                    "controls", "oled", "video", "system", "diagnostics"])
                self.assertEqual(categories[0]["name"], "Controls & Media")
                catalogue = mqtt["entityCatalogue"]
                self.assertEqual(len(catalogue), 114)
                self.assertEqual(catalogue[0], {
                    "id": "display_panel", "type": "switch",
                    "name": "Display Panel", "cat": "controls"})
                self.assertEqual(catalogue[-1], {
                    "id": "smart_tips_in_settings", "type": "switch",
                    "name": "Smart Tips in Settings", "cat": "diagnostics"})
                self.assertEqual(len(contextRequests), 1)
                self.assertEqual(contextRequests[0]["path"], "/_internal/settings-context")
                self.assertEqual(contextRequests[0]["secret"], secret)
                self.assertIsNone(contextRequests[0]["peer"])
                self.assertEqual(contextRequests[0]["body"], b"")
                self.assertEqual(forwardedGets, [])

                state["fail"] = True
                with urlopen(request, timeout=5) as response:
                    self.assertEqual(response.status, 200)
                    self.assertEqual(json.loads(response.read().decode("utf-8")),
                                     {"ok": True, "fallback": True})
                self.assertEqual(forwardedGets, ["/api/settings"])
            finally:
                if process.poll() is None:
                    process.send_signal(signal.SIGTERM)
                    # CivetWeb may take several seconds to stop its public,
                    # handoff and icon listeners after the final request.
                    process.communicate(timeout=10)
        node.shutdown()
        node.server_close()
        thread.join(timeout=2)

    def test_native_settings_backup_export(self):
        binary = Path(sys.argv[1]).resolve()
        fakeLuna = binary.parent / "native-fake-luna-send"
        token = "backup-export-token"
        secret = "backup-export-proxy-secret-0123456789abcdef"
        contextRequests = []
        forwardedGets = []
        forwardedBackups = []
        state = {"fail": False}
        layout = {"items": [{"id": "netflix", "position": 2}],
                  "picture": "movie"}
        context = {
            "status": {"state": "connected", "forMs": 10},
            "categories": [],
            "entityCatalogue": [],
            "homeLayout": layout,
            "displayVersion": "2.4.1",
            "backupDaily": True
        }

        class BackupContextHandler(BaseHTTPRequestHandler):
            def do_POST(self):
                length = int(self.headers.get("Content-Length", "0"))
                body = self.rfile.read(length)
                if self.path == "/api/backup":
                    forwardedBackups.append({"path": self.path, "body": body})
                    payload = b'{"ok":true,"restored":"node"}'
                    self.send_response(201)
                    self.send_header("Content-Type", "application/json")
                    self.send_header("Content-Length", str(len(payload)))
                    self.end_headers()
                    self.wfile.write(payload)
                    return
                contextRequests.append({
                    "path": self.path,
                    "secret": self.headers.get("X-NativeTv-Proxy-Secret", ""),
                    "peer": self.headers.get("X-NativeTv-Original-Remote"),
                    "body": body,
                })
                if self.path != "/_internal/settings-context":
                    payload = b'{"ok":false,"error":"not found"}'
                    self.send_response(404)
                elif state["fail"]:
                    payload = b'{"ok":false,"error":"unavailable"}'
                    self.send_response(500)
                else:
                    payload = json.dumps(context).encode("utf-8")
                    self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(payload)))
                self.end_headers()
                self.wfile.write(payload)

            def do_GET(self):
                forwardedGets.append(self.path)
                payload = b'{"format":"node-fallback"}'
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(payload)))
                self.end_headers()
                self.wfile.write(payload)

            def log_message(self, *_args):
                pass

        publicPort = unused_port_pair()
        nodePort = unused_port_pair((publicPort, publicPort + 1))
        node = HTTPServer(("127.0.0.1", nodePort), BackupContextHandler)
        thread = threading.Thread(target=node.serve_forever, daemon=True)
        thread.start()
        with tempfile.TemporaryDirectory(prefix="nativetv-backup-export-") as folder:
            root = Path(folder)
            config = root / "config.json"
            heartbeat = root / "tvweb.beat"
            assets = root / "assets"
            assets.mkdir()
            details = root / "luna-details.jsonl"
            backupDir = root / "backups"
            backupDir.mkdir()
            savedBackupName = "nativetv-backup-2026-10-26.json"
            savedBackupBody = '{"format":"nativetv-backup","version":1}\n'
            (backupDir / savedBackupName).write_text(savedBackupBody)
            (root / "restore-apps.json").write_text(json.dumps({
                "saved": 123,
                "apps": [
                    {"id": "com.example.alpha", "title": "Already installed"},
                    {"id": "com.example.missing", "title": "Missing app"}
                ]
            }))
            config.write_text(json.dumps({
                "host": "127.0.0.1", "port": publicPort, "token": token,
                "allowControl": True, "device": {"model": "OLED65C4"},
                "mqtt": {"username": "private-user", "password": "private-pass"},
                "logging": {"level": "info", "unknown": True},
                "syslog": {"server": "nas.local", "hostname": "this-tv"},
                "backup": {"daily": False},
                "screenTime": {"enabled": True, "dailyMinutes": 90,
                               "unknown": "omit"},
                "update": {"check": False, "intervalHours": 24}
            }))
            environment = os.environ.copy()
            environment["TVWEB_PROXY_SECRET"] = secret
            environment["TVWEB_DATA_DIR"] = str(root)
            environment["TVWEB_LUNA_SEND"] = str(fakeLuna)
            environment["NATIVETV_FAKE_LUNA_MODE"] = "backup"
            environment["NATIVETV_FAKE_LUNA_DETAILS"] = str(details)
            process = subprocess.Popen(
                [str(binary), "--config", str(config), "--heartbeat", str(heartbeat),
                 "--workers", "4", "--listen", "--node-port", str(nodePort),
                 "--assets-root", str(assets)], stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT, text=True, env=environment)
            try:
                def get(path, authorization=True):
                    headers = {}
                    if authorization:
                        headers["Authorization"] = "Bearer " + token
                    request = Request("http://127.0.0.1:%d%s" % (publicPort, path),
                                      headers=headers)
                    return urlopen(request, timeout=8)

                def postBackup(body, contentType="application/json", origin=None):
                    headers = {"Authorization": "Bearer " + token,
                               "Content-Type": contentType}
                    if origin is not None:
                        headers["Origin"] = origin
                    request = Request("http://127.0.0.1:%d/api/backup" % publicPort,
                                      data=body, method="POST", headers=headers)
                    try:
                        with urlopen(request, timeout=8) as response:
                            return response.status, response.read()
                    except HTTPError as error:
                        return error.code, error.read()

                def updateInstallerSnapshot(snapshot):
                    request = Request(
                        "http://127.0.0.1:%d/_internal/apps/install-job" % publicPort,
                        data=json.dumps({"action": "snapshot", "snapshot": snapshot}).encode("utf-8"),
                        method="POST", headers={
                            "Content-Type": "application/json",
                            "X-NativeTv-Proxy-Secret": secret
                        })
                    try:
                        with urlopen(request, timeout=8) as response:
                            return response.status, response.read()
                    except HTTPError as error:
                        return error.code, error.read()

                def postPublicClear(origin=None):
                    headers = {"Authorization": "Bearer " + token}
                    if origin is not None:
                        headers["Origin"] = origin
                    request = Request(
                        "http://127.0.0.1:%d/api/backup/apps/clear" % publicPort,
                        data=b"ignored", method="POST", headers=headers)
                    try:
                        with urlopen(request, timeout=8) as response:
                            return response.status, response.read()
                    except HTTPError as error:
                        return error.code, error.read()

                deadline = time.monotonic() + 8
                ready = False
                while time.monotonic() < deadline and process.poll() is None:
                    try:
                        get("/api/caps").read()
                        ready = True
                        break
                    except (OSError, URLError):
                        time.sleep(0.03)
                if not ready:
                    if process.poll() is None:
                        process.send_signal(signal.SIGTERM)
                    output, _ = process.communicate(timeout=6)
                    self.fail("native daemon did not start for backup export:\n" +
                              output[-4000:])

                with get("/api/backup") as response:
                    self.assertEqual(response.status, 200)
                    self.assertEqual(response.headers.get("Content-Type"), "application/json")
                    self.assertEqual(response.headers.get("Cache-Control"), "no-store")
                    backup = json.loads(response.read().decode("utf-8"))

                self.assertEqual(backup["format"], "nativetv-backup")
                self.assertEqual(backup["version"], 1)
                self.assertRegex(backup["created"],
                                 r"^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}\.\d{3}Z$")
                self.assertEqual(backup["from"], {
                    "model": "OLED65C4",
                    "nativetv": (Path(__file__).resolve().parents[1] / "VERSION").read_text().strip()})
                self.assertEqual(backup["lg"]["homePromotion"], False)
                self.assertEqual(backup["lg"]["soundMode"], "movie")
                # Settings no module was given are saved at their defaults.
                self.assertEqual(backup["nativetv"], {
                    "history": {"enabled": False},
                    "logging": {"level": "info"},
                    "ntp": {"port": 123, "server": ""},
                    "syslog": {"server": "nas.local", "port": 514, "redact": True,
                               "sources": ["system", "nativetv"]},
                    "backup": {"daily": False},
                    "screenTime": {"action": "off", "appAction": "home", "apps": {},
                                   "bedtimeEnd": "", "bedtimeStart": "",
                                   "enabled": True, "dailyMinutes": 90},
                    "update": {"check": False}
                })
                # The Home layout is the daemon's own reading of home.json now.
                self.assertEqual(backup["home"]["rows"], ["app", "dev", "ch"])
                self.assertIn("order", backup["home"])
                self.assertEqual(backup["apps"], [
                    {"id": "com.example.alpha", "title": "com.example.alpha",
                     "version": "true"},
                    {"id": "com.example.zed", "title": "Zed", "version": "2"}
                ])
                self.assertNotIn("token", backup)
                self.assertNotIn("host", backup)
                self.assertNotIn("mqtt", backup)
                self.assertNotIn("hostname", backup["nativetv"].get("syslog", {}))

                status, response = postBackup(b"{}", contentType="text/plain")
                self.assertEqual(status, 415, response.decode("utf-8", "replace"))
                status, response = postBackup(b"{}", origin="https://other.example")
                self.assertEqual(status, 403, response.decode("utf-8", "replace"))
                status, response = postBackup(b"{")
                self.assertEqual(status, 400, response.decode("utf-8", "replace"))
                status, response = postBackup(b"x" * 65537)
                self.assertEqual(status, 413, response.decode("utf-8", "replace"))
                self.assertEqual(forwardedBackups, [],
                                 "invalid restore requests must stop at the native front door")
                validRestore = b'{"format":"nativetv-backup","version":1}'
                pendingApps = (root / "restore-apps.json").read_text()
                status, response = postBackup(validRestore)
                self.assertEqual(status, 200, response.decode("utf-8", "replace"))
                self.assertEqual(json.loads(response.decode("utf-8")), {
                    "ok": True,
                    "lg": {"applied": [], "unchanged": 0, "skipped": [], "failed": []},
                    "nativetv": [], "restart": False,
                    "apps": {"toInstall": [], "manual": [], "present": 0,
                             "catalogError": None},
                    "home": False
                }, "the native front door restores a backup itself")
                self.assertEqual(forwardedBackups, [],
                                 "a restore is the native front door's, not Node's")
                self.assertFalse((root / "restore-apps.json").exists(),
                                 "a restore with no apps to install clears the pending list")
                (root / "restore-apps.json").write_text(pendingApps)
                status, response = postBackup(b"null")
                self.assertEqual(status, 400, response.decode("utf-8", "replace"))
                self.assertEqual(json.loads(response.decode("utf-8")), {
                    "ok": False, "error": "That file is not a Native Tv backup"})

                workingSnapshot = {
                    "jobId": "backup-busy-job", "state": "installing",
                    "source": "catalog", "appId": "com.example.app",
                    "repo": "https://repo.example/apps.json",
                    "progress": {"bytes": 1, "total": 2},
                    "preview": None, "error": None, "result": None
                }
                status, response = updateInstallerSnapshot(workingSnapshot)
                self.assertEqual(status, 200, response.decode("utf-8", "replace"))
                status, response = postBackup(b"{}")
                self.assertEqual(status, 409, response.decode("utf-8", "replace"))
                self.assertEqual(json.loads(response.decode("utf-8")), {
                    "ok": False,
                    "error": "an install is in progress; restart when it has finished"
                }, "native must refuse restore during active install work before Node forwarding")
                self.assertEqual(forwardedBackups, [])

                previewSnapshot = dict(workingSnapshot)
                previewSnapshot["state"] = "awaiting-confirm"
                previewSnapshot["preview"] = {"appId": "com.example.app"}
                status, response = updateInstallerSnapshot(previewSnapshot)
                self.assertEqual(status, 200, response.decode("utf-8", "replace"))
                status, response = postBackup(validRestore)
                self.assertEqual(status, 200, response.decode("utf-8", "replace"))
                self.assertEqual(forwardedBackups, [])
                (root / "restore-apps.json").write_text(pendingApps)

                with get("/api/backup/apps") as response:
                    self.assertEqual(response.status, 200)
                    self.assertEqual(json.loads(response.read().decode("utf-8")), {
                        "ok": True,
                        "apps": [{"id": "com.example.missing", "title": "Missing app"}]
                    })

                pendingPath = root / "restore-apps.json"

                def updatePending(payload, proxySecret=None, originalRemote=None,
                                  contentType="application/json"):
                    headers = {"Content-Type": contentType}
                    if proxySecret is not None:
                        headers["X-NativeTv-Proxy-Secret"] = proxySecret
                    if originalRemote is not None:
                        headers["X-NativeTv-Original-Remote"] = originalRemote
                    request = Request(
                        "http://127.0.0.1:%d/_internal/backup/apps-state" % publicPort,
                        data=json.dumps(payload).encode("utf-8"), method="POST",
                        headers=headers)
                    try:
                        with urlopen(request, timeout=8) as response:
                            return response.status, response.read()
                    except HTTPError as error:
                        return error.code, error.read()

                saveCommand = {"action": "save", "saved": 456,
                               "apps": [
                                   {"id": "com.example.alpha", "title": "Already installed"},
                                   {"id": "com.example.missing", "title": "Missing app"}
                               ]}
                status, response = updatePending(saveCommand)
                self.assertEqual(status, 403, response.decode("utf-8", "replace"))
                status, response = updatePending(saveCommand, "wrong-secret")
                self.assertEqual(status, 403, response.decode("utf-8", "replace"))
                status, response = updatePending(saveCommand, secret, "203.0.113.10")
                self.assertEqual(status, 403, response.decode("utf-8", "replace"))
                status, response = updatePending(saveCommand, secret,
                                                 contentType="text/plain")
                self.assertEqual(status, 415, response.decode("utf-8", "replace"))
                status, response = updatePending({"action": "save", "saved": 456,
                    "apps": [{"id": "../config", "title": "Escape"}]}, secret)
                self.assertEqual(status, 400, response.decode("utf-8", "replace"))
                self.assertEqual(json.loads(pendingPath.read_text())["apps"][0]["id"],
                                 "com.example.alpha",
                                 "invalid native app updates must preserve the previous file")
                status, response = updatePending(saveCommand, secret)
                self.assertEqual(status, 204, response.decode("utf-8", "replace"))
                self.assertEqual(json.loads(pendingPath.read_text()), {
                    "saved": 456,
                    "apps": [
                        {"id": "com.example.alpha", "title": "Already installed"},
                        {"id": "com.example.missing", "title": "Missing app"}
                    ]
                })
                self.assertEqual(pendingPath.stat().st_mode & 0o777, 0o600)
                with get("/api/backup/apps") as response:
                    self.assertEqual(response.status, 200)
                    self.assertEqual(json.loads(response.read().decode("utf-8")), {
                        "ok": True,
                        "apps": [{"id": "com.example.missing", "title": "Missing app"}]
                    })
                status, response = postPublicClear(origin="https://other.example")
                self.assertEqual(status, 200, response.decode("utf-8", "replace"))
                self.assertEqual(json.loads(response.decode("utf-8")),
                                 {"ok": True, "apps": []})
                self.assertFalse(pendingPath.exists())
                status, response = postPublicClear()
                self.assertEqual(status, 200,
                                 "clearing a missing pending list should remain idempotent: " +
                                 response.decode("utf-8", "replace"))
                self.assertEqual(json.loads(response.decode("utf-8")),
                                 {"ok": True, "apps": []})
                status, response = updatePending({"action": "clear"}, secret)
                self.assertEqual(status, 204, response.decode("utf-8", "replace"))
                self.assertFalse(pendingPath.exists())
                status, response = updatePending({"action": "clear"}, secret)
                self.assertEqual(status, 204,
                                 "clearing an already missing app list should succeed: " +
                                 response.decode("utf-8", "replace"))

                with get("/api/backup/saved?name=" + savedBackupName) as response:
                    self.assertEqual(response.status, 200)
                    self.assertEqual(response.headers.get("Content-Type"), "application/json")
                    self.assertEqual(response.headers.get("Cache-Control"), "no-store")
                    self.assertEqual(response.read().decode("utf-8"), savedBackupBody)
                for badName in (
                        "nativetv-backup-2026-10-25.json",
                        "..%2Fconfig.json",
                        savedBackupName + "&name=nativetv-backup-2026-10-26.json"):
                    try:
                        get("/api/backup/saved?name=" + badName)
                        self.fail("invalid or missing saved backup should return 404")
                    except HTTPError as error:
                        self.assertEqual(error.code, 404)
                        self.assertEqual(json.loads(error.read().decode("utf-8")),
                                         {"ok": False, "error": "no such backup"})

                def planRestoreApps(payload, proxySecret=None, originalRemote=None,
                                    contentType="application/json"):
                    headers = {"Content-Type": contentType}
                    if proxySecret is not None:
                        headers["X-NativeTv-Proxy-Secret"] = proxySecret
                    if originalRemote is not None:
                        headers["X-NativeTv-Original-Remote"] = originalRemote
                    request = Request(
                        "http://127.0.0.1:%d/_internal/backup/restore-plan" % publicPort,
                        data=json.dumps(payload).encode("utf-8"),
                        method="POST", headers=headers)
                    try:
                        with urlopen(request, timeout=8) as response:
                            return response.status, response.read()
                    except HTTPError as error:
                        return error.code, error.read()

                restorePlanInput = {
                    "apps": [
                        {"id": "com.example.present", "title": "Present"},
                        {"id": "youtube.leanback.v4", "title": "YouTube"},
                        {"id": "my.sideloaded.tool", "title": "Manual"},
                        {"id": "org.webosbrew.hbchannel", "title": "Protected"},
                        {"id": "../../etc", "title": "Invalid"}
                    ],
                    "installedIds": ["com.example.present"],
                    "catalogIds": ["youtube.leanback.v4"],
                    "catalogAvailable": True,
                    "catalogError": ""
                }
                status, response = planRestoreApps(restorePlanInput)
                self.assertEqual(status, 403,
                                 "the restore planner requires the loopback secret")
                status, response = planRestoreApps(restorePlanInput, "wrong-secret")
                self.assertEqual(status, 403,
                                 "the restore planner rejects a wrong loopback secret")
                status, response = planRestoreApps(restorePlanInput, secret,
                                                   "203.0.113.10")
                self.assertEqual(status, 403,
                                 "the restore planner rejects forwarded peers")
                status, response = planRestoreApps(restorePlanInput, secret)
                self.assertEqual(status, 200, response.decode("utf-8", "replace"))
                self.assertEqual(json.loads(response.decode("utf-8")), {
                    "toInstall": [{"id": "youtube.leanback.v4", "title": "YouTube"}],
                    "manual": [{"id": "my.sideloaded.tool", "title": "Manual"}],
                    "present": 1,
                    "catalogError": None
                })
                status, response = planRestoreApps({"apps": [
                    {"id": "com.example.alpha", "title": "Alpha"},
                    {"id": "com.example.zed", "title": "Zed"},
                    {"id": "org.webosbrew.hbchannel", "title": "Protected"},
                    {"id": "com.webos.app.input.hdmi2", "title": "HDMI"}
                ]}, secret)
                self.assertEqual(status, 200, response.decode("utf-8", "replace"))
                self.assertEqual(json.loads(response.decode("utf-8")), {
                    "toInstall": [], "manual": [], "present": 2,
                    "catalogError": None
                }, "app-only planning must derive installed developer apps in native code")

                def restoreBackupLg(lg, proxySecret=None, originalRemote=None,
                                    contentType="application/json"):
                    headers = {"Content-Type": contentType}
                    if proxySecret is not None:
                        headers["X-NativeTv-Proxy-Secret"] = proxySecret
                    if originalRemote is not None:
                        headers["X-NativeTv-Original-Remote"] = originalRemote
                    request = Request(
                        "http://127.0.0.1:%d/_internal/backup/restore-lg" % publicPort,
                        data=json.dumps({"lg": lg}).encode("utf-8"),
                        method="POST", headers=headers)
                    try:
                        with urlopen(request, timeout=30) as response:
                            return response.status, response.read()
                    except HTTPError as error:
                        return error.code, error.read()

                restoreValues = {
                    "homePromotion": True,
                    "soundMode": "movie",
                    "eArcSupport": True
                }
                status, response = restoreBackupLg(restoreValues)
                self.assertEqual(status, 403,
                                 "LG restore writes require the loopback secret")
                status, response = restoreBackupLg(restoreValues, "wrong-secret")
                self.assertEqual(status, 403,
                                 "LG restore writes reject a wrong loopback secret")
                status, response = restoreBackupLg(restoreValues, secret,
                                                   "203.0.113.10")
                self.assertEqual(status, 403,
                                 "LG restore writes reject forwarded peers")
                status, response = restoreBackupLg(restoreValues, secret,
                                                   contentType="text/plain")
                self.assertEqual(status, 415,
                                 "LG restore writes require JSON content type")
                status, response = restoreBackupLg(restoreValues, secret)
                self.assertEqual(status, 200, response.decode("utf-8", "replace"))
                self.assertEqual(json.loads(response.decode("utf-8")), {
                    "ok": True,
                    "applied": ["homePromotion"],
                    "unchanged": 1,
                    "skipped": [{"id": "eArcSupport", "reason": "absent"}],
                    "failed": []
                })
                calls = [json.loads(line) for line in details.read_text().splitlines()]
                lgRestoreWrites = [call for call in calls
                                   if call["uri"] ==
                                   "com.webos.service.settings/setSystemSettings" and
                                   call["payload"].get("category") == "general" and
                                   call["payload"].get("settings", {}).get(
                                       "homePromotion") == "on"]
                self.assertEqual(len(lgRestoreWrites), 1,
                                 "native LG restore must write each changed offered setting once")

                def createDaily(name, proxySecret=None, originalRemote=None,
                                contentType="application/json"):
                    headers = {"Content-Type": contentType}
                    if proxySecret is not None:
                        headers["X-NativeTv-Proxy-Secret"] = proxySecret
                    if originalRemote is not None:
                        headers["X-NativeTv-Original-Remote"] = originalRemote
                    request = Request(
                        "http://127.0.0.1:%d/_internal/backup/daily" % publicPort,
                        data=json.dumps({"name": name}).encode("utf-8"),
                        method="POST", headers=headers)
                    try:
                        with urlopen(request, timeout=12) as response:
                            return response.status, response.read()
                    except HTTPError as error:
                        return error.code, error.read()

                status, response = createDaily(savedBackupName)
                self.assertEqual(status, 403, response.decode("utf-8", "replace"))
                status, response = createDaily(savedBackupName, "wrong-secret")
                self.assertEqual(status, 403, response.decode("utf-8", "replace"))
                status, response = createDaily(savedBackupName, secret, "203.0.113.10")
                self.assertEqual(status, 403, response.decode("utf-8", "replace"))
                status, response = createDaily(savedBackupName, secret,
                                               contentType="text/plain")
                self.assertEqual(status, 415, response.decode("utf-8", "replace"))
                status, response = createDaily("../config.json", secret)
                self.assertEqual(status, 400, response.decode("utf-8", "replace"))
                self.assertEqual((backupDir / savedBackupName).read_text(), savedBackupBody,
                                 "invalid names must preserve the existing file")
                generatedBackupName = "nativetv-backup-2026-10-28.json"
                status, response = createDaily(generatedBackupName, secret)
                self.assertEqual(status, 204, response.decode("utf-8", "replace"))
                generatedPath = backupDir / generatedBackupName
                generatedBytes = generatedPath.read_bytes()
                self.assertEqual(generatedPath.stat().st_mode & 0o777,
                                 0o600)
                generated = json.loads(generatedBytes.decode("utf-8"))
                self.assertEqual(generated["format"], "nativetv-backup")
                self.assertEqual(generated["version"], 1)
                self.assertEqual(generated["from"], backup["from"])
                self.assertEqual(generated["lg"], backup["lg"])
                self.assertEqual(generated["home"]["rows"], ["app", "dev", "ch"])
                self.assertEqual(generated["apps"], backup["apps"])
                self.assertEqual((backupDir / savedBackupName).read_text(), savedBackupBody,
                                 "daily creation must not replace an existing date")
                status, response = createDaily(generatedBackupName, secret)
                self.assertEqual(status, 204, response.decode("utf-8", "replace"))
                self.assertEqual(generatedPath.read_bytes(), generatedBytes,
                                 "a duplicate daily check must not rewrite the file")
                with get("/api/backup/saved?name=" + generatedBackupName) as response:
                    self.assertEqual(response.status, 200)
                    self.assertEqual(json.loads(response.read().decode("utf-8")), generated)
                with get("/api/backup/saved/list") as response:
                    self.assertEqual(response.status, 200)
                    self.assertEqual(json.loads(response.read().decode("utf-8")), {
                        "ok": True,
                        "saved": [
                            {"name": generatedBackupName, "date": "2026-10-28",
                             "size": len(generatedBytes)},
                            {"name": savedBackupName, "date": "2026-10-26",
                             "size": len(savedBackupBody.encode("utf-8"))}
                        ]
                    })

                self.assertEqual(contextRequests, [],
                                 "the export and the daily backup need nothing from Node")
                self.assertEqual(forwardedGets, [])
                lunaCalls = [json.loads(line)["uri"]
                             for line in details.read_text().splitlines()]
                self.assertIn("com.webos.service.settings/getSystemSettings", lunaCalls)
                self.assertIn("com.webos.applicationManager/listApps", lunaCalls)

                # With Node unreachable the export and the daily backup carry on.
                state["fail"] = True
                with get("/api/backup") as response:
                    self.assertEqual(response.status, 200)
                    self.assertEqual(json.loads(response.read().decode("utf-8"))["format"],
                                     "nativetv-backup")
                self.assertEqual(forwardedGets, [])
            finally:
                if process.poll() is None:
                    process.send_signal(signal.SIGTERM)
                    process.communicate(timeout=5)
        node.shutdown()
        node.server_close()
        thread.join(timeout=2)

    def test_native_display_version_shows_matching_build_metadata(self):
        # test-version.js: the version shown carries the git build metadata
        # deploy.sh wrote, only when it names the running version and a plain commit.
        binary = Path(sys.argv[1]).resolve()
        fakeLuna = binary.parent / "native-fake-luna-send"
        version = Path(__file__).resolve().parent.parent.joinpath("VERSION").read_text().strip()
        token = "display-version-token"
        publicPort = unused_port_pair()
        nodePort = unused_port_pair((publicPort, publicPort + 1))
        with tempfile.TemporaryDirectory(prefix="nativetv-display-version-") as folder:
            root = Path(folder)
            config = root / "config.json"
            assets = root / "assets"
            assets.mkdir()
            config.write_text(json.dumps({
                "host": "127.0.0.1", "port": publicPort, "token": token, "allowControl": True}))
            environment = os.environ.copy()
            environment["TVWEB_PROXY_SECRET"] = "display-version-secret-0123456789abcdef"
            environment["TVWEB_DATA_DIR"] = str(root)
            environment["TVWEB_LUNA_SEND"] = str(fakeLuna)
            process = subprocess.Popen(
                [str(binary), "--config", str(config), "--heartbeat", str(root / "tvweb.beat"),
                 "--workers", "4", "--listen", "--node-port", str(nodePort),
                 "--assets-root", str(assets)], stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT, text=True, env=environment)
            try:
                def shown():
                    request = Request("http://127.0.0.1:%d/api/home/beat" % publicPort, data=b"{}",
                                      method="POST",
                                      headers={"Authorization": "Bearer " + token,
                                               "Content-Type": "application/json"})
                    with urlopen(request, timeout=10) as response:
                        return json.loads(response.read().decode("utf-8"))["build"].split("@")[0]

                deadline = time.monotonic() + 8
                while time.monotonic() < deadline and process.poll() is None:
                    try:
                        urlopen("http://127.0.0.1:%d/api/caps" % publicPort, timeout=2).read()
                        break
                    except (OSError, URLError):
                        time.sleep(0.03)
                self.assertIsNone(process.poll(), "daemon exited early")
                build = root / "build.json"
                self.assertEqual(shown(), version)
                build.write_text(json.dumps({"version": version, "commit": "55e51e5", "dirty": False}))
                self.assertEqual(shown(), version + "+55e51e5")
                build.write_text(json.dumps({"version": version, "commit": "55e51e5", "dirty": True}))
                self.assertEqual(shown(), version + "+55e51e5.dirty")
                build.write_text(json.dumps({"version": "0.0.1", "commit": "55e51e5", "dirty": True}))
                self.assertEqual(shown(), version, "metadata from another version is stale")
                build.write_text(json.dumps({"version": version, "commit": "../bad"}))
                self.assertEqual(shown(), version, "a commit that is not hex is not shown")
            finally:
                if process.poll() is None:
                    process.send_signal(signal.SIGTERM)
                    try:
                        process.communicate(timeout=6)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.communicate(timeout=3)

    def test_native_restart_waits_for_install_started_meanwhile(self):
        # test-restart-wait.js: a restart after a settings save waits for an
        # install that started in the moment before it, rather than cutting it off.
        binary = Path(sys.argv[1]).resolve()
        fakeLuna = binary.parent / "native-fake-luna-send"
        token = "restart-wait-token"
        secret = "restart-wait-proxy-secret-0123456789abcdef"
        publicPort = unused_port_pair()
        nodePort = unused_port_pair((publicPort, publicPort + 1))
        with tempfile.TemporaryDirectory(prefix="nativetv-restart-wait-") as folder:
            root = Path(folder)
            config = root / "config.json"
            assets = root / "assets"
            assets.mkdir()
            restarted = root / "restarted"
            (root / "tvwebctl").write_text('echo "$@" >> "%s"\n' % restarted)
            config.write_text(json.dumps({
                "host": "127.0.0.1", "port": publicPort, "token": token, "allowControl": True,
                "ntp": {"server": "", "port": 123}}))
            environment = os.environ.copy()
            environment["TVWEB_PROXY_SECRET"] = secret
            environment["TVWEB_DATA_DIR"] = str(root)
            environment["TVWEB_LUNA_SEND"] = str(fakeLuna)
            environment["NATIVETV_FAKE_LUNA_MODE"] = "backup"
            process = subprocess.Popen(
                [str(binary), "--config", str(config), "--heartbeat", str(root / "tvweb.beat"),
                 "--workers", "4", "--listen", "--node-port", str(nodePort),
                 "--assets-root", str(assets)], stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT, text=True, env=environment)
            try:
                def snapshot(state):
                    request = Request(
                        "http://127.0.0.1:%d/_internal/apps/install-job" % publicPort,
                        data=json.dumps({"action": "snapshot", "snapshot": {
                            "jobId": "restart-wait-job", "state": state,
                            "source": "catalog", "appId": "com.example.app",
                            "progress": {"bytes": 1, "total": 2}, "preview": None,
                            "error": None, "result": None}}).encode("utf-8"),
                        method="POST", headers={"Content-Type": "application/json",
                                                "X-NativeTv-Proxy-Secret": secret})
                    with urlopen(request, timeout=8) as response:
                        return response.status

                deadline = time.monotonic() + 8
                while time.monotonic() < deadline and process.poll() is None:
                    try:
                        urlopen("http://127.0.0.1:%d/api/caps" % publicPort, timeout=2).read()
                        break
                    except (OSError, URLError):
                        time.sleep(0.03)
                self.assertIsNone(process.poll(), "daemon exited early")
                self.assertEqual(snapshot("idle"), 200)
                save = Request("http://127.0.0.1:%d/api/backup" % publicPort,
                               data=json.dumps({
                                   "format": "nativetv-backup", "version": 1,
                                   "nativetv": {"ntp": {"server": "time.example"}}}).encode("utf-8"),
                               method="POST", headers={"Authorization": "Bearer " + token,
                                                       "Content-Type": "application/json"})
                with urlopen(save, timeout=30) as response:
                    self.assertTrue(json.loads(response.read().decode("utf-8"))["restart"])
                # An install starts just after the answer, before the restart is due.
                self.assertEqual(snapshot("installing"), 200)
                time.sleep(2.2)
                self.assertFalse(restarted.exists(), "the restart waits while the install runs")
                self.assertEqual(snapshot("idle"), 200)
                deadline = time.monotonic() + 6
                while time.monotonic() < deadline and not restarted.exists():
                    time.sleep(0.05)
                self.assertEqual(restarted.read_text().split(), ["restart"],
                                 "and happens once it has finished")
            finally:
                if process.poll() is None:
                    process.send_signal(signal.SIGTERM)
                    try:
                        process.communicate(timeout=6)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.communicate(timeout=3)

    def test_native_backup_restore_route(self):
        binary = Path(sys.argv[1]).resolve()
        fakeLuna = binary.parent / "native-fake-luna-send"
        token = "backup-restore-token"
        secret = "backup-restore-proxy-secret-0123456789abcdef"
        upstreamRequests = []

        class UpstreamHandler(BaseHTTPRequestHandler):
            def do_POST(self):
                upstreamRequests.append(self.path)
                self.send_response(500)
                self.send_header("Content-Length", "0")
                self.end_headers()

            do_GET = do_POST

            def log_message(self, *_args):
                pass

        publicPort = unused_port_pair()
        nodePort = unused_port_pair((publicPort, publicPort + 1))
        node = HTTPServer(("127.0.0.1", nodePort), UpstreamHandler)
        threading.Thread(target=node.serve_forever, daemon=True).start()
        with tempfile.TemporaryDirectory(prefix="nativetv-backup-restore-") as folder:
            root = Path(folder)
            config = root / "config.json"
            heartbeat = root / "tvweb.beat"
            assets = root / "assets"
            assets.mkdir()
            details = root / "luna-details.jsonl"
            restarted = root / "restarted"
            (root / "tvwebctl").write_text('echo "$@" >> "%s"\n' % restarted)
            (root / "restore-apps.json").write_text(json.dumps({
                "saved": 1, "apps": [{"id": "com.example.old", "title": "Old"}]}))
            config.write_text(json.dumps({
                "host": "127.0.0.1", "port": publicPort, "token": token,
                "allowControl": True, "mqtt": {"password": "keep-this-secret"},
                "ntp": {"server": "", "port": 123}, "custom": {"preserve": 7}
            }))
            environment = os.environ.copy()
            environment["TVWEB_PROXY_SECRET"] = secret
            environment["TVWEB_DATA_DIR"] = str(root)
            environment["TVWEB_LUNA_SEND"] = str(fakeLuna)
            environment["NATIVETV_FAKE_LUNA_MODE"] = "backup"
            environment["NATIVETV_FAKE_LUNA_DETAILS"] = str(details)
            process = subprocess.Popen(
                [str(binary), "--config", str(config), "--heartbeat", str(heartbeat),
                 "--workers", "4", "--listen", "--node-port", str(nodePort),
                 "--assets-root", str(assets)], stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT, text=True, env=environment)
            try:
                def post(body):
                    request = Request("http://127.0.0.1:%d/api/backup" % publicPort,
                                      data=json.dumps(body).encode("utf-8"), method="POST",
                                      headers={"Authorization": "Bearer " + token,
                                               "Content-Type": "application/json"})
                    try:
                        with urlopen(request, timeout=30) as response:
                            return response.status, json.loads(response.read().decode("utf-8"))
                    except HTTPError as error:
                        return error.code, json.loads(error.read().decode("utf-8"))

                deadline = time.monotonic() + 8
                ready = False
                while time.monotonic() < deadline and process.poll() is None:
                    try:
                        urlopen("http://127.0.0.1:%d/api/caps" % publicPort, timeout=2).read()
                        ready = True
                        break
                    except (OSError, URLError):
                        time.sleep(0.03)
                if not ready:
                    if process.poll() is None:
                        process.send_signal(signal.SIGTERM)
                    output, _ = process.communicate(timeout=6)
                    self.fail("native daemon did not start for backup restore:\n" + output[-4000:])

                backup = {
                    "format": "nativetv-backup", "version": 1,
                    "lg": {"homePromotion": True, "soundMode": "movie", "bogus": 1},
                    "apps": [{"id": "com.example.alpha", "title": "Alpha"},
                             {"id": "com.example.zed", "title": "Zed"}],
                    "home": {"rows": ["ch", "app", "dev"], "picture": False},
                    "nativetv": {"ntp": {"server": "time.example"},
                                   "backup": {"daily": True},
                                   "mqtt": {"password": "not-allowed"}}
                }
                status, body = post(backup)
                self.assertEqual(status, 200, body)
                self.assertEqual(body["lg"], {
                    "applied": ["homePromotion"], "unchanged": 1,
                    "skipped": [{"id": "bogus", "reason": "absent"}], "failed": []})
                self.assertEqual(sorted(body["nativetv"]), ["backup.daily", "ntp.server"])
                self.assertTrue(body["restart"])
                self.assertEqual(body["apps"], {"toInstall": [], "manual": [], "present": 2,
                                                "catalogError": None})
                self.assertTrue(body["home"])
                self.assertEqual(list(body), ["ok", "lg", "nativetv", "restart", "apps", "home"])

                saved = json.loads(config.read_text())
                self.assertEqual(saved["ntp"]["server"], "time.example")
                self.assertTrue(saved["backup"]["daily"])
                self.assertEqual(saved["mqtt"], {"password": "keep-this-secret"},
                                 "only the allow-listed settings are restored")
                self.assertEqual(saved["custom"], {"preserve": 7})
                self.assertEqual(saved["token"], token)
                homeDocument = json.loads((root / "home.json").read_text())
                self.assertEqual(homeDocument["rows"], ["ch", "app", "dev"])
                self.assertFalse(homeDocument["picture"])
                self.assertFalse((root / "restore-apps.json").exists(),
                                 "no app left to install clears the pending list")
                calls = [json.loads(line) for line in details.read_text().splitlines()]
                written = [call for call in calls if call["uri"] ==
                           "com.webos.service.settings/setSystemSettings"]
                self.assertEqual(len(written), 1, written)
                self.assertEqual(written[0]["payload"]["settings"], {"homePromotion": "on"})
                deadline = time.monotonic() + 5
                while time.monotonic() < deadline and not restarted.exists():
                    time.sleep(0.05)
                self.assertEqual(restarted.read_text().split(), ["restart"],
                                 "changed Native Tv settings restart the server")

                # The same backup again changes nothing, so nothing restarts.
                restarted.unlink()
                status, body = post(backup)
                self.assertEqual(status, 200, body)
                self.assertEqual(body["nativetv"], [])
                self.assertFalse(body["restart"])
                # The stand-in TV keeps homePromotion off, so it is written again.
                self.assertEqual(body["lg"]["applied"], ["homePromotion"])
                self.assertEqual(body["lg"]["unchanged"], 1)
                time.sleep(1)
                self.assertFalse(restarted.exists())

                status, body = post({"format": "nativetv-backup", "version": 2})
                self.assertEqual(status, 400, body)
                self.assertEqual(body, {
                    "ok": False,
                    "error": "That backup is from a newer Native Tv: update this TV first"})
                status, body = post([1])
                self.assertEqual(status, 400, body)
                self.assertEqual(body["error"], "That file is not a Native Tv backup")

                self.assertEqual(upstreamRequests, [],
                                 "a restore needs nothing from Node")
            finally:
                if process.poll() is None:
                    process.send_signal(signal.SIGTERM)
                try:
                    process.communicate(timeout=6)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.communicate()
                node.shutdown()
                node.server_close()

    def test_native_backup_config_restore_bridge(self):
        binary = Path(sys.argv[1]).resolve()
        publicPort = unused_port_pair()
        nodePort = unused_port_pair((publicPort, publicPort + 1))
        token = "backup-config-route-token"
        secret = "backup-config-proxy-secret-0123456789abcdef"
        with tempfile.TemporaryDirectory(prefix="nativetv-backup-config-") as folder:
            root = Path(folder)
            config = root / "config.json"
            heartbeat = root / "tvweb.beat"
            assets = root / "assets"
            assets.mkdir()
            config.write_text(json.dumps({
                "host": "127.0.0.1", "port": publicPort, "token": token,
                "allowControl": True, "allowPower": False,
                "mqtt": {"host": "broker.local", "password": "keep-this-secret"},
                "logging": {"level": "info", "custom": "keep"},
                "syslog": {"server": "old.local", "port": 514,
                           "hostname": "this-tv", "redact": True},
                "screenTime": {"enabled": False, "dailyMinutes": 0,
                               "apps": {"com.keep.app": 15}},
                "custom": {"preserve": 7}
            }))
            environment = os.environ.copy()
            environment["TVWEB_PROXY_SECRET"] = secret
            environment["TVWEB_DATA_DIR"] = str(root / "data")
            environment["TVWEB_LUNA_SEND"] = str(binary.parent / "native-fake-luna-send")
            process = subprocess.Popen(
                [str(binary), "--config", str(config), "--heartbeat", str(heartbeat),
                 "--workers", "3", "--listen", "--node-port", str(nodePort),
                 "--assets-root", str(assets)], stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT, text=True, env=environment)

            def post(body, headers=None, route="/_internal/backup/config"):
                headers = dict(headers or {})
                request = Request(
                    "http://127.0.0.1:%d%s" % (publicPort, route),
                    data=body, method="POST", headers=headers)
                try:
                    with urlopen(request, timeout=8) as response:
                        return response.status, response.read()
                except HTTPError as error:
                    return error.code, error.read()

            try:
                deadline = time.monotonic() + 8
                ready = False
                while time.monotonic() < deadline and process.poll() is None:
                    request = Request(
                        "http://127.0.0.1:%d/api/caps" % publicPort,
                        headers={"Authorization": "Bearer " + token})
                    try:
                        with urlopen(request, timeout=2):
                            ready = True
                            break
                    except (OSError, URLError):
                        time.sleep(0.03)
                if not ready:
                    if process.poll() is None:
                        process.send_signal(signal.SIGTERM)
                    output, _ = process.communicate(timeout=6)
                    self.fail("native daemon did not start for backup config restore:\n" +
                              output[-4000:])

                validationRoute = "/_internal/backup/validate"
                validationHeaders = {
                    "Content-Type": "application/json",
                    "X-NativeTv-Proxy-Secret": secret
                }
                status, response = post(b'{"format":"nativetv-backup","version":true}', {
                    "Content-Type": "application/json",
                    "X-NativeTv-Proxy-Secret": "wrong-secret"
                }, validationRoute)
                self.assertEqual(status, 403, response.decode("utf-8", "replace"))
                for document, expected in [
                        (None, "notBackup"),
                        ({"format": "other", "version": 1}, "notBackup"),
                        ({"format": "nativetv-backup", "version": True}, "notBackup"),
                        ({"format": "nativetv-backup", "version": 2}, "newer"),
                        ({"format": "nativetv-backup", "version": 1}, "valid")]:
                    status, response = post(json.dumps(document).encode("utf-8"),
                                            validationHeaders, validationRoute)
                    self.assertEqual(status, 200, response.decode("utf-8", "replace"))
                    self.assertEqual(json.loads(response.decode("utf-8")),
                                     {"ok": True, "result": expected})
                status, response = post(b"{", validationHeaders, validationRoute)
                self.assertEqual(status, 400, response.decode("utf-8", "replace"))

                body = json.dumps({"patch": {"logging": {"level": "debug",
                                                           "unknown": True},
                                              "syslog": {"server": "new.local",
                                                         "hostname": "other-tv",
                                                         "port": 70000},
                                              "screenTime": {"enabled": True,
                                                             "dailyMinutes": 75,
                                                             "apps": {"com.example.app": 30},
                                                             "action": "shutdown"},
                                              "token": "must-not-change",
                                              "mqtt": {"password": "must-not-change"}}})
                status, response = post(body.encode("utf-8"), {
                    "Content-Type": "application/json",
                    "X-NativeTv-Proxy-Secret": "wrong-secret"
                })
                self.assertEqual(status, 403, response.decode("utf-8", "replace"))
                status, response = post(body.encode("utf-8"), {
                    "Content-Type": "application/json",
                    "X-NativeTv-Proxy-Secret": secret,
                    "X-NativeTv-Original-Remote": "203.0.113.10"
                })
                self.assertEqual(status, 403, response.decode("utf-8", "replace"))
                status, response = post(body.encode("utf-8"), {
                    "Content-Type": "text/plain",
                    "X-NativeTv-Proxy-Secret": secret
                })
                self.assertEqual(status, 415, response.decode("utf-8", "replace"))

                status, response = post(body.encode("utf-8"), {
                    "Content-Type": "application/json",
                    "X-NativeTv-Proxy-Secret": secret
                })
                self.assertEqual(status, 200, response.decode("utf-8", "replace"))
                self.assertEqual(json.loads(response.decode("utf-8")), {
                    "ok": True,
                    "changed": ["logging.level", "syslog.server",
                                "screenTime.enabled", "screenTime.dailyMinutes",
                                "screenTime.apps"]
                })
                saved = json.loads(config.read_text())
                self.assertEqual(saved["logging"], {"level": "debug", "custom": "keep"})
                self.assertEqual(saved["syslog"], {
                    "server": "new.local", "port": 514,
                    "hostname": "this-tv", "redact": True
                })
                self.assertEqual(saved["screenTime"], {
                    "enabled": True, "dailyMinutes": 75,
                    "apps": {"com.example.app": 30}
                })
                self.assertEqual(saved["token"], token)
                self.assertEqual(saved["host"], "127.0.0.1")
                self.assertEqual(saved["allowPower"], False)
                self.assertEqual(saved["mqtt"], {
                    "host": "broker.local", "password": "keep-this-secret"
                })
                self.assertEqual(saved["custom"], {"preserve": 7})
                self.assertEqual(stat.S_IMODE(config.stat().st_mode), 0o600)
                savedBytes = config.read_bytes()
                status, response = post(body.encode("utf-8"), {
                    "Content-Type": "application/json",
                    "X-NativeTv-Proxy-Secret": secret
                })
                self.assertEqual(status, 200, response.decode("utf-8", "replace"))
                self.assertEqual(json.loads(response.decode("utf-8")),
                                 {"ok": True, "changed": []})
                self.assertEqual(config.read_bytes(), savedBytes,
                                 "an unchanged restore must not rewrite config.json")
            finally:
                if process.poll() is None:
                    process.send_signal(signal.SIGTERM)
                    process.communicate(timeout=6)

    def test_emulated_setup_actions_do_not_change_tv_state(self):
        binary = Path(sys.argv[1]).resolve()
        with tempfile.TemporaryDirectory(prefix="nativetv-setup-demo-") as folder:
            root = Path(folder)
            config = root / "config.json"
            heartbeat = root / "tvweb.beat"
            assets = root / "assets"
            assets.mkdir()
            marker = root / ".setup-pending"
            marker.touch()
            port = unused_port_pair()
            upstreamPort = unused_port_pair((port, port + 1))
            capture = root / "luna-call.json"
            fakeLuna = root / "fake-luna-send"
            fakeLuna.write_text(
                "#!/usr/bin/env python3\n"
                "import json, os, sys\n"
                "with open(os.environ['TVWEB_LUNA_CAPTURE'], 'w') as output:\n"
                "    json.dump(sys.argv[1:], output)\n"
                "print('{\"returnValue\":true}')\n")
            fakeLuna.chmod(0o700)
            config.write_text(json.dumps({"host": "127.0.0.1", "port": port,
                                          "allowControl": False, "emulateSetup": True}))
            environment = os.environ.copy()
            environment["TVWEB_PROXY_SECRET"] = "setup-demo-proxy-secret-for-tests-0123456789abcdef"
            environment["TVWEB_DATA_DIR"] = str(root)
            environment["TVWEB_LUNA_SEND"] = str(fakeLuna)
            environment["TVWEB_LUNA_CAPTURE"] = str(capture)
            process = subprocess.Popen(
                [str(binary), "--config", str(config), "--heartbeat", str(heartbeat),
                 "--workers", "2", "--listen", "--node-port", str(upstreamPort),
                 "--assets-root", str(assets)],
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, env=environment)
            try:
                deadline = time.monotonic() + 5
                ready = False
                while time.monotonic() < deadline and process.poll() is None:
                    try:
                        urlopen("http://127.0.0.1:%d/api/caps" % port, timeout=1).read()
                        ready = True
                        break
                    except (OSError, URLError):
                        time.sleep(0.02)
                self.assertTrue(ready, "native front door did not start for demo setup")

                def post(path, body):
                    request = Request("http://127.0.0.1:%d%s" % (port, path),
                                      data=json.dumps(body).encode("utf-8"),
                                      headers={"Content-Type": "application/json"}, method="POST")
                    with urlopen(request, timeout=3) as response:
                        return json.loads(response.read().decode("utf-8"))

                network = post("/api/setup/network", {"action": "network", "open": True})
                self.assertEqual(network, {"ok": True, "restarting": False, "demo": True})
                self.assertEqual(json.loads(config.read_text())["host"], "127.0.0.1",
                                 "demo setup must not write network configuration")

                completed = post("/api/setup", {"action": "done"})
                self.assertEqual(completed, {"ok": True, "demo": True})
                self.assertTrue(marker.exists(), "demo setup must preserve the pending marker")

                alwaysReady = post("/api/setup/always-ready",
                                   {"action": "alwaysReady", "on": True})
                self.assertEqual(alwaysReady, {"ok": True, "demo": True})
                self.assertFalse(capture.exists(), "demo setup must not call TV settings")

                handoff = post("/api/setup/handoff", {"action": "handoff"})
                self.assertEqual(handoff["ok"], True)
                self.assertEqual(handoff["demo"], True)
                handoffUrl = urlsplit(handoff["url"])
                self.assertEqual(handoffUrl.port, port)
                self.assertEqual(handoffUrl.query, "c=demo")

                stopped = post("/api/setup/handoff-stop", {"action": "handoffStop"})
                self.assertEqual(stopped, {"ok": True, "demo": True})
                self.assertTrue(marker.exists(), "demo setup actions must not finish setup")
                self.assertFalse(capture.exists(), "demo setup must not make Luna calls")
            finally:
                if process.poll() is None:
                    process.send_signal(signal.SIGTERM)
                    process.communicate(timeout=5)

    def test_front_door_auth_proxy_and_shutdown(self):
        binary = Path(sys.argv[1]).resolve()
        sourceAssets = Path(__file__).resolve().parents[1] / "web"
        secret = "runtime-proxy-secret-for-tests-0123456789abcdef"
        observed = []
        handoffObserved = []
        nowMs = int(time.time() * 1000)
        nowHour = time.strftime("%Y-%m-%dT%H", time.gmtime(nowMs / 1000))
        historyOverlay = {
            "enabled": True, "running": True, "readPersisted": True,
            "current": "hdmi2", "clockSet": True,
            "hours": {nowHour: {"netflix": 450, "hdmi2": 120}},
            "titles": {"netflix": "Netflix", "hdmi2": "PlayStation"}
        }

        class CaptureHandler(BaseHTTPRequestHandler):
            def send_capture(self, item, response_payload=None):
                observed.append(item)
                payload = json.dumps(observed[-1] if response_payload is None
                                     else response_payload).encode("utf-8")
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(payload)))
                self.end_headers()
                self.wfile.write(payload)

            def do_GET(self):
                self.send_capture({
                    "path": self.path,
                    "peer": self.headers.get("X-NativeTv-Original-Remote", ""),
                    "secret": self.headers.get("X-NativeTv-Proxy-Secret", ""),
                })

            def do_POST(self):
                length = int(self.headers.get("Content-Length", "0"))
                body = self.rfile.read(length).decode("utf-8")
                if self.path == "/_internal/reload-config":
                    return self.send_capture({
                        "path": self.path,
                        "peer": self.headers.get("X-NativeTv-Original-Remote", ""),
                        "secret": self.headers.get("X-NativeTv-Proxy-Secret", ""),
                        "body": body,
                    }, {"ok": True})
                if self.path == "/_internal/native-stats":
                    nativePayload = json.loads(body)
                    if nativePayload.get("withPrometheus") is True:
                        return self.send_capture({
                            "path": self.path,
                            "peer": self.headers.get("X-NativeTv-Original-Remote", ""),
                            "secret": self.headers.get("X-NativeTv-Proxy-Secret", ""),
                            "body": body,
                        }, {
                            "stats": {
                                "ok": True,
                                "device": {"model": "Test TV"},
                                "system": {"firmware": "1.0", "webos": "9.0"},
                            },
                            "syslog": None,
                            "ntp": None,
                            "watch": None,
                        })
                if self.path == "/_internal/native-history":
                    observed.append({
                        "path": self.path,
                        "peer": self.headers.get("X-NativeTv-Original-Remote", ""),
                        "secret": self.headers.get("X-NativeTv-Proxy-Secret", ""),
                        "body": body,
                    })
                    payload = json.dumps(historyOverlay).encode("utf-8")
                    self.send_response(200)
                    self.send_header("Content-Type", "application/json")
                    self.send_header("Content-Length", str(len(payload)))
                    self.end_headers()
                    self.wfile.write(payload)
                    return
                self.send_capture({
                    "path": self.path,
                    "peer": self.headers.get("X-NativeTv-Original-Remote", ""),
                    "secret": self.headers.get("X-NativeTv-Proxy-Secret", ""),
                    "body": body,
                })

            def log_message(self, *_args):
                pass

        class HandoffCaptureHandler(BaseHTTPRequestHandler):
            def capture(self):
                length = int(self.headers.get("Content-Length", "0"))
                handoffObserved.append({
                    "path": self.path,
                    "peer": self.headers.get("X-NativeTv-Original-Remote", ""),
                    "secret": self.headers.get("X-NativeTv-Proxy-Secret", ""),
                    "body": self.rfile.read(length).decode("utf-8"),
                })
                payload = json.dumps(handoffObserved[-1]).encode("utf-8")
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(payload)))
                self.end_headers()
                self.wfile.write(payload)

            def do_GET(self):
                self.capture()

            def do_POST(self):
                self.capture()

            def log_message(self, *_args):
                pass

        port = unused_port_pair()
        upstreamPort = unused_port_pair((port, port + 1))
        upstream = HTTPServer(("127.0.0.1", upstreamPort), CaptureHandler)
        handoffUpstream = HTTPServer(("127.0.0.1", upstreamPort + 1),
                                     HandoffCaptureHandler)
        upstreamThread = threading.Thread(target=upstream.serve_forever, daemon=True)
        handoffThread = threading.Thread(target=handoffUpstream.serve_forever, daemon=True)
        upstreamThread.start()
        handoffThread.start()
        with tempfile.TemporaryDirectory(prefix="nativetv-front-door-") as folder:
            root = Path(folder)
            historyDirectory = root / "history"
            historyDirectory.mkdir()
            historyMonth = nowHour[:7]
            (historyDirectory / (historyMonth + ".json")).write_text(json.dumps({
                "v": 1,
                "hours": {nowHour: {"netflix": 300, "screen_off": 30}},
                "titles": {"netflix": "Netflix old"}
            }))
            assetsRoot = root / "assets"
            shutil.copytree(str(sourceAssets), str(assetsRoot))
            outside = root / "outside.js"
            outside.write_text("must not be served through an assets symlink")
            (assetsRoot / "ui" / "escape.js").symlink_to(outside)
            procRoot = root / "proc"
            processRoot = procRoot / "4242"
            processRoot.mkdir(parents=True)
            (procRoot / "uptime").write_text("100.0 200.0\n")
            systemLogs = root / "var" / "log"
            systemLogs.mkdir(parents=True)
            (systemLogs / "messages").write_text(
                "2026-10-07T12:00:00.000Z [4.0] user.info system [] system ready\n")
            nativeTvLog = root / "var" / "lib" / "tvweb" / "tvweb.log"
            nativeTvLog.parent.mkdir(parents=True)
            nativeTvLog.write_text(
                "2026-10-07T12:00:01.000Z [5.0] [INFO] app: serialNumber: \"000EXMP0S001\" at 192.168.1.12\n")
            (processRoot / "statm").write_text("1000 512 0 0")
            (processRoot / "cmdline").write_bytes(b"/usr/bin/fixture-proc\0--test")
            hdmiFixtureRoot = (Path(__file__).resolve().parent /
                               "fixtures" / "tv" / "hdmi20-c4")
            for number in range(4):
                receiver = procRoot / "lg" / "hdmi20" / ("port%d" % number)
                receiver.mkdir(parents=True)
                shutil.copyfile(str(hdmiFixtureRoot / ("port%d.status" % number)),
                                str(receiver / "status"))

            def writeCpuSnapshot(user, system, aggregate):
                fields = ["S"] + ["0"] * 19
                fields[11] = str(user)
                fields[12] = str(system)
                fields[19] = "7"
                (processRoot / "stat").write_text(
                    "4242 (fixture-proc) " + " ".join(fields))
                (procRoot / "stat").write_text(aggregate)

            writeCpuSnapshot(100, 50, "cpu 100 100 200 600 0 0 0 0 0 0\n")
            lunaCalls = root / "luna-calls"
            fakeLuna = root / "fake-luna-send"
            fakeLuna.write_text(
                "#!/usr/bin/env python3\n"
                "import json, os, sys\n"
                "args = sys.argv[1:]\n"
                "uri = args[args.index('-f') + 1].replace('luna://', '')\n"
                "with open(os.environ['TVWEB_LUNA_CAPTURE'], 'a') as output:\n"
                "    output.write(uri + '\\n')\n"
                "if uri == 'com.webos.service.config/getConfigs':\n"
                "    reply = {'returnValue': True, 'configs': {"
                "'inputMap.videoInputMapIndexInfo0': [{'assignment': {"
                "'hdmi1': '3', 'hdmi2': '2', 'hdmi3': '1', 'hdmi4': '0'}}]}}\n"
                "elif uri == 'com.webos.service.eim/getAllInputStatus':\n"
                "    reply = {'returnValue': True, 'devices': ["
                "{'id': 'HDMI_1', 'port': 1, 'label': 'Streamer', 'activate': True, 'hdmiPlugIn': True},"
                "{'id': 'HDMI_2', 'port': 2, 'label': 'HDMI 2', 'activate': False},"
                "{'id': 'HDMI_3', 'port': 3, 'label': 'HDMI 3', 'activate': False},"
                "{'id': 'HDMI_4', 'port': 4, 'label': 'PC', 'activate': True, 'hdmiPlugIn': True}] }\n"
                "elif uri == 'com.webos.applicationManager/getForegroundAppInfo':\n"
                "    reply = {'returnValue': True, 'appId': 'com.webos.app.hdmi1'}\n"
                "else:\n"
                "    reply = {'returnValue': False, 'errorText': 'Unknown method'}\n"
                "print(json.dumps(reply))\n")
            fakeLuna.chmod(0o700)
            config = root / "config.json"
            heartbeat = root / "tvweb.beat"
            (root / ".setup-pending").touch()
            (root / ".from-homebrew-channel").write_text("/test/homebrew-channel")
            config.write_text(json.dumps({"host": "127.0.0.1", "port": port,
                                          "token": "front-door-token",
                                          "allowControl": False, "allowPower": True,
                                          "prometheus": {"enabled": True}}))
            environment = os.environ.copy()
            environment["TVWEB_PROXY_SECRET"] = secret
            environment["TVWEB_DATA_DIR"] = str(root)
            environment["TVWEB_LUNA_SEND"] = str(fakeLuna)
            environment["TVWEB_LUNA_CAPTURE"] = str(lunaCalls)
            environment["TVWEB_LOG"] = str(nativeTvLog)
            process = subprocess.Popen(
                [str(binary), "--config", str(config), "--heartbeat", str(heartbeat),
                 "--workers", "2", "--listen", "--node-port", str(upstreamPort),
                 "--assets-root", str(assetsRoot), "--proc-root", str(procRoot),
                 "--system-root", str(root), "--cpu-window-ms", "500"],
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, env=environment)

            try:
                deadline = time.monotonic() + 5
                loopbackResponse = None
                last_error = None
                while time.monotonic() < deadline and process.poll() is None:
                    try:
                        response = urlopen("http://127.0.0.1:%d/api/caps" % port, timeout=1)
                        loopbackResponse = (response.status, response.read(), response.headers)
                        break
                    except (OSError, URLError) as error:
                        last_error = error
                        time.sleep(0.02)
                if loopbackResponse is None:
                    output = "daemon still running"
                    if process.poll() is not None:
                        output, _ = process.communicate(timeout=2)
                    self.fail("front door did not become ready: %r\n%s" % (last_error, output))
                clientConfig = json.loads(
                    (root / "native" / "client-config.json").read_text())
                self.assertEqual(clientConfig, {"port": port},
                                 "the daemon publishes the configured native-client port")
                self.assertEqual(
                    (root / "native" / "client-config.json").stat().st_mode & 0o777,
                    0o644, "the sandboxed client can read its non-secret port config")
                self.assertEqual(loopbackResponse[0], 200,
                                 "a TV-local request should retain the legacy token bypass")
                self.assertEqual(loopbackResponse[2].get("Content-Type"), "application/json")
                self.assertEqual(loopbackResponse[2].get("Cache-Control"), "no-store")
                self.assertEqual(loopbackResponse[2].get("Referrer-Policy"), "no-referrer")
                caps = json.loads(loopbackResponse[1].decode("utf-8"))
                self.assertEqual(caps["allowControl"], False)
                self.assertEqual(caps["allowPower"], True)
                self.assertIsNone(caps["origin"], "loopback-only config must not publish a LAN origin")
                self.assertEqual(caps["version"], "0.118.0")
                self.assertTrue(caps["fromHomebrewChannel"])
                self.assertTrue(caps["setupNeeded"])
                self.assertEqual(caps["key"], "front-door-token",
                                 "the native caps route reveals the token to the TV peer")
                self.assertEqual(len(observed), 0, "native capabilities must not reach Node")

                response = urlopen("http://127.0.0.1:%d/api/processes?all=%%31" % port,
                                    timeout=3)
                processes = json.loads(response.read().decode("utf-8"))
                self.assertEqual(response.status, 200)
                self.assertTrue(processes["ok"], "native process metrics should be available")
                self.assertEqual(processes["count"], 1)
                self.assertEqual(len(processes["top"]), 1,
                                 "all=1 should return every readable process")
                self.assertEqual(processes["top"][0]["pid"], 4242)
                self.assertEqual(processes["top"][0]["name"], "fixture-proc")
                self.assertEqual(len(observed), 0, "native process metrics must not reach Node")

                logsUrl = ("http://127.0.0.1:%d/api/logs?"
                           "sources=system%%2Cnativetv&limit=5&redact=1") % port
                response = urlopen(logsUrl, timeout=3)
                logs = json.loads(response.read().decode("utf-8"))
                self.assertEqual(response.status, 200)
                self.assertEqual(logs["ok"], True)
                self.assertEqual(logs["total"], 2)
                self.assertFalse(logs["incremental"])
                self.assertTrue(logs["sources"]["system"]["available"])
                self.assertTrue(logs["sources"]["nativetv"]["available"])
                self.assertIn("<SERIAL>", logs["entries"][1]["msg"])
                self.assertIn("<IP>", logs["entries"][1]["msg"])
                self.assertEqual(len(observed), 0, "native log reads must not reach Node")

                head = Request(logsUrl, method="HEAD")
                response = urlopen(head, timeout=3)
                self.assertEqual(response.status, 200)
                self.assertEqual(response.read(), b"")
                self.assertEqual(response.headers.get("Content-Type"), "application/json")
                self.assertEqual(len(observed), 0, "native log HEAD must not reach Node")

                response = urlopen("http://127.0.0.1:%d/api/hdmi" % port, timeout=8)
                hdmi = json.loads(response.read().decode("utf-8"))
                self.assertTrue(hdmi["ok"])
                self.assertEqual(hdmi["inputs"][0]["signal"]["port"], 3)
                self.assertEqual(hdmi["inputs"][0]["signal"]["resolution"], "1920x1080")
                self.assertTrue(hdmi["inputs"][0]["active"])
                self.assertIsNone(hdmi["inputs"][1]["signal"])
                self.assertEqual(hdmi["inputs"][3]["signal"]["port"], 0)
                self.assertEqual(hdmi["inputs"][3]["signal"]["refreshHz"], 120)
                self.assertEqual(len(hdmi["ports"]), 4)
                self.assertEqual(len(observed), 0, "native HDMI telemetry must not reach Node")
                urlopen("http://127.0.0.1:%d/api/hdmi" % port, timeout=8).read()
                calls = lunaCalls.read_text().splitlines()
                self.assertEqual(calls.count("com.webos.service.config/getConfigs"), 1,
                                 "the fixed input map is fetched once")

                nodeCallsBeforeCpu = len(observed)

                def advanceCpuFixture():
                    time.sleep(0.15)
                    writeCpuSnapshot(350, 50, "cpu 200 100 300 1400 0 0 0 0 0 0\n")

                cpuFixture = threading.Thread(target=advanceCpuFixture, daemon=True)
                cpuFixture.start()
                response = urlopen("http://127.0.0.1:%d/api/cpu" % port, timeout=3)
                cpuFixture.join(timeout=1)
                cpu = json.loads(response.read().decode("utf-8"))
                self.assertEqual(response.status, 200)
                self.assertEqual(cpu["ok"], True)
                self.assertEqual(cpu["windowMs"], 500)
                self.assertEqual(cpu["active"], 1)
                self.assertEqual(cpu["busy"], 25)
                self.assertEqual(cpu["top"], [{"pid": 4242, "name": "fixture-proc", "pct": 25}])
                self.assertEqual(len(observed), nodeCallsBeforeCpu,
                                 "native CPU metrics must not reach Node")

                (procRoot / "stat").unlink()
                response = urlopen("http://127.0.0.1:%d/api/cpu" % port, timeout=3)
                self.assertEqual(json.loads(response.read().decode("utf-8")),
                                 {"ok": False, "error": "could not read /proc"})
                self.assertEqual(len(observed), nodeCallsBeforeCpu,
                                 "native CPU errors must not reach Node")

                response = urlopen("http://127.0.0.1:%d/api/proxy-test?k=front-door-token" % port,
                                   timeout=3)
                forwarded = json.loads(response.read().decode("utf-8"))
                status = response.status
                self.assertEqual(status, 200)
                self.assertEqual(forwarded["secret"], secret)
                self.assertIn(forwarded["peer"], ("127.0.0.1", "::1", "::ffff:127.0.0.1"))
                self.assertEqual(forwarded["path"], "/api/proxy-test?k=front-door-token")

                def assertRejectedPost(path, contentType, origin, expectedStatus):
                    request = Request("http://127.0.0.1:%d%s" % (port, path), data=b"{}",
                                      headers={"Content-Type": contentType, "Origin": origin},
                                      method="POST")
                    try:
                        urlopen(request, timeout=3)
                        self.fail("guarded POST should be rejected with %d" % expectedStatus)
                    except HTTPError as error:
                        self.assertEqual(error.code, expectedStatus)
                        refusal = json.loads(error.read().decode("utf-8"))
                        self.assertFalse(refusal["ok"])

                assertRejectedPost("/api/control", "text/plain", "http://evil.example", 415)
                assertRejectedPost("/api/control", "application/json; charset=utf-8",
                                   "http://evil.example", 403)
                assertRejectedPost("/api/apps/install/upload",
                                   "application/octet-stream; charset=binary",
                                   "http://127.0.0.1:%d" % port, 415)
                assertRejectedPost("/api/home/wallpapers/upload", "image/jpeg; charset=utf-8",
                                   "http://127.0.0.1:%d" % port, 415)
                self.assertEqual(len(observed), 1, "rejected CSRF requests must not reach Node")

                validPost = Request("http://127.0.0.1:%d/api/control" % port,
                                    data=b"{}",
                                    headers={"Content-Type": "application/json; charset=utf-8",
                                             "Origin": "http://127.0.0.1:%d" % port},
                                    method="POST")
                response = urlopen(validPost, timeout=3)
                answered = json.loads(response.read().decode("utf-8"))
                self.assertFalse(answered["ok"])
                self.assertIn(answered["error"], ("unknown action", "controls disabled in config"))
                self.assertEqual(len(observed), 1,
                                 "a control request is answered by the daemon, not Node")

                setupPost = Request("http://127.0.0.1:%d/api/setup" % port,
                                    data=b'{"action":"not-a-setup-action"}',
                                    headers={"Content-Type": "application/json",
                                             "Origin": "http://evil.example"},
                                    method="POST")
                try:
                    urlopen(setupPost, timeout=3)
                    self.fail("unknown setup actions should be rejected natively")
                except HTTPError as error:
                    self.assertEqual(error.code, 400,
                                     "TV-only setup retains its no-Origin-comparison contract")

                invalidHandoff = Request(
                    "http://127.0.0.1:%d/api/setup/handoff" % port,
                    data=b'{"action":"network"}',
                    headers={"Content-Type": "application/json"}, method="POST")
                try:
                    urlopen(invalidHandoff, timeout=3)
                    self.fail("the native handoff route should reject other setup actions")
                except HTTPError as error:
                    self.assertEqual(error.code, 400)
                invalidHandoffType = Request(
                    "http://127.0.0.1:%d/api/setup/handoff" % port,
                    data=b'{"action":"handoff"}',
                    headers={"Content-Type": "text/plain"}, method="POST")
                try:
                    urlopen(invalidHandoffType, timeout=3)
                    self.fail("the native handoff route should require JSON")
                except HTTPError as error:
                    self.assertEqual(error.code, 415)
                self.assertEqual(len(observed), 1,
                                 "native setup actions must not reach Node")

                clearBackup = Request("http://127.0.0.1:%d/api/backup/apps/clear" % port,
                                      data=b"", method="POST")
                try:
                    urlopen(clearBackup, timeout=3)
                    self.fail("clearing the pending apps needs controls")
                except HTTPError as error:
                    self.assertEqual(error.code, 403)
                    self.assertEqual(json.loads(error.read()), {
                        "ok": False, "error": "controls disabled in config"})

                deniedAlwaysReady = Request(
                    "http://127.0.0.1:%d/api/setup/always-ready" % port,
                    data=b"{\"action\":\"alwaysReady\",\"on\":true}",
                    headers={"Content-Type": "application/json"}, method="POST")
                try:
                    urlopen(deniedAlwaysReady, timeout=3)
                    self.fail("setup controls should be refused when controls are disabled")
                except HTTPError as error:
                    self.assertEqual(error.code, 403)
                self.assertEqual(len(observed), 1,
                                 "a refused native setup control must not reach Node")

                deniedNetwork = Request(
                    "http://127.0.0.1:%d/api/setup/network" % port,
                    data=b'{"action":"network","open":true}',
                    headers={"Content-Type": "application/json"}, method="POST")
                try:
                    urlopen(deniedNetwork, timeout=3)
                    self.fail("network setup should be refused when controls are disabled")
                except HTTPError as error:
                    self.assertEqual(error.code, 403)
                self.assertEqual(len(observed), 1,
                                 "a refused network setup must not reach Node or write config")

                invalidSetupDone = Request("http://127.0.0.1:%d/api/setup/done" % port,
                                           data=b"{\"action\":\"other\"}",
                                           headers={"Content-Type": "application/json"},
                                           method="POST")
                try:
                    urlopen(invalidSetupDone, timeout=3)
                    self.fail("unknown native setup action should be rejected")
                except HTTPError as error:
                    self.assertEqual(error.code, 400)
                self.assertTrue((root / ".setup-pending").exists(),
                                "invalid setup actions must not complete setup")

                oversizedSetupDone = Request("http://127.0.0.1:%d/api/setup/done" % port,
                                              data=b" " * 1025,
                                              headers={"Content-Type": "application/json"},
                                              method="POST")
                try:
                    urlopen(oversizedSetupDone, timeout=3)
                    self.fail("oversized native setup requests should be rejected")
                except HTTPError as error:
                    self.assertEqual(error.code, 413)
                    error.read()
                self.assertTrue((root / ".setup-pending").exists(),
                                "oversized setup requests must not complete setup")

                finishSetup = Request("http://127.0.0.1:%d/api/setup/done" % port,
                                      data=b"{\"action\":\"done\"}",
                                      headers={"Content-Type": "application/json; charset=utf-8"},
                                      method="POST")
                response = urlopen(finishSetup, timeout=3)
                self.assertEqual(response.status, 200)
                self.assertEqual(json.loads(response.read().decode("utf-8")), {"ok": True})
                self.assertFalse((root / ".setup-pending").exists(),
                                 "native setup completion must clear the setup marker")
                self.assertEqual(len(observed), 1,
                                 "native setup completion must not be proxied to Node")

                handoffStart = Request(
                    "http://127.0.0.1:%d/api/setup/handoff" % port,
                    data=b'{"action":"handoff"}',
                    headers={"Content-Type": "application/json",
                             "Origin": "http://evil.example"}, method="POST")
                try:
                    urlopen(handoffStart, timeout=3)
                    self.fail("native handoff start should respect disabled controls")
                except HTTPError as error:
                    self.assertEqual(error.code, 403)

                handoffStop = Request(
                    "http://127.0.0.1:%d/api/setup/handoff-stop" % port,
                    data=b'{"action":"handoffStop"}',
                    headers={"Content-Type": "application/json"}, method="POST")
                try:
                    urlopen(handoffStop, timeout=3)
                    self.fail("native handoff stop should respect disabled controls")
                except HTTPError as error:
                    self.assertEqual(error.code, 403)
                self.assertEqual(len(observed), 1,
                                 "native handoff actions must be handled locally")

                handoffPage = urlopen(
                    "http://127.0.0.1:%d/?c=0123456789abcdef" % (port + 1), timeout=3)
                forwardedPage = json.loads(handoffPage.read().decode("utf-8"))
                self.assertEqual(forwardedPage["path"], "/?c=0123456789abcdef")
                self.assertEqual(forwardedPage["secret"], secret)
                self.assertEqual(len(handoffObserved), 1,
                                 "the native secondary listener must forward the phone page")

                handoffPost = Request(
                    "http://127.0.0.1:%d/mqtt?c=0123456789abcdef" % (port + 1),
                    data=b'{"host":"broker.local"}',
                    headers={"Content-Type": "application/json"}, method="POST")
                forwardedMqtt = urlopen(handoffPost, timeout=3)
                forwardedMqttBody = json.loads(forwardedMqtt.read().decode("utf-8"))
                self.assertEqual(forwardedMqttBody["path"], "/mqtt?c=0123456789abcdef")
                self.assertEqual(forwardedMqttBody["body"], '{"host":"broker.local"}')
                self.assertEqual(len(handoffObserved), 2,
                                 "the Node phone setup fallback must remain available")

                response = urlopen("http://127.0.0.1:%d/api/caps" % port, timeout=3)
                caps = json.loads(response.read().decode("utf-8"))
                self.assertFalse(caps["setupNeeded"],
                                 "capabilities should reflect native setup completion")

                # The first load builds and compresses the page: 0.1 s here, 3 s under ASan.
                response = urlopen("http://127.0.0.1:%d/" % port, timeout=20)
                dashboard = response.read().decode("utf-8")
                self.assertEqual(response.status, 200)
                self.assertEqual(response.headers.get("Cache-Control"), "no-store")
                self.assertNotIn('<link rel="stylesheet" href="/assets/ui/', dashboard)
                self.assertNotIn('<script src="/assets/ui/', dashboard)
                self.assertIn("<style>", dashboard)
                self.assertIn("<script>\n", dashboard)
                self.assertNotIn("ui.css", dashboard)

                compressedRequest = Request("http://127.0.0.1:%d/" % port,
                                            headers={"Accept-Encoding": "gzip;q=0.5"})
                response = urlopen(compressedRequest, timeout=3)
                self.assertEqual(response.headers.get("Content-Encoding"), "gzip")
                self.assertEqual(response.headers.get("Vary"), "Accept-Encoding")
                self.assertIn("<style>", gzip.decompress(response.read()).decode("utf-8"))
                uncompressedRequest = Request("http://127.0.0.1:%d/" % port,
                                              headers={"Accept-Encoding": "gzip;q=0"})
                response = urlopen(uncompressedRequest, timeout=3)
                self.assertIsNone(response.headers.get("Content-Encoding"))
                self.assertIn("<style>", response.read().decode("utf-8"))

                response = urlopen("http://127.0.0.1:%d/assets/ui/core.js" % port, timeout=3)
                coreScript = response.read().decode("utf-8")
                self.assertEqual(response.status, 200)
                self.assertEqual(response.headers.get("Content-Type"), "application/javascript")
                etag = response.headers.get("ETag")
                self.assertTrue(etag)
                self.assertIn("function", coreScript)
                conditional = Request("http://127.0.0.1:%d/assets/ui/core.js" % port,
                                      headers={"If-None-Match": etag})
                try:
                    urlopen(conditional, timeout=3)
                    self.fail("matching static-asset ETag should return 304")
                except HTTPError as error:
                    self.assertEqual(error.code, 304)

                response = urlopen("http://127.0.0.1:%d/assets/ui/core.js?v=1" % port,
                                   timeout=3)
                response.read()
                self.assertEqual(response.headers.get("Cache-Control"),
                                 "public, max-age=31536000, immutable")

                head = Request("http://127.0.0.1:%d/assets/ui/core.js" % port,
                               method="HEAD")
                response = urlopen(head, timeout=3)
                self.assertEqual(response.status, 200)
                self.assertEqual(response.read(), b"")
                self.assertGreater(int(response.headers.get("Content-Length", "0")), 0)

                connection = http.client.HTTPConnection("127.0.0.1", port, timeout=3)
                connection.request("GET", "/assets/dashboard.html?tab=apps")
                response = connection.getresponse()
                self.assertEqual(response.status, 302)
                self.assertEqual(response.getheader("Location"),
                                 "/assets/google-tv/pages/dashboard/index.html?tab=apps")
                response.read()
                connection.close()

                unsafe = "http://127.0.0.1:%d/assets/ui/%%2e%%2e/ui.html" % port
                try:
                    urlopen(unsafe, timeout=3)
                    self.fail("encoded traversal path should be rejected")
                except HTTPError as error:
                    self.assertEqual(error.code, 400)
                self.assertEqual(len(observed), 1, "native static routes must not reach Node")

                response = urlopen("http://127.0.0.1:%d/assets/ui/escape.js" % port, timeout=3)
                forwarded = json.loads(response.read().decode("utf-8"))
                self.assertEqual(forwarded["path"], "/assets/ui/escape.js")
                self.assertEqual(len(observed), 2,
                                 "an asset symlink escaping the root must not be read natively")

                request = Request("http://127.0.0.1:%d/api/hdmi" % port, data=b"{}",
                                  method="POST")
                try:
                    urlopen(request, timeout=3)
                    self.fail("a wrong method on a read-only route should be refused")
                except HTTPError as error:
                    self.assertEqual(error.code, 405)
                self.assertEqual(observed[-1]["path"], "/assets/ui/escape.js",
                                 "wrong methods on native routes do not reach Node")
                self.assertEqual(len(observed), 2)

                request = Request("http://127.0.0.1:%d/api/logs" % port, data=b"{}",
                                  method="POST")
                try:
                    urlopen(request, timeout=3)
                    self.fail("a wrong method on the logs route should be refused")
                except HTTPError as error:
                    self.assertEqual(error.code, 405)
                self.assertEqual(len(observed), 2)

                response = urlopen("http://127.0.0.1:%d/api/stats?with=settings" % port,
                                   timeout=5)
                nativeStats = json.loads(response.read().decode("utf-8"))
                self.assertEqual(response.status, 200)
                self.assertEqual(nativeStats["path"], "/_internal/native-stats")
                self.assertEqual(nativeStats["peer"], "",
                                 "the internal enrichment request must not forward a public peer")
                self.assertEqual(nativeStats["secret"], secret)
                nativePayload = json.loads(nativeStats["body"])
                self.assertTrue(nativePayload["system"]["ok"])
                self.assertTrue(nativePayload["withSettings"])
                self.assertEqual(set(nativePayload["system"]["cpu"]["statsWindow"].keys()),
                                 {"cores", "overall"})
                if nativePayload["system"]["net"] is not None:
                    self.assertIn("rate", nativePayload["system"]["net"])
                self.assertEqual(len(observed), 3)

                request = Request("http://127.0.0.1:%d/api/stats" % port, method="HEAD")
                with urlopen(request, timeout=5) as response:
                    self.assertEqual(response.status, 200)
                    self.assertEqual(response.read(), b"")
                self.assertEqual(observed[-1]["path"], "/_internal/native-stats",
                                 "HEAD stats should use native collection and suppress the body")
                self.assertEqual(len(observed), 4)

                response = urlopen("http://127.0.0.1:%d/api/stats?fast=1" % port,
                                   timeout=3)
                fastStats = json.loads(response.read().decode("utf-8"))
                self.assertEqual(fastStats["path"], "/api/stats?fast=1",
                                 "the initial loading response keeps the Node compatibility path")
                self.assertEqual(len(observed), 5)

                response = urlopen("http://127.0.0.1:%d/api/stats?reader=publish" % port,
                                   timeout=5)
                publishStats = json.loads(response.read().decode("utf-8"))
                self.assertEqual(response.status, 200)
                self.assertEqual(publishStats["path"], "/_internal/native-stats")
                publishPayload = json.loads(publishStats["body"])
                self.assertTrue(publishPayload["system"]["ok"])
                self.assertEqual(set(publishPayload["system"]["cpu"]["statsWindow"].keys()),
                                 {"cores", "overall"})
                if publishPayload["system"]["net"] is not None:
                    self.assertIn("rate", publishPayload["system"]["net"])
                self.assertEqual(len(observed), 6,
                                 "unexpected requests during publish sample: %r" %
                                 [item["path"] for item in observed])

                historyUrl = "http://127.0.0.1:%d/api/history?days=35" % port
                response = urlopen(historyUrl, timeout=5)
                history = json.loads(response.read().decode("utf-8"))
                self.assertEqual(history["ok"], True)
                self.assertEqual(history["enabled"], True)
                self.assertEqual(history["current"], "hdmi2")
                self.assertEqual(history["hours"][nowHour],
                                 {"netflix": 450, "screen_off": 30, "hdmi2": 120})
                self.assertEqual(history["titles"]["netflix"], "Netflix")
                self.assertEqual(history["titles"]["hdmi2"], "PlayStation")
                self.assertEqual(observed[-1]["path"], "/_internal/native-history")
                self.assertEqual(observed[-1]["peer"], "",
                                 "the history overlay request must not forward a public peer")
                self.assertEqual(observed[-1]["secret"], secret)
                self.assertEqual(sum(1 for item in observed
                                     if item["path"] == "/_internal/native-history"), 1)

                request = Request(historyUrl, method="HEAD")
                with urlopen(request, timeout=5) as response:
                    self.assertEqual(response.status, 200)
                    self.assertEqual(response.read(), b"")
                self.assertEqual(observed[-1]["path"], "/_internal/native-history")
                self.assertEqual(sum(1 for item in observed
                                     if item["path"] == "/_internal/native-history"), 2)

                request = Request(historyUrl, data=b"{}", method="POST")
                with urlopen(request, timeout=3) as response:
                    fallback = json.loads(response.read().decode("utf-8"))
                self.assertEqual(fallback["path"], "/api/history?days=35",
                                 "other history methods retain the Node fallback")
                self.assertEqual(observed[-1]["path"], "/api/history?days=35")
                self.assertEqual(sum(1 for item in observed
                                     if item["path"] == "/api/history?days=35"), 1)

                prometheusUrl = "http://127.0.0.1:%d/api/prometheus/metrics" % port
                response = urlopen(prometheusUrl, timeout=5)
                prometheus = response.read().decode("utf-8")
                self.assertEqual(response.status, 200)
                self.assertEqual(response.headers.get("Content-Type"),
                                 "text/plain; version=0.0.4; charset=utf-8")
                self.assertIn("# HELP nativetv_info", prometheus)
                self.assertIn('nativetv_info{model="Test TV"', prometheus)
                self.assertIn('version="0.118.0"', prometheus)
                self.assertEqual(observed[-1]["path"], "/_internal/native-stats")
                self.assertEqual(observed[-1]["peer"], "")
                self.assertEqual(observed[-1]["secret"], secret)
                prometheusPayload = json.loads(observed[-1]["body"])
                self.assertTrue(prometheusPayload["withPrometheus"])
                self.assertTrue(prometheusPayload["system"]["ok"])
                self.assertEqual(set(prometheusPayload["system"]["cpu"]["statsWindow"].keys()),
                                 {"cores", "overall"})

                request = Request(prometheusUrl, method="HEAD")
                with urlopen(request, timeout=5) as response:
                    self.assertEqual(response.status, 200)
                    self.assertEqual(response.read(), b"")
                self.assertEqual(observed[-1]["path"], "/_internal/native-stats",
                                 "HEAD Prometheus should use native collection and suppress its body")

                request = Request(prometheusUrl, data=b"{}", method="POST")
                with urlopen(request, timeout=3) as response:
                    fallback = json.loads(response.read().decode("utf-8"))
                self.assertEqual(fallback["path"], "/api/prometheus/metrics",
                                 "other Prometheus methods retain the Node fallback")
                self.assertEqual(observed[-1]["path"], "/api/prometheus/metrics")

                screentimeUrl = "http://127.0.0.1:%d/api/screentime" % port
                response = urlopen(screentimeUrl, timeout=5)
                screentimeStatus = json.loads(response.read().decode("utf-8"))
                self.assertEqual(screentimeStatus["ok"], True)
                self.assertEqual(screentimeStatus["writable"], False)
                self.assertEqual(screentimeStatus["watchedToday"], 570)
                self.assertEqual(observed[-1]["path"], "/_internal/native-history")
                self.assertEqual(observed[-1]["peer"], "")
                self.assertEqual(observed[-1]["secret"], secret)
                screenTimeHistoryPayload = json.loads(observed[-1]["body"])
                self.assertIn("fromMs", screenTimeHistoryPayload)
                self.assertIn("toMs", screenTimeHistoryPayload)

                request = Request(screentimeUrl, method="HEAD")
                with urlopen(request, timeout=5) as response:
                    self.assertEqual(response.status, 200)
                    self.assertEqual(response.read(), b"")

                request = Request(screentimeUrl, data=b'{"dailyMinutes":30}',
                                  headers={"Content-Type": "application/json"}, method="POST")
                try:
                    urlopen(request, timeout=5)
                    self.fail("a controls-off screen-time POST should be refused")
                except HTTPError as error:
                    self.assertEqual(error.code, 403)
                    self.assertEqual(json.loads(error.read()), {
                        "ok": False, "error": "controls disabled in config"})
                self.assertEqual(observed[-1]["path"], "/_internal/native-history",
                                 "the refusal does not reach Node")

                process.send_signal(signal.SIGTERM)
                output, _ = process.communicate(timeout=5)
                self.assertEqual(process.returncode, 0, output)
                self.assertIn("HTTP front door=active", output)

                configDocument = json.loads(config.read_text())
                configDocument["allowControl"] = True
                config.write_text(json.dumps(configDocument))
                process = subprocess.Popen(
                    [str(binary), "--config", str(config), "--heartbeat", str(heartbeat),
                     "--workers", "2", "--listen", "--node-port", str(upstreamPort),
                     "--assets-root", str(assetsRoot), "--proc-root", str(procRoot),
                     "--system-root", str(root), "--cpu-window-ms", "500"],
                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, env=environment)
                deadline = time.monotonic() + 8
                ready = False
                while time.monotonic() < deadline and process.poll() is None:
                    try:
                        response = urlopen("http://127.0.0.1:%d/api/caps" % port, timeout=1)
                        response.read()
                        ready = True
                        break
                    except (OSError, URLError):
                        time.sleep(0.02)
                if not ready:
                    output, _ = process.communicate(timeout=2) if process.poll() is not None else ("", None)
                    self.fail("second front door did not become ready\n%s" % output)

                screenTimeCallsBeforeInvalid = sum(
                    1 for item in observed if item["path"] == "/_internal/reload-config")
                request = Request(screentimeUrl, data=b'{"apps":[]}',
                                  headers={"Content-Type": "application/json"}, method="POST")
                try:
                    urlopen(request, timeout=5)
                    self.fail("native screen-time validation should reject an array of app limits")
                except HTTPError as response:
                    self.assertEqual(response.code, 400)
                    invalidStatus = json.loads(response.read().decode("utf-8"))
                self.assertEqual(invalidStatus["error"], "invalid apps")
                self.assertEqual(
                    sum(1 for item in observed
                        if item["path"] == "/_internal/reload-config"),
                    screenTimeCallsBeforeInvalid,
                    "invalid screen-time settings are rejected before saving or reloading config")

                request = Request(screentimeUrl, data=b'{"dailyMinutes":30}',
                                  headers={"Content-Type": "application/json"}, method="POST")
                response = urlopen(request, timeout=5)
                savedStatus = json.loads(response.read().decode("utf-8"))
                self.assertEqual(response.status, 200)
                self.assertEqual(savedStatus["settings"]["dailyMinutes"], 30)
                self.assertEqual(json.loads(config.read_text())["screenTime"]["dailyMinutes"], 30)
                self.assertEqual(observed[-1]["path"], "/_internal/native-history")
                self.assertEqual(observed[-1]["peer"], "")
                self.assertEqual(observed[-1]["secret"], secret)
                self.assertEqual(sum(1 for item in observed
                                     if item["path"] == "/_internal/reload-config"), 1,
                                 "native screen-time writes should reload Node config")
                process.send_signal(signal.SIGTERM)
                output, _ = process.communicate(timeout=5)
                self.assertEqual(process.returncode, 0, output)
                self.assertIn("HTTP front door=active", output)
            finally:
                if process.poll() is None:
                    process.kill()
                    process.wait(timeout=3)
                if process.stdout:
                    process.stdout.close()
                upstream.shutdown()
                upstream.server_close()
                handoffUpstream.shutdown()
                handoffUpstream.server_close()
                upstreamThread.join(timeout=3)
                handoffThread.join(timeout=3)

    def test_native_saved_page_routes_and_title_store(self):
        binary = Path(sys.argv[1]).resolve()
        fakeLuna = binary.parent / "native-fake-luna-send"
        publicPort = unused_port_pair()
        nodePort = unused_port_pair((publicPort, publicPort + 1))
        secret = "saved-page-proxy-secret-0123456789abcdef"
        token = "saved-page-route-token"

        class NodeCaptureHandler(BaseHTTPRequestHandler):
            def do_POST(self):
                length = int(self.headers.get("Content-Length", "0"))
                self.rfile.read(length)
                self.send_response(204)
                self.send_header("Content-Length", "0")
                self.end_headers()

            def log_message(self, *_args):
                pass

        node = HTTPServer(("127.0.0.1", nodePort), NodeCaptureHandler)
        nodeThread = threading.Thread(target=node.serve_forever, daemon=True)
        nodeThread.start()

        with tempfile.TemporaryDirectory(prefix="nativetv-native-saved-pages-") as folder:
            root = Path(folder)
            dataRoot = root / "data"
            config = root / "config.json"
            heartbeat = root / "tvweb.beat"
            assets = root / "assets"
            details = root / "luna.jsonl"
            assets.mkdir()
            config.write_text(json.dumps({
                "host": "127.0.0.1", "port": publicPort, "token": token,
                "allowControl": True
            }))
            page = {
                "id": "com.webos.app.browser", "launchPointId": "saved-page-1",
                "lptype": "bookmark", "title": "Browser title",
                "params": {"target": "https://example.com/original", "keep": "yes"}
            }
            otherBookmark = {
                "id": "com.webos.app.settings", "launchPointId": "settings-page-1",
                "lptype": "bookmark", "title": "Not a browser page"
            }
            environment = os.environ.copy()
            environment["TVWEB_LUNA_SEND"] = str(fakeLuna)
            environment["TVWEB_DATA_DIR"] = str(dataRoot)
            environment["TVWEB_PROXY_SECRET"] = secret
            environment["NATIVETV_FAKE_LUNA_MODE"] = "saved-pages"
            environment["NATIVETV_FAKE_LUNA_DETAILS"] = str(details)
            environment["NATIVETV_FAKE_SAVED_PAGE_POINTS"] = json.dumps([page, otherBookmark])
            environment["NATIVETV_FAKE_SAVED_PAGE_ID"] = "saved-page-1"
            process = subprocess.Popen(
                [str(binary), "--config", str(config), "--heartbeat", str(heartbeat),
                 "--workers", "4", "--listen", "--node-port", str(nodePort),
                 "--assets-root", str(assets)],
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                env=environment)
            output = ""

            def request(path, payload, origin=None, contentType="application/json"):
                headers = {
                    "Authorization": "Bearer " + token,
                    "Content-Type": contentType
                }
                if origin is not None:
                    headers["Origin"] = origin
                req = Request("http://127.0.0.1:%d%s" % (publicPort, path),
                              data=json.dumps(payload).encode("utf-8"),
                              method="POST", headers=headers)
                try:
                    with urlopen(req, timeout=10) as response:
                        return response.status, response.read()
                except HTTPError as error:
                    return error.code, error.read()

            try:
                deadline = time.monotonic() + 8
                ready = False
                while time.monotonic() < deadline and process.poll() is None:
                    req = Request("http://127.0.0.1:%d/api/caps" % publicPort,
                                  headers={"Authorization": "Bearer " + token})
                    try:
                        with urlopen(req, timeout=2):
                            ready = True
                            break
                    except (OSError, URLError):
                        time.sleep(0.03)
                if not ready:
                    if process.poll() is None:
                        process.send_signal(signal.SIGTERM)
                    output, _ = process.communicate(timeout=6)
                    self.fail("native daemon did not start:\n" + output[-4000:])

                status, body = request("/api/apps/add-page", {
                    "address": "  example.com/watch?q=1  ", "title": " A   saved  page "
                }, "http://127.0.0.1:%d" % publicPort)
                self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                self.assertEqual(json.loads(body), {
                    "ok": True, "launchPointId": "saved-page-1", "title": "A saved page"
                })

                status, body = request("/api/apps/edit-page", {
                    "launchPointId": "saved-page-1", "title": "Custom name",
                    "address": "www.example.com/watch?v=1"
                })
                self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                self.assertEqual(json.loads(body), {
                    "ok": True, "title": "Custom name",
                    "address": "https://www.example.com/watch?v=1"
                })
                titles = dataRoot / "saved_page_titles.json"
                self.assertEqual(json.loads(titles.read_text()), {
                    "saved-page-1": "Browser title"
                })
                self.assertEqual(titles.stat().st_mode & 0o777, 0o600)

                status, body = request("/api/apps/edit-page", {
                    "launchPointId": "saved-page-1", "title": " "
                })
                self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                self.assertEqual(json.loads(body), {"ok": True, "title": "Browser title"})
                self.assertEqual(json.loads(titles.read_text()), {})

                status, body = request("/api/apps/remove-page", {
                    "launchPointId": "saved-page-1"
                })
                self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                self.assertEqual(json.loads(body), {"ok": True})

                status, body = request("/api/apps/remove-page", {
                    "launchPointId": "settings-page-1"
                })
                self.assertEqual(status, 400)
                self.assertIn("no longer on the TV", json.loads(body)["error"])

                status, body = request("/api/apps/add-page", {
                    "address": "javascript:alert(1)", "title": "bad"
                })
                self.assertEqual(status, 400)
                self.assertIn("enter a web address", json.loads(body)["error"])
                status, body = request("/api/apps/remove-page", {
                    "launchPointId": "saved-page-1"
                }, "http://attacker.example")
                self.assertEqual(status, 403)
                self.assertIn("cross-origin", json.loads(body)["error"])

                calls = [json.loads(line) for line in details.read_text().splitlines()]
                savedPageUris = {
                    "com.webos.applicationManager/addLaunchPoint",
                    "com.webos.applicationManager/listLaunchPoints",
                    "com.webos.applicationManager/updateLaunchPoint",
                    "com.webos.applicationManager/removeLaunchPoint"
                }
                appCalls = [call for call in calls if call["uri"] in savedPageUris]
                self.assertEqual([call["uri"] for call in appCalls], [
                    "com.webos.applicationManager/addLaunchPoint",
                    "com.webos.applicationManager/listLaunchPoints",
                    "com.webos.applicationManager/updateLaunchPoint",
                    "com.webos.applicationManager/listLaunchPoints",
                    "com.webos.applicationManager/updateLaunchPoint",
                    "com.webos.applicationManager/listLaunchPoints",
                    "com.webos.applicationManager/removeLaunchPoint",
                    "com.webos.applicationManager/listLaunchPoints"
                ])
                update = appCalls[2]["payload"]
                self.assertEqual(update["title"], "Custom name")
                self.assertEqual(update["params"], {
                    "target": "https://www.example.com/watch?v=1", "keep": "yes"
                })
                restored = appCalls[4]["payload"]
                self.assertEqual(restored["title"], "Browser title")
                self.assertNotIn("params", restored)

                process.send_signal(signal.SIGTERM)
                output, _ = process.communicate(timeout=6)
                self.assertEqual(process.returncode, 0, output)
            finally:
                if process.poll() is None:
                    process.send_signal(signal.SIGTERM)
                    try:
                        process.communicate(timeout=6)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait(timeout=3)
                node.shutdown()
                node.server_close()
                nodeThread.join(timeout=3)

    def test_native_last_app_internal_bridge(self):
        binary = Path(sys.argv[1]).resolve()
        fakeLuna = binary.parent / "native-fake-luna-send"
        publicPort = unused_port_pair()
        nodePort = unused_port_pair((publicPort, publicPort + 1))
        secret = "last-app-proxy-secret-0123456789abcdef"
        token = "last-app-route-token"

        class NodeCaptureHandler(BaseHTTPRequestHandler):
            def do_POST(self):
                length = int(self.headers.get("Content-Length", "0"))
                self.rfile.read(length)
                self.send_response(204)
                self.send_header("Content-Length", "0")
                self.end_headers()

            def log_message(self, *_args):
                pass

        node = HTTPServer(("127.0.0.1", nodePort), NodeCaptureHandler)
        nodeThread = threading.Thread(target=node.serve_forever, daemon=True)
        nodeThread.start()

        with tempfile.TemporaryDirectory(prefix="nativetv-native-last-app-") as folder:
            root = Path(folder)
            dataRoot = root / "data"
            config = root / "config.json"
            heartbeat = root / "tvweb.beat"
            assets = root / "assets"
            systemRoot = root / "system"
            staging = root / "install-stage"
            assets.mkdir()
            staging.mkdir()
            packageFile = staging / "com.example.app.ipk"
            packageBytes = app_install_ipk()
            packageFile.write_bytes(packageBytes)
            packageSha256 = hashlib.sha256(packageBytes).hexdigest()
            installLog = root / "install-arguments.jsonl"
            installMode = root / "install-mode"
            installMode.write_text("installed")
            installLuna = root / "install-luna-send"
            installLuna.write_text(
                "#!/usr/bin/env python3\n"
                "import json, os, sys, time\n"
                "args = sys.argv[1:]\n"
                "uri = next((arg for arg in args if arg.startswith('luna://')), '')\n"
                "if not uri.endswith('/dev/install'):\n"
                "    fake = os.environ['NATIVETV_TEST_FAKE_LUNA']\n"
                "    os.execv(fake, [fake] + args)\n"
                "with open(os.environ['NATIVETV_TEST_INSTALL_LOG'], 'a') as output:\n"
                "    output.write(json.dumps(args) + '\\n')\n"
                "mode = open(os.environ['NATIVETV_TEST_INSTALL_MODE']).read().strip()\n"
                "if mode == 'failed':\n"
                "    print(json.dumps({'returnValue': False, 'errorText': 'denied'}), flush=True)\n"
                "elif mode == 'error-code':\n"
                "    print(json.dumps({'statusValue': 99, 'details': {'errorCode': -5, 'reason': 'package is corrupt'}}), flush=True)\n"
                "elif mode.startswith('closed') or mode.startswith('timeout'):\n"
                "    print(json.dumps({'returnValue': True, 'statusValue': 5}), flush=True)\n"
                "    if mode.startswith('closed'): raise SystemExit(0)\n"
                "    time.sleep(20)\n"
                "else:\n"
                "    print(json.dumps({'returnValue': True, 'statusValue': 10}), flush=True)\n"
                "    print(json.dumps({'statusValue': 30, 'details': {'packageId': 'com.example.app'}}), flush=True)\n"
                "    time.sleep(20)\n")
            installLuna.chmod(0o700)
            elevateDirectory = (systemRoot / "media/developer/apps/usr/palm/services/"
                                "org.webosbrew.hbchannel.service")
            elevateDirectory.mkdir(parents=True)
            elevateLog = elevateDirectory / "calls.log"
            elevateExecutable = elevateDirectory / "elevate-service"
            elevateExecutable.write_text(
                "#!/bin/sh\n"
                "printf '%s\\n' \"$1\" >> " + shlex.quote(str(elevateLog)) + "\n"
                "if [ \"$1\" = 'com.example.app.two' ]; then\n"
                "  echo denied >&2\n"
                "  exit 7\n"
                "fi\n"
                "exit 0\n")
            elevateExecutable.chmod(0o700)
            config.write_text(json.dumps({
                "host": "127.0.0.1", "port": publicPort, "token": token,
                "allowControl": True
            }))
            environment = os.environ.copy()
            environment["TVWEB_LUNA_SEND"] = str(installLuna)
            environment["TVWEB_DATA_DIR"] = str(dataRoot)
            environment["TVWEB_PROXY_SECRET"] = secret
            environment["TVWEB_INSTALL_STAGING"] = str(staging)
            environment["NATIVETV_TEST_FAKE_LUNA"] = str(fakeLuna)
            environment["NATIVETV_TEST_INSTALL_LOG"] = str(installLog)
            environment["NATIVETV_TEST_INSTALL_MODE"] = str(installMode)
            environment["NATIVETV_FAKE_LUNA_MODE"] = "app-install-recovery"
            process = subprocess.Popen(
                [str(binary), "--config", str(config), "--heartbeat", str(heartbeat),
                 "--workers", "4", "--listen", "--node-port", str(nodePort),
                 "--assets-root", str(assets), "--system-root", str(systemRoot)],
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                env=environment)

            def post(payload, proxySecret=None, originalRemote=None):
                headers = {"Content-Type": "application/json"}
                if proxySecret is not None:
                    headers["X-NativeTv-Proxy-Secret"] = proxySecret
                if originalRemote is not None:
                    headers["X-NativeTv-Original-Remote"] = originalRemote
                req = Request(
                    "http://127.0.0.1:%d/_internal/last-app" % publicPort,
                    data=json.dumps(payload).encode("utf-8"), method="POST", headers=headers)
                try:
                    with urlopen(req, timeout=5) as response:
                        return response.status, response.read()
                except HTTPError as error:
                    return error.code, error.read()

            def postAppsState(payload, proxySecret=None, originalRemote=None):
                headers = {"Content-Type": "application/json"}
                if proxySecret is not None:
                    headers["X-NativeTv-Proxy-Secret"] = proxySecret
                if originalRemote is not None:
                    headers["X-NativeTv-Original-Remote"] = originalRemote
                req = Request(
                    "http://127.0.0.1:%d/_internal/apps-state" % publicPort,
                    data=json.dumps(payload).encode("utf-8"), method="POST", headers=headers)
                try:
                    with urlopen(req, timeout=5) as response:
                        return response.status, response.read()
                except HTTPError as error:
                    return error.code, error.read()

            def postElevation(payload, proxySecret=None, originalRemote=None):
                headers = {"Content-Type": "application/json"}
                if proxySecret is not None:
                    headers["X-NativeTv-Proxy-Secret"] = proxySecret
                if originalRemote is not None:
                    headers["X-NativeTv-Original-Remote"] = originalRemote
                req = Request(
                    "http://127.0.0.1:%d/_internal/apps/elevated" % publicPort,
                    data=json.dumps(payload).encode("utf-8"), method="POST", headers=headers)
                try:
                    with urlopen(req, timeout=5) as response:
                        return response.status, response.read()
                except HTTPError as error:
                    return error.code, error.read()

            def postInstallerJob(payload, proxySecret=None, originalRemote=None):
                headers = {"Content-Type": "application/json"}
                if proxySecret is not None:
                    headers["X-NativeTv-Proxy-Secret"] = proxySecret
                if originalRemote is not None:
                    headers["X-NativeTv-Original-Remote"] = originalRemote
                req = Request(
                    "http://127.0.0.1:%d/_internal/apps/install-job" % publicPort,
                    data=json.dumps(payload).encode("utf-8"), method="POST", headers=headers)
                try:
                    with urlopen(req, timeout=5) as response:
                        return response.status, response.read()
                except HTTPError as error:
                    return error.code, error.read()

            def postAppElevationRun(payload, proxySecret=None, originalRemote=None):
                headers = {"Content-Type": "application/json"}
                if proxySecret is not None:
                    headers["X-NativeTv-Proxy-Secret"] = proxySecret
                if originalRemote is not None:
                    headers["X-NativeTv-Original-Remote"] = originalRemote
                req = Request(
                    "http://127.0.0.1:%d/_internal/apps/elevate" % publicPort,
                    data=json.dumps(payload).encode("utf-8"), method="POST", headers=headers)
                try:
                    with urlopen(req, timeout=5) as response:
                        return response.status, response.read()
                except HTTPError as error:
                    return error.code, error.read()

            def postAppInstallRun(payload, proxySecret=None, originalRemote=None):
                headers = {"Content-Type": "application/json"}
                if proxySecret is not None:
                    headers["X-NativeTv-Proxy-Secret"] = proxySecret
                if originalRemote is not None:
                    headers["X-NativeTv-Original-Remote"] = originalRemote
                req = Request(
                    "http://127.0.0.1:%d/_internal/apps/install/run" % publicPort,
                    data=json.dumps(payload).encode("utf-8"), method="POST", headers=headers)
                try:
                    with urlopen(req, timeout=8) as response:
                        return response.status, response.read()
                except HTTPError as error:
                    return error.code, error.read()

            def postInstalledApps(proxySecret=None, originalRemote=None):
                headers = {"Content-Type": "application/json"}
                if proxySecret is not None:
                    headers["X-NativeTv-Proxy-Secret"] = proxySecret
                if originalRemote is not None:
                    headers["X-NativeTv-Original-Remote"] = originalRemote
                req = Request(
                    "http://127.0.0.1:%d/_internal/apps/install/apps" % publicPort,
                    data=b"{}", method="POST", headers=headers)
                try:
                    with urlopen(req, timeout=8) as response:
                        return response.status, response.read()
                except HTTPError as error:
                    return error.code, error.read()

            def postAppInspect(payload, proxySecret=None, originalRemote=None):
                headers = {"Content-Type": "application/json"}
                if proxySecret is not None:
                    headers["X-NativeTv-Proxy-Secret"] = proxySecret
                if originalRemote is not None:
                    headers["X-NativeTv-Original-Remote"] = originalRemote
                req = Request(
                    "http://127.0.0.1:%d/_internal/apps/inspect" % publicPort,
                    data=json.dumps(payload).encode("utf-8"), method="POST", headers=headers)
                try:
                    with urlopen(req, timeout=8) as response:
                        return response.status, response.read()
                except HTTPError as error:
                    return error.code, error.read()

            try:
                deadline = time.monotonic() + 8
                ready = False
                while time.monotonic() < deadline and process.poll() is None:
                    req = Request("http://127.0.0.1:%d/api/caps" % publicPort,
                                  headers={"Authorization": "Bearer " + token})
                    try:
                        with urlopen(req, timeout=2):
                            ready = True
                            break
                    except (OSError, URLError):
                        time.sleep(0.03)
                if not ready:
                    if process.poll() is None:
                        process.send_signal(signal.SIGTERM)
                    output, _ = process.communicate(timeout=6)
                    self.fail("native daemon did not start:\n" + output[-4000:])

                path = dataRoot / "last_app.json"
                status, body = post({"app": "Netflix", "app_id": "netflix"})
                self.assertEqual(status, 403, body.decode("utf-8", "replace"))
                status, body = post({"app": "Netflix", "app_id": "netflix"}, "wrong-secret")
                self.assertEqual(status, 403, body.decode("utf-8", "replace"))
                status, body = post(
                    {"app": "Netflix", "app_id": "netflix"}, secret, "192.0.2.20")
                self.assertEqual(status, 403, body.decode("utf-8", "replace"))
                self.assertFalse(path.exists(), "refused requests must not persist a last app")

                status, body = post(
                    {"app": "Netflix", "app_id": "netflix", "ignored": "drop"}, secret)
                self.assertEqual(status, 204, body.decode("utf-8", "replace"))
                self.assertEqual(json.loads(path.read_text()), {
                    "app": "Netflix", "app_id": "netflix"
                })
                self.assertEqual(path.stat().st_mode & 0o777, 0o600)

                status, body = post({"app": "YouTube"}, secret)
                self.assertEqual(status, 400, body.decode("utf-8", "replace"))
                self.assertEqual(json.loads(path.read_text()), {
                    "app": "Netflix", "app_id": "netflix"
                }, "invalid snapshots must preserve the last good file")

                status, body = post({"app": None, "app_id": "youtube"}, secret)
                self.assertEqual(status, 204, body.decode("utf-8", "replace"))
                self.assertEqual(json.loads(path.read_text()), {
                    "app": None, "app_id": "youtube"
                })

                hiddenPath = dataRoot / "hidden_apps"
                flagPath = dataRoot / "tile_hiding_enabled"
                appsState = {"hiddenApps": ["z.app", "a-app"], "tileHidingEnabled": True}
                status, body = postAppsState(appsState)
                self.assertEqual(status, 403, body.decode("utf-8", "replace"))
                status, body = postAppsState(appsState, secret, "192.0.2.20")
                self.assertEqual(status, 403, body.decode("utf-8", "replace"))
                self.assertFalse(hiddenPath.exists(), "refused requests must not persist apps state")
                status, body = postAppsState(appsState, secret)
                self.assertEqual(status, 204, body.decode("utf-8", "replace"))
                self.assertEqual(hiddenPath.read_text(), "a-app\nz.app\n")
                self.assertEqual(flagPath.read_text(), "1\n")
                self.assertEqual(hiddenPath.stat().st_mode & 0o777, 0o600)
                self.assertEqual(flagPath.stat().st_mode & 0o777, 0o600)

                invalidState = {"hiddenApps": ["../escape"], "tileHidingEnabled": False}
                status, body = postAppsState(invalidState, secret)
                self.assertEqual(status, 400, body.decode("utf-8", "replace"))
                self.assertEqual(hiddenPath.read_text(), "a-app\nz.app\n",
                                 "invalid ids must preserve the previous list")
                self.assertEqual(flagPath.read_text(), "1\n",
                                 "invalid ids must preserve the previous flag")

                elevatedPath = dataRoot / "elevated.json"
                readElevation = {"action": "read", "package": "com.example.app"}
                status, body = postElevation(readElevation)
                self.assertEqual(status, 403, body.decode("utf-8", "replace"))
                status, body = postElevation(readElevation, secret, "192.0.2.20")
                self.assertEqual(status, 403, body.decode("utf-8", "replace"))
                status, body = postElevation(readElevation, secret)
                self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                self.assertEqual(json.loads(body), {"ok": True, "services": []})
                writeElevation = {
                    "action": "write", "package": "com.example.app",
                    "services": ["com.example.app.service", "com.example.app.worker"]
                }
                status, body = postElevation(writeElevation, secret)
                self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                self.assertEqual(json.loads(body), {"ok": True})
                self.assertEqual(json.loads(elevatedPath.read_text()), {
                    "com.example.app": ["com.example.app.service", "com.example.app.worker"]
                })
                self.assertEqual(elevatedPath.stat().st_mode & 0o777, 0o600)
                status, body = postElevation(readElevation, secret)
                self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                self.assertEqual(json.loads(body)["services"], writeElevation["services"])
                helperBackup = elevateExecutable.with_name("elevate-service.available")
                elevateExecutable.rename(helperBackup)
                status, body = postElevation(readElevation, secret)
                self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                self.assertEqual(json.loads(body)["services"], [],
                                 "saved elevations are ignored while the helper is unavailable")
                helperBackup.rename(elevateExecutable)
                status, body = postElevation(readElevation, secret)
                self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                self.assertEqual(json.loads(body)["services"], writeElevation["services"])
                invalidElevation = dict(writeElevation)
                invalidElevation["services"] = ["../outside"]
                status, body = postElevation(invalidElevation, secret)
                self.assertEqual(status, 400, body.decode("utf-8", "replace"))
                self.assertEqual(json.loads(elevatedPath.read_text()), {
                    "com.example.app": ["com.example.app.service", "com.example.app.worker"]
                }, "invalid service names must preserve the previous elevation state")

                elevationRequest = {
                    "package": "com.example.app",
                    "services": ["com.example.app.one", "com.example.app.two"]
                }
                status, body = postAppElevationRun(elevationRequest)
                self.assertEqual(status, 403, body.decode("utf-8", "replace"))
                status, body = postAppElevationRun(elevationRequest, secret, "192.0.2.20")
                self.assertEqual(status, 403, body.decode("utf-8", "replace"))
                status, body = postAppElevationRun(elevationRequest, secret)
                self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                self.assertEqual(json.loads(body), {
                    "ok": True, "done": ["com.example.app.one"],
                    "failed": [{"service": "com.example.app.two", "error": "denied"}]
                })
                self.assertEqual(elevateLog.read_text(),
                                 "com.example.app.one\ncom.example.app.two\n")
                invalidElevationRequest = {
                    "package": "com.example.app", "services": ["../outside"]
                }
                status, body = postAppElevationRun(invalidElevationRequest, secret)
                self.assertEqual(status, 400, body.decode("utf-8", "replace"))
                self.assertEqual(elevateLog.read_text(),
                                 "com.example.app.one\ncom.example.app.two\n",
                                 "invalid service names must not execute the helper")

                installRequest = {
                    "package": "com.example.app", "file": packageFile.name,
                    "sha256": packageSha256, "timeoutMs": 5000
                }
                inspectRequest = {
                    "file": packageFile.name,
                    "expectedPackageId": "com.example.app",
                    "expectedSha256": packageSha256
                }
                status, body = postAppInspect(inspectRequest)
                self.assertEqual(status, 403, body.decode("utf-8", "replace"))
                status, body = postAppInspect(inspectRequest, secret)
                self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                self.assertTrue(json.loads(body)["ok"])
                wrongDigest = dict(inspectRequest)
                wrongDigest["expectedSha256"] = "0" * 64
                status, body = postAppInspect(wrongDigest, secret)
                self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                self.assertEqual(json.loads(body)["code"], "hash-mismatch")
                wrongSelection = dict(inspectRequest)
                wrongSelection["expectedPackageId"] = "com.example.other"
                status, body = postAppInspect(wrongSelection, secret)
                self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                self.assertEqual(json.loads(body)["code"], "catalog-id-mismatch")

                status, body = postAppInstallRun(installRequest)
                self.assertEqual(status, 403, body.decode("utf-8", "replace"))
                status, body = postAppInstallRun(installRequest, secret, "192.0.2.20")
                self.assertEqual(status, 403, body.decode("utf-8", "replace"))
                self.assertFalse(installLog.exists(), "refused requests must not start luna-send")
                invalidInstall = dict(installRequest)
                invalidInstall["file"] = "../outside.ipk"
                status, body = postAppInstallRun(invalidInstall, secret)
                self.assertEqual(status, 400, body.decode("utf-8", "replace"))
                invalidInstall = dict(installRequest)
                invalidInstall["package"] = "com.other.app"
                status, body = postAppInstallRun(invalidInstall, secret)
                self.assertEqual(status, 400, body.decode("utf-8", "replace"))
                invalidInstall = dict(installRequest)
                invalidInstall["sha256"] = "0" * 64
                status, body = postAppInstallRun(invalidInstall, secret)
                self.assertEqual(status, 400, body.decode("utf-8", "replace"))
                self.assertFalse(installLog.exists(), "invalid staged packages must not install")

                startedAt = time.monotonic()
                status, body = postAppInstallRun(installRequest, secret)
                self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                self.assertEqual(json.loads(body), {"ok": True, "outcome": "installed"})
                self.assertLess(time.monotonic() - startedAt, 3,
                                "terminal install frames should cancel luna-send promptly")
                args = json.loads(installLog.read_text().splitlines()[0])
                self.assertEqual(args[:2], [
                    "-i", "luna://com.webos.appInstallService/dev/install"])
                installPayload = json.loads(args[2])
                self.assertEqual(installPayload, {
                    "id": "com.example.app", "ipkUrl": str(packageFile), "subscribe": True
                })

                installMode.write_text("failed")
                status, body = postAppInstallRun(installRequest, secret)
                self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                self.assertEqual(json.loads(body), {
                    "ok": True, "outcome": "failed", "error": "denied"
                })
                installMode.write_text("error-code")
                status, body = postAppInstallRun(installRequest, secret)
                self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                self.assertEqual(json.loads(body), {
                    "ok": True, "outcome": "failed", "error": "package is corrupt"
                })
                installMode.write_text("closed")
                status, body = postAppInstallRun(installRequest, secret)
                self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                self.assertEqual(json.loads(body), {"ok": True, "outcome": "closed"})
                installMode.write_text("closed-recovered")
                status, body = postAppInstallRun(installRequest, secret)
                self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                self.assertEqual(json.loads(body), {"ok": True, "outcome": "installed"})
                timeoutInstall = dict(installRequest)
                timeoutInstall["timeoutMs"] = 1000
                installMode.write_text("timeout")
                status, body = postAppInstallRun(timeoutInstall, secret)
                self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                self.assertEqual(json.loads(body), {"ok": True, "outcome": "timeout"})
                installMode.write_text("timeout-recovered")
                status, body = postAppInstallRun(timeoutInstall, secret)
                self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                self.assertEqual(json.loads(body), {"ok": True, "outcome": "installed"})
                self.assertEqual(len(installLog.read_text().splitlines()), 7)

                status, body = postInstalledApps()
                self.assertEqual(status, 403, body.decode("utf-8", "replace"))
                status, body = postInstalledApps(secret, "192.0.2.20")
                self.assertEqual(status, 403, body.decode("utf-8", "replace"))
                status, body = postInstalledApps(secret)
                self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                self.assertEqual(json.loads(body), {
                    "ok": True,
                    "apps": [{"id": "com.example.app", "version": "1.2.3"}]
                })

                installJobPath = dataRoot / "install-job.json"
                readJob = {"action": "read"}
                status, body = postInstallerJob(readJob)
                self.assertEqual(status, 403, body.decode("utf-8", "replace"))
                status, body = postInstallerJob(readJob, "wrong-secret")
                self.assertEqual(status, 403, body.decode("utf-8", "replace"))
                status, body = postInstallerJob(readJob, secret, "192.0.2.20")
                self.assertEqual(status, 403, body.decode("utf-8", "replace"))
                self.assertFalse(installJobPath.exists(),
                                 "refused requests must not create installer state")
                status, body = postInstallerJob(readJob, secret)
                self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                self.assertEqual(json.loads(body), {"ok": True, "found": False, "job": None})

                installJob = {
                    "jobId": "7e48a92c", "state": "downloading",
                    "package": "com.example.app", "version": "1.2.3",
                    "updated": 1700000000000
                }
                status, body = postInstallerJob({"action": "write", "job": installJob}, secret)
                self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                self.assertEqual(json.loads(body), {"ok": True})
                self.assertEqual(json.loads(installJobPath.read_text()), installJob)
                self.assertEqual(installJobPath.stat().st_mode & 0o777, 0o600)
                status, body = postInstallerJob(readJob, secret)
                self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                self.assertEqual(json.loads(body), {"ok": True, "found": True, "job": installJob})
                skippedJob = dict(installJob)
                skippedJob["state"] = "installing"
                status, body = postInstallerJob({
                    "action": "write", "job": skippedJob
                }, secret)
                self.assertEqual(status, 400,
                                 "managed persistence must reject skipped install states")
                self.assertEqual(json.loads(installJobPath.read_text()), installJob,
                                 "a rejected transition must preserve the last good record")

                verifyingJob = dict(installJob)
                verifyingJob["state"] = "verifying"
                status, body = postInstallerJob(
                    {"action": "write", "job": verifyingJob}, secret)
                self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                previewJob = dict(verifyingJob)
                previewJob["state"] = "awaiting-confirm"
                status, body = postInstallerJob({"action": "write", "job": previewJob}, secret)
                self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                status, body = postInstallerJob({"action": "recover"}, secret)
                self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                self.assertEqual(json.loads(body), {"ok": True, "found": False, "job": None})
                self.assertEqual(json.loads(installJobPath.read_text())["state"],
                                 "awaiting-confirm",
                                 "a preview must not be mistaken for an interrupted install")
                activeJob = dict(previewJob)
                activeJob["state"] = "installing"
                status, body = postInstallerJob({"action": "write", "job": activeJob}, secret)
                self.assertEqual(status, 200, body.decode("utf-8", "replace"))

                status, body = postInstallerJob({"action": "recover"})
                self.assertEqual(status, 403, body.decode("utf-8", "replace"))
                status, body = postInstallerJob({"action": "recover"}, secret,
                                                 "192.0.2.20")
                self.assertEqual(status, 403, body.decode("utf-8", "replace"))
                self.assertEqual(json.loads(installJobPath.read_text())["state"],
                                 "installing",
                                 "unauthorized recovery must not mutate installer state")
                status, body = postInstallerJob({"action": "recover"}, secret)
                self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                recoveredJob = json.loads(body)
                self.assertTrue(recoveredJob["found"])
                self.assertEqual(recoveredJob["job"]["state"], "interrupted")
                self.assertGreaterEqual(recoveredJob["job"]["updated"],
                                        installJob["updated"])
                self.assertEqual(json.loads(installJobPath.read_text())["state"],
                                 "interrupted")
                status, body = postInstallerJob({"action": "recover"}, secret)
                self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                self.assertEqual(json.loads(body), {"ok": True, "found": False, "job": None})
                self.assertFalse(installJobPath.exists(),
                                 "an interrupted job should be consumed once")
                installJob = dict(installJob)
                installJob["jobId"] = "new-job-id"
                status, body = postInstallerJob({"action": "write", "job": installJob}, secret)
                self.assertEqual(status, 200, body.decode("utf-8", "replace"))

                invalidJob = dict(installJob)
                invalidJob["jobId"] = ""
                status, body = postInstallerJob({"action": "write", "job": invalidJob}, secret)
                self.assertEqual(status, 400, body.decode("utf-8", "replace"))
                self.assertEqual(json.loads(installJobPath.read_text()), installJob,
                                 "invalid writes must preserve the last good installer job")
                (dataRoot / "install-job.json.last-good").write_text("{}")
                status, body = postInstallerJob({"action": "clear"}, secret)
                self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                self.assertEqual(json.loads(body), {"ok": True})
                self.assertFalse(installJobPath.exists())
                self.assertFalse((dataRoot / "install-job.json.last-good").exists())

                process.send_signal(signal.SIGTERM)
                output, _ = process.communicate(timeout=6)
                self.assertEqual(process.returncode, 0, output)
            finally:
                if process.poll() is None:
                    process.send_signal(signal.SIGTERM)
                    try:
                        process.communicate(timeout=6)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait(timeout=3)
                node.shutdown()
                node.server_close()
                nodeThread.join(timeout=3)

    def test_native_app_icon_route(self):
        binary = Path(sys.argv[1]).resolve()
        fakeLuna = binary.parent / "native-fake-luna-send"
        publicPort = unused_port_pair()
        nodePort = unused_port_pair((publicPort, publicPort + 1, publicPort + 2))
        token = "app-icon-route-token"
        secret = "app-icon-proxy-secret-0123456789abcdef"

        class NodeCaptureHandler(BaseHTTPRequestHandler):
            def do_POST(self):
                length = int(self.headers.get("Content-Length", "0"))
                self.rfile.read(length)
                self.send_response(204)
                self.send_header("Content-Length", "0")
                self.end_headers()

            def log_message(self, *_args):
                pass

        node = HTTPServer(("127.0.0.1", nodePort), NodeCaptureHandler)
        nodeThread = threading.Thread(target=node.serve_forever, daemon=True)
        nodeThread.start()

        with tempfile.TemporaryDirectory(prefix="nativetv-native-app-icon-") as folder:
            root = Path(folder)
            systemRoot = root / "system"
            appsRoot = systemRoot / "media/developer/apps/usr/palm/applications"
            app = appsRoot / "com.example.app"
            escapedApp = appsRoot / "com.example.escape"
            assets = root / "assets"
            dataRoot = root / "data"
            config = root / "config.json"
            heartbeat = root / "tvweb.beat"
            iconBytes = b"\x89PNG\r\n\x1a\nicon"
            appAssets = app / "assets"
            appAssets.mkdir(parents=True)
            escapedApp.mkdir(parents=True)
            assets.mkdir()
            (app / "appinfo.json").write_text(json.dumps({
                "largeIcon": "assets/icon.png"
            }))
            (appAssets / "icon.png").write_bytes(iconBytes)
            outside = root / "secret.json"
            outside.write_text('{"token":"private"}')
            (escapedApp / "appinfo.json").write_text(json.dumps({"icon": "escape.png"}))
            (escapedApp / "escape.png").symlink_to(outside)
            config.write_text(json.dumps({
                "host": "127.0.0.1", "port": publicPort, "token": token,
                "allowControl": True, "iconServer": True
            }))
            environment = os.environ.copy()
            environment["TVWEB_LUNA_SEND"] = str(fakeLuna)
            environment["TVWEB_DATA_DIR"] = str(dataRoot)
            environment["TVWEB_PROXY_SECRET"] = secret
            process = subprocess.Popen(
                [str(binary), "--config", str(config), "--heartbeat", str(heartbeat),
                 "--workers", "4", "--listen", "--node-port", str(nodePort),
                 "--assets-root", str(assets), "--system-root", str(systemRoot)],
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                env=environment)

            def request(path, method="GET", authorization=token, extraHeaders=None):
                headers = {}
                if authorization:
                    headers["Authorization"] = "Bearer " + authorization
                if extraHeaders:
                    headers.update(extraHeaders)
                req = Request("http://127.0.0.1:%d%s" % (publicPort, path),
                              method=method, headers=headers)
                try:
                    with urlopen(req, timeout=5) as response:
                        return response.status, response.headers, response.read()
                except HTTPError as error:
                    return error.code, error.headers, error.read()

            def requestLan(path, method="GET", extraHeaders=None):
                req = Request("http://127.0.0.1:%d%s" % (publicPort + 2, path),
                              method=method, headers=extraHeaders or {})
                try:
                    with urlopen(req, timeout=5) as response:
                        return response.status, response.headers, response.read()
                except HTTPError as error:
                    return error.code, error.headers, error.read()

            try:
                deadline = time.monotonic() + 8
                ready = False
                while time.monotonic() < deadline and process.poll() is None:
                    req = Request("http://127.0.0.1:%d/api/caps" % publicPort,
                                  headers={"Authorization": "Bearer " + token})
                    try:
                        with urlopen(req, timeout=2):
                            ready = True
                            break
                    except (OSError, URLError):
                        time.sleep(0.03)
                if not ready:
                    if process.poll() is None:
                        process.send_signal(signal.SIGTERM)
                    output, _ = process.communicate(timeout=6)
                    self.fail("native daemon did not start:\n" + output[-4000:])

                iconPath = "/api/apps/icon?id=com.example.app"
                status, headers, body = request(iconPath)
                self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                self.assertEqual(body, iconBytes)
                self.assertEqual(headers.get("Content-Type"), "image/png")
                self.assertEqual(headers.get("Cache-Control"), "public, max-age=86400")
                self.assertTrue(headers.get("ETag"))

                status, headHeaders, body = request(iconPath, method="HEAD")
                self.assertEqual(status, 200)
                self.assertEqual(body, b"")
                self.assertEqual(headHeaders.get("Content-Length"), str(len(iconBytes)))
                self.assertEqual(headHeaders.get("ETag"), headers.get("ETag"))

                status, _notModifiedHeaders, body = request(
                    iconPath, extraHeaders={"If-None-Match": headers.get("ETag")})
                self.assertEqual(status, 304)
                self.assertEqual(body, b"")

                status, _headers, body = request(iconPath, authorization=None)
                self.assertEqual(status, 200,
                                 "loopback requests may use the daemon's local token bypass: " +
                                 body.decode("utf-8", "replace"))
                self.assertEqual(body, iconBytes)
                status, _headers, body = request(
                    "/api/apps/icon?id=com.example.escape")
                self.assertEqual(status, 404, body.decode("utf-8", "replace"))
                status, _headers, body = request(
                    "/api/apps/icon?id=..%2F..%2Fsecret")
                self.assertEqual(status, 404, body.decode("utf-8", "replace"))
                status, _headers, body = request(
                    "/api/apps/icon?id=com.example.app&id=com.example.escape")
                self.assertEqual(status, 404, body.decode("utf-8", "replace"))

                status, lanHeaders, body = requestLan("/icons/com.example.app.png")
                self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                self.assertEqual(body, iconBytes)
                self.assertEqual(lanHeaders.get("Cache-Control"), "public, max-age=86400")
                lanEtag = lanHeaders.get("ETag")
                self.assertTrue(lanEtag)
                status, headHeaders, body = requestLan(
                    "/icons/com.example.app.png", method="HEAD")
                self.assertEqual(status, 200)
                self.assertEqual(body, b"")
                self.assertEqual(headHeaders.get("Content-Length"), str(len(iconBytes)))
                status, _headers, body = requestLan(
                    "/icons/com.example.app.png", extraHeaders={"If-None-Match": lanEtag})
                self.assertEqual(status, 304)
                self.assertEqual(body, b"")
                status, _headers, body = requestLan("/icons/com.example.escape.png")
                self.assertEqual(status, 404, body.decode("utf-8", "replace"))
                status, _headers, body = requestLan("/icons/%2e%2e.png")
                self.assertEqual(status, 404, body.decode("utf-8", "replace"))
                status, _headers, body = requestLan(
                    "/icons/com.example.app.png", method="POST")
                self.assertEqual(status, 404, body.decode("utf-8", "replace"))

                process.send_signal(signal.SIGTERM)
                output, _ = process.communicate(timeout=15)
                self.assertEqual(process.returncode, 0, output)
            finally:
                if process.poll() is None:
                    process.send_signal(signal.SIGTERM)
                    try:
                        process.communicate(timeout=6)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait(timeout=3)
                node.shutdown()
                node.server_close()
                nodeThread.join(timeout=3)

    def test_native_devtools_firewall_internal_bridge(self):
        binary = Path(sys.argv[1]).resolve()
        fakeLuna = binary.parent / "native-fake-luna-send"
        publicPort = unused_port_pair()
        nodePort = unused_port_pair((publicPort, publicPort + 1))
        secret = "devtools-proxy-secret-0123456789abcdef"
        token = "devtools-route-token"

        class NodeCaptureHandler(BaseHTTPRequestHandler):
            def do_POST(self):
                length = int(self.headers.get("Content-Length", "0"))
                self.rfile.read(length)
                self.send_response(204)
                self.send_header("Content-Length", "0")
                self.end_headers()

            def log_message(self, *_args):
                pass

        node = HTTPServer(("127.0.0.1", nodePort), NodeCaptureHandler)
        nodeThread = threading.Thread(target=node.serve_forever, daemon=True)
        nodeThread.start()

        with tempfile.TemporaryDirectory(prefix="nativetv-native-devtools-") as folder:
            root = Path(folder)
            config = root / "config.json"
            heartbeat = root / "tvweb.beat"
            assets = root / "assets"
            systemRoot = root / "system"
            procRoot = root / "proc"
            binaryDirectory = systemRoot / "usr/sbin"
            devmode = systemRoot / "var/luna/preferences/devmode_enabled"
            ruleState = root / "iptables-rule"
            ruleCalls = root / "iptables-calls"
            assets.mkdir()
            binaryDirectory.mkdir(parents=True)
            devmode.parent.mkdir(parents=True)
            devmode.write_text("1\n")
            (procRoot / "net").mkdir(parents=True)
            (procRoot / "net/ip_tables_names").write_text("filter\n")
            iptables = binaryDirectory / "iptables"
            iptables.write_text(
                "#!/bin/sh\n"
                "echo \"$1\" >> \"$NATIVETV_IPTABLES_CALLS\"\n"
                "case \"$1\" in\n"
                "  -C) test -f \"$NATIVETV_IPTABLES_RULE\" ;;\n"
                "  -I) touch \"$NATIVETV_IPTABLES_RULE\" ;;\n"
                "  -D) rm -f \"$NATIVETV_IPTABLES_RULE\" ;;\n"
                "  *) exit 2 ;;\n"
                "esac\n")
            iptables.chmod(0o755)
            config.write_text(json.dumps({
                "host": "127.0.0.1", "port": publicPort, "token": token,
                "allowControl": True, "allowNetworkDebugger": True,
                "mqtt": {"password": "preserve-this-secret"},
                "custom": {"setting": "keep"}
            }))
            environment = os.environ.copy()
            environment["TVWEB_LUNA_SEND"] = str(fakeLuna)
            environment["TVWEB_DATA_DIR"] = str(root / "data")
            environment["TVWEB_PROXY_SECRET"] = secret
            environment["NATIVETV_IPTABLES_RULE"] = str(ruleState)
            environment["NATIVETV_IPTABLES_CALLS"] = str(ruleCalls)
            process = subprocess.Popen(
                [str(binary), "--config", str(config), "--heartbeat", str(heartbeat),
                 "--workers", "4", "--listen", "--node-port", str(nodePort),
                 "--assets-root", str(assets), "--proc-root", str(procRoot),
                 "--system-root", str(systemRoot)],
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                env=environment)
            output = ""

            def request(allowed, withSecret=True):
                data = json.dumps({"allowNetworkDebugger": allowed}).encode("utf-8")
                headers = {"Content-Type": "application/json"}
                if withSecret:
                    headers["X-NativeTv-Proxy-Secret"] = secret
                req = Request("http://127.0.0.1:%d/_internal/devtools" % publicPort,
                              data=data, method="POST", headers=headers)
                try:
                    with urlopen(req, timeout=8) as response:
                        return response.status, response.read()
                except HTTPError as error:
                    return error.code, error.read()

            try:
                deadline = time.monotonic() + 8
                ready = False
                while time.monotonic() < deadline and process.poll() is None:
                    req = Request("http://127.0.0.1:%d/api/caps" % publicPort,
                                  headers={"Authorization": "Bearer " + token})
                    try:
                        with urlopen(req, timeout=2):
                            ready = True
                            break
                    except (OSError, URLError):
                        time.sleep(0.03)
                if not ready:
                    if process.poll() is None:
                        process.send_signal(signal.SIGTERM)
                    output, _ = process.communicate(timeout=6)
                    self.fail("native daemon did not start:\n" + output[-4000:])

                status, body = request(False, withSecret=False)
                self.assertEqual(status, 403, body.decode("utf-8", "replace"))
                self.assertFalse(ruleState.exists(), "missing secret must not change firewall state")

                configLock = Path(str(config) + ".lock")
                configLock.mkdir()
                (configLock / "owner").write_text("%d:active-writer" % os.getpid())
                status, body = request(False)
                self.assertEqual(status, 500, body.decode("utf-8", "replace"))
                refusal = json.loads(body)
                self.assertFalse(refusal["ok"])
                self.assertTrue(refusal["handled"])
                self.assertFalse(refusal["persisted"])
                self.assertIn("config file lock", refusal["error"])
                self.assertFalse(ruleState.exists(), "firewall changed before config was saved")
                self.assertFalse(ruleCalls.exists(), "failed config persistence must not run firewall commands")
                self.assertIs(json.loads(config.read_text())["allowNetworkDebugger"], True)
                shutil.rmtree(configLock)

                status, body = request(False)
                self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                self.assertEqual(json.loads(body), {
                    "ok": True, "devtools": "closed", "handled": True, "persisted": True
                })
                self.assertTrue(ruleState.exists())
                saved = json.loads(config.read_text())
                self.assertIs(saved["allowNetworkDebugger"], False)
                self.assertEqual(saved["host"], "127.0.0.1")
                self.assertEqual(saved["token"], token)
                self.assertEqual(saved["mqtt"]["password"], "preserve-this-secret")
                self.assertEqual(saved["custom"], {"setting": "keep"})
                self.assertEqual(config.stat().st_mode & 0o777, 0o600)
                status, body = request(False)
                self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                self.assertEqual(json.loads(body)["devtools"], "closed")
                self.assertIs(json.loads(config.read_text())["allowNetworkDebugger"], False)
                self.assertEqual(ruleCalls.read_text().splitlines(), ["-C", "-I", "-C"],
                                 "repeated startup sync must not duplicate the rule")

                status, body = request(True)
                self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                self.assertEqual(json.loads(body), {
                    "ok": True, "devtools": "allowed", "handled": True, "persisted": True
                })
                self.assertIs(json.loads(config.read_text())["allowNetworkDebugger"], True)
                self.assertFalse(ruleState.exists())
                callsBeforeDevModeOff = ruleCalls.read_text().splitlines()
                devmode.unlink()
                status, body = request(False)
                self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                self.assertEqual(json.loads(body), {
                    "ok": True, "devtools": "off", "handled": True, "persisted": True
                })
                self.assertIs(json.loads(config.read_text())["allowNetworkDebugger"], False)
                self.assertEqual(ruleCalls.read_text().splitlines(), callsBeforeDevModeOff,
                                 "Developer Mode off must leave firewall rules untouched")

                process.send_signal(signal.SIGTERM)
                output, _ = process.communicate(timeout=6)
                self.assertEqual(process.returncode, 0, output)
                self.assertIn("command-policy: action=devtools-firewall result=success", output)
            finally:
                if process.poll() is None:
                    process.send_signal(signal.SIGTERM)
                    try:
                        process.communicate(timeout=6)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait(timeout=3)
                node.shutdown()
                node.server_close()
                nodeThread.join(timeout=3)

    def test_native_screensaver_boot_lifecycle(self):
        binary = Path(sys.argv[1]).resolve()
        fakeLuna = binary.parent / "native-fake-luna-send"
        with tempfile.TemporaryDirectory(prefix="nativetv-screensaver-boot-") as folder:
            root = Path(folder)
            assets = root / "assets"
            assets.mkdir()

            def run_case(name, major, stockType, allowExperimental, shouldMount):
                systemRoot = root / name / "system"
                app = systemRoot / "usr/palm/applications/com.webos.app.screensaver"
                stage = systemRoot / "var/lib/tvweb/screensaver"
                tools = systemRoot / "bin"
                app.mkdir(parents=True)
                (stage / "qml").mkdir(parents=True)
                tools.mkdir(parents=True)
                (systemRoot / "etc").mkdir(parents=True)
                (systemRoot / "etc/issue").write_text(
                    "webOS %d.0.0 test fixture\n" % major)
                (app / "appinfo.json").write_text(json.dumps({
                    "id": "com.webos.app.screensaver", "type": stockType,
                    "main": "main"
                }))
                (stage / ".tvweb-screensaver").write_text("clock\n")
                (stage / "appinfo.json").write_text(json.dumps({
                    "id": "com.webos.app.screensaver", "type": "qml",
                    "main": "qml/main.qml"
                }))
                mountCalls = root / (name + "-mount-calls")
                lunaCalls = root / (name + "-luna-calls")
                lunaDetails = root / (name + "-luna-details")
                for command in ("mount", "umount"):
                    tool = tools / command
                    tool.write_text(
                        "#!/bin/sh\n"
                        "echo '%s $*' >> \"$NATIVETV_MOUNT_CALLS\"\n"
                        "exit 0\n" % command)
                    tool.chmod(0o755)
                config = root / (name + "-config.json")
                config.write_text(json.dumps({
                    "port": 8177,
                    "allowExperimentalScreensavers": allowExperimental,
                    "allowControl": False
                }))
                environment = os.environ.copy()
                environment["TVWEB_LUNA_SEND"] = str(fakeLuna)
                environment["TVWEB_DATA_DIR"] = str(root / (name + "-data"))
                environment["NATIVETV_FAKE_LUNA_MODE"] = "screensaver-boot"
                environment["NATIVETV_FAKE_LUNA_LOG"] = str(lunaCalls)
                environment["NATIVETV_FAKE_LUNA_DETAILS"] = str(lunaDetails)
                environment["NATIVETV_FAKE_SCREENSAVER_SAM_TYPE"] = "qml"
                environment["NATIVETV_MOUNT_CALLS"] = str(mountCalls)
                result = subprocess.run(
                    [str(binary), "--config", str(config), "--assets-root", str(assets),
                     "--system-root", str(systemRoot), "--reconcile-screensaver"],
                    capture_output=True, text=True, timeout=10, env=environment)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertEqual(
                    (systemRoot / "var/lib/tvweb/screensaver-stock-type").read_text(),
                    stockType + "\n")
                self.assertEqual(mountCalls.exists(), shouldMount,
                                 "unexpected screensaver bind-mount decision for " + name)
                calls = lunaCalls.read_text().splitlines() if lunaCalls.exists() else []
                details = [json.loads(line) for line in lunaDetails.read_text().splitlines()]
                self.assertTrue(any("releaseBlockSystemApp" in call for call in calls),
                                "boot must clear an interrupted system-app block")
                if shouldMount:
                    self.assertTrue(any("blockSystemApp" in call for call in calls),
                                    "changed runner type must ask SAM to reread the saver")
                    self.assertTrue(any(
                        detail["uri"] == "com.webos.applicationManager/getAppInfo" and
                        detail["payload"].get("id") == "com.webos.app.screensaver"
                        for detail in details), "SAM type must be verified after reread")

            run_case("webos10", 10, "flutter", False, True)
            run_case("held-webos12", 12, "flutter", False, False)
            run_case("opted-in-webos12", 12, "flutter", True, True)

    def test_native_home_open_at_boot_policy(self):
        binary = Path(sys.argv[1]).resolve()
        fakeLuna = binary.parent / "native-fake-luna-send"

        def run_case(name, settings, foreground="com.tvweb.splash",
                     powerReason="remoteKey", bootApp="com.webos.app.livetv"):
            with tempfile.TemporaryDirectory(prefix="nativetv-home-boot-") as folder:
                root = Path(folder)
                dataRoot = root / "data"
                systemRoot = root / "system"
                (dataRoot).mkdir(parents=True)
                (systemRoot / "var/log").mkdir(parents=True)
                (dataRoot / "home.json").write_text(json.dumps(settings))
                (systemRoot / "var/log/bootd.log").write_text(
                    "BootSequencer) '%s' is foreground\n" % bootApp)
                config = root / "config.json"
                config.write_text(json.dumps({"logging": {"level": "debug"}}))
                details = root / "luna-details.jsonl"
                environment = os.environ.copy()
                environment["TVWEB_LUNA_SEND"] = str(fakeLuna)
                environment["TVWEB_DATA_DIR"] = str(dataRoot)
                environment["NATIVETV_FAKE_LUNA_MODE"] = "home-boot"
                environment["NATIVETV_FAKE_LUNA_DETAILS"] = str(details)
                environment["NATIVETV_FAKE_HOME_BOOT_FOREGROUND"] = foreground
                environment["NATIVETV_FAKE_HOME_BOOT_REASON"] = powerReason
                result = subprocess.run(
                    [str(binary), "--config", str(config), "--system-root",
                     str(systemRoot), "--open-home-at-boot"],
                    capture_output=True, text=True, timeout=8, env=environment)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                calls = [json.loads(line) for line in details.read_text().splitlines()] \
                    if details.exists() else []
                launched = any(
                    call["uri"] == "com.webos.applicationManager/launch" and
                    call["payload"].get("id") == "com.webos.app.home"
                    for call in calls)
                return launched, calls

        opened, calls = run_case("selected-input", {"openOnPowerOn": True})
        self.assertTrue(opened, "boot input should be covered when policy allows it")
        self.assertTrue(any(
            call["uri"] == "com.webos.service.tvpower/power/getPowerOnReason"
            for call in calls))
        self.assertTrue(any(
            call["uri"] == "com.webos.applicationManager/getForegroundAppInfo"
            for call in calls))

        opened, _ = run_case("changed-app", {"openOnPowerOn": True},
                             foreground="com.webos.app.netflix")
        self.assertFalse(opened, "a later foreground app must stay in front")

        opened, _ = run_case("unlisted-reason", {"lgAutoLaunch": "on"},
                             powerReason="timer")
        self.assertFalse(opened, "unlisted power-on reasons must not open Home")

        opened, calls = run_case("disabled", {"openOnPowerOn": False})
        self.assertFalse(opened)
        self.assertEqual(calls, [], "disabled policy should avoid Luna calls")

        opened, _ = run_case("unsupported-boot-app", {"openOnPowerOn": True},
                             bootApp="com.webos.app.netflix")
        self.assertFalse(opened, "only a boot input should be covered")

    def test_native_home_starting_notice(self):
        binary = Path(sys.argv[1]).resolve()
        fakeLuna = binary.parent / "native-fake-luna-send"
        with tempfile.TemporaryDirectory(prefix="nativetv-home-notice-") as folder:
            root = Path(folder)
            dataRoot = root / "data"
            dataRoot.mkdir()
            config = root / "config.json"
            config.write_text(json.dumps({"logging": {"level": "debug"}}))
            details = root / "luna-details.jsonl"
            message = 'Press Home to open Native Tv: "Ready\\soon" — 中文'
            (dataRoot / "remotekeys.json").write_text(
                json.dumps({"starting": message}, ensure_ascii=False))
            environment = os.environ.copy()
            environment["TVWEB_LUNA_SEND"] = str(fakeLuna)
            environment["TVWEB_DATA_DIR"] = str(dataRoot)
            environment["NATIVETV_FAKE_LUNA_MODE"] = "home-starting-notice"
            environment["NATIVETV_FAKE_LUNA_DETAILS"] = str(details)
            result = subprocess.run(
                [str(binary), "--config", str(config), "--system-root", "/",
                 "--show-home-starting-notice"],
                capture_output=True, text=True, timeout=8, env=environment)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            calls = [json.loads(line) for line in details.read_text().splitlines()]
            self.assertEqual(len(calls), 1)
            self.assertEqual(calls[0]["uri"], "com.webos.notification/createToast")
            self.assertEqual(calls[0]["applicationId"], "com.webos.app.home")
            self.assertEqual(calls[0]["payload"], {
                "sourceId": "com.webos.app.home", "message": message})

        for saved, label in ((None, "missing"), ("", "empty"), (42, "non-string")):
            with self.subTest(notice=label), tempfile.TemporaryDirectory(
                    prefix="nativetv-home-notice-empty-") as folder:
                root = Path(folder)
                dataRoot = root / "data"
                dataRoot.mkdir()
                config = root / "config.json"
                config.write_text("{}")
                details = root / "luna-details.jsonl"
                if saved is not None:
                    (dataRoot / "remotekeys.json").write_text(
                        json.dumps({"starting": saved}))
                environment = os.environ.copy()
                environment["TVWEB_LUNA_SEND"] = str(fakeLuna)
                environment["TVWEB_DATA_DIR"] = str(dataRoot)
                environment["NATIVETV_FAKE_LUNA_MODE"] = "home-starting-notice"
                environment["NATIVETV_FAKE_LUNA_DETAILS"] = str(details)
                result = subprocess.run(
                    [str(binary), "--config", str(config),
                     "--show-home-starting-notice"],
                    capture_output=True, text=True, timeout=8, env=environment)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertFalse(details.exists(), "an empty notice must not call Luna")

        with tempfile.TemporaryDirectory(prefix="nativetv-home-notice-fail-") as folder:
            root = Path(folder)
            dataRoot = root / "data"
            dataRoot.mkdir()
            (dataRoot / "remotekeys.json").write_text(
                json.dumps({"starting": "Starting"}))
            config = root / "config.json"
            config.write_text("{}")
            details = root / "luna-details.jsonl"
            environment = os.environ.copy()
            environment["TVWEB_LUNA_SEND"] = str(fakeLuna)
            environment["TVWEB_DATA_DIR"] = str(dataRoot)
            environment["NATIVETV_FAKE_LUNA_MODE"] = "home-starting-notice"
            environment["NATIVETV_FAKE_LUNA_DETAILS"] = str(details)
            environment["NATIVETV_FAKE_HOME_NOTICE_FAIL"] = "1"
            result = subprocess.run(
                [str(binary), "--config", str(config),
                 "--show-home-starting-notice"],
                capture_output=True, text=True, timeout=8, env=environment)
            self.assertNotEqual(result.returncode, 0,
                                "a rejected notice should report dispatch failure")
            self.assertEqual(len(details.read_text().splitlines()), 1,
                             "a failed native dispatch must not be replayed")

    def test_native_home_orchestrator_start(self):
        binary = Path(sys.argv[1]).resolve()
        fakeLuna = binary.parent / "native-fake-luna-send"
        for owned in (True, False):
            publicPort = unused_port_pair()
            nodePort = unused_port_pair((publicPort, publicPort + 1))
            token = "home-orchestrator-token"
            with tempfile.TemporaryDirectory(prefix="nativetv-home-orch-") as folder:
                root = Path(folder)
                dataRoot = root / "data"
                dataRoot.mkdir()
                systemRoot = root / "system"
                (systemRoot / "etc").mkdir(parents=True)
                (systemRoot / "etc/issue").write_text("webOS TV 9.2.2\n")
                assets = root / "assets"
                assets.mkdir()
                (dataRoot / "home.json").write_text(json.dumps({"enabled": False}))
                config = root / "config.json"
                config.write_text(json.dumps({
                    "host": "127.0.0.1", "port": publicPort, "token": token,
                    "allowControl": True}))
                details = root / "luna-details.jsonl"
                environment = os.environ.copy()
                environment["TVWEB_LUNA_SEND"] = str(fakeLuna)
                environment["TVWEB_DATA_DIR"] = str(dataRoot)
                environment["TVWEB_PROXY_SECRET"] = "home-orchestrator-secret-0123456789abcdef"
                environment["NATIVETV_FAKE_LUNA_DETAILS"] = str(details)
                environment.pop("TVWEB_NATIVE_HOME_ORCH", None)
                if owned:
                    environment["TVWEB_NATIVE_HOME_ORCH"] = "1"
                process = subprocess.Popen(
                    [str(binary), "--config", str(config), "--heartbeat",
                     str(root / "tvweb.beat"), "--workers", "4", "--listen",
                     "--node-port", str(nodePort), "--assets-root", str(assets),
                     "--system-root", str(systemRoot)],
                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                    env=environment)

                def calls():
                    if not details.exists():
                        return []
                    return [json.loads(line) for line in details.read_text().splitlines()]

                def reads():
                    return [c for c in calls() if c["uri"] == "com.webos.service.settings/getSystemSettings" and
                            "defaultApps" in c["payload"].get("keys", [])]

                try:
                    deadline = time.monotonic() + 8
                    while time.monotonic() < deadline and process.poll() is None:
                        try:
                            req = Request("http://127.0.0.1:%d/api/caps" % publicPort,
                                          headers={"Authorization": "Bearer " + token})
                            with urlopen(req, timeout=2):
                                break
                        except (OSError, URLError):
                            time.sleep(0.03)
                    self.assertIsNone(process.poll(), "daemon exited early")
                    # The start runs once the services are up; it asks what earlier builds left.
                    deadline = time.monotonic() + 6
                    while owned and not reads() and time.monotonic() < deadline:
                        time.sleep(0.05)
                    time.sleep(0.5)
                    if owned:
                        self.assertEqual(len(reads()), 1, "the orchestrator's start reads the legacy settings once")
                        self.assertEqual(reads()[0]["payload"]["keys"],
                                         ["defaultApps", "lastInputApp", "physicalLastInputApp"])
                    else:
                        self.assertEqual(reads(), [], "without the flag the daemon leaves the start to Node")
                finally:
                    if process.poll() is None:
                        process.send_signal(signal.SIGTERM)
                        try:
                            process.communicate(timeout=6)
                        except subprocess.TimeoutExpired:
                            process.kill()
                            process.communicate(timeout=3)

    def _home_world(self, root, publicPort, token, secret, mode, probeExit=0, nativeClient=None):
        """The files and scripts of a TV that can take LG's Home's place (webOS 11.2, as the lifecycle test has it),
        and the daemon's environment against the stand-in luna-send."""
        from types import SimpleNamespace
        binary = Path(sys.argv[1]).resolve()
        world = SimpleNamespace(root=root, binary=binary, port=publicPort, token=token, secret=secret)
        world.data = root / "data"
        world.data.mkdir()
        world.assets = root / "assets"
        world.system = root / "system"
        world.proc = root / "proc"
        world.proc.mkdir()
        # The stub that ships, staged by the daemon over LG's Home.
        shutil.copytree(Path(__file__).resolve().parent.parent / "assets/home-app", world.assets / "home-app")
        systemBin = world.system / "bin"
        systemBin.mkdir(parents=True)
        (systemBin / "sh").symlink_to("/bin/sh")
        world.earlyFlag = world.system / "var/lib/tvweb/early-home"
        world.earlyFlag.parent.mkdir(parents=True)
        earlyManifest = world.system / "var/luna-service2-dev/manifests.d/com.webos.nativetv.boot.json"
        earlyServiceDir = world.assets / "early-service"
        earlyServiceDir.mkdir(parents=True)
        (earlyServiceDir / "install.sh").write_text(
            "#!/bin/sh\n"
            "if [ \"$1\" = install ]; then\n"
            "  mkdir -p \"" + str(earlyManifest.parent) + "\"\n"
            "  : > \"" + str(earlyManifest) + "\"\n"
            "else\n"
            "  rm -f \"" + str(earlyManifest) + "\"\n"
            "fi\n")
        originalHome = root / "lg-home"
        originalHome.mkdir()
        (originalHome / "appinfo.json").write_text('{"id":"com.webos.app.home","type":"native"}\n')
        layoutDir = world.system / "mnt/lg/cmn_data/manifest"
        layoutDir.mkdir(parents=True)
        etc = world.system / "etc"
        etc.mkdir()
        (etc / "issue").write_text("webOS TV 11.2.0\n")
        samSchema = etc / "palm/schemas/sam/applicationManager.blockSystemApp.schema"
        samSchema.parent.mkdir(parents=True)
        samSchema.write_text('{"properties":{"forceBlockSystemApp":{"type":"boolean"}}}\n')
        original = '<home><!-- original LG layout --></home>\n'
        (layoutDir / "home_layoutShelfView.xml").write_text(original)
        (layoutDir / "home_layout.xml").write_text(original)
        fakeProcHome = world.proc / "2000000000"
        fakeProcHome.mkdir()
        (fakeProcHome / "cmdline").write_bytes(b"/usr/bin/com.webos.app.home\0")
        world.appDirectory = world.system / "usr/palm/applications/com.webos.app.home"
        world.appDirectory.parent.mkdir(parents=True)
        world.appDirectory.symlink_to(originalHome, target_is_directory=True)
        world.originalHome = originalHome
        world.mountLog = root / "mount.log"
        world.mounted = root / "home-mounted"
        world.samType = root / "sam-home-type"
        world.samType.write_text("native\n")
        mount = systemBin / "mount"
        mount.write_text(
            "#!/bin/sh\n"
            "printf '%s\\n' \"$*\" >> \"" + str(world.mountLog) + "\"\n"
            "[ \"$1\" = \"--bind\" ] || exit 2\n"
            "rm -f \"$3\" && ln -s \"$2\" \"$3\" && touch \"" + str(world.mounted) + "\"\n")
        mount.chmod(0o755)
        umount = systemBin / "umount"
        umount.write_text(
            "#!/bin/sh\n"
            "target=\"$1\"\n"
            "[ \"$1\" != \"-l\" ] || target=\"$2\"\n"
            "rm -f \"$target\" && ln -s \"" + str(originalHome) + "\" \"$target\" && rm -f \"" +
            str(world.mounted) + "\"\n")
        umount.chmod(0o755)
        world.client = probe_stand_in(root, probeExit)
        world.clientCalls = root / "probe-calls"
        world.clientExit = root / "probe-exit"
        world.unregistered = root / "unregistered"   # while it exists, sam does not know the client app
        world.foreground = root / "foreground.json"
        world.running = root / "running.json"
        world.running.write_text(json.dumps({"running": []}))
        world.details = root / "luna-details.jsonl"
        world.config = root / "config.json"
        client = {"path": str(world.client)}
        client.update(nativeClient or {})
        world.config.write_text(json.dumps({
            "host": "127.0.0.1", "port": publicPort, "token": token, "allowControl": True,
            "allowExperimentalHome": True, "nativeClient": client}))
        world.environment = os.environ.copy()
        world.environment.update({
            "TVWEB_LUNA_SEND": str(binary.parent / "native-fake-luna-send"),
            "NATIVETV_FAKE_LUNA_MODE": mode,
            "NATIVETV_FAKE_HOME_TAKEOVER_TYPE": str(world.samType),
            "NATIVETV_FAKE_HOME_TAKEOVER_MOUNTED": str(world.mounted),
            "NATIVETV_FAKE_HOME_TAKEOVER_REFUSE": str(root / "sam-refuse"),
            "NATIVETV_FAKE_LUNA_DETAILS": str(world.details),
            "NATIVETV_FAKE_LUNA_FOREGROUND_FILE": str(world.foreground),
            "NATIVETV_FAKE_LUNA_RUNNING_FILE": str(world.running),
            "NATIVETV_FAKE_LUNA_UNREGISTERED": str(root / "unregistered"),
            "TVWEB_DATA_DIR": str(world.data),
            "TVWEB_PROXY_SECRET": secret,
        })
        world.command = lambda *extra: [
            str(binary), "--config", str(world.config), "--assets-root", str(world.assets),
            "--proc-root", str(world.proc), "--system-root", str(world.system)] + list(extra)
        return world

    def _home_api(self, world, path, post=False):
        request = Request("http://127.0.0.1:%d%s" % (world.port, path),
                          data=b"{}" if post else None, method="POST" if post else "GET",
                          headers={"Authorization": "Bearer " + world.token,
                                   "Content-Type": "application/json"})
        try:
            with urlopen(request, timeout=15) as response:
                return response.status, json.loads(response.read())
        except HTTPError as error:
            return error.code, json.loads(error.read())

    def _forget_probe(self, world):
        for name in ("native-probe.json", "native-probe.json.last-good"):
            (world.data / name).unlink(missing_ok=True)

    def _wait_until(self, condition, seconds, message):
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            if condition():
                return
            time.sleep(0.05)
        self.fail(message)

    def _stop_daemon(self, process):
        if process.poll() is None:
            process.send_signal(signal.SIGTERM)
            try:
                return process.communicate(timeout=6)[0]
            except subprocess.TimeoutExpired:
                process.kill()
                return process.communicate(timeout=3)[0]
        return ""

    def _start_home_daemon(self, world, nodeFree=False, orchestrator=False):
        nodePort = unused_port_pair((world.port, world.port + 1))
        environment = dict(world.environment)
        if orchestrator:
            environment["TVWEB_NATIVE_STATE"] = "1"
            environment["TVWEB_NATIVE_HOME_ORCH"] = "1"
        process = subprocess.Popen(
            [str(world.binary), "--config", str(world.config), "--heartbeat", str(world.root / "tvweb.beat"),
             "--workers", "12", "--listen", "--node-port", str(nodePort), "--assets-root", str(world.assets),
             "--proc-root", str(world.proc), "--system-root", str(world.system)],
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, env=environment)

        def answers():
            try:
                req = Request("http://127.0.0.1:%d/api/caps" % world.port,
                              headers={"Authorization": "Bearer " + world.token})
                with urlopen(req, timeout=2):
                    return True
            except (OSError, URLError):
                return False
        self._wait_until(lambda: process.poll() is not None or answers(), 10, "the daemon did not answer")
        if process.poll() is not None:
            self.fail("daemon exited early:\n" + process.stdout.read())
        return process

    def test_native_home_probe_gate(self):
        """The native client's probe decides whether Native Tv Home may take LG's Home's place."""
        publicPort = unused_port_pair()
        with tempfile.TemporaryDirectory(prefix="nativetv-home-probe-") as folder:
            world = self._home_world(Path(folder), publicPort, "home-probe-token",
                                     "home-probe-secret-0123456789abcdef", "home-takeover", probeExit=12)
            process = self._start_home_daemon(world)
            try:
                status, answer = self._home_api(world, "/api/home")
                self.assertEqual(status, 200, answer)
                self.assertTrue(answer["supported"]["ok"], "the status does not run the probe: " + json.dumps(answer))
                self.assertNotIn("nativeClient", answer)
                self.assertFalse(world.clientCalls.exists(), "the status route must not open the probe's window")

                # A driver fault: refused, with the exit class in the reason, and nothing mounted.
                status, answer = self._home_api(world, "/api/home/enable", post=True)
                self.assertEqual(status, 200, answer)
                self.assertFalse(answer["ok"], answer)
                self.assertIn("graphics driver", answer["error"])
                self.assertIn("exit 12", answer["error"])
                self.assertFalse(world.mountLog.exists(), "a refused probe must not mount the stub")
                self.assertFalse(world.mounted.exists())
                self.assertFalse(json.loads((world.data / "home.json").read_text()
                                            if (world.data / "home.json").exists() else "{}").get("enabled", False))
                self.assertEqual(world.clientCalls.read_text().splitlines(), ["--probe"])
                status, answer = self._home_api(world, "/api/home")
                self.assertFalse(answer["supported"]["ok"], answer)
                self.assertEqual(answer["supported"]["probe"]["class"], "gl")
                self.assertEqual(answer["supported"]["probe"]["exit"], 12)
                self.assertEqual(answer["supported"]["probe"]["report"]["renderer"], "Stand-in GPU")
                self.assertEqual(answer["nativeClient"]["class"], "gl")
                self.assertFalse(answer["available"], "Home is not offered where the probe refuses it")

                # A display that is not up yet is tried three times and refused with its own reason; the
                # switch stays offered, since asking again may find it.
                world.clientExit.write_text("10\n")
                status, answer = self._home_api(world, "/api/home/enable", post=True)
                self.assertFalse(answer["ok"], answer)
                self.assertIn("display", answer["error"])
                self.assertEqual(len(world.clientCalls.read_text().splitlines()), 1 + 3)
                status, answer = self._home_api(world, "/api/home")
                self.assertTrue(answer["supported"]["ok"], answer)
                self.assertEqual(answer["nativeClient"]["class"], "display")
                self.assertFalse((world.data / "native-probe.json").exists(), "a verdict that may be timing is not kept")

                # Switching Home on probes again, so a client updated since is looked at.
                world.clientExit.write_text("0\n")
                status, answer = self._home_api(world, "/api/home/enable", post=True)
                self.assertEqual(answer, {"ok": True}, answer)
                self.assertTrue(world.mounted.exists())
                self.assertEqual(len(world.clientCalls.read_text().splitlines()), 1 + 3 + 1)
                self.assertTrue((world.data / "native-probe.json").is_file(), "an ok verdict is kept across restarts")
                status, answer = self._home_api(world, "/api/home")
                self.assertTrue(answer["supported"]["ok"], answer)
                self.assertTrue(answer["nativeClient"]["ok"])
                status, answer = self._home_api(world, "/api/home/disable", post=True)
                self.assertTrue(answer["ok"], answer)
                self.assertFalse(world.mounted.exists())
            finally:
                output = self._stop_daemon(process)

            # The boot hook's takeover is gated the same way: exit 4, which it reports as a skip.
            self._forget_probe(world)
            (world.data / "home.json").write_text(json.dumps({"enabled": True}))
            world.clientExit.write_text("13\n")
            mounts = len(world.mountLog.read_text().splitlines())
            refused = subprocess.run(world.command("--takeover-home"), capture_output=True, text=True,
                                     env=world.environment, timeout=30)
            self.assertEqual(refused.returncode, 4, refused.stdout + refused.stderr)
            self.assertEqual(len(world.mountLog.read_text().splitlines()), mounts, "no mount after a refusal")
            self.assertFalse(world.mounted.exists())
            # A driver fault stays the answer for that client on that firmware, as a restart would find it.
            world.clientExit.write_text("0\n")
            kept = subprocess.run(world.command("--takeover-home"), capture_output=True, text=True,
                                  env=world.environment, timeout=30)
            self.assertEqual(kept.returncode, 4, kept.stdout + kept.stderr)
            self.assertEqual(len(world.clientCalls.read_text().splitlines()), 5 + 1)
            self._forget_probe(world)           # a new client build, say
            allowed = subprocess.run(world.command("--takeover-home"), capture_output=True, text=True,
                                     env=world.environment, timeout=30)
            self.assertEqual(allowed.returncode, 0, allowed.stdout + allowed.stderr)
            self.assertTrue(world.mounted.exists())
            released = subprocess.run(world.command("--release-home"), capture_output=True, text=True,
                                      env=world.environment, timeout=30)
            self.assertEqual(released.returncode, 0, released.stdout + released.stderr)
            self.assertFalse(world.mounted.exists())
            # The verdict was kept: the boot after that does not open the probe's window again.
            calls = len(world.clientCalls.read_text().splitlines())
            again = subprocess.run(world.command("--takeover-home"), capture_output=True, text=True,
                                   env=world.environment, timeout=30)
            self.assertEqual(again.returncode, 0, again.stdout + again.stderr)
            self.assertEqual(len(world.clientCalls.read_text().splitlines()), calls)

            # No client file at all (an install that has only the server): refused, not mounted.
            subprocess.run(world.command("--release-home"), capture_output=True, text=True,
                           env=world.environment, timeout=30)
            world.client.unlink()
            self._forget_probe(world)
            missing = subprocess.run(world.command("--takeover-home"), capture_output=True, text=True,
                                     env=world.environment, timeout=30)
            self.assertEqual(missing.returncode, 4, missing.stdout + missing.stderr)
            self.assertFalse(world.mounted.exists())

    def test_native_home_crash_breaker(self):
        """A native client that keeps failing gives LG's Home back, and Home is not taken again until restart."""
        publicPort = unused_port_pair()
        with tempfile.TemporaryDirectory(prefix="nativetv-home-breaker-") as folder:
            world = self._home_world(Path(folder), publicPort, "home-breaker-token",
                                     "home-breaker-secret-0123456789abcdef", "home-breaker",
                                     nativeClient={"stubGraceMs": 1500})
            (world.data / "home.json").write_text(json.dumps({"enabled": True}))
            process = self._start_home_daemon(world, orchestrator=True)

            def front(appId, listed=()):
                world.running.write_text(json.dumps({"running": [{"id": app} for app in listed]}))
                world.foreground.write_text(json.dumps({"appId": appId}))

            def calls():
                return [json.loads(line) for line in world.details.read_text().splitlines()] \
                    if world.details.exists() else []

            def home():
                return self._home_api(world, "/api/home")[1]
            try:
                # The start takes the place once the probe has passed.
                self._wait_until(world.mounted.exists, 10, "the start did not take LG's Home's place")
                self.assertEqual(world.clientCalls.read_text().splitlines(), ["--probe"])
                self.assertEqual(len(world.mountLog.read_text().splitlines()), 1)
                self.assertTrue(home()["nativeHome"]["watching"])
                self.assertFalse(home()["nativeHome"]["halted"])
                # The stub opens the Native Tv tile's app, the one sam is told of, and the watch follows that app.
                stub = (world.data / "home-takeover/index.html").read_text()
                self.assertIn("var APP_ID = 'com.tvweb.dashboard'", stub)
                self.assertNotIn("com.tvweb.native", stub)
                front("com.tvweb.native", [])                           # the retired developer client is not it
                time.sleep(0.4)
                front("com.webos.app.home", [])
                time.sleep(0.6)
                self.assertEqual(home()["nativeHome"]["crashes"]["recentExits"], 0)
                front("com.webos.app.netflix", [])

                # Neither the Home key nor opening an app is a failure.
                for _ in range(3):
                    front("com.tvweb.dashboard", ["com.tvweb.dashboard"])
                    time.sleep(0.4)
                    front("com.webos.app.home", ["com.tvweb.dashboard"])   # the Home key: the client stays listed
                    time.sleep(0.6)
                front("com.tvweb.dashboard", ["com.tvweb.dashboard"])
                time.sleep(0.6)
                front("com.webos.app.netflix", [])                      # an app opened from Home
                time.sleep(1.0)
                status = home()["nativeHome"]
                self.assertFalse(status["halted"], status)
                self.assertEqual(status["crashes"]["recentExits"], 0, status)
                self.assertTrue(world.mounted.exists())

                # The client dies while it is in front, three times: LG's Home comes back.
                for attempt in range(3):
                    front("com.tvweb.dashboard", ["com.tvweb.dashboard"])
                    time.sleep(0.5)
                    front("com.webos.app.home", [])
                    if attempt < 2:
                        self._wait_until(lambda n=attempt + 1: home()["nativeHome"]["crashes"]["recentExits"] >= n,
                                         8, "crash %d was not counted" % (attempt + 1))
                self._wait_until(lambda: not world.mounted.exists(), 12, "LG's Home was not given back")
                self.assertEqual(world.appDirectory.resolve(), world.originalHome.resolve())
                status = home()
                self.assertTrue(status["nativeHome"]["halted"], status)
                self.assertEqual(status["nativeHome"]["cause"], "crashLoop")
                self.assertIn("stopped 3 times within 120 seconds", status["nativeHome"]["reason"])
                self.assertTrue(status["enabled"], "Home stays switched on, for the next start")
                self.assertTrue(json.loads((world.data / "home.json").read_text())["enabled"])
                self.assertFalse(status["active"])
                remote = world.data / "remotekeys.json"
                self._wait_until(lambda: not remote.exists() or "starting" not in json.loads(remote.read_text()),
                                 8, "the Home key's starting notice was not cleared")

                # Not taken again: the client coming and going, and the stub left in front, do nothing.
                released = len(world.mountLog.read_text().splitlines())
                front("com.tvweb.dashboard", ["com.tvweb.dashboard"])
                time.sleep(0.5)
                front("com.webos.app.home", [])
                time.sleep(4.0)
                self.assertEqual(len(world.mountLog.read_text().splitlines()), released)
                self.assertFalse(world.mounted.exists())
                self.assertTrue(home()["nativeHome"]["halted"])

                # The owner switches Home on again: it is taken again and the watch starts over.
                front("com.webos.app.netflix", [])
                status, answer = self._home_api(world, "/api/home/enable", post=True)
                self.assertEqual(answer, {"ok": True}, answer)
                self.assertTrue(world.mounted.exists())
                status = home()["nativeHome"]
                self.assertFalse(status["halted"], status)
                self.assertEqual(status["crashes"]["recentExits"], 0)

                # A stub that stays in front with no client behind it is a failure too: the third gives LG's Home back.
                front("com.webos.app.home", [])
                self._wait_until(lambda: not world.mounted.exists(), 15, "a stub left in front was not given up")
                status = home()["nativeHome"]
                self.assertTrue(status["halted"], status)
                self.assertEqual(status["cause"], "crashLoop")
            finally:
                output = self._stop_daemon(process)
            self.assertIn("home: Native Tv's Home screen stopped 3 times within 120 seconds.", output)

            # A restart takes the place again: home.json still says Home is on.
            (world.data / "home.json").write_text(json.dumps({"enabled": True}))
            process = self._start_home_daemon(world, orchestrator=True)
            try:
                self._wait_until(world.mounted.exists, 10, "a restart did not take the place again")
                self.assertFalse(home()["nativeHome"]["halted"])
            finally:
                self._stop_daemon(process)

            # A probe that now refuses it: a restart gives LG's Home the place and keeps Home switched on.
            world.clientExit.write_text("12\n")
            self._forget_probe(world)
            process = self._start_home_daemon(world, orchestrator=True)
            try:
                self._wait_until(lambda: home()["nativeHome"]["halted"], 10, "the probe's refusal was not taken")
                status = home()
                self.assertEqual(status["nativeHome"]["cause"], "client")
                self.assertIn("graphics driver", status["nativeHome"]["reason"])
                self.assertFalse(world.mounted.exists())
                self.assertTrue(json.loads((world.data / "home.json").read_text())["enabled"])
                self.assertFalse(status["supported"]["ok"])
            finally:
                self._stop_daemon(process)

    def test_native_home_client_registration(self):
        """Home needs the Native Tv tile's app known to sam: refused at switch-on, and given up at once if it goes."""
        publicPort = unused_port_pair()
        with tempfile.TemporaryDirectory(prefix="nativetv-home-registered-") as folder:
            world = self._home_world(Path(folder), publicPort, "home-registered-token",
                                     "home-registered-secret-0123456789abcdef", "home-breaker",
                                     nativeClient={"stubGraceMs": 30000})
            # The developer client an earlier release staged goes at start; the tile's app stays.
            apps = world.system / "media/developer/apps/usr/palm"
            for name in ("applications/com.tvweb.native", "applications/com.tvweb.dashboard", "packages/com.tvweb.native"):
                (apps / name).mkdir(parents=True)
            (apps / "applications/com.tvweb.native/nativetv-tv").write_text("old")
            (apps / "applications/com.tvweb.dashboard/nativetv-tv").write_text("tile")
            world.unregistered.write_text("")
            process = self._start_home_daemon(world)
            try:
                self._wait_until(lambda: not (apps / "applications/com.tvweb.native").exists(), 10,
                                 "the retired developer client was not removed")
                self.assertFalse((apps / "packages/com.tvweb.native").exists())
                self.assertTrue((apps / "applications/com.tvweb.dashboard/nativetv-tv").is_file())

                # The tile is off the home screen: switching on is refused, saying how to put it back.
                status, answer = self._home_api(world, "/api/home/enable", post=True)
                self.assertFalse(answer["ok"], answer)
                self.assertIn("home screen", answer["error"])
                self.assertIn("Server tab", answer["error"])
                self.assertFalse(world.mountLog.exists(), "nothing is mounted for a client sam will not open")
                self.assertFalse(world.clientCalls.exists(), "and the probe is not opened for it")
                status, answer = self._home_api(world, "/api/home")
                self.assertEqual(answer["nativeClient"]["class"], "unregistered", answer)
                self.assertFalse(answer["enabled"])

                # Back on the home screen: the same switch works.
                world.unregistered.unlink()
                status, answer = self._home_api(world, "/api/home/enable", post=True)
                self.assertEqual(answer, {"ok": True}, answer)
                self.assertTrue(world.mounted.exists())
                status, answer = self._home_api(world, "/api/home/disable", post=True)
                self.assertTrue(answer["ok"], answer)
                self.assertFalse(world.mounted.exists())

                # The boot hook's takeover asks sam too.
                (world.data / "home.json").write_text(json.dumps({"enabled": True}))
                world.unregistered.write_text("")
                refused = subprocess.run(world.command("--takeover-home"), capture_output=True, text=True,
                                         env=world.environment, timeout=30)
                self.assertEqual(refused.returncode, 4, refused.stdout + refused.stderr)
                self.assertFalse(world.mounted.exists())
                world.unregistered.unlink()
                allowed = subprocess.run(world.command("--takeover-home"), capture_output=True, text=True,
                                         env=world.environment, timeout=30)
                self.assertEqual(allowed.returncode, 0, allowed.stdout + allowed.stderr)
                self.assertTrue(world.mounted.exists())
                released = subprocess.run(world.command("--release-home"), capture_output=True, text=True,
                                          env=world.environment, timeout=30)
                self.assertEqual(released.returncode, 0, released.stdout + released.stderr)
            finally:
                self._stop_daemon(process)

            # Home is on and in place; the owner takes the tile off. The next time the stub comes to the front
            # (the Home key from an input) LG's Home is given back at once, not after three graces.
            (world.data / "home.json").write_text(json.dumps({"enabled": True}))
            process = self._start_home_daemon(world, orchestrator=True)

            def front(appId, listed=()):
                world.running.write_text(json.dumps({"running": [{"id": app} for app in listed]}))
                world.foreground.write_text(json.dumps({"appId": appId}))

            def home():
                return self._home_api(world, "/api/home")[1]
            try:
                self._wait_until(world.mounted.exists, 10, "the start did not take LG's Home's place")
                front("com.webos.app.hdmi1")
                time.sleep(0.5)
                world.unregistered.write_text("")
                front("com.webos.app.home")                              # the Home key, from HDMI
                started = time.monotonic()
                self._wait_until(lambda: not world.mounted.exists(), 6, "LG's Home was not given back at once")
                self.assertLess(time.monotonic() - started, 6)
                status = home()
                self.assertTrue(status["nativeHome"]["halted"], status)
                self.assertEqual(status["nativeHome"]["cause"], "client")
                self.assertIn("home screen", status["nativeHome"]["reason"])
                self.assertEqual(status["nativeHome"]["crashes"]["recentExits"], 0,
                                 "one refusal, not three exits, is what did it")
                self.assertTrue(status["enabled"], "Home stays switched on")
            finally:
                output = self._stop_daemon(process)
            self.assertIn("is not on this TV's home screen", output)

            # A start that finds the tile gone does not take the place at all.
            (world.data / "home.json").write_text(json.dumps({"enabled": True}))
            process = self._start_home_daemon(world, orchestrator=True)
            try:
                self._wait_until(lambda: home()["nativeHome"]["halted"], 10, "the start did not give it up")
                self.assertEqual(home()["nativeHome"]["cause"], "client")
                self.assertFalse(world.mounted.exists())
                # Put the tile back, switch Home on again: the watch starts over and the place is taken.
                world.unregistered.unlink()
                status, answer = self._home_api(world, "/api/home/enable", post=True)
                self.assertEqual(answer, {"ok": True}, answer)
                self.assertTrue(world.mounted.exists())
                self.assertFalse(home()["nativeHome"]["halted"])
            finally:
                self._stop_daemon(process)

    def test_native_home_startup_route(self):
        binary = Path(sys.argv[1]).resolve()
        fakeLuna = binary.parent / "native-fake-luna-send"
        publicPort = unused_port_pair()
        nodePort = unused_port_pair((publicPort, publicPort + 1))
        token = "home-startup-route-token"
        secret = "home-startup-route-secret-0123456789abcdef"
        with tempfile.TemporaryDirectory(prefix="nativetv-home-startup-route-") as folder:
            root = Path(folder)
            dataRoot = root / "data"
            dataRoot.mkdir()
            systemRoot = root / "system"
            etc = systemRoot / "etc"
            etc.mkdir(parents=True)
            systemShell = systemRoot / "bin/sh"
            systemShell.parent.mkdir(parents=True)
            systemShell.symlink_to("/bin/sh")
            earlyHomeFlag = systemRoot / "var/lib/tvweb/early-home"
            earlyHomeFlag.parent.mkdir(parents=True)
            earlyManifest = (systemRoot /
                             "var/luna-service2-dev/manifests.d/"
                             "com.webos.nativetv.boot.json")
            earlyServiceAssets = assets = root / "assets"
            earlyServiceDir = earlyServiceAssets / "early-service"
            earlyServiceDir.mkdir(parents=True)
            earlyServiceLog = root / "early-service.log"
            earlyInstall = earlyServiceDir / "install.sh"
            earlyInstall.write_text(
                "#!/bin/sh\n"
                "printf '%s\\n' \"$1\" >> \"" + str(earlyServiceLog) + "\"\n"
                "if [ \"$1\" = install ]; then\n"
                "  mkdir -p \"" + str(earlyManifest.parent) + "\"\n"
                "  : > \"" + str(earlyManifest) + "\"\n"
                "else\n"
                "  rm -f \"" + str(earlyManifest) + "\"\n"
                "fi\n")
            issue = etc / "issue"
            issue.write_text("webOS TV 9.2.2\n")
            filterDirectory = (systemRoot /
                               "usr/lib/qt5/qml/KeyFilters")
            filterDirectory.mkdir(parents=True)
            (filterDirectory / "lg-keys.js").write_text(
                "var netflixKey = WebOS.Key_webOS_Netflix;\n")
            home = dataRoot / "home.json"
            home.write_text(json.dumps({
                "enabled": True, "openOnPowerOn": False,
                "lgAutoLaunch": None, "lgAutoLaunchCheck": None
            }))
            remoteKeys = dataRoot / "remotekeys.json"
            remoteKeys.write_text(json.dumps({
                "keys": {"guide": "com.example.guide"},
                "home": "com.webos.app.home", "starting": "Old notice",
                "splash": False, "updated": "2026-10-28T00:00:00.000Z"
            }))
            nativeAssets = dataRoot / "native"
            nativeAssets.mkdir()
            shutil.copyfile(
                Path(__file__).resolve().parents[1] /
                "assets/remotekeys-filter.js",
                nativeAssets / "remotekeys-filter.js")
            filterChain = root / "filter-chain.json"
            filterChain.write_text("[]")
            settingsFailure = root / "reject-auto-launch-restore"
            config = root / "config.json"
            config.write_text(json.dumps({
                "host": "127.0.0.1", "port": publicPort, "token": token,
                "allowControl": True
            }))
            details = root / "luna-details.jsonl"
            environment = os.environ.copy()
            environment["TVWEB_LUNA_SEND"] = str(fakeLuna)
            environment["TVWEB_DATA_DIR"] = str(dataRoot)
            environment["TVWEB_PROXY_SECRET"] = secret
            environment["NATIVETV_FAKE_LUNA_MODE"] = "home-startup-route"
            environment["NATIVETV_FAKE_LUNA_DETAILS"] = str(details)
            environment["NATIVETV_FAKE_HOME_STARTUP_CHAIN"] = str(filterChain)
            environment["NATIVETV_FAKE_HOME_STARTUP_SETTINGS_FAIL"] = str(settingsFailure)
            process = subprocess.Popen(
                [str(binary), "--config", str(config), "--heartbeat",
                 str(root / "tvweb.beat"), "--workers", "4", "--listen",
                 "--node-port", str(nodePort), "--assets-root", str(assets),
                 "--system-root", str(systemRoot)],
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                env=environment)

            def post(value, origin=None):
                headers = {
                    "Authorization": "Bearer " + token,
                    "Content-Type": "application/json"
                }
                if origin:
                    headers["Origin"] = origin
                req = Request(
                    "http://127.0.0.1:%d/api/home/startup" % publicPort,
                    data=json.dumps(value).encode("utf-8"), method="POST",
                    headers=headers)
                try:
                    with urlopen(req, timeout=6) as response:
                        return response.status, json.loads(response.read())
                except HTTPError as error:
                    return error.code, json.loads(error.read())

            def getCaps():
                req = Request("http://127.0.0.1:%d/api/caps" % publicPort,
                              headers={"Authorization": "Bearer " + token})
                with urlopen(req, timeout=2) as response:
                    return response.status

            try:
                deadline = time.monotonic() + 8
                ready = False
                while time.monotonic() < deadline and process.poll() is None:
                    try:
                        getCaps()
                        ready = True
                        break
                    except (OSError, URLError):
                        time.sleep(0.03)
                if not ready:
                    if process.poll() is None:
                        process.send_signal(signal.SIGTERM)
                    output, _ = process.communicate(timeout=6)
                    self.fail("native daemon did not start:\n" + output[-4000:])

                status, response = post({"openOnPowerOn": True})
                self.assertEqual(status, 200, response)
                self.assertEqual(response, {"ok": True})
                savedHome = json.loads(home.read_text())
                self.assertTrue(savedHome["openOnPowerOn"])
                self.assertTrue(savedHome["enabled"])
                notice = json.loads(remoteKeys.read_text())
                self.assertEqual(notice["keys"], {"guide": "com.example.guide"})
                self.assertEqual(notice["home"], "com.webos.app.home")
                self.assertEqual(notice["starting"],
                                 "Native Tv Home is starting. It opens in a few seconds.")
                self.assertTrue(notice["splash"])
                self.assertTrue(earlyHomeFlag.is_file())
                self.assertTrue(earlyManifest.is_file())
                self.assertEqual(earlyServiceLog.read_text().splitlines(), ["install"])

                calls = [json.loads(line) for line in details.read_text().splitlines()]
                writes = [call for call in calls
                          if call["uri"] == "com.webos.service.config/setConfigs"]
                self.assertEqual(len(writes), 2)
                self.assertEqual(writes[0]["payload"]["configs"][
                    "nativetv.remoteKeys"], {
                        "keys": {"guide": "com.example.guide"},
                        "home": "com.webos.app.home",
                        "starting": "Native Tv Home is starting. It opens in a few seconds.",
                        "splash": True
                    })
                filterPath = str(dataRoot / "remotekeys-filter.js")
                self.assertEqual(writes[1]["payload"]["configs"][
                    "com.webos.surfacemanager.keyFilters"][0], {
                        "file": filterPath,
                        "handler": "handleNativeTvKeys"
                    })
                self.assertTrue((dataRoot / "remotekeys-filter.js").is_file())

                status, response = post({"openOnPowerOn": False})
                self.assertEqual(status, 200, response)
                self.assertEqual(response, {"ok": True})
                notice = json.loads(remoteKeys.read_text())
                self.assertNotIn("splash", notice)
                self.assertEqual(notice["starting"],
                                 "Native Tv Home is starting. It opens in a few seconds.")
                self.assertTrue(earlyHomeFlag.is_file(),
                                "the early Home hook stays enabled while Home is on")
                calls = [json.loads(line) for line in details.read_text().splitlines()]
                writes = [call for call in calls
                          if call["uri"] == "com.webos.service.config/setConfigs"]
                self.assertEqual(writes[-1]["payload"]["configs"][
                    "nativetv.remoteKeys"], {
                        "keys": {"guide": "com.example.guide"},
                        "home": "com.webos.app.home",
                        "starting": "Native Tv Home is starting. It opens in a few seconds."
                    })

                # A configd reset gives LG's chain back: ours goes in again, first.
                filterChain.write_text("[]")
                status, response = post({"openOnPowerOn": False})
                self.assertEqual(status, 200, response)
                calls = [json.loads(line) for line in details.read_text().splitlines()]
                writes = [call for call in calls
                          if call["uri"] == "com.webos.service.config/setConfigs"]
                self.assertEqual(writes[-1]["payload"]["configs"][
                    "com.webos.surfacemanager.keyFilters"][0], {
                        "file": filterPath, "handler": "handleNativeTvKeys"})
                # A filter file that is not the bundled one is replaced, and the entry is
                # taken out of the chain and put back so the compositor reloads it.
                (dataRoot / "remotekeys-filter.js").write_text("// an older filter\n")
                before = len(writes)
                status, response = post({"openOnPowerOn": False})
                self.assertEqual(status, 200, response)
                calls = [json.loads(line) for line in details.read_text().splitlines()]
                writes = [call for call in calls
                          if call["uri"] == "com.webos.service.config/setConfigs"]
                chainWrites = [call["payload"]["configs"]["com.webos.surfacemanager.keyFilters"]
                               for call in writes[before:]
                               if "com.webos.surfacemanager.keyFilters" in call["payload"]["configs"]]
                self.assertEqual(len(chainWrites), 2)
                self.assertNotIn(filterPath, [entry["file"] for entry in chainWrites[0]])
                self.assertEqual(chainWrites[1][0]["file"], filterPath)
                self.assertNotEqual((dataRoot / "remotekeys-filter.js").read_text(),
                                    "// an older filter\n")

                status, response = post({})
                self.assertEqual(status, 200)
                self.assertEqual(response, {
                    "ok": False, "error": "openOnPowerOn expected"
                })
                self.assertFalse(json.loads(home.read_text())["openOnPowerOn"])

                status, response = post({"openOnPowerOn": True},
                                        origin="http://attacker.invalid")
                self.assertEqual(status, 403)
                self.assertFalse(json.loads(home.read_text())["openOnPowerOn"])

                issue.write_text("webOS TV 12.0.0\n")
                status, response = post({"openOnPowerOn": True})
                self.assertEqual(status, 200)
                self.assertFalse(response["ok"])
                self.assertIn("This TV runs webOS 12.0.0", response["error"])
                self.assertFalse(json.loads(home.read_text())["openOnPowerOn"])

                issue.write_text("webOS TV 9.2.2\n")
                savedHome = json.loads(home.read_text())
                savedHome["lgAutoLaunch"] = "on"
                savedHome["lgAutoLaunchCheck"] = "default"
                home.write_text(json.dumps(savedHome))
                settingsFailure.touch()
                status, response = post({"openOnPowerOn": True})
                self.assertEqual(status, 200)
                self.assertFalse(response["ok"])
                self.assertIn("would not change the power-on Home setting",
                              response["error"])
                rolledBack = json.loads(home.read_text())
                self.assertFalse(rolledBack["openOnPowerOn"])
                self.assertEqual(rolledBack["lgAutoLaunch"], "on")
                settingsFailure.unlink()

                process.send_signal(signal.SIGTERM)
                output, _ = process.communicate(timeout=6)
                self.assertEqual(process.returncode, 0, output)
            finally:
                if process.poll() is None:
                    process.send_signal(signal.SIGTERM)
                    try:
                        process.communicate(timeout=6)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait(timeout=3)

    def test_native_screensaver_stage_internal_bridge(self):
        binary = Path(sys.argv[1]).resolve()
        fakeLuna = binary.parent / "native-fake-luna-send"
        publicPort = unused_port_pair()
        nodePort = unused_port_pair((publicPort, publicPort + 1))
        secret = "screensaver-proxy-secret-0123456789abcdef"
        token = "screensaver-route-token"

        class NodeCaptureHandler(BaseHTTPRequestHandler):
            def do_POST(self):
                length = int(self.headers.get("Content-Length", "0"))
                self.rfile.read(length)
                self.send_response(204)
                self.send_header("Content-Length", "0")
                self.end_headers()

            def log_message(self, *_args):
                pass

        node = HTTPServer(("127.0.0.1", nodePort), NodeCaptureHandler)
        nodeThread = threading.Thread(target=node.serve_forever, daemon=True)
        nodeThread.start()

        with tempfile.TemporaryDirectory(prefix="nativetv-native-screensaver-") as folder:
            root = Path(folder)
            config = root / "config.json"
            heartbeat = root / "tvweb.beat"
            assets = root / "assets"
            assetDirectory = assets / "screensavers"
            systemRoot = root / "system"
            binaryDirectory = systemRoot / "bin"
            appDirectory = systemRoot / "usr/palm/applications/com.webos.app.screensaver"
            procRoot = root / "proc"
            calls = root / "mount-calls"
            assets.mkdir()
            assetDirectory.mkdir()
            binaryDirectory.mkdir(parents=True)
            appDirectory.mkdir(parents=True)
            (procRoot / "net").mkdir(parents=True)
            staleStage = systemRoot / "var/lib/tvweb/screensaver"
            (staleStage / "qml").mkdir(parents=True)
            (staleStage / ".tvweb-screensaver").write_text("clock\n")
            (staleStage / "qml/orphan.qml").write_text("old screen saver")
            (appDirectory / "appinfo.json").write_text(json.dumps({
                "id": "com.webos.app.screensaver", "type": "flutter", "main": "main"
            }))
            (assetDirectory / "clock.qml").write_text(
                "Text { property string endpoint: '__TVWEB_URL__'; "
                "property int bright: __TVWEB_LEVEL__ }\n")
            (assetDirectory / "star.png").write_bytes(b"png fixture")
            for command in ("mount", "umount"):
                tool = binaryDirectory / command
                tool.write_text(
                    "#!/bin/sh\n"
                    "echo \"%s $*\" >> \"$NATIVETV_MOUNT_CALLS\"\n"
                    "exit 0\n" % command)
                tool.chmod(0o755)
            config.write_text(json.dumps({
                "host": "127.0.0.1", "port": publicPort, "token": token,
                "allowControl": True
            }))
            environment = os.environ.copy()
            environment["TVWEB_LUNA_SEND"] = str(fakeLuna)
            environment["TVWEB_DATA_DIR"] = str(root / "data")
            environment["TVWEB_PROXY_SECRET"] = secret
            environment["NATIVETV_MOUNT_CALLS"] = str(calls)
            process = subprocess.Popen(
                [str(binary), "--config", str(config), "--heartbeat", str(heartbeat),
                 "--workers", "4", "--listen", "--node-port", str(nodePort),
                 "--assets-root", str(assets), "--proc-root", str(procRoot),
                 "--system-root", str(systemRoot)],
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                env=environment)
            output = ""

            def request(command, withSecret=True):
                data = json.dumps(command).encode("utf-8")
                headers = {"Content-Type": "application/json"}
                if withSecret:
                    headers["X-NativeTv-Proxy-Secret"] = secret
                req = Request("http://127.0.0.1:%d/_internal/screensaver" % publicPort,
                              data=data, method="POST", headers=headers)
                try:
                    with urlopen(req, timeout=8) as response:
                        return response.status, response.read()
                except HTTPError as error:
                    return error.code, error.read()

            try:
                deadline = time.monotonic() + 8
                ready = False
                while time.monotonic() < deadline and process.poll() is None:
                    req = Request("http://127.0.0.1:%d/api/caps" % publicPort,
                                  headers={"Authorization": "Bearer " + token})
                    try:
                        with urlopen(req, timeout=2):
                            ready = True
                            break
                    except (OSError, URLError):
                        time.sleep(0.03)
                if not ready:
                    if process.poll() is None:
                        process.send_signal(signal.SIGTERM)
                    output, _ = process.communicate(timeout=6)
                    self.fail("native daemon did not start:\n" + output[-4000:])

                self.assertFalse((staleStage / ".tvweb-screensaver").exists(),
                                 "startup must clear an orphan marker when stock saver is active")
                self.assertFalse((staleStage / "qml").exists(),
                                 "startup must remove orphaned staged assets")
                self.assertEqual((systemRoot / "var/lib/tvweb/screensaver-stock-type")
                                 .read_text().strip(), "flutter")

                command = {"action": "prepare", "mode": "clock", "level": "bright",
                           "port": publicPort, "token": "screen saver token"}
                status, body = request(command, withSecret=False)
                self.assertEqual(status, 403, body.decode("utf-8", "replace"))
                self.assertFalse(calls.exists(), "missing secret must not run mount commands")

                status, body = request(command)
                self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                self.assertEqual(json.loads(body), {
                    "ok": True, "mounted": True, "current": "clock", "level": "bright"
                })
                stage = systemRoot / "var/lib/tvweb/screensaver"
                self.assertEqual((stage / ".tvweb-screensaver").read_text().strip(), "clock")
                self.assertEqual((stage / ".tvweb-brightness").read_text().strip(), "bright")
                stagedManifest = json.loads((stage / "appinfo.json").read_text())
                self.assertEqual(stagedManifest["type"], "qml")
                self.assertEqual(stagedManifest["main"], "qml/main.qml")
                stagedQml = (stage / "qml/main.qml").read_text()
                self.assertIn("http://127.0.0.1:%d/api/stats?k=screen%%20saver%%20token" % publicPort,
                              stagedQml)
                self.assertIn("property int bright: 1", stagedQml)
                self.assertEqual((stage / "qml/star.png").read_bytes(), b"png fixture")
                self.assertEqual((systemRoot / "var/lib/tvweb/screensaver-stock-type")
                                 .read_text().strip(), "flutter")

                restage = dict(command)
                restage["action"] = "restage"
                status, body = request(restage)
                self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                self.assertEqual(json.loads(body)["current"], "clock")
                self.assertEqual((systemRoot / "var/lib/tvweb/screensaver-stock-type")
                                 .read_text().strip(), "flutter",
                                 "restaging under a bind mount must preserve the stock runner type")

                status, body = request({"action": "restore"})
                self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                self.assertFalse((stage / ".tvweb-screensaver").exists())
                self.assertFalse((stage / "appinfo.json").exists())
                self.assertFalse((stage / "qml").exists())
                self.assertIn("mount --bind", calls.read_text())

                process.send_signal(signal.SIGTERM)
                output, _ = process.communicate(timeout=6)
                self.assertEqual(process.returncode, 0, output)
                self.assertIn("command-policy: action=screensaver-stage result=success", output)
            finally:
                if process.poll() is None:
                    process.send_signal(signal.SIGTERM)
                    try:
                        process.communicate(timeout=6)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait(timeout=3)
                node.shutdown()
                node.server_close()
                nodeThread.join(timeout=3)

    def test_native_screensaver_list_route(self):
        binary = Path(sys.argv[1]).resolve()
        fakeLuna = binary.parent / "native-fake-luna-send"
        publicPort = unused_port_pair()
        nodePort = unused_port_pair((publicPort, publicPort + 1))
        token = "screensaver-list-token"
        with tempfile.TemporaryDirectory(prefix="nativetv-native-saver-list-") as folder:
            root = Path(folder)
            config = root / "config.json"
            assets = root / "assets"
            systemRoot = root / "system"
            appDirectory = systemRoot / "usr/palm/applications/com.webos.app.screensaver"
            procRoot = root / "proc"
            (assets / "screensavers").mkdir(parents=True)
            (appDirectory).mkdir(parents=True)
            (procRoot / "net").mkdir(parents=True)
            (systemRoot / "etc").mkdir(parents=True)
            (systemRoot / "etc/issue").write_text("webOS 12.0.0\n")
            (systemRoot / "var/lib/webosbrew/init.d").mkdir(parents=True)
            (systemRoot / "var/lib/webosbrew/init.d/60-screensaver-hook").write_text("")
            (appDirectory / "appinfo.json").write_text(json.dumps({
                "id": "com.webos.app.screensaver", "type": "flutter", "main": "main"
            }))
            (assets / "screensavers/clock.qml").write_text("Text {}\n")
            config.write_text(json.dumps({
                "host": "127.0.0.1", "port": publicPort, "token": token,
                "allowControl": True
            }))
            environment = os.environ.copy()
            environment["TVWEB_LUNA_SEND"] = str(fakeLuna)
            environment["TVWEB_DATA_DIR"] = str(root / "data")
            environment["TVWEB_PROXY_SECRET"] = "screensaver-list-secret-0123456789abcdef"
            process = subprocess.Popen(
                [str(binary), "--config", str(config), "--heartbeat", str(root / "tvweb.beat"),
                 "--workers", "4", "--listen", "--node-port", str(nodePort),
                 "--assets-root", str(assets), "--proc-root", str(procRoot),
                 "--system-root", str(systemRoot)],
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                env=environment)

            def get(path, method="GET"):
                req = Request("http://127.0.0.1:%d%s" % (publicPort, path), method=method,
                              headers={"Authorization": "Bearer " + token})
                try:
                    with urlopen(req, timeout=4) as response:
                        return response.status, response.read()
                except HTTPError as error:
                    return error.code, error.read()

            try:
                deadline = time.monotonic() + 8
                while time.monotonic() < deadline and process.poll() is None:
                    try:
                        if get("/api/caps")[0] == 200:
                            break
                    except (OSError, URLError):
                        time.sleep(0.03)
                self.assertIsNone(process.poll(), "daemon exited early")

                # Flutter on webOS 12 is held back: stock is the only saver offered.
                status, body = get("/api/screensaver")
                self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                held = json.loads(body)
                self.assertTrue(held["ok"])
                self.assertEqual(held["current"], "stock")
                self.assertEqual(held["level"], "dim")
                self.assertTrue(held["writable"])
                self.assertTrue(held["slowSwitch"])
                self.assertTrue(held["held"])
                self.assertFalse(held["heldOverridden"])
                self.assertFalse(held["switching"])
                self.assertEqual(held["external"], {"active": False, "hook": True})
                self.assertEqual([m["id"] for m in held["modes"]],
                                 ["stock", "clock", "starfield", "fireworks",
                                  "bokeh", "harmonograph", "vitals"])
                available = {m["id"]: m["available"] for m in held["modes"]}
                self.assertEqual([k for k, v in available.items() if v], ["stock"])

                status, body = get("/api/screensaver", method="HEAD")
                self.assertEqual(status, 200)
                self.assertEqual(body, b"")
                status, body = get("/api/screensaver", method="PUT")
                self.assertEqual(status, 405, body.decode("utf-8", "replace"))

                process.send_signal(signal.SIGTERM)
                output, _ = process.communicate(timeout=6)
                self.assertEqual(process.returncode, 0, output)

                # Allowed anyway, with a mounted saver and a bright level.
                config.write_text(json.dumps({
                    "host": "127.0.0.1", "port": publicPort, "token": token,
                    "allowControl": False, "allowExperimentalScreensavers": True
                }))
                (appDirectory / ".tvweb-screensaver").write_text("clock\n")
                (appDirectory / ".tvweb-brightness").write_text("bright\n")
                process = subprocess.Popen(
                    [str(binary), "--config", str(config), "--heartbeat", str(root / "tvweb.beat"),
                     "--workers", "4", "--listen", "--node-port", str(nodePort),
                     "--assets-root", str(assets), "--proc-root", str(procRoot),
                     "--system-root", str(systemRoot)],
                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                    env=environment)
                deadline = time.monotonic() + 8
                while time.monotonic() < deadline and process.poll() is None:
                    try:
                        if get("/api/caps")[0] == 200:
                            break
                    except (OSError, URLError):
                        time.sleep(0.03)
                status, body = get("/api/screensaver")
                self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                open_list = json.loads(body)
                self.assertEqual(open_list["current"], "clock")
                self.assertEqual(open_list["level"], "bright")
                self.assertFalse(open_list["writable"])
                self.assertFalse(open_list["held"])
                self.assertTrue(open_list["heldOverridden"])
                available = {m["id"]: m["available"] for m in open_list["modes"]}
                self.assertEqual(sorted(k for k, v in available.items() if v),
                                 ["clock", "stock"])
            finally:
                if process.poll() is None:
                    process.send_signal(signal.SIGTERM)
                    try:
                        process.communicate(timeout=6)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait(timeout=3)

    def test_native_node_free_start_serves_without_node(self):
        # TVWEB_NODE_FREE=1 (tvwebctl, from native.env): the daemon is the whole server.
        # Nothing listens on the Node port, no proxy secret is given, and the setup,
        # a control, stats, settings, a restart and the shutdown all work.
        binary = Path(sys.argv[1]).resolve()
        fakeLuna = binary.parent / "native-fake-luna-send"
        fixture = json.loads((Path(__file__).parent / "fixtures/system-metrics.json").read_text())
        publicPort = unused_port_pair()
        nodePort = unused_port_pair((publicPort, publicPort + 1))
        token = "node-free-token"
        with tempfile.TemporaryDirectory(prefix="nativetv-native-nodefree-") as folder:
            root = Path(folder)
            config = root / "config.json"
            assets = root / "assets"
            data = root / "data"
            (assets / "screensavers").mkdir(parents=True)
            (root / "proc/net").mkdir(parents=True)
            (root / "system/etc").mkdir(parents=True)
            data.mkdir()
            (data / ".setup-pending").touch()
            restarts = root / "restarts"
            (data / "tvwebctl").write_text('#!/bin/sh\necho "$@" >> "%s"\n' % restarts)
            (root / "native-system.json").write_text(json.dumps(fixture["expected"]))
            config.write_text(json.dumps({
                "host": "127.0.0.1", "port": publicPort, "token": token,
                "allowControl": True}))
            environment = os.environ.copy()
            environment.pop("TVWEB_PROXY_SECRET", None)
            environment["TVWEB_NODE_FREE"] = "1"
            environment["TVWEB_LUNA_SEND"] = str(fakeLuna)
            environment["TVWEB_DATA_DIR"] = str(data)
            environment["TVWEB_STATS_SYSTEM_JSON"] = str(root / "native-system.json")
            process = subprocess.Popen(
                [str(binary), "--config", str(config), "--heartbeat", str(root / "tvweb.beat"),
                 "--workers", "4", "--listen", "--node-port", str(nodePort),
                 "--assets-root", str(assets), "--proc-root", str(root / "proc"),
                 "--system-root", str(root / "system")],
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                env=environment)

            def request(path, method="GET", payload=None):
                req = Request("http://127.0.0.1:%d%s" % (publicPort, path), method=method,
                              data=None if payload is None else json.dumps(payload).encode(),
                              headers={"Authorization": "Bearer " + token,
                                       "Content-Type": "application/json"})
                try:
                    with urlopen(req, timeout=20) as response:
                        return response.status, response.read()
                except HTTPError as error:
                    return error.code, error.read()

            output = ""
            try:
                deadline = time.monotonic() + 8
                while time.monotonic() < deadline and process.poll() is None:
                    try:
                        if request("/api/caps")[0] == 200:
                            break
                    except (OSError, URLError):
                        time.sleep(0.03)
                self.assertIsNone(process.poll(), "daemon exited early")
                status, body = request("/api/caps")
                self.assertTrue(json.loads(body)["setupNeeded"])

                # The setup read asked Node for its live part; with none it is the daemon's.
                status, body = request("/api/setup")
                self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                setup = json.loads(body)
                self.assertTrue(setup["needed"])
                self.assertEqual(setup["homeAssistant"]["state"], "disabled")

                status, body = request("/api/setup", "POST", {"action": "done"})
                self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                status, body = request("/api/caps")
                self.assertFalse(json.loads(body)["setupNeeded"])

                status, body = request("/api/control", "POST", {"action": "mute", "mute": True})
                self.assertEqual(status, 200, body.decode("utf-8", "replace"))

                status, body = request("/api/stats")
                self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                self.assertIn("capabilities", json.loads(body))

                status, body = request("/api/settings")
                self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                self.assertEqual(json.loads(body)["status"]["state"], "disabled")

                status, body = request("/api/nothing-here")
                self.assertEqual(status, 404, "a path nothing serves is not found, not forwarded")

                # A setting that restarts the server: the readiness check and the restart
                # are the daemon's own, and reach tvwebctl.
                status, body = request("/api/setup/network", "POST",
                                       {"action": "network", "open": True})
                self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                deadline = time.monotonic() + 5
                while time.monotonic() < deadline and not restarts.exists():
                    time.sleep(0.05)
                self.assertEqual(restarts.read_text().strip(), "restart")
                with self.assertRaises(OSError):
                    socket.create_connection(("127.0.0.1", nodePort), timeout=1).close()
            finally:
                if process.poll() is None:
                    process.send_signal(signal.SIGTERM)
                try:
                    output = process.communicate(timeout=8)[0]
                except subprocess.TimeoutExpired:
                    process.kill()
                    output = process.communicate(timeout=3)[0]
            self.assertEqual(process.returncode, 0, "shutdown was not clean:\n" + output)
            self.assertNotIn("state bridge", output, "the daemon tried to tell a Node that is not there")
            self.assertNotIn("native service unavailable", output)

    def test_native_default_start_is_node_free_and_retires_the_node_release(self):
        # tvwebctl starts `nativetv-daemon --listen` and nothing else: no Node port, no
        # secret, no switch. An install updated from the last Node release still holds
        # tvweb.js and lib/; the first start removes them and keeps the user's files.
        binary = Path(sys.argv[1]).resolve()
        fakeLuna = binary.parent / "native-fake-luna-send"
        fixture = json.loads((Path(__file__).parent / "fixtures/system-metrics.json").read_text())
        publicPort = unused_port_pair()
        token = "default-node-free-token"
        with tempfile.TemporaryDirectory(prefix="nativetv-native-upgrade-") as folder:
            root = Path(folder)
            install = root / "tvweb"
            assets = install / "assets"
            data = install
            (assets / "ui").mkdir(parents=True)
            (install / "lib").mkdir()
            (root / "proc/net").mkdir(parents=True)
            (root / "system/etc").mkdir(parents=True)
            for old in ("tvweb.js", "lib/luna.js", "lib/routes.js", "lib/remotebuttons.py",
                        "lib/home-takeover.js"):
                (install / old).write_text("// the Node release\n")
            (assets / "ui.html").write_text("<html>dashboard</html>")
            (assets / "ui/core.js").write_text("// the phone dashboard\n")
            (install / "home.json").write_text('{"enabled": false}')
            (install / "tvweb.restarts").write_text("Wed Oct 28: process gone - starting\n")
            config = install / "config.json"
            config.write_text(json.dumps({
                "host": "127.0.0.1", "port": publicPort, "token": token,
                "allowControl": True, "tmdbKey": "kept"}))
            before = config.read_text()
            (root / "native-system.json").write_text(json.dumps(fixture["expected"]))
            environment = os.environ.copy()
            for name in ("TVWEB_PROXY_SECRET", "TVWEB_NODE_FREE"):
                environment.pop(name, None)
            environment["TVWEB_LUNA_SEND"] = str(fakeLuna)
            environment["TVWEB_DATA_DIR"] = str(data)
            environment["TVWEB_STATS_SYSTEM_JSON"] = str(root / "native-system.json")
            process = subprocess.Popen(
                [str(binary), "--config", str(config), "--heartbeat", str(root / "tvweb.beat"),
                 "--workers", "4", "--listen", "--assets-root", str(assets),
                 "--proc-root", str(root / "proc"), "--system-root", str(root / "system")],
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, env=environment)

            def request(path):
                req = Request("http://127.0.0.1:%d%s" % (publicPort, path),
                              headers={"Authorization": "Bearer " + token})
                try:
                    with urlopen(req, timeout=20) as response:
                        return response.status, response.read()
                except HTTPError as error:
                    return error.code, error.read()

            try:
                deadline = time.monotonic() + 10
                while time.monotonic() < deadline and process.poll() is None:
                    try:
                        if request("/api/caps")[0] == 200:
                            break
                    except (OSError, URLError):
                        time.sleep(0.03)
                self.assertIsNone(process.poll(), "daemon exited early")
                status, body = request("/api/stats")
                self.assertEqual(status, 200, "stats are the daemon's own, with no Node behind it")
                status, _ = request("/api/nothing-here")
                self.assertEqual(status, 404, "a path nothing serves is not found, not forwarded")
                for old in ("tvweb.js", "lib/luna.js", "lib/routes.js", "lib/remotebuttons.py",
                            "lib/home-takeover.js"):
                    self.assertFalse((install / old).exists(), old + " of the Node release was kept")
                self.assertFalse((install / "lib").exists(), "the emptied lib folder was kept")
                self.assertTrue((assets / "ui.html").exists(), "the phone dashboard is not Node's")
                self.assertTrue((assets / "ui/core.js").exists())
                self.assertEqual(config.read_text(), before, "config.json must carry over untouched")
                self.assertEqual((install / "home.json").read_text(), '{"enabled": false}')
                self.assertEqual((install / "tvweb.restarts").read_text(),
                                 "Wed Oct 28: process gone - starting\n")
            finally:
                if process.poll() is None:
                    process.send_signal(signal.SIGTERM)
                try:
                    output = process.communicate(timeout=8)[0]
                except subprocess.TimeoutExpired:
                    process.kill()
                    output = process.communicate(timeout=3)[0]
            self.assertNotIn("state bridge", output)

    def test_native_stats_reuse_and_invalidation(self):
        binary = Path(sys.argv[1]).resolve()
        fakeLuna = binary.parent / "native-fake-luna-send"
        fixture = json.loads((Path(__file__).parent / "fixtures/system-metrics.json").read_text())
        publicPort = unused_port_pair()
        nodePort = unused_port_pair((publicPort, publicPort + 1))
        token = "stats-invalidation-token"
        with tempfile.TemporaryDirectory(prefix="nativetv-native-stats-") as folder:
            root = Path(folder)
            config = root / "config.json"
            assets = root / "assets"
            (assets / "screensavers").mkdir(parents=True)
            (root / "proc/net").mkdir(parents=True)
            (root / "system/etc").mkdir(parents=True)
            (root / "native-system.json").write_text(json.dumps(fixture["expected"]))
            config.write_text(json.dumps({
                "host": "127.0.0.1", "port": publicPort, "token": token,
                "allowControl": True}))
            details = root / "luna-details"
            environment = os.environ.copy()
            environment["TVWEB_LUNA_SEND"] = str(fakeLuna)
            environment["TVWEB_DATA_DIR"] = str(root / "data")
            environment["TVWEB_PROXY_SECRET"] = "stats-secret-0123456789abcdef-0123"
            environment["TVWEB_NATIVE_STATS"] = "1"
            environment["TVWEB_STATS_SYSTEM_JSON"] = str(root / "native-system.json")
            environment["NATIVETV_FAKE_LUNA_DETAILS"] = str(details)
            process = subprocess.Popen(
                [str(binary), "--config", str(config), "--heartbeat", str(root / "tvweb.beat"),
                 "--workers", "4", "--listen", "--node-port", str(nodePort),
                 "--assets-root", str(assets), "--proc-root", str(root / "proc"),
                 "--system-root", str(root / "system")],
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                env=environment)

            def request(path, method="GET", payload=None):
                req = Request("http://127.0.0.1:%d%s" % (publicPort, path), method=method,
                              data=None if payload is None else json.dumps(payload).encode(),
                              headers={"Authorization": "Bearer " + token,
                                       "Content-Type": "application/json"})
                try:
                    with urlopen(req, timeout=20) as response:
                        return response.status, response.read()
                except HTTPError as error:
                    return error.code, error.read()

            def lunaCalls():
                return len(details.read_text().splitlines()) if details.exists() else 0

            try:
                deadline = time.monotonic() + 8
                while time.monotonic() < deadline and process.poll() is None:
                    try:
                        if request("/api/caps")[0] == 200:
                            break
                    except (OSError, URLError):
                        time.sleep(0.03)
                self.assertIsNone(process.poll(), "daemon exited early")
                status, body = request("/api/stats?fast=1")
                self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                self.assertIn("device", json.loads(body),
                              "the first load must show what is known, or the whole read")
                status, body = request("/api/stats")
                self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                self.assertIn("capabilities", json.loads(body))
                afterFirst = lunaCalls()
                self.assertGreater(afterFirst, 0, "the first collection must ask the TV")
                status, _ = request("/api/stats")
                self.assertEqual(status, 200)
                self.assertEqual(lunaCalls(), afterFirst,
                                 "a read within 1.5 s must be answered from the last one")
                status, body = request("/api/control", "POST", {"action": "mute", "mute": True})
                self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                afterWrite = lunaCalls()
                status, _ = request("/api/stats")
                self.assertEqual(status, 200)
                self.assertGreater(lunaCalls(), afterWrite,
                                   "an accepted control must make the next read fresh")
            finally:
                if process.poll() is None:
                    process.send_signal(signal.SIGTERM)
                    try:
                        process.communicate(timeout=6)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.communicate(timeout=3)

    def start_control_daemon(self, folder, extra=None, statsFixture=False):
        binary = Path(sys.argv[1]).resolve()
        root = Path(folder)
        publicPort = unused_port_pair()
        nodePort = unused_port_pair((publicPort, publicPort + 1))
        (root / "assets/screensavers").mkdir(parents=True)
        (root / "proc/net").mkdir(parents=True)
        (root / "system/etc").mkdir(parents=True)
        (root / "config.json").write_text(json.dumps({
            "host": "127.0.0.1", "port": publicPort, "token": "t", "allowControl": True}))
        environment = os.environ.copy()
        environment["TVWEB_LUNA_SEND"] = str(binary.parent / "native-fake-luna-send")
        environment["TVWEB_DATA_DIR"] = str(root / "data")
        environment["TVWEB_PROXY_SECRET"] = "control-secret-0123456789abcdef-0123"
        environment["NATIVETV_FAKE_LUNA_DETAILS"] = str(root / "luna-details")
        if statsFixture:
            fixture = json.loads((Path(__file__).parent / "fixtures/system-metrics.json").read_text())
            (root / "native-system.json").write_text(json.dumps(fixture["expected"]))
            environment["TVWEB_NATIVE_STATS"] = "1"
            environment["TVWEB_STATS_SYSTEM_JSON"] = str(root / "native-system.json")
        environment.update(extra or {})
        process = subprocess.Popen(
            [str(binary), "--config", str(root / "config.json"),
             "--heartbeat", str(root / "tvweb.beat"), "--workers", "4", "--listen",
             "--node-port", str(nodePort), "--assets-root", str(root / "assets"),
             "--proc-root", str(root / "proc"), "--system-root", str(root / "system")],
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, env=environment)

        def request(path, method="GET", payload=None):
            req = Request("http://127.0.0.1:%d%s" % (publicPort, path), method=method,
                          data=None if payload is None else json.dumps(payload).encode(),
                          headers={"Authorization": "Bearer t",
                                   "Content-Type": "application/json"})
            try:
                with urlopen(req, timeout=20) as response:
                    return response.status, response.read()
            except HTTPError as error:
                return error.code, error.read()

        deadline = time.monotonic() + 8
        while time.monotonic() < deadline and process.poll() is None:
            try:
                if request("/api/caps")[0] == 200:
                    break
            except (OSError, URLError):
                time.sleep(0.03)
        self.assertIsNone(process.poll(), "daemon exited early")
        return process, request, root / "luna-details"

    def stop_control_daemon(self, process):
        if process.poll() is None:
            process.send_signal(signal.SIGTERM)
            try:
                process.communicate(timeout=6)
            except subprocess.TimeoutExpired:
                process.kill()
                process.communicate(timeout=3)

    def test_native_sound_output_gives_up_after_four_seconds(self):
        # A B8 on HDMI ARC with nothing answering never reports the new route:
        # the change is still answered, once the audio service has been polled
        # for 4 s (test-sound-output.js).
        with tempfile.TemporaryDirectory(prefix="nativetv-native-sound-") as folder:
            process, request, details = self.start_control_daemon(
                folder, {"NATIVETV_FAKE_LUNA_SOUND_STUCK": "1"})
            try:
                started = time.monotonic()
                status, body = request("/api/control", "POST",
                                       {"action": "sound_output", "value": "external_arc"})
                took = time.monotonic() - started
                self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                self.assertEqual(json.loads(body), {"ok": True})
                self.assertGreaterEqual(took, 3.8, "answered before the 4 s were up")
                self.assertLess(took, 6.5, "answered long after the 4 s")
                polls = [json.loads(line) for line in details.read_text().splitlines()]
                self.assertGreaterEqual(len([call for call in polls if call["uri"] ==
                                             "com.webos.service.audio/master/getVolume"]), 4,
                                        "the route is read again while waiting")
            finally:
                self.stop_control_daemon(process)

    def test_native_refused_volume_is_noted_in_stats(self):
        # A volume change the TV refuses where the sound goes now is passed to the
        # stats collector, which stops offering the volume there (test-controls.js).
        with tempfile.TemporaryDirectory(prefix="nativetv-native-refused-") as folder:
            process, request, details = self.start_control_daemon(
                folder, {"NATIVETV_FAKE_LUNA_MODE": "quick-access"}, statsFixture=True)
            try:
                status, body = request("/api/stats")
                self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                self.assertNotEqual(json.loads(body)["volume_control"], "none",
                                    "the volume can be set before the refusal")
                request("/api/control", "POST", {"action": "volume", "value": 0})
                status, body = request("/api/stats")
                self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                self.assertEqual(json.loads(body)["volume_control"], "none",
                                 "a refusal for this output must stop offering volume")
            finally:
                self.stop_control_daemon(process)

    def test_native_stats_source_frame_rate_and_piccap(self):
        binary = Path(sys.argv[1]).resolve()
        fakeLuna = binary.parent / "native-fake-luna-send"
        fixture = json.loads((Path(__file__).parent / "fixtures/system-metrics.json").read_text())
        token = "stats-extras-token"

        def run(mode, installPiccap, check):
            publicPort = unused_port_pair()
            nodePort = unused_port_pair((publicPort, publicPort + 1))
            with tempfile.TemporaryDirectory(prefix="nativetv-native-stats-extras-") as folder:
                root = Path(folder)
                (root / "assets/screensavers").mkdir(parents=True)
                (root / "proc/net").mkdir(parents=True)
                (root / "system/etc").mkdir(parents=True)
                (root / "system/etc/issue").write_text("webOS TV 4.4.3\n")
                if installPiccap:
                    (root / "system/media/developer/apps/usr/palm/applications/org.webosbrew.piccap").mkdir(parents=True)
                (root / "native-system.json").write_text(json.dumps(fixture["expected"]))
                (root / "config.json").write_text(json.dumps({
                    "host": "127.0.0.1", "port": publicPort, "token": token, "allowControl": True}))
                environment = os.environ.copy()
                environment["TVWEB_LUNA_SEND"] = str(fakeLuna)
                environment["TVWEB_DATA_DIR"] = str(root / "data")
                environment["TVWEB_PROXY_SECRET"] = "stats-extras-secret-0123456789abcdef"
                environment["TVWEB_NATIVE_STATS"] = "1"
                environment["TVWEB_STATS_SYSTEM_JSON"] = str(root / "native-system.json")
                environment["NATIVETV_FAKE_LUNA_MODE"] = mode
                environment["NATIVETV_FAKE_LUNA_FPS_FOREGROUND"] = "onscreen"
                process = subprocess.Popen(
                    [str(binary), "--config", str(root / "config.json"),
                     "--heartbeat", str(root / "tvweb.beat"), "--workers", "4", "--listen",
                     "--node-port", str(nodePort), "--assets-root", str(root / "assets"),
                     "--proc-root", str(root / "proc"), "--system-root", str(root / "system")],
                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, env=environment)

                def stats():
                    req = Request("http://127.0.0.1:%d/api/stats" % publicPort,
                                  headers={"Authorization": "Bearer " + token})
                    try:
                        with urlopen(req, timeout=30) as response:
                            return json.loads(response.read())
                    except HTTPError as error:
                        self.fail("stats %d: %s" % (error.code, error.read()[:300]))

                try:
                    deadline = time.monotonic() + 8
                    while time.monotonic() < deadline and process.poll() is None:
                        try:
                            req = Request("http://127.0.0.1:%d/api/caps" % publicPort,
                                          headers={"Authorization": "Bearer " + token})
                            with urlopen(req, timeout=2):
                                break
                        except (OSError, URLError):
                            time.sleep(0.03)
                    self.assertIsNone(process.poll(), "daemon exited early")
                    check(stats)
                finally:
                    if process.poll() is None:
                        process.send_signal(signal.SIGTERM)
                        try:
                            process.communicate(timeout=6)
                        except subprocess.TimeoutExpired:
                            process.kill()
                            process.communicate(timeout=3)

        def frameRate(stats):
            # The first read after a switch to an input waits for the bind; a
            # later one, past the 1.5 s reuse, has it.
            seen = None
            for _ in range(8):
                seen = stats()
                if seen.get("source_frame_rate"):
                    break
                time.sleep(1.7)
            self.assertEqual(seen["source_frame_rate"],
                             {"hz": 119.88, "vrr_type": "VRR", "port": "HDMI_2"})
            self.assertNotIn("piccapCapture", seen, "no PicCap app, no row")

        run("game-fps", False, frameRate)

        def piccap(stats):
            seen = stats()
            self.assertIn("running", seen.get("piccapCapture", {}),
                          "an installed, answering PicCap shows its row")
            self.assertIsNone(seen["source_frame_rate"])

        run("piccap", True, piccap)

        def piccapAbsent(stats):
            self.assertNotIn("piccapCapture", stats())

        run("piccap", False, piccapAbsent)

    def test_native_power_timer_writes(self):
        # test/test-timers.js: LG's On and Off Timers, as written through /api/control.
        binary = Path(sys.argv[1]).resolve()
        fakeLuna = binary.parent / "native-fake-luna-send"
        publicPort = unused_port_pair()
        nodePort = unused_port_pair((publicPort, publicPort + 1))
        token = "power-timer-token"
        c2 = {"onTimerEnable": "on", "onTimerHour": "7", "onTimerMinute": "5",
              "onTimerWeekday": "62", "onTimerAppId": "com.webos.app.livetv",
              "onTimerChannel": "noChannel", "autoOff2HourOnTimer": "on",
              "offTimerEnable": "off", "offTimerHour": "24", "offTimerMinute": "0",
              "offTimerWeekday": "0", "sleepTimer": "off"}
        untuned = {"onTimerEnable": "off", "onTimerAppId": "com.webos.app.livetv",
                   "onTimerChannel": "noChannel"}
        with tempfile.TemporaryDirectory(prefix="nativetv-native-timers-") as folder:
            root = Path(folder)
            config = root / "config.json"
            assets = root / "assets"
            (assets / "screensavers").mkdir(parents=True)
            (root / "proc/net").mkdir(parents=True)
            (root / "system/etc").mkdir(parents=True)
            config.write_text(json.dumps({
                "host": "127.0.0.1", "port": publicPort, "token": token, "allowControl": True}))
            details = root / "luna-details"
            stored = root / "stored.json"
            noHome = root / "no-home-app"
            environment = os.environ.copy()
            environment["TVWEB_LUNA_SEND"] = str(fakeLuna)
            environment["TVWEB_DATA_DIR"] = str(root / "data")
            environment["TVWEB_PROXY_SECRET"] = "power-timer-secret-0123456789abcdef-0123"
            environment["NATIVETV_FAKE_LUNA_MODE"] = "quick-access"
            environment["NATIVETV_FAKE_LUNA_DETAILS"] = str(details)
            environment["NATIVETV_FAKE_TIME_SETTINGS_FILE"] = str(stored)
            environment["NATIVETV_FAKE_NO_HOME_APP"] = str(noHome)
            process = subprocess.Popen(
                [str(binary), "--config", str(config), "--heartbeat", str(root / "tvweb.beat"),
                 "--workers", "4", "--listen", "--node-port", str(nodePort),
                 "--assets-root", str(assets), "--proc-root", str(root / "proc"),
                 "--system-root", str(root / "system")],
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, env=environment)

            def request(path, method="GET", payload=None):
                req = Request("http://127.0.0.1:%d%s" % (publicPort, path), method=method,
                              data=None if payload is None else json.dumps(payload).encode(),
                              headers={"Authorization": "Bearer " + token,
                                       "Content-Type": "application/json"})
                try:
                    with urlopen(req, timeout=20) as response:
                        return response.status, response.read()
                except HTTPError as error:
                    return error.code, error.read()

            def timer(value, settings, homeApp=True):
                stored.write_text(json.dumps(settings))
                if homeApp and noHome.exists():
                    noHome.unlink()
                if not homeApp:
                    noHome.write_text("")
                before = len(details.read_text().splitlines()) if details.exists() else 0
                status, body = request("/api/control", "POST", {"action": "powerTimer", "value": value})
                self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                calls = [json.loads(line) for line in details.read_text().splitlines()][before:] \
                    if details.exists() else []
                writes = [call["payload"]["settings"] for call in calls
                          if call["uri"] == "com.webos.service.settings/setSystemSettings"]
                return json.loads(body), writes

            try:
                deadline = time.monotonic() + 8
                while time.monotonic() < deadline and process.poll() is None:
                    try:
                        if request("/api/caps")[0] == 200:
                            break
                    except (OSError, URLError):
                        time.sleep(0.03)
                self.assertIsNone(process.poll(), "daemon exited early")

                result, writes = timer({"timer": "off", "time": "23:30", "days": [0, 6]}, c2)
                self.assertEqual(result, {"ok": True})
                self.assertEqual(writes, [{"offTimerHour": "23", "offTimerMinute": "30",
                                           "offTimerWeekday": "65"}],
                                 "a timer that is off has its time and days stored, and stays off")
                result, writes = timer({"timer": "on", "time": "06:45"}, c2)
                self.assertEqual(writes, [{"onTimerEnable": "off"},
                                          {"onTimerHour": "6", "onTimerMinute": "45",
                                           "onTimerAppId": "com.webos.app.home"},
                                          {"onTimerEnable": "on"}],
                                 "a timer that is on is switched off, changed and on again")
                result, writes = timer({"timer": "off", "enabled": True}, c2)
                self.assertEqual(writes, [{"offTimerEnable": "off"}, {"offTimerEnable": "on"}],
                                 "switching a timer on arms it from off")
                result, writes = timer({"timer": "on", "enabled": False}, c2)
                self.assertEqual(writes, [{"onTimerEnable": "off"}],
                                 "switching a timer off is one write")
                result, writes = timer({"timer": "on", "enabled": True}, untuned)
                self.assertEqual(writes, [{"onTimerEnable": "off"},
                                          {"onTimerAppId": "com.webos.app.home"},
                                          {"onTimerEnable": "on"}],
                                 "Live TV with no channel turns the TV on to Home")
                result, writes = timer({"timer": "on", "enabled": True}, untuned, homeApp=False)
                self.assertEqual(writes, [{"onTimerEnable": "off"}, {"onTimerEnable": "on"}],
                                 "without a Home app (webOS 3-5) it is switched on as set")
                result, writes = timer({"timer": "on", "enabled": True},
                                       {"onTimerEnable": "off", "onTimerAppId": "com.webos.app.hdmi2",
                                        "onTimerChannel": "noChannel"})
                self.assertEqual(writes, [{"onTimerEnable": "off"}, {"onTimerEnable": "on"}],
                                 "an input already chosen is kept")
                for bad in ({"timer": "on", "time": "24:00"}, {"timer": "on", "time": "7"},
                            {"timer": "on", "days": [7]}, {"timer": "on", "days": "1"},
                            {"timer": "sleep", "enabled": True}, {"timer": "on"}):
                    result, writes = timer(bad, c2)
                    self.assertFalse(result["ok"], json.dumps(bad))
                    self.assertEqual(writes, [], "a refused timer writes nothing: " + json.dumps(bad))
            finally:
                if process.poll() is None:
                    process.send_signal(signal.SIGTERM)
                    try:
                        process.communicate(timeout=6)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.communicate(timeout=3)

    def test_native_json_body_guards(self):
        # test/test-routes.js readJsonBody: content type, origin and JSON shape, before any action.
        binary = Path(sys.argv[1]).resolve()
        fakeLuna = binary.parent / "native-fake-luna-send"
        publicPort = unused_port_pair()
        nodePort = unused_port_pair((publicPort, publicPort + 1))
        token = "body-guard-token"
        with tempfile.TemporaryDirectory(prefix="nativetv-native-guards-") as folder:
            root = Path(folder)
            config = root / "config.json"
            assets = root / "assets"
            (assets / "screensavers").mkdir(parents=True)
            (root / "proc/net").mkdir(parents=True)
            (root / "system/etc").mkdir(parents=True)
            config.write_text(json.dumps({
                "host": "127.0.0.1", "port": publicPort, "token": token, "allowControl": True}))
            details = root / "luna-details"
            environment = os.environ.copy()
            environment["TVWEB_LUNA_SEND"] = str(fakeLuna)
            environment["TVWEB_DATA_DIR"] = str(root / "data")
            environment["TVWEB_PROXY_SECRET"] = "body-guard-secret-0123456789abcdef-0123"
            environment["NATIVETV_FAKE_LUNA_DETAILS"] = str(details)
            process = subprocess.Popen(
                [str(binary), "--config", str(config), "--heartbeat", str(root / "tvweb.beat"),
                 "--workers", "4", "--listen", "--node-port", str(nodePort),
                 "--assets-root", str(assets), "--proc-root", str(root / "proc"),
                 "--system-root", str(root / "system")],
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, env=environment)

            def post(body, contentType="application/json", origin=None):
                headers = {"Authorization": "Bearer " + token}
                if contentType:
                    headers["Content-Type"] = contentType
                if origin:
                    headers["Origin"] = origin
                req = Request("http://127.0.0.1:%d/api/control" % publicPort, method="POST",
                              data=body, headers=headers)
                try:
                    with urlopen(req, timeout=20) as response:
                        return response.status, json.loads(response.read())
                except HTTPError as error:
                    return error.code, json.loads(error.read())

            def lunaCalls():
                # Volume calls only: the daemon makes start-up calls of its own meanwhile.
                lines = details.read_text().splitlines() if details.exists() else []
                return len([line for line in lines if "olume" in json.loads(line)["uri"]])

            try:
                deadline = time.monotonic() + 8
                while time.monotonic() < deadline and process.poll() is None:
                    try:
                        urlopen("http://127.0.0.1:%d/api/caps" % publicPort, timeout=2).read()
                        break
                    except (OSError, URLError, HTTPError):
                        time.sleep(0.03)
                self.assertIsNone(process.poll(), "daemon exited early")
                good = json.dumps({"action": "volume", "value": 20}).encode()
                callsAtStart = lunaCalls()
                self.assertEqual(post(good, "text/plain"),
                                 (415, {"ok": False, "error": "Content-Type must be application/json"}))
                self.assertEqual(post(good, None)[0], 415, "a missing Content-Type is refused too")
                self.assertEqual(post(good, origin="http://evil-attacker.com"),
                                 (403, {"ok": False, "error": "cross-origin request refused"}))
                self.assertEqual(post(b"{ not valid json"),
                                 (400, {"ok": False, "error": "malformed JSON"}))
                self.assertEqual(post(b"12345"), (400, {"ok": False, "error": "malformed JSON"}),
                                 "JSON that is not an object is malformed")
                self.assertEqual(lunaCalls(), callsAtStart, "a refused body reaches nothing")
                origin = "http://127.0.0.1:%d" % publicPort
                status, answer = post(good, origin=origin)
                self.assertEqual(status, 200, answer)
                self.assertGreater(lunaCalls(), callsAtStart, "a good body is acted on")
                self.assertEqual(post(good, "application/json; charset=utf-8")[0], 200,
                                 "a charset on the Content-Type is allowed")
            finally:
                if process.poll() is None:
                    process.send_signal(signal.SIGTERM)
                    try:
                        process.communicate(timeout=6)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.communicate(timeout=3)

    def test_native_services_keeper_runs_at_start_only_when_switched_on(self):
        binary = Path(sys.argv[1]).resolve()
        fakeLuna = binary.parent / "native-fake-luna-send"
        for switchedOn in (False, True):
            publicPort = unused_port_pair()
            nodePort = unused_port_pair((publicPort, publicPort + 1))
            with tempfile.TemporaryDirectory(prefix="nativetv-native-keeper-") as folder:
                root = Path(folder)
                (root / "assets/screensavers").mkdir(parents=True)
                (root / "proc/net").mkdir(parents=True)
                systemRoot = root / "system"
                hookDirectory = systemRoot / "var/lib/webosbrew/init.d"
                hookDirectory.mkdir(parents=True)
                (systemRoot / "etc").mkdir()
                oldScript = hookDirectory / "20-services.sh"
                oldScript.write_text("#!/bin/sh\n")
                (root / "data").mkdir()
                disabledList = root / "data/disabled_services.json"
                disabledList.write_text(json.dumps(["alwaysready", "mycar"]))
                (root / "config.json").write_text(json.dumps({
                    "host": "127.0.0.1", "port": publicPort, "token": "keeper-token",
                    "allowControl": True}))
                environment = os.environ.copy()
                environment["TVWEB_LUNA_SEND"] = str(fakeLuna)
                environment["TVWEB_DATA_DIR"] = str(root / "data")
                environment["TVWEB_PROXY_SECRET"] = "keeper-secret-0123456789abcdef-0123"
                if switchedOn:
                    environment["TVWEB_NATIVE_SERVICES"] = "1"
                process = subprocess.Popen(
                    [str(binary), "--config", str(root / "config.json"),
                     "--heartbeat", str(root / "tvweb.beat"), "--workers", "4", "--listen",
                     "--node-port", str(nodePort), "--assets-root", str(root / "assets"),
                     "--proc-root", str(root / "proc"), "--system-root", str(systemRoot)],
                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, env=environment)
                try:
                    deadline = time.monotonic() + 8
                    while time.monotonic() < deadline and process.poll() is None:
                        if switchedOn and not oldScript.exists():
                            break
                        time.sleep(0.1)
                    time.sleep(0.5)
                    self.assertIsNone(process.poll(), "daemon exited early")
                    if switchedOn:
                        self.assertEqual(json.loads(disabledList.read_text()), ["mycar"],
                                         "the retired entry goes from the list")
                        self.assertFalse(oldScript.exists(), "the old init script goes")
                        self.assertTrue((hookDirectory / "20-tvweb-services").is_file(),
                                        "the boot hook is written")
                    else:
                        self.assertEqual(json.loads(disabledList.read_text()),
                                         ["alwaysready", "mycar"],
                                         "Node's module owns the list while the flag is off")
                        self.assertTrue(oldScript.exists())
                finally:
                    if process.poll() is None:
                        process.send_signal(signal.SIGTERM)
                        try:
                            process.communicate(timeout=6)
                        except subprocess.TimeoutExpired:
                            process.kill()
                            process.communicate(timeout=3)

    def test_native_tile_hiding_flag_and_hidden_list(self):
        # test/test-apps.js: readHiddenAppsList filters protected ids; isTileHidingEnabled.
        binary = Path(sys.argv[1]).resolve()
        fakeLuna = binary.parent / "native-fake-luna-send"
        publicPort = unused_port_pair()
        nodePort = unused_port_pair((publicPort, publicPort + 1))
        token = "tile-hiding-flag-token"
        with tempfile.TemporaryDirectory(prefix="nativetv-native-tiles-") as folder:
            root = Path(folder)
            config = root / "config.json"
            assets = root / "assets"
            dataRoot = root / "data"
            (assets / "screensavers").mkdir(parents=True)
            (root / "proc/net").mkdir(parents=True)
            (root / "system/etc").mkdir(parents=True)
            dataRoot.mkdir()
            config.write_text(json.dumps({
                "host": "127.0.0.1", "port": publicPort, "token": token, "allowControl": True}))
            environment = os.environ.copy()
            environment["TVWEB_LUNA_SEND"] = str(fakeLuna)
            environment["TVWEB_DATA_DIR"] = str(dataRoot)
            environment["TVWEB_PROXY_SECRET"] = "tile-flag-secret-0123456789abcdef-0123"
            environment["NATIVETV_FAKE_LUNA_MODE"] = "apps-list"
            process = subprocess.Popen(
                [str(binary), "--config", str(config), "--heartbeat", str(root / "tvweb.beat"),
                 "--workers", "4", "--listen", "--node-port", str(nodePort),
                 "--assets-root", str(assets), "--proc-root", str(root / "proc"),
                 "--system-root", str(root / "system")],
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, env=environment)
            flag, hidden = dataRoot / "tile_hiding_enabled", dataRoot / "hidden_apps"

            def apps():
                req = Request("http://127.0.0.1:%d/api/apps" % publicPort,
                              headers={"Authorization": "Bearer " + token})
                with urlopen(req, timeout=20) as response:
                    self.assertEqual(response.status, 200)
                    return json.loads(response.read())

            def state(flagText, hiddenText):
                for path, text in ((flag, flagText), (hidden, hiddenText)):
                    if text is None:
                        if path.exists():
                            path.unlink()
                    else:
                        path.write_text(text)
                result = apps()
                return result["tileHidingEnabled"], result["hiddenCount"]

            try:
                deadline = time.monotonic() + 8
                while time.monotonic() < deadline and process.poll() is None:
                    try:
                        urlopen("http://127.0.0.1:%d/api/caps" % publicPort, timeout=2).read()
                        break
                    except (OSError, URLError, HTTPError):
                        time.sleep(0.03)
                self.assertIsNone(process.poll(), "daemon exited early")
                self.assertEqual(state(None, None), (False, 0), "no flag and no list is off")
                self.assertEqual(state("1\n", None)[0], True, "the flag file switches it on")
                self.assertEqual(state("0\n", None)[0], False, "an explicit 0 is off")
                self.assertEqual(state(None, "com.webos.app.igallery\n"), (True, 1),
                                 "a hidden list without a flag file means it is on")
                self.assertEqual(state("0\n", "com.webos.app.igallery\n")[0], False,
                                 "an explicit 0 overrides an existing hidden list")
                self.assertEqual(state("1\n", "com.webos.app.igallery\ncom.webos.app.livetv\n"
                                              "com.webos.app.music\n")[1], 2,
                                 "protected apps are filtered from the hidden list")
            finally:
                if process.poll() is None:
                    process.send_signal(signal.SIGTERM)
                    try:
                        process.communicate(timeout=6)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.communicate(timeout=3)

    def test_native_unknown_paths_are_a_404_without_node(self):
        # What Node ends its request handler with, answered with nothing behind the upstream.
        binary = Path(sys.argv[1]).resolve()
        fakeLuna = binary.parent / "native-fake-luna-send"
        publicPort = unused_port_pair()
        nodePort = unused_port_pair((publicPort, publicPort + 1))
        token = "unknown-path-token"
        with tempfile.TemporaryDirectory(prefix="nativetv-native-404-") as folder:
            root = Path(folder)
            config = root / "config.json"
            assets = root / "assets"
            (assets / "screensavers").mkdir(parents=True)
            (root / "proc/net").mkdir(parents=True)
            (root / "system").mkdir()
            config.write_text(json.dumps({
                "host": "127.0.0.1", "port": publicPort, "token": token}))
            environment = os.environ.copy()
            environment["TVWEB_LUNA_SEND"] = str(fakeLuna)
            environment["TVWEB_DATA_DIR"] = str(root / "data")
            environment["TVWEB_PROXY_SECRET"] = "unknown-path-secret-0123456789abcdef"
            environment["TVWEB_NODE_FREE"] = "1"
            process = subprocess.Popen(
                [str(binary), "--config", str(config), "--heartbeat", str(root / "tvweb.beat"),
                 "--workers", "4", "--listen", "--node-port", str(nodePort),
                 "--assets-root", str(assets), "--proc-root", str(root / "proc"),
                 "--system-root", str(root / "system")],
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                env=environment)

            def get(path, method="GET", auth=True):
                headers = {"Authorization": "Bearer " + token} if auth else {}
                req = Request("http://127.0.0.1:%d%s" % (publicPort, path), method=method,
                              headers=headers)
                try:
                    with urlopen(req, timeout=6) as response:
                        return response.status, response.read()
                except HTTPError as error:
                    return error.code, error.read()

            try:
                deadline = time.monotonic() + 8
                while time.monotonic() < deadline and process.poll() is None:
                    try:
                        if get("/api/caps")[0] == 200:
                            break
                    except (OSError, URLError):
                        time.sleep(0.03)
                self.assertIsNone(process.poll(), "daemon exited early")
                for path, method in (("/api/nothing-here", "GET"), ("/nothing-here", "GET"),
                                     ("/assets/ui/missing.js", "GET"), ("/mqtt", "POST"),
                                     ("/icons/com.webos.app.home.png", "GET")):
                    status, body = get(path, method)
                    self.assertEqual(status, 404, path)
                    self.assertEqual(json.loads(body), {"ok": False, "error": "not found"}, path)
            finally:
                if process.poll() is None:
                    process.send_signal(signal.SIGTERM)
                    try:
                        process.communicate(timeout=6)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.communicate(timeout=3)

    def test_native_server_status_route(self):
        binary = Path(sys.argv[1]).resolve()
        fakeLuna = binary.parent / "native-fake-luna-send"
        publicPort = unused_port_pair()
        nodePort = unused_port_pair((publicPort, publicPort + 1))
        token = "server-status-token"
        with tempfile.TemporaryDirectory(prefix="nativetv-native-status-") as folder:
            root = Path(folder)
            config = root / "config.json"
            assets = root / "assets"
            systemRoot = root / "system"
            appDirectory = systemRoot / "usr/palm/applications/com.webos.app.screensaver"
            procRoot = root / "proc"
            (assets / "screensavers").mkdir(parents=True)
            appDirectory.mkdir(parents=True)
            (procRoot / "net").mkdir(parents=True)
            (systemRoot / "etc").mkdir(parents=True)
            (systemRoot / "etc/issue").write_text("webOS 12.0.0\n")
            (appDirectory / "appinfo.json").write_text(json.dumps({
                "id": "com.webos.app.screensaver", "type": "flutter", "main": "main"
            }))
            config.write_text(json.dumps({
                "host": "127.0.0.1", "port": publicPort, "token": token,
                "allowControl": False, "allowNetworkDebugger": True,
                "allowTileHiding": True, "allowExperimentalScreensavers": True,
                "prometheus": {"enabled": True}, "backup": {"daily": True},
                "ntp": {"server": " pool.ntp.org ", "port": 0}
            }))
            environment = os.environ.copy()
            environment["TVWEB_LUNA_SEND"] = str(fakeLuna)
            environment["TVWEB_DATA_DIR"] = str(root / "data")
            environment["TVWEB_PROXY_SECRET"] = "server-status-secret-0123456789abcdef"
            process = subprocess.Popen(
                [str(binary), "--config", str(config), "--heartbeat", str(root / "tvweb.beat"),
                 "--workers", "4", "--listen", "--node-port", str(nodePort),
                 "--assets-root", str(assets), "--proc-root", str(procRoot),
                 "--system-root", str(systemRoot)],
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                env=environment)

            def get(path, method="GET"):
                req = Request("http://127.0.0.1:%d%s" % (publicPort, path), method=method,
                              headers={"Authorization": "Bearer " + token})
                try:
                    with urlopen(req, timeout=4) as response:
                        return response.status, response.read()
                except HTTPError as error:
                    return error.code, error.read()

            try:
                deadline = time.monotonic() + 8
                while time.monotonic() < deadline and process.poll() is None:
                    try:
                        if get("/api/caps")[0] == 200:
                            break
                    except (OSError, URLError):
                        time.sleep(0.03)
                self.assertIsNone(process.poll(), "daemon exited early")
                status, body = get("/api/update")
                self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                result = json.loads(body)
                self.assertTrue(result["ok"])
                self.assertFalse(result["writable"])
                self.assertFalse(result["tvUpdatesBlocked"])
                self.assertTrue(result["allowNetworkDebugger"])
                self.assertTrue(result["allowTileHiding"])
                self.assertTrue(result["allowExperimentalScreensavers"])
                self.assertTrue(result["isWebos10"])
                self.assertTrue(result["prometheusEnabled"])
                self.assertEqual(result["backupDaily"], {"enabled": True})
                self.assertEqual(result["ntpSettings"],
                                 {"server": "pool.ntp.org", "port": 123, "enabled": True})
                self.assertIsNone(result["ntp"])
                self.assertIsNone(result["syslog"])
                for key in ("devtools", "homeLayout", "watchHistory"):
                    self.assertIn(key, result)
                self.assertEqual(result["daemon"]["port"], publicPort)
                self.assertEqual(result["daemon"]["pid"], process.pid)
                self.assertGreaterEqual(result["daemon"]["uptime"], 0)
                self.assertIsNone(result["daemon"]["heapUsed"])
                status, body = get("/api/update", method="HEAD")
                self.assertEqual((status, body), (200, b""))
                self.assertEqual(get("/api/update", method="DELETE")[0], 405)
                # Read-only routes answer a wrong method themselves, not through Node.
                for route in ("/api/cpu", "/api/processes", "/api/hdmi", "/api/logs",
                              "/api/game", "/api/game/fps", "/api/lgsettings",
                              "/api/oledcare", "/api/servicemenu", "/api/stats"):
                    status, body = get(route, method="DELETE")
                    self.assertEqual(status, 405, route + ": " + body.decode("utf-8", "replace"))

                # Settings saved after start show at once, as Node's live config did.
                saved = json.loads(config.read_text())
                saved["prometheus"] = {"enabled": False}
                saved["allowNetworkDebugger"] = False
                replacement = root / "config.json.next"
                replacement.write_text(json.dumps(saved))
                os.replace(replacement, config)
                result = json.loads(get("/api/update")[1])
                self.assertFalse(result["prometheusEnabled"])
                self.assertFalse(result["allowNetworkDebugger"])
            finally:
                if process.poll() is None:
                    process.send_signal(signal.SIGTERM)
                    try:
                        process.communicate(timeout=6)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait(timeout=3)

    def test_native_tv_app_route_and_startup(self):
        binary = Path(sys.argv[1]).resolve()
        fakeLuna = binary.parent / "native-fake-luna-send"
        publicPort = unused_port_pair()
        nodePort = unused_port_pair((publicPort, publicPort + 1))
        token = "tv-app-token"
        with tempfile.TemporaryDirectory(prefix="nativetv-native-tvapp-") as folder:
            root = Path(folder)
            config = root / "config.json"
            assets = root / "assets"
            calls = root / "tvapp-calls"
            (assets / "dashboard-app").mkdir(parents=True)
            (assets / "home-app").mkdir()
            (assets / "home-app/appinfo.json").write_text("{}")
            (assets / "splash-app").mkdir()
            (assets / "splash-app/appinfo.json").write_text("{}")
            (assets / "dashboard-app/install-app.sh").write_text(
                "echo \"$@\" >> \"$TVAPP_CALLS\"\n"
                "case \"$1\" in\n"
                "  status) echo '{\"ok\":true,\"supported\":true,\"installed\":false}';;\n"
                "  install) echo 'Dashboard added to the home screen';;\n"
                "  remove) echo 'warming up'; echo '{\"ok\":true,\"installed\":false}';;\n"
                "  refresh) echo '{\"ok\":true,\"installed\":true,\"refreshed\":true}';;\n"
                "  retire) echo '{\"ok\":true,\"retired\":true}';;\n"
                "  *) echo garbage; exit 3;;\n"
                "esac\n")
            config.write_text(json.dumps({
                "host": "127.0.0.1", "port": publicPort, "token": token,
                "allowControl": True
            }))
            environment = os.environ.copy()
            environment["TVWEB_LUNA_SEND"] = str(fakeLuna)
            environment["TVWEB_DATA_DIR"] = str(root / "data")
            environment["TVWEB_PROXY_SECRET"] = "tv-app-secret-0123456789abcdef0123"
            environment["TVAPP_CALLS"] = str(calls)

            def launch():
                return subprocess.Popen(
                    [str(binary), "--config", str(config), "--heartbeat", str(root / "tvweb.beat"),
                     "--workers", "4", "--listen", "--node-port", str(nodePort),
                     "--assets-root", str(assets), "--proc-root", str(root / "proc"),
                     "--system-root", str(root / "system")],
                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                    env=environment)

            def call(path, method="GET", payload=None):
                data = json.dumps(payload).encode() if payload is not None else None
                headers = {"Authorization": "Bearer " + token}
                if data:
                    headers["Content-Type"] = "application/json"
                req = Request("http://127.0.0.1:%d%s" % (publicPort, path), data=data,
                              method=method, headers=headers)
                try:
                    with urlopen(req, timeout=8) as response:
                        return response.status, response.read()
                except HTTPError as error:
                    return error.code, error.read()

            def wait_ready(process):
                deadline = time.monotonic() + 8
                while time.monotonic() < deadline and process.poll() is None:
                    try:
                        if call("/api/caps")[0] == 200:
                            return
                    except (OSError, URLError):
                        time.sleep(0.03)
                self.fail("daemon did not start")

            (root / "proc/net").mkdir(parents=True)
            (root / "system").mkdir()
            process = launch()
            try:
                wait_ready(process)
                deadline = time.monotonic() + 5
                while time.monotonic() < deadline and not calls.exists():
                    time.sleep(0.05)
                # Both native client apps, the tile and then the splash, at every start.
                deadline = time.monotonic() + 5
                while time.monotonic() < deadline and len(calls.read_text().split("\n")) < 3:
                    time.sleep(0.05)
                self.assertEqual(calls.read_text().split("\n")[:2],
                                 ["refresh", "refresh " + str(assets / "splash-app")],
                                 "startup brings the tile's and the splash's copies up to date")
                status, body = call("/api/tvapp")
                self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                self.assertEqual(json.loads(body), {
                    "ok": True, "supported": True, "installed": False, "writable": True})
                status, body = call("/api/control", "POST", {"action": "tvAppInstall"})
                self.assertEqual(json.loads(body), {"ok": True, "refreshed": False})
                status, body = call("/api/control", "POST", {"action": "tvAppRemove"})
                self.assertEqual(json.loads(body), {"ok": True, "installed": False})
                self.assertEqual(call("/api/tvapp", "POST", {})[0], 405)
                logged = calls.read_text().split("\n")
                self.assertIn("install", logged)
                self.assertIn("remove", logged)
                process.send_signal(signal.SIGTERM)
                process.communicate(timeout=6)

                # A tile from the Homebrew Channel app is retired at boot instead.
                (root / "data").mkdir(exist_ok=True)
                (root / "data/.from-homebrew-channel").write_text("")
                calls.unlink()
                process = launch()
                wait_ready(process)
                deadline = time.monotonic() + 5
                while time.monotonic() < deadline and not calls.exists():
                    time.sleep(0.05)
                self.assertEqual(calls.read_text().split("\n")[0], "retire")
                self.assertNotIn("refresh", calls.read_text())
                process.send_signal(signal.SIGTERM)
                process.communicate(timeout=6)

                # With the script absent the app is simply unsupported.
                (assets / "dashboard-app/install-app.sh").unlink()
                process = launch()
                wait_ready(process)
                status, body = call("/api/tvapp")
                self.assertEqual(json.loads(body), {
                    "ok": True, "supported": False, "installed": False, "writable": True})
            finally:
                if process.poll() is None:
                    process.send_signal(signal.SIGTERM)
                    try:
                        process.communicate(timeout=6)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait(timeout=3)

    def test_native_server_tab_controls(self):
        binary = Path(sys.argv[1]).resolve()
        fakeLuna = binary.parent / "native-fake-luna-send"
        publicPort = unused_port_pair()
        nodePort = unused_port_pair((publicPort, publicPort + 1))
        token = "server-controls-token"
        with tempfile.TemporaryDirectory(prefix="nativetv-native-server-tab-") as folder:
            root = Path(folder)
            config = root / "config.json"
            (root / "proc/net").mkdir(parents=True)
            (root / "system").mkdir()
            (root / "assets").mkdir()
            config.write_text(json.dumps({
                "host": "127.0.0.1", "port": publicPort, "token": token,
                "allowControl": True, "allowPower": False, "allowOnWebos10": True,
                "mqtt": {"host": "keep.example"}
            }))
            environment = os.environ.copy()
            environment["TVWEB_LUNA_SEND"] = str(fakeLuna)
            environment["TVWEB_DATA_DIR"] = str(root / "data")
            environment["TVWEB_PROXY_SECRET"] = "server-tab-secret-0123456789abcdef0"
            process = subprocess.Popen(
                [str(binary), "--config", str(config), "--heartbeat", str(root / "tvweb.beat"),
                 "--workers", "4", "--listen", "--node-port", str(nodePort),
                 "--assets-root", str(root / "assets"), "--proc-root", str(root / "proc"),
                 "--system-root", str(root / "system")],
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                env=environment)

            def control(action, value=None):
                req = Request("http://127.0.0.1:%d/api/control" % publicPort,
                              data=json.dumps({"action": action, "value": value}).encode(),
                              method="POST",
                              headers={"Authorization": "Bearer " + token,
                                       "Content-Type": "application/json"})
                with urlopen(req, timeout=15) as response:
                    return response.status, json.loads(response.read())

            def saved():
                return json.loads(config.read_text())

            try:
                deadline = time.monotonic() + 8
                while time.monotonic() < deadline and process.poll() is None:
                    try:
                        urlopen(Request("http://127.0.0.1:%d/api/caps" % publicPort), timeout=2)
                        break
                    except (OSError, URLError):
                        time.sleep(0.03)
                self.assertIsNone(process.poll(), "daemon exited early")

                status, result = control("setPrometheusEnabled", "on")
                self.assertEqual(status, 200)
                self.assertTrue(result["ok"] and result["prometheusEnabled"])
                self.assertEqual(saved()["prometheus"], {"enabled": True})
                self.assertEqual(saved()["mqtt"], {"host": "keep.example"})
                self.assertFalse(control("setPrometheusEnabled", False)[1]["prometheusEnabled"])

                # NTP: a bad port and a bad host are refused, a good one is saved on.
                self.assertIn("1 to 65535", control("setNtpServer", "pool.ntp.org:99999")[1]["error"])
                self.assertIn("server name", control("setNtpServer", "not a host!")[1]["error"])
                self.assertNotIn("ntp", saved())
                result = control("setNtpServer", "time.example.org:124")[1]
                self.assertEqual(result["ntpSettings"],
                                 {"server": "time.example.org", "port": 124, "enabled": True})
                self.assertEqual(saved()["ntp"],
                                 {"server": "time.example.org", "port": 124, "enabled": True})
                self.assertFalse(control("setNtpEnabled", False)[1]["ntpSettings"]["enabled"])
                self.assertEqual(saved()["ntp"]["server"], "time.example.org")
                self.assertEqual(control("setNtpServer", "")[1]["ntpSettings"]["server"], "")

                result = control("setBackupDaily", True)[1]
                self.assertEqual(result["backupDaily"], {"enabled": True})
                self.assertEqual(saved()["backup"], {"daily": True})

                result = control("setNetworkDebuggerAllowed", "true")[1]
                self.assertTrue(result["allowNetworkDebugger"])
                self.assertIs(saved()["allowNetworkDebugger"], True)

                # The experimental screen saver switch drops the old key's name.
                result = control("setWebos10ScreensaversAllowed", True)[1]
                self.assertTrue(result["allowExperimentalScreensavers"])
                self.assertIs(saved()["allowExperimentalScreensavers"], True)
                self.assertNotIn("allowOnWebos10", saved())
                self.assertEqual(saved()["mqtt"], {"host": "keep.example"})

                # Tile hiding: held back until allowed, and not from the Homebrew Channel.
                result = control("setTileHidingAllowed", False)[1]
                self.assertFalse(result["allowTileHiding"])
                self.assertIs(saved()["allowTileHiding"], False)
                held = control("tileHiding", True)[1]
                self.assertFalse(held["ok"])
                self.assertIn("turned off on this TV", held["error"])
                (root / "data").mkdir(exist_ok=True)
                (root / "data/.from-homebrew-channel").write_text("")
                self.assertIn("Homebrew Channel", control("tileHiding", True)[1]["error"])

                # The dashboard port: range, a port in use, then a real change.
                for refused in ("", "abc", "12ab", "80", "1023", "65534", "9996", "9997", "9998",
                                "8080.5", "-8081", None):
                    result = control("setDashboardPort", refused)[1]
                    self.assertFalse(result["ok"], repr(refused))
                    self.assertIn("1024 to 65533", result["error"], repr(refused))
                self.assertIn("already using", control("setDashboardPort", publicPort + 1)[1]["error"])
                self.assertEqual(saved()["port"], publicPort)
                self.assertNotIn("restarting", control("setDashboardPort", publicPort)[1])
                with socket.socket() as probe:
                    probe.bind(("127.0.0.1", 0))
                    freePort = probe.getsockname()[1]
                result = control("setDashboardPort", str(freePort))[1]
                self.assertTrue(result["restarting"])
                self.assertEqual(result["newPort"], freePort)
                self.assertEqual(saved()["port"], freePort)
                self.assertEqual(saved()["mqtt"], {"host": "keep.example"})

                # allowPower is off in this config.
                result = control("reboot")[1]
                self.assertFalse(result["ok"])
                self.assertIn("allowPower", result["error"])
            finally:
                if process.poll() is None:
                    process.send_signal(signal.SIGTERM)
                    try:
                        process.communicate(timeout=6)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait(timeout=3)

    def test_native_preload_reapply_and_resend(self):
        binary = Path(sys.argv[1]).resolve()
        fakeLuna = binary.parent / "native-fake-luna-send"
        publicPort = unused_port_pair()
        nodePort = unused_port_pair((publicPort, publicPort + 1))
        with tempfile.TemporaryDirectory(prefix="nativetv-native-preload-start-") as folder:
            root = Path(folder)
            config = root / "config.json"
            details = root / "luna-details.jsonl"
            (root / "proc/net").mkdir(parents=True)
            (root / "assets").mkdir()
            (root / "system/tmp").mkdir(parents=True)
            (root / "system/tmp/boot-done").write_text("")
            (root / "data").mkdir()
            # The owner switched Netflix off; LG's list had it on.
            (root / "data/preload.json").write_text(json.dumps({
                "off": ["netflix"],
                "lg": [{"id": "netflix", "isEnabled": True, "requiredMemory": 100}]}))
            config.write_text(json.dumps({
                "host": "127.0.0.1", "port": publicPort, "token": "t", "allowControl": True}))
            environment = os.environ.copy()
            environment["TVWEB_LUNA_SEND"] = str(fakeLuna)
            environment["TVWEB_DATA_DIR"] = str(root / "data")
            environment["TVWEB_PROXY_SECRET"] = "preload-start-secret-0123456789abcdef"
            environment["NATIVETV_FAKE_LUNA_MODE"] = "quick-access"
            environment["NATIVETV_FAKE_LUNA_DETAILS"] = str(details)
            process = subprocess.Popen(
                [str(binary), "--config", str(config), "--heartbeat", str(root / "beat"),
                 "--workers", "4", "--listen", "--node-port", str(nodePort),
                 "--assets-root", str(root / "assets"), "--proc-root", str(root / "proc"),
                 "--system-root", str(root / "system")],
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                env=environment)
            try:
                deadline = time.monotonic() + 15
                calls = []
                while time.monotonic() < deadline and process.poll() is None:
                    time.sleep(0.2)
                    if details.exists():
                        calls = [json.loads(line) for line in details.read_text().splitlines()]
                        if any(c["uri"] == "com.webos.service.preloadmanager/setPreloadPolicy"
                               for c in calls):
                            break
                self.assertIsNone(process.poll(), "daemon exited early")
                writes = [c for c in calls if c["uri"] == "com.webos.service.config/setConfigs"]
                self.assertEqual(len(writes), 1, calls)
                written = writes[0]["payload"]["configs"][
                    "com.webos.service.preloadmanager.supportAppList"]
                self.assertEqual([(e["id"], e["isEnabled"]) for e in written],
                                 [("com.webos.app.home", True), ("com.webos.app.hdmi1", True),
                                  ("netflix", False), ("airplay", False)])
                live = [c for c in calls
                        if c["uri"] == "com.webos.service.preloadmanager/setPreloadPolicy"]
                self.assertEqual(live[0]["payload"],
                                 {"application": {"id": "netflix", "isEnabled": False}})
            finally:
                if process.poll() is None:
                    process.send_signal(signal.SIGTERM)
                    try:
                        process.communicate(timeout=6)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait(timeout=3)

    def test_native_app_tiles_boot_reconciliation(self):
        binary = Path(sys.argv[1]).resolve()
        fakeLuna = binary.parent / "native-fake-luna-send"
        with tempfile.TemporaryDirectory(prefix="nativetv-app-tiles-boot-") as folder:
            root = Path(folder)
            systemRoot = root / "system"
            dataRoot = root / "data"
            appId = "example.tile"
            appDirectory = systemRoot / "media/system/apps/usr/palm/applications" / appId
            appDirectory.mkdir(parents=True)
            (appDirectory / "appinfo.json").write_text(json.dumps({
                "id": appId, "title": "Example Tile", "visible": True
            }))
            override = dataRoot / "appinfo-overrides" / (appId + ".json")
            override.parent.mkdir(parents=True)
            override.write_text(json.dumps({
                "id": appId, "title": "Example Tile", "visible": False
            }))
            dataRoot.mkdir(exist_ok=True)
            (dataRoot / "tile_hiding_enabled").write_text("1\n")
            (dataRoot / "hidden_apps").write_text(appId + "\n")
            (dataRoot / "screensaver-stock-type").write_text("flutter\n")
            tools = systemRoot / "bin"
            tools.mkdir(parents=True)
            mountCalls = root / "mount-calls"
            for command in ("mount", "umount"):
                tool = tools / command
                tool.write_text(
                    "#!/bin/sh\n"
                    "echo '%s' \"$*\" >> \"$NATIVETV_MOUNT_CALLS\"\n"
                    "exit 0\n" % command)
                tool.chmod(0o755)
            config = root / "config.json"
            config.write_text(json.dumps({
                "port": 8177, "allowControl": False, "allowTileHiding": True
            }))
            calls = root / "luna-calls"
            details = root / "luna-details"
            environment = os.environ.copy()
            environment["TVWEB_LUNA_SEND"] = str(fakeLuna)
            environment["TVWEB_DATA_DIR"] = str(dataRoot)
            environment["NATIVETV_FAKE_LUNA_MODE"] = "sam-rescan"
            environment["NATIVETV_FAKE_LUNA_LOG"] = str(calls)
            environment["NATIVETV_FAKE_LUNA_DETAILS"] = str(details)
            environment["NATIVETV_MOUNT_CALLS"] = str(mountCalls)
            command = [str(binary), "--config", str(config),
                       "--system-root", str(systemRoot), "--reconcile-app-tiles"]
            result = subprocess.run(command, capture_output=True, text=True,
                                    timeout=12, env=environment)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            mountOutput = mountCalls.read_text()
            self.assertIn("--bind", mountOutput)
            self.assertIn(str(override), mountOutput)
            self.assertIn(str(appDirectory / "appinfo.json"), mountOutput)
            lunaCalls = calls.read_text().splitlines() if calls.exists() else []
            self.assertTrue(any(
                "com.webos.service.config/setConfigs" in line
                for line in lunaCalls),
                "native startup must nudge SAM after restoring hidden app tiles; "
                "mount=%s output=%s" % (mountOutput, result.stdout + result.stderr))
            lunaDetails = [json.loads(line) for line in details.read_text().splitlines()]
            self.assertTrue(any(
                item["uri"] == "com.webos.applicationManager/getAppInfo" and
                item["payload"].get("id") == appId
                for item in lunaDetails), "SAM visibility must be checked after the nudge")

            shellCalls = root / "sam-restart-calls"
            fakeShell = tools / "sh"
            fakeShell.write_text(
                "#!/bin/sh\n"
                "echo \"$*\" >> \"$NATIVETV_SAM_RESTART_CALLS\"\n"
                "exit 0\n")
            fakeShell.chmod(0o755)
            environment["NATIVETV_FAKE_SAM_VISIBLE"] = "true"
            environment["NATIVETV_SAM_RESTART_CALLS"] = str(shellCalls)
            mountCalls.unlink()
            calls.unlink()
            details.unlink()
            result = subprocess.run(command, capture_output=True, text=True,
                                    timeout=12, env=environment)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("systemctl kill -s 9 sam.service", shellCalls.read_text(),
                          "SAM restart recovery must run when visibility stays stale")
            restartDetails = [json.loads(line) for line in details.read_text().splitlines()]
            self.assertTrue(any(
                item["uri"] == "com.webos.applicationManager/launch" and
                item["payload"].get("id") == "com.webos.app.hdmi2"
                for item in restartDetails),
                "SAM restart recovery must restore the app that was in front")

            (dataRoot / ".from-homebrew-channel").write_text("1\n")
            mountCalls.unlink()
            calls.unlink()
            details.unlink()
            shellCalls.unlink()
            result = subprocess.run(command, capture_output=True, text=True,
                                    timeout=4, env=environment)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertFalse(mountCalls.exists(),
                             "Homebrew Channel installs must not mount hidden tiles at boot")
            self.assertFalse(calls.exists(),
                             "Homebrew Channel installs must not restart or nudge SAM")

    def test_native_app_tile_hiding_internal_bridge(self):
        binary = Path(sys.argv[1]).resolve()
        fakeLuna = binary.parent / "native-fake-luna-send"
        publicPort = unused_port_pair()
        nodePort = unused_port_pair((publicPort, publicPort + 1))
        secret = "app-tiles-proxy-secret-0123456789abcdef"
        token = "app-tiles-route-token"
        with tempfile.TemporaryDirectory(prefix="nativetv-native-app-tiles-") as folder:
            root = Path(folder)
            config = root / "config.json"
            heartbeat = root / "tvweb.beat"
            assets = root / "assets"
            systemRoot = root / "system"
            dataRoot = root / "data"
            details = root / "luna-details.jsonl"
            toolLog = root / "mount.log"
            samVisible = root / "sam-visible"
            samVisible.write_text("false\n")
            assets.mkdir()
            appDirectory = systemRoot / "media/system/apps/usr/palm/applications/example.tile"
            appDirectory.mkdir(parents=True)
            appInfo = appDirectory / "appinfo.json"
            appInfo.write_text(json.dumps({
                "id": "example.tile", "title": "Example Tile", "visible": True,
                "vendorData": {"keep": 7}
            }))
            (systemRoot / "bin").mkdir(parents=True)
            for name in ("mount", "umount"):
                tool = systemRoot / "bin" / name
                tool.write_text("#!/bin/sh\nprintf '%s %s\\n' '" + name +
                                "' \"$*\" >> '" + str(toolLog) + "'\nexit 0\n")
                tool.chmod(0o755)
            config.write_text(json.dumps({
                "host": "127.0.0.1", "port": publicPort, "token": token,
                "allowControl": True, "allowTileHiding": True
            }))
            environment = os.environ.copy()
            environment["TVWEB_LUNA_SEND"] = str(fakeLuna)
            environment["TVWEB_DATA_DIR"] = str(dataRoot)
            environment["TVWEB_PROXY_SECRET"] = secret
            environment["NATIVETV_FAKE_LUNA_DETAILS"] = str(details)
            environment["NATIVETV_FAKE_LUNA_MODE"] = "sam-rescan"
            environment["NATIVETV_FAKE_SAM_VISIBLE_FILE"] = str(samVisible)
            process = subprocess.Popen(
                [str(binary), "--config", str(config), "--heartbeat", str(heartbeat),
                 "--workers", "4", "--listen", "--node-port", str(nodePort),
                 "--assets-root", str(assets), "--system-root", str(systemRoot)],
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                env=environment)

            def request(command, withSecret=True, originalRemote=None):
                data = json.dumps(command).encode("utf-8")
                headers = {"Content-Type": "application/json"}
                if withSecret:
                    headers["X-NativeTv-Proxy-Secret"] = secret
                if originalRemote is not None:
                    headers["X-NativeTv-Original-Remote"] = originalRemote
                req = Request("http://127.0.0.1:%d/_internal/apps/tile-hiding" % publicPort,
                              data=data, method="POST", headers=headers)
                try:
                    with urlopen(req, timeout=30) as response:
                        return response.status, response.read()
                except HTTPError as error:
                    return error.code, error.read()

            def publicRequest(path, command, origin=None):
                data = json.dumps(command).encode("utf-8")
                headers = {"Content-Type": "application/json",
                           "Authorization": "Bearer " + token}
                if origin is not None:
                    headers["Origin"] = origin
                req = Request("http://127.0.0.1:%d%s" % (publicPort, path),
                              data=data, method="POST", headers=headers)
                try:
                    with urlopen(req, timeout=30) as response:
                        return response.status, response.read()
                except HTTPError as error:
                    return error.code, error.read()

            try:
                deadline = time.monotonic() + 8
                ready = False
                while time.monotonic() < deadline and process.poll() is None:
                    req = Request("http://127.0.0.1:%d/api/caps" % publicPort,
                                  headers={"Authorization": "Bearer " + token})
                    try:
                        with urlopen(req, timeout=2):
                            ready = True
                            break
                    except (OSError, URLError):
                        time.sleep(0.03)
                if not ready:
                    if process.poll() is None:
                        process.send_signal(signal.SIGTERM)
                    output, _ = process.communicate(timeout=6)
                    self.fail("native daemon did not start:\n" + output[-4000:])

                status, body = publicRequest("/api/apps/hide", {"id": "example.tile"},
                                             origin="http://attacker.example")
                self.assertEqual(status, 403, body.decode("utf-8", "replace"))
                self.assertFalse(toolLog.exists(), "unauthorized public routes must not mount tiles")
                status, body = publicRequest("/api/apps/hide", {"id": "../etc/passwd"})
                self.assertEqual(status, 400, body.decode("utf-8", "replace"))
                self.assertEqual(json.loads(body), {"ok": False, "error": "Invalid app ID format"})

                samVisible.write_text("false\n")
                status, body = publicRequest("/api/apps/hide", {"id": "example.tile"})
                self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                self.assertEqual(json.loads(body), {
                    "ok": True, "id": "example.tile", "hidden": True,
                    "samRestarted": True
                })
                self.assertEqual((dataRoot / "hidden_apps").read_text(), "example.tile\n")
                self.assertEqual((dataRoot / "tile_hiding_enabled").read_text(), "1\n")
                samVisible.write_text("true\n")
                status, body = publicRequest("/api/apps/unhide", {"id": "example.tile"})
                self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                self.assertEqual(json.loads(body), {
                    "ok": True, "id": "example.tile", "hidden": False,
                    "samRestarted": True
                })
                self.assertEqual((dataRoot / "hidden_apps").read_text(), "")
                self.assertTrue((dataRoot / "tile_hiding_enabled").is_file())
                toolLog.unlink()

                status, body = request({"action": "hide", "id": "example.tile"},
                                       withSecret=False)
                self.assertEqual(status, 403, body.decode("utf-8", "replace"))
                status, body = request({"action": "hide", "id": "example.tile"},
                                       originalRemote="203.0.113.20")
                self.assertEqual(status, 403, body.decode("utf-8", "replace"))
                self.assertFalse(toolLog.exists(), "unauthorized calls must not run mount tools")
                for appId in ("../etc/passwd", "com.webos.app.livetv",
                              "com.webos.app.input.hdmi2"):
                    status, _ = request({"action": "hide", "id": appId})
                    self.assertEqual(status, 400, "unsafe/protected id accepted: %s" % appId)
                self.assertFalse(toolLog.exists(), "unsafe IDs must be rejected before mount tools")

                status, body = request({"action": "hide", "id": "example.tile"})
                self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                self.assertEqual(json.loads(body), {
                    "ok": True, "id": "example.tile", "hidden": True
                })
                override = dataRoot / "appinfo-overrides/example.tile.json"
                self.assertTrue(override.is_file())
                self.assertEqual(stat.S_IMODE(override.stat().st_mode), 0o600)
                hiddenApp = json.loads(override.read_text())
                self.assertFalse(hiddenApp["visible"])
                self.assertEqual(hiddenApp["vendorData"], {"keep": 7})

                for action in ("mount", "unmount", "mount"):
                    status, body = request({"action": action, "id": "example.tile"})
                    self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                    self.assertTrue(json.loads(body)["ok"])
                status, body = request({"action": "unhide", "id": "example.tile"})
                self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                self.assertTrue(json.loads(body)["ok"])
                self.assertFalse(override.exists(), "unhide must remove the native override")
                tools = toolLog.read_text()
                self.assertIn("mount --bind", tools)
                self.assertIn("umount -l", tools)
                self.assertIn(str(appInfo), tools)
                self.assertIn(str(appDirectory), tools)

                samVisible.write_text("false\n")
                status, body = request({"action": "hide-native", "id": "example.tile"})
                self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                lifecycle = json.loads(body)
                self.assertEqual(lifecycle["hidden"], True)
                self.assertEqual(lifecycle["tileHidingEnabled"], True)
                self.assertEqual(lifecycle["samRestarted"], True)
                self.assertEqual((dataRoot / "hidden_apps").read_text(), "example.tile\n")
                self.assertEqual((dataRoot / "tile_hiding_enabled").read_text(), "1\n")

                status, body = request({"action": "enable-all", "id": ""})
                self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                enabled = json.loads(body)
                self.assertEqual(enabled["tileHidingEnabled"], True)
                self.assertEqual(enabled["samRestarted"], True)
                self.assertIn("mount --bind", toolLog.read_text())

                samVisible.write_text("true\n")
                status, body = request({"action": "unhide-all-native", "id": ""})
                self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                restored = json.loads(body)
                self.assertEqual(restored["restoredCount"], 1)
                self.assertEqual(restored["tileHidingEnabled"], True)
                self.assertEqual(restored["samRestarted"], True)
                self.assertEqual((dataRoot / "hidden_apps").read_text(), "")
                self.assertFalse(override.exists())

                samVisible.write_text("false\n")
                status, body = publicRequest("/api/apps/hide", {"id": "example.tile"})
                self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                samVisible.write_text("true\n")
                status, body = publicRequest("/api/apps/tile-hiding", {"enabled": False})
                self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                self.assertEqual(json.loads(body), {
                    "ok": True, "tileHidingEnabled": False, "samRestarted": True
                })
                self.assertEqual((dataRoot / "tile_hiding_enabled").read_text(), "0\n")
                status, body = publicRequest("/api/apps/unhide-all", {})
                self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                self.assertEqual(json.loads(body), {
                    "ok": True, "restoredCount": 1, "samRestarted": True
                })
                self.assertEqual((dataRoot / "hidden_apps").read_text(), "")

                status, body = request({"action": "disable-all", "id": ""})
                self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                disabled = json.loads(body)
                self.assertEqual(disabled["tileHidingEnabled"], False)
                self.assertEqual(disabled["samRestarted"], True)
                self.assertEqual((dataRoot / "tile_hiding_enabled").read_text(), "0\n")

                process.send_signal(signal.SIGTERM)
                output, _ = process.communicate(timeout=6)
                self.assertEqual(process.returncode, 0, output)
                for action in ("hide", "mount", "unmount", "unhide"):
                    self.assertIn("command-policy: action=apps.tile-hiding." + action +
                                  " result=success", output)
            finally:
                if process.poll() is None:
                    process.send_signal(signal.SIGTERM)
                    try:
                        process.communicate(timeout=6)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait(timeout=3)

    def test_native_apps_list_projection_and_context_fallback(self):
        binary = Path(sys.argv[1]).resolve()
        fakeLuna = binary.parent / "native-fake-luna-send"
        publicPort = unused_port_pair()
        nodePort = unused_port_pair((publicPort, publicPort + 1))
        secret = "apps-list-context-secret-0123456789abcdef"
        token = "apps-list-route-token"
        with tempfile.TemporaryDirectory(prefix="nativetv-native-app-list-") as folder:
            root = Path(folder)
            config = root / "config.json"
            heartbeat = root / "tvweb.beat"
            assets = root / "assets"
            dataRoot = root / "data"
            systemRoot = root / "system"
            details = root / "luna-details.jsonl"
            assets.mkdir()
            keyFilters = systemRoot / "usr/lib/qt5/qml/KeyFilters"
            keyFilters.mkdir(parents=True)
            (keyFilters / "streaming.js").write_text(
                "WebOS.Key_webOS_Netflix WebOS.Key_webOS_Hulu WebOS.Key_webOS_ALEXA")
            (dataRoot / "hidden_apps").parent.mkdir(parents=True)
            (dataRoot / "native").mkdir(parents=True)
            (dataRoot / "native/remotekeys-filter.js").write_text(
                "// remote keys fixture filter\n")
            (dataRoot / "hidden_apps").write_text("org.example.hidden\n")
            (dataRoot / "remotekeys.json").write_text(json.dumps({
                "keys": {"hulu": "", "existing": "org.example.preserved"},
                "home": "com.webos.app.home", "starting": "Starting Native Tv",
                "splash": True
            }))
            (dataRoot / "tile_hiding_enabled").write_text("1\n")
            (dataRoot / "saved_page_titles.json").write_text(
                json.dumps({"page-1": "Original Movie Title"}))
            hiddenAppInfo = (systemRoot / "media/system/apps/usr/palm/applications/"
                             "org.example.hidden/appinfo.json")
            hiddenAppInfo.parent.mkdir(parents=True)
            hiddenAppInfo.write_text(json.dumps({
                "id": "org.example.hidden", "title": "Hidden Tile"}))
            config.write_text(json.dumps({
                "host": "127.0.0.1", "port": publicPort, "token": token,
                "allowControl": True, "allowTileHiding": True
            }))

            context = {
                "tileHidingAvailable": True, "tileHidingHeld": False,
                "tileHidingOverridden": False, "tileHidingNudge": True,
                "nativeTvHome": True, "services": [{"id": "network"}],
                "preload": [{"id": "netflix"}], "quickAccess": None,
                "remoteKeys": None, "homeScreen": {"enabled": True}
            }
            contextRequests = []
            bridgeState = {"fail": False}

            class NodeContextHandler(BaseHTTPRequestHandler):
                def do_POST(self):
                    length = int(self.headers.get("Content-Length", "0"))
                    body = self.rfile.read(length)
                    contextRequests.append({
                        "path": self.path,
                        "secret": self.headers.get("X-NativeTv-Proxy-Secret"),
                        "peer": self.headers.get("X-NativeTv-Original-Remote"),
                        "body": body
                    })
                    if self.path != "/_internal/apps-context":
                        payload = b'{"ok":false,"error":"not found"}'
                        self.send_response(404)
                    elif self.headers.get("X-NativeTv-Proxy-Secret") != secret or \
                            self.headers.get("X-NativeTv-Original-Remote") is not None:
                        payload = b'{"ok":false,"error":"forbidden"}'
                        self.send_response(403)
                    elif bridgeState["fail"]:
                        payload = b'{"ok":false,"error":"unavailable"}'
                        self.send_response(503)
                    else:
                        payload = json.dumps(context).encode("utf-8")
                        self.send_response(200)
                    self.send_header("Content-Length", str(len(payload)))
                    self.end_headers()
                    self.wfile.write(payload)

                def do_GET(self):
                    payload = json.dumps({"ok": True, "owner": "node-fallback"}).encode("utf-8")
                    self.send_response(200)
                    self.send_header("Content-Type", "application/json")
                    self.send_header("Content-Length", str(len(payload)))
                    self.end_headers()
                    self.wfile.write(payload)

                def log_message(self, format, *args):
                    pass

            nodeServer = HTTPServer(("127.0.0.1", nodePort), NodeContextHandler)
            nodeThread = threading.Thread(target=nodeServer.serve_forever)
            nodeThread.daemon = True
            nodeThread.start()
            environment = os.environ.copy()
            environment["TVWEB_LUNA_SEND"] = str(fakeLuna)
            environment["TVWEB_DATA_DIR"] = str(dataRoot)
            environment["TVWEB_PROXY_SECRET"] = secret
            environment["NATIVETV_FAKE_LUNA_MODE"] = "apps-list"
            environment["NATIVETV_FAKE_LUNA_DETAILS"] = str(details)
            process = subprocess.Popen(
                [str(binary), "--config", str(config), "--heartbeat", str(heartbeat),
                 "--workers", "4", "--listen", "--node-port", str(nodePort),
                 "--assets-root", str(assets), "--system-root", str(systemRoot)],
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                env=environment)

            def getApps(withToken=True):
                headers = {"Authorization": "Bearer " + token} if withToken else {}
                req = Request("http://127.0.0.1:%d/api/apps" % publicPort, headers=headers)
                try:
                    with urlopen(req, timeout=20) as response:
                        return response.status, response.read()
                except HTTPError as error:
                    return error.code, error.read()

            def getCatalogWithHost(host):
                req = Request("http://127.0.0.1:%d/api/apps/catalog" % publicPort,
                              headers={"Authorization": "Bearer " + token, "Host": host})
                try:
                    with urlopen(req, timeout=20) as response:
                        return response.status, response.read()
                except HTTPError as error:
                    return error.code, error.read()

            def postRemoteKey(key, action, target):
                requestBody = json.dumps({"key": key, "action": action,
                                          "id": target}).encode("utf-8")
                req = Request("http://127.0.0.1:%d/api/remotekeys" % publicPort,
                              data=requestBody, method="POST", headers={
                                  "Authorization": "Bearer " + token,
                                  "Content-Type": "application/json"
                              })
                try:
                    with urlopen(req, timeout=20) as response:
                        return response.status, response.read()
                except HTTPError as error:
                    return error.code, error.read()

            try:
                deadline = time.monotonic() + 8
                ready = False
                while time.monotonic() < deadline and process.poll() is None:
                    req = Request("http://127.0.0.1:%d/api/caps" % publicPort,
                                  headers={"Authorization": "Bearer " + token})
                    try:
                        with urlopen(req, timeout=2):
                            ready = True
                            break
                    except (OSError, URLError):
                        time.sleep(0.03)
                if not ready:
                    if process.poll() is None:
                        process.send_signal(signal.SIGTERM)
                    output, _ = process.communicate(timeout=6)
                    self.fail("native daemon did not start:\n" + output[-4000:])

                status, body = getApps(withToken=False)
                self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                self.assertTrue(json.loads(body)["ok"])
                self.assertEqual(len(contextRequests), 0,
                                 "the apps context is built by the daemon, not asked of Node")

                status, body = getApps()
                self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                result = json.loads(body)
                self.assertTrue(result["ok"])
                self.assertTrue(result["writable"])
                self.assertTrue(result["tileHidingAvailable"])
                self.assertTrue(result["tileHidingEnabled"])
                self.assertEqual(result["hiddenCount"], 1)
                self.assertIn("tileHidingNudge", result)
                self.assertIsInstance(result["services"], list)
                self.assertIsInstance(result["preload"], list)
                self.assertIn("quickAccess", result)
                self.assertIn("homeScreen", result)
                self.assertFalse(result["tileHidingHeld"])
                self.assertFalse(result["tileHidingOverridden"])
                self.assertTrue(result["remoteKeys"]["supported"])
                self.assertTrue(result["remoteKeys"]["inChain"])
                self.assertEqual([key["key"] for key in result["remoteKeys"]["keys"]],
                                 ["netflix", "hulu", "alexa"])
                self.assertEqual(result["remoteKeys"]["keys"][0]["action"], "lg")
                self.assertIn("com.webos.app.quickinputpicker",
                              [choice["id"] for choice in result["remoteKeys"]["choices"]])
                self.assertIn("Fixture HDMI 1",
                              [choice["title"] for choice in result["remoteKeys"]["choices"]])
                self.assertEqual([app["id"] for app in result["installed"]], [
                    "org.example.store", "org.example.youtube"])
                self.assertEqual(result["installed"][1]["size"], 42455683)
                self.assertEqual(result["systemTiles"], [
                    {"id": "com.webos.app.gallery", "title": "Gallery Home",
                     "iconUrl": "/api/apps/icon?id=com.webos.app.gallery", "hidden": False,
                     "systemApp": True, "removable": False},
                    {"id": "org.example.hidden", "title": "Hidden Tile",
                     "iconUrl": "/api/apps/icon?id=org.example.hidden", "hidden": True,
                     "systemApp": True, "removable": False}
                ])
                self.assertEqual(result["savedPages"], [{
                    "launchPointId": "page-1", "title": "Movie Site",
                    "appId": "com.webos.app.browser", "address": "https://example.org/watch",
                    "renamed": True
                }])
                self.assertEqual(contextRequests, [])
                lunaCalls = [json.loads(line) for line in details.read_text().splitlines()]
                self.assertTrue(any(call["uri"] == "com.webos.service.config/getConfigs" and
                                    call["payload"].get("configNames") == [
                                        "com.webos.surfacemanager.keyFilters"]
                                    for call in lunaCalls))
                self.assertTrue(any(call["uri"] ==
                                    "com.webos.service.eim/getAllInputStatus"
                                    for call in lunaCalls))

                status, body = postRemoteKey(
                    "netflix", "open", "org.example.not-installed")
                self.assertEqual(status, 400, body.decode("utf-8", "replace"))
                self.assertEqual(json.loads((dataRoot / "remotekeys.json").read_text())[
                    "keys"], {"hulu": "", "existing": "org.example.preserved"},
                    "an unavailable app target must not mutate the key map")

                status, body = postRemoteKey(
                    "netflix", "open", "org.example.youtube")
                self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                self.assertEqual(json.loads(body), {"ok": True})
                remoteKeyMap = json.loads((dataRoot / "remotekeys.json").read_text())
                self.assertEqual(remoteKeyMap["keys"], {
                    "hulu": "", "existing": "org.example.preserved",
                    "netflix": "org.example.youtube"
                })
                self.assertEqual(remoteKeyMap["home"], "com.webos.app.home")
                self.assertEqual(remoteKeyMap["starting"], "Starting Native Tv")
                self.assertTrue(remoteKeyMap["splash"])
                installedFilter = dataRoot / "remotekeys-filter.js"
                self.assertEqual(installedFilter.read_text(),
                                 "// remote keys fixture filter\n")
                lunaCalls = [json.loads(line) for line in details.read_text().splitlines()]
                configWrites = [call["payload"]["configs"] for call in lunaCalls
                                if call["uri"] == "com.webos.service.config/setConfigs"]
                self.assertGreaterEqual(len(configWrites), 3)
                self.assertEqual(configWrites[-3]["nativetv.remoteKeys"], {
                    "keys": remoteKeyMap["keys"], "home": "com.webos.app.home",
                    "starting": "Starting Native Tv", "splash": True
                })
                self.assertEqual(configWrites[-1][
                    "com.webos.surfacemanager.keyFilters"][0], {
                        "file": str(installedFilter),
                        "handler": "handleNativeTvKeys"
                    })

                # Alexa on JavaScript-filter firmware is a filter key like Netflix:
                # saved and written to configd here, with no app block and no Node.
                status, body = postRemoteKey("alexa", "open", "org.example.youtube")
                self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                self.assertEqual(json.loads((dataRoot / "remotekeys.json").read_text())[
                    "keys"]["alexa"], "org.example.youtube")
                lunaCalls = [json.loads(line) for line in details.read_text().splitlines()]
                self.assertEqual(
                    [call for call in lunaCalls
                     if call["uri"] == "com.webos.applicationManager/blockSystemApp"], [],
                    "the app block belongs to the native filters")
                keyWrites = [call["payload"]["configs"]["nativetv.remoteKeys"]
                             for call in lunaCalls
                             if call["uri"] == "com.webos.service.config/setConfigs" and
                             "nativetv.remoteKeys" in call["payload"]["configs"]]
                self.assertEqual(keyWrites[-1]["keys"]["alexa"], "org.example.youtube")

                status, body = getCatalogWithHost("unlisted.example")
                self.assertEqual(status, 403, body.decode("utf-8", "replace"))
                self.assertIn("not allowed to install apps", json.loads(body)["error"])

                # Node being unreachable changes nothing for the list.
                bridgeState["fail"] = True
                status, body = getApps()
                self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                self.assertTrue(json.loads(body)["ok"])
                self.assertEqual(len(contextRequests), 0)

                process.send_signal(signal.SIGTERM)
                output, _ = process.communicate(timeout=6)
                self.assertEqual(process.returncode, 0, output)
            finally:
                if process.poll() is None:
                    process.send_signal(signal.SIGTERM)
                    try:
                        process.communicate(timeout=6)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait(timeout=3)
                nodeServer.shutdown()
                nodeServer.server_close()

            offlineConfig = json.loads(config.read_text())
            offlineConfig["allowControl"] = False
            config.write_text(json.dumps(offlineConfig))
            restored = subprocess.run(
                [str(binary), "--restore-remote-keys", "--config", str(config),
                 "--assets-root", str(assets), "--system-root", str(systemRoot)],
                capture_output=True, text=True, env=environment, timeout=30)
            self.assertEqual(restored.returncode, 0,
                             restored.stdout + restored.stderr)
            self.assertFalse((dataRoot / "remotekeys.json").exists())
            self.assertFalse((dataRoot / "remotekeys-filter.js").exists())
            restoreWrites = [json.loads(line)["payload"]["configs"]
                             for line in details.read_text().splitlines()
                             if json.loads(line)["uri"] ==
                             "com.webos.service.config/setConfigs"]
            self.assertEqual(restoreWrites[-2]["nativetv.remoteKeys"], {
                "keys": {}
            })
            self.assertEqual(restoreWrites[-1][
                "com.webos.surfacemanager.keyFilters"], [])

    def test_native_webos11_remote_keys_read_and_write(self):
        binary = Path(sys.argv[1]).resolve()
        fakeLuna = binary.parent / "native-fake-luna-send"
        publicPort = unused_port_pair()
        nodePort = unused_port_pair((publicPort, publicPort + 1))
        secret = "native-remote-keys-secret-0123456789abcdef"
        token = "native-remote-keys-token"
        with tempfile.TemporaryDirectory(prefix="nativetv-native-remote-keys-") as folder:
            root = Path(folder)
            config = root / "config.json"
            heartbeat = root / "tvweb.beat"
            assets = root / "assets"
            dataRoot = root / "data"
            systemRoot = root / "system"
            procRoot = root / "proc"
            details = root / "luna-details.jsonl"
            remoteLog = root / "messages"
            assets.mkdir()
            dataRoot.mkdir()
            (dataRoot / "disabled_services.json").write_text(
                json.dumps(["camera"]))
            procRoot.mkdir()
            (systemRoot / "bin").mkdir(parents=True)
            systemctlLog = root / "systemctl.log"
            systemctl = systemRoot / "bin/systemctl"
            systemctl.write_text(
                "#!/bin/sh\n"
                "echo \"$*\" >> \"" + str(systemctlLog) + "\"\n"
                "exit 0\n")
            systemctl.chmod(0o755)
            voiceUnit = systemRoot / "lib/systemd/system/voiceconductor.service"
            voiceUnit.parent.mkdir(parents=True)
            voiceProgram = systemRoot / "usr/sbin/voiceconductor"
            voiceProgram.parent.mkdir(parents=True)
            voiceProgram.write_text("fixture voice service\n")
            voiceUnit.write_text("[Service]\nExecStart=/usr/sbin/voiceconductor\n")
            voiceProgram.chmod(0o755)
            mountLog = root / "mount.log"
            for utility in ("mount", "umount"):
                command = systemRoot / "bin" / utility
                command.write_text(
                    "#!/bin/sh\n"
                    "echo \"" + utility + " $*\" >> \"" + str(mountLog) + "\"\n"
                    "exit 0\n")
                command.chmod(0o755)
            remoteLog.write_text("boot\n")
            (dataRoot / "remotekeys.json").write_text(json.dumps({
                "keys": {"existing": "org.example.preserved"},
                "home": "com.webos.app.home", "starting": "Starting Native Tv",
                "splash": True
            }))
            config.write_text(json.dumps({
                "host": "127.0.0.1", "port": publicPort, "token": token,
                "allowControl": True
            }))
            contextRequests = []

            class NodeContextHandler(BaseHTTPRequestHandler):
                def do_POST(self):
                    length = int(self.headers.get("Content-Length", "0"))
                    body = self.rfile.read(length)
                    contextRequests.append((self.path, body))
                    payload = json.dumps({"ok": True, "remoteKeys": None}).encode("utf-8")
                    self.send_response(200)
                    self.send_header("Content-Length", str(len(payload)))
                    self.end_headers()
                    self.wfile.write(payload)

                def log_message(self, format, *args):
                    pass

            nodeServer = HTTPServer(("127.0.0.1", nodePort), NodeContextHandler)
            nodeThread = threading.Thread(target=nodeServer.serve_forever)
            nodeThread.daemon = True
            nodeThread.start()
            environment = os.environ.copy()
            environment["TVWEB_LUNA_SEND"] = str(fakeLuna)
            environment["TVWEB_DATA_DIR"] = str(dataRoot)
            environment["TVWEB_PROXY_SECRET"] = secret
            environment["NATIVETV_FAKE_LUNA_MODE"] = "remote-keys-native"
            environment["NATIVETV_FAKE_LUNA_DETAILS"] = str(details)
            environment["TVWEB_NATIVE_REMOTE_INPUT"] = "1"
            environment["TVWEB_REMOTE_KEYS_LOG_PATH"] = str(remoteLog)
            environment["TVWEB_REMOTE_INPUT_DIRECTORY"] = str(root / "no-input")
            environment["TVWEB_REMOTE_INPUT_DEVICES"] = str(root / "no-input-devices")
            environment["TVWEB_REMOTE_INPUT_LOG"] = str(root / "no-input-log")
            process = subprocess.Popen(
                [str(binary), "--config", str(config), "--heartbeat", str(heartbeat),
                 "--workers", "4", "--listen", "--node-port", str(nodePort),
                 "--assets-root", str(assets), "--system-root", str(systemRoot),
                 "--proc-root", str(procRoot)],
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                env=environment)

            def request(path, method="GET", body=None):
                headers = {"Authorization": "Bearer " + token}
                if body is not None:
                    headers["Content-Type"] = "application/json"
                    body = json.dumps(body).encode("utf-8")
                req = Request("http://127.0.0.1:%d%s" % (publicPort, path),
                              data=body, method=method, headers=headers)
                try:
                    with urlopen(req, timeout=20) as response:
                        return response.status, response.read()
                except HTTPError as error:
                    return error.code, error.read()

            try:
                deadline = time.monotonic() + 8
                ready = False
                while time.monotonic() < deadline and process.poll() is None:
                    try:
                        with urlopen(Request(
                            "http://127.0.0.1:%d/api/caps" % publicPort,
                            headers={"Authorization": "Bearer " + token}), timeout=2):
                            ready = True
                            break
                    except (OSError, URLError):
                        time.sleep(0.03)
                if not ready:
                    if process.poll() is None:
                        process.send_signal(signal.SIGTERM)
                    output, _ = process.communicate(timeout=6)
                    self.fail("native daemon did not start:\n" + output[-4000:])

                status, body = request("/api/apps")
                self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                apps = json.loads(body)
                self.assertTrue(apps["remoteKeys"]["supported"])
                self.assertFalse(apps["remoteKeys"]["inChain"])
                self.assertEqual([row["key"] for row in apps["remoteKeys"]["keys"]], [
                    "netflix", "amazon", "alexa", "homehub", "ai", "multikey",
                    "multikeyhold", "guide"
                ])
                self.assertTrue(next(row for row in apps["remoteKeys"]["keys"]
                                     if row["key"] == "multikey")["noNothing"])
                self.assertIn("Fixture HDMI 1", [choice["title"]
                                                  for choice in apps["remoteKeys"]["choices"]])
                self.assertEqual(contextRequests, [], "the apps context is the daemon's own")

                before = len(details.read_text().splitlines())
                status, body = request("/api/remotekeys", "POST", {
                    "key": "netflix", "action": "open", "id": "org.example.not-installed"
                })
                self.assertEqual(status, 400, body.decode("utf-8", "replace"))
                calls = [json.loads(line) for line in details.read_text().splitlines()]
                self.assertFalse(any(call["uri"].endswith("/setSystemSettings") or
                                     call["uri"].endswith("/setConfigs")
                                     for call in calls[before:]),
                                 "unoffered targets must not mutate native key settings")

                before = len(details.read_text().splitlines())
                status, body = request("/api/remotekeys", "POST", {
                    "key": "multikey", "action": "nothing", "id": ""
                })
                self.assertEqual(status, 400, body.decode("utf-8", "replace"))
                calls = [json.loads(line) for line in details.read_text().splitlines()]
                self.assertFalse(any(call["uri"].endswith("/setSystemSettings") or
                                     call["uri"].endswith("/setConfigs")
                                     for call in calls[before:]),
                                 "custom button cannot be assigned to nothing")

                status, body = request("/api/remotekeys", "POST", {
                    "key": "netflix", "action": "open", "id": "org.example.youtube"
                })
                self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                self.assertEqual(json.loads(body), {"ok": True})
                savedMap = json.loads((dataRoot / "remotekeys.json").read_text())
                self.assertEqual(savedMap["keys"], {
                    "existing": "org.example.preserved",
                    "netflix": "org.example.youtube"
                })
                self.assertEqual(savedMap["home"], "com.webos.app.home")
                self.assertEqual(savedMap["starting"], "Starting Native Tv")
                self.assertTrue(savedMap["splash"])
                lgRecord = json.loads((dataRoot / "remotekeys-lg.json").read_text())
                self.assertEqual(lgRecord["mapping"]["netflix"]["app_id"], "netflix")
                calls = [json.loads(line) for line in details.read_text().splitlines()]
                settingsWrites = [call["payload"] for call in calls
                                  if call["uri"] ==
                                  "com.webos.settingsservice/setSystemSettings"]
                self.assertEqual(settingsWrites[-1]["category"], "other")
                self.assertEqual(settingsWrites[-1]["settings"]["mapping_info"][0]
                                 ["netflix"]["app_id"], "org.example.youtube")
                self.assertFalse(any("nativetv.remoteKeys" in call.get("payload", {})
                                     .get("configs", {}) for call in calls
                                     if call["uri"] == "com.webos.service.config/setConfigs"),
                                 "webOS 11 must not write the JavaScript filter map")

                status, body = request("/api/remotekeys", "POST", {
                    "key": "netflix", "action": "lg", "id": ""
                })
                self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                restoredMap = json.loads((dataRoot / "remotekeys.json").read_text())
                self.assertEqual(restoredMap["keys"], {
                    "existing": "org.example.preserved"
                })
                calls = [json.loads(line) for line in details.read_text().splitlines()]
                settingsWrites = [call["payload"]["settings"] for call in calls
                                  if call["uri"] ==
                                  "com.webos.settingsservice/setSystemSettings"]
                self.assertEqual(settingsWrites[-1]["mapping_info"][0]
                                 ["netflix"]["app_id"], "netflix")
                self.assertFalse((dataRoot / "remotekeys-lg.json").exists(),
                                 "restoring LG's values clears the saved baseline")

                status, body = request("/api/remotekeys", "POST", {
                    "key": "guide", "action": "open", "id": "org.example.youtube"
                })
                self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                calls = [json.loads(line) for line in details.read_text().splitlines()]
                actionWrites = [call["payload"]["configs"] for call in calls
                                if call["uri"] == "com.webos.service.config/setConfigs"]
                self.assertEqual(actionWrites[-1]["com.webos.keyaction.tvGuide"], {
                    "action": "launch", "id": "org.example.youtube"
                })

                status, body = request("/api/remotekeys", "POST", {
                    "key": "guide", "action": "lg", "id": ""
                })
                self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                calls = [json.loads(line) for line in details.read_text().splitlines()]
                actionWrites = [call["payload"]["configs"] for call in calls
                                if call["uri"] == "com.webos.service.config/setConfigs"]
                self.assertEqual(actionWrites[-1]["com.webos.keyaction.tvGuide"], {
                    "action": "launch", "id": "guide.app"
                })
                self.assertFalse((dataRoot / "remotekeys-lg.json").exists(),
                                 "restoring the native action clears its LG baseline")

                status, body = request("/api/remotekeys", "POST", {
                    "key": "ai", "action": "open", "id": "org.example.youtube"
                })
                self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                savedMap = json.loads((dataRoot / "remotekeys.json").read_text())
                self.assertEqual(savedMap["keys"]["ai"], "org.example.youtube")
                self.assertEqual(contextRequests, [],
                                 "native AI key changes must not be forwarded to Node")
                self.assertEqual(json.loads(
                    (dataRoot / "disabled_services.json").read_text()),
                    ["camera", "voiceconductor"])
                lgRecord = json.loads((dataRoot / "remotekeys-lg.json").read_text())
                self.assertTrue(lgRecord["services"]["voiceconductor"])
                self.assertIn("stop voiceconductor.service", systemctlLog.read_text())
                self.assertEqual((dataRoot / "service-off").stat().st_mode & 0o777,
                                 0o755)
                self.assertIn("mount --bind " + str(dataRoot / "service-off") +
                              " " + str(voiceProgram), mountLog.read_text())
                voiceServiceLaunchCount = len([
                    call for call in map(json.loads, details.read_text().splitlines())
                    if call["uri"] == "com.webos.applicationManager/launch" and
                    call["payload"].get("id") == "org.example.youtube"])
                with remoteLog.open("a") as inputLog:
                    inputLog.write(
                        '2026-10-28T11:59:11.600Z [60] user.info lginput2 [] '
                        'lginput2 NL_BUTTON_CLICK '
                        '{"remote_type":"LGE MR25GA",'
                        '"button_type":"KEY_VOICE"} \n' +
                        '2026-10-28T11:59:11.700Z [60] user.info lginput2 [] '
                        'lginput2 NL_BUTTON_CLICK '
                        '{"remote_type":"LGE MR25GA",'
                        '"button_type":"KEY_VOICE"} \n')
                aiLaunchDeadline = time.monotonic() + 5
                while time.monotonic() < aiLaunchDeadline:
                    calls = [json.loads(line) for line in details.read_text().splitlines()]
                    launches = [call for call in calls if call["uri"] ==
                                "com.webos.applicationManager/launch" and
                                call["payload"].get("id") == "org.example.youtube"]
                    if len(launches) > voiceServiceLaunchCount:
                        break
                    time.sleep(0.05)
                self.assertEqual(len(launches), voiceServiceLaunchCount + 1,
                                 "the native AI log watcher must debounce duplicate presses")

                (procRoot / "mounts").write_text(
                    "/var/lib/tvweb/service-off " + str(voiceProgram) +
                    " none rw 0 0\n")
                status, body = request("/api/remotekeys", "POST", {
                    "key": "ai", "action": "lg", "id": ""
                })
                self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                self.assertEqual(json.loads(
                    (dataRoot / "disabled_services.json").read_text()), ["camera"])
                self.assertFalse((dataRoot / "remotekeys-lg.json").exists(),
                                 "restoring the AI key must release owned service state")
                # The log watcher loads this record every 200 ms, and a load brings a
                # missing file back from its .last-good copy: the copy must go too.
                time.sleep(0.7)
                self.assertFalse((dataRoot / "remotekeys-lg.json").exists(),
                                 "a deleted LG record must not come back from its .last-good copy")
                self.assertFalse((dataRoot / "remotekeys-lg.json.last-good").exists())
                self.assertIn("start voiceconductor.service", systemctlLog.read_text())
                self.assertIn("umount " + str(voiceProgram), mountLog.read_text())

                status, body = request("/api/remotekeys", "POST", {
                    "key": "alexa", "action": "open", "id": "org.example.youtube"
                })
                self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                savedMap = json.loads((dataRoot / "remotekeys.json").read_text())
                self.assertEqual(savedMap["keys"]["alexa"], "org.example.youtube")
                lgRecord = json.loads((dataRoot / "remotekeys-lg.json").read_text())
                self.assertTrue(lgRecord["blocks"]["amazon.alexa.view"])
                calls = [json.loads(line) for line in details.read_text().splitlines()]
                blockCalls = [call for call in calls if call["uri"] ==
                              "com.webos.applicationManager/blockSystemApp"]
                self.assertEqual(blockCalls[-1]["payload"], {
                    "id": "amazon.alexa.view", "forceBlockSystemApp": False
                })

                status, body = request("/api/apps")
                self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                apps = json.loads(body)
                alexaRow = next(row for row in apps["remoteKeys"]["keys"]
                                if row["key"] == "alexa")
                self.assertEqual(alexaRow["action"], "open",
                                 "a Native Tv-owned block must keep Alexa restorable")

                time.sleep(0.35)
                # The AI key's press earlier launched the same app: count from here.
                launchesBeforeAlexa = len([
                    call for call in map(json.loads, details.read_text().splitlines())
                    if call["uri"] == "com.webos.applicationManager/launch" and
                    call["payload"].get("id") == "org.example.youtube"])
                with remoteLog.open("a") as inputLog:
                    inputLog.write(
                        '2026-10-28T11:59:12.600Z [60] user.info lginput2 [] '
                        'lginput2 NL_BUTTON_CLICK '
                        '{"remote_type":"LGE MR25GA",'
                        '"button_type":"KEY_ALEXAVOICE"} \n' +
                        '2026-10-28T11:59:12.700Z [60] user.info lginput2 [] '
                        'lginput2 NL_BUTTON_CLICK '
                        '{"remote_type":"LGE MR25GA",'
                        '"button_type":"KEY_ALEXAVOICE"} \n')
                launchDeadline = time.monotonic() + 5
                while time.monotonic() < launchDeadline:
                    calls = [json.loads(line) for line in details.read_text().splitlines()]
                    launches = [call for call in calls if call["uri"] ==
                                "com.webos.applicationManager/launch" and
                                call["payload"].get("id") == "org.example.youtube"]
                    toastsDisabled = any(call["uri"] ==
                                         "com.webos.notification/disable"
                                         for call in calls)
                    if len(launches) > launchesBeforeAlexa and toastsDisabled:
                        break
                    time.sleep(0.05)
                time.sleep(0.6)  # a second launch, if the debounce failed, would be recorded by now
                calls = [json.loads(line) for line in details.read_text().splitlines()]
                launches = [call for call in calls if call["uri"] ==
                            "com.webos.applicationManager/launch" and
                            call["payload"].get("id") == "org.example.youtube"]
                self.assertEqual(len(launches) - launchesBeforeAlexa, 1,
                                 "the native log watcher must launch once per debounced press: " +
                                 json.dumps([c for c in calls if c["uri"] ==
                                             "com.webos.applicationManager/launch"]))
                self.assertTrue((dataRoot / "remotekeys-toasts-off").exists(),
                                "Alexa presses must leave a toast recovery marker")
                calls = [json.loads(line) for line in details.read_text().splitlines()]
                self.assertTrue(any(call["uri"] == "com.webos.notification/disable"
                                    for call in calls),
                                "Alexa's loading toast must be muted; Luna calls: %r" %
                                [call["uri"] for call in calls])
                enableDeadline = time.monotonic() + 6
                while time.monotonic() < enableDeadline:
                    calls = [json.loads(line) for line in details.read_text().splitlines()]
                    toastEnabled = any(call["uri"] ==
                                       "com.webos.notification/enable"
                                       for call in calls)
                    if toastEnabled and not (dataRoot /
                                             "remotekeys-toasts-off").exists():
                        break
                    time.sleep(0.05)
                self.assertTrue(any(call["uri"] == "com.webos.notification/enable"
                                    for call in calls),
                                "Alexa toast muting must expire")
                self.assertFalse((dataRoot / "remotekeys-toasts-off").exists(),
                                 "successful toast restoration clears the recovery marker")

                status, body = request("/api/remotekeys", "POST", {
                    "key": "alexa", "action": "lg", "id": ""
                })
                self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                savedMap = json.loads((dataRoot / "remotekeys.json").read_text())
                self.assertNotIn("alexa", savedMap["keys"])
                self.assertFalse((dataRoot / "remotekeys-lg.json").exists(),
                                 "restoring Alexa must release and clear the owned block")
                calls = [json.loads(line) for line in details.read_text().splitlines()]
                releaseCalls = [call for call in calls if call["uri"] ==
                                "com.webos.applicationManager/releaseBlockSystemApp"]
                self.assertEqual(releaseCalls[-1]["payload"], {
                    "id": "amazon.alexa.view"
                })
                self.assertEqual(contextRequests, [])

                for key, target in (("netflix", "org.example.youtube"),
                                    ("guide", "org.example.youtube"),
                                    ("multikey", "org.example.youtube"),
                                    ("ai", "org.example.youtube"),
                                    ("alexa", "org.example.youtube")):
                    status, body = request("/api/remotekeys", "POST", {
                        "key": key, "action": "open", "id": target
                    })
                    self.assertEqual(status, 200,
                                     body.decode("utf-8", "replace"))

                # Node's Home key, starting message and uninstall ask the daemon's own
                # reconcile (TVWEB_NATIVE_REMOTEKEYS_LIFECYCLE) rather than
                # writing LG's values a second time.
                def internalApply(payload, proxySecret):
                    headers = {"Content-Type": "application/json"}
                    if proxySecret:
                        headers["X-NativeTv-Proxy-Secret"] = proxySecret
                    req = Request("http://127.0.0.1:%d/_internal/remote-keys/apply" % publicPort,
                                  data=json.dumps(payload).encode("utf-8"),
                                  method="POST", headers=headers)
                    with urlopen(req, timeout=20) as response:
                        return json.loads(response.read().decode("utf-8"))

                with self.assertRaises(HTTPError) as refused:
                    internalApply({}, "")
                self.assertEqual(refused.exception.code, 403)
                applied = internalApply({}, secret)
                self.assertTrue(applied["ok"], applied)
                self.assertTrue((dataRoot / "remotekeys.json").exists(),
                                "an apply keeps the saved key map")
                # The restore branch is the one --restore-remote-keys runs, below.

                process.send_signal(signal.SIGTERM)
                output, _ = process.communicate(timeout=6)
                self.assertEqual(process.returncode, 0, output)
            finally:
                if process.poll() is None:
                    process.send_signal(signal.SIGTERM)
                    try:
                        process.communicate(timeout=6)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait(timeout=3)
                nodeServer.shutdown()
                nodeServer.server_close()

            offlineConfig = json.loads(config.read_text())
            offlineConfig["allowControl"] = False
            config.write_text(json.dumps(offlineConfig))
            restoreStart = len(details.read_text().splitlines())
            failedEnvironment = environment.copy()
            failedEnvironment["NATIVETV_FAKE_REMOTE_KEYS_RESTORE_FAIL"] = "1"
            failedRestore = subprocess.run(
                [str(binary), "--restore-remote-keys", "--config", str(config),
                 "--assets-root", str(assets), "--proc-root", str(procRoot),
                 "--system-root", str(systemRoot)],
                capture_output=True, text=True, env=failedEnvironment, timeout=30)
            self.assertNotEqual(failedRestore.returncode, 0,
                                "a rejected LG mapping restore must report failure")
            self.assertEqual(json.loads(
                (dataRoot / "remotekeys.json").read_text())["keys"], {},
                "failed restoration must keep Native Tv mappings disabled")
            failedLgRecord = json.loads(
                (dataRoot / "remotekeys-lg.json").read_text())
            self.assertEqual(failedLgRecord["mapping"]["netflix"]["app_id"],
                             "netflix",
                             "failed restoration must preserve LG's saved mapping")
            self.assertFalse((dataRoot / "remotekeys.json.last-good").exists(),
                             "a failed restore must not retain an active map backup")
            restoreStart = len(details.read_text().splitlines())
            restored = subprocess.run(
                [str(binary), "--restore-remote-keys", "--config", str(config),
                 "--assets-root", str(assets), "--proc-root", str(procRoot),
                 "--system-root", str(systemRoot)],
                capture_output=True, text=True, env=environment, timeout=30)
            self.assertEqual(restored.returncode, 0,
                             restored.stdout + restored.stderr)
            self.assertFalse((dataRoot / "remotekeys.json").exists())
            self.assertFalse((dataRoot / "remotekeys.json.last-good").exists(),
                             "native restore must not leave a recoverable active map")
            self.assertFalse((dataRoot / "remotekeys-lg.json").exists(),
                             "native restore must clear the LG baseline after restoring")
            self.assertEqual(json.loads(
                (dataRoot / "disabled_services.json").read_text()), ["camera"])
            restoreCalls = [json.loads(line) for line in
                            details.read_text().splitlines()[restoreStart:]]
            mappingWrites = [call["payload"]["settings"]["mapping_info"]
                             for call in restoreCalls
                             if call["uri"] ==
                             "com.webos.settingsservice/setSystemSettings" and
                             "mapping_info" in call["payload"].get("settings", {})]
            self.assertTrue(mappingWrites)
            self.assertEqual(mappingWrites[-1][0]["netflix"]["app_id"], "netflix")
            actionWrites = [call["payload"]["configs"] for call in restoreCalls
                            if call["uri"] ==
                            "com.webos.service.config/setConfigs" and
                            "com.webos.keyaction.tvGuide" in
                            call["payload"].get("configs", {})]
            self.assertTrue(actionWrites)
            self.assertEqual(actionWrites[-1]["com.webos.keyaction.tvGuide"], {
                "action": "launch", "id": "guide.app"
            })
            customWrites = [call["payload"]["settings"] for call in restoreCalls
                            if call["uri"] ==
                            "com.webos.settingsservice/setSystemSettings" and
                            "customShortPress" in call["payload"].get("settings", {})]
            self.assertTrue(customWrites)
            self.assertEqual(customWrites[-1]["customShortPress"], {
                "id": "accessibility", "type": "app", "params": {}
            })
            self.assertTrue(any(
                call["uri"] == "com.webos.applicationManager/releaseBlockSystemApp" and
                call["payload"].get("id") == "amazon.alexa.view"
                for call in restoreCalls))
            self.assertIn("start voiceconductor.service", systemctlLog.read_text())
            self.assertFalse(any(
                "nativetv.remoteKeys" in call.get("payload", {}).get("configs", {})
                for call in restoreCalls),
                "native-filter restore must not install or clear the JavaScript map")

    def test_native_mqtt_shadow_publishes_retained_state_on_isolated_topics(self):
        binary = Path(sys.argv[1]).resolve()
        fakeLuna = binary.parent / "native-fake-luna-send"
        publicPort = unused_port_pair()
        nodePort = unused_port_pair((publicPort, publicPort + 1))
        secret = "mqtt-shadow-proxy-secret-0123456789abcdef"
        token = "mqtt-shadow-route-token"
        broker = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        broker.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        broker.bind(("127.0.0.1", 0))
        broker.listen(1)
        brokerPort = broker.getsockname()[1]
        broker.settimeout(1)
        brokerStop = threading.Event()
        brokerCondition = threading.Condition()
        brokerClient = {"socket": None}
        brokerMessages = []
        brokerErrors = []
        commandSent = {"volume": False}

        def readExact(connection, size):
            chunks = []
            remaining = size
            while remaining:
                chunk = connection.recv(remaining)
                if not chunk:
                    return None
                chunks.append(chunk)
                remaining -= len(chunk)
            return b"".join(chunks)

        def readPacket(connection):
            first = readExact(connection, 1)
            if not first:
                return None
            remaining = 0
            multiplier = 1
            for _ in range(4):
                encoded = readExact(connection, 1)
                if not encoded:
                    raise RuntimeError("truncated MQTT remaining length")
                remaining += (encoded[0] & 0x7f) * multiplier
                if not encoded[0] & 0x80:
                    break
                multiplier *= 128
            else:
                raise RuntimeError("invalid MQTT remaining length")
            body = readExact(connection, remaining)
            if body is None:
                raise RuntimeError("truncated MQTT packet")
            return first[0], body

        def brokerWorker():
            connection = None
            try:
                while not brokerStop.is_set():
                    try:
                        connection, _ = broker.accept()
                        break
                    except socket.timeout:
                        continue
                if connection is None:
                    return
                connection.settimeout(0.5)
                brokerClient["socket"] = connection
                packet = readPacket(connection)
                if not packet or packet[0] >> 4 != 1:
                    raise RuntimeError("native client did not send MQTT CONNECT")
                if b"migration_fixture_tvweb_native" not in packet[1]:
                    raise RuntimeError("native MQTT shadow client id was not isolated")
                connection.sendall(b"\x20\x02\x00\x00")
                while not brokerStop.is_set():
                    try:
                        packet = readPacket(connection)
                    except socket.timeout:
                        continue
                    if packet is None:
                        break
                    packetHeader, body = packet
                    packetType = packetHeader >> 4
                    if packetType == 3:
                        if len(body) < 2:
                            raise RuntimeError("truncated MQTT publish topic")
                        topicSize = (body[0] << 8) | body[1]
                        topicEnd = 2 + topicSize
                        if topicEnd > len(body):
                            raise RuntimeError("invalid MQTT publish topic length")
                        message = (body[2:topicEnd].decode("utf-8"),
                                   body[topicEnd:].decode("utf-8"),
                                   bool(packetHeader & 1))
                        with brokerCondition:
                            brokerMessages.append(message)
                            brokerCondition.notify_all()
                    elif packetType == 8:
                        if len(body) < 5:
                            raise RuntimeError("truncated MQTT subscribe packet")
                        packetId = body[:2]
                        topicSize = (body[2] << 8) | body[3]
                        topicEnd = 4 + topicSize
                        if topicEnd + 1 > len(body):
                            raise RuntimeError("invalid MQTT subscription topic length")
                        topic = body[4:topicEnd].decode("utf-8")
                        connection.sendall(b"\x90\x03" + packetId + b"\x00")
                        if topic == "migration/telemetry":
                            payload = b'{"app_id":"com.webos.app.home"}'
                            publishBody = (bytes([(len(topic) >> 8) & 0xff,
                                                  len(topic) & 0xff]) +
                                           topic.encode("utf-8") + payload)
                            if len(publishBody) >= 128:
                                raise RuntimeError("fixture MQTT publish is too large")
                            connection.sendall(b"\x31" + bytes([len(publishBody)]) +
                                               publishBody)
                        elif topic == "migration_native/command/#" and not commandSent["volume"]:
                            commandTopic = "migration_native/command/volume"
                            payload = b"37"
                            publishBody = (bytes([(len(commandTopic) >> 8) & 0xff,
                                                  len(commandTopic) & 0xff]) +
                                           commandTopic.encode("utf-8") + payload)
                            if len(publishBody) >= 128:
                                raise RuntimeError("fixture MQTT command is too large")
                            connection.sendall(b"\x30" + bytes([len(publishBody)]) +
                                               publishBody)
                            commandSent["volume"] = True
                    elif packetType == 12:
                        connection.sendall(b"\xd0\x00")
                    elif packetType == 14:
                        break
                    else:
                        raise RuntimeError("unexpected MQTT packet %d" % packetType)
            except OSError as error:
                if not brokerStop.is_set():
                    brokerErrors.append(str(error))
            except Exception as error:
                brokerErrors.append(str(error))
            finally:
                if connection is not None:
                    connection.close()

        brokerThread = threading.Thread(target=brokerWorker, daemon=True)
        brokerThread.start()
        with tempfile.TemporaryDirectory(prefix="nativetv-native-mqtt-shadow-") as folder:
            root = Path(folder)
            config = root / "config.json"
            heartbeat = root / "tvweb.beat"
            assets = root / "assets"
            systemRoot = root / "system"
            assets.mkdir()
            systemFiles = {
                "proc/lg/pm/temperature": "48\n",
                "proc/lg/pe/hdr_status": "VPQ_PQ_MODE_INFO=\n",
                "proc/lg/hdmi20/port0/status": "",
                "proc/lg/base/chip_name": "O22\n",
                "proc/lg/sys/status": "gpu pll out: 550000000\n",
                "mnt/lg/cmn_data/pnwash/completedOffRsCount": "1\n",
                "mnt/lg/cmn_data/pnwash/autoOffRsTime": "1\n",
                "mnt/lg/cmn_data/mrcu/mrcu1.info":
                    "Name = LGE MR24\nBattery = 80\n",
                "sys/block/mmcblk0/device/life_time": "0x01 0x01\n"
            }
            for relative, contents in systemFiles.items():
                path = systemRoot / relative
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(contents)
            config.write_text(json.dumps({
                "host": "127.0.0.1", "port": publicPort, "token": token,
                "allowPower": True,
                "logging": {"level": "debug"},
                "mqtt": {
                    "enabled": True, "host": "127.0.0.1", "port": brokerPort,
                    "tls": False, "topicPrefix": "migration",
                    "telemetryIntervalMs": 1000
                },
                "device": {
                    "id": "migration_fixture", "name": "Migration Fixture",
                    "model": "webOS Fixture", "manufacturer": "LG Fixture",
                    "sw_version": "Fixture OS"
                }
            }))
            environment = os.environ.copy()
            environment["TVWEB_LUNA_SEND"] = str(fakeLuna)
            environment["TVWEB_DATA_DIR"] = str(root)
            environment["TVWEB_PROXY_SECRET"] = secret
            environment["TVWEB_NATIVE_STATE"] = "1"
            environment["TVWEB_NATIVE_MQTT_SHADOW"] = "1"
            environment["TVWEB_NATIVE_MQTT_DISCOVERY"] = "1"
            environment["NATIVETV_FAKE_LUNA_MODE"] = "mqtt-discovery"
            environment["NATIVETV_FAKE_LUNA_LOG"] = str(root / "luna.log")
            environment["NATIVETV_FAKE_LUNA_DETAILS"] = str(root / "luna-details.jsonl")
            process = subprocess.Popen(
                [str(binary), "--config", str(config), "--heartbeat", str(heartbeat),
                 "--workers", "8", "--listen", "--node-port", str(nodePort),
                 "--assets-root", str(assets), "--system-root", str(systemRoot)],
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                env=environment)
            try:
                with brokerCondition:
                    ready = brokerCondition.wait_for(
                        lambda: any(message[0] == "migration_native/status" and
                                    message[1] == "online" and message[2]
                                    for message in brokerMessages) and
                        any(message[0] == "migration_native/state/application/remote" and
                            message[1].startswith('{"app_id":') and message[2]
                            for message in brokerMessages) and
                        any(message[0] ==
                            "migration_native/state/application/app_id" and message[2]
                            for message in brokerMessages) and
                        any(message[0] == "migration_native/telemetry" and
                            '"app_id":"com.webos.app.netflix"' in message[1] and
                            '"time":' in message[1] and
                            message[2] for message in brokerMessages) and
                        any(message[0] == "migration_legacy/telemetry" and
                            message[1] == '{"app_id":"com.webos.app.home"}' and
                            message[2] for message in brokerMessages) and
                        any(message[0] ==
                            "homeassistant_native/select/"
                            "migration_fixture_native/sound_mode/config" and message[1]
                            for message in brokerMessages) and
                        any(message[0] ==
                            "homeassistant_native/sensor/"
                            "migration_fixture_native/soc_temperature/config" and
                            message[1] for message in brokerMessages),
                        timeout=12)
                if not ready:
                    if process.poll() is None:
                        process.send_signal(signal.SIGTERM)
                    output, _ = process.communicate(timeout=8)
                    self.fail("native MQTT shadow did not publish retained state; "
                              "messages=%r errors=%r logs=%s" %
                              (brokerMessages, brokerErrors,
                               output[-6000:] + "\nLuna calls:\n" +
                               ((root / "luna.log").read_text()
                                if (root / "luna.log").exists() else "<none>")))
                def nativeTelemetryTimes():
                    times = set()
                    for message in brokerMessages:
                        if message[0] != "migration_native/telemetry" or not message[2]:
                            continue
                        try:
                            payload = json.loads(message[1])
                        except ValueError:
                            continue
                        if payload.get("app_id") == "com.webos.app.netflix" and \
                                isinstance(payload.get("time"), (int, float)):
                            times.add(payload["time"])
                    return times

                with brokerCondition:
                    periodic = brokerCondition.wait_for(
                        lambda: len(nativeTelemetryTimes()) >= 2, timeout=4)
                if not periodic:
                    if process.poll() is None:
                        process.send_signal(signal.SIGTERM)
                    output, _ = process.communicate(timeout=8)
                    nativeMessages = [message for message in brokerMessages
                                      if message[0] == "migration_native/telemetry"]
                    self.fail("native system telemetry did not refresh periodically; "
                              "messages=%r errors=%r logs=%s" %
                              (nativeMessages, brokerErrors, output[-6000:]))
                details = root / "luna-details.jsonl"
                routedVolume = False
                commandDeadline = time.monotonic() + 5
                while time.monotonic() < commandDeadline:
                    if details.exists():
                        for line in details.read_text().splitlines():
                            try:
                                call = json.loads(line)
                            except ValueError:
                                continue
                            if call.get("uri") == "com.webos.audio/setVolume" and \
                                    call.get("payload", {}).get("volume") == 37:
                                routedVolume = True
                                break
                    if routedVolume:
                        break
                    time.sleep(0.05)
                self.assertTrue(routedVolume,
                                "native MQTT volume command did not reach Luna")
                with brokerCondition:
                    self.assertTrue(all(
                        message[0].startswith("migration_native/") or
                        message[0].startswith("migration_legacy/") or
                        message[0].startswith("homeassistant_native/")
                        for message in brokerMessages),
                        "shadow client wrote into a live Node topic prefix")
                    self.assertTrue(any(message[0] ==
                                        "migration_native/state/application/app_id"
                                        for message in brokerMessages))
                    self.assertTrue(any(message[0] == "migration_legacy/telemetry" and
                                        message[1] == '{"app_id":"com.webos.app.home"}' and
                                        message[2] for message in brokerMessages),
                                    "retained legacy telemetry was not mirrored to shadow")
                    nativeTelemetryMessage = next(message for message in brokerMessages
                                                  if message[0] == "migration_native/telemetry" and
                                                  '"app_id":"com.webos.app.netflix"' in
                                                  message[1] and '"time":' in message[1] and
                                                  message[2])
                    nativeTelemetry = json.loads(nativeTelemetryMessage[1])
                    self.assertEqual(nativeTelemetry["tvwebVersion"], "0.118.0")
                    self.assertEqual(nativeTelemetry["device"]["name"],
                                     "Migration Fixture")
                    self.assertEqual(nativeTelemetry["app_id"],
                                     "com.webos.app.netflix")
                    self.assertIsInstance(nativeTelemetry["time"], (int, float))
                    configMessage = next(message for message in brokerMessages
                                         if message[0] ==
                                         "homeassistant_native/sensor/"
                                         "migration_fixture_native/soc_temperature/config" and
                                         message[1])
                    configPayload = json.loads(configMessage[1])
                    self.assertEqual(configPayload["state_topic"],
                                     "migration_native/telemetry")
                    device = configPayload["device"]
                    self.assertEqual(device["identifiers"], ["migration_fixture_native"])
                    self.assertEqual(device["name"], "Migration Fixture")
                    self.assertEqual(device["model"], "webOS Fixture")
                    self.assertEqual(device["manufacturer"], "LG Fixture")
                    self.assertEqual(device["sw_version"], "Fixture OS")
                    appMessage = next(message for message in brokerMessages
                                      if message[0] ==
                                      "homeassistant_native/select/"
                                      "migration_fixture_native/app/config" and message[1])
                    appConfig = json.loads(appMessage[1])
                    self.assertEqual(appConfig["options"],
                                     ["Netflix", "YouTube", "Settings"])
                    self.assertIn('"app_colors":{"netflix":"#aabbcc"}',
                                  appConfig["json_attributes_template"])
                    modeMessage = next(message for message in brokerMessages
                                       if message[0] ==
                                       "homeassistant_native/select/"
                                       "migration_fixture_native/picture_mode/config" and
                                       message[1])
                    self.assertEqual(json.loads(modeMessage[1])["options"], ["HDR Game"])
                    soundMessage = next(message for message in brokerMessages
                                         if message[0] ==
                                         "homeassistant_native/select/"
                                         "migration_fixture_native/sound_output/config" and
                                         message[1])
                    self.assertEqual(json.loads(soundMessage[1])["options"], [
                        "TV Speaker", "Optical", "HDMI ARC", "Bluetooth",
                        "TV Speaker + Bluetooth", "Line Out", "Headphone / AUX",
                        "TV Speaker + Headphone", "Mobile Phone"])
                    soundModeMessage = next(message for message in brokerMessages
                        if message[0] == "homeassistant_native/select/"
                        "migration_fixture_native/sound_mode/config" and message[1])
                    self.assertEqual(json.loads(soundModeMessage[1])["options"], [
                        "Standard", "Cinema"])
                    timerTopic = ("homeassistant_native/switch/"
                                  "migration_fixture_native/on_timer/config")
                    timerMessages = [message for message in brokerMessages
                                     if message[0] == timerTopic and message[2]]
                    self.assertTrue(timerMessages,
                                    "native discovery did not clear unsupported On Timer")
                    self.assertEqual(timerMessages[-1][1], "",
                                     "unsupported On Timer should not be announced")

                thermalTopic = ("homeassistant_native/sensor/"
                                "migration_fixture_native/soc_temperature/config")
                beforeThermalChange = len(brokerMessages)
                (systemRoot / "proc/lg/pm/temperature").unlink()
                with brokerCondition:
                    thermalCleared = brokerCondition.wait_for(
                        lambda: any(message[0] == thermalTopic and not message[1] and
                                    message[2]
                                    for message in brokerMessages[beforeThermalChange:]),
                        timeout=5)
                self.assertTrue(
                    thermalCleared,
                    "native discovery did not clear an entity after its capability disappeared")
                self.assertFalse(brokerErrors, "fake broker failed: %s" % brokerErrors)
            finally:
                if process.poll() is None:
                    process.send_signal(signal.SIGTERM)
                    try:
                        process.communicate(timeout=8)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.communicate(timeout=3)
                brokerStop.set()
                broker.close()
                if brokerClient["socket"] is not None:
                    try:
                        brokerClient["socket"].shutdown(socket.SHUT_RDWR)
                    except OSError:
                        pass
                brokerThread.join(timeout=3)

    def test_native_mqtt_production_uses_live_identity_topics_and_status(self):
        binary = Path(sys.argv[1]).resolve()
        fakeLuna = binary.parent / "native-fake-luna-send"
        publicPort = unused_port_pair()
        nodePort = unused_port_pair((publicPort, publicPort + 1))
        secret = "mqtt-production-proxy-secret-0123456789abcdef"
        token = "mqtt-production-route-token"
        broker = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        broker.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        broker.bind(("127.0.0.1", 0))
        broker.listen(1)
        brokerPort = broker.getsockname()[1]
        broker.settimeout(1)
        brokerStop = threading.Event()
        brokerCondition = threading.Condition()
        brokerClient = {"socket": None}
        brokerMessages = []
        subscriptions = []
        brokerErrors = []
        commandSent = {"volume": False}
        nodeEvents = []
        process = None

        def readExact(connection, size):
            chunks = []
            remaining = size
            while remaining:
                chunk = connection.recv(remaining)
                if not chunk:
                    return None
                chunks.append(chunk)
                remaining -= len(chunk)
            return b"".join(chunks)

        def readPacket(connection):
            first = readExact(connection, 1)
            if not first:
                return None
            remaining = 0
            multiplier = 1
            for _ in range(4):
                encoded = readExact(connection, 1)
                if not encoded:
                    raise RuntimeError("truncated MQTT remaining length")
                remaining += (encoded[0] & 0x7f) * multiplier
                if not encoded[0] & 0x80:
                    break
                multiplier *= 128
            else:
                raise RuntimeError("invalid MQTT remaining length")
            body = readExact(connection, remaining)
            if body is None:
                raise RuntimeError("truncated MQTT packet")
            return first[0], body

        def brokerWorker():
            connection = None
            try:
                while not brokerStop.is_set():
                    try:
                        connection, _ = broker.accept()
                        break
                    except socket.timeout:
                        continue
                if connection is None:
                    return
                connection.settimeout(0.5)
                brokerClient["socket"] = connection
                packet = readPacket(connection)
                if not packet or packet[0] >> 4 != 1:
                    raise RuntimeError("native client did not send MQTT CONNECT")
                body = packet[1]
                if len(body) < 12:
                    raise RuntimeError("truncated MQTT CONNECT body")
                clientIdLength = (body[10] << 8) | body[11]
                clientId = body[12:12 + clientIdLength].decode("utf-8")
                if clientId != "migration_fixture_tvweb":
                    raise RuntimeError("production client ID was not preserved: %r" % clientId)
                connection.sendall(b"\x20\x02\x00\x00")
                while not brokerStop.is_set():
                    try:
                        packet = readPacket(connection)
                    except socket.timeout:
                        continue
                    if packet is None:
                        break
                    packetHeader, body = packet
                    packetType = packetHeader >> 4
                    if packetType == 3:
                        if len(body) < 2:
                            raise RuntimeError("truncated MQTT publish topic")
                        topicSize = (body[0] << 8) | body[1]
                        topicEnd = 2 + topicSize
                        if topicEnd > len(body):
                            raise RuntimeError("invalid MQTT publish topic length")
                        message = (body[2:topicEnd].decode("utf-8"),
                                   body[topicEnd:].decode("utf-8"),
                                   bool(packetHeader & 1))
                        with brokerCondition:
                            brokerMessages.append(message)
                            brokerCondition.notify_all()
                    elif packetType == 8:
                        if len(body) < 5:
                            raise RuntimeError("truncated MQTT SUBSCRIBE packet")
                        packetId = body[:2]
                        offset = 2
                        granted = 0
                        while offset + 2 <= len(body):
                            topicSize = (body[offset] << 8) | body[offset + 1]
                            offset += 2
                            if offset + topicSize + 1 > len(body):
                                raise RuntimeError("invalid MQTT subscription topic")
                            topic = body[offset:offset + topicSize].decode("utf-8")
                            subscriptions.append(topic)
                            offset += topicSize + 1
                            granted += 1
                        connection.sendall(b"\x90" + bytes([2 + granted]) +
                                           packetId + bytes([0] * granted))
                        if "migration/command/#" in subscriptions and not commandSent["volume"]:
                            topic = "migration/command/volume"
                            payload = b"37"
                            publishBody = (bytes([(len(topic) >> 8) & 0xff,
                                                  len(topic) & 0xff]) +
                                           topic.encode("utf-8") + payload)
                            connection.sendall(b"\x30" + bytes([len(publishBody)]) +
                                               publishBody)
                            commandSent["volume"] = True
                    elif packetType == 12:
                        connection.sendall(b"\xd0\x00")
                    elif packetType == 14:
                        break
                    else:
                        raise RuntimeError("unexpected MQTT packet %d" % packetType)
            except OSError as error:
                if not brokerStop.is_set():
                    brokerErrors.append(str(error))
            except Exception as error:
                brokerErrors.append(str(error))
            finally:
                if connection is not None:
                    connection.close()

        class NodeCaptureHandler(BaseHTTPRequestHandler):
            def do_POST(self):
                length = int(self.headers.get("Content-Length", "0"))
                try:
                    event = json.loads(self.rfile.read(length).decode("utf-8"))
                except (ValueError, UnicodeDecodeError):
                    self.send_error(400)
                    return
                nodeEvents.append(event)
                self.send_response(204)
                self.send_header("Content-Length", "0")
                self.end_headers()

            def log_message(self, *_args):
                pass

        brokerThread = threading.Thread(target=brokerWorker, daemon=True)
        brokerThread.start()
        node = HTTPServer(("127.0.0.1", nodePort), NodeCaptureHandler)
        nodeThread = threading.Thread(target=node.serve_forever, daemon=True)
        nodeThread.start()

        with tempfile.TemporaryDirectory(prefix="nativetv-native-mqtt-production-") as folder:
            root = Path(folder)
            config = root / "config.json"
            heartbeat = root / "tvweb.beat"
            assets = root / "assets"
            systemRoot = root / "system"
            assets.mkdir()
            for relative, contents in {
                "proc/lg/pm/temperature": "48\n",
                "proc/lg/pe/hdr_status": "VPQ_PQ_MODE_INFO=\n",
                "proc/lg/hdmi20/port0/status": "",
                "proc/lg/base/chip_name": "O22\n",
                "proc/lg/sys/status": "gpu pll out: 550000000\n",
                "mnt/lg/cmn_data/pnwash/completedOffRsCount": "1\n",
                "mnt/lg/cmn_data/pnwash/autoOffRsTime": "1\n",
                "mnt/lg/cmn_data/mrcu/mrcu1.info": "Name = LGE MR24\nBattery = 80\n",
                "sys/block/mmcblk0/device/life_time": "0x01 0x01\n",
                "etc/issue": "webOS TV 4.4.3\n",
                "media/developer/apps/usr/palm/applications/org.webosbrew.piccap/appinfo.json": "{}",
                "proc/net/dev": "Inter-|   Receive |  Transmit\n face |bytes packets errs drop "
                                "fifo frame compressed multicast|bytes packets errs drop fifo colls "
                                "carrier compressed\n  eth0: 1000 10 0 0 0 0 0 0 2000 20 0 0 0 0 0 0\n",
                "sys/class/net/eth0/address": "A8:23:FE:11:22:33\n"
            }.items():
                path = systemRoot / relative
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(contents)
            config.write_text(json.dumps({
                "host": "127.0.0.1", "port": publicPort, "token": token,
                "allowControl": True,
                "mqtt": {"enabled": True, "host": "127.0.0.1", "port": brokerPort,
                         "topicPrefix": "migration", "telemetryIntervalMs": 1000},
                "device": {"id": "migration_fixture", "name": "Migration Fixture"}
            }))
            environment = os.environ.copy()
            environment["TVWEB_LUNA_SEND"] = str(fakeLuna)
            environment["TVWEB_DATA_DIR"] = str(root)
            environment["TVWEB_PROXY_SECRET"] = secret
            environment["TVWEB_NATIVE_STATE"] = "1"
            environment["TVWEB_NATIVE_MQTT_PRODUCTION"] = "1"
            environment.pop("TVWEB_NATIVE_MQTT_SHADOW", None)
            environment.pop("TVWEB_NATIVE_MQTT_DISCOVERY", None)
            environment["NATIVETV_FAKE_LUNA_MODE"] = "mqtt-discovery"
            environment["NATIVETV_FAKE_LUNA_DETAILS"] = str(root / "luna-details.jsonl")
            process = subprocess.Popen(
                [str(binary), "--config", str(config), "--heartbeat", str(heartbeat),
                 "--workers", "8", "--listen", "--node-port", str(nodePort),
                 "--assets-root", str(assets), "--system-root", str(systemRoot)],
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, env=environment)
            try:
                deadline = time.monotonic() + 12
                ready = False
                while time.monotonic() < deadline and process.poll() is None:
                    try:
                        request = Request("http://127.0.0.1:%d/api/caps" % publicPort,
                                          headers={"Authorization": "Bearer " + token})
                        response = urlopen(request, timeout=1)
                        response.read()
                        ready = True
                        break
                    except (OSError, URLError):
                        time.sleep(0.03)
                if not ready:
                    if process.poll() is None:
                        process.send_signal(signal.SIGTERM)
                    output, _ = process.communicate(timeout=8)
                    self.fail("native MQTT production daemon did not start:\n" + output[-4000:])

                with brokerCondition:
                    published = brokerCondition.wait_for(
                        lambda: any(message[0] == "migration/status" and
                                    message[1] == "online" and message[2]
                                    for message in brokerMessages) and
                        any(message[0] == "migration/telemetry" and
                            '"app_id":"com.webos.app.netflix"' in message[1] and
                            '"time":' in message[1] and message[2]
                            for message in brokerMessages) and
                        any(message[0] ==
                            "homeassistant/select/migration_fixture/sound_mode/config" and
                            message[1] for message in brokerMessages), timeout=12)
                if not published:
                    if process.poll() is None:
                        process.send_signal(signal.SIGTERM)
                    output, _ = process.communicate(timeout=8)
                    self.fail("native MQTT production topics were not published; "
                              "messages=%r errors=%r logs=%s" %
                              (brokerMessages, brokerErrors, output[-5000:]))

                details = root / "luna-details.jsonl"
                commandDeadline = time.monotonic() + 5
                routedVolume = False
                while time.monotonic() < commandDeadline:
                    if details.exists():
                        calls = [json.loads(line) for line in details.read_text().splitlines()]
                        routedVolume = any(call.get("uri") == "com.webos.audio/setVolume" and
                                           call.get("payload", {}).get("volume") == 37
                                           for call in calls)
                    if routedVolume:
                        break
                    time.sleep(0.05)
                self.assertTrue(routedVolume, "production MQTT command did not reach Luna")
                statusDeadline = time.monotonic() + 4
                while time.monotonic() < statusDeadline and not any(
                        event.get("type") == "mqtt-status" and
                        event.get("status", {}).get("state") == "connected" and
                        event.get("status", {}).get("lastPublishMs", 0) > 0
                        for event in nodeEvents):
                    time.sleep(0.05)
                self.assertTrue(any(
                    event.get("type") == "mqtt-status" and
                    event.get("status", {}).get("state") == "connected" and
                    event.get("status", {}).get("lastPublishMs", 0) > 0
                    for event in nodeEvents),
                    "native MQTT connection and publish status did not reach Node")
                self.assertIn("migration/command/#", subscriptions)
                self.assertNotIn("migration/telemetry", subscriptions,
                                 "production mode must not subscribe to Node telemetry")
                self.assertTrue(all(
                    message[0].startswith("migration/") or
                    message[0].startswith("homeassistant/")
                    for message in brokerMessages),
                    "production MQTT wrote isolated shadow or legacy topics")
                discovery = json.loads(next(message[1] for message in brokerMessages
                                            if message[0] ==
                                            "homeassistant/select/migration_fixture/sound_mode/config" and
                                            message[1]))
                self.assertEqual(discovery["state_topic"], "migration/telemetry")
                self.assertEqual(discovery["device"]["identifiers"], ["migration_fixture"])
                self.assertEqual(discovery["device"]["connections"], [["mac", "a8:23:fe:11:22:33"]],
                                 "the TV's network address goes into the device record")
                # The device record carries what the stats read detected (the model
                # and firmware the TV reports), not the placeholders.
                latestDiscovery = json.loads(next(
                    message[1] for message in reversed(brokerMessages)
                    if message[0] == "homeassistant/select/migration_fixture/sound_mode/config" and
                    message[1]))
                self.assertEqual(latestDiscovery["device"]["model"], "OLED65B3PUA")
                self.assertEqual(latestDiscovery["device"]["sw_version"], "33.31.75")
                self.assertEqual(latestDiscovery["device"]["name"], "Migration Fixture")
                # The sensors the TV has are offered. (Node behind the daemon withheld
                # them on a B3 only because it had no system snapshot when it published.)
                for entity in ("sensor/migration_fixture/soc_temperature",
                               "sensor/migration_fixture/flash_wear",
                               "sensor/migration_fixture/remote_battery"):
                    self.assertTrue(any(
                        message[0] == "homeassistant/%s/config" % entity and message[1]
                        for message in brokerMessages), entity + " is not offered")
                # What Node does not publish: the daemon's player snapshot is no state
                # topic; and the retained release state of servers before this one is cleared.
                self.assertFalse(any(message[0].startswith("migration/state/media/")
                                     for message in brokerMessages),
                                 "the internal media snapshot reached the broker")
                self.assertIn(("migration/update", "", True), brokerMessages)
                # PicCap answers: its switch exists and reads its own retained topic.
                self.assertTrue(any(
                    message[0] == "homeassistant/switch/migration_fixture/piccap/config" and message[1]
                    for message in brokerMessages), "the PicCap switch needs PicCap to answer")
                self.assertIn(("migration/state/piccap/power", "OFF", True), brokerMessages)
                # The whole stats read, as Node's publishTelemetry made it: what
                # Home Assistant's entities read, and not the parts nothing reads.
                telemetry = json.loads(next(
                    message[1] for message in reversed(brokerMessages)
                    if message[0] == "migration/telemetry" and '"capabilities"' in message[1]))
                for key in ("capabilities", "device", "hardware", "inputs", "lgs", "tvOff", "watch",
                            "volume_control", "signal_format", "picture_engine", "load", "mem"):
                    self.assertIn(key, telemetry, "telemetry lacks " + key)
                self.assertFalse(telemetry["tvOff"])
                self.assertEqual(telemetry["piccap"], {"power": False})
                for key in ("apps", "temps", "cpuTimes"):
                    self.assertNotIn(key, telemetry, "telemetry carries " + key)

                # Uninstall: Node asks the daemon, which owns the connection, to
                # clear every retained topic and disconnect cleanly.
                refused = Request("http://127.0.0.1:%d/_internal/mqtt/forget" % publicPort,
                                  data=b"", method="POST")
                with self.assertRaises(HTTPError) as denied:
                    urlopen(refused, timeout=5)
                self.assertEqual(denied.exception.code, 403)
                forgotten = json.loads(urlopen(Request(
                    "http://127.0.0.1:%d/_internal/mqtt/forget" % publicPort, data=b"",
                    method="POST", headers={"X-NativeTv-Proxy-Secret": secret}),
                    timeout=10).read().decode("utf-8"))
                self.assertTrue(forgotten["ok"])
                self.assertGreater(forgotten["cleared"], 50)
                cleared = {message[0] for message in brokerMessages if message[1] == "" and message[2]}
                for topic in ("migration/telemetry", "migration/status",
                              "homeassistant/select/migration_fixture/sound_mode/config"):
                    self.assertIn(topic, cleared, "retained topic not cleared: " + topic)
                brokerThread.join(timeout=5)
                self.assertFalse(brokerThread.is_alive(), "the daemon did not disconnect cleanly")
                self.assertFalse(brokerErrors, "fake broker failed: %s" % brokerErrors)
            finally:
                if process is not None and process.poll() is None:
                    process.send_signal(signal.SIGTERM)
                    try:
                        process.communicate(timeout=8)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.communicate(timeout=3)
                node.shutdown()
                node.server_close()
                brokerStop.set()
                broker.close()
                if brokerClient["socket"] is not None:
                    try:
                        brokerClient["socket"].shutdown(socket.SHUT_RDWR)
                    except OSError:
                        pass
                brokerThread.join(timeout=3)
                nodeThread.join(timeout=3)

    def test_native_app_upload_streams_into_reserved_staging_file(self):
        binary = Path(sys.argv[1]).resolve()
        fakeLuna = binary.parent / "native-fake-luna-send"
        publicPort = unused_port_pair()
        nodePort = unused_port_pair((publicPort, publicPort + 1))
        secret = "apps-upload-proxy-secret-0123456789abcdef"
        token = "apps-upload-route-token"
        with tempfile.TemporaryDirectory(prefix="nativetv-native-app-upload-") as folder:
            root = Path(folder)
            config = root / "config.json"
            heartbeat = root / "tvweb.beat"
            assets = root / "assets"
            staging = root / "install-staging"
            systemRoot = root / "system"
            elevateHelper = root / "fake-elevate-service"
            assets.mkdir()
            staging.mkdir(mode=0o700)
            systemRoot.mkdir()
            config.write_text(json.dumps({
                "host": "127.0.0.1", "port": publicPort, "token": token,
                "allowControl": True,
                "apps": {"hosts": ["nativetv.example"], "sideload": False}
            }))
            nodeEvents = []
            statusReads = []
            installActionReads = []
            uploaded = {}

            class UploadNodeHandler(BaseHTTPRequestHandler):
                def do_GET(self):
                    if self.path != "/api/apps/install/status":
                        self.send_response(404)
                        self.end_headers()
                        return
                    statusReads.append(True)
                    response = json.dumps({
                        "state": "idle", "jobId": None, "source": None,
                        "appId": None, "progress": None, "preview": None,
                        "error": None, "result": None, "ok": True,
                        "writable": True, "sideload": True, "sideloadVia": "token"
                    }).encode()
                    self.send_response(200)
                    self.send_header("Content-Type", "application/json")
                    self.send_header("Content-Length", str(len(response)))
                    self.end_headers()
                    self.wfile.write(response)

                def do_POST(self):
                    length = int(self.headers.get("Content-Length", "0"))
                    raw = self.rfile.read(length)
                    if self.headers.get("X-NativeTv-Proxy-Secret") != secret or \
                            self.headers.get("X-NativeTv-Original-Remote") is not None:
                        response = json.dumps({"ok": False, "error": "forbidden"}).encode()
                        self.send_response(403)
                    elif self.path == "/_internal/apps/install-action":
                        payload = json.loads(raw.decode("utf-8"))
                        installActionReads.append(payload)
                        if payload.get("action") == "confirm":
                            response = json.dumps({
                                "ok": True,
                                "install": {
                                    "jobId": "fixture-job", "state": "installing"
                                }
                            }).encode()
                            self.send_response(200)
                        elif payload.get("action") == "cancel":
                            response = b'{"ok":true}'
                            self.send_response(200)
                        else:
                            response = b'{"ok":false,"error":"unknown action"}'
                            self.send_response(400)
                    elif self.path == "/_internal/apps/upload":
                        payload = json.loads(raw.decode("utf-8"))
                        action = payload["action"]
                        nodeEvents.append((action, payload))
                        if action == "start":
                            staged = staging / payload["file"]
                            uploaded["bytes"] = staged.read_bytes()
                            uploaded["mode"] = stat.S_IMODE(staged.stat().st_mode)
                            response = json.dumps({
                                "ok": True,
                                "install": {
                                    "jobId": "fixture-job", "state": "verifying",
                                    "source": "file", "appId": None, "repo": None,
                                    "progress": {"bytes": 0, "total": None},
                                    "preview": None, "error": None, "result": None
                                }
                            }).encode()
                            self.send_response(200)
                        else:
                            response = b'{"ok":false,"error":"unknown action"}'
                            self.send_response(400)
                    else:
                        response = b'{"ok":false,"error":"not found"}'
                        self.send_response(404)
                    self.send_header("Content-Type", "application/json")
                    self.send_header("Content-Length", str(len(response)))
                    self.end_headers()
                    self.wfile.write(response)

                def log_message(self, *_args):
                    pass

            node = HTTPServer(("127.0.0.1", nodePort), UploadNodeHandler)
            nodeThread = threading.Thread(target=node.serve_forever, daemon=True)
            nodeThread.start()
            environment = os.environ.copy()
            environment["TVWEB_LUNA_SEND"] = str(fakeLuna)
            environment["TVWEB_DATA_DIR"] = str(root)
            environment["TVWEB_PROXY_SECRET"] = secret
            environment["TVWEB_INSTALL_STAGING"] = str(staging)
            environment["TVWEB_ELEVATE_SERVICE"] = str(elevateHelper)
            process = subprocess.Popen(
                [str(binary), "--config", str(config), "--heartbeat", str(heartbeat),
                 "--workers", "4", "--listen", "--node-port", str(nodePort),
                 "--assets-root", str(assets), "--system-root", str(systemRoot)],
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                env=environment)
            output = ""

            def upload(data, contentType="application/octet-stream", origin=None):
                headers = {
                    "Authorization": "Bearer " + token,
                    "Content-Type": contentType
                }
                if origin is not None:
                    headers["Origin"] = origin
                request = Request(
                    "http://127.0.0.1:%d/api/apps/install/upload" % publicPort,
                    data=data, method="POST", headers=headers)
                try:
                    with urlopen(request, timeout=10) as response:
                        return response.status, response.read()
                except HTTPError as error:
                    return error.code, error.read()

            def install_status(host=None):
                headers = {"Authorization": "Bearer " + token}
                if host is not None:
                    headers["Host"] = host
                request = Request(
                    "http://127.0.0.1:%d/api/apps/install/status" % publicPort,
                    headers=headers)
                try:
                    with urlopen(request, timeout=5) as response:
                        return response.status, response.read()
                except HTTPError as error:
                    return error.code, error.read()

            def post_install_job(payload, proxy_secret=None, original_remote=None):
                headers = {"Content-Type": "application/json"}
                if proxy_secret is not None:
                    headers["X-NativeTv-Proxy-Secret"] = proxy_secret
                if original_remote is not None:
                    headers["X-NativeTv-Original-Remote"] = original_remote
                request = Request(
                    "http://127.0.0.1:%d/_internal/apps/install-job" % publicPort,
                    data=json.dumps(payload).encode("utf-8"), method="POST", headers=headers)
                try:
                    with urlopen(request, timeout=5) as response:
                        return response.status, response.read()
                except HTTPError as error:
                    return error.code, error.read()

            def post_install_preview(payload, proxy_secret=None, original_remote=None):
                headers = {"Content-Type": "application/json"}
                if proxy_secret is not None:
                    headers["X-NativeTv-Proxy-Secret"] = proxy_secret
                if original_remote is not None:
                    headers["X-NativeTv-Original-Remote"] = original_remote
                request = Request(
                    "http://127.0.0.1:%d/_internal/apps/install/preview" % publicPort,
                    data=json.dumps(payload).encode("utf-8"), method="POST", headers=headers)
                try:
                    with urlopen(request, timeout=5) as response:
                        return response.status, response.read()
                except HTTPError as error:
                    return error.code, error.read()

            def post_install_space(payload, proxy_secret=None, original_remote=None):
                headers = {"Content-Type": "application/json"}
                if proxy_secret is not None:
                    headers["X-NativeTv-Proxy-Secret"] = proxy_secret
                if original_remote is not None:
                    headers["X-NativeTv-Original-Remote"] = original_remote
                request = Request(
                    "http://127.0.0.1:%d/_internal/apps/install/space" % publicPort,
                    data=json.dumps(payload).encode("utf-8"), method="POST", headers=headers)
                try:
                    with urlopen(request, timeout=5) as response:
                        return response.status, response.read()
                except HTTPError as error:
                    return error.code, error.read()

            def download_native_package(payload, proxy_secret=None):
                headers = {"Content-Type": "application/json"}
                if proxy_secret is not None:
                    headers["X-NativeTv-Proxy-Secret"] = proxy_secret
                request = Request(
                    "http://127.0.0.1:%d/_internal/apps/download" % publicPort,
                    data=json.dumps(payload).encode("utf-8"), method="POST", headers=headers)
                try:
                    with urlopen(request, timeout=5) as response:
                        return response.status, response.read()
                except HTTPError as error:
                    return error.code, error.read()

            def install_action(action, job_id, extra=None):
                headers = {
                    "Authorization": "Bearer " + token,
                    "Content-Type": "application/json"
                }
                payload = {"jobId": job_id}
                if extra:
                    payload.update(extra)
                request = Request(
                    "http://127.0.0.1:%d/api/apps/install/%s" % (publicPort, action),
                    data=json.dumps(payload).encode("utf-8"),
                    method="POST", headers=headers)
                try:
                    with urlopen(request, timeout=5) as response:
                        return response.status, response.read()
                except HTTPError as error:
                    return error.code, error.read()

            try:
                deadline = time.monotonic() + 8
                ready = False
                while time.monotonic() < deadline and process.poll() is None:
                    request = Request(
                        "http://127.0.0.1:%d/api/caps" % publicPort,
                        headers={"Authorization": "Bearer " + token})
                    try:
                        with urlopen(request, timeout=1):
                            ready = True
                            break
                    except (OSError, URLError):
                        time.sleep(0.03)
                if not ready:
                    if process.poll() is None:
                        process.send_signal(signal.SIGTERM)
                    output, _ = process.communicate(timeout=6)
                    self.fail("native daemon did not start:\n" + output[-4000:])

                status, body = post_install_space({})
                self.assertEqual(status, 403, body.decode("utf-8", "replace"))
                status, body = post_install_space({}, secret, "192.0.2.20")
                self.assertEqual(status, 403, body.decode("utf-8", "replace"))
                status, body = post_install_space({"unexpected": True}, secret)
                self.assertEqual(status, 400, body.decode("utf-8", "replace"))
                status, body = post_install_space({}, secret)
                self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                expectedSpace = os.statvfs(staging)
                expectedBlockSize = expectedSpace.f_frsize or expectedSpace.f_bsize
                expectedFreeBytes = expectedSpace.f_bavail * expectedBlockSize
                spaceResult = json.loads(body)
                self.assertTrue(spaceResult["ok"])
                self.assertIsInstance(spaceResult["freeBytes"], (int, float))
                self.assertGreaterEqual(spaceResult["freeBytes"], 0)
                self.assertLessEqual(abs(spaceResult["freeBytes"] - expectedFreeBytes),
                                     256 * expectedBlockSize,
                                     "available space should come from the staging filesystem")

                hugePackageBytes = 9007199254740991 // 3
                status, body = download_native_package({
                    "url": "https://example.org/large.ipk",
                    "file": "0123456789abcdef.ipk",
                    "maxBytes": 512 * 1024 * 1024,
                    "expectedBytes": hugePackageBytes
                }, secret)
                self.assertEqual(status, 507, body.decode("utf-8", "replace"))
                capacityFailure = json.loads(body)
                self.assertTrue(capacityFailure["noSpace"])
                self.assertTrue(capacityFailure["maxBytesExceeded"])
                self.assertEqual(capacityFailure["needBytes"], hugePackageBytes * 3)
                self.assertLessEqual(
                    abs(capacityFailure["freeBytes"] - expectedFreeBytes),
                    256 * expectedBlockSize,
                    "native download capacity should read the staging filesystem")
                self.assertFalse((staging / "0123456789abcdef.ipk").exists(),
                                 "capacity refusal must happen before staging or downloading")
                status, body = download_native_package({
                    "url": "https://example.org/large.ipk",
                    "file": "0123456789abcdef.ipk",
                    "maxBytes": 512 * 1024 * 1024,
                    "expectedBytes": 1.5
                }, secret)
                self.assertEqual(status, 400, body.decode("utf-8", "replace"))

                status, body = install_status()
                self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                self.assertEqual(json.loads(body), {
                    "state": "idle", "jobId": None, "source": None,
                    "appId": None, "progress": None, "preview": None,
                    "error": None, "result": None, "ok": True,
                    "writable": True, "sideload": True, "sideloadVia": "token"
                })
                self.assertEqual(statusReads, [True],
                                 "native status falls back to Node until the first snapshot arrives")
                status, body = upload(b"x")
                self.assertEqual(status, 409, body.decode("utf-8", "replace"))
                self.assertEqual(nodeEvents, [],
                                 "uploads must wait for Node's recovered install snapshot")
                status, body = install_status(host="blocked.example")
                self.assertEqual(status, 403, body.decode("utf-8", "replace"))
                self.assertEqual(statusReads, [True],
                                 "disallowed Hosts must be rejected before consulting Node")

                previewInput = {
                    "jobId": "preview-fixture-job", "source": "catalog",
                    "package": "com.example.preview", "version": "1.2.0",
                    "apps": [{"id": "com.example.preview", "title": "Preview app"}],
                    "installedApps": [],
                    "services": ["com.example.preview.service"],
                    "rootRequired": True, "pkgVetted": True, "reelevate": [],
                    "cpuMismatch": False, "storeInstalled": True,
                    "elevationAvailable": True, "packageBytes": 512,
                    "freeBytes": 0, "needBytes": 999999,
                    "sha256": "a" * 64
                }
                status, body = post_install_preview(previewInput)
                self.assertEqual(status, 403, body.decode("utf-8", "replace"))
                status, body = post_install_preview(previewInput, secret, "192.0.2.20")
                self.assertEqual(status, 403, body.decode("utf-8", "replace"))
                status, body = post_install_preview(previewInput, secret)
                self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                previewResponse = json.loads(body)
                self.assertTrue(previewResponse["ok"])
                self.assertTrue(previewResponse["capacityOkay"])
                self.assertEqual(previewResponse["preview"]["needBytes"], 1024,
                                 "native preview derives the unpack/install space estimate")
                self.assertGreater(previewResponse["preview"]["freeBytes"], 0,
                                   "native preview reads free bytes from staging storage")
                self.assertFalse(previewResponse["elevationAvailable"],
                                 "native preview must ignore the caller's helper claim")
                self.assertFalse(previewResponse["preview"]["storeInstalled"],
                                 "native preview must ignore the caller's store claim")
                self.assertFalse(previewResponse["needsReview"],
                                 "vetted catalog root services may auto-elevate")
                self.assertEqual(previewResponse["preview"]["direction"], "new")
                self.assertIsNone(previewResponse["preview"]["installedVersion"])
                self.assertTrue(previewResponse["preview"]["rootRequired"])
                self.assertFalse(previewResponse["preview"]["willElevate"])
                self.assertEqual(previewResponse["preview"]["apps"], previewInput["apps"])
                self.assertEqual(previewResponse["preview"]["sha256"], previewInput["sha256"])

                installedPreviewInput = dict(previewInput)
                installedPreviewInput["installedApps"] = [
                    {"id": "com.example.preview", "version": "1.3.0"}
                ]
                installedPreviewInput["installedVersion"] = "99.0.0"
                status, body = post_install_preview(installedPreviewInput, secret)
                self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                installedPreview = json.loads(body)
                self.assertEqual(installedPreview["preview"]["installedVersion"], "1.3.0")
                self.assertEqual(installedPreview["preview"]["direction"], "down")
                self.assertTrue(installedPreview["needsReview"])

                storeAppInfo = systemRoot / "media/cryptofs/apps/usr/palm/applications" / \
                    "com.example.preview" / "appinfo.json"
                storeAppInfo.parent.mkdir(parents=True)
                storeAppInfo.write_text(json.dumps({"id": "com.example.preview"}))
                status, body = post_install_preview(previewInput, secret)
                self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                storePreview = json.loads(body)
                self.assertFalse(storePreview["elevationAvailable"])
                self.assertTrue(storePreview["preview"]["storeInstalled"])
                self.assertTrue(storePreview["needsReview"],
                                 "LG store apps require an explicit review")

                invalidPreviewInput = dict(previewInput)
                invalidPreviewInput["package"] = "../outside"
                status, body = post_install_preview(invalidPreviewInput, secret)
                self.assertEqual(status, 400,
                                 "unsafe package IDs must be rejected before path checks")

                activeSnapshot = {
                    "state": "downloading", "jobId": "fixture-job",
                    "source": "catalog", "appId": "com.example.app",
                    "repo": "https://repo.example/apps.json",
                    "progress": {"bytes": 7, "total": 21},
                    "preview": None, "error": None, "result": None
                }
                status, body = post_install_job({
                    "action": "snapshot", "snapshot": activeSnapshot
                })
                self.assertEqual(status, 403, body.decode("utf-8", "replace"))
                status, body = post_install_job({
                    "action": "snapshot", "snapshot": activeSnapshot
                }, secret, "192.0.2.20")
                self.assertEqual(status, 403, body.decode("utf-8", "replace"))
                status, body = post_install_job({
                    "action": "snapshot", "snapshot": activeSnapshot
                }, secret)
                self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                self.assertEqual(json.loads(body), {"ok": True})
                status, body = install_status()
                self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                currentSnapshot = json.loads(body)
                self.assertEqual(currentSnapshot["state"], "downloading")
                self.assertEqual(currentSnapshot["progress"], {"bytes": 7, "total": 21})
                self.assertEqual(currentSnapshot["repo"], activeSnapshot["repo"])
                self.assertEqual(statusReads, [True], "cached status reads must not call Node")

                invalidSnapshot = dict(activeSnapshot)
                invalidSnapshot["state"] = "not-a-state"
                status, body = post_install_job({
                    "action": "snapshot", "snapshot": invalidSnapshot
                }, secret)
                self.assertEqual(status, 400, body.decode("utf-8", "replace"))
                status, body = install_status()
                self.assertEqual(json.loads(body)["state"], "downloading",
                                 "invalid snapshots must preserve the last valid state")

                status, body = install_action("cancel", "stale-job")
                self.assertEqual(status, 400, body.decode("utf-8", "replace"))
                self.assertEqual(installActionReads, [],
                                 "stale job IDs must be rejected before Node")
                status, body = install_action("confirm", "fixture-job")
                self.assertEqual(status, 400, body.decode("utf-8", "replace"))
                self.assertEqual(installActionReads, [],
                                 "confirm must wait until preview confirmation")
                status, body = install_action("cancel", "fixture-job")
                self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                self.assertEqual(len(installActionReads), 1)
                self.assertEqual(installActionReads[-1]["action"], "cancel")
                self.assertFalse(installActionReads[-1]["nativeValidated"])
                self.assertEqual(installActionReads[-1]["body"]["jobId"], "fixture-job")

                awaitingConfirm = dict(activeSnapshot)
                awaitingConfirm["state"] = "awaiting-confirm"
                awaitingConfirm["preview"] = {
                    "storeInstalled": True,
                    "vetted": False,
                    "services": ["com.example.app.first", "com.example.app.second"]
                }
                status, body = post_install_job({
                    "action": "snapshot", "snapshot": awaitingConfirm
                }, secret)
                self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                status, body = install_action("confirm", "fixture-job")
                self.assertEqual(status, 400, body.decode("utf-8", "replace"))
                self.assertIn("separate confirmation", json.loads(body)["error"])
                self.assertEqual(len(installActionReads), 1,
                                 "store replacement must be refused before Node")

                untypedStoreConfirmation = {
                    "replaceStore": "confirmed",
                    "elevate": True,
                    "confirmRoot": ["com.example.app.second", "com.example.app.first"]
                }
                status, body = install_action(
                    "confirm", "fixture-job", untypedStoreConfirmation)
                self.assertEqual(status, 400, body.decode("utf-8", "replace"))
                self.assertIn("separate confirmation", json.loads(body)["error"])
                self.assertEqual(len(installActionReads), 1,
                                 "store replacement requires the strict boolean used by Node")

                confirmOptions = {
                    "replaceStore": True,
                    "elevate": True,
                    "confirmRoot": ["com.example.app.second", "com.example.app.first"]
                }
                status, body = install_action("confirm", "fixture-job", confirmOptions)
                self.assertEqual(status, 400, body.decode("utf-8", "replace"))
                self.assertIn("Homebrew Channel", json.loads(body)["error"])
                self.assertEqual(len(installActionReads), 1,
                                 "unavailable elevation helper must be refused before Node")

                elevateHelper.write_text("fixture helper")
                wrongRootConfirmation = dict(confirmOptions)
                wrongRootConfirmation["confirmRoot"] = ["com.example.app.other"]
                status, body = install_action("confirm", "fixture-job", wrongRootConfirmation)
                self.assertEqual(status, 400, body.decode("utf-8", "replace"))
                self.assertIn("services listed in its preview", json.loads(body)["error"])
                self.assertEqual(len(installActionReads), 1,
                                 "unconfirmed root services must be refused before Node")

                status, body = install_action("confirm", "fixture-job", confirmOptions)
                self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                self.assertEqual(len(installActionReads), 2)
                self.assertEqual(installActionReads[-1]["action"], "confirm")
                self.assertTrue(installActionReads[-1]["nativeValidated"])

                status, body = post_install_preview(previewInput, secret)
                self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                helperPreview = json.loads(body)
                self.assertTrue(helperPreview["elevationAvailable"])
                self.assertTrue(helperPreview["preview"]["storeInstalled"])
                self.assertEqual(installActionReads[-1]["body"], {
                    "jobId": "fixture-job", **confirmOptions,
                    "nativeServices": [
                        "com.example.app.first", "com.example.app.second"
                    ]
                })

                installing = dict(activeSnapshot)
                installing["state"] = "installing"
                status, body = post_install_job({
                    "action": "snapshot", "snapshot": installing
                }, secret)
                self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                status, body = install_action("cancel", "fixture-job")
                self.assertEqual(status, 400, body.decode("utf-8", "replace"))
                self.assertEqual(len(installActionReads), 2,
                                 "an install in progress must not reach Node cancellation")
                status, body = upload(b"x")
                self.assertEqual(status, 409, body.decode("utf-8", "replace"))
                self.assertEqual(nodeEvents, [],
                                 "an active install must block upload reservation before Node")

                status, body = post_install_job({"action": "clear"}, secret)
                self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                status, body = install_status()
                self.assertEqual(json.loads(body)["state"], "idle",
                                 "clearing a job must clear the live snapshot")

                status, body = upload(b"x", origin="http://attacker.example")
                self.assertEqual(status, 403, body.decode("utf-8", "replace"))
                status, body = upload(b"x", contentType="application/octet-stream; charset=binary")
                self.assertEqual(status, 415, body.decode("utf-8", "replace"))
                self.assertEqual(nodeEvents, [], "refused uploads must not reserve staging")

                chunked = http.client.HTTPConnection("127.0.0.1", publicPort, timeout=5)
                chunked.putrequest("POST", "/api/apps/install/upload")
                chunked.putheader("Authorization", "Bearer " + token)
                chunked.putheader("Content-Type", "application/octet-stream")
                chunked.putheader("Transfer-Encoding", "chunked")
                chunked.putheader("Connection", "close")
                chunked.endheaders()
                chunked.send(b"1\r\nx\r\n0\r\n\r\n")
                response = chunked.getresponse()
                chunkedBody = response.read()
                self.assertEqual(response.status, 411, chunkedBody.decode("utf-8", "replace"))
                chunked.close()

                oversized = http.client.HTTPConnection("127.0.0.1", publicPort, timeout=5)
                oversized.putrequest("POST", "/api/apps/install/upload")
                oversized.putheader("Authorization", "Bearer " + token)
                oversized.putheader("Content-Type", "application/octet-stream")
                oversized.putheader("Content-Length", str(536870913))
                oversized.putheader("Connection", "close")
                oversized.endheaders()
                response = oversized.getresponse()
                oversizedBody = response.read()
                self.assertEqual(response.status, 413,
                                 oversizedBody.decode("utf-8", "replace"))
                oversized.close()
                self.assertEqual(nodeEvents, [], "invalid upload lengths must not reserve staging")

                package = bytes((index * 13) % 256 for index in range(256 * 1024))
                status, body = upload(package)
                self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                self.assertEqual(json.loads(body), {
                    "ok": True,
                    "install": {
                        "jobId": "fixture-job", "state": "verifying",
                        "source": "file", "appId": None, "repo": None,
                        "progress": {"bytes": 0, "total": None},
                        "preview": None, "error": None, "result": None
                    }
                })
                self.assertEqual([event[0] for event in nodeEvents], ["start"])
                self.assertEqual(nodeEvents[0][1].keys(), {"action", "file"})
                self.assertRegex(nodeEvents[-1][1]["file"], r"^upload-[0-9a-f]{16}\.ipk$")
                self.assertEqual(uploaded["bytes"], package)
                self.assertEqual(uploaded["mode"], 0o600)
                status, body = install_status()
                self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                cachedStatus = json.loads(body)
                self.assertEqual(cachedStatus["state"], "verifying")
                self.assertEqual(cachedStatus["source"], "file")
                self.assertEqual(statusReads, [True],
                                 "native upload start must seed managed status without another Node read")

                process.send_signal(signal.SIGTERM)
                output, _ = process.communicate(timeout=6)
                self.assertEqual(process.returncode, 0, output)
            finally:
                if process.poll() is None:
                    process.send_signal(signal.SIGTERM)
                    try:
                        process.communicate(timeout=6)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait(timeout=3)
                node.shutdown()
                node.server_close()
                nodeThread.join(timeout=3)

    def test_native_install_jobs_run_without_node(self):
        binary = Path(sys.argv[1]).resolve()
        fakeLuna = binary.parent / "native-fake-luna-send"
        publicPort = unused_port_pair()
        nodePort = unused_port_pair((publicPort, publicPort + 1))
        secret = "install-jobs-proxy-secret-0123456789abcdef"
        token = "install-jobs-route-token"
        nodeRequests = []

        class NodeHandler(BaseHTTPRequestHandler):
            def do_POST(self):
                length = int(self.headers.get("Content-Length", "0"))
                nodeRequests.append((self.path, self.rfile.read(length)))
                self.send_response(204)
                self.send_header("Content-Length", "0")
                self.end_headers()

            def do_GET(self):
                nodeRequests.append((self.path, b""))
                self.send_response(404)
                self.send_header("Content-Length", "0")
                self.end_headers()

            def log_message(self, *_args):
                pass

        node = HTTPServer(("127.0.0.1", nodePort), NodeHandler)
        nodeThread = threading.Thread(target=node.serve_forever, daemon=True)
        nodeThread.start()
        with tempfile.TemporaryDirectory(prefix="nativetv-native-install-jobs-") as folder:
            root = Path(folder)
            dataRoot = root / "data"
            staging = root / "install-stage"
            systemRoot = root / "system"
            assets = root / "assets"
            for directory in (dataRoot, assets, systemRoot):
                directory.mkdir()
            staging.mkdir(mode=0o700)
            packageBytes = app_install_ipk()
            installLog = root / "install-arguments.jsonl"
            installMode = root / "install-mode"
            installMode.write_text("installed")
            installLuna = root / "install-luna-send"
            installLuna.write_text(
                "#!/usr/bin/env python3\n"
                "import json, os, sys, time\n"
                "args = sys.argv[1:]\n"
                "uri = next((arg for arg in args if arg.startswith('luna://')), '')\n"
                "if not uri.endswith('/dev/install'):\n"
                "    fake = os.environ['NATIVETV_TEST_FAKE_LUNA']\n"
                "    os.execv(fake, [fake] + args)\n"
                "with open(os.environ['NATIVETV_TEST_INSTALL_LOG'], 'a') as output:\n"
                "    output.write(json.dumps(args) + '\\n')\n"
                "print(json.dumps({'returnValue': True, 'statusValue': 10}), flush=True)\n"
                "print(json.dumps({'statusValue': 30, 'details': {'packageId': 'com.example.app'}}), flush=True)\n"
                "time.sleep(20)\n")
            installLuna.chmod(0o700)
            elevateDirectory = (systemRoot / "media/developer/apps/usr/palm/services/"
                                "org.webosbrew.hbchannel.service")
            elevateDirectory.mkdir(parents=True)
            elevateLog = elevateDirectory / "calls.log"
            elevateExecutable = elevateDirectory / "elevate-service"
            elevateExecutable.write_text(
                "#!/bin/sh\nprintf '%s\\n' \"$1\" >> " + shlex.quote(str(elevateLog)) + "\nexit 0\n")
            elevateExecutable.chmod(0o700)
            config = root / "config.json"
            config.write_text(json.dumps({
                "host": "127.0.0.1", "port": publicPort, "token": token,
                "allowControl": True, "apps": {"sideload": True}
            }))
            environment = os.environ.copy()
            environment["TVWEB_LUNA_SEND"] = str(installLuna)
            environment["TVWEB_DATA_DIR"] = str(dataRoot)
            environment["TVWEB_PROXY_SECRET"] = secret
            environment["TVWEB_INSTALL_STAGING"] = str(staging)
            environment["TVWEB_NATIVE_INSTALL"] = "1"
            environment["NATIVETV_TEST_FAKE_LUNA"] = str(fakeLuna)
            environment["NATIVETV_TEST_INSTALL_LOG"] = str(installLog)
            environment["NATIVETV_TEST_INSTALL_MODE"] = str(installMode)
            environment["NATIVETV_FAKE_LUNA_MODE"] = "app-install-recovery"
            process = subprocess.Popen(
                [str(binary), "--config", str(config), "--heartbeat", str(root / "tvweb.beat"),
                 "--workers", "6", "--listen", "--node-port", str(nodePort),
                 "--assets-root", str(assets), "--system-root", str(systemRoot)],
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, env=environment)

            def call(path, method="GET", data=None, contentType=None):
                headers = {"Authorization": "Bearer " + token}
                if contentType:
                    headers["Content-Type"] = contentType
                request = Request("http://127.0.0.1:%d%s" % (publicPort, path),
                                  data=data, method=method, headers=headers)
                try:
                    with urlopen(request, timeout=10) as response:
                        return response.status, json.loads(response.read() or b"null")
                except HTTPError as error:
                    return error.code, json.loads(error.read() or b"null")

            def post(path, payload):
                return call(path, "POST", json.dumps(payload).encode("utf-8"), "application/json")

            def status_in(states, seconds=10):
                deadline = time.monotonic() + seconds
                last = None
                while time.monotonic() < deadline:
                    code, last = call("/api/apps/install/status")
                    self.assertEqual(code, 200, last)
                    if last["state"] in states:
                        return last
                    time.sleep(0.05)
                self.fail("install stayed in %r, wanted %r" % (last, states))

            try:
                deadline = time.monotonic() + 12
                ready = False
                while time.monotonic() < deadline and process.poll() is None:
                    try:
                        code, _ = call("/api/caps")
                        ready = code == 200
                    except (OSError, URLError):
                        ready = False
                    if ready:
                        break
                    time.sleep(0.05)
                if not ready:
                    if process.poll() is None:
                        process.send_signal(signal.SIGTERM)
                    output, _ = process.communicate(timeout=8)
                    self.fail("daemon did not start:\n" + output[-4000:])

                code, idle = call("/api/apps/install/status")
                self.assertEqual(code, 200, idle)
                self.assertEqual(idle["state"], "idle")
                self.assertTrue(idle["ok"] and idle["writable"] and idle["sideload"])

                code, started = call("/api/apps/install/upload", "POST", packageBytes,
                                     "application/octet-stream")
                self.assertEqual(code, 200, started)
                self.assertTrue(started["ok"])
                self.assertEqual(started["install"]["source"], "file")
                jobId = started["install"]["jobId"]
                preview = status_in(["awaiting-confirm"])["preview"]
                self.assertEqual(preview["package"], "com.example.app")
                self.assertEqual(preview["services"], ["com.example.app.service"])
                self.assertEqual(preview["sha256"], hashlib.sha256(packageBytes).hexdigest())
                self.assertFalse(preview["vetted"])
                code, busy = call("/api/apps/install/upload", "POST", b"x", "application/octet-stream")
                self.assertEqual(code, 409, busy)

                code, refused = post("/api/apps/install/confirm", {"jobId": jobId, "elevate": True})
                self.assertEqual(code, 400, refused)
                self.assertIn("must be confirmed for the services", refused["error"])
                self.assertEqual(status_in(["awaiting-confirm"])["jobId"], jobId)

                code, confirmed = post("/api/apps/install/confirm", {
                    "jobId": jobId, "elevate": True, "confirmRoot": ["com.example.app.service"]})
                self.assertEqual(code, 200, confirmed)
                self.assertEqual(confirmed["install"]["state"], "installing")
                done = status_in(["installed", "error"])
                self.assertEqual(done["state"], "installed", done)
                self.assertEqual(done["result"]["package"], "com.example.app")
                self.assertEqual(done["result"]["title"], "Example")
                self.assertEqual(done["result"]["elevation"],
                                 {"done": ["com.example.app.service"], "failed": []})
                self.assertEqual(elevateLog.read_text().split(), ["com.example.app.service"])
                installArgs = json.loads(installLog.read_text().splitlines()[0])
                self.assertEqual(installArgs[:2], ["-i", "luna://com.webos.appInstallService/dev/install"])
                self.assertEqual(json.loads((dataRoot / "elevated.json").read_text()),
                                 {"com.example.app": ["com.example.app.service"]})
                self.assertEqual(json.loads((dataRoot / "install-job.json").read_text())["state"], "installed")
                self.assertEqual(list(staging.iterdir()), [], "a finished install clears the staging folder")
                deadline = time.monotonic() + 3
                while time.monotonic() < deadline and not any(
                        path == "/_internal/native-state" and b"apps-changed" in body
                        for path, body in nodeRequests):
                    time.sleep(0.05)
                self.assertTrue(any(path == "/_internal/native-state" and b"apps-changed" in body
                                    for path, body in nodeRequests),
                                "Node is told the app list changed")

                code, second = call("/api/apps/install/upload", "POST", packageBytes,
                                    "application/octet-stream")
                self.assertEqual(code, 200, second)
                secondId = second["install"]["jobId"]
                self.assertNotEqual(secondId, jobId)
                status_in(["awaiting-confirm"])
                code, stale = post("/api/apps/install/cancel", {"jobId": jobId})
                self.assertEqual(code, 400, stale)
                code, cancelled = post("/api/apps/install/cancel", {"jobId": secondId})
                self.assertEqual(code, 200, cancelled)
                self.assertEqual(status_in(["idle"])["state"], "idle")
                self.assertFalse((dataRoot / "install-job.json").exists())
                self.assertEqual(list(staging.iterdir()), [])

                self.assertEqual([path for path, _ in nodeRequests
                                  if path.startswith("/_internal/apps/install") or
                                  path.startswith("/_internal/apps/upload") or
                                  path.startswith("/api/apps/install")], [],
                                 "the install flow must not touch Node's coordinator")
            finally:
                if process.poll() is None:
                    process.send_signal(signal.SIGTERM)
                    try:
                        process.communicate(timeout=8)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.communicate(timeout=3)
                node.shutdown()
                node.server_close()
                nodeThread.join(timeout=3)

    def test_native_apps_uninstall_sideload_and_repo_routes(self):
        binary = Path(sys.argv[1]).resolve()
        fakeLuna = binary.parent / "native-fake-luna-send"
        publicPort = unused_port_pair()
        nodePort = unused_port_pair((publicPort, publicPort + 1))
        secret = "apps-uninstall-proxy-secret-0123456789abcdef"
        token = "apps-uninstall-route-token"
        with tempfile.TemporaryDirectory(prefix="nativetv-native-app-uninstall-") as folder:
            root = Path(folder)
            config = root / "config.json"
            heartbeat = root / "tvweb.beat"
            assets = root / "assets"
            details = root / "luna-details.jsonl"
            assets.mkdir()
            config.write_text(json.dumps({
                "host": "127.0.0.1", "port": publicPort, "token": token,
                "allowControl": True,
                "apps": {"hosts": ["nativetv.example"],
                         "repos": ["https://example.org/repo.json"],
                         "sideload": False}
            }))
            nodeEvents = []
            nodeReloadStatus = [200]

            class NativeStateHandler(BaseHTTPRequestHandler):
                def do_POST(self):
                    length = int(self.headers.get("Content-Length", "0"))
                    body = self.rfile.read(length)
                    nodeEvents.append((self.path,
                                       self.headers.get("X-NativeTv-Proxy-Secret"),
                                       json.loads(body) if body else None))
                    if self.path == "/_internal/reload-config":
                        response = b'{"ok":true}'
                        self.send_response(nodeReloadStatus[0])
                        self.send_header("Content-Length", str(len(response)))
                        self.end_headers()
                        self.wfile.write(response)
                    else:
                        self.send_response(204)
                        self.end_headers()

                def log_message(self, format, *args):
                    pass

            nodeServer = HTTPServer(("127.0.0.1", nodePort), NativeStateHandler)
            nodeThread = threading.Thread(target=nodeServer.serve_forever)
            nodeThread.daemon = True
            nodeThread.start()
            environment = os.environ.copy()
            environment["TVWEB_LUNA_SEND"] = str(fakeLuna)
            environment["TVWEB_DATA_DIR"] = str(root / "data")
            environment["TVWEB_PROXY_SECRET"] = secret
            environment["NATIVETV_FAKE_LUNA_MODE"] = "apps-uninstall"
            environment["NATIVETV_FAKE_LUNA_DETAILS"] = str(details)
            process = subprocess.Popen(
                [str(binary), "--config", str(config), "--heartbeat", str(heartbeat),
                 "--workers", "4", "--listen", "--node-port", str(nodePort),
                 "--assets-root", str(assets)],
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                env=environment)

            def request(command, withSecret=True, originalRemote=None):
                data = json.dumps(command).encode("utf-8")
                headers = {"Content-Type": "application/json"}
                if withSecret:
                    headers["X-NativeTv-Proxy-Secret"] = secret
                if originalRemote is not None:
                    headers["X-NativeTv-Original-Remote"] = originalRemote
                req = Request("http://127.0.0.1:%d/_internal/apps/uninstall" % publicPort,
                              data=data, method="POST", headers=headers)
                try:
                    with urlopen(req, timeout=15) as response:
                        return response.status, response.read()
                except HTTPError as error:
                    return error.code, error.read()

            def publicRequest(appId, origin=None):
                data = json.dumps({"id": appId}).encode("utf-8")
                headers = {"Content-Type": "application/json",
                           "Authorization": "Bearer " + token}
                if origin is not None:
                    headers["Origin"] = origin
                req = Request("http://127.0.0.1:%d/api/apps/uninstall" % publicPort,
                              data=data, method="POST", headers=headers)
                try:
                    with urlopen(req, timeout=15) as response:
                        return response.status, response.read()
                except HTTPError as error:
                    return error.code, error.read()

            def publicSideloadRequest(enabled, host="nativetv.example", origin=None,
                                      contentType="application/json"):
                data = json.dumps({"enabled": enabled}).encode("utf-8")
                headers = {"Content-Type": contentType,
                           "Authorization": "Bearer " + token,
                           "Host": host}
                if origin is not None:
                    headers["Origin"] = origin
                req = Request("http://127.0.0.1:%d/api/apps/install/sideload" % publicPort,
                              data=data, method="POST", headers=headers)
                try:
                    with urlopen(req, timeout=15) as response:
                        return response.status, response.read()
                except HTTPError as error:
                    return error.code, error.read()

            def nativeReposRequest(repos, withSecret=True, originalRemote=None):
                data = json.dumps({"repos": repos}).encode("utf-8")
                headers = {"Content-Type": "application/json"}
                if withSecret:
                    headers["X-NativeTv-Proxy-Secret"] = secret
                if originalRemote is not None:
                    headers["X-NativeTv-Original-Remote"] = originalRemote
                req = Request("http://127.0.0.1:%d/_internal/apps/repos" % publicPort,
                              data=data, method="POST", headers=headers)
                try:
                    with urlopen(req, timeout=15) as response:
                        return response.status, response.read()
                except HTTPError as error:
                    return error.code, error.read()

            def publicReposRequest(payload, host="nativetv.example", origin=None,
                                   contentType="application/json"):
                data = json.dumps(payload).encode("utf-8")
                headers = {"Content-Type": contentType,
                           "Authorization": "Bearer " + token,
                           "Host": host}
                if origin is not None:
                    headers["Origin"] = origin
                req = Request("http://127.0.0.1:%d/api/apps/install/repos" % publicPort,
                              data=data, method="POST", headers=headers)
                try:
                    with urlopen(req, timeout=15) as response:
                        return response.status, response.read()
                except HTTPError as error:
                    return error.code, error.read()

            def removalCalls():
                if not details.exists():
                    return []
                routes = {
                    "com.webos.appInstallService/remove",
                    "com.webos.appInstallService/dev/remove"
                }
                return [json.loads(line) for line in details.read_text().splitlines()
                        if json.loads(line)["uri"] in routes]

            def lifecycleCalls():
                if not details.exists():
                    return []
                routes = {
                    "com.webos.applicationManager/closeByAppId",
                    "com.webos.applicationManager/listApps"
                }
                return [json.loads(line) for line in details.read_text().splitlines()
                        if json.loads(line)["uri"] in routes]

            try:
                deadline = time.monotonic() + 8
                ready = False
                while time.monotonic() < deadline and process.poll() is None:
                    req = Request("http://127.0.0.1:%d/api/caps" % publicPort,
                                  headers={"Authorization": "Bearer " + token})
                    try:
                        with urlopen(req, timeout=2):
                            ready = True
                            break
                    except (OSError, URLError):
                        time.sleep(0.03)
                if not ready:
                    if process.poll() is None:
                        process.send_signal(signal.SIGTERM)
                    output, _ = process.communicate(timeout=6)
                    self.fail("native daemon did not start:\n" + output[-4000:])

                status, _ = nativeReposRequest(["https://custom.example/repo.json"],
                                               withSecret=False)
                self.assertEqual(status, 403,
                                 "repo config bridge must require the shared secret")
                status, _ = nativeReposRequest(["https://custom.example/repo.json"],
                                               originalRemote="192.0.2.50")
                self.assertEqual(status, 403,
                                 "repo config bridge must reject forwarded public requests")
                status, _ = nativeReposRequest(["https://custom.example/repo.json", 2])
                self.assertEqual(status, 400,
                                 "repo config bridge must validate the persisted list")
                status, body = nativeReposRequest([
                    "https://custom.example/repo.json", "https://second.example/repo.json"])
                self.assertEqual(status, 204, body.decode("utf-8", "replace"))
                savedConfig = json.loads(config.read_text())
                self.assertEqual(savedConfig["apps"], {
                    "hosts": ["nativetv.example"],
                    "repos": ["https://custom.example/repo.json",
                              "https://second.example/repo.json"],
                    "sideload": False})

                status, body = request({"id": "store.app"}, withSecret=False)
                self.assertEqual(status, 403, body.decode("utf-8", "replace"))
                self.assertEqual(removalCalls(), [], "missing secret must not call appInstallService")
                status, body = request({"id": "store.app"}, originalRemote="203.0.113.20")
                self.assertEqual(status, 403, body.decode("utf-8", "replace"))
                self.assertEqual(removalCalls(), [], "proxied requests must not use the internal route")
                for invalid in ("../etc/passwd", "com.webos.app.livetv",
                                "com.webos.app.input.hdmi2"):
                    status, _ = request({"id": invalid})
                    self.assertEqual(status, 400, "unsafe app id was accepted: %s" % invalid)
                self.assertEqual(removalCalls(), [], "invalid/protected IDs must be rejected before Luna")

                status, _ = publicRequest("public.app", origin="http://attacker.example")
                self.assertEqual(status, 403, "public uninstall must reject cross-origin POSTs")
                status, _ = publicRequest("com.webos.app.livetv")
                self.assertEqual(status, 400, "public uninstall must refuse protected apps")
                self.assertEqual(removalCalls(), [], "rejected public requests must not reach Luna")

                status, body = request({"id": "store.app"})
                self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                self.assertEqual(json.loads(body), {"ok": True, "id": "store.app"})
                status, body = request({"id": "dev.app"})
                self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                self.assertEqual(json.loads(body), {"ok": True, "id": "dev.app"})
                status, body = publicRequest(
                    "public.app", origin="http://127.0.0.1:%d" % publicPort)
                self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                self.assertEqual(json.loads(body), {"ok": True, "id": "public.app"})
                status, body = publicRequest(
                    "custom.screensaver", origin="http://127.0.0.1:%d" % publicPort)
                self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                self.assertEqual(json.loads(body), {"ok": True, "id": "custom.screensaver"})
                self.assertEqual(nodeEvents, [
                    ("/_internal/native-state", secret, {"type": "apps-changed"}),
                    ("/_internal/native-state", secret,
                     {"type": "apps-uninstall-prep", "id": "custom.screensaver"}),
                    ("/_internal/native-state", secret, {"type": "apps-changed"})
                ])

                status, _ = publicReposRequest(
                    {"action": "remove", "url": "https://custom.example/repo.json"},
                    host="attacker.example")
                self.assertEqual(status, 403,
                                 "public repo route must reject unlisted Host headers")
                status, _ = publicReposRequest(
                    {"action": "remove", "url": "https://custom.example/repo.json"},
                    origin="http://attacker.example")
                self.assertEqual(status, 403,
                                 "public repo route must reject cross-origin POSTs")
                status, _ = publicReposRequest(
                    {"action": "remove", "url": "https://custom.example/repo.json"},
                    contentType="text/plain")
                self.assertEqual(status, 415,
                                 "public repo route must require JSON content type")
                status, body = publicReposRequest({"action": "replace"})
                self.assertEqual(status, 400, body.decode("utf-8", "replace"))
                self.assertEqual(json.loads(body), {
                    "ok": False, "error": "action must be add or remove"})
                status, body = publicReposRequest({
                    "action": "remove", "url": "https://missing.example/repo.json"})
                self.assertEqual(status, 400, body.decode("utf-8", "replace"))
                self.assertEqual(json.loads(body), {
                    "ok": False, "error": "that catalog is not one added here"})
                status, body = publicReposRequest({"action": "add", "url": ""})
                self.assertEqual(status, 400, body.decode("utf-8", "replace"))

                cappedRepos = ["https://repo-%d.example/apps.json" % index
                               for index in range(10)]
                status, body = nativeReposRequest(cappedRepos)
                self.assertEqual(status, 204, body.decode("utf-8", "replace"))
                status, body = publicReposRequest({
                    "action": "add", "url": "https://unused.example/apps.json"})
                self.assertEqual(status, 400, body.decode("utf-8", "replace"))
                self.assertEqual(json.loads(body), {
                    "ok": False, "error": "at most 10 catalogs can be added"})
                status, body = nativeReposRequest([
                    "https://custom.example/repo.json",
                    "https://second.example/repo.json"])
                self.assertEqual(status, 204, body.decode("utf-8", "replace"))

                status, body = publicReposRequest({
                    "action": "remove", "url": "https://custom.example/repo.json"})
                self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                self.assertEqual(json.loads(body), {
                    "ok": True, "repos": ["https://second.example/repo.json"]})
                savedConfig = json.loads(config.read_text())
                self.assertEqual(savedConfig["apps"], {
                    "hosts": ["nativetv.example"],
                    "repos": ["https://second.example/repo.json"],
                    "sideload": False})
                self.assertEqual(nodeEvents[-1],
                                 ("/_internal/reload-config", secret, None),
                                 "native repo changes must reload Node config")

                status, body = publicSideloadRequest(
                    True, host="attacker.example")
                self.assertEqual(status, 403,
                                 "sideload settings must reject unlisted Host headers")
                status, body = publicSideloadRequest(True, host="[::1]:invalid")
                self.assertEqual(status, 403,
                                 "sideload settings must reject malformed bracketed Host headers")
                status, body = publicSideloadRequest(
                    True, origin="http://attacker.example")
                self.assertEqual(status, 403,
                                 "sideload settings must reject cross-origin POSTs")
                status, _ = publicSideloadRequest(True, contentType="text/plain")
                self.assertEqual(status, 415,
                                 "sideload settings must require JSON content type")
                status, body = publicSideloadRequest(
                    True, origin="http://nativetv.example")
                self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                self.assertEqual(json.loads(body), {
                    "ok": True, "sideload": True, "sideloadVia": "config"})
                status, body = publicSideloadRequest(
                    False, origin="http://nativetv.example")
                self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                self.assertEqual(json.loads(body), {
                    "ok": True, "sideload": False, "sideloadVia": "token"})
                savedConfig = json.loads(config.read_text())
                self.assertEqual(savedConfig["apps"], {
                    "hosts": ["nativetv.example"],
                    "repos": ["https://second.example/repo.json"],
                    "sideload": False})
                self.assertEqual(nodeEvents[-2:], [
                    ("/_internal/reload-config", secret, None),
                    ("/_internal/reload-config", secret, None)
                ], "native config writes must reload Node's installer policy")
                nodeReloadStatus[0] = 500
                status, body = publicSideloadRequest(
                    True, origin="http://nativetv.example")
                self.assertEqual(status, 502,
                                 "native must report when Node did not accept the policy reload")
                self.assertEqual(json.loads(body), {
                    "ok": False,
                    "error": "sideload setting was saved but the installer policy could not be reloaded"
                })
                self.assertTrue(json.loads(config.read_text())["apps"]["sideload"],
                                "failed Node reload must not erase the already-saved setting")
                status, body = publicReposRequest({
                    "action": "remove", "url": "https://second.example/repo.json"})
                self.assertEqual(status, 502, body.decode("utf-8", "replace"))
                self.assertEqual(json.loads(body), {
                    "ok": False,
                    "error": "app repository list was saved but the installer config could not be reloaded"
                })
                savedConfig = json.loads(config.read_text())
                self.assertEqual(savedConfig["apps"]["repos"], [],
                                 "failed reload must preserve the accepted native repository write")
                self.assertEqual(nodeEvents[-1],
                                 ("/_internal/reload-config", secret, None))
                calls = removalCalls()
                self.assertEqual([call["uri"] for call in calls], [
                    "com.webos.appInstallService/remove",
                    "com.webos.appInstallService/remove",
                    "com.webos.appInstallService/dev/remove",
                    "com.webos.appInstallService/remove",
                    "com.webos.appInstallService/remove"
                ])
                self.assertEqual([call["payload"]["id"] for call in calls], [
                    "store.app", "dev.app", "dev.app", "public.app",
                    "custom.screensaver"
                ])
                lifecycle = lifecycleCalls()
                closeCalls = [call for call in lifecycle
                              if call["uri"] == "com.webos.applicationManager/closeByAppId"]
                pollCalls = [call for call in lifecycle
                             if call["uri"] == "com.webos.applicationManager/listApps"]
                self.assertEqual([call["payload"]["id"] for call in closeCalls],
                                 ["store.app", "dev.app", "public.app", "custom.screensaver"])
                self.assertEqual(len(pollCalls), 4,
                                 "native uninstall must poll listApps after each accepted removal")

                process.send_signal(signal.SIGTERM)
                output, _ = process.communicate(timeout=6)
                self.assertEqual(process.returncode, 0, output)
                self.assertIn("command-policy: action=apps.uninstall.remove result=success", output)
                self.assertIn("command-policy: action=apps.uninstall.dev-remove result=success", output)
                self.assertIn("command-policy: action=apps.uninstall.close result=success", output)
                self.assertIn("command-policy: action=apps.uninstall.list-apps result=success", output)
            finally:
                if process.poll() is None:
                    process.send_signal(signal.SIGTERM)
                    try:
                        process.communicate(timeout=6)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait(timeout=3)
                nodeServer.shutdown()
                nodeServer.server_close()
                nodeThread.join(timeout=3)

    def test_native_sam_rescan_internal_bridge(self):
        binary = Path(sys.argv[1]).resolve()
        fakeLuna = binary.parent / "native-fake-luna-send"
        publicPort = unused_port_pair()
        nodePort = unused_port_pair((publicPort, publicPort + 1))
        secret = "sam-proxy-secret-0123456789abcdef"
        token = "sam-route-token"
        with tempfile.TemporaryDirectory(prefix="nativetv-native-sam-") as folder:
            root = Path(folder)
            config = root / "config.json"
            heartbeat = root / "tvweb.beat"
            assets = root / "assets"
            systemRoot = root / "system"
            schema = systemRoot / "etc/palm/schemas/sam/applicationManager.blockSystemApp.schema"
            details = root / "luna-details.jsonl"
            restartLog = root / "sam-restart.log"
            assets.mkdir()
            schema.parent.mkdir(parents=True)
            schema.write_text('{"properties":{"forceBlockSystemApp":{"type":"boolean"}}}')
            shell = systemRoot / "bin/sh"
            shell.parent.mkdir(parents=True)
            shell.write_text("#!/bin/sh\nprintf '%s\\n' \"$*\" >> '" +
                             str(restartLog) + "'\nexit 0\n")
            shell.chmod(0o755)
            config.write_text(json.dumps({
                "host": "127.0.0.1", "port": publicPort, "token": token,
                "allowControl": True
            }))
            environment = os.environ.copy()
            environment["TVWEB_LUNA_SEND"] = str(fakeLuna)
            environment["TVWEB_DATA_DIR"] = str(root / "data")
            environment["TVWEB_PROXY_SECRET"] = secret
            environment["NATIVETV_FAKE_LUNA_MODE"] = "sam-rescan"
            environment["NATIVETV_FAKE_LUNA_DETAILS"] = str(details)
            process = subprocess.Popen(
                [str(binary), "--config", str(config), "--heartbeat", str(heartbeat),
                 "--workers", "4", "--listen", "--node-port", str(nodePort),
                 "--assets-root", str(assets), "--system-root", str(systemRoot)],
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                env=environment)

            def request(command, withSecret=True, originalRemote=None):
                data = json.dumps(command).encode("utf-8")
                headers = {"Content-Type": "application/json"}
                if withSecret:
                    headers["X-NativeTv-Proxy-Secret"] = secret
                if originalRemote is not None:
                    headers["X-NativeTv-Original-Remote"] = originalRemote
                req = Request("http://127.0.0.1:%d/_internal/sam-rescan" % publicPort,
                              data=data, method="POST", headers=headers)
                try:
                    with urlopen(req, timeout=65) as response:
                        return response.status, response.read()
                except HTTPError as error:
                    return error.code, error.read()

            def recordedCalls():
                return details.read_text().splitlines() if details.exists() else []

            def recordedSamCalls():
                routes = {
                    "com.webos.service.config/getConfigs",
                    "com.webos.service.config/setConfigs",
                    "com.webos.applicationManager/blockSystemApp",
                    "com.webos.applicationManager/releaseBlockSystemApp"
                }
                return [line for line in recordedCalls()
                        if json.loads(line)["uri"] in routes]

            try:
                deadline = time.monotonic() + 8
                ready = False
                while time.monotonic() < deadline and process.poll() is None:
                    req = Request("http://127.0.0.1:%d/api/caps" % publicPort,
                                  headers={"Authorization": "Bearer " + token})
                    try:
                        with urlopen(req, timeout=2):
                            ready = True
                            break
                    except (OSError, URLError):
                        time.sleep(0.03)
                if not ready:
                    if process.poll() is None:
                        process.send_signal(signal.SIGTERM)
                    output, _ = process.communicate(timeout=6)
                    self.fail("native daemon did not start:\n" + output[-4000:])

                beforeUnauthorized = recordedSamCalls()
                getList = {
                    "uri": "com.webos.service.config/getConfigs",
                    "payload": {"configNames": ["profile.blockedAppList"]}
                }
                status, body = request(getList, withSecret=False)
                self.assertEqual(status, 403, body.decode("utf-8", "replace"))
                afterUnauthorized = recordedSamCalls()
                self.assertEqual(afterUnauthorized, beforeUnauthorized,
                                 "missing secret must not call Luna")
                status, body = request(getList, originalRemote="203.0.113.20")
                self.assertEqual(status, 403, body.decode("utf-8", "replace"))
                self.assertEqual(recordedSamCalls(), beforeUnauthorized,
                                 "proxied remote requests must not use the internal route")

                status, body = request(getList)
                self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                self.assertEqual(json.loads(body), {
                    "ok": True,
                    "response": {
                        "returnValue": True,
                        "configs": {"profile.blockedAppList": [
                            "com.webos.app.roomconnect", "netflix"]}
                    }
                })

                writeList = {
                    "uri": "com.webos.service.config/setConfigs",
                    "payload": {"configs": {"profile.blockedAppList": ["netflix", "youtube"]}}
                }
                status, body = request(writeList)
                self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                self.assertTrue(json.loads(body)["response"]["returnValue"])

                block = {
                    "uri": "com.webos.applicationManager/blockSystemApp",
                    "payload": {"id": "com.webos.app.screensaver"}
                }
                status, body = request(block)
                self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                self.assertTrue(json.loads(body)["response"]["returnValue"])
                release = {
                    "uri": "com.webos.applicationManager/releaseBlockSystemApp",
                    "payload": {"id": "com.webos.app.home"}
                }
                status, body = request(release)
                self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                self.assertTrue(json.loads(body)["response"]["returnValue"])

                before = len(recordedCalls())
                invalidApp = {
                    "uri": "com.webos.applicationManager/blockSystemApp",
                    "payload": {"id": "com.example.unrelated"}
                }
                status, _ = request(invalidApp)
                self.assertEqual(status, 400)
                invalidUri = {"uri": "com.webos.applicationManager/launch",
                              "payload": {"id": "com.webos.app.home"}}
                status, _ = request(invalidUri)
                self.assertEqual(status, 400)
                self.assertEqual(len(recordedCalls()), before,
                                 "unsupported app IDs and methods must be rejected before Luna")

                calls = [json.loads(line) for line in recordedCalls()]
                calls = [call for call in calls if call["uri"] in {
                    "com.webos.service.config/getConfigs",
                    "com.webos.service.config/setConfigs",
                    "com.webos.applicationManager/blockSystemApp",
                    "com.webos.applicationManager/releaseBlockSystemApp"}]
                self.assertEqual([call["uri"] for call in calls], [
                    "com.webos.service.config/getConfigs",
                    "com.webos.service.config/setConfigs",
                    "com.webos.applicationManager/blockSystemApp",
                    "com.webos.applicationManager/releaseBlockSystemApp"])
                self.assertEqual(calls[1]["payload"], writeList["payload"])
                self.assertEqual(calls[2]["payload"], {
                    "id": "com.webos.app.screensaver", "forceBlockSystemApp": False})
                self.assertEqual(calls[3]["payload"], {"id": "com.webos.app.home"})

                operationStart = len(recordedCalls())
                for operation in ({"action": "canNudge"}, {"action": "nudge"},
                                  {"action": "rereadApp", "id": "com.webos.app.home"}):
                    status, body = request(operation)
                    self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                    self.assertEqual(json.loads(body)["result"], True,
                                     "native SAM operation failed: %r" % operation)
                operationCalls = [json.loads(line) for line in recordedCalls()[operationStart:]]
                self.assertEqual([call["uri"] for call in operationCalls], [
                    "com.webos.service.config/getConfigs",
                    "com.webos.service.config/getConfigs",
                    "com.webos.service.config/setConfigs",
                    "com.webos.service.config/setConfigs",
                    "com.webos.applicationManager/blockSystemApp",
                    "com.webos.applicationManager/releaseBlockSystemApp"])
                self.assertEqual(operationCalls[2]["payload"]["configs"][
                    "profile.blockedAppList"][-1],
                    "io.github.ihtisham1211.nativetv.rescan")
                self.assertEqual(operationCalls[3]["payload"]["configs"][
                    "profile.blockedAppList"], ["com.webos.app.roomconnect", "netflix"])

                refreshStart = len(recordedCalls())
                status, body = request({"action": "refresh", "expected": {"netflix": False}})
                self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                self.assertEqual(json.loads(body)["result"], True,
                                 body.decode("utf-8", "replace"))
                refreshCalls = [json.loads(line) for line in recordedCalls()[refreshStart:]]
                self.assertEqual([call["uri"] for call in refreshCalls], [
                    "com.webos.service.config/getConfigs",
                    "com.webos.service.config/setConfigs",
                    "com.webos.service.config/setConfigs",
                    "com.webos.applicationManager/getAppInfo",
                    "com.webos.applicationManager/listLaunchPoints",
                    "com.webos.applicationManager/listLaunchPoints",
                    "com.webos.applicationManager/listLaunchPoints"
                ])
                self.assertEqual(refreshCalls[3]["payload"], {"id": "netflix"})
                self.assertFalse(restartLog.exists(),
                                 "a successful nudge must not restart SAM")

                restartStart = len(recordedCalls())
                status, body = request({"action": "restart"})
                self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                self.assertEqual(json.loads(body)["result"], True,
                                 body.decode("utf-8", "replace"))
                restartCalls = [json.loads(line) for line in recordedCalls()[restartStart:]]
                restartUris = [call["uri"] for call in restartCalls]
                self.assertEqual(restartUris, [
                    "com.webos.applicationManager/getForegroundAppInfo",
                    "com.webos.applicationManager/listLaunchPoints",
                    "com.webos.applicationManager/listLaunchPoints",
                    "com.webos.applicationManager/listLaunchPoints",
                    "com.webos.applicationManager/getForegroundAppInfo",
                    "com.webos.applicationManager/launch"
                ])
                self.assertEqual(restartCalls[-1]["payload"], {
                    "id": "com.webos.app.hdmi2"
                })
                scripts = restartLog.read_text().splitlines()
                self.assertEqual(len(scripts), 2, "restart and dependent recovery both run")
                self.assertIn("systemctl reset-failed", scripts[0])
                self.assertIn("systemctl kill -s 9 sam.service", scripts[0])
                self.assertIn("initctl restart sam", scripts[0])
                self.assertIn("systemctl start --no-block", scripts[1])
                self.assertIn("initctl restart eim", scripts[1])

                process.send_signal(signal.SIGTERM)
                output, _ = process.communicate(timeout=6)
                self.assertEqual(process.returncode, 0, output)
                self.assertIn("command-policy: action=sam-rescan.get-blocked-list result=success", output)
                self.assertIn("command-policy: action=sam-rescan.set-blocked-list result=success", output)
                self.assertIn("command-policy: action=sam-rescan.block-app result=success", output)
                self.assertIn("command-policy: action=sam-rescan.release-app result=success", output)
                self.assertIn("command-policy: action=sam-rescan.check-app-visibility result=success", output)
                self.assertIn("command-policy: action=sam-rescan.wait-for-launch-points result=success", output)
                self.assertIn("command-policy: action=sam-rescan.restart result=success", output)
            finally:
                if process.poll() is None:
                    process.send_signal(signal.SIGTERM)
                    try:
                        process.communicate(timeout=6)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait(timeout=3)

    def test_native_home_takeover_lifecycle(self):
        binary = Path(sys.argv[1]).resolve()
        fakeLuna = binary.parent / "native-fake-luna-send"
        publicPort = unused_port_pair()
        nodePort = unused_port_pair((publicPort, publicPort + 1))
        secret = "home-takeover-stage-secret-0123456789abcdef"
        token = "home-takeover-stage-route-token"
        with tempfile.TemporaryDirectory(prefix="nativetv-home-takeover-") as folder:
            root = Path(folder)
            dataRoot = root / "data"
            config = root / "config.json"
            assets = root / "assets"
            systemRoot = root / "system"
            procRoot = root / "proc"
            source = assets / "home-app"
            sourceAssets = source / "assets"
            sourceAssets.mkdir(parents=True)
            systemBin = systemRoot / "bin"
            systemBin.mkdir(parents=True, exist_ok=True)
            (systemBin / "sh").symlink_to("/bin/sh")
            earlyHomeFlag = systemRoot / "var/lib/tvweb/early-home"
            earlyHomeFlag.parent.mkdir(parents=True)
            earlyManifest = (systemRoot /
                             "var/luna-service2-dev/manifests.d/"
                             "com.webos.nativetv.boot.json")
            earlyServiceDir = assets / "early-service"
            earlyServiceDir.mkdir(parents=True)
            earlyServiceLog = root / "early-service.log"
            (earlyServiceDir / "install.sh").write_text(
                "#!/bin/sh\n"
                "printf '%s\\n' \"$1\" >> \"" + str(earlyServiceLog) + "\"\n"
                "if [ \"$1\" = install ]; then\n"
                "  mkdir -p \"" + str(earlyManifest.parent) + "\"\n"
                "  : > \"" + str(earlyManifest) + "\"\n"
                "else\n"
                "  rm -f \"" + str(earlyManifest) + "\"\n"
                "fi\n")
            (source / "index.html").write_text(
                "<html><script>var PORT = 8080; // tvweb:port\n"
                "var BASE = 'http://localhost:' + PORT;</script></html>\n")
            (source / "appinfo.json").write_text('{"id":"must-not-be-staged"}\n')
            (sourceAssets / "icon80.png").write_bytes(b"icon-80")
            (sourceAssets / "icon130.png").write_bytes(b"icon-130")
            originalHome = root / "lg-home"
            originalHome.mkdir()
            (originalHome / "appinfo.json").write_text('{"id":"com.webos.app.home","type":"native"}\n')
            layoutDir = systemRoot / "mnt/lg/cmn_data/manifest"
            layoutDir.mkdir(parents=True)
            etc = systemRoot / "etc"
            etc.mkdir()
            (etc / "issue").write_text("webOS TV 11.2.0\n")
            samSchema = etc / "palm/schemas/sam/applicationManager.blockSystemApp.schema"
            samSchema.parent.mkdir(parents=True)
            samSchema.write_text('{"properties":{"forceBlockSystemApp":{"type":"boolean"}}}\n')
            originalLayout = '<home><!-- original LG layout --></home>\n'
            takeoverLayout = '<home><!-- nativetv-home-starting --></home>\n'
            (layoutDir / "home_layoutShelfView.xml").write_text(originalLayout)
            (layoutDir / "home_layout.xml").write_text(originalLayout)
            fakeHomePid = 2000000000
            fakeProcHome = procRoot / str(fakeHomePid)
            fakeProcHome.mkdir(parents=True)
            (fakeProcHome / "cmdline").write_bytes(b"/usr/bin/com.webos.app.home\0")
            appDirectory = systemRoot / "usr/palm/applications/com.webos.app.home"
            appDirectory.parent.mkdir(parents=True)
            appDirectory.symlink_to(originalHome, target_is_directory=True)
            systemBin = systemRoot / "bin"
            systemBin.mkdir(parents=True, exist_ok=True)
            mountLog = root / "mount.log"
            mountedMarker = root / "home-mounted"
            samHomeType = root / "sam-home-type"
            samRefuse = root / "sam-refuse"
            samHomeType.write_text("native\n")
            mount = systemBin / "mount"
            mount.write_text(
                "#!/bin/sh\n"
                "printf '%s\\n' \"$*\" >> \"" + str(mountLog) + "\"\n"
                "[ \"$1\" = \"--bind\" ] || exit 2\n"
                "rm -f \"$3\" && ln -s \"$2\" \"$3\" && touch \"" +
                    str(mountedMarker) + "\"\n")
            mount.chmod(0o755)
            umountLog = root / "umount.log"
            umount = systemBin / "umount"
            umount.write_text(
                "#!/bin/sh\n"
                "printf '%s\\n' \"$*\" >> \"" + str(umountLog) + "\"\n"
                "if [ \"$1\" != \"-l\" ] && [ -f \"" +
                    str(root / "force-lazy-unmount") + "\" ]; then exit 1; fi\n"
                "target=\"$1\"\n"
                "[ \"$1\" != \"-l\" ] || target=\"$2\"\n"
                "rm -f \"$target\" && ln -s \"" + str(originalHome) +
                    "\" \"$target\" && rm -f \"" + str(mountedMarker) + "\"\n")
            umount.chmod(0o755)
            probeClient = probe_stand_in(root)
            config.write_text(json.dumps({
                "host": "127.0.0.1", "port": publicPort, "token": token,
                "allowControl": True, "allowExperimentalHome": True,
                "nativeClient": {"path": str(probeClient)}
            }))
            environment = os.environ.copy()
            environment["TVWEB_LUNA_SEND"] = str(fakeLuna)
            environment["NATIVETV_FAKE_LUNA_MODE"] = "home-takeover"
            environment["NATIVETV_FAKE_HOME_TAKEOVER_TYPE"] = str(samHomeType)
            environment["NATIVETV_FAKE_HOME_TAKEOVER_MOUNTED"] = str(mountedMarker)
            environment["NATIVETV_FAKE_HOME_TAKEOVER_REFUSE"] = str(samRefuse)
            environment["NATIVETV_FAKE_LUNA_DETAILS"] = str(root / "luna-details.jsonl")
            environment["TVWEB_DATA_DIR"] = str(dataRoot)
            environment["TVWEB_PROXY_SECRET"] = secret
            process = subprocess.Popen(
                [str(binary), "--config", str(config), "--heartbeat",
                 str(root / "tvweb.beat"), "--workers", "4", "--listen",
                 "--node-port", str(nodePort), "--assets-root", str(assets),
                 "--proc-root", str(procRoot), "--system-root",
                 str(systemRoot)],
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                env=environment)

            def request(action="takeover", withSecret=True, fields=None):
                headers = {"Content-Type": "application/json"}
                if withSecret:
                    headers["X-NativeTv-Proxy-Secret"] = secret
                payload = {"action": action}
                if action == "takeover":
                    payload["blank"] = False
                if fields:
                    payload.update(fields)
                req = Request(
                    "http://127.0.0.1:%d/_internal/home-screen" % publicPort,
                    data=json.dumps(payload).encode("utf-8"), method="POST",
                    headers=headers)
                try:
                    with urlopen(req, timeout=12) as response:
                        return response.status, json.loads(response.read())
                except HTTPError as error:
                    return error.code, error.read().decode("utf-8", "replace")

            try:
                deadline = time.monotonic() + 8
                ready = False
                while time.monotonic() < deadline and process.poll() is None:
                    req = Request("http://127.0.0.1:%d/api/caps" % publicPort,
                                  headers={"Authorization": "Bearer " + token})
                    try:
                        with urlopen(req, timeout=2):
                            ready = True
                            break
                    except (OSError, URLError):
                        time.sleep(0.03)
                if not ready:
                    if process.poll() is None:
                        process.send_signal(signal.SIGTERM)
                    output, _ = process.communicate(timeout=6)
                    self.fail("native daemon did not start:\n" + output[-4000:])

                status, body = request(withSecret=False)
                self.assertEqual(status, 403, body)
                staged = root / "data/home-takeover"
                self.assertFalse(staged.exists(), "unauthorized requests must not stage files")
                self.assertFalse(mountLog.exists(), "unauthorized requests must not mount LG Home")
                status, body = request("setStartLayout")
                self.assertEqual(status, 400, body)

                status, body = request()
                self.assertEqual(status, 200, body)
                self.assertEqual(body["ok"], True)
                self.assertEqual(body["result"], {
                    "ok": True, "changed": True, "mounted": True, "already": False
                })
                self.assertEqual(samHomeType.read_text().strip(), "web")
                self.assertTrue(mountedMarker.exists())
                samCalls = [json.loads(line) for line in
                            (root / "luna-details.jsonl").read_text().splitlines()]
                blockCalls = [call for call in samCalls
                              if call["uri"] == "com.webos.applicationManager/blockSystemApp"]
                releaseCalls = [call for call in samCalls
                                if call["uri"] == "com.webos.applicationManager/releaseBlockSystemApp"]
                self.assertTrue(blockCalls, "native takeover must reread Home through SAM")
                self.assertFalse(blockCalls[0]["payload"]["forceBlockSystemApp"])
                self.assertTrue(releaseCalls, "native takeover must release Home after the SAM block")
                status, body = request("stopLgHome")
                self.assertEqual(status, 200, body)
                self.assertEqual(body["result"], {"ok": True, "stopped": True})
                self.assertEqual((staged / "index.html").read_text(),
                    "<html><script>var PORT = %d; // tvweb:port\n"
                    "var BASE = 'http://localhost:' + PORT;</script></html>\n"
                    % publicPort)
                self.assertEqual((staged / "assets/icon80.png").read_bytes(), b"icon-80")
                appInfo = json.loads((staged / "appinfo.json").read_text())
                self.assertEqual(appInfo, {
                    "id": "com.webos.app.home", "version": "1.0.0",
                    "vendor": "tvweb", "type": "web", "main": "index.html",
                    "title": "Home", "icon": "assets/icon80.png",
                    "largeIcon": "assets/icon130.png", "iconColor": "#000000",
                    "bgColor": "#000000", "resolution": "1920x1080",
                    "visible": False, "disableBackHistoryAPI": True,
                    "appCategory": ["home"]
                })
                self.assertEqual(appDirectory.resolve(), staged.resolve())

                status, body = request()
                self.assertEqual(status, 200, body)
                self.assertEqual(body["result"], {
                    "ok": True, "changed": False, "mounted": True, "already": True
                })
                self.assertEqual(len(mountLog.read_text().splitlines()), 1,
                                 "an existing takeover must not mount twice")
                (source / "index.html").write_text(
                    "<html><script>var PORT = 8080; // tvweb:port\n"
                    "var BASE = 'http://localhost:' + PORT;</script><p>v2</p></html>\n")
                status, body = request()
                self.assertEqual(status, 200, body)
                self.assertEqual(body["result"], {
                    "ok": True, "changed": True, "mounted": True, "already": True
                })
                self.assertEqual((staged / "index.html").read_text(),
                    "<html><script>var PORT = %d; // tvweb:port\n"
                    "var BASE = 'http://localhost:' + PORT;</script><p>v2</p></html>\n"
                    % publicPort)

                keptLayout = root / "data/home-takeover-lg-layout"
                keptLayout.mkdir()
                (keptLayout / "home_layoutShelfView.xml").write_text(originalLayout)
                (keptLayout / "home_layout.xml.none").write_text("")
                (layoutDir / "home_layoutShelfView.xml").write_text(takeoverLayout)
                (layoutDir / "home_layout.xml").write_text(takeoverLayout)

                status, body = request("releaseTakeover")
                self.assertEqual(status, 200, body)
                self.assertEqual(body["result"], {
                    "ok": True, "changed": False, "mounted": False, "already": False
                })
                self.assertEqual(samHomeType.read_text().strip(), "native")
                self.assertFalse(mountedMarker.exists())
                self.assertEqual(appDirectory.resolve(), originalHome.resolve())
                self.assertEqual((layoutDir / "home_layoutShelfView.xml").read_text(),
                                 originalLayout)
                self.assertFalse((layoutDir / "home_layout.xml").exists())
                self.assertFalse(keptLayout.exists())
                self.assertEqual(len(umountLog.read_text().splitlines()), 1,
                                 "the normal unmount should work first")
                status, body = request("setStartLayout", fields={"blank": True})
                self.assertEqual(status, 200, body)
                self.assertEqual(body["result"], {
                    "ok": True, "changed": True, "blank": True
                })
                self.assertIn(
                    "<!-- nativetv-home-starting -->",
                    (layoutDir / "home_layoutShelfView.xml").read_text())
                self.assertEqual((keptLayout / "home_layoutShelfView.xml").read_text(),
                                 originalLayout)
                status, body = request("setStartLayout", fields={"blank": False})
                self.assertEqual(status, 200, body)
                self.assertEqual(body["result"], {
                    "ok": True, "changed": True, "blank": False
                })
                self.assertEqual((layoutDir / "home_layoutShelfView.xml").read_text(),
                                 originalLayout)
                self.assertFalse(keptLayout.exists())
                status, body = request("takeover")
                self.assertEqual(status, 200, body)
                self.assertEqual(body["result"], {
                    "ok": True, "changed": False, "mounted": True, "already": False
                })
                (root / "force-lazy-unmount").touch()
                status, body = request("releaseTakeover")
                self.assertEqual(status, 200, body)
                self.assertEqual(body["result"], {
                    "ok": True, "changed": False, "mounted": False, "already": False
                })
                self.assertEqual(appDirectory.resolve(), originalHome.resolve())
                umountCalls = umountLog.read_text().splitlines()
                self.assertEqual(len(umountCalls), 3,
                                 "a failed normal unmount should fall back to lazy unmount")
                self.assertTrue(umountCalls[-1].startswith("-l "), umountCalls)
                status, body = request("releaseTakeover")
                self.assertEqual(status, 200, body)
                self.assertEqual(body["result"], {
                    "ok": True, "changed": False, "mounted": False, "already": True
                })
                self.assertEqual(len(umountLog.read_text().splitlines()), 3,
                                 "a released takeover must not unmount twice")

                (root / "force-lazy-unmount").unlink()
                samRefuse.touch()
                status, body = request("takeover")
                self.assertEqual(status, 200, body)
                self.assertEqual(body["result"]["ok"], False, body)
                self.assertIn("sam did not take", body["result"]["error"])
                self.assertFalse(mountedMarker.exists())
                self.assertEqual(appDirectory.resolve(), originalHome.resolve())
                self.assertEqual(samHomeType.read_text().strip(), "native")
                self.assertEqual(len(mountLog.read_text().splitlines()), 3)
                self.assertEqual(len(umountLog.read_text().splitlines()), 4,
                                 "a SAM refusal must roll back the native mount")

                (dataRoot / "home.json").write_text(json.dumps({
                    "enabled": False, "restorePending": False
                }))
                if samRefuse.exists():
                    samRefuse.unlink()

                def postHome(path):
                    req = Request(
                        "http://127.0.0.1:%d%s" % (publicPort, path),
                        data=b"{}", method="POST",
                        headers={"Authorization": "Bearer " + token,
                                 "Content-Type": "application/json"})
                    try:
                        with urlopen(req, timeout=12) as response:
                            return response.status, json.loads(response.read())
                    except HTTPError as error:
                        return error.code, json.loads(error.read())

                status, response = postHome("/api/home/enable")
                self.assertEqual(status, 200, response)
                self.assertEqual(response, {"ok": True})
                self.assertTrue(json.loads((dataRoot / "home.json").read_text())["enabled"])
                self.assertTrue(mountedMarker.exists(),
                                "native enable must complete the managed takeover")
                self.assertIn("starting", json.loads(
                    (dataRoot / "remotekeys.json").read_text()))
                self.assertTrue(earlyHomeFlag.is_file())
                self.assertTrue(earlyManifest.is_file())
                self.assertEqual(earlyServiceLog.read_text().splitlines(), ["install"])

                status, response = postHome("/api/home/disable")
                self.assertEqual(status, 200, response)
                self.assertEqual(response, {"ok": True})
                disabledHome = json.loads((dataRoot / "home.json").read_text())
                self.assertFalse(disabledHome["enabled"])
                self.assertFalse(disabledHome["restorePending"])
                self.assertFalse(mountedMarker.exists(),
                                 "native disable must release LG Home")
                self.assertEqual(appDirectory.resolve(), originalHome.resolve())
                self.assertNotIn("starting", json.loads(
                    (dataRoot / "remotekeys.json").read_text()))
                self.assertFalse(earlyHomeFlag.exists())
                self.assertFalse(earlyManifest.exists())
                self.assertEqual(earlyServiceLog.read_text().splitlines(),
                                 ["install", "remove"])

                process.send_signal(signal.SIGTERM)
                output, _ = process.communicate(timeout=6)
                self.assertEqual(process.returncode, 0, output)
            finally:
                if process.poll() is None:
                    process.send_signal(signal.SIGTERM)
                    try:
                        process.communicate(timeout=6)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait(timeout=3)

            dataRoot = root / "data"
            dataRoot.mkdir(exist_ok=True)
            (dataRoot / "home.json").write_text(json.dumps({
                "enabled": True, "hideLgHomeAtStart": True
            }))
            if samRefuse.exists():
                samRefuse.unlink()

            def run_home_operation(operation):
                return subprocess.run(
                    [str(binary), operation, "--config", str(config),
                     "--assets-root", str(assets), "--proc-root", str(procRoot),
                     "--system-root", str(systemRoot)],
                    capture_output=True, text=True, env=environment, timeout=30)

            takeover = run_home_operation("--takeover-home")
            self.assertEqual(takeover.returncode, 0, takeover.stdout + takeover.stderr)
            self.assertTrue(mountedMarker.exists(),
                            "the boot entry point should use the native lifecycle")
            self.assertEqual(appDirectory.resolve(),
                             (dataRoot / "home-takeover").resolve())
            self.assertEqual(samHomeType.read_text().strip(), "web")
            webos11Layout = systemRoot / "mnt/lg/cmn_data/manifest/home_layoutShelfView.xml"
            self.assertEqual(
                webos11Layout.read_text(), originalLayout,
                "the webOS 11 takeover must leave LG's layout unchanged")
            self.assertNotIn("nativetv-home-starting", webos11Layout.read_text())

            release = run_home_operation("--release-home")
            self.assertEqual(release.returncode, 0, release.stdout + release.stderr)
            self.assertFalse(mountedMarker.exists())
            self.assertEqual(appDirectory.resolve(), originalHome.resolve())
            self.assertEqual(samHomeType.read_text().strip(), "native")

            config.write_text(json.dumps({
                "host": "127.0.0.1", "port": publicPort, "token": token,
                "allowControl": True, "allowExperimentalHome": False,
                "nativeClient": {"path": str(probeClient)}
            }))
            mountCount = len(mountLog.read_text().splitlines())
            unsupported = run_home_operation("--takeover-home")
            self.assertEqual(unsupported.returncode, 4,
                             unsupported.stdout + unsupported.stderr)
            self.assertEqual(len(mountLog.read_text().splitlines()), mountCount,
                             "unsupported webOS 11 must skip before mounting")
            self.assertFalse(mountedMarker.exists())

            (etc / "issue").write_text("webOS TV 9.2.2\n")
            (dataRoot / "home.json").write_text(json.dumps({
                "enabled": True, "hideLgHomeAtStart": True
            }))
            takeover9 = run_home_operation("--takeover-home")
            self.assertEqual(takeover9.returncode, 0,
                             takeover9.stdout + takeover9.stderr)
            self.assertIn(
                "nativetv-home-starting",
                (layoutDir / "home_layoutShelfView.xml").read_text(),
                "webOS 9 must apply the saved blank-start option")
            self.assertEqual(
                (dataRoot / "home-takeover-lg-layout/home_layoutShelfView.xml").read_text(),
                originalLayout,
                "webOS 9 must save LG's layout before blanking it")
            release9 = run_home_operation("--release-home")
            self.assertEqual(release9.returncode, 0,
                             release9.stdout + release9.stderr)
            self.assertEqual(
                (layoutDir / "home_layoutShelfView.xml").read_text(), originalLayout,
                "webOS 9 must restore LG's original layout on release")
            self.assertFalse((dataRoot / "home-takeover-lg-layout").exists())

            (dataRoot / "home.json").write_text(json.dumps({"enabled": False}))
            mountCount = len(mountLog.read_text().splitlines())
            disabled = run_home_operation("--takeover-home")
            self.assertEqual(disabled.returncode, 3,
                             disabled.stdout + disabled.stderr)
            self.assertEqual(len(mountLog.read_text().splitlines()), mountCount,
                             "disabled Home must skip without mounting")
            self.assertFalse(mountedMarker.exists())

    def test_native_nativetv_home_restore(self):
        binary = Path(sys.argv[1]).resolve()
        fakeLuna = binary.parent / "native-fake-luna-send"
        with tempfile.TemporaryDirectory(prefix="nativetv-home-restore-") as folder:
            root = Path(folder)
            config = root / "config.json"
            dataRoot = root / "data"
            systemRoot = root / "system"
            assets = root / "assets"
            procRoot = root / "proc"
            details = root / "luna-details.jsonl"
            dataRoot.mkdir()
            assets.mkdir()
            (systemRoot / "etc").mkdir(parents=True)
            (systemRoot / "etc/issue").write_text("webOS TV 9.2.2\n")
            config.write_text(json.dumps({
                "host": "127.0.0.1", "port": 8080,
                "allowControl": False, "allowPower": False
            }))
            home = dataRoot / "home.json"
            home.write_text(json.dumps({
                "enabled": True, "restorePending": False,
                "lgAutoLaunch": "on", "lgAutoLaunchCheck": "user",
                "snapshot": {"settings": {"screenSaverEnabled": False}}
            }))
            remote = dataRoot / "remotekeys.json"
            remote.write_text(json.dumps({
                "keys": {"guide": "com.example.guide"},
                "home": "com.webos.app.home", "starting": "Home is starting",
                "splash": True, "updated": "2026-10-28T00:00:00.000Z"
            }))
            environment = os.environ.copy()
            environment["TVWEB_LUNA_SEND"] = str(fakeLuna)
            environment["TVWEB_DATA_DIR"] = str(dataRoot)
            environment["NATIVETV_FAKE_LUNA_MODE"] = "home-restore"
            environment["NATIVETV_FAKE_LUNA_DETAILS"] = str(details)
            environment["NATIVETV_FAKE_HOME_RESTORE_SETTINGS"] = "{}"

            command = [str(binary), "--restore-nativetv-home", "--config",
                       str(config), "--assets-root", str(assets), "--proc-root",
                       str(procRoot), "--system-root", str(systemRoot)]
            result = subprocess.run(command, capture_output=True, text=True,
                                    env=environment, timeout=30)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            restored = json.loads(home.read_text())
            self.assertFalse(restored["enabled"])
            self.assertFalse(restored["restorePending"])
            self.assertIsNone(restored["lgAutoLaunch"])
            self.assertIsNone(restored["lgAutoLaunchCheck"])
            self.assertIsNone(restored["snapshot"])

            remoteState = json.loads(remote.read_text())
            self.assertEqual(remoteState["keys"], {"guide": "com.example.guide"})
            self.assertEqual(remoteState["home"], "com.webos.app.home")
            self.assertNotIn("starting", remoteState)
            self.assertNotIn("splash", remoteState)
            self.assertFalse(Path(str(remote) + ".last-good").exists(),
                             "the Node-owned map must not retain a recovery copy that can resurrect its old notice")
            calls = [json.loads(line) for line in details.read_text().splitlines()]
            writes = [call["payload"]["settings"] for call in calls
                      if call["uri"] == "com.webos.service.settings/setSystemSettings"]
            self.assertIn({"homeAutoLaunch": "on",
                           "checkHomeAutoLaunchValue": "user"}, writes)
            self.assertIn({"screenSaverEnabled": False}, writes)
            mapWrites = [call["payload"]["configs"]["nativetv.remoteKeys"]
                         for call in calls
                         if call["uri"] == "com.webos.service.config/setConfigs" and
                         "nativetv.remoteKeys" in call["payload"]["configs"].keys()]
            self.assertEqual(mapWrites, [{"keys": {"guide": "com.example.guide"},
                                          "home": "com.webos.app.home"}])

            remote.write_text(json.dumps({
                "keys": {"guide": "com.example.guide"},
                "home": "com.tvweb.home", "starting": "Legacy Home",
                "splash": True, "updated": "2020-01-01T00:00:00.000Z"
            }))
            oldHome = subprocess.run(command, capture_output=True, text=True,
                                     env=environment, timeout=30)
            self.assertEqual(oldHome.returncode, 0,
                             oldHome.stdout + oldHome.stderr)
            oldHomeState = json.loads(remote.read_text())
            self.assertNotIn("home", oldHomeState)
            self.assertNotIn("starting", oldHomeState)
            self.assertNotIn("splash", oldHomeState)
            self.assertNotEqual(oldHomeState["updated"],
                                "2020-01-01T00:00:00.000Z")

            # Restore settings left by the old standalone Home app. Saved
            # values are reinstated; missing saved values remove defaultApps
            # keys and fall back to Live TV for input selections.
            home.write_text(json.dumps({
                "enabled": True, "restorePending": False,
                "snapshot": {"settings": {
                    "defaultApps": {"home": "com.example.saved-home"},
                    "lastInputApp": "com.example.saved-input"
                }}
            }))
            details.unlink()
            environment["NATIVETV_FAKE_HOME_RESTORE_SETTINGS"] = json.dumps({
                "defaultApps": {
                    "home": "com.tvweb.home", "idleApp": "com.tvweb.home",
                    "music": "com.example.music"
                },
                "lastInputApp": "com.tvweb.home",
                "physicalLastInputApp": "com.tvweb.home"
            })
            legacy = subprocess.run(command, capture_output=True, text=True,
                                    env=environment, timeout=30)
            self.assertEqual(legacy.returncode, 0, legacy.stdout + legacy.stderr)
            legacyHome = json.loads(home.read_text())
            self.assertFalse(legacyHome["enabled"])
            self.assertIsNone(legacyHome["snapshot"])
            calls = [json.loads(line) for line in details.read_text().splitlines()]
            legacyWrites = [call["payload"]["settings"] for call in calls
                            if call["uri"] == "com.webos.service.settings/setSystemSettings"]
            legacyRestore = next((write for write in legacyWrites
                                  if "defaultApps" in write), None)
            self.assertIsNotNone(legacyRestore)
            self.assertEqual(legacyRestore["defaultApps"], {
                "home": "com.example.saved-home", "music": "com.example.music"
            })
            self.assertEqual(legacyRestore["lastInputApp"],
                             "com.example.saved-input")
            self.assertEqual(legacyRestore["physicalLastInputApp"],
                             "com.webos.app.livetv")

    def test_native_home_screen_public_route(self):
        binary = Path(sys.argv[1]).resolve()
        fakeLuna = binary.parent / "native-fake-luna-send"
        publicPort = unused_port_pair()
        nodePort = unused_port_pair((publicPort, publicPort + 1))
        token = "home-screen-public-token"
        with tempfile.TemporaryDirectory(prefix="nativetv-native-home9-public-") as folder:
            root = Path(folder)
            config = root / "config.json"
            systemRoot = root / "system"
            procRoot = root / "proc"
            home = systemRoot / "usr/palm/applications/com.webos.app.home/qml"
            layoutDir = systemRoot / "mnt/lg/cmn_data/manifest"
            (root / "assets").mkdir()
            (systemRoot / "etc").mkdir(parents=True)
            (systemRoot / "etc/issue").write_text("webOS TV 9.2.2\n")
            (procRoot / "net").mkdir(parents=True)
            (home / "UserInterfaceLayer/Containers").mkdir(parents=True)
            (home / "UserInterfaceLayer/home_layoutShelfView.xml").write_text(
                '<?xml version="1.0"?>\n<home>\n'
                '<item type="Hero" id="hero"/>\n'
                '<item type="Qcard" id="cards"/>\n'
                '<item type="AppList" id="apps"/>\n'
                '<item type="AdShelf" id="devices"/>\n</home>\n')
            (home / "UserInterfaceLayer/Containers/MainView_M.qml").write_text(
                'property string path: "/mnt/lg/cmn_data/manifest/home_layout.xml"\n')
            config.write_text(json.dumps({
                "host": "127.0.0.1", "port": publicPort, "token": token,
                "allowControl": True, "mqtt": {"host": "keep.example"}
            }))
            environment = os.environ.copy()
            environment["TVWEB_LUNA_SEND"] = str(fakeLuna)
            environment["TVWEB_DATA_DIR"] = str(root / "data")
            environment["TVWEB_PROXY_SECRET"] = "home-screen-public-secret-0123456789"
            environment["NATIVETV_FAKE_LUNA_MODE"] = "home-screen"
            process = subprocess.Popen(
                [str(binary), "--config", str(config), "--heartbeat", str(root / "tvweb.beat"),
                 "--workers", "4", "--listen", "--node-port", str(nodePort),
                 "--assets-root", str(root / "assets"), "--proc-root", str(procRoot),
                 "--system-root", str(systemRoot)],
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                env=environment)

            def post(payload, raw=None):
                data = raw if raw is not None else json.dumps(payload).encode("utf-8")
                req = Request("http://127.0.0.1:%d/api/homescreen" % publicPort, data=data,
                              method="POST",
                              headers={"Content-Type": "application/json",
                                       "Authorization": "Bearer " + token})
                try:
                    with urlopen(req, timeout=15) as response:
                        return response.status, json.loads(response.read())
                except HTTPError as error:
                    return error.code, json.loads(error.read())

            try:
                deadline = time.monotonic() + 8
                while time.monotonic() < deadline and process.poll() is None:
                    try:
                        urlopen(Request("http://127.0.0.1:%d/api/caps" % publicPort), timeout=2)
                        break
                    except (OSError, URLError):
                        time.sleep(0.03)
                self.assertIsNone(process.poll(), "daemon exited early")

                self.assertEqual(post(None, b"{not json"), (400, {"ok": False, "error": "malformed JSON"}))
                self.assertEqual(post({}), (400, {
                    "ok": False, "error": "part, input or restore expected"}))
                self.assertEqual(post({"input": "hdmi9"}), (400, {
                    "ok": False, "error": "There is no such input"}))

                self.assertEqual(post({"part": "quickCards", "shown": False}), (200, {"ok": True}))
                layout = (layoutDir / "home_layoutShelfView.xml").read_text()
                self.assertIn("<!-- nativetv -->", layout)
                self.assertNotIn('type="Qcard"', layout)
                self.assertEqual(post({"part": "nonsense", "shown": True})[0], 400)

                # webOS 9's layout is not experimental: allowing it is a saved no-op.
                self.assertEqual(post({"allow": True}), (200, {"ok": True}))
                saved = json.loads(config.read_text())
                self.assertIs(saved["allowHomeLayout"], True)
                self.assertEqual(saved["mqtt"], {"host": "keep.example"})

                beat = Request("http://127.0.0.1:%d/api/home/beat" % publicPort, data=b"{}",
                               method="POST",
                               headers={"Content-Type": "application/json",
                                        "Authorization": "Bearer " + token})
                with urlopen(beat, timeout=5) as response:
                    pulse = json.loads(response.read())
                self.assertTrue(pulse["ok"])
                self.assertIs(pulse["enabled"], False)
                self.assertRegex(pulse["build"], r"^\d+\.\d+\.\d+(\+[0-9a-f]+(\.dirty)?)?@\d+$")
                self.assertIn("wallpapers", pulse)
                self.assertRegex(pulse["layoutVersion"], r"^\d+$")
                # The apps list carries the Home screen section: parts with their
                # words, and the HDMI inputs that are tiles on Home.
                listing = Request("http://127.0.0.1:%d/api/apps" % publicPort,
                                  headers={"Authorization": "Bearer " + token})
                with urlopen(listing, timeout=30) as response:
                    homeScreen = json.loads(response.read())["homeScreen"]
                self.assertEqual([p["id"] for p in homeScreen["parts"]],
                                 ["banner", "quickCards", "apps", "deviceFunctions"])
                self.assertEqual(homeScreen["parts"][1]["title"], "Quick Cards")
                self.assertFalse(homeScreen["parts"][1]["shown"])
                self.assertTrue(homeScreen["parts"][0]["shown"])
                self.assertIsNone(homeScreen["experimental"])
                self.assertEqual([i["id"] for i in homeScreen["inputs"]],
                                 ["hdmi1", "hdmi2", "hdmi3", "hdmi4"])
                self.assertEqual([i["onHome"] for i in homeScreen["inputs"]],
                                 [False, True, False, False])
                self.assertEqual(post({"restore": True}), (200, {"ok": True, "changed": True}))
                self.assertFalse((layoutDir / "home_layoutShelfView.xml").exists(),
                                 "restoring removes the layout this server wrote")
            finally:
                if process.poll() is None:
                    process.send_signal(signal.SIGTERM)
                    try:
                        process.communicate(timeout=6)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait(timeout=3)

    def test_native_home_screen_webos9_internal_bridge(self):
        binary = Path(sys.argv[1]).resolve()
        fakeLuna = binary.parent / "native-fake-luna-send"
        publicPort = unused_port_pair()
        nodePort = unused_port_pair((publicPort, publicPort + 1))
        secret = "home-screen-proxy-secret-0123456789abcdef"
        token = "home-screen-route-token"
        with tempfile.TemporaryDirectory(prefix="nativetv-native-home9-") as folder:
            root = Path(folder)
            config = root / "config.json"
            heartbeat = root / "tvweb.beat"
            assets = root / "assets"
            systemRoot = root / "system"
            procRoot = root / "proc"
            home = systemRoot / "usr/palm/applications/com.webos.app.home/qml"
            layoutDir = systemRoot / "mnt/lg/cmn_data/manifest"
            details = root / "luna-details.jsonl"
            assets.mkdir()
            (systemRoot / "etc").mkdir(parents=True)
            (systemRoot / "etc/issue").write_text("webOS TV 9.2.2\n")
            (procRoot / "net").mkdir(parents=True)
            (home / "UserInterfaceLayer/Containers").mkdir(parents=True)
            (home / "UserInterfaceLayer/home_layoutShelfView.xml").write_text(
                '<?xml version="1.0"?>\n<home>\n'
                '<item type="Hero" id="hero"/>\n'
                '<item type="Qcard" id="cards"/>\n'
                '<item type="AppList" id="apps"/>\n'
                '<item type="AdShelf" id="devices"/>\n</home>\n')
            (home / "UserInterfaceLayer/Containers/MainView_M.qml").write_text(
                'property string path: "/mnt/lg/cmn_data/manifest/home_layout.xml"\n')
            config.write_text(json.dumps({
                "host": "127.0.0.1", "port": publicPort, "token": token,
                "allowControl": True
            }))
            environment = os.environ.copy()
            environment["TVWEB_LUNA_SEND"] = str(fakeLuna)
            environment["TVWEB_DATA_DIR"] = str(root / "data")
            environment["TVWEB_PROXY_SECRET"] = secret
            environment["NATIVETV_FAKE_LUNA_MODE"] = "home-screen"
            environment["NATIVETV_FAKE_LUNA_DETAILS"] = str(details)
            process = subprocess.Popen(
                [str(binary), "--config", str(config), "--heartbeat", str(heartbeat),
                 "--workers", "4", "--listen", "--node-port", str(nodePort),
                 "--assets-root", str(assets), "--proc-root", str(procRoot),
                 "--system-root", str(systemRoot)],
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                env=environment)

            def request(command, withSecret=True, originalRemote=None):
                data = json.dumps(command).encode("utf-8")
                headers = {"Content-Type": "application/json"}
                if withSecret:
                    headers["X-NativeTv-Proxy-Secret"] = secret
                if originalRemote is not None:
                    headers["X-NativeTv-Original-Remote"] = originalRemote
                req = Request("http://127.0.0.1:%d/_internal/home-screen" % publicPort,
                              data=data, method="POST", headers=headers)
                try:
                    with urlopen(req, timeout=12) as response:
                        return response.status, response.read()
                except HTTPError as error:
                    return error.code, error.read()

            try:
                deadline = time.monotonic() + 8
                ready = False
                while time.monotonic() < deadline and process.poll() is None:
                    req = Request("http://127.0.0.1:%d/api/caps" % publicPort,
                                  headers={"Authorization": "Bearer " + token})
                    try:
                        with urlopen(req, timeout=2):
                            ready = True
                            break
                    except (OSError, URLError):
                        time.sleep(0.03)
                if not ready:
                    if process.poll() is None:
                        process.send_signal(signal.SIGTERM)
                    output, _ = process.communicate(timeout=6)
                    self.fail("native daemon did not start:\n" + output[-4000:])

                state = {"action": "state"}
                beforeUnauthorized = without_country_learning(
                    [json.loads(line) for line in details.read_text().splitlines()]
                    if details.exists() else [])
                status, body = request(state, withSecret=False)
                self.assertEqual(status, 403, body.decode("utf-8", "replace"))
                afterUnauthorized = without_country_learning(
                    [json.loads(line) for line in details.read_text().splitlines()]
                    if details.exists() else [])
                self.assertEqual(afterUnauthorized, beforeUnauthorized,
                                 "missing secret must not call Luna")
                status, body = request(state, originalRemote="203.0.113.20")
                self.assertEqual(status, 403, body.decode("utf-8", "replace"))

                status, body = request(state)
                self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                stateResult = json.loads(body)["result"]
                self.assertTrue(stateResult["supported"], stateResult)
                self.assertEqual([part["id"] for part in stateResult["parts"]],
                                 ["banner", "quickCards", "apps", "deviceFunctions"])
                self.assertTrue(all(part["shown"] for part in stateResult["parts"]))

                status, body = request({"action": "setPart", "id": "quickCards", "shown": False})
                self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                self.assertTrue(json.loads(body)["result"]["ok"])
                expected = (layoutDir / "home_layoutShelfView.xml").read_text()
                self.assertIn("<!-- nativetv -->", expected)
                self.assertNotIn('type="Qcard"', expected)
                self.assertIn('type="Hero"', expected)
                self.assertEqual((layoutDir / "home_layout.xml").read_text(), expected)

                addInput = {
                    "action": "luna",
                    "uri": "com.webos.applicationManager/addLaunchPoint",
                    "payload": {"id": "com.webos.app.hdmi2", "title": "Mac",
                                "params": {}, "titleVisible": True}
                }
                status, body = request(addInput)
                self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                self.assertEqual(json.loads(body)["result"]["launchPointId"], "1001")
                listPoints = {"action": "luna",
                              "uri": "com.webos.applicationManager/listLaunchPoints",
                              "payload": {}}
                status, body = request(listPoints)
                self.assertTrue(json.loads(body)["result"]["returnValue"])
                status, body = request({"action": "setInput", "id": "hdmi1",
                                       "onHome": True, "title": "HDMI 1"})
                self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                self.assertTrue(json.loads(body)["result"]["ok"])
                status, body = request(listPoints)
                points = json.loads(body)["result"]["launchPoints"]
                self.assertEqual([(point["id"], point["launchPointId"]) for point in points[:2]],
                                 [("com.webos.app.hdmi1", "1002"),
                                  ("com.webos.app.hdmi2", "200")],
                                 "native Home puts the HDMI tiles first in input order")
                tracking = {"action": "inputTracking",
                            "inputs": {"hdmi1": "1001", "hdmi2": "200"}}
                status, body = request(tracking)
                self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                self.assertTrue(json.loads(body)["result"]["ok"])
                homeInputs = root / "data/home-inputs.json"
                self.assertTrue(homeInputs.exists(),
                                "native Home state was not saved: %r" %
                                [str(path) for path in (root / "data").glob("*")])
                self.assertEqual(json.loads(homeInputs.read_text()),
                                 {"hdmi1": "1001", "hdmi2": "200"})
                status, body = request(state)
                self.assertEqual(json.loads(body)["result"]["addedInputs"],
                                 {"hdmi1": "1001", "hdmi2": "200"})
                status, body = request({"action": "inputTracking", "inputs": {"hdmi5": "1001"}})
                self.assertFalse(json.loads(body)["result"]["ok"])
                status, body = request({"action": "setInput", "id": "hdmi2",
                                       "onHome": False, "title": "HDMI 2"})
                self.assertTrue(json.loads(body)["result"]["ok"])
                self.assertEqual(json.loads(homeInputs.read_text()), {"hdmi1": "1001"})
                invalid = {"action": "luna", "uri": "com.webos.applicationManager/launch",
                           "payload": {"id": "com.example.other"}}
                beforeInvalid = len(details.read_text().splitlines())
                status, _ = request(invalid)
                self.assertEqual(status, 400)
                self.assertEqual(len(details.read_text().splitlines()), beforeInvalid,
                                 "unknown launcher targets must be rejected before Luna")

                for part in ("banner", "apps"):
                    status, body = request({"action": "setPart", "id": part, "shown": False})
                    self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                status, body = request({"action": "setPart", "id": "deviceFunctions", "shown": False})
                self.assertFalse(json.loads(body)["result"]["ok"],
                                 "the final visible Home part must remain")

                takeover = '<home><!-- nativetv-home-starting --></home>\n'
                (layoutDir / "home_layoutShelfView.xml").write_text(takeover)
                status, body = request({"action": "setPart", "id": "banner", "shown": True})
                self.assertFalse(json.loads(body)["result"]["ok"])
                self.assertIn("Native Tv Home", json.loads(body)["result"]["error"])
                status, body = request({"action": "restore"})
                self.assertTrue(json.loads(body)["result"]["ok"])
                self.assertFalse((root / "data/home-inputs.json").exists())
                self.assertTrue(any(call["uri"] == "com.webos.applicationManager/removeLaunchPoint" and
                                    call["payload"] == {"launchPointId": "1001"}
                                    for call in map(json.loads, details.read_text().splitlines())),
                                "native restore removes the tracked HDMI tile")
                self.assertEqual((layoutDir / "home_layoutShelfView.xml").read_text(), takeover,
                                 "restore must leave the Home takeover marker untouched")
                self.assertFalse((layoutDir / "home_layout.xml").exists(),
                                 "restore removes the daemon-owned layout")

                calls = [json.loads(line) for line in details.read_text().splitlines()]
                homeCalls = [call for call in calls if call["uri"].startswith(
                    "com.webos.applicationManager/")]
                self.assertIn({"uri": "com.webos.applicationManager/addLaunchPoint",
                               "applicationId": "", "payload": addInput["payload"]}, homeCalls)
                process.send_signal(signal.SIGTERM)
                output, _ = process.communicate(timeout=6)
                self.assertEqual(process.returncode, 0, output)
                self.assertIn("command-policy: action=home-screen.setPart result=success", output)
                self.assertIn("command-policy: action=home-screen.setInput result=success", output)
                self.assertIn("command-policy: action=home-screen.inputTracking result=success", output)
                self.assertIn("command-policy: action=home-screen.luna.restore-input result=success", output)
                self.assertIn("command-policy: action=home-screen.luna.add-input result=success", output)
                self.assertIn("command-policy: action=home-screen.luna.remove-input result=success", output)
                self.assertIn("command-policy: action=home-screen.luna.move-input result=success", output)
            finally:
                if process.poll() is None:
                    process.send_signal(signal.SIGTERM)
                    try:
                        process.communicate(timeout=6)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait(timeout=3)

            dataRoot = root / "data"
            dataRoot.mkdir(exist_ok=True)
            homeInputs = dataRoot / "home-inputs.json"
            homeInputs.write_text(json.dumps({"hdmi3": "restore-home-cli-1"}))
            offlineConfig = json.loads(config.read_text())
            offlineConfig["allowControl"] = False
            config.write_text(json.dumps(offlineConfig))
            restore = subprocess.run(
                [str(binary), "--restore-home-screen", "--config", str(config),
                 "--assets-root", str(assets), "--proc-root", str(procRoot),
                 "--system-root", str(systemRoot)],
                capture_output=True, text=True, env=environment, timeout=30)
            self.assertEqual(restore.returncode, 0,
                             restore.stdout + restore.stderr)
            self.assertFalse(homeInputs.exists(),
                             "the native restore command should clear input tracking")
            restoreCalls = [json.loads(line) for line in details.read_text().splitlines()]
            self.assertTrue(any(call["uri"] ==
                                "com.webos.applicationManager/removeLaunchPoint" and
                                call["payload"] == {"launchPointId": "restore-home-cli-1"}
                                for call in restoreCalls),
                            "the native restore command should remove tracked input tiles")

    def test_native_home_screen_webos10_overlay(self):
        binary = Path(sys.argv[1]).resolve()
        fakeLuna = binary.parent / "native-fake-luna-send"
        publicPort = unused_port_pair()
        nodePort = unused_port_pair((publicPort, publicPort + 1))
        secret = "home-screen10-secret-0123456789abcdef"
        token = "home-screen10-route-token"
        with tempfile.TemporaryDirectory(prefix="nativetv-native-home10-") as folder:
            root = Path(folder)
            config = root / "config.json"
            heartbeat = root / "tvweb.beat"
            assets = root / "assets"
            systemRoot = root / "system"
            procRoot = root / "proc"
            dataRoot = root / "data"
            homeAssets = systemRoot / "usr/palm/applications/com.webos.app.home/data/flutter_assets/assets"
            binaryDirectory = systemRoot / "bin"
            details = root / "luna-details.jsonl"
            calls = root / "mount-calls"
            mounts = systemRoot / "proc/mounts"
            umountFailure = root / "umount-failure"
            assets.mkdir()
            (systemRoot / "etc").mkdir(parents=True)
            (systemRoot / "etc/issue").write_text("webOS TV 10.3.1\n")
            (systemRoot / "proc").mkdir(parents=True)
            (systemRoot / "proc/filesystems").write_text("nodev overlay\n")
            mounts.write_text("")
            procRoot.mkdir()
            homeAssets.mkdir(parents=True)
            binaryDirectory.mkdir(parents=True)
            lg = ('<home>\n'
                  '<item id="container" hasChildren="true" itemHeight="1000">\n'
                  '<item id="herobanner" itemHeight="500"/>\n'
                  '<item id="globalline" itemWidth="32" itemHeight="120"/>\n'
                  '<item id="qcardList" itemHeight="100"/>\n'
                  '<item id="recommendedShelf" itemHeight="200"/>\n'
                  '<item id="appList" itemHeight="300"/>\n'
                  '</item>\n</home>\n')
            (homeAssets / "home.xml").write_text(lg)
            mount = binaryDirectory / "mount"
            mount.write_text(
                "#!/bin/sh\n"
                "for target do :; done\n"
                "echo \"mount $*\" >> \"$NATIVETV_HOME_MOUNT_CALLS\"\n"
                "printf 'none %s overlay rw 0 0\\n' \"$target\" >> \"$NATIVETV_HOME_MOUNTS\"\n"
                "exit 0\n")
            mount.chmod(0o755)
            umount = binaryDirectory / "umount"
            umount.write_text(
                "#!/bin/sh\n"
                "target=\"$1\"\n"
                "echo \"umount $*\" >> \"$NATIVETV_HOME_MOUNT_CALLS\"\n"
                "if [ -e \"$NATIVETV_HOME_UMOUNT_FAILURE\" ]; then exit 1; fi\n"
                "tmp=\"$NATIVETV_HOME_MOUNTS.tmp\"\n"
                "awk -v target=\"$target\" '$2 != target { print }' \"$NATIVETV_HOME_MOUNTS\" > \"$tmp\"\n"
                "mv \"$tmp\" \"$NATIVETV_HOME_MOUNTS\"\n"
                "exit 0\n")
            umount.chmod(0o755)
            config.write_text(json.dumps({
                "host": "127.0.0.1", "port": publicPort, "token": token,
                "allowControl": True, "allowHomeLayout": True
            }))
            environment = os.environ.copy()
            environment["TVWEB_LUNA_SEND"] = str(fakeLuna)
            environment["TVWEB_DATA_DIR"] = str(dataRoot)
            environment["TVWEB_PROXY_SECRET"] = secret
            environment["NATIVETV_FAKE_LUNA_MODE"] = "home-screen"
            environment["NATIVETV_FAKE_LUNA_DETAILS"] = str(details)
            environment["NATIVETV_HOME_MOUNT_CALLS"] = str(calls)
            environment["NATIVETV_HOME_MOUNTS"] = str(mounts)
            environment["NATIVETV_HOME_UMOUNT_FAILURE"] = str(umountFailure)
            process = subprocess.Popen(
                [str(binary), "--config", str(config), "--heartbeat", str(heartbeat),
                 "--workers", "4", "--listen", "--node-port", str(nodePort),
                 "--assets-root", str(assets), "--proc-root", str(procRoot),
                 "--system-root", str(systemRoot)],
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                env=environment)

            def request(command):
                data = json.dumps(command).encode("utf-8")
                req = Request("http://127.0.0.1:%d/_internal/home-screen" % publicPort,
                              data=data, method="POST", headers={
                                  "Content-Type": "application/json",
                                  "X-NativeTv-Proxy-Secret": secret})
                try:
                    with urlopen(req, timeout=12) as response:
                        return response.status, response.read()
                except HTTPError as error:
                    return error.code, error.read()

            try:
                deadline = time.monotonic() + 8
                ready = False
                while time.monotonic() < deadline and process.poll() is None:
                    req = Request("http://127.0.0.1:%d/api/caps" % publicPort,
                                  headers={"Authorization": "Bearer " + token})
                    try:
                        with urlopen(req, timeout=2):
                            ready = True
                            break
                    except (OSError, URLError):
                        time.sleep(0.03)
                self.assertTrue(ready, "native daemon did not start")

                status, body = request({"action": "state"})
                self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                state = json.loads(body)["result"]
                self.assertTrue(state["supported"], state)
                self.assertTrue(state["experimental"])
                self.assertTrue(state["allowed"])
                self.assertEqual([part["id"] for part in state["parts"]],
                                 ["sideIcons", "quickCards", "recommended"])

                status, body = request({"action": "setPart", "id": "quickCards", "shown": False})
                self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                self.assertTrue(json.loads(body)["result"]["ok"])
                stage = systemRoot / "tmp/nativetv-home/upper/home.xml"
                staged = stage.read_text()
                self.assertNotIn('id="qcardList"', staged)
                self.assertIn('id="recommendedShelf"', staged)
                self.assertEqual((dataRoot / "home-webos10-lg.xml").read_text(), lg)
                self.assertEqual(json.loads((dataRoot / "home-webos10.json").read_text()),
                                 {"hidden": ["quickCards"]})
                self.assertIn(str(homeAssets), mounts.read_text())

                status, body = request({"action": "state"})
                updated = json.loads(body)["result"]
                self.assertFalse(updated["parts"][1]["shown"])

                umountFailure.touch()
                status, body = request({"action": "allowChanged", "allow": False})
                self.assertFalse(json.loads(body)["result"]["ok"])
                self.assertTrue((dataRoot / "home-webos10.json").exists())
                self.assertIn(str(homeAssets), mounts.read_text())

                umountFailure.unlink()
                status, body = request({"action": "allowChanged", "allow": False})
                self.assertTrue(json.loads(body)["result"]["ok"])
                self.assertFalse((dataRoot / "home-webos10.json").exists())
                self.assertNotIn(str(homeAssets), mounts.read_text())
                self.assertTrue(any(line.startswith("umount ") for line in calls.read_text().splitlines()))

                status, body = request({"action": "setPart", "id": "recommended", "shown": False})
                self.assertFalse(json.loads(body)["result"]["ok"],
                                 "experimental layout controls stay gated")

                status, body = request({"action": "allowChanged", "allow": True})
                self.assertTrue(json.loads(body)["result"]["ok"])
                dataRoot.mkdir(exist_ok=True)
                (dataRoot / "home-webos10.json").write_text(
                    json.dumps({"hidden": ["recommended"]}) + "\n")
                status, body = request({"action": "reapply"})
                self.assertTrue(json.loads(body)["result"]["ok"])
                self.assertNotIn('id="recommendedShelf"',
                                 (systemRoot / "tmp/nativetv-home/upper/home.xml").read_text())
                status, body = request({"action": "restore"})
                self.assertTrue(json.loads(body)["result"]["ok"])
                self.assertFalse((dataRoot / "home-webos10.json").exists())
                self.assertNotIn(str(homeAssets), mounts.read_text())

                process.send_signal(signal.SIGTERM)
                output, _ = process.communicate(timeout=6)
                self.assertEqual(process.returncode, 0, output)
                self.assertIn("command-policy: action=home-screen.setPart result=success", output)
                self.assertIn("command-policy: action=home-screen.allowChanged result=success", output)
            finally:
                if process.poll() is None:
                    process.send_signal(signal.SIGTERM)
                    try:
                        process.communicate(timeout=6)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait(timeout=3)

    def test_installer_job_survives_daemon_kill_as_interrupted(self):
        binary = Path(sys.argv[1]).resolve()
        fakeLuna = binary.parent / "native-fake-luna-send"
        publicPort = unused_port_pair()
        nodePort = unused_port_pair((publicPort, publicPort + 1))
        token = "install-power-cut-route-token"
        secret = "install-power-cut-proxy-secret-0123456789abcdef"
        process = None

        with tempfile.TemporaryDirectory(prefix="nativetv-install-power-cut-") as folder:
            root = Path(folder)
            dataRoot = root / "data"
            config = root / "config.json"
            heartbeat = root / "tvweb.beat"
            assets = root / "assets"
            assets.mkdir()
            config.write_text(json.dumps({
                "host": "127.0.0.1", "port": publicPort, "token": token,
                "allowControl": True
            }))
            environment = os.environ.copy()
            environment["TVWEB_DATA_DIR"] = str(dataRoot)
            environment["TVWEB_PROXY_SECRET"] = secret
            environment["TVWEB_LUNA_SEND"] = str(fakeLuna)

            def startDaemon():
                return subprocess.Popen(
                    [str(binary), "--config", str(config), "--heartbeat", str(heartbeat),
                     "--workers", "2", "--listen", "--node-port", str(nodePort),
                     "--assets-root", str(assets)],
                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                    env=environment)

            def waitReady(current):
                deadline = time.monotonic() + 8
                while time.monotonic() < deadline and current.poll() is None:
                    request = Request(
                        "http://127.0.0.1:%d/api/caps" % publicPort,
                        headers={"Authorization": "Bearer " + token})
                    try:
                        with urlopen(request, timeout=1) as response:
                            if response.status == 200:
                                return
                    except (OSError, URLError):
                        time.sleep(0.03)
                output = ""
                if current.poll() is not None:
                    output, _ = current.communicate(timeout=3)
                self.fail("native daemon did not start for installer recovery:\n" + output[-4000:])

            def postInstallJob(payload):
                request = Request(
                    "http://127.0.0.1:%d/_internal/apps/install-job" % publicPort,
                    data=json.dumps(payload).encode("utf-8"), method="POST",
                    headers={"Content-Type": "application/json",
                             "X-NativeTv-Proxy-Secret": secret})
                try:
                    with urlopen(request, timeout=5) as response:
                        return response.status, response.read()
                except HTTPError as error:
                    return error.code, error.read()

            try:
                process = startDaemon()
                waitReady(process)
                job = {
                    "jobId": "power-cut-install-1", "state": "downloading",
                    "package": "com.example.app", "version": "1.2.3",
                    "updated": 1700000000000
                }
                for state in ("downloading", "verifying", "awaiting-confirm", "installing"):
                    job["state"] = state
                    status, body = postInstallJob({"action": "write", "job": job})
                    self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                installJobPath = dataRoot / "install-job.json"
                self.assertEqual(json.loads(installJobPath.read_text())["state"], "installing")

                # SIGKILL models power loss: no daemon shutdown hook can rewrite or clear state.
                process.kill()
                output, _ = process.communicate(timeout=5)
                self.assertEqual(process.returncode, -signal.SIGKILL, output)
                process = startDaemon()
                waitReady(process)

                status, body = postInstallJob({"action": "recover"})
                self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                recovered = json.loads(body)
                self.assertTrue(recovered["found"], recovered)
                self.assertEqual(recovered["job"]["state"], "interrupted")
                self.assertEqual(json.loads(installJobPath.read_text())["state"],
                                 "interrupted")
                status, body = postInstallJob({"action": "recover"})
                self.assertEqual(status, 200, body.decode("utf-8", "replace"))
                self.assertEqual(json.loads(body), {"ok": True, "found": False, "job": None})
                self.assertFalse(installJobPath.exists(),
                                 "the recovered interruption should be consumed once")
            finally:
                if process is not None and process.poll() is None:
                    process.kill()
                    process.communicate(timeout=5)


if __name__ == "__main__":
    if sys.argv[1:] == ["--list-tests"]:
        suite = unittest.defaultTestLoader.loadTestsFromTestCase(DaemonRuntimeTests)
        for test in suite:
            print(test.id().rsplit(".", 1)[-1])
        sys.exit(0)
    unittest.main(argv=[sys.argv[0]] + sys.argv[2:])
