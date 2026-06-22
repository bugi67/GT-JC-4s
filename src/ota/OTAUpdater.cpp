#include "OTAUpdater.h"
#include "../config.h"
#include "../logger/Logger.h"
#include "../state.h"
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <Update.h>
#include <ArduinoJson.h>
#include <SPIFFS.h>
#include <nvs_flash.h>
#include <nvs.h>
#include <mbedtls/md5.h>

static const char* loadRootCA() {
    static char ca[4096] = {};
    if (ca[0]) return ca;
    File f = SPIFFS.open("/certs/github_root_ca.pem");
    if (!f) return nullptr;
    size_t n = f.readBytes(ca, sizeof(ca) - 1);
    ca[n] = 0;
    f.close();
    return ca;
}

static String getManifestUrl() {
    nvs_handle_t nvs;
    char url[256] = OTA_MANIFEST_URL_DEFAULT;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) == ESP_OK) {
        size_t sz = sizeof(url);
        nvs_get_str(nvs, NVS_OTA_MANIFEST_URL, url, &sz);
        nvs_close(nvs);
    }
    return String(url);
}

static int semverCompare(const char* a, const char* b) {
    int ma, mi, pa, mb, nb, pb;
    sscanf(a, "%d.%d.%d", &ma, &mi, &pa);
    sscanf(b, "%d.%d.%d", &mb, &nb, &pb);
    if (ma != mb) return ma - mb;
    if (mi != nb) return mi - nb;
    return pa - pb;
}

void OTAUpdater::reportProgress(uint8_t pct, const char* stateStr) {
    StateLock lock;
    g_state.otaProgress = pct;
    (void)stateStr;
}

bool OTAUpdater::checkGitHub(OTAManifest& manifest) {
    {
        StateLock lock;
        g_state.otaState = TunerState::OtaState::CHECKING;
    }

    WiFiClientSecure client;
    const char* ca = loadRootCA();
    if (ca) client.setCACert(ca); else client.setInsecure();

    HTTPClient http;
    http.setTimeout(OTA_TIMEOUT_MS);
    if (!http.begin(client, getManifestUrl())) {
        LOG_ERROR("OTA", "HTTP begin failed for manifest");
        return false;
    }
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    int code = http.GET();
    if (code != 200) {
        LOG_ERROR("OTA", "Manifest HTTP %d", code);
        http.end();
        return false;
    }

    StaticJsonDocument<1024> doc;
    DeserializationError err = deserializeJson(doc, http.getStream());
    http.end();
    if (err) {
        LOG_ERROR("OTA", "JSON parse: %s", err.c_str());
        return false;
    }

    strlcpy(manifest.version,   doc["version"] | "",                sizeof(manifest.version));
    strlcpy(manifest.fwUrl,     doc["fw"]["url"] | "",              sizeof(manifest.fwUrl));
    strlcpy(manifest.fwMd5,     doc["fw"]["md5"] | "",              sizeof(manifest.fwMd5));
    manifest.fwSize =           doc["fw"]["size"] | 0;
    strlcpy(manifest.fsUrl,     doc["fs"]["url"] | "",              sizeof(manifest.fsUrl));
    strlcpy(manifest.fsMd5,     doc["fs"]["md5"] | "",              sizeof(manifest.fsMd5));
    manifest.fsSize =           doc["fs"]["size"] | 0;
    strlcpy(manifest.changelog, doc["changelog"] | "",              sizeof(manifest.changelog));

    bool newer = (semverCompare(manifest.version, FIRMWARE_VERSION) > 0);
    LOG_INFO("OTA", "Remote v%s, local v%s – %s", manifest.version, FIRMWARE_VERSION,
             newer ? "update available" : "up to date");
    return newer;
}

