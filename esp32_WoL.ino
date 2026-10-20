// sources: https://medium.com/@myth7672/anomaly-detection-in-network-traffic-using-ai-ml-zeek-logs-a47ccb88f4af

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <WiFiUdp.h>
#include <ArduinoJson.h>
#include "secrets.h"

// =========================
// VERSION / KONFIGURATION
// =========================

const char* FIRMWARE_VERSION = "1.1.0";

const char* SERVER_MAC = "2C:F0:5D:05:0F:FB";
IPAddress BROADCAST_IP(192, 168, 178, 255);
const uint16_t WOL_PORT = 9;

const char* TARGET_IP = "192.168.178.50"; // IP des Ziel-PCs
const uint16_t TARGET_CHECK_PORT = 22;    // SSH; bei Windows z. B. 3389 oder anderer offener Port

const char* COMMAND_TOPIC = "carl/home/esp32/wake";
const char* STATUS_TOPIC  = "carl/home/esp32/status";
const char* LOG_TOPIC     = "carl/home/esp32/log";

const uint8_t WOL_ATTEMPTS = 3;
const uint16_t WOL_ATTEMPT_DELAY_MS = 250;

const unsigned long PC_CHECK_DELAY_MS = 15000;
const unsigned long PC_CHECK_TIMEOUT_MS = 60000;
const unsigned long PC_CHECK_INTERVAL_MS = 3000;

const unsigned long MQTT_RECONNECT_MIN_MS = 2000;
const unsigned long MQTT_RECONNECT_MAX_MS = 60000;


// =========================
// OBJEKTE
// =========================

WiFiClientSecure tlsClient;
PubSubClient mqtt(tlsClient);
WiFiUDP udp;

byte targetMac[6];

String activeCmdId = "";
enum WolState {
  IDLE,
  WAITING_PC,
  FINISHED
};

WolState wolState = IDLE;
unsigned long wolStartMs = 0;
unsigned long lastPcCheckMs = 0;
unsigned long lastMqttReconnectAttempt = 0;
unsigned long currentMqttBackoffMs = MQTT_RECONNECT_MIN_MS;

// =========================
// HELPER
// =========================

bool parseMac(const char* mac, byte out[6]) {
  return sscanf(
    mac,
    "%2hhx:%2hhx:%2hhx:%2hhx:%2hhx:%2hhx",
    &out[0], &out[1], &out[2],
    &out[3], &out[4], &out[5]
  ) == 6;
}

String buildJson(
  const char* event,
  const char* state,
  const char* reason = "",
  int attempts = 0,
  long latencyMs = -1
) {
  JsonDocument doc;

  doc["event"] = event;
  doc["cmd_id"] = activeCmdId;
  doc["state"] = state;
  doc["firmware"] = FIRMWARE_VERSION;
  doc["uptime_s"] = millis() / 1000;
  doc["wifi_rssi"] = WiFi.RSSI();
  doc["wifi_state"] = WiFi.status();
  doc["mqtt_connected"] = mqtt.connected();

  if (strlen(reason) > 0) {
    doc["reason"] = reason;
  }

  if (attempts > 0) {
    doc["attempts"] = attempts;
  }

  if (latencyMs >= 0) {
    doc["latency_ms"] = latencyMs;
  }

  String output;
  serializeJson(doc, output);
  return output;
}

void publishJson(const char* topic, const String& json, bool retain = false) {
  Serial.println(json);

  if (mqtt.connected()) {
    mqtt.publish(topic, json.c_str(), retain);
  }
}

void mqttLog(const char* event, const char* state, const char* reason = "") {
  publishJson(LOG_TOPIC, buildJson(event, state, reason));
}

void publishWolStatus(const char* state, const char* reason = "", int attempts = 0, long latencyMs = -1) {
  publishJson(
    STATUS_TOPIC,
    buildJson("wol_command", state, reason, attempts, latencyMs),
    true
  );
}

// =========================
// WAKE-ON-LAN
// =========================

bool sendMagicPacket() {
  byte packet[102];

  memset(packet, 0xFF, 6);

  for (int i = 0; i < 16; i++) {
    memcpy(packet + 6 + i * 6, targetMac, 6);
  }

  udp.beginPacket(BROADCAST_IP, WOL_PORT);
  udp.write(packet, sizeof(packet));
  bool ok = udp.endPacket();
  udp.stop();

  return ok;
}

bool sendMagicPackets() {
  int successful = 0;

  for (uint8_t i = 1; i <= WOL_ATTEMPTS; i++) {
    if (sendMagicPacket()) {
      successful++;
    }

    if (i < WOL_ATTEMPTS) {
      delay(WOL_ATTEMPT_DELAY_MS);
    }
  }

  return successful > 0;
}

bool isPcReachable() {
  WiFiClient client;
  return client.connect(TARGET_IP, TARGET_CHECK_PORT);
}

// =========================
// WOL-PROZESS
// =========================

