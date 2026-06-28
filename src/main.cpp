#include <Arduino.h>
#include <nvs_flash.h>
#include <LittleFS.h>
#include <time.h>
#include "config.h"
#include "state.h"
#include "cfg/AppConfig.h"
#include "logger/Logger.h"
#include "tuner/I2CController.h"
#include "tuner/AutoTuner.h"
#include "tuner/PresetStore.h"
#include "network/WiFiManager.h"
#include "network/MQTTClient.h"
#include "network/WebServer.h"

// ── Global state ──────────────────────────────────────────────────────────────
TunerState        g_state;
SemaphoreHandle_t g_stateMutex    = nullptr;
SemaphoreHandle_t g_tuneStartSem     = nullptr;
SemaphoreHandle_t g_fineTuneStartSem = nullptr;
SemaphoreHandle_t g_tuneAbortSem     = nullptr;
QueueHandle_t     g_i2cCmdQueue   = nullptr;

// ── Serial console task ───────────────────────────────────────────────────────
static void taskSerial(void*) {
    static char lineBuf[32];
    static int  linePos = 0;

    for (;;) {
        while (Serial.available()) {
            char c = (char)Serial.read();
            if (c == '\r' || c == '\n') {
                lineBuf[linePos] = 0;
                linePos = 0;

                // Process command (case-insensitive)
                String cmd = String(lineBuf);
                cmd.toLowerCase();
                cmd.trim();

                if      (cmd == "log error") { Logger::setLevel(LogLevel::ERROR); Serial.println("Level: ERROR"); }
                else if (cmd == "log warn")  { Logger::setLevel(LogLevel::WARN);  Serial.println("Level: WARN");  }
                else if (cmd == "log info")  { Logger::setLevel(LogLevel::INFO);  Serial.println("Level: INFO");  }
                else if (cmd == "log debug") { Logger::setLevel(LogLevel::DEBUG); Serial.println("Level: DEBUG"); }
                else if (cmd == "log?")      { Serial.printf("Level: %d\r\n", (int)Logger::getLevel()); }
                else if (cmd == "reboot")    { ESP.restart(); }
                else if (cmd == "status") {
                    uint16_t L, C, freq; uint8_t mode; bool kTune;
                    float swr, rl; uint8_t vfwd, vrev; int tuneState;
                    { StateLock lock;
                      L=g_state.L; C=g_state.C; mode=g_state.mode; freq=g_state.freq_kHz;
                      swr=g_state.swr; rl=g_state.returnLoss;
                      vfwd=g_state.vfwd; vrev=g_state.vrev;
                      kTune=g_state.kTune; tuneState=(int)g_state.tuneState; }
                    Serial.printf("L=%u C=%u mode=%u freq=%u SWR=%.2f RL=%.1fdB vfwd=%u vrev=%u kTune=%d tune=%d\r\n",
                        L, C, mode, freq, swr, rl, vfwd, vrev, (int)kTune, tuneState);
                }
            } else if (linePos < (int)sizeof(lineBuf) - 1) {
                lineBuf[linePos++] = c;
            }
        }

        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

void setup() {
    // 1. Serial
    Serial.begin(115200);
    delay(100);

    // 2. Logger (before everything else, so init steps are loggable)
    Logger::init();
    Logger::setLevel(LogLevel::INFO);

    // 3. NVS (required by ESP32 WiFi stack internally)
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    // 4. LittleFS + config — must happen before WiFi/MQTT/OTA begin()
    if (!LittleFS.begin(true)) {
        LOG_ERROR("System", "LittleFS mount failed");
    }
    Config::load();
    Logger::setLevel((LogLevel)g_cfg.log_level);

    LOG_INFO("System", "GT-JC-4s firmware v%s starting", FIRMWARE_VERSION);

    // 5. FreeRTOS primitives
    g_stateMutex        = xSemaphoreCreateMutex();
    g_tuneStartSem      = xSemaphoreCreateBinary();
    g_fineTuneStartSem  = xSemaphoreCreateBinary();
    g_tuneAbortSem      = xSemaphoreCreateBinary();
    g_i2cCmdQueue  = xQueueCreate(I2C_CMD_QUEUE_DEPTH, sizeof(I2CCommand));

    // 6. I2C + hardware probe
    if (!I2CController::init()) {
        LOG_WARN("System", "Some I2C devices not found – check wiring");
    }

    // 7. Presets
    PresetStore::begin();

    // 8. WiFi — if no credentials or connection fails, run captive portal (never returns)
    if (!WiFiManager::begin()) {
        WiFiManager::runCaptivePortal();
    }

    // 8b. NTP — start SNTP sync (non-blocking, runs in ESP-IDF background)
    configTzTime(NTP_TIMEZONE, g_cfg.ntp_server);
    LOG_INFO("NTP", "SNTP started, server: %s", g_cfg.ntp_server);

    // 9. MQTT
    MQTTClient::begin();

    // 10. Web server
    WebUI::begin();

    // 11. Start FreeRTOS tasks
    xTaskCreate(WebUI::taskWebServer,       "taskWeb",    TASK_WEB_STACK,   nullptr, TASK_WEB_PRIO,    nullptr);
    xTaskCreate(MQTTClient::taskMQTT,       "taskMQTT",   TASK_MQTT_STACK,  nullptr, TASK_MQTT_PRIO,   nullptr);
    xTaskCreate(AutoTuner::taskAutoTuner,   "taskTuner",  TASK_TUNER_STACK, nullptr, TASK_TUNER_PRIO,  nullptr);
    xTaskCreate(I2CController::taskI2C,     "taskI2C",    TASK_I2C_STACK,   nullptr, TASK_I2C_PRIO,    nullptr);
    xTaskCreate(taskSerial,                 "taskSerial", TASK_SERIAL_STACK,nullptr, TASK_SERIAL_PRIO, nullptr);

    LOG_INFO("System", "All tasks started");
}

void loop() {
    // All work is done in FreeRTOS tasks; loop() can yield
    vTaskDelay(pdMS_TO_TICKS(1000));
}
