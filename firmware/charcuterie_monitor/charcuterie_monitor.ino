/*
 * Salume Studio - Curing Chamber Monitor (NodeMCU V3 / ESP8266 + DHT)
 *
 * Publishes retained temperature/humidity (plus a derived dew point and
 * Wi-Fi-signal/uptime/IP diagnostics) to an MQTT broker and announces itself to
 * Home Assistant via MQTT discovery. A retained "online" message
 * plus a Last-Will "offline" on charcuterie/monitor/status drive availability
 * in both Home Assistant and the Curing Chamber panel in charcuterie.html.
 *
 * The broker connection is anonymous (no username/password). If your broker
 * requires auth, that user must also be allowed to publish under homeassistant/#
 * or discovery will silently fail even though the readings still go through.
 *
 * Supports over-the-air (OTA) updates: after the first USB flash, the board
 * appears as a network port in the Arduino IDE and every later upload goes
 * over Wi-Fi - no need to unplug it from the fridge. See setupOTA().
 *
 * Console over Wi-Fi: the serial monitor only works over USB, so the sketch
 * also mirrors everything it logs to a telnet server on port 23. Watch it with
 *   telnet <board-ip> 23      (or PuTTY in "Raw" mode, port 23)
 * Console output is plain ASCII so it renders in any terminal. See handleTelnet().
 *
 * Libraries:
 *   - "DHT sensor library" by Adafruit (+ "Adafruit Unified Sensor")
 *   - "PubSubClient" by Nick O'Leary
 *   - ArduinoOTA + ESP8266mDNS (both bundled with the ESP8266 core - no install)
 *
 * IMPORTANT: PubSubClient's default packet buffer is 256 bytes, but the Home
 * Assistant discovery payloads below are ~500 bytes. Without setBufferSize()
 * the discovery publishes silently fail (publish() returns false) and HA never
 * creates the entities - even though the small temperature/humidity payloads
 * still publish fine. See setup().
 */

#include <DHT.h>
#include <ESP8266WiFi.h>
#include <ESP8266mDNS.h>
#include <ArduinoOTA.h>
#include <PubSubClient.h>
#include <math.h>   // logf() for the dew-point calculation

// ============================ EDIT THESE =============================
// Your Wi-Fi and broker details. Keep real credentials out of git - these
// are placeholders; fill them in on the copy you flash.
const char* ssid        = "YOUR_WIFI_SSID";
const char* password    = "YOUR_WIFI_PASSWORD";
const char* mqtt_server = "192.168.250.3";

// OTA (over-the-air updates). The board shows up under Tools > Port as a
// "Network port" named ota_hostname. Set an OTA password so only you can push
// firmware to it; leave it "" to allow unauthenticated updates on your LAN.
const char* ota_hostname = "charcuterie-monitor";
const char* ota_password = "CHANGE_ME_OTA_PASSWORD";
// ====================================================================

// How often to read the sensor and publish. Readings are retained, so the
// last value is always available instantly to anyone who connects; this just
// controls how fresh the "live" number is. Curing chambers move slowly, so
// there's no need to hammer it.
const unsigned long PUBLISH_INTERVAL_MS = 15000;   // 15 seconds

// Re-announce discovery + "online" this often so Home Assistant re-creates the
// entities on its own after a broker restart or a cleared retained message.
const unsigned long REANNOUNCE_INTERVAL_MS = 300000;   // 5 minutes

// PubSubClient's buffer must hold the WHOLE packet, not just the payload:
// a discovery payload (~500 B) + its config topic (~59 B) + MQTT header (~7 B)
// is ~570 B, so 512 is too small and the publish silently fails. 1024 leaves
// comfortable headroom.
const uint16_t MQTT_BUFFER_SIZE = 1024;

// The DHT sits at the end of a cable plumbed into the fridge, and a one-wire
// sensor on a long run throws the occasional corrupted frame (a NaN read).
// One bad read isn't news, so tolerate a few in a row before deciding the
// sensor is genuinely down. The retry is cross-cycle, not a tight inline loop:
// the DHT library caches within its sampling interval, so re-reading faster
// than PUBLISH_INTERVAL_MS just hands back the same NaN. Each 15s cycle is a
// fresh attempt.
const uint8_t SENSOR_FAIL_THRESHOLD = 4;   // consecutive failed cycles before flagging offline

