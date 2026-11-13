#include "I2CController.h"
#include "PresetStore.h"
#include "../config.h"
#include "../cfg/AppConfig.h"
#include "../logger/Logger.h"
#include <Wire.h>
#include <math.h>
#include <Adafruit_ADS1X15.h>

// Single ADS1115 instance for the Vfwd/Vrev bridge (0x48). Using Adafruit's
// library rather than hand-rolled register writes — a hand-rolled driver's
// config writes were ACK'ing (no I2C error) but never actually taking effect
// on the chip (config-register readback never matched what was written, on
// real hardware); switched to the well-tested library rather than keep
// debugging that blind over OTA.
static Adafruit_ADS1115 s_ads;
static bool              s_adsOk = false;

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
//
// 0x39.P4-P7 = sense inputs from the tuning-circuit bridge (F-PWR/Z-HIGH/
// Z-LOW/PHASE — see SenseInputs). These must always be written '1' (never
// driven low like P0-P3) so the PCF8574's quasi-bidirectional pins release
// and let the external circuit pull them — hence PCF8574_C_HI_INPUT_MASK
// is OR'd into every write to this chip.

bool I2CController::init() {
    Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN, (uint32_t)I2C_FREQ_HZ);

    // Probe the PCF8574 relay expanders (no library — plain register writes)
    const uint8_t probeAddrs[] = {
        ADDR_PCF8574_C_LO, ADDR_PCF8574_C_HI,
        ADDR_PCF8574_L_HI, ADDR_PCF8574_L_LO
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

    // ADS1115 now lives on its own dedicated bus (Wire1, D0/D1) rather than
    // sharing Wire with the PCF8574s — the shared bus was found to corrupt
    // its register reads (writes ACK fine, but every read came back 0x0000).
    Wire1.begin(I2C2_SDA_PIN, I2C2_SCL_PIN, (uint32_t)I2C2_FREQ_HZ);

    // ADS1115 — begin() itself talks the real register protocol (config
    // write + readback), a far more meaningful check than a bare address ACK.
    s_adsOk = s_ads.begin(ADDR_ADS1115, &Wire1);
    if (!s_adsOk) {
        // Adafruit_I2CDevice::begin() calls Wire.begin() again internally (no
        // args) right before its own ACK check — immediately after our own
        // explicit Wire.begin() just above, that can transiently upset the
        // bus with no settle time. A single retry reliably succeeds once the
        // bus has settled; address ACKs outside this path are unaffected.
        delay(5);
        s_adsOk = s_ads.begin(ADDR_ADS1115, &Wire1);
        if (s_adsOk) LOG_WARN("I2C", "ADS1115 begin() needed a retry");
    }
    if (s_adsOk) {
        s_ads.setGain(GAIN_ONE);              // ±4.096V, ~125 µV/count
        s_ads.setDataRate(RATE_ADS1115_860SPS); // fastest — coarse scan does ~2000 reads
    } else {
        LOG_ERROR("I2C", "ADS1115 0x%02X not found/not responding", ADDR_ADS1115);
    }
    ok = ok && s_adsOk;

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
    c39 |= PCF8574_C_HI_INPUT_MASK;      // release P4-P7 (sense inputs) — never drive them low

    writePCF8574(ADDR_PCF8574_C_LO, c38);
    writePCF8574(ADDR_PCF8574_C_HI, c39);
    writePCF8574(ADDR_PCF8574_L_HI, l3A);
    writePCF8574(ADDR_PCF8574_L_LO, l3B);

    LOG_DEBUG("I2C", "setLC L=%u C=%u mode=%u kTune=%d → 3A=0x%02X 3B=0x%02X 38=0x%02X 39=0x%02X",
              L, C, mode, (int)g_state.kTune, l3A, l3B, c38, c39);
}

