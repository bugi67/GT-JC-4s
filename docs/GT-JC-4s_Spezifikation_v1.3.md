# GT-JC-4s — Projektspezifikation v1.3
**Antennenkoppler-Steuerung mit AutoTuner**
Datum: 2026-06-28 | Autor: HB9CZF | Status: Implementiert / In Test

---

## 1. Projektübersicht

### 1.1 Ziel
Migration des bestehenden Arduino-IDE-Sketches `GT_JC_4s_ESP32C3_20260614.ino` auf PlatformIO mit Integration des AutoTuner-Algorithmus (ZL2APV), einer Web-GUI für Konfiguration und Wartung sowie OTA-Update-Funktion (lokal und via GitHub).

### 1.2 Hardware-Basis
| Komponente | Beschreibung |
|---|---|
| MCU | Seeed Studio XIAO ESP32-C3 |
| Architektur | RISC-V, Single-Core, 160 MHz |
| RAM | 400 KB SRAM |
| Flash | 4 MB |
| Level-Shifter | 3.3 V → 5 V (für I2C-Bus zum JC-4s) |
| Tuner | Stockcomer JC-4s (Original-Hardware bleibt unverändert) |

### 1.3 I2C-Geräte (JC-4s intern)
| Adresse | Chip | Funktion |
|---|---|---|
| 0x38 | PCF8574AP | C-Relais Bit 0–7 (6 pF – 800 pF, direkt auf P0–P7) |
| 0x39 | PCF8574AP | C-Bit 8 (1600 pF, P0) + K1 Modus (P1) + K9/K10 Modus (P2) + **K-Tune (P3)** |
| 0x3A | PCF8574AP | L-Relais L1–L8 (0.078–10 µH, P0–P7) |
| 0x3B | PCF8574AP | L0 → P3 (0.039 µH), L9 → P0 (20 µH), L10 → P1 (40 µH); P2 = Eingang (Ant A, nie treiben!) |
| 0x48 | PCF8591 | ADC: AN1 = Vfwd, AN0 = Vrev (Hardware-Kanäle vertauscht) |
| 0x50 | PCF8582C | EEPROM 256 Byte (Band-Presets) |
| 0x51 | PCF8582C | EEPROM 256 Byte (reserviert) |

---

## 2. Systemarchitektur

### 2.1 Software-Stack
```
PlatformIO (Arduino-Framework für ESP32-C3)
  └─ FreeRTOS (transparent, via ESP-IDF unter dem Arduino-Layer)
       ├─ Task: WiFi + Web-Server (Core 0, 8 KB Stack)
       ├─ Task: MQTT-Client (Core 0, 4 KB Stack)
       ├─ Task: AutoTuner (Core 0, 6 KB Stack, niedriger Prio)
       ├─ Task: I2C-Controller (Core 0, 4 KB Stack, höchste Prio)
       └─ Task: Serielle Konsole (Core 0, 2 KB Stack, niedrigste Prio)
```

> **Hinweis zum ESP32-C3:** Das Modul hat nur **einen CPU-Core** (RISC-V). FreeRTOS arbeitet daher kooperativ/preemptiv auf diesem einen Core. Der I2C-Bus wird ausschliesslich vom `taskI2C` bedient — kein direkter Bus-Zugriff aus anderen Tasks.

### 2.2 Steuerungspfade
```
Node-RED  ──MQTT──►  ESP32-C3  ──I2C──►  PCF8574 × 4  ──►  Relais L/C/K-Tune
Web-GUI   ──HTTP──►  ESP32-C3            PCF8591 ADC   ◄──  SWR-Brücke
                         ▲
                    AutoTuner
                    (interner Loop)
```

---

## 3. Funktionale Anforderungen

### 3.1 Manuelle Steuerung

