#include <Arduino.h>
#include <string.h>



#ifndef ESP32
#error "Matter support is ESP32-only. Use an ESP32 board and Arduino-ESP32 with Matter enabled."
#endif

#include <Matter.h>
#include <Preferences.h>
#include <WiFi.h>
#include <esp_err.h>
#include <esp_wifi.h>
#include <ArduinoJson.h>
#include <painlessMesh.h>
#include <WiFiClient.h>
#include <PubSubClient.h>
extern "C" bool arduino_matter_mesh_preserve_apsta;

#define MESH_PREFIX "RNTMESH"
#define MESH_PASSWORD "MESHpassword"
#define MESH_PORT 5555
#define MESH_CHANNEL 6

#define STATION_SSID "NETGEAR31"
#define STATION_PASSWORD "basicshoe744"

IPAddress mqttBroker(192,168,1,41);

#define HOSTNAME "Matter_Bridge"

constexpr uint8_t LED_NODE_COUNT = 7;
constexpr uint32_t NODE_OFFLINE_MS = 30000;

struct LedNodeMap {
  uint8_t logicalId;
  uint32_t meshNodeId;
  bool state;
  bool online;
  bool desiredState;
  bool pendingCommand;
  uint32_t lastSeen;
  uint32_t lastCommandSeq;
};

void receivedCallback(const uint32_t &from, const String &msg);
bool matterLedChanged(uint8_t index, bool state);
bool prepareMatterRadio();
bool ensureSharedRadioMode(const char *where);
void beginMatter();
bool connectMatterStation();
void beginMesh();
bool restoreMeshSoftAP(const char *where);
void maintainMeshSoftAP();
void printCommissioningStatus();
void printMatterNetworkStatus();
void printBridgeStatus();
void printNodeMap();
void sendStatusRequest();
void sendLedCommand(uint8_t index, bool state, const char *reason);
void handleLedStatus(uint32_t from, JsonDocument &doc);
void handleNodeHello(uint32_t from, JsonDocument &doc);
void learnNode(uint8_t logicalId, uint32_t meshNodeId, bool state);
void updateMatterFromNode(uint8_t index, bool state);
void newConnectionCallback(uint32_t nodeId);
void changedConnectionCallback();
int8_t indexFromLogicalId(uint8_t logicalId);
String buildLedCommandJson(uint8_t logicalId, bool state, uint32_t seq);
void mqttCallback(
        char* topic,
        uint8_t* payload,
        unsigned int length);

void handlePong(uint32_t from, JsonDocument &doc);
void sendPing(uint32_t target);

const char *boolText(bool value);
IPAddress getlocalIP();

painlessMesh mesh;
WiFiClient wifiClient;
PubSubClient mqttClient(wifiClient);
MatterOnOffLight matterLeds[LED_NODE_COUNT];
Preferences matterPrefs;

LedNodeMap ledNodes[LED_NODE_COUNT] = {
  {1, 0, false, false, false, false, 0, 0},
  {2, 0, false, false, false, false, 0, 0},
  {3, 0, false, false, false, false, 0, 0},
  {4, 0, false, false, false, false, 0, 0},
  {5, 0, false, false, false, false, 0, 0},
  {6, 0, false, false, false, false, 0, 0},
  {7, 0, false, false, false, false, 0, 0},
};

IPAddress myIP(0, 0, 0, 0);
bool matterStarted = false;
bool meshStarted = false;
bool updatingMatterFromMesh = false;

// RTT variables
unsigned long pingStartTime = 0;
uint32_t lastPingedNode = 0;

uint32_t lastCommissioningPrint = 0;
uint32_t lastBridgeStatusPrint = 0;
uint32_t lastStatusRequest = 0;
uint8_t activeMeshChannel = MESH_CHANNEL;

void setup() {
  Serial.begin(115200);

  prepareMatterRadio();
  connectMatterStation();
  beginMatter();
  ensureSharedRadioMode("Matter.begin()");
  beginMesh();
  restoreMeshSoftAP("mesh start");
  mqttClient.setServer(mqttBroker,1883);
mqttClient.setCallback(mqttCallback);
mqttClient.setBufferSize(1024);
  sendStatusRequest();
}

