#pragma once
#include <Arduino.h>
#include <WebServer.h>
#include <WiFi.h>
#include "../state.h"

class WebUI {
public:
    static bool begin();
    static void taskWebServer(void* param);

private:
    static ::WebServer s_server;

    // Static file routes
    static void handleRoot();
    static void handleNotFound();

    // REST API
    static void apiStatus();
    static void apiTune();
    static void apiAutotune();
    static void apiFinetune();
    static void apiKTune();
    static void apiPresetsGet();
    static void apiPresetDeleteOne();
    static void apiPresetsDeleteAll();
    static void apiConfigGet();
    static void apiConfigPost();
    static void apiReboot();

    // SSE — non-blocking: handleSSE() stores client, pushSSE() runs in task loop
    static void handleSSE();
    static void pushSSE();
    static WiFiClient            s_sseClient;
    static float                 s_sseLastSwr;
    static TunerState::TuneState s_sseLastTune;
    static TunerState::OtaState  s_sseLastOta;
    static uint16_t              s_sseLastFreq;
    static uint16_t              s_sseLastL;
    static uint16_t              s_sseLastC;
    static uint8_t               s_sseLastMode;
    static bool                  s_sseLastKTune;
    static int8_t                s_sseLastRssi;
    static unsigned long         s_sseLastHb;

    // OTA routes
    static void otaLocalFW();
    static void otaLocalFS();
    static void otaGitHubCheck();
    static void otaGitHubInstall();

    // Helpers
    static void sendJSON(int code, const String& json);
    static void sendError(int code, const char* msg);
    static String buildStatusJSON();
};
