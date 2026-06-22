#pragma once
#include <Arduino.h>

enum class LogLevel : uint8_t { ERROR = 0, WARN = 1, INFO = 2, DEBUG = 3 };

class Logger {
public:
    static void init();
    static void setLevel(LogLevel level);
    static LogLevel getLevel();
    static void log(LogLevel level, const char* module, const char* fmt, ...);

private:
    static LogLevel  s_level;
    static SemaphoreHandle_t s_mutex;
};

// ── Compile-time minimum level ────────────────────────────────────────────────
#ifndef LOG_LEVEL_DEFAULT
#define LOG_LEVEL_DEFAULT 2
#endif

#if LOG_LEVEL_DEFAULT <= 0
#define LOG_ERROR(mod, fmt, ...) Logger::log(LogLevel::ERROR, mod, fmt, ##__VA_ARGS__)
#else
#define LOG_ERROR(mod, fmt, ...) do {} while(0)
#endif

#if LOG_LEVEL_DEFAULT <= 1
#define LOG_WARN(mod, fmt, ...)  Logger::log(LogLevel::WARN,  mod, fmt, ##__VA_ARGS__)
#else
#define LOG_WARN(mod, fmt, ...)  do {} while(0)
#endif

#if LOG_LEVEL_DEFAULT <= 2
#define LOG_INFO(mod, fmt, ...)  Logger::log(LogLevel::INFO,  mod, fmt, ##__VA_ARGS__)
#else
#define LOG_INFO(mod, fmt, ...)  do {} while(0)
#endif

#if LOG_LEVEL_DEFAULT <= 3
#define LOG_DEBUG(mod, fmt, ...) Logger::log(LogLevel::DEBUG, mod, fmt, ##__VA_ARGS__)
#else
#define LOG_DEBUG(mod, fmt, ...) do {} while(0)
#endif
