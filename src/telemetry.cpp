#include "telemetry.h"
#include <WiFi.h>
#include <WiFiManager.h>
#include <WebSocketsClient.h>
#include <ArduinoJson.h>
#include <time.h>

namespace telemetry {
namespace {

SemaphoreHandle_t g_mtx = nullptr;
Snapshot          g_snap;
bool              g_have = false;
NetInfo           g_net;

volatile bool     g_reqPortal = false;
volatile bool     g_wsUp = false;
volatile bool     g_txNow = false;
volatile bool     g_haveTx = false;
volatile uint32_t g_lastTxAt = 0;

QueueHandle_t     g_inQ = nullptr;     // String*  server  -> main loop
QueueHandle_t     g_outQ = nullptr;    // String*  main loop -> server
WebSocketsClient  g_ws;

inline float rnd(float v, int d) {
  float k = powf(10.0f, d);
  return roundf(v * k) / k;
}

void flushOut() {
  String* p = nullptr;
  while (xQueueReceive(g_outQ, &p, 0) == pdTRUE) delete p;
}

// Frame layout follows the web app's telemetry sample; relayStates is an additive field.
String buildTelemetry(const Snapshot& s) {
  JsonDocument doc;
  doc["type"]               = "telemetry";
  doc["power"]              = rnd(s.m.power, 1);          // W
  doc["energy"]             = rnd(s.m.energy, 3);         // kWh
  doc["voltage"]            = rnd(s.m.voltage, 1);        // V
  doc["current"]            = rnd(s.m.current, 3);        // A
  doc["frequency"]          = rnd(s.m.frequency, 1);      // Hz
  doc["powerFactor"]        = rnd(s.m.pf, 2);
  doc["sheddingActive"]     = s.sheddingActive;
  doc["ackedConfigVersion"] = s.configVersion;
  JsonArray rs = doc["relayStates"].to<JsonArray>();      // actual relay states, CH1..CH8
  for (uint8_t i = 0; i < cfg::NUM_CHANNELS; ++i) rs.add(s.ch[i].on);

  String out;
  serializeJson(doc, out);
  return out;
}

void sendTelemetry() {
  Snapshot s;
  if (xSemaphoreTake(g_mtx, pdMS_TO_TICKS(100)) != pdTRUE) return;
  s = g_snap;
  xSemaphoreGive(g_mtx);

  const String out = buildTelemetry(s);
  g_ws.sendTXT(out.c_str());
  g_lastTxAt = millis();
  g_haveTx = true;
}

void wsEvent(WStype_t type, uint8_t* payload, size_t length) {
  switch (type) {
    case WStype_CONNECTED:
      g_wsUp = true;
      g_txNow = true;                              // report immediately; the ack carries the settings
      Serial.println("[CLOUD] connected");
      break;
    case WStype_DISCONNECTED:
      g_wsUp = false;
      Serial.println("[CLOUD] disconnected");
      break;
    case WStype_TEXT: {
      String* m = new String();
      m->concat((const char*)payload, (unsigned int)length);
      if (xQueueSend(g_inQ, &m, 0) != pdTRUE) delete m;     // queue full: drop
      break;
    }
    default:
      break;
  }
}

void updateNet(NetState st, WiFiManager& wm) {
  NetInfo n;
  n.state = st;
  const bool conn = (WiFi.status() == WL_CONNECTED);

  const String ssid = conn ? WiFi.SSID() : wm.getWiFiSSID();
  strlcpy(n.ssid, ssid.c_str(), sizeof n.ssid);

  if (st == NetState::Portal)  strlcpy(n.ip, WiFi.softAPIP().toString().c_str(), sizeof n.ip);
  else if (conn)               strlcpy(n.ip, WiFi.localIP().toString().c_str(), sizeof n.ip);
  n.rssi = conn ? WiFi.RSSI() : 0;
  n.cloud = g_wsUp;

  if (xSemaphoreTake(g_mtx, pdMS_TO_TICKS(50)) == pdTRUE) {
    g_net = n;
    xSemaphoreGive(g_mtx);
  }
}

void task(void*) {
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.setHostname(DEVICE_ID);

  WiFiManager wm;
  wm.setDebugOutput(false);
  wm.setConfigPortalBlocking(false);            // keep this task responsive while the portal is open
  wm.setConfigPortalTimeout(cfg::PORTAL_TIMEOUT_S);

  const char* apPass = strlen(PORTAL_AP_PASS) > 0 ? PORTAL_AP_PASS : nullptr;

  NetState st = NetState::Connecting;
  NetState lastSt = NetState::Offline;
  uint32_t stateSince = millis(), lastTry = millis(), lastNet = 0, nextSend = 0;
  bool ntpStarted = false, wsStarted = false, wasConn = false;

  auto openPortal = [&]() {
    Serial.printf("[NET] setup portal: AP '%s'\n", PORTAL_AP_NAME);
    wm.startConfigPortal(PORTAL_AP_NAME, apPass);   // non-blocking: returns immediately
    st = NetState::Portal;
    stateSince = millis();
  };
  auto connectSaved = [&]() {
    Serial.println("[NET] connecting to saved network");
    WiFi.begin();                                   // uses credentials stored by WiFiManager
    st = NetState::Connecting;
    stateSince = millis();
  };

  if (wm.getWiFiIsSaved()) connectSaved();
  else                     openPortal();            // first boot: nothing saved yet

  for (;;) {
    const uint32_t now = millis();
    const bool conn = (WiFi.status() == WL_CONNECTED);

    if (g_reqPortal) {                              // requested from the menu
      g_reqPortal = false;
      if (!wm.getConfigPortalActive()) openPortal();
    }

    switch (st) {
      case NetState::Portal:
        wm.process();
        if (!wm.getConfigPortalActive()) {          // credentials saved, or portal timed out
          st = conn ? NetState::Connected : NetState::Offline;
          lastTry = now;
        }
        break;

      case NetState::Connecting:
        if (conn) st = NetState::Connected;
        else if (now - stateSince >= cfg::WIFI_CONNECT_TIMEOUT_MS) { st = NetState::Offline; lastTry = now; }
        break;

      case NetState::Connected:
        if (!conn) { st = NetState::Offline; lastTry = now; }   // auto-reconnect may bring it back
        break;

      case NetState::Offline:
        if (conn) { st = NetState::Connected; break; }
        if (now - lastTry >= cfg::WIFI_RETRY_MS) {
          lastTry = now;
          if (wm.getWiFiIsSaved()) { WiFi.disconnect(); connectSaved(); }
          else                     openPortal();
        }
        break;
    }

    if (conn) {
      if (!ntpStarted) {
        configTzTime(TIMEZONE_POSIX, "pool.ntp.org", "time.google.com");
        ntpStarted = true;
        Serial.printf("[NET] connected to %s, IP %s\n", WiFi.SSID().c_str(), WiFi.localIP().toString().c_str());
      }
      if (!wsStarted) {
        g_ws.onEvent(wsEvent);
        g_ws.beginSSL(WS_HOST, WS_PORT, WS_PATH, "", "");   // no cert pinning, no subprotocol
        g_ws.setReconnectInterval(cfg::WS_RECONNECT_MS);
        g_ws.enableHeartbeat(15000, 3000, 2);               // ping every 15 s, drop after 2 missed pongs
        wsStarted = true;
      }
      g_ws.loop();

      if (g_wsUp) {
        if (g_txNow) { g_txNow = false; nextSend = now; }

        String* p = nullptr;                                // frames queued by the main loop
        while (xQueueReceive(g_outQ, &p, 0) == pdTRUE) { g_ws.sendTXT(p->c_str()); delete p; }

        if (g_have && reached(now, nextSend)) {
          nextSend = now + cfg::TELEMETRY_INTERVAL_MS;      // fixed 30 s cadence
          sendTelemetry();
        }
      }
    } else if (wasConn) {                                   // WiFi just dropped
      if (wsStarted) g_ws.disconnect();
      g_wsUp = false;
      flushOut();
    }
    wasConn = conn;

    if (st != lastSt || now - lastNet >= 500) {
      lastSt = st;
      lastNet = now;
      updateNet(st, wm);
    }

    vTaskDelay(pdMS_TO_TICKS(st == NetState::Portal ? 10 : 20));
  }
}

}  // namespace

void begin() {
  g_mtx  = xSemaphoreCreateMutex();
  g_inQ  = xQueueCreate(6, sizeof(String*));
  g_outQ = xQueueCreate(6, sizeof(String*));
  xTaskCreatePinnedToCore(task, "net", 12288, nullptr, 1, nullptr, 0);
}

void publish(const Snapshot& s) {
  if (xSemaphoreTake(g_mtx, pdMS_TO_TICKS(20)) == pdTRUE) {
    g_snap = s;
    g_have = true;
    xSemaphoreGive(g_mtx);
  }
}

NetInfo netInfo() {
  static NetInfo cache;                          // last good copy if the mutex is briefly busy
  if (xSemaphoreTake(g_mtx, pdMS_TO_TICKS(20)) == pdTRUE) {
    cache = g_net;
    xSemaphoreGive(g_mtx);
  }
  NetInfo n = cache;
  n.cloud = g_wsUp;
  n.lastTxAgeS = g_haveTx ? (int32_t)((millis() - g_lastTxAt) / 1000) : -1;
  return n;
}

void startPortal() { g_reqPortal = true; }
bool cloudConnected() { return g_wsUp; }

bool sendText(const String& json) {
  if (!g_wsUp) return false;
  String* p = new String(json);
  if (xQueueSend(g_outQ, &p, 0) != pdTRUE) { delete p; return false; }
  return true;
}

bool pollInbound(String& out) {
  String* p = nullptr;
  if (xQueueReceive(g_inQ, &p, 0) != pdTRUE) return false;
  out = *p;
  delete p;
  return true;
}

}  // namespace telemetry