// Single-shot read of one ADS1115 input via the Adafruit library; clamped to
// >= 0 (the forward/reverse detector outputs are physically unipolar — small
// negative codes only happen from noise right at 0 V).
//
// Deliberately NOT using the library's own readADC_SingleEnded(): it polls
// conversionComplete() in an unbounded `while (!conversionComplete());` loop
// with no timeout. A single glitched status read there hangs taskI2C (and
// the whole tuner) permanently, with no way to recover short of a reboot —
// observed directly during bring-up. Same register protocol, just bounded.
static uint16_t readADS1115(uint8_t channel) {
    if (!s_adsOk) return 0;

    // Once a channel has timed out, retry at most once/second instead of
    // hammering the bus with a doomed conversion on every ~250ms idle cycle
    // (measureSWR() alone calls this 8x per channel per measurement).
    static unsigned long s_lastAttempt[2] = {0, 0};
    static bool          s_lastOk[2]      = {true, true};
    if (!s_lastOk[channel] && millis() - s_lastAttempt[channel] < 1000) return 0;
    s_lastAttempt[channel] = millis();

    s_ads.startADCReading(MUX_BY_CHANNEL[channel], /*continuous=*/false);
    // 860SPS converts in ~1.16ms; wait that out up front instead of hammering
    // the bus with repeated status-register transactions from t=0 — normally
    // turns the poll below into a single confirming read instead of several.
    delayMicroseconds(1250);
    unsigned long t0 = millis();
    while (!s_ads.conversionComplete()) {
        if (millis() - t0 > 20) {   // generous margin beyond the expected ~1.2ms
            LOG_ERROR("I2C", "ADS1115 ch%u: conversion timeout", channel);
            s_lastOk[channel] = false;
            return 0;
        }
    }
    s_lastOk[channel] = true;
    int16_t raw = s_ads.getLastConversionResults();
    LOG_DEBUG("I2C", "ADS1115 ch%u: raw=%d", channel, raw);
    return raw > 0 ? (uint16_t)raw : 0;
}

// RF power calibration curve (ADS1115 voltage -> watts), measured directly
// on this hardware (2026-11-05, 50 ohm load, SWR 1.01-1.02 throughout) rather
// than borrowed from a sensor datasheet — an initial attempt using WaveNode's
// published HF-1/UHF-1 POWER_CAL_3 curve was off by ~3.5x against a real 5W
// reference, showing this coupler/divider/buffer chain has its own transfer
// characteristic, not WaveNode's. The 6 real points below span 1-10W (the
// only range available for live calibration); the per-segment exponent
// (power vs. voltage) rises smoothly from ~1.0 near 1-2W to ~1.8 near 7-10W,
// consistent with a real diode detector's transition from its square-law
// (low-level, V proportional to power) toward its linear (higher-level, V
// proportional to RF voltage amplitude) region — not measurement noise.
// Source power levels are the Elecraft KX3's own front-panel setting, not an
// inline power meter — good enough to fit a sane curve shape, but the whole
// table's absolute scale should be rechecked against a real inline power
// meter when available. Readings above 10W extrapolate linearly past the
// last segment; treat them as indicative only until recalibrated with real
// higher-power points.
struct PowerCalPoint { float volts, watts; };
static const PowerCalPoint GTJC4S_PWR_CAL[] = {
    {0.00000f,  0.0f},
    {0.03250f,  1.0f},   // vfwd=260
    {0.06488f,  2.0f},   // vfwd=519
    {0.08975f,  3.0f},   // vfwd=718
    {0.12125f,  5.0f},   // vfwd=970
    {0.15263f,  7.0f},   // vfwd=1221
    {0.18575f, 10.0f},   // vfwd=1486
};
static const int GTJC4S_PWR_CAL_N = sizeof(GTJC4S_PWR_CAL) / sizeof(GTJC4S_PWR_CAL[0]);

// Piecewise-linear interpolation over the table above; extrapolates linearly
// past the last point rather than clamping at 10W, since nothing stops a
// higher-power run from being logged even if it's past the calibrated range.
static float wavenodePowerW(float voltageV) {
    if (voltageV <= GTJC4S_PWR_CAL[0].volts) return 0.0f;
    int i = 1;
    while (i < GTJC4S_PWR_CAL_N - 1 && voltageV > GTJC4S_PWR_CAL[i].volts) i++;
    const PowerCalPoint& a = GTJC4S_PWR_CAL[i - 1];
    const PowerCalPoint& b = GTJC4S_PWR_CAL[i];
    float t = (voltageV - a.volts) / (b.volts - a.volts);
    return a.watts + t * (b.watts - a.watts);
}

