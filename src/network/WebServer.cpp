#include "WebServer.h"
#include "../config.h"
#include <time.h>
#include "../cfg/AppConfig.h"
#include "../logger/Logger.h"
#include "../state.h"
#include "../tuner/I2CController.h"
#include "../tuner/PresetStore.h"
#include "../ota/OTAUpdater.h"
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <Update.h>

::WebServer WebUI::s_server(WEB_PORT);

// SSE state — one active client, pushed non-blocking from task loop
WiFiClient            WebUI::s_sseClient;
float                 WebUI::s_sseLastSwr  = -1.0f;
TunerState::TuneState WebUI::s_sseLastTune = TunerState::TuneState::IDLE;
TunerState::OtaState  WebUI::s_sseLastOta  = TunerState::OtaState::IDLE;
uint16_t              WebUI::s_sseLastFreq = 0xFFFF;
uint16_t              WebUI::s_sseLastL    = 0xFFFF;
uint16_t              WebUI::s_sseLastC    = 0xFFFF;
uint8_t               WebUI::s_sseLastMode  = 0xFF;
bool                  WebUI::s_sseLastKTune = false;
int8_t                WebUI::s_sseLastRssi  = 0;
unsigned long         WebUI::s_sseLastHb   = 0;

// ── Helpers ──────────────────────────────────────────────────────────────────

void WebUI::sendJSON(int code, const String& json) {
    s_server.sendHeader("Access-Control-Allow-Origin", "*");
    s_server.send(code, "application/json", json);
}

void WebUI::sendError(int code, const char* msg) {
    String j = "{\"error\":\"";
    j += msg;
    j += "\"}";
    sendJSON(code, j);
}

String WebUI::buildStatusJSON() {
    StaticJsonDocument<512> doc;
    StateLock lock;
    doc["L"]           = g_state.L;
    doc["C"]           = g_state.C;
    doc["mode"]        = g_state.mode;
    doc["freq_kHz"]    = g_state.freq_kHz;
    doc["swr"]         = serialized(String(g_state.swr, 2));
    doc["returnLoss"]  = serialized(String(g_state.returnLoss, 1));
    doc["vfwd"]        = g_state.vfwd;
    doc["vrev"]        = g_state.vrev;
    doc["kTune"]       = g_state.kTune;
    doc["tuneState"]   = (int)g_state.tuneState;
    doc["tuneProgress"]= g_state.tuneProgress;
    doc["otaState"]    = (int)g_state.otaState;
    doc["otaProgress"] = g_state.otaProgress;
    doc["rssi"]        = g_state.rssi;
    doc["fwVersion"]   = g_state.fwVersion;
    time_t now = time(nullptr);
    doc["ntpSynced"]   = (now > 1000000000UL);
    if (now > 1000000000UL) {
        struct tm ti;
        localtime_r(&now, &ti);
        char tstr[20];
        strftime(tstr, sizeof(tstr), "%Y-%m-%d %H:%M:%S", &ti);
        doc["localTime"] = tstr;
    } else {
        doc["localTime"] = "";
    }
    String out;
    serializeJson(doc, out);
    return out;
}

// ── Static file serving ───────────────────────────────────────────────────────

void WebUI::handleRoot() {
    File f = LittleFS.open("/index.html");
    if (!f) { sendError(404, "index.html not found"); return; }
    s_server.streamFile(f, "text/html");
    f.close();
}

void WebUI::handleNotFound() {
    String path = s_server.uri();
    String contentType = "text/plain";
    if (path.endsWith(".js"))  contentType = "application/javascript";
    if (path.endsWith(".css")) contentType = "text/css";

    if (LittleFS.exists(path)) {
        File f = LittleFS.open(path);
        s_server.streamFile(f, contentType);
        f.close();
        return;
    }
    sendError(404, "Not found");
}

// ── REST API ──────────────────────────────────────────────────────────────────

void WebUI::apiStatus() {
    sendJSON(200, buildStatusJSON());
}

void WebUI::apiTune() {
    if (!s_server.hasArg("plain")) { sendError(400, "No body"); return; }
    StaticJsonDocument<128> doc;
    if (deserializeJson(doc, s_server.arg("plain"))) { sendError(400, "JSON error"); return; }

    I2CCommand cmd;
    {
        StateLock lock;
        cmd.L    = doc.containsKey("L")    ? (uint16_t)constrain((int)doc["L"], 0, L_MAX) : g_state.L;
        cmd.C    = doc.containsKey("C")    ? (uint16_t)constrain((int)doc["C"], 0, C_MAX) : g_state.C;
        cmd.mode = doc.containsKey("mode") ? (uint8_t)constrain((int)doc["mode"], 1, 3)   : g_state.mode;
    }
    cmd.cmd = I2CCmd::SET_LC;
    xQueueSend(g_i2cCmdQueue, &cmd, 0);
    sendJSON(200, "{\"ok\":true}");
}

