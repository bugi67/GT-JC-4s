#pragma once
#include <Arduino.h>

struct WifiNetworkConfig {
    char ssid[64]    = "";
    char pass[64]    = "";
    bool useStaticIP = false;
    char ip[16]      = "";
    char netmask[16] = "";
    char gateway[16] = "";
    char dns[16]     = "";
};

class WiFiManager {
public:
    static bool begin();
    static void runCaptivePortal();
    static bool isAPMode();
    static bool isConnected();
    static bool reconnect();
    static String getSSID();
    static int8_t getRSSI();

    static WifiNetworkConfig getNetworkConfig(uint8_t idx);   // idx: 0 = Network 1, 1 = Network 2
    static void setNetworkConfig(uint8_t idx, const WifiNetworkConfig& cfg);

private:
    static bool connectStation(const WifiNetworkConfig& net);
    static void startAP();
    static bool s_apMode;
    static WifiNetworkConfig s_active;
};
