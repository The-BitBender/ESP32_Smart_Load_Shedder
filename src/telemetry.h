#pragma once
#include "types.h"

// Network + cloud transport. Everything here runs in one background FreeRTOS task (core 0):
// WiFiManager portal, reconnects, NTP, the WebSocket client and the 30 s telemetry frames.
// The main loop only exchanges plain strings with it through thread-safe queues.

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
  bool     cloud = false;       // WebSocket to the web app is up
  int32_t  lastTxAgeS = -1;     // seconds since the last telemetry frame (-1 = none yet)
};

void begin();
void publish(const Snapshot& s);       // latest snapshot for the 30 s telemetry frame
NetInfo netInfo();                     // thread-safe copy of the network status
void startPortal();                    // open the WiFiManager setup portal
bool cloudConnected();

bool sendText(const String& json);     // queue a text frame for the socket; false if it is down
bool pollInbound(String& out);         // next text frame received from the server, if any

}  // namespace telemetry
