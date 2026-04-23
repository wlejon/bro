#include "video/media_clock.h"

#include <chrono>

namespace bro::video {

namespace {
int64_t hostNowNs() {
    using namespace std::chrono;
    return duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count();
}

double loadRate(const std::atomic<int64_t>& r) {
    return r.load(std::memory_order_acquire) / 1'000'000.0;
}
} // namespace

TimeNs FileClock::nowNs() const {
    if (playing_.load(std::memory_order_acquire)) {
        const double r = loadRate(rateMicros_);
        const int64_t host = hostNowNs() - anchorHostNs_.load(std::memory_order_acquire);
        return static_cast<int64_t>(host * r);
    }
    return anchorPausedNs_.load(std::memory_order_acquire);
}

void FileClock::setPlaying(bool playing) {
    if (playing == playing_.load(std::memory_order_acquire)) return;
    if (playing) {
        const double r = loadRate(rateMicros_);
        const int64_t pausedAt = anchorPausedNs_.load(std::memory_order_acquire);
        // hostOffset such that (hostNow - hostOffset) * r == pausedAt
        const int64_t hostOffset = hostNowNs() - static_cast<int64_t>(pausedAt / r);
        anchorHostNs_.store(hostOffset, std::memory_order_release);
        playing_.store(true, std::memory_order_release);
    } else {
        const int64_t here = nowNs();
        anchorPausedNs_.store(here, std::memory_order_release);
        playing_.store(false, std::memory_order_release);
    }
}

void FileClock::seekTo(TimeNs pts) {
    if (playing_.load(std::memory_order_acquire)) {
        const double r = loadRate(rateMicros_);
        anchorHostNs_.store(hostNowNs() - static_cast<int64_t>(pts / r),
                            std::memory_order_release);
    } else {
        anchorPausedNs_.store(pts, std::memory_order_release);
    }
}

void FileClock::setRate(double rate) {
    if (rate <= 0.0) rate = 1.0;
    const int64_t newMicros = static_cast<int64_t>(rate * 1'000'000.0);
    if (newMicros == rateMicros_.load(std::memory_order_acquire)) return;
    // Re-anchor so the current stream position stays continuous across the
    // rate change. When paused the position is already latched in
    // anchorPausedNs_; only the playing branch needs host-anchor adjustment.
    if (playing_.load(std::memory_order_acquire)) {
        const int64_t here = nowNs(); // computed with old rate
        rateMicros_.store(newMicros, std::memory_order_release);
        anchorHostNs_.store(hostNowNs() - static_cast<int64_t>(here / rate),
                            std::memory_order_release);
    } else {
        rateMicros_.store(newMicros, std::memory_order_release);
    }
}

double FileClock::rate() const {
    return loadRate(rateMicros_);
}

} // namespace bro::video
