#include "audio_inference/audio_inference.h"

#include <algorithm>
#include <cstdio>
#include <exception>
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
    if (!thread_.joinable()) drainCommands();
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
    for (;;) {
        // Block while idle; a signalPump()/shutdown() store + notify wakes us.
        // If a store already moved state_ off kIdle, wait() returns at once —
        // so there is no lost-wakeup window between the CAS below and here.
        state_.wait(kIdle, std::memory_order_acquire);

        const std::uint32_t s = state_.load(std::memory_order_acquire);
        if (s == kShutdown) break;
        if (s != kPump) continue;   // spurious

        state_.store(kBusy, std::memory_order_release);
        drainCommands();
        pumpTasks();

        // Return to idle unless a new pump (or shutdown) arrived while we were
        // busy. If signalPump() overwrote kBusy with kPump, the CAS fails and we
        // loop to service it; wait() at the top will not block (state != kIdle).
        std::uint32_t expected = kBusy;
        if (!state_.compare_exchange_strong(expected, kIdle,
                                            std::memory_order_acq_rel)) {
            if (expected == kShutdown) break;
            // expected == kPump: fall through and loop again.
        }
    }

    // Shutting down: apply any final commands, then destroy the tasks HERE so
    // each model's destructor (and its CUDA frees) runs on the worker thread.
    drainCommands();
    tasks_.clear();
}

void AudioInference::signalPump() {
    if (!thread_.joinable()) return;   // headless / not threaded: stepInline() drives
    state_.store(kPump, std::memory_order_release);
    state_.notify_one();
}

void AudioInference::stepInline() {
    if (thread_.joinable()) return;    // threaded: the worker drives
    drainCommands();
    pumpTasks();
}

void AudioInference::shutdown() {
    if (thread_.joinable()) {
        state_.store(kShutdown, std::memory_order_release);
        state_.notify_one();
        thread_.join();
        return;
    }
    // Never threaded (headless): destroy tasks on the calling thread. tickWake's
    // cleanup path has already removed the tap, so no producer is writing.
    drainCommands();
    tasks_.clear();
}

}  // namespace bro::engine
