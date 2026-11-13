# GT-JC-4s — Projektspezifikation v3.22
**Antennenkoppler-Steuerung mit AutoTuner**
Datum: 2026-11-02 | Autor: HB9CZF | Status: Implementiert / In Test

---

## 1. Projektübersicht

### 1.1 Ziel
Migration des bestehenden Arduino-IDE-Sketches `GT_JC_4s_ESP32C3_20260614.ino` auf PlatformIO mit Integration des AutoTuner-Algorithmus (ZL2APV), einer Web-GUI für Konfiguration und Wartung sowie OTA-Update-Funktion (lokal und via GitHub).

### 1.2 Hardware-Basis
| Komponente | Beschreibung |
|---|---|
| MCU | Seeed Studio XIAO ESP32-S3 |
| Architektur | Xtensa LX7, Dual-Core, 240 MHz |
| RAM | 512 KB SRAM + 8 MB PSRAM |
| Flash | 8 MB (Partitionstabelle nutzt weiterhin nur ~4 MB, siehe `custom_partitions.csv`) |
| I2C-Busse | Zwei unabhängige Busse: `Wire` (GPIO5/6, 100 kHz, 5 V über PCA9517-Buffer) für die 4× PCF8574-Relaistreiber; `Wire1` (GPIO1/2 = D0/D1, 100 kHz, eigener dedizierter PCA9517-Buffer) exklusiv für das ADS1115 — siehe Item 74 |
| Tuner | Stockcomer JC-4s (Original-Hardware bleibt unverändert) |

### 1.3 I2C-Geräte (JC-4s intern)
| Adresse | Chip | Funktion |
|---|---|---|
| 0x38 | PCF8574AP | C-Relais Bit 0–7 (6 pF – 800 pF, direkt auf P0–P7) |
| 0x39 | PCF8574AP | C-Bit 8 (1600 pF, P0) + K1 Modus (P1) + K9/K10 Modus (P2) + **K-Tune (P3)** |
| 0x3A | PCF8574AP | L-Relais L1–L8 (0.078–10 µH, P0–P7) |
| 0x3B | PCF8574AP | L0 → P3 (0.039 µH), L9 → P0 (20 µH), L10 → P1 (40 µH); P2 = Eingang (Ant A, nie treiben!) |
| 0x48 (auf `Wire1`) | ADS1115 | 16-Bit-ADC: A0 = Vfwd, A1 = Vrev, je über TLC2272-Op-Amp-Stufe + 4:1-Teiler; `GAIN_ONE` (±4,096 V, ~125 µV/Count) |
| 0x50 | PCF8582C | EEPROM 256 Byte (Band-Presets) |
| 0x51 | PCF8582C | EEPROM 256 Byte (reserviert) |

---

## 2. Systemarchitektur

### 2.1 Software-Stack
```
PlatformIO (Arduino-Framework für ESP32-S3)
  └─ FreeRTOS (transparent, via ESP-IDF unter dem Arduino-Layer)
       ├─ Task: WiFi + Web-Server (8 KB Stack)
       ├─ Task: MQTT-Client (4 KB Stack)
       ├─ Task: AutoTuner (6 KB Stack, niedriger Prio)
       ├─ Task: I2C-Controller (4 KB Stack, höchste Prio)
       └─ Task: Serielle Konsole (2 KB Stack, niedrigste Prio)
```

> **Hinweis zum ESP32-S3:** Das Modul hat **zwei CPU-Cores** (Xtensa LX7). Tasks werden per `xTaskCreate()` ohne Core-Pinning erzeugt und können vom Scheduler auf beide Cores verteilt werden (anders als beim vorherigen ESP32-C3, der nur einen Core hatte und `taskI2C`/`taskWeb` etc. zwangsläufig zeitscheiben-seriell ausführte). Die Synchronisierung über `StateLock`/`g_i2cCmdQueue` bleibt korrekt, da beide bereits vollständig Mutex-/Queue-basiert sind — der I2C-Bus wird weiterhin ausschliesslich von `taskI2C` bedient, unabhängig davon, auf welchem Core es läuft.

### 2.2 Steuerungspfade
```
Node-RED  ──MQTT──►  ESP32-S3  ──I2C──►  PCF8574 × 4  ──►  Relais L/C/K-Tune
Web-GUI   ──HTTP──►  ESP32-S3            PCF8591 ADC   ◄──  SWR-Brücke
                         ▲               PCF8582C EEPROM ◄─► Presets
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
| `JC-4s/finetune` | ◄ SUB | `1` = Fine-Tune (ab aktuellem L/C) starten |
| `JC-4s/ktune` | ◄ SUB | `1` = K-Tune-Relais EIN, `0` = AUS |
| `JC-4s/feedback/L` | ► PUB | Bestätigung L (nach I2C-Ausführung) |
| `JC-4s/feedback/C` | ► PUB | Bestätigung C |
| `JC-4s/feedback/tunermode` | ► PUB | Bestätigung Modus |
| `JC-4s/feedback/ktune` | ► PUB | Bestätigung K-Tune-Zustand (0/1) |
| `JC-4s/feedback/fpwr` | ► PUB | Sense-Input F-PWR, roh/unkalibriert (0/1) |
| `JC-4s/feedback/zhigh` | ► PUB | Sense-Input Z-HIGH, roh/unkalibriert (0/1) |
| `JC-4s/feedback/zlow` | ► PUB | Sense-Input Z-LOW, roh/unkalibriert (0/1) |
| `JC-4s/feedback/phase` | ► PUB | Sense-Input PHASE, roh/unkalibriert (0/1) |
| `JC-4s/L_uH` | ► PUB | Berechneter L-Wert in µH |
| `JC-4s/C_pF` | ► PUB | Berechneter C-Wert in pF |
| `JC-4s/swr` | ► PUB | Aktuelles SWR |
| `JC-4s/rssi` | ► PUB | WiFi-Signalstärke (alle 10 s) |
| `JC-4s/id` | ► PUB | Firmware-Version (retained) |
| `JC-4s/ip` | ► PUB | IP-Adresse (retained, bei jedem MQTT-Connect) |
| `JC-4s/mac` | ► PUB | MAC-Adresse (retained, bei jedem MQTT-Connect) |
| `JC-4s/log` | ► PUB | Log-Nachrichten (alle Levels ≥ konfiguriertem Level) |

**MQTT-Feedback-Logik:** Da die I2C-Ausführung asynchron via Queue läuft, erkennt `taskMQTT` Zustandsänderungen von L/C/mode/kTune in g_state und publiziert dann `publishStatus()`. Ebenso bei Tune-Status-Wechsel.

**MQTT Frequenz-Auto-Apply (Segment-Wechsel):**
Wenn `JC-4s/freq` einen Wert sendet der zu einem anderen 20 kHz-Segment gehört als die zuletzt empfangene Frequenz, sucht `taskMQTT` via `PresetStore::findBest(seg)` nach einem Preset für dieses Segment. Falls ein Preset mit exakt passender Segment-Basisfrequenz (`p.freq_kHz == seg`) existiert, wird sofort `SET_LC` gesendet — ohne AutoTune. Kein Match: kein SET_LC, `lastSeg` wird trotzdem aktualisiert (verhindert Retry-Schleife).

**MQTT Frequenz-Auto-Shelly (80m-Band):**
Bei jedem Segment-Wechsel setzt `onMessage()` ein Pending-Flag (`s_shellyPending`, `s_shellyTarget`). Der `taskMQTT`-Loop ruft ausserhalb des Callbacks `shellyApply(on)` auf (`GET /rpc/Switch.Set?id=0&on=true/false` via HTTPClient). Ziel: Shelly Plus1PM **EIN** bei 80m (3500–4000 kHz), **AUS** bei allen anderen Bändern. Das Pending-Flag verhindert, dass der MQTT-Callback durch einen HTTP-Call blockiert wird.

**MQTT Retained-Message-Schutz:**
MQTT-Broker liefern beim Subscribe sofort alle retained Messages zurück. Dies würde nach dem Boot L/C/mode auf gespeicherte Werte setzen und die Hardware-Initialisierung (L=0, C=0, mode=1) überschreiben. Zwei Schutzmechanismen:
1. **`g_relayInitDone`-Flag** (volatile bool, default false): `taskMQTT::onMessage()` verwirft alle SET_LC-artigen Befehle (`L`, `C`, `tunermode`) solange das Flag nicht gesetzt ist. `taskI2C` setzt es erst nach Abschluss der Hardware-Initialisierung und Queue-Spülung.
2. **`g_mqttConnectedAt`-Suppress-Fenster** (volatile unsigned long): Bei jedem MQTT-Connect/Reconnect wird der `millis()`-Zeitstempel gespeichert. `onMessage()` verwirft SET_LC-Befehle für **2 Sekunden** nach dem Connect. Retained Messages kommen innerhalb von Millisekunden; Live-Befehle vom Benutzer kommen später.

#### Web-GUI Steuerung
- Slider für L (0–2047) und C (0–511)
- Buttons für Tuner-Modi (C@TRX / C@ANT / kein C)
- Button „AutoTune starten / Abbrechen" — blau (btn-primary)
- Button „Fine-Tune" — blau (btn-primary); startet Fine-Tune ab aktuellem L/C/Mode
- Button „K-Tune" — immer blau (btn-primary); weisser Outline-Ring (`btn-on`) wenn EIN
- SWR-Anzeige live via SSE (zeigt „—" bei keinem TX-Signal, Zahlenwert bei aktivem TX)
- Frequenzanzeige (aktuell via MQTT empfangen)

### 3.2 AutoTuner-Integration

#### Algorithmus (adaptiert von ZL2APV)

**Suchraumparameter:**
- L: 2048 Stufen (11 Bit), Coarse-Schritt: 64
- C: 512 Stufen (9 Bit), Coarse-Schritt: 16
- Tuner-Modi: 1 (C@TRX) und 2 (C@ANT) werden im Coarse-Scan berücksichtigt

**Phase 1 — Preset-Suche**
1. RAM-Cache nach gespeicherten Band-Presets durchsuchen (kein Wire-Zugriff)
2. Bestes Preset als Startpunkt verwenden, SWR messen
3. Wenn Return Loss ≥ Schwellwert (default 18 dB ≈ SWR 1.29): fertig

**Phase 2 — Coarse-Scan**
- C×L-Kombinationen in konfigurierbaren Schritten (Default: L-Schritt=64, C-Schritt=16), Modi 1 und 2
- **Band-spezifische Suchraumgrenzen:** `getBandLimits(freq_kHz)` liefert pro Band ein reduziertes L_max / C_max (Tabelle unten). Bei unbekannter oder ausserband-Frequenz: voller Bereich (L_max=2047, C_max=511). Ergibt auf 10m bis zu 56× weniger Messpunkte als ein Vollscan.
- **Top-3 diverse Kandidaten:** Statt nur dem globalen Besten werden die drei besten Messpunkte verfolgt, wobei jeder Kandidat mindestens 1 Coarse-Schritt Abstand (in L *und* C) von den anderen haben muss. Ziel: verschiedene Regionen des L/C-Raums abdecken, damit schmale Optima nicht durch einen dominanten Falsch-Treffer verdrängt werden.
- Abbruch via MQTT `JC-4s/tune = 0` oder Web-GUI

**Coarse-Scan Band-Grenzen:**

| Band | f (kHz) | L_max raw | L_max (µH) | C_max raw | C_max (pF) | Messpunkte × 2 Modi |
|------|---------|-----------|-----------|-----------|-----------|---------------------|
| 160m | 1800–2000 | 2047 | 79,8 | 511 | 3193 | 2048 |
| 80m | 3500–3800 | 2047 | 79,8 | 511 | 3193 | 2048 |
| 60m | 5351–5366 | 763 | 29,8 | 494 | 2975 | 720 |
| 40m | 7000–7200 | 576 | 22,5 | 511 | 3193 | 576 |
| 30m | 10100–10150 | 404 | 15,8 | 494 | 2975 | 390 |
| 20m | 14000–14350 | 292 | 11,4 | 357 | 2175 | 161 |
| 17m | 18068–18168 | 225 | 8,8 | 275 | 1668 | 96 |
| 15m | 21000–21450 | 195 | 7,6 | 237 | 1440 | 72 |
| 12m | 24890–24990 | 164 | 6,4 | 200 | 1212 | 52 |
| 10m | 28000–29700 | 146 | 5,7 | 178 | 1075 | 36 |

> Messpunkte = ⌊L_max / step_L + 1⌋ × ⌊C_max / step_C + 1⌋; step_L=64, step_C=16 (Defaults)

**Phase 2.25 — Inter-L Scan**
- Ziel: L-Werte zwischen den Coarse-Stützpunkten abdecken (z.B. L=15 liegt zwischen L=0 und L=64).
- Schrittweite: `coarse_step_l / 4` = 16 (Default). Probierte L-Werte: 16, 32, 48 (L=0 und L=64 sind bereits im Coarse-Scan).
- Alle C-Werte im Coarse C-Schritt (step=16), beide Modi.
- Messpunkte: 3 × 33 × 2 ≈ 200, Dauer: ~7 s.
- Das beste Ergebnis dient als 4. Medium-Kandidat.

**Phase 2.5 — Medium-Scan**
- Ausgeführt für jeden der Top-3 Coarse-Kandidaten **und** den Inter-L Besten (falls nicht bereits in Top-3).
- Bereich: ±1 Coarse-Schritt (±64) in L und C um den jeweiligen Startpunkt.
- Schrittweite: `coarse_step_l / 8` = 8 (L), `coarse_step_c / 4` = 4 (C).
- Beispiel: von (L=16, C=64) → L=0..80 step=8, C=48..80 step=4 → findet L=16, C=72 → Fine-Tune → L=15, C=71.
- Nach allen Medium-Scans: globales Bestes (höchster Return Loss) wird für Fine-Tune übernommen.

**Phase 3 — Fine-Step (Sliding Window)**
- Fenstergrösse: 9 Punkte (±4) um Coarse-Optimum
- Abwechselnd L-Sweep (festes C) und C-Sweep (festes L), max. 5 Iterationen
- **Frühzeitiger Abbruch:** wenn Return Loss > 60 dB (Vrev = 0, ADC-Rauschgrenze) nach einem L-Sweep, wird kein weiterer C-Sweep gestartet. Verhindert Drift in die „flache" Region bei perfekter Anpassung.

**Fine-Tune-Modus (standalone):**
- Auslösung über Web-GUI Button „Fine-Tune" (`/api/finetune`) oder MQTT `JC-4s/finetune = 1`
- Startet `fineTune()` ab aktuellem L/C/Mode (kein Coarse-Scan)
- Gibt bei laufendem Tune **ausführliche Schritt-Tabellen** via `LOG_INFO` aus (L-Sweep und C-Sweep pro Iteration) — erscheinen damit sowohl auf Serial als auch auf `JC-4s/log`:

```
 Iter 2 / L-Sweep (C=29):
   L   |   C   |   SWR  |    RL    |
 ------+-------+--------+----------+
    19 |    29 |   1.11 |   25.6 dB |
    20 |    29 |   1.11 |   25.6 dB |
    21 |    29 |   1.00 |    inf dB | <--
    22 |    29 |   1.00 |    inf dB |
