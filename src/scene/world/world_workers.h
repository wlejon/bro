#pragma once
// The world's CPU workers: a fixed
// pool of threads the world's systems hand jobs to (a cell's plants placed
// from its fields now; field tiles and collision later), so nothing a frame
// does waits on them. A job owns what it reads (immutable rules, its cell's
// key) and leaves its result in its owner's inbox (a shared, locked list
// the owner drains when the frame next looks), so an owner gone before its
// jobs finish loses nothing but their results.
//
// Jobs run in the order posted. An owner that wants the nearest first posts
// them nearest first and keeps only a few in flight, so a turn of the eye
// reorders what is still its own queue, not the pool's.

#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace bro::scene {

class WorldWorkers {
public:
    /// The process's pool: about half the hardware's threads (1 to 8).
    static WorldWorkers& shared();

    explicit WorldWorkers(int threads);
    ~WorldWorkers();
    WorldWorkers(const WorldWorkers&) = delete;
    WorldWorkers& operator=(const WorldWorkers&) = delete;

    int threads() const { return static_cast<int>(threads_.size()); }
    void post(std::function<void()> job);
    /// Block until every job posted so far has run (tests; never a frame).
    void drain();

private:
    void run();

    std::mutex mutex_;
    std::condition_variable wake_, idle_;
    std::deque<std::function<void()>> jobs_;
    int busy_ = 0;
    bool stop_ = false;
    std::vector<std::thread> threads_;
};

}  // namespace bro::scene
