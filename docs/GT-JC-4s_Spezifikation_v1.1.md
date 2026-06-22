# GT-JC-4s — Projektspezifikation v1.1
**Antennenkoppler-Steuerung mit AutoTuner**
Datum: 2026-06-22 | Autor: HB9CZF | Status: Implementiert

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
| 0x38 | PCF8574AP | C-Relais Bit 0–7 (6 pF – 800 pF) |
| 0x39 | PCF8574AP | C-Relais Bit 8 (1600 pF) + Umschalter K1/K9+K10 |
| 0x3A | PCF8574AP | L-Relais Bit 1–7 (0.078 µH – 5 µH) |
| 0x3B | PCF8574AP | L-Relais Bit 0 (0.039 µH) + L8–L10 (10–40 µH) |
| 0x48 | PCF8591 | ADC: AN0 = Vfwd, AN1 = Vrev |
| 0x50 | PCF8582C | EEPROM 256 Byte (Band-Presets) |
| 0x51 | PCF8582C | EEPROM 256 Byte (Konfiguration) |

---

## 2. Systemarchitektur

### 2.1 Software-Stack
```
PlatformIO (Arduino-Framework für ESP32-C3)
  └─ FreeRTOS (transparent, via ESP-IDF unter dem Arduino-Layer)
       ├─ Task: WiFi + Web-Server (Core 0, 8 KB Stack)
       ├─ Task: MQTT-Client (Core 0, 4 KB Stack)
       ├─ Task: AutoTuner (Core 0, 6 KB Stack, niedriger Prio)
       └─ Task: Hauptsteuerung / I2C (Core 0, 4 KB Stack)
```

> **Hinweis zum ESP32-C3:** Das Modul hat nur **einen CPU-Core** (RISC-V). FreeRTOS arbeitet daher kooperativ/preemptiv auf diesem einen Core. Der Hauptvorteil ist die saubere Trennung von Web-Server-Stack, MQTT-Keepalive und dem blockierenden AutoTune-Vorgang. Der Entwickler muss sich **nicht** explizit mit FreeRTOS-API befassen — PlatformIO und das Arduino-Framework kapseln dies transparent.

### 2.2 Steuerungspfade
```
Node-RED  ──MQTT──►  ESP32-C3  ──I2C──►  PCF8574 × 4  ──►  Relais L/C
Web-GUI   ──HTTP──►  ESP32-C3            PCF8591 ADC   ◄──  SWR-Brücke
                         ▲
                    AutoTuner
                    (interner Loop)
```

---

## 3. Funktionale Anforderungen

### 3.1 Manuelle Steuerung (wie bisher, erweitert)

#### MQTT-Steuerung (beibehalten)
| Topic | Richtung | Beschreibung |
|---|---|---|
| `JC-4s/L` | ◄ SUB | L-Wert 0–2047 |
| `JC-4s/C` | ◄ SUB | C-Wert 0–511 |
| `JC-4s/tunermode` | ◄ SUB | 1=C@TRX, 2=C@ANT, 3=kein C |
| `JC-4s/freq` | ◄ SUB | Frequenz in kHz (neu) |
| `JC-4s/tune` | ◄ SUB | `1` = AutoTune starten |
| `JC-4s/feedback/L` | ► PUB | Bestätigung L |
| `JC-4s/feedback/C` | ► PUB | Bestätigung C |
| `JC-4s/feedback/tunermode` | ► PUB | Bestätigung Modus |
| `JC-4s/L_uH` | ► PUB | Berechneter L-Wert in µH |
| `JC-4s/C_pF` | ► PUB | Berechneter C-Wert in pF |
| `JC-4s/swr` | ► PUB | Aktuelles SWR (nach Tune) |
| `JC-4s/rssi` | ► PUB | WiFi-Signalstärke (10s) |
| `JC-4s/id` | ► PUB | Firmware-Version |

#### Web-GUI Steuerung (neu)
- Slider für L (0–2047) und C (0–511)
- Buttons für Tuner-Modi (C@TRX / C@ANT / kein C)
- Button „AutoTune starten"
- SWR-Anzeige (live, via WebSocket oder SSE)
- Frequenzanzeige (aktuell via MQTT empfangen)

### 3.2 AutoTuner-Integration

