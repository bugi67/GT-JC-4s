#include "PresetStore.h"
#include "../config.h"
#include "../logger/Logger.h"
#include <Wire.h>

int    PresetStore::s_count = 0;
Preset PresetStore::s_presets[PRESET_MAX_COUNT];

// ── Low-level EEPROM I/O ─────────────────────────────────────────────────────

bool PresetStore::eepromRead(uint8_t addr, uint8_t reg, uint8_t* buf, uint8_t len) {
    Wire.beginTransmission(addr);
    Wire.write(reg);
    if (Wire.endTransmission() != 0) return false;
    Wire.requestFrom(addr, len);
    for (uint8_t i = 0; i < len && Wire.available(); i++) buf[i] = Wire.read();
    return true;
}

bool PresetStore::eepromWrite(uint8_t addr, uint8_t reg, const uint8_t* buf, uint8_t len) {
    for (uint8_t i = 0; i < len; i++) {
        Wire.beginTransmission(addr);
        Wire.write((uint8_t)(reg + i));
        Wire.write(buf[i]);
        if (Wire.endTransmission() != 0) {
            LOG_ERROR("PresetStore", "EEPROM NACK at addr=%d", reg + i);
            return false;
        }
        // Acknowledge polling (busy-wait, no task yield → no Wire races).
        // PCF8582C NACKs its device address during the ~5 ms write cycle.
        bool ready = false;
        for (int t = 0; t < 40 && !ready; t++) {
            delayMicroseconds(500);
            Wire.beginTransmission(addr);
            ready = (Wire.endTransmission() == 0);
        }
        if (!ready) {
            LOG_ERROR("PresetStore", "EEPROM write timeout at addr=%d", reg + i);
            return false;
        }
    }
    return true;
}

// ── Pack / unpack ────────────────────────────────────────────────────────────

void PresetStore::packPreset(const Preset& p, uint8_t* buf) {
    buf[0] = (p.freq_kHz >> 8) & 0xFF;
    buf[1] = p.freq_kHz & 0xFF;
    buf[2] = (p.L >> 8) & 0xFF;
    buf[3] = p.L & 0xFF;
    uint16_t c_mode = ((uint16_t)(p.C & 0x1FF) << 7) | (p.mode & 0x03);
    buf[4] = (c_mode >> 8) & 0xFF;
    buf[5] = c_mode & 0xFF;
}

void PresetStore::unpackPreset(const uint8_t* buf, Preset& p) {
    p.freq_kHz = ((uint16_t)buf[0] << 8) | buf[1];
    p.L        = ((uint16_t)buf[2] << 8) | buf[3];
    uint16_t c_mode = ((uint16_t)buf[4] << 8) | buf[5];
    p.C    = (c_mode >> 7) & 0x1FF;
    p.mode = c_mode & 0x03;
    if (p.mode == 0) p.mode = 1;
}

// ── Public API ───────────────────────────────────────────────────────────────

bool PresetStore::begin() {
    s_count = 0;
    memset(s_presets, 0, sizeof(s_presets));
    uint8_t buf[PRESET_RECORD_SIZE];
    for (int i = 0; i < PRESET_MAX_COUNT; i++) {
        if (!eepromRead(ADDR_EEPROM_PRESET, (uint8_t)(i * PRESET_RECORD_SIZE), buf, PRESET_RECORD_SIZE)) {
            LOG_ERROR("PresetStore", "EEPROM read failed at slot %d", i);
            return false;
        }
        uint16_t f = ((uint16_t)buf[0] << 8) | buf[1];
        LOG_DEBUG("PresetStore", "slot %d: raw f=0x%04X", i, f);
        if (f == 0xFFFF) break;
        if (f == 0x0000 && buf[2] == 0 && buf[3] == 0 && buf[4] == 0 && buf[5] == 0) {
            LOG_WARN("PresetStore", "Slot %d all-zero, treating as end-of-list", i);
            break;
        }
        unpackPreset(buf, s_presets[s_count]);
        s_count++;
    }
    LOG_INFO("PresetStore", "%d presets loaded", s_count);
    return true;
}

