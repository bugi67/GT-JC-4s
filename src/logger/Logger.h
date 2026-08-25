#pragma once
#include <Arduino.h>

enum class LogLevel : uint8_t { ERROR = 0, WARN = 1, INFO = 2, DEBUG = 3 };

class Logger {
public:
    static void init();
    static void setLevel(LogLevel level);
    static LogLevel getLevel();
    static void log(LogLevel level, const char* module, const char* fmt, ...);
    static void setPublishHook(void (*fn)(const char*));

private:
    static LogLevel          s_level;
    static SemaphoreHandle_t s_mutex;
    static void            (*s_publishHook)(const char*);
};

// ── Compile-time minimum level ────────────────────────────────────────────────
#ifndef LOG_LEVEL_DEFAULT
#define LOG_LEVEL_DEFAULT 2
#endif

// Filtering happens at runtime in Logger::log() against the level set via
// Logger::setLevel() - these macros must not also gate at compile time,
// otherwise LOG_LEVEL_DEFAULT permanently disables every less-verbose level
// (e.g. default 2 would compile out LOG_ERROR/LOG_WARN entirely).
#define LOG_ERROR(mod, fmt, ...) Logger::log(LogLevel::ERROR, mod, fmt, ##__VA_ARGS__)
#define LOG_WARN(mod, fmt, ...)  Logger::log(LogLevel::WARN, mod, fmt, ##__VA_ARGS__)
#define LOG_INFO(mod, fmt, ...)  Logger::log(LogLevel::INFO, mod, fmt, ##__VA_ARGS__)
#define LOG_DEBUG(mod, fmt, ...) Logger::log(LogLevel::DEBUG, mod, fmt, ##__VA_ARGS__)