// Spike rejection. A one-wire sensor on a long cable run occasionally returns a
// frame that DECODES to a wrong number rather than to NaN, so it slips past the
// isnan() guard and lands in Home Assistant as a sharp glitch. Two cheap gates
// catch those: reject anything physically implausible outright, and reject a
// value that jumps further in a single 15s cycle than the chamber ever really
// could. The jump gate has a safety valve - after a few straight rejections we
// accept the new value, so a genuine step change (a door left open, the sensor
// relocated) isn't rejected forever.
const float   TEMP_VALID_MIN = -40.0f;   // DHT22 sensing floor
const float   TEMP_VALID_MAX =  80.0f;   // DHT22 sensing ceiling
const float   HUM_VALID_MIN  =   0.0f;
const float   HUM_VALID_MAX  = 100.0f;
const float   TEMP_MAX_JUMP  =  10.0f;   // deg C plausibly attainable in one cycle
const float   HUM_MAX_JUMP   =  25.0f;   // % RH plausibly attainable in one cycle
const uint8_t SPIKE_OVERRIDE_AFTER = 4;  // accept a persistent new level after this many rejections

// How often to refresh the diagnostic entities (Wi-Fi signal, uptime, IP). These
// move slowly and don't need the sensor cadence, so publish them less often to
// keep the broker quiet.
const unsigned long DIAG_INTERVAL_MS = 60000;   // 1 minute

#define DHTPIN  D4
// DHT22/AM2302 is what's wired in now - a big step up from the old DHT11 in
// humidity accuracy and with 0.1-degree resolution. This build uses the common
// 3-pin breakout module, which carries its own pull-up on the daughterboard, so
// no external resistor is needed on the data line. Keep the cable as short as
// you can for a reliable read over the run into the fridge.
#define DHTTYPE DHT22

// MQTT topics
const char* TOPIC_STATUS      = "charcuterie/monitor/status";
const char* TOPIC_TEMPERATURE = "charcuterie/monitor/temperature";
const char* TOPIC_HUMIDITY    = "charcuterie/monitor/humidity";
const char* TOPIC_DEWPOINT    = "charcuterie/monitor/dewpoint";
const char* TOPIC_RSSI        = "charcuterie/monitor/rssi";
const char* TOPIC_UPTIME      = "charcuterie/monitor/uptime";
const char* TOPIC_IP          = "charcuterie/monitor/ip";

WiFiClient espClient;
PubSubClient client(espClient);
DHT dht(DHTPIN, DHTTYPE);

// --- Remote console over telnet (port 23) ---------------------------
// Serial output only reaches a USB cable, so mirror every log line to a telnet
// client too. Connect with `telnet <board-ip> 23` to watch it over Wi-Fi.
WiFiServer telnetServer(23);
WiFiClient telnetClient;

// A Print that "tees" output to both USB serial and the telnet client, so the
// same Log.print(...) calls show up in either place (or both at once).
class TeeLogger : public Print {
  public:
    size_t write(uint8_t c) override {
      Serial.write(c);
      if (telnetClient && telnetClient.connected()) telnetClient.write(c);
      return 1;
    }
    size_t write(const uint8_t* buf, size_t size) override {
      Serial.write(buf, size);
      if (telnetClient && telnetClient.connected()) telnetClient.write(buf, size);
      return size;
    }
};
TeeLogger Log;

void handleTelnet() {
  if (telnetServer.hasClient()) {
    // Only one console at a time - drop any stale client for the newcomer.
    if (telnetClient && telnetClient.connected()) telnetClient.stop();
    telnetClient = telnetServer.accept();
    telnetClient.println();
    telnetClient.println("== Charcuterie Sensor - live log ==");
  }
  // Discard anything the client types; this is an output-only console.
  while (telnetClient && telnetClient.available()) telnetClient.read();
}

// Dew point from temperature (C) and relative humidity (%), via the
// Magnus-Tetens approximation - accurate to a few hundredths of a degree across
// the 0-60 C / 1-100% range a curing chamber lives in.
float dewPointC(float tempC, float rh) {
  const float a = 17.625f;
  const float b = 243.04f;
  float gamma = logf(rh / 100.0f) + (a * tempC) / (b + tempC);
  return (b * gamma) / (a - gamma);
}

// The Home Assistant "device" block, identical for every entity, so all of them
// group under one Charcuterie Monitor device in HA. Kept as a macro so the
// discovery payloads below stay readable and can't drift out of sync.
#define HA_DEVICE_BLOCK \
  "\"device\":{\"identifiers\":[\"charcuterie_monitor\"]," \
  "\"name\":\"Charcuterie Monitor\",\"manufacturer\":\"Zach\"," \
  "\"model\":\"NodeMCU ESP8266\"}"

