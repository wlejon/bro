#include "util/parallel.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace bro::util {
namespace {

thread_local bool tInJob = false;

struct Job {
    const std::function<void(uint32_t)>* fn = nullptr;
    uint32_t chunks = 0;
    std::atomic<uint32_t> next{0};
    uint32_t active = 0;   // workers inside it (under the pool's lock)

    void drain() {
        for (uint32_t c; (c = next.fetch_add(1, std::memory_order_relaxed)) < chunks;) (*fn)(c);
    }
};

// Never destroyed: its workers are detached and wait out the process (a
// join from a static destructor deadlocks under the loader's lock when the
// engine is a DLL).
struct Pool {
    std::mutex m;
    std::condition_variable wake, idle;
    Job* job = nullptr;
    uint64_t generation = 0;
    std::mutex running;   // one job at a time
    uint32_t workers = 0;

    explicit Pool(uint32_t n) : workers(n) {
        for (uint32_t i = 0; i < n; ++i) std::thread([this] { loop(); }).detach();
    }

    void loop() {
        tInJob = true;
        uint64_t seen = 0;
        std::unique_lock lk(m);
        for (;;) {
            wake.wait(lk, [&] { return generation != seen; });
            seen = generation;
            Job* j = job;
            if (!j) continue;
            j->active++;
            lk.unlock();
            j->drain();
            lk.lock();
            if (--j->active == 0) idle.notify_all();
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
        p.generation++;
    }
    p.wake.notify_all();
    tInJob = true;
    j.drain();
    tInJob = false;
    std::unique_lock lk(p.m);
    p.job = nullptr;
    p.idle.wait(lk, [&] { return j.active == 0; });
}

}  // namespace bro::util