void loop() {
  if (meshStarted) {
    mesh.update();
  }

  IPAddress stationIP = getlocalIP();
  if (myIP != stationIP) {
    myIP = stationIP;
    Serial.println("Station IP is " + myIP.toString());
  }

  uint32_t now = millis();
  for (uint8_t i = 0; i < LED_NODE_COUNT; ++i) {
    if (ledNodes[i].online && now - ledNodes[i].lastSeen > NODE_OFFLINE_MS) {
      ledNodes[i].online = false;
      Serial.printf("LED node %u is offline\r\n", ledNodes[i].logicalId);
      printNodeMap();
    }
  }

  if (meshStarted && now - lastStatusRequest > 15000) {
    sendStatusRequest();
  }


if (!mqttClient.connected())
{
    static unsigned long lastReconnect=0;

    if (millis()-lastReconnect>5000)
    {
        lastReconnect=millis();

        if (mqttClient.connect("MatterMeshBridge"))
        {
            mqttClient.subscribe(
                "painlessMesh/to/#");

            mqttClient.publish(
                "painlessMesh/from/gateway",
                "Reconnected");
        }
    }
}

mqttClient.loop();


if (Serial.available())
{
    String cmd =
        Serial.readStringUntil('\n');

    cmd.trim();

    if (cmd.startsWith("ping "))
    {
        uint32_t node =
            cmd.substring(5).toInt();

        sendPing(node);
    }
}


  printCommissioningStatus();
  printBridgeStatus();
  maintainMeshSoftAP();
}

void beginMatter() {
  if (matterStarted) {
    return;
  }

  matterPrefs.begin("MatterLedBridge", false);
  for (uint8_t i = 0; i < LED_NODE_COUNT; ++i) {
    char key[8];
    snprintf(key, sizeof(key), "led%u", ledNodes[i].logicalId);
    bool lastState = matterPrefs.getBool(key, false);
    ledNodes[i].state = lastState;
    ledNodes[i].desiredState = lastState;

    if (!matterLeds[i].begin(lastState)) {
      Serial.printf("Matter LED %u endpoint failed to start.\r\n", ledNodes[i].logicalId);
      continue;
    }

    matterLeds[i].onChangeOnOff([i](bool state) {
      return matterLedChanged(i, state);
    });
  }

  arduino_matter_mesh_preserve_apsta = false;
  Matter.begin();
  matterStarted = true;
  arduino_matter_mesh_preserve_apsta = true;

  Serial.println("Matter started with five mapped LED endpoints.");
  printMatterNetworkStatus();
  if (Matter.isDeviceCommissioned()) {
    Serial.println("Matter node is already commissioned.");
    for (uint8_t i = 0; i < LED_NODE_COUNT; ++i) {
      matterLeds[i].updateAccessory();
    }
  } else {
    printCommissioningStatus();
  }
}

void printCommissioningStatus() {
  if (!matterStarted || Matter.isDeviceCommissioned()) {
    return;
  }

  uint32_t now = millis();
  if (now - lastCommissioningPrint < 5000) {
    return;
  }
  lastCommissioningPrint = now;

  Serial.println("Matter node is not commissioned yet.");
  Serial.println("Manual pairing code: " + Matter.getManualPairingCode());
  Serial.println("QR code URL: " + Matter.getOnboardingQRCodeUrl());
}

bool matterLedChanged(uint8_t index, bool state) {
  if (index >= LED_NODE_COUNT) {
    return false;
  }

  uint8_t logicalId = ledNodes[index].logicalId;
  Serial.printf("Matter LED %u changed to %s\r\n", logicalId, boolText(state));
  ledNodes[index].desiredState = state;

  char key[8];
  snprintf(key, sizeof(key), "led%u", logicalId);
  matterPrefs.putBool(key, state);

  if (!updatingMatterFromMesh) {
    sendLedCommand(index, state, "Matter");
  }

  return true;
}

bool prepareMatterRadio() {
  WiFi.persistent(false);
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  WiFi.setAutoReconnect(false);
#else
  WiFi.setAutoConnect(false);
#endif
  WiFi.setSleep(false);

  Serial.println("Starting WIFI_STA radio for Matter bootstrap.");
  if (!WiFi.mode(WIFI_STA)) {
    Serial.println("Failed to start WIFI_STA radio.");
    return false;
  }

  Serial.printf("Radio mode after prepare: %d\r\n", WiFi.getMode());
  return true;
}

bool ensureSharedRadioMode(const char *where) {
  wifi_mode_t mode = WIFI_MODE_NULL;
  esp_err_t err = esp_wifi_get_mode(&mode);
  if (err != ESP_OK) {
    Serial.printf("Could not read WiFi mode after %s: %s\r\n", where, esp_err_to_name(err));
    return false;
  }

  if ((mode & WIFI_MODE_AP) && (mode & WIFI_MODE_STA)) {
    return true;
  }

  Serial.printf("Restoring WIFI_AP_STA after %s; current mode=%d\r\n", where, mode);
  if (!WiFi.mode(WIFI_AP_STA)) {
    Serial.println("Failed to restore WIFI_AP_STA through Arduino WiFi.");
    return false;
  }

  err = esp_wifi_get_mode(&mode);
  if (err == ESP_OK) {
    Serial.printf("WiFi mode restored after %s: %d\r\n", where, mode);
  }

  return true;
}

