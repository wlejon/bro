#pragma once

// WebGLWaitWord: how one side of the WebGL thread's handoffs sleeps until the
// other has made progress, with no syscall when nobody sleeps.
//
// A waiter waits for a condition on state the other side publishes (a ring
// position, a counter); the other side publishes the state first, then
// calls notify(). notify() is a fence and a load while nobody waits; it bumps
// the word and wakes (a futex on Linux, WaitOnAddress on Windows) only when
// someone does. The waiter spins a short while before it sleeps, since most
// waits are shorter than a sleep and a wake.
//
// Correctness (a Dekker pair): the waiter announces itself (waiters, then a
// seq_cst fence) before it reads the word and checks the condition; the
// notifier publishes its state, fences, then reads waiters. Either the
// waiter sees the state, or the notifier sees the waiter and bumps the word,
// which the waiter's sleep (comparing against the word it read) then sees.

#include <atomic>
#include <chrono>
#include <cstdint>

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#include <immintrin.h>
#elif defined(_M_ARM64)
#include <intrin.h>
#endif

namespace bro::webgl {

inline void cpuRelax() {
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
    _mm_pause();
#elif defined(_M_ARM64)
    __yield();
#elif defined(__aarch64__)
    asm volatile("yield");
#endif
}

class WebGLWaitWord {
public:
    /// The other side's state changed: wake whoever waits on it.
    void notify() {
        std::atomic_thread_fence(std::memory_order_seq_cst);
        if (waiters_.load(std::memory_order_relaxed) == 0) return;
        word_.fetch_add(1, std::memory_order_seq_cst);
        word_.notify_all();
    }

    /// Return once ready() is true: spinning up to `spin`, then sleeping
    /// until a notify() (ready() is checked again after every wake).
    template <class Ready>
    void wait(Ready&& ready, std::chrono::microseconds spin) {
        if (ready()) return;
        const auto until = std::chrono::steady_clock::now() + spin;
        for (uint32_t i = 1;; ++i) {
            if (ready()) return;
            cpuRelax();
            if ((i & 63) == 0 && std::chrono::steady_clock::now() >= until) break;
        }
        for (;;) {
            waiters_.fetch_add(1, std::memory_order_seq_cst);
            std::atomic_thread_fence(std::memory_order_seq_cst);
            const uint32_t seen = word_.load(std::memory_order_acquire);
            if (ready()) {
                waiters_.fetch_sub(1, std::memory_order_relaxed);
                return;
            }
            word_.wait(seen, std::memory_order_acquire);
            waiters_.fetch_sub(1, std::memory_order_relaxed);
            if (ready()) return;
        }
    }

    /// Sleep once until a notify() unless ready() (no spin): the WebGL
    /// thread's idle wait, which checks its own work after it returns.
    template <class Ready>
    void sleepUnless(Ready&& ready) {
        waiters_.fetch_add(1, std::memory_order_seq_cst);
        std::atomic_thread_fence(std::memory_order_seq_cst);
        const uint32_t seen = word_.load(std::memory_order_acquire);
        if (!ready()) word_.wait(seen, std::memory_order_acquire);
        waiters_.fetch_sub(1, std::memory_order_relaxed);
    }

    /// How many sleeps notify() has had to end (a statistic).
    uint32_t wakes() const { return word_.load(std::memory_order_relaxed); }

private:
    alignas(64) std::atomic<uint32_t> word_{0};
    std::atomic<uint32_t> waiters_{0};
};

}  // namespace bro::webgl
