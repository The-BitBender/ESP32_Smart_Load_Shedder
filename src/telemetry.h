#pragma once
#include "types.h"

namespace telemetry {

enum class NetState : uint8_t {
  Connecting,   // trying the saved network
  Connected,    // STA is up
  Portal,       // WiFiManager setup hotspot is open
  Offline       // not connected, will retry (or open the portal if nothing is saved)
};

struct NetInfo {
  NetState state = NetState::Connecting;
  char     ssid[33] = {0};      // connected SSID, or the saved one while connecting/offline
  char     ip[16] = {0};        // station IP, or the hotspot IP while the portal is open
  int16_t  rssi = 0;
  int8_t   lastPostOk = -1;     // -1 = nothing sent yet, 0 = failed, 1 = OK
  int32_t  lastPostAgeS = -1;   // seconds since the last upload attempt
};

void begin();                          // starts the background network + telemetry task (core 0)
void publish(const Snapshot& s);       // hand the latest snapshot to the telemetry task
NetInfo netInfo();                     // thread-safe copy of the current network status
void startPortal();                    // ask the task to open the WiFiManager setup portal

}  // namespace telemetry