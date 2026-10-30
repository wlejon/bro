#include "bronze_host/host_gc.h"

#include "bronze_host/bronze_host.h"
#include "bronze_host/host_globals_internal.h"
#include "bronze_host/host_brokit.h"

#include "embed/embed.h"
#include "embed/embed_gc.h"

#include "util/log.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

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
#elif defined(__APPLE__)
#  include <mach/mach.h>
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

// Between frames (hostCollectBetweenFrames): the heap is collected ahead of
// its own triggers, so they seldom fire in the middle of a frame. A minor
// collection once eden is this full, a full one once the old generation has
// grown this near its threshold. A frame allocating more than the rest of
// eden still collects where it fills it. Eden's capacity is the collector's
// adaptive limit (brass HeapConfig::adaptive_eden), which shrinks while much
// of each eden survives so a minor collection's copying stays short; the
// fill is of that limit, so the early collection keeps the same bound.
constexpr double kBetweenFramesEdenFill = 0.5;
constexpr double kBetweenFramesOldFill = 0.75;

// The old generation is collected incrementally between frames
// (BRO_GC_INCREMENTAL, on unless 0): a marking cycle (bronze embed_gc.h)
// starts once the old generation has grown this far toward its threshold,
// which leaves the cycle the rest of the threshold's growth to finish in
// before the heap's own trigger would finish it at an allocation. A program
// that promotes faster than the slices mark makes its cycle fall behind
// (below); its next cycle then starts kCycleStartStep earlier, down to
// kCycleStartMin, and one that finishes well ahead moves back up.
constexpr double kCycleStartFill = 0.5;
constexpr double kCycleStartMin = 0.2;
constexpr double kCycleStartStep = 0.1;
constexpr double kCycleAheadFill = 0.65;
// A cycle still marking when the growth reaches this has fallen behind the
// program's allocation: its slices get kCycleBehindFactor times the budget.
constexpr double kCycleBehindFill = 0.75;
constexpr double kCycleBehindFactor = 2.0;
// And at this it is finished now, one longer pause between frames, rather
// than by the heap's trigger inside a frame.
constexpr double kCycleFallbackFill = 0.9;
// The GC work a frame aims at, in ms (BRO_GC_FRAME_MS): a minor collection
// due this frame comes out of it first, then a mark or sweep slice gets what
// is left, but a mark slice never less than kMinMarkSliceMs, so a program
// whose every frame fills eden still gets its marking done. The planet runs
// at 360 frames a second, 2.8 ms each; half a millisecond of that is the
// collector's.
constexpr double kDefaultFrameBudgetMs = 0.5;
// brass runs a slice on its marker threads only when its budget is over
// 0.2 ms (below that, starting and joining them costs too much of it), and a
// slice on one thread marks several times less: a shorter one is not worth
// having.
constexpr double kMinMarkSliceMs = 0.25;
// A sweep slice shorter than this waits for the next frame.
constexpr double kMinSweepSliceMs = 0.05;
// The finish evacuates the whole young generation (every survivor promoted
// and scanned) and rescans the dirty cards, so a cycle whose marking is done
// with eden this full runs a minor collection first, in a frame of its own,
// and finishes in the next one, with one frame's allocation young.
constexpr double kPreFinishEdenFill = 0.1;

double s_idleAccumulatorMs = 0.0;
double s_timeSinceLastGcMs = 0.0;
double s_timeSinceSampleMs = 0.0;
double s_pressureWaitedMs = 0.0;
uint64_t s_bytesAfterLastGc = 0;
bool s_collectedForCurrentIdle = false;
bool s_pressurePending = false;
// A full collection the frame's policy decided on, run between frames.
const char* s_requested = nullptr;
bool s_requestedIsIdleSpell = false;  // it counts as the idle spell's collection
// The page's boot data is still young: the first frames' slot promotes it
// with a full collection, so it is copied once rather than aged through two
// minor collections mid-animation.
bool s_bootCollected = false;
std::atomic<int> s_evalDepth{0};

