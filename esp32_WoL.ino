#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <WiFiUdp.h>
#include <ArduinoJson.h>
#include <time.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <lwip/sockets.h>
#include <lwip/inet.h>

#include "secrets.h"
#include "config.h"

// ============================================================
// OBJECTS AND STATE
// ============================================================

WiFiClientSecure tlsClient;
PubSubClient mqtt(tlsClient);
WiFiUDP udp;

byte targetMac[6];
bool udpReady = false;

String activeCmdId;
String lastCmdId;
String queuedCmdId;
bool commandQueued = false;

// A small, one-command queue avoids silently discarding a command
// while a previous wake verification is running.
enum WolState { IDLE, WAITING_PC };
WolState wolState = IDLE;

unsigned long wolStartMs = 0;
unsigned long lastPcCheckMs = 0;
unsigned long lastWolSendMs = 0;
unsigned long lastCommandStartMs = 0;
uint8_t wolAttemptsSent = 0;

unsigned long lastMqttReconnectAttempt = 0;
unsigned long currentMqttBackoffMs = MQTT_RECONNECT_MIN_MS;

bool wifiConnecting = false;
bool wifiWasConnected = false;
unsigned long wifiConnectStartMs = 0;
const unsigned long WIFI_CONNECT_TIMEOUT_MS = 20000UL;

bool sntpStarted = false;
bool clockIsValid = false;
unsigned long sntpStartMs = 0;
unsigned long lastSntpRetryMs = 0;

// Non-blocking TCP reachability probe state.
int reachabilitySocket = -1;
bool tcpProbePending = false;
unsigned long tcpProbeStartMs = 0;

enum ProbeResult { PROBE_PENDING, PROBE_REACHABLE, PROBE_UNREACHABLE };

// ============================================================
// HELPERS
// ============================================================

bool parseMac(const char* mac, byte out[6]) {
  if (mac == nullptr) return false;

  unsigned int values[6];
  int consumed = 0;
  int parsed = sscanf(mac, "%2x:%2x:%2x:%2x:%2x:%2x%n",
                       &values[0], &values[1], &values[2],
                       &values[3], &values[4], &values[5], &consumed);
  if (parsed != 6 || mac[consumed] != '\0') return false;

  bool allZero = true;
  bool allBroadcast = true;
  for (int i = 0; i < 6; i++) {
    if (values[i] > 0xFF) return false;
    out[i] = static_cast<byte>(values[i]);
    if (out[i] != 0x00) allZero = false;
    if (out[i] != 0xFF) allBroadcast = false;
  }

  // Reject invalid, broadcast, and multicast destination MACs.
  if (allZero || allBroadcast || (out[0] & 0x01)) return false;
  return true;
}

String buildJson(const char* event, const char* state,
                 const char* reason = "", int attempts = 0,
                 long latencyMs = -1) {
  JsonDocument doc;
  doc["event"] = event;
  doc["cmd_id"] = activeCmdId;
  doc["state"] = state;
  doc["firmware"] = FIRMWARE_VERSION;
  doc["uptime_s"] = millis() / 1000;
  doc["wifi_rssi"] = WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : 0;
  doc["wifi_state"] = static_cast<int>(WiFi.status());
  doc["mqtt_connected"] = mqtt.connected();

  if (reason != nullptr && reason[0] != '\0') doc["reason"] = reason;
  if (attempts > 0) doc["attempts"] = attempts;
  if (latencyMs >= 0) doc["latency_ms"] = latencyMs;

  String output;
  serializeJson(doc, output);
  return output;
}

bool publishJson(const char* topic, const String& json, bool retain = false) {
  Serial.printf("[MQTT OUT] topic=%s payload=%s\n", topic, json.c_str());
  if (!mqtt.connected()) return false;

  // PubSubClient only supports QoS 0 for outgoing publish calls.
  // Broker ACLs must explicitly allow this ESP32 to publish here.
  bool ok = mqtt.publish(topic, json.c_str(), retain);
  if (!ok) Serial.printf("[MQTT] Publish failed (topic=%s, state=%d)\n", topic, mqtt.state());
  return ok;
}

void mqttLog(const char* event, const char* state, const char* reason = "") {
  publishJson(LOG_TOPIC, buildJson(event, state, reason), false);
}

void publishWolStatus(const char* state, const char* reason = "",
                      int attempts = 0, long latencyMs = -1) {
  publishJson(STATUS_TOPIC, buildJson("wol_command", state, reason, attempts, latencyMs), true);
}

