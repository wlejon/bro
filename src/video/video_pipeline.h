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

    // Open a media file through the registered backends. Returns false only
    // when nothing in it can be decoded: a file with no video track is opened
    // and plays as sound, with no picture, and a file whose audio codec is
    // unsupported plays silently. Refusing the first of those is why an
    // audio-only source could not be played at all.
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

    /// How far the picture has to be turned clockwise to be the right way up:
    /// 0, 90, 180 or 270. See TrackInfo::rotationDegrees. Zero for every
    /// source that says nothing, which is most of them.
    int rotationDegrees() const { return rotation_; }

    /// The size the picture is *shown* at, which is frameWidth/frameHeight
    /// swapped at a quarter turn. The rule lives here rather than at each
    /// caller because there are several — the element's intrinsic size, its
    /// object-fit arithmetic — and a caller that forgot to swap would lay a
    /// portrait clip out as a landscape one.
    int displayWidth() const { return quarterTurned() ? frameH_ : frameW_; }
    int displayHeight() const { return quarterTurned() ? frameW_ : frameH_; }

    bool hasFrame() const { return cur_.valid; }

    /// How long the media resource is: the longest of the tracks this pipeline
    /// opened, and deliberately not the video track's own length.
    ///
    /// The two differ by more than a rounding. A soundtrack routinely runs a
    /// fraction of a second past the last picture, and a second of animation
    /// over six seconds of music is an ordinary file — which reported a
    /// duration of 1 while it was the video track's length that was taken.
    TimeNs durationNs() const { return duration_; }

    /// Where the CLOCK is, which after the last picture is not where
    /// `currentPts()` is.
    ///
    /// `currentPts()` is the timestamp of the picture on screen, so it freezes
    /// on the final frame — that is what a viewer is looking at, and it is the
    /// right answer to "which frame". The clock is host time scaled by the
    /// rate, so it goes on past that frame for as long as the resource does,
    /// which is the only thing that can say whether a file whose sound outlives
    /// its picture is over. See `ElVideo::isEnded`, the one caller.
    ///
    /// Deliberately NOT folded into `currentPts()`: two clocks arbitrating over
    /// which frame to show is how A/V sync bugs are born, and `currentPts()` is
    /// the one that answers that question. Safe from any thread — MediaClock
    /// takes its own lock for one multi-field snapshot.
    TimeNs clockNs() const { return clock_ ? clock_->nowNs() : 0; }

    /// True when this source has a video track this backend could decode.
    /// False for a sound-only file, which plays with no picture at all.
    bool hasVideo() const { return vdec_ != nullptr; }

    /// Where playback is.
    ///
    /// With a picture that is the timestamp of the picture on screen, which is
    /// why a seek reads back snapped to a frame boundary. With no picture
    /// there are no timestamps to read, so it is the media clock the sound is
    /// anchored to instead — the same clock that decides which frame to show
    /// when there is one.
    ///
    /// The clock is a FALLBACK, taken only when the pictures are not answering.
    /// Two clocks arbitrating is how A/V sync bugs are born: for as long as
    /// pictures are arriving they decide, always, and the sound is re-anchored
    /// to them. There are two ways for them to stop answering and the second
    /// was missed: a file with no video track at all, and a file whose pictures
    /// have RUN OUT while its sound plays on. Holding the last frame's
    /// timestamp through the rest of a soundtrack is an element that looks
    /// stuck — no `timeupdate` fires, the position never reaches `duration`,
    /// and then `ended` arrives from nowhere.
    ///
    /// Only while PLAYING, and clamped to the length. Paused at the end the
    /// last picture's own timestamp is still the answer, which is what keeps a
    /// seek reading back snapped to the frame it landed on.
    TimeNs currentPts() const;

    /// Nominal frames per second as the container declares it, or 0 when it
    /// declares nothing. An average — a variable-frame-rate file still has
    /// one — so it is good for timecode display and useless for stepping.
    double frameRate() const { return frameRate_; }

    // True once the demuxer has returned no more packets and every decoded
    // frame has been shown. Cleared by seekTo().
    //
    // This is "the PICTURES have run out", which is not the same thing as "the
    // resource is over" — a file can have six seconds of sound behind one
    // second of picture. `ElVideo::isEnded` is the second question and is what
    // the HTMLMediaElement "ended" event is fired from; this is a necessary
    // half of it and was mistaken for the whole of it.
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

    /// Pull every picture the decoder is willing to hand over and file it.
    /// True if the displayed frame changed.
    bool collectFrames(TimeNs nowNs);

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

    bool quarterTurned() const { return rotation_ == 90 || rotation_ == 270; }

    /// Wire an opened source and its backend's decoders into this pipeline.
    /// False when this backend can decode neither the picture nor the sound,
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
    int rotation_ = 0;

    // Where a sound-only file has got to. Written by advanceTo() from the
    // clock and read by currentPts(); untouched, and unread, whenever there
    // is a picture.
    TimeNs soundPos_ = 0;

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

    // The demuxer has run out of packets and the decoder has been told so.
    // Separate from endOfStream_ because there is a whole reorder buffer of
    // pictures still to come out between those two moments.
    bool drained_ = false;
    bool endOfStream_ = false;
};

} // namespace bro::video
