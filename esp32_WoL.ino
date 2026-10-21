#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <WiFiUdp.h>
#include <ArduinoJson.h>
#include <string.h>

#include "secrets.h"
#include "config.h"


// =========================
// OBJECTS
// =========================

WiFiClientSecure tlsClient;
PubSubClient mqtt(tlsClient);
WiFiUDP udp;

byte targetMac[6];

String activeCmdId = "";
String lastCmdId = "";

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

// Additional state for non-blocking Wi-Fi connection management.
bool wifiConnecting = false;
bool wifiWasConnected = false;
unsigned long wifiConnectStartMs = 0;

const unsigned long WIFI_CONNECT_TIMEOUT_MS = 20000;
const unsigned long TCP_CONNECT_TIMEOUT_MS = 1000;

// Tracks whether the UDP socket was initialized successfully.
bool udpReady = false;


// =========================
// HELPER
// =========================

bool parseMac(const char* mac, byte out[6]) {
  // Reject null pointers and malformed MAC addresses.
  if (mac == nullptr) {
    return false;
  }

  unsigned int values[6];
  int consumed = 0;

  int parsed = sscanf(
    mac,
    "%2x:%2x:%2x:%2x:%2x:%2x%n",
    &values[0], &values[1], &values[2],
    &values[3], &values[4], &values[5],
    &consumed
  );

  // Require exactly six bytes and no trailing characters.
  if (parsed != 6 || mac[consumed] != '\0') {
    return false;
  }

  for (int i = 0; i < 6; i++) {
    if (values[i] > 0xFF) {
      return false;
    }

    out[i] = static_cast<byte>(values[i]);
  }

  return true;
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
  doc["wifi_rssi"] = WiFi.status() == WL_CONNECTED
                       ? WiFi.RSSI()
                       : 0;
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


bool publishJson(
  const char* topic,
  const String& json,
  bool retain = false
) {
  // Always print the message locally, even if MQTT is unavailable.
  Serial.println(json);

  if (!mqtt.connected()) {
    return false;
  }

  // Report whether the MQTT library accepted the publication.
  return mqtt.publish(topic, json.c_str(), retain);
}


void mqttLog(
  const char* event,
  const char* state,
  const char* reason = ""
) {
  publishJson(LOG_TOPIC, buildJson(event, state, reason));
}


void publishWolStatus(
  const char* state,
  const char* reason = "",
  int attempts = 0,
  long latencyMs = -1
) {
  // Retained status allows clients to read the latest reported state.
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

  // The first six bytes contain the synchronization pattern.
  memset(packet, 0xFF, 6);

  // Append the target MAC address sixteen times.
  for (int i = 0; i < 16; i++) {
    memcpy(packet + 6 + i * 6, targetMac, 6);
  }

  // Reuse the initialized UDP socket for repeated transmissions.
  if (!udpReady) {
    return false;
  }

  if (!udp.beginPacket(BROADCAST_IP, WOL_PORT)) {
    return false;
  }

  if (udp.write(packet, sizeof(packet)) != sizeof(packet)) {
    udp.endPacket();
    return false;
  }

  // A successful send does not prove that the target received the packet.
  return udp.endPacket();
}


bool sendMagicPackets() {
  int successful = 0;

  for (uint8_t i = 1; i <= WOL_ATTEMPTS; i++) {
    if (sendMagicPacket()) {
      successful++;
    }

    // Do not block the ESP32 for a long time between attempts.
    // The main loop will schedule the next attempt instead.
    if (i < WOL_ATTEMPTS) {
      // Repeated sends are handled by the dedicated scheduler below.
      // This function sends the first packet; see startWolProcess().
      break;
    }
  }

  return successful > 0;
}


// Tracks scheduled Magic Packet retransmissions.
uint8_t wolAttemptsSent = 0;
unsigned long lastWolSendMs = 0;


// Send packets over multiple loop iterations instead of using delay().
void sendScheduledWolPackets() {
  if (wolState != WAITING_PC) {
    return;
  }

  if (wolAttemptsSent >= WOL_ATTEMPTS) {
    return;
  }

  if (millis() - lastWolSendMs < WOL_ATTEMPT_DELAY_MS) {
    return;
  }

  if (sendMagicPacket()) {
    wolAttemptsSent++;
  } else {
    mqttLog("wol_command", "UDP_SEND_FAILED");
    // Count failed transmissions too, to prevent endless retries.
    wolAttemptsSent++;
  }

  lastWolSendMs = millis();
}


bool isPcReachable() {
  WiFiClient client;

  // Limit the time spent waiting for a TCP connection.
  bool connected = client.connect(
    TARGET_IP,
    TARGET_CHECK_PORT,
    TCP_CONNECT_TIMEOUT_MS
  );

  if (connected) {
    client.stop();
  }

  return connected;
}


// =========================
// WOL PROCESS
// =========================

void startWolProcess(const String& cmdId) {
  activeCmdId = cmdId;
  lastCmdId = cmdId;

  wolStartMs = millis();
  lastPcCheckMs = 0;
  lastWolSendMs = 0;
  wolAttemptsSent = 0;

  wolState = WAITING_PC;

  publishWolStatus("RECEIVED");

  if (WiFi.status() != WL_CONNECTED) {
    publishWolStatus("FAILED", "wifi_disconnected");
    wolState = FINISHED;
    return;
  }

  // Send the first Magic Packet immediately.
  if (sendMagicPacket()) {
    wolAttemptsSent = 1;
    lastWolSendMs = millis();

    publishWolStatus("SENT", "", wolAttemptsSent);
  } else {
    publishWolStatus("FAILED", "udp_send_failed", 1);
    wolState = FINISHED;
  }
}


void checkWolProcess() {
  if (wolState != WAITING_PC) {
    return;
  }

  unsigned long elapsed = millis() - wolStartMs;

  // Give the target time to start before the first TCP check.
  if (elapsed < PC_CHECK_DELAY_MS) {
    return;
  }

  if (millis() - lastPcCheckMs < PC_CHECK_INTERVAL_MS) {
    return;
  }

  lastPcCheckMs = millis();

  if (isPcReachable()) {
    publishWolStatus(
      "ONLINE",
      "",
      wolAttemptsSent,
      millis() - wolStartMs
    );

    wolState = FINISHED;
    return;
  }

  if (elapsed >= PC_CHECK_TIMEOUT_MS) {
    publishWolStatus(
      "TIMEOUT",
      "pc_not_reachable",
      wolAttemptsSent,
      elapsed
    );

    wolState = FINISHED;
  }
}


// =========================
// MQTT CALLBACK
// =========================

void mqttCallback(
  char* topic,
  byte* payload,
  unsigned int length
) {
  // Only process commands received on the expected topic.
  if (strcmp(topic, COMMAND_TOPIC) != 0) {
    mqttLog("wol_command", "INVALID_TOPIC");
    return;
  }

  String message;
  message.reserve(length);

  for (unsigned int i = 0; i < length; i++) {
    message += static_cast<char>(payload[i]);
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

  // Validate the command identifier before starting any operation.
  if (strlen(cmdId) == 0) {
    mqttLog("wol_command", "INVALID_PAYLOAD", "missing_cmd_id");
    return;
  }

  // Reject a repeated command, even if its previous execution finished.
  if (lastCmdId == cmdId) {
    mqttLog("wol_command", "DUPLICATE_COMMAND", "same_cmd_id");
    return;
  }

  // Do not allow overlapping Wake-on-LAN operations.
  if (wolState == WAITING_PC) {
    mqttLog("wol_command", "BUSY", "wol_process_running");
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

  startWolProcess(String(cmdId));
}


// =========================
// WLAN
// =========================

void connectWifi() {
  // Return immediately if Wi-Fi is already connected.
  if (WiFi.status() == WL_CONNECTED) {
    wifiConnecting = false;
    return;
  }

  // Monitor an ongoing connection attempt without blocking the main loop.
  if (wifiConnecting) {
    if (millis() - wifiConnectStartMs >= WIFI_CONNECT_TIMEOUT_MS) {
      Serial.println("[WIFI] Connection timeout");

      WiFi.disconnect(false);
      wifiConnecting = false;
    }

    return;
  }

  Serial.println("[WIFI] Starting connection...");

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  wifiConnecting = true;
  wifiConnectStartMs = millis();
}


// =========================
// MQTT
// =========================

void connectMqtt() {
  // Never attempt an MQTT connection without Wi-Fi.
  if (WiFi.status() != WL_CONNECTED || mqtt.connected()) {
    return;
  }

  Serial.println("[MQTT] Connecting...");

  // Perform one connection attempt per scheduled invocation.
  bool connected = mqtt.connect(
    MQTT_CLIENT_ID,
    MQTT_USER,
    MQTT_PASSWORD,
    AVAILABILITY_TOPIC,
    1,
    true,
    "offline"
  );

  if (!connected) {
    Serial.printf("[MQTT] Connection failed, state=%d\n", mqtt.state());

    // Apply exponential backoff, bounded by the configured maximum.
    currentMqttBackoffMs = min(
      currentMqttBackoffMs * 2,
      MQTT_RECONNECT_MAX_MS
    );

    return;
  }

  currentMqttBackoffMs = MQTT_RECONNECT_MIN_MS;

  Serial.println("[MQTT] Connected");

  if (!mqtt.publish(AVAILABILITY_TOPIC, "online", true)) {
    Serial.println("[MQTT] Availability publish failed");
  }

  if (mqtt.subscribe(COMMAND_TOPIC, 1)) {
    mqttLog("mqtt", "SUBSCRIBED", COMMAND_TOPIC);
  } else {
    mqttLog("mqtt", "SUBSCRIBE_FAILED", COMMAND_TOPIC);
  }

  mqttLog("esp32", "READY");
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
    Serial.println("[BOOT] ERROR: Invalid server MAC");
    ESP.restart();
    return;
  }

  // Initialize Wi-Fi without waiting for the connection to finish.
  connectWifi();

  // Configure TLS and MQTT before the first connection attempt.
  tlsClient.setCACert(ROOT_CA);

  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setCallback(mqttCallback);
  mqtt.setBufferSize(1024);
  mqtt.setKeepAlive(30);

  // Initialize a persistent UDP socket for Wake-on-LAN.
  udpReady = udp.begin(WOL_PORT) == 1;

  if (!udpReady) {
    Serial.println("[UDP] Socket initialization failed");
  }
}


// =========================
// LOOP
// =========================

void loop() {
  // Monitor Wi-Fi and initiate a connection if necessary.
  connectWifi();

  bool wifiConnected = WiFi.status() == WL_CONNECTED;

  // Log changes in Wi-Fi connectivity rather than logging every loop.
  if (wifiConnected && !wifiWasConnected) {
    Serial.printf("[WIFI] Connected: %s\n",
                  WiFi.localIP().toString().c_str());

    Serial.printf("[WIFI] RSSI: %d dBm\n", WiFi.RSSI());

    wifiConnecting = false;
    mqttLog("wifi", "CONNECTED");
  }

  if (!wifiConnected && wifiWasConnected) {
    mqttLog("wifi", "DISCONNECTED");
  }

  wifiWasConnected = wifiConnected;

  // Schedule MQTT reconnects using the current backoff interval.
  if (wifiConnected && !mqtt.connected()) {
    if (millis() - lastMqttReconnectAttempt >= currentMqttBackoffMs) {
      lastMqttReconnectAttempt = millis();
      connectMqtt();
    }
  }

  // Process incoming MQTT messages and maintain the connection.
  mqtt.loop();

  // Schedule additional Magic Packets without blocking the main loop.
  sendScheduledWolPackets();

  // Continue checking whether the target computer is reachable.
  checkWolProcess();
}
