#include "WebServer.h"
#include "../config.h"
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
    doc["tuneState"]   = (int)g_state.tuneState;
    doc["tuneProgress"]= g_state.tuneProgress;
    doc["otaState"]    = (int)g_state.otaState;
    doc["otaProgress"] = g_state.otaProgress;
    doc["rssi"]        = g_state.rssi;
    doc["fwVersion"]   = g_state.fwVersion;
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
    if (doc.containsKey("log_level")) {
        g_cfg.log_level = doc["log_level"];
        Logger::setLevel((LogLevel)g_cfg.log_level);
    }

    Config::save();
    sendJSON(200, "{\"ok\":true,\"reboot\":false}");
}

// ── SSE ───────────────────────────────────────────────────────────────────────

void WebUI::handleSSE() {
    WiFiClient client = s_server.client();
    client.print("HTTP/1.1 200 OK\r\n"
                 "Content-Type: text/event-stream\r\n"
                 "Cache-Control: no-cache\r\n"
                 "Access-Control-Allow-Origin: *\r\n"
                 "Connection: keep-alive\r\n\r\n");

    TunerState::TuneState lastTune = TunerState::TuneState::IDLE;
    TunerState::OtaState  lastOta  = TunerState::OtaState::IDLE;
    float lastSWR = 0;

    unsigned long lastHb = 0;
    while (client.connected()) {
        float swr; uint8_t tp, op;
        TunerState::TuneState ts; TunerState::OtaState os;
        {
            StateLock lock;
            swr = g_state.swr;
            ts  = g_state.tuneState;
            tp  = g_state.tuneProgress;
            os  = g_state.otaState;
            op  = g_state.otaProgress;
        }

        if (fabs(swr - lastSWR) > 0.05f || ts != lastTune || os != lastOta) {
            lastSWR  = swr;
            lastTune = ts;
            lastOta  = os;
            char buf[192];
            snprintf(buf, sizeof(buf),
                "data:{\"swr\":%.2f,\"tuneState\":%d,\"tuneProgress\":%d,"
                "\"otaState\":%d,\"otaProgress\":%d}\n\n",
                swr, (int)ts, tp, (int)os, op);
            client.print(buf);
        }

        // Heartbeat comment every 15 s to keep connection alive
        if (millis() - lastHb > 15000) {
            client.print(": hb\n\n");
            lastHb = millis();
        }
        delay(200);
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
        started = Update.begin(UPDATE_SIZE_UNKNOWN, U_LittleFS);
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
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}
