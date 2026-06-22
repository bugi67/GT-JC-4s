#pragma once
#include <Arduino.h>
#include <PubSubClient.h>

class MQTTClient {
public:
    static bool begin();
    static void taskMQTT(void* param);

    static void publishStatus();   // publish L, C, mode, SWR, freq
    static void publishTuneStatus(const char* status, uint8_t progress = 0);

private:
    static void onMessage(char* topic, byte* payload, unsigned int len);
    static bool ensureConnected();
    static void subscribe();
};
