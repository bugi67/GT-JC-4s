# GT-JC-4s — Automatischer Antennentuner-Controller

ESP32-basierte Firmware für einen automatischen HF-Antennentuner (L/C-Netzwerk) mit WLAN-Fernsteuerung, MQTT-Integration und Web-UI.

## Hardware

| Komponente | Details |
|---|---|
| Mikrocontroller | Seeed XIAO ESP32-C3 |
| I/O-Erweiterung | PCF8574 (I2C) — Relais-Steuerung |
| ADC | AD8591 (I2C) — SWR-Messung |
| Netzwerk | WiFi 802.11 b/g/n |

**Pinbelegung** (siehe [src/config.h](src/config.h)):
- SDA / SCL: I2C-Bus für Relais und ADC
- Spannungsversorgung: 3,3 V

## Funktionen

- **Automatisches Tuning** in drei Phasen:
  1. Preset-Suche (gespeicherte Bandkombinationen)
  2. Grobabtastung (L/C-Raster über alle 3 Modi)
  3. Feinabstimmung (±4 Schritte um das Optimum)
- **42 Presets** im I2C-EEPROM (0x50), sortiert nach Frequenz
- **Web-UI** mit Dashboard, Presets, Einstellungen und Wartung
  - Sprache: Englisch (Standard) / Deutsch umschaltbar
  - Theme: Dark Mode (Standard) / Hell umschaltbar
- **REST API** & Server-Sent Events (SSE) für Echtzeit-Statusupdates
- **MQTT** Publish/Subscribe (`JC-4s/`-Topic)
- **OTA-Updates** direkt von GitHub-Releases oder per lokalem Datei-Upload
- **WLAN-Captive-Portal**: Beim ersten Start (oder ohne gültige Zugangsdaten) öffnet das Gerät einen Accesspoint mit automatischer Weiterleitung zur Konfigurationsseite

## Voraussetzungen

- [PlatformIO](https://platformio.org/) (VSCode-Extension oder CLI)
- USB-Verbindung zum XIAO ESP32-C3

## Build & Flash

```bash
# Firmware bauen und flashen
pio run --target upload

# Weboberfläche (LittleFS) bauen und flashen
pio run --target buildfs
pio run --target uploadfs

# Seriellen Monitor öffnen (115200 Baud)
pio run --target monitor
```

## Erstkonfiguration (Captive Portal)

Beim ersten Start oder wenn keine gültigen WLAN-Zugangsdaten gespeichert sind, öffnet das Gerät automatisch einen Accesspoint:

- **SSID**: `GT-JC-4s-Setup`
- **Passwort**: siehe [src/config.h](src/config.h) → `AP_PASSWORD`

Alle Geräte im Netz werden automatisch auf die Konfigurationsseite weitergeleitet (Captive Portal). Dort SSID und Passwort eingeben und speichern — das Gerät startet anschließend neu und verbindet sich mit dem WLAN.

Die Konfigurations­daten (WLAN, MQTT, Tuner-Parameter) werden in LittleFS als `/config.json` gespeichert und können jederzeit über die Web-UI unter **Settings** aktualisiert werden.

Hardwarekonstanten (I2C-Adressen, L/C-Werte, MQTT-Root-Topic) werden in [src/config.h](src/config.h) definiert.

## Web-UI

Nach erfolgreicher WLAN-Verbindung ist die Oberfläche unter der IP-Adresse des Geräts erreichbar. Das Interface ist an Shelly Cloud angelehnt (dreispaltig: Sidebar + Hauptbereich + Echtzeit-Statsleiste).

| Seite | Inhalt |
|---|---|
| Dashboard | L/C-Schieberegler, Tuner-Modus, AutoTune-Button |
| Presets | Liste gespeicherter Abstimmungen, laden / löschen |
| Settings | WLAN, MQTT, Tuner-Parameter, OTA-URL, Log-Level |
| Maintenance | GitHub-OTA, lokaler Datei-Upload, Systemneustart |

Sprache (EN/DE) und Theme (Dark/Light) werden über Buttons in der Sidebar umgeschaltet und im Browser-LocalStorage gespeichert.

## REST API

| Methode | Pfad | Beschreibung |
|---|---|---|
| GET | `/api/status` | Aktueller Zustand (JSON) |
| POST | `/api/tune` | L, C, Modus manuell setzen |
| POST | `/api/autotune` | Automatisches Tuning starten / abbrechen |
| GET | `/api/presets` | Alle Presets auflisten |
| DELETE | `/api/presets/:freq` | Preset löschen |
| DELETE | `/api/presets` | Alle Presets löschen |
| GET | `/api/config` | Konfiguration lesen |
| POST | `/api/config` | Konfiguration speichern |
| POST | `/api/reboot` | Gerät neu starten |
| GET | `/events` | SSE-Stream (Echtzeit-Updates) |
| POST | `/ota/local/fw` | Firmware lokal hochladen |
| POST | `/ota/local/fs` | LittleFS lokal hochladen |
| GET | `/ota/github/check` | GitHub auf neue Version prüfen |
| POST | `/ota/github/install` | Update von GitHub installieren |

## MQTT

Root-Topic: `JC-4s/`

**Befehle (Subscribe):**

| Topic | Wert |
|---|---|
| `JC-4s/L` | Spulenwert setzen |
| `JC-4s/C` | Kondensatorwert setzen |
| `JC-4s/tunermode` | Tuner-Modus (1–3) |
| `JC-4s/freq` | Frequenz (kHz) |
| `JC-4s/tune` | AutoTune auslösen |

**Status (Publish):**

`JC-4s/feedback/{L,C,tunermode}`, `JC-4s/{L_uH,C_pF,swr,rssi,id,tune/status,tune/progress}`

## Serieller Monitor

| Befehl | Funktion |
|---|---|
| `log error/warn/info/debug` | Log-Level setzen |
| `log?` | Aktuellen Log-Level abfragen |
| `status` | L, C, Modus, Frequenz, SWR ausgeben |
| `reboot` | Gerät neu starten |

## OTA-Updates

Firmware-Releases werden über GitHub Actions automatisch gebaut und veröffentlicht (Tag `v*.*.*`). Das Gerät lädt Firmware- und LittleFS-Images über HTTPS von GitHub herunter und verifiziert MD5-Prüfsummen vor dem Flashen.

Alternativ können `.bin`-Dateien direkt über die Web-UI (Seite **Maintenance**) hochgeladen werden.

## Partitionstabelle

| Partition | Größe |
|---|---|
| NVS | 20 KB |
| OTA-Metadaten | 8 KB |
| App OTA-0 / OTA-1 | je 1,5 MB |
| LittleFS (Web-Assets + config.json) | 960 KB |

## Lizenz

Siehe Repository-Einstellungen auf GitHub.
