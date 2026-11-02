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
    xSemaphoreTake(g_swrDoneSem, pdMS_TO_TICKS(100));

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

// ── Band limits for coarse scan ──────────────────────────────────────────────
// L_max = XL_max(1000Ω) / (2π·f) / 0.039µH; C_max = 1/(2π·f·XC_min(10Ω)) in pF / 6.25 pF/step
// Full range below 40m; reduced above to cut scan time (4–50× faster on higher bands).

struct BandLimit { uint16_t fLow, fHigh, lMax, cMax; };

static const BandLimit BAND_LIMITS[] = {
    { 1800,  2000, 2047, 511 },   // 160m — full range
    { 3500,  3800, 2047, 511 },   // 80m  — full range
    { 5351,  5366,  763, 494 },   // 60m  ~30 µH, ~2970 pF
    { 7000,  7200,  576, 511 },   // 40m  ~22 µH, >3000 pF
    {10100, 10150,  404, 494 },   // 30m  ~16 µH, ~1580 pF
    {14000, 14350,  292, 357 },   // 20m  ~11 µH, ~1140 pF
    {18068, 18168,  225, 275 },   // 17m  ~8.8 µH, ~880 pF
    {21000, 21450,  195, 237 },   // 15m  ~7.6 µH, ~760 pF
    {24890, 24990,  164, 200 },   // 12m  ~6.4 µH, ~640 pF
    {28000, 29700,  146, 178 },   // 10m  ~5.7 µH, ~570 pF
};

static void getBandLimits(uint16_t freq_kHz, uint16_t& lMax, uint16_t& cMax) {
    for (const auto& b : BAND_LIMITS) {
        if (freq_kHz >= b.fLow && freq_kHz <= b.fHigh) {
            lMax = b.lMax; cMax = b.cMax;
            return;
        }
    }
    lMax = L_MAX; cMax = C_MAX;  // default: full range
}

// ── Grid helpers (Grundmatch / Submatch) ─────────────────────────────────────
// L/C are literal bit-weighted relay masks (see L_UH[]/C_PF[] in config.h), so
// a doubling sequence isn't an arbitrary choice — it's "energise one more
// relay bit at a time."

// {0, 1, 2, 4, 8, ..., last power-of-2 <= maxVal}, then append maxVal if not
// already present — guarantees the band-limited top of the range is always
// sampled even when maxVal isn't a power of two (e.g. 15m band: lMax=195).
static int buildGeomGrid(uint16_t maxVal, uint16_t out[], int outCap) {
    int n = 0;
    out[n++] = 0;
    for (uint32_t v = 1; v <= maxVal && n < outCap - 1; v <<= 1) out[n++] = (uint16_t)v;
    if (out[n - 1] != maxVal) out[n++] = maxVal;
    return n;
}

static int indexOfExact(const uint16_t* grid, int n, uint16_t val) {
    for (int i = 0; i < n; i++) if (grid[i] == val) return i;
    return 0;  // shouldn't happen — val always comes from this same grid
}

// Level-0 Submatch step from the Grundmatch winner's immediate neighbours in
// the geometric grid — width = 2x the larger neighbour gap ("roughly double
// the immediate neighbourhood").
static uint16_t deriveInitialStep(const uint16_t* grid, int n, uint16_t val) {
    int idx = indexOfExact(grid, n, val);
    uint16_t loGap = (idx > 0)     ? (uint16_t)(val - grid[idx - 1]) : val;
    uint16_t hiGap = (idx < n - 1) ? (uint16_t)(grid[idx + 1] - val) : 0;
    return max((uint16_t)1, (uint16_t)(max(loGap, hiGap) / 4));
}

// Up to 9 clamped/deduped points spanning [center-4*step, center+4*step] ∩ [0,maxVal].
static int genAxisPoints(uint16_t center, uint16_t step, uint16_t maxVal, uint16_t out[9]) {
    int n = 0;
    for (int i = -4; i <= 4; i++) {
        int32_t v = constrain((int32_t)center + i * (int32_t)step, 0, (int32_t)maxVal);
        if (n == 0 || out[n - 1] != (uint16_t)v) out[n++] = (uint16_t)v;
    }
    return n;
}