// The marking cycle this file opened (hostCollectBetweenFrames). The heap may
// finish it itself (old-generation growth at an allocation, collectGarbage),
// so whether one is open is always read from the heap; this is the cycle's
// bookkeeping, for the policy and the log.
struct Cycle {
    bool open = false;
    bool markDone = false;  // the last slice drained the grey work: finish next
    bool preFinishMinor = false;  // the minor before the finish has run
    bool behind = false;          // marked at kCycleBehindFill or later
    bool sweeping = false;  // finished by this file, its sweep not yet done
    uint32_t frames = 0;
    uint32_t slices = 0;
    double startMs = 0.0;
    double markMs = 0.0;
    double sliceMaxMs = 0.0;
    double finishMs = 0.0;
    double sweepMs = 0.0;
    uint32_t sweepSlices = 0;
    uint64_t index = 0;
};
Cycle s_cycle;
uint64_t s_cycles = 0;
uint64_t s_cyclesFinished = 0;
uint32_t s_lastCycleFrames = 0;
// Where the next cycle starts (kCycleStartFill, adapted; below).
double s_startFill = -1.0;

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
// BRO_GC_INCREMENTAL=0: the old generation is collected as before, a full
// collection between frames once it nears its threshold (the A/B switch).
bool incrementalEnabled() {
    static const bool on = envFlag("BRO_GC_INCREMENTAL", true);
    return on;
}
double envNumber(const char* name, double fallback, double lo, double hi) {
    const char* v = std::getenv(name);
    if (!v || !*v) return fallback;
    char* end = nullptr;
    const double d = std::strtod(v, &end);
    if (end == v || *end != '\0' || !(d >= lo && d <= hi)) {
        LOG_ERROR("gc: %s=%s is not a number in [%g, %g]", name, v, lo, hi);
        std::abort();
    }
    return d;
}
double frameBudgetMs() {
    static const double ms = envNumber("BRO_GC_FRAME_MS", kDefaultFrameBudgetMs, 0.01, 1000.0);
    return ms;
}
// BRO_GC_CYCLE_START: the fill of the threshold a cycle starts at
// (kCycleStartFill); 0 starts one as soon as the last is swept, for
// measuring cycles on a heap that seldom grows that far.
double configuredStartFill() {
    static const double f = envNumber("BRO_GC_CYCLE_START", kCycleStartFill, 0.0, kCycleBehindFill);
    return f;
}
double cycleStartFill() {
    if (s_startFill < 0.0) s_startFill = configuredStartFill();
    return s_startFill;
}
// After a cycle's finish at `fill`: the next one starts earlier if this one
// fell behind, later (up to the configured fill) if it finished well ahead.
void adaptStartFill(bool behind, double fill) {
    const double configured = configuredStartFill();
    double f = cycleStartFill();
    if (behind) f = std::max(std::min(kCycleStartMin, configured), f - kCycleStartStep);
    else if (fill < kCycleAheadFill) f = std::min(configured, f + kCycleStartStep / 2);
    s_startFill = f;
}

void collectFor(const char* why);

// Collections the frame's policy decides on run after the frame is
// presented, not in the frame (hostCollectBetweenFrames).
void requestFor(const char* why, bool idleSpell = true) {
    if (s_requested) return;
    s_requested = why;
    s_requestedIsIdleSpell = idleSpell;
}

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

namespace {
// After a full collection, or a cycle's finish: the memory-pressure and idle
// clocks start over from what is left.
void noteFullCollected() {
    s_requested = nullptr;
    s_bootCollected = true;
    s_pressurePending = false;
    s_timeSinceLastGcMs = 0.0;
    s_timeSinceSampleMs = 0.0;
    s_bytesAfterLastGc = hostProcessPrivateBytes();
}
}  // namespace

void hostCollectGarbage() {
    if (isExecutionStackActive()) {
        return;
    }
    // A cycle open now is finished by this collection (and a whole one run
    // after it), and a sweep left is completed: its bookkeeping goes with it.
    bronze::embed::collectGarbage();
    if ((s_cycle.open || s_cycle.sweeping) && gcLogEnabled())
        LOG_INFO("gc: cycle %llu ended by a full collection after %u frames",
                 static_cast<unsigned long long>(s_cycle.index), s_cycle.frames);
    s_cycle = Cycle{};
    noteFullCollected();
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
#elif defined(__APPLE__)
    // The task's physical footprint: what Activity Monitor calls its memory,
    // and macOS's measure of what a process costs the machine (without it the
    // memory-pressure collection never fired on macOS).
    task_vm_info_data_t info{};
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO, reinterpret_cast<task_info_t>(&info), &count) != KERN_SUCCESS)
        return 0;
    return static_cast<uint64_t>(info.phys_footprint);
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
    s_requested = nullptr;
    s_bootCollected = false;
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
            requestFor("idle");
        else if (s_timeSinceLastGcMs >= kIdlePeriodicIntervalMs)
            requestFor("periodic");
        else if (s_pressurePending && s_idleAccumulatorMs >= kPressureQuietMs)
            requestFor("pressure (quiet)");
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
                requestFor("pressure (busy)", /*idleSpell=*/false);
            }
        }
    }
}