SWRResult I2CController::measureSWR(uint16_t minVfwd) {
    uint16_t samples[TUNE_MEASUREMENTS];
    uint16_t revSamples[TUNE_MEASUREMENTS];

    for (int i = 0; i < TUNE_MEASUREMENTS; i++) {
        samples[i]    = readADS1115(0);   // A0 = Vfwd
        revSamples[i] = readADS1115(1);   // A1 = Vrev
    }

    // Outlier rejection: replace values outside ±5% of max with max
    uint16_t maxFwd = *std::max_element(samples, samples + TUNE_MEASUREMENTS);
    for (int i = 0; i < TUNE_MEASUREMENTS; i++) {
        if (abs((int)samples[i] - (int)maxFwd) > maxFwd * 5 / 100)
            samples[i] = maxFwd;
    }

    uint32_t sumFwd = 0, sumRev = 0;
    for (int i = 0; i < TUNE_MEASUREMENTS; i++) {
        sumFwd += samples[i];
        sumRev += revSamples[i];
    }
    // Keep the averages in floating point — same reasoning as before (avoids
    // quantising return loss into whole-count steps near a match), though the
    // ADS1115's 16-bit resolution makes this far less critical than it was on
    // the old 8-bit PCF8591.
    float vfwdAvg = (float)sumFwd / TUNE_MEASUREMENTS;
    float vrevAvg = (float)sumRev / TUNE_MEASUREMENTS;

    // No FOR/2-style correction here: A0 (Vfwd) and A1 (Vrev) both go through
    // a matching TLC2272 op-amp stage + 4:1 divider, so rho = Vrev/Vfwd needs
    // no asymmetry fix (unlike the old PCF8591 bridge).

    float powerFwdW = wavenodePowerW(vfwdAvg * ADS1115_VOLTS_PER_COUNT);
    float powerRefW = wavenodePowerW(vrevAvg * ADS1115_VOLTS_PER_COUNT);

    SWRResult res = {0.0f, 0.0f, (uint16_t)(vfwdAvg + 0.5f), (uint16_t)(vrevAvg + 0.5f),
                      powerFwdW, powerRefW};
    if (vfwdAvg < (float)minVfwd) return res;   // no TX signal → swr=0 so UI shows "—"

    float rho = (vfwdAvg > 0.0f) ? vrevAvg / vfwdAvg : 1.0f;
    if (rho >= 1.0f) rho = 0.999f;
    res.returnLoss = -20.0f * log10f(rho);
    res.swr = (1.0f + rho) / (1.0f - rho);

    LOG_DEBUG("I2C", "SWR meas: vfwd=%.2f vrev=%.2f rho=%.4f SWR=%.2f RL=%.1fdB Pfwd=%.1fW Prev=%.1fW",
              vfwdAvg, vrevAvg, rho, res.swr, res.returnLoss, powerFwdW, powerRefW);
    return res;
}

SenseInputs I2CController::readSenseInputs() {
    Wire.requestFrom(ADDR_PCF8574_C_HI, (uint8_t)1);
    uint8_t v = Wire.available() ? Wire.read() : 0xFF;
    return SenseInputs{
        (v & 0x10) != 0,   // P4 = F-PWR
        (v & 0x20) != 0,   // P5 = Z-HIGH
        (v & 0x40) != 0,   // P6 = Z-LOW
        (v & 0x80) != 0,   // P7 = PHASE
    };
}

