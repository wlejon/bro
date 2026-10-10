#include "bronze_host/host_gc.h"

#include "bronze_host/bronze_host.h"
#include "bronze_host/host_globals_internal.h"
#include "bronze_host/host_brokit.h"

#include "embed/embed.h"

#include "util/log.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#  include <psapi.h>
#elif defined(__linux__)
#  include <unistd.h>
#endif

namespace bro::bronze_host {
namespace {

constexpr double kIdleQuiescentThresholdMs = 1000.0;  // 1 second quiescent before idle GC
constexpr double kIdlePeriodicIntervalMs = 10000.0;    // 10 seconds of idle between periodic GCs

// Memory pressure: a page that never goes quiescent (a rAF loop, a scan in
// flight, a worker streaming bitmaps) is still collected once the process has
// grown by this much since the last collection. Native memory held by JS
// wrappers does not count toward the JS heap's own allocation triggers, so a
// page that allocates little JS but much native memory would otherwise hold
// all of it until it next goes idle.
constexpr uint64_t kPressureMinGrowthBytes = 64ull << 20;  // at least 64 MB
constexpr uint64_t kPressureGrowthDivisor = 4;             // or a quarter of the baseline
constexpr double kPressureMinIntervalMs = 1000.0;          // at most one a second
constexpr double kPressureSampleIntervalMs = 250.0;        // sample private bytes 4x a second
constexpr double kPressureMaxWaitMs = 2000.0;              // then collect even on a busy frame
constexpr double kPressureQuietMs = 100.0;                 // quiet this long: collect now

double s_idleAccumulatorMs = 0.0;
double s_timeSinceLastGcMs = 0.0;
double s_timeSinceSampleMs = 0.0;
double s_pressureWaitedMs = 0.0;
uint64_t s_bytesAfterLastGc = 0;
bool s_collectedForCurrentIdle = false;
bool s_pressurePending = false;
std::atomic<int> s_evalDepth{0};

// BRO_GC_PRESSURE=0 turns the memory-pressure collection off; BRO_GC_LOG=1
// logs every collection this file starts, with why and how long it took.
bool envFlag(const char* name, bool fallback) {
    const char* v = std::getenv(name);
    if (!v || !*v) return fallback;
    return !(v[0] == '0' && v[1] == '\0');
}
bool pressureEnabled() {
    static const bool on = envFlag("BRO_GC_PRESSURE", true);
    return on;
}
bool gcLogEnabled() {
    static const bool on = envFlag("BRO_GC_LOG", false);
    return on;
}

void collectFor(const char* why);

bool underMemoryPressure() {
    if (s_timeSinceLastGcMs < kPressureMinIntervalMs) return false;
    if (s_timeSinceSampleMs < kPressureSampleIntervalMs) return false;
    s_timeSinceSampleMs = 0.0;
    const uint64_t now = hostProcessPrivateBytes();
    if (now == 0) return false;
    if (s_bytesAfterLastGc == 0) {
        s_bytesAfterLastGc = now;
        return false;
    }
    const uint64_t growth = std::max(kPressureMinGrowthBytes, s_bytesAfterLastGc / kPressureGrowthDivisor);
    return now > s_bytesAfterLastGc + growth;
}

// Compiled JS frames on the stack do not block a collection (their Values are
// stack-map roots the collector walks to); a host eval in progress does.
bool isExecutionStackActive() {
    return s_evalDepth.load(std::memory_order_relaxed) > 0;
}

}  // namespace

void hostEnterEval() {
    s_evalDepth.fetch_add(1, std::memory_order_relaxed);
}

void hostLeaveEval() {
    s_evalDepth.fetch_sub(1, std::memory_order_relaxed);
}

HostEvalSuspend::HostEvalSuspend() : depth_(s_evalDepth.exchange(0, std::memory_order_relaxed)) {}

HostEvalSuspend::~HostEvalSuspend() {
    s_evalDepth.fetch_add(depth_, std::memory_order_relaxed);
}

bool isHostEvaluating() {
    return s_evalDepth.load(std::memory_order_relaxed) > 0;
}

void hostCollectGarbage() {
    if (isExecutionStackActive()) {
        return;
    }
    bronze::embed::collectGarbage();
    s_pressurePending = false;
    s_timeSinceLastGcMs = 0.0;
    s_timeSinceSampleMs = 0.0;
    s_bytesAfterLastGc = hostProcessPrivateBytes();
    s_collectedForCurrentIdle = true;
}

bool hostGcLogEnabled() {
    return gcLogEnabled();
}

namespace {
void collectFor(const char* why) {
    if (!gcLogEnabled()) {
        hostCollectGarbage();
        return;
    }
    const uint64_t before = hostProcessPrivateBytes();
    const auto t0 = std::chrono::steady_clock::now();
    hostCollectGarbage();
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    LOG_INFO("gc: %s collection in %.1f ms, private %.1f -> %.1f MB", why, ms, before / 1048576.0,
             hostProcessPrivateBytes() / 1048576.0);
}
}  // namespace

uint64_t hostProcessPrivateBytes() {
#if defined(_WIN32)
    PROCESS_MEMORY_COUNTERS_EX pmc{};
    if (GetProcessMemoryInfo(GetCurrentProcess(),
                             reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc)))
        return static_cast<uint64_t>(pmc.PrivateUsage);
    return 0;
#elif defined(__linux__)
    unsigned long size = 0, resident = 0;
    FILE* f = std::fopen("/proc/self/statm", "r");
    if (!f) return 0;
    const int got = std::fscanf(f, "%lu %lu", &size, &resident);
    std::fclose(f);
    if (got != 2) return 0;
    return static_cast<uint64_t>(resident) * static_cast<uint64_t>(sysconf(_SC_PAGESIZE));
#else
    return 0;
#endif
}

