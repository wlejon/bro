#pragma once

#include <cstdint>

namespace bro::bronze_host {

/// Trigger an immediate full collection of the Bronze JavaScript heap.
/// Safe to call only when no Bronze JavaScript execution is currently on the stack.
void hostCollectGarbage();

/// Notify the Bronze host layer of an advancing frame.
/// When the host environment is quiescent (no pending microtasks, no pending rAF callbacks,
/// and no active JavaScript eval/stack execution) and sufficient idle time has elapsed,
/// triggers a garbage collection cycle.
void hostNotifyIdleFrame(double dtMs);

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

}  // namespace bro::bronze_host
