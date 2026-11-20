#include "telemetry.h"
#include <WiFi.h>
#include <WiFiManager.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <memory>
#include <time.h>

// All WiFi handling (WiFiManager portal, reconnects, NTP) and the HTTP uploads live in this one
// FreeRTOS task, so network problems can never stall metering or load shedding in loop().

namespace telemetry {
namespace {

SemaphoreHandle_t g_mtx = nullptr;
Snapshot          g_snap;
bool              g_have = false;
NetInfo           g_net;
volatile int8_t   g_lastOk = -1;
volatile uint32_t g_lastPostAt = 0;
volatile bool     g_reqPortal = false;

inline float rnd(float v, int d) {
  float k = powf(10.0f, d);
  return roundf(v * k) / k;
}

String buildPayload(const Snapshot& s) {
  JsonDocument doc;
  const time_t t = time(nullptr);

  doc["device_id"] = DEVICE_ID;
  doc["fw"]        = FW_VERSION;
  doc["ts"]        = (t > 1700000000) ? (uint32_t)t : 0;   // UTC epoch, 0 until NTP syncs
  doc["uptime_s"]  = s.uptimeS;
  doc["rssi"]      = WiFi.RSSI();

  JsonObject sup = doc["supply"].to<JsonObject>();
  sup["max_kw"]   = rnd(s.maxSupplyDeciKw / 10.0f, 1);
  sup["load_kw"]  = rnd(s.m.power / 1000.0f, 3);
  sup["load_pct"] = rnd(s.loadPct, 1);
  sup["state"]    = supplyStateName(s.state);

  JsonObject m = doc["meter"].to<JsonObject>();
  m["valid"]      = s.m.valid;
  m["voltage"]    = rnd(s.m.voltage, 1);
  m["current"]    = rnd(s.m.current, 3);
  m["power_w"]    = rnd(s.m.power, 1);
  m["energy_kwh"] = rnd(s.m.energy, 3);
  m["frequency"]  = rnd(s.m.frequency, 1);
  m["pf"]         = rnd(s.m.pf, 2);

  JsonArray arr = doc["channels"].to<JsonArray>();
  for (uint8_t i = 0; i < cfg::NUM_CHANNELS; ++i) {
    JsonObject c = arr.add<JsonObject>();
    c["ch"]       = i + 1;
    c["priority"] = priorityName(s.ch[i].prio);
    c["on"]       = s.ch[i].on;
    c["shed"]     = s.ch[i].shed;
    c["est_w"]    = s.ch[i].known ? (int)s.ch[i].estW : -1;
  }

  String out;
  serializeJson(doc, out);
  return out;
}

bool postJson(const String& body) {
  const bool https = strncmp(TELEMETRY_URL, "https://", 8) == 0;
  std::unique_ptr<WiFiClient> client;
  if (https) {
    WiFiClientSecure* sc = new WiFiClientSecure();
    sc->setInsecure();   // TODO production: sc->setCACert(rootCa) to validate the server
    client.reset(sc);
  } else {
    client.reset(new WiFiClient());
  }

  HTTPClient http;
  if (!http.begin(*client, TELEMETRY_URL)) return false;
  http.setConnectTimeout(cfg::HTTP_TIMEOUT_MS);
  http.setTimeout(cfg::HTTP_TIMEOUT_MS);
  http.addHeader("Content-Type", "application/json");
  if (strlen(TELEMETRY_API_KEY) > 0) http.addHeader("X-API-Key", TELEMETRY_API_KEY);

  const int code = http.POST(body);
  http.end();
  Serial.printf("[TEL] POST -> %d\n", code);
  return code >= 200 && code < 300;
}

void sendNow() {
  Snapshot s;
  if (xSemaphoreTake(g_mtx, pdMS_TO_TICKS(100)) != pdTRUE) return;
  s = g_snap;
  xSemaphoreGive(g_mtx);

  const bool ok = postJson(buildPayload(s));
  g_lastPostAt = millis();
  g_lastOk = ok ? 1 : 0;
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
  bool ntpStarted = false;

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
        configTime(0, 0, "pool.ntp.org", "time.google.com");
        ntpStarted = true;
        Serial.printf("[NET] connected to %s, IP %s\n", WiFi.SSID().c_str(), WiFi.localIP().toString().c_str());
      }
      if (g_have && reached(now, nextSend)) {
        nextSend = now + cfg::TELEMETRY_INTERVAL_MS;   // fixed 30 s cadence, even after a failure
        sendNow();
      }
    }

    if (st != lastSt || now - lastNet >= 500) {
      lastSt = st;
      lastNet = now;
      updateNet(st, wm);
    }

    vTaskDelay(pdMS_TO_TICKS(st == NetState::Portal ? 10 : 200));
  }
}

}  // namespace

void begin() {
  g_mtx = xSemaphoreCreateMutex();
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
  n.lastPostOk = g_lastOk;
  n.lastPostAgeS = (g_lastOk < 0) ? -1 : (int32_t)((millis() - g_lastPostAt) / 1000);
  return n;
}

void startPortal() { g_reqPortal = true; }

}  // namespace telemetry