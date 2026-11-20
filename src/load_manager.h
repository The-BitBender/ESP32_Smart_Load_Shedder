#pragma once
#include "types.h"
#include "settings.h"

namespace loadmgr {

struct Event {                 // last relay action, for the LCD banner
  uint32_t id = 0;
  char line1[17] = {0};
  char line2[17] = {0};
};

void begin();                                              // forces all relays OFF
void update(const Metrics& m, const settings::Data& s);    // call once per metering tick
SupplyState state();
void fillChannels(ChannelInfo* out, const settings::Data& s);
const Event& lastEvent();

}  // namespace loadmgr
