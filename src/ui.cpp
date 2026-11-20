#include "ui.h"
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include "settings.h"
#include "load_manager.h"
#include "metering.h"
#include "telemetry.h"

// ---------------------------------------------------------------------------
// One-button UI
//   Monitor : tap = next page              hold = open settings menu
//   Menu    : tap = next item              hold = select
//   Edit    : tap = change value           hold = next step / save
// Row 0, column 16 of the first three pages carries a WiFi indicator:
//   (wifi glyph) WiFi + cloud OK   ! WiFi OK but cloud down   ~ connecting   S setup portal   x offline
// ---------------------------------------------------------------------------

namespace ui {
namespace {

using telemetry::NetInfo;
using telemetry::NetState;

enum class Screen : uint8_t { Monitor, Menu, EditSupplyInt, EditSupplyDec, EditPriority, ConfirmReset };
enum class Btn : uint8_t { None, Short, Long };

constexpr uint8_t NCH        = cfg::NUM_CHANNELS;
constexpr uint8_t PAGES      = 7;      // elec, power/energy, supply, ch1-4, ch5-8, wifi, server
constexpr uint8_t PAGE_WIFI  = 5;
constexpr uint8_t MENU_ITEMS = NCH + 4;  // supply, CH1..CH8, WiFi setup, reset energy, exit

LiquidCrystal_I2C* lcd = nullptr;
char rowCache[2][17] = {"", ""};

Screen   screen = Screen::Monitor;
uint8_t  page = 0, menuIdx = 0, editCh = 0, edInt = 0, edDec = 0;
Priority edPrio = Priority::Medium;
uint32_t lastInput = 0, lastPageAt = 0, lastDraw = 0, lastNetPoll = 0, bannerUntil = 0, seenEvent = 0;
bool     dirty = true;
NetInfo  netNow;
NetState netPrev = NetState::Connecting;

// ------------------------------ LCD helpers ------------------------------
bool probe(uint8_t addr) {
  Wire.beginTransmission(addr);
  return Wire.endTransmission() == 0;
}

void row(uint8_t r, const char* text) {
  char buf[17];
  snprintf(buf, sizeof buf, "%-16.16s", text);   // pad / truncate to 16 columns
  if (strcmp(buf, rowCache[r]) == 0) return;     // only touch the LCD when something changed
  strcpy(rowCache[r], buf);
  lcd->setCursor(0, r);
  lcd->print(buf);
}

char netIcon(const NetInfo& n) {
  switch (n.state) {
    case NetState::Connected:  return n.cloud ? '\x01' : '!';   // wifi glyph = WiFi + cloud OK
    case NetState::Connecting: return '~';
    case NetState::Portal:     return 'S';
    default:                   return 'x';
  }
}

// ------------------------------- Button ----------------------------------
Btn pollButton(uint32_t now) {
  static bool lastRaw = false, stable = false, longFired = false;
  static uint32_t lastChange = 0, pressStart = 0;

  const bool raw = (digitalRead(cfg::BUTTON_PIN) == LOW);
  if (raw != lastRaw) { lastRaw = raw; lastChange = now; }

  if ((now - lastChange) >= cfg::DEBOUNCE_MS && raw != stable) {
    stable = raw;
    if (stable) { pressStart = now; longFired = false; }
    else if (!longFired) return Btn::Short;       // released before the long-press threshold
  }
  if (stable && !longFired && (now - pressStart) >= cfg::LONG_PRESS_MS) {
    longFired = true;                             // fire as soon as the threshold is crossed
    return Btn::Long;
  }
  return Btn::None;
}

// ------------------------------ Navigation -------------------------------
void handleButton(Btn b, uint32_t now) {
  const settings::Data& s = settings::get();

  switch (screen) {
    case Screen::Monitor:
      if (b == Btn::Short) { page = (page + 1) % PAGES; lastPageAt = now; bannerUntil = 0; }
      else                 { screen = Screen::Menu; menuIdx = 0; }
      break;

    case Screen::Menu:
      if (b == Btn::Short) { menuIdx = (menuIdx + 1) % MENU_ITEMS; break; }
      if (menuIdx == 0) {
        edInt = s.maxSupplyW / 1000;
        edDec = (s.maxSupplyW % 1000) / 100;
        screen = Screen::EditSupplyInt;
      } else if (menuIdx <= NCH) {
        editCh = menuIdx - 1;
        edPrio = s.relay[editCh].prio;
        screen = Screen::EditPriority;
      } else if (menuIdx == NCH + 1) {            // WiFi setup portal
        telemetry::startPortal();
        screen = Screen::Monitor;
        page = PAGE_WIFI;
        lastPageAt = now;
        bannerUntil = 0;
      } else if (menuIdx == NCH + 2) {
        screen = Screen::ConfirmReset;
      } else {
        screen = Screen::Monitor;
        lastPageAt = now;
      }
      break;

    case Screen::EditSupplyInt:
      if (b == Btn::Short) edInt = (edInt + 1) % (cfg::MAX_SUPPLY_W / 1000 + 1);
      else                 screen = Screen::EditSupplyDec;
      break;

    case Screen::EditSupplyDec:
      if (b == Btn::Short) edDec = (edDec + 1) % 10;
      else {
        settings::setMaxSupply(edInt * 1000u + edDec * 100u);   // clamped inside settings
        screen = Screen::Menu;
      }
      break;

    case Screen::EditPriority:
      if (b == Btn::Short) edPrio = (Priority)((((uint8_t)edPrio) + 1) % 3);   // LOW > MEDIUM > HIGH > LOW
      else { settings::setPriority(editCh, edPrio); screen = Screen::Menu; }
      break;

    case Screen::ConfirmReset:
      if (b == Btn::Long) metering::resetEnergy();
      screen = Screen::Menu;
      break;
  }
}

// -------------------------------- Drawing --------------------------------
void cell(char* out, size_t n, uint8_t i, const Snapshot& s) {
  snprintf(out, n, "%u:%s %-3s", i + 1, priorityShort(s.ch[i].prio), s.ch[i].on ? "ON" : "OFF");   // 8 chars
}

void wifiPage(const NetInfo& n, char* a, char* b, size_t sz) {
  switch (n.state) {
    case NetState::Connected:
      snprintf(a, sz, "WiFi:%s", n.ssid);
      snprintf(b, sz, "%s", n.ip);
      break;
    case NetState::Connecting:
      snprintf(a, sz, "WiFi:Connecting");
      snprintf(b, sz, "%s", n.ssid[0] ? n.ssid : "...");
      break;
    case NetState::Portal:
      snprintf(a, sz, "Join:%s", PORTAL_AP_NAME);
      snprintf(b, sz, "Open %s", n.ip[0] ? n.ip : "192.168.4.1");
      break;
    default:
      snprintf(a, sz, "WiFi:Offline");
      snprintf(b, sz, "%s", n.ssid[0] ? "Retrying..." : "Not configured");
      break;
  }
}

void serverPage(const NetInfo& n, char* a, char* b, size_t sz) {
  if (n.state == NetState::Connected) snprintf(a, sz, "RSSI:%ddBm", (int)n.rssi);
  else                                snprintf(a, sz, "RSSI:--");

  if (!n.cloud) {
    snprintf(b, sz, "Cloud:Offline");
  } else if (n.lastTxAgeS < 0) {
    snprintf(b, sz, "Cloud:OK");
  } else {
    const int age = n.lastTxAgeS;
    snprintf(b, sz, "Cloud:OK Tx:%d%c", age < 100 ? age : age / 60, age < 100 ? 's' : 'm');
  }
}

void drawMonitor(const Snapshot& s, uint32_t now) {
  char a[40], b[40];
  const Metrics& m = s.m;

  const loadmgr::Event& ev = loadmgr::lastEvent();
  if (now < bannerUntil) { row(0, ev.line1); row(1, ev.line2); return; }

  if (!m.valid && page < 3) {
    row(0, "PZEM NO DATA"); row(1, "Check wiring");
    return;
  }

  switch (page) {
    case 0:
      snprintf(a, sizeof a, "V:%.1f I:%.2f", m.voltage, m.current);
      snprintf(b, sizeof b, "F:%.1fHz PF:%.2f", m.frequency, m.pf);
      break;
    case 1: {
      char e[16];
      if      (m.energy < 1000)  snprintf(e, sizeof e, "%.2f", m.energy);
      else if (m.energy < 10000) snprintf(e, sizeof e, "%.1f", m.energy);
      else                       snprintf(e, sizeof e, "%.0f", m.energy);
      snprintf(a, sizeof a, "Load:%.2fkW", m.power / 1000.0f);
      snprintf(b, sizeof b, "Energy:%skWh", e);
      break;
    }
    case 2:
      snprintf(a, sizeof a, "Max:%.1fkW %3.0f%%", s.maxSupplyW / 1000.0f, s.loadPct);
      snprintf(b, sizeof b, "Status:%s", supplyStateLcd(s.state));
      break;
    case 3:
    case 4: {
      const uint8_t base = (page - 3) * 4;
      char c0[16], c1[16], c2[16], c3[16];
      cell(c0, sizeof c0, base + 0, s);  cell(c1, sizeof c1, base + 1, s);
      cell(c2, sizeof c2, base + 2, s);  cell(c3, sizeof c3, base + 3, s);
      snprintf(a, sizeof a, "%s%s", c0, c1);
      snprintf(b, sizeof b, "%s%s", c2, c3);
      break;
    }
    case PAGE_WIFI:
      wifiPage(netNow, a, b, sizeof a);
      break;
    default:
      serverPage(netNow, a, b, sizeof a);
      break;
  }

  if (page < 3) {                                   // WiFi indicator in the last column
    char t[48];
    snprintf(t, sizeof t, "%-15.15s%c", a, netIcon(netNow));
    row(0, t);
  } else {
    row(0, a);
  }
  row(1, b);
}

void drawMenu() {
  const settings::Data& s = settings::get();
  char a[32], b[32];

  if (menuIdx == 0) {
    snprintf(a, sizeof a, "> Max Supply");
    snprintf(b, sizeof b, "  %u.%u kW", (unsigned)(s.maxSupplyW / 1000), (unsigned)((s.maxSupplyW % 1000) / 100));
  } else if (menuIdx <= NCH) {
    snprintf(a, sizeof a, "> CH%u Priority", menuIdx);
    snprintf(b, sizeof b, "  %s", priorityName(s.relay[menuIdx - 1].prio));
  } else if (menuIdx == NCH + 1) {
    snprintf(a, sizeof a, "> WiFi Setup");
    snprintf(b, sizeof b, "  (hold to start)");
  } else if (menuIdx == NCH + 2) {
    snprintf(a, sizeof a, "> Reset Energy");
    snprintf(b, sizeof b, "  (hold to open)");
  } else {
    snprintf(a, sizeof a, "> Exit");
    snprintf(b, sizeof b, "  (hold to exit)");
  }
  row(0, a);
  row(1, b);
}

void drawEdit() {
  char a[32], b[32];
  switch (screen) {
    case Screen::EditSupplyInt:
      snprintf(a, sizeof a, "Set Max Supply");
      snprintf(b, sizeof b, "[%u].%u kW", edInt, edDec);
      break;
    case Screen::EditSupplyDec:
      snprintf(a, sizeof a, "Set Max Supply");
      snprintf(b, sizeof b, "%u.[%u] kW", edInt, edDec);
      break;
    case Screen::EditPriority:
      snprintf(a, sizeof a, "CH%u Priority", editCh + 1);
      snprintf(b, sizeof b, "[%s]", priorityName(edPrio));
      break;
    default:
      snprintf(a, sizeof a, "Reset energy?");
      snprintf(b, sizeof b, "Hold=Yes Tap=No");
      break;
  }
  row(0, a);
  row(1, b);
}

}  // namespace

void begin() {
  pinMode(cfg::BUTTON_PIN, INPUT_PULLUP);
  Wire.begin(cfg::I2C_SDA, cfg::I2C_SCL);

  uint8_t addr = cfg::LCD_ADDR_PRIMARY;
  if (!probe(addr) && probe(cfg::LCD_ADDR_FALLBACK)) addr = cfg::LCD_ADDR_FALLBACK;

  lcd = new LiquidCrystal_I2C(addr, 16, 2);
  lcd->init();
  lcd->backlight();

  uint8_t wifiGlyph[8] = {0b00000, 0b01110, 0b10001, 0b00100, 0b01010, 0b00000, 0b00100, 0b00000};
  lcd->createChar(1, wifiGlyph);                  // char code 1 = wifi icon
  lcd->clear();

  row(0, "Smart Load Shed");
  row(1, "Energy Manager");
  delay(1500);
  lastInput = lastPageAt = millis();
}

void update(const Snapshot& snap) {
  const uint32_t now = millis();

  const Btn b = pollButton(now);
  if (b != Btn::None) { lastInput = now; dirty = true; handleButton(b, now); }

  if (screen != Screen::Monitor && reached(now, lastInput + cfg::MENU_TIMEOUT_MS)) {
    screen = Screen::Monitor;                     // abandon unsaved edits
    dirty = true;
  }

  // Network status (polled ~5x/s). When the setup portal opens by itself (first boot,
  // nothing saved) jump straight to the WiFi page so the hotspot name is visible.
  if (reached(now, lastNetPoll + 200)) {
    lastNetPoll = now;
    netNow = telemetry::netInfo();
    if (netNow.state == NetState::Portal && netPrev != NetState::Portal && screen == Screen::Monitor) {
      page = PAGE_WIFI;
      lastPageAt = now;
      bannerUntil = 0;
      dirty = true;
    }
    netPrev = netNow.state;
  }

  const loadmgr::Event& ev = loadmgr::lastEvent();
  if (ev.id != seenEvent) {
    seenEvent = ev.id;
    if (screen == Screen::Monitor) bannerUntil = now + cfg::EVENT_BANNER_MS;
  }

  if (screen == Screen::Monitor && cfg::AUTO_PAGE_MS > 0 && netNow.state != NetState::Portal &&
      reached(now, lastPageAt + cfg::AUTO_PAGE_MS)) {
    page = (page + 1) % PAGES;
    lastPageAt = now;
  }

  if (!dirty && !reached(now, lastDraw + cfg::LCD_REFRESH_MS)) return;
  dirty = false;
  lastDraw = now;

  switch (screen) {
    case Screen::Monitor: drawMonitor(snap, now); break;
    case Screen::Menu:    drawMenu();             break;
    default:              drawEdit();             break;
  }
}

}  // namespace ui
