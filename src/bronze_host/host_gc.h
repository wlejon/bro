#pragma once

#include <cstdint>

namespace bro::bronze_host {

/// Trigger an immediate full collection of the Bronze JavaScript heap.
/// Safe to call only when no Bronze JavaScript execution is currently on the stack.
void hostCollectGarbage();

/// Notify the Bronze host layer of an advancing frame.
/// When the host environment is quiescent (no pending microtasks, no pending rAF callbacks,
/// and no active JavaScript eval/stack execution) and sufficient idle time has elapsed,
/// triggers a garbage collection cycle. A busy page is collected too once the
/// process has grown well past what it held after the last collection: native
/// memory owned by JS wrappers (bitmaps, detached DOM, scans) is invisible to
/// the JS heap's own triggers and is only given back by their finalizers.
void hostNotifyIdleFrame(double dtMs);

/// What hostCollectBetweenFrames did in a frame (bits; 0 nothing), for the
/// frame recorder.
enum : uint32_t {
    kGcWorkMinor = 1u << 0,   // a minor collection
    kGcWorkFull = 1u << 1,    // a full collection, stop-the-world
    kGcWorkStart = 1u << 2,   // a marking cycle started
    kGcWorkMark = 1u << 3,    // a mark slice
    kGcWorkFinish = 1u << 4,  // the cycle's finish
    kGcWorkSweep = 1u << 5,   // a sweep slice
};

/// Called by the engine once a frame is presented (or held), before the next
/// one starts: the one place the host collects during an animation. It runs
/// what hostNotifyIdleFrame decided on (idle, periodic, pressure), a full
/// collection once after the page has booted (promoting its boot data in one
/// copy), and otherwise collects ahead of the heap's own triggers: a minor
/// collection once eden is half full, and the old generation incrementally,
/// a marking cycle started once it has grown halfway to its threshold and
/// carried on in slices over the following frames, each frame's GC work kept
/// within a budget (BRO_GC_FRAME_MS, 0.5 ms), then finished and swept in
/// slices; BRO_GC_INCREMENTAL=0 instead collects it whole once it is near its
/// threshold. The pauses then fall in the time the frame left before the next
/// one rather than inside a frame. Returns the kGcWork bits of what it did.
uint32_t hostCollectBetweenFrames();

/// The marking cycles hostCollectBetweenFrames has run (perf.stats().bronze):
/// how many it finished, how many frames the last one took from its start to
/// its finish, and whether one is open now.
struct HostGcCycleStats {
    uint64_t finished = 0;
    uint32_t lastFrames = 0;
    bool open = false;
};
HostGcCycleStats hostGcCycleStats();

/// The process's private bytes (Windows PrivateUsage, Linux resident set); 0
/// where unknown. What the memory-pressure collection and perf.stats() read.
uint64_t hostProcessPrivateBytes();

/// BRO_GC_LOG=1: log each collection the host starts (idle, periodic,
/// pressure, the DOM sweep's), with why and how long it took.
bool hostGcLogEnabled();

/// Reset idle GC timers (e.g. across app reload or major scene teardown).
void hostResetIdleGCTimer();

/// Track active eval scopes so idle GC does not fire while test scripts or evals are executing.
void hostEnterEval();
void hostLeaveEval();
bool isHostEvaluating();

struct HostEvalScope {
    HostEvalScope() { hostEnterEval(); }
    ~HostEvalScope() { hostLeaveEval(); }
    HostEvalScope(const HostEvalScope&) = delete;
    HostEvalScope& operator=(const HostEvalScope&) = delete;
};

/// Lifts the eval scopes for its lifetime, for a native that runs window
/// frames from inside a script (headless runFrames): those frames collect as
/// a window's frames do. The script's own frames beneath are compiled JS,
/// stack-map roots the collector walks (the DOM sweep collects there too).
struct HostEvalSuspend {
    HostEvalSuspend();
    ~HostEvalSuspend();
    HostEvalSuspend(const HostEvalSuspend&) = delete;
    HostEvalSuspend& operator=(const HostEvalSuspend&) = delete;
private:
    int depth_ = 0;
};

}  // namespace bro::bronze_host