```

- Nach Abschluss: Ergebnis-Tabelle via `LOG_INFO` (Serial + `JC-4s/log`) und Preset automatisch gespeichert

**Fine-Tune-Ergebnis-Tabelle (Serial):**
```
+----------------------+------------+
| Fine-Tune Result     |            |
+----------------------+------------+
| L (raw)              |         21 |
| L                    |   0.820 uH |
| C (raw)              |         29 |
| C                    |   187.0 pF |
| Mode                 |      C@ANT |
| SWR                  |       1.00 |
| Return Loss          |     inf dB |
+----------------------+------------+
```

**Abschluss (AutoTune und Fine-Tune):**
1. `SET_LC` mit 20 ms Settle-Zeit (Relais-Federrückstellung kann bis ~10 ms dauern)
2. `READ_SWR` auslösen, 50 ms warten
3. Final-SWR aus `g_state` lesen (vor K-Tune OFF, um Hintergrundmessung zu vermeiden)
4. K-Tune OFF, 30 ms warten
5. `tuneState = DONE`
6. Falls `freq_kHz > 0`: `SAVE_PRESET` an I2C-Queue senden

**SWR-Messung (PCF8591)**
```
Vfwd = PCF8591 read(AN1)          // 8-Bit ADC, 0–255  (Hardware: AN1=Vfwd)
Vrev = PCF8591 read(AN0)          //                   (Hardware: AN0=Vrev)
rho  = Vrev / Vfwd
Return Loss [dB] = -20 × log10(rho)
SWR = (1 + rho) / (1 - rho)
```
- ADC-Referenzspannung: **1.6 V** → 1 Count ≈ 6.27 mV (1600 mV / 255)
- 8 Messungen, Ausreisser ±5% durch Maximalwert ersetzen
- Mindest-TX-Level: `g_cfg.tune_tx_level` (runtime-konfigurierbar, default 3; entspricht ~19 mV)
- Kein TX-Signal (Vfwd < tune_tx_level): swr = 0.0, returnLoss = 0.0 → Web-GUI zeigt „—"
- Hintergrundmessung: `taskI2C` misst alle ~250 ms bei leerem Befehlspuffer

**Preset-Speicherung (I2C-EEPROM 0x50) — Format v3**
```
Struktur je Preset (7 Byte):
  uint16_t freq;      // Frequenz in kHz (Big-Endian), gerundet auf 20 kHz-Segment-Basis
  uint16_t L;         // L-Wert 0–2047 (Big-Endian)
  uint16_t C_mode;    // Bits 15..7: C-Wert 0–511, Bit 2: shellyOn, Bits 1..0: Tuner-Modus
  uint8_t  swr10;     // SWR × 10 als uint8_t (0 = unbekannt); SWR 1.05 → 10
```
- **`shellyOn` (Bit 2 von C_mode):** Ob der Shelly Plus1PM bei diesem Preset eingeschaltet sein soll. AutoTuner setzt das Bit automatisch beim Speichern: `1` für 80m-Band (3500–4000 kHz), `0` für alle anderen Bänder. Manuell umschaltbar via Web-GUI Preset-Tabelle oder `POST /api/presets/{freq}/shelly`. Rückwärtskompatibel: Bit 2 war in Format v2 ungenutzt → alte Presets lesen als 0 (Shelly AUS).
- Max. 36 Presets (⌊256 / 7⌋)
- **20 kHz-Segment-Adressierung:** Frequenz wird beim Speichern auf die Segment-Basisfrequenz gerundet (`floor(freq_kHz / 20) * 20`). 7032 kHz → gespeichert als 7020. Pro Segment darf nur **ein** Preset existieren; bestehende Einträge im selben Segment werden vor dem Speichern gelöscht.
- `PresetStore::findBest(seg)` sucht das Preset mit kleinstem Abstand zur übergebenen Frequenz. Für MQTT-Auto-Apply wird auf exakten Segment-Match geprüft (`p.freq_kHz == seg`).
- **Format-Migration:** Beim Laden (`begin()`) wird jeder Eintrag auf gültige Frequenz (1800–30000 kHz) geprüft. Ausserhalb liegende Werte signalisieren das alte 6-Byte-Format → Ladevorgang stoppt, EEPROM gilt als leer.
- Sortierung nach Frequenz; EEPROM-Schreiben: Byte-für-Byte mit **Acknowledge-Polling** (PCF8582C NACKt während Write-Cycle; kein `vTaskDelay` → kein Taskwechsel → kein Wire-Konflikt)
- **RAM-Cache:** `PresetStore::read()` und `findBest()` lesen ausschliesslich aus einem In-Memory-Array (`s_presets[36]`). Wire wird nur in `taskI2C` für Schreibvorgänge verwendet. Damit sind Lesezugriffe aus `taskWeb` (API) und `taskAutoTuner` (Preset-Suche) race-condition-frei.

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

### 4.3 Hardware-Initialisierung beim Boot

`taskI2C` initialisiert alle PCF8574-Chips nach einem 500 ms Verzögerung (Stromversorgung stabilisieren). PCF8574 Power-on-Zustand ist alle Pins HIGH = alle Relais-Spulen bestromt; die Initialisierung setzt alle Relais auf AUS (L=0, C=0, mode=C@TRX).

**Ablauf:**
```
+500 ms  Schreibe 0x3B = 0x04  (L_LO: L0/L9/L10 AUS, P2=High)
+150 ms  Schreibe 0x3A = 0x00  (L_HI: L1–L8 AUS)
+150 ms  Schreibe 0x38 = 0x00  (C_LO: C0–C7 AUS)
+150 ms  Schreibe 0x39 = 0x06  (C_HI: C8 AUS, K1 EIN (C@TRX), K9/K10 EIN)
+150 ms  PCF8574 Readback-Verifikation (LOG_INFO mit [OK] / [MISMATCH])
         Queue spülen (verwirft Befehle die während Init eintrafen)
         g_relayInitDone = true
