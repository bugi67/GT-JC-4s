#pragma once
#include <Arduino.h>
#include <WebServer.h>

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
    static void apiPresetsGet();
    static void apiPresetDeleteOne();
    static void apiPresetsDeleteAll();
    static void apiConfigGet();
    static void apiConfigPost();
    static void apiReboot();

    // SSE
    static void handleSSE();

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
