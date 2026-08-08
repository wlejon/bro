#include "video/video_pipeline.h"

#include "video/media_backend.h"
#include "video/yuv_to_rgb.h"

#include "util/log.h"

#include <chrono>
#include <cstring>

namespace bro::video {

// How long any of the three blocking calls here — `flush`, and the two waits a
// deliberate single step makes — will wait for the worker before giving up and
// answering with what it has. Nothing on the drawing path waits at all; this is
// the ceiling on a headless frame and on a keypress, not on a drag.
static constexpr auto kWaitCeiling = std::chrono::seconds(5);

// What one headless frame's `flush` will spend waiting for the worker. A frame
// decode is single-digit milliseconds, so this is two orders of magnitude of
// slack — and it is bounded at all because the alternative is a source that has
// stopped producing (a finished render, a camera unplugged) freezing the pump
// for `kWaitCeiling` on every frame.
static constexpr auto kFrameWait = std::chrono::milliseconds(100);

// How many pictures the worker may run ahead of the moment being shown, and
// what that is in time when the file will not say its frame rate.
//
// Six, and the number is a compromise between two failures that have both been
// measured. The synchronous pipeline this replaced kept ONE — it decoded until
// a picture was staged and stopped — which is what makes a live producer's
// "the screen has reached this moment" reading true, and one is too few to
// absorb a decode that takes longer than a frame, which is the stutter the
// worker exists to remove. Thirty-two — the ring, which was the only bound
// there was — is a second of lead at 25 fps, far enough ahead that ffmpeg-bro's
// output preview stopped making pictures at all.
static constexpr int kLeadFrames = 6;
static constexpr TimeNs kLeadNsUnknownRate = 250 * 1000000LL;

VideoPipeline::VideoPipeline() {
    clock_ = std::make_unique<FileClock>();
}

VideoPipeline::~VideoPipeline() {
    stopWorker();
}

void VideoPipeline::startWorker() {
    if (workerRunning_) return;
    stopWorker_ = false;
    workerRunning_ = true;
    workerThread_ = std::thread([this] { workerLoop(); });
}

void VideoPipeline::stopWorker() {
    if (!workerRunning_) return;
    stopWorker_ = true;
    cvWorker_.notify_all();
    if (workerThread_.joinable()) {
        workerThread_.join();
    }
    workerRunning_ = false;
}

bool VideoPipeline::adoptSource(const MediaBackend& backend,
                                std::unique_ptr<MediaSource> source) {
    stopWorker();

    videoTrackId_ = 0;
    audioTrackId_ = 0;
    audioRate_ = 0;
    audioChannels_ = 0;
    duration_ = 0;
    frameRate_ = 0.0;
    rotation_ = 0;
    frameW_ = 0;
    frameH_ = 0;
    soundPos_ = 0;
    vdec_.reset();
    adec_.reset();
    staged_.clear();
    workerStaged_.clear();
    recycleCaller(std::move(cur_));
    drained_ = false;
    endOfStream_ = false;
    pendingSeekPts_ = -1;
    pendingTarget_ = -1;
    lastPts_ = -1;
    workerInSeek_ = false;
    workerNewestPts_ = -1;
    decodeCeiling_ = 0;
    // A different file is a different frame interval, and nothing measured about
    // the last one says anything about this one.
    seenIntervalNs_ = -1;

    TimeNs videoDuration = 0;
    TimeNs audioDuration = 0;

    for (const auto& t : source->tracks()) {
        if (t.kind == TrackKind::Video && videoTrackId_ == 0) {
            auto dec = backend.makeVideoDecoder ? backend.makeVideoDecoder(t) : nullptr;
            if (!dec) continue;
            vdec_ = std::move(dec);
            videoTrackId_ = t.id;
            frameW_ = static_cast<int>(t.width);
            frameH_ = static_cast<int>(t.height);
            videoDuration = t.durationNs;
            frameRate_ = t.frameRate;
            const int r = ((t.rotationDegrees % 360) + 360) % 360;
            rotation_ = (r == 90 || r == 180 || r == 270) ? r : 0;
        } else if (t.kind == TrackKind::Audio && audioTrackId_ == 0) {
            auto dec = backend.makeAudioDecoder ? backend.makeAudioDecoder(t) : nullptr;
            if (!dec) continue;
            adec_ = std::move(dec);
            audioTrackId_ = t.id;
            audioRate_ = t.sampleRate;
            audioChannels_ = t.channels;
            audioDuration = t.durationNs;
        }
    }

    if (!vdec_ && !adec_) return false;

    duration_ = videoDuration > audioDuration ? videoDuration : audioDuration;

    if (!vdec_) {
        source->setActiveTracks({});
        source_ = std::move(source);
        return true;
    }

    source->setActiveTracks({videoTrackId_});
    source_ = std::move(source);

    startWorker();
    flush();

    return true;
}

bool VideoPipeline::open(const std::string& path) {
    for (const auto& backend : mediaBackends()) {
        if (!backend.open) continue;
        auto source = backend.open(path);
        if (!source) continue;
        if (adoptSource(backend, std::move(source))) {
            LOG_INFO("video: '%s' opened by the %s backend", path.c_str(),
                     backend.name.c_str());
            return true;
        }
    }
    return false;
}

TimeNs VideoPipeline::currentPts() const {
    if (!vdec_) return soundPos_;
    // No picture: a seek is in flight, or the worker has not handed one over
    // yet. The honest answer is where the pipeline was asked to be, and the
    // position it last showed if it was never asked for anything — 0 is a
    // claim about the file and the two below are claims about this pipeline.
    if (!cur_.valid) {
        const int64_t pending = pendingSeekPts_.load(std::memory_order_relaxed);
        if (pending >= 0) return static_cast<TimeNs>(pending);
        if (pendingTarget_ >= 0) return pendingTarget_;
        return lastPts_ >= 0 ? lastPts_ : 0;
    }
    // The pictures answer for as long as there are any. Once they have run out
    // the clock takes over — see the header for why that is not a second clock
    // arbitrating — and it is clamped to the length so a position can never be
    // reported past the end of the resource it belongs to.
    if (!clock_ || !clock_->isPlaying() || !isEnded()) return cur_.pts;
    const TimeNs now = clock_->nowNs();
    if (now <= cur_.pts) return cur_.pts;
    return duration_ > 0 && now > duration_ ? duration_ : now;
}

bool VideoPipeline::isEnded() const {
    return endOfStream_.load(std::memory_order_relaxed) &&
           decodedQueue_.sizeApprox() == 0 &&
           staged_.empty();
}

void VideoPipeline::play() { if (clock_) clock_->setPlaying(true); }
void VideoPipeline::pause() { if (clock_) clock_->setPlaying(false); }
bool VideoPipeline::isPlaying() const { return clock_ && clock_->isPlaying(); }

void VideoPipeline::setRate(double rate) {
    if (clock_) clock_->setRate(rate);
}

void VideoPipeline::setClock(std::unique_ptr<MediaClock> clock) {
    if (!clock) return;
    if (clock_) {
        const bool playing = clock_->isPlaying();
        const double rate = clock_->rate();
        const TimeNs pos = clock_->nowNs();
        clock->setRate(rate);
        clock->seekTo(pos);
        clock->setPlaying(playing);
    }
    clock_ = std::move(clock);
}

void VideoPipeline::seekTo(TimeNs pts) {
    if (!source_) return;
    pendingTarget_ = pts;
    // Stored rather than raised: a seek is a new position and a backward one
    // must not leave the worker licensed to decode where the playhead was.
    decodeCeiling_.store(static_cast<int64_t>(pts) + leadNs(), std::memory_order_relaxed);
    if (!vdec_) {
        if (clock_) clock_->seekTo(pts);
        advanceTo(pts);
        return;
    }

    while (!staged_.empty()) {
        recycleCaller(std::move(staged_.front()));
        staged_.pop_front();
    }
    recycleCaller(std::move(cur_));

    Picture oldFrame;
    while (decodedQueue_.tryPop(oldFrame)) {
        recycleCaller(std::move(oldFrame));
    }

    // Un-ended here rather than in `performWorkerSeek`, which also does it: a
    // seek is a promise that more pictures are coming, and `isEnded()` is read
    // on THIS thread on the line after the ask. The worker sets the flag and
    // the caller clears it, so the window between them belonged to whichever
    // ran first — seeking back from the end reported `ended` until the worker
    // got round to the request, which is an element that will not play again
    // and a transport that has to be pressed twice.
    endOfStream_ = false;

    pendingSeekPts_ = pts;
    cvWorker_.notify_one();
    if (clock_) clock_->seekTo(pts);
}

/// Wait until the worker has handed over a picture at or past `pts`, staging
/// everything it produces on the way. A negative `pts` asks only for the first
/// picture there is, which is what an `open` wants.
///
/// This is the one thing the async pipeline needed and did not have. The ring
/// between the two threads holds 32 pictures, so *where a seek lands* was a
/// function of how far the worker happened to have got: a keyframe a second
/// behind the target fills the ring with frames that never reach it, the
/// caller adopts the last one it can see, and the same seek on the same file
/// answers with a different frame on the next run. Every "the same picture"
/// comparison in ffmpeg-bro's suites is that determinism, measured in dB.
/// Let the worker decode `kLeadFrames` past `from`. Raised and never lowered:
/// a draw says which moment the screen has reached, a wait says which moment it
/// needs, and whichever is further on is what may be decoded. A seek is the one
/// thing that puts it back — it is a new position, not a later one — and does
/// so by storing rather than through here.
void VideoPipeline::allowDecodeThrough(TimeNs from) {
    const int64_t want = static_cast<int64_t>(from) + leadNs();
    int64_t seen = decodeCeiling_.load(std::memory_order_relaxed);
    while (want > seen &&
           !decodeCeiling_.compare_exchange_weak(seen, want, std::memory_order_relaxed)) {
    }
}

TimeNs VideoPipeline::leadNs() const {
    return frameRate_ > 0.0 ? static_cast<TimeNs>(kLeadFrames * 1e9 / frameRate_)
                            : kLeadNsUnknownRate;
}

/// How long one picture lasts here, declared or measured.
///
/// **A container need not say, and the end of the file is where that shows.**
/// The last picture of a file starts one interval before its duration, so a wait
/// that has to know whether it is being asked for the final frame has to know
/// how long a frame is — and with `frameRate_` at zero the answer was "no
/// interval at all", which put the last picture strictly inside the file and
/// turned `drainThrough`'s end-of-file wait off. What that looked like: a settle
/// on the last frame returned the moment that picture arrived, so `isEnded()` on
/// the line after it was true or false depending on whether the worker had got
/// round to the two reads that discover the EOF. One test in three.
///
/// So it is measured when it is not declared: the smallest positive gap between
/// two pictures this pipeline has actually handed to the screen, which is the
/// frame interval by construction — pictures are staged and presented in
/// presentation order, and a gap larger than one frame is a picture that was
/// skipped or a seek. Nothing is inferred from one picture, so a file settled at
/// its own end before anything else has been shown falls back to the same zero
/// as before and is no worse.
///
/// Deliberately not `leadNs`'s fallback, which is a *policy* — how far ahead of
/// the screen the worker may run — rather than a measurement, and is answered
/// for a pipeline that has decoded nothing at all.
TimeNs VideoPipeline::frameIntervalNs() const {
    if (frameRate_ > 0.0) return static_cast<TimeNs>(1e9 / frameRate_);
    return seenIntervalNs_ > 0 ? seenIntervalNs_ : 0;
}

/// Put a picture the worker has handed over on the caller's staging deque.
///
/// One place rather than two `push_back`s, because the gap between the picture
/// going on and the one before it is the only measurement of the frame interval
/// there is — see `frameIntervalNs`. The picture before it is the back of the
/// deque, or the one on the screen when the deque is empty.
void VideoPipeline::stage(Picture&& p) {
    const TimeNs prev = !staged_.empty() ? staged_.back().pts
                                         : (cur_.valid ? cur_.pts : -1);
    if (prev >= 0 && p.pts > prev) {
        const TimeNs gap = p.pts - prev;
        if (seenIntervalNs_ < 0 || gap < seenIntervalNs_) seenIntervalNs_ = gap;
    }
    staged_.push_back(std::move(p));
}

void VideoPipeline::drainThrough(TimeNs pts, std::chrono::milliseconds budget) {
    if (!vdec_ || !workerRunning_) return;

    // **Never wait for a picture the stream cannot contain.** A media element's
    // clock goes on running past the last frame — that is how `currentPts`
    // reports the end of a file at all — so a wait that chased it asked for a
    // picture at 11.6 s of a ten-second render and sat there until it gave up.
    // One of those is five seconds on the thread pumping a headless frame, and
    // it is exactly the frame a finished preview is on.
    //
    // Asking AT that last instant rather than past it is a different question
    // and `atEnd` is what carries it: "the last picture, and is there anything
    // after it?" A wait that stopped at the picture answered the first half
    // only, so `isEnded()` on the next line was still false — not because the
    // file goes on but because the worker had not yet been asked to find out.
    // Waiting for the EOF it is about to reach anyway is what makes settling at
    // the end of a file mean the end of the file.
    bool atEnd = false;
    if (duration_ > 0) {
        // `frameIntervalNs`, not `frameRate_` alone: a container that declares
        // no rate is exactly the case where this went wrong.
        const TimeNs frame = frameIntervalNs();
        const TimeNs last = duration_ > frame ? duration_ - frame : 0;
        if (pts >= last) {
            pts = last;
            atEnd = true;
        }
    }

    const auto deadline = std::chrono::steady_clock::now() + budget;
    if (pts >= 0) allowDecodeThrough(pts);

    while (true) {
        Picture incoming;
        while (decodedQueue_.tryPop(incoming)) stage(std::move(incoming));
        cvWorker_.notify_one();

        const bool seeking = pendingSeekPts_.load(std::memory_order_relaxed) >= 0 ||
                             workerSeeking_.load(std::memory_order_relaxed);
        if (!seeking) {
            const bool have =
                pts < 0 ? (cur_.valid || !staged_.empty())
                        : (!staged_.empty() ? staged_.back().pts >= pts
                                            : cur_.valid && cur_.pts >= pts);
            // Nothing more is coming: the answer is what has arrived. Checked
            // before `have` so that a wait at the end of the file ends on the
            // EOF rather than on the deadline.
            if (endOfStream_.load(std::memory_order_relaxed) &&
                decodedQueue_.sizeApprox() == 0)
                return;
            if (have && !atEnd) return;
        }
        if (std::chrono::steady_clock::now() > deadline) return;

        std::unique_lock<std::mutex> lock(mutex_);
        flushRequested_ = true;
        cvWorker_.notify_one();
        cvCaller_.wait_for(lock, std::chrono::milliseconds(2));
    }
}

void VideoPipeline::flush(TimeNs through) {
    if (!vdec_ || !workerRunning_) return;

    // Through whichever is further on: the instant the caller named, the seek
    // that has been asked for and not yet answered, and the position the clock
    // has reached. The clock is the half that matters headless — a run there
    // fast-forwards time in steps a real decode would take much longer than, so
    // waiting only for a seek left a playing element showing the frame it had
    // when the step began.
    TimeNs want = through;
    if (pendingTarget_ > want) want = pendingTarget_;
    if (clock_) {
        const TimeNs now = clock_->nowNs();
        if (now > want) want = now;
    }
    // A frame's worth of patience and no more. This is called once per headless
    // frame, so what it may cost is the price of a late picture — a deliberate
    // step or a caller that named an instant (`settleAt`) is what may wait for
    // seconds, and both of those are somebody asking rather than a clock ticking.
    drainThrough(want, kFrameWait);

    if (!cur_.valid && !staged_.empty()) {
        recycleCaller(std::move(cur_));
        cur_ = std::move(staged_.front());
        staged_.pop_front();
        pendingTarget_ = -1;
        rgbaStale_ = true;
        refreshRgba();
    }
}

/// A step is the one place a caller is allowed to wait for the worker, and it
/// has to be: the header's contract is that a step lands on a real decoded
/// picture and that the walk is exactly reversible, which cannot be answered
/// with "whatever has arrived". A deliberate keypress may take a few
/// milliseconds; a drag may not, and a drag goes through `seekTo`, which never
/// waits for anything.
bool VideoPipeline::stepFrame(int direction) {
    if (!source_ || !vdec_) return false;

    if (direction < 0) {
        if (!cur_.valid || cur_.pts <= 0) return false;
        const TimeNs was = cur_.pts;

        // Restart the demuxer a little BEFORE the picture on screen, then
        // decode forward and keep the last frame that is still earlier than
        // it. Comparing frames is exact — their timestamps are already in ns —
        // so the only question is where to restart from.
        //
        // Not one nanosecond before: a demuxer converts the target into the
        // container's own timebase, where one nanosecond is far below a tick,
        // and the rounding puts it back on the current frame. When that frame
        // is a keyframe the seek then lands on the frame we are trying to
        // leave, the step reports nothing to do, and stepping back stalls
        // there for good — which is exactly what a viewer sees as "it won't
        // go back past this point".
        //
        // A wider guard is never wrong, only slower: from wherever we land we
        // still decode forward to the frame just before `was`. So start at a
        // couple of frames and widen hard if the file disagrees.
        TimeNs guard = frameRate_ > 0.0
                           ? static_cast<TimeNs>(2e9 / frameRate_)
                           : 100 * 1000000LL;                     // 100 ms
        for (int attempt = 0; attempt < 6; ++attempt) {
            const TimeNs target = was > guard ? was - guard : 0;
            seekTo(target);
            // Through `was` and not merely "until something arrived": stopping
            // at the first candidate answers with whatever the worker had got
            // to, so a back step jumped several frames and sixty forward
            // followed by sixty back no longer came home.
            drainThrough(was, kWaitCeiling);
            // Everything the search does not want stays staged, which is what
            // makes the step after this one free — and, going the other way,
            // exactly reversible: the picture a forward step wants is the one
            // this call left at the front of the queue.
            presentNext(was - 1);

            if (cur_.valid && cur_.pts < was) {
                if (clock_) clock_->seekTo(cur_.pts);
                return true;
            }
            if (target == 0) break;
            guard *= 8;
        }
        return false;
    }

    if (direction == 0) return false;

    // Forward: whatever picture comes after the one on screen.
    const TimeNs current = cur_.valid ? cur_.pts : -1;
    drainThrough(current + 1, kWaitCeiling);
    while (!staged_.empty() && staged_.front().pts <= current) {
        recycleCaller(std::move(staged_.front()));
        staged_.pop_front();
    }
    if (staged_.empty()) return false;

    const TimeNs t = staged_.front().pts;
    if (clock_) clock_->seekTo(t);
    presentNext(t);
    return true;
}

VideoPipeline::Picture VideoPipeline::takePictureWorker() {
    Picture p;
    if (recycleQueue_.tryPop(p)) {
        p.valid = false;
        p.pts = -1;
        return p;
    }
    if (!workerPool_.empty()) {
        p = std::move(workerPool_.back());
        workerPool_.pop_back();
        return p;
    }
    return Picture{};
}

void VideoPipeline::recycleWorker(Picture&& p) {
    p.valid = false;
    p.pts = -1;
    if (workerPool_.size() < 32) workerPool_.push_back(std::move(p));
    p = Picture{};
}

void VideoPipeline::recycleCaller(Picture&& p) {
    if (p.valid) lastPts_ = p.pts;
    p.valid = false;
    p.pts = -1;
    if (p.w > 0 && p.h > 0) {
        recycleQueue_.tryPush(std::move(p));
    }
    p = Picture{};
}

void VideoPipeline::storeFrame(Picture& dst, const VideoFrame& frame) {
    const int w = static_cast<int>(frame.width);
    const int h = static_cast<int>(frame.height);
    if (w <= 0 || h <= 0) return;

    const int cw = (w + 1) / 2, ch = (h + 1) / 2;
    const size_t ySize = static_cast<size_t>(w) * h;
    const size_t cSize = static_cast<size_t>(cw) * ch;
    dst.yuv.resize(ySize + cSize * 2);
    dst.w = w;
    dst.h = h;
    dst.pts = frame.pts;
    dst.valid = true;

    uint8_t* dstY = dst.yuv.data();
    uint8_t* dstU = dstY + ySize;
    uint8_t* dstV = dstU + cSize;
    for (int row = 0; row < h; ++row)
        std::memcpy(dstY + static_cast<size_t>(row) * w,
                    frame.y + static_cast<size_t>(row) * frame.strideY, w);
    for (int row = 0; row < ch; ++row) {
        std::memcpy(dstU + static_cast<size_t>(row) * cw,
                    frame.u + static_cast<size_t>(row) * frame.strideU, cw);
        std::memcpy(dstV + static_cast<size_t>(row) * cw,
                    frame.v + static_cast<size_t>(row) * frame.strideV, cw);
    }
}

void VideoPipeline::refreshRgba() {
    if (!rgbaStale_ || !cur_.valid || cur_.w <= 0 || cur_.h <= 0) return;
    rgbaStale_ = false;

    frameW_ = cur_.w;
    frameH_ = cur_.h;
    const int cw = (cur_.w + 1) / 2;
    const size_t ySize = static_cast<size_t>(cur_.w) * cur_.h;
    const size_t cSize = static_cast<size_t>(cw) * ((cur_.h + 1) / 2);
    if (cur_.yuv.size() < ySize + cSize * 2) return;

    rgba_.resize(ySize * 4);
    const uint8_t* y = cur_.yuv.data();
    i420ToRgba(y, y + ySize, y + ySize + cSize,
               cur_.w, cw, cw, cur_.w, cur_.h, rgba_.data(), cur_.w * 4);
}

void VideoPipeline::performWorkerSeek(TimeNs target) {
    if (!source_ || !vdec_) return;

    workerSeeking_ = true;
    workerInSeek_ = true;

    Picture dummy;
    while (decodedQueue_.tryPop(dummy)) {
        recycleWorker(std::move(dummy));
    }
    while (!workerStaged_.empty()) {
        recycleWorker(std::move(workerStaged_.front()));
        workerStaged_.pop_front();
    }

    source_->seekTo(target);
    if (vdec_) vdec_->flush();
    if (adec_) adec_->flush();
    drained_ = false;
    endOfStream_ = false;
    workerNewestPts_ = -1;   // the pictures after this one are a new position

    const TimeNs targetUpper = target + 100 * 1000000LL;
    const TimeNs pruneBeforePts = target > 200 * 1000000LL ? target - 200 * 1000000LL : 0;

    bool reachedTarget = false;
    while (!stopWorker_.load(std::memory_order_relaxed)) {
        if (pendingSeekPts_.load(std::memory_order_relaxed) >= 0) {
            workerInSeek_ = false;
            workerSeeking_ = false;
            return;
        }

        while (!workerStaged_.empty()) {
            Picture p = std::move(workerStaged_.front());
            workerStaged_.pop_front();

            if (p.pts >= targetUpper) {
                reachedTarget = true;
            }

            if (p.pts < pruneBeforePts && !reachedTarget) {
                recycleWorker(std::move(p));
                continue;
            }

            if (!decodedQueue_.tryPush(std::move(p))) {
                workerStaged_.push_front(std::move(p));
                break;
            }
        }

        if (reachedTarget) break;

        if (!workerStaged_.empty()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }

        bool changed = false;
        if (!pumpOneWorker(&changed)) break;
    }

    workerInSeek_ = false;
    workerSeeking_ = false;
    cvCaller_.notify_all();
}

bool VideoPipeline::collectFramesWorker() {
    if (!vdec_) return false;
    VideoFrame frame;
    bool collectedAny = false;
    while (vdec_->nextFrame(frame)) {
        Picture p = takePictureWorker();
        storeFrame(p, frame);
        // During a seek every picture goes through the staging deque, because
        // that is where the seek decides whether it is still behind its target
        // (throw it away) or past it (stop). Pushing into the ring here instead
        // meant a seek only ever examined the pictures the ring had no room
        // for, so it neither pruned nor stopped and simply filled 32 slots
        // from wherever the keyframe was.
        if (p.pts > workerNewestPts_) workerNewestPts_ = p.pts;
        if (workerInSeek_ || !decodedQueue_.tryPush(std::move(p))) {
            workerStaged_.push_back(std::move(p));
        }
        collectedAny = true;
    }
    return collectedAny;
}

bool VideoPipeline::pumpOneWorker(bool* changed) {
    if (!source_) return false;
    MediaPacket pkt;
    if (!source_->readPacket(pkt)) {
        if (!drained_) {
            drained_ = true;
            if (vdec_) vdec_->drain();
            if (collectFramesWorker() && changed) *changed = true;
            return true;
        }
        endOfStream_ = true;
        return false;
    }
    if (pkt.trackId != videoTrackId_) return true;
    if (vdec_ && vdec_->decode(pkt)) {
        if (collectFramesWorker() && changed) *changed = true;
    }
    return true;
}

void VideoPipeline::workerLoop() {
    while (!stopWorker_.load(std::memory_order_relaxed)) {
        if (flushRequested_.exchange(false, std::memory_order_relaxed)) {
            cvCaller_.notify_all();
        }

        int64_t seekTarget = pendingSeekPts_.exchange(-1);
        if (seekTarget >= 0) {
            performWorkerSeek(static_cast<TimeNs>(seekTarget));
            cvCaller_.notify_all();
            continue;
        }

        Picture recycled;
        while (recycleQueue_.tryPop(recycled)) {
            if (workerPool_.size() < 16) {
                workerPool_.push_back(std::move(recycled));
            }
        }

        // Moved out of the deque only once the ring has accepted it: taking it
        // out first and putting it back on failure is what dropped a picture
        // every time the ring was full, since the copy left behind is a husk.
        while (!workerStaged_.empty()) {
            if (!decodedQueue_.tryPush(std::move(workerStaged_.front()))) break;
            workerStaged_.pop_front();
        }

        // Ahead of what has been asked for is a reason to stop, exactly as a
        // full ring is: see `decodeCeiling_` for what reads that as a signal.
        const bool aheadEnough =
            workerNewestPts_ >= 0 &&
            workerNewestPts_ > decodeCeiling_.load(std::memory_order_relaxed);

        if (workerStaged_.empty() && !aheadEnough &&
            decodedQueue_.sizeApprox() < decodedQueue_.capacity() &&
            !endOfStream_.load(std::memory_order_relaxed)) {
            bool changed = false;
            pumpOneWorker(&changed);
            cvCaller_.notify_all();
        } else {
            std::unique_lock<std::mutex> lock(mutex_);
            workerIdle_ = true;
            cvCaller_.notify_all();
            if (!stopWorker_.load(std::memory_order_relaxed) &&
                pendingSeekPts_.load(std::memory_order_relaxed) == -1 &&
                !flushRequested_.load(std::memory_order_relaxed)) {
                // The caller notifies on every advance, seek and wait, so this
                // is only the backstop — and it is a per-element poll, on a
                // timeline that holds one element per clip near the playhead.
                cvWorker_.wait_for(lock, std::chrono::milliseconds(5));
            }
            workerIdle_ = false;
        }
    }
}

/// `advanceTo` with the end-of-stream tail rule off: exactly the pictures whose
/// moment has come and no more. A step moves by one picture whether or not the
/// file has ended, which is the difference between walking to the last frame and
/// jumping to it.
bool VideoPipeline::presentNext(TimeNs at) {
    presentTail_ = false;
    const bool changed = advanceTo(at);
    presentTail_ = true;
    return changed;
}

bool VideoPipeline::settleAt(TimeNs nowNs) {
    // `kWaitCeiling` and not the frame budget: this is somebody naming an
    // instant and saying they want the picture at it, which is a question that
    // may cost a whole GOP to answer and is worth what it costs.
    drainThrough(nowNs, kWaitCeiling);
    return advanceTo(nowNs);
}

bool VideoPipeline::advance() {
    if (!clock_) return false;
    return advanceTo(clock_->nowNs());
}

bool VideoPipeline::advanceTo(TimeNs nowNs) {
    if (!source_) return false;
    if (!vdec_) {
        soundPos_ = nowNs < 0 ? 0 : nowNs;
        if (duration_ > 0 && soundPos_ >= duration_) {
            soundPos_ = duration_;
            drained_ = true;
            endOfStream_ = true;
        } else {
            drained_ = false;
            endOfStream_ = false;
        }
        return false;
    }

    // Which moment the screen has reached, which is the whole of what licenses
    // the worker to decode any further. See `decodeCeiling_`.
    allowDecodeThrough(nowNs);

    Picture incoming;
    while (decodedQueue_.tryPop(incoming)) {
        stage(std::move(incoming));
    }
    cvWorker_.notify_one();

    bool changed = false;

    // **Once the source is over, what is left staged is due.** Holding those
    // back until the clock reaches them assumes a clock that goes on moving,
    // and an audio-slaved one stops when the sound does — the sound of a file
    // stops with the file, so the last picture and the clock ended up each
    // waiting for the other and the element never reported `ended`. Measured
    // on a 1.5 s render: the picture stopped at 1.40 s with 1.48 s decoded and
    // staged, and playback never finished. `tail` is deliberately not what a
    // frame step uses (see `presentNext`): stepping is one picture at a time
    // whether or not the file has ended.
    const bool tail = endOfStream_.load(std::memory_order_relaxed) &&
                      decodedQueue_.sizeApprox() == 0 && presentTail_;

    // Otherwise one rule, and it is the seek contract as well: the picture shown
    // at an instant is the last one at or before it. A seek is `seekTo(t)`
    // followed by an advance to the same `t`, so there is nothing here that
    // knows a seek happened — a second branch for one landed on the first frame
    // at or AFTER the target instead, which is a whole frame late everywhere a
    // suite compares two pictures.
    while (!staged_.empty() && (tail || staged_.front().pts <= nowNs)) {
        recycleCaller(std::move(cur_));
        cur_ = std::move(staged_.front());
        staged_.pop_front();
        rgbaStale_ = true;
        changed = true;
    }

    if (!cur_.valid && !staged_.empty()) {
        recycleCaller(std::move(cur_));
        cur_ = std::move(staged_.front());
        staged_.pop_front();
        rgbaStale_ = true;
        changed = true;
    }

    if (changed) {
        pendingTarget_ = -1;   // a picture has been placed; `cur_` is the answer
        refreshRgba();
    }
    return changed;
}

} // namespace bro::video
