#pragma once
#include <Arduino.h>
#include "../state.h"

struct SWRResult {
    float swr;
    float returnLoss;   // dB
    uint8_t vfwd;
    uint8_t vrev;
};

class I2CController {
public:
    static bool init();

    // Relay control – called only from taskI2C
    static void setLC(uint16_t L, uint16_t C, uint8_t mode);

    // SWR measurement (8 samples, outlier rejection) – called from taskI2C
    static SWRResult measureSWR(uint8_t minVfwd = 10);

    // I2C task entry point
    static void taskI2C(void* param);

private:
    static uint8_t  readADC(uint8_t channel);
    static void     writePCF8574(uint8_t addr, uint8_t val);
};

// Helper: calculate L value in µH from relay code
float calcLuH(uint16_t L);
// Helper: calculate C value in pF from relay code
float calcCpF(uint16_t C);
