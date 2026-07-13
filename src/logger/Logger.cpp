#include "Logger.h"
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <stdarg.h>
#include <stdio.h>

LogLevel           Logger::s_level = LogLevel::INFO;
SemaphoreHandle_t  Logger::s_mutex = nullptr;

static Logger::LogEntry s_ring[Logger::LOG_RING_CAPACITY];
static uint32_t         s_ringSeq = 0;

static const char* levelStr(LogLevel l) {
    switch (l) {
        case LogLevel::ERROR: return "ERROR";
        case LogLevel::WARN:  return "WARN ";
        case LogLevel::INFO:  return "INFO ";
        case LogLevel::DEBUG: return "DEBUG";
        default:              return "?    ";
    }
}

void Logger::init() {
    s_mutex = xSemaphoreCreateMutex();
}

void Logger::setLevel(LogLevel level) {
    s_level = level;
}

LogLevel Logger::getLevel() {
    return s_level;
}

void Logger::log(LogLevel level, const char* module, const char* fmt, ...) {
    if (static_cast<uint8_t>(level) > static_cast<uint8_t>(s_level)) return;

    char msgBuf[192];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msgBuf, sizeof(msgBuf), fmt, ap);
    va_end(ap);

    char lineBuf[256];
    snprintf(lineBuf, sizeof(lineBuf), "[%7lums] [%s] [%-8.8s] %s\r\n",
             (unsigned long)millis(), levelStr(level), module, msgBuf);

    if (s_mutex) xSemaphoreTake(s_mutex, portMAX_DELAY);
    Serial.print(lineBuf);

    // Write to ring buffer (strip trailing \r\n before storing)
    uint32_t seq = ++s_ringSeq;
    int idx = (int)((seq - 1) % (uint32_t)LOG_RING_CAPACITY);
    s_ring[idx].seq = seq;
    int len = (int)strlen(lineBuf);
    while (len > 0 && (lineBuf[len - 1] == '\r' || lineBuf[len - 1] == '\n')) len--;
    int copyLen = (len < (int)sizeof(s_ring[idx].text) - 1) ? len : (int)sizeof(s_ring[idx].text) - 1;
    memcpy(s_ring[idx].text, lineBuf, copyLen);
    s_ring[idx].text[copyLen] = '\0';

    if (s_mutex) xSemaphoreGive(s_mutex);
}

uint32_t Logger::getEntries(uint32_t afterSeq, LogEntry* buf, int maxCount, int* outCount) {
    *outCount = 0;
    if (!s_mutex) return afterSeq;
    xSemaphoreTake(s_mutex, portMAX_DELAY);

    uint32_t tail  = s_ringSeq;
    uint32_t start = afterSeq;
    if (tail >= (uint32_t)LOG_RING_CAPACITY && start < tail - (uint32_t)LOG_RING_CAPACITY)
        start = tail - (uint32_t)LOG_RING_CAPACITY;

    int available = (tail > start) ? (int)(tail - start) : 0;
    int count     = (available < maxCount) ? available : maxCount;

    for (int i = 0; i < count; i++) {
        uint32_t seq = start + 1 + (uint32_t)i;
        int slot = (int)((seq - 1) % (uint32_t)LOG_RING_CAPACITY);
        if (s_ring[slot].seq == seq) buf[(*outCount)++] = s_ring[slot];
    }

    xSemaphoreGive(s_mutex);
    return (count > 0) ? start + (uint32_t)count : afterSeq;
}
