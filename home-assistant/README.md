# Home Assistant — curing-chamber alerts

The web app shows live chamber readings and an in-panel banner when they drift
out of range, but it **can't push a phone notification** on a plain-HTTP LAN:
browser notifications (and the offline service worker) require a secure context
— HTTPS or `localhost` — which this app isn't served over by design (no login,
no reverse proxy).

Home Assistant has no such limitation. Since the ESP8266 already announces its
sensors to HA over MQTT discovery, a small automation package can push an alert
to the **companion app** on your phone whenever the fridge goes out of range or
the sensor drops offline — whether or not any browser is open.

## What you get

[`charcuterie_alerts.yaml`](charcuterie_alerts.yaml) is a self-contained HA
**package** that adds:

- **Four tunable sliders** (Helpers) for the temp/humidity target range,
  pre-seeded to the app's in-fridge defaults (1–5 °C / 70–85 %).
- **Temperature out of range** — pushes after the reading sits above/below the
  target for 10 minutes (so a defrost cycle or an open door doesn't nag you).
- **Humidity out of range** — same, on the humidity sensor.
- **Sensor offline** — pushes if either reading goes unavailable for 5 minutes
  (the ESP's last-will publishes `offline`, which turns the sensors unavailable).
- **All-clear** — one notification when both readings are back in range.

## Install

1. Copy `charcuterie_alerts.yaml` into your HA config at
   `<config>/packages/charcuterie_alerts.yaml`.
2. Make sure `configuration.yaml` loads the packages directory:

   ```yaml
   homeassistant:
     packages: !include_dir_named packages
   ```

3. Open the file and replace every `notify.mobile_app_your_phone` with your own
   notify action. Find it under **Developer Tools → Actions** and search
   `notify` (e.g. `notify.mobile_app_pixel_9`); use `notify.notify` to hit every
   registered device at once.
4. **Developer Tools → YAML → Reload all YAML configuration** (or restart HA).

## Notes

- The automations target `sensor.charcuterie_temperature` and
  `sensor.charcuterie_humidity` — the entity IDs Home Assistant derives from the
  firmware's discovery payload. If yours differ, adjust them throughout.
- Tune the range any time from **Settings → Devices & Services → Helpers**; no
  YAML editing needed. (With `initial:` set, a restart resets a slider to the
  default — remove that line on a helper to make a hand-tuned value stick.)
- The `tag:`/`channel:` keys let the Android companion app replace an earlier
  warning with the all-clear instead of stacking notifications.