void I2CController::taskI2C(void* param) {
    (void)param;

    // PCF8574 expanders boot with all ports HIGH → all relay coils energised.
    // Wait 500 ms so chips and transistors are fully settled before the first write.
    vTaskDelay(pdMS_TO_TICKS(500));

    // De-energise relays one chip at a time with 150 ms gaps.
    // Switching all 27 coils simultaneously droops the PCF8574 supply below
    // their ~1.5 V reset threshold, causing them to latch back to all-HIGH.
    LOG_INFO("I2C", "Relay init step 1/4: L_LO (0x3B)");
    writePCF8574(ADDR_PCF8574_L_LO, 0x04);   // L0, L9, L10 off; input P2 HIGH
    vTaskDelay(pdMS_TO_TICKS(150));

    LOG_INFO("I2C", "Relay init step 2/4: L_HI (0x3A)");
    writePCF8574(ADDR_PCF8574_L_HI, 0x00);   // L1-L8 off
    vTaskDelay(pdMS_TO_TICKS(150));

    LOG_INFO("I2C", "Relay init step 3/4: C_LO (0x38)");
    writePCF8574(ADDR_PCF8574_C_LO, 0x00);   // C bits 0-7 off
    vTaskDelay(pdMS_TO_TICKS(150));

    LOG_INFO("I2C", "Relay init step 4/4: C_HI (0x39)");
    // C bit 8 off; K1 + K9/K10 on (C@TRX mode); P4-P7 released for sense inputs
    writePCF8574(ADDR_PCF8574_C_HI, 0x06 | PCF8574_C_HI_INPUT_MASK);
    vTaskDelay(pdMS_TO_TICKS(150));

    {
        StateLock lock;
        g_state.L    = 0;
        g_state.C    = 0;
        g_state.mode = 1;
    }
    // Read back each chip to verify the writes actually changed the output register.
    // If the PCF8574 returns 0xFF instead of the written value, it reset (VCC droop).
    auto readPCF = [](uint8_t addr) -> uint8_t {
        Wire.requestFrom(addr, (uint8_t)1);
        return Wire.available() ? Wire.read() : 0xEE;
    };
    uint8_t rb3B = readPCF(ADDR_PCF8574_L_LO);
    uint8_t rb3A = readPCF(ADDR_PCF8574_L_HI);
    uint8_t rb38 = readPCF(ADDR_PCF8574_C_LO);
    uint8_t rb39 = readPCF(ADDR_PCF8574_C_HI);
    // 0x39.P4-P7 reflect the external sense circuit now, not our write, so only
    // the output bits (P0-P3) are checked against the expected value.
    LOG_INFO("I2C", "PCF8574 readback: 0x3B=0x%02X(exp 0x04) 0x3A=0x%02X(exp 0x00) 0x38=0x%02X(exp 0x00) 0x39=0x%02X(exp low nibble 0x6)",
             rb3B, rb3A, rb38, rb39);
    bool initOk = (rb3B == 0x04) && (rb3A == 0x00) && (rb38 == 0x00) && ((rb39 & 0x0F) == 0x06);
    LOG_INFO("I2C", "Hardware initialised: L=0 C=0 mode=C@TRX %s", initOk ? "[OK]" : "[MISMATCH - chips may have reset!]");
    LOG_INFO("I2C", "Sense inputs at boot: F-PWR=%d Z-HIGH=%d Z-LOW=%d PHASE=%d (raw, polarity uncalibrated)",
             (rb39 >> 4) & 1, (rb39 >> 5) & 1, (rb39 >> 6) & 1, (rb39 >> 7) & 1);

    // Drain any commands that accumulated in the queue during the ~600 ms init
    // window (typically MQTT retained-message replays). Without this flush they
    // would immediately override the freshly zeroed relay state.
    {
        I2CCommand discard;
        int n = 0;
        while (xQueueReceive(g_i2cCmdQueue, &discard, 0) == pdTRUE) n++;
        if (n) LOG_INFO("I2C", "Flushed %d stale pre-init command(s)", n);
    }
    g_relayInitDone = true;
    LOG_INFO("I2C", "Ready — accepting relay commands");

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
                SenseInputs s = readSenseInputs();
                {
                    StateLock lock;
                    g_state.swr        = r.swr;
                    g_state.returnLoss = r.returnLoss;
                    g_state.vfwd       = r.vfwd;
                    g_state.vrev       = r.vrev;
                    g_state.powerFwdW  = r.powerFwdW;
                    g_state.powerRefW  = r.powerRefW;
                    g_state.fPwr       = s.fPwr;
                    g_state.zHigh      = s.zHigh;
                    g_state.zLow       = s.zLow;
                    g_state.phase      = s.phase;
                }
                // AutoTuner blocks on this instead of guessing a fixed delay
                // before reading g_state. Only given here, not from the idle-
                // loop background refresh below — that one has no waiter and
                // would otherwise risk unblocking AutoTuner with an unrelated
                // measurement's result.
                xSemaphoreGive(g_swrDoneSem);
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
                c39 |= PCF8574_C_HI_INPUT_MASK;   // release P4-P7 (sense inputs)
                writePCF8574(ADDR_PCF8574_C_HI, c39);
                LOG_INFO("I2C", "K-Tune relay %s (0x39=0x%02X)", cmd.kTune ? "ON" : "OFF", c39);
            } else if (cmd.cmd == I2CCmd::SAVE_PRESET) {
                Preset p;
                p.freq_kHz = cmd.freq_kHz;
                p.L = cmd.L; p.C = cmd.C; p.mode = cmd.mode; p.swr = cmd.swr;
                p.shellyOn = cmd.shellyOn;
                LOG_INFO("I2C", "SAVE_PRESET: freq=%u L=%u C=%u mode=%u swr=%.2f count=%d",
                         p.freq_kHz, p.L, p.C, p.mode, p.swr, PresetStore::count());
                if (!PresetStore::save(p)) {
                    LOG_ERROR("I2C", "SAVE_PRESET failed (count=%d max=%d)",
                              PresetStore::count(), PRESET_MAX_COUNT);
                }
            }
        } else {
            // Queue idle: background SWR + sense-input refresh every ~250 ms
            SWRResult r = measureSWR(g_cfg.tune_tx_level);
            SenseInputs s = readSenseInputs();
            {
                StateLock lock;
                g_state.swr        = r.swr;
                g_state.returnLoss = r.returnLoss;
                g_state.vfwd       = r.vfwd;
                g_state.vrev       = r.vrev;
                g_state.powerFwdW  = r.powerFwdW;
                g_state.powerRefW  = r.powerRefW;
                g_state.fPwr       = s.fPwr;
                g_state.zHigh      = s.zHigh;
                g_state.zLow       = s.zLow;
                g_state.phase      = s.phase;
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
