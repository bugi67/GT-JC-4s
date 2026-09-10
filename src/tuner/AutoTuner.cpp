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

// ── Coarse candidate tracking ────────────────────────────────────────────────

struct Candidate { uint16_t L, C; uint8_t mode; float RL; };

static Candidate s_cands[3];
static int       s_nCands;


// Add a point to the top-3 diverse candidate list.
// "Diverse" means each candidate must be at least 1 coarse step away from all
// others in BOTH L and C — so we explore different regions, not just refine one.
static void addCandidate(uint16_t L, uint16_t C, uint8_t mode, float rl) {
    for (int i = 0; i < s_nCands; i++) {
        bool nearL = (uint16_t)abs((int)L - (int)s_cands[i].L) < g_cfg.coarse_step_l;
        bool nearC = (uint16_t)abs((int)C - (int)s_cands[i].C) < g_cfg.coarse_step_c;
        if (nearL && nearC) {
            if (rl > s_cands[i].RL) s_cands[i] = {L, C, mode, rl};
            return;
        }
    }
    if (s_nCands < 3) { s_cands[s_nCands++] = {L, C, mode, rl}; return; }
    // Replace weakest if this is better
    int weak = 0;
    for (int i = 1; i < 3; i++) if (s_cands[i].RL < s_cands[weak].RL) weak = i;
    if (rl > s_cands[weak].RL) s_cands[weak] = {L, C, mode, rl};
}

// ── Phase 2: Coarse scan ─────────────────────────────────────────────────────

void AutoTuner::coarseScan(uint16_t& bestL, uint16_t& bestC, uint8_t& bestMode) {
    float bestRL = -999.0f;
    I2CCommand mCmd = {I2CCmd::READ_SWR, 0, 0, 0};
    s_nCands = 0;

    uint16_t lMax, cMax;
    getBandLimits(stateGet(&TunerState::freq_kHz), lMax, cMax);
    LOG_INFO("AutoTuner", "Coarse scan: L 0..%u step %u, C 0..%u step %u",
             lMax, g_cfg.coarse_step_l, cMax, g_cfg.coarse_step_c);

    uint16_t lSteps = (lMax / g_cfg.coarse_step_l) + 1;
    uint16_t cSteps = (cMax / g_cfg.coarse_step_c) + 1;
    uint32_t totalSteps = (uint32_t)lSteps * cSteps * 2;  // modes 1+2 only (no "No C")
    uint32_t step = 0;

    for (uint8_t m = 1; m <= 2; m++) {  // 1=C@TRX, 2=C@ANT; mode 3 (No C) skipped
        for (uint16_t c = 0; c <= cMax; c += g_cfg.coarse_step_c) {
            for (uint16_t l = 0; l <= lMax; l += g_cfg.coarse_step_l) {
                if (isAbortRequested()) {
                    LOG_INFO("AutoTuner", "Coarse scan aborted");
                    return;
                }
                setLCAndWait(l, c, m, 3);
                xQueueSend(g_i2cCmdQueue, &mCmd, portMAX_DELAY);
                vTaskDelay(pdMS_TO_TICKS(15));

                float rl = getRL();
                if (rl > bestRL) { bestRL = rl; bestL = l; bestC = c; bestMode = m; }
                addCandidate(l, c, m, rl);
                step++;
                reportProgress((uint8_t)(step * 70 / totalSteps));   // 0-70% for coarse
            }
        }
    }
    LOG_INFO("AutoTuner", "Coarse done: L=%u C=%u mode=%u RL=%.1fdB", bestL, bestC, bestMode, bestRL);
}

// ── Phase 2.25: Inter-L scan ─────────────────────────────────────────────────
// The coarse scan steps L at coarse_step_l (64). Optimal L values that fall in
// the gaps (e.g. L=15 between L=0 and L=64) never appear in the coarse top-3.
// This pass probes the intermediate L values — at coarse_step_l/4 spacing
// (L=16, 32, 48 for step=64) — against ALL C at the coarse C step.
// Cost: ~200 measurements (~7 s). The best result feeds an extra medium scan.

void AutoTuner::interLScan(uint16_t& bestL, uint16_t& bestC, uint8_t& bestMode) {
    uint16_t lMax, cMax;
    getBandLimits(stateGet(&TunerState::freq_kHz), lMax, cMax);
    // Intermediate step: coarse/4, minimum 8 to avoid overlap with fine-tune
    uint16_t stepL = max((uint16_t)8, (uint16_t)(g_cfg.coarse_step_l / 4));

    float bestRL = -999.0f;
    I2CCommand mCmd = {I2CCmd::READ_SWR, 0, 0, 0};

    for (uint8_t m = 1; m <= 2; m++) {
        for (uint16_t c = 0; c <= cMax; c += g_cfg.coarse_step_c) {
            // Only probe intermediate L values — L=0 and L=coarseStep already in coarse
            for (uint16_t l = stepL; l < g_cfg.coarse_step_l; l += stepL) {
                if (isAbortRequested()) return;
                setLCAndWait(l, c, m, 3);
                xQueueSend(g_i2cCmdQueue, &mCmd, portMAX_DELAY);
                vTaskDelay(pdMS_TO_TICKS(15));
                float rl = getRL();
                if (rl > bestRL) { bestRL = rl; bestL = l; bestC = c; bestMode = m; }
            }
        }
    }
    LOG_INFO("AutoTuner", "Inter-L scan best: L=%u C=%u mode=%u RL=%.1f dB", bestL, bestC, bestMode, bestRL);
}

