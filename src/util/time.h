#pragma once

namespace bro::util {

/// Returns the current time in milliseconds (monotonic clock).
/// Cross-platform: uses QueryPerformanceCounter on Windows, clock_gettime elsewhere.
double currentTimeMs();

/// CPU time the calling thread has run, in milliseconds, at a fine
/// resolution: what a measurement of a thread's own work reads instead of
/// the wall clock, which also counts the time other processes had the core.
/// CLOCK_THREAD_CPUTIME_ID on POSIX; on Windows the thread's cycle count
/// (QueryThreadCycleTime, which counts TSC ticks) over the TSC frequency,
/// since GetThreadTimes only advances once per scheduler tick (15.6 ms).
double threadCpuTimeMs();

} // namespace bro::util
