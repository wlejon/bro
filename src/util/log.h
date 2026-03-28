#pragma once

#include <cstdio>
#include <cstdarg>

namespace bro::util {

enum class LogLevel {
    Info,
    Warn,
    Error
};

inline void log(LogLevel level, const char* file, int line, const char* fmt, ...) {
    const char* prefix = "";
    switch (level) {
        case LogLevel::Info:  prefix = "[INFO]";  break;
        case LogLevel::Warn:  prefix = "[WARN]";  break;
        case LogLevel::Error: prefix = "[ERROR]"; break;
    }

    std::fprintf(stderr, "%s %s:%d: ", prefix, file, line);

    va_list args;
    va_start(args, fmt);
    std::vfprintf(stderr, fmt, args);
    va_end(args);

    std::fprintf(stderr, "\n");
}

} // namespace bro::util

#define LOG_INFO(fmt, ...)  ::bro::util::log(::bro::util::LogLevel::Info,  __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define LOG_WARN(fmt, ...)  ::bro::util::log(::bro::util::LogLevel::Warn,  __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define LOG_ERROR(fmt, ...) ::bro::util::log(::bro::util::LogLevel::Error, __FILE__, __LINE__, fmt, ##__VA_ARGS__)
