#include "MQTTClient.h"
#include "../config.h"
#include "../cfg/AppConfig.h"
#include "../logger/Logger.h"
#include "../state.h"
#include "../tuner/I2CController.h"
#include "../tuner/PresetStore.h"
#include <WiFi.h>

static WiFiClient   s_wifiClient;
static PubSubClient s_mqtt(s_wifiClient);

// ── MQTT log queue ────────────────────────────────────────────────────────────
struct LogLine { char text[200]; };
static QueueHandle_t s_logQueue = nullptr;

static void logHook(const char* line) {
    if (!s_logQueue) return;
    LogLine entry;
    strncpy(entry.text, line, sizeof(entry.text) - 1);
    entry.text[sizeof(entry.text) - 1] = '\0';
    int len = (int)strlen(entry.text);
    while (len > 0 && (entry.text[len-1] == '\r' || entry.text[len-1] == '\n'))
        entry.text[--len] = '\0';
    xQueueSend(s_logQueue, &entry, 0);  // drop if full
}

void MQTTClient::onMessage(char* topic, byte* payload, unsigned int len) {
    char val[32] = {};
    if (len >= sizeof(val)) len = sizeof(val) - 1;
    memcpy(val, payload, len);

    LOG_DEBUG("MQTT", "RX [%s] = '%s'", topic, val);

    // Discard relay commands that arrive before the hardware init is done OR
    // within 2 s of an MQTT (re)connect. Retained messages are delivered by
    // the broker within milliseconds of subscribe; 2 s is enough to let them
    // pass silently while still accepting live commands from the user.
    bool initPending   = !g_relayInitDone;
    bool retainWindow  = (millis() - g_mqttConnectedAt) < 2000UL;
    if (initPending || retainWindow) {
        LOG_INFO("MQTT", "Discarding [%s]='%s' (%s)", topic, val,
                 initPending ? "init pending" : "retained-msg window");
        return;
    }

    I2CCommand cmd;
    bool sendCmd = false;

    if (strcmp(topic, MQTT_SUB_L) == 0) {
        uint16_t L = (uint16_t)constrain(atoi(val), 0, L_MAX);
        LOG_INFO("MQTT", "SET L=%u (via MQTT)", L);
        StateLock lock;
        cmd = {I2CCmd::SET_LC, L, g_state.C, g_state.mode};
        sendCmd = true;
    } else if (strcmp(topic, MQTT_SUB_C) == 0) {
        uint16_t C = (uint16_t)constrain(atoi(val), 0, C_MAX);
        LOG_INFO("MQTT", "SET C=%u (via MQTT)", C);
        StateLock lock;
        cmd = {I2CCmd::SET_LC, g_state.L, C, g_state.mode};
        sendCmd = true;
    } else if (strcmp(topic, MQTT_SUB_MODE) == 0) {
        uint8_t mode = (uint8_t)constrain(atoi(val), 1, 3);
        LOG_INFO("MQTT", "SET mode=%u (via MQTT)", mode);
        StateLock lock;
        cmd = {I2CCmd::SET_LC, g_state.L, g_state.C, mode};
        sendCmd = true;
    } else if (strcmp(topic, MQTT_SUB_FREQ) == 0) {
        uint16_t newFreq = (uint16_t)atoi(val);
        { StateLock lock; g_state.freq_kHz = newFreq; }
        // Auto-apply preset when entering a new 20 kHz segment (RAM cache, no Wire)
        static uint16_t lastSeg = 0xFFFF;
        uint16_t seg = (newFreq / 20) * 20;
        if (newFreq > 0 && seg != lastSeg) {
            lastSeg = seg;
            // Search by segment base frequency and require exact match.
            // findBest(newFreq) could return a preset from an adjacent segment
            // if that segment's preset is numerically closer (e.g. 7040 at delta 8
            // beats 7020 at delta 12 for newFreq=7032).
            Preset p;
            if (PresetStore::findBest(seg, p) && p.freq_kHz == seg) {
                LOG_INFO("MQTT", "New seg %u kHz: applying preset L=%u C=%u mode=%u", seg, p.L, p.C, p.mode);
                cmd = {I2CCmd::SET_LC, p.L, p.C, p.mode};
                sendCmd = true;
            }
        }
    } else if (strcmp(topic, MQTT_SUB_TUNE) == 0) {
        if (strcmp(val, "1") == 0) {
            xSemaphoreGive(g_tuneStartSem);
        } else if (strcmp(val, "0") == 0) {
            xSemaphoreGive(g_tuneAbortSem);
        }
    } else if (strcmp(topic, MQTT_SUB_FINETUNE) == 0) {
        if (strcmp(val, "1") == 0) {
            xSemaphoreGive(g_fineTuneStartSem);
        } else if (strcmp(val, "0") == 0) {
            xSemaphoreGive(g_tuneAbortSem);
        }
    } else if (strcmp(topic, MQTT_SUB_KTUNE) == 0) {
        I2CCommand kCmd = {};
        kCmd.cmd   = I2CCmd::SET_KTUNE;
        kCmd.kTune = (strcmp(val, "1") == 0);
        xQueueSend(g_i2cCmdQueue, &kCmd, 0);
    }

    if (sendCmd) xQueueSend(g_i2cCmdQueue, &cmd, 0);
}