// Build the config topic for an entity and publish its (retained) discovery
// payload. Returns publish() success so announce() can flag a silent failure.
// "force_update":true makes Home Assistant record a state (and history point) on
// EVERY message, not just when the value changes - HA otherwise de-duplicates
// identical consecutive values, so a run of unchanged readings makes the entity
// look like it only refreshes every ~30s or slower. Only the measurement
// entities pass forceUpdate=true; the slow diagnostics don't need it.
bool publishDiscovery(const char* objectId, const char* payload) {
  char topic[96];
  snprintf(topic, sizeof(topic),
           "homeassistant/sensor/charcuterie_monitor/%s/config", objectId);
  return client.publish(topic, payload, true);
}

// Publish availability + the Home Assistant discovery configs (all retained).
void announce() {

  client.publish(TOPIC_STATUS, "online", true);

  bool ok = true;

  ok &= publishDiscovery("temperature",
    "{\"name\":\"Charcuterie Temperature\",\"unique_id\":\"charcuterie_temperature\","
    "\"state_topic\":\"charcuterie/monitor/temperature\","
    "\"availability_topic\":\"charcuterie/monitor/status\","
    "\"payload_available\":\"online\",\"payload_not_available\":\"offline\","
    "\"unit_of_measurement\":\"°C\",\"device_class\":\"temperature\","
    "\"state_class\":\"measurement\",\"force_update\":true," HA_DEVICE_BLOCK "}");

  ok &= publishDiscovery("humidity",
    "{\"name\":\"Charcuterie Humidity\",\"unique_id\":\"charcuterie_humidity\","
    "\"state_topic\":\"charcuterie/monitor/humidity\","
    "\"availability_topic\":\"charcuterie/monitor/status\","
    "\"payload_available\":\"online\",\"payload_not_available\":\"offline\","
    "\"unit_of_measurement\":\"%\",\"device_class\":\"humidity\","
    "\"state_class\":\"measurement\",\"force_update\":true," HA_DEVICE_BLOCK "}");

  // Dew point (computed on-device). When chamber temp nears the dew point you're
  // at condensation risk - the wet spots and soaked sensors that plague curing.
  ok &= publishDiscovery("dewpoint",
    "{\"name\":\"Charcuterie Dew Point\",\"unique_id\":\"charcuterie_dewpoint\","
    "\"state_topic\":\"charcuterie/monitor/dewpoint\","
    "\"availability_topic\":\"charcuterie/monitor/status\","
    "\"payload_available\":\"online\",\"payload_not_available\":\"offline\","
    "\"unit_of_measurement\":\"°C\",\"device_class\":\"temperature\","
    "\"state_class\":\"measurement\",\"force_update\":true," HA_DEVICE_BLOCK "}");

  // Diagnostics - grouped under HA's Diagnostic section (entity_category), so
  // you can tell a bad cable from weak Wi-Fi from a rebooting board at a glance.
  ok &= publishDiscovery("rssi",
    "{\"name\":\"Charcuterie Wi-Fi Signal\",\"unique_id\":\"charcuterie_rssi\","
    "\"state_topic\":\"charcuterie/monitor/rssi\","
    "\"availability_topic\":\"charcuterie/monitor/status\","
    "\"payload_available\":\"online\",\"payload_not_available\":\"offline\","
    "\"unit_of_measurement\":\"dBm\",\"device_class\":\"signal_strength\","
    "\"state_class\":\"measurement\",\"entity_category\":\"diagnostic\"," HA_DEVICE_BLOCK "}");

  ok &= publishDiscovery("uptime",
    "{\"name\":\"Charcuterie Uptime\",\"unique_id\":\"charcuterie_uptime\","
    "\"state_topic\":\"charcuterie/monitor/uptime\","
    "\"availability_topic\":\"charcuterie/monitor/status\","
    "\"payload_available\":\"online\",\"payload_not_available\":\"offline\","
    "\"unit_of_measurement\":\"s\",\"device_class\":\"duration\","
    "\"state_class\":\"total_increasing\",\"entity_category\":\"diagnostic\"," HA_DEVICE_BLOCK "}");

  ok &= publishDiscovery("ip",
    "{\"name\":\"Charcuterie IP Address\",\"unique_id\":\"charcuterie_ip\","
    "\"state_topic\":\"charcuterie/monitor/ip\","
    "\"availability_topic\":\"charcuterie/monitor/status\","
    "\"payload_available\":\"online\",\"payload_not_available\":\"offline\","
    "\"entity_category\":\"diagnostic\",\"icon\":\"mdi:ip-network\"," HA_DEVICE_BLOCK "}");

  if (ok) {
    Log.println("Published Home Assistant discovery.");
  } else {
    // If this ever prints, either the packet buffer is too small for the payload
    // or the broker rejected the publish (e.g. an ACL that blocks homeassistant/#).
    Log.println("Discovery publish FAILED. Check MQTT_BUFFER_SIZE and broker permissions.");
  }
}

