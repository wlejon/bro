#pragma once

// unhandledrejection / rejectionhandled (host_rejection_events.cpp): the
// calling thread's bronze rejection hook, and where its events go.

#include "embed/embed.h"

#include <functional>

namespace bro::bronze_host {

struct RejectionSink {
    // The log-line tag for an uncancelled report.
    const char* what = "unhandledrejection";
    // Arrange for flushRejectionEvents() to run as a later task.
    std::function<void()> queueFlush;
    // The global the events fire at (its on<type> attribute is read off it).
    std::function<bronze::Value()> global;
    // Hand the event to the global's listeners.
    std::function<void(bronze::Value evt)> dispatch;
    // An unhandledrejection nothing cancelled was just reported.
    std::function<void()> onReported;
};

// Install / remove the hook for THIS thread. Install replaces the sink.
void installRejectionTracking(RejectionSink sink);
void uninstallRejectionTracking();

// The notify task: fire what the hook queued since the last flush.
void flushRejectionEvents();
bool rejectionEventsPending();

// The main window's sink: host tasks, window.onunhandledrejection, the
// window's listeners, and a failed headless run for an uncancelled report.
void installMainThreadRejectionTracking();

}  // namespace bro::bronze_host
