#pragma once
#include <Arduino.h>

class WiFiManager {
public:
    // Connect using NVS credentials; falls back to AP mode if not configured
    static bool begin();

    // True if connected to station
    static bool isConnected();

    // Blocking reconnect attempt (called by MQTT task on disconnect)
    static bool reconnect();

    static String getSSID();
    static int8_t getRSSI();

    // Save credentials to NVS and reboot into station mode
    static void saveCredentials(const char* ssid, const char* pass);

private:
    static bool connectStation(const char* ssid, const char* pass);
    static void startAP();
};
