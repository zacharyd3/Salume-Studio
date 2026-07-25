#!/usr/bin/env python3
"""
Salume Studio recorder / history API.

Runs inside the same container as nginx (see docker-entrypoint.sh) and gives the
web app the two things a static page can't do on its own:

  1. Always-on recording. It subscribes to the chamber's MQTT topics and writes
     samples to a SQLite database, so the "Condition history" chart has real
     history the moment you open the app - no need to leave a browser tab open
     to collect it. (The old behaviour sampled into per-browser localStorage.)

  2. Always-on alerting. It evaluates each reading against your target range and,
     when the chamber drifts out of range or the sensor drops offline, publishes
     an alert to MQTT - so an alert can fire with no browser open at all. This is
     opt-in and, by design, needs no Home Assistant token (see AlertEngine).

Everything it persists lives under the one bind-mounted data directory the
install script already mounts (/usr/share/nginx/html/data), so it survives image
rebuilds with no change to that script:

    data/history.db          the recorded time series (SQLite)
    data/monitor-config.json broker + thresholds, editable from the web app

nginx reverse-proxies /api/ to this service on 127.0.0.1:8090, so only port 80
is ever exposed to the host.
"""

import json
import os
import sqlite3
import threading
import time
from datetime import datetime, timezone

import paho.mqtt.client as mqtt
from flask import Flask, jsonify, request
from waitress import serve

# --- Paths & constants -----------------------------------------------------
# Default to the container's bind-mounted data dir; override with DATA_DIR for
# local testing. Both files live here so they persist across rebuilds.
DATA_DIR = os.environ.get("DATA_DIR", "/usr/share/nginx/html/data")
DB_PATH = os.path.join(DATA_DIR, "history.db")
CONFIG_PATH = os.path.join(DATA_DIR, "monitor-config.json")

LISTEN_HOST = "127.0.0.1"   # only nginx talks to us; never bind to the world
LISTEN_PORT = 8090

# The config file is the source of truth, but these defaults let the service come
# up and record on a fresh install with nothing configured. Env vars override the
# defaults so the container CAN be pointed at a different broker without editing a
# file, but the install script doesn't need to pass any.
DEFAULT_CONFIG = {
    "mqtt_host": os.environ.get("MQTT_HOST", "192.168.250.3"),
    "mqtt_port": int(os.environ.get("MQTT_PORT", "1883")),
    "mqtt_user": os.environ.get("MQTT_USER", ""),
    "mqtt_pass": os.environ.get("MQTT_PASS", ""),
    "topics": {
        "temperature": "charcuterie/monitor/temperature",
        "humidity": "charcuterie/monitor/humidity",
        "dewpoint": "charcuterie/monitor/dewpoint",
        "status": "charcuterie/monitor/status",
    },
    # How often a combined row is written. Readings arrive ~every 15s; one row a
    # minute is plenty of resolution for a chamber and keeps the DB tiny.
    "sample_interval_sec": 60,
    # Rows older than this are pruned nightly. ~13 months of minute samples is a
    # few hundred thousand rows - nothing for SQLite.
    "retention_days": 400,
    "alerts": {
        "enabled": False,
        "temp_min": 1.0,
        "temp_max": 5.0,
        "hum_min": 70.0,
        "hum_max": 85.0,
        # Sustain an out-of-range reading this long before alerting, so a defrost
        # cycle or a briefly opened door doesn't trip it.
        "hold_sec": 600,
        # No fresh reading for this long => the sensor is treated as offline.
        "offline_after_sec": 300,
        # Where the alert state is published. An HA automation (or anything else)
        # can subscribe and turn it into a phone push - no token stored here.
        "publish_topic": "charcuterie/monitor/alert",
    },
}


# --- Config load / save ----------------------------------------------------
_config_lock = threading.Lock()


def _deep_merge(base, override):
    """Recursively merge override into a copy of base (dicts only)."""
    out = dict(base)
    for k, v in (override or {}).items():
        if isinstance(v, dict) and isinstance(out.get(k), dict):
            out[k] = _deep_merge(out[k], v)
        else:
            out[k] = v
    return out


def load_config():
    """Return DEFAULT_CONFIG merged with whatever is on disk (disk wins)."""
    try:
        with open(CONFIG_PATH, "r", encoding="utf-8") as f:
            disk = json.load(f)
    except (FileNotFoundError, json.JSONDecodeError):
        disk = {}
    return _deep_merge(DEFAULT_CONFIG, disk)


def save_config(new_config):
    """Persist config atomically (temp file + rename on the same filesystem)."""
    merged = _deep_merge(DEFAULT_CONFIG, new_config)
    os.makedirs(DATA_DIR, exist_ok=True)
    tmp = CONFIG_PATH + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(merged, f, indent=2)
    os.replace(tmp, CONFIG_PATH)
    return merged


