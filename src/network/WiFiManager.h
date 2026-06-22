#pragma once
#include <Arduino.h>

class WiFiManager {
public:
    // Connect using credentials from g_cfg; returns false and starts AP if unavailable
    static bool begin();

    // Blocking captive portal — call when begin() returns false; reboots after save
    static void runCaptivePortal();

    static bool isAPMode();
    static bool isConnected();

    // Blocking reconnect attempt (called by MQTT task on disconnect)
    static bool reconnect();

    static String getSSID();
    static int8_t getRSSI();

    // Save credentials to config.json and reboot into station mode
    static void saveCredentials(const char* ssid, const char* pass);

private:
    static bool connectStation(const char* ssid, const char* pass);
    static void startAP();
    static bool s_apMode;
};
