# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project

GT-JC-4s is ESP32-C3 (Seeed XIAO) firmware for an automatic HF antenna tuner. It controls L/C relay networks via I2C, measures SWR via an AD8591 ADC, and exposes a Web-UI, REST API, MQTT, and OTA update functionality. Built with PlatformIO + Arduino framework.

## Commands

```bash
# Build and flash firmware (COM6)
pio run --target upload

# Build and flash web assets (LittleFS)
pio run --target buildfs
pio run --target uploadfs

# Serial monitor (115200 baud, COM6)
pio run --target monitor

# Build only (no upload)
pio run
```

Releases are created by pushing a `v*.*.*` tag — GitHub Actions builds both `firmware.bin` and `littlefs.bin`, computes MD5s, and publishes a `release.json` manifest used by the OTA subsystem.

## Architecture

### Task model (FreeRTOS)

All work runs in dedicated FreeRTOS tasks; `loop()` only yields. Tasks and their priorities (higher = more urgent):

| Task | Priority | Role |
|---|---|---|
| `taskI2C` | 5 | Sole owner of I2C bus; drives relays and ADC |
| `taskMQTT` | 4 | MQTT reconnect loop, publish/subscribe |
| `taskWeb` | 3 | HTTP request handling (REST + SSE + OTA) |
| `taskAutoTuner` | 1 | Blocking tune algorithm (preset → coarse → fine) |
| `taskSerial` | 0 | Serial console commands |

### Shared state and synchronisation

`g_state` (`TunerState` in [src/state.h](src/state.h)) is the single source of truth for tuner position, SWR readings, tune/OTA progress, and system metadata. Always access it under `StateLock` (RAII mutex wrapper). Use the `stateGet<T>()` helper for simple field reads.

Inter-task signalling:
- `g_i2cCmdQueue` — any task sends `I2CCommand`; only `taskI2C` dequeues it. Never call I2C directly from non-I2C tasks.
- `g_tuneStartSem` — WebUI/MQTT give this semaphore to trigger AutoTuner.
- `g_tuneAbortSem` — used to cancel an in-progress tune.

### AutoTuner algorithm ([src/tuner/AutoTuner.cpp](src/tuner/AutoTuner.cpp))

Three sequential phases, each checking `g_tuneAbortSem` between steps:
1. **Preset search** — look up stored presets near the current frequency; apply and measure SWR.
2. **Coarse scan** — sweep L (`DEFAULT_COARSE_L` steps) × C (`DEFAULT_COARSE_C` steps) × 3 modes; find global minimum return loss.
3. **Fine tune** — ±4 steps around the coarse optimum (`FINE_WINDOW_SIZE = 9`), up to `FINE_MAX_ITER` iterations.

Success threshold: `DEFAULT_TUNE_THRESHOLD` (18 dB return loss ≈ SWR 1.29).

### I2C hardware mapping ([src/config.h](src/config.h))

Four PCF8574 expanders control the relay matrix:
- `0x38` / `0x39` — capacitor relays (C bits 0-8 + tuner mode switches)
- `0x3A` / `0x3B` — inductor relays (L bits 0-10)

SWR is measured from AD8591 ADC (`0x48`): AN0 = Vfwd, AN1 = Vrev. `measureSWR()` takes 8 samples with outlier rejection.

Presets are stored in an I2C EEPROM at `0x50` (6 bytes each, up to 42 entries sorted by frequency). Device config (WiFi, MQTT, tuner params) is stored in **LittleFS as `/config.json`** via `AppConfig g_cfg` ([src/cfg/AppConfig.h](src/cfg/AppConfig.h)). `Config::load()` / `Config::save()` handle serialisation with ArduinoJson; defaults are applied if the file is missing.

### WiFi / Captive Portal ([src/network/WiFiManager.cpp](src/network/WiFiManager.cpp))

`WiFiManager::begin()` attempts station-mode connection using credentials from `g_cfg`. If no credentials are stored or the connection fails, it returns `false` and `main.cpp` calls `WiFiManager::runCaptivePortal()`, which:
- Starts a SoftAP (`GT-JC-4s-Setup`) and a `DNSServer` redirecting all DNS queries to the AP IP.
- Serves a minimal HTML form on port 80 for SSID/password entry.
- On save, calls `saveCredentials()` and reboots — this function **never returns**.

### Logging

Macros `LOG_ERROR / LOG_WARN / LOG_INFO / LOG_DEBUG` are stripped at compile time below `LOG_LEVEL_DEFAULT` (set to `2` = INFO in `platformio.ini`). Runtime level can be changed without recompile via the serial console (`log debug`, `log info`, etc.) and is persisted to `/config.json`.

### Web assets (LittleFS)

The Web-UI lives in LittleFS (HTML/CSS/JS served as static files from the `data/` directory). After any change to web assets, run `buildfs` + `uploadfs` in addition to `upload`. The partition table (`custom_partitions.csv`) allocates 960 KB for LittleFS, with dual OTA app slots of 1.5 MB each.

The UI is Shelly-inspired: three-column layout (72 px sidebar + main + 264 px stats panel), dark mode by default (toggleable), English by default with German switchable. Language and theme are persisted in `localStorage`. Live updates arrive via SSE (`/events`); the 30 s polling fallback calls `/api/status`.
