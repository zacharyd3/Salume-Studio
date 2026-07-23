# Salume Studio 🥓

An offline-first charcuterie **curing calculator** and **drying tracker**, plus
the makings of a live **curing-chamber monitor** fed by a cheap ESP8266 sensor.

The app itself is a single self-contained file — [`charcuterie.html`](charcuterie.html) —
with no build step and no runtime dependencies. You can double-click it to run
it locally, or serve it from the NGINX container below so it's reachable from a
phone or tablet by the fridge.

## Run with Docker + NGINX

```bash
# build & start (serves on http://<host>:8080/)
docker compose up -d --build

# stop
docker compose down
```

Then open `http://localhost:8080/` (or `http://<host-ip>:8080/` from another
device on your LAN). Change the host port in [`docker-compose.yml`](docker-compose.yml)
if `8080` is already in use.

To rebuild after editing `charcuterie.html`:

```bash
docker compose up -d --build
```

### Without compose

```bash
docker build -t salume-studio .
docker run -d --name salume-studio -p 8080:80 --restart unless-stopped salume-studio
```

## Data, backups & auto-sync

Your curing entries live in the browser's `localStorage`, but the app also
**auto-syncs** them to a JSON file on the host so the data survives browser
clears and is shared across every device that opens the app.

- On load, the app fetches `data/curing.json` from the server (the shared
  source of truth) and shows it.
- On every change it writes the file back via **nginx WebDAV** (a debounced
  `PUT`). The little pill at the top of **My Curing** shows the sync state
  (`Synced ✓`, `Saving…`, or `Local only` when opened as a bare file).

The file is bind-mounted to the host, so it lands at:

```
/mnt/user/appdata/salume-studio/curing.json
```

That directory must exist and be writable by the container:

```bash
mkdir -p /mnt/user/appdata/salume-studio
chmod 777 /mnt/user/appdata/salume-studio     # unraid appdata
```

> No auth sits in front of the data file — anyone on your LAN can read or
> overwrite it. That's fine for a single-user homelab; `DELETE` is disabled.

### Export / Import

**My Curing** has **Export** (download a JSON backup) and **Import** (load one
back, merged by entry). This is also how you migrate data from an old browser:
export there, import here.

To pull data out of an **old** app that predates the Export button, run this in
that browser's console and import the downloaded file:

```js
(()=>{const d=localStorage.getItem('salume.recipes.v1')||'[]';
const b=new Blob([JSON.stringify({app:'salume-studio',version:1,recipes:JSON.parse(d)},null,2)],{type:'application/json'});
const a=document.createElement('a');a.href=URL.createObjectURL(b);a.download='salume-studio-backup.json';a.click();})()
```

## The fridge sensor (live chamber monitor)

A NodeMCU V3 (ESP8266) with a DHT sensor lives on a wire in the curing fridge and
publishes **retained** readings to a Mosquitto broker. The sketch lives in
[`firmware/charcuterie_monitor/`](firmware/charcuterie_monitor/charcuterie_monitor.ino) —
fill in your Wi-Fi/broker details at the top before flashing.

| Topic                               | Value                       |
| ----------------------------------- | --------------------------- |
| `charcuterie/monitor/temperature`   | °C                          |
| `charcuterie/monitor/humidity`      | % RH                        |
| `charcuterie/monitor/status`        | `online` / `offline` (LWT)  |

The **Curing Chamber** panel at the top of the My Curing tab subscribes to these
and shows live temp/humidity with target bands — green inside your target, amber
just outside, red beyond. The ranges are **configurable** in the panel's settings
and default to **in-fridge drying** (1–5 °C / 70–85 % RH); bump them to ~11–15 °C
for a dedicated curing chamber. If no fresh reading arrives for 3 minutes, or the
sensor's `status` last-will reports `offline`, the tiles grey out and the panel
says the sensor is offline instead of showing stale numbers as "Live". Because a
browser can't open a raw MQTT/TCP socket (port 1883), it speaks **MQTT over
WebSockets** — a tiny client is inlined in the page, no library needed.

### 1. Give Mosquitto a websockets listener

`mosquitto.conf`:

```
listener 1883
listener 9001
protocol websockets
```

(A read-only user scoped to `charcuterie/#` is a good idea for the dashboard —
you enter its credentials in the panel, and they never leave your browser.)
Restart Mosquitto.

