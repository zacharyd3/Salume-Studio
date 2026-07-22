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
publishes **retained** readings to a Mosquitto broker:

| Topic                              | Value            |
| ---------------------------------- | ---------------- |
| `charcuterie/filetto/temperature`  | °C               |
| `charcuterie/filetto/humidity`     | % RH             |

The **Curing Chamber** panel at the top of the Calculator tab subscribes to these
and shows live temp/humidity with target bands (green 11–15 °C / 75–85 % RH, amber
marginal, red out of range). Because a browser can't open a raw MQTT/TCP socket
(port 1883), it speaks **MQTT over WebSockets** — a tiny client is inlined in the
page, no library needed.

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

In the app: **Calculator → 🌡️ Curing Chamber → ⚙️**, enter the MQTT user/password
(the WebSocket URL defaults to the `/mqtt` proxy; use `ws://<broker>:9001` for a
direct connection), and **Save & Connect**. Retained messages mean the last
reading shows up immediately. Settings persist in `localStorage`.

> **Note on the sensor:** a DHT11 is fine for proving the pipeline but is only
> ±5% RH and unreliable above ~90% RH — the high end that matters for curing.
> A DHT22/AM2302 or SHT31 is worth the swap before trusting the numbers.
>
> **Firmware tip:** the sketch's `delay(60000)` blocks `client.loop()` for the
> whole minute, so with the default 15 s keepalive the broker may drop the
> connection between publishes. Retained messages hide it from the app, but a
> non-blocking `millis()` timer that keeps calling `client.loop()` is tidier.

## Repository layout

```
charcuterie.html      # the entire app (calculator, tracker, chamber monitor)
Dockerfile            # nginx:alpine serving the app
docker-compose.yml    # one-command build/run + data volume
nginx/default.conf    # static serving, WebDAV data sync, MQTT WS proxy
```