// ── Phase 2: Grundmatch ──────────────────────────────────────────────────────
// Doubling/geometric grid over the full band-limited L/C range, both modes
// (1=C@TRX, 2=C@ANT; mode 3 "No C" skipped, as before). Replaces the old
// linear ~2000-point coarse scan with ~100-300 points while still covering
// the whole space — every grid point other than 0 is a single relay bit.

void AutoTuner::grundMatch(uint16_t lGrid[], int& nL, uint16_t cGrid[], int& nC,
                            uint16_t lByMode[3], uint16_t cByMode[3], float rlByMode[3]) {
    uint16_t lMax, cMax;
    getBandLimits(stateGet(&TunerState::freq_kHz), lMax, cMax);
    nL = buildGeomGrid(lMax, lGrid, 16);
    nC = buildGeomGrid(cMax, cGrid, 16);
    LOG_INFO("AutoTuner", "Grundmatch scan: L grid n=%d (0..%u), C grid n=%d (0..%u), modes 1+2",
             nL, lMax, nC, cMax);

    // Track the top-2 single-shot readings per mode, not just a single running
    // best — a plain "if (rl > best)" tracker has no protection against noise,
    // and a later noisy spike can silently and permanently discard an earlier,
    // genuinely excellent (often broad) matching region with no way to recover
    // it. Confirmed live (2026-11-02, 25Ω load): grundMatch() reported L=256
    // C=128 mode=1 as the winner, but L=0-8/C=100-160 was a real, broad, ~45dB
    // plateau measured earlier in the same scan — it just lost a one-shot
    // numeric comparison to a later noisy reading at a point that, on direct
    // re-measurement, isn't a real match at all. Same class of bug as the
    // original addCandidate() issue, just one phase upstream.
    struct Top2 { uint16_t L, C; float rl; } top1[3], top2[3];
    top1[1] = top1[2] = top2[1] = top2[2] = {0, 0, -999.0f};

    I2CCommand mCmd = {I2CCmd::READ_SWR, 0, 0, 0};
    uint32_t totalSteps = (uint32_t)nL * nC * 2;
    uint32_t step = 0;

    for (uint8_t m = 1; m <= 2; m++) {
        for (int ci = 0; ci < nC; ci++) {
            for (int li = 0; li < nL; li++) {
                if (isAbortRequested()) {
                    LOG_INFO("AutoTuner", "Grundmatch aborted");
                    return;
                }
                setLCAndWait(lGrid[li], cGrid[ci], m, GRUNDMATCH_SETTLE_MS);
                xQueueSend(g_i2cCmdQueue, &mCmd, portMAX_DELAY);
                xSemaphoreTake(g_swrDoneSem, pdMS_TO_TICKS(100));

                float rl = getRL();
                if (rl > top1[m].rl) { top2[m] = top1[m]; top1[m] = {lGrid[li], cGrid[ci], rl}; }
                else if (rl > top2[m].rl) { top2[m] = {lGrid[li], cGrid[ci], rl}; }
                step++;
                reportProgress((uint8_t)(step * 30 / totalSteps));   // 0-30%
            }
        }
    }

    // Re-confirm each mode's top-2 with averaging before committing to a
    // winner — same noise protection subMatch() already uses per level.
    for (uint8_t m = 1; m <= 2; m++) {
        float rl1 = measureAvg(top1[m].L, top1[m].C, m, SUBMATCH_CONFIRM_SAMPLES);
        float rl2 = (top2[m].rl > -999.0f)
                        ? measureAvg(top2[m].L, top2[m].C, m, SUBMATCH_CONFIRM_SAMPLES) : -999.0f;
        if (rl2 > rl1) { lByMode[m] = top2[m].L; cByMode[m] = top2[m].C; rlByMode[m] = rl2; }
        else           { lByMode[m] = top1[m].L; cByMode[m] = top1[m].C; rlByMode[m] = rl1; }
    }
}