void startWolProcess(const String& cmdId) {
  activeCmdId = cmdId;
  wolStartMs = millis();
  lastPcCheckMs = 0;
  wolState = WAITING_PC;

  publishWolStatus("RECEIVED");

  if (WiFi.status() != WL_CONNECTED) {
    publishWolStatus("FAILED", "wifi_disconnected");
    wolState = FINISHED;
    return;
  }

  bool sent = sendMagicPackets();

  if (!sent) {
    publishWolStatus("FAILED", "udp_send_failed", WOL_ATTEMPTS);
    wolState = FINISHED;
    return;
  }

  publishWolStatus("SENT", "", WOL_ATTEMPTS);
}

void checkWolProcess() {
  if (wolState != WAITING_PC) {
    return;
  }

  unsigned long elapsed = millis() - wolStartMs;

  if (elapsed < PC_CHECK_DELAY_MS) {
    return;
  }

  if (millis() - lastPcCheckMs < PC_CHECK_INTERVAL_MS) {
    return;
  }

  lastPcCheckMs = millis();

  if (isPcReachable()) {
    publishWolStatus("ONLINE", "", WOL_ATTEMPTS, elapsed);
    wolState = FINISHED;
    return;
  }

  if (elapsed >= PC_CHECK_TIMEOUT_MS) {
    publishWolStatus("TIMEOUT", "pc_not_reachable", WOL_ATTEMPTS, elapsed);
    wolState = FINISHED;
  }
}

// =========================
// MQTT CALLBACK
// =========================

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String message;

  for (unsigned int i = 0; i < length; i++) {
    message += (char)payload[i];
  }

  message.trim();

  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, message);

  if (error) {
    mqttLog("wol_command", "INVALID_PAYLOAD", "json_parse_error");
    return;
  }

  const char* cmdId = doc["cmd_id"] | "";
  const char* action = doc["action"] | "";
  const char* target = doc["target"] | "";

  if (strlen(cmdId) == 0) {
    mqttLog("wol_command", "INVALID_PAYLOAD", "missing_cmd_id");
    return;
  }

  if (activeCmdId == cmdId && wolState != FINISHED) {
    mqttLog("wol_command", "DUPLICATE_COMMAND", "same_cmd_id");
    return;
  }

  if (strcmp(action, "WAKE") != 0) {
    mqttLog("wol_command", "INVALID_PAYLOAD", "unknown_action");
    return;
  }

  if (strcmp(target, "proxmox-host") != 0) {
    mqttLog("wol_command", "INVALID_TARGET", target);
    return;
  }

  startWolProcess(cmdId);
}

// =========================
// WLAN
// =========================

void connectWifi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  Serial.print("[WIFI] Verbinde");

  int attempts = 0;

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");

    if (++attempts >= 40) {
      Serial.println("\n[WIFI] Verbindung fehlgeschlagen. Neustart.");
      ESP.restart();
    }
  }

  Serial.println("\n[WIFI] Verbunden");
  Serial.printf("[WIFI] IP: %s\n", WiFi.localIP().toString().c_str());
  Serial.printf("[WIFI] RSSI: %d dBm\n", WiFi.RSSI());

  mqttLog("wifi", "CONNECTED");
}

// =========================
// MQTT
// =========================

void connectMqtt() {
  while (!mqtt.connected()) {
    Serial.println("[MQTT] Verbinde...");

    bool connected = mqtt.connect(
      MQTT_CLIENT_ID,
      MQTT_USER,
      MQTT_PASSWORD,
      STATUS_TOPIC,
      1,
      true,
      "offline"
    );

    if (!connected) {
      Serial.printf("[MQTT] Fehler, state=%d\n", mqtt.state());

      delay(currentMqttBackoffMs);
      currentMqttBackoffMs = min(currentMqttBackoffMs * 2, MQTT_RECONNECT_MAX_MS);
      continue;
    }

    currentMqttBackoffMs = MQTT_RECONNECT_MIN_MS;

    Serial.println("[MQTT] Verbunden");

    publishStatus("online", true);

    bool subscribed = mqtt.subscribe(COMMAND_TOPIC, 1);

    if (subscribed) {
      mqttLog("mqtt", "SUBSCRIBED", COMMAND_TOPIC);
    } else {
      mqttLog("mqtt", "SUBSCRIBE_FAILED", COMMAND_TOPIC);
    }

    mqttLog("esp32", "READY");
  }
}

// =========================
// SETUP
// =========================

void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println("\nESP32 Remote Wake Gateway");
  Serial.printf("Firmware: %s\n", FIRMWARE_VERSION);

  if (!parseMac(SERVER_MAC, targetMac)) {
    Serial.println("[BOOT] ERROR: Server-MAC ungültig");
    ESP.restart();
  }

  connectWifi();

  tlsClient.setCACert(ROOT_CA);

  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setCallback(mqttCallback);
  mqtt.setBufferSize(1024);
  mqtt.setKeepAlive(30);

  connectMqtt();
}

// =========================
// LOOP
// =========================

void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    mqttLog("wifi", "DISCONNECTED");
    connectWifi();
  }

  if (!mqtt.connected()) {
    if (millis() - lastMqttReconnectAttempt >= currentMqttBackoffMs) {
      lastMqttReconnectAttempt = millis();
      connectMqtt();
    }
  }

  mqtt.loop();

  checkWolProcess();
}