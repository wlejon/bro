#pragma once

#include "video/media_packet.h"
#include <functional>
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

// Audio-slaved clock. When audio is playing, nowNs() is derived from what
// the audio output/device has actually consumed (played samples * ns_per_sample + anchor PTS).
// Seeks and rate changes re-anchor the slaved clock. If audio underruns, the clock
// holds instead of steady_clock walking ahead into desync.
class AudioSlavedClock final : public MediaClock {
public:
    // How many frames the audio device has ACTUALLY played, or a **negative
    // number** when the stream cannot say — paused, not open, restarted. That
    // distinction is the whole of this class's correctness: it used to answer 0
    // for "cannot say", and 0 is a count. A paused stream therefore pulled the
    // frame anchor back to the top of the file while the position anchor stayed
    // where the playhead was, so the first real reading afterwards — a quarter
    // of a million frames — read as five seconds of elapsed time and the clock
    // jumped from six seconds to eleven. What that looked like: resuming a
    // preview after a pause ran it to the end of its range and stopped it.
    using AudioPositionProvider = std::function<int64_t()>;

    explicit AudioSlavedClock(uint32_t sampleRate = 48000,
                              AudioPositionProvider provider = nullptr);
    ~AudioSlavedClock() override = default;

    TimeNs nowNs() const override;
    void setPlaying(bool playing) override;
    bool isPlaying() const override;
    void seekTo(TimeNs pts) override;
    void setRate(double rate) override;
    double rate() const override;

    void setAudioProvider(uint32_t sampleRate, AudioPositionProvider provider);
    void updatePlayedFrames(uint64_t playedFrames);
    uint32_t sampleRate() const;

private:
    TimeNs nowNsLocked() const;
    int64_t currentPlayedFramesLocked() const;
    int64_t readingLocked() const;
    void reanchorLocked(TimeNs at);

    mutable std::mutex m_;
    bool playing_ = false;
    uint32_t sampleRate_ = 48000;
    AudioPositionProvider provider_;

    int64_t fallbackHostAnchorNs_ = 0;
    mutable int64_t anchorPtsNs_ = 0;
    // Negative means "not anchored to a reading yet" — the first believable one
    // becomes the anchor. That happens when a re-anchor lands while the stream
    // cannot answer, which is what a resume is: an element restarts its pipeline
    // before it restarts its audio stream.
    mutable int64_t anchorPlayedFrames_ = -1;
    // The last position this clock reported. What it re-anchors *to*, so a
    // restarted counter keeps the position rather than adding itself to it.
    mutable TimeNs lastNowNs_ = 0;
    // The highest reading this counter has given. It is cumulative for the life
    // of the stream, so anything below this is not a position — see
    // `readingLocked`. Reset only when the provider is replaced, which is a
    // different counter.
    mutable int64_t maxSeenFrames_ = -1;
    int64_t anchorPausedNs_ = 0;
    double rate_ = 1.0;
    uint64_t manualPlayedFrames_ = 0;
};

} // namespace bro::video

