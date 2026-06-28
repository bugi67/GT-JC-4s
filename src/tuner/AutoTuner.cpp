#include "AutoTuner.h"
#include "I2CController.h"
#include "PresetStore.h"
#include "../config.h"
#include "../cfg/AppConfig.h"
#include "../logger/Logger.h"

// ── Helpers ──────────────────────────────────────────────────────────────────

void AutoTuner::setLCAndWait(uint16_t L, uint16_t C, uint8_t mode, uint32_t settleMs) {
    I2CCommand cmd = {I2CCmd::SET_LC, L, C, mode};
    xQueueSend(g_i2cCmdQueue, &cmd, portMAX_DELAY);
    vTaskDelay(pdMS_TO_TICKS(settleMs));
}

float AutoTuner::getSWR() {
    return stateGet(&TunerState::swr);
}

float AutoTuner::getRL() {
    return stateGet(&TunerState::returnLoss);
}

bool AutoTuner::isAbortRequested() {
    return xSemaphoreTake(g_tuneAbortSem, 0) == pdTRUE;
}

void AutoTuner::reportProgress(uint8_t pct) {
    StateLock lock;
    g_state.tuneProgress = pct;
}

// ── Phase 1: Preset search ───────────────────────────────────────────────────

bool AutoTuner::presetSearch(uint16_t& L, uint16_t& C, uint8_t& mode) {
    uint16_t freq = stateGet(&TunerState::freq_kHz);
    if (freq == 0) return false;

    Preset best;
    if (!PresetStore::findBest(freq, best)) return false;

    LOG_INFO("AutoTuner", "Preset found: freq=%u L=%u C=%u mode=%u", best.freq_kHz, best.L, best.C, best.mode);

    // Measure SWR with preset values
    setLCAndWait(best.L, best.C, best.mode);
    I2CCommand mCmd = {I2CCmd::READ_SWR, 0, 0, 0};
    xQueueSend(g_i2cCmdQueue, &mCmd, portMAX_DELAY);
    vTaskDelay(pdMS_TO_TICKS(20));

    float rl = getRL();
    LOG_INFO("AutoTuner", "Preset SWR: RL=%.1f dB (threshold=%.1f)", rl, g_cfg.tune_threshold);

    if (rl >= g_cfg.tune_threshold) {
        L = best.L; C = best.C; mode = best.mode;
        return true;
    }
    // Preset not good enough → use as starting point for coarse scan
    L = best.L; C = best.C; mode = best.mode;
    return false;
}

// ── Phase 2: Coarse scan ─────────────────────────────────────────────────────

void AutoTuner::coarseScan(uint16_t& bestL, uint16_t& bestC, uint8_t& bestMode) {
    float bestRL = -999.0f;
    I2CCommand mCmd = {I2CCmd::READ_SWR, 0, 0, 0};

    uint16_t lSteps = (L_MAX / g_cfg.coarse_step_l) + 1;
    uint16_t cSteps = (C_MAX / g_cfg.coarse_step_c) + 1;
    uint32_t totalSteps = (uint32_t)lSteps * cSteps * 2;  // modes 1+2 only (no "No C")
    uint32_t step = 0;

    for (uint8_t m = 1; m <= 2; m++) {  // 1=C@TRX, 2=C@ANT; mode 3 (No C) skipped
        for (uint16_t c = 0; c <= C_MAX; c += g_cfg.coarse_step_c) {
            for (uint16_t l = 0; l <= L_MAX; l += g_cfg.coarse_step_l) {
                if (isAbortRequested()) {
                    LOG_INFO("AutoTuner", "Coarse scan aborted");
                    return;
                }
                setLCAndWait(l, c, m, 3);
                xQueueSend(g_i2cCmdQueue, &mCmd, portMAX_DELAY);
                vTaskDelay(pdMS_TO_TICKS(15));

                float rl = getRL();
                if (rl > bestRL) {
                    bestRL = rl;
                    bestL = l; bestC = c; bestMode = m;
                }
                step++;
                reportProgress((uint8_t)(step * 80 / totalSteps));   // 0-80% for coarse
            }
        }
    }
    LOG_INFO("AutoTuner", "Coarse done: L=%u C=%u mode=%u RL=%.1fdB", bestL, bestC, bestMode, bestRL);
}

// ── Phase 3: Fine-step ───────────────────────────────────────────────────────