void MQTTClient::subscribe() {
    s_mqtt.subscribe(MQTT_SUB_L);
    s_mqtt.subscribe(MQTT_SUB_C);
    s_mqtt.subscribe(MQTT_SUB_MODE);
    s_mqtt.subscribe(MQTT_SUB_FREQ);
    s_mqtt.subscribe(MQTT_SUB_TUNE);
    s_mqtt.subscribe(MQTT_SUB_FINETUNE);
    s_mqtt.subscribe(MQTT_SUB_KTUNE);
}

bool MQTTClient::ensureConnected() {
    if (s_mqtt.connected()) return true;
    LOG_WARN("MQTT", "Disconnected – reconnecting to %s:%u", g_cfg.mqtt_server, g_cfg.mqtt_port);

    char clientId[24];
    snprintf(clientId, sizeof(clientId), "GT-JC-4s-%06llX", (uint64_t)ESP.getEfuseMac() & 0xFFFFFF);
    if (s_mqtt.connect(clientId)) {
        g_mqttConnectedAt = millis();   // retained-message suppression window starts now
        LOG_INFO("MQTT", "Connected as %s", clientId);
        subscribe();
        s_mqtt.publish(MQTT_PUB_ID, FIRMWARE_VERSION);
        return true;
    }
    LOG_WARN("MQTT", "Connect failed state=%d", s_mqtt.state());
    return false;
}

bool MQTTClient::begin() {
    if (!g_cfg.mqtt_enabled || strlen(g_cfg.mqtt_server) == 0) {
        LOG_INFO("MQTT", "MQTT disabled or no server configured");
        return false;
    }
    s_mqtt.setServer(g_cfg.mqtt_server, g_cfg.mqtt_port);
    s_mqtt.setCallback(onMessage);
    s_logQueue = xQueueCreate(16, sizeof(LogLine));
    Logger::setPublishHook(logHook);
    return true;
}

void MQTTClient::publishStatus() {
    if (!s_mqtt.connected()) return;

    char buf[24];
    uint16_t L, C; uint8_t mode; float swr; bool kTune;
    {
        StateLock lock;
        L = g_state.L; C = g_state.C; mode = g_state.mode;
        swr = g_state.swr; kTune = g_state.kTune;
    }

    snprintf(buf, sizeof(buf), "%u", L);
    s_mqtt.publish(MQTT_PUB_FB_L, buf);
    snprintf(buf, sizeof(buf), "%u", C);
    s_mqtt.publish(MQTT_PUB_FB_C, buf);
    snprintf(buf, sizeof(buf), "%u", mode);
    s_mqtt.publish(MQTT_PUB_FB_MODE, buf);
    snprintf(buf, sizeof(buf), "%.3f", calcLuH(L));
    s_mqtt.publish(MQTT_PUB_L_UH, buf);
    s_mqtt.loop();   // let TCP flush before derived-value burst
    snprintf(buf, sizeof(buf), "%.1f", calcCpF(C));
    s_mqtt.publish(MQTT_PUB_C_PF, buf);
    snprintf(buf, sizeof(buf), "%.2f", swr);
    s_mqtt.publish(MQTT_PUB_SWR, buf);
    s_mqtt.publish(MQTT_PUB_FB_KTUNE, kTune ? "1" : "0");
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
    static uint16_t lastL     = 0xFFFF;
    static uint16_t lastC     = 0xFFFF;
    static uint8_t  lastMode  = 0;
    static bool     lastKTune = false;
    static uint16_t lastFreq  = 0xFFFF;

    for (;;) {
        if (g_cfg.mqtt_enabled && strlen(g_cfg.mqtt_server) > 0) {
            if (WiFi.status() == WL_CONNECTED) {
                ensureConnected();
                if (s_mqtt.connected()) {
                    s_mqtt.loop();

                    // Drain and publish pending log lines (max 8 per cycle to
                    // avoid flooding the TCP send buffer before status publishes)
                    {
                        LogLine logEntry;
                        int limit = 8;
                        while (limit-- > 0 && s_logQueue && xQueueReceive(s_logQueue, &logEntry, 0) == pdTRUE)
                            s_mqtt.publish(MQTT_PUB_LOG, logEntry.text);
                    }

                    // Let PubSubClient flush outgoing TCP data and process any
                    // incoming packets before the status publish burst below.
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

                    // Publish feedback whenever L/C/mode/kTune/freq changes
                    uint16_t curL, curC, curFreq; uint8_t curMode; bool curKTune;
                    {
                        StateLock lock;
                        curL = g_state.L; curC = g_state.C;
                        curMode = g_state.mode; curKTune = g_state.kTune;
                        curFreq = g_state.freq_kHz;
                    }
                    if (curL != lastL || curC != lastC || curMode != lastMode || curKTune != lastKTune || curFreq != lastFreq) {
                        lastL = curL; lastC = curC; lastMode = curMode; lastKTune = curKTune; lastFreq = curFreq;
                        publishStatus();
                    }
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}
