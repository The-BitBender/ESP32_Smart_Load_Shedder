#pragma once
#include <Arduino.h>

// ============================ Identity ============================
#define FW_VERSION         "1.0.0"
#define DEVICE_ID          "LSEMS-001"

// ====================== WiFi / Telemetry ==========================
// WiFi credentials are NOT hardcoded: they are entered through the WiFiManager setup portal.
#define PORTAL_AP_NAME     "LSEMS-Setup"     // hotspot name while the setup portal is open
#define PORTAL_AP_PASS     "setup1234"       // min 8 chars; "" = open hotspot
#define TELEMETRY_URL      "http://192.168.1.100:3000/api/telemetry"  // http:// or https://
#define TELEMETRY_API_KEY  ""                                         // sent as X-API-Key if not empty

namespace cfg {

// ============================ Hardware ============================
constexpr uint8_t NUM_CHANNELS = 8;

// Avoid strapping pins (0,2,5,12,15), flash pins (6-11) and input-only pins (34-39).
constexpr uint8_t RELAY_PINS[NUM_CHANNELS] = {23, 25, 26, 27, 32, 33, 18, 19};
constexpr bool    RELAY_ACTIVE_LOW = true;   // most 8-ch opto relay boards are active LOW

// PZEM-004T v3 on UART2. NOTE: GPIO16/17 are used by PSRAM on WROVER modules -> pick other pins there.
constexpr uint8_t PZEM_RX_PIN = 16;          // ESP32 RX <- PZEM TX (use a level shifter / divider, see notes)
constexpr uint8_t PZEM_TX_PIN = 17;          // ESP32 TX -> PZEM RX

constexpr uint8_t BUTTON_PIN = 4;            // tact switch between GPIO4 and GND (internal pull-up)
constexpr uint8_t I2C_SDA = 21;
constexpr uint8_t I2C_SCL = 22;
constexpr uint8_t LCD_ADDR_PRIMARY  = 0x27;
constexpr uint8_t LCD_ADDR_FALLBACK = 0x3F;

// ======================= Supply configuration =====================
constexpr uint16_t DEFAULT_SUPPLY_DECI_KW = 30;   // 3.0 kW (stored in 0.1 kW units)
constexpr uint16_t MIN_SUPPLY_DECI_KW     = 1;    // 0.1 kW
constexpr uint16_t MAX_SUPPLY_DECI_KW     = 220;  // 22.0 kW (PZEM-004T 100A range limit)

// ===================== Load shedding tuning =======================
constexpr uint32_t METER_INTERVAL_MS     = 1000;  // metering + control loop period
constexpr uint32_t OVERLOAD_CONFIRM_MS   = 2000;  // overload must persist this long (ignores inrush)
constexpr float    SEVERE_OVERLOAD_FACTOR = 1.15f;// above this x max supply: shed immediately
constexpr uint32_t SETTLE_MS             = 4000;  // wait after a relay change before judging/learning
constexpr uint32_t RESTORE_DELAY_MS      = 60000; // stable time after last shed before restoring
constexpr uint32_t BOOT_DELAY_MS         = 5000;  // delay before first loads are energised at boot
constexpr float    RESTORE_MARGIN        = 0.90f; // only restore if (load + est) <= 90% of max supply
constexpr uint16_t UNKNOWN_LOAD_W        = 300;   // assumed size of a channel never measured yet
constexpr uint16_t MIN_LEARN_W           = 20;    // ignore smaller power steps when learning load size

// Low-supply (brown-out) protection, tuned for 230 V mains. Use ~95/100 for 110 V systems.
constexpr float    MAINS_PRESENT_V       = 50.0f;
constexpr float    UNDERVOLT_TRIP_V      = 190.0f;
constexpr float    UNDERVOLT_CLEAR_V     = 200.0f;
constexpr uint32_t UNDERVOLT_CONFIRM_MS  = 3000;

// ============================== UI ================================
constexpr uint32_t DEBOUNCE_MS       = 30;
constexpr uint32_t LONG_PRESS_MS     = 800;
constexpr uint32_t MENU_TIMEOUT_MS   = 30000;
constexpr uint32_t AUTO_PAGE_MS      = 0;     // auto-rotate monitor pages, 0 = disabled
constexpr uint32_t LCD_REFRESH_MS    = 250;
constexpr uint32_t EVENT_BANNER_MS   = 3000;

// =========================== Telemetry ============================
constexpr uint32_t TELEMETRY_INTERVAL_MS = 30000;
constexpr uint32_t HTTP_TIMEOUT_MS       = 5000;
constexpr uint32_t WIFI_RETRY_MS         = 15000;   // retry interval while offline
constexpr uint32_t WIFI_CONNECT_TIMEOUT_MS = 20000; // give up on one connect attempt after this
constexpr uint32_t PORTAL_TIMEOUT_S      = 180;     // setup portal closes itself after this

}  // namespace cfg