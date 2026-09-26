#include <WiFi.h>
#include <ArduinoJson.h>
#include <ESPAsyncWebServer.h>
#include <RadioLib.h>
#include <set>
#include <map>
#pragma message("ArduinoJson included")

SX1278 radio = new Module(5, 26, 14, 33);
volatile bool receivedFlag = false;

void setFlag() {
  receivedFlag = true;
}

const char* ap_ssid = "LoRaChat-A";   // change to "LoRaChat-B" on the other board
const char* ap_password = "12345678";

AsyncWebServer server(80);
AsyncWebSocket ws("/ws");

unsigned long msgCounter = 0;
std::set<String> seenMessageIds;
const size_t MAX_SEEN = 200;


// =====================================================
// LoRa LINK-QUALITY STATISTICS
// =====================================================

unsigned long txCount = 0;          // Unique packets transmitted
unsigned long ackRxCount = 0;       // ACKs successfully received
unsigned long txAttempts = 0;       // Total TX attempts
unsigned long retryCount = 0;       // Retransmission attempts
unsigned long duplicateCount = 0;   // Duplicate packets received
unsigned long sequenceErrors = 0;   // Missing sequence numbers

unsigned long totalTxBytes = 0;
unsigned long totalRxBytes = 0;

unsigned long testStartTime = 0;

// RTT / latency
unsigned long totalRtt = 0;
unsigned long minRtt = 0xFFFFFFFF;
unsigned long maxRtt = 0;

float lastRtt = 0;
float jitter = 0;
bool firstRtt = true;

// RSSI / SNR
float lastRssi = 0;
float lastSnr = 0;

float rssiSum = 0;
float snrSum = 0;
unsigned long rssiSamples = 0;

// Message ID -> TX time
std::map<String, unsigned long> txTimestamps;

// Sender -> last received sequence number
std::map<String, unsigned long> lastRxSequence;

// Distance entered manually for the test
float testDistanceMeters = 0.0;

String makeMsgId() {
  msgCounter++;
  return String(ESP.getEfuseMac(), HEX) + "-" + String(msgCounter);
}

// Single source of truth for duplicate detection.
// Returns true if this id has already been seen (and does NOT re-insert it).
// Returns false and inserts the id if this is the first time we've seen it.
bool alreadySeen(const String& id) {
  if (seenMessageIds.count(id)) return true;
  seenMessageIds.insert(id);
  if (seenMessageIds.size() > MAX_SEEN) {
    seenMessageIds.erase(seenMessageIds.begin());
  }
  return false;
}

// Drop TX timestamps that never got an ACK back (lost packets),
// so the map doesn't grow forever over a long test run.
const unsigned long TX_TIMEOUT = 10000; // 10s - no ACK by then, assume lost
void purgeStaleTimestamps() {
  unsigned long now = millis();
  for (auto it = txTimestamps.begin(); it != txTimestamps.end(); ) {
    if (now - it->second > TX_TIMEOUT) {
      it = txTimestamps.erase(it);
    } else {
      ++it;
    }
  }
}

// =====================================================
// CALCULATE LINK STATISTICS
// =====================================================

float getPDR() {
  if (txCount == 0) return 0.0;
  return (100.0 * (float)ackRxCount) / (float)txCount;
}

float getPacketLoss() {
  return 100.0 - getPDR();
}

float getAverageRtt() {
  if (ackRxCount == 0) return 0.0;
  return (float)totalRtt / (float)ackRxCount;
}

float getThroughputKbps() {
  unsigned long elapsed = millis() - testStartTime;
  if (elapsed == 0) return 0.0;
  float seconds = elapsed / 1000.0;
  return ((float)totalTxBytes * 8.0) / seconds / 1000.0;
}

float getGoodputKbps() {
  unsigned long elapsed = millis() - testStartTime;
  if (elapsed == 0) return 0.0;
  float seconds = elapsed / 1000.0;
  return ((float)totalRxBytes * 8.0) / seconds / 1000.0;
}

float getAverageRssi() {
  if (rssiSamples == 0) return 0.0;
  return rssiSum / rssiSamples;
}

