#include "util/time.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#endif

namespace bro::util {

double currentTimeMs() {
#ifdef _WIN32
    static LARGE_INTEGER freq = {};
    if (freq.QuadPart == 0) {
        QueryPerformanceFrequency(&freq);
    }
    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);
    return static_cast<double>(counter.QuadPart) * 1000.0 / static_cast<double>(freq.QuadPart);
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1000000.0;
#endif
}

} // namespace bro::util
