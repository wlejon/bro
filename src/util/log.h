#pragma once

namespace bro::util {

enum class LogLevel {
    Info,
    Warn,
    Error
};

void log(LogLevel level, const char* fmt, ...);

} // namespace bro::util

#define LOG_INFO(fmt, ...)  ::bro::util::log(::bro::util::LogLevel::Info,  fmt, ##__VA_ARGS__)
#define LOG_WARN(fmt, ...)  ::bro::util::log(::bro::util::LogLevel::Warn,  fmt, ##__VA_ARGS__)
#define LOG_ERROR(fmt, ...) ::bro::util::log(::bro::util::LogLevel::Error, fmt, ##__VA_ARGS__)
