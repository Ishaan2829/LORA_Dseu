#include <WiFi.h>
#include <ArduinoJson.h>
#include <ESPAsyncWebServer.h>
#include <RadioLib.h>
#include <set>
#pragma message("ArduinoJson included")

SX1278 radio = new Module(5, 26, 14, 33);
volatile bool receivedFlag = false;

void setFlag() {
  receivedFlag = true;
}

const char* ap_ssid = "LoRaChat-B";   // change to "LoRaChat-B" on the other board
const char* ap_password = "12345678";

AsyncWebServer server(80);
AsyncWebSocket ws("/ws");

unsigned long msgCounter = 0;
std::set<String> seenMessageIds;
const size_t MAX_SEEN = 200;

String makeMsgId() {
  msgCounter++;
  return String(ESP.getEfuseMac(), HEX) + "-" + String(msgCounter);
}

bool alreadySeen(const String& id) {
  if (seenMessageIds.count(id)) return true;
  seenMessageIds.insert(id);
  if (seenMessageIds.size() > MAX_SEEN) {
    seenMessageIds.erase(seenMessageIds.begin());
  }
  return false;
}

void notifyClients(const String& message) {
  ws.textAll(message);
}

const char index_html[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>LoRa Chat</title>
<style>
  * { box-sizing: border-box; }
  body {
    font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif;
    margin: 0; padding: 0;
    background: #0b141a;
    color: #e9edef;
    height: 100vh;
    overflow: hidden;
  }
  #nameScreen {
    display: flex; flex-direction: column; align-items: center; justify-content: center;
    height: 100vh; gap: 16px; padding: 24px;
  }
  #nameScreen h1 { font-size: 20px; font-weight: 600; margin: 0 0 8px; }
  #nameScreen input {
    padding: 14px 16px; font-size: 16px; border-radius: 10px; border: none;
    width: 100%; max-width: 300px; background: #1f2c34; color: #e9edef;
  }
  #nameScreen input:focus { outline: 2px solid #00a884; }
  #nameScreen button {
    padding: 14px 28px; font-size: 16px; border: none; border-radius: 10px;
    background: #00a884; color: white; font-weight: 600; width: 100%; max-width: 300px;
  }
  #chatScreen { display: none; height: 100vh; flex-direction: column; }
  #header {
    background: #1f2c34; padding: 16px; font-weight: 600; font-size: 16px;
    display: flex; align-items: center; gap: 8px; flex-shrink: 0;
  }
  #statusDot { width: 8px; height: 8px; border-radius: 50%; background: #00a884; }
  #chat {
    flex: 1; overflow-y: auto; padding: 16px; display: flex; flex-direction: column; gap: 8px;
  }
  .msgRow { display: flex; }
  .msgRow.self { justify-content: flex-end; }
  .bubble {
    max-width: 75%; padding: 8px 12px; border-radius: 12px; font-size: 15px; line-height: 1.4;
    word-wrap: break-word;
  }
  .msgRow.self .bubble { background: #005c4b; border-bottom-right-radius: 2px; }
  .msgRow.other .bubble { background: #1f2c34; border-bottom-left-radius: 2px; }
  .senderName { font-size: 12px; font-weight: 600; color: #00a884; margin-bottom: 2px; }
  .metaRow { display: flex; align-items: center; gap: 4px; justify-content: flex-end; margin-top: 4px; }
  .tick { font-size: 13px; }
  .tick.sent { color: #8696a0; }
  .tick.delivered { color: #53bdeb; }
  #inputBar {
    display: flex; padding: 10px; background: #1f2c34; gap: 8px; flex-shrink: 0;
  }
  #msg {
    flex: 1; padding: 12px 16px; border: none; border-radius: 20px; font-size: 15px;
    background: #2a3942; color: #e9edef;
  }
  #sendBtn {
    width: 44px; height: 44px; border-radius: 50%; border: none; background: #00a884;
    color: white; font-size: 18px; flex-shrink: 0; cursor: pointer;
  }
  #sendBtn:disabled { background: #445; opacity: 0.5; }
</style>
</head>
<body>

<div id="nameScreen">
  <h1>LoRa Chat</h1>
  <input id="nameInput" type="text" placeholder="Enter your name" autocomplete="off" maxlength="20">
  <button onclick="setName()">Start chatting</button>
</div>

<div id="chatScreen">
  <div id="header">
    <div id="statusDot"></div>
    <span id="headerText">LoRa Chat</span>
  </div>
  <div id="chat"></div>
  <div id="inputBar">
    <input id="msg" type="text" placeholder="Type a message" autocomplete="off" maxlength="200">
    <button id="sendBtn" onclick="sendMsg()">&#10148;</button>
  </div>
</div>

<script>
  var myName = "";
  var websocket = null;
  var seenIds = new Set();
  var chatDiv = document.getElementById('chat');
  var reconnectTimer = null;

  function connectWS() {
    if (websocket) {
      websocket.onclose = null;
      websocket.onerror = null;
      websocket.close();
      websocket = null;
    }
    if (reconnectTimer) clearTimeout(reconnectTimer);

    websocket = new WebSocket(`ws://${window.location.hostname}/ws`);

    websocket.onopen = function() {
      document.getElementById('statusDot').style.background = '#00a884';
      document.getElementById('sendBtn').disabled = false;
    };

    websocket.onclose = function() {
      document.getElementById('statusDot').style.background = '#e74c3c';
      document.getElementById('sendBtn').disabled = true;
      reconnectTimer = setTimeout(connectWS, 1500);
    };

    websocket.onmessage = function(event) {
      var data;
      try { data = JSON.parse(event.data); } catch(e) { return; }

      if (data.type === "msg") {
        if (seenIds.has(data.id)) return;
        seenIds.add(data.id);

        var row = document.createElement('div');
        row.className = 'msgRow ' + (data.self ? 'self' : 'other');
        row.id = 'msg-' + data.id;

        var bubble = document.createElement('div');
        bubble.className = 'bubble';

        var html = '';
        if (!data.self) html += '<div class="senderName">' + escapeHtml(data.sender) + '</div>';
        html += escapeHtml(data.text);
        if (data.self) {
          html += '<div class="metaRow"><span class="tick sent" id="tick-' + data.id + '">&#10003;</span></div>';
        }
        bubble.innerHTML = html;
        row.appendChild(bubble);
        chatDiv.appendChild(row);
        chatDiv.scrollTop = chatDiv.scrollHeight;
      }

      if (data.type === "ack") {
        var tick = document.getElementById('tick-' + data.id);
        if (tick) {
          tick.className = 'tick delivered';
          tick.innerHTML = '&#10003;&#10003;';
        }
      }
    };
  }

  function escapeHtml(str) {
    var div = document.createElement('div');
    div.textContent = str;
    return div.innerHTML;
  }

  function setName() {
    var input = document.getElementById('nameInput');
    var val = input.value.trim();
    if (val === "") return;
    myName = val;
    document.getElementById('headerText').textContent = 'LoRa Chat — ' + myName;
    document.getElementById('nameScreen').style.display = 'none';
    document.getElementById('chatScreen').style.display = 'flex';
    connectWS();
    document.getElementById('msg').focus();
  }

  function sendMsg() {
    var input = document.getElementById('msg');
    var val = input.value.trim();
    if (val === "" || !websocket || websocket.readyState !== 1) return;
    websocket.send(JSON.stringify({ sender: myName, text: val }));
    input.value = "";
  }

  document.getElementById('msg').addEventListener('keypress', function(e) {
    if (e.key === 'Enter') sendMsg();
  });
  document.getElementById('nameInput').addEventListener('keypress', function(e) {
    if (e.key === 'Enter') setName();
  });

  window.addEventListener('beforeunload', function() {
    if (websocket) { websocket.onclose = null; websocket.close(); }
  });
</script>
</body>
</html>
)rawliteral";

void onWsEvent(AsyncWebSocket *server, AsyncWebSocketClient *client,
               AwsEventType type, void *arg, uint8_t *data, size_t len) {
  if (type == WS_EVT_CONNECT) {
    Serial.printf("WS client #%u connected\n", client->id());
  } else if (type == WS_EVT_DISCONNECT) {
    Serial.printf("WS client #%u disconnected\n", client->id());
  } else if (type == WS_EVT_DATA) {
    ArduinoJson::JsonDocument doc;
    DeserializationError err = deserializeJson(doc, data, len);
    if (err) {
      Serial.print("JSON parse failed: ");
      Serial.println(err.c_str());
      return;
    }

    String sender = doc["sender"] | "Unknown";
    String text = doc["text"] | "";
    if (text.length() == 0) return;

    String msgId = makeMsgId();

    // Show locally immediately
    JsonDocument localDoc;
    localDoc["type"] = "msg";
    localDoc["id"] = msgId;
    localDoc["sender"] = sender;
    localDoc["text"] = text;
    localDoc["self"] = true;
    String localOut;
    serializeJson(localDoc, localOut);
    notifyClients(localOut);

    // Transmit over LoRa as compact JSON
    JsonDocument loraDoc;
    loraDoc["t"] = "m";
    loraDoc["id"] = msgId;
    loraDoc["s"] = sender;
    loraDoc["x"] = text;
    String loraOut;
    serializeJson(loraDoc, loraOut);

    int state = radio.transmit(loraOut);
    if (state != RADIOLIB_ERR_NONE) {
      Serial.print(F("LoRa TX failed, code "));
      Serial.println(state);
    }
    radio.startReceive();
  }
}

void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.print(F("Initializing SX1278... "));
  int state = radio.begin(434.0, 125.0, 9, 7, RADIOLIB_LORA_SYNC_WORD_PRIVATE, 10, 8, 0);
  if (state == RADIOLIB_ERR_NONE) {
    Serial.println(F("success"));
  } else {
    Serial.print(F("failed, code "));
    Serial.println(state);
    while (true) { delay(10); }
  }

  radio.setDio0Action(setFlag, RISING);
  radio.startReceive();

  WiFi.softAP(ap_ssid, ap_password);
  delay(500);
  Serial.print("AP IP address: ");
  Serial.println(WiFi.softAPIP());

  ws.onEvent(onWsEvent);
  server.addHandler(&ws);

  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
    AsyncWebServerResponse *response = request->beginResponse_P(200, "text/html", index_html);
    response->addHeader("Cache-Control", "no-store");
    request->send(response);
  });

  server.begin();
}

