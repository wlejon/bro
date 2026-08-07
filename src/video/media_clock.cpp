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

// ── AudioSlavedClock ────────────────────────────────────────────────────────

AudioSlavedClock::AudioSlavedClock(uint32_t sampleRate, AudioPositionProvider provider)
    : sampleRate_(sampleRate > 0 ? sampleRate : 48000), provider_(std::move(provider)) {
    fallbackHostAnchorNs_ = hostNowNs();
}

uint64_t AudioSlavedClock::currentPlayedFramesLocked() const {
    if (provider_) {
        return provider_();
    }
    if (manualPlayedFrames_ > 0) {
        return manualPlayedFrames_;
    }
    const int64_t nowHost = hostNowNs();
    const int64_t elapsedHost = nowHost - fallbackHostAnchorNs_;
    if (elapsedHost <= 0) return 0;
    return static_cast<uint64_t>(static_cast<double>(elapsedHost) * sampleRate_ / 1e9);
}

TimeNs AudioSlavedClock::nowNsLocked() const {
    if (!playing_) {
        return anchorPausedNs_;
    }

    const uint64_t curPlayed = currentPlayedFramesLocked();
    if (curPlayed < anchorPlayedFrames_) {
        anchorPlayedFrames_ = curPlayed;
    }

    const uint64_t deltaFrames = curPlayed - anchorPlayedFrames_;
    const double nsPerSample = 1e9 / static_cast<double>(sampleRate_ > 0 ? sampleRate_ : 48000);
    const int64_t elapsedNs = static_cast<int64_t>(static_cast<double>(deltaFrames) * nsPerSample);

    return anchorPtsNs_ + elapsedNs;
}

TimeNs AudioSlavedClock::nowNs() const {
    std::lock_guard<std::mutex> lk(m_);
    return nowNsLocked();
}

bool AudioSlavedClock::isPlaying() const {
    std::lock_guard<std::mutex> lk(m_);
    return playing_;
}

void AudioSlavedClock::setPlaying(bool playing) {
    std::lock_guard<std::mutex> lk(m_);
    if (playing == playing_) return;
    if (playing) {
        playing_ = true;
        anchorPtsNs_ = anchorPausedNs_;
        anchorPlayedFrames_ = currentPlayedFramesLocked();
    } else {
        anchorPausedNs_ = nowNsLocked();
        playing_ = false;
    }
}

void AudioSlavedClock::seekTo(TimeNs pts) {
    std::lock_guard<std::mutex> lk(m_);
    const uint64_t curPlayed = currentPlayedFramesLocked();
    if (playing_) {
        anchorPtsNs_ = pts;
        anchorPlayedFrames_ = curPlayed;
    } else {
        anchorPausedNs_ = pts;
        anchorPtsNs_ = pts;
        anchorPlayedFrames_ = curPlayed;
    }
}

void AudioSlavedClock::setRate(double rate) {
    if (rate <= 0.0) rate = 1.0;
    std::lock_guard<std::mutex> lk(m_);
    if (rate == rate_) return;
    if (playing_) {
        const TimeNs here = nowNsLocked();
        rate_ = rate;
        anchorPtsNs_ = here;
        anchorPlayedFrames_ = currentPlayedFramesLocked();
    } else {
        rate_ = rate;
    }
}

double AudioSlavedClock::rate() const {
    std::lock_guard<std::mutex> lk(m_);
    return rate_;
}

void AudioSlavedClock::setAudioProvider(uint32_t sampleRate, AudioPositionProvider provider) {
    std::lock_guard<std::mutex> lk(m_);
    const TimeNs here = nowNsLocked();
    sampleRate_ = sampleRate > 0 ? sampleRate : 48000;
    provider_ = std::move(provider);
    anchorPtsNs_ = here;
    anchorPlayedFrames_ = currentPlayedFramesLocked();
}

void AudioSlavedClock::updatePlayedFrames(uint64_t playedFrames) {
    std::lock_guard<std::mutex> lk(m_);
    manualPlayedFrames_ = playedFrames;
}

uint32_t AudioSlavedClock::sampleRate() const {
    std::lock_guard<std::mutex> lk(m_);
    return sampleRate_;
}

} // namespace bro::video

