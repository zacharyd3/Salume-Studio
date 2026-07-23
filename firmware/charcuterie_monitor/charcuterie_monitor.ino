/*
 * Salume Studio - Curing Chamber Monitor (NodeMCU V3 / ESP8266 + DHT)
 *
 * Publishes retained temperature/humidity to an MQTT broker and announces
 * itself to Home Assistant via MQTT discovery. A retained "online" message
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

#define DHTPIN  D4
#define DHTTYPE DHT11

// MQTT topics
const char* TOPIC_STATUS      = "charcuterie/monitor/status";
const char* TOPIC_TEMPERATURE = "charcuterie/monitor/temperature";
const char* TOPIC_HUMIDITY    = "charcuterie/monitor/humidity";

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

// Publish availability + the Home Assistant discovery configs (all retained).
void announce() {

  client.publish(TOPIC_STATUS, "online", true);

  const char* tempConfigTopic = "homeassistant/sensor/charcuterie_monitor/temperature/config";
  const char* humConfigTopic  = "homeassistant/sensor/charcuterie_monitor/humidity/config";

  const char* tempConfig = R"rawliteral(
{
  "name":"Charcuterie Temperature",
  "unique_id":"charcuterie_temperature",
  "state_topic":"charcuterie/monitor/temperature",
  "availability_topic":"charcuterie/monitor/status",
  "payload_available":"online",
  "payload_not_available":"offline",
  "unit_of_measurement":"°C",
  "device_class":"temperature",
  "state_class":"measurement",
  "device":{
    "identifiers":["charcuterie_monitor"],
    "name":"Charcuterie Monitor",
    "manufacturer":"Zach",
    "model":"NodeMCU ESP8266"
  }
}
)rawliteral";

  const char* humConfig = R"rawliteral(
{
  "name":"Charcuterie Humidity",
  "unique_id":"charcuterie_humidity",
  "state_topic":"charcuterie/monitor/humidity",
  "availability_topic":"charcuterie/monitor/status",
  "payload_available":"online",
  "payload_not_available":"offline",
  "unit_of_measurement":"%",
  "device_class":"humidity",
  "state_class":"measurement",
  "device":{
    "identifiers":["charcuterie_monitor"],
    "name":"Charcuterie Monitor",
    "manufacturer":"Zach",
    "model":"NodeMCU ESP8266"
  }
}
)rawliteral";

  bool okTemp = client.publish(tempConfigTopic, tempConfig, true);
  bool okHum  = client.publish(humConfigTopic, humConfig, true);

  if (okTemp && okHum) {
    Log.println("Published Home Assistant discovery.");
  } else {
    // If this ever prints, either the packet buffer is too small for the payload
    // or the broker rejected the publish (e.g. an ACL that blocks homeassistant/#).
    Log.print("Discovery publish FAILED (temp=");
    Log.print(okTemp);
    Log.print(", hum=");
    Log.print(okHum);
    Log.println("). Check MQTT_BUFFER_SIZE and broker permissions.");
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

  Serial.println();
  Serial.println("Connecting to WiFi...");

  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  Serial.println();
  Serial.println("WiFi connected!");

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

    if (isnan(humidity) || isnan(temperature)) {
      Log.println("Sensor failed");
      return;
    }

    Log.print("Temperature: ");
    Log.print(temperature);
    Log.println(" C");

    Log.print("Humidity: ");
    Log.print(humidity);
    Log.println(" %");

    char tempString[8];
    dtostrf(temperature, 1, 2, tempString);

    char humString[8];
    dtostrf(humidity, 1, 2, humString);

    client.publish(TOPIC_TEMPERATURE, tempString, true);
    client.publish(TOPIC_HUMIDITY, humString, true);

    Log.println("Published MQTT data.");
    Log.println();
  }
}
