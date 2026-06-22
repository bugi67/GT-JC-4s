#include "Logger.h"
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <stdarg.h>
#include <stdio.h>

LogLevel           Logger::s_level = LogLevel::INFO;
SemaphoreHandle_t  Logger::s_mutex = nullptr;

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
    if (s_mutex) xSemaphoreGive(s_mutex);
}