#### MQTT-Steuerung
| Topic | Richtung | Beschreibung |
|---|---|---|
| `JC-4s/L` | ◄ SUB | L-Wert 0–2047 |
| `JC-4s/C` | ◄ SUB | C-Wert 0–511 |
| `JC-4s/tunermode` | ◄ SUB | 1=C@TRX, 2=C@ANT, 3=kein C |
| `JC-4s/freq` | ◄ SUB | Frequenz in kHz |
| `JC-4s/tune` | ◄ SUB | `1` = AutoTune starten, `0` = Abbrechen |
| `JC-4s/ktune` | ◄ SUB | `1` = K-Tune-Relais EIN, `0` = AUS |
| `JC-4s/feedback/L` | ► PUB | Bestätigung L (nach I2C-Ausführung) |
| `JC-4s/feedback/C` | ► PUB | Bestätigung C |
| `JC-4s/feedback/tunermode` | ► PUB | Bestätigung Modus |
| `JC-4s/feedback/ktune` | ► PUB | Bestätigung K-Tune-Zustand (0/1) |
| `JC-4s/L_uH` | ► PUB | Berechneter L-Wert in µH |
| `JC-4s/C_pF` | ► PUB | Berechneter C-Wert in pF |
| `JC-4s/swr` | ► PUB | Aktuelles SWR |
| `JC-4s/rssi` | ► PUB | WiFi-Signalstärke (alle 10 s) |
| `JC-4s/id` | ► PUB | Firmware-Version |

**MQTT-Feedback-Logik:** Da die I2C-Ausführung asynchron via Queue läuft, erkennt `taskMQTT` Zustandsänderungen von L/C/mode/kTune in g_state und publiziert dann `publishStatus()`. Ebenso bei Tune-Status-Wechsel.

