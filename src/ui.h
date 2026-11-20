#pragma once
#include "types.h"

namespace ui {

void begin();
void update(const Snapshot& snap);   // call every loop(): polls the button and refreshes the LCD

}  // namespace ui
