#!/usr/bin/env python3
"""A stand-in for the daemon's first-run routes, over a data root that starts wiped.

The routes and their answers are those of contract/routes.json (/api/caps, /api/setup,
/api/setup/{network,always-ready,handoff,handoff-stop,done}, /api/settings); what the daemon
keeps in files, the marker `.setup-pending` and `config.json`, this keeps in the root it is given,
so a test reads what the TV client wrote. A write that restarts the real server makes the next
requests fail for `restart_ms`.

  setup_fixture.py ROOT [--port N] [--no-standby] [--demo] [--settled]
"""
import argparse
import contextlib
import http.server
import json
import os
import sys
import threading
import time
import urllib.parse


HANDOFF_URL = "http://192.168.1.42:3000/?c=" + "0f3a9c" * 5 + "ab"


class State:
    def __init__(self, root, standby=True, demo=False, restart_ms=1200, pending=True):
        self.root, self.standby, self.demo, self.restart_ms = root, standby, demo, restart_ms
        self.lock = threading.Lock()
        self.down_until = 0.0
        self.handoff = ""
        self.calls = []
        self.broker = None
        self.always = False
        os.makedirs(root, exist_ok=True)
        if pending:
            open(os.path.join(root, ".setup-pending"), "w").close()
        self.save({"host": "127.0.0.1", "port": 0})

    def path(self, name):
        return os.path.join(self.root, name)

    def config(self):
        with open(self.path("config.json")) as file:
            return json.load(file)

    def save(self, config):
        with open(self.path("config.json"), "w") as file:
            json.dump(config, file)

    def restart(self):
        self.down_until = time.time() + self.restart_ms / 1000.0

    def pending(self):
        return os.path.exists(self.path(".setup-pending"))


class Handler(http.server.BaseHTTPRequestHandler):
    state = None

    def log_message(self, *_):
        pass

    def send(self, status, value):
        body = json.dumps(value).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        try:
            self.wfile.write(body)
        except (BrokenPipeError, ConnectionResetError):
            pass

    def refuse_while_restarting(self):
        # The restarting server answers nothing: the connection is cut.
        if time.time() < self.state.down_until:
            self.connection.close()
            self.close_connection = True
            return True
        return False

    def setup_state(self):
        state = self.state
        config = state.config()
        answer = {"ok": True, "needed": state.pending(), "writable": True,
                  "network": config["host"] != "127.0.0.1", "token": False,
                  "address": "192.168.1.42:3000", "hostname": "lgwebostv",
                  "homeAssistant": {"configured": bool(state.broker),
                                    "state": "connected" if state.broker else "unknown"},
                  "handoff": state.handoff or None}
        if state.standby:
            answer["alwaysReady"] = state.always
        if state.demo:
            answer["demo"] = True
        return answer

    def do_GET(self):
        url = urllib.parse.urlsplit(self.path).path
        state = self.state
        if url == "/api/caps":
            return self.send(200, {"ok": True, "setupNeeded": state.pending()})
        if self.refuse_while_restarting():
            return
        with state.lock:
            state.calls.append("GET " + url)
            if url == "/api/setup":
                return self.send(200, self.setup_state())
            if url == "/api/settings":
                return self.send(200, {
                    "ok": True, "writable": True, "iconServer": False,
                    "mqtt": {"enabled": False, "host": "", "port": None, "tls": False,
                             "tlsRejectUnauthorized": True, "username": "", "passwordSet": False,
                             "topicPrefix": "lgtv", "discoveryPrefix": "homeassistant",
                             "telemetryIntervalMs": 30000,
                             "entities": {"controls": True, "oled": True, "video": True,
                                          "system": True, "diagnostics": True, "disabled": []}},
                    "device": {"id": "lg_tv", "name": "LG TV"}})
        self.send(404, {"ok": False})

    def do_POST(self):
        size = int(self.headers.get("Content-Length", "0"))
        try:
            body = json.loads(self.rfile.read(size) or b"{}")
        except ValueError:
            return self.send(400, {"ok": False, "error": "malformed JSON"})
        if self.headers.get("Content-Type", "").split(";")[0] != "application/json":
            return self.send(415, {"ok": False, "error": "Content-Type must be application/json"})
        if self.refuse_while_restarting():
            return
        state = self.state
        url = urllib.parse.urlsplit(self.path).path
        with state.lock:
            state.calls.append("POST %s %s" % (url, json.dumps(body, sort_keys=True)))
            if url == "/api/setup/network":
                config = state.config()
                config["host"] = "0.0.0.0" if body.get("open") else "127.0.0.1"
                if not body.get("demo"):
                    state.save(config)
                    state.restart()
                return self.send(200, {"ok": True, "demo": bool(body.get("demo")) or None})
            if url == "/api/setup/always-ready":
                if not body.get("demo"):
                    state.always = bool(body.get("on"))
                return self.send(200, {"ok": True})
            if url == "/api/setup/handoff":
                state.handoff = HANDOFF_URL
                return self.send(200, {"ok": True, "url": state.handoff})
            if url == "/api/setup/handoff-stop":
                state.handoff = ""
                return self.send(200, {"ok": True})
            if url == "/api/setup/done":
                if not body.get("demo") and state.pending():
                    os.remove(state.path(".setup-pending"))
                return self.send(200, {"ok": True})
            if url == "/api/settings":
                mqtt = body.get("mqtt", {})
                if not mqtt.get("host") or not isinstance(mqtt.get("telemetryIntervalMs"), int):
                    return self.send(400, {"ok": False, "error": "a broker address is required"})
                state.broker = {"mqtt": mqtt, "device": body.get("device")}
                with open(state.path("settings.json"), "w") as file:
                    json.dump(state.broker, file)
                state.restart()
                return self.send(200, {"ok": True, "restarting": True})
            if url == "/fixture/phone":
                # What the phone's form does: the link closes, the server restarts with the broker.
                state.handoff = ""
                state.broker = {"mqtt": body, "device": None}
                state.restart()
                return self.send(200, {"ok": True})
        self.send(404, {"ok": False})


@contextlib.contextmanager
def serve(root, port=0, **options):
    state = State(root, **options)
    handler = type("Bound", (Handler,), {"state": state})
    server = http.server.ThreadingHTTPServer(("127.0.0.1", port), handler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        yield "http://127.0.0.1:%d" % server.server_port, state
    finally:
        server.shutdown()
        server.server_close()
        thread.join()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("root")
    parser.add_argument("--port", type=int, default=0)
    parser.add_argument("--no-standby", action="store_true")
    parser.add_argument("--demo", action="store_true")
    parser.add_argument("--settled", action="store_true", help="setup has been done already")
    args = parser.parse_args()
    with serve(args.root, args.port, standby=not args.no_standby, demo=args.demo,
               pending=not args.settled) as (origin, _):
        print(origin, flush=True)
        threading.Event().wait()


if __name__ == "__main__":
    sys.exit(main())
