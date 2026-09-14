#include "bronze_host/host_gc.h"

#include "bronze_host/bronze_host.h"
#include "bronze_host/host_globals_internal.h"

#include "embed/embed.h"
#include "abi/bronze_abi_tls.h"

#include <atomic>

namespace bro::bronze_host {
namespace {

constexpr double kIdleQuiescentThresholdMs = 1000.0;  // 1 second quiescent before idle GC
constexpr double kIdlePeriodicIntervalMs = 10000.0;    // 10 seconds of idle between periodic GCs

double s_idleAccumulatorMs = 0.0;
double s_timeSinceLastGcMs = 0.0;
bool s_collectedForCurrentIdle = false;
std::atomic<int> s_evalDepth{0};

bool isExecutionStackActive() {
    if (s_evalDepth.load(std::memory_order_relaxed) > 0) {
        return true;
    }
    auto* tls = bronze_tls_block_addr();
    if (tls && tls->frame_top != nullptr) {
        return true;
    }
    return false;
}

}  // namespace

void hostEnterEval() {
    s_evalDepth.fetch_add(1, std::memory_order_relaxed);
}

void hostLeaveEval() {
    s_evalDepth.fetch_sub(1, std::memory_order_relaxed);
}

bool isHostEvaluating() {
    return s_evalDepth.load(std::memory_order_relaxed) > 0;
}

void hostCollectGarbage() {
    if (isExecutionStackActive()) {
        return;
    }
    bronze::embed::collectGarbage();
    s_timeSinceLastGcMs = 0.0;
    s_collectedForCurrentIdle = true;
}

void hostResetIdleGCTimer() {
    s_idleAccumulatorMs = 0.0;
    s_timeSinceLastGcMs = 0.0;
    s_collectedForCurrentIdle = false;
}

void hostNotifyIdleFrame(double dtMs) {
    if (!isWebHostGlobalsInstalled()) return;
    if (isExecutionStackActive()) return;

    const double delta = (dtMs > 0.0) ? dtMs : 16.67;
    s_timeSinceLastGcMs += delta;

    const bool microtasks = bronze::embed::microtasksPending();
    const bool rafs = hasPendingAnimationFrames();
    const bool brokit = brokitHasPendingWork();
    const bool isQuiescent = (!microtasks && !rafs && !brokit);

    if (isQuiescent) {
        s_idleAccumulatorMs += delta;
        if ((!s_collectedForCurrentIdle && s_idleAccumulatorMs >= kIdleQuiescentThresholdMs) ||
            (s_timeSinceLastGcMs >= kIdlePeriodicIntervalMs)) {
            hostCollectGarbage();
        }
    } else {
        s_idleAccumulatorMs = 0.0;
        s_collectedForCurrentIdle = false;
    }
}

}  // namespace bro::bronze_host