#### Algorithmus (adaptiert von ZL2APV)
Der Algorithmus wird auf die JC-4s-Hardware angepasst. Shift-Register, LCD und analoge SWR-Pins des Originals entfallen vollständig.

**Suchraumparameter:**
- L: 2048 Stufen (11 Bit), Coarse-Schritt: 64
- C: 512 Stufen (9 Bit), Coarse-Schritt: 16
- Tuner-Modi: alle drei (C@TRX, C@ANT, kein C) werden im Coarse-Scan berücksichtigt

**Phase 1 — Preset-Suche**
1. EEPROM (I2C 0x50) nach gespeicherten Band-Presets durchsuchen
2. Für jeden Preset: Relais setzen, SWR messen
3. Bestes Preset (höchste Return Loss) als Startpunkt verwenden
4. Wenn Return Loss > Schwellwert (konfigurierbar, default 18 dB ≈ SWR 1.29): direkt zu Phase 3

**Phase 2 — Coarse-Scan**
- Schleife über alle C-Werte in 16er-Schritten (0, 16, 32, … 512)
- Innere Schleife über alle L-Werte in 64er-Schritten (0, 64, 128, … 2048)
- SWR-Messung an jedem Punkt via PCF8591 (AN0/AN1)
- Ergebnis in 2D-Array → Bestimmung des Groboptimums
- Vorgang für alle drei Tuner-Modi → bestes Ergebnis wird behalten
- Bei Button-Press (via MQTT `JC-4s/tune` = `0` oder Web-GUI) → Abbruch

**Phase 3 — Fine-Step (Sliding Window)**
- Fenstergrösse: 9 Punkte (ARRAY_SIZE), zentriert um Coarse-Optimum
- Getrennte Optimierung für L und C (abwechselnd, bis kein Fortschritt)
- Volle Auflösung (Einzelschritte)
- Maximale Iterations: 5 pro L/C-Zyklus (Schutz gegen Endlosschleife)

**SWR-Messung (NEU — PCF8591)**
```
Vfwd = PCF8591 read(AN0)   // 8-bit ADC, 0–255
Vrev = PCF8591 read(AN1)   // 8-bit ADC, 0–255
rho = Vrev / Vfwd
Return Loss [dB] = -20 × log10(rho)
SWR = (1 + rho) / (1 - rho)
```
- 8 Messungen, Ausreisser ±5% werden durch Maximalwert ersetzt (wie Original)
- Mindest-Tx-Level: konfigurierbar (default: Vfwd > 10)

**Preset-Speicherung (I2C-EEPROM 0x50)**
```
Struktur je Preset (6 Byte):
  uint16_t freq;      // Frequenz in kHz
  uint16_t L;         // L-Wert 0–2047
  uint16_t C_mode;    // Bits 15..9: C-Wert 0–511, Bits 1..0: Tuner-Modus (1/2/3)
```
- Max. ~40 Presets (256 Byte EEPROM)
- Frequenz kommt via MQTT `JC-4s/freq`
- Sortierung nach Frequenz (wie ZL2APV-Original)
- Preset-Verwaltung via Web-GUI (anzeigen, löschen, alle löschen)

#### AutoTune-Status via MQTT
| Topic | Wert | Beschreibung |
|---|---|---|
| `JC-4s/tune/status` | `idle` / `tuning` / `done` / `aborted` | Aktueller Zustand |
| `JC-4s/tune/progress` | 0–100 | Fortschritt Coarse-Scan in % |
| `JC-4s/swr` | float | SWR nach abgeschlossenem Tune |

---

## 4. Web-GUI

### 4.1 Struktur (Single-Page, ausgeliefert vom ESP32-C3)
Das HTML/CSS/JS liegt als separate Dateien in LittleFS (`data/`). Kein externes CDN, alles self-contained.

**Layout:** Drei-Spalten-Design (angelehnt an Shelly Cloud): 72 px Sidebar + scrollbarer Hauptbereich + 264 px Echtzeit-Statsleiste.

**Seiten:**
1. **Dashboard** — L/C-Slider, Tuner-Modi, AutoTune-Button
2. **Presets** — Tabelle aller EEPROM-Presets, Löschen-Button je Preset
3. **Settings** — WLAN, MQTT, Tune-Parameter, OTA-URL, Log-Level
4. **Maintenance** — OTA Update (lokal + GitHub), Systemneustart

