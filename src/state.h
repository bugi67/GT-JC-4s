#pragma once
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/queue.h>

// ── I2C command sent via queue to taskI2C ─────────────────────────────────────
enum class I2CCmd : uint8_t {
    SET_LC,        // set L, C, mode relays
    READ_SWR,      // trigger SWR measurement, result written to state
    SET_KTUNE,     // set K-Tune relay (0x39.P3); uses kTune field only
    SAVE_PRESET,   // write preset to EEPROM; uses freq_kHz, L, C, mode fields
};

struct I2CCommand {
    I2CCmd   cmd;
    uint16_t L;
    uint16_t C;
    uint8_t  mode;
    bool     kTune;      // used by SET_KTUNE
    uint16_t freq_kHz;   // used by SAVE_PRESET
};

// ── Central runtime state ─────────────────────────────────────────────────────
struct TunerState {
    // Current tuner position
    uint16_t L          = 0;
    uint16_t C          = 0;
    uint8_t  mode       = 1;      // 1=C@TRX  2=C@ANT  3=no C
    uint16_t freq_kHz   = 0;

    // SWR measurement
    float    swr        = 99.9f;
    float    returnLoss = 0.0f;   // dB
    uint8_t  vfwd       = 0;
    uint8_t  vrev       = 0;

    // K-Tune relay
    bool     kTune     = false;    // 0x39.P3

    // AutoTuner state
    enum class TuneState : uint8_t {
        IDLE, TUNING, DONE, ABORTED
    } tuneState = TuneState::IDLE;
    uint8_t tuneProgress = 0;     // 0-100 %

    // OTA state
    enum class OtaState : uint8_t {
        IDLE, CHECKING, DOWNLOADING_FW, DOWNLOADING_FS, DONE, ERROR
    } otaState = OtaState::IDLE;
    uint8_t  otaProgress = 0;
    char     otaError[48] = {};

    // System
    int8_t   rssi       = 0;
    char     fwVersion[12] = FIRMWARE_VERSION;
};

// ── Global state + synchronisation primitives ────────────────────────────────
extern TunerState          g_state;
extern SemaphoreHandle_t   g_stateMutex;
extern SemaphoreHandle_t   g_tuneStartSem;
extern SemaphoreHandle_t   g_fineTuneStartSem;
extern SemaphoreHandle_t   g_tuneAbortSem;
extern QueueHandle_t       g_i2cCmdQueue;

// ── Convenience RAII-style accessors ─────────────────────────────────────────
class StateLock {
public:
    StateLock()  { xSemaphoreTake(g_stateMutex, portMAX_DELAY); }
    ~StateLock() { xSemaphoreGive(g_stateMutex); }
};

// Helper: read a field safely
template<typename T>
T stateGet(T TunerState::*field) {
    StateLock lock;
    return g_state.*field;
}