### 2. Point the app at the broker

The nginx image already proxies same-origin `ws://<host>:<port>/mqtt` through to
the broker — see the `location /mqtt` block in
[`nginx/default.conf`](nginx/default.conf); update its `proxy_pass` if your broker
isn't at `192.168.250.3:9001`, then rebuild. Proxying this way also works when the
broker sits on a VLAN the browser can't reach directly but the container can.

In the app: **My Curing → 🌡️ Curing Chamber → ⚙️**, set the topics if they differ
from the defaults, leave MQTT user/password blank for an anonymous broker (the
WebSocket URL defaults to the `/mqtt` proxy; use `ws://<broker>:9001` for a direct
connection), and **Save & Connect**. Retained messages mean the last reading shows
up immediately. Settings persist in `localStorage`.

### Home Assistant

The firmware publishes **MQTT discovery** payloads under `homeassistant/`, so once
the MQTT integration is connected to the broker, HA auto-creates the
`sensor.charcuterie_temperature` / `sensor.charcuterie_humidity` entities (with
availability from the `status` last-will) — no YAML needed. Add a `history-graph`
card with those two entities for temp/humidity history and long-term trends.

> **If HA never creates the entities** (but temp/humidity still show up in MQTT
> Explorer), it's almost always the PubSubClient buffer. That buffer must hold
> the **whole packet** — payload (~500 B) plus the config topic (~59 B) plus MQTT
> header (~7 B), so ~570 B — and its default is just **256 bytes**, so the
> discovery publishes fail silently while the tiny reading payloads go through.
> The sketch calls `client.setBufferSize(1024)` in `setup()` to fix this — don't
> shrink it below ~600.

### Updating the firmware over Wi-Fi (OTA)

So you don't have to pull the board off the fridge every time, the sketch enables
**over-the-air (OTA) updates** via the `ArduinoOTA` library (bundled with the
ESP8266 core — nothing to install).

1. **Flash once over USB** with this firmware. Set `ota_hostname` and, importantly,
   an `ota_password` in the `EDIT THESE` block first.
2. After it boots and joins Wi-Fi, the board advertises itself over mDNS. In the
   Arduino IDE, **Tools → Port** now lists a **Network port** named
   `charcuterie-monitor` (your `ota_hostname`).
3. Select that port and upload as usual — it goes over Wi-Fi and asks for the OTA
   password. The board publishes `offline`, applies the update, and reboots.

Every later change is a wireless upload; USB is only needed for the very first
flash (or if OTA ever gets bricked). OTA stays responsive even while the MQTT
broker is unreachable, so a bad broker can't lock you out of pushing new firmware.
The default NodeMCU flash layout (**4MB, FS:2MB, OTA:~1019KB**) leaves plenty of
room; if the IDE complains there isn't enough space, pick a layout with a smaller
filesystem under **Tools → Flash Size**.

### Watching the logs over Wi-Fi (telnet)

The Arduino **Serial Monitor only works over USB** — pick the network port and it
just says "No monitor available for the port protocol network." So the sketch also
runs a **telnet console on port 23** that mirrors everything it prints. Once the
board is on Wi-Fi:

```bash
telnet <board-ip> 23        # or: telnet charcuterie-monitor.local 23
```

On Windows the built-in telnet client is off by default — enable it (*Turn Windows
features on or off → Telnet Client*) or just use **PuTTY** in **Raw** mode on port
23. You'll see the same `Temperature: … / Humidity: … / Published MQTT data.` lines
every 15 s, so you can confirm it's alive without plugging anything in. It's
output-only (one client at a time) and stays responsive even while the broker is
down.

> **Note on the sensor:** a DHT11 is fine for proving the pipeline but is only
> ±5% RH and unreliable above ~90% RH — the high end that matters for curing.
> A DHT22/AM2302 or SHT31 is worth the swap before trusting the numbers.

## Repository layout

```
charcuterie.html      # the entire app (calculator, tracker, chamber monitor)
icon.png              # favicon + unraid Docker icon
Dockerfile            # nginx:alpine serving the app
docker-compose.yml    # one-command build/run + data volume
nginx/default.conf    # static serving, WebDAV data sync, MQTT WS proxy
firmware/             # ESP8266 sketch for the DHT fridge sensor
```
