/*
 * Salume Studio — Curing Chamber Monitor (NodeMCU V3 / ESP8266 + DHT)
 *
 * Publishes retained temperature/humidity to an MQTT broker and announces
 * itself to Home Assistant via MQTT discovery. A retained "online" message
 * plus a Last-Will "offline" on charcuterie/monitor/status drive availability
 * in both Home Assistant and the Curing Chamber panel in charcuterie.html.
 *
 * Libraries (Arduino Library Manager):
 *   - "DHT sensor library" by Adafruit (+ "Adafruit Unified Sensor")
 *   - "PubSubClient" by Nick O'Leary
 *
 * IMPORTANT: PubSubClient's default packet buffer is 256 bytes, but the Home
 * Assistant discovery payloads below are ~500 bytes. Without setBufferSize()
 * the discovery publishes silently fail (publish() returns false) and HA never
 * creates the entities — even though the small temperature/humidity payloads
 * still publish fine. See setup().
 */

#include <DHT.h>
#include <ESP8266WiFi.h>
#include <PubSubClient.h>

// ============================ EDIT THESE =============================
// Your Wi-Fi and broker details. Keep real credentials out of git — these
// are placeholders; fill them in on the copy you flash.
const char* ssid        = "YOUR_WIFI_SSID";
const char* password    = "YOUR_WIFI_PASSWORD";
const char* mqtt_server = "192.168.250.3";
const char* mqtt_user   = "mqtt_user";
const char* mqtt_password = "mqtt_password";
// ====================================================================

// How often to read the sensor and publish. Readings are retained, so the
// last value is always available instantly to anyone who connects; this just
// controls how fresh the "live" number is. Curing chambers move slowly, so
// there's no need to hammer it.
const unsigned long PUBLISH_INTERVAL_MS = 30000;   // 30 seconds

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

void publishDiscovery() {

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
    Serial.println("Published Home Assistant discovery.");
  } else {
    // If this ever prints, the packet buffer is too small for the payload.
    Serial.print("Discovery publish FAILED (temp=");
    Serial.print(okTemp);
    Serial.print(", hum=");
    Serial.print(okHum);
    Serial.println("). Increase MQTT_BUFFER_SIZE.");
  }
}

void reconnectMQTT() {

  while (!client.connected()) {

    Serial.print("Connecting to MQTT...");

    String clientId = "charcuterie-sensor-";
    clientId += String(random(0xffff), HEX);

    if (client.connect(
          clientId.c_str(),
          mqtt_user,
          mqtt_password,
          TOPIC_STATUS,   // Last-Will topic
          1,              // Last-Will QoS
          true,           // Last-Will retained
          "offline")) {   // Last-Will payload

      Serial.println("connected!");

      // Retained "online" so availability is known the instant anyone connects.
      client.publish(TOPIC_STATUS, "online", true);

      publishDiscovery();

    } else {

      Serial.print("failed, rc=");
      Serial.println(client.state());

      delay(5000);
    }
  }
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

  dht.begin();

  delay(1000);

  Serial.println();
  Serial.println("==============================");
  Serial.println("  Charcuterie Sensor Online");
  Serial.println("==============================");
  Serial.println();
}

void loop() {

  if (!client.connected()) {
    reconnectMQTT();
  }

  client.loop();   // keeps the MQTT connection alive (keepalive pings) at all times

  static unsigned long lastReading = 0;

  if (millis() - lastReading >= PUBLISH_INTERVAL_MS || lastReading == 0) {

    lastReading = millis();

    float humidity = dht.readHumidity();
    float temperature = dht.readTemperature();

    if (isnan(humidity) || isnan(temperature)) {
      Serial.println("Sensor failed");
      return;
    }

    Serial.print("Temperature: ");
    Serial.print(temperature);
    Serial.println(" °C");

    Serial.print("Humidity: ");
    Serial.print(humidity);
    Serial.println(" %");

    char tempString[8];
    dtostrf(temperature, 1, 2, tempString);

    char humString[8];
    dtostrf(humidity, 1, 2, humString);

    client.publish(TOPIC_TEMPERATURE, tempString, true);
    client.publish(TOPIC_HUMIDITY, humString, true);

    Serial.println("Published MQTT data.");
    Serial.println();
  }
}
