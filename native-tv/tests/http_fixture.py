#!/usr/bin/env python3
"""Run the HTTP adapter against bounded, local failure fixtures."""
import http.server
import json
import subprocess
import sys
import threading
import time
import urllib.parse


class Handler(http.server.BaseHTTPRequestHandler):
    caps_calls = 0

    def log_message(self, *_):
        pass

    def reply(self, status, body):
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        try:
            self.wfile.write(body)
        except (BrokenPipeError, ConnectionResetError):
            pass

    def do_GET(self):
        url = urllib.parse.urlsplit(self.path)
        if url.path == "/api/caps":
            type(self).caps_calls += 1
            if type(self).caps_calls in (1, 2) or type(self).caps_calls > 3:
                self.reply(503, b'{"ok":false,"error":"Starting"}')
            else:
                self.reply(200, b'{"ok":true}')
        elif url.path == "/api/home/search":
            query = urllib.parse.parse_qs(url.query).get("q", [""])[0]
            if query in ("slow", "hang"):
                time.sleep(0.3 if query == "slow" else 5)
            self.reply(200, json.dumps({"ok": True, "query": query}).encode())
        elif url.path == "/api/home/now":
            self.reply(200, b'{"kind":"input","port":4}')
        elif url.path in ("/api/home/picture", "/api/home/recents/image"):
            self.reply(200, json.dumps({"query": urllib.parse.parse_qs(url.query)}).encode())
        elif url.path == "/api/home/wallpaper":
            self.reply(200, json.dumps({"query": urllib.parse.parse_qs(url.query)}).encode())
        elif url.path == "/api/privacy":
            self.reply(503, b'{"ok":false,"error":"Unavailable"}')
        elif url.path == "/api/home/icon":
            self.reply(200, b"image")
        elif url.path == "/api/apps":
            self.reply(200, b"x" * (8 * 1024 * 1024 + 1))
        else:
            self.reply(404, b"{}")

    def do_POST(self):
        if self.headers.get("Content-Type") != "application/json":
            return self.reply(415, b"{}")
        size = int(self.headers.get("Content-Length", "0"))
        if size > 65536:
            return self.reply(413, b"{}")
        body = json.loads(self.rfile.read(size))
        self.reply(200, json.dumps({"ok": True, "path": self.path, "body": body}).encode())


def main():
    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    worker = threading.Thread(target=server.serve_forever, daemon=True)
    worker.start()
    try:
        result = subprocess.run([sys.argv[1], "http://127.0.0.1:" + str(server.server_port)], timeout=20)
    finally:
        server.shutdown()
        server.server_close()
        worker.join()
    return result.returncode


if __name__ == "__main__":
    sys.exit(main())
