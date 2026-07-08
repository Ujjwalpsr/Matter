#include <Arduino.h>
#include <ArduinoJson.h>
#include <painlessMesh.h>

// ---------------- Mesh settings ----------------
String meshName = "RNTMESH";
String meshPassword = "MESHpassword";
uint16_t meshPort = 5555;
uint8_t meshChannel = 6;

// ---------------- Node settings ----------------
int logicalNodeId = 7;

int ledPin = 2;

// Built-in blue LED is active LOW on many ESP32 boards
int ledOnValue = LOW;
int ledOffValue = HIGH;

bool ledState = false;
uint32_t rootNodeId = 0;
uint32_t statusSeq = 0;
uint32_t lastCommandSeq = 0;

Scheduler userScheduler;
painlessMesh mesh;

// ---------------- Function declarations ----------------
void setLed(bool state);
void sendStatus(String reason);
void sendHello();
void onReceive(uint32_t from, String &msg);
void onNewConnection(uint32_t nodeId);
void onChangedConnections();
bool parseLedState(JsonVariant value, bool &state);

// Periodic status every 5 seconds
Task statusTask(5000, TASK_FOREVER, []() {
  sendStatus("periodic");
});

void setup() {
  Serial.begin(115200);

  pinMode(ledPin, OUTPUT);
  setLed(false);

  Serial.println();
  Serial.println("LED end node started");
  Serial.print("Logical node ID: ");
  Serial.println(logicalNodeId);
  Serial.print("LED pin: GPIO ");
  Serial.println(ledPin);

  mesh.setDebugMsgTypes(ERROR | STARTUP | CONNECTION);

  mesh.init(
    meshName.c_str(),
    meshPassword.c_str(),
    &userScheduler,
    meshPort,
    WIFI_AP_STA,
    meshChannel
  );

  mesh.setHostname("LED_End_Node");
  mesh.setContainsRoot(true);

  mesh.onReceive(&onReceive);
  mesh.onNewConnection(&onNewConnection);
  mesh.onChangedConnections(&onChangedConnections);

  userScheduler.addTask(statusTask);
  statusTask.enable();

  sendHello();
}

void loop() {
  mesh.update();
}

// -----------------------------------------------------
// LED control
// -----------------------------------------------------
void setLed(bool state) {
  ledState = state;

  int gpioValue;

  if (state == true) {
    gpioValue = ledOnValue;
  } else {
    gpioValue = ledOffValue;
  }

  // Force GPIO as output every time
  pinMode(ledPin, OUTPUT);
  digitalWrite(ledPin, gpioValue);

  delay(5);

  int readBack = digitalRead(ledPin);

  Serial.print("LED changed to: ");
  Serial.println(ledState ? "ON" : "OFF");

  Serial.print("GPIO written value: ");
  Serial.println(gpioValue);

  Serial.print("GPIO readback value: ");
  Serial.println(readBack);
}

// -----------------------------------------------------
// Receive mesh message
// -----------------------------------------------------
void onReceive(uint32_t from, String &msg) {
  Serial.println();
  Serial.print("Received from ");
  Serial.print(from);
  Serial.print(": ");
  Serial.println(msg);

  StaticJsonDocument<512> doc;
  DeserializationError error = deserializeJson(doc, msg);

  if (error) {
    Serial.print("JSON parse failed: ");
    Serial.println(error.c_str());
    return;
  }

  String type = doc["type"] | "";

  if (type == "ledCommand") {
    int targetNode = doc["node"] | 0;

    if (targetNode != logicalNodeId) {
      Serial.println("Command ignored. This command is for another node.");
      return;
    }

    bool receivedState;

    if (!parseLedState(doc["state"], receivedState)) {
      Serial.println("Invalid LED state received.");
      return;
    }

    rootNodeId = from;
    lastCommandSeq = doc["seq"] | 0;

    setLed(receivedState);
    sendStatus("commandAck");
  }

  else if (type == "statusRequest") {
    rootNodeId = from;
    sendStatus("request");
  }

else if (type == "ping")
{
    Serial.println("Ping received");

    unsigned long ts = doc["ts"] | 0;

    StaticJsonDocument<128> pongDoc;
    pongDoc["type"] = "pong";
    pongDoc["ts"] = ts;

    String pongMsg;
    serializeJson(pongDoc, pongMsg);

    mesh.sendSingle(from, pongMsg);

    Serial.print("Sent pong: ");
    Serial.println(pongMsg);



}
}

// -----------------------------------------------------
// Accept true/false, 1/0, "ON"/"OFF"
// -----------------------------------------------------
bool parseLedState(JsonVariant value, bool &state) {
  if (value.is<bool>()) {
    state = value.as<bool>();
    return true;
  }

  if (value.is<int>()) {
    state = value.as<int>() != 0;
    return true;
  }

  if (value.is<const char *>()) {
    String text = value.as<String>();
    text.trim();
    text.toLowerCase();

    if (text == "on" || text == "true" || text == "1") {
      state = true;
      return true;
    }

    if (text == "off" || text == "false" || text == "0") {
      state = false;
      return true;
    }
  }

  return false;
}

// -----------------------------------------------------
// Send hello/status to root
// -----------------------------------------------------
void sendHello() {
  sendStatus("hello");
}

void sendStatus(String reason) {
  StaticJsonDocument<384> doc;

  doc["type"] = "ledStatus";
  doc["node"] = logicalNodeId;
  doc["meshNodeId"] = mesh.getNodeId();
  doc["state"] = ledState;
  doc["seq"] = ++statusSeq;
  doc["lastCommandSeq"] = lastCommandSeq;
  doc["reason"] = reason;
  doc["freeHeap"] = ESP.getFreeHeap();

  String msg;
  serializeJson(doc, msg);

  bool sent = false;

  if (rootNodeId != 0 && rootNodeId != mesh.getNodeId()) {
    sent = mesh.sendSingle(rootNodeId, msg);
  }

  if (!sent) {
    sent = mesh.sendBroadcast(msg);
  }

  if (sent) {
    Serial.print("Status sent: ");
    Serial.println(msg);
  } else {
    Serial.println("Status send failed.");
  }
}

// -----------------------------------------------------
// Mesh callbacks
// -----------------------------------------------------
void onNewConnection(uint32_t nodeId) {
  Serial.print("New connection: ");
  Serial.println(nodeId);
  sendHello();
}

void onChangedConnections() {
  Serial.println("Mesh connection changed");
  sendHello();
}