void WebUI::apiFinetune() {
    if (!s_server.hasArg("plain")) { sendError(400, "No body"); return; }
    StaticJsonDocument<64> doc;
    deserializeJson(doc, s_server.arg("plain"));
    bool start = doc["start"] | true;
    if (start) {
        xSemaphoreGive(g_fineTuneStartSem);
    } else {
        xSemaphoreGive(g_tuneAbortSem);
    }
    sendJSON(200, "{\"ok\":true}");
}

void WebUI::apiKTune() {
    if (!s_server.hasArg("plain")) { sendError(400, "No body"); return; }
    StaticJsonDocument<64> doc;
    if (deserializeJson(doc, s_server.arg("plain"))) { sendError(400, "JSON error"); return; }
    I2CCommand cmd = {};
    cmd.cmd   = I2CCmd::SET_KTUNE;
    cmd.kTune = doc["ktune"] | false;
    xQueueSend(g_i2cCmdQueue, &cmd, 0);
    sendJSON(200, "{\"ok\":true}");
}

void WebUI::apiAutotune() {
    if (!s_server.hasArg("plain")) { sendError(400, "No body"); return; }
    StaticJsonDocument<64> doc;
    deserializeJson(doc, s_server.arg("plain"));
    bool start = doc["start"] | true;
    if (start) {
        xSemaphoreGive(g_tuneStartSem);
    } else {
        xSemaphoreGive(g_tuneAbortSem);
    }
    sendJSON(200, "{\"ok\":true}");
}

void WebUI::apiPresetsGet() {
    StaticJsonDocument<2048> doc;
    JsonArray arr = doc.to<JsonArray>();
    for (int i = 0; i < PresetStore::count(); i++) {
        Preset p;
        if (PresetStore::read(i, p)) {
            JsonObject o = arr.createNestedObject();
            o["freq_kHz"] = p.freq_kHz;
            o["L"]        = p.L;
            o["C"]        = p.C;
            o["mode"]     = p.mode;
        }
    }
    String out;
    serializeJson(doc, out);
    sendJSON(200, out);
}

void WebUI::apiPresetDeleteOne() {
    String uri = s_server.uri();   // /api/presets/7100
    int lastSlash = uri.lastIndexOf('/');
    if (lastSlash < 0) { sendError(400, "Bad URI"); return; }
    uint16_t freq = (uint16_t)uri.substring(lastSlash + 1).toInt();
    PresetStore::deleteByFreq(freq);
    sendJSON(200, "{\"ok\":true}");
}

void WebUI::apiPresetsDeleteAll() {
    PresetStore::deleteAll();
    sendJSON(200, "{\"ok\":true}");
}

void WebUI::apiConfigGet() {
    StaticJsonDocument<512> doc;
    doc["wifi_ssid"]        = g_cfg.wifi_ssid;
    doc["mqtt_server"]      = g_cfg.mqtt_server;
    doc["mqtt_port"]        = g_cfg.mqtt_port;
    doc["mqtt_enabled"]     = g_cfg.mqtt_enabled;
    doc["tune_threshold"]   = g_cfg.tune_threshold;
    doc["tune_tx_level"]    = g_cfg.tune_tx_level;
    doc["coarse_step_l"]    = g_cfg.coarse_step_l;
    doc["coarse_step_c"]    = g_cfg.coarse_step_c;
    doc["ota_manifest_url"] = g_cfg.ota_manifest_url;
    doc["log_level"]        = g_cfg.log_level;
    doc["ntp_server"]       = g_cfg.ntp_server;
    String out;
    serializeJson(doc, out);
    sendJSON(200, out);
}

