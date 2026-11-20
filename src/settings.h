#pragma once
#include <ArduinoJson.h>
#include "types.h"

namespace settings {

struct RelayCfg {
  Priority prio;
  bool     targetState;       // what the user wants (web UI toggle)
  bool     manualOverride;    // forced to targetState, exempt from automatic shedding
  bool     scheduleEnabled;   // only allowed ON inside [schedStartMin, schedEndMin]
  uint16_t schedStartMin;     // minutes since local midnight
  uint16_t schedEndMin;
};

struct Data {
  uint32_t magic;
  uint32_t maxSupplyW;          // maxSupplyLimitWatts
  uint16_t lowVoltageCutoff;    // volts, 0 = disabled
  bool     autoShedding;        // autoSheddingEnabled
  bool     peakEnabled;         // peakHoursEnabled
  uint16_t peakStartMin;
  uint16_t peakEndMin;
  uint32_t configVersion;       // last server version applied or acknowledged
  bool     localDirty;          // a local edit has not been confirmed by the server yet
  RelayCfg relay[cfg::NUM_CHANNELS];
};

void begin();
const Data& get();

// Local (button/menu) edits: persisted, flagged dirty and uploaded as settings_update.
void setMaxSupply(uint32_t watts);
void setPriority(uint8_t ch, Priority p);
uint32_t localRev();            // increments on every local edit

// Cloud -> device
void applyRemote(JsonObjectConst payload, uint32_t version);   // authoritative payload from the server
void markSynced(uint32_t version, uint32_t sentRev);           // settings:synced for our own edit

}  // namespace settings
