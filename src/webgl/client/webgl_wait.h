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

#if defined(_MSC_VER)
#include <intrin.h>
#elif defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
#include <x86intrin.h>
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

/// A spin's deadline, read from a counter that costs a few cycles: the TSC
/// on x86, the virtual counter on arm64, steady_clock elsewhere. A spin
/// checks it every 16 pauses; steady_clock::now() there was a vDSO call each
/// time, about 0.7M cycles a frame of the WebGL thread's idle spin on
/// three_meshes.
class SpinDeadline {
public:
    explicit SpinDeadline(std::chrono::microseconds spin)
        : until_(now() + ticksPerUs() * static_cast<uint64_t>(spin.count())) {}
    bool passed() const { return now() >= until_; }

    static uint64_t now() {
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
        return __rdtsc();
#elif defined(_M_ARM64)
        return static_cast<uint64_t>(_ReadStatusReg(ARM64_CNTVCT));
#elif defined(__aarch64__)
        uint64_t v;
        asm volatile("mrs %0, cntvct_el0" : "=r"(v));
        return v;
#else
        return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
                .count());
#endif
    }

    /// Counter ticks per microsecond, measured once (the arm64 counter's
    /// frequency is a register; the TSC's is timed against steady_clock for
    /// 200 us, the first time any spin asks).
    static uint64_t ticksPerUs() {
        static const uint64_t perUs = [] {
#if defined(__aarch64__) && !defined(_MSC_VER)
            uint64_t hz;
            asm volatile("mrs %0, cntfrq_el0" : "=r"(hz));
            return hz >= 1000000 ? hz / 1000000 : uint64_t{1};
#elif defined(_M_ARM64)
            const uint64_t hz = static_cast<uint64_t>(_ReadStatusReg(ARM64_SYSREG(3, 3, 14, 0, 0)));  // CNTFRQ_EL0
            return hz >= 1000000 ? hz / 1000000 : uint64_t{1};
#elif defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
            using clock = std::chrono::steady_clock;
            const auto t0 = clock::now();
            const uint64_t c0 = now();
            auto t1 = t0;
            while (t1 - t0 < std::chrono::microseconds(200)) t1 = clock::now();
            const uint64_t c1 = now();
            const auto us = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count() / 1000.0;
            const uint64_t r = static_cast<uint64_t>(static_cast<double>(c1 - c0) / us);
            return r > 0 ? r : uint64_t{1};
#else
            return uint64_t{1000};  // nanoseconds
#endif
        }();
        return perUs;
    }

private:
    uint64_t until_;
};

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
        const SpinDeadline until(spin);
        for (uint32_t i = 1;; ++i) {
            if (ready()) return;
            cpuRelax();
            if ((i & 15) == 0 && until.passed()) break;
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