void closeTcpProbe() {
  if (reachabilitySocket >= 0) close(reachabilitySocket);
  reachabilitySocket = -1;
  tcpProbePending = false;
}

void finishWolProcess(const char* state, const char* reason = "") {
  publishWolStatus(state, reason, wolAttemptsSent,
                   static_cast<long>(millis() - wolStartMs));
  closeTcpProbe();
  wolState = IDLE;
  activeCmdId = ""; // Clear only after the final status has used the active ID.
}

// ============================================================
// WAKE-ON-LAN
// ============================================================

bool sendMagicPacket() {
  if (!udpReady || WiFi.status() != WL_CONNECTED) return false;

  byte packet[102];
  memset(packet, 0xFF, 6);
  for (int i = 0; i < 16; i++) memcpy(packet + 6 + i * 6, targetMac, 6);

  // Derive the subnet broadcast address from the current Wi-Fi config.
  IPAddress broadcast = WiFi.broadcastIP();
  if (!udp.beginPacket(broadcast, WOL_PORT)) return false;
  if (udp.write(packet, sizeof(packet)) != sizeof(packet)) {
    udp.endPacket();
    return false;
  }
  return udp.endPacket() == 1;
}

void sendScheduledWolPackets() {
  if (wolState != WAITING_PC || wolAttemptsSent >= WOL_ATTEMPTS) return;
  if (millis() - lastWolSendMs < WOL_ATTEMPT_DELAY_MS) return;

  wolAttemptsSent++; // Count attempts even if the local UDP send fails.
  bool sent = sendMagicPacket();
  lastWolSendMs = millis();

  if (sent) {
    mqttLog("wol_packet", "SENT", "scheduled_attempt");
  } else {
    mqttLog("wol_packet", "UDP_SEND_FAILED", "scheduled_attempt");
  }
}

// ============================================================
// NON-BLOCKING TCP REACHABILITY PROBE
// ============================================================

ProbeResult pollTcpProbe() {
  if (!tcpProbePending || reachabilitySocket < 0) return PROBE_UNREACHABLE;

  fd_set writeSet;
  FD_ZERO(&writeSet);
  FD_SET(reachabilitySocket, &writeSet);
  timeval timeout = {0, 0};
  int selected = select(reachabilitySocket + 1, nullptr, &writeSet, nullptr, &timeout);

  if (selected > 0 && FD_ISSET(reachabilitySocket, &writeSet)) {
    int socketError = 0;
    socklen_t errorLength = sizeof(socketError);
    if (getsockopt(reachabilitySocket, SOL_SOCKET, SO_ERROR,
                   &socketError, &errorLength) == 0 && socketError == 0) {
      closeTcpProbe();
      return PROBE_REACHABLE;
    }
    closeTcpProbe();
    return PROBE_UNREACHABLE;
  }

  if (millis() - tcpProbeStartMs >= TCP_CONNECT_TIMEOUT_MS) {
    closeTcpProbe();
    return PROBE_UNREACHABLE;
  }
  return PROBE_PENDING;
}

bool startTcpProbe() {
  closeTcpProbe();

  sockaddr_in address = {};
  address.sin_family = AF_INET;
  address.sin_port = htons(TARGET_CHECK_PORT);
  if (inet_aton(TARGET_IP, &address.sin_addr) == 0) {
    mqttLog("reachability", "CONFIG_ERROR", "invalid_target_ip");
    return false;
  }

  int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (sock < 0) {
    mqttLog("reachability", "SOCKET_ERROR", "socket_create_failed");
    return false;
  }

  int flags = fcntl(sock, F_GETFL, 0);
  if (flags < 0 || fcntl(sock, F_SETFL, flags | O_NONBLOCK) < 0) {
    close(sock);
    mqttLog("reachability", "SOCKET_ERROR", "nonblocking_setup_failed");
    return false;
  }

  int result = connect(sock, reinterpret_cast<sockaddr*>(&address), sizeof(address));
  if (result == 0) {
    close(sock);
    // Immediate success is rare but valid.
    return true;
  }

  if (errno != EINPROGRESS && errno != EWOULDBLOCK) {
    close(sock);
    return false;
  }

  reachabilitySocket = sock;
  tcpProbePending = true;
  tcpProbeStartMs = millis();
  return false;
}

