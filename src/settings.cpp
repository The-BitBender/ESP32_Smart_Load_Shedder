#include "settings.h"
#include <Preferences.h>

namespace settings {
namespace {
Preferences prefs;
Data g_data;
const Priority kDefaultPrio[cfg::NUM_CHANNELS] = {
  Priority::High, Priority::High, Priority::Medium, Priority::Medium,
  Priority::Low,  Priority::Low,  Priority::Low,    Priority::Low
};
}

void begin() {
  prefs.begin("lsems", false);

  uint16_t v = prefs.getUShort("maxsup", cfg::DEFAULT_SUPPLY_DECI_KW);
  if (v < cfg::MIN_SUPPLY_DECI_KW) v = cfg::MIN_SUPPLY_DECI_KW;
  if (v > cfg::MAX_SUPPLY_DECI_KW) v = cfg::MAX_SUPPLY_DECI_KW;
  g_data.maxSupplyDeciKw = v;

  for (uint8_t i = 0; i < cfg::NUM_CHANNELS; ++i) {
    char key[4];
    snprintf(key, sizeof key, "p%u", i);
    uint8_t p = prefs.getUChar(key, (uint8_t)kDefaultPrio[i]);
    g_data.prio[i] = (p <= (uint8_t)Priority::High) ? (Priority)p : kDefaultPrio[i];
  }
}

const Data& get() { return g_data; }

void setMaxSupply(uint16_t deciKw) {
  if (deciKw < cfg::MIN_SUPPLY_DECI_KW) deciKw = cfg::MIN_SUPPLY_DECI_KW;
  if (deciKw > cfg::MAX_SUPPLY_DECI_KW) deciKw = cfg::MAX_SUPPLY_DECI_KW;
  g_data.maxSupplyDeciKw = deciKw;
  prefs.putUShort("maxsup", deciKw);
}

void setPriority(uint8_t ch, Priority p) {
  if (ch >= cfg::NUM_CHANNELS) return;
  g_data.prio[ch] = p;
  char key[4];
  snprintf(key, sizeof key, "p%u", ch);
  prefs.putUChar(key, (uint8_t)p);
}

}  // namespace settings
