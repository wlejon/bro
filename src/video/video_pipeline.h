#pragma once

#include "video/audio_decoder.h"
#include "video/frame_queue.h"
#include "video/media_backend.h"
#include "video/media_clock.h"
#include "video/media_source.h"
#include "video/video_decoder.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace bro::video {

// Owns a demuxer, a video decoder, optionally an audio decoder, and a
// clock. Glues them into something a UI element can drive with a single
// call per rendered frame.
//
// Demux + decode runs on a per-pipeline worker thread. Decoded frames are
// handed back through a bounded queue (SpscRing).
//
// AUDIO THREAD FEED POINT DOCUMENTATION:
// Demux + decode for video runs on VideoPipeline's dedicated per-pipeline worker thread.
// Video decoded frames are handed back to the caller/render thread via a bounded SpscRing queue.
// Audio playback, when streaming via ElVideo::pumpStreamingAudio(), is fed from a second
// dedicated MediaSource on the main UI thread into the broaudio stream ring.
// The audio output device thread consumes samples directly from the broaudio stream ring.
class VideoPipeline {
public:
    VideoPipeline();
    ~VideoPipeline();

    // Open a media file through the registered backends. Returns false only
    // when nothing in it can be decoded: a file with no video track is opened
    // and plays as sound, with no picture, and a file whose audio codec is
    // unsupported plays silently.
    bool open(const std::string& path);

    void play();
    void pause();
    bool isPlaying() const;

    // Post a seek request to the worker thread. Flushes decoders, seeks demuxer,
    // and decodes to target. Requests coalesce to the newest target.
    // Non-blocking on caller thread.
    void seekTo(TimeNs pts);

    // Move exactly one picture forward (direction > 0) or back (direction < 0).
    // Blocks/synchronizes until exact frame step is complete.
    bool stepFrame(int direction);

    void setRate(double rate);

    // Called by the render path. Adopts decoded frames up to `nowNs` from worker queue.
    // Never decodes on caller thread. Returns true if displayed frame changed.
    bool advanceTo(TimeNs nowNs);

    // Convenience: advance based on pipeline's FileClock.
    bool advance();

    // The picture at `nowNs`, WAITED FOR. Same landing rule as advanceTo — the
    // last picture at or before the instant — and the same answer every run.
    //
    // advanceTo is the render path: it shows whatever the worker has handed
    // over, because a thread that draws must never wait for a decode. A
    // consumer that is not a screen wants the opposite and can afford it: a
    // suite comparing two renders frame for frame, a still pulled out of a
    // file. Before the decode moved off this thread, advanceTo *was* both, and
    // every such caller silently became "whatever had arrived" — which is a
    // comparison of two files at two different frames, and reads as twenty
    // decibels of disagreement about an edit that is in fact identical.
    bool settleAt(TimeNs nowNs);

    // Headless determinism, and the ONLY call on this class that is allowed to
    // wait for the worker. Blocks until the pictures the pipeline has been
    // asked for — the pending seek's target, and the position its clock has
    // reached — have been handed over, so `currentPts` and `currentRgba` are
    // observable on the line after the ask.
    //
    // Called per frame from Engine::pumpVideoEvents in HEADLESS mode only —
    // engine.cpp gates it on DisplayMode::Headless and reaches here through
    // ElVideo::advancePipeline, whose own header says the windowed engine must
    // not call it — and from tests; the windowed path draws whatever has
    // arrived and never blocks. A headless run fast-forwards its clock, so
    // without this the decode a frame needs has had a few real milliseconds to
    // happen and the picture simply stands still — which is what a headless
    // suite measures as a six-second stall.
    //
    // `through` is an instant the caller is about to ask about; the wait is for
    // whichever of it, the pending seek's target and the clock is furthest on.
    void flush(TimeNs through = -1);

    const std::vector<uint8_t>& currentRgba() const { return rgba_; }
    int frameWidth() const { return frameW_; }
    int frameHeight() const { return frameH_; }
    int rotationDegrees() const { return rotation_; }
    int displayWidth() const { return quarterTurned() ? frameH_ : frameW_; }
    int displayHeight() const { return quarterTurned() ? frameW_ : frameH_; }

    bool hasFrame() const { return cur_.valid || pendingSeekPts_.load(std::memory_order_relaxed) >= 0 || !staged_.empty(); }
    TimeNs durationNs() const { return duration_; }
    TimeNs clockNs() const { return clock_ ? clock_->nowNs() : 0; }
    MediaClock* clock() const { return clock_.get(); }
    void setClock(std::unique_ptr<MediaClock> clock);
    bool hasVideo() const { return vdec_ != nullptr; }

    TimeNs currentPts() const;
    double frameRate() const { return frameRate_; }
    bool isEnded() const;

    // Is there a picture for `pts` — decoded, on hand, and not waiting behind a
    // seek? Callable from any thread.
    //
    // **The question a caller asks when it is holding something else back until
    // the picture is there**, which is ElVideo's audio preroll gate and nothing
    // else so far. Neither of the two obvious substitutes answers it.
    // `currentPts()` reports where the pipeline was *asked* to be while a seek
    // is in flight, which is the opposite of what a gate needs — it would open
    // on the promise. `hasFrame()` is true for a seek that has not landed, for
    // the same reason.
    //
    // Deliberately about what has been *made* rather than what has been shown:
    // `cur_` is placed by the render path, so an element that is not being drawn
    // — hidden behind another stage, or a preview taken off the canvas — would
    // never open a gate that waited for it, and the sound would never start.
    // Decoded is the honest reading of "there is a picture for that moment".
    bool decodedThrough(TimeNs pts) const;

