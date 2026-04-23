#include "video/media_clock.h"

#include <chrono>

namespace bro::video {

namespace {
int64_t hostNowNs() {
    using namespace std::chrono;
    return duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count();
}
} // namespace

TimeNs FileClock::nowNs() const {
    if (playing_.load(std::memory_order_acquire)) {
        return hostNowNs() - anchorHostNs_.load(std::memory_order_acquire);
    }
    return anchorPausedNs_.load(std::memory_order_acquire);
}

void FileClock::setPlaying(bool playing) {
    if (playing == playing_.load(std::memory_order_acquire)) return;
    if (playing) {
        const int64_t pausedAt = anchorPausedNs_.load(std::memory_order_acquire);
        anchorHostNs_.store(hostNowNs() - pausedAt, std::memory_order_release);
        playing_.store(true, std::memory_order_release);
    } else {
        const int64_t here = nowNs();
        anchorPausedNs_.store(here, std::memory_order_release);
        playing_.store(false, std::memory_order_release);
    }
}

void FileClock::seekTo(TimeNs pts) {
    if (playing_.load(std::memory_order_acquire)) {
        anchorHostNs_.store(hostNowNs() - pts, std::memory_order_release);
    } else {
        anchorPausedNs_.store(pts, std::memory_order_release);
    }
}

} // namespace bro::video
