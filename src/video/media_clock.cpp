#include "video/media_clock.h"

#include <chrono>

namespace bro::video {

namespace {
int64_t hostNowNs() {
    using namespace std::chrono;
    return duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count();
}
} // namespace

TimeNs FileClock::nowNsLocked() const {
    if (playing_) {
        const int64_t host = hostNowNs() - anchorHostNs_;
        return static_cast<int64_t>(host * rate_);
    }
    return anchorPausedNs_;
}

TimeNs FileClock::nowNs() const {
    std::lock_guard<std::mutex> lk(m_);
    return nowNsLocked();
}

bool FileClock::isPlaying() const {
    std::lock_guard<std::mutex> lk(m_);
    return playing_;
}

void FileClock::setPlaying(bool playing) {
    std::lock_guard<std::mutex> lk(m_);
    if (playing == playing_) return;
    if (playing) {
        // hostOffset such that (hostNow - hostOffset) * rate == pausedAt
        anchorHostNs_ = hostNowNs() - static_cast<int64_t>(anchorPausedNs_ / rate_);
        playing_ = true;
    } else {
        anchorPausedNs_ = nowNsLocked();
        playing_ = false;
    }
}

void FileClock::seekTo(TimeNs pts) {
    std::lock_guard<std::mutex> lk(m_);
    if (playing_) {
        anchorHostNs_ = hostNowNs() - static_cast<int64_t>(pts / rate_);
    } else {
        anchorPausedNs_ = pts;
    }
}

void FileClock::setRate(double rate) {
    if (rate <= 0.0) rate = 1.0;
    std::lock_guard<std::mutex> lk(m_);
    if (rate == rate_) return;
    // Re-anchor so the current stream position stays continuous across the
    // rate change. When paused the position is already latched in
    // anchorPausedNs_; only the playing branch needs host-anchor adjustment.
    if (playing_) {
        const int64_t here = nowNsLocked(); // computed with old rate
        rate_ = rate;
        anchorHostNs_ = hostNowNs() - static_cast<int64_t>(here / rate);
    } else {
        rate_ = rate;
    }
}

double FileClock::rate() const {
    std::lock_guard<std::mutex> lk(m_);
    return rate_;
}

} // namespace bro::video
