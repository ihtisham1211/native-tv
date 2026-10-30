#!/usr/bin/env python3
"""A stand-in daemon for the TV client's Live, Settings and Catalog pages: the readings of a
real C4 (tests/fixtures/tv), the sections of the dashboard and the Homebrew catalogue, all
deterministic. It answers what home_fixture.py answers too (the catalogue, icons, artwork), so
the same top bar is drawn.

  dashboard_fixture.py [--port N]    serve until killed (prints the origin)
  import dashboard_fixture; with dashboard_fixture.serve() as origin: ...

Writes are accepted and answered {"ok": true}. A catalogue install is a small state machine:
the fetch leaves the job awaiting confirmation, the confirm runs it for two status reads, and
the next one reports it installed.
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

import home_fixture

HERE = os.path.dirname(os.path.abspath(__file__))
TV = os.path.join(HERE, "fixtures", "tv")
# The processor's load, one per read of /api/stats: the line under the card has a shape.
LOADS = [31, 36, 42, 38, 33, 29, 34, 41, 47, 39, 35, 30, 28, 33, 37, 34]


def fixture(name):
    with open(os.path.join(TV, name)) as file:
        return json.load(file)


def stats(reads):
    value = fixture("stats-c4-webos9.json")
    value["load"] = LOADS[reads % len(LOADS)]
    value.update(section("options-extra"))
    return value


def section(name):
    with open(os.path.join(TV, "dashboard", name + ".json")) as file:
        return json.load(file)


APPS, OLED, SAVER, HOME = section("apps"), section("oled"), section("saver"), section("home")
PRIVACY, SERVICE, SETUP = section("privacy"), section("service"), section("setup")
CATALOG, PREVIEW, GAME = section("catalog"), section("install-preview"), section("game")


def catalogue():
    """Home's catalogue with the Live tab on, so the bar reads Home, Live, Apps as the design does."""
    value = home_fixture.catalogue()
    value["layout"].update({"showLive": True, "showInputs": False})
    value["sources"] = {"channels": 1}
    return value


class Handler(home_fixture.Handler):
    reads = 0
    installs = 0       # status reads since the confirm; 0 = none running
    job = "idle"       # idle, awaiting-confirm, installing, installed

    def status(self):
        cls = Handler
        if cls.job == "installing":
            cls.installs += 1
            if cls.installs > 2:
                cls.job = "installed"
        if cls.job == "awaiting-confirm":
            return dict(PREVIEW, ok=True)
        if cls.job == "installing":
            return {"ok": True, "state": "downloading", "jobId": "job-1", "progress": {"bytes": 2600000, "total": 5200000}}
        if cls.job == "installed":
            return {"ok": True, "state": "installed", "jobId": "job-1",
                    "result": {"title": "Photo Frame", "version": "1.4.0", "package": "org.example.photos"}}
        return {"ok": True, "state": "idle"}

    def do_GET(self):
        # NATIVETV_FIXTURE_DELAY_MS makes the sections answer late, to see the loading indicator.
        delay = float(os.environ.get("NATIVETV_FIXTURE_DELAY_MS", "0")) / 1000
        if delay and self.path.startswith("/api/") and not self.path.startswith("/api/home"):
            time.sleep(delay)
        url = urllib.parse.urlsplit(self.path)
        query = urllib.parse.parse_qs(url.query)
        routes = {
            "/api/apps": APPS, "/api/oledcare": OLED, "/api/screensaver": SAVER,
            "/api/home": HOME, "/api/privacy": PRIVACY, "/api/servicemenu": SERVICE, "/api/setup": SETUP,
            "/api/apps/catalog": CATALOG,
            "/api/game": GAME}
        if url.path == "/api/caps":
            self.json({"ok": True, "allowPower": True, "origin": "http://192.168.1.50:3000"})
        elif url.path == "/api/home/catalogue":
            self.json(catalogue())
        elif url.path == "/api/stats":
            Handler.reads += 1
            self.json(stats(Handler.reads - 1))
        elif url.path == "/api/apps/install/status":
            self.json(self.status())
        elif url.path in routes:
            self.json(routes[url.path])
        else:
            super().do_GET()

    def do_POST(self):
        size = int(self.headers.get("Content-Length", "0"))
        body = json.loads(self.rfile.read(size) or b"{}")
        if self.path == "/api/apps/install/fetch":
            Handler.job = "awaiting-confirm"
            self.json({"ok": True, "install": self.status()})
        elif self.path == "/api/apps/install/confirm":
            Handler.job, Handler.installs = "installing", 0
            self.json({"ok": True, "install": self.status()})
        elif self.path == "/api/apps/install/cancel":
            Handler.job = "idle"
            self.json({"ok": True, "install": {"state": "idle"}})
        elif self.path == "/api/home/beat":
            super().do_POST()
        else:
            self.json({"ok": True, "echo": body})


@contextlib.contextmanager
def serve(port=0):
    Handler.reads, Handler.installs, Handler.job = 0, 0, "idle"
    Handler.art = None
    server = http.server.ThreadingHTTPServer(("127.0.0.1", port), Handler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        yield "http://127.0.0.1:%d" % server.server_port
    finally:
        server.shutdown()
        server.server_close()
        thread.join()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", type=int, default=0)
    args = parser.parse_args()
    with serve(args.port) as origin:
        print(origin, flush=True)
        threading.Event().wait()


if __name__ == "__main__":
    sys.exit(main())