void checkWolProcess() {
  if (wolState != WAITING_PC) return;

  unsigned long elapsed = millis() - wolStartMs;
  if (elapsed >= PC_CHECK_TIMEOUT_MS) {
    finishWolProcess("TIMEOUT", "target_service_not_reachable_before_deadline");
    return;
  }

  if (elapsed < PC_CHECK_DELAY_MS) return;

  if (tcpProbePending) {
    ProbeResult result = pollTcpProbe();
    if (result == PROBE_REACHABLE) {
      finishWolProcess("ONLINE", "target_service_reachable");
      return;
    }
    if (result == PROBE_PENDING) return;
    // A failed connection attempt is not a diagnosis; try again at the interval.
  }

  if (millis() - lastPcCheckMs < PC_CHECK_INTERVAL_MS) return;
  lastPcCheckMs = millis();
  bool connectedImmediately = startTcpProbe();
  if (connectedImmediately) finishWolProcess("ONLINE", "target_service_reachable");
}

// ============================================================
// COMMAND QUEUE AND RATE LIMIT
// ============================================================

void startWolProcess(const String& cmdId) {
  activeCmdId = cmdId;
  lastCmdId = cmdId;
  lastCommandStartMs = millis();
  wolStartMs = millis();
  lastPcCheckMs = 0;
  lastWolSendMs = millis();
  wolAttemptsSent = 0;
  wolState = WAITING_PC;
  closeTcpProbe();

  publishWolStatus("RECEIVED");

  if (WiFi.status() != WL_CONNECTED) {
    finishWolProcess("FAILED", "wifi_disconnected");
    return;
  }
  if (!udpReady) {
    finishWolProcess("FAILED", "udp_socket_not_ready");
    return;
  }

  // First packet is sent immediately; subsequent attempts are scheduled.
  wolAttemptsSent++;
  bool sent = sendMagicPacket();
  lastWolSendMs = millis();
  if (sent) publishWolStatus("SENT", "first_magic_packet_sent", wolAttemptsSent);
  else mqttLog("wol_packet", "UDP_SEND_FAILED", "first_attempt");
}

void enqueueCommand(const String& cmdId) {
  if (!commandQueued) {
    queuedCmdId = cmdId;
    commandQueued = true;
    lastCmdId = cmdId;
    activeCmdId = cmdId;
    publishWolStatus("QUEUED", "another_command_is_running");
    activeCmdId = "";
    return;
  }

  activeCmdId = cmdId;
  publishWolStatus("REJECTED", "command_queue_full");
  activeCmdId = "";
}

void processQueuedCommand() {
  if (!commandQueued || wolState != IDLE) return;
  if (millis() - lastCommandStartMs < COMMAND_COOLDOWN_MS) return;

  String nextId = queuedCmdId;
  queuedCmdId = "";
  commandQueued = false;
  startWolProcess(nextId);
}

// ============================================================
// MQTT CALLBACK
// ============================================================

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  if (strcmp(topic, COMMAND_TOPIC) != 0) return;

  String message;
  message.reserve(length);
  for (unsigned int i = 0; i < length; i++) message += static_cast<char>(payload[i]);

  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, message);
  if (error) {
    mqttLog("wol_command", "INVALID_PAYLOAD", "json_parse_error");
    return;
  }

  const char* cmdId = doc["cmd_id"] | "";
  const char* action = doc["action"] | "";
  const char* target = doc["target"] | "";

  if (cmdId[0] == '\0') {
    mqttLog("wol_command", "INVALID_PAYLOAD", "missing_cmd_id");
    return;
  }
  if (lastCmdId == cmdId || activeCmdId == cmdId || (commandQueued && queuedCmdId == cmdId)) {
    activeCmdId = cmdId;
    publishWolStatus("DUPLICATE_COMMAND", "same_cmd_id");
    activeCmdId = "";
    return;
  }
  if (strcmp(action, "WAKE") != 0) {
    activeCmdId = cmdId;
    publishWolStatus("INVALID_PAYLOAD", "unknown_action");
    activeCmdId = "";
    return;
  }
  if (strcmp(target, TARGET_NAME) != 0) {
    activeCmdId = cmdId;
    publishWolStatus("INVALID_TARGET", "target_name_mismatch");
    activeCmdId = "";
    return;
  }

  if (wolState == WAITING_PC || commandQueued ||
      (lastCommandStartMs != 0 && millis() - lastCommandStartMs < COMMAND_COOLDOWN_MS)) {
    enqueueCommand(String(cmdId));
    return;
  }

  startWolProcess(String(cmdId));
}

// ============================================================
// WI-FI AND TIME SYNCHRONIZATION
// ============================================================

