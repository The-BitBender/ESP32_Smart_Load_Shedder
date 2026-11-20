#include "settings.h"
#include <Preferences.h>
#include <strings.h>

namespace settings {
namespace {

constexpr uint32_t MAGIC = 0x4C534533;       // bump when Data's layout changes
constexpr uint8_t  N = cfg::NUM_CHANNELS;

Preferences prefs;
Data        g_data;
uint32_t    g_localRev = 0;

void save() { prefs.putBytes("cfg", &g_data, sizeof g_data); }

uint32_t clampW(uint32_t w) {
  if (w < cfg::MIN_SUPPLY_W) return cfg::MIN_SUPPLY_W;
  if (w > cfg::MAX_SUPPLY_W) return cfg::MAX_SUPPLY_W;
  return w;
}

void setDefaults() {
  memset(&g_data, 0, sizeof g_data);
  g_data.magic            = MAGIC;
  g_data.maxSupplyW       = cfg::DEFAULT_SUPPLY_W;
  g_data.lowVoltageCutoff = cfg::DEFAULT_LOW_VOLTAGE_V;
  g_data.autoShedding     = true;
  g_data.peakEnabled      = false;
  g_data.peakStartMin     = 17 * 60;
  g_data.peakEndMin       = 22 * 60;
  const Priority def[N] = {Priority::High, Priority::High, Priority::Medium, Priority::Medium,
                           Priority::Low,  Priority::Low,  Priority::Low,    Priority::Low};
  for (uint8_t i = 0; i < N; ++i) {
    g_data.relay[i].prio            = def[i];
    g_data.relay[i].targetState     = true;
    g_data.relay[i].manualOverride  = false;
    g_data.relay[i].scheduleEnabled = false;
    g_data.relay[i].schedStartMin   = 0;
    g_data.relay[i].schedEndMin     = 23 * 60 + 59;
  }
}

void sanitize() {
  g_data.maxSupplyW = clampW(g_data.maxSupplyW);
  if (g_data.lowVoltageCutoff > 260) g_data.lowVoltageCutoff = cfg::DEFAULT_LOW_VOLTAGE_V;
  if (g_data.peakStartMin > 1439) g_data.peakStartMin = 17 * 60;
  if (g_data.peakEndMin > 1439)   g_data.peakEndMin = 22 * 60;
  for (uint8_t i = 0; i < N; ++i) {
    if ((uint8_t)g_data.relay[i].prio > (uint8_t)Priority::High) g_data.relay[i].prio = Priority::Medium;
    if (g_data.relay[i].schedStartMin > 1439) g_data.relay[i].schedStartMin = 0;
    if (g_data.relay[i].schedEndMin > 1439)   g_data.relay[i].schedEndMin = 1439;
  }
}

void markLocal() {
  g_data.localDirty = true;
  ++g_localRev;
  save();
}

bool num(JsonVariantConst v, float& out) {
  if (v.isNull() || !v.is<float>()) return false;       // is<float>() is true for integers too
  out = v.as<float>();
  return true;
}
bool flag(JsonVariantConst v, bool& out) {
  if (!v.is<bool>()) return false;
  out = v.as<bool>();
  return true;
}
bool parseHM(const char* s, uint16_t& out) {
  int h, m;
  if (!s || sscanf(s, "%d:%d", &h, &m) != 2 || h < 0 || h > 23 || m < 0 || m > 59) return false;
  out = (uint16_t)(h * 60 + m);
  return true;
}
bool parsePrio(const char* s, Priority& out) {
  if (!s) return false;
  if (!strcasecmp(s, "HIGH"))   { out = Priority::High;   return true; }
  if (!strcasecmp(s, "MEDIUM")) { out = Priority::Medium; return true; }
  if (!strcasecmp(s, "LOW"))    { out = Priority::Low;    return true; }
  return false;
}

}  // namespace

void begin() {
  prefs.begin("lsems", false);
  setDefaults();
  if (prefs.getBytesLength("cfg") == sizeof(Data)) {
    Data tmp;
    prefs.getBytes("cfg", &tmp, sizeof tmp);
    if (tmp.magic == MAGIC) g_data = tmp;               // otherwise: first boot or layout change -> defaults
  }
  sanitize();
}

const Data& get() { return g_data; }
uint32_t localRev() { return g_localRev; }

void setMaxSupply(uint32_t watts) {
  watts = clampW(watts);
  if (watts == g_data.maxSupplyW) return;
  g_data.maxSupplyW = watts;
  markLocal();
}

void setPriority(uint8_t ch, Priority p) {
  if (ch >= N || g_data.relay[ch].prio == p) return;
  g_data.relay[ch].prio = p;
  markLocal();
}

void applyRemote(JsonObjectConst p, uint32_t version) {
  float f;
  bool b;

  if (num(p["maxSupplyLimitWatts"], f)) g_data.maxSupplyW = clampW((uint32_t)(f + 0.5f));
  if (num(p["lowVoltageCutoff"], f))    g_data.lowVoltageCutoff = (f < 0) ? 0 : (f > 260 ? 260 : (uint16_t)(f + 0.5f));
  if (flag(p["autoSheddingEnabled"], b)) g_data.autoShedding = b;
  if (flag(p["peakHoursEnabled"], b))    g_data.peakEnabled = b;
  parseHM(p["peakStartTime"] | (const char*)nullptr, g_data.peakStartMin);
  parseHM(p["peakEndTime"]   | (const char*)nullptr, g_data.peakEndMin);

  // Flat priorities array first, then the per-relay objects (which win if both are present).
  JsonArrayConst pr = p["priorities"].as<JsonArrayConst>();
  if (!pr.isNull()) {
    uint8_t i = 0;
    for (JsonVariantConst e : pr) {
      if (i >= N) break;
      Priority x;
      if (e.is<const char*>() && parsePrio(e.as<const char*>(), x)) g_data.relay[i].prio = x;
      ++i;
    }
  }

  // NOTE: the top-level relayStates array is deliberately ignored; per-relay targetState is the intent.
  JsonArrayConst rl = p["relays"].as<JsonArrayConst>();
  if (!rl.isNull()) {
    for (JsonVariantConst e : rl) {
      JsonObjectConst r = e.as<JsonObjectConst>();
      const int ch = r["channel"] | 0;
      if (ch < 1 || ch > N) continue;
      RelayCfg& c = g_data.relay[ch - 1];
      Priority x;
      if (parsePrio(r["priority"] | (const char*)nullptr, x)) c.prio = x;
      if (flag(r["targetState"], b))     c.targetState = b;
      if (flag(r["manualOverride"], b))  c.manualOverride = b;
      if (flag(r["scheduleEnabled"], b)) c.scheduleEnabled = b;
      parseHM(r["scheduleStart"] | (const char*)nullptr, c.schedStartMin);
      parseHM(r["scheduleEnd"]   | (const char*)nullptr, c.schedEndMin);
    }
  }

  sanitize();
  g_data.configVersion = version;
  g_data.localDirty = false;
  save();
}

void markSynced(uint32_t version, uint32_t sentRev) {
  g_data.configVersion = version;
  if (sentRev == g_localRev) g_data.localDirty = false;   // no newer local edit since we sent it
  save();
}

}  // namespace settings