bool OTAUpdater::downloadAndFlash(const char* url, int partition, const char* expectedMd5,
                                   uint32_t totalSize, uint8_t progressBase, uint8_t progressRange) {
    WiFiClientSecure client;
    const char* ca = loadRootCA();
    if (ca) client.setCACert(ca); else client.setInsecure();

    HTTPClient http;
    http.setTimeout(OTA_TIMEOUT_MS);
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    if (!http.begin(client, url)) return false;
    int code = http.GET();
    if (code != 200) {
        LOG_ERROR("OTA", "Download HTTP %d for %s", code, url);
        http.end();
        return false;
    }

    int contentLen = http.getSize();
    if (totalSize == 0 && contentLen > 0) totalSize = (uint32_t)contentLen;

    if (!Update.begin(totalSize > 0 ? totalSize : UPDATE_SIZE_UNKNOWN, partition)) {
        LOG_ERROR("OTA", "Update.begin failed: %s", Update.errorString());
        http.end();
        return false;
    }

    mbedtls_md5_context md5ctx;
    mbedtls_md5_init(&md5ctx);
    mbedtls_md5_starts(&md5ctx);

    WiFiClient* stream = http.getStreamPtr();
    uint8_t buf[OTA_BUF_SIZE];
    uint32_t written = 0;

    while (http.connected() && (contentLen > 0 || contentLen == -1)) {
        size_t avail = stream->available();
        if (avail == 0) { delay(1); continue; }
        size_t toRead = min(avail, (size_t)sizeof(buf));
        size_t n = stream->readBytes(buf, toRead);
        if (n == 0) break;

        mbedtls_md5_update(&md5ctx, buf, n);
        if (Update.write(buf, n) != n) {
            LOG_ERROR("OTA", "Update.write failed");
            Update.abort();
            http.end();
            return false;
        }
        written += n;
        if (contentLen > 0) contentLen -= n;
        uint8_t pct = (uint8_t)(progressBase + (uint8_t)((uint64_t)written * progressRange / totalSize));
        reportProgress(pct, "downloading");
    }
    http.end();

    uint8_t digest[16];
    mbedtls_md5_finish(&md5ctx, digest);
    mbedtls_md5_free(&md5ctx);

    char calcMd5[33];
    for (int i = 0; i < 16; i++) snprintf(calcMd5 + i*2, 3, "%02x", digest[i]);

    if (strcasecmp(calcMd5, expectedMd5) != 0) {
        LOG_ERROR("OTA", "MD5 mismatch: expected=%s got=%s", expectedMd5, calcMd5);
        Update.abort();
        return false;
    }

    if (!Update.end(true)) {
        LOG_ERROR("OTA", "Update.end failed: %s", Update.errorString());
        return false;
    }

    LOG_INFO("OTA", "Flash OK: %u bytes, MD5 verified", written);
    return true;
}

bool OTAUpdater::installGitHub(const OTAManifest& manifest) {
    {
        StateLock lock;
        g_state.otaState    = TunerState::OtaState::DOWNLOADING_FW;
        g_state.otaProgress = 0;
    }
    LOG_INFO("OTA", "Flashing FW v%s ...", manifest.version);

    if (!downloadAndFlash(manifest.fwUrl, U_FLASH, manifest.fwMd5, manifest.fwSize, 0, 45)) {
        StateLock lock;
        g_state.otaState = TunerState::OtaState::ERROR;
        strlcpy(g_state.otaError, "FW flash failed", sizeof(g_state.otaError));
        return false;
    }

    {
        StateLock lock;
        g_state.otaState    = TunerState::OtaState::DOWNLOADING_FS;
        g_state.otaProgress = 45;
    }
    LOG_INFO("OTA", "Flashing FS ...");

    if (!downloadAndFlash(manifest.fsUrl, U_SPIFFS, manifest.fsMd5, manifest.fsSize, 45, 50)) {
        StateLock lock;
        g_state.otaState = TunerState::OtaState::ERROR;
        strlcpy(g_state.otaError, "FS flash failed (FW already applied)", sizeof(g_state.otaError));
        return false;
    }

    {
        StateLock lock;
        g_state.otaState    = TunerState::OtaState::DONE;
        g_state.otaProgress = 100;
    }
    LOG_INFO("OTA", "OTA complete – rebooting");
    delay(1000);
    ESP.restart();
    return true;
}

bool OTAUpdater::flashLocal(int partition, Stream& data, size_t size, const char* md5) {
    if (!Update.begin(size, partition)) {
        LOG_ERROR("OTA", "Update.begin failed for local flash");
        return false;
    }
    size_t written = Update.writeStream(data);
    if (written != size) {
        LOG_ERROR("OTA", "Written %u != expected %u", written, size);
        Update.abort();
        return false;
    }
    if (!Update.end(true)) {
        LOG_ERROR("OTA", "Update.end failed: %s", Update.errorString());
        return false;
    }
    // MD5 is verified by the caller (or WebCrypto on browser side)
    LOG_INFO("OTA", "Local flash OK: %u bytes", written);
    return true;
}