namespace {

double msSince(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

// One frame's GC work, for the frame recorder and the log.
struct FrameGc {
    uint32_t work = 0;
    double minorMs = 0.0, startMs = 0.0, markMs = 0.0, finishMs = 0.0, sweepMs = 0.0, fullMs = 0.0;
    const char* note = nullptr;

    double total() const { return minorMs + startMs + markMs + finishMs + sweepMs + fullMs; }
    void log() const {
        if (!work || !gcLogEnabled()) return;
        char parts[256];
        int n = 0;
        auto part = [&](const char* name, double ms) {
            if (ms > 0.0 && n < static_cast<int>(sizeof parts))
                n += std::snprintf(parts + n, sizeof parts - n, " %s %.3f", name, ms);
        };
        part("minor", minorMs);
        part("start", startMs);
        part("mark", markMs);
        part("finish", finishMs);
        part("sweep", sweepMs);
        part("full", fullMs);
        LOG_INFO("gc: between frames %.3f ms:%s%s%s", total(), n ? parts : "", note ? " | " : "", note ? note : "");
    }
};

void minorIfDue(const bronze::embed::HeapPressure& p, FrameGc& f) {
    if (p.edenCapacity == 0 ||
        static_cast<double>(p.edenUsed) < kBetweenFramesEdenFill * static_cast<double>(p.edenCapacity))
        return;
    const auto t0 = std::chrono::steady_clock::now();
    bronze::embed::collectYoungGarbage();
    f.minorMs = msSince(t0);
    f.work |= kGcWorkMinor;
}

void openCycle(double startMs) {
    s_cycle = Cycle{};
    s_cycle.open = true;
    s_cycle.frames = 1;
    s_cycle.startMs = startMs;
    s_cycle.index = ++s_cycles;
}

void finishCycle(FrameGc& f, double fill, const char* why) {
    adaptStartFill(s_cycle.behind, fill);
    const auto t0 = std::chrono::steady_clock::now();
    bronze::embed::finishGcCycle();
    f.finishMs = msSince(t0);
    f.work |= kGcWorkFinish;
    if (why) f.note = why;
    s_cycle.finishMs = f.finishMs;
    ++s_cyclesFinished;
    s_lastCycleFrames = s_cycle.frames;
    s_cycle.open = false;
    s_cycle.markDone = false;
    s_cycle.sweeping = true;
    noteFullCollected();
}

// The cycle's summary, once its sweep is done (or the heap took it over).
void logCycleEnd(const char* how) {
    if (!gcLogEnabled()) return;
    const Cycle& c = s_cycle;
    LOG_INFO("gc: cycle %llu %s: %u frames, start %.3f ms, %u mark slices %.3f ms (longest %.3f), finish %.3f ms, "
             "%u sweep slices %.3f ms%s; the next starts at %.0f%% of the threshold",
             static_cast<unsigned long long>(c.index), how, c.frames, c.startMs, c.slices, c.markMs, c.sliceMaxMs,
             c.finishMs, c.sweepSlices, c.sweepMs, c.behind ? ", fell behind" : "", cycleStartFill() * 100.0);
}

// The incremental half of the between-frames policy: a cycle started at
// kCycleStartFill of the threshold, then each frame a slice of what the
// frame's budget has left after a minor collection, the finish in a frame of
// its own once a slice drained the grey work, then sweep slices.
void cycleStep(const bronze::embed::HeapPressure& p, FrameGc& f) {
    using clock = std::chrono::steady_clock;
    const double fill = p.fullThreshold > 0
                            ? static_cast<double>(p.oldGrowth) / static_cast<double>(p.fullThreshold)
                            : 0.0;
    if (s_cycle.open && !p.marking) {
        // Finished by the heap's own trigger or a full collection: the pause
        // happened where the heap wanted it.
        logCycleEnd("finished by the heap");
        s_cycle = Cycle{};
    }
    if (p.marking && s_cycle.open) {
        ++s_cycle.frames;
        if (fill >= kCycleFallbackFill) {
            s_cycle.behind = true;
            finishCycle(f, fill, "fallback: the old generation reached 90% of its threshold while marking");
            return;
        }
        if (s_cycle.markDone) {
            if (!s_cycle.preFinishMinor && p.edenCapacity > 0 &&
                static_cast<double>(p.edenUsed) >= kPreFinishEdenFill * static_cast<double>(p.edenCapacity)) {
                s_cycle.preFinishMinor = true;
                const auto t0 = clock::now();
                bronze::embed::collectYoungGarbage();
                f.minorMs = msSince(t0);
                f.work |= kGcWorkMinor;
                f.note = "before the finish";
                return;
            }
            // The frame's work is the finish alone: the young generation it
            // evacuates is about one frame's allocation, the grey work what
            // the program shaded since the last slice.
            finishCycle(f, fill, nullptr);
            return;
        }
        minorIfDue(p, f);
        const bool behind = fill >= kCycleBehindFill;
        if (behind) s_cycle.behind = true;
        const double budget =
            std::max(kMinMarkSliceMs, frameBudgetMs() * (behind ? kCycleBehindFactor : 1.0) - f.total());
        const auto t0 = clock::now();
        s_cycle.markDone = bronze::embed::gcMarkSlice(static_cast<uint64_t>(budget * 1e6));
        f.markMs = msSince(t0);
        f.work |= kGcWorkMark;
        ++s_cycle.slices;
        s_cycle.markMs += f.markMs;
        s_cycle.sliceMaxMs = std::max(s_cycle.sliceMaxMs, f.markMs);
        if (behind) f.note = "behind: the slice's budget doubled";
        return;
    }
    if (p.marking) return;  // a cycle someone else opened: theirs to drive
    minorIfDue(p, f);
    const double budget = frameBudgetMs() - f.total();
    if (p.sweeping) {
        if (budget < kMinSweepSliceMs && fill < kCycleBehindFill) return;
        const auto t0 = clock::now();
        const bool swept =
            bronze::embed::gcSweepSlice(static_cast<uint64_t>(std::max(budget, kMinSweepSliceMs) * 1e6));
        f.sweepMs = msSince(t0);
        f.work |= kGcWorkSweep;
        if (s_cycle.sweeping) {
            s_cycle.sweepMs += f.sweepMs;
            ++s_cycle.sweepSlices;
            if (swept) {
                logCycleEnd("swept");
                s_cycle = Cycle{};
            }
        }
        // A cycle starts only once the last one is swept (its start would
        // finish the sweep in its own pause), unless the growth will not wait.
        if (!swept && fill < kCycleBehindFill) return;
    } else if (s_cycle.sweeping) {
        // Swept by the allocator, or by a full collection.
        logCycleEnd("swept by the heap");
        s_cycle = Cycle{};
    }
    if (p.fullThreshold == 0 || fill < cycleStartFill()) return;
    const auto t0 = clock::now();
    if (!bronze::embed::startGcCycle()) return;
    f.startMs = msSince(t0);
    f.work |= kGcWorkStart;
    openCycle(f.startMs);
}

}  // namespace

HostGcCycleStats hostGcCycleStats() {
    HostGcCycleStats st;
    st.finished = s_cyclesFinished;
    st.lastFrames = s_lastCycleFrames;
    st.open = s_cycle.open;
    return st;
}

uint32_t hostCollectBetweenFrames() {
    if (!isWebHostGlobalsInstalled()) return 0;
    if (isExecutionStackActive()) return 0;
    FrameGc f;
    if (!s_bootCollected) requestFor("boot", /*idleSpell=*/false);
    // A busy page under memory pressure is collected by a cycle like any
    // other (its native memory comes back at the finish, a few frames on)
    // rather than in one pause in the middle of what keeps it busy.
    if (s_requested && incrementalEnabled() && std::strcmp(s_requested, "pressure (busy)") == 0) {
        s_requested = nullptr;
        s_pressurePending = false;
        s_timeSinceLastGcMs = 0.0;
        const bronze::embed::HeapPressure p = bronze::embed::heapPressure();
        if (!p.marking) {
            const auto t0 = std::chrono::steady_clock::now();
            if (bronze::embed::startGcCycle()) {
                f.startMs = msSince(t0);
                f.work |= kGcWorkStart;
                openCycle(f.startMs);
                f.note = "memory pressure";
            }
        }
        f.log();
        return f.work;
    }
    if (s_requested) {
        const bool wasIdle = s_collectedForCurrentIdle;
        const bool idleSpell = s_requestedIsIdleSpell;
        const auto t0 = std::chrono::steady_clock::now();
        collectFor(s_requested);
        f.fullMs = msSince(t0);
        f.work |= kGcWorkFull;
        // One made for the boot or a busy page's pressure is not the idle
        // spell's: the page still gets that when it settles.
        if (!idleSpell) s_collectedForCurrentIdle = wasIdle;
        f.log();
        return f.work;
    }
    const bronze::embed::HeapPressure p = bronze::embed::heapPressure();
    if (incrementalEnabled()) {
        cycleStep(p, f);
        f.log();
        return f.work;
    }
    if (p.fullThreshold > 0 &&
        static_cast<double>(p.oldGrowth) >= kBetweenFramesOldFill * static_cast<double>(p.fullThreshold)) {
        const auto t0 = std::chrono::steady_clock::now();
        collectFor("old generation near its threshold");
        f.fullMs = msSince(t0);
        f.work |= kGcWorkFull;
        f.log();
        return f.work;
    }
    minorIfDue(p, f);
    f.log();
    return f.work;
}

}  // namespace bro::bronze_host