    AudioDecoder* audioDecoder() const { return adec_.get(); }
    uint32_t audioSampleRate() const { return audioRate_; }
    uint32_t audioChannels() const { return audioChannels_; }

private:
    struct Picture {
        std::vector<uint8_t> yuv;
        int w = 0;
        int h = 0;
        TimeNs pts = -1;
        bool valid = false;
    };

    void startWorker();
    void stopWorker();
    void workerLoop();

    bool pumpOneWorker(bool* changed);
    bool collectFramesWorker();
    void performWorkerSeek(TimeNs target);
    void drainThrough(TimeNs pts, std::chrono::milliseconds budget);
    void allowDecodeThrough(TimeNs from);
    bool presentNext(TimeNs at);
    TimeNs leadNs() const;
    TimeNs frameIntervalNs() const;
    void stage(Picture&& p);
    void storeFrame(Picture& dst, const VideoFrame& frame);
    void refreshRgba();

    Picture takePictureWorker();
    void recycleWorker(Picture&& p);
    void recycleCaller(Picture&& p);

    bool quarterTurned() const { return rotation_ == 90 || rotation_ == 270; }
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

    TimeNs soundPos_ = 0;
    // Where the pipeline was last ASKED to be, until a picture has been placed
    // for it, and the pts of the last picture that was. Both exist for one
    // question — what does `currentPts` answer while a seek is in flight and
    // there is no picture — and neither is a second clock: as soon as `cur_`
    // is valid it is the answer and these are not consulted. Answering 0 there
    // is what a scrubbed playhead snapping back to the top of the file looked
    // like.
    TimeNs pendingTarget_ = -1;
    TimeNs lastPts_ = -1;
    // How long a picture lasts, measured, for the files that do not say — the
    // smallest positive gap between two pictures this pipeline has staged. See
    // `frameIntervalNs`, which is the only reader.
    TimeNs seenIntervalNs_ = -1;
    // Whether an advance may hand over everything left staged once the source
    // is over — see advanceTo. Off only for the duration of a frame step.
    bool presentTail_ = true;

    // Caller-thread state
    Picture cur_;
    std::deque<Picture> staged_;
    std::vector<uint8_t> rgba_;
    int frameW_ = 0;
    int frameH_ = 0;
    bool rgbaStale_ = false;

    // Worker thread & sync
    std::thread workerThread_;
    mutable std::mutex mutex_;
    std::condition_variable cvWorker_;
    std::condition_variable cvCaller_;
    std::atomic<bool> workerRunning_{false};
    std::atomic<bool> stopWorker_{false};
    std::atomic<int64_t> pendingSeekPts_{-1};
    std::atomic<bool> workerSeeking_{false};
    std::atomic<bool> flushRequested_{false};
    std::atomic<bool> workerIdle_{false};
    // How far the worker may decode, in the stream's own timestamps. Written by
    // the caller whenever it says which moment it is showing (advanceTo), asks
    // to be shown one (drainThrough) or moves (seekTo); read by the worker
    // before it pulls another packet.
    //
    // A worker with no ceiling decodes until the ring is full, which is not the
    // pipeline the rest of the system was built against: the *synchronous*
    // pipeline decoded "until something is staged", one picture of lead, and a
    // producer on the other end of a live source reads that ask as the moment
    // the screen has reached. ffmpeg-bro's output preview is exactly that — it
    // makes the next picture one frame after the last one asked for — so a
    // reader running a second ahead asked it for pictures past the moment its
    // own sound had reached, it stopped making them, and the element's decoder
    // ended its video track on the read timeout. A preview that played its
    // sound perfectly over one frozen frame.
    //
    // It is also the memory: 32 pictures of 4K YUV is 400 MB per element, on a
    // timeline that holds one element per clip near the playhead.
    std::atomic<int64_t> decodeCeiling_{0};
    // The newest picture the worker has produced for the position the pipeline
    // is at now, or -1 for "nothing since the last seek". The atomic twin of
    // `workerNewestPts_`, published for `decodedThrough` — which is asked from
    // the main thread, where none of the caller-thread picture state may be
    // read. Cleared by the caller in `seekTo` before the request is posted, and
    // by the worker as it takes one up: the pictures on either side of a seek
    // belong to different positions, so one must never answer for the other.
    std::atomic<int64_t> decodedThroughNs_{-1};

    // Worker thread internal state
    std::vector<Picture> workerPool_;
    std::deque<Picture> workerStaged_;
    // Set for the duration of performWorkerSeek. While it is set, decoded
    // pictures go to `workerStaged_` rather than straight into the ring, which
    // is what lets the seek prune the frames before its target and notice when
    // it has decoded past it — both of which were written and neither of which
    // ran, because collectFramesWorker pushed into the ring and the seek's loop
    // only ever saw the overflow.
    bool workerInSeek_ = false;
    // The newest picture the worker has produced, so it can tell whether it is
    // already past `decodeCeiling_`. Worker-thread only.
    TimeNs workerNewestPts_ = -1;
    bool drained_ = false;
    std::atomic<bool> endOfStream_{false};

    // Bounded queues between worker and caller
    SpscRing<Picture> decodedQueue_{32};
    SpscRing<Picture> recycleQueue_{32};
};

} // namespace bro::video
