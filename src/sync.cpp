#include "sync.h"
#include <ArduinoJson.h>
#include "types.h"
#include "settings.h"
#include "telemetry.h"

namespace cloudsync {
namespace {

bool     haveSent = false, wasUp = false;
uint32_t lastSentAt = 0, sentRev = 0;

void sendAck(uint32_t version) {
  JsonDocument doc;
  doc["type"] = "ack";
  doc["configVersion"] = version;
  String out;
  serializeJson(doc, out);
  telemetry::sendText(out);
  Serial.printf("[SYNC] ack v%u\n", (unsigned)version);
}

void sendLocalUpdate(uint32_t now) {
  const settings::Data& s = settings::get();

  JsonDocument doc;
  doc["type"] = "settings_update";
  doc["maxSupplyLimitWatts"] = s.maxSupplyW;
  doc["lowVoltageCutoff"] = s.lowVoltageCutoff;
  doc["autoSheddingEnabled"] = s.autoShedding;
  JsonArray pr = doc["priorities"].to<JsonArray>();
  for (uint8_t i = 0; i < cfg::NUM_CHANNELS; ++i) pr.add(priorityName(s.relay[i].prio));

  String out;
  serializeJson(doc, out);
  if (telemetry::sendText(out)) {
    lastSentAt = now;
    sentRev = settings::localRev();
    haveSent = true;
    Serial.println("[SYNC] settings_update sent");
  }
}

// settings:init, settings:updated and telemetry:ack all carry the authoritative settings payload.
void onSettingsPayload(JsonObjectConst p) {
  const settings::Data& cur = settings::get();
  const uint32_t ver = p["configVersion"] | 0u;
  const bool syncRequired = p["syncRequired"] | false;

  if (cur.localDirty) return;                          // our own edit is still on its way; it wins
  if (!syncRequired && ver == cur.configVersion) return;   // nothing new

  settings::applyRemote(p, ver);
  Serial.printf("[SYNC] applied v%u (syncRequired=%d)\n", (unsigned)ver, (int)syncRequired);
  sendAck(ver);
}

void onMessage(const String& msg) {
  JsonDocument doc;
  if (deserializeJson(doc, msg)) {
    Serial.println("[SYNC] bad JSON from server");
    return;
  }
  const char* type = doc["type"] | "";

  if (!strcmp(type, "settings:init") || !strcmp(type, "settings:updated") || !strcmp(type, "telemetry:ack")) {
    JsonObjectConst p = doc["payload"].as<JsonObjectConst>();
    if (!p.isNull()) onSettingsPayload(p);
  } else if (!strcmp(type, "settings:synced")) {
    // The server stored our local edit. Adopt its version; the payload is just our own values echoed.
    const uint32_t ver = doc["payload"]["configVersion"] | 0u;
    if (haveSent) settings::markSynced(ver, sentRev);
    Serial.printf("[SYNC] synced v%u\n", (unsigned)ver);
  }
  // "ack:ok" needs no action.
}

}  // namespace

void update() {
  const uint32_t now = millis();

  String msg;
  while (telemetry::pollInbound(msg)) onMessage(msg);

  const bool up = telemetry::cloudConnected();
  if (up && settings::get().localDirty) {
    const bool newEdit = !haveSent || settings::localRev() != sentRev;
    const bool justConnected = !wasUp;
    if (newEdit || justConnected || (now - lastSentAt) >= cfg::LOCAL_SYNC_RETRY_MS) sendLocalUpdate(now);
  }
  wasUp = up;
}

}  // namespace cloudsync
