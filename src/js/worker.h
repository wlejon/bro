#pragma once

#include "js/message_queue.h"
#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <vector>

extern "C" {
#include "quickjs.h"
}

namespace bro::net { class NetService; }

namespace bro::js {

class Runtime;
class Timers;

/// A Web Worker running JavaScript on a dedicated thread.
/// Each worker gets its own JSRuntime + JSContext (separate heap, GC, job queue).
/// Workers have access to brokit APIs only — no DOM, no canvas, no window.
/// Communication is via postMessage/onmessage with structured clone semantics.
class Worker {
public:
    /// Create a worker. scriptPath is resolved relative to basePath.
    /// If netService is non-null, the worker installs bro.net bindings
    /// against it (own subscriber, polled on the worker's event loop).
    Worker(const std::string& scriptPath, const std::string& basePath,
           net::NetService* netService);
    ~Worker();

    // Non-copyable, non-movable
    Worker(const Worker&) = delete;
    Worker& operator=(const Worker&) = delete;

    /// Start the worker thread. Must be called after construction.
    void start();

    /// Post a message from main thread → worker.
    /// Serializes value using structured clone. transferList is a JS Array
    /// of ArrayBuffers (or JS_UNDEFINED for no transfers).
    /// Returns true on success, false on error (JS exception thrown).
    bool postMessage(JSContext* mainCtx, JSValue value, JSValue transferList);

    /// Drain worker → main messages. For each message, calls the JS
    /// onmessage callback on the main-thread Worker object.
    /// Called by the Engine once per frame on the main thread.
    void drainMessages(JSContext* mainCtx);

    /// Terminate the worker thread (non-blocking signal + join).
    void terminate();

    /// Check if the worker thread is still alive.
    bool isAlive() const { return alive_.load(std::memory_order_acquire); }

    /// Tick rate in Hz — drives the worker's idle sleep and rate-limits
    /// the loop iteration. Exposed through bro.server.tickrate inside the
    /// worker script. Safe to read/write across threads.
    void setTickRate(double hz);
    double tickRate() const { return tickRate_.load(std::memory_order_relaxed); }

    /// Seconds since the worker thread started executing its script.
    /// Returns 0 before the thread reaches its main loop.
    double uptimeSec() const;

    /// The JS Worker object on the main thread (DupValue'd).
    /// Set by the bindings after construction.
    JSValue jsObject = JS_UNDEFINED;

    /// Push a message onto the worker→main queue (called from worker thread).
    bool pushToMain(Message* msg) { return fromWorker_.push(msg); }

    /// Request the worker to stop (called from worker thread via self.close()).
    void requestClose() { terminated_.store(true, std::memory_order_release); }

private:
    void threadFunc();

    std::string scriptPath_;
    std::string basePath_;
    net::NetService* netService_ = nullptr;
    std::thread thread_;

    // Lock-free message queues (SPSC ring buffers)
    MessageQueue toWorker_;    // main → worker
    MessageQueue fromWorker_;  // worker → main

    // Wakeup signal: main thread increments + notifies to wake the worker.
    std::atomic<uint32_t> wakeup_{0};
    std::atomic<bool> terminated_{false};
    std::atomic<bool> alive_{false};

    // Event loop rate limit. Default 1000 Hz matches the legacy 1ms idle
    // sleep, so existing compute-heavy workers (no tickrate set) are not
    // slowed down.
    std::atomic<double> tickRate_{1000.0};
    // Worker-thread-only: set at the top of threadFunc, read by uptimeSec().
    std::atomic<double> startTimeMs_{0.0};
};

// ---------------------------------------------------------------------------
// JS bindings
// ---------------------------------------------------------------------------

/// Install the Worker constructor on the global object.
/// If netService is non-null, workers created from this context will have
/// bro.net bindings installed on their JS context.
/// Must be called after brokit APIs are installed.
void installWorkerBindings(JSContext* ctx, const std::string& appBasePath,
                           net::NetService* netService);

/// Clean up worker bindings state for a context (call before freeing context).
void cleanupWorkerBindings(JSContext* ctx);

/// Drain messages from all workers on this context. Called once per frame.
void tickWorkers(JSContext* ctx);

}  // namespace bro::js
