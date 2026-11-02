#pragma once
#include <Arduino.h>
#include "../state.h"

class AutoTuner {
public:
    // FreeRTOS task – waits on g_tuneStartSem, runs tune sequence
    static void taskAutoTuner(void* param);

private:
    // Returns best {L, C, mode} and SWR found; true = found below threshold
    static bool runTune(uint16_t& bestL, uint16_t& bestC, uint8_t& bestMode);

    static void setLCAndWait(uint16_t L, uint16_t C, uint8_t mode, uint32_t settleMs = 5);
    static float getSWR();
    static float getRL();
    // Apply L/C/mode, settle, average `samples` SWR reads → mean return loss (dB)
    static float measureAvg(uint16_t L, uint16_t C, uint8_t mode, int samples);

    static bool isAbortRequested();
    static void reportProgress(uint8_t pct);

    // Phase 1: preset search
    static bool presetSearch(uint16_t& L, uint16_t& C, uint8_t& mode);

    // Phase 2: Grundmatch — doubling/geometric grid over the full L/C range,
    // both modes (1=C@TRX, 2=C@ANT). Returns per-mode L/C/grid so the caller
    // can zoom the winner and fall back to the other mode if needed.
    static void grundMatch(uint16_t lGrid[], int& nL, uint16_t cGrid[], int& nC,
                            uint16_t lByMode[3], uint16_t cByMode[3], float rlByMode[3]);

    // Phase 2.5: Submatch — iterative zoom around (L,C) at fixed mode: scan a
    // ±4-step window, reconfirm the top-2 readings with averaging, recenter
    // on the winner, halve the step, repeat until step=1 or an early-exit
    // quality threshold is reached.
    static void subMatch(uint16_t& L, uint16_t& C, uint8_t mode, float& bestRL,
                          uint16_t initStepL, uint16_t initStepC);

    // Phase 3: fine-step within sliding window around coarse optimum
    // verbose=true prints every measurement as a table on Serial
    static void fineTune(uint16_t& L, uint16_t& C, uint8_t mode, bool verbose = false);
};
