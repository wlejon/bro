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

/// The process's private bytes (Windows PrivateUsage, Linux resident set); 0
/// where unknown. What the memory-pressure collection and perf.stats() read.
uint64_t hostProcessPrivateBytes();

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
