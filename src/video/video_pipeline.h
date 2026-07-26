#pragma once

#include "video/audio_decoder.h"
#include "video/media_backend.h"
#include "video/media_clock.h"
#include "video/media_source.h"
#include "video/video_decoder.h"

#include <deque>
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
    // Displays the frame whose presentation interval contains `pts` — the
    // last one at or before it, which is the picture a viewer at that instant
    // would be looking at.
    void seekTo(TimeNs pts);

    /// Move exactly one picture forward (direction > 0) or back (direction <
    /// 0), landing on a real decoded frame. False when there is none — the
    /// last frame of the file going forward, the first going back.
    ///
    /// Callers must NOT emulate this with currentTime += 1/fps. A nominal
    /// frame rate is an average: it is wrong for variable-rate files, and
    /// even at a constant 30000/1001 the seconds-to-ns round trip misses the
    /// frame boundary by a few ns, so the step lands back on the frame it
    /// started from. The file's own timestamps are the only thing that knows
    /// where the next picture is.
    bool stepFrame(int direction);

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
    bool hasFrame() const { return cur_.valid; }
    TimeNs durationNs() const { return duration_; }
    TimeNs currentPts() const { return cur_.pts; }

    /// Nominal frames per second as the container declares it, or 0 when it
    /// declares nothing. An average — a variable-frame-rate file still has
    /// one — so it is good for timecode display and useless for stepping.
    double frameRate() const { return frameRate_; }

    // True once the demuxer has returned no more packets and every decoded
    // frame has been shown. Callers use this to fire the HTMLMediaElement
    // "ended" event. Cleared by seekTo().
    bool isEnded() const { return endOfStream_ && staged_.empty(); }

    // Audio decode pulls packets as wall-clock advances; callers can drain
    // PCM on the audio thread. Null if the file has no audio track.
    AudioDecoder* audioDecoder() const { return adec_.get(); }

    uint32_t audioSampleRate() const { return audioRate_; }
    uint32_t audioChannels() const { return audioChannels_; }

private:
    /// A decoded picture in our own memory: I420, tight strides.
    ///
    /// The decoder's planes are only valid until its next call, so a frame we
    /// want to keep has to be copied somewhere. Copying to I420 rather than
    /// converting to RGBA is what makes seeking cheap: catching up across a
    /// seek walks a whole GOP, and a memcpy is an order of magnitude cheaper
    /// than a per-pixel colour conversion of a picture nobody will look at.
    struct Picture {
        std::vector<uint8_t> yuv;   // Y plane, then U, then V
        int w = 0;
        int h = 0;
        TimeNs pts = -1;
        bool valid = false;
    };

    /// Decode one packet, filing whatever pictures come out either as the
    /// frame to display (pts at or before `nowNs`) or as staged future ones.
    /// True if the displayed frame changed.
    bool decodePacket(const MediaPacket& pkt, TimeNs nowNs);

    /// Restart the demuxer at the keyframe at or before `target`, dropping
    /// every picture decoded from the old position.
    void restartAt(TimeNs target);

    /// Demux and decode one video packet. False at end of stream. Sets
    /// *changed when the displayed picture moved on.
    bool pumpOne(TimeNs nowNs, bool* changed);

    void storeFrame(Picture& dst, const VideoFrame& frame);

    /// Convert the displayed picture to RGBA, once, if it has changed.
    void refreshRgba();

    Picture takePicture();
    void recycle(Picture&& p);

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
    double frameRate_ = 0.0;

    // What is on screen: the last picture at or before `now`.
    Picture cur_;

    // Pictures already decoded whose time has not come, oldest first. Holding
    // one is what lets cur_ be the frame the viewer is *inside* rather than
    // the next one up — we only know a picture is the current one once we
    // have seen that the following one is still in the future. Normally holds
    // exactly one; it is a queue because a decoder is free to hand back
    // several pictures for a single packet.
    std::deque<Picture> staged_;

    // Buffers from retired pictures, kept so steady-state playback does not
    // allocate a frame's worth of memory every frame.
    std::vector<Picture> pool_;

    std::vector<uint8_t> rgba_;
    int frameW_ = 0;
    int frameH_ = 0;
    bool rgbaStale_ = false;        // cur_ has changed since the last convert

    bool endOfStream_ = false;
};

} // namespace bro::video
