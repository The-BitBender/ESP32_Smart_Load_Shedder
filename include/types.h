#pragma once
#include <Arduino.h>
#include "config.h"

// NOTE: Arduino defines LOW/HIGH as macros, hence Low/Medium/High (case sensitive).
enum class Priority : uint8_t { Low = 0, Medium = 1, High = 2 };

inline const char* priorityName(Priority p) {
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

inline const char* supplyStateName(SupplyState s) {   // used in telemetry
  switch (s) {
    case SupplyState::Normal:      return "NORMAL";
    case SupplyState::Overload:    return "OVERLOAD";
    case SupplyState::Undervolt:   return "LOW_VOLTAGE";
    default:                       return "SENSOR_FAULT";
  }
}
inline const char* supplyStateLcd(SupplyState s) {    // used on the 16x2 LCD
  switch (s) {
    case SupplyState::Normal:      return "NORMAL";
    case SupplyState::Overload:    return "OVERLOAD";
    case SupplyState::Undervolt:   return "LOW VOLT";
    default:                       return "NO METER";
  }
}

// Rollover-safe "has time t been reached" check.
inline bool reached(uint32_t now, uint32_t t) { return (int32_t)(now - t) >= 0; }

struct ChannelInfo {
  Priority prio;
  bool     on;
  bool     shed;     // true if switched off by the load-shedding logic
  uint16_t estW;     // learned load size
  bool     known;    // estW is a measured value
};

struct Snapshot {
  Metrics     m;
  uint16_t    maxSupplyDeciKw = cfg::DEFAULT_SUPPLY_DECI_KW;
  float       loadPct = 0;
  SupplyState state = SupplyState::Normal;
  ChannelInfo ch[cfg::NUM_CHANNELS];
  uint32_t    uptimeS = 0;
};