**Sprache:** Englisch (Standard) / Deutsch umschaltbar, persistiert in `localStorage`.  
**Theme:** Dark Mode (Standard) / Hell umschaltbar, persistiert in `localStorage`.

### 4.2 Technologie
- Vanilla HTML5 + CSS3 + JavaScript (kein Framework, kein Build-Step)
- Kommunikation: REST-API (JSON) + Server-Sent Events (SSE) für Live-Daten
- Responsive: <900 px blendet Statsleiste aus, <560 px Sidebar wird zur Bottom-Navigation

### 4.3 REST-API (HTTP/1.1)
| Methode | Pfad | Beschreibung |
|---|---|---|
| GET | `/api/status` | Aktueller Status (L, C, Modus, SWR, freq, wifi, otaState) |
| POST | `/api/tune` | L, C, Modus setzen |
| POST | `/api/autotune` | AutoTune starten / stoppen |
| GET | `/api/presets` | Alle Presets aus EEPROM |
| DELETE | `/api/presets/{freq}` | Preset löschen |
| DELETE | `/api/presets` | Alle Presets löschen |
| GET | `/api/config` | Aktuelle Konfiguration (inkl. `ota_manifest_url`) |
| POST | `/api/config` | Konfiguration speichern |
| GET | `/events` | SSE-Stream (SWR, tuneState, tuneProgress, otaState, otaProgress) |
| POST | `/ota/local/fw` | FW-Upload lokal (multipart, Header `X-MD5`) |
| POST | `/ota/local/fs` | FS-Upload lokal (multipart, Header `X-MD5`) |
| GET | `/ota/github/check` | Manifest abrufen, Version vergleichen, Info zurückgeben |
| POST | `/ota/github/install` | GitHub-OTA starten (FW + FS sequenziell) |

### 4.4 OTA Update

FW und FS werden immer gemeinsam aktualisiert — zuerst FW, dann FS, jeweils mit MD5-Verifikation. Ein Abbruch bei der FW lässt das FS unverändert; ein Abbruch bei der FS lässt die neue FW trotzdem aktiv (FS ist abwärtskompatibel zu gestalten).

#### GitHub Release-Struktur
Jedes Release enthält exakt diese Assets:

```
gt-jc-4s @ v1.2.0
├── firmware.bin       # App-Partition (OTA-Slot)
├── firmware.bin.md5   # MD5 als Plaintext (32 Hex-Zeichen + Newline)
├── littlefs.bin       # LittleFS-Partition (Web-GUI, Root-CA)
├── littlefs.bin.md5   # MD5 als Plaintext
└── release.json       # Manifest
```

**`release.json` Format:**
```json
{
  "version": "1.2.0",
  "fw": {
    "url": "https://github.com/hb9czf/gt-jc-4s/releases/download/v1.2.0/firmware.bin",
    "md5": "a3f5c2d1e8b7049f6c3a2d1e8b704900",
    "size": 524288
  },
  "fs": {
    "url": "https://github.com/hb9czf/gt-jc-4s/releases/download/v1.2.0/littlefs.bin",
    "md5": "b4e6d3f2a9c8150e7d4b3e2a9c815011",
    "size": 1048576
  },
  "changelog": "AutoTuner Fine-Step verbessert, Web-GUI Preset-Verwaltung"
}
```

**Manifest-URL** (Default im Code, überschreibbar in Web-GUI / NVS):
```
https://github.com/hb9czf/gt-jc-4s/releases/latest/download/release.json
```
GitHub leitet `/latest/download/` automatisch auf das neueste Release um — keine API-Abfrage nötig.

#### OTA-Ablauf (GitHub)

```
1. GET manifest_url → release.json parsen
2. version vergleichen mit FIRMWARE_VERSION (SemVer)
   └─ gleich oder älter → "Kein Update verfügbar" → Ende
3. Web-GUI zeigt: neue Version, Grösse FW+FS, Changelog
4. Nutzer bestätigt → OTA startet

── Phase 1: Firmware ──────────────────────────────────────
5. GET fw.url (HTTPS, chunked stream)
   - MD5 laufend berechnen (kein zweiter Puffer)
   - esp_ota_write() chunk für chunk in OTA-Slot
6. MD5 vergleichen mit fw.md5
   └─ Mismatch → esp_ota_abort() → Fehlermeldung → Ende
7. esp_ota_end() → OTA-Slot validiert

── Phase 2: Filesystem ────────────────────────────────────
8. GET fs.url (HTTPS, chunked stream)
   - MD5 laufend berechnen
   - LittleFS-Partition direkt beschreiben
9. MD5 vergleichen mit fs.md5
   └─ Mismatch → Fehlermeldung (FW bereits geflasht, FS-Retry möglich)
10. Neustart → neue FW + neues FS aktiv
```

