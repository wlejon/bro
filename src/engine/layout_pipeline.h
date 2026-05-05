#pragma once

#include <atomic>
#include <cstdint>

namespace bro::dom { class Element; }

namespace bro::engine {

/// Coordinates the main thread with the layout worker. Same shape as
/// FramePresenter (signal a snapshot, wait for a result), but with no GL
/// fence — layout is pure CPU work that mutates DOM-side layout boxes.
///
///     Idle ──signalLayout()──▶ Requested
///     Requested ──waitForRequest()──▶ (worker proceeds) ──markBusy()──▶ Busy
///     Busy ──publishDone()──▶ Done
///     Done ──claimDone()──▶ Idle
class LayoutPipeline {
public:
    enum State : uint32_t {
        Idle      = 0,
        Requested = 1,
        Busy      = 2,
        Done      = 3,
        Shutdown  = 4,
    };

    struct Snapshot {
        int vpWidth = 0;
        int vpHeight = 0;
        int insetTop = 0;
        int insetRight = 0;
        int insetBottom = 0;
        bool animationsActive = false;
        dom::Element* hoveredElement = nullptr;
    };

    // ---- main thread ----

    void signalLayout(const Snapshot& s) {
        vpWidth_.store(s.vpWidth, std::memory_order_relaxed);
        vpHeight_.store(s.vpHeight, std::memory_order_relaxed);
        insetTop_.store(s.insetTop, std::memory_order_relaxed);
        insetRight_.store(s.insetRight, std::memory_order_relaxed);
        insetBottom_.store(s.insetBottom, std::memory_order_relaxed);
        animationsActive_.store(s.animationsActive, std::memory_order_relaxed);
        hoveredElement_.store(s.hoveredElement, std::memory_order_relaxed);
        uint32_t expected = Idle;
        if (state_.compare_exchange_strong(expected, Requested,
                                           std::memory_order_release,
                                           std::memory_order_relaxed)) {
            state_.notify_one();
        }
    }

    bool isIdle() const { return state_.load(std::memory_order_acquire) == Idle; }
    bool isDone() const { return state_.load(std::memory_order_acquire) == Done; }

    /// Block until the worker reaches Done (or already there), then transition
    /// to Idle. Returns true iff Done was actually claimed (false if state
    /// drifted to Shutdown).
    bool waitClaimDone() {
        for (;;) {
            uint32_t s = state_.load(std::memory_order_acquire);
            if (s == Done) {
                state_.store(Idle, std::memory_order_release);
                return true;
            }
            if (s == Idle || s == Shutdown) return false;
            // Requested or Busy — wait for any transition.
            state_.wait(s, std::memory_order_acquire);
        }
    }

    /// Same as waitClaimDone but only if the worker is currently mid-flight.
    /// Returns false if the worker was Idle to begin with (no-op).
    bool waitForIdle() {
        uint32_t s = state_.load(std::memory_order_acquire);
        if (s == Idle) return false;
        for (;;) {
            if (s == Idle || s == Shutdown) return true;
            if (s == Done) {
                state_.store(Idle, std::memory_order_release);
                return true;
            }
            state_.wait(s, std::memory_order_acquire);
            s = state_.load(std::memory_order_acquire);
        }
    }

    // ---- worker thread ----

    /// Block until either a request arrives or shutdown is signaled.
    /// Returns false on shutdown.
    bool waitForRequest() {
        for (;;) {
            state_.wait(Idle, std::memory_order_acquire);
            uint32_t s = state_.load(std::memory_order_acquire);
            if (s == Shutdown) return false;
            if (s == Requested) return true;
        }
    }

    void markBusy() { state_.store(Busy, std::memory_order_release); }

    /// Busy → Done, notify. If main has already shut down, store Shutdown
    /// (no notify needed — main isn't waiting).
    void publishDone() {
        uint32_t expected = Busy;
        if (state_.compare_exchange_strong(expected, Done,
                                           std::memory_order_acq_rel,
                                           std::memory_order_acquire)) {
            state_.notify_one();
        }
    }

    Snapshot loadSnapshot() const {
        Snapshot s;
        s.vpWidth = vpWidth_.load(std::memory_order_relaxed);
        s.vpHeight = vpHeight_.load(std::memory_order_relaxed);
        s.insetTop = insetTop_.load(std::memory_order_relaxed);
        s.insetRight = insetRight_.load(std::memory_order_relaxed);
        s.insetBottom = insetBottom_.load(std::memory_order_relaxed);
        s.animationsActive = animationsActive_.load(std::memory_order_relaxed);
        s.hoveredElement = hoveredElement_.load(std::memory_order_relaxed);
        return s;
    }

    /// Worker tells main about CSS animation state (drives "needs another
    /// layout pass" decision).
    void setAnimationsActive(bool active) {
        animationsActive_.store(active, std::memory_order_relaxed);
    }
    bool animationsActive() const {
        return animationsActive_.load(std::memory_order_relaxed);
    }

    // ---- lifecycle ----

    void postShutdown() {
        state_.store(Shutdown, std::memory_order_release);
        state_.notify_one();
    }

private:
    std::atomic<uint32_t> state_{Idle};
    std::atomic<int> vpWidth_{0};
    std::atomic<int> vpHeight_{0};
    std::atomic<int> insetTop_{0};
    std::atomic<int> insetRight_{0};
    std::atomic<int> insetBottom_{0};
    std::atomic<bool> animationsActive_{false};
    std::atomic<dom::Element*> hoveredElement_{nullptr};
};

} // namespace bro::engine