# --- Storage ---------------------------------------------------------------
class Store:
    """Thin SQLite wrapper. One combined row (temp/hum/dew) per sample tick."""

    def __init__(self, path):
        os.makedirs(os.path.dirname(path), exist_ok=True)
        # check_same_thread=False: the MQTT thread writes, Flask threads read.
        # A single lock serialises access, which is ample for this write rate.
        self._db = sqlite3.connect(path, check_same_thread=False)
        self._db.execute("PRAGMA journal_mode=WAL")
        self._lock = threading.Lock()
        with self._lock:
            self._db.execute(
                "CREATE TABLE IF NOT EXISTS readings ("
                "  ts   INTEGER PRIMARY KEY,"  # unix seconds; one row per tick
                "  temp REAL,"
                "  hum  REAL,"
                "  dew  REAL"
                ")"
            )
            self._db.commit()

    def insert(self, ts, temp, hum, dew):
        with self._lock:
            self._db.execute(
                "INSERT OR REPLACE INTO readings (ts, temp, hum, dew) VALUES (?,?,?,?)",
                (int(ts), temp, hum, dew),
            )
            self._db.commit()

    def history(self, since_ts, bucket_sec):
        """Return time-bucketed averages since since_ts, oldest first."""
        bucket_sec = max(1, int(bucket_sec))
        with self._lock:
            rows = self._db.execute(
                "SELECT (ts/?)*? AS b, AVG(temp), AVG(hum), AVG(dew) "
                "FROM readings WHERE ts >= ? GROUP BY b ORDER BY b",
                (bucket_sec, bucket_sec, int(since_ts)),
            ).fetchall()
        return [
            {
                "t": int(b),
                "temp": None if t is None else round(t, 2),
                "hum": None if h is None else round(h, 2),
                "dew": None if d is None else round(d, 2),
            }
            for (b, t, h, d) in rows
        ]

    def prune(self, older_than_ts):
        with self._lock:
            self._db.execute("DELETE FROM readings WHERE ts < ?", (int(older_than_ts),))
            self._db.commit()


# --- Alerting (token-free) -------------------------------------------------
class AlertEngine:
    """
    Evaluates readings against the target range and publishes an alert *state*
    to MQTT. It deliberately does NOT call Home Assistant's REST API, so no
    long-lived token is ever stored on the server: whoever wants a phone push
    (an HA automation, Node-RED, anything) subscribes to the alert topic and
    already holds its own notify credentials. See home-assistant/README.md.

    Published payload (retained) on <alerts.publish_topic>, e.g.:
        {"state":"temp_high","temp":7.2,"hum":80,"dew":3.9,"ts":...}
    state is one of: ok, temp_high, temp_low, hum_high, hum_low, offline.
    """

    def __init__(self, client, get_config):
        self._client = client
        self._get_config = get_config
        self._pending = None          # (state, since_ts) awaiting the hold
        self._published_state = None  # last state actually published

    def evaluate(self, latest):
        cfg = self._get_config()["alerts"]
        if not cfg.get("enabled"):
            return

        now = time.time()
        state = self._classify(cfg, latest, now)

        # Debounce: a candidate must persist for hold_sec before we publish it.
        # "ok" and "offline" publish immediately - you want the all-clear and a
        # dead sensor without delay; range excursions wait out the hold.
        if state != self._pending_state():
            self._pending = (state, now)

        immediate = state in ("ok", "offline")
        held_long_enough = (
            self._pending is not None
            and now - self._pending[1] >= cfg.get("hold_sec", 600)
        )
        if (immediate or held_long_enough) and state != self._published_state:
            self._publish(cfg, state, latest, now)

    def _pending_state(self):
        return self._pending[0] if self._pending else None

    def _classify(self, cfg, latest, now):
        ts = latest.get("ts")
        if ts is None or now - ts > cfg.get("offline_after_sec", 300):
            return "offline"
        if latest.get("status") == "offline":
            return "offline"
        temp, hum = latest.get("temp"), latest.get("hum")
        if temp is not None and temp > cfg["temp_max"]:
            return "temp_high"
        if temp is not None and temp < cfg["temp_min"]:
            return "temp_low"
        if hum is not None and hum > cfg["hum_max"]:
            return "hum_high"
        if hum is not None and hum < cfg["hum_min"]:
            return "hum_low"
        return "ok"

    def _publish(self, cfg, state, latest, now):
        payload = json.dumps(
            {
                "state": state,
                "temp": latest.get("temp"),
                "hum": latest.get("hum"),
                "dew": latest.get("dew"),
                "ts": int(now),
            }
        )
        self._client.publish(cfg["publish_topic"], payload, qos=1, retain=True)
        self._published_state = state
        print(f"[alert] {state} -> {cfg['publish_topic']}", flush=True)