#### OTA-Ablauf (Lokal / HTTP Upload)
Für Entwicklung und Offline-Betrieb. FW und FS werden separat hochgeladen, aber in der gleichen Sitzung (Web-GUI führt durch beide Schritte).

```
1. Web-GUI: Datei-Auswahl firmware.bin + littlefs.bin
2. POST /ota/fw  (multipart) → MD5 aus Header X-MD5 oder mitgeschickt
3. MD5-Verifikation → Flash OTA-Slot
4. POST /ota/fs  (multipart) → MD5-Verifikation → Flash LittleFS
5. Neustart
```

MD5 beim lokalen Upload: Der Browser berechnet die MD5 der Datei via `crypto.subtle` (WebCrypto API) vor dem Upload und schickt sie im Header `X-MD5`. Der ESP32 vergleicht nach dem Empfang.

#### Partitionstabelle (`custom_partitions.csv` → anpassen)
```
# Name,   Type, SubType,  Offset,   Size
nvs,      data, nvs,      0x9000,   0x5000
otadata,  data, ota,      0xe000,   0x2000
app0,     app,  ota_0,    0x10000,  0x180000   # 1.5 MB FW-Slot A
app1,     app,  ota_1,    0x190000, 0x180000   # 1.5 MB FW-Slot B
spiffs,   data, spiffs,   0x310000, 0xF0000    # 960 KB FS
```

#### GitHub Actions Workflow (CI/CD)
Bei jedem Push auf `main` mit Tag `v*.*.*`:
1. PlatformIO build → `firmware.bin` + `littlefs.bin`
2. MD5 berechnen → `.md5`-Dateien erzeugen
3. `release.json` generieren (Version aus Tag, URLs, MD5, Grösse)
4. GitHub Release erstellen, alle 5 Assets anhängen

```yaml
# .github/workflows/release.yml (Kurzfassung)
on:
  push:
    tags: ['v*.*.*']
jobs:
  build:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4
      - uses: actions/cache@v4
        with:
          path: ~/.platformio
          key: pio-${{ hashFiles('platformio.ini') }}
      - run: pip install platformio
      - run: pio run && pio run -t buildfs
      - run: |
          md5sum .pio/build/seeed_xiao_esp32c3/firmware.bin | awk '{print $1}' > firmware.bin.md5
          md5sum .pio/build/seeed_xiao_esp32c3/littlefs.bin  | awk '{print $1}' > littlefs.bin.md5
          VERSION=${GITHUB_REF_NAME#v}
          cat > release.json <<EOF
          {
            "version": "$VERSION",
            "fw": {
              "url": "https://github.com/${{ github.repository }}/releases/download/${{ github.ref_name }}/firmware.bin",
              "md5": "$(cat firmware.bin.md5)",
              "size": $(stat -c%s .pio/build/seeed_xiao_esp32c3/firmware.bin)
            },
            "fs": {
              "url": "https://github.com/${{ github.repository }}/releases/download/${{ github.ref_name }}/littlefs.bin",
              "md5": "$(cat littlefs.bin.md5)",
              "size": $(stat -c%s .pio/build/seeed_xiao_esp32c3/littlefs.bin)
            },
            "changelog": "$(git log -1 --pretty=%B | head -1)"
          }
          EOF
      - uses: softprops/action-gh-release@v2
        with:
          files: |
            .pio/build/seeed_xiao_esp32c3/firmware.bin
            .pio/build/seeed_xiao_esp32c3/littlefs.bin
            firmware.bin.md5
            littlefs.bin.md5
            release.json
```

---

## 5. Datenhaltung

### 5.1 RAM-Datastore (zentraler Systemzustand)

