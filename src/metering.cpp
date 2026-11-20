#include "metering.h"
#include <PZEM004Tv30.h>

namespace metering {
namespace {
PZEM004Tv30 pzem(Serial2, cfg::PZEM_RX_PIN, cfg::PZEM_TX_PIN);
constexpr uint8_t MAX_CONSECUTIVE_FAILS = 3;

inline float nz(float v) { return isnan(v) ? 0.0f : v; }
}

void begin() {
  Serial.println("[PZEM] UART2 ready");
}

Metrics read() {
  static Metrics last;            // last good reading (valid=false until first success)
  static uint8_t fails = 0;

  float v = pzem.voltage();
  if (isnan(v)) {
    if (fails < 255) ++fails;
    if (fails >= MAX_CONSECUTIVE_FAILS) last.valid = false;
    return last;
  }
  fails = 0;

  Metrics m;
  m.voltage   = v;
  m.current   = nz(pzem.current());
  m.power     = nz(pzem.power());
  m.energy    = nz(pzem.energy());       // kWh
  m.frequency = nz(pzem.frequency());
  m.pf        = nz(pzem.pf());
  m.valid     = true;
  last = m;
  return m;
}

bool resetEnergy() { return pzem.resetEnergy(); }

}  // namespace metering