// ── Phase 2.5: Submatch ──────────────────────────────────────────────────────
// Iterative zoom at a fixed mode: scan a ±4-step window around (L,C), take the
// top-2 single-shot readings, re-confirm BOTH with averaging, recenter on
// whichever reconfirms higher, halve the step, repeat. Terminates at step=1
// in both axes or once a reconfirmed reading clears SUBMATCH_EARLY_EXIT_RL.
//
// Re-confirming the top-2 (not just trusting the top-1 single shot) matters:
// live debugging (2026-11-02) proved a single READ_SWR — already 8-sample-
// averaged internally — can read 47dB when a stable re-measurement at the
// exact same point reads 12.7dB. Blindly recentring on one bad single-shot
// reading wouldn't just corrupt a result here — each level's window is
// strictly smaller than the last, so a wrong recentre could permanently walk
// the search away from the real match with no later level able to see it
// again. Reconfirming before committing a new centre closes that gap.

void AutoTuner::subMatch(uint16_t& L, uint16_t& C, uint8_t mode, float& bestRL,
                          uint16_t initStepL, uint16_t initStepC) {
    uint16_t lMax, cMax;
    getBandLimits(stateGet(&TunerState::freq_kHz), lMax, cMax);

    // Estimate total levels up front (for progress reporting only) — number
    // of halvings of the larger initial step until it reaches 1.
    int totalLevels = 1;
    for (uint16_t s = max(initStepL, initStepC); s > 1; s = max((uint16_t)1, (uint16_t)(s / 2)))
        totalLevels++;

    uint16_t stepL = initStepL, stepC = initStepC;
    I2CCommand mCmd = {I2CCmd::READ_SWR, 0, 0, 0};

    for (int level = 0; level < SUBMATCH_MAX_LEVELS; level++) {
        uint16_t lPts[9], cPts[9];
        int nL = genAxisPoints(L, stepL, lMax, lPts);
        int nC = genAxisPoints(C, stepC, cMax, cPts);

        struct { uint16_t L, C; float rl; } top1 = {L, C, -999.0f}, top2 = {L, C, -999.0f};

        for (int ci = 0; ci < nC; ci++) {
            for (int li = 0; li < nL; li++) {
                if (isAbortRequested()) return;
                setLCAndWait(lPts[li], cPts[ci], mode, FINE_SETTLE_MS);
                xQueueSend(g_i2cCmdQueue, &mCmd, portMAX_DELAY);
                xSemaphoreTake(g_swrDoneSem, pdMS_TO_TICKS(100));
                float rl = getRL();
                if (rl > top1.rl) { top2 = top1; top1 = {lPts[li], cPts[ci], rl}; }
                else if (rl > top2.rl) { top2 = {lPts[li], cPts[ci], rl}; }
            }
        }
        if (isAbortRequested()) return;

        float rl1 = measureAvg(top1.L, top1.C, mode, SUBMATCH_CONFIRM_SAMPLES);
        float rl2 = (top2.rl > -999.0f) ? measureAvg(top2.L, top2.C, mode, SUBMATCH_CONFIRM_SAMPLES) : -999.0f;
        if (rl2 > rl1) { L = top2.L; C = top2.C; bestRL = rl2; }
        else           { L = top1.L; C = top1.C; bestRL = rl1; }

        LOG_INFO("AutoTuner", "Submatch level %d: L=%u C=%u mode=%u RL=%.1fdB (stepL=%u stepC=%u)",
                 level, L, C, mode, bestRL, stepL, stepC);
        reportProgress((uint8_t)(30 + min(level + 1, totalLevels) * 40 / totalLevels));   // 30-70%

        if (bestRL >= SUBMATCH_EARLY_EXIT_RL) {
            LOG_INFO("AutoTuner", "Submatch early-exit: RL=%.1f >= %.1f dB", bestRL, (float)SUBMATCH_EARLY_EXIT_RL);
            return;
        }
        if (stepL <= 1 && stepC <= 1) return;
        stepL = max((uint16_t)1, (uint16_t)(stepL / 2));
        stepC = max((uint16_t)1, (uint16_t)(stepC / 2));
    }
    LOG_WARN("AutoTuner", "Submatch: hit max levels (%d) without reaching step=1 — unexpected", SUBMATCH_MAX_LEVELS);
}

