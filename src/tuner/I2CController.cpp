#include "I2CController.h"
#include "../config.h"
#include "../logger/Logger.h"
#include <Wire.h>
#include <math.h>

// ── Relay bit mapping ────────────────────────────────────────────────────────
// L value (0-2047, 11 bits):
//   bit 0  = L0  (0.039 µH) → 0x3B.P0
//   bit 1  = L1  (0.078 µH) → 0x3A.P0
//   ...
//   bit 7  = L7  (5 µH)     → 0x3A.P6
//   bit 8  = L8  (10 µH)    → 0x3B.P1
//   bit 9  = L9  (20 µH)    → 0x3B.P2
//   bit 10 = L10 (40 µH)    → 0x3B.P3
//
// C value (0-511, 9 bits):
//   bits 0-7 = C0-C7 → 0x38 (6-800 pF)
//   bit  8   = C8    → 0x39.P0 (1600 pF)
//
// Tuner mode in 0x39:
//   mode 1 (C@TRX): P1=1, P2=0
//   mode 2 (C@ANT): P1=0, P2=1
//   mode 3 (no C):  P1=0, P2=0

bool I2CController::init() {
    Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN, (uint32_t)I2C_FREQ_HZ);

    // Probe required devices
    const uint8_t probeAddrs[] = {
        ADDR_PCF8574_C_LO, ADDR_PCF8574_C_HI,
        ADDR_PCF8574_L_HI, ADDR_PCF8574_L_LO,
        ADDR_PCF8591
    };
    bool ok = true;
    for (uint8_t addr : probeAddrs) {
        Wire.beginTransmission(addr);
        uint8_t err = Wire.endTransmission();
        if (err != 0) {
            LOG_ERROR("I2C", "Device 0x%02X not found (err=%d)", addr, err);
            ok = false;
        }
    }
    if (ok) LOG_INFO("I2C", "All I2C devices found");
    return ok;
}

void I2CController::writePCF8574(uint8_t addr, uint8_t val) {
    Wire.beginTransmission(addr);
    Wire.write(val);
    uint8_t err = Wire.endTransmission();
    if (err) LOG_WARN("I2C", "PCF8574 0x%02X write err=%d", addr, err);
}

void I2CController::setLC(uint16_t L, uint16_t C, uint8_t mode) {
    uint8_t l3A = (uint8_t)((L >> 1) & 0x7F);
    uint8_t l3B = (uint8_t)((L & 0x01) | (((L >> 8) & 0x07) << 1));

    uint8_t c38 = (uint8_t)(C & 0xFF);
    uint8_t c39 = (uint8_t)((C >> 8) & 0x01);
    if (mode == 1) c39 |= 0x02;   // K1
    if (mode == 2) c39 |= 0x04;   // K9+K10

    writePCF8574(ADDR_PCF8574_C_LO, c38);
    writePCF8574(ADDR_PCF8574_C_HI, c39);
    writePCF8574(ADDR_PCF8574_L_HI, l3A);
    writePCF8574(ADDR_PCF8574_L_LO, l3B);

    LOG_DEBUG("I2C", "setLC L=%u C=%u mode=%u → 3A=0x%02X 3B=0x%02X 38=0x%02X 39=0x%02X",
              L, C, mode, l3A, l3B, c38, c39);
}

uint8_t I2CController::readADC(uint8_t channel) {
    // PCF8591 control byte: auto-increment off, single-ended, select channel
    Wire.beginTransmission(ADDR_PCF8591);
    Wire.write(0x40 | (channel & 0x03));
    Wire.endTransmission();
    // Need at least one conversion cycle before result is valid
    delayMicroseconds(250);
    Wire.requestFrom((uint8_t)ADDR_PCF8591, (uint8_t)2);
    Wire.read();          // discard: result of previous cycle
    return Wire.available() ? Wire.read() : 0;
}

SWRResult I2CController::measureSWR(uint8_t minVfwd) {
    uint8_t samples[TUNE_MEASUREMENTS];
    uint8_t revSamples[TUNE_MEASUREMENTS];

    for (int i = 0; i < TUNE_MEASUREMENTS; i++) {
        samples[i]    = readADC(ADC_CH_VFWD);
        revSamples[i] = readADC(ADC_CH_VREV);
        delayMicroseconds(100);
    }

    // Outlier rejection: replace values outside ±5% of max with max
    uint8_t maxFwd = *std::max_element(samples, samples + TUNE_MEASUREMENTS);
    for (int i = 0; i < TUNE_MEASUREMENTS; i++) {
        if (abs((int)samples[i] - maxFwd) > maxFwd * 5 / 100)
            samples[i] = maxFwd;
    }

    uint32_t sumFwd = 0, sumRev = 0;
    for (int i = 0; i < TUNE_MEASUREMENTS; i++) {
        sumFwd += samples[i];
        sumRev += revSamples[i];
    }
    uint8_t vfwd = (uint8_t)(sumFwd / TUNE_MEASUREMENTS);
    uint8_t vrev = (uint8_t)(sumRev / TUNE_MEASUREMENTS);

    SWRResult res = {99.9f, 0.0f, vfwd, vrev};
    if (vfwd < minVfwd) return res;   // no TX signal

    float rho = (vfwd > 0) ? (float)vrev / (float)vfwd : 1.0f;
    if (rho >= 1.0f) rho = 0.999f;
    res.returnLoss = -20.0f * log10f(rho);
    res.swr = (1.0f + rho) / (1.0f - rho);

    LOG_DEBUG("I2C", "SWR meas: vfwd=%d vrev=%d rho=%.3f SWR=%.2f RL=%.1fdB",
              vfwd, vrev, rho, res.swr, res.returnLoss);
    return res;
}

void I2CController::taskI2C(void* param) {
    (void)param;
    I2CCommand cmd;
    for (;;) {
        if (xQueueReceive(g_i2cCmdQueue, &cmd, portMAX_DELAY) == pdTRUE) {
            if (cmd.cmd == I2CCmd::SET_LC) {
                setLC(cmd.L, cmd.C, cmd.mode);
                StateLock lock;
                g_state.L    = cmd.L;
                g_state.C    = cmd.C;
                g_state.mode = cmd.mode;
            } else if (cmd.cmd == I2CCmd::READ_SWR) {
                SWRResult r = measureSWR(DEFAULT_TX_LEVEL);
                StateLock lock;
                g_state.swr        = r.swr;
                g_state.returnLoss = r.returnLoss;
                g_state.vfwd       = r.vfwd;
                g_state.vrev       = r.vrev;
            }
        }
    }
}

// ── Value calculators ────────────────────────────────────────────────────────
float calcLuH(uint16_t L) {
    float val = 0.0f;
    for (int i = 0; i <= 10; i++) {
        if (L & (1u << i)) val += L_UH[i];
    }
    return val;
}

float calcCpF(uint16_t C) {
    float val = 0.0f;
    for (int i = 0; i <= 8; i++) {
        if (C & (1u << i)) val += C_PF[i];
    }
    return val;
}
