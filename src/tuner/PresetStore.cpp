#include "PresetStore.h"
#include "../config.h"
#include "../logger/Logger.h"
#include <Wire.h>

int PresetStore::s_count = 0;

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
        if (Wire.endTransmission() != 0) return false;
        delay(5);   // PCF8582C write cycle time
    }
    return true;
}

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
    p.C   = (c_mode >> 7) & 0x1FF;
    p.mode = c_mode & 0x03;
    if (p.mode == 0) p.mode = 1;   // default
}

bool PresetStore::begin() {
    // Count valid presets (non-0xFFFF freq means written)
    s_count = 0;
    uint8_t buf[PRESET_RECORD_SIZE];
    for (int i = 0; i < PRESET_MAX_COUNT; i++) {
        if (!eepromRead(ADDR_EEPROM_PRESET, (uint8_t)(i * PRESET_RECORD_SIZE), buf, PRESET_RECORD_SIZE))
            return false;
        uint16_t f = ((uint16_t)buf[0] << 8) | buf[1];
        if (f == 0xFFFF) break;
        s_count++;
    }
    LOG_INFO("PresetStore", "%d presets loaded", s_count);
    return true;
}

int PresetStore::count() { return s_count; }

bool PresetStore::read(int index, Preset& out) {
    if (index < 0 || index >= s_count) return false;
    uint8_t buf[PRESET_RECORD_SIZE];
    if (!eepromRead(ADDR_EEPROM_PRESET, (uint8_t)(index * PRESET_RECORD_SIZE), buf, PRESET_RECORD_SIZE))
        return false;
    unpackPreset(buf, out);
    return true;
}

bool PresetStore::findBest(uint16_t freq_kHz, Preset& out) {
    int bestIdx = -1;
    uint16_t bestDelta = 0xFFFF;
    for (int i = 0; i < s_count; i++) {
        Preset p;
        if (!read(i, p)) continue;
        uint16_t delta = (uint16_t)abs((int)p.freq_kHz - (int)freq_kHz);
        if (delta < bestDelta) {
            bestDelta = delta;
            bestIdx = i;
        }
    }
    if (bestIdx < 0) return false;
    return read(bestIdx, out);
}

bool PresetStore::save(const Preset& p) {
    if (s_count >= PRESET_MAX_COUNT) return false;

    // Find insertion point (keep sorted by freq)
    int insertAt = s_count;
    for (int i = 0; i < s_count; i++) {
        Preset existing;
        if (!read(i, existing)) return false;
        if (existing.freq_kHz == p.freq_kHz) {
            insertAt = i;   // overwrite
            s_count--;
            break;
        }
        if (existing.freq_kHz > p.freq_kHz) {
            insertAt = i;
            break;
        }
    }

    // Shift presets above insertion point right by one slot
    uint8_t buf[PRESET_RECORD_SIZE];
    for (int i = s_count; i > insertAt; i--) {
        eepromRead(ADDR_EEPROM_PRESET, (uint8_t)((i - 1) * PRESET_RECORD_SIZE), buf, PRESET_RECORD_SIZE);
        eepromWrite(ADDR_EEPROM_PRESET, (uint8_t)(i * PRESET_RECORD_SIZE), buf, PRESET_RECORD_SIZE);
    }

    packPreset(p, buf);
    if (!eepromWrite(ADDR_EEPROM_PRESET, (uint8_t)(insertAt * PRESET_RECORD_SIZE), buf, PRESET_RECORD_SIZE))
        return false;

    s_count++;
    LOG_INFO("PresetStore", "Saved preset freq=%u L=%u C=%u mode=%u", p.freq_kHz, p.L, p.C, p.mode);
    return true;
}

bool PresetStore::deleteByFreq(uint16_t freq_kHz) {
    int deleteIdx = -1;
    for (int i = 0; i < s_count; i++) {
        Preset p;
        if (read(i, p) && p.freq_kHz == freq_kHz) { deleteIdx = i; break; }
    }
    if (deleteIdx < 0) return false;

    uint8_t buf[PRESET_RECORD_SIZE];
    for (int i = deleteIdx; i < s_count - 1; i++) {
        eepromRead(ADDR_EEPROM_PRESET, (uint8_t)((i + 1) * PRESET_RECORD_SIZE), buf, PRESET_RECORD_SIZE);
        eepromWrite(ADDR_EEPROM_PRESET, (uint8_t)(i * PRESET_RECORD_SIZE), buf, PRESET_RECORD_SIZE);
    }
    // Mark last slot as empty
    memset(buf, 0xFF, sizeof(buf));
    eepromWrite(ADDR_EEPROM_PRESET, (uint8_t)((s_count - 1) * PRESET_RECORD_SIZE), buf, PRESET_RECORD_SIZE);
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
    LOG_INFO("PresetStore", "All presets deleted");
    return true;
}