void reconnectMQTT() {

  while (!client.connected()) {

    Log.print("Connecting to MQTT...");

    String clientId = "charcuterie-sensor-";
    clientId += String(random(0xffff), HEX);

    // Anonymous connect with a retained Last-Will so availability flips to
    // "offline" if the board drops off unexpectedly.
    if (client.connect(
          clientId.c_str(),
          TOPIC_STATUS,   // Last-Will topic
          1,              // Last-Will QoS
          true,           // Last-Will retained
          "offline")) {   // Last-Will payload

      Log.println("connected!");

      announce();

    } else {

      Log.print("failed, rc=");
      Log.println(client.state());

      // Wait ~5s before retrying, but keep OTA and the telnet console responsive
      // so a down broker can't lock you out of firmware pushes or the logs.
      for (int i = 0; i < 50 && !client.connected(); i++) {
        ArduinoOTA.handle();
        handleTelnet();
        delay(100);
      }
    }
  }
}

void setupOTA() {

  ArduinoOTA.setHostname(ota_hostname);
  if (strlen(ota_password) > 0) {
    ArduinoOTA.setPassword(ota_password);
  }

  ArduinoOTA.onStart([]() {
    // Announce a clean shutdown so HA and the chamber panel don't flag an error.
    if (client.connected()) client.publish(TOPIC_STATUS, "offline", true);
    Log.println("OTA update starting...");
  });
  ArduinoOTA.onEnd([]() {
    Log.println("\nOTA update complete - rebooting.");
  });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    Log.printf("OTA progress: %u%%\r", (progress * 100) / total);
  });
  ArduinoOTA.onError([](ota_error_t error) {
    Log.printf("OTA error [%u]\n", error);
  });

  ArduinoOTA.begin();

  Log.print("OTA ready - network port \"");
  Log.print(ota_hostname);
  Log.println("\" available in the Arduino IDE.");
}

void setup() {

  Serial.begin(115200);

  // Seed the PRNG from the hardware RNG so the per-connection MQTT client id
  // (random(0xffff) in reconnectMQTT) actually varies between boots. Left
  // unseeded it repeats the same sequence every power-on, so a quick reconnect
  // could collide with a session the broker hasn't timed out yet.
  randomSeed(RANDOM_REG32);

  Serial.println();
  Serial.println("Connecting to WiFi...");

  WiFi.mode(WIFI_STA);
  WiFi.persistent(false);        // don't rewrite creds to flash on every boot
  WiFi.setAutoReconnect(true);   // let the SDK re-join on its own if Wi-Fi drops
  WiFi.begin(ssid, password);

  // Don't block here forever. If Wi-Fi is down at boot, spinning in this loop
  // would also stop us from ever reaching setupOTA() and the telnet console -
  // leaving no way in but a power-cycle. Wait ~20s, then carry on: loop()'s
  // reconnect path plus setAutoReconnect() recover once Wi-Fi comes back.
  unsigned long wifiStart = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - wifiStart < 20000) {
    delay(500);
    Serial.print(".");
  }

  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("WiFi connected!");
  } else {
    Serial.println("WiFi not up yet - continuing; will keep retrying in the background.");
  }

  client.setServer(mqtt_server, 1883);

  // Must be large enough to hold the Home Assistant discovery payloads,
  // otherwise those publishes silently fail and HA never sees the sensor.
  client.setBufferSize(MQTT_BUFFER_SIZE);

  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());

  setupOTA();

  // Start the telnet console. mDNS is already running (ArduinoOTA.begin), so
  // the board is reachable as ota_hostname.local too.
  telnetServer.begin();
  telnetServer.setNoDelay(true);
  Serial.print("Telnet console ready - connect with: telnet ");
  Serial.print(WiFi.localIP());
  Serial.println(" 23");

  dht.begin();

  delay(1000);

  Log.println();
  Log.println("==============================");
  Log.println("  Charcuterie Sensor Online");
  Log.println("==============================");
  Log.println();
}