// ── Phase 3: Fine-step ───────────────────────────────────────────────────────

namespace {
struct FineStep { uint16_t L, C; float swr, rl; };

// Collapse a completed sweep onto the value to settle on. A real match is
// rarely a single point — a span of adjacent settings all read within
// PLATEAU_MARGIN_DB of the peak. Settle on that span's midpoint so the result
// sits in the centre of the null (most tolerant of drift), not on whichever
// edge happened to read highest. `coordIsL` picks the L or C field; `bounded`
// is cleared when the span runs to a window edge (the true optimum may lie
// beyond it, so the caller should keep iterating rather than stop early).
uint16_t plateauCentre(const FineStep* s, int n, uint16_t peak, float peakRL,
                       bool coordIsL, bool& bounded) {
    auto coord = [&](int i) { return coordIsL ? s[i].L : s[i].C; };
    int pk = 0;
    for (int i = 0; i < n; i++) if (coord(i) == peak) { pk = i; break; }
    int lo = pk, hi = pk;
    while (lo > 0     && s[lo - 1].rl >= peakRL - PLATEAU_MARGIN_DB) lo--;
    while (hi < n - 1 && s[hi + 1].rl >= peakRL - PLATEAU_MARGIN_DB) hi++;
    bounded = (lo > 0 && hi < n - 1);
    return coord((lo + hi) / 2);
}

// Best few L/C points seen across the whole fine-tune. The iterative L/C walk
// explores, but single SWR readings scatter several dB (and occasionally 10+),
// so the walk can end on a point far worse than one it passed through. We keep
// the strongest handful — kept spatially diverse so they are real alternatives,
// not three noisy neighbours — and re-measure them at the end to choose.
struct FineCand { uint16_t L, C; float rl; bool used; };

void considerCand(FineCand* c, int n, uint16_t L, uint16_t C, float rl) {
    for (int i = 0; i < n; i++) {
        if (!c[i].used) continue;
        if (abs((int)L - (int)c[i].L) <= 2 && abs((int)C - (int)c[i].C) <= 2) {
            if (rl > c[i].rl) { c[i].L = L; c[i].C = C; c[i].rl = rl; }
            return;
        }
    }
    int weak = -1;
    for (int i = 0; i < n; i++) {
        if (!c[i].used) { c[i] = {L, C, rl, true}; return; }
        if (weak < 0 || c[i].rl < c[weak].rl) weak = i;
    }
    if (rl > c[weak].rl) c[weak] = {L, C, rl, true};
}
}  // namespace