Ein einziger globaler Struct im RAM hält den kompletten Laufzeitzustand. Alle Tasks lesen und schreiben ausschliesslich hier — kein direkter Datenaustausch zwischen Tasks. Ein Mutex schützt den Zugriff; für Aktions-Trigger existieren minimale FreeRTOS-Primitive.

```cpp
struct TunerState {
    // Tuner-Position (aktuell am Gerät eingestellt)
    uint16_t L;              // 0–2047
    uint16_t C;              // 0–511
    uint8_t  mode;           // 1=C@TRX  2=C@ANT  3=kein C
    uint16_t freq_kHz;       // zuletzt via MQTT empfangen

    // SWR-Messung
    float    swr;
    float    returnLoss;     // dB
    uint8_t  vfwd, vrev;     // Rohwerte PCF8591 AN0/AN1

    // AutoTuner
    enum class TuneState : uint8_t
        { IDLE, TUNING, DONE, ABORTED } tuneState;
    uint8_t  tuneProgress;   // 0–100 %

    // OTA
    enum class OtaState : uint8_t
        { IDLE, CHECKING, DOWNLOADING_FW, DOWNLOADING_FS,
          DONE, ERROR } otaState;
    uint8_t  otaProgress;    // 0–100 %
    char     otaError[48];   // Fehlermeldung im Fehlerfall

    // System
    int8_t   rssi;
    char     fwVersion[12];  // z.B. "1.2.0"
} state;

SemaphoreHandle_t stateMutex;

// Minimale Signalisierung für Aktionen
SemaphoreHandle_t tuneStartSem;    // Web/MQTT → AutoTuner-Task
SemaphoreHandle_t tuneAbortSem;    // Web/MQTT → AutoTuner-Task (Abbruch)
QueueHandle_t     i2cCmdQueue;     // alle Tasks → I2C-Task (Tiefe 8)
```

**Zugriffsmuster:**
```cpp
// Lesen (alle Tasks)
xSemaphoreTake(stateMutex, portMAX_DELAY);
float swr = state.swr;
xSemaphoreGive(stateMutex);

// Schreiben (z.B. I2C-Task nach Messung)
xSemaphoreTake(stateMutex, portMAX_DELAY);
state.vfwd = raw_fwd;
state.swr  = calcSWR(raw_fwd, raw_rev);
xSemaphoreGive(stateMutex);
```

### 5.2 Persistente Parameter (LittleFS `/config.json`)
Die Gerätekonfiguration wird als JSON-Datei in LittleFS gespeichert (`/config.json`). Zugriff über `AppConfig g_cfg` (`src/cfg/AppConfig.h`), serialisiert mit ArduinoJson. Beim Fehlen der Datei werden Defaults angewendet.

| Parameter | Typ | Default | Beschreibung |
|---|---|---|---|
| `wifi_ssid` | string | — | WLAN SSID |
| `wifi_pass` | string | — | WLAN Passwort |
| `mqtt_server` | string | — | MQTT Broker IP/Host |
| `mqtt_port` | uint16 | 1883 | MQTT Port |
| `mqtt_enabled` | bool | true | MQTT aktiv |
| `tune_threshold` | float | 18.0 | Min. Return Loss [dB] für Preset-Hit |
| `tune_tx_level` | uint8 | 10 | Min. Vfwd für Tx-Erkennung |
| `coarse_step_l` | uint16 | 64 | L-Schrittweite Coarse-Scan |
| `coarse_step_c` | uint16 | 16 | C-Schrittweite Coarse-Scan |
| `ota_manifest_url` | string | (GitHub latest) | Manifest-URL, überschreibt Default |
| `log_level` | uint8 | 2 | Log-Level (0=ERROR … 3=DEBUG), NVS-persistent |

### 5.3 Captive Portal (WiFi-Erstkonfiguration)
Beim ersten Start oder ohne gültige WLAN-Zugangsdaten startet `WiFiManager::runCaptivePortal()`:
- SoftAP `GT-JC-4s-Setup` + `DNSServer` (alle DNS-Anfragen → AP-IP)
- Minimal-HTML-Formular auf Port 80 (SSID + Passwort)
- Nach Speichern: `Config::save()` → Neustart im Station-Modus
- Funktion kehrt nie zurück (blockiert bis Reboot)

---

## 6. Logging-System

### 6.1 Übersicht

