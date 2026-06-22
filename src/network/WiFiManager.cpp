#include "WiFiManager.h"
#include "../config.h"
#include "../logger/Logger.h"
#include <WiFi.h>
#include <nvs_flash.h>
#include <nvs.h>

static char s_ssid[64] = {};
static char s_pass[64] = {};

static void nvsLoad() {
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) return;
    size_t sz = sizeof(s_ssid);
    nvs_get_str(nvs, NVS_WIFI_SSID, s_ssid, &sz);
    sz = sizeof(s_pass);
    nvs_get_str(nvs, NVS_WIFI_PASS, s_pass, &sz);
    nvs_close(nvs);
}

bool WiFiManager::connectStation(const char* ssid, const char* pass) {
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid, pass);
    LOG_INFO("WiFi", "Connecting to '%s'...", ssid);

    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED) {
        if (millis() - start > 15000) {
            LOG_WARN("WiFi", "Connection timeout");
            return false;
        }
        delay(250);
    }
    LOG_INFO("WiFi", "Connected – IP %s", WiFi.localIP().toString().c_str());
    return true;
}

void WiFiManager::startAP() {
    WiFi.mode(WIFI_AP);
    WiFi.softAP(AP_SSID, strlen(AP_PASS) ? AP_PASS : nullptr);
    LOG_INFO("WiFi", "AP mode: SSID='%s' IP=%s", AP_SSID, WiFi.softAPIP().toString().c_str());
}

bool WiFiManager::begin() {
    nvsLoad();
    if (strlen(s_ssid) == 0) {
        LOG_WARN("WiFi", "No credentials – starting setup AP");
        startAP();
        return false;
    }
    if (!connectStation(s_ssid, s_pass)) {
        startAP();
        return false;
    }
    return true;
}

bool WiFiManager::isConnected() {
    return WiFi.status() == WL_CONNECTED;
}

bool WiFiManager::reconnect() {
    if (WiFi.status() == WL_CONNECTED) return true;
    return connectStation(s_ssid, s_pass);
}

String WiFiManager::getSSID() { return String(s_ssid); }
int8_t WiFiManager::getRSSI() { return WiFi.RSSI(); }

void WiFiManager::saveCredentials(const char* ssid, const char* pass) {
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK) return;
    nvs_set_str(nvs, NVS_WIFI_SSID, ssid);
    nvs_set_str(nvs, NVS_WIFI_PASS, pass);
    nvs_commit(nvs);
    nvs_close(nvs);
    LOG_INFO("WiFi", "Credentials saved, rebooting...");
    delay(500);
    ESP.restart();
}
