#pragma once
#include <Arduino.h>

// Preset layout in EEPROM: 6 bytes each, up to 42 presets in 256 bytes
// Byte 0-1: freq (kHz, uint16, big-endian)
// Byte 2-3: L value (uint16, big-endian)
// Byte 4-5: C_mode – bits 15..7: C value (9 bits), bits 1..0: tuner mode (1-3)
struct Preset {
    uint16_t freq_kHz;
    uint16_t L;
    uint16_t C;
    uint8_t  mode;
};

#define PRESET_RECORD_SIZE  6
#define PRESET_MAX_COUNT    42   // floor(256 / 6)

class PresetStore {
public:
    static bool begin();

    static int  count();
    static bool read(int index, Preset& out);
    static bool findBest(uint16_t freq_kHz, Preset& out);

    // Save or update preset for given frequency (inserts sorted by freq)
    static bool save(const Preset& p);
    static bool deleteByFreq(uint16_t freq_kHz);
    static bool deleteAll();

private:
    static int  s_count;
    static bool eepromRead(uint8_t addr, uint8_t reg, uint8_t* buf, uint8_t len);
    static bool eepromWrite(uint8_t addr, uint8_t reg, const uint8_t* buf, uint8_t len);
    static void packPreset(const Preset& p, uint8_t* buf);
    static void unpackPreset(const uint8_t* buf, Preset& p);
};
