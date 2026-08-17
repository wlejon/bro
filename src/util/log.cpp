#include "util/log.h"

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <ctime>

namespace bro::util {

namespace {

void writeTimestamp(FILE* f) {
    using namespace std::chrono;
    auto now = system_clock::now();
    auto ms = duration_cast<milliseconds>(now.time_since_epoch()) % 1000;
    std::time_t t = system_clock::to_time_t(now);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    std::fprintf(f, "%02d:%02d:%02d.%03d",
                 tm.tm_hour, tm.tm_min, tm.tm_sec,
                 static_cast<int>(ms.count()));
}

} // namespace

void log(LogLevel level, const char* fmt, ...) {
    const char* prefix = "";
    switch (level) {
        case LogLevel::Info:  prefix = "[INFO]";  break;
        case LogLevel::Warn:  prefix = "[WARN]";  break;
        case LogLevel::Error: prefix = "[ERROR]"; break;
    }

    char line[4096];
    using namespace std::chrono;
    auto now = system_clock::now();
    auto ms = duration_cast<milliseconds>(now.time_since_epoch()) % 1000;
    std::time_t t = system_clock::to_time_t(now);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    int headerLen = std::snprintf(line, sizeof(line), "[%02d:%02d:%02d.%03d] %s ",
                                  tm.tm_hour, tm.tm_min, tm.tm_sec,
                                  static_cast<int>(ms.count()), prefix);

    va_list args;
    va_start(args, fmt);
    int msgLen = (headerLen < static_cast<int>(sizeof(line)) - 2)
                     ? std::vsnprintf(line + headerLen, sizeof(line) - headerLen - 2, fmt, args)
                     : 0;
    va_end(args);

    int totalLen = headerLen + (msgLen > 0 ? msgLen : 0);
    if (totalLen >= static_cast<int>(sizeof(line)) - 2) {
        totalLen = static_cast<int>(sizeof(line)) - 2;
    }
    line[totalLen] = '\n';
    line[totalLen + 1] = '\0';

    std::fputs(line, stderr);
    std::fflush(stderr);
}

} // namespace bro::util
