#pragma once

// WebGLThread: the thread every WebGL context's backend lives on.
//
// One per engine (Engine makes it with its first context; a context made
// alone makes its own), shared by its contexts, which keep it alive. It runs
// one loop over the channels registered with it (WebGLChannel): replay what
// each client has published, publish what the replay produced, and, when no
// channel has anything, spin briefly and then sleep until a client
// publishes (WebGLWaitWord: a publish costs a fence and a load while the
// thread is awake, a futex wake only while it sleeps).
//
// Deadlock rules: the WebGL thread never waits for a JS thread and never
// takes the Skia lock; it only waits for the GPU (the backend's readbacks
// and finish) and for VulkanQueue's lock. A client waits for the WebGL
// thread only at its sync points (round trips, a publication not made yet,
// a full ring or blob budget, the context's teardown).

#include "webgl/client/webgl_wait.h"

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace bro::webgl {

class WebGLChannel;

class WebGLThread {
public:
    /// How long the thread spins for more work before it sleeps.
    static constexpr std::chrono::microseconds kIdleSpin{30};

    WebGLThread();
    /// Every channel must be gone (each context keeps its thread alive).
    ~WebGLThread();

    WebGLThread(const WebGLThread&) = delete;
    WebGLThread& operator=(const WebGLThread&) = delete;

    /// Start replaying `channel` (its records may already be published).
    void add(WebGLChannel& channel);
    /// Wait until the thread has replayed the channel's Destroy record and
    /// dropped the channel: it never touches the channel again.
    void awaitDestroyed(WebGLChannel& channel);

    /// A client published: wake the thread if it sleeps.
    void wake() { work_.notify(); }

    std::thread::id id() const { return thread_.get_id(); }

    /// How many times a publish had to wake the sleeping thread.
    uint32_t wakes() const { return work_.wakes(); }

private:
    void run();
    bool hasWork() const;

    WebGLWaitWord work_;
    alignas(64) std::atomic<bool> controlPending_{false};

    std::mutex controlMutex_;
    std::condition_variable destroyedCv_;
    std::vector<WebGLChannel*> adds_;  // under controlMutex_
    bool stop_ = false;                // under controlMutex_

    std::vector<WebGLChannel*> channels_;  // the thread's own
    std::thread thread_;                   // last: starts once the rest is built
};

}  // namespace bro::webgl