// ── Phase 2.5: Medium scan ───────────────────────────────────────────────────
// Searches ±1 coarse step around the coarse optimum using step/8 granularity.
// Bridges the gap between coarse step (64) and fine window (±4): without this,
// the true optimum can be up to ±32 away — outside the fine tune reach.

void AutoTuner::mediumScan(uint16_t& bestL, uint16_t& bestC, uint8_t mode) {
    uint16_t medStepL = max((uint16_t)2, (uint16_t)(g_cfg.coarse_step_l / 8));
    uint16_t medStepC = max((uint16_t)1, (uint16_t)(g_cfg.coarse_step_c / 4));
    uint16_t lLo = (bestL >= g_cfg.coarse_step_l) ? bestL - g_cfg.coarse_step_l : 0;
    uint16_t lHi = min((int)L_MAX, (int)bestL + g_cfg.coarse_step_l);
    uint16_t cLo = (bestC >= g_cfg.coarse_step_c) ? bestC - g_cfg.coarse_step_c : 0;
    uint16_t cHi = min((int)C_MAX, (int)bestC + g_cfg.coarse_step_c);

    float bestRL = -999.0f;
    I2CCommand mCmd = {I2CCmd::READ_SWR, 0, 0, 0};

    for (uint16_t c = cLo; c <= cHi; c += medStepC) {
        for (uint16_t l = lLo; l <= lHi; l += medStepL) {
            if (isAbortRequested()) return;
            setLCAndWait(l, c, mode, 3);
            xQueueSend(g_i2cCmdQueue, &mCmd, portMAX_DELAY);
            vTaskDelay(pdMS_TO_TICKS(15));
            float rl = getRL();
            if (rl > bestRL) { bestRL = rl; bestL = l; bestC = c; }
        }
    }
    LOG_INFO("AutoTuner", "Medium done: L=%u C=%u mode=%u RL=%.1f dB", bestL, bestC, mode, bestRL);
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
            vTaskDelay(pdMS_TO_TICKS(15));
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
            vTaskDelay(pdMS_TO_TICKS(15));
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
        vTaskDelay(pdMS_TO_TICKS(20));
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

    // Phase 2
    coarseScan(bestL, bestC, bestMode);
    if (isAbortRequested()) return false;

    // Phase 2.25 — inter-L scan: fills coarse L gaps (e.g. L=16,32,48) at all C.
    // The best result is used as a 4th medium candidate alongside the top-3.
    uint16_t ilL = 0, ilC = 0; uint8_t ilMode = 1;
    interLScan(ilL, ilC, ilMode);
    if (isAbortRequested()) return false;

    // Phase 2.5 — medium scan from top-3 coarse candidates + inter-L best
    {
        float overallRL = -999.0f;

        // Top-3 from coarse scan
        for (int ci = 0; ci < s_nCands; ci++) {
            uint16_t tL = s_cands[ci].L, tC = s_cands[ci].C; uint8_t tMode = s_cands[ci].mode;
            LOG_INFO("AutoTuner", "Medium scan %d/%d from L=%u C=%u mode=%u", ci+1, s_nCands, tL, tC, tMode);
            mediumScan(tL, tC, tMode);
            if (isAbortRequested()) return false;
            setLCAndWait(tL, tC, tMode, 5);
            I2CCommand mCmd2 = {I2CCmd::READ_SWR, 0, 0, 0};
            xQueueSend(g_i2cCmdQueue, &mCmd2, portMAX_DELAY);
            vTaskDelay(pdMS_TO_TICKS(20));
            float rl = getRL();
            if (rl > overallRL) { overallRL = rl; bestL = tL; bestC = tC; bestMode = tMode; }
        }

        // Inter-L best — always explore unless already covered by top-3
        bool ilInCands = false;
        for (int ci = 0; ci < s_nCands; ci++) {
            uint16_t dL = (uint16_t)abs((int)ilL - (int)s_cands[ci].L);
            uint16_t dC = (uint16_t)abs((int)ilC - (int)s_cands[ci].C);
            if (dL < g_cfg.coarse_step_l && dC < g_cfg.coarse_step_c) { ilInCands = true; break; }
        }
        if (!ilInCands) {
            LOG_INFO("AutoTuner", "Medium scan (inter-L) from L=%u C=%u mode=%u", ilL, ilC, ilMode);
            mediumScan(ilL, ilC, ilMode);
            if (isAbortRequested()) return false;
            setLCAndWait(ilL, ilC, ilMode, 5);
            I2CCommand mCmd3 = {I2CCmd::READ_SWR, 0, 0, 0};
            xQueueSend(g_i2cCmdQueue, &mCmd3, portMAX_DELAY);
            vTaskDelay(pdMS_TO_TICKS(20));
            float rl = getRL();
            if (rl > overallRL) { overallRL = rl; bestL = ilL; bestC = ilC; bestMode = ilMode; }
            LOG_INFO("AutoTuner", "Medium inter-L result: L=%u C=%u mode=%u RL=%.1f dB", ilL, ilC, ilMode, rl);
        }

        LOG_INFO("AutoTuner", "Medium best: L=%u C=%u mode=%u RL=%.1f dB", bestL, bestC, bestMode, overallRL);
    }
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
