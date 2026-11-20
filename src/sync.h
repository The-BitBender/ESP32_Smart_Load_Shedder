#pragma once

namespace cloudsync {

// Main-loop side of the settings protocol (all settings changes happen in this task):
//   server -> device : settings:init / settings:updated / telemetry:ack(syncRequired) -> apply + {"type":"ack"}
//                      settings:synced -> adopt the new configVersion after a local edit
//   device -> server : settings_update after a local button/menu edit (retried until settings:synced)
void update();   // call every loop()

}  // namespace cloudsync
