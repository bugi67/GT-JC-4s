#pragma once
#include <Arduino.h>
#include "../state.h"

class AutoTuner {
public:
    // FreeRTOS task – waits on g_tuneStartSem, runs tune sequence
    static void taskAutoTuner(void* param);

private:
    static float  s_threshold;   // Return Loss threshold (dB)
    static uint16_t s_coarseL;
    static uint16_t s_coarseC;

    // Returns best {L, C, mode} and SWR found; true = found below threshold
    static bool runTune(uint16_t& bestL, uint16_t& bestC, uint8_t& bestMode);

    static void setLCAndWait(uint16_t L, uint16_t C, uint8_t mode, uint32_t settleMs = 5);
    static float getSWR();
    static float getRL();

    static bool isAbortRequested();
    static void reportProgress(uint8_t pct);

    // Phase 1: preset search
    static bool presetSearch(uint16_t& L, uint16_t& C, uint8_t& mode);

    // Phase 2: coarse scan over all L/C in step increments, all 3 modes
    static void coarseScan(uint16_t& L, uint16_t& C, uint8_t& mode);

    // Phase 3: fine-step within sliding window around coarse optimum
    static void fineTune(uint16_t& L, uint16_t& C, uint8_t mode);
};
