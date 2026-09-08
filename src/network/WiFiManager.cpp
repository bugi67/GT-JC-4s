#include "WiFiManager.h"
#include "../config.h"
#include "../logger/Logger.h"
#include <WiFi.h>
#include <DNSServer.h>
#include <WebServer.h>
#include <Preferences.h>
#include <LittleFS.h>
#include <ArduinoJson.h>

bool WiFiManager::s_apMode = false;
WifiNetworkConfig WiFiManager::s_active;

static const char WIFI_CFG_PATH[] = "/wifi.json";

// ── LittleFS-backed storage for both networks ─────────────────────────────────

static void netToJson(JsonObject obj, const WifiNetworkConfig& net) {
    obj["ssid"]   = net.ssid;
    obj["pass"]   = net.pass;
    obj["static"] = net.useStaticIP;
    obj["ip"]     = net.ip;
    obj["mask"]   = net.netmask;
    obj["gw"]     = net.gateway;
    obj["dns"]    = net.dns;
}

static void jsonToNet(JsonObjectConst obj, WifiNetworkConfig& net) {
    strlcpy(net.ssid,    obj["ssid"]   | "", sizeof(net.ssid));
    strlcpy(net.pass,    obj["pass"]   | "", sizeof(net.pass));
    net.useStaticIP =    obj["static"] | false;
    strlcpy(net.ip,      obj["ip"]     | "", sizeof(net.ip));
    strlcpy(net.netmask, obj["mask"]   | "", sizeof(net.netmask));
    strlcpy(net.gateway, obj["gw"]     | "", sizeof(net.gateway));
    strlcpy(net.dns,     obj["dns"]    | "", sizeof(net.dns));
}

static bool loadAllNetworks(WifiNetworkConfig& net0, WifiNetworkConfig& net1) {
    File file = LittleFS.open(WIFI_CFG_PATH, "r");
    if (!file) return false;

    StaticJsonDocument<768> doc;
    DeserializationError err = deserializeJson(doc, file);
    file.close();
    if (err) {
        LOG_WARN("WiFi", "wifi.json parse error: %s", err.c_str());
        return false;
    }
    jsonToNet(doc["n0"], net0);
    jsonToNet(doc["n1"], net1);
    return true;
}

static void saveAllNetworks(const WifiNetworkConfig& net0, const WifiNetworkConfig& net1) {
    StaticJsonDocument<768> doc;
    netToJson(doc.createNestedObject("n0"), net0);
    netToJson(doc.createNestedObject("n1"), net1);

    File file = LittleFS.open(WIFI_CFG_PATH, "w");
    if (!file) {
        LOG_ERROR("WiFi", "Unable to open %s for writing", WIFI_CFG_PATH);
        return;
    }
    serializeJson(doc, file);
    file.close();
}

// Seeds wifi.json on first boot from NVS (if credentials exist there from a
// previous firmware) or from the WIFI_SEED* compile-time constants otherwise.
static void seedIfMissing() {
    if (LittleFS.exists(WIFI_CFG_PATH)) return;

    WifiNetworkConfig net0, net1;

    // Migrate credentials stored by the old firmware in NVS
    Preferences prefs;
    prefs.begin("wifi", true);
    String nvsSsid = prefs.getString("ssid", "");
    String nvsPass = prefs.getString("pass", "");
    prefs.end();

    if (nvsSsid.length() > 0) {
        strlcpy(net0.ssid, nvsSsid.c_str(), sizeof(net0.ssid));
        strlcpy(net0.pass, nvsPass.c_str(), sizeof(net0.pass));
        LOG_INFO("WiFi", "wifi.json seeded from NVS (SSID: %s)", net0.ssid);
    } else {
        strlcpy(net0.ssid, WIFI_SEED1_SSID, sizeof(net0.ssid));
        strlcpy(net0.pass, WIFI_SEED1_PASS, sizeof(net0.pass));
        strlcpy(net1.ssid, WIFI_SEED2_SSID, sizeof(net1.ssid));
        strlcpy(net1.pass, WIFI_SEED2_PASS, sizeof(net1.pass));
        LOG_INFO("WiFi", "wifi.json seeded from compile-time constants");
    }

    saveAllNetworks(net0, net1);
}

static void applyStaticIP(const WifiNetworkConfig& net) {
    if (!net.useStaticIP) {
        WiFi.config((uint32_t)0, (uint32_t)0, (uint32_t)0);   // revert to DHCP
        return;
    }
    IPAddress ip, gw, mask, dns;
    if (!ip.fromString(net.ip) || !gw.fromString(net.gateway) || !mask.fromString(net.netmask)) {
        LOG_WARN("WiFi", "Invalid static IP config for '%s', falling back to DHCP", net.ssid);
        WiFi.config((uint32_t)0, (uint32_t)0, (uint32_t)0);
        return;
    }
    if (!dns.fromString(net.dns)) dns = gw;
    WiFi.config(ip, gw, mask, dns);
}

