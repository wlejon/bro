#include "audio_inference/audio_inference.h"

#include "util/log.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <exception>
#include <thread>
#include <utility>

namespace bro::engine {

// ─── PcmRing ─────────────────────────────────────────────────────────────────

void PcmRing::write(const float* s, int n) {
    if (n <= 0) return;
    const std::size_t cap = buf.size();
    if (cap == 0) return;
    const uint64_t w = writePos.load(std::memory_order_relaxed);
    const uint64_t r = readPos.load(std::memory_order_acquire);
    const std::size_t freeSpace = cap - static_cast<std::size_t>(w - r);
    const int toWrite = static_cast<int>(
        std::min(static_cast<std::size_t>(n), freeSpace));
    for (int i = 0; i < toWrite; ++i) {
        buf[(w + static_cast<uint64_t>(i)) % cap] = s[i];
    }
    writePos.store(w + static_cast<uint64_t>(toWrite), std::memory_order_release);
}

int PcmRing::read(std::vector<float>& out) {
    const std::size_t cap = buf.size();
    if (cap == 0) { out.clear(); return 0; }
    const uint64_t w = writePos.load(std::memory_order_acquire);
    const uint64_t r = readPos.load(std::memory_order_relaxed);
    std::size_t avail = static_cast<std::size_t>(w - r);
    if (avail > cap) avail = cap;   // defensive; should not happen
    out.resize(avail);
    for (std::size_t i = 0; i < avail; ++i) {
        out[i] = buf[(r + i) % cap];
    }
    readPos.store(r + static_cast<uint64_t>(avail), std::memory_order_release);
    return static_cast<int>(avail);
}

// ─── AudioInference ──────────────────────────────────────────────────────────

AudioInference::AudioInference() = default;

AudioInference::~AudioInference() {
    shutdown();
    // Commands queued but never drained (e.g. pushed after the worker exited)
    // are destroyed with cmdQueue_ here, on the main thread.
}

void AudioInference::startThread() {
    if (thread_.joinable()) return;
    thread_ = std::thread([this]() { workerFunc(); });
}

AudioInference::TaskId AudioInference::addTask(std::shared_ptr<PcmRing> ring,
                                               ProcessFn process) {
    const TaskId id = nextId_.fetch_add(1, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lk(m_);
        cmdQueue_.push_back(Command{Command::Add, id, std::move(ring), std::move(process)});
        ++cmdsPushed_;
    }
    cv_.notify_all();
    // Not threaded (headless): apply immediately on the calling thread so the
    // task list is current for the next stepInline and stale models are not
    // pinned in the command queue across stop()/listen() cycles.
    if (!thread_.joinable()) drainCommands();
    return id;
}

void AudioInference::removeTask(TaskId id) {
    if (id == kInvalidTask) return;
    std::uint64_t target = 0;
    {
        std::lock_guard<std::mutex> lk(m_);
        cmdQueue_.push_back(Command{Command::Remove, id, nullptr, ProcessFn{}});
        target = ++cmdsPushed_;
    }
    cv_.notify_all();
    if (!thread_.joinable()) { drainCommands(); return; }

    // Threaded: BLOCK until the worker has applied this command. removeTask is a
    // barrier, not a fire-and-forget: callers detach a tenant precisely so they
    // can then mutate (or destroy) that tenant's model state from the main
    // thread — enroll/remove/clear on a PhonemeSpotter, etc. If we returned
    // while the worker was still inside the pump that runs the tenant's closure,
    // the main thread would mutate the model concurrently with feed() (the
    // worker iterating PhonemeSpotter::matchers, say) — a data race and heap
    // corruption. The shared_ptr in the closure keeps the model ALIVE across the
    // pump, but does nothing to serialize access to its mutable state; that is
    // what this barrier provides, upholding the single-producer discipline the
    // bindings assume.
    //
    // Commands apply in FIFO order at the top of each worker iteration, before
    // pumpTasks(). So once cmdsApplied_ reaches our push count, the task has
    // been erased AND any pump that was still running the removed task's
    // closure — sequenced before that drain — has completed. The shutdown_
    // check avoids waiting forever should the worker have exited; removeTask
    // and shutdown() are both main-thread, so they never race.
    std::unique_lock<std::mutex> lk(m_);
    cv_.wait(lk, [this, target] {
        return cmdsApplied_ >= target || shutdown_;
    });
}

void AudioInference::drainCommands() {
    // Take the batch under the lock; apply it outside — a Remove drops the
    // task's model reference, and that destructor (CUDA frees included) must
    // not run under m_.
    std::deque<Command> batch;
    {
        std::lock_guard<std::mutex> lk(m_);
        if (cmdQueue_.empty()) return;
        batch.swap(cmdQueue_);
    }
    for (auto& c : batch) {
        if (c.type == Command::Add) {
            tasks_.push_back(Task{c.id, std::move(c.ring), std::move(c.process)});
        } else {
            tasks_.erase(
                std::remove_if(tasks_.begin(), tasks_.end(),
                               [&](const Task& tk) { return tk.id == c.id; }),
                tasks_.end());
        }
    }
    {
        std::lock_guard<std::mutex> lk(m_);
        cmdsApplied_ += batch.size();
    }
    cv_.notify_all();   // wake removeTask barrier waiters
}

void AudioInference::pumpTasks() {
    for (auto& task : tasks_) {
        if (!task.ring) continue;
        const int n = task.ring->read(scratch_);
        if (n <= 0) continue;
        try {
            task.process(scratch_.data(), n);
        } catch (const std::exception& e) {
            LOG_ERROR("[audioinfer] task process: %s", e.what());
        } catch (...) {
            LOG_ERROR("[audioinfer] task process: unknown");
        }
    }
}

void AudioInference::workerFunc() {
    // Self-paced: we drain the rings and run inference on our OWN clock, fully
    // independent of the render frame loop. The audio thread only ever writes the
    // lock-free PcmRing (no signal, no syscall — it stays real-time pure); the
    // main thread only delivers results (tickWake). Nothing throttles us when
    // rendering hiccups, and we never burden a frame.
    //
    // A short fixed drain interval keeps detection latency low. Combined with the
    // ~2 s ring, no samples are lost even if the frame loop stalls for the better
    // part of a second — the requirement that wake reliability not be a function
    // of frame rate. The exact OS sleep granularity is immaterial: any sub-second
    // cadence drains well within the ring span, and an empty ring read is a no-op
    // (pumpTasks early-outs), so idle cost is negligible.
    using namespace std::chrono;
    constexpr auto kDrainInterval = milliseconds(5);
    while (true) {
        drainCommands();
        pumpTasks();
        std::unique_lock<std::mutex> lk(m_);
        if (shutdown_) break;
        // Timed wait, not a sleep: audio arrival still can't signal us (the
        // RT thread never takes a lock), but a pushed command or shutdown
        // wakes us immediately instead of waiting out the interval.
        cv_.wait_for(lk, kDrainInterval, [this] {
            return shutdown_ || !cmdQueue_.empty();
        });
        if (shutdown_) break;
    }

    // Shutting down: apply any final commands, then destroy the tasks HERE so
    // each model's destructor (and its CUDA frees) runs on the worker thread.
    drainCommands();
    tasks_.clear();
}

void AudioInference::stepInline() {
    if (thread_.joinable()) return;    // threaded: the worker drives
    drainCommands();
    pumpTasks();
}

void AudioInference::shutdown() {
    if (thread_.joinable()) {
        {
            std::lock_guard<std::mutex> lk(m_);
            shutdown_ = true;
        }
        cv_.notify_all();
        thread_.join();
        return;
    }
    // Never threaded (headless): destroy tasks on the calling thread. tickWake's
    // cleanup path has already removed the tap, so no producer is writing.
    drainCommands();
    tasks_.clear();
}

}  // namespace bro::engine