void WebUI::apiConfigPost() {
    if (!s_server.hasArg("plain")) { sendError(400, "No body"); return; }
    StaticJsonDocument<512> doc;
    if (deserializeJson(doc, s_server.arg("plain"))) { sendError(400, "JSON error"); return; }

    if (doc.containsKey("wifi_ssid"))       strlcpy(g_cfg.wifi_ssid,        doc["wifi_ssid"]        | "", sizeof(g_cfg.wifi_ssid));
    if (doc.containsKey("wifi_pass"))       strlcpy(g_cfg.wifi_pass,        doc["wifi_pass"]        | "", sizeof(g_cfg.wifi_pass));
    if (doc.containsKey("mqtt_server"))     strlcpy(g_cfg.mqtt_server,      doc["mqtt_server"]      | "", sizeof(g_cfg.mqtt_server));
    if (doc.containsKey("mqtt_port"))       g_cfg.mqtt_port      = doc["mqtt_port"];
    if (doc.containsKey("mqtt_enabled"))    g_cfg.mqtt_enabled   = doc["mqtt_enabled"];
    if (doc.containsKey("tune_threshold"))  g_cfg.tune_threshold = doc["tune_threshold"];
    if (doc.containsKey("tune_tx_level"))   g_cfg.tune_tx_level  = doc["tune_tx_level"];
    if (doc.containsKey("coarse_step_l"))   g_cfg.coarse_step_l  = doc["coarse_step_l"];
    if (doc.containsKey("coarse_step_c"))   g_cfg.coarse_step_c  = doc["coarse_step_c"];
    if (doc.containsKey("ota_manifest_url"))strlcpy(g_cfg.ota_manifest_url, doc["ota_manifest_url"] | "", sizeof(g_cfg.ota_manifest_url));
    if (doc.containsKey("ntp_server")) {
        strlcpy(g_cfg.ntp_server, doc["ntp_server"] | NTP_SERVER_DEFAULT, sizeof(g_cfg.ntp_server));
        configTzTime(NTP_TIMEZONE, g_cfg.ntp_server);
    }
    if (doc.containsKey("log_level")) {
        g_cfg.log_level = doc["log_level"];
        Logger::setLevel((LogLevel)g_cfg.log_level);
    }

    Config::save();
    sendJSON(200, "{\"ok\":true,\"reboot\":false}");
}

// ── SSE ───────────────────────────────────────────────────────────────────────
// Non-blocking: handleSSE() just sends headers and stores the client.
// pushSSE() is called from taskWebServer every loop iteration.

void WebUI::handleSSE() {
    s_sseClient   = s_server.client();
    // Force full send on first push
    s_sseLastSwr  = -1.0f;
    s_sseLastTune = (TunerState::TuneState)0xFF;
    s_sseLastOta  = (TunerState::OtaState)0xFF;
    s_sseLastFreq = 0xFFFF;
    s_sseLastL    = 0xFFFF;
    s_sseLastC    = 0xFFFF;
    s_sseLastMode  = 0xFF;
    s_sseLastKTune = false;
    s_sseLastRssi  = 0;
    s_sseLastHb   = 0;
    s_sseClient.print("HTTP/1.1 200 OK\r\n"
                      "Content-Type: text/event-stream\r\n"
                      "Cache-Control: no-cache\r\n"
                      "Access-Control-Allow-Origin: *\r\n"
                      "Connection: keep-alive\r\n\r\n");
    pushSSE();
}

void WebUI::pushSSE() {
    if (!s_sseClient || !s_sseClient.connected()) return;

    float swr, rl; uint16_t L, C, freq; uint8_t tp, op, mode; int8_t rssi; bool kTune;
    TunerState::TuneState ts; TunerState::OtaState os;
    {
        StateLock lock;
        swr   = g_state.swr;
        rl    = g_state.returnLoss;
        L     = g_state.L;
        C     = g_state.C;
        mode  = g_state.mode;
        kTune = g_state.kTune;
        freq  = g_state.freq_kHz;
        rssi  = g_state.rssi;
        ts    = g_state.tuneState;
        tp    = g_state.tuneProgress;
        os    = g_state.otaState;
        op    = g_state.otaProgress;
    }

    bool changed = fabs(swr - s_sseLastSwr) > 0.05f
                || ts    != s_sseLastTune
                || os    != s_sseLastOta
                || freq  != s_sseLastFreq
                || L     != s_sseLastL
                || C     != s_sseLastC
                || mode  != s_sseLastMode
                || kTune != s_sseLastKTune
                || rssi  != s_sseLastRssi;
    bool periodic = (millis() - s_sseLastHb > 2000);

    if (changed || periodic) {
        s_sseLastSwr   = swr;
        s_sseLastTune  = ts;
        s_sseLastOta   = os;
        s_sseLastFreq  = freq;
        s_sseLastL     = L;
        s_sseLastC     = C;
        s_sseLastMode  = mode;
        s_sseLastKTune = kTune;
        s_sseLastRssi  = rssi;
        s_sseLastHb    = millis();
        char buf[352];
        snprintf(buf, sizeof(buf),
            "data:{\"swr\":%.2f,\"returnLoss\":%.1f,"
            "\"L\":%u,\"C\":%u,\"mode\":%u,\"kTune\":%d,\"freq_kHz\":%u,"
            "\"rssi\":%d,\"tuneState\":%d,\"tuneProgress\":%d,"
            "\"otaState\":%d,\"otaProgress\":%d}\n\n",
            swr, rl, L, C, mode, kTune ? 1 : 0, freq, rssi,
            (int)ts, tp, (int)os, op);
        s_sseClient.print(buf);
    }
}

