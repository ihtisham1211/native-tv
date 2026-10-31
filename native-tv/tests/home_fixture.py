#!/usr/bin/env python3
"""A stand-in daemon for the TV client's Home: the catalogue, the discover feed, the beat, app
icons and generated artwork, all invented and deterministic.

  home_fixture.py [--port N] [--art DIR]    serve until killed (prints the origin)
  import home_fixture; with home_fixture.serve() as origin: ...

It also answers what the Quick drawer, the profile pages, title details and search read: stats,
details, search, and (with `profiles=3`) a catalogue with three profiles.

--art DIR serves hero-<name>.jpg / card-<n>.jpg from DIR in place of the generated pictures
(the prototype's art, for a look at the screens against its screenshots).
"""
import argparse
import contextlib
import http.server
import json
import os
import struct
import sys
import threading
import urllib.parse
import zlib

APPS = [
    ("streamline", "Streamline", "#C0392B"), ("tunes", "Tunes", "#8E44AD"), ("arcade", "Arcade", "#2E86C1"),
    ("lens", "Lens", "#CA6F1E"), ("weatherly", "Weatherly", "#2874A6"), ("daily", "Daily", "#566573"),
    ("pulse", "Pulse", "#117A65"), ("market", "Market", "#B03A2E"), ("cinema", "Cinema", "#6C3483"),
    ("audio", "Audiobooks", "#1F618D"), ("shelf", "Shelf", "#7D6608"), ("podcasts", "Podcasts", "#AF601A"),
    ("hdmi", "Inputs", "#2C3E50"), ("browser", "Browser", "#1A5276"), ("photos", "Photos", "#A04000"),
    ("home", "Home Assistant", "#0E6655"), ("remote", "Remote", "#4A235A"), ("sound", "Sound", "#154360"),
    ("display", "Display", "#1B2631"), ("network", "Network", "#1D8348"), ("timer", "Sleep timer", "#7B241C"),
    ("energy", "Energy", "#9A7D0A"), ("themes", "Themes", "#6C3483"), ("livetv", "Live TV", "#1F618D"),
]
# The kinds the daemon would give these apps, and the kinds in their default order.
KINDS = {"streamline": "video", "tunes": "music", "arcade": "games", "lens": "tools", "weatherly": "tools",
         "pulse": "video", "market": "tools", "cinema": "video", "audio": "music", "podcasts": "music",
         "hdmi": "tv", "browser": "tools", "photos": "tools", "home": "home", "remote": "tv", "sound": "tv",
         "display": "tv", "network": "tv", "timer": "tv", "energy": "tv", "themes": "tv", "livetv": "video"}
KIND_LABELS = [("video", "Movies & TV"), ("music", "Music & audio"), ("games", "Games"), ("home", "Smart home"),
               ("tv", "LG & TV"), ("tools", "Tools"), ("other", "Other")]
TITLES = [
    ("Harbor Lights", "movie", "A ferry pilot and a lighthouse keeper trade letters across one long winter.", "Drama"),
    ("The Quiet Orbit", "movie", "Four astronomers wait out a signal that may be older than the stars it came from.", "Science fiction"),
    ("Salt & Ember", "tv", "The caravan reaches the coast, and the debts it carried start coming due.", "Adventure"),
    ("Northbound", "movie", "One sled, nine dogs and a map that stops two days short of the pole.", "Adventure"),
    ("Paper Cities", "tv", "An architect folds a city out of unsent letters, one district at a time.", "Drama"),
    ("Low Tide", "movie", "When the sea draws back, a fishing town finds what it buried to forget.", "Mystery"),
    ("Glass Season", "tv", "A greenhouse keeper learns the weather from the plants.", "Drama"),
    ("Tin Kingdom", "movie", "A toy maker's last commission.", "Family"),
    ("Slow Rivers", "tv", "Four barges, one summer.", "Documentary"),
    ("Midnight Ledger", "movie", "The accountant who kept the books of a lighthouse.", "Thriller"),
]
# Dark top, bright horizon: the colours of the art, one set per picture.
PALETTES = [((10, 26, 40), (214, 190, 140)), ((30, 12, 56), (230, 120, 190)), ((52, 20, 12), (240, 150, 70)),
            ((8, 36, 46), (130, 220, 210)), ((56, 22, 44), (240, 150, 190)), ((14, 40, 28), (220, 230, 140))]


def chunk(kind, body):
    return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body))


def png(width, height, rows):
    raw = b"".join(b"\0" + row for row in rows)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b""))


def mix(a, b, t):
    return tuple(int(a[i] + (b[i] - a[i]) * t) for i in range(3))


