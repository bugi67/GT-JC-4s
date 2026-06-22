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
    uint32_t totalSteps = (uint32_t)lSteps * cSteps * 3;
    uint32_t step = 0;

    for (uint8_t m = 1; m <= 3; m++) {
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

void AutoTuner::fineTune(uint16_t& bestL, uint16_t& bestC, uint8_t mode) {
    I2CCommand mCmd = {I2CCmd::READ_SWR, 0, 0, 0};
    int half = FINE_WINDOW_SIZE / 2;

    for (int iter = 0; iter < FINE_MAX_ITER; iter++) {
        bool improved = false;

        // Optimise L with fixed C
        float bestRL = -999.0f;
        uint16_t newL = bestL;
        for (int dl = -half; dl <= half; dl++) {
            int l = (int)bestL + dl;
            if (l < 0 || l > L_MAX) continue;
            setLCAndWait((uint16_t)l, bestC, mode, 3);
            xQueueSend(g_i2cCmdQueue, &mCmd, portMAX_DELAY);
            vTaskDelay(pdMS_TO_TICKS(15));
            float rl = getRL();
            if (rl > bestRL) { bestRL = rl; newL = (uint16_t)l; }
        }
        if (newL != bestL) { bestL = newL; improved = true; }

        // Optimise C with fixed L
        float bestRLC = -999.0f;
        uint16_t newC = bestC;
        for (int dc = -half; dc <= half; dc++) {
            int c = (int)bestC + dc;
            if (c < 0 || c > C_MAX) continue;
            setLCAndWait(bestL, (uint16_t)c, mode, 3);
            xQueueSend(g_i2cCmdQueue, &mCmd, portMAX_DELAY);
            vTaskDelay(pdMS_TO_TICKS(15));
            float rl = getRL();
            if (rl > bestRLC) { bestRLC = rl; newC = (uint16_t)c; }
        }
        if (newC != bestC) { bestC = newC; improved = true; }

        reportProgress((uint8_t)(80 + iter * 4));   // 80-100%
        if (!improved) break;
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
        // Wait for tune start signal
        xSemaphoreTake(g_tuneStartSem, portMAX_DELAY);

        {
            StateLock lock;
            g_state.tuneState    = TunerState::TuneState::TUNING;
            g_state.tuneProgress = 0;
        }

        LOG_INFO("AutoTuner", "AutoTune started");

        uint16_t L = 0, C = 0;
        uint8_t  mode = 1;

        // Drain any stale abort signal before we start
        xSemaphoreTake(g_tuneAbortSem, 0);

        bool ok = runTune(L, C, mode);

        // Apply best result
        setLCAndWait(L, C, mode);
        I2CCommand mCmd = {I2CCmd::READ_SWR, 0, 0, 0};
        xQueueSend(g_i2cCmdQueue, &mCmd, portMAX_DELAY);
        vTaskDelay(pdMS_TO_TICKS(30));

        auto finalState = ok ? TunerState::TuneState::DONE : TunerState::TuneState::ABORTED;
        float finalSWR;
        {
            StateLock lock;
            g_state.tuneState = finalState;
            finalSWR = g_state.swr;
        }

        if (ok) {
            // Save preset
            Preset p;
            p.freq_kHz = stateGet(&TunerState::freq_kHz);
            p.L = L; p.C = C; p.mode = mode;
            if (p.freq_kHz > 0) PresetStore::save(p);
        }

        LOG_INFO("AutoTuner", "AutoTune %s – L=%u C=%u mode=%u SWR=%.2f",
                 ok ? "DONE" : "ABORTED", L, C, mode, finalSWR);
    }
}
