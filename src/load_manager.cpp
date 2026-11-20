#include "load_manager.h"

// Strategy
//  * HIGH priority loads are critical: never shed, always energised.
//  * On confirmed overload (P > max supply) or low voltage, shed ONE non-critical load,
//    lowest priority first (ties: highest channel number first), then wait SETTLE_MS and re-evaluate.
//  * Restore strictly by priority (HIGH -> LOW) once the supply has been stable for RESTORE_DELAY_MS
//    and the learned size of the load fits under RESTORE_MARGIN x max supply.
//  * The PZEM only measures the total, so each channel's size is learned from the power step
//    observed when it is switched ON or OFF.

namespace loadmgr {
namespace {

constexpr int N = cfg::NUM_CHANNELS;

struct Ch {
  bool on = false;
  bool shed = false;
  uint16_t estW = 0;
  bool known = false;
};

struct Pending {            // a relay change whose power step is being measured
  bool active = false;
  uint8_t ch = 0;
  bool turnOn = false;
  float prePower = 0;
  uint32_t at = 0;
};

Ch          g_ch[N];
Pending     g_pend;
SupplyState g_state = SupplyState::Normal;
bool        g_uv = false;
bool        g_overTimer = false, g_uvTimer = false;
uint32_t    g_overSince = 0, g_uvSince = 0, g_restoreAllowedAt = 0;
Event       g_event;

inline void relayWrite(uint8_t i, bool on) {
  const bool high = cfg::RELAY_ACTIVE_LOW ? !on : on;
  digitalWrite(cfg::RELAY_PINS[i], high ? HIGH : LOW);
}

void postEvent(const char* title, uint8_t ch, Priority p, bool on) {
  g_event.id++;
  snprintf(g_event.line1, sizeof g_event.line1, "%s", title);
  snprintf(g_event.line2, sizeof g_event.line2, "CH%u (%s) %s", ch + 1, priorityName(p), on ? "ON" : "OFF");
  Serial.printf("[LOAD] %s | %s\n", g_event.line1, g_event.line2);
}

void switchChannel(int i, bool on, float prePower, Priority p) {
  relayWrite(i, on);
  g_ch[i].on = on;
  g_ch[i].shed = !on;
  g_pend.active = true;
  g_pend.ch = i;
  g_pend.turnOn = on;
  g_pend.prePower = prePower;
  g_pend.at = millis();
  g_overTimer = false;
  postEvent(on ? "LOAD RESTORED" : "LOAD SHED", i, p, on);
}

void finishLearning(float powerNow) {
  float delta = g_pend.turnOn ? (powerNow - g_pend.prePower) : (g_pend.prePower - powerNow);
  g_pend.active = false;
  if (delta < cfg::MIN_LEARN_W) return;
  uint16_t d = delta > 65000.0f ? 65000 : (uint16_t)delta;
  Ch& c = g_ch[g_pend.ch];
  c.estW = c.known ? (uint16_t)((c.estW + d) / 2) : d;   // smooth repeated measurements
  c.known = true;
  Serial.printf("[LOAD] CH%u size ~%uW\n", g_pend.ch + 1, c.estW);
}

// Lowest priority first; ties -> highest channel number. HIGH is never returned.
int pickShed(const settings::Data& s) {
  int best = -1;
  for (int i = N - 1; i >= 0; --i) {
    if (!g_ch[i].on || s.prio[i] == Priority::High) continue;
    if (best < 0 || s.prio[i] < s.prio[best]) best = i;
  }
  return best;
}

// Highest priority first; ties -> lowest channel number.
int pickRestore(const settings::Data& s, bool highOnly) {
  int best = -1;
  for (int i = 0; i < N; ++i) {
    if (g_ch[i].on) continue;
    if (highOnly && s.prio[i] != Priority::High) continue;
    if (best < 0 || s.prio[i] > s.prio[best]) best = i;
  }
  return best;
}

}  // namespace

void begin() {
  for (uint8_t i = 0; i < N; ++i) {
    relayWrite(i, false);                        // set level first...
    pinMode(cfg::RELAY_PINS[i], OUTPUT);         // ...then enable output (avoids a relay glitch)
    g_ch[i] = Ch();
  }
  g_restoreAllowedAt = millis() + cfg::BOOT_DELAY_MS;
}

void update(const Metrics& m, const settings::Data& s) {
  const uint32_t now = millis();

  // ---- Sensor fault: hold state, never shed blind, but keep critical loads powered ----
  if (!m.valid) {
    g_state = SupplyState::SensorFault;
    g_pend.active = false;
    g_overTimer = g_uvTimer = false;
    if (reached(now, g_restoreAllowedAt)) {
      int h = pickRestore(s, true);
      if (h >= 0) switchChannel(h, true, 0, s.prio[h]);
    }
    return;
  }

  const float maxW = s.maxSupplyDeciKw * 100.0f;
  const bool severe = m.power > maxW * cfg::SEVERE_OVERLOAD_FACTOR;

  // ---- Wait for a relay change to settle, then learn that channel's size ----
  if (g_pend.active) {
    if (severe)                                   g_pend.active = false;   // too dangerous to wait
    else if (!reached(now, g_pend.at + cfg::SETTLE_MS)) return;
    else                                          finishLearning(m.power);
  }

  // ---- Low voltage (supply sag) with hysteresis ----
  const bool uvLow = (m.voltage > cfg::MAINS_PRESENT_V && m.voltage < cfg::UNDERVOLT_TRIP_V);
  if (uvLow) {
    if (!g_uvTimer) { g_uvTimer = true; g_uvSince = now; }
    if (reached(now, g_uvSince + cfg::UNDERVOLT_CONFIRM_MS)) g_uv = true;
  } else {
    g_uvTimer = false;
    if (m.voltage >= cfg::UNDERVOLT_CLEAR_V || m.voltage <= cfg::MAINS_PRESENT_V) g_uv = false;
  }

  // ---- Overload with confirmation timer ----
  const bool over = m.power > maxW;
  bool overConfirmed = false;
  if (over) {
    if (!g_overTimer) { g_overTimer = true; g_overSince = now; }
    overConfirmed = severe || reached(now, g_overSince + cfg::OVERLOAD_CONFIRM_MS);
  } else {
    g_overTimer = false;
  }

  // ---- Shed ----
  if (overConfirmed || g_uv) {
    g_state = overConfirmed ? SupplyState::Overload : SupplyState::Undervolt;
    g_restoreAllowedAt = now + cfg::RESTORE_DELAY_MS;
    int idx = pickShed(s);
    if (idx >= 0) { switchChannel(idx, false, m.power, s.prio[idx]); return; }
    // Nothing left to shed: still make sure critical loads are on.
    int h = pickRestore(s, true);
    if (h >= 0) switchChannel(h, true, m.power, s.prio[h]);
    return;
  }

  g_state = SupplyState::Normal;
  if (over) return;                                   // short spike: no restore yet
  if (!reached(now, g_restoreAllowedAt)) return;

  // ---- Restore (strict priority order) ----
  int idx = pickRestore(s, false);
  if (idx < 0) return;
  if (s.prio[idx] != Priority::High) {
    const float need = g_ch[idx].known ? g_ch[idx].estW : cfg::UNKNOWN_LOAD_W;
    if (m.power + need > maxW * cfg::RESTORE_MARGIN) return;
  }
  switchChannel(idx, true, m.power, s.prio[idx]);
}

SupplyState state() { return g_state; }

void fillChannels(ChannelInfo* out, const settings::Data& s) {
  for (int i = 0; i < N; ++i) {
    out[i].prio  = s.prio[i];
    out[i].on    = g_ch[i].on;
    out[i].shed  = g_ch[i].shed;
    out[i].estW  = g_ch[i].estW;
    out[i].known = g_ch[i].known;
  }
}

const Event& lastEvent() { return g_event; }

}  // namespace loadmgr