def landscape(width, height, index):
    """A sun over two ridges, from row-sized pieces so it is fast without an imaging library."""
    top, glow = PALETTES[index % len(PALETTES)]
    sun_x, sun_y, sun_r = int(width * (.3 + .1 * (index % 5))), int(height * .56), int(height * .11)
    rows = []
    for y in range(height):
        t = y / height
        row = bytearray(bytes(mix(top, glow, max(0.0, (t - .25) / .75) ** 1.6)) * width)
        dy = y - sun_y
        if abs(dy) < sun_r:
            half = int((sun_r * sun_r - dy * dy) ** .5)
            lo, hi = max(0, sun_x - half), min(width, sun_x + half)
            row[lo * 3:hi * 3] = bytes(mix(glow, (255, 240, 210), .6)) * (hi - lo)
        ridge = int(height * .74)
        if y > ridge:
            shade = mix(top, (0, 0, 0), .55 + .3 * (y - ridge) / max(1, height - ridge))
            row[:] = bytes(shade) * width
        rows.append(bytes(row))
    return png(width, height, rows)


def icon(width, color, index):
    r, g, b = (int(color[i:i + 2], 16) for i in (1, 3, 5))
    rows = []
    for y in range(width):
        row = bytearray(bytes((r, g, b)) * width)
        for x in range(width):
            dx, dy = x - width / 2, y - width / 2
            inside = (abs(dx) < width * .22 and abs(dy) < width * .22) if index % 3 == 0 else \
                (dx * dx + dy * dy < (width * .24) ** 2) if index % 3 == 1 else \
                (abs(dx) + abs(dy) < width * .26)
            if inside:
                row[x * 3:x * 3 + 3] = b"\xf4\xf4\xf6"
        rows.append(bytes(row))
    return png(width, width, rows)


def item(number):
    title, kind, overview, genre = TITLES[number % len(TITLES)]
    return {"id": 100 + number, "type": kind, "title": title, "year": str(2021 + number % 5),
            "rating": round(7.2 + (number % 7) / 10, 1), "runtime": 98 + number * 3 if kind == "movie" else 0,
            "genres": [genre], "overview": overview, "poster": "/p%d.jpg" % number,
            "backdrop": "/b%d.jpg" % number}


PROFILES = [("main", "Alex", "#5C5AA8"), ("sara", "Sara", "#B05E8A"), ("guest", "Guest", "#117A65")]


# What the Quick drawer changes; the fixture keeps it, so a read after a write says what was written.
SETTINGS = {"volume": 24, "muted": False, "backlight": 70, "sleepTimer": "off", "pictureMode": "cinema",
            "soundOutput": "tv_speaker"}
MODES = {"vivid": "Vivid", "normal": "Standard", "cinema": "Cinema", "game": "Game"}


def stats():
    return {"ok": True, "volume": SETTINGS["volume"], "muted": SETTINGS["muted"], "oled": {"care": True},
            "sleepTimer": SETTINGS["sleepTimer"], "device": {"id": "lg_tv", "name": "LG OLED65B3PUA", "model": "OLED65B3PUA.DUSQLJR"},
            "audio_output": "TV Speaker",
            "picture": {"mode": MODES.get(SETTINGS["pictureMode"], "Cinema"), "mode_raw": SETTINGS["pictureMode"],
                        "backlight": SETTINGS["backlight"],
                        "modes": [{"value": "vivid", "label": "Vivid"}, {"value": "normal", "label": "Standard"},
                                  {"value": "cinema", "label": "Cinema"}, {"value": "game", "label": "Game"}]},
            "sound": {"output": "TV Speaker", "output_raw": SETTINGS["soundOutput"],
                      "outputs": [{"id": "tv_speaker", "name": "TV Speaker"}, {"id": "external_arc", "name": "HDMI ARC"},
                                  {"id": "bt_soundbar", "name": "Bluetooth"}]}}


def details(number):
    full = item(number)
    full.update({"cast": [{"name": "Mara Voss"}, {"name": "Tomas Oyelaran"}, {"name": "June Haldane"}],
                 "trailer": "dQw4w9WgXcQ", "overview": full["overview"] + " " + full["overview"],
                 "similar": [item(number + k) for k in range(1, 9)]})
    return {"ok": True, "enabled": True, "netflixEnabled": False, "item": full}


def search(query):
    query = query.lower()
    found = [item(n) for n in range(len(TITLES)) if query in TITLES[n][0].lower() or query in TITLES[n][3].lower()]
    if len(query) == 2:  # a short query matches widely: every second title
        found = [item(n) for n in range(0, len(TITLES), 2)]
    return {"ok": True, "enabled": True, "items": found}


def catalogue(profiles=1, sort="custom"):
    apps = [{"id": a, "title": t, "icon": "/icon/%s" % a, "color": c, "kind": KINDS.get(a, "other")}
            for a, t, c in APPS]
    devices = [{"id": "hdmi%d" % n, "name": name, "port": n, "unused": unused}
               for n, name, unused in ((1, "Game Console", False), (2, "Streaming Stick", False),
                                       (3, "Nothing", True), (4, "PC", False))]
    return {"ok": True, "apps": apps, "devices": devices, "channels": [],
            "kinds": [{"key": key, "label": label} for key, label in KIND_LABELS],
            "order": {"app": [a for a, _, _ in APPS] + ["lg-home"], "dev": [d["id"] for d in devices], "ch": []},
            "layout": {"rows": ["app", "dev"], "hidden": {}, "showInputs": True, "showLive": False,
                       "sort": {"app": sort, "dev": "custom", "ch": "custom"},
                       "kinds": [key for key, _ in KIND_LABELS],
                       "showLgHome": True, "appNames": True, "clockFormat": "12h", "picture": False},
            "sources": {"channels": 0},
            "profiles": [{"id": pid, "name": name, "color": color, "active": n == 0}
                         for n, (pid, name, color) in enumerate(PROFILES[:profiles])],
            "wallpapers": {"available": False, "slots": {}, "revision": ""}}