float getAverageSnr() {
  if (rssiSamples == 0) return 0.0;
  return snrSum / rssiSamples;
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
  #statsBar {
    background: #111b21; color: #8696a0; font-size: 12px; padding: 6px 16px;
    display: flex; gap: 14px; flex-wrap: wrap; flex-shrink: 0; border-bottom: 1px solid #1f2c34;
  }
  #statsBar span b { color: #e9edef; }
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
  <div id="statsBar">
  <span>RSSI: <b id="statRssi">--</b> dBm</span>
  <span>SNR: <b id="statSnr">--</b> dB</span>
  <span>Latency: <b id="statRtt">--</b> ms</span>
  <span>PDR: <b id="statPdr">--</b>%</span>
  <span>Loss: <b id="statLoss">--</b>%</span>
  <span>Throughput: <b id="statThroughput">--</b> kbps</span>
  <span>Goodput: <b id="statGoodput">--</b> kbps</span>
  <span>Jitter: <b id="statJitter">--</b> ms</span>
  <span>Retries: <b id="statRetries">0</b></span>
  <span>Duplicates: <b id="statDuplicates">0</b></span>
  <span>Seq Errors: <b id="statSeqErrors">0</b></span>
  <span>TX/ACK: <b id="statTxAck">0/0</b></span>
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
  var msgCount = 0;
  const CLEANUP_EVERY = 15;
  const KEEP_LAST = 5; // how many recent bubbles to keep after a cleanup

  function cleanupChat() {
    while (chatDiv.children.length > KEEP_LAST) {
      chatDiv.removeChild(chatDiv.firstChild);
    }
  }

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

        msgCount++;
        if (msgCount % CLEANUP_EVERY === 0) {
          cleanupChat();
        }
      }

      if (data.type === "ack") {
        var tick = document.getElementById('tick-' + data.id);
        if (tick) {
          tick.className = 'tick delivered';
          tick.innerHTML = '&#10003;&#10003;';
        }
      }

      if (data.type === "stats") {
        document.getElementById('statRssi').textContent = Number(data.rssi).toFixed(1);
        document.getElementById('statSnr').textContent = Number(data.snr).toFixed(1);
        document.getElementById('statRtt').textContent = Number(data.rtt).toFixed(1);
        document.getElementById('statPdr').textContent = Number(data.pdr).toFixed(1);
        document.getElementById('statLoss').textContent = Number(data.loss).toFixed(1);
        document.getElementById('statThroughput').textContent = Number(data.throughput).toFixed(2);
        document.getElementById('statGoodput').textContent = Number(data.goodput).toFixed(2);
        document.getElementById('statJitter').textContent = Number(data.jitter).toFixed(1);
        document.getElementById('statRetries').textContent = data.retries;
        document.getElementById('statDuplicates').textContent = data.duplicates;
        document.getElementById('statSeqErrors').textContent = data.seqErrors;
        document.getElementById('statTxAck').textContent = data.acked + '/' + data.tx;
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
    document.getElementById('headerText').textContent = 'LoRa Chat - ' + myName;
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
    loraDoc["q"] = msgCounter;
    String loraOut;
    serializeJson(loraDoc, loraOut);

    txCount++;
    txAttempts++;                       // FIX: was only counted in periodic broadcast
    totalTxBytes += loraOut.length();   // FIX: was only counted in periodic broadcast
    txTimestamps[msgId] = millis();

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
  testStartTime = millis();

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

  unsigned long now = millis();
  static unsigned long lastPurge = 0;
  if (now - lastPurge >= 1000) {
    lastPurge = now;
    purgeStaleTimestamps();
    }

  

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

        if (msgId.length() > 0) {

          // FIX: single source of truth for duplicate detection.
          // alreadySeen() both checks AND inserts, so the old redundant
          // "seenMessageIds.count(msgId)" pre-check + separate insert
          // has been collapsed into one call.
          if (alreadySeen(msgId)) {
            duplicateCount++;
            Serial.println("Duplicate message ignored: " + msgId);
          } else {

            // ================= MESSAGE =================
            if (type == "m") {

              String sender = doc["s"] | "Unknown";
              String text = doc["x"] | "";
              unsigned long seq = doc["q"] | 0UL;

              // Sequence error detection
              if (lastRxSequence.count(sender)) {
                unsigned long expected = lastRxSequence[sender] + 1;

                if (seq > expected) {
                  unsigned long missing = seq - expected;
                  sequenceErrors += missing;

                  Serial.printf(
                    "Sequence error: expected %lu, received %lu (missing %lu)\n",
                    expected, seq, missing
                  );
                }
              }

              lastRxSequence[sender] = seq;

              // Link quality of received packet
              float rssi = radio.getRSSI();
              float snr = radio.getSNR();

              lastRssi = rssi;
              lastSnr = snr;
              rssiSum += rssi;
              snrSum += snr;
              rssiSamples++;
              totalRxBytes += received.length();

              // Show message in web chat
              JsonDocument outDoc;
              outDoc["type"] = "msg";
              outDoc["id"] = msgId;
              outDoc["sender"] = sender;
              outDoc["text"] = text;
              outDoc["self"] = false;
              outDoc["rssi"] = rssi;
              outDoc["snr"] = snr;

              String outStr;
              serializeJson(outDoc, outStr);
              notifyClients(outStr);

              // ACK with receiver-side RSSI/SNR
              delay(50);

              JsonDocument ackDoc;
              ackDoc["t"] = "a";
              ackDoc["id"] = msgId;
              ackDoc["r"] = rssi;
              ackDoc["n"] = snr;
              ackDoc["q"] = seq;

              String ackOut;
              serializeJson(ackDoc, ackOut);

              int ackState = radio.transmit(ackOut);

              if (ackState != RADIOLIB_ERR_NONE) {
                Serial.print(F("ACK TX failed, code "));
                Serial.println(ackState);
              }

            // ================= ACK =================
            } else if (type == "a") {

              JsonDocument outDoc;
              outDoc["type"] = "ack";
              outDoc["id"] = msgId;

              String outStr;
              serializeJson(outDoc, outStr);
              notifyClients(outStr);

              float peerRssi = doc["r"] | 0.0;
              float peerSnr = doc["n"] | 0.0;

              ackRxCount++;

              unsigned long rtt = 0;
              auto it = txTimestamps.find(msgId);

              if (it != txTimestamps.end()) {
                rtt = millis() - it->second;
                txTimestamps.erase(it);

                totalRtt += rtt;

                if (rtt < minRtt) minRtt = rtt;
                if (rtt > maxRtt) maxRtt = rtt;

                if (!firstRtt) {
                  float difference = fabs((float)rtt - lastRtt);
                  jitter = (jitter * 0.75) + (difference * 0.25);
                } else {
                  firstRtt = false;
                }

                lastRtt = rtt;
              }

              lastRssi = peerRssi;
              lastSnr = peerSnr;

              float pdr = getPDR();
              float loss = getPacketLoss();
              float throughput = getThroughputKbps();
              float goodput = getGoodputKbps();

              JsonDocument statsDoc;
              statsDoc["type"] = "stats";
              statsDoc["rssi"] = lastRssi;
              statsDoc["snr"] = lastSnr;
              statsDoc["rtt"] = rtt;
              statsDoc["avgRtt"] = getAverageRtt();
              statsDoc["minRtt"] = (minRtt == 0xFFFFFFFF) ? 0 : minRtt;
              statsDoc["maxRtt"] = maxRtt;
              statsDoc["jitter"] = jitter;
              statsDoc["pdr"] = pdr;
              statsDoc["loss"] = loss;
              statsDoc["throughput"] = throughput;
              statsDoc["goodput"] = goodput;
              statsDoc["tx"] = txCount;
              statsDoc["acked"] = ackRxCount;
              statsDoc["attempts"] = txAttempts;
              statsDoc["retries"] = retryCount;
              statsDoc["duplicates"] = duplicateCount;
              statsDoc["seqErrors"] = sequenceErrors;
              statsDoc["avgRssi"] = getAverageRssi();
              statsDoc["avgSnr"] = getAverageSnr();
              statsDoc["distance"] = testDistanceMeters;

              String statsOut;
              serializeJson(statsDoc, statsOut);
              notifyClients(statsOut);

              Serial.println();
              Serial.println(F("========== LoRa LINK STATISTICS =========="));
              Serial.printf("RSSI            : %.1f dBm\n", lastRssi);
              Serial.printf("SNR             : %.1f dB\n", lastSnr);
              Serial.printf("Latency / RTT   : %lu ms\n", rtt);
              Serial.printf("Average RTT     : %.1f ms\n", getAverageRtt());
              Serial.printf("Min RTT         : %lu ms\n",
                            (minRtt == 0xFFFFFFFF) ? 0 : minRtt);
              Serial.printf("Max RTT         : %lu ms\n", maxRtt);
              Serial.printf("Jitter          : %.1f ms\n", jitter);
              Serial.printf("PDR             : %.2f %%\n", pdr);
              Serial.printf("Packet Loss     : %.2f %%\n", loss);
              Serial.printf("Throughput      : %.2f kbps\n", throughput);
              Serial.printf("Goodput         : %.2f kbps\n", goodput);
              Serial.printf("Retries         : %lu\n", retryCount);
              Serial.printf("Duplicates      : %lu\n", duplicateCount);
              Serial.printf("Sequence Errors : %lu\n", sequenceErrors);
              Serial.printf("TX / ACK        : %lu / %lu\n", txCount, ackRxCount);
              Serial.printf("Average RSSI    : %.1f dBm\n", getAverageRssi());
              Serial.printf("Average SNR     : %.1f dB\n", getAverageSnr());
              Serial.println(F("=========================================="));
            }
          }
        }
      }
    } else {
      Serial.print(F("RX error: "));
      Serial.println(state);
    }

    radio.startReceive();
  }
}