bool connectMatterStation() {
  wifi_mode_t mode = WIFI_MODE_NULL;
  if (esp_wifi_get_mode(&mode) != ESP_OK || !(mode & WIFI_MODE_STA)) {
    Serial.println("Restoring WIFI_STA before station connect.");
    if (!WiFi.mode(WIFI_STA)) {
      Serial.println("Failed to restore WIFI_STA for station connect.");
      return false;
    }
  }

  Serial.println("Connecting Matter station.");
  esp_wifi_disconnect();
  delay(100);

  wifi_config_t staConfig = {};
  strncpy(reinterpret_cast<char *>(staConfig.sta.ssid), STATION_SSID, sizeof(staConfig.sta.ssid));
  strncpy(reinterpret_cast<char *>(staConfig.sta.password), STATION_PASSWORD, sizeof(staConfig.sta.password));
  staConfig.sta.channel = 0;
  staConfig.sta.scan_method = WIFI_FAST_SCAN;
  staConfig.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
  staConfig.sta.threshold.rssi = -127;
  staConfig.sta.pmf_cfg.capable = true;

  esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &staConfig);
  if (err != ESP_OK) {
    Serial.printf("Matter station config failed: %s\r\n", esp_err_to_name(err));
    return false;
  }

  err = esp_wifi_connect();
  if (err != ESP_OK) {
    Serial.printf("Matter station connect start failed: %s\r\n", esp_err_to_name(err));
    return false;
  }

  uint32_t startedAt = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startedAt < 45000) {
    delay(100);
  }

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Matter station connection timed out.");
    return false;
  }

  activeMeshChannel = WiFi.channel();
  if (activeMeshChannel == 0) {
    activeMeshChannel = MESH_CHANNEL;
  }

  Serial.println("Matter station connected: " + WiFi.localIP().toString());
  Serial.printf("Using WiFi channel %u for mesh AP.\r\n", activeMeshChannel);
  return true;
}

void beginMesh() {
  mesh.setDebugMsgTypes(ERROR | STARTUP | CONNECTION);

  mesh.init(MESH_PREFIX, MESH_PASSWORD, MESH_PORT, WIFI_AP, activeMeshChannel, 0, MAX_CONN, true);
  mesh.onReceive(&receivedCallback);
  mesh.onNewConnection(&newConnectionCallback);
  mesh.onChangedConnections(&changedConnectionCallback);
  mesh.setHostname(HOSTNAME);
  mesh.setRoot(true);
  mesh.setContainsRoot(true);

  meshStarted = true;
  Serial.println("painlessMesh AP started.");
}

bool restoreMeshSoftAP(const char *where) {
  if (!meshStarted) {
    return false;
  }

  Serial.printf("Restoring mesh SoftAP after %s.\r\n", where);
  if (!ensureSharedRadioMode(where)) {
    return false;
  }

  uint32_t nodeId = mesh.getNodeId();
  IPAddress apIp(10, (nodeId & 0xFF00) >> 8, nodeId & 0xFF, 1);
  IPAddress netmask(255, 255, 255, 0);

  if (!WiFi.softAPConfig(apIp, apIp, netmask)) {
    Serial.println("Mesh SoftAP config restore failed.");
    return false;
  }

  if (!WiFi.softAP(MESH_PREFIX, MESH_PASSWORD, activeMeshChannel, 0, MAX_CONN)) {
    Serial.println("Mesh SoftAP restore failed.");
    return false;
  }

  Serial.printf("Mesh SoftAP active: %s channel=%u mode=%d\r\n",
                WiFi.softAPIP().toString().c_str(),
                activeMeshChannel,
                WiFi.getMode());
  return true;
}

void maintainMeshSoftAP() {
  static uint32_t lastSoftAPCheck = 0;
  uint32_t now = millis();
  if (now - lastSoftAPCheck < 5000) {
    return;
  }
  lastSoftAPCheck = now;

  wifi_mode_t mode = WIFI_MODE_NULL;
  if (esp_wifi_get_mode(&mode) != ESP_OK) {
    return;
  }

  if (!(mode & WIFI_MODE_AP) || WiFi.softAPIP() == IPAddress(0, 0, 0, 0)) {
    restoreMeshSoftAP("SoftAP monitor");
  }
}

