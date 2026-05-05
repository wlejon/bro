#pragma once

#include <atomic>
#include <cstdint>

#include <glad/gl.h>

namespace bro::render {

/// Tiny coordination primitive shared between the main thread and a single
/// worker thread that produces GPU work. The worker draws into a GL surface
/// owned by its own context, places a fence, and hands off to the main thread
/// which waits on the fence before sampling the resulting texture.
///
/// The state machine is intentionally tiny:
///
///     Idle ──postRequest()──▶ Requested
///     Requested ──worker waitForRequest()──▶ (worker proceeds) ──markBusy()──▶ Busy
///     Busy ──publishResult(fence)──▶ ResultReady
///     ResultReady ──main tryClaimResult()──▶ Idle
///
/// Either side may issue postShutdown() at any time; the worker's
/// waitForRequest() returns false in that case, and publishResult() drops
/// the fence on the floor instead of publishing.
///
/// The class wraps the GL fence handshake in one place. Owners only call
/// postRequest / tryClaimResult; they never glWaitSync directly. This makes
/// the "produce fence, wait on fence, delete fence" lifetime impossible to
/// get wrong from the call site.
class FrameWorker {
public:
    enum State : uint32_t {
        Idle        = 0,
        Requested   = 1,
        Busy        = 2,
        ResultReady = 3,
        Shutdown    = 4,
    };

    // ---- main thread ----

    /// Idle → Requested. No-op if not Idle (caller must check isIdle()).
    void postRequest() {
        uint32_t expected = Idle;
        if (state_.compare_exchange_strong(expected, Requested,
                                           std::memory_order_release,
                                           std::memory_order_relaxed)) {
            state_.notify_one();
        }
    }

    /// Any state → Shutdown. Safe to call once; subsequent calls are no-ops
    /// because waitForRequest treats Shutdown as terminal.
    void postShutdown() {
        state_.store(Shutdown, std::memory_order_release);
        state_.notify_one();
    }

    /// If the worker has published a result, glWaitSync + glDeleteSync the
    /// fence and transition back to Idle. Returns true iff a result was
    /// claimed.
    bool tryClaimResult() {
        if (state_.load(std::memory_order_acquire) != ResultReady) return false;
        auto fence = reinterpret_cast<GLsync>(
            fence_.exchange(0, std::memory_order_acquire));
        if (fence) {
            glWaitSync(fence, 0, GL_TIMEOUT_IGNORED);
            glDeleteSync(fence);
        }
        state_.store(Idle, std::memory_order_release);
        return true;
    }

    /// Block until the worker is Idle. Used at shutdown / before destructive
    /// operations (e.g. resizing pools) where we need a quiescent worker.
    /// Drops a published result on the way through.
    void waitUntilIdle() {
        for (;;) {
            uint32_t s = state_.load(std::memory_order_acquire);
            if (s == Idle) return;
            if (s == ResultReady) {
                tryClaimResult();
                return;
            }
            // Requested or Busy — wait for any transition.
            state_.wait(s, std::memory_order_acquire);
        }
    }

    bool isIdle() const {
        return state_.load(std::memory_order_acquire) == Idle;
    }

    bool isResultReady() const {
        return state_.load(std::memory_order_acquire) == ResultReady;
    }

    bool isBusyOrRequested() const {
        uint32_t s = state_.load(std::memory_order_acquire);
        return s == Requested || s == Busy;
    }

    State stateForDebug() const {
        return static_cast<State>(state_.load(std::memory_order_acquire));
    }

    // ---- worker thread ----

    /// Block until either a request arrives or shutdown is signaled.
    /// Returns false on shutdown (worker should exit its loop).
    bool waitForRequest() {
        for (;;) {
            state_.wait(Idle, std::memory_order_acquire);
            uint32_t s = state_.load(std::memory_order_acquire);
            if (s == Shutdown) return false;
            if (s == Requested) return true;
            // Spurious wake (shouldn't happen with futex but be safe).
        }
    }

    /// Requested → Busy. Worker calls this once it has captured the snapshot
    /// and is about to start drawing. Allows the main thread's isBusyOrRequested()
    /// check to differentiate "queued" from "actively running".
    void markBusy() {
        state_.store(Busy, std::memory_order_release);
    }

    /// Publish the fence + transition Busy → ResultReady. If the main thread
    /// already issued shutdown, we delete the fence instead so it doesn't
    /// leak.
    void publishResult(GLsync fence) {
        fence_.store(reinterpret_cast<uintptr_t>(fence), std::memory_order_release);
        uint32_t expected = Busy;
        if (state_.compare_exchange_strong(expected, ResultReady,
                                           std::memory_order_acq_rel,
                                           std::memory_order_acquire)) {
            state_.notify_one();
        } else {
            // Likely Shutdown — drop the fence to avoid GPU resource leak.
            if (fence) glDeleteSync(fence);
            fence_.store(0, std::memory_order_release);
        }
    }

private:
    std::atomic<uint32_t> state_{Idle};
    std::atomic<uintptr_t> fence_{0};  // GLsync handle as opaque uintptr
};

} // namespace bro::render