def discover():
    return {"ok": True, "enabled": True, "titlesEnabled": True, "rows": [
        {"key": "trending", "items": [item(n) for n in range(8)]},
        {"key": "movies", "items": [item(n) for n in range(2, 10)]},
        {"key": "series", "items": [item(n) for n in range(4, 10)]}]}


class Handler(http.server.BaseHTTPRequestHandler):
    art = None
    beats = 0
    profiles = 1
    sort = "custom" # the apps row's order: custom, recent or kind

    def log_message(self, *_):
        pass

    def send(self, status, body, kind="application/json"):
        self.send_response(status)
        self.send_header("Content-Type", kind)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        try:
            self.wfile.write(body)
        except (BrokenPipeError, ConnectionResetError):
            pass

    def json(self, value, status=200):
        self.send(status, json.dumps(value).encode())

    def picture(self, name, hero):
        number = int("".join(c for c in name if c.isdigit()) or 0)
        if self.art:
            name = ("hero-%s.jpg" % ("harbor", "orbit", "ember", "north", "paper", "tide")[number % 6]
                    if hero else "card-%d.jpg" % (number % 10))
            path = os.path.join(self.art, name)
            if os.path.exists(path):
                with open(path, "rb") as file:
                    return self.send(200, file.read(), "image/jpeg")
        width, height = (640, 360) if hero else (224, 140)
        self.send(200, landscape(width, height, number), "image/png")

    def do_GET(self):
        url = urllib.parse.urlsplit(self.path)
        query = urllib.parse.parse_qs(url.query)
        if url.path == "/api/caps":
            self.json({"ok": True})
        elif url.path == "/api/home/catalogue":
            self.json(catalogue(self.profiles, self.sort))
        elif url.path == "/api/home/discover":
            self.json(discover())
        elif url.path == "/api/home/guide":
            self.json({"ok": True, "guide": {}})
        elif url.path == "/api/home/now":
            self.json({"ok": True, "kind": "none"})
        elif url.path == "/api/home/icon":
            name = query.get("id", [""])[0]
            index = [a for a, _, _ in APPS].index(name) if name in [a for a, _, _ in APPS] else 0
            self.send(200, icon(110, APPS[index][2], index), "image/png")
        elif url.path == "/api/home/image":
            target = urllib.parse.urlsplit(query.get("u", [""])[0]).path
            self.picture(os.path.basename(target), "w1280" in target)
        elif url.path == "/api/stats":
            self.json(stats())
        elif url.path == "/api/home/details":
            self.json(details(int(query.get("id", ["100"])[0]) - 100))
        elif url.path == "/api/home/search":
            self.json(search(query.get("q", [""])[0]))
        elif url.path == "/api/home/netflix":
            self.json({"ok": True, "netflixId": ""})
        elif url.path == "/api/home/recents":
            self.json({"ok": True, "apps": [{"id": a, "title": t, "image": False} for a, t, _ in APPS[:4]]})
        else:
            self.json({"ok": False}, 404)

    def do_POST(self):
        size = int(self.headers.get("Content-Length", "0"))
        raw = self.rfile.read(size)
        if self.path == "/api/control":
            try:
                body = json.loads(raw or b"{}")
            except ValueError:
                body = {}
            action, value = body.get("action"), body.get("value")
            keys = {"volume": "volume", "mute": "muted", "backlight": "backlight", "sleepTimer": "sleepTimer",
                    "pictureMode": "pictureMode", "soundOutput": "soundOutput"}
            if action in keys:
                SETTINGS[keys[action]] = value
            return self.json({"ok": True})
        if self.path == "/api/home/beat":
            self.json({"ok": True, "enabled": True, "build": "fixture@1", "layoutVersion": "1",
                       "wallpapers": {"available": False, "slots": {}, "revision": ""}})
        else:
            self.json({"ok": True})


@contextlib.contextmanager
def serve(art=None, port=0, profiles=1, sort="custom"):
    SETTINGS.update(volume=24, muted=False, backlight=70, sleepTimer="off", pictureMode="cinema",
                    soundOutput="tv_speaker")
    Handler.art = art
    Handler.profiles = profiles
    Handler.sort = sort
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
    parser.add_argument("--art")
    args = parser.parse_args()
    with serve(args.art, args.port) as origin:
        print(origin, flush=True)
        threading.Event().wait()


if __name__ == "__main__":
    sys.exit(main())
