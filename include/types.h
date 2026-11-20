#pragma once
#include <Arduino.h>
#include <time.h>
#include "config.h"

// NOTE: Arduino defines LOW/HIGH as macros, hence Low/Medium/High (case sensitive).
enum class Priority : uint8_t { Low = 0, Medium = 1, High = 2 };

inline const char* priorityName(Priority p) {       // also the wire format ("LOW"/"MEDIUM"/"HIGH")
  switch (p) {
    case Priority::Low:    return "LOW";
    case Priority::Medium: return "MEDIUM";
    default:               return "HIGH";
  }
}
inline const char* priorityShort(Priority p) {
  switch (p) {
    case Priority::Low:    return "LO";
    case Priority::Medium: return "ME";
    default:               return "HI";
  }
}

struct Metrics {
  float voltage = 0, current = 0, power = 0, energy = 0, frequency = 0, pf = 0;
  bool valid = false;
};

enum class SupplyState : uint8_t { Normal, Overload, Undervolt, SensorFault };

inline const char* supplyStateLcd(SupplyState s) {
  switch (s) {
    case SupplyState::Normal:      return "NORMAL";
    case SupplyState::Overload:    return "OVERLOAD";
    case SupplyState::Undervolt:   return "LOW VOLT";
    default:                       return "NO METER";
  }
}

// Rollover-safe "has time t been reached" check.
inline bool reached(uint32_t now, uint32_t t) { return (int32_t)(now - t) >= 0; }

// Local minutes since midnight, or -1 until NTP has set the clock.
inline int minutesOfDayLocal() {
  const time_t t = time(nullptr);
  if (t < 1700000000) return -1;
  struct tm tmv;
  localtime_r(&t, &tmv);
  return tmv.tm_hour * 60 + tmv.tm_min;
}

// Inclusive window [start, end] in minutes; wraps past midnight; start == end means "all day".
inline bool inWindow(int now, uint16_t start, uint16_t end) {
  if (start == end) return true;
  if (start < end)  return now >= start && now <= end;
  return now >= start || now <= end;
}

struct ChannelInfo {
  Priority prio;
  bool     on;
  bool     shed;     // true if switched off by the load-shedding logic
  uint16_t estW;     // learned load size
  bool     known;    // estW is a measured value
};

struct Snapshot {
  Metrics     m;
  uint32_t    maxSupplyW = cfg::DEFAULT_SUPPLY_W;
  float       loadPct = 0;
  SupplyState state = SupplyState::Normal;
  bool        sheddingActive = false;     // at least one channel is currently shed
  uint32_t    configVersion = 0;          // last settings version applied (sent as ackedConfigVersion)
  ChannelInfo ch[cfg::NUM_CHANNELS];
  uint32_t    uptimeS = 0;
};