void loop() {
  ws.cleanupClients();

  if (receivedFlag) {
    receivedFlag = false;

    String received;
    int state = radio.readData(received);

    if (state == RADIOLIB_ERR_NONE) {
      JsonDocument doc;
      DeserializationError err = deserializeJson(doc, received);

      if (err) {
        Serial.print("LoRa JSON parse failed: ");
        Serial.println(err.c_str());
      } else {
        String type = doc["t"] | "";
        String msgId = doc["id"] | "";

        if (msgId.length() > 0 && !alreadySeen(msgId)) {

          if (type == "m") {
            String sender = doc["s"] | "Unknown";
            String text = doc["x"] | "";

            JsonDocument outDoc;
            outDoc["type"] = "msg";
            outDoc["id"] = msgId;
            outDoc["sender"] = sender;
            outDoc["text"] = text;
            outDoc["self"] = false;
            String outStr;
            serializeJson(outDoc, outStr);
            notifyClients(outStr);

            // send ACK back
            delay(50);
            JsonDocument ackDoc;
            ackDoc["t"] = "a";
            ackDoc["id"] = msgId;
            String ackOut;
            serializeJson(ackDoc, ackOut);
            radio.transmit(ackOut);

          } else if (type == "a") {
            JsonDocument outDoc;
            outDoc["type"] = "ack";
            outDoc["id"] = msgId;
            String outStr;
            serializeJson(outDoc, outStr);
            notifyClients(outStr);
          }
        } else {
          Serial.println("Duplicate message ignored: " + msgId);
        }
      }
    } else {
      Serial.print(F("RX error: "));
      Serial.println(state);
    }

    radio.startReceive();
  }
}