void sendLedCommand(uint8_t index, bool state, const char *reason) {
  if (index >= LED_NODE_COUNT) {
    return;
  }

  LedNodeMap &node = ledNodes[index];
  node.desiredState = state;
  node.pendingCommand = true;
  uint32_t seq = ++node.lastCommandSeq;
  String msg = buildLedCommandJson(node.logicalId, state, seq);

  bool sent = false;
  if (meshStarted && node.meshNodeId != 0) {
    sent = mesh.sendSingle(node.meshNodeId, msg);
    Serial.printf("%s -> LED %u via mesh node %u: %s\r\n",
                  reason, node.logicalId, node.meshNodeId, msg.c_str());
  }

  if (meshStarted && !sent) {
    sent = mesh.sendBroadcast(msg);
    Serial.printf("%s -> LED %u broadcast: %s\r\n", reason, node.logicalId, msg.c_str());
  }

  if (!sent) {
    Serial.printf("LED %u command queued until node reports online.\r\n", node.logicalId);
  }
}

String buildLedCommandJson(uint8_t logicalId, bool state, uint32_t seq) {
  String msg = "{\"type\":\"ledCommand\",\"node\":";
  msg += logicalId;
  msg += ",\"state\":";
  msg += state ? "true" : "false";
  msg += ",\"seq\":";
  msg += seq;
  msg += "}";
  return msg;
}

void sendStatusRequest() {
  if (!meshStarted) {
    return;
  }

  lastStatusRequest = millis();
  String msg = "{\"type\":\"statusRequest\"}";
  mesh.sendBroadcast(msg);
}

void newConnectionCallback(uint32_t nodeId) {
  Serial.printf("matterBridge: New mesh node %u\r\n", nodeId);
  sendStatusRequest();
}

void changedConnectionCallback() {
  auto nodes = mesh.getNodeList(false);
  Serial.printf("matterBridge: Mesh changed, %u mesh links:", nodes.size());
  for (auto &&id : nodes) {
    Serial.printf(" %u", id);
  }
  Serial.println();
  sendStatusRequest();
}

void receivedCallback(const uint32_t &from, const String &msg) {
  Serial.printf("matterBridge: Received from %u msg=%s\r\n", from, msg.c_str());

  DynamicJsonDocument doc(384);
  DeserializationError error = deserializeJson(doc, msg);
  if (error) {
    return;
  }

  const char *type = doc["type"] | "";
if (strcmp(type, "ledStatus") == 0)
{
    handleLedStatus(from, doc);
}
else if (strcmp(type, "nodeHello") == 0)
{
    handleNodeHello(from, doc);
}
else if (strcmp(type, "pong") == 0)
{
    handlePong(from, doc);
}
}

void handleLedStatus(uint32_t from, JsonDocument &doc) {
  uint8_t logicalId = doc["node"] | 0;
  bool state = doc["state"] | false;
  learnNode(logicalId, from, state);
}

void handleNodeHello(uint32_t from, JsonDocument &doc) {
  uint8_t logicalId = doc["node"] | 0;
  bool state = doc["state"] | false;
  learnNode(logicalId, from, state);
}

void sendPing(uint32_t target)
{
    if (!mesh.isConnected(target))
    {
        Serial.printf(
            "Node %u not connected\n",
            target);
        return;
    }

    unsigned long ts = millis();

    String pingMsg =
        "{\"type\":\"ping\",\"ts\":" +
        String(ts) +
        "}";

    mesh.sendSingle(
        target,
        pingMsg);

    lastPingedNode = target;

    Serial.printf(
        "Ping sent to node %u\n",
        target);
}

void handlePong(uint32_t from, JsonDocument &doc)
{
    unsigned long ts = doc["ts"] | 0;

    unsigned long rtt = millis() - ts;

    Serial.printf(
        "Latency from node %u = %lu ms\n",
        from,
        rtt);

    // Optional: publish to Node-RED through Serial
    String latencyMsg =
        "Latency from node " +
        String(from) +
        " = " +
        String(rtt) +
        " ms";

if(mqttClient.connected())
{
    mqttClient.publish(
        "painlessMesh/from/latency",
        latencyMsg.c_str());
}

Serial.println(latencyMsg);
}