void hostResetIdleGCTimer() {
    s_idleAccumulatorMs = 0.0;
    s_timeSinceLastGcMs = 0.0;
    s_timeSinceSampleMs = 0.0;
    s_bytesAfterLastGc = 0;
    s_collectedForCurrentIdle = false;
}

void hostNotifyIdleFrame(double dtMs) {
    if (!isWebHostGlobalsInstalled()) return;
    if (isExecutionStackActive()) return;

    const double delta = (dtMs > 0.0) ? dtMs : 16.67;
    s_timeSinceLastGcMs += delta;
    s_timeSinceSampleMs += delta;

    const bool microtasks = bronze::embed::microtasksPending();
    const bool rafs = hasPendingAnimationFrames();
    const bool brokit = brokitHasWorkInFlight();
    const bool isQuiescent = (!microtasks && !rafs && !brokit);

    if (isQuiescent) {
        s_idleAccumulatorMs += delta;
        if (!s_collectedForCurrentIdle && s_idleAccumulatorMs >= kIdleQuiescentThresholdMs)
            collectFor("idle");
        else if (s_timeSinceLastGcMs >= kIdlePeriodicIntervalMs)
            collectFor("periodic");
        else if (s_pressurePending && s_idleAccumulatorMs >= kPressureQuietMs)
            collectFor("pressure (quiet)");
    } else {
        s_idleAccumulatorMs = 0.0;
        s_collectedForCurrentIdle = false;
        // Under pressure a busy page is collected at its next quiescent
        // frame, or, if it never has one, once the pressure has waited
        // kPressureMaxWaitMs: not in the frame that made it (a big bitmap
        // arriving is a frame the page is busy showing it).
        if (!s_pressurePending && pressureEnabled() && underMemoryPressure()) {
            s_pressurePending = true;
            s_pressureWaitedMs = 0.0;
        } else if (s_pressurePending) {
            s_pressureWaitedMs += delta;
            if (s_pressureWaitedMs >= kPressureMaxWaitMs) {
                collectFor("pressure (busy)");
                // A busy page still gets its idle collection when it settles.
                s_collectedForCurrentIdle = false;
            }
        }
    }
}

}  // namespace bro::bronze_host
