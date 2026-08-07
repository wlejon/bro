#pragma once

#include "video/audio_decoder.h"
#include "video/frame_queue.h"
#include "video/media_backend.h"
#include "video/media_clock.h"
#include "video/media_source.h"
#include "video/video_decoder.h"

#include <atomic>
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

    // Headless determinism: synchronously drains pending worker requests and decodes available
    // frames so presentation/currentTime are immediately observable during tests.
    void flush();

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

    // Worker thread internal state
    std::vector<Picture> workerPool_;
    std::deque<Picture> workerStaged_;
    bool drained_ = false;
    std::atomic<bool> endOfStream_{false};

    // Bounded queues between worker and caller
    SpscRing<Picture> decodedQueue_{32};
    SpscRing<Picture> recycleQueue_{32};
};

} // namespace bro::video
