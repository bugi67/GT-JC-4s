#include "WiFiManager.h"
#include "../config.h"
#include "../cfg/AppConfig.h"
#include "../logger/Logger.h"
#include <WiFi.h>
#include <DNSServer.h>
#include <WebServer.h>

bool WiFiManager::s_apMode = false;

// ── Station ───────────────────────────────────────────────────────────────────

bool WiFiManager::connectStation(const char* ssid, const char* pass) {
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid, pass);
    LOG_INFO("WiFi", "Connecting to '%s'...", ssid);

    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED) {
        if (millis() - start > 15000) {
            LOG_WARN("WiFi", "Connection timeout");
            return false;
        }
        delay(250);
    }
    LOG_INFO("WiFi", "Connected – IP %s", WiFi.localIP().toString().c_str());
    return true;
}

// ── AP / Captive portal ───────────────────────────────────────────────────────

void WiFiManager::startAP() {
    WiFi.mode(WIFI_AP);
    WiFi.softAP(AP_SSID, strlen(AP_PASS) ? AP_PASS : nullptr);
    s_apMode = true;
    LOG_INFO("WiFi", "AP mode: SSID='%s' IP=%s", AP_SSID, WiFi.softAPIP().toString().c_str());
}

static const char PORTAL_HTML[] = R"html(<!DOCTYPE html>
<html lang="de">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>GT-JC-4s Setup</title>
<style>
body{font-family:sans-serif;max-width:420px;margin:40px auto;padding:0 1rem;background:#f0f4f8}
.card{background:#fff;border-radius:8px;padding:24px;box-shadow:0 2px 8px rgba(0,0,0,.15)}
h1{margin:0 0 20px;font-size:1.2rem;color:#333}
label{display:block;margin-top:14px;font-size:.9rem;color:#555}
input{width:100%;padding:9px;margin-top:4px;border:1px solid #ccc;border-radius:4px;
      box-sizing:border-box;font-size:1rem}
button{display:block;width:100%;margin-top:22px;padding:11px;background:#1a73e8;
       color:#fff;border:none;border-radius:4px;font-size:1rem;cursor:pointer}
button:hover{background:#1558b0}
</style>
</head>
<body>
<div class="card">
<h1>GT-JC-4s &ndash; WLAN konfigurieren</h1>
<form method="POST" action="/save">
<label>WLAN-Name (SSID)
  <input name="ssid" type="text" required autocomplete="off">
</label>
<label>Passwort
  <input name="pass" type="password" autocomplete="new-password">
</label>
<button type="submit">Verbinden &amp; Speichern</button>
</form>
</div>
</body>
</html>)html";

static const char PORTAL_SAVED_HTML[] = R"html(<!DOCTYPE html>
<html><head><meta charset="utf-8"><title>GT-JC-4s</title>
<style>body{font-family:sans-serif;max-width:420px;margin:40px auto;
padding:0 1rem;text-align:center;background:#f0f4f8}
.card{background:#fff;border-radius:8px;padding:32px;
box-shadow:0 2px 8px rgba(0,0,0,.15)}</style>
</head><body>
<div class="card">
<h2>&#10003; Gespeichert</h2>
<p>Das Ger&auml;t verbindet sich jetzt mit dem WLAN und startet neu.<br>
Du kannst diese Seite schlie&szlig;en.</p>
</div>
</body></html>)html";

void WiFiManager::runCaptivePortal() {
    IPAddress apIP = WiFi.softAPIP();

    DNSServer dns;
    dns.setErrorReplyCode(DNSReplyCode::NoError);
    dns.start(53, "*", apIP);

    WebServer portal(80);

    // Redirect helper used for captive-portal detection probes
    auto redirect = [&]() {
        portal.sendHeader("Location", String("http://") + apIP.toString() + "/");
        portal.send(302, "text/plain", "");
    };

    portal.on("/", HTTP_GET, [&]() {
        portal.send(200, "text/html", PORTAL_HTML);
    });

    portal.on("/save", HTTP_POST, [&]() {
        String ssid = portal.arg("ssid");
        String pass = portal.arg("pass");
        if (ssid.length() == 0) {
            redirect();
            return;
        }
        portal.send(200, "text/html", PORTAL_SAVED_HTML);
        delay(800);
        WiFiManager::saveCredentials(ssid.c_str(), pass.c_str());  // reboots
    });

    // OS-specific captive portal detection endpoints
    portal.on("/hotspot-detect.html", HTTP_GET, redirect);   // iOS / macOS
    portal.on("/generate_204",        HTTP_GET, redirect);   // Android
    portal.on("/connecttest.txt",     HTTP_GET, redirect);   // Windows
    portal.on("/ncsi.txt",            HTTP_GET, redirect);   // Windows NCSI
    portal.on("/redirect",            HTTP_GET, redirect);
    portal.onNotFound(redirect);

    portal.begin();
    LOG_INFO("WiFi", "Captive portal active – connect to '%s', open http://%s",
             AP_SSID, apIP.toString().c_str());

    for (;;) {
        dns.processNextRequest();
        portal.handleClient();
        delay(5);
    }
}

// ── Public ────────────────────────────────────────────────────────────────────

bool WiFiManager::begin() {
    if (strlen(g_cfg.wifi_ssid) == 0) {
        LOG_WARN("WiFi", "No credentials – starting setup AP");
        startAP();
        return false;
    }
    if (!connectStation(g_cfg.wifi_ssid, g_cfg.wifi_pass)) {
        startAP();
        return false;
    }
    return true;
}

bool WiFiManager::isAPMode()   { return s_apMode; }
bool WiFiManager::isConnected(){ return WiFi.status() == WL_CONNECTED; }

bool WiFiManager::reconnect() {
    if (WiFi.status() == WL_CONNECTED) return true;
    return connectStation(g_cfg.wifi_ssid, g_cfg.wifi_pass);
}

String WiFiManager::getSSID() { return String(g_cfg.wifi_ssid); }
int8_t WiFiManager::getRSSI() { return WiFi.RSSI(); }

void WiFiManager::saveCredentials(const char* ssid, const char* pass) {
    strlcpy(g_cfg.wifi_ssid, ssid, sizeof(g_cfg.wifi_ssid));
    strlcpy(g_cfg.wifi_pass, pass, sizeof(g_cfg.wifi_pass));
    Config::save();
    LOG_INFO("WiFi", "Credentials saved, rebooting...");
    delay(500);
    ESP.restart();
}
