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

    std::fputc('[', stderr);
    writeTimestamp(stderr);
    std::fprintf(stderr, "] %s ", prefix);

    va_list args;
    va_start(args, fmt);
    std::vfprintf(stderr, fmt, args);
    va_end(args);

    std::fputc('\n', stderr);
    std::fflush(stderr);
}

} // namespace bro::util
