#pragma once
#include <Arduino.h>
#include "../config.h"

struct AppConfig {
    char     wifi_ssid[64];
    char     wifi_pass[64];
    char     mqtt_server[64];
    uint16_t mqtt_port;
    bool     mqtt_enabled;
    float    tune_threshold;
    uint8_t  tune_tx_level;
    uint16_t coarse_step_l;
    uint16_t coarse_step_c;
    char     ota_manifest_url[256];
    uint8_t  log_level;
};

extern AppConfig g_cfg;

namespace Config {
    bool load();   // reads /config.json from LittleFS; applies defaults if missing
    bool save();   // writes /config.json to LittleFS
}