void loop() {

  ArduinoOTA.handle();   // check for an incoming Wi-Fi firmware upload every loop
  handleTelnet();        // accept/service the telnet console

  if (!client.connected()) {
    reconnectMQTT();
  }

  client.loop();   // keeps the MQTT connection alive (keepalive pings) at all times

  // Periodically re-assert availability + discovery so Home Assistant recovers
  // on its own after a broker restart or a cleared retained message.
  static unsigned long lastAnnounce = millis();
  if (client.connected() && millis() - lastAnnounce >= REANNOUNCE_INTERVAL_MS) {
    lastAnnounce = millis();
    announce();
  }

  static unsigned long lastReading = 0;

  if (millis() - lastReading >= PUBLISH_INTERVAL_MS || lastReading == 0) {

    lastReading = millis();

    float humidity = dht.readHumidity();
    float temperature = dht.readTemperature();

    static uint8_t sensorFailStreak = 0;
    static bool sensorFlaggedOffline = false;

    if (isnan(humidity) || isnan(temperature)) {
      // Tolerate the odd corrupted frame from the cable run. Only after several
      // consecutive failures do we treat the sensor as genuinely down and flip
      // availability to "offline" - that stops Home Assistant and the chamber
      // panel from presenting a frozen, stale retained value as if it were live.
      sensorFailStreak++;
      Log.print("Sensor read failed (");
      Log.print(sensorFailStreak);
      Log.print("/");
      Log.print(SENSOR_FAIL_THRESHOLD);
      Log.println(")");
      if (sensorFailStreak >= SENSOR_FAIL_THRESHOLD && !sensorFlaggedOffline) {
        client.publish(TOPIC_STATUS, "offline", true);
        sensorFlaggedOffline = true;
        Log.println("Sensor considered down - marked offline.");
      }
      return;
    }

    // Spike rejection. The frame decoded (not NaN) but may still be a glitch off
    // the long cable run. Drop anything physically impossible outright, and drop
    // a value that leapt further in one cycle than the chamber ever really could
    // - unless we've rejected several in a row, in which case treat it as a real
    // new level and let it through so we can't get stuck ignoring reality.
    static float   lastGoodTemp = NAN;
    static float   lastGoodHum  = NAN;
    static uint8_t spikeStreak  = 0;

    bool implausible = temperature < TEMP_VALID_MIN || temperature > TEMP_VALID_MAX ||
                       humidity    < HUM_VALID_MIN  || humidity    > HUM_VALID_MAX;
    bool jumped = !isnan(lastGoodTemp) &&
                  (fabsf(temperature - lastGoodTemp) > TEMP_MAX_JUMP ||
                   fabsf(humidity    - lastGoodHum)  > HUM_MAX_JUMP);

    if ((implausible || jumped) && spikeStreak < SPIKE_OVERRIDE_AFTER) {
      spikeStreak++;
      Log.print("Rejected spike read (temp=");
      Log.print(temperature);
      Log.print(" C, hum=");
      Log.print(humidity);
      Log.print(" %) - ");
      Log.println(implausible ? "out of range." : "jumped too far in one cycle.");
      return;
    }
    spikeStreak = 0;
    lastGoodTemp = temperature;
    lastGoodHum  = humidity;

    // Good read: clear the streak, and if we'd flagged the sensor down, bring
    // availability back so the panels start trusting the readings again.
    if (sensorFlaggedOffline) {
      client.publish(TOPIC_STATUS, "online", true);
      sensorFlaggedOffline = false;
      Log.println("Sensor recovered - marked online.");
    }
    sensorFailStreak = 0;

    float dew = dewPointC(temperature, humidity);

    Log.print("Temperature: ");
    Log.print(temperature);
    Log.println(" C");

    Log.print("Humidity: ");
    Log.print(humidity);
    Log.println(" %");

    Log.print("Dew point: ");
    Log.print(dew);
    Log.println(" C");

    char tempString[8];
    dtostrf(temperature, 1, 2, tempString);

    char humString[8];
    dtostrf(humidity, 1, 2, humString);

    char dewString[8];
    dtostrf(dew, 1, 2, dewString);

    client.publish(TOPIC_TEMPERATURE, tempString, true);
    client.publish(TOPIC_HUMIDITY, humString, true);
    client.publish(TOPIC_DEWPOINT, dewString, true);

    Log.println("Published MQTT data.");
    Log.println();
  }

  // Diagnostics on their own slower cadence: Wi-Fi signal, uptime, and IP. All
  // retained so HA shows the last value immediately on reconnect.
  static unsigned long lastDiag = 0;
  if (client.connected() && (millis() - lastDiag >= DIAG_INTERVAL_MS || lastDiag == 0)) {
    lastDiag = millis();

    char buf[24];

    snprintf(buf, sizeof(buf), "%ld", (long)WiFi.RSSI());
    client.publish(TOPIC_RSSI, buf, true);

    snprintf(buf, sizeof(buf), "%lu", millis() / 1000UL);
    client.publish(TOPIC_UPTIME, buf, true);

    client.publish(TOPIC_IP, WiFi.localIP().toString().c_str(), true);
  }
}