bool WiFiManager::connectStation(const WifiNetworkConfig& net) {
    WiFi.mode(WIFI_STA);
    // Mains-powered device with no reason to trade latency for radio power
    // savings - default modem sleep waits out the AP's beacon/DTIM interval
    // before answering, which shows up as tens-to-150ms+ jitter on anything
    // hitting this device (ping, dashboard polling/SSE, MQTT).
    WiFi.setSleep(false);
    applyStaticIP(net);
    WiFi.begin(net.ssid, net.pass);
    LOG_INFO("WiFi", "Connecting to '%s'%s...", net.ssid, net.useStaticIP ? " (static IP)" : "");

    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED) {
        if (millis() - start > 15000) {
            LOG_WARN("WiFi", "Connection timeout");
            return false;
        }
        delay(250);
    }
    LOG_INFO("WiFi", "Connected – IP %s", WiFi.localIP().toString().c_str());
    s_active = net;
    return true;
}

void WiFiManager::startAP() {
    WiFi.mode(WIFI_AP);
    WiFi.softAP(AP_SSID, strlen(AP_PASS) ? AP_PASS : nullptr);
    s_apMode = true;
    LOG_INFO("WiFi", "AP mode: SSID='%s' IP=%s", AP_SSID, WiFi.softAPIP().toString().c_str());
}

WifiNetworkConfig WiFiManager::getNetworkConfig(uint8_t idx) {
    WifiNetworkConfig net0, net1;
    loadAllNetworks(net0, net1);
    return idx == 0 ? net0 : net1;
}

void WiFiManager::setNetworkConfig(uint8_t idx, const WifiNetworkConfig& cfg) {
    WifiNetworkConfig net0, net1;
    loadAllNetworks(net0, net1);
    if (idx == 0) net0 = cfg; else net1 = cfg;
    saveAllNetworks(net0, net1);
    LOG_INFO("WiFi", "Network %u config saved: %s", idx + 1, cfg.ssid);
}

// ── Captive portal ────────────────────────────────────────────────────────────