// ── OTA endpoints ─────────────────────────────────────────────────────────────

void WebUI::otaLocalFW() {
    HTTPUpload& up = s_server.upload();
    static bool started = false;

    if (up.status == UPLOAD_FILE_START) {
        LOG_INFO("OTA", "Local FW upload start: %s", up.filename.c_str());
        started = Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH);
    } else if (up.status == UPLOAD_FILE_WRITE && started) {
        Update.write(up.buf, up.currentSize);
    } else if (up.status == UPLOAD_FILE_END && started) {
        if (Update.end(true)) {
            LOG_INFO("OTA", "Local FW flash OK");
            sendJSON(200, "{\"ok\":true}");
        } else {
            sendError(500, Update.errorString());
        }
        started = false;
    }
}

void WebUI::otaLocalFS() {
    HTTPUpload& up = s_server.upload();
    static bool started = false;

    if (up.status == UPLOAD_FILE_START) {
        LOG_INFO("OTA", "Local FS upload start");
        started = Update.begin(UPDATE_SIZE_UNKNOWN, U_SPIFFS);
    } else if (up.status == UPLOAD_FILE_WRITE && started) {
        Update.write(up.buf, up.currentSize);
    } else if (up.status == UPLOAD_FILE_END && started) {
        if (Update.end(true)) {
            LOG_INFO("OTA", "Local FS flash OK – rebooting");
            sendJSON(200, "{\"ok\":true}");
            delay(500);
            ESP.restart();
        } else {
            sendError(500, Update.errorString());
        }
        started = false;
    }
}

void WebUI::otaGitHubCheck() {
    OTAManifest manifest;
    if (!OTAUpdater::checkGitHub(manifest)) {
        sendJSON(200, "{\"updateAvailable\":false}");
        return;
    }
    StaticJsonDocument<512> doc;
    doc["updateAvailable"] = true;
    doc["version"]   = manifest.version;
    doc["fwSize"]    = manifest.fwSize;
    doc["fsSize"]    = manifest.fsSize;
    doc["changelog"] = manifest.changelog;
    String out;
    serializeJson(doc, out);
    sendJSON(200, out);
}

void WebUI::otaGitHubInstall() {
    OTAManifest manifest;
    if (!OTAUpdater::checkGitHub(manifest)) {
        sendJSON(200, "{\"ok\":false,\"reason\":\"no update\"}");
        return;
    }
    sendJSON(200, "{\"ok\":true,\"message\":\"OTA started\"}");
    delay(100);
    OTAUpdater::installGitHub(manifest);   // reboots on success
}

// ── Reboot ────────────────────────────────────────────────────────────────────

void WebUI::apiReboot() {
    sendJSON(200, "{\"ok\":true}");
    delay(300);
    ESP.restart();
}

// ── Init ──────────────────────────────────────────────────────────────────────

bool WebUI::begin() {
    s_server.on("/", HTTP_GET, handleRoot);
    s_server.on("/api/status",       HTTP_GET,    apiStatus);
    s_server.on("/api/tune",         HTTP_POST,   apiTune);
    s_server.on("/api/autotune",     HTTP_POST,   apiAutotune);
    s_server.on("/api/finetune",     HTTP_POST,   apiFinetune);
    s_server.on("/api/ktune",        HTTP_POST,   apiKTune);
    s_server.on("/api/presets",      HTTP_GET,    apiPresetsGet);
    s_server.on("/api/presets",      HTTP_DELETE, apiPresetsDeleteAll);
    s_server.on("/api/config",       HTTP_GET,    apiConfigGet);
    s_server.on("/api/config",       HTTP_POST,   apiConfigPost);
    s_server.on("/api/reboot",       HTTP_POST,   apiReboot);
    s_server.on("/events",           HTTP_GET,    handleSSE);
    s_server.on("/ota/local/fw",     HTTP_POST,   [](){}, otaLocalFW);
    s_server.on("/ota/local/fs",     HTTP_POST,   [](){}, otaLocalFS);
    s_server.on("/ota/github/check", HTTP_GET,    otaGitHubCheck);
    s_server.on("/ota/github/install", HTTP_POST, otaGitHubInstall);
    // Preset delete by freq: /api/presets/{freq}
    s_server.onNotFound([]() {
        if (s_server.uri().startsWith("/api/presets/") && s_server.method() == HTTP_DELETE) {
            apiPresetDeleteOne();
        } else {
            handleNotFound();
        }
    });
    s_server.begin();
    LOG_INFO("WebServer", "HTTP server started on port %d", WEB_PORT);
    return true;
}

void WebUI::taskWebServer(void* param) {
    (void)param;
    for (;;) {
        s_server.handleClient();
        pushSSE();
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}