int PresetStore::count() { return s_count; }

// read() and findBest() serve from the in-memory cache — no Wire call.
// This makes them safe to call from any task without a mutex.

bool PresetStore::read(int index, Preset& out) {
    if (index < 0 || index >= s_count) return false;
    out = s_presets[index];
    return true;
}

bool PresetStore::findBest(uint16_t freq_kHz, Preset& out) {
    int bestIdx = -1;
    uint16_t bestDelta = 0xFFFF;
    for (int i = 0; i < s_count; i++) {
        uint16_t delta = (uint16_t)abs((int)s_presets[i].freq_kHz - (int)freq_kHz);
        if (delta < bestDelta) { bestDelta = delta; bestIdx = i; }
    }
    if (bestIdx < 0) return false;
    out = s_presets[bestIdx];
    return true;
}

bool PresetStore::save(const Preset& p) {
    if (s_count >= PRESET_MAX_COUNT) return false;

    int insertAt = s_count;
    for (int i = 0; i < s_count; i++) {
        if (s_presets[i].freq_kHz == p.freq_kHz) {
            insertAt = i;
            s_count--;   // will be re-incremented below (overwrite, net count stays same)
            break;
        }
        if (s_presets[i].freq_kHz > p.freq_kHz) {
            insertAt = i;
            break;
        }
    }

    // Shift right in both EEPROM and cache
    uint8_t buf[PRESET_RECORD_SIZE];
    for (int i = s_count; i > insertAt; i--) {
        s_presets[i] = s_presets[i - 1];
        packPreset(s_presets[i], buf);
        if (!eepromWrite(ADDR_EEPROM_PRESET, (uint8_t)(i * PRESET_RECORD_SIZE), buf, PRESET_RECORD_SIZE))
            return false;
    }

    packPreset(p, buf);
    if (!eepromWrite(ADDR_EEPROM_PRESET, (uint8_t)(insertAt * PRESET_RECORD_SIZE), buf, PRESET_RECORD_SIZE)) {
        LOG_ERROR("PresetStore", "EEPROM write failed at slot %d", insertAt);
        return false;
    }
    s_presets[insertAt] = p;
    s_count++;
    LOG_INFO("PresetStore", "Saved preset freq=%u L=%u C=%u mode=%u", p.freq_kHz, p.L, p.C, p.mode);
    return true;
}

bool PresetStore::deleteByFreq(uint16_t freq_kHz) {
    int deleteIdx = -1;
    for (int i = 0; i < s_count; i++) {
        if (s_presets[i].freq_kHz == freq_kHz) { deleteIdx = i; break; }
    }
    if (deleteIdx < 0) return false;

    uint8_t buf[PRESET_RECORD_SIZE];
    for (int i = deleteIdx; i < s_count - 1; i++) {
        s_presets[i] = s_presets[i + 1];
        packPreset(s_presets[i], buf);
        eepromWrite(ADDR_EEPROM_PRESET, (uint8_t)(i * PRESET_RECORD_SIZE), buf, PRESET_RECORD_SIZE);
    }
    // Mark vacated last slot with 0xFF sentinel
    memset(buf, 0xFF, sizeof(buf));
    eepromWrite(ADDR_EEPROM_PRESET, (uint8_t)((s_count - 1) * PRESET_RECORD_SIZE), buf, PRESET_RECORD_SIZE);
    memset(&s_presets[s_count - 1], 0, sizeof(Preset));
    s_count--;
    return true;
}

bool PresetStore::deleteAll() {
    uint8_t buf[PRESET_RECORD_SIZE];
    memset(buf, 0xFF, sizeof(buf));
    for (int i = 0; i < PRESET_MAX_COUNT; i++) {
        eepromWrite(ADDR_EEPROM_PRESET, (uint8_t)(i * PRESET_RECORD_SIZE), buf, PRESET_RECORD_SIZE);
    }
    s_count = 0;
    memset(s_presets, 0, sizeof(s_presets));
    LOG_INFO("PresetStore", "All presets deleted");
    return true;
}
