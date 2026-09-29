#!/usr/bin/env python3
"""Local stand-in for the ESP32's REST API, serving the real main/web_ui/
files with synthetic backing data -- so the web UI can be iterated on and
visually verified in a browser without flashing/rebooting a real device for
every change. Stdlib only, no dependencies.

Usage:
  python3 tools/mock_server.py [port]   # default port 8000

Then open http://localhost:8000/ in a browser. GET endpoints return
synthetic data (a plausible 24h burn curve plus a couple of seeded
historical events); POST /api/event and POST /api/settings are accepted
and reflected back by subsequent GETs, exactly like the real device, so
the full dashboard/graph/settings flow works end to end -- including
logging a brand new event via the real "Log what you did" button, which
lands at age 0 in /api/history immediately, the same edge case that
exposed the "event plotted past the end of the graph" bug this tool was
built to let us reproduce and verify without real hardware.
"""
import http.server
import json
import random
import time
from pathlib import Path
from urllib.parse import urlparse

WEB_UI_DIR = Path(__file__).resolve().parent.parent / "main" / "web_ui"

# Mirrors rest_api.c's EVENTS[] exactly -- keep in sync by hand if that list changes.
EVENTS = [
    {"slug": "cold_start", "label": "Cold Start"},
    {"slug": "opened_stove", "label": "Opened Stove"},
    {"slug": "added_wood", "label": "Added Wood"},
    {"slug": "damper_up", "label": "Damper Up"},
    {"slug": "damper_down", "label": "Damper Down"},
    {"slug": "burning_optimally", "label": "Burning Optimally"},
    {"slug": "roaring", "label": "Stove Roaring"},
    {"slug": "dying_down", "label": "Dying Down"},
    {"slug": "fire_out", "label": "Fire Out"},
    {"slug": "stove_off", "label": "Stove Off"},
]
EVENT_LABEL = {e["slug"]: e["label"] for e in EVENTS}

settings = {
    "log_heartbeat_min": 15,
    "zone_cold_max_c": 150,
    "zone_optimal_max_c": 280,
    "fast_rise_c_per_min": 20,
    "thermocouple_deadband_c": 5,
    "google_sheets_webhook_url": "",
    "google_sheets_secret": "",
    "idle_pulse_period_ms": 8000,
    "fast_pulse_period_ms": 1400,
    "rate_deadband_c_per_min": 3,
    "color_transition_exponent": 3,
}

server_start = time.monotonic()


def zone_code(temp_c):
    if temp_c <= settings["zone_cold_max_c"]:
        return 1
    if temp_c <= settings["zone_optimal_max_c"]:
        return 2
    return 3


def zone_name(temp_c):
    return {1: "cold", 2: "optimal", 3: "hot"}[zone_code(temp_c)]


def synth_temp_at_age_s(age_s):
    """A plausible 24h burn curve: cold start ~4h ago, climbs to a hot
    overshoot, settles into the optimal band, tapers off toward the end --
    walking BACKWARD from "now" (age 0) since that's the direction the
    real device's own history covers (~24h back from whenever it's asked)."""
    hours_ago = age_s / 3600.0
    if hours_ago > 4.2:
        return 21.0 + random.uniform(-0.3, 0.3)  # cold, pre-burn
    t = 4.2 - hours_ago  # 0..4.2, hours into the burn
    if t < 0.5:
        base = 21 + (240 - 21) * (t / 0.5)  # cold start ramp
    elif t < 0.8:
        base = 240 + (300 - 240) * ((t - 0.5) / 0.3)  # brief hot overshoot
    elif t < 3.5:
        base = 300 - (300 - 190) * ((t - 0.8) / 2.7)  # settle into optimal
        base = max(base, 190)
    else:
        base = 190 - (190 - 60) * ((t - 3.5) / 0.7)  # dying down near "now"
    return round(base + random.uniform(-2, 2), 1)


# Seeded once at startup so repeated /api/history calls stay stable within
# one server run (only the live "now" endpoint and freshly-logged events move).
random.seed(42)
HISTORY_SAMPLES = []  # [(age_s, temp_c, rate, zone_code), ...], oldest first
_prev_temp = None
for _age in range(24 * 3600, -1, -240):  # every 4min, matching HISTORY_PUSH_EVERY_N's real cadence
    _t = synth_temp_at_age_s(_age)
    _rate = 0.0 if _prev_temp is None else round((_t - _prev_temp) / 4.0, 2)  # per-minute, 4min steps
    HISTORY_SAMPLES.append((_age, _t, _rate, zone_code(_t)))
    _prev_temp = _t

