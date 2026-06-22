#include "AppConfig.h"
#include "../logger/Logger.h"
#include <LittleFS.h>
#include <ArduinoJson.h>

AppConfig g_cfg;

#define CONFIG_PATH "/config.json"

static void applyDefaults(AppConfig& c) {
    c.wifi_ssid[0]    = '\0';
    c.wifi_pass[0]    = '\0';
    c.mqtt_server[0]  = '\0';
    c.mqtt_port       = DEFAULT_MQTT_PORT;
    c.mqtt_enabled    = true;
    c.tune_threshold  = DEFAULT_TUNE_THRESHOLD;
    c.tune_tx_level   = DEFAULT_TX_LEVEL;
    c.coarse_step_l   = DEFAULT_COARSE_L;
    c.coarse_step_c   = DEFAULT_COARSE_C;
    strlcpy(c.ota_manifest_url, OTA_MANIFEST_URL_DEFAULT, sizeof(c.ota_manifest_url));
    c.log_level       = LOG_LEVEL_DEFAULT;
}

bool Config::load() {
    applyDefaults(g_cfg);

    File f = LittleFS.open(CONFIG_PATH, "r");
    if (!f) {
        LOG_INFO("Config", "No config.json – using defaults");
        return false;
    }

    StaticJsonDocument<1024> doc;
    DeserializationError err = deserializeJson(doc, f);
    f.close();

    if (err) {
        LOG_WARN("Config", "config.json parse error: %s – using defaults", err.c_str());
        return false;
    }

    strlcpy(g_cfg.wifi_ssid,        doc["wifi_ssid"]        | "", sizeof(g_cfg.wifi_ssid));
    strlcpy(g_cfg.wifi_pass,        doc["wifi_pass"]        | "", sizeof(g_cfg.wifi_pass));
    strlcpy(g_cfg.mqtt_server,      doc["mqtt_server"]      | "", sizeof(g_cfg.mqtt_server));
    g_cfg.mqtt_port      = doc["mqtt_port"]      | DEFAULT_MQTT_PORT;
    g_cfg.mqtt_enabled   = doc["mqtt_enabled"]   | true;
    g_cfg.tune_threshold = doc["tune_threshold"] | DEFAULT_TUNE_THRESHOLD;
    g_cfg.tune_tx_level  = doc["tune_tx_level"]  | (uint8_t)DEFAULT_TX_LEVEL;
    g_cfg.coarse_step_l  = doc["coarse_step_l"]  | (uint16_t)DEFAULT_COARSE_L;
    g_cfg.coarse_step_c  = doc["coarse_step_c"]  | (uint16_t)DEFAULT_COARSE_C;
    strlcpy(g_cfg.ota_manifest_url, doc["ota_manifest_url"] | OTA_MANIFEST_URL_DEFAULT,
            sizeof(g_cfg.ota_manifest_url));
    g_cfg.log_level      = doc["log_level"]      | (uint8_t)LOG_LEVEL_DEFAULT;

    LOG_INFO("Config", "Loaded from " CONFIG_PATH);
    return true;
}

bool Config::save() {
    StaticJsonDocument<1024> doc;

    doc["wifi_ssid"]        = g_cfg.wifi_ssid;
    doc["wifi_pass"]        = g_cfg.wifi_pass;
    doc["mqtt_server"]      = g_cfg.mqtt_server;
    doc["mqtt_port"]        = g_cfg.mqtt_port;
    doc["mqtt_enabled"]     = g_cfg.mqtt_enabled;
    doc["tune_threshold"]   = g_cfg.tune_threshold;
    doc["tune_tx_level"]    = g_cfg.tune_tx_level;
    doc["coarse_step_l"]    = g_cfg.coarse_step_l;
    doc["coarse_step_c"]    = g_cfg.coarse_step_c;
    doc["ota_manifest_url"] = g_cfg.ota_manifest_url;
    doc["log_level"]        = g_cfg.log_level;

    File f = LittleFS.open(CONFIG_PATH, "w");
    if (!f) {
        LOG_ERROR("Config", "Cannot open " CONFIG_PATH " for writing");
        return false;
    }

    serializeJson(doc, f);
    f.close();
    LOG_INFO("Config", "Saved to " CONFIG_PATH);
    return true;
}