void AutoTuner::fineTune(uint16_t& bestL, uint16_t& bestC, uint8_t mode, bool verbose) {
    I2CCommand mCmd = {I2CCmd::READ_SWR, 0, 0, 0};
    int half = FINE_WINDOW_SIZE / 2;

    struct Step { uint16_t L, C; float swr, rl; };
    Step steps[FINE_WINDOW_SIZE];

    for (int iter = 0; iter < FINE_MAX_ITER; iter++) {
        bool improved = false;

        // Optimise L with fixed C
        float bestRL = -999.0f;
        uint16_t newL = bestL;
        int nL = 0;
        for (int dl = -half; dl <= half; dl++) {
            int l = (int)bestL + dl;
            if (l < 0 || l > L_MAX) continue;
            setLCAndWait((uint16_t)l, bestC, mode, 3);
            xQueueSend(g_i2cCmdQueue, &mCmd, portMAX_DELAY);
            vTaskDelay(pdMS_TO_TICKS(15));
            float rl  = getRL();
            float swr = stateGet(&TunerState::swr);
            if (rl > bestRL) { bestRL = rl; newL = (uint16_t)l; }
            if (verbose) steps[nL++] = { (uint16_t)l, bestC, swr, rl };
        }
        if (newL != bestL) { bestL = newL; improved = true; }

        if (verbose) {
            Serial.printf("\r\n Iter %d / L-Sweep (C=%u):\r\n", iter + 1, bestC);
            Serial.printf("   L   |   C   |   SWR  |    RL    |\r\n");
            Serial.printf(" ------+-------+--------+----------+\r\n");
            for (int i = 0; i < nL; i++)
                Serial.printf(" %5u | %5u | %6.2f | %6.1f dB |%s\r\n",
                    steps[i].L, steps[i].C, steps[i].swr, steps[i].rl,
                    steps[i].L == bestL ? " <--" : "");
        }

        // RL > 60 dB means Vrev=0 (ADC noise floor) — perfect match.
        // Rescanning C from this plateau edge causes drift to the first
        // equal-RL position in the new window, which may be less stable.
        if (bestRL > 60.0f) {
            reportProgress((uint8_t)(80 + iter * 4));
            break;
        }

        // Optimise C with fixed L
        float bestRLC = -999.0f;
        uint16_t newC = bestC;
        int nC = 0;
        for (int dc = -half; dc <= half; dc++) {
            int c = (int)bestC + dc;
            if (c < 0 || c > C_MAX) continue;
            setLCAndWait(bestL, (uint16_t)c, mode, 3);
            xQueueSend(g_i2cCmdQueue, &mCmd, portMAX_DELAY);
            vTaskDelay(pdMS_TO_TICKS(15));
            float rl  = getRL();
            float swr = stateGet(&TunerState::swr);
            if (rl > bestRLC) { bestRLC = rl; newC = (uint16_t)c; }
            if (verbose) steps[nC++] = { bestL, (uint16_t)c, swr, rl };
        }
        if (newC != bestC) { bestC = newC; improved = true; }

        if (verbose) {
            Serial.printf("\r\n Iter %d / C-Sweep (L=%u):\r\n", iter + 1, bestL);
            Serial.printf("   L   |   C   |   SWR  |    RL    |\r\n");
            Serial.printf(" ------+-------+--------+----------+\r\n");
            for (int i = 0; i < nC; i++)
                Serial.printf(" %5u | %5u | %6.2f | %6.1f dB |%s\r\n",
                    steps[i].L, steps[i].C, steps[i].swr, steps[i].rl,
                    steps[i].C == bestC ? " <--" : "");
        }

        reportProgress((uint8_t)(80 + iter * 4));   // 80-100%
        if (!improved || bestRLC > 60.0f) break;
    }
    LOG_INFO("AutoTuner", "Fine done: L=%u C=%u mode=%u", bestL, bestC, mode);
}

// ── Main tune sequence ───────────────────────────────────────────────────────

bool AutoTuner::runTune(uint16_t& bestL, uint16_t& bestC, uint8_t& bestMode) {
    reportProgress(0);

    // Phase 1
    bool presetHit = presetSearch(bestL, bestC, bestMode);
    if (presetHit) {
        reportProgress(100);
        return true;
    }

    // Phase 2
    coarseScan(bestL, bestC, bestMode);
    if (isAbortRequested()) return false;

    // Phase 3
    setLCAndWait(bestL, bestC, bestMode);
    fineTune(bestL, bestC, bestMode);

    reportProgress(100);
    return true;
}

// ── FreeRTOS task ─────────────────────────────────────────────────────────────

