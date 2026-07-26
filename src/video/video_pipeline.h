#pragma once

#include "video/audio_decoder.h"
#include "video/media_backend.h"
#include "video/media_clock.h"
#include "video/media_source.h"
#include "video/video_decoder.h"

#include <memory>
#include <string>
#include <vector>

namespace bro::video {

// Owns a demuxer, a video decoder, optionally an audio decoder, and a
// clock. Glues them into something a UI element can drive with a single
// call per rendered frame.
//
// Runs synchronously on the caller's thread — every advanceTo(now) demuxes
// and decodes until we catch up. Fine for short files; a large file would
// benefit from moving demux+decode onto its own thread using the SpscRing
// primitive (already available) to hand frames back.
class VideoPipeline {
public:
    VideoPipeline();
    ~VideoPipeline();

    // Open a WebM file. Returns false if the file is missing or has no
    // supported video track. Audio is optional — absence is not an error.
    bool open(const std::string& path);

    void play();
    void pause();
    bool isPlaying() const;

    // Move playback to `pts` ns; the next advanceTo() resumes from there.
    void seekTo(TimeNs pts);

    /// True when seekTo(pts) would just decode on from the current position
    /// rather than restarting the demuxer. Callers with their own per-seek
    /// costs — an audio ring that has to be torn down and refilled — use this
    /// to skip that work for a frame step.
    bool wouldNudge(TimeNs pts) const;

    // Playback rate multiplier applied to the pipeline's MediaClock. Audio
    // is not yet rate-scaled; callers that care about audio pitch will need
    // to gate rate != 1.0.
    void setRate(double rate);

    // Called by the render path. Demuxes/decodes packets whose pts has
    // been reached, keeping the "current video frame" up to date. Returns
    // true if the current frame changed since the last call.
    bool advanceTo(TimeNs nowNs);

    // Convenience: advance based on the pipeline's own FileClock, which
    // starts at 0 when play() is called and monotonically walks forward
    // via steady_clock. Render-loop callers use this; seek tests use
    // advanceTo(pts) directly.
    bool advance();

    // Decoded RGBA of the most recent video frame ready for presentation.
    // Empty until the first frame has been decoded.
    const std::vector<uint8_t>& currentRgba() const { return rgba_; }
    int frameWidth() const { return frameW_; }
    int frameHeight() const { return frameH_; }
    bool hasFrame() const { return hasFrame_; }
    TimeNs durationNs() const { return duration_; }
    TimeNs currentPts() const { return currentPts_; }

    // True once the demuxer has returned no more packets and the decoder has
    // produced all frames. Callers use this to fire the HTMLMediaElement
    // "ended" event. Cleared by seekTo().
    bool isEnded() const { return endOfStream_; }

    // Audio decode pulls packets as wall-clock advances; callers can drain
    // PCM on the audio thread. Null if the file has no audio track.
    AudioDecoder* audioDecoder() const { return adec_.get(); }

    uint32_t audioSampleRate() const { return audioRate_; }
    uint32_t audioChannels() const { return audioChannels_; }

private:
    bool decodePacket(const MediaPacket& pkt);

    /// Keep a decoded frame as I420 in our own buffer. The decoder's planes
    /// are only valid until its next call, so catching up across a seek would
    /// otherwise have to convert every intermediate frame to RGBA just to
    /// keep it alive — and a memcpy is an order of magnitude cheaper than a
    /// per-pixel colour conversion.
    void storeFrame(const VideoFrame& frame);

    /// Convert the stored I420 to RGBA, once, if it has changed.
    void refreshRgba();

    /// Wire an opened source and its backend's decoders into this pipeline.
    /// False when the file carries no video track this backend can decode,
    /// with the pipeline left clean so open() can try the next one.
    bool adoptSource(const MediaBackend& backend, std::unique_ptr<MediaSource> source);

    std::unique_ptr<MediaSource> source_;
    std::unique_ptr<VideoDecoder> vdec_;
    std::unique_ptr<AudioDecoder> adec_;
    std::unique_ptr<MediaClock> clock_;

    uint32_t videoTrackId_ = 0;
    uint32_t audioTrackId_ = 0;
    uint32_t audioRate_ = 0;
    uint32_t audioChannels_ = 0;
    TimeNs duration_ = 0;

    // Latest decoded frame kept hot for the render thread. The I420 copy is
    // what decode writes; the RGBA is derived from it once per advance, not
    // once per decoded frame.
    std::vector<uint8_t> yuv_;      // packed Y then U then V, tight strides
    int yuvW_ = 0;
    int yuvH_ = 0;
    bool rgbaStale_ = false;

    std::vector<uint8_t> rgba_;
    int frameW_ = 0;
    int frameH_ = 0;
    bool hasFrame_ = false;
    TimeNs currentPts_ = -1;

    // Demux is one packet ahead — once a packet's pts exceeds `now`, we
    // stash it here and consume it on the next advanceTo() when time
    // catches up. Keeps the loop from over-eagerly decoding.
    MediaPacket pendingVideo_;
    bool pendingVideoValid_ = false;

    bool endOfStream_ = false;
};

} // namespace bro::video
