#pragma once
#include <Arduino.h>
#include <PubSubClient.h>

class MQTTClient {
public:
    static bool begin();
    static void taskMQTT(void* param);

    static void publishStatus();   // publish L, C, mode, SWR, freq
    static void publishTuneStatus(const char* status, uint8_t progress = 0);
    static bool isConnected();

    // Shelly power switch. All Shelly HTTP runs in taskMQTT so an unreachable
    // Shelly never blocks the web server. Web handlers only read the cache /
    // set request flags.
    static bool shellyCachedValid();
    static bool shellyCachedOn();
    static bool shellyToggleOptimistic();   // flip cache, schedule real toggle, return predicted state
    static void shellyRequestRefresh();

private:
    static void onMessage(char* topic, byte* payload, unsigned int len);
    static bool ensureConnected();
    static void subscribe();
};
