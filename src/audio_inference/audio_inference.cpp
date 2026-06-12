#include "audio_inference/audio_inference.h"

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
    // Free any commands that were queued but never drained (e.g. a removeTask
    // after the worker already exited). After join() nobody else touches these.
    std::size_t h = cmdHead_.load(std::memory_order_relaxed);
    const std::size_t t = cmdTail_.load(std::memory_order_relaxed);
    for (; h != t; ++h) {
        delete cmdSlots_[h & (kCmdCap - 1)];
        cmdSlots_[h & (kCmdCap - 1)] = nullptr;
    }
}

void AudioInference::startThread() {
    if (thread_.joinable()) return;
    thread_ = std::thread([this]() { workerFunc(); });
}

void AudioInference::pushCommand(Command* c) {
    const std::size_t t = cmdTail_.load(std::memory_order_relaxed);
    const std::size_t h = cmdHead_.load(std::memory_order_acquire);
    if (t - h >= kCmdCap) {
        // Ring full — only possible if dozens of add/remove queued without a
        // single pump. Drop rather than block; never expected in practice.
        std::fprintf(stderr, "[ERROR] [audioinfer] command queue full, dropping\n");
        delete c;
        return;
    }
    cmdSlots_[t & (kCmdCap - 1)] = c;
    cmdTail_.store(t + 1, std::memory_order_release);
}

AudioInference::TaskId AudioInference::addTask(std::shared_ptr<PcmRing> ring,
                                               ProcessFn process) {
    const TaskId id = nextId_.fetch_add(1, std::memory_order_relaxed);
    auto* c = new Command{Command::Add, id, std::move(ring), std::move(process)};
    pushCommand(c);
    // Not threaded (headless): apply immediately on the calling thread so the
    // task list is current for the next stepInline and stale models are not
    // pinned in the command queue across stop()/listen() cycles.
    if (!thread_.joinable()) drainCommands();
    return id;
}

void AudioInference::removeTask(TaskId id) {
    if (id == kInvalidTask) return;
    pushCommand(new Command{Command::Remove, id, nullptr, ProcessFn{}});
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
    // drainCommands() runs at the TOP of the worker loop, before each
    // pumpTasks(). So once cmdHead_ reaches the tail we just published, the
    // command has been applied (the task erased) AND the pump that may still
    // have been running the removed task's closure — sequenced before that
    // drain — has completed. (A pump that starts after the drain no longer sees
    // the task.) The state_ guard avoids spinning forever should the worker have
    // exited; removeTask and shutdown() are both main-thread, so they never race.
    // Short sleeps, not a yield spin: the worker only drains every
    // kDrainInterval (5 ms), so a tight yield loop would burn a core for the
    // whole wait. Removal latency is irrelevant (user-action cadence).
    const std::size_t target = cmdTail_.load(std::memory_order_relaxed);
    while (cmdHead_.load(std::memory_order_acquire) < target &&
           state_.load(std::memory_order_acquire) != kShutdown) {
        std::this_thread::sleep_for(std::chrono::microseconds(500));
    }
}

void AudioInference::drainCommands() {
    std::size_t h = cmdHead_.load(std::memory_order_relaxed);
    const std::size_t t = cmdTail_.load(std::memory_order_acquire);
    for (; h != t; ++h) {
        Command* c = cmdSlots_[h & (kCmdCap - 1)];
        cmdSlots_[h & (kCmdCap - 1)] = nullptr;
        if (c->type == Command::Add) {
            tasks_.push_back(Task{c->id, std::move(c->ring), std::move(c->process)});
        } else {
            tasks_.erase(
                std::remove_if(tasks_.begin(), tasks_.end(),
                               [&](const Task& tk) { return tk.id == c->id; }),
                tasks_.end());
        }
        delete c;
    }
    cmdHead_.store(h, std::memory_order_release);
}

void AudioInference::pumpTasks() {
    for (auto& task : tasks_) {
        if (!task.ring) continue;
        const int n = task.ring->read(scratch_);
        if (n <= 0) continue;
        try {
            task.process(scratch_.data(), n);
        } catch (const std::exception& e) {
            std::fprintf(stderr, "[ERROR] [audioinfer] task process: %s\n", e.what());
        } catch (...) {
            std::fprintf(stderr, "[ERROR] [audioinfer] task process: unknown\n");
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
    while (state_.load(std::memory_order_acquire) != kShutdown) {
        drainCommands();
        pumpTasks();
        std::this_thread::sleep_for(kDrainInterval);
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
        // The worker self-paces, so it isn't parked on the atomic — it observes
        // kShutdown on its next drain tick (within one interval) and exits.
        state_.store(kShutdown, std::memory_order_release);
        thread_.join();
        return;
    }
    // Never threaded (headless): destroy tasks on the calling thread. tickWake's
    // cleanup path has already removed the tap, so no producer is writing.
    drainCommands();
    tasks_.clear();
}

}  // namespace bro::engine
