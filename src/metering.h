#pragma once
#include "types.h"

namespace metering {

void begin();
Metrics read();          // call about once per second; tolerates a few failed reads
bool resetEnergy();      // clears the PZEM's internal kWh counter

}  // namespace metering