Das Logging-System ist die erste Komponente die initialisiert wird (`setup()`, vor allen Tasks). Es schreibt ausschliesslich auf die serielle Konsole (UART0, 115200 Baud). Das Log-Level ist zur Laufzeit änderbar — via Web-GUI (REST-API) und via serieller Konsole (Kommando).

### 6.2 Log-Levels

| Level | Wert | Verwendung |
|---|---|---|
| `ERROR` | 0 | Kritische Fehler, System funktioniert nicht korrekt |
| `WARN`  | 1 | Unerwartete Zustände, Fallbacks aktiv |
| `INFO`  | 2 | Normaler Betrieb, wichtige Ereignisse — **Default** |
| `DEBUG` | 3 | Detaillierte Zustände, I2C-Werte, SWR-Messungen |

Default-Level zur Compile-Zeit: `INFO` (via Build-Flag überschreibbar).

### 6.3 Ausgabeformat

```
[  1234ms] [INFO ] [AutoTuner] Coarse-Scan gestartet, L-Steps=32 C-Steps=32
[  1891ms] [DEBUG] [I2C     ] PCF8591 AN0=142 AN1=12 SWR=1.17
[  2001ms] [WARN ] [MQTT    ] Verbindung getrennt, Reconnect in 5s
[  2006ms] [ERROR] [OTA     ] MD5-Mismatch: erwartet=a3f5c2 erhalten=ff0011
```

Format: `[timestamp_ms] [LEVEL] [Modul   ] Nachricht`
- Timestamp: Millisekunden seit Boot (`millis()`), 7 Stellen, rechtsbündig
- Level: 5 Zeichen, linksbündig, in eckigen Klammern
- Modul: 8 Zeichen, linksbündig (gekürzt falls nötig)

### 6.4 API (`Logger.h`)

```cpp
// Makros — kompilieren bei zu niedrigem Level zu leerem Statement (zero overhead)
LOG_ERROR("OTA",    "MD5-Mismatch: erwartet=%s erhalten=%s", exp, got);
LOG_WARN ("MQTT",   "Verbindung getrennt, Reconnect in %ds", 5);
LOG_INFO ("System", "Firmware v%s gestartet", FIRMWARE_VERSION);
LOG_DEBUG("I2C",    "PCF8591 AN0=%d AN1=%d", vfwd, vrev);

// Level zur Laufzeit setzen/lesen
Logger::setLevel(LogLevel::DEBUG);   // sofort wirksam
Logger::getLevel();                  // → LogLevel::DEBUG
```

Intern thread-sicher via FreeRTOS-Mutex — mehrere Tasks können gleichzeitig loggen ohne Ausgaben-Mischung.

### 6.5 Laufzeit-Steuerung

**Via serielle Konsole** — Kommandos (CR-terminiert, nicht case-sensitiv):

| Kommando | Beschreibung |
|---|---|
| `log error` | Level auf ERROR setzen |
| `log warn`  | Level auf WARN setzen |
| `log info`  | Level auf INFO setzen |
| `log debug` | Level auf DEBUG setzen |
| `log?`      | Aktuelles Level anzeigen |
| `status`    | TunerState-Snapshot auf Konsole ausgeben |
| `reboot`    | Neustart auslösen |

Die serielle Konsole wird nicht-blockierend per `Serial.available()` gepollt (eigener Mini-Task, 2 KB Stack, niedrigste Priorität).

**Via Web-GUI** — Wartungs-Tab:
- Dropdown: ERROR / WARN / INFO / DEBUG → sofort wirksam
- Level wird in NVS gespeichert und nach Reboot wiederhergestellt

**Via REST-API:**

| Methode | Pfad | Beschreibung |
|---|---|---|
| GET | `/api/config` | Antwort enthält `"log_level": "info"` |
| POST | `/api/config` | `{"log_level": "debug"}` → sofort wirksam, NVS-persistent |

### 6.6 Build-Flag (Compile-Zeit Minimum)

```ini
; platformio.ini
build_flags =
    -D LOG_LEVEL_DEFAULT=2   ; 0=ERROR 1=WARN 2=INFO 3=DEBUG
```

Mit `LOG_LEVEL_DEFAULT=0` werden alle WARN/INFO/DEBUG-Makros zu leeren Statements kompiliert — kein Code, kein RAM-Verbrauch für Release-Builds auf Produktionshardware. Das Laufzeit-Level kann nie unterhalb des Compile-Zeit-Minimums gesetzt werden.

