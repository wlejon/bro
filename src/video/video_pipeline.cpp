#include "video/video_pipeline.h"

#include "video/media_backend.h"
#include "video/yuv_to_rgb.h"

#include "util/log.h"

#include <chrono>
#include <cstring>

namespace bro::video {

static constexpr TimeNs kForwardNudgeNs = 500 * 1000000LL;   // 0.5 s

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
    if (cur_.valid) return cur_.pts;
    const int64_t pending = pendingSeekPts_.load(std::memory_order_relaxed);
    if (pending >= 0) return static_cast<TimeNs>(pending);
    return 0;
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
    if (!vdec_) {
        if (clock_) clock_->seekTo(pts);
        advanceTo(pts);
        return;
    }

    while (!staged_.empty()) {
        recycleCaller(std::move(staged_.front()));
        staged_.pop_front();
    }

    pendingSeekPts_ = pts;
    cvWorker_.notify_one();
    if (clock_) clock_->seekTo(pts);
}

void VideoPipeline::flush() {
    if (!vdec_ || !workerRunning_) return;

    flushRequested_ = true;
    cvWorker_.notify_one();

    std::unique_lock<std::mutex> lock(mutex_);
    cvCaller_.wait_for(lock, std::chrono::milliseconds(5000), [this] {
        return pendingSeekPts_.load(std::memory_order_relaxed) == -1 &&
               !workerSeeking_.load(std::memory_order_relaxed) &&
               (workerIdle_.load(std::memory_order_relaxed) || decodedQueue_.sizeApprox() > 0 || endOfStream_.load(std::memory_order_relaxed));
    });

    Picture incoming;
    while (decodedQueue_.tryPop(incoming)) {
        staged_.push_back(std::move(incoming));
    }
    cvWorker_.notify_one();

    if (!cur_.valid && !staged_.empty()) {
        recycleCaller(std::move(cur_));
        cur_ = std::move(staged_.front());
        staged_.pop_front();
        rgbaStale_ = true;
        refreshRgba();
    }
}

bool VideoPipeline::stepFrame(int direction) {
    if (!source_ || !vdec_) return false;

    if (direction < 0) {
        if (!cur_.valid || cur_.pts <= 0) return false;
        const TimeNs was = cur_.pts;

        while (!staged_.empty()) {
            recycleCaller(std::move(staged_.front()));
            staged_.pop_front();
        }

        TimeNs guard = frameRate_ > 0.0
                           ? static_cast<TimeNs>(1.5e9 / frameRate_)
                           : 50 * 1000000LL;
        for (int attempt = 0; attempt < 6; ++attempt) {
            const TimeNs target = was > guard ? was - guard : 0;
            seekTo(target);
            flush();

            Picture best;
            while (true) {
                Picture incoming;
                while (decodedQueue_.tryPop(incoming)) {
                    staged_.push_back(std::move(incoming));
                }
                while (!staged_.empty() && staged_.front().pts < was) {
                    best = std::move(staged_.front());
                    staged_.pop_front();
                }
                if (best.valid) break;
                if (isEnded() || target == 0) break;
                flush();
            }

            if (best.valid && best.pts < was) {
                while (!staged_.empty()) {
                    recycleCaller(std::move(staged_.front()));
                    staged_.pop_front();
                }
                recycleCaller(std::move(cur_));
                cur_ = std::move(best);
                rgbaStale_ = true;
                refreshRgba();
                if (clock_) clock_->seekTo(cur_.pts);
                return true;
            }

            if (target == 0) break;
            guard *= 8;
        }
        return false;
    }

    if (direction == 0) return false;

    const TimeNs current = cur_.valid ? cur_.pts : -1;

    Picture incoming;
    while (decodedQueue_.tryPop(incoming)) {
        staged_.push_back(std::move(incoming));
    }

    while (!staged_.empty() && staged_.front().pts <= current) {
        staged_.pop_front();
    }

    int retries = 0;
    while (staged_.empty() && !isEnded() && retries < 50) {
        flush();
        while (decodedQueue_.tryPop(incoming)) {
            staged_.push_back(std::move(incoming));
        }
        while (!staged_.empty() && staged_.front().pts <= current) {
            staged_.pop_front();
        }
        if (!staged_.empty()) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        retries++;
    }

    if (staged_.empty()) return false;

    recycleCaller(std::move(cur_));
    cur_ = std::move(staged_.front());
    staged_.pop_front();
    rgbaStale_ = true;
    refreshRgba();
    if (clock_) clock_->seekTo(cur_.pts);
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

    const TimeNs targetUpper = target + 100 * 1000000LL;
    const TimeNs pruneBeforePts = target > 200 * 1000000LL ? target - 200 * 1000000LL : 0;

    bool reachedTarget = false;
    while (!stopWorker_.load(std::memory_order_relaxed)) {
        if (pendingSeekPts_.load(std::memory_order_relaxed) >= 0) {
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

    workerSeeking_ = false;
    cvCaller_.notify_all();
}

bool VideoPipeline::collectFramesWorker() {
    VideoFrame frame;
    bool collectedAny = false;
    while (vdec_->nextFrame(frame)) {
        Picture p = takePictureWorker();
        storeFrame(p, frame);
        if (!decodedQueue_.tryPush(std::move(p))) {
            workerStaged_.push_back(std::move(p));
        }
        collectedAny = true;
    }
    return collectedAny;
}

bool VideoPipeline::pumpOneWorker(bool* changed) {
    MediaPacket pkt;
    if (!source_->readPacket(pkt)) {
        if (!drained_) {
            drained_ = true;
            vdec_->drain();
            if (collectFramesWorker() && changed) *changed = true;
            return true;
        }
        endOfStream_ = true;
        return false;
    }
    if (pkt.trackId != videoTrackId_) return true;
    if (vdec_->decode(pkt)) {
        if (collectFramesWorker() && changed) *changed = true;
    }
    return true;
}

void VideoPipeline::workerLoop() {
    while (!stopWorker_.load(std::memory_order_relaxed)) {
        int64_t seekTarget = pendingSeekPts_.exchange(-1);
        if (seekTarget >= 0) {
            performWorkerSeek(static_cast<TimeNs>(seekTarget));
            continue;
        }

        Picture recycled;
        while (recycleQueue_.tryPop(recycled)) {
            if (workerPool_.size() < 16) {
                workerPool_.push_back(std::move(recycled));
            }
        }

        while (!workerStaged_.empty()) {
            Picture p = std::move(workerStaged_.front());
            if (decodedQueue_.tryPush(std::move(p))) {
                workerStaged_.pop_front();
            } else {
                break;
            }
        }

        if (workerStaged_.empty() &&
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
                cvWorker_.wait_for(lock, std::chrono::milliseconds(5));
            }
            workerIdle_ = false;
            flushRequested_ = false;
        }
    }
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

    Picture incoming;
    while (decodedQueue_.tryPop(incoming)) {
        staged_.push_back(std::move(incoming));
    }
    cvWorker_.notify_one();

    bool changed = false;

    while (!staged_.empty() && staged_.front().pts <= nowNs) {
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

    if (changed) refreshRgba();
    return changed;
}

} // namespace bro::video
