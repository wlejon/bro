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

int64_t AudioSlavedClock::currentPlayedFramesLocked() const {
    if (provider_) {
        return provider_();
    }
    if (manualPlayedFrames_ > 0) {
        return static_cast<int64_t>(manualPlayedFrames_);
    }
    const int64_t nowHost = hostNowNs();
    const int64_t elapsedHost = nowHost - fallbackHostAnchorNs_;
    if (elapsedHost <= 0) return 0;
    return static_cast<int64_t>(static_cast<double>(elapsedHost) * sampleRate_ / 1e9);
}

/// The counter's reading, or a negative number when it cannot be believed.
///
/// Two ways it cannot. The stream says so itself (see AudioPositionProvider),
/// and — the one that cost five seconds of playhead — it answers *below* a
/// reading it has already given. It counts frames the device has played for the
/// life of the stream, so it only goes up; a stream that is not running reports
/// zero, and zero is a count. Anchoring to that zero while the position anchor
/// stayed where the playhead was made the next honest reading — a quarter of a
/// million frames — read as five seconds of elapsed time. Measured: resuming a
/// paused preview at 6.42 s put its clock at 11.66 s, which ran the picture to
/// the end of the render's range and stopped it there.
int64_t AudioSlavedClock::readingLocked() const {
    const int64_t v = currentPlayedFramesLocked();
    if (v < 0) return -1;
    if (maxSeenFrames_ >= 0 && v < maxSeenFrames_) return -1;
    maxSeenFrames_ = v;
    return v;
}

/// Start counting again from `at`, whatever the audio stream's counter does
/// next. Both halves together — the position and the frame it is measured
/// from — because moving one without the other is a clock that jumps by the
/// whole of the counter. A reading that cannot be believed leaves the frame
/// anchor unset (-1) and the first believable one takes it, which is what a
/// resume needs: an element restarts its pipeline before its audio stream.
void AudioSlavedClock::reanchorLocked(TimeNs at) {
    anchorPtsNs_ = at;
    lastNowNs_ = at;
    anchorPlayedFrames_ = readingLocked();
}

TimeNs AudioSlavedClock::nowNsLocked() const {
    if (!playing_) {
        return anchorPausedNs_;
    }

    // **A stream that cannot say where it is does not move the clock** — see
    // `readingLocked`, which is where that judgement lives.
    const int64_t curPlayed = readingLocked();
    if (curPlayed < 0) return lastNowNs_;

    // Not anchored to a reading yet — the first believable one is where the
    // position we are already at was reached.
    if (anchorPlayedFrames_ < 0) {
        anchorPtsNs_ = lastNowNs_;
        anchorPlayedFrames_ = curPlayed;
    }

    const int64_t deltaFrames = curPlayed - anchorPlayedFrames_;
    const double nsPerSample = 1e9 / static_cast<double>(sampleRate_ > 0 ? sampleRate_ : 48000);
    const int64_t elapsedNs = static_cast<int64_t>(static_cast<double>(deltaFrames) * nsPerSample);

    lastNowNs_ = anchorPtsNs_ + elapsedNs;
    return lastNowNs_;
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
        // Deliberately not anchored to a reading here: an element resumes its
        // pipeline before it resumes its audio stream, so the counter at this
        // instant is either stale or unavailable. The first reading the stream
        // can give is the anchor — see `nowNsLocked`.
        reanchorLocked(anchorPausedNs_);
    } else {
        anchorPausedNs_ = nowNsLocked();
        playing_ = false;
    }
}

void AudioSlavedClock::seekTo(TimeNs pts) {
    std::lock_guard<std::mutex> lk(m_);
    if (!playing_) anchorPausedNs_ = pts;
    reanchorLocked(pts);
}

void AudioSlavedClock::setRate(double rate) {
    if (rate <= 0.0) rate = 1.0;
    std::lock_guard<std::mutex> lk(m_);
    if (rate == rate_) return;
    if (playing_) {
        const TimeNs here = nowNsLocked();
        rate_ = rate;
        reanchorLocked(here);
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
    // A different stream is a different counter, so the position is kept, the
    // frame anchor is taken from whatever the new one first answers, and what
    // the old one had reached says nothing about this one.
    maxSeenFrames_ = -1;
    reanchorLocked(here);
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

