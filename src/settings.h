#pragma once
#include "types.h"

namespace settings {

struct Data {
  uint16_t maxSupplyDeciKw;                 // 0.1 kW units
  Priority prio[cfg::NUM_CHANNELS];
};

void begin();
const Data& get();
void setMaxSupply(uint16_t deciKw);
void setPriority(uint8_t ch, Priority p);

}  // namespace settings
