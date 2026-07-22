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

## The fridge sensor (work in progress)

A NodeMCU V3 (ESP8266) with a DHT sensor lives on a wire in the curing fridge and
publishes retained readings to a Mosquitto broker:

| Topic                              | Value            |
| ---------------------------------- | ---------------- |
| `charcuterie/filetto/temperature`  | °C               |
| `charcuterie/filetto/humidity`     | % RH             |

Because a browser can't open a raw MQTT/TCP socket (port 1883), the app will read
the broker over **MQTT-over-WebSockets**. NGINX can reverse-proxy that same-origin
so the page connects to `ws://<host>:8080/mqtt`:

1. Give Mosquitto a websockets listener (`mosquitto.conf`):

   ```
   listener 1883
   listener 9001
   protocol websockets
   ```

2. Uncomment the `location /mqtt { … }` block in
   [`nginx/default.conf`](nginx/default.conf) and point `proxy_pass` at that
   listener (e.g. `http://192.168.250.3:9001`), then rebuild.

The in-app monitor panel that subscribes to these topics is the next piece to
build on top of this.

> **Note on the sensor:** a DHT11 is fine for proving the pipeline but is only
> ±5% RH and unreliable above ~90% RH — the high end that matters for curing.
> A DHT22/AM2302 or SHT31 is worth the swap before trusting the numbers.

## Repository layout

```
charcuterie.html      # the entire app (calculator + drying tracker)
Dockerfile            # nginx:alpine serving the app
docker-compose.yml    # one-command build/run
nginx/default.conf    # static serving + optional MQTT WebSocket proxy
```
