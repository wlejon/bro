#include "util/time.h"

#ifdef _WIN32
#include <windows.h>
#if defined(_M_X64) || defined(_M_IX86)
#include <intrin.h>
#endif
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

#ifdef _WIN32
namespace {

#if defined(_M_X64) || defined(_M_IX86)
// TSC ticks per millisecond: the TSC against QueryPerformanceCounter over a
// short window (both wall clocks, so a preemption during it changes
// nothing). Taken once; invariant TSCs run at one rate whatever the core's
// clock does. This is how Chromium converts thread cycles too.
double tscTicksPerMs() {
    static const double rate = [] {
        LARGE_INTEGER f, q0, q1;
        QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&q0);
        const unsigned long long t0 = __rdtsc();
        do {
            QueryPerformanceCounter(&q1);
        } while (double(q1.QuadPart - q0.QuadPart) * 1000.0 / double(f.QuadPart) < 20.0);
        const unsigned long long t1 = __rdtsc();
        const double ms = double(q1.QuadPart - q0.QuadPart) * 1000.0 / double(f.QuadPart);
        return double(t1 - t0) / ms;
    }();
    return rate;
}
#endif

} // namespace
#endif

double threadCpuTimeMs() {
#ifdef _WIN32
#if defined(_M_X64) || defined(_M_IX86)
    ULONG64 cycles = 0;
    if (QueryThreadCycleTime(GetCurrentThread(), &cycles)) return double(cycles) / tscTicksPerMs();
#endif
    FILETIME created, exited, kernel, user;
    if (!GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user)) return 0.0;
    auto ms = [](const FILETIME& t) {
        return double((ULONGLONG(t.dwHighDateTime) << 32) | t.dwLowDateTime) / 10000.0;
    };
    return ms(kernel) + ms(user);
#else
    struct timespec ts;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1000000.0;
#endif
}

} // namespace bro::util
