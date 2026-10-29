"""Exercise native ad-block routes against isolated state and a Node bridge."""
import http.client
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import time
import unittest


def unused_port_pair(excluded=()):
    excluded = set(excluded)
    base = 24000 + ((os.getpid() * 29) % 12000)
    for attempt in range(100):
        port = base + attempt * 2
        if port >= 60000 or port in excluded or port + 1 in excluded:
            continue
        sockets = [socket.socket(), socket.socket()]
        try:
            sockets[0].bind(("127.0.0.1", port))
            sockets[1].bind(("127.0.0.1", port + 1))
            return port
        except OSError:
            pass
        finally:
            for item in sockets:
                item.close()
    raise RuntimeError("could not reserve a test port pair")


class NodeBridgeHandler(BaseHTTPRequestHandler):
    secret = ""
    invalidations = 0

    def do_POST(self):
        if self.path != "/_internal/native-state":
            self.send_error(404)
            return
        if self.headers.get("X-NativeTv-Proxy-Secret") != self.secret:
            self.send_error(403)
            return
        length = int(self.headers.get("Content-Length", "0"))
        payload = json.loads(self.rfile.read(length) or b"{}")
        if payload.get("type") != "invalidate" or payload.get("group") != "control":
            self.send_error(400)
            return
        type(self).invalidations += 1
        self.send_response(204)
        self.end_headers()

    def log_message(self, *_args):
        pass


class AdBlockFrontDoorTests(unittest.TestCase):
    def test_custom_list_and_control_routes_share_native_store(self):
        binary = Path(sys.argv[1]).resolve()
        fake_luna = binary.parent / "native-fake-luna-send"
        token = "adblock-route-test-token"
        secret = "adblock-route-proxy-secret-0123456789abcdef"
        public_port = unused_port_pair()
        node_port = unused_port_pair((public_port, public_port + 1))

        with tempfile.TemporaryDirectory(prefix="nativetv-adblock-route-") as folder:
            root = Path(folder)
            data = root / "data"
            data.mkdir()
            system = root / "system"
            hosts = system / "etc" / "hosts"
            hosts.parent.mkdir(parents=True)
            hosts.write_text("127.0.0.1 localhost\n")
            config = root / "config.json"
            heartbeat = root / "daemon.beat"
            assets = root / "assets"
            assets.mkdir()
            details = root / "luna-details.jsonl"
            config.write_text(json.dumps({
                "host": "127.0.0.1", "port": public_port, "token": token,
                "allowControl": True,
            }))

            NodeBridgeHandler.secret = secret
            NodeBridgeHandler.invalidations = 0
            node = ThreadingHTTPServer(("127.0.0.1", node_port), NodeBridgeHandler)
            node_thread = threading.Thread(target=node.serve_forever, daemon=True)
            node_thread.start()

            environment = os.environ.copy()
            environment["TVWEB_LUNA_SEND"] = str(fake_luna)
            environment["TVWEB_DATA_DIR"] = str(data)
            environment["TVWEB_PROXY_SECRET"] = secret
            environment["NATIVETV_FAKE_LUNA_MODE"] = "adblock"
            environment["NATIVETV_FAKE_LUNA_DETAILS"] = str(details)
            restore = subprocess.run(
                [str(binary), "--restore-adblock", "--system-root", str(system)],
                capture_output=True, text=True, timeout=4, env=environment)
            self.assertEqual(restore.returncode, 0, restore.stderr)
            process = subprocess.Popen(
                [str(binary), "--config", str(config), "--heartbeat", str(heartbeat),
                 "--workers", "4", "--listen", "--node-port", str(node_port),
                 "--assets-root", str(assets), "--system-root", str(system)],
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                env=environment)

            def request(path, payload=None, token_value=token, content_type="application/json"):
                body = None if payload is None else payload.encode("utf-8") if isinstance(payload, str) else json.dumps(payload).encode("utf-8")
                connection = http.client.HTTPConnection("127.0.0.1", public_port, timeout=4)
                headers = {}
                if token_value:
                    headers["Authorization"] = "Bearer " + token_value
                if content_type:
                    headers["Content-Type"] = content_type
                if body is not None:
                    headers["Content-Length"] = str(len(body))
                connection.request("POST", path, body=body, headers=headers)
                response = connection.getresponse()
                status, data_bytes = response.status, response.read()
                connection.close()
                return status, json.loads(data_bytes or b"{}")

            try:
                deadline = time.monotonic() + 8
                while time.monotonic() < deadline and process.poll() is None:
                    try:
                        with socket.create_connection(("127.0.0.1", public_port), timeout=0.2):
                            break
                    except OSError:
                        time.sleep(0.03)
                self.assertIsNone(process.poll(), "native daemon exited before serving requests")
                country_deadline = time.monotonic() + 3
                while time.monotonic() < country_deadline and not (data / "country").exists():
                    time.sleep(0.02)
                self.assertEqual((data / "country").read_text(), "us")

                status, result = request("/api/adblock/custom", {
                    "mode": "replace",
                    "text": "0.0.0.0 ads.example.com\nnot-a-domain\n",
                    "file": "fixture list.txt",
                })
                self.assertEqual(status, 200)
                self.assertTrue(result["ok"], result)
                self.assertEqual(result["custom"]["mode"], "replace")
                self.assertEqual(result["custom"]["count"], 1)
                self.assertEqual(result["skipped"], 1)
                saved = json.loads((data / "adblock_custom.json").read_text())
                self.assertEqual(saved["names"], ["ads.example.com"])

                status, result = request("/api/adblock/custom", {"enabled": False})
                self.assertEqual(status, 200)
                self.assertTrue(result["ok"])
                self.assertFalse(result["custom"]["enabled"])

                status, result = request("/api/control", {"action": "adblock", "value": "off"})
                self.assertEqual(status, 200)
                self.assertTrue(result["ok"])
                self.assertEqual(result["mode"], "off")
                self.assertFalse((data / "adblock_enabled").exists())

                status, result = request("/api/control", {"action": "blockTvUpdates", "value": False})
                self.assertEqual(status, 200)
                self.assertTrue(result["ok"])
                self.assertFalse(result["tvUpdatesBlocked"])

                status, result = request("/api/adblock/custom", "{bad json")
                self.assertEqual(status, 400)
                self.assertFalse(result["ok"])

                status, result = request("/api/adblock/custom", {"clear": True})
                self.assertEqual(status, 200)
                self.assertTrue(result["ok"])
                self.assertFalse((data / "adblock_custom.json").exists())
                self.assertGreaterEqual(NodeBridgeHandler.invalidations, 3)
            finally:
                if process.poll() is None:
                    process.send_signal(signal.SIGTERM)
                    output, _ = process.communicate(timeout=6)
                else:
                    output, _ = process.communicate(timeout=2)
                node.shutdown()
                node.server_close()
                node_thread.join(timeout=2)
                self.assertEqual(process.returncode, 0, output[-4000:])


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]], verbosity=2)
