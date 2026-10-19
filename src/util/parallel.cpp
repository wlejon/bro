#include "util/parallel.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
#include <intrin.h>
#define BRO_PAUSE() _mm_pause()
#elif defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
#define BRO_PAUSE() _mm_pause()
#elif defined(__aarch64__)
#define BRO_PAUSE() asm volatile("yield")
#else
#define BRO_PAUSE() ((void)0)
#endif

namespace bro::util {
namespace {

thread_local bool tInJob = false;

struct Job {
    const std::function<void(uint32_t)>* fn = nullptr;
    uint32_t chunks = 0;
    std::atomic<uint32_t> next{0};
    std::atomic<uint32_t> active{0};   // workers inside it

    void drain() {
        for (uint32_t c; (c = next.fetch_add(1, std::memory_order_relaxed)) < chunks;) (*fn)(c);
    }
};

// A frame's jobs come microseconds apart: a worker done with one spins this
// long for the next before it sleeps (a sleeping worker's wake is tens of
// microseconds, a whole small job's length).
constexpr auto kSpin = std::chrono::microseconds(200);

// Never destroyed: its workers are detached and wait out the process (a
// join from a static destructor deadlocks under the loader's lock when the
// engine is a DLL).
struct Pool {
    std::mutex m;
    std::condition_variable wake;
    Job* job = nullptr;   // under m
    std::atomic<uint64_t> generation{0};
    std::mutex running;   // one job at a time
    uint32_t workers = 0;

    explicit Pool(uint32_t n) : workers(n) {
        for (uint32_t i = 0; i < n; ++i) std::thread([this] { loop(); }).detach();
    }

    void loop() {
        tInJob = true;
        uint64_t seen = 0;
        for (;;) {
            const auto until = std::chrono::steady_clock::now() + kSpin;
            for (int i = 0; generation.load(std::memory_order_acquire) == seen; ++i) {
                BRO_PAUSE();
                if ((i & 63) == 63 && std::chrono::steady_clock::now() > until) break;
            }
            Job* j;
            {
                std::unique_lock lk(m);
                wake.wait(lk, [&] { return generation.load(std::memory_order_relaxed) != seen; });
                seen = generation.load(std::memory_order_relaxed);
                j = job;
                if (!j) continue;
                j->active.fetch_add(1, std::memory_order_relaxed);
            }
            j->drain();
            j->active.fetch_sub(1, std::memory_order_release);
        }
    }
};

Pool& pool() {
    // Half the hardware threads (other processes share the machine), at
    // most 16 workers.
    static Pool* p = new Pool(std::clamp(std::thread::hardware_concurrency() / 2, 1u, 16u));
    return *p;
}

}  // namespace

uint32_t parallelWidth() { return pool().workers + 1; }

void parallelChunks(uint32_t chunks, const std::function<void(uint32_t)>& fn) {
    if (chunks == 0) return;
    Pool& p = pool();
    std::unique_lock<std::mutex> run(p.running, std::defer_lock);
    if (chunks == 1 || tInJob || !run.try_lock()) {
        for (uint32_t c = 0; c < chunks; ++c) fn(c);
        return;
    }
    Job j;
    j.fn = &fn;
    j.chunks = chunks;
    {
        std::lock_guard lk(p.m);
        p.job = &j;
        p.generation.fetch_add(1, std::memory_order_release);
    }
    p.wake.notify_all();
    tInJob = true;
    j.drain();
    tInJob = false;
    {
        // A worker joins only under the lock, so once the job is withdrawn
        // the ones inside are all counted.
        std::lock_guard lk(p.m);
        p.job = nullptr;
    }
    // The last chunks finish within microseconds of the caller's own: it
    // spins for them (its wait counted as the frame's CPU, as it is).
    while (j.active.load(std::memory_order_acquire) != 0) BRO_PAUSE();
}

}  // namespace bro::util
