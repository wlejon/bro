#pragma once

#include <cstdint>

namespace bro::bronze_host {

/// Trigger an immediate full collection of the Bronze JavaScript heap.
/// Safe to call only when no Bronze JavaScript execution is currently on the stack.
void hostCollectGarbage();

/// Notify the Bronze host layer of an advancing frame.
/// When the host environment is quiescent (no pending microtasks, no pending rAF callbacks)
/// and sufficient idle time has elapsed (e.g. 1 second of quiescence or every 10 seconds of
/// sustained idle), schedules/triggers a garbage collection cycle.
void hostNotifyIdleFrame(double dtMs);

/// Reset idle GC timers (e.g. across app reload or major scene teardown).
void hostResetIdleGCTimer();

}  // namespace bro::bronze_host