## 7. FreeRTOS-Tasklayout

| Task | Prio | Stack | Funktion |
|---|---|---|---|
| `taskWebServer` | 3 | 8192 | HTTP-Requests, REST-API, SSE, OTA |
| `taskMQTT` | 4 | 4096 | MQTT-Reconnect, Publish/Subscribe |
| `taskAutoTuner` | 1 | 6144 | Tune-Algorithmus (wartet auf Semaphore) |
| `taskI2C` | 5 | 4096 | Alleiniger I2C-Bus-Master (Queue-basiert) |
| `taskSerial` | 0 | 2048 | Serielle Konsole, Kommando-Parser |

**Synchronisation:**
- `stateMutex` — Schutz des RAM-Datastores (alle Tasks)
- `logMutex` — Serialisierung der UART-Ausgabe (alle Tasks)
- `tuneStartSem` / `tuneAbortSem` — Web/MQTT → AutoTuner-Task
- `i2cCmdQueue` (Tiefe 8) — alle Tasks → I2C-Task

Der I2C-Bus wird **ausschliesslich** vom `taskI2C` bedient. Alle anderen Tasks stellen Befehle in die Queue — keine Bus-Kollisionen ohne manuelle Mutex-Verwaltung.

**Initialisierungsreihenfolge in `setup()`:**
```
1. Serial.begin(115200)
2. Logger::init()           ← als erstes, damit alle folgenden Schritte loggbar sind
3. NVS laden (inkl. log_level)
4. Logger::setLevel(nvs_log_level)
5. I2C init + PCF8574/PCF8591 Verbindungstest
6. WiFiManager::begin()
7. MQTTClient::begin()
8. WebServer::begin()
9. PresetStore::begin()
10. FreeRTOS-Tasks starten
```

---

## 8. PlatformIO-Projektkonfiguration

### 7.1 `platformio.ini`
```ini
[env:seeed_xiao_esp32c3]
platform  = espressif32
board     = seeed_xiao_esp32c3
framework = arduino

monitor_speed = 115200

lib_deps =
    adafruit/Adafruit PCF8574 @ ^2.0.0
    knolleary/PubSubClient   @ ^2.8
    ; PCF8591: Direktzugriff via Wire (kein Lib-Overhead nötig)

board_build.partitions = custom_partitions.csv   ; max Flash für OTA
board_build.filesystem = littlefs

build_flags =
    -D FIRMWARE_VERSION=\"1.0.0\"
    -D GITHUB_OWNER=\"hb9czf\"
    -D GITHUB_REPO=\"gt-jc-4s\"
    -D GITHUB_ASSET=\"firmware.bin\"
    -D CONFIG_FREERTOS_UNICORE=1
    -D LOG_LEVEL_DEFAULT=2    ; 0=ERROR 1=WARN 2=INFO 3=DEBUG          ; Single-Core-Modus

upload_protocol = esptool
```

### 7.2 Verzeichnisstruktur
```
GT-JC-4s/
├── platformio.ini
├── .github/
│   └── workflows/
│       └── release.yml          # CI/CD: Build + Release bei Tag v*.*.*
├── src/
│   ├── main.cpp                 # Setup, Task-Starts, RAM-Datastore init
│   ├── config.h                 # Konstanten, Default-URLs, Defines
│   ├── state.h                  # TunerState Struct + Mutex-Deklaration
│   ├── logger/
│   │   └── Logger.h/cpp        # Logging-System, Makros, Mutex                  # TunerState Struct + Mutex-Deklaration
│   ├── tuner/
│   │   ├── I2CController.h/cpp  # PCF8574, PCF8591 Treiber
│   │   ├── AutoTuner.h/cpp      # Tune-Algorithmus (adaptiert von ZL2APV)
│   │   └── PresetStore.h/cpp    # I2C-EEPROM 0x50 Verwaltung
│   ├── cfg/
│   │   └── AppConfig.h/cpp      # Konfigurationsstruktur, Config::load/save → /config.json
│   ├── network/
│   │   ├── WiFiManager.h/cpp    # Connect, Captive Portal (runCaptivePortal)
│   │   ├── MQTTClient.h/cpp     # PubSubClient-Wrapper
│   │   └── WebServer.h/cpp      # REST-API, SSE, OTA-Endpunkte
│   └── ota/
│       └── OTAUpdater.h/cpp     # Lokal + GitHub OTA, MD5-Verifikation
├── data/                        # LittleFS-Inhalt (wird zu littlefs.bin)
│   ├── index.html               # Web-GUI (Shelly-Layout, EN/DE, Dark/Light)
│   ├── app.js                   # Web-GUI JavaScript (i18n, SSE, REST)
│   ├── style.css                # Web-GUI CSS (CSS-Variables, 3-Spalten-Grid)
│   └── certs/
│       └── github_root_ca.pem   # Root-CA für api.github.com / HTTPS-Downloads
└── test/                        # Unit-Tests (optional)
```