void AutoTuner::taskAutoTuner(void* param) {
    (void)param;
    for (;;) {
        // Wait for either AutoTune or Fine-Tune start signal
        bool fineOnly = false;
        for (;;) {
            if (xSemaphoreTake(g_tuneStartSem,     0) == pdTRUE) break;
            if (xSemaphoreTake(g_fineTuneStartSem, 0) == pdTRUE) { fineOnly = true; break; }
            vTaskDelay(pdMS_TO_TICKS(50));
        }

        {
            StateLock lock;
            g_state.tuneState    = TunerState::TuneState::TUNING;
            g_state.tuneProgress = 0;
        }
        xSemaphoreTake(g_tuneAbortSem, 0);   // drain stale abort

        // K-Tune ON before tuning starts
        { I2CCommand k = {}; k.cmd = I2CCmd::SET_KTUNE; k.kTune = true;
          xQueueSend(g_i2cCmdQueue, &k, portMAX_DELAY); }
        vTaskDelay(pdMS_TO_TICKS(30));        // relay settle

        uint16_t L, C; uint8_t mode; bool ok;

        if (fineOnly) {
            LOG_INFO("AutoTuner", "Fine-Tune started from current L/C/mode");
            { StateLock lock; L = g_state.L; C = g_state.C; mode = g_state.mode; }
            reportProgress(0);
            setLCAndWait(L, C, mode);
            fineTune(L, C, mode, true);
            ok = !isAbortRequested();
            reportProgress(100);
        } else {
            LOG_INFO("AutoTuner", "AutoTune started");
            L = 0; C = 0; mode = 1;
            ok = runTune(L, C, mode);
        }

        // Apply best result and measure final SWR (with K-Tune still ON)
        // 20 ms settle: relay de-energise (spring return) can take up to ~10 ms
        setLCAndWait(L, C, mode, 20);
        I2CCommand mCmd = {I2CCmd::READ_SWR, 0, 0, 0};
        xQueueSend(g_i2cCmdQueue, &mCmd, portMAX_DELAY);
        vTaskDelay(pdMS_TO_TICKS(50));        // give I2C task time to complete READ_SWR

        // Capture SWR now, before K-Tune OFF triggers a background measurement
        float finalSWR, finalRL;
        { StateLock lock; finalSWR = g_state.swr; finalRL = g_state.returnLoss; }

        // K-Tune OFF after tuning ends
        { I2CCommand k = {}; k.cmd = I2CCmd::SET_KTUNE; k.kTune = false;
          xQueueSend(g_i2cCmdQueue, &k, portMAX_DELAY); }
        vTaskDelay(pdMS_TO_TICKS(30));        // relay settle

        auto finalState = ok ? TunerState::TuneState::DONE : TunerState::TuneState::ABORTED;
        { StateLock lock; g_state.tuneState = finalState; }

        if (ok) {
            uint16_t freq = stateGet(&TunerState::freq_kHz);
            if (freq > 0) {
                I2CCommand sc = {};
                sc.cmd = I2CCmd::SAVE_PRESET;
                sc.freq_kHz = freq; sc.L = L; sc.C = C; sc.mode = mode;
                xQueueSend(g_i2cCmdQueue, &sc, portMAX_DELAY);
            }
        }

        LOG_INFO("AutoTuner", "%s %s – L=%u C=%u mode=%u SWR=%.2f",
                 fineOnly ? "FineTune" : "AutoTune",
                 ok ? "DONE" : "ABORTED", L, C, mode, finalSWR);

        if (fineOnly) {
            static const char* modeNames[] = { "", "C@TRX", "C@ANT", "No C" };
            const char* modeStr = (mode >= 1 && mode <= 3) ? modeNames[mode] : "?";
            Serial.printf("\r\n+----------------------+------------+\r\n");
            Serial.printf("| Fine-Tune Result     |            |\r\n");
            Serial.printf("+----------------------+------------+\r\n");
            Serial.printf("| L (raw)              | %10u |\r\n", L);
            Serial.printf("| L                    | %7.3f uH |\r\n", calcLuH(L));
            Serial.printf("| C (raw)              | %10u |\r\n", C);
            Serial.printf("| C                    | %7.1f pF |\r\n", calcCpF(C));
            Serial.printf("| Mode                 | %10s |\r\n", modeStr);
            Serial.printf("| SWR                  | %10.2f |\r\n", finalSWR);
            Serial.printf("| Return Loss          | %7.1f dB |\r\n", finalRL);
            Serial.printf("+----------------------+------------+\r\n\r\n");
        }
    }
}
