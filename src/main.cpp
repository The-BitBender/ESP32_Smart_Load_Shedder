#include <Arduino.h>
#include "config.h"
#include "types.h"
#include "settings.h"
#include "metering.h"
#include "load_manager.h"
#include "ui.h"
#include "telemetry.h"

static Snapshot g_snap;
static Metrics  g_metrics;
static uint32_t g_lastMeter = 0;

// Cheap (no I/O): rebuilt every loop so the LCD reflects menu edits immediately.
static void buildSnapshot() {
  const settings::Data& s = settings::get();
  const float maxW = s.maxSupplyDeciKw * 100.0f;

  g_snap.m = g_metrics;
  g_snap.maxSupplyDeciKw = s.maxSupplyDeciKw;
  g_snap.loadPct = (g_metrics.valid && maxW > 0) ? (g_metrics.power / maxW * 100.0f) : 0.0f;
  g_snap.state = loadmgr::state();
  loadmgr::fillChannels(g_snap.ch, s);
  g_snap.uptimeS = millis() / 1000;
}

void setup() {
  Serial.begin(115200);
  delay(100);
  Serial.printf("\n[BOOT] Smart Load Shedding & Energy Management v%s\n", FW_VERSION);

  loadmgr::begin();      // first: drive all relays to a known OFF state
  settings::begin();
  metering::begin();
  ui::begin();
  telemetry::begin();
  buildSnapshot();
}

void loop() {
  const uint32_t now = millis();

  if (now - g_lastMeter >= cfg::METER_INTERVAL_MS) {
    g_lastMeter = now;
    g_metrics = metering::read();
    loadmgr::update(g_metrics, settings::get());
    buildSnapshot();
    telemetry::publish(g_snap);

    Serial.printf("[MET] %.1fV %.2fA %.1fW %.3fkWh | %s\n", g_metrics.voltage, g_metrics.current,
                  g_metrics.power, g_metrics.energy, supplyStateName(g_snap.state));
  }

  buildSnapshot();
  ui::update(g_snap);
  delay(5);
}
