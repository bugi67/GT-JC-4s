#include "I2CController.h"
#include "PresetStore.h"
#include "../config.h"
#include "../cfg/AppConfig.h"
#include "../logger/Logger.h"
#include <Wire.h>
#include <math.h>

// ── Relay bit mapping ────────────────────────────────────────────────────────
// L value (0-2047, 11 bits):
//   bit 0  = L0  (0.039 µH) → 0x3B.P3   (K15A)
//   bit 1  = L1  (0.078 µH) → 0x3A.P0   (K15)
//   bit 2  = L2  (0.156 µH) → 0x3A.P1   (K16)
//   bit 3  = L3  (0.312 µH) → 0x3A.P2   (K17)
//   bit 4  = L4  (0.624 µH) → 0x3A.P3   (K18)
//   bit 5  = L5  (1.25 µH)  → 0x3A.P4   (K19)
//   bit 6  = L6  (2.5 µH)   → 0x3A.P5   (K20)
//   bit 7  = L7  (5 µH)     → 0x3A.P6   (K21)
//   bit 8  = L8  (10 µH)    → 0x3A.P7   (K22)
//   bit 9  = L9  (20 µH)    → 0x3B.P0   (K23+K24 parallel)
//   bit 10 = L10 (40 µH)    → 0x3B.P1   (K25+K26+K27 parallel)
//   0x3B.P2 = Memory Dis / Ant A — INPUT, never drive low!
//
// C value (0-511, 9 bits) → directly to port bits:
//   bit 0 → 0x38.P0 (K13+K14 = 6 pF)
//   bit 1 → 0x38.P1 (K11+K12 = 12 pF)
//   bit 2 → 0x38.P2 (K8      = 25 pF)
//   bit 3 → 0x38.P3 (K7      = 50 pF)
//   bit 4 → 0x38.P4 (K6      = 100 pF)
//   bit 5 → 0x38.P5 (K5      = 200 pF)
//   bit 6 → 0x38.P6 (K4      = 400 pF)
//   bit 7 → 0x38.P7 (K3      = 800 pF)
//   bit 8 → 0x39.P0 (K2      = 1600 pF)
//
// Tuner mode in 0x39:
//   mode 1 (C@TRX): P1=1, P2=1  (K9+K10 inverted: active when mode != 2)
//   mode 2 (C@ANT): P1=0, P2=0
//   mode 3 (no C):  P1=0, P2=1  (K9+K10 inverted)
//   K-Tune relay:   0x39.P3     (active = bit 3 set)

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
    // 0x3A: L bits 1-7 → P0-P6; L bit 8 (10µH) → P7
    uint8_t l3A = (uint8_t)(((L >> 1) & 0x7F) | (((L >> 8) & 0x01) << 7));
    // 0x3B: L bit 9 → P0; L bit 10 → P1; P2=Input (keep high!); L bit 0 → P3
    uint8_t l3B = (uint8_t)(((L >> 9) & 0x01) | (((L >> 10) & 0x01) << 1) | 0x04 | ((L & 0x01) << 3));

    uint8_t c38 = (uint8_t)(C & 0xFF);
    uint8_t c39 = (uint8_t)((C >> 8) & 0x01);
    if (mode == 1) c39 |= 0x02;          // K1
    if (mode != 2) c39 |= 0x04;          // K9+K10 inverted: active when NOT in C@ANT mode
    if (g_state.kTune)  c39 |= 0x08;    // K-Tune at P3

    writePCF8574(ADDR_PCF8574_C_LO, c38);
    writePCF8574(ADDR_PCF8574_C_HI, c39);
    writePCF8574(ADDR_PCF8574_L_HI, l3A);
    writePCF8574(ADDR_PCF8574_L_LO, l3B);

    LOG_DEBUG("I2C", "setLC L=%u C=%u mode=%u kTune=%d → 3A=0x%02X 3B=0x%02X 38=0x%02X 39=0x%02X",
              L, C, mode, (int)g_state.kTune, l3A, l3B, c38, c39);
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

    SWRResult res = {0.0f, 0.0f, vfwd, vrev};
    if (vfwd < minVfwd) return res;   // no TX signal → swr=0 so UI shows "—"

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
        if (xQueueReceive(g_i2cCmdQueue, &cmd, pdMS_TO_TICKS(250)) == pdTRUE) {
            if (cmd.cmd == I2CCmd::SET_LC) {
                setLC(cmd.L, cmd.C, cmd.mode);
                StateLock lock;
                g_state.L    = cmd.L;
                g_state.C    = cmd.C;
                g_state.mode = cmd.mode;
            } else if (cmd.cmd == I2CCmd::READ_SWR) {
                SWRResult r = measureSWR(g_cfg.tune_tx_level);
                StateLock lock;
                g_state.swr        = r.swr;
                g_state.returnLoss = r.returnLoss;
                g_state.vfwd       = r.vfwd;
                g_state.vrev       = r.vrev;
            } else if (cmd.cmd == I2CCmd::SET_KTUNE) {
                uint16_t C; uint8_t mode;
                {
                    StateLock lock;
                    g_state.kTune = cmd.kTune;
                    C = g_state.C; mode = g_state.mode;
                }
                // Only rewrite 0x39 — leaves 0x38 (C_LO), 0x3A and 0x3B (L) untouched
                uint8_t c39 = (uint8_t)((C >> 8) & 0x01);
                if (mode == 1)     c39 |= 0x02;
                if (mode != 2)     c39 |= 0x04;
                if (cmd.kTune)     c39 |= 0x08;
                writePCF8574(ADDR_PCF8574_C_HI, c39);
                LOG_INFO("I2C", "K-Tune relay %s (0x39=0x%02X)", cmd.kTune ? "ON" : "OFF", c39);
            } else if (cmd.cmd == I2CCmd::SAVE_PRESET) {
                Preset p;
                p.freq_kHz = cmd.freq_kHz;
                p.L = cmd.L; p.C = cmd.C; p.mode = cmd.mode;
                LOG_INFO("I2C", "SAVE_PRESET: freq=%u L=%u C=%u mode=%u count=%d",
                         p.freq_kHz, p.L, p.C, p.mode, PresetStore::count());
                if (!PresetStore::save(p)) {
                    LOG_ERROR("I2C", "SAVE_PRESET failed (count=%d max=%d)",
                              PresetStore::count(), PRESET_MAX_COUNT);
                }
            }
        } else {
            // Queue idle: background SWR measurement every ~250 ms
            SWRResult r = measureSWR(g_cfg.tune_tx_level);
            {
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