void connectWifi() {
  if (WiFi.status() == WL_CONNECTED) {
    wifiConnecting = false;
    return;
  }

  if (wifiConnecting) {
    if (millis() - wifiConnectStartMs >= WIFI_CONNECT_TIMEOUT_MS) {
      Serial.println("[WIFI] Connection timeout; retrying");
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

void beginTimeSync() {
  configTime(0, 0, "pool.ntp.org", "time.google.com", "time.cloudflare.com");
  sntpStarted = true;
  sntpStartMs = millis();
  lastSntpRetryMs = millis();
  Serial.println("[TIME] SNTP synchronization started");
}

void maintainTimeSync() {
  if (WiFi.status() != WL_CONNECTED) {
    clockIsValid = false;
    sntpStarted = false;
    return;
  }

  time_t now = time(nullptr);
  if (now > 1700000000) {
    if (!clockIsValid) Serial.println("[TIME] System clock synchronized");
    clockIsValid = true;
    return;
  }

  clockIsValid = false;
  if (!sntpStarted &&
      (lastSntpRetryMs == 0 || millis() - lastSntpRetryMs >= SNTP_RETRY_INTERVAL_MS)) {
    beginTimeSync();
  }
  if (sntpStarted && millis() - sntpStartMs >= SNTP_SYNC_TIMEOUT_MS) {
    Serial.println("[TIME] SNTP has not synchronized yet; TLS remains disabled until valid time is available");
    lastSntpRetryMs = millis();
    sntpStarted = false;
  }
}

// ============================================================
// MQTT
// ============================================================

void connectMqtt() {
  if (WiFi.status() != WL_CONNECTED || !clockIsValid || mqtt.connected()) return;

  Serial.println("[MQTT] Connecting with certificate and time validation...");
  bool connected = mqtt.connect(MQTT_CLIENT_ID, MQTT_USER, MQTT_PASSWORD,
                                AVAILABILITY_TOPIC, 1, true, "offline");
  if (!connected) {
    Serial.printf("[MQTT] Connection failed, state=%d\n", mqtt.state());
    currentMqttBackoffMs = min(currentMqttBackoffMs * 2, MQTT_RECONNECT_MAX_MS);
    return;
  }

  currentMqttBackoffMs = MQTT_RECONNECT_MIN_MS;
  Serial.println("[MQTT] Connected");

  if (!mqtt.publish(AVAILABILITY_TOPIC, "online", true)) {
    Serial.println("[MQTT] Availability publish failed; check broker publish ACL");
  }
  if (mqtt.subscribe(COMMAND_TOPIC, 1)) Serial.println("[MQTT] Command topic subscribed at QoS 1");
  else Serial.println("[MQTT] Subscribe failed; check broker subscribe ACL");
}

// ============================================================
// SETUP AND LOOP
// ============================================================

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\nESP32 Remote Wake Gateway");
  Serial.printf("Firmware: %s\n", FIRMWARE_VERSION);

  if (!parseMac(SERVER_MAC, targetMac)) {
    Serial.println("[BOOT] ERROR: Invalid server MAC; check SERVER_MAC in secrets.h");
    Serial.flush();
    delay(250);
    ESP.restart();
    delay(1000);
    return;
  }

  tlsClient.setCACert(ROOT_CA); // Certificate validation remains enabled.
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setCallback(mqttCallback);
  mqtt.setBufferSize(1024);
  mqtt.setKeepAlive(30);
  mqtt.setSocketTimeout(2);

  // Bind an ephemeral local UDP port; destination port is WOL_PORT.
  udpReady = udp.begin(0) == 1;
  if (!udpReady) Serial.println("[UDP] Socket initialization failed");

  connectWifi();
}

void loop() {
  connectWifi();
  bool wifiConnected = WiFi.status() == WL_CONNECTED;

  if (wifiConnected && !wifiWasConnected) {
    Serial.printf("[WIFI] Connected: %s\n", WiFi.localIP().toString().c_str());
    Serial.printf("[WIFI] Broadcast: %s\n", WiFi.broadcastIP().toString().c_str());
    Serial.printf("[WIFI] RSSI: %d dBm\n", WiFi.RSSI());
    wifiConnecting = false;
    beginTimeSync();
  } else if (!wifiConnected && wifiWasConnected) {
    Serial.println("[WIFI] Disconnected");
    closeTcpProbe();
  }
  wifiWasConnected = wifiConnected;

  maintainTimeSync();

  if (wifiConnected && clockIsValid && !mqtt.connected()) {
    if (millis() - lastMqttReconnectAttempt >= currentMqttBackoffMs) {
      lastMqttReconnectAttempt = millis();
      connectMqtt();
    }
  }

  // Keep this call frequent; the TCP probe below is non-blocking.
  mqtt.loop();
  sendScheduledWolPackets();
  checkWolProcess();
  processQueuedCommand();
}
