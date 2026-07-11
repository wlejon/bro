#pragma once

#include "video/media_packet.h"
#include <mutex>

namespace bro::video {

// Drives playback timing. File playback and realtime calling have very
// different clock sources — file mode advances by PTS (optionally slaved to
// audio), calling mode advances by wall clock with a jitter buffer. Both
// present the same interface so the rest of the pipeline doesn't care.
//
// Thread-safety: `nowNs()` is called from the render thread; `setPlaying()`
// and `seekTo()` are called from the control thread. Implementations guard
// their state with a lock — the fields form one multi-field snapshot
// (anchor and rate must be read together or a rate change is observed
// half-applied), and every access is at most once per frame.
class MediaClock {
public:
    virtual ~MediaClock() = default;

    // Current playback position in stream-relative ns.
    virtual TimeNs nowNs() const = 0;

    virtual void setPlaying(bool playing) = 0;
    virtual bool isPlaying() const = 0;

    // Jump to a point in the stream. For file clocks this is cheap; for
    // network clocks the implementation may ignore or translate it.
    virtual void seekTo(TimeNs pts) = 0;

    // Playback rate multiplier (1.0 = realtime). Network clocks typically
    // ignore this; file clocks scale host time into stream time.
    virtual void setRate(double rate) { (void)rate; }
    virtual double rate() const { return 1.0; }
};

// Monotonic clock driven off the host steady_clock. Suitable for file
// playback with no external sync source. An audio-slaved variant will
// replace the tick source later without changing the interface.
class FileClock final : public MediaClock {
public:
    TimeNs nowNs() const override;
    void setPlaying(bool playing) override;
    bool isPlaying() const override;
    void seekTo(TimeNs pts) override;
    void setRate(double rate) override;
    double rate() const override;

private:
    TimeNs nowNsLocked() const;  // caller holds m_

    mutable std::mutex m_;
    bool playing_ = false;
    // Anchor: the host time at which stream-ts 0 was "now". When paused,
    // we freeze the computed stream position into anchorPausedNs_.
    // Stream position when playing is (hostNow - anchorHost) * rate.
    int64_t anchorHostNs_ = 0;
    int64_t anchorPausedNs_ = 0;
    double rate_ = 1.0;
};

} // namespace bro::video
