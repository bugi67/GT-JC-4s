#include "MQTTClient.h"
#include "../config.h"
#include "../logger/Logger.h"
#include "../state.h"
#include "../tuner/I2CController.h"
#include <WiFi.h>
#include <nvs_flash.h>
#include <nvs.h>

static WiFiClient      s_wifiClient;
static PubSubClient    s_mqtt(s_wifiClient);
static char            s_server[64] = {};
static uint16_t        s_port       = DEFAULT_MQTT_PORT;
static bool            s_enabled    = true;

static void nvsLoad() {
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) return;
    size_t sz = sizeof(s_server);
    nvs_get_str(nvs, NVS_MQTT_SERVER, s_server, &sz);
    nvs_get_u16(nvs, NVS_MQTT_PORT, &s_port);
    uint8_t en = 1;
    nvs_get_u8(nvs, NVS_MQTT_ENABLED, &en);
    s_enabled = (bool)en;
    nvs_close(nvs);
}

void MQTTClient::onMessage(char* topic, byte* payload, unsigned int len) {
    char val[32] = {};
    if (len >= sizeof(val)) len = sizeof(val) - 1;
    memcpy(val, payload, len);

    LOG_DEBUG("MQTT", "RX [%s] = '%s'", topic, val);

    I2CCommand cmd;
    bool sendCmd = false;

    if (strcmp(topic, MQTT_SUB_L) == 0) {
        uint16_t L = (uint16_t)constrain(atoi(val), 0, L_MAX);
        StateLock lock;
        cmd = {I2CCmd::SET_LC, L, g_state.C, g_state.mode};
        sendCmd = true;
    } else if (strcmp(topic, MQTT_SUB_C) == 0) {
        uint16_t C = (uint16_t)constrain(atoi(val), 0, C_MAX);
        StateLock lock;
        cmd = {I2CCmd::SET_LC, g_state.L, C, g_state.mode};
        sendCmd = true;
    } else if (strcmp(topic, MQTT_SUB_MODE) == 0) {
        uint8_t mode = (uint8_t)constrain(atoi(val), 1, 3);
        StateLock lock;
        cmd = {I2CCmd::SET_LC, g_state.L, g_state.C, mode};
        sendCmd = true;
    } else if (strcmp(topic, MQTT_SUB_FREQ) == 0) {
        StateLock lock;
        g_state.freq_kHz = (uint16_t)atoi(val);
    } else if (strcmp(topic, MQTT_SUB_TUNE) == 0) {
        if (strcmp(val, "1") == 0) {
            xSemaphoreGive(g_tuneStartSem);
        } else if (strcmp(val, "0") == 0) {
            xSemaphoreGive(g_tuneAbortSem);
        }
    }

    if (sendCmd) xQueueSend(g_i2cCmdQueue, &cmd, 0);
}

void MQTTClient::subscribe() {
    s_mqtt.subscribe(MQTT_SUB_L);
    s_mqtt.subscribe(MQTT_SUB_C);
    s_mqtt.subscribe(MQTT_SUB_MODE);
    s_mqtt.subscribe(MQTT_SUB_FREQ);
    s_mqtt.subscribe(MQTT_SUB_TUNE);
}

bool MQTTClient::ensureConnected() {
    if (s_mqtt.connected()) return true;
    LOG_WARN("MQTT", "Disconnected – reconnecting to %s:%u", s_server, s_port);

    char clientId[24];
    snprintf(clientId, sizeof(clientId), "GT-JC-4s-%06llX", (uint64_t)ESP.getEfuseMac() & 0xFFFFFF);
    if (s_mqtt.connect(clientId)) {
        LOG_INFO("MQTT", "Connected as %s", clientId);
        subscribe();
        s_mqtt.publish(MQTT_PUB_ID, FIRMWARE_VERSION);
        return true;
    }
    LOG_WARN("MQTT", "Connect failed state=%d", s_mqtt.state());
    return false;
}

bool MQTTClient::begin() {
    nvsLoad();
    if (!s_enabled || strlen(s_server) == 0) {
        LOG_INFO("MQTT", "MQTT disabled or no server configured");
        return false;
    }
    s_mqtt.setServer(s_server, s_port);
    s_mqtt.setCallback(onMessage);
    return true;
}

void MQTTClient::publishStatus() {
    if (!s_mqtt.connected()) return;

    char buf[24];
    uint16_t L, C; uint8_t mode; float swr;
    {
        StateLock lock;
        L = g_state.L; C = g_state.C; mode = g_state.mode; swr = g_state.swr;
    }

    snprintf(buf, sizeof(buf), "%u", L);
    s_mqtt.publish(MQTT_PUB_FB_L, buf);
    snprintf(buf, sizeof(buf), "%u", C);
    s_mqtt.publish(MQTT_PUB_FB_C, buf);
    snprintf(buf, sizeof(buf), "%u", mode);
    s_mqtt.publish(MQTT_PUB_FB_MODE, buf);
    snprintf(buf, sizeof(buf), "%.3f", calcLuH(L));
    s_mqtt.publish(MQTT_PUB_L_UH, buf);
    snprintf(buf, sizeof(buf), "%.1f", calcCpF(C));
    s_mqtt.publish(MQTT_PUB_C_PF, buf);
    snprintf(buf, sizeof(buf), "%.2f", swr);
    s_mqtt.publish(MQTT_PUB_SWR, buf);
}

void MQTTClient::publishTuneStatus(const char* status, uint8_t progress) {
    if (!s_mqtt.connected()) return;
    s_mqtt.publish(MQTT_PUB_TUNE_STATUS, status);
    if (progress > 0) {
        char buf[8];
        snprintf(buf, sizeof(buf), "%u", progress);
        s_mqtt.publish(MQTT_PUB_TUNE_PROGRESS, buf);
    }
}

void MQTTClient::taskMQTT(void* param) {
    (void)param;
    static unsigned long lastRssi = 0;
    static TunerState::TuneState lastTuneState = TunerState::TuneState::IDLE;

    for (;;) {
        if (s_enabled && strlen(s_server) > 0) {
            if (WiFi.status() == WL_CONNECTED) {
                ensureConnected();
                if (s_mqtt.connected()) {
                    s_mqtt.loop();

                    // Publish RSSI every 10 s
                    if (millis() - lastRssi > RSSI_INTERVAL_MS) {
                        lastRssi = millis();
                        char buf[8];
                        snprintf(buf, sizeof(buf), "%d", WiFi.RSSI());
                        s_mqtt.publish(MQTT_PUB_RSSI, buf);
                        {
                            StateLock lock;
                            g_state.rssi = (int8_t)WiFi.RSSI();
                        }
                    }

                    // Publish tune state changes
                    auto ts = stateGet(&TunerState::tuneState);
                    if (ts != lastTuneState) {
                        lastTuneState = ts;
                        const char* stStr[] = {"idle","tuning","done","aborted"};
                        publishTuneStatus(stStr[(int)ts],
                            ts == TunerState::TuneState::TUNING ? stateGet(&TunerState::tuneProgress) : 0);
                        if (ts == TunerState::TuneState::DONE) publishStatus();
                    }
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}
