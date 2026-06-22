#pragma once
#include <Arduino.h>

struct OTAManifest {
    char version[16];
    char fwUrl[256];
    char fwMd5[33];
    uint32_t fwSize;
    char fsUrl[256];
    char fsMd5[33];
    uint32_t fsSize;
    char changelog[128];
};

class OTAUpdater {
public:
    // Check manifest URL for newer version; returns true if update available
    static bool checkGitHub(OTAManifest& manifest);

    // Download and flash FW + FS from GitHub manifest
    static bool installGitHub(const OTAManifest& manifest);

    // Flash firmware from local buffer; md5 is expected hex string (32 chars)
    static bool flashLocal(int partition, Stream& data, size_t size, const char* md5);

private:
    static bool downloadAndFlash(const char* url, int partition, const char* expectedMd5,
                                 uint32_t totalSize, uint8_t progressBase, uint8_t progressRange);
    static void reportProgress(uint8_t pct, const char* state);
};
