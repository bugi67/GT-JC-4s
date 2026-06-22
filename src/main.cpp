#include <Arduino.h>
#include <nvs_flash.h>
#include <SPIFFS.h>
#include "config.h"
#include "state.h"
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
SemaphoreHandle_t g_tuneStartSem  = nullptr;
SemaphoreHandle_t g_tuneAbortSem  = nullptr;
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
                    StateLock lock;
                    Serial.printf("L=%u C=%u mode=%u freq=%u SWR=%.2f RL=%.1fdB tune=%d\r\n",
                        g_state.L, g_state.C, g_state.mode, g_state.freq_kHz,
                        g_state.swr, g_state.returnLoss, (int)g_state.tuneState);
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

    // 3. NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    // Restore log level from NVS
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) == ESP_OK) {
        uint8_t ll = 2;
        nvs_get_u8(nvs, NVS_LOG_LEVEL, &ll);
        nvs_close(nvs);
        Logger::setLevel((LogLevel)ll);
    }

    LOG_INFO("System", "GT-JC-4s firmware v%s starting", FIRMWARE_VERSION);

    // 4. FreeRTOS primitives
    g_stateMutex   = xSemaphoreCreateMutex();
    g_tuneStartSem = xSemaphoreCreateBinary();
    g_tuneAbortSem = xSemaphoreCreateBinary();
    g_i2cCmdQueue  = xQueueCreate(I2C_CMD_QUEUE_DEPTH, sizeof(I2CCommand));

    // 5. I2C + hardware probe
    if (!I2CController::init()) {
        LOG_WARN("System", "Some I2C devices not found – check wiring");
    }

    // 6. SPIFFS
    if (!SPIFFS.begin(true)) {
        LOG_ERROR("System", "SPIFFS mount failed");
    }

    // 7. Presets
    PresetStore::begin();

    // 8. WiFi
    WiFiManager::begin();

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