void AutoTuner::fineTune(uint16_t& bestL, uint16_t& bestC, uint8_t mode, bool verbose) {
    I2CCommand mCmd = {I2CCmd::READ_SWR, 0, 0, 0};
    int half = FINE_WINDOW_SIZE / 2;

    FineStep steps[FINE_WINDOW_SIZE];
    FineCand cand[4] = {};

    // Confirmed baseline: the position we start from. The walk is only allowed
    // to move the result away from here if a re-measured candidate genuinely
    // beats it — so fine-tune can never hand back something worse than the start.
    const uint16_t startL = bestL, startC = bestC;
    const float    startRL = measureAvg(startL, startC, mode, FINE_CONFIRM_SAMPLES);
    considerCand(cand, 4, startL, startC, startRL);

    for (int iter = 0; iter < FINE_MAX_ITER; iter++) {
        bool improved = false;

        // Optimise L with fixed C
        float bestRL = -999.0f;
        uint16_t newL = bestL;
        int nL = 0;
        for (int dl = -half; dl <= half; dl++) {
            int l = (int)bestL + dl;
            if (l < 0 || l > L_MAX) continue;
            setLCAndWait((uint16_t)l, bestC, mode, FINE_SETTLE_MS);
            xQueueSend(g_i2cCmdQueue, &mCmd, portMAX_DELAY);
            xSemaphoreTake(g_swrDoneSem, pdMS_TO_TICKS(100));
            float rl  = getRL();
            float swr = stateGet(&TunerState::swr);
            if (rl > bestRL) { bestRL = rl; newL = (uint16_t)l; }
            considerCand(cand, 4, (uint16_t)l, bestC, rl);
            steps[nL++] = { (uint16_t)l, bestC, swr, rl };
        }
        bool lBounded = true;
        newL = plateauCentre(steps, nL, newL, bestRL, true, lBounded);
        if (newL != bestL) { bestL = newL; improved = true; }

        if (verbose) {
            LOG_INFO("AutoTuner", "Iter %d / L-Sweep (C=%u):", iter + 1, bestC);
            LOG_INFO("AutoTuner", "  L   |   C   |   SWR  |    RL    |");
            LOG_INFO("AutoTuner", "------+-------+--------+----------+");
            for (int i = 0; i < nL; i++)
                LOG_INFO("AutoTuner", "%5u | %5u | %6.2f | %6.1f dB |%s",
                    steps[i].L, steps[i].C, steps[i].swr, steps[i].rl,
                    steps[i].L == bestL ? " <--" : "");
        }

        // Perfect match (Vrev ≈ 0) on a bounded null — nothing to gain from
        // rescanning C. If the null ran to a window edge, keep going: the real
        // optimum may be further out.
        if (bestRL > 60.0f && lBounded) {
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
            setLCAndWait(bestL, (uint16_t)c, mode, FINE_SETTLE_MS);
            xQueueSend(g_i2cCmdQueue, &mCmd, portMAX_DELAY);
            xSemaphoreTake(g_swrDoneSem, pdMS_TO_TICKS(100));
            float rl  = getRL();
            float swr = stateGet(&TunerState::swr);
            if (rl > bestRLC) { bestRLC = rl; newC = (uint16_t)c; }
            considerCand(cand, 4, bestL, (uint16_t)c, rl);
            steps[nC++] = { bestL, (uint16_t)c, swr, rl };
        }
        bool cBounded = true;
        newC = plateauCentre(steps, nC, newC, bestRLC, false, cBounded);
        if (newC != bestC) { bestC = newC; improved = true; }

        if (verbose) {
            LOG_INFO("AutoTuner", "Iter %d / C-Sweep (L=%u):", iter + 1, bestL);
            LOG_INFO("AutoTuner", "  L   |   C   |   SWR  |    RL    |");
            LOG_INFO("AutoTuner", "------+-------+--------+----------+");
            for (int i = 0; i < nC; i++)
                LOG_INFO("AutoTuner", "%5u | %5u | %6.2f | %6.1f dB |%s",
                    steps[i].L, steps[i].C, steps[i].swr, steps[i].rl,
                    steps[i].C == bestC ? " <--" : "");
        }

        reportProgress((uint8_t)(80 + iter * 4));   // 80-100%
        if (!improved || (bestRLC > 60.0f && cBounded)) break;
    }

    // Re-measure the walk endpoint and every surviving candidate with heavier
    // averaging; keep the start position as the fallback so we never regress.
    uint16_t pickL = startL, pickC = startC;
    float pickRL = startRL;
    if (verbose)
        LOG_INFO("AutoTuner", "Confirm: L=%3u C=%3u  start          re-meas RL=%5.1f dB",
                 startL, startC, startRL);

    float walkRL = measureAvg(bestL, bestC, mode, FINE_CONFIRM_SAMPLES);
    if (verbose)
        LOG_INFO("AutoTuner", "Confirm: L=%3u C=%3u  walk-end       re-meas RL=%5.1f dB",
                 bestL, bestC, walkRL);
    if (walkRL > pickRL) { pickRL = walkRL; pickL = bestL; pickC = bestC; }

    for (int i = 0; i < 4; i++) {
        if (!cand[i].used) continue;
        if ((cand[i].L == bestL && cand[i].C == bestC) ||
            (cand[i].L == startL && cand[i].C == startC)) continue;
        float rl = measureAvg(cand[i].L, cand[i].C, mode, FINE_CONFIRM_SAMPLES);
        if (verbose)
            LOG_INFO("AutoTuner", "Confirm: L=%3u C=%3u  walk RL=%5.1f  re-meas RL=%5.1f dB",
                     cand[i].L, cand[i].C, cand[i].rl, rl);
        if (rl > pickRL) { pickRL = rl; pickL = cand[i].L; pickC = cand[i].C; }
    }
    bestL = pickL; bestC = pickC;
    if (pickRL < DEFAULT_TUNE_THRESHOLD)
        LOG_WARN("AutoTuner", "Fine-tune: best only %.1f dB (<%.0f) — mismatch too large here, or no carrier",
                 pickRL, DEFAULT_TUNE_THRESHOLD);
    LOG_INFO("AutoTuner", "Fine done: L=%u C=%u mode=%u (RL=%.1f dB)", bestL, bestC, mode, pickRL);
}