# --- The recorder: MQTT in, SQLite out -------------------------------------
class Recorder:
    def __init__(self, store):
        self._store = store
        self._latest = {"temp": None, "hum": None, "dew": None,
                        "status": None, "ts": None}
        self._latest_lock = threading.Lock()
        self._last_write = 0.0

        self._client = mqtt.Client(
            client_id="salume-recorder",
            callback_api_version=mqtt.CallbackAPIVersion.VERSION2,
        )
        self._client.on_connect = self._on_connect
        self._client.on_message = self._on_message
        self._alerts = AlertEngine(self._client, load_config)

    # -- lifecycle --
    def start(self):
        cfg = load_config()
        if cfg["mqtt_user"]:
            self._client.username_pw_set(cfg["mqtt_user"], cfg["mqtt_pass"])
        # loop_start reconnects on its own, so a broker that's down at boot (or
        # that restarts later) is handled transparently.
        self._client.reconnect_delay_set(min_delay=1, max_delay=30)
        try:
            self._client.connect_async(cfg["mqtt_host"], cfg["mqtt_port"], keepalive=60)
        except Exception as e:  # DNS/socket errors shouldn't crash startup
            print(f"[mqtt] initial connect deferred: {e}", flush=True)
        self._client.loop_start()

    def _on_connect(self, client, userdata, flags, reason_code, properties=None):
        topics = load_config()["topics"]
        for t in topics.values():
            client.subscribe(t)
        print(f"[mqtt] connected ({reason_code}); subscribed {list(topics.values())}",
              flush=True)

    def _on_message(self, client, userdata, msg):
        topics = load_config()["topics"]
        payload = msg.payload.decode("utf-8", "replace").strip()

        with self._latest_lock:
            if msg.topic == topics["status"]:
                self._latest["status"] = payload
            else:
                try:
                    value = float(payload)
                except ValueError:
                    return
                if msg.topic == topics["temperature"]:
                    self._latest["temp"] = value
                elif msg.topic == topics["humidity"]:
                    self._latest["hum"] = value
                elif msg.topic == topics["dewpoint"]:
                    self._latest["dew"] = value
            self._latest["ts"] = time.time()
            snapshot = dict(self._latest)

        self._maybe_record(snapshot)
        self._alerts.evaluate(snapshot)

    def _maybe_record(self, snapshot):
        """Write one combined row per sample_interval once we have a temp+hum."""
        if snapshot["temp"] is None or snapshot["hum"] is None:
            return
        interval = load_config()["sample_interval_sec"]
        now = time.time()
        if now - self._last_write < interval:
            return
        self._last_write = now
        self._store.insert(now, snapshot["temp"], snapshot["hum"], snapshot["dew"])

    def latest(self):
        with self._latest_lock:
            return dict(self._latest)

    @property
    def client(self):
        return self._client


# --- Retention (nightly prune) ---------------------------------------------
def retention_loop(store):
    while True:
        try:
            days = load_config()["retention_days"]
            store.prune(time.time() - days * 86400)
        except Exception as e:
            print(f"[retention] {e}", flush=True)
        time.sleep(86400)


# --- HTTP API --------------------------------------------------------------
def build_app(store, recorder):
    app = Flask(__name__)

    @app.get("/api/health")
    def health():
        return jsonify({"ok": True, "time": datetime.now(timezone.utc).isoformat()})

    @app.get("/api/latest")
    def latest():
        cfg = load_config()
        snap = recorder.latest()
        stale = (
            snap["ts"] is None
            or time.time() - snap["ts"] > cfg["alerts"]["offline_after_sec"]
        )
        return jsonify({**snap, "stale": stale})

    @app.get("/api/history")
    def history():
        # ?hours=72&bucket=300  (bucket in seconds; defaults scale with range)
        hours = max(1, min(int(request.args.get("hours", 72)), 24 * 400))
        default_bucket = 60 if hours <= 24 else 300 if hours <= 168 else 1800
        bucket = int(request.args.get("bucket", default_bucket))
        since = time.time() - hours * 3600
        pts = store.history(since, bucket)
        return jsonify({"bucket": bucket, "hours": hours, "points": pts})

    @app.get("/api/config")
    def get_config():
        # Never hand the broker password back out to the browser.
        cfg = load_config()
        cfg["mqtt_pass"] = "" if not cfg["mqtt_pass"] else "********"
        return jsonify(cfg)

    @app.post("/api/config")
    def post_config():
        incoming = request.get_json(force=True, silent=True) or {}
        # A masked password coming back from the UI means "leave it unchanged".
        if incoming.get("mqtt_pass") == "********":
            incoming.pop("mqtt_pass")
        with _config_lock:
            merged = save_config(incoming)
        merged["mqtt_pass"] = "" if not merged["mqtt_pass"] else "********"
        return jsonify({"saved": True, "config": merged})

    return app


def main():
    os.makedirs(DATA_DIR, exist_ok=True)
    store = Store(DB_PATH)
    recorder = Recorder(store)
    recorder.start()
    threading.Thread(target=retention_loop, args=(store,), daemon=True).start()
    app = build_app(store, recorder)
    print(f"[http] serving on {LISTEN_HOST}:{LISTEN_PORT}", flush=True)
    serve(app, host=LISTEN_HOST, port=LISTEN_PORT, threads=8)


if __name__ == "__main__":
    main()