static const char PORTAL_HTML[] = R"html(<!DOCTYPE html>
<html lang="de">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>GT-JC-4s Setup</title>
<style>
body{font-family:sans-serif;background:#f4f7fb;color:#333;margin:0;padding:0}
.main{max-width:480px;margin:40px auto;padding:24px;background:#fff;border-radius:12px;box-shadow:0 12px 30px rgba(0,0,0,.12)}
h1{margin-top:0;font-size:1.4rem}
h2{font-size:1.05rem;margin:24px 0 4px;border-top:1px solid #e5e9f0;padding-top:16px}
label{display:block;margin:12px 0 4px;font-weight:600;font-size:.9rem}
input[type=text],input[type=password]{width:100%;padding:9px;border:1px solid #ccd6e0;border-radius:6px;box-sizing:border-box}
.chk{display:flex;align-items:center;gap:8px;margin:14px 0 4px;font-weight:600;font-size:.9rem}
.chk input{width:auto}
.static-fields{display:none;padding-left:10px;border-left:2px solid #e5e9f0;margin-left:2px}
.static-fields.show{display:block}
button{margin-top:24px;width:100%;padding:12px;border:none;border-radius:8px;background:#0066cc;color:#fff;font-size:1rem;cursor:pointer}
button:hover{background:#0051a8}
</style>
</head>
<body>
<div class="main">
<h1>GT-JC-4s WLAN Setup</h1>
<form method="POST" action="/save">

<h2>Netzwerk 1</h2>
<label>SSID<input name="n0_ssid" type="text" autocomplete="off"></label>
<label>Passwort<input name="n0_pass" type="password" autocomplete="new-password"></label>
<label class="chk"><input type="checkbox" name="n0_static" onchange="document.getElementById('n0_fields').classList.toggle('show',this.checked)"> Statische IP</label>
<div class="static-fields" id="n0_fields">
<label>IP-Adresse<input name="n0_ip" type="text" placeholder="192.168.0.50"></label>
<label>Subnetzmaske<input name="n0_mask" type="text" placeholder="255.255.255.0"></label>
<label>Gateway<input name="n0_gw" type="text" placeholder="192.168.0.1"></label>
<label>DNS<input name="n0_dns" type="text" placeholder="192.168.0.1"></label>
</div>

<h2>Netzwerk 2</h2>
<label>SSID<input name="n1_ssid" type="text" autocomplete="off"></label>
<label>Passwort<input name="n1_pass" type="password" autocomplete="new-password"></label>
<label class="chk"><input type="checkbox" name="n1_static" onchange="document.getElementById('n1_fields').classList.toggle('show',this.checked)"> Statische IP</label>
<div class="static-fields" id="n1_fields">
<label>IP-Adresse<input name="n1_ip" type="text" placeholder="192.168.0.51"></label>
<label>Subnetzmaske<input name="n1_mask" type="text" placeholder="255.255.255.0"></label>
<label>Gateway<input name="n1_gw" type="text" placeholder="192.168.0.1"></label>
<label>DNS<input name="n1_dns" type="text" placeholder="192.168.0.1"></label>
</div>

<button type="submit">Speichern und verbinden</button>
</form>
</div>
</body>
</html>)html";

static const char PORTAL_SAVED_HTML[] = R"html(<!DOCTYPE html>
<html><head><meta charset="utf-8"><title>Gespeichert</title>
<style>body{font-family:sans-serif;background:#f4f7fb;color:#333;text-align:center;padding-top:40px} .card{display:inline-block;padding:24px;background:#fff;border-radius:12px;box-shadow:0 12px 30px rgba(0,0,0,.12)}</style>
</head><body><div class="card"><h2>&#10003; Gespeichert</h2><p>Das Gerät verbindet sich und startet neu.</p></div></body></html>)html";

void WiFiManager::runCaptivePortal() {
    IPAddress apIP = WiFi.softAPIP();
    DNSServer dns;
    dns.setErrorReplyCode(DNSReplyCode::NoError);
    dns.start(53, "*", apIP);

    WebServer portal(80);
    auto redirect = [&]() {
        portal.sendHeader("Location", String("http://") + apIP.toString() + "/");
        portal.send(302, "text/plain", "");
    };

    portal.on("/", HTTP_GET, [&]() { portal.send(200, "text/html", PORTAL_HTML); });

    portal.on("/save", HTTP_POST, [&]() {
        WifiNetworkConfig net0, net1;
        strlcpy(net0.ssid, portal.arg("n0_ssid").c_str(), sizeof(net0.ssid));
        strlcpy(net0.pass, portal.arg("n0_pass").c_str(), sizeof(net0.pass));
        net0.useStaticIP = portal.hasArg("n0_static");
        strlcpy(net0.ip,      portal.arg("n0_ip").c_str(),   sizeof(net0.ip));
        strlcpy(net0.netmask, portal.arg("n0_mask").c_str(), sizeof(net0.netmask));
        strlcpy(net0.gateway, portal.arg("n0_gw").c_str(),   sizeof(net0.gateway));
        strlcpy(net0.dns,     portal.arg("n0_dns").c_str(),  sizeof(net0.dns));

        strlcpy(net1.ssid, portal.arg("n1_ssid").c_str(), sizeof(net1.ssid));
        strlcpy(net1.pass, portal.arg("n1_pass").c_str(), sizeof(net1.pass));
        net1.useStaticIP = portal.hasArg("n1_static");
        strlcpy(net1.ip,      portal.arg("n1_ip").c_str(),   sizeof(net1.ip));
        strlcpy(net1.netmask, portal.arg("n1_mask").c_str(), sizeof(net1.netmask));
        strlcpy(net1.gateway, portal.arg("n1_gw").c_str(),   sizeof(net1.gateway));
        strlcpy(net1.dns,     portal.arg("n1_dns").c_str(),  sizeof(net1.dns));

        if (strlen(net0.ssid) == 0 && strlen(net1.ssid) == 0) {
            redirect();
            return;
        }

        portal.send(200, "text/html", PORTAL_SAVED_HTML);
        delay(800);
        saveAllNetworks(net0, net1);
        LOG_INFO("WiFi", "Captive portal: credentials saved, rebooting");
        delay(200);
        ESP.restart();
    });

    // OS-specific captive portal detection probes → redirect to setup page
    portal.on("/hotspot-detect.html", HTTP_GET, redirect);
    portal.on("/generate_204",        HTTP_GET, redirect);
    portal.on("/connecttest.txt",     HTTP_GET, redirect);
    portal.on("/ncsi.txt",            HTTP_GET, redirect);
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
    seedIfMissing();

    WifiNetworkConfig net0 = getNetworkConfig(0);
    if (strlen(net0.ssid) > 0 && connectStation(net0)) return true;

    WifiNetworkConfig net1 = getNetworkConfig(1);
    if (strlen(net1.ssid) > 0 && connectStation(net1)) return true;

    LOG_WARN("WiFi", "No configured WLAN reachable – starting AP");
    startAP();
    return false;
}

bool WiFiManager::isAPMode()    { return s_apMode; }
bool WiFiManager::isConnected() { return WiFi.status() == WL_CONNECTED; }

bool WiFiManager::reconnect() {
    if (WiFi.status() == WL_CONNECTED) return true;
    if (strlen(s_active.ssid) == 0) return false;
    return connectStation(s_active);
}

String WiFiManager::getSSID()   { return WiFi.SSID(); }
int8_t WiFiManager::getRSSI()   { return (int8_t)WiFi.RSSI(); }