// Set L/C/mode, settle, then average `samples` SWR reads → return mean return loss.
float AutoTuner::measureAvg(uint16_t L, uint16_t C, uint8_t mode, int samples) {
    I2CCommand mCmd = {I2CCmd::READ_SWR, 0, 0, 0};
    setLCAndWait(L, C, mode, 20);
    float sum = 0.0f; int n = 0;
    for (int i = 0; i < samples; i++) {
        xQueueSend(g_i2cCmdQueue, &mCmd, portMAX_DELAY);
        xSemaphoreTake(g_swrDoneSem, pdMS_TO_TICKS(100));
        float rl = getRL();
        if (rl > 80.0f) rl = 80.0f;   // clamp "perfect" (Vrev≈0) so one read can't dominate the mean
        sum += rl; n++;
    }
    return n ? sum / n : -999.0f;
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

    // Phase 2 — Grundmatch: doubling grid, both modes, tracks the best point
    // per mode (free — same measurement count as tracking only the overall
    // best).
    uint16_t lGrid[16], cGrid[16]; int nL, nC;
    uint16_t lByMode[3], cByMode[3]; float rlByMode[3] = {-999.0f, -999.0f, -999.0f};
    grundMatch(lGrid, nL, cGrid, nC, lByMode, cByMode, rlByMode);
    if (isAbortRequested()) return false;

    bestMode = (rlByMode[1] >= rlByMode[2]) ? 1 : 2;
    bestL = lByMode[bestMode]; bestC = cByMode[bestMode];
    float bestRL = rlByMode[bestMode];
    uint8_t otherMode = (bestMode == 1) ? 2 : 1;
    LOG_INFO("AutoTuner", "Grundmatch done: L=%u C=%u mode=%u RL=%.1fdB (mode %u RL=%.1fdB)",
             bestL, bestC, bestMode, bestRL, otherMode, rlByMode[otherMode]);

    // Phase 2.5 — Submatch: iterative zoom on the winning mode
    uint16_t stepL0 = deriveInitialStep(lGrid, nL, bestL);
    uint16_t stepC0 = deriveInitialStep(cGrid, nC, bestC);
    subMatch(bestL, bestC, bestMode, bestRL, stepL0, stepC0);
    if (isAbortRequested()) return false;

    // Fallback — if the winning mode's zoom still isn't good enough, try the
    // other mode's Grundmatch point too (mirrors presetSearch()'s own
    // "good enough" bar — no new threshold invented).
    if (bestRL < g_cfg.tune_threshold) {
        LOG_WARN("AutoTuner", "Submatch RL=%.1fdB < threshold %.1fdB — trying mode %u as fallback",
                 bestRL, g_cfg.tune_threshold, otherMode);
        uint16_t altL = lByMode[otherMode], altC = cByMode[otherMode];
        float altRL = rlByMode[otherMode];
        subMatch(altL, altC, otherMode, altRL,
                 deriveInitialStep(lGrid, nL, altL), deriveInitialStep(cGrid, nC, altC));
        if (isAbortRequested()) return false;
        if (altRL > bestRL) {
            bestRL = altRL; bestL = altL; bestC = altC; bestMode = otherMode;
            LOG_INFO("AutoTuner", "Fallback mode %u wins: RL=%.1fdB", otherMode, altRL);
        }
    }
    if (isAbortRequested()) return false;

    // Phase 3
    reportProgress(80);   // fineTune()'s own progress math is hardcoded "80 + iter*4"
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
        xSemaphoreTake(g_swrDoneSem,   0);   // drain stale give (e.g. a prior run aborted mid-measurement)

        // K-Tune is no longer required for a tune run (the ADS1115 bridge is
        // always in the RF path, unlike the old PCF8591 sense circuit) — the
        // relay stays untouched here; the manual button/API still exist for
        // diagnostics.

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

        // Apply best result and measure final SWR
        // 20 ms settle: relay de-energise (spring return) can take up to ~10 ms
        setLCAndWait(L, C, mode, 20);
        I2CCommand mCmd = {I2CCmd::READ_SWR, 0, 0, 0};
        xQueueSend(g_i2cCmdQueue, &mCmd, portMAX_DELAY);
        xSemaphoreTake(g_swrDoneSem, pdMS_TO_TICKS(100));

        float finalSWR, finalRL;
        { StateLock lock; finalSWR = g_state.swr; finalRL = g_state.returnLoss; }

        auto finalState = ok ? TunerState::TuneState::DONE : TunerState::TuneState::ABORTED;
        { StateLock lock; g_state.tuneState = finalState; }

        if (ok) {
            uint16_t freq = stateGet(&TunerState::freq_kHz);
            if (freq > 0) {
                uint16_t freqSnapped = (freq / 20) * 20;  // floor to 20 kHz segment start
                LOG_INFO("AutoTuner", "Saving preset at %u kHz (snapped from %u kHz)", freqSnapped, freq);
                I2CCommand sc = {};
                sc.cmd = I2CCmd::SAVE_PRESET;
                sc.freq_kHz = freqSnapped; sc.L = L; sc.C = C; sc.mode = mode; sc.swr = finalSWR;
                sc.shellyOn = (freqSnapped >= 3500 && freqSnapped <= 4000);
                xQueueSend(g_i2cCmdQueue, &sc, portMAX_DELAY);
            }
        }

        LOG_INFO("AutoTuner", "%s %s – L=%u C=%u mode=%u SWR=%.2f",
                 fineOnly ? "FineTune" : "AutoTune",
                 ok ? "DONE" : "ABORTED", L, C, mode, finalSWR);

        if (fineOnly) {
            static const char* modeNames[] = { "", "C@TRX", "C@ANT", "No C" };
            const char* modeStr = (mode >= 1 && mode <= 3) ? modeNames[mode] : "?";
            LOG_INFO("AutoTuner", "+----------------------+------------+");
            LOG_INFO("AutoTuner", "| Fine-Tune Result     |            |");
            LOG_INFO("AutoTuner", "+----------------------+------------+");
            LOG_INFO("AutoTuner", "| L (raw)              | %10u |", L);
            LOG_INFO("AutoTuner", "| L                    | %7.3f uH |", calcLuH(L));
            LOG_INFO("AutoTuner", "| C (raw)              | %10u |", C);
            LOG_INFO("AutoTuner", "| C                    | %7.1f pF |", calcCpF(C));
            LOG_INFO("AutoTuner", "| Mode                 | %10s |", modeStr);
            LOG_INFO("AutoTuner", "| SWR                  | %10.2f |", finalSWR);
            LOG_INFO("AutoTuner", "| Return Loss          | %7.1f dB |", finalRL);
            LOG_INFO("AutoTuner", "+----------------------+------------+");
        }
    }
}