```

Die 150 ms Abstände zwischen den Schreibvorgängen verhindern Stromspitzen beim gleichzeitigen Schalten aller Relais.

---

## 5. Web-GUI

### 5.1 Struktur (Single-Page, ausgeliefert vom ESP32-S3)
HTML/CSS/JS liegen als separate Dateien in LittleFS (`data/`). Kein externes CDN.

**Layout:** Drei-Spalten-Design (angelehnt an Shelly): 88 px Sidebar + Hauptbereich + 264 px Echtzeit-Statsleiste.

**Seiten:**
1. **Dashboard** — L/C-Slider, Tuner-Modi, AutoTune-Button, **Fine-Tune-Button**, **K-Tune-Button**, SWR-Live-Anzeige, **Shelly Plus1PM Card** (EIN/AUS-Toggle + Leistungsanzeige)
2. **Presets** — Tabelle aller gespeicherten Presets (Freq, L, C, Modus, **SWR**, **Shelly ●/○**), Shelly-Toggle je Preset, Löschen-Button je Preset; **automatische Aktualisierung 500 ms nach Tune-Abschluss**
3. **Settings** — WLAN (zwei Netzwerke), MQTT, **Shelly-URL**, Tune-Parameter, OTA-URL, Log-Level
4. **Maintenance** — OTA Update (lokal + GitHub), Systemneustart

**Sidebar-Navigation:**
- **Brand-Logo „GT / JC-4s"** oben in der Sidebar — Klick navigiert zum Dashboard
- Dashboard, Presets, Settings, Maintenance — Seiten-Buttons
- **API** — Link-Button ganz unten in der Nav; öffnet `/api/status` in neuem Tab (externes Link-Icon)

**Sprache:** Englisch (Standard) / Deutsch umschaltbar, `localStorage`-persistent.
**Theme:** Dark Mode (Standard) / Hell umschaltbar, `localStorage`-persistent.

### 5.2 Technologie
- Vanilla HTML5 + CSS3 + JavaScript (kein Framework)
- Kommunikation: REST-API (JSON) + Server-Sent Events (SSE) für Live-Daten
- SSE: nicht-blockierend implementiert (Client in `s_sseClient` gespeichert, Push aus `taskWebServer`-Loop)
- SSE-Push: bei Zustandsänderung **sofort** (≤ 5 ms), spätestens aber alle **2 Sekunden** (periodic push)
- Polling-Fallback: alle **5 s** `/api/status`
- SSE-Reconnect nach Verbindungsabbruch: **2 s** (alte `EventSource` wird vor Neuaufbau geschlossen — verhindert mehrfache simultane Verbindungen)
- **Preset-Auto-Refresh:** `syncDashboard()` erkennt `tuneState: TUNING → DONE` und ruft `loadPresets()` mit 500 ms Verzögerung auf (gibt EEPROM-Write-Cycle ~30 ms Zeit zum Abschliessen)

### 5.3 REST-API (HTTP/1.1)
| Methode | Pfad | Beschreibung |
|---|---|---|
| GET | `/api/status` | Status: L, C, **L_uH, C_pF**, mode, **kTune**, SWR, returnLoss, vfwd, vrev, **fPwr, zHigh, zLow, phase** (Sense-Inputs, roh), freq, rssi, tuneState, otaState, … |
| POST | `/api/tune` | L, C, mode setzen |
| POST | `/api/autotune` | AutoTune starten / stoppen |
| POST | `/api/finetune` | Fine-Tune ab aktuellem L/C/Mode starten |
| POST | `/api/ktune` | K-Tune-Relais: `{"ktune": true/false}` |
| GET | `/api/presets` | Alle Presets aus RAM-Cache; Felder: freq_kHz, L, C, mode, swr, **shellyOn** |
| POST | `/api/presets/{freq}/shelly` | `{"shellyOn": true/false}` — shellyOn-Bit eines Presets setzen |
| DELETE | `/api/presets/{freq}` | Preset löschen |
| DELETE | `/api/presets` | Alle Presets löschen |
| GET | `/api/config` | Aktuelle Konfiguration (inkl. **shelly_url**) |
| POST | `/api/config` | Konfiguration speichern |
| GET | `/api/shelly/status` | Shelly-Status proxied via ESP32 (vermeidet Browser-CORS) |
| POST | `/api/shelly/toggle` | Shelly EIN/AUS umschalten (proxied) |
| POST | `/api/reboot` | Gerät neu starten |
| GET | `/events` | SSE-Stream (SWR, L, C, mode, **kTune**, freq, tuneState, otaState, …) |
| POST | `/ota/local/fw` | FW-Upload lokal (multipart, Header `X-MD5`) |
| POST | `/ota/local/fs` | FS-Upload lokal (multipart, Header `X-MD5`) |
| GET | `/ota/github/check` | Manifest abrufen, Version vergleichen |
| POST | `/ota/github/install` | GitHub-OTA starten (FW + FS sequenziell) |

> **Diagnose:** `/api/status` enthält `vfwd` und `vrev` (Rohwerte PCF8591, 0–255; Referenzspannung 1.6 V → 1 Count ≈ 6.27 mV; Hardware: AN1=Vfwd, AN0=Vrev). Damit kann die SWR-Brücken-Aussteuerung ohne Serial Monitor überprüft werden.

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
  "version": "1.4.0",
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

volatile bool          g_relayInitDone;   // gesetzt von taskI2C nach Hardware-Init + Queue-Flush
volatile unsigned long g_mqttConnectedAt; // millis() beim letzten MQTT-Connect

SemaphoreHandle_t g_stateMutex;
SemaphoreHandle_t g_tuneStartSem;       // AutoTune
SemaphoreHandle_t g_fineTuneStartSem;   // Fine-Tune standalone
SemaphoreHandle_t g_tuneAbortSem;
QueueHandle_t     g_i2cCmdQueue;        // Tiefe 8
```

### 6.2 Persistente Parameter

#### LittleFS `/config.json`
Zugriff über `AppConfig g_cfg` (`src/cfg/AppConfig.h`), serialisiert mit ArduinoJson.

| Parameter | Typ | Default | Beschreibung |
|---|---|---|---|
| `mqtt_server` | string | — | MQTT Broker IP/Host |
| `shelly_url` | string | — | Basis-URL des Shelly Plus1PM (z.B. `http://192.168.100.136`) |
| `mqtt_port` | uint16 | 1883 | MQTT Port |
| `mqtt_enabled` | bool | true | MQTT aktiv |
| `tune_threshold` | float | 18.0 | Min. Return Loss [dB] für Tune-Erfolg |
| `tune_tx_level` | uint8 | 3 | Min. Vfwd für TX-Erkennung (ADC-Counts 0–255) |
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

**Netzwerkspeicherung:** Beide Netzwerke in `/wifi.json` (LittleFS), je mit den Feldern `ssid`, `pass`, `static`, `ip`, `mask`, `gw`, `dns`. Beim ersten Boot wird `/wifi.json` angelegt: bevorzugt aus NVS-Credentials (Migration von älterer Firmware), sonst aus `WIFI_SEED1_*` / `WIFI_SEED2_*` (`config.h`). Diese Konstanten werden danach nicht mehr gelesen.

**Ladestrategie in `WiFiManager::begin()`:**
1. `seedIfMissing()` — `/wifi.json` anlegen falls nicht vorhanden (NVS-Migration oder Seed-Konstanten)
2. Netzwerk 1 aus `/wifi.json` versuchen (inkl. statische IP falls konfiguriert)
3. Netzwerk 2 aus `/wifi.json` versuchen
4. Falls beide scheitern: SoftAP + Captive Portal starten

