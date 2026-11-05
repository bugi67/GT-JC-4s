#pragma once
#include <Arduino.h>
#include "../config.h"
#include "../state.h"

struct SWRResult {
    float swr;
    float returnLoss;   // dB
    uint16_t vfwd;       // ADS1115 counts (was uint8_t for the old 8-bit PCF8591)
    uint16_t vrev;
    float powerFwdW;     // forward power, watts (WaveNode HF-1/UHF-1 cal curve)
    float powerRevW;     // reflected power, watts
};

// Raw hardware sense inputs on 0x39.P4-P7. Polarity (which level = which
// condition) is not yet calibrated — these are exposed as-read; see
// /api/status and the Web-UI stats panel "Sense Inputs (raw)".
struct SenseInputs {
    bool fPwr;    // P4 — F-PWR: power applied to the tuner
    bool zHigh;   // P5 — Z-HIGH: antenna impedance > 50 ohm
    bool zLow;    // P6 — Z-LOW: antenna impedance < 50 ohm
    bool phase;   // P7 — PHASE: sign of V/I phase difference
};

class I2CController {
public:
    static bool init();

    // Relay control – called only from taskI2C
    static void setLC(uint16_t L, uint16_t C, uint8_t mode);

    // SWR measurement (8 samples, outlier rejection) – called from taskI2C
    static SWRResult measureSWR(uint16_t minVfwd = DEFAULT_TX_LEVEL);

    // Read the F-PWR/Z-HIGH/Z-LOW/PHASE sense inputs – called from taskI2C
    static SenseInputs readSenseInputs();

    // I2C task entry point
    static void taskI2C(void* param);

private:
    static void writePCF8574(uint8_t addr, uint8_t val);
};

// Helper: calculate L value in µH from relay code
float calcLuH(uint16_t L);
// Helper: calculate C value in pF from relay code
float calcCpF(uint16_t C);