#### Web-GUI Steuerung
- Slider für L (0–2047) und C (0–511)
- Buttons für Tuner-Modi (C@TRX / C@ANT / kein C)
- Button „AutoTune starten / Abbrechen"
- Button „K-Tune" — Toggle (ghost = AUS, blau = EIN)
- SWR-Anzeige live via SSE (zeigt „—" bei keinem TX-Signal, Zahlenwert bei aktivem TX)
- Frequenzanzeige (aktuell via MQTT empfangen)

### 3.2 AutoTuner-Integration

#### Algorithmus (adaptiert von ZL2APV)

**Suchraumparameter:**
- L: 2048 Stufen (11 Bit), Coarse-Schritt: 64
- C: 512 Stufen (9 Bit), Coarse-Schritt: 16
- Tuner-Modi: alle drei werden im Coarse-Scan berücksichtigt

**Phase 1 — Preset-Suche**
1. EEPROM (I2C 0x50) nach gespeicherten Band-Presets durchsuchen
2. Bestes Preset als Startpunkt verwenden, SWR messen
3. Wenn Return Loss ≥ Schwellwert (default 18 dB ≈ SWR 1.29): fertig

**Phase 2 — Coarse-Scan**
- Alle C×L-Kombinationen in konfigurierbaren Schritten
- Alle drei Tuner-Modi → bestes Ergebnis behalten
- Abbruch via MQTT `JC-4s/tune = 0` oder Web-GUI

**Phase 3 — Fine-Step (Sliding Window)**
- Fenstergrösse: 9 Punkte (±4) um Coarse-Optimum
- Getrennte L/C-Optimierung abwechselnd, max. 5 Iterationen

**SWR-Messung (PCF8591)**
```
Vfwd = PCF8591 read(AN1)          // 8-Bit ADC, 0–255  (Hardware: AN1=Vfwd)
Vrev = PCF8591 read(AN0)          //                   (Hardware: AN0=Vrev)
rho  = Vrev / Vfwd
Return Loss [dB] = -20 × log10(rho)
SWR = (1 + rho) / (1 - rho)
```
- 8 Messungen, Ausreisser ±5% durch Maximalwert ersetzen
- Mindest-TX-Level: `g_cfg.tune_tx_level` (runtime-konfigurierbar, default 3)
- Kein TX-Signal (Vfwd < tune_tx_level): swr = 0.0, returnLoss = 0.0 → Web-GUI zeigt „—"
- Hintergrundmessung: `taskI2C` misst alle ~250 ms bei leerem Befehlspuffer (ohne Semaphore-Blocker)

**Preset-Speicherung (I2C-EEPROM 0x50)**
```
Struktur je Preset (6 Byte):
  uint16_t freq;      // Frequenz in kHz
  uint16_t L;         // L-Wert 0–2047
  uint16_t C_mode;    // Bits 15..9: C-Wert 0–511, Bits 1..0: Tuner-Modus
```
- Max. ~42 Presets (256 Byte EEPROM)
- Sortierung nach Frequenz

#### AutoTune-Status via MQTT
| Topic | Wert | Beschreibung |
|---|---|---|
| `JC-4s/tune/status` | `idle` / `tuning` / `done` / `aborted` | Zustand |
| `JC-4s/tune/progress` | 0–100 | Fortschritt Coarse-Scan in % |
| `JC-4s/swr` | float | SWR nach abgeschlossenem Tune |

---

## 4. Relay-Bit-Mapping

### 4.1 C-Relais (0x38 / 0x39)
C-Bit N wird direkt auf Port P_N von 0x38 gelegt (Bits 0–7), Bit 8 auf P0 von 0x39.

| C-Bit | Port | Relay | Kapazität |
|---|---|---|---|
| 0 | 0x38.P0 | K13+K14 | 6 pF |
| 1 | 0x38.P1 | K11+K12 | 12 pF |
| 2 | 0x38.P2 | K8 | 25 pF |
| 3 | 0x38.P3 | K7 | 50 pF |
| 4 | 0x38.P4 | K6 | 100 pF |
| 5 | 0x38.P5 | K5 | 200 pF |
| 6 | 0x38.P6 | K4 | 400 pF |
| 7 | 0x38.P7 | K3 | 800 pF |
| 8 | 0x39.P0 | K2 | 1600 pF |

**Steuer-Bits in 0x39:**
| Bit | Relay | Aktiv bei |
|---|---|---|
| P1 | K1 (C left) | mode == 1 (C@TRX) |
| P2 | K9+K10 (C right) | mode ≠ 2 (invertiert: aktiv wenn NICHT C@ANT) |
| P3 | K-Tune | `g_state.kTune == true` |

> **Wichtig K-Tune:** Der `SET_KTUNE`-Befehl schreibt **ausschliesslich 0x39** neu (re-derived aus g_state.C, g_state.mode, kTune). Die L-Chips 0x3A und 0x3B sowie C-LO 0x38 werden nicht angefasst. So bleiben alle L/C-Relais unverändert.

### 4.2 L-Relais (0x3A / 0x3B)
```cpp
// 0x3A: L-Bits 1–7 → P0–P6; L-Bit 8 (10 µH, K22) → P7
l3A = ((L >> 1) & 0x7F) | (((L >> 8) & 0x01) << 7);

// 0x3B: L9→P0, L10→P1, P2=Eingang (nie 0!), L0→P3
l3B = ((L >> 9) & 0x01) | (((L >> 10) & 0x01) << 1) | 0x04 | ((L & 0x01) << 3);
```

| L-Bit | Port | Relay | Induktivität |
|---|---|---|---|
| 0 (L0) | 0x3B.P3 | K15A | 0.039 µH |
| 1 (L1) | 0x3A.P0 | K15 | 0.078 µH |
| 2 (L2) | 0x3A.P1 | K16 | 0.156 µH |
| 3 (L3) | 0x3A.P2 | K17 | 0.312 µH |
| 4 (L4) | 0x3A.P3 | K18 | 0.624 µH |
| 5 (L5) | 0x3A.P4 | K19 | 1.25 µH |
| 6 (L6) | 0x3A.P5 | K20 | 2.5 µH |
| 7 (L7) | 0x3A.P6 | K21 | 5 µH |
| 8 (L8) | 0x3A.P7 | K22 | 10 µH |
| 9 (L9) | 0x3B.P0 | K23+K24 | 20 µH |
| 10 (L10) | 0x3B.P1 | K25+K26+K27 | 40 µH |
| — | 0x3B.P2 | — | Eingang „Memory Dis / Ant A" — immer High! |

---

## 5. Web-GUI

### 5.1 Struktur (Single-Page, ausgeliefert vom ESP32-C3)
HTML/CSS/JS liegen als separate Dateien in LittleFS (`data/`). Kein externes CDN.

**Layout:** Drei-Spalten-Design (angelehnt an Shelly): 72 px Sidebar + Hauptbereich + 264 px Echtzeit-Statsleiste.

**Seiten:**
1. **Dashboard** — L/C-Slider, Tuner-Modi, AutoTune-Button, **K-Tune-Button**, SWR-Live-Anzeige
2. **Presets** — Tabelle aller EEPROM-Presets, Löschen-Button je Preset
3. **Settings** — WLAN, MQTT, Tune-Parameter, OTA-URL, Log-Level
4. **Maintenance** — OTA Update (lokal + GitHub), Systemneustart

**Sprache:** Englisch (Standard) / Deutsch umschaltbar, `localStorage`-persistent.  
**Theme:** Dark Mode (Standard) / Hell umschaltbar, `localStorage`-persistent.

### 5.2 Technologie
- Vanilla HTML5 + CSS3 + JavaScript (kein Framework)
- Kommunikation: REST-API (JSON) + Server-Sent Events (SSE) für Live-Daten
- SSE: nicht-blockierend implementiert (Client in `s_sseClient` gespeichert, Push aus `taskWebServer`-Loop)
- SSE-Push: bei Zustandsänderung **sofort**, spätestens aber alle **2 Sekunden** (periodic push)
- Polling-Fallback: alle **5 s** `/api/status`
- SSE-Reconnect nach Verbindungsabbruch: **2 s**

### 5.3 REST-API (HTTP/1.1)
| Methode | Pfad | Beschreibung |
|---|---|---|
| GET | `/api/status` | Status: L, C, mode, **kTune**, SWR, returnLoss, vfwd, vrev, freq, rssi, tuneState, otaState, … |
| POST | `/api/tune` | L, C, mode setzen |
| POST | `/api/autotune` | AutoTune starten / stoppen |
| POST | `/api/ktune` | K-Tune-Relais: `{"ktune": true/false}` |
| GET | `/api/presets` | Alle Presets aus EEPROM |
| DELETE | `/api/presets/{freq}` | Preset löschen |
| DELETE | `/api/presets` | Alle Presets löschen |
| GET | `/api/config` | Aktuelle Konfiguration |
| POST | `/api/config` | Konfiguration speichern |
| POST | `/api/reboot` | Gerät neu starten |
| GET | `/events` | SSE-Stream (SWR, L, C, mode, **kTune**, freq, tuneState, otaState, …) |
| POST | `/ota/local/fw` | FW-Upload lokal (multipart, Header `X-MD5`) |
| POST | `/ota/local/fs` | FS-Upload lokal (multipart, Header `X-MD5`) |
| GET | `/ota/github/check` | Manifest abrufen, Version vergleichen |
| POST | `/ota/github/install` | GitHub-OTA starten (FW + FS sequenziell) |

> **Diagnose:** `/api/status` enthält `vfwd` und `vrev` (Rohwerte PCF8591, 0–255; Hardware: AN1=Vfwd, AN0=Vrev). Damit kann die SWR-Brücken-Aussteuerung ohne Serial Monitor überprüft werden.

### 5.4 OTA Update

FW und FS werden gemeinsam aktualisiert (FW zuerst, dann FS, jeweils mit MD5-Verifikation).

#### GitHub Release-Struktur
```
gt-jc-4s @ v1.x.x
├── firmware.bin / firmware.bin.md5
├── littlefs.bin / littlefs.bin.md5
└── release.json
```

**`release.json`:**
```json
{
  "version": "1.3.0",
  "fw":  { "url": "…/firmware.bin", "md5": "…", "size": 524288 },
  "fs":  { "url": "…/littlefs.bin", "md5": "…", "size": 1048576 },
  "changelog": "…"
}
```

#### Partitionstabelle (`custom_partitions.csv`)
```
nvs,      data, nvs,      0x9000,   0x5000    # NVS (WiFi-Credentials, Preferences)
otadata,  data, ota,      0xe000,   0x2000
app0,     app,  ota_0,    0x10000,  0x180000  # 1.5 MB FW-Slot A
app1,     app,  ota_1,    0x190000, 0x180000  # 1.5 MB FW-Slot B
spiffs,   data, spiffs,   0x310000, 0xF0000   # 960 KB LittleFS
```

> **Wichtig:** Der NVS-Bereich liegt auf einer eigenen Partition und wird durch `uploadfs` **nicht** überschrieben. WiFi-Credentials bleiben nach LittleFS-Flashes erhalten.

---

## 6. Datenhaltung

### 6.1 RAM-Datastore (zentraler Systemzustand)

```cpp
struct TunerState {
    uint16_t L, C;           // 0–2047 / 0–511
    uint8_t  mode;           // 1=C@TRX  2=C@ANT  3=kein C
    bool     kTune;          // K-Tune-Relais (0x39.P3)
    uint16_t freq_kHz;

    float    swr;            // 0.0 = kein TX-Signal (UI: "—")
    float    returnLoss;     // dB
    uint8_t  vfwd, vrev;     // Rohwerte PCF8591 AN0/AN1

    TuneState  tuneState;    // IDLE/TUNING/DONE/ABORTED
    uint8_t    tuneProgress; // 0–100 %
    OtaState   otaState;
    uint8_t    otaProgress;
    char       otaError[48];

    int8_t   rssi;
    char     fwVersion[12];
};

SemaphoreHandle_t g_stateMutex;
SemaphoreHandle_t g_tuneStartSem, g_tuneAbortSem;
QueueHandle_t     g_i2cCmdQueue;   // Tiefe 8
```

### 6.2 Persistente Parameter

#### LittleFS `/config.json`
Zugriff über `AppConfig g_cfg` (`src/cfg/AppConfig.h`), serialisiert mit ArduinoJson.

| Parameter | Typ | Default | Beschreibung |
|---|---|---|---|
| `wifi_ssid` | string | — | WLAN SSID (Backup; primär NVS) |
| `wifi_pass` | string | — | WLAN Passwort (Backup; primär NVS) |
| `mqtt_server` | string | — | MQTT Broker IP/Host |
| `mqtt_port` | uint16 | 1883 | MQTT Port |
| `mqtt_enabled` | bool | true | MQTT aktiv |
| `tune_threshold` | float | 18.0 | Min. Return Loss [dB] für Tune-Erfolg |
| `tune_tx_level` | uint8 | **3** | Min. Vfwd für TX-Erkennung (ADC-Counts 0–255) |
| `coarse_step_l` | uint16 | 64 | L-Schrittweite Coarse-Scan |
| `coarse_step_c` | uint16 | 16 | C-Schrittweite Coarse-Scan |
| `ota_manifest_url` | string | (GitHub latest) | Manifest-URL |
| `log_level` | uint8 | 2 | Log-Level (0=ERROR … 3=DEBUG) |
| `ntp_server` | string | ntp.metas.ch | NTP-Server |

#### ESP32 NVS (Preferences, Namespace „wifi")
WiFi-Credentials werden **zusätzlich** im NVS gespeichert. Dieser liegt auf einer eigenen Flash-Partition und überlebt LittleFS-Flashes.

| Key | Typ | Beschreibung |
|---|---|---|
| `ssid` | String | WLAN SSID |
| `pass` | String | WLAN Passwort |

**Ladestrategie in `WiFiManager::begin()`:**
1. NVS lesen → falls `ssid` nicht leer: in `g_cfg` übernehmen
2. Fallback auf `g_cfg.wifi_ssid` (aus config.json)
3. Falls leer: Captive Portal starten

### 6.3 Captive Portal (WiFi-Erstkonfiguration)
Bei fehlendem SSID startet `WiFiManager::runCaptivePortal()`:
- SoftAP `GT-JC-4s-Setup` + `DNSServer` (alle DNS → AP-IP)
- HTML-Formular auf Port 80 (SSID + Passwort)
- Nach Speichern: Credentials in **NVS** + `Config::save()` → Neustart
- Funktion kehrt nie zurück

> **Hinweis:** Nach jedem `uploadfs` sind nur die config.json-Credentials weg. Wenn die Credentials vorher via Captive Portal im NVS gespeichert wurden, verbindet sich das Gerät trotzdem automatisch.

---

## 7. Logging-System

### 7.1 Log-Levels
| Level | Wert | Default |
|---|---|---|
| `ERROR` | 0 | |
| `WARN` | 1 | |
| `INFO` | 2 | ✅ |
| `DEBUG` | 3 | |

### 7.2 Ausgabeformat
```
[  1234ms] [INFO ] [AutoTuner] Coarse-Scan gestartet
[  1891ms] [DEBUG] [I2C     ] PCF8591 AN0=142 AN1=12 SWR=1.17
```

### 7.3 Laufzeit-Steuerung
- Serielle Konsole: `log debug` / `log info` / `log warn` / `log error` / `status` / `reboot`
- **Periodische Statusausgabe:** `taskSerial` gibt alle **2 Sekunden** automatisch eine Statuszeile aus:
  ```
  [STATUS] L=0 C=0 mode=1 kTune=0 freq=0 SWR=0.00 RL=0.0dB vfwd=0 vrev=0 tune=0 rssi=-62
  ```
- Web-GUI Settings: Dropdown → sofort wirksam, in config.json gespeichert

---

## 8. FreeRTOS-Tasklayout

| Task | Prio | Stack | Funktion |
|---|---|---|---|
| `taskI2C` | 5 | 4096 | Alleiniger I2C-Bus-Master; Hintergrund-SWR alle 250 ms |
| `taskMQTT` | 4 | 4096 | MQTT-Reconnect, Publish/Subscribe, Feedback-Erkennung |
| `taskWebServer` | 3 | 8192 | HTTP, REST-API, SSE (nicht-blockierend), OTA |
| `taskAutoTuner` | 1 | 6144 | Tune-Algorithmus (wartet auf Semaphore) |
| `taskSerial` | 0 | 2048 | Serielle Konsole, Kommando-Parser, periodische Statusausgabe |

**I2C-Queue-Befehle:**
| Befehl | Sender | Empfänger | Beschreibung |
|---|---|---|---|
| `SET_LC` | Web, MQTT, AutoTuner | taskI2C | Schreibt alle vier PCF8574 (L + C + mode + kTune) |
| `READ_SWR` | AutoTuner | taskI2C | SWR-Messung on-demand |
| `SET_KTUNE` | Web, MQTT | taskI2C | Schreibt **nur 0x39** (C-Bit 8 + mode-Bits + K-Tune-Bit); 0x38/0x3A/0x3B bleiben unverändert |

Bei leerem Queue-Puffer (250 ms Timeout) misst taskI2C automatisch SWR im Hintergrund.

---

## 9. PlatformIO-Projektkonfiguration

### 9.1 `platformio.ini`
```ini
[env:seeed_xiao_esp32c3]
platform  = espressif32
board     = seeed_xiao_esp32c3
framework = arduino

monitor_speed = 115200
upload_port   = COM8

lib_deps =
    adafruit/Adafruit PCF8574 @ ^1.1.2
    knolleary/PubSubClient @ ^2.8
    bblanchon/ArduinoJson @ ^7.4

board_build.partitions = custom_partitions.csv
board_build.filesystem = littlefs

build_flags =
    -D FIRMWARE_VERSION=\"1.3.0\"
    -D LOG_LEVEL_DEFAULT=2
```

### 9.2 Verzeichnisstruktur
```
GT-JC-4s/
├── platformio.ini
├── custom_partitions.csv
├── .github/workflows/release.yml
├── docs/
│   └── GT-JC-4s_Spezifikation_v1.3.md
├── src/
│   ├── main.cpp
│   ├── config.h          # Konstanten, C_PF[], L_UH[], Defines, MQTT-Topics
│   ├── state.h           # TunerState (inkl. kTune) + I2CCommand + Mutex/Queue
│   ├── logger/Logger.h/cpp
│   ├── tuner/
│   │   ├── I2CController.h/cpp   # PCF8574/PCF8591-Treiber, Relay-Bit-Mapping, SET_KTUNE
│   │   ├── AutoTuner.h/cpp
│   │   └── PresetStore.h/cpp
│   ├── cfg/AppConfig.h/cpp       # config.json via ArduinoJson
│   ├── network/
│   │   ├── WiFiManager.h/cpp     # Station, Captive Portal, NVS-Credentials
│   │   ├── MQTTClient.h/cpp      # inkl. JC-4s/ktune Sub/Pub
│   │   └── WebServer.h/cpp       # REST-API, SSE (2s periodic), OTA
│   └── ota/OTAUpdater.h/cpp
└── data/                         # LittleFS-Inhalt
    ├── index.html
    ├── config.json               # Deployment-Defaults (ohne WiFi-Credentials)
    └── certs/github_root_ca.pem
```

---

## 10. Nicht-funktionale Anforderungen

| Anforderung | Zielwert |
|---|---|
| Web-GUI Ladezeit | < 2 s im lokalen WLAN |
| SSE-Update-Latenz | ≤ 2 s (periodic push, bei Änderung sofort) |
| AutoTune-Dauer (full scan) | < 60 s |
| AutoTune-Dauer (Preset-Hit) | < 5 s |
| MQTT-Reconnect-Intervall | 5 s |
| Bootzeit bis WiFi verbunden | < 10 s |
| RAM-Auslastung | ~14 % (45 KB von 320 KB) |
| Flash-Auslastung | ~68 % (1065 KB von 1536 KB) |

---

## 11. Offene Punkte / Risiken

| # | Punkt | Priorität |
|---|---|---|
| 1 | SWR-Brücke: PCF8591-Rohwerte (vfwd/vrev) bei 10 W TX noch nicht verifiziert — abrufbar via `/api/status` und Serial-Monitor. Falls vfwd < tune_tx_level (3): SWR-Brücken-Signal zu schwach, tune_tx_level per Web-GUI anpassen | Hoch |
| 2 | I2C-EEPROM 0x50: ~1 Mio. Schreibzyklen; Preset-Schreiben nur bei explizitem Nutzer-Befehl | Mittel |
| 3 | MD5 beim lokalen Upload: `crypto.subtle` nur in sicheren Kontexten — HTTP im LAN; Fallback: MD5 serverseitig nach Empfang | Mittel |
| 4 | `data/config.json` enthält keine WiFi-Credentials (leere Strings). Nach `uploadfs` verbindet sich das Gerät via NVS. Ersteinrichtung erfordert Captive Portal einmalig. | Info |

---

## 12. Status v1.3 — Implementiert

| # | Feature | Status |
|---|---|---|
| 1 | PlatformIO-Projekt, Partitionstabelle, CI/CD (GitHub Actions) | ✅ |
| 2 | I2CController: PCF8574 × 4, PCF8591, korrektes Relay-Bit-Mapping | ✅ |
| 3 | K9/K10 Relay invertiert (aktiv wenn mode ≠ 2) | ✅ |
| 4 | AutoTuner (Preset → Coarse → Fine) | ✅ |
| 5 | Web-GUI (Shelly-Layout, EN/DE, Dark/Light, SSE, REST) | ✅ |
| 6 | Captive Portal (WiFi-Erstkonfiguration) | ✅ |
| 7 | WiFi-Credentials in NVS (überlebt LittleFS-Flashes) | ✅ |
| 8 | Config in LittleFS `/config.json` via ArduinoJson | ✅ |
| 9 | MQTT-Client mit asynchronem Feedback-Publishing | ✅ |
| 10 | OTA lokal + GitHub mit MD5-Verifikation | ✅ |
| 11 | Hintergrund-SWR-Messung (taskI2C, alle 250 ms) | ✅ |
| 12 | vfwd/vrev in `/api/status` und Serial-Monitor (ADC-Diagnose) | ✅ |
| 13 | SWR-Anzeige: 0.0 = kein TX → Web-GUI „—" | ✅ |
| 14 | tune_tx_level runtime-konfigurierbar (default 3) | ✅ |
| 15 | K-Tune-Relais (0x39.P3): Web-GUI Button, REST `/api/ktune`, MQTT `JC-4s/ktune` | ✅ |
| 16 | SET_KTUNE schreibt nur 0x39 — L/C-Relais bleiben unverändert | ✅ |
| 17 | SSE periodic push alle 2 s (auch ohne Zustandsänderung) | ✅ |
| 18 | Polling-Fallback 5 s, SSE-Reconnect 2 s | ✅ |
| 19 | taskSerial: periodische Statusausgabe alle 2 s auf COM8 | ✅ |