### 6.3 Captive Portal (WiFi-Erstkonfiguration)
Bei keinem erreichbaren Netzwerk startet `WiFiManager::runCaptivePortal()`:
- SoftAP `GT-JC-4s-Setup` + `DNSServer` (alle DNS → AP-IP)
- HTML-Formular auf Port 80 mit **zwei Netzwerk-Blöcken** (SSID, Passwort, statische IP-Felder)
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
[  1234ms] [INFO ] [AutoTuner] Fine done: L=21 C=29 mode=2
[  1891ms] [DEBUG] [I2C     ] SWR meas: vfwd=142 vrev=0 rho=0.000 SWR=1.00
```

### 7.3 Laufzeit-Steuerung
- Serielle Konsole: `log debug` / `log info` / `log warn` / `log error` / `log?`
- Manueller Status-Abruf: `status` (gibt aktuelle L/C/SWR/freq etc. aus)
- Neustart: `reboot`
- Web-GUI Settings: Dropdown → sofort wirksam, in config.json gespeichert

### 7.4 MQTT-Log-Forwarding
Alle Log-Nachrichten (die den konfigurierten Log-Level erreichen) werden zusätzlich auf dem MQTT-Topic `JC-4s/log` publiziert.

**Implementierung:**
- `Logger` stellt einen statischen Callback-Hook bereit (`Logger::setPublishHook(fn)`).
- `MQTTClient::begin()` registriert `logHook()` als Callback und legt eine FreeRTOS-Queue an (Tiefe 16, je 200 Zeichen).
- `logHook()` wird von `Logger::log()` nach dem Serial-Print aufgerufen (ausserhalb des Logger-Mutex), kopiert die Zeile in die Queue (trailing `\r\n` wird abgeschnitten). Bei voller Queue wird die Nachricht verworfen.
- `taskMQTT` entleert die Queue in jedem 50-ms-Zyklus (nur wenn verbunden) und ruft `s_mqtt.publish(„JC-4s/log", ...)` auf. Pro Zyklus werden **maximal 8 Einträge** verarbeitet, um den TCP-Sendepuffer nicht zu überlasten.
- Nach dem Log-Drain wird `s_mqtt.loop()` aufgerufen, damit ausstehende TCP-Daten geflushd und eingehende Pakete verarbeitet werden, bevor der Status-Publish-Burst startet.
- Nachrichten die vor dem ersten MQTT-Connect anfallen bleiben in der Queue (max. 16 Einträge) und werden nach Verbindungsaufbau publiziert.

---

## 8. FreeRTOS-Tasklayout

| Task | Prio | Stack | Funktion |
|---|---|---|---|
| `taskI2C` | 5 | 4096 | Alleiniger I2C-Bus-Master; Hardware-Init beim Boot; Hintergrund-SWR alle 250 ms |
| `taskMQTT` | 4 | 4096 | MQTT-Reconnect, Publish/Subscribe, Feedback-Erkennung, Log-Queue-Drain |
| `taskWebServer` | 3 | 8192 | HTTP, REST-API, SSE (nicht-blockierend), OTA |
| `taskAutoTuner` | 1 | 6144 | Tune-Algorithmus (wartet auf Semaphore) |
| `taskSerial` | 0 | 2048 | Serielle Konsole, Kommando-Parser |

**I2C-Queue-Befehle (`I2CCmd`):**
| Befehl | Felder | Beschreibung |
|---|---|---|
| `SET_LC` | L, C, mode | Schreibt alle vier PCF8574 (L + C + mode + kTune aus g_state) |
| `READ_SWR` | — | SWR-Messung on-demand; Ergebnis in g_state |
| `SET_KTUNE` | kTune | Schreibt **nur 0x39** (C-Bit 8 + mode-Bits + K-Tune-Bit); 0x38/0x3A/0x3B bleiben unverändert |
| `SAVE_PRESET` | freq_kHz, L, C, mode, **shellyOn** | Schreibt Preset in EEPROM (byte-für-byte mit Acknowledge-Polling) und aktualisiert RAM-Cache |

Bei leerem Queue-Puffer (250 ms Timeout) misst taskI2C automatisch SWR im Hintergrund.

**Wichtig — Wire-Zugriff:**
- `taskI2C` ist der **einzige** Task der `Wire` direkt aufruft
- `PresetStore::read()` und `findBest()` arbeiten aus dem RAM-Cache — kein Wire-Aufruf aus anderen Tasks
- EEPROM-Schreibvorgänge (save, delete) laufen über `SAVE_PRESET`-Queue oder werden von taskI2C direkt ausgeführt (deleteAll/deleteByFreq via WebServer sind eine bekannte Ausnahme mit niedrigem Risiko, da selten aufgerufen)

---

## 9. PlatformIO-Projektkonfiguration

### 9.1 `platformio.ini`
```ini
[env:seeed_xiao_esp32s3]
platform  = espressif32
board     = seeed_xiao_esp32s3
framework = arduino

monitor_speed = 115200
upload_port   = COM7

lib_deps =
    adafruit/Adafruit PCF8574 @ ^1.0.0
    knolleary/PubSubClient @ ^2.8
    bblanchon/ArduinoJson @ ^7.0

board_build.partitions = custom_partitions.csv
board_build.filesystem = littlefs

build_flags =
    -D FIRMWARE_VERSION=\"1.4.0\"
    -D GITHUB_OWNER=\"bugi67\"
    -D GITHUB_REPO=\"GT-JC-4s\"
    -D LOG_LEVEL_DEFAULT=2
    -D ARDUINO_USB_CDC_ON_BOOT=1
```

### 9.2 Verzeichnisstruktur
```
GT-JC-4s/
├── platformio.ini
├── custom_partitions.csv
├── .github/workflows/release.yml
├── docs/
│   └── GT-JC-4s_Spezifikation_v1.5.md
├── src/
│   ├── main.cpp
│   ├── config.h          # Konstanten, C_PF[], L_UH[], Defines, MQTT-Topics (inkl. MQTT_PUB_LOG)
│   ├── state.h           # TunerState + I2CCommand + Mutex/Queue + g_relayInitDone + g_mqttConnectedAt
│   ├── logger/
│   │   ├── Logger.h      # setPublishHook(); Compile-time LOG_LEVEL_DEFAULT
│   │   └── Logger.cpp    # Serial-Output + Hook-Aufruf nach Mutex-Release
│   ├── tuner/
│   │   ├── I2CController.h/cpp   # PCF8574/PCF8591-Treiber, Relay-Bit-Mapping, Hardware-Init, alle I2CCmd-Handler
│   │   ├── AutoTuner.h/cpp       # Preset→Coarse→Fine; fineTune(verbose); SAVE_PRESET nach Tune
│   │   └── PresetStore.h/cpp     # RAM-Cache + EEPROM-Write mit Acknowledge-Polling
│   ├── cfg/AppConfig.h/cpp       # config.json via ArduinoJson
│   ├── network/
│   │   ├── WiFiManager.h/cpp     # Station, Captive Portal, NVS-Credentials
│   │   ├── MQTTClient.h/cpp      # MQTT Sub/Pub + Log-Queue (s_logQueue, logHook, Logger::setPublishHook)
│   │   └── WebServer.h/cpp       # REST-API, SSE, OTA, /api/finetune
│   └── ota/OTAUpdater.h/cpp
└── data/                         # LittleFS-Inhalt
    ├── index.html                # SPA: Dashboard/Presets/Settings/Maintenance + API-Sidebar-Link
    ├── config.json               # Deployment-Defaults (ohne WiFi-Credentials)
    └── certs/github_root_ca.pem
```

---

## 10. Nicht-funktionale Anforderungen

| Anforderung | Zielwert |
|---|---|
| Web-GUI Ladezeit | < 2 s im lokalen WLAN |
| SSE-Update-Latenz | ≤ 5 ms bei Zustandsänderung (sofortiger Push) |
| AutoTune-Dauer (full scan) | < 90 s (inkl. Inter-L + 4× Medium-Scan) |
| AutoTune-Dauer (Preset-Hit) | < 5 s |
| Fine-Tune-Dauer | < 15 s (typisch 2–3 Iterationen) |
| MQTT-Reconnect-Intervall | 5 s |
| Bootzeit bis WiFi verbunden | < 10 s |
| RAM-Auslastung | ~14 % (46 KB von 327 KB) |
| Flash-Auslastung | ~68 % (1076 KB von 1572 KB) |

---

## 11. Offene Punkte / Risiken

| # | Punkt | Priorität |
|---|---|---|
| 1 | SWR-Brücke: PCF8591-Rohwerte (vfwd/vrev) bei 10 W TX noch nicht verifiziert — abrufbar via `/api/status` und Serial-Monitor. Falls vfwd < tune_tx_level (3): SWR-Brücken-Signal zu schwach, tune_tx_level per Web-GUI anpassen | Hoch |
| 2 | EEPROM-Schreibvorgänge von taskWeb (deleteAll/deleteByFreq) nutzen Wire direkt — bekannte Ausnahme. Da selten aufgerufen, ist das Risiko gering. Saubere Lösung: DELETE_PRESET-Befehle in I2C-Queue routen | Niedrig |
| 3 | MD5 beim lokalen Upload: `crypto.subtle` nur in sicheren Kontexten — HTTP im LAN; Fallback: MD5 serverseitig nach Empfang | Mittel |
| 4 | `data/config.json` enthält keine WiFi-Credentials (leere Strings). Nach `uploadfs` verbindet sich das Gerät via NVS. Ersteinrichtung erfordert Captive Portal einmalig. | Info |

---

## 12. Status v3.8 — Implementiert

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
| 17 | SSE periodic push alle 2 s; Sofort-Push bei Zustandsänderung (≤ 5 ms) | ✅ |
| 18 | Polling-Fallback 5 s, SSE-Reconnect 2 s (close-before-reconnect verhindert mehrfache Verbindungen) | ✅ |
| 19 | Fine-Tune standalone (ab aktuellem L/C) via Web-GUI + MQTT `JC-4s/finetune` | ✅ |
| 20 | Fine-Tune: Verbose Schritt-Tabellen (L-Sweep / C-Sweep pro Iteration) auf Serial | ✅ |
| 21 | Fine-Tune: Ergebnis-Tabelle auf Serial nach Abschluss | ✅ |
| 22 | Fine-Tune: Frühzeitiger Abbruch bei RL > 60 dB (verhindert Drift in ADC-Rauschbereich) | ✅ |
| 23 | Preset-Auto-Speicherung nach Tune (SAVE_PRESET via I2C-Queue, inkl. `freq_kHz`) | ✅ |
| 24 | EEPROM-Write: Acknowledge-Polling statt fixer Delays (robust für Clone-EEPROMs) | ✅ |
| 25 | Preset RAM-Cache: `read()` / `findBest()` wire-frei → keine Race-Condition mit taskI2C | ✅ |
| 26 | Web-GUI: Preset-Tabelle auto-refresh 500 ms nach tuneState → DONE | ✅ |
| 27 | Final-SWR: 20 ms Settle vor Messung (Relay-Federrückstellung); Erfassung vor K-Tune OFF | ✅ |
| 28 | Coarse-Scan: band-spezifische L_max/C_max-Grenzen (160m–10m + 60m); Fallback = voller Bereich | ✅ |
| 29 | Preset-Frequenz: Rundung auf 20 kHz-Segment-Basis (`floor(f/20)*20`); max. 1 Preset/Segment | ✅ |
| 30 | EEPROM-Format v2: 7 Byte/Eintrag, Byte 6 = SWR×10; max. 36 Presets; Migrations-Check beim Start | ✅ |
| 31 | SWR je Preset: gespeichert nach Tune, angezeigt in Web-GUI Preset-Tabelle | ✅ |
| 32 | MQTT `JC-4s/freq`: Segment-Wechsel → Auto-Apply gespeichertes Preset (exakter Segment-Match) | ✅ |
| 33 | Coarse-Scan: Top-3 diverse Kandidaten (räumlich getrennt, ≥ 1 Coarse-Schritt Abstand) | ✅ |
| 34 | Phase 2.5 Medium-Scan: ±coarseStep bei step/8 Granularität; von allen Top-3 Kandidaten | ✅ |
| 35 | Phase 2.25 Inter-L Scan: L=16,32,48 bei allen C (coarse C step); 4. Medium-Kandidat | ✅ |
| 36 | Hardware-Init beim Boot: gestaffelte PCF8574-Schreibvorgänge (150 ms Abstände), Readback-Verifikation | ✅ |
| 37 | MQTT Retained-Message-Schutz: `g_relayInitDone`-Flag + 2 s Suppress-Fenster nach Connect | ✅ |
| 38 | MQTT Log-Forwarding: alle Log-Nachrichten auf `JC-4s/log` (FreeRTOS-Queue, Hook in Logger) | ✅ |
| 39 | Web-GUI: Fine-Tune- und K-Tune-Buttons immer blau (btn-primary); K-Tune weisser Ring wenn EIN | ✅ |
| 40 | Web-GUI Sidebar: API-Link-Button öffnet `/api/status` in neuem Tab | ✅ |
| 41 | Web-GUI Sidebar: Breite 88 px (war 72 px) — „MAINTENANCE" vollständig lesbar | ✅ |
| 42 | Web-GUI Sidebar: Klick auf Brand-Logo „GT / JC-4s" navigiert zum Dashboard | ✅ |
| 43 | Fine-Tune: Sweep-Tabellen und Ergebnis-Box via LOG_INFO (→ Serial + JC-4s/log) | ✅ |
| 44 | MQTT publish-Robustheit: Log-Drain max. 8 Einträge/Zyklus; `s_mqtt.loop()` nach Log-Drain und innerhalb `publishStatus()` (zwischen L_uH und C_pF) — verhindert stille TCP-Drops bei publish()-Bursts | ✅ |
| 45 | MQTT: IP-Adresse (`JC-4s/ip`) und MAC-Adresse (`JC-4s/mac`) bei jedem MQTT-Connect als retained Message publiziert — erleichtert Gerät-Identifikation nach DHCP-Wechsel oder Netzwerkwechsel | ✅ |
| 46 | Web-GUI: API-Sidebar-Link verwendet relativen Pfad `/api/status` statt hardcodierter IP — funktioniert korrekt nach DHCP-Adresswechsel | ✅ |
| 47 | Firmware-Version 1.2.0 als GitHub-Release `v1.2.0` publiziert (Items 44–46 enthalten); OTA-Update via Web-GUI Maintenance oder `POST /ota/github/install` | ✅ |
| 48 | WiFi: Zwei konfigurierbare Netzwerke in `/wifi.json` (LittleFS), je mit optionaler statischer IP (IP, Maske, GW, DNS); Seed-Konstanten (`WIFI_SEED1_*`, `WIFI_SEED2_*` in `config.h`) werden beim ersten Boot geschrieben; NVS-Credentials älterer Firmware werden automatisch migriert; Captive Portal zeigt beide Netzwerke mit Static-IP-Feldern; Web-GUI Einstellungen erlauben Änderung ohne Reboot | ✅ |
| 49 | Web-GUI Stats-Panel: WLAN-SSID (`wifiSsid` aus `/api/status`) zwischen RSSI und Uhrzeit angezeigt | ✅ |
| 50 | Fix: RSSI-Update (`g_state.rssi`) erfolgt unabhängig vom MQTT-Verbindungsstatus; MQTT-Publish hat eigenen Timer — RSSI wird im Web-GUI auch ohne erreichbaren MQTT-Broker korrekt angezeigt | ✅ |
| 51 | Web-GUI Stats-Panel: MQTT-Broker-Verbindungsstatus (`mqttConnected` aus `/api/status`) als `● Online` (grün) / `○ Offline` (orange) angezeigt; `MQTTClient::isConnected()` exponiert `s_mqtt.connected()` an WebServer | ✅ |
| 52 | WiFi: REST-API `GET /api/wifi` + `POST /api/wifi/0` / `POST /api/wifi/1` für Netzwerkkonfiguration; Web-GUI Settings zeigt zwei separate Netzwerk-Cards mit Static-IP-Toggle; Passwort wird bei GET nicht zurückgegeben (nur schreiben möglich) | ✅ |
| 53 | Shelly Plus1PM: EIN/AUS-Toggle-Button auf Dashboard; `shelly_url` in `config.json` konfigurierbar; Shelly-API via ESP32 proxied (`GET /api/shelly/status`, `POST /api/shelly/toggle`) — vermeidet Browser-CORS | ✅ |
| 54 | MQTT 80m-Auto-Shelly: Bei Segment-Wechsel via `JC-4s/freq` wird Shelly automatisch EIN geschaltet wenn 3500–4000 kHz, sonst AUS; Pending-Flag-Muster verhindert HTTP-Call im MQTT-Callback | ✅ |
| 55 | Preset EEPROM-Format v3: Bit 2 von C_mode = `shellyOn`; AutoTuner setzt Bit beim Speichern automatisch (80m=1, andere=0); rückwärtskompatibel (altes Bit war 0 = Shelly AUS) | ✅ |
| 56 | Web-GUI Presets-Tabelle: Shelly ●/○ Toggle-Spalte; `POST /api/presets/{freq}/shelly` zum manuellen Umschalten; `GET /api/presets` liefert `shellyOn`-Feld je Preset | ✅ |
| 57 | Web-GUI Stats-Panel: L- und C-Zelle zeigen zusätzlich den physikalischen Wert in µH bzw. pF (`calcLuH()` / `calcCpF()` aus `I2CController`); neue Felder `L_uH` / `C_pF` in `/api/status` und im SSE-Push (`/events`) | ✅ |
| 58 | WLAN-Modem-Sleep deaktiviert (`WiFi.setSleep(false)` in `connectStation()`, greift für Erst-Connect und Reconnect) — der Default-Modem-Sleep wartet das Beacon/DTIM-Intervall des APs ab und verursacht 70–150 ms Ping-/SSE-/MQTT-Jitter auf dem netzgespeisten Gerät | ✅ |
| 59 | Firmware-Version 1.3.0 als GitHub-Release `v1.3.0` publiziert (Items 57–58 enthalten); OTA-Update via Web-GUI Maintenance oder `POST /ota/github/install` | ✅ |
| 57 | `LOG_LEVEL_DEFAULT`-Vergleichsrichtung in `Logger.h` korrigiert (`<=` → unbedingte Makros wie [GT-Pod](../../GT-Pod)) — `LOG_ERROR`/`LOG_WARN` wurden beim Standard-Log-Level INFO (2) zuvor als No-Op wegkompiliert, unabhängig vom Laufzeit-Log-Level; die FreeRTOS-Log-Queue (Item 38) war davon nicht betroffen (2026-08-20) | ✅ |
| 60 | SWR-Messung mit Gleitkomma-Mittelwert statt `uint8_t`-Trunkierung (`measureSWR()`): nahe Anpassung ist Vrev nur 0–3 ADC-Counts, die Ganzzahl-Division quantisierte Return Loss in ~6-dB-Stufen und der Fine-Tuner jagte LSB-Rauschen. `TUNE_MEASUREMENTS` bleibt bei 8 (Coarse-Scan macht ~2000 Messungen — Träger muss die ganze Zeit anliegen) | ✅ |
| 61 | Fine-Tune-Robustheit: (a) Kandidaten-Bestätigung — die stärksten ~4 Punkte des Laufs werden am Ende mit `FINE_CONFIRM_SAMPLES`=6 Mittelung nachgemessen, der real haltbare gewinnt (verhindert Landung auf Rausch-Spitzen); (b) Plateau-Zentrierung (`PLATEAU_MARGIN_DB`) — flaches Minimum → Mittelpunkt statt Rand; (c) Start-Position als Fallback: Ergebnis nie schlechter als Ausgangspunkt; (d) `FINE_SETTLE_MS`=8 (war 3 — Relais-Prellen verursachte RL-Streuung); WARN wenn bestes RL < Schwelle | ✅ |
| 62 | Web-UI Reaktionszeit: SSE auf eigenem Listener `SSE_PORT`=81 — der dauerhaft offene EventSource-Socket blockierte den Single-Client-`WebServer` auf :80 sonst je ~5 s (`HTTP_MAX_DATA_WAIT`) pro Runde; `EventSource` in `index.html` zeigt jetzt auf `:81` | ✅ |
| 63 | Web-UI Reaktionszeit: sämtliche Shelly-HTTP-Aufrufe nach `taskMQTT` verlagert (`SHELLY_HTTP_TIMEOUT_MS`=1200, 60 s Backoff nach Fehler). `/api/shelly/status` liefert nur noch den Cache, `/api/shelly/toggle` ist optimistisch — der synchrone HTTP-Call im Handler fror den Webserver bei nicht erreichbarem Shelly 17 s pro Aufruf ein | ✅ |
| 64 | Firmware-Version 1.4.0 als GitHub-Release `v1.4.0` publiziert (Items 60–63 enthalten) | ✅ |
| 65 | Neue Hardware-Sense-Eingänge auf 0x39.P4-P7 (`SenseInputs`): F-PWR (Leistung am Koppler anliegend), Z-HIGH (Antennenimpedanz > 50 Ω), Z-LOW (< 50 Ω), PHASE (Vorzeichen U/I-Phasendifferenz, Polarität noch nicht kalibriert). `PCF8574_C_HI_INPUT_MASK`=0xF0 wird bei jedem Schreibzugriff auf 0x39 (`setLC()`, `SET_KTUNE`, Boot-Init) mit eingeblendet, damit die quasi-bidirektionalen Pins freigegeben bleiben — vorher wurden P4-P7 bei jedem Relais-Kommando aktiv auf LOW gezogen. Rohwerte (unkalibriert) in `/api/status`, SSE-Push, Serial `status`-Kommando und Web-UI Stats-Panel „Sense Inputs (raw)" — bewusst noch nicht in die AutoTuner-Logik eingebunden, bis die PHASE-Polarität feststeht | ✅ |
| 66 | Sense-Inputs als MQTT-Feedback: `JC-4s/feedback/fpwr`, `/zhigh`, `/zlow`, `/phase` (roh, unkalibriert), publiziert von `publishStatus()`. Die bestehende Änderungserkennung in `taskMQTT` (bisher L/C/mode/kTune/freq) berücksichtigt jetzt auch die 4 Sense-Bits — während Fine-Tune/AutoTune ändert sich L oder C praktisch bei jedem Schritt, wodurch die Sense-Werte automatisch pro Schritt mitpubliziert werden (kein Zusatzcode in `AutoTuner.cpp` nötig) | ✅ |
| 67 | Fix `runTune()` Phase 2.5 (Medium-Scan-Kandidatenvergleich): die Top-3-Coarse-Kandidaten + Inter-L-Kandidat wurden mit je einer einzelnen, ungemittelten Messung (5 ms Settle, 1 Messwert) verglichen — dieselbe Rauschanfälligkeit, die für die Fine-Tune-Bestätigung bereits behoben wurde (Item 61), war hier noch vorhanden. Reproduzierbar auf Hardware nachgewiesen (25 Ω resistive Last, 3530 kHz): `mediumScan()` fand einen echten guten C@TRX-Kandidaten (L=8 C=120 mode=1, RL 20,4 dB), die Einzelmessung bewertete ihn aber schlechter als einen mittelmäßigen C@ANT-Kandidaten → AutoTune endete zweimal reproduzierbar bei C@ANT/SWR 2,66 statt beim bekannten C@TRX-Match/SWR 1,00. Beide Vergleichsstellen nutzen jetzt `measureAvg()` (6-fache Mittelung, wie bei Fine-Tune) — nach dem Fix fand AutoTune auf Anhieb L=4 C=129 mode=1, SWR=1,00 | ✅ |
| 68 | Hardware-Kalibrierung `measureSWR()`: der Vorwärtsleistungs-Detektor (FOR) liefert nur FOR/2 relativ zum Rückwärts-Detektor — `vfwd` wird nach der Mittelung ×2 skaliert, bevor `rho = Vrev/Vfwd` gebildet wird (mit Clamp auf 255 für den angezeigten Rohwert). Ohne Korrektur las Rho ~2× zu hoch und Return Loss ~6 dB zu niedrig gegenüber dem echten Match — `DEFAULT_TUNE_THRESHOLD`=18 dB entspricht damit jetzt tatsächlich der im Kommentar dokumentierten SWR≈1,29, keine Anpassung der Schwelle nötig | ✅ |
| 69 | Kalibrierungs-Messreihe Sense-Inputs (5 W RF, 3530 kHz, 5 Lasten: 25 Ω, 100 Ω, 250 Ω, 50 Ω [~30 cm Zuleitung], 10 Ω): **F-PWR** bestätigt aktiv-High, keine Invertierung nötig (`1`=Leistung anliegend). **Z-HIGH/Z-LOW**: `zh=1,zl=0` markiert reproduzierbar (4/4 erfolgreiche Läufe) die Nähe zum echten Impedanz-Match, unabhängig von Fehlanpassungsrichtung/-betrag; bei der 10-Ω-Last (kein Match gefunden, vermutlich außerhalb des Anpassbereichs bei dieser Frequenz) trat das Muster korrekt nicht auf. `zh=0,zl=1` kam in keinem der 6 Mitschnitte (~5400 Samples) vor — Z-LOW ist strukturell eine Teilmenge von Z-HIGH, vermutlich ein zweites (höheres) Abweichungs-Schwellwert-Paar statt Richtungsanzeige „>50 Ω"/„<50 Ω"; ohne Schaltplan des Sense-Boards nicht abschliessend klärbar. **PHASE** kippt beim Sweep durch die Resonanz wie bei einem echten Vorzeichen-Detektor erwartet, ist aber genau am Nulldurchgang instabil (physikalisch plausibel); welcher Pegel kapazitiv vs. induktiv bedeutet, ist weiterhin offen. Alle 4 Sense-Inputs bleiben auf Anwenderwunsch vorerst reine Anzeige, nicht in die AutoTuner-Logik eingebunden. Messmethode: Python/paho-mqtt Hintergrund-Logger auf `JC-4s/#` statt Node-RED-Debug-Sidebar (deren Retention bei einem vollen Coarse-Scan zu früh überläuft) | ℹ️ Beobachtung |
| 70 | Hardware-Plattformwechsel: Seeed Studio XIAO **ESP32-C3 → ESP32-S3** (COM18, per USB-Seriell geflasht — neues Board, noch ohne angeschlossene Tuner-Hardware/EEPROM/PCF8574/PCF8591, bootet sauber in den Captive-Portal-Modus). `platformio.ini`-Env umbenannt (`seeed_xiao_esp32s3`), `I2C_SDA_PIN`/`I2C_SCL_PIN` auf die S3-Default-Pins (5/6) angepasst, `custom_partitions.csv` unverändert übernommen (passt weiterhin in die ersten ~4 MB des jetzt 8-MB-Flashs). Das inzwischen wirkungslose `CONFIG_FREERTOS_UNICORE=1` (nie im eigenen Code referenziert) wurde entfernt | ✅ |
| 71 | Dual-Core-Nutzung auf dem neuen ESP32-S3: alle 5 eigenen Tasks (`taskWeb`, `taskMQTT`, `taskI2C`, `taskTuner`, `taskSerial`) laufen jetzt per `xTaskCreatePinnedToCore()` auf Core 1, Core 0 bleibt dem WiFi/BT-Treiber des SDK vorbehalten (analog zum Pattern in anderen GT-Projekten). Status-LED (`LED_BUILTIN`, aktiv-low) zeigt den WLAN-Status: aus solange nicht verbunden, an (LOW) sobald `WiFi.status()==WL_CONNECTED`, geprüft 1×/s in `loop()` | ✅ |
| 72 | Inbetriebnahme ESP32-S3 mit angeschlossener Tuner-Hardware abgeschlossen: WLAN-Verbindung über Captive Portal hergestellt (192.168.0.198), MQTT/NTP verbunden. Fine-Tune-Testlauf bestätigt die komplette Kette (I2C-Timing, Relais, ADC-Brücke inkl. FOR/2-Korrektur, Kandidaten-Bestätigung, Dual-Core-Scheduling) funktioniert unverändert: `L=2 C=123 mode=1 SWR=1,00 RL=80 dB`, mehrere Nachbarpunkte ebenfalls bei RL 80 dB bestätigt (breites, stabiles Minimum). `WiFi.setSleep(false)` unverändert aktiv (Code-Review); beobachtete Ping-Jitter (3–98 ms) bei RSSI −80/−81 dBm ist wie beim alten C3-Gerät auf schwaches Signal zurückzuführen, nicht auf Modem-Sleep | ✅ |
| 73 | Erstes ESP32-S3-Board (COM18) als defekt diagnostiziert (u. a. Captive-Portal-AP war von einem iPhone nicht erreichbar) und durch ein zweites Board ersetzt — neue USB-Seriell-Adresse **COM7** (`platformio.ini`, `CLAUDE.md`, Abschnitt 9.1 aktualisiert). Firmware + Filesystem per USB-Seriell neu geflasht, bootet sauber, Tuner-Hardware am neuen Board angeschlossen und funktionsfähig bestätigt | ✅ |
| 74 | ADC-Hardware-Migration **PCF8591 → ADS1115** (16-Bit, I2C 0x48) abgeschlossen, nach mehrwöchiger Fehlersuche. Treiber nutzt die `Adafruit_ADS1X15`-Bibliothek (`GAIN_ONE` = ±4,096 V, ~125 µV/Count, `RATE_ADS1115_860SPS`); `readADS1115()` ersetzt deren unbeschränktes `while(!conversionComplete());` durch eine 20-ms-Zeitschranke + 1 s Retry-Throttle pro Kanal — ein einzelner gestörter Status-Read hing sonst `taskI2C` (und damit den ganzen Tuner) dauerhaft auf, nur per Reboot behebbar. **Ursache des eigentlichen Hardware-Fehlers**: das ADS1115 ACKte zuverlässig und `begin()` (Config-Register-Schreib-/Lesezugriff) funktionierte, aber jede tatsächliche AD-Wandlung schlug fehl (Conversion-Ready-Bit wurde nie gesetzt), sobald das ADS1115 den I2C-Bus mit den vier PCF8574-Relaistreibern teilte — reproduzierbar über mehrere Wochen, unabhängig von Pegelwandler-Typ (passiver BSS138 ebenso wie aktiver PCA9517-Bus-Buffer), Pull-up-Dimensionierung, Betriebsspannung (3,3 V/5 V) oder Entstörkondensatoren; ein isolierter Breadboard-Aufbau (gleicher Chip, gleicher TLC2272, mit und ohne Pegelwandler) funktionierte dagegen stets einwandfrei. **Lösung**: ADS1115 erhält einen eigenen, vollständig unabhängigen zweiten I2C-Bus (`Wire1`, XIAO-Pins D0/D1 = GPIO1/GPIO2, eigener dedizierter PCA9517-Buffer) statt sich den PCF8574-Bus (`Wire`, GPIO5/6) zu teilen — seitdem 100 % fehlerfrei, verifiziert über mehrere Minuten Dauerbetrieb und mit realer Testspannung (5 V → 4:1-Teiler → TLC2272 → ADS1115) auf beiden Kanälen (A0/A1) korrekt vermessen. Konstanten `I2C2_SDA_PIN`/`I2C2_SCL_PIN`/`I2C2_FREQ_HZ` in `config.h`; `I2C_FREQ_HZ` nach einem zwischenzeitlichen 50-kHz-Diagnoseversuch wieder auf 100 kHz zurückgesetzt (beide Busse). **Separater Fix im selben Zuge**: `otaLocalFW()` (`POST /ota/local/fw`) rief nach erfolgreichem Flash nie `ESP.restart()` auf (im Gegensatz zum benachbarten `otaLocalFS()`) — neue Firmware wurde zwar korrekt geschrieben und als bootfähig markiert, aber nie aktiv; das Gerät lief bis zum nächsten zufälligen Reset (Absturz oder manueller Stromzyklus) mit der alten Firmware weiter. Betraf das OTA-Verfahren generell, nicht nur diese Fehlersuche | ✅ |
| 75 | Erste reale On-Air-Validierung der ADS1115-Messkette (3530 kHz, 5 W, Item-74-Fix): Tuner meldete SWR 1,29 (RL 17,8 dB), TRX-eigenes Meter zeigte 1,4:1 — plausible Übereinstimmung zweier unabhängiger Brücken. Dabei bestätigt: K-Tune (0x39.P3) ist für einen Tune-Lauf nicht mehr erforderlich, da die ADS1115-Messbrücke (anders als die alte PCF8591-Sense-Schaltung) dauerhaft im HF-Pfad liegt, nicht erst nach K-Tune-Zuschaltung. `taskAutoTuner()` schaltet K-Tune daher nicht mehr automatisch EIN/AUS vor/nach AutoTune bzw. Fine-Tune (vorher: immer, unabhängig vom Button-Zustand). Manueller K-Tune-Button, `POST /api/ktune` und MQTT `JC-4s/ktune` bleiben für Diagnosezwecke unverändert bestehen — nur die automatische Schaltung im Tune-Ablauf entfällt | ✅ |
| 76 | `DEFAULT_TX_LEVEL` (minVfwd-Schwelle für „Träger vorhanden", unterhalb derer `measureSWR()` `swr=0` liefert und die Web-GUI „—" statt eines Wertes zeigt) war seit der ADS1115-Migration ein unkalibrierter Platzhalter (1300 — zu hoch, hätte sogar ein reales 1-W-Signal ausgeblendet); das laufende Gerät nutzte zudem noch den alten, persistierten Wert 3 aus `/config.json` (von vor der ADS1115-Migration), wodurch das Rauschen im Leerlauf (~5–7 Counts) als scheinbar gültiger SWR-Wert angezeigt wurde. Mit realen Messwerten kalibriert (Rauschen ~5–7 Counts, 1 W @ 3530 kHz ~242 Counts): neuer Default **50** (deutlicher Abstand zu beiden). Laufendes Gerät live auf 50 gesetzt; `/api/status` zeigt jetzt `swr=0.00` im Leerlauf | ✅ |
| 77 | AutoTune-Regression nach der ADS1115-Migration behoben (2026-11-02, reale Fehlersuche bei 5 W auf 3530 kHz): deutlich langsamer als mit dem alten PCF8591-Treiber und landete bei SWR 1,9–2,0 statt eines echten ~1,01-Matches nur wenige Schritte entfernt (L=0, C=51, mode=2 — live bestätigt: `vfwd=901 vrev=4`, durchgehend stabil, sogar als **breites Plateau** SWR=1,01 von C=32 bis C=80+). Vier unabhängig gerechtfertigte Fixes: **(1) Geschwindigkeit** — `Wire1` (ADS1115-Bus) auf 400 kHz Fast-Mode (unkritisch, da exklusiv genutzt); `readADS1115()` wartet die bekannte Wandlungszeit (~1,16 ms) erst ab, statt ab t=0 zu pollen. **(2) Race Condition** — `mediumScan()`/`fineTune()`/`measureAvg()` sendeten `READ_SWR` und lasen `g_state` nach einer festen 15–20-ms-Wartezeit, einer Schätzung der `measureSWR()`-Dauer (jetzt 16 ADS1115-Reads/Aufruf — die Schätzung konnte zu knapp sein, wodurch veraltete Werte vom vorherigen Rasterpunkt gelesen wurden). Neues `g_swrDoneSem`, von `taskI2C` erst nach tatsächlichem Abschluss von `READ_SWR` gegeben — alle Aufrufer blockieren jetzt darauf statt zu schätzen. **(3) Relais-Settle** — `coarseScan()`/`interLScan()`/`mediumScan()` nutzten noch die alte 3-ms-Settle-Zeit, die für Fine-Tune bereits als zu kurz erkannt und auf `FINE_SETTLE_MS`=8 erhöht wurde (Relais-Prellen), aber nie auf die früheren Phasen übertragen wurde. **(4) Kandidaten-Auswahl-Bug** — `addCandidate()`s „diverse Kandidaten"-Deduplizierung verglich mit striktem „<" gegen `coarse_step_l`/`coarse_step_c`; da aufeinanderfolgende Coarse-Scan-Punkte exakt einen Coarse-Schritt auseinanderliegen, wurden sie fälschlich als unterschiedliche Regionen behandelt — ein echtes, breites Match-Plateau fragmentierte sich dadurch über mehrere fast identische, einzeln schwächere Kandidaten, wodurch ein einzelner verrauschter Treffer anderswo einen der 3 Plätze gewinnen konnte. Zusätzlich verglich die Dedup-Logik den Modus nicht, sodass ein Kandidat von einem anderen Modus an ähnlicher L/C-Position überschrieben werden konnte. Fix: „<=" statt „<", plus Pflicht-Modus-Vergleich. Keine Einzelursache war abschließend isolierbar — alle vier Fixes sind unabhängig voneinander gerechtfertigt und bleiben bestehen | ✅ |
| 78 | AutoTune-Suchalgorithmus grundlegend ersetzt (2026-11-02), inspiriert von der öffentlich dokumentierten Suche des DL4JAL PicATU500v2 (Grundmatch/Submatch). `coarseScan()`+`interLScan()`+`mediumScan()`+`addCandidate()` (die fragile „einmalig Top-3-Kandidaten extrahieren"-Pipeline aus Item 77) ersetzt durch **`grundMatch()`**: ein geometrisches Verdopplungs-Raster (0,1,2,4,8,…,Maxwert) über den vollen bandlimitierten L/C-Bereich, beide Modi in einem Durchgang — L/C sind binär gewichtete Relais-Bitmasken, die Verdopplungsfolge ist also „ein Relais-Bit mehr zuschalten", keine willkürliche Wahl. ~100–300 statt ~2000 Messungen, deckt aber weiterhin den vollen Raum ab. Gefolgt von **`subMatch()`**: iterativer Zoom — Fenster ±4 Schritte um den aktuellen Bestwert, die 2 besten Einzelmessungen werden mit Mittelung (`SUBMATCH_CONFIRM_SAMPLES`=3) bestätigt, auf den Gewinner rezentriert, Schrittweite halbiert, wiederholt bis Schrittweite=1 oder eine bestätigte RL ≥ `SUBMATCH_EARLY_EXIT_RL`=30dB erreicht ist (Sicherheitsgrenze `SUBMATCH_MAX_LEVELS`=12). Jede Zoomstufe tastet ihre Umgebung frisch neu ab — ein einzelner Ausreisser (live bestätigt: 47dB Einzelmessung vs. stabile 12,7dB bei Nachmessung am selben Punkt) kann dadurch das Ergebnis nicht mehr dauerhaft verfälschen, anders als bei der alten einmaligen Kandidaten-Extraktion. Fallback auf den jeweils anderen Modus, falls das Zoom-Ergebnis `g_cfg.tune_threshold` nicht erreicht (bestehendes Feld wiederverwendet, keine neue Konfiguration). `fineTune()`/`measureAvg()`/`plateauCentre()`/`considerCand()` unverändert — bereits bewährt, erhalten jetzt nur einen besseren Startpunkt. `coarse_step_l`/`coarse_step_c` (AppConfig/REST/Settings-UI) bewusst unangetastet gelassen, aber vom neuen Algorithmus nicht mehr referenziert — keine Config-Schema- oder Frontend-Änderung in diesem Schritt. Rauchtest ohne HF: sauberer Durchlauf, korrekte Log-Sequenz (Grundmatch → Submatch → Fallback → Fine-Tune), keine Hänger, ~45s Gesamtlaufzeit. Echte Validierung mit realem HF-Signal: siehe Item 79 — erste Läufe deckten zwei weitere Bugs auf, die dort behoben wurden | ⏳ |
| 79 | Zwei weitere AutoTune-Bugs live aufgedeckt und behoben (2026-11-02, 50-Ω- und 25-Ω-Lastwiderstand, Nachtest von Item 78): **(1)** `grundMatch()` verfolgte pro Modus nur einen einzigen laufenden Bestwert ohne Rausch-Schutz (anders als `subMatch()`, das seine Top-2-Kandidaten bereits mit Mittelung nachbestätigt) — ein einzelner späterer Rausch-Ausreisser konnte eine echte, breite ~45-dB-Match-Region (live am 25-Ω-Fall bestätigt: L=4,C=147,C@TRX, SWR 1,01, live nachgemessen reproduzierbar von L=0–8/C=100–160) dauerhaft und spurlos überschreiben — derselbe Fehlertyp wie der ursprüngliche `addCandidate()`-Bug, nur eine Phase früher. Fix: `grundMatch()` verfolgt jetzt pro Modus die 2 besten Einzelmessungen und bestätigt beide mit Mittelung (`SUBMATCH_CONFIRM_SAMPLES`), bevor ein Gewinner feststeht. **(2)** Selbst damit wurde der echte Match noch verfehlt — `FINE_SETTLE_MS` (8 ms) war für Fine-Tunes kleine Einzelschritt-Relaisänderungen kalibriert, aber Grundmatchs geometrisches Raster macht weit größere Sprünge, u. a. springt L bei jeder C-Erhöhung vom Maximalwert zurück auf 0 (ein grosser gleichzeitiger Mehrfach-Relais-Wechsel) — das liess ausgerechnet die Punkte direkt nach diesem Sprung (L=0–8, genau wo der echte Match lag) als Quasi-Totalreflexion erscheinen, obwohl dieselben Punkte bei direkter Nachmessung Sekunden später zuverlässig ~45 dB zeigten. Neuer, separater `GRUNDMATCH_SETTLE_MS`=20 (identisch mit der bereits bewährten Settle-Zeit von `measureAvg()`) behebt dies. Live verifiziert (25-Ω-Last): Grundmatch meldet jetzt in beiden Modi korrekt ~45 dB; ein vollständiger, eigenständiger AutoTune-Lauf landet bei SWR=1,12 (RL=31,2 dB) — ohne jeden Hinweis, trifft den manuell gefundenen echten Optimalpunkt (L=4/C=147, SWR 1,01) innerhalb der Raster-/Messauflösung | ✅ |
| 80 | Zwei weitere, beim Live-Test von Item 79 aufgefallene AutoTune-Bugs behoben (2026-11-02, 25-Ω-Last): **(1)** `fineTune()` misst seinen eigenen Startpunkt immer frisch neu, statt dem vom Aufrufer bereits bestätigten RL-Wert zu vertrauen. Direkt an einem echten Nullpunkt beträgt Vrev nur wenige ADC-Counts — genau am Rauschteppich —, weshalb selbst eine gemittelte Nachmessung spürbar schlechter ausfallen kann als die Realität (live bestätigt: Submatch bestätigte einen Punkt mit 45,1 dB, `fineTune()`s eigene frische Basismessung las so niedrig, dass die „nie schlechter als der Start"-Garantie nur einen bereits verschlechterten Wert schützte). `runTune()` misst das `fineTune()`-Ergebnis jetzt nach und behält Submatchs vorher bestätigtes Resultat, falls `fineTune()` es tatsächlich nicht übertrifft. **(2)** `subMatch()` nutzte für jede Zoomstufe `FINE_SETTLE_MS` (8 ms, korrekt für `fineTune()`s eigenen finalen ±1-Schritt-Walk), aber Submatchs frühe Stufen machen weit größere Relais-Sprünge (Anfangsschrittweite oft im zweistelligen Bereich) — dieselbe Unter-Settle-Problematik wie bei `grundMatch()`. Live bestätigt: Grundmatch fand einen echten 38-dB-Punkt, aber Submatchs eigener Stufe-0-Rescan genau dieser Umgebung (mit `FINE_SETTLE_MS`) maß nur ~5 dB und wanderte in ein schwächeres, aber genuin konvergiertes lokales Optimum ab, ohne Weg zurück. Nutzt jetzt durchgehend `GRUNDMATCH_SETTLE_MS`. Live verifiziert (25-Ω-Last): ein vollständiger AutoTune-Lauf bleibt jetzt durchgehend auf dem echten Match — Grundmatch L=2/C=128 44,9 dB → Submatch L=1/C=96 44,9 dB (Early-Exit) → Fine-Tune SWR=1,05 (RL=32,3 dB), ~68 s gesamt, keine Abdrift mehr in ein schwächeres Optimum | ✅ |
| 81 | Finale AutoTune-Anzeige gemittelt statt Einzelmessung (2026-11-02): `taskAutoTuner()` wandte das von der Suche gewählte L/C/mode korrekt an, maß das für Anzeige/MQTT verwendete finale SWR/RL aber mit einem einzelnen, ungemittelten `READ_SWR`. Direkt an einem echten Nullpunkt beträgt Vrev nur wenige ADC-Counts — nahe am Rauschteppich —, weshalb genau dieser eine zusätzliche Einzelmesswert spürbar schlechter ausfallen kann als der bereits von der Suche bestätigte, gemittelte Wert, obwohl die gewählte L/C-Einstellung tatsächlich korrekt ist. Nutzerbeobachtung: Fine-Tune zeigte sichtbar mehrere SWR≈1,01-Messwerte, der Tuner blieb aber bei einer Anzeige von 1,05 stehen — exakt dieselbe L/C-Kombination, die Submatch bereits mit 45,1 dB bestätigt hatte, nur eine schlechtere letzte Einzelmessung davon. Nutzt jetzt `measureAvg(FINE_CONFIRM_SAMPLES)` für das Endergebnis; SWR wird aus dem gemittelten RL abgeleitet, sodass der angezeigte/veröffentlichte Wert mit dem übereinstimmt, was der Algorithmus tatsächlich bestätigt hat | ✅ |
| 82 | AutoTune-Redesign (Items 77–81) als Gesamtergebnis live bestätigt (2026-11-02, Sessionabschluss): zweiter vollständiger Lauf auf der 25-Ω-Last liefert `AutoTune DONE – L=2 C=124 mode=1 SWR=1.01` — nahezu identisch mit dem manuell gefundenen Optimalpunkt (L=4/C=147, SWR 1,01). Dabei bewährte sich der Moden-Fallback (Item 78) erstmals in der Praxis: Submatch wanderte in Modus 2 wieder in ein schwächeres, aber echtes lokales Optimum ab (~6 dB) und unterschritt die Schwelle, der automatische Fallback auf Modus 1 fand daraufhin sauber den echten Match (45,4 dB, Early-Exit) — das Gesamtsystem lieferte trotz des einzelnen Fehlschlags ein korrektes Endergebnis. **Offener Punkt (niedrige Priorität):** Submatchs Abdrift in ein falsches lokales Optimum tritt trotz der `GRUNDMATCH_SETTLE_MS`-Fixes (Items 79–80) weiterhin gelegentlich auf, wurde diesmal in Modus 2 statt wie zuvor in Modus 1 beobachtet — Ursache nicht abschliessend geklärt (Settle-Zeit als alleinige Erklärung ausgeschlossen). Der Moden-Fallback fängt dies in der Praxis ab, behebt aber nicht die zugrundeliegende Abdrift; relevant würde es nur, falls beide Modi in einem Lauf abdriften. **Separate Feldbeobachtung:** WLAN-RSSI verschlechterte sich im Lauf der Session (−55 auf −90 dBm, zeitweise 50 % Ping-Verlust) — Nutzer-Theorie: Gerät hat sich mit einem entfernteren AP im Mesh/Multi-AP-Netz verbunden und roamt nicht automatisch; kein Firmware-Fehler, ein Reboot erzwingt Neuverbindung zum nächsten AP. Geräteseitige I2C-/Mess-Timings blieben dabei nachweislich unbeeinflusst (Log-Zeitstempel durchgehend konsistent) — nur der Beobachtungspfad (HTTP/MQTT von dieser Session aus) war betroffen | ✅ |
| 83 | Ursache für den in Item 82 offen gelassenen Punkt gefunden und behoben (2026-11-03, 100-Ω-Last): Referenzpunkt live bestätigt (L=39/C=57/C@ANT, SWR 1,01, RL 45 dB). AutoTune landete reproduzierbar bei L=128/C=64/mode=2 — von Grundmatch **und** von Submatchs eigener Stufe-0-Nachbestätigung übereinstimmend mit 36,7–39,4 dB gemessen (zwei unabhängig gemittelte Messungen), fineTune mass am selben Punkt aber nur 2,8–3,6 dB; die Regressions-Sperre (Item 80) hielt daraufhin (korrekt nach ihrer eigenen Logik, aber auf Basis der falschen früheren Werte) am falschen 36,7-dB-Ergebnis fest. Kein einfaches Messrauschen — zwei übereinstimmend falsche, gemittelte Messungen vs. drei übereinstimmend korrekte. Per temporärem Firmware-Diagnoseaufbau (`SETTLE_TEST`-Kommando, einmaliger direkter Sprung vom Referenzpunkt zum Verdachtspunkt mit Rohwert-Logging in hoher zeitlicher Auflösung, nach Gebrauch wieder entfernt) direkt am Gerät geprüft: die Analogkette erreicht ihren wahren Wert (ob gut oder schlecht) innerhalb von ~90 ms — weit innerhalb der bestehenden 20-ms-Relais-Settle-Zeit (Items 79–80) und damit keine Erklärung. Das räumte zugleich die bisherige Arbeitshypothese „ADC-Rauschteppich bei hohem RL" aus (Item 82s offener Punkt, Kommentar in `subMatch()`): der tatsächliche Vrev am Verdachtspunkt lag bei ~400 Counts, zwei Grössenordnungen über dem Rauschteppich (~5–7 Counts) — keine Grauzone, sondern eindeutig zwei verschiedene Messergebnisse am selben Punkt. **Tatsächliche Ursache:** sämtliche `xSemaphoreTake(g_swrDoneSem, pdMS_TO_TICKS(100))`-Aufrufe (in `presetSearch()`, `grundMatch()`, `subMatch()`, `fineTune()` ×2, `measureAvg()`) prüften den Rückgabewert nie — bei Zeitüberschreitung lief der Code einfach mit dem noch in `g_state` stehenden Wert der **vorherigen** Messung weiter. Per Instrumentierung bestätigt: `measureSWR()`s tatsächliche Dauer über den 100-kHz-`Wire1`-Bus liegt konstant bei ~108 ms — die 100-ms-Wartezeit lief damit praktisch bei jeder einzelnen Messung im gesamten AutoTune-Lauf ab, nicht nur bei diesem einen Punkt. Fix: alle 6 Stellen nutzen jetzt `portMAX_DELAY` statt der festen 100-ms-Schranke — `taskI2C` gibt das Semaphor ohnehin garantiert, `readADS1115()`s eigene 20-ms-Zeitschranke pro Konversion verhindert bereits ein echtes Hängenbleiben. Live sofort bestätigt (zwei aufeinanderfolgende vollständige Läufe, 100-Ω-Last): Grundmatch selbst findet jetzt direkt den echten Match (L=32/C=64/mode=2, RL 46,0–46,2 dB), keine Abweichung mehr zwischen den Messphasen, kein Eingreifen der Regressions-Sperre mehr nötig — `AutoTune DONE – L=32 C=64 mode=2 SWR=1.01 RL=46,9 dB`, deckungsgleich mit dem Referenzpunkt. Die Incumbent-Verfolgung in `subMatch()` (Item 82) bleibt als zusätzliche, jetzt korrekt begründete Absicherung gegen echtes Einzelmessungs-Rauschen bestehen, war aber nicht die eigentliche Fehlerursache | ✅ |
| 84 | Breiter Validierungs-Durchlauf nach dem Item-83-Fix (2026-11-03, 8 reale ohmsche Lasten nacheinander, 5 W RF, je ein frischer, eigenständiger AutoTune-Lauf ohne Hinweise): durchgehend saubere, konsistente Ergebnisse, keine einzige Stale-Read-Anomalie, keine fehlerhafte Regressions-Sperren-Intervention. Ergebnisse (Last → L/C/Modus, SWR, RL): 10 Ω → L=0/C=256/mode=1, 1,01, 46,1 dB. 25 Ω → L=3/C=128/mode=1, 1,01, 44,8 dB. 50 Ω → L=2/C=64/mode=2, 1,01, 45,5 dB. 100 Ω → L=32/C=64/mode=2, 1,01, 45,5–46,9 dB (zwei Läufe). 250 Ω → L=96/C=48/mode=2, 1,01, 46,1 dB. 400 Ω → L=128/C=32/mode=2, 1,01, 47,1 dB. 750 Ω → L=192/C=28/mode=2, 1,01, 46,4 dB. 1500 Ω → L=261/C=18/mode=2, 1,01, 46,5 dB. Bei 250 Ω, 750 Ω und 1500 Ω fand Grundmatchs grobes Raster jeweils nur einen mittelmässigen Punkt (RL 17,9–23,6 dB) — Submatchs feinerer Zoom fand den echten Match in diesem Fenster sofort (kein Abdriften, keine mehreren Zoomstufen nötig), genau wie beim Redesign (Item 78) beabsichtigt. `freq_kHz` war während dieser Session nicht gesetzt (kein CAT/MQTT-Frequenz-Feed aktiv) — Grundmatch/Submatch liefen daher mit den vollen bandunabhängigen L/C-Grenzen, nicht einem bandspezifisch reduzierten Bereich; die 10-Ω-Last fand diesmal einen echten Match (frühere Kalibrierung, Item 69, hatte bei 10 Ω keinen gefunden — vermutlich ein Symptom des jetzt behobenen Item-83-Bugs, nicht eine echte Bereichsgrenze des Kopplers) | ✅ |
| 85 | Vorwärts-/Reflektierte Leistung (Watt) hinzugefügt (2026-11-05): `measureSWR()` berechnet zusätzlich zu SWR/RL aus `vfwd`/`vrev` (ADS1115-Counts × 125 µV/Count) über eine Kalibrierkurve `powerFwdW`/`powerRevW`, neue Felder in `TunerState`, `/api/status`, SSE-Push und MQTT (`JC-4s/power/fwd`, `JC-4s/power/rev`). Web-GUI Stats-Panel zeigt beide neu zwischen RL/Freq und L/C (Label „P fwd (W)"/„P rev (W)", DE „P vor (W)"/„P refl (W)"). **Kalibrierung:** erster Versuch mit der publizierten WaveNode-HF-1/UHF-1-Kalibrierkurve (POWER_CAL_3-Datenblatt) schlug fehl — deren Spannungs-Leistungs-Beziehung ist eine saubere Quadratwurzel-Beziehung (P=k·V², k≈200 W/V² über den gesamten publizierten Bereich bestätigt), aber mit einer realen 5-W-Referenz (Elecraft KX3, 50-Ω-Last) ergab sich ein ~3,5× falscher Wert (1,4 W statt 5 W) — dieser Koppler/Verstärker/Teiler-Signalweg hat eine eigene, andere Übertragungskennlinie als WaveNodes Referenzschaltung. Stattdessen direkt am Gerät mit 6 echten Messpunkten (1/2/3/5/7/10 W, 50-Ω-Last, SWR 1,01–1,02) kalibriert: vfwd=260/519/718/970/1221/1486 Counts. Der lokale Exponent zwischen benachbarten Punkten steigt gleichmässig von ~1,0 (1–2 W) auf ~1,8 (7–10 W) — konsistent mit dem erwarteten Übergang eines Dioden-Detektors vom Quadratgesetz-Bereich (niedriger Pegel, U∝P) zum linearen Bereich (höherer Pegel, U∝HF-Spannungsamplitude), kein Messrauschen. Stückweise lineare Interpolation über diese 6 Punkte (gleiche Kurve für `vrev`, symmetrischer Signalweg); Werte über 10 W extrapolieren linear über das letzte Segment hinaus und sind bis zu einer Nachkalibrierung mit höherer Leistung nur indikativ. **Bekannte Einschränkung:** die KX3-Leistungsangaben sind Frontplatten-Sollwerte, nicht mit einem Inline-Leistungsmesser nachgemessen — die gesamte Kurve sollte bei Gelegenheit damit nachkalibriert werden; Nutzer plant ausserdem spätere Kalibrierpunkte bis 1000 W mit einem separaten Verstärker | ✅ |
| 86 | Risiko beim FS-Flash entdeckt und behoben (2026-11-05, im Zuge von Item 85s `uploadfs`): `data/config.json` (ins Repo eingecheckte Vorlage für einen Erstboot ohne vorhandene Konfigurationsdatei) enthielt noch `tune_tx_level: 3`, den veralteten Vor-Kalibrierungswert von vor Item 76 (aktueller korrekter Default in `config.h`: `DEFAULT_TX_LEVEL`=50) — da jeder `uploadfs`/`POST /ota/local/fs` die gesamte LittleFS-Partition überschreibt, wurde der live kalibrierte Wert durch diesen veralteten Vorlagenwert ersetzt, sobald die Datei im neuen Image vorhanden war. Per `POST /api/config` sofort auf 50 zurückgesetzt; `data/config.json` im Repo ebenfalls korrigiert, damit zukünftige FS-Flashes das nicht wiederholen. **Deutlich gravierenderer Nebenbefund:** derselbe FS-Flash hat zusätzlich `/wifi.json` gelöscht (nicht im Repo/Build enthalten, nur zur Laufzeit geschrieben) — das Gerät ging daraufhin in den `GT-JC-4s-Setup`-Captive-Portal-Modus, WLAN mangels Zugangsdaten verloren; Nutzer musste SSID/Passwort manuell über den Captive Portal neu eingeben, um das Gerät zurück ins Netz zu holen. Kein automatischer NVS-Fallback wie bei Item 7 dokumentiert griff hier spürbar — falls Item 48s NVS→`/wifi.json`-Migration tatsächlich nur einmalig (beim ersten Boot nach dem Firmware-Update) läuft und nicht bei jedem fehlenden `/wifi.json`, würde das diesen Befund erklären, aber das ist nicht abschliessend verifiziert. **Offener Punkt:** kein Firmware-Fix in diesem Schritt — jeder künftige FS-Flash trägt weiterhin das Risiko, `/wifi.json` zu löschen; vor jedem `uploadfs`/`/ota/local/fs` idealerweise WLAN-Zugangsdaten griffbereit haben bzw. physischen Zugang zum Gerät sicherstellen | ✅ |
| 87 | Terminologie korrigiert: „Rev" → „Ref" für reflektierte Leistung (2026-11-13), auf Nutzerhinweis hin, dass „REF" (nicht „REV") die übliche Abkürzung für „reflektiert" ist. Umbenannt in allen neuen Bezeichnern aus Item 85: `powerRevW`→`powerRefW` (`TunerState`, `SWRResult`, `/api/status`, SSE), `MQTT_PUB_PWR_REV`→`MQTT_PUB_PWR_REF` (`JC-4s/power/rev`→`JC-4s/power/ref`), Web-GUI-Element-IDs und i18n-Labels (`lbl_pwr_rev`→`lbl_pwr_ref`, „P rev (W)"/„P refl (W)" → einheitlich „P ref (W)" in EN und DE). Das seit langem etablierte `vrev`/„Vrev" (ADS1115-Rohkanal A1, siehe `I2CController.cpp`/`state.h`) bewusst NICHT umbenannt — eigenständige, unabhängig gewachsene Benennung, nicht Teil dieser Änderung. Zusätzlich auf Nutzerwunsch die Stats-Panel-Reihenfolge geändert: „P fwd"/„P ref" jetzt direkt unter der SWR-Grossanzeige (vor RL/Freq), L/C bleiben in der letzten Zeile. **Weiterer FS-Flash-Vorfall (gleicher Tag, erneut `/wifi.json` verloren):** Gerät lief diesmal an einem anderen Standort/Netz (`GTrappers`, statische IP 192.168.100.135, zuvor 192.168.0.97/`hb9czf`) mit sehr schwachem WLAN-Signal (RSSI −85 bis −95 dBm); zwei FW-Upload-Versuche schlugen mit Verbindungsabbruch fehl (curl exit 56), bevor einer bei RSSI −86 dBm durchging — das Gerät selbst blieb bei den fehlgeschlagenen Versuchen unversehrt (kein Reboot ohne abgeschlossenen `Update.end()`). Der nachfolgende FS-Flash hat erneut `/wifi.json` gelöscht, Nutzer musste wieder manuell über den Captive Portal verbinden — bestätigt Item 86s Befund als reproduzierbar, nicht einmalig | ✅ |
| 88 | `data/config.json`-Vorlage komplett aus dem Repo entfernt (2026-11-13): nach dem Item-87-Vorfall reverteten FS-Flashes nicht nur `tune_tx_level` (Item 86) sondern auch `mqtt_server` auf den fest einprogrammierten Wert `192.168.0.172` — das Gerät lief zu diesem Zeitpunkt aber am Standort „GTrappers" mit Broker `192.168.100.110`; `mqttConnected` wurde `false`, bis der Wert per `POST /api/config` manuell zurückgesetzt wurde. Da das Gerät nachweislich an mehreren Standorten mit unterschiedlichen MQTT-Brokern betrieben wird, ist JEDER fest einprogrammierte Wert in der Vorlage für mindestens einen Standort falsch. Strukturelle Lösung statt erneutem Wert-Patch: `data/config.json` komplett aus `data/` entfernt — `Config::load()` fällt bei fehlender Datei ohnehin bereits auf sinnvolle einkompilierte Defaults zurück (siehe `applyDefaults()` in `AppConfig.cpp`). Ein FS-Flash setzt die Konfiguration damit auf Defaults zurück statt auf einen falschen, aber gültig aussehenden anderen Standort — weiterhin ein manuelles `POST /api/config` nach jedem FS-Flash nötig, aber nicht mehr das Risiko einer stillen Fehlverbindung an einen falschen Broker | ✅ |
