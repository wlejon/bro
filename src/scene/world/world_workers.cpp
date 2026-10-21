#include "scene/world/world_workers.h"

#include <algorithm>

namespace bro::scene {

WorldWorkers& WorldWorkers::shared() {
    static WorldWorkers pool(std::clamp(static_cast<int>(std::thread::hardware_concurrency()) / 2, 1, 8));
    return pool;
}

WorldWorkers::WorldWorkers(int threads) {
    for (int i = 0; i < std::max(threads, 1); ++i) threads_.emplace_back([this] { run(); });
}

WorldWorkers::~WorldWorkers() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_ = true;
        jobs_.clear();
    }
    wake_.notify_all();
    for (std::thread& t : threads_) t.join();
}

void WorldWorkers::post(std::function<void()> job) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        jobs_.push_back(std::move(job));
    }
    wake_.notify_one();
}

void WorldWorkers::drain() {
    std::unique_lock<std::mutex> lock(mutex_);
    idle_.wait(lock, [this] { return jobs_.empty() && busy_ == 0; });
}

void WorldWorkers::run() {
    std::unique_lock<std::mutex> lock(mutex_);
    for (;;) {
        wake_.wait(lock, [this] { return stop_ || !jobs_.empty(); });
        if (stop_) return;
        std::function<void()> job = std::move(jobs_.front());
        jobs_.pop_front();
        ++busy_;
        lock.unlock();
        job();
        lock.lock();
        --busy_;
        if (jobs_.empty() && busy_ == 0) idle_.notify_all();
    }
}

}  // namespace bro::scene
