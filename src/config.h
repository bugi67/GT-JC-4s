#pragma once

// ── Firmware ──────────────────────────────────────────────────────────────────
#ifndef FIRMWARE_VERSION
#define FIRMWARE_VERSION "1.0.0"
#endif

// ── Hardware ─────────────────────────────────────────────────────────────────
#define I2C_SDA_PIN        6
#define I2C_SCL_PIN        7
#define I2C_FREQ_HZ        100000

// I2C addresses
#define ADDR_PCF8574_C_LO  0x38   // C relays bits 0-7
#define ADDR_PCF8574_C_HI  0x39   // C bit 8 + tuner mode switches
#define ADDR_PCF8574_L_HI  0x3A   // L relays bits 1-7
#define ADDR_PCF8574_L_LO  0x3B   // L bit 0 + L8-L10
#define ADDR_PCF8591       0x48   // ADC: AN0=Vfwd, AN1=Vrev
#define ADDR_EEPROM_PRESET 0x50   // Band presets (256 bytes)
#define ADDR_EEPROM_CFG    0x51   // Configuration (256 bytes)

// ADC channel
#define ADC_CH_VFWD        0
#define ADC_CH_VREV        1

// ── Tuner limits ─────────────────────────────────────────────────────────────
#define L_MAX              2047
#define C_MAX              511
#define TUNER_MODE_MIN     1
#define TUNER_MODE_MAX     3
#define TUNE_MEASUREMENTS  8      // SWR averaging samples

// Inductor values in µH (for L_uH MQTT publish)
static const float L_UH[] = {
    0.039f, 0.078f, 0.156f, 0.312f, 0.625f,
    1.25f,  2.5f,   5.0f,   10.0f,  20.0f, 40.0f
};
// Capacitor values in pF (for C_pF MQTT publish)
static const float C_PF[] = {
    6.0f, 12.0f, 25.0f, 50.0f, 100.0f, 200.0f, 400.0f, 800.0f, 1600.0f
};

// ── AutoTuner defaults ────────────────────────────────────────────────────────
#define DEFAULT_TUNE_THRESHOLD  18.0f   // dB Return Loss (~SWR 1.29)
#define DEFAULT_TX_LEVEL        10      // min Vfwd for TX detect
#define DEFAULT_COARSE_L        64
#define DEFAULT_COARSE_C        16
#define FINE_WINDOW_SIZE        9       // ±4 steps around optimum
#define FINE_MAX_ITER           5

// ── NVS keys ─────────────────────────────────────────────────────────────────
#define NVS_NAMESPACE          "gt-jc-4s"
#define NVS_WIFI_SSID          "wifi_ssid"
#define NVS_WIFI_PASS          "wifi_pass"
#define NVS_MQTT_SERVER        "mqtt_server"
#define NVS_MQTT_PORT          "mqtt_port"
#define NVS_MQTT_ENABLED       "mqtt_en"
#define NVS_TUNE_THRESHOLD     "tune_thr"
#define NVS_TUNE_TX_LEVEL      "tune_tx"
#define NVS_COARSE_STEP_L      "coarse_l"
#define NVS_COARSE_STEP_C      "coarse_c"
#define NVS_OTA_MANIFEST_URL   "ota_url"
#define NVS_LOG_LEVEL          "log_level"

// ── MQTT topics ───────────────────────────────────────────────────────────────
#define MQTT_ROOT              "JC-4s"
#define MQTT_SUB_L             MQTT_ROOT "/L"
#define MQTT_SUB_C             MQTT_ROOT "/C"
#define MQTT_SUB_MODE          MQTT_ROOT "/tunermode"
#define MQTT_SUB_FREQ          MQTT_ROOT "/freq"
#define MQTT_SUB_TUNE          MQTT_ROOT "/tune"
#define MQTT_PUB_FB_L          MQTT_ROOT "/feedback/L"
#define MQTT_PUB_FB_C          MQTT_ROOT "/feedback/C"
#define MQTT_PUB_FB_MODE       MQTT_ROOT "/feedback/tunermode"
#define MQTT_PUB_L_UH          MQTT_ROOT "/L_uH"
#define MQTT_PUB_C_PF          MQTT_ROOT "/C_pF"
#define MQTT_PUB_SWR           MQTT_ROOT "/swr"
#define MQTT_PUB_RSSI          MQTT_ROOT "/rssi"
#define MQTT_PUB_ID            MQTT_ROOT "/id"
#define MQTT_PUB_TUNE_STATUS   MQTT_ROOT "/tune/status"
#define MQTT_PUB_TUNE_PROGRESS MQTT_ROOT "/tune/progress"

// ── Network ───────────────────────────────────────────────────────────────────
#define AP_SSID                "GT-JC-4s-Setup"
#define AP_PASS                ""              // open AP for first-time setup
#define DEFAULT_MQTT_PORT      1883
#define MQTT_RECONNECT_MS      5000
#define RSSI_INTERVAL_MS       10000
#define WEB_PORT               80

// ── OTA ───────────────────────────────────────────────────────────────────────
#define OTA_MANIFEST_URL_DEFAULT \
    "https://github.com/bugi67/GT-JC-4s/releases/latest/download/release.json"
#define OTA_TIMEOUT_MS         60000
#define OTA_BUF_SIZE           1024

// ── Task stack / priority ─────────────────────────────────────────────────────
#define TASK_WEB_STACK         8192
#define TASK_WEB_PRIO          3
#define TASK_MQTT_STACK        4096
#define TASK_MQTT_PRIO         4
#define TASK_TUNER_STACK       6144
#define TASK_TUNER_PRIO        1
#define TASK_I2C_STACK         4096
#define TASK_I2C_PRIO          5
#define TASK_SERIAL_STACK      2048
#define TASK_SERIAL_PRIO       0

#define I2C_CMD_QUEUE_DEPTH    8