# A couple of plausible historical events, at fixed ages that never drift
# (matching HISTORY_SAMPLES' own static ages -- see its comment above) --
# `live=False` entries always report `age` as-is. Events logged for real via
# POST /api/event during a test session get `live=True` and `age` holding a
# time.monotonic() timestamp instead, aged dynamically in
# _events_with_live_ages() below -- this is the exact edge case (an event
# logged seconds ago, far more recent than any 4-minute-cadence sample) that
# originally exposed the "event plotted past the graph's right edge" bug.
logged_events = [
    {"age": 3.9 * 3600, "slug": "cold_start", "live": False},
    {"age": 2.6 * 3600, "slug": "added_wood", "live": False},
    {"age": 1.1 * 3600, "slug": "burning_optimally", "live": False},
]


def current_reading():
    age_s = time.monotonic() - server_start
    temp = synth_temp_at_age_s(max(0, 600 - age_s))  # keep drifting for ~10min then hold near the end of the curve
    prev_temp = synth_temp_at_age_s(max(0, 600 - age_s) + 30)
    rate = round((temp - prev_temp) / 0.5, 2)
    return {
        "thermocouple_ok": True,
        "thermocouple_c": temp,
        "cold_junction_c": round(temp * 0.3 + 20, 1),
        "thermocouple_rate_c_per_min": rate,
        "thermocouple_zone": zone_name(temp),
    }


class Handler(http.server.BaseHTTPRequestHandler):
    def _json(self, obj, status=200):
        body = json.dumps(obj).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _file(self, rel_path, content_type):
        f = WEB_UI_DIR / rel_path
        if not f.is_file():
            self.send_error(404, f"{rel_path} not found in {WEB_UI_DIR}")
            return
        body = f.read_bytes()
        self.send_response(200)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        path = urlparse(self.path).path

        static = {
            "/": ("dashboard.html", "text/html"),
            "/dashboard.js": ("dashboard.js", "application/javascript"),
            "/dashboard.css": ("dashboard.css", "text/css"),
            "/alpinejs.min.js": ("alpinejs.min.js", "application/javascript"),
            "/fonts/caprasimo-400.woff2": ("fonts/caprasimo-400.woff2", "font/woff2"),
            "/fonts/figtree-400.woff2": ("fonts/figtree-400.woff2", "font/woff2"),
            "/fonts/figtree-600.woff2": ("fonts/figtree-600.woff2", "font/woff2"),
            "/fonts/figtree-700.woff2": ("fonts/figtree-700.woff2", "font/woff2"),
        }
        if path in static:
            self._file(*static[path])
            return

        if path == "/api/reading":
            self._json(current_reading())
            return

        if path == "/api/settings":
            self._json(settings)
            return

        if path == "/api/events":
            self._json(EVENTS)
            return

        if path == "/api/history":
            now_s = int(time.monotonic() - server_start)
            samples = [[age_s, temp, rate, zc] for (age_s, temp, rate, zc) in HISTORY_SAMPLES]
            events = [[round(age_s), EVENT_LABEL[slug]] for (age_s, slug) in _events_with_live_ages()]
            self._json({"now_s": now_s, "samples": samples, "events": events})
            return

        self.send_error(404)

    def do_POST(self):
        path = urlparse(self.path).path
        length = int(self.headers.get("Content-Length", 0))
        body = self.rfile.read(length) if length else b""
        try:
            data = json.loads(body) if body else {}
        except json.JSONDecodeError:
            self._json({"error": "invalid JSON"}, 400)
            return

        if path == "/api/settings":
            for k in settings:
                if k in data:
                    settings[k] = data[k]
            self._json({"success": True})
            return

        if path == "/api/event":
            slug = data.get("event")
            if slug not in EVENT_LABEL:
                self._json({"error": "unknown event"}, 400)
                return
            # age 0 relative to *now* -- the exact case that exposed the
            # "event plotted past the graph's right edge" bug: logged
            # in-browser via the real event sheet, immediately reflected
            # in the next /api/history poll at age ~0.
            logged_events.append({"age": time.monotonic(), "slug": slug, "live": True})
            print(f"[mock] logged event: {slug}")
            self._json({"success": True})
            return

        self.send_error(404)

    def log_message(self, format, *args):  # noqa: A002 -- matches BaseHTTPRequestHandler's own signature
        pass  # the default per-request access log is just noise here


def _events_with_live_ages():
    """Resolves each logged_events entry to a real (age_s, slug) pair --
    static ages for the seeded entries (never drift, matching
    HISTORY_SAMPLES), computed-from-real-time ages for ones logged live via
    POST /api/event (see logged_events' own comment above)."""
    now_mono = time.monotonic()
    return [
        (now_mono - e["age"] if e["live"] else e["age"], e["slug"])
        for e in logged_events
    ]


if __name__ == "__main__":
    import sys
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8000
    if not WEB_UI_DIR.is_dir():
        raise SystemExit(f"web_ui directory not found at {WEB_UI_DIR}")
    print(f"Mock flu-monitor web UI server: http://localhost:{port}/")
    print("(GET endpoints return synthetic data; POST /api/event and /api/settings are accepted and reflected back)")
    http.server.ThreadingHTTPServer(("localhost", port), Handler).serve_forever()