void learnNode(uint8_t logicalId, uint32_t meshNodeId, bool state) {
  int8_t index = indexFromLogicalId(logicalId);
  if (index < 0) {
    Serial.printf("Ignoring unknown logical LED node %u from mesh node %u\r\n", logicalId, meshNodeId);
    return;
  }

  LedNodeMap &node = ledNodes[index];
  bool wasOnline = node.online;
  uint32_t previousMeshNodeId = node.meshNodeId;
  node.meshNodeId = meshNodeId;
  node.online = true;
  node.lastSeen = millis();

  if (!wasOnline || previousMeshNodeId != meshNodeId) {
    Serial.printf("Logical LED %u is online at mesh node %u\r\n", logicalId, meshNodeId);
    printNodeMap();
  }

  if (node.state != state) {
    node.state = state;
    updateMatterFromNode(index, state);
  }

  if (node.pendingCommand && node.desiredState != state) {
    sendLedCommand(index, node.desiredState, "Pending retry");
  } else {
    node.pendingCommand = false;
  }
}

void updateMatterFromNode(uint8_t index, bool state) {
  if (index >= LED_NODE_COUNT || !matterStarted) {
    return;
  }

  uint8_t logicalId = ledNodes[index].logicalId;
  char key[8];
  snprintf(key, sizeof(key), "led%u", logicalId);
  matterPrefs.putBool(key, state);

  if (matterLeds[index].getOnOff() == state) {
    return;
  }

  updatingMatterFromMesh = true;
  matterLeds[index].setOnOff(state);
  updatingMatterFromMesh = false;
  Serial.printf("Mesh LED %u status updated Matter to %s\r\n", logicalId, boolText(state));
}

int8_t indexFromLogicalId(uint8_t logicalId) {
  if (logicalId == 0 || logicalId > LED_NODE_COUNT) {
    return -1;
  }
  return logicalId - 1;
}

void printMatterNetworkStatus() {
  Serial.printf("Matter WiFi station enabled: %s\r\n",
                Matter.isWiFiStationEnabled() ? "yes" : "no");
  Serial.printf("Matter WiFi AP enabled: %s\r\n",
                Matter.isWiFiAccessPointEnabled() ? "yes" : "no");
  Serial.printf("WiFi mode after Matter.begin(): %d\r\n", WiFi.getMode());
}

void printBridgeStatus() {
  uint32_t now = millis();
  if (now - lastBridgeStatusPrint < 10000) {
    return;
  }
  lastBridgeStatusPrint = now;

  Serial.printf("Bridge status: WiFiStatus=%d mode=%d STA=%s channel=%u mesh=%s Matter=%s\r\n",
                WiFi.status(),
                WiFi.getMode(),
                WiFi.localIP().toString().c_str(),
                WiFi.channel(),
                meshStarted ? "started" : "stopped",
                Matter.isDeviceCommissioned() ? "commissioned" : "not-commissioned");
  printNodeMap();
}

void printNodeMap() {
  Serial.println("LED node map:");
  for (uint8_t i = 0; i < LED_NODE_COUNT; ++i) {
    Serial.printf("  LED %u -> mesh=%u %s state=%s desired=%s\r\n",
                  ledNodes[i].logicalId,
                  ledNodes[i].meshNodeId,
                  ledNodes[i].online ? "online" : "offline",
                  boolText(ledNodes[i].state),
                  boolText(ledNodes[i].desiredState));
  }
}

const char *boolText(bool value) {
  return value ? "ON" : "OFF";
}

IPAddress getlocalIP() {
  return WiFi.localIP();
}

void mqttCallback(
        char* topic,
        uint8_t* payload,
        unsigned int length)
{
    String msg;
for(unsigned int i=0;i<length;i++)
    msg += (char)payload[i];

    // String msg=(char*)payload;
    String topicStr=String(topic);

    String targetStr=
        topicStr.substring(16);

    if(targetStr=="gateway")
    {
        if(msg=="pingAll")
        {
            auto nodes=
                mesh.getNodeList();

            for(auto &&id:nodes)
            {
                sendPing(id);
            }
        }

        if(msg=="getNodes")
        {
            auto nodes=
                mesh.getNodeList(true);

            String nodeList;

            for(auto &&id:nodes)
                nodeList+=String(id)+" ";

            mqttClient.publish(
                "painlessMesh/from/gateway",
                nodeList.c_str());
        }
        else if(msg=="getTree")
{
    String tree = mesh.subConnectionJson();

    Serial.println("TREE START");
    Serial.println(tree);
    Serial.println("TREE END");

    mqttClient.publish(
        "painlessMesh/from/gateway",
        tree.c_str());

    Serial.println("Tree published to MQTT");
}

    }
    else
    {
        uint32_t target=
            strtoul(
                targetStr.c_str(),
                NULL,
                10);

        if(msg=="ping")
        {
            sendPing(target);
        }
    }
}