---

## 9. Nicht-funktionale Anforderungen

| Anforderung | Zielwert |
|---|---|
| Web-GUI Ladezeit | < 2 s im lokalen WLAN |
| AutoTune-Dauer (full scan) | < 60 s (bei Tx-Signal) |
| AutoTune-Dauer (Preset-Hit) | < 5 s |
| MQTT-Reconnect-Intervall | 5 s (wie bisher) |
| OTA-Upload Timeout | 60 s |
| Bootzeit bis WiFi verbunden | < 10 s |
| RAM-Auslastung (Ziel) | < 300 KB (von 400 KB) |
| Serieller Monitor | 115200 Baud |

---

## 10. Abgrenzungen (Out of Scope v1.1)

- CAT/Serial-Schnittstelle zum Transceiver (Frequenz kommt via MQTT)
- HTTPS für lokale Web-GUI (nur HTTP im LAN)
- Privates GitHub-Repository (kein Token-Management)
- Mobile App

---

## 11. Offene Punkte / Risiken

| # | Punkt | Priorität |
|---|---|---|
| 1 | PCF8591 braucht nach Channel-Select ~1 Konversionszyklus Einschwingzeit — in `readSWR()` berücksichtigen (Dummy-Read oder `delayMicroseconds`) | Hoch |
| 2 | GitHub HTTPS: Root-CA für `objects.githubusercontent.com` (Download-CDN) **und** `api.github.com` müssen ins LittleFS — zwei verschiedene CAs prüfen | Hoch |
| 3 | MD5 beim lokalen Upload: WebCrypto (`crypto.subtle`) ist nur in sicheren Kontexten (HTTPS oder localhost) verfügbar — lokale Web-GUI läuft über HTTP; Fallback: MD5 serverseitig nach Empfang berechnen | Mittel |
| 4 | I2C-EEPROM 0x50: ~1 Mio. Schreibzyklen; Preset-Schreiben nur bei explizitem Nutzer-Befehl, nie automatisch im Tune-Loop | Mittel |
| 5 | Partitionstabelle: 2× 1.5 MB OTA-Slots + 960 KB LittleFS passen in 4 MB Flash — knapp, aber ausreichend; Web-GUI-Assets komprimiert halten (< 200 KB) | Info |
| 6 | CoarseStep Worst Case: 32 × 9 × 3 Modi = 864 Messpunkte; bei 20 ms/Messung ≈ 17 s → akzeptabel, via SSE/MQTT als Progress anzeigen | Info |

---

## 12. Status v1.1 — Implementiert

Alle Kernfunktionen sind implementiert und im Repository:

| # | Feature | Status |
|---|---|---|
| 1 | PlatformIO-Projekt, Partitionstabelle, CI/CD | ✅ |
| 2 | I2CController (PCF8574 × 4, PCF8591) | ✅ |
| 3 | AutoTuner (Preset → Coarse → Fine) | ✅ |
| 4 | Web-GUI (Shelly-Layout, EN/DE, Dark/Light) | ✅ |
| 5 | Captive Portal (WiFi-Erstkonfiguration) | ✅ |
| 6 | Config in LittleFS (`/config.json`) | ✅ |
| 7 | MQTT-Client (PubSubClient) | ✅ |
| 8 | OTA lokal + GitHub | ✅ |
| 9 | REST-API + SSE | ✅ |

**Nächste Schritte:**
- Integration & Test auf Hardware
- CAT-Schnittstelle (Out of Scope v1.x)
