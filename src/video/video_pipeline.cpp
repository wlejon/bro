#include "video/video_pipeline.h"

#include "video/media_backend.h"
#include "video/yuv_to_rgb.h"

#include "util/log.h"

namespace bro::video {

VideoPipeline::VideoPipeline() {
    clock_ = std::make_unique<FileClock>();
}

VideoPipeline::~VideoPipeline() = default;

// Try one backend against an already-opened source. Returns false if the
// file has no video track this backend can decode, leaving the pipeline
// clean for the next candidate.
bool VideoPipeline::adoptSource(const MediaBackend& backend,
                                std::unique_ptr<MediaSource> source) {
    videoTrackId_ = 0;
    audioTrackId_ = 0;
    audioRate_ = 0;
    audioChannels_ = 0;
    duration_ = 0;
    frameRate_ = 0.0;
    frameW_ = 0;
    frameH_ = 0;
    vdec_.reset();
    adec_.reset();

    for (const auto& t : source->tracks()) {
        if (t.kind == TrackKind::Video && videoTrackId_ == 0) {
            auto dec = backend.makeVideoDecoder ? backend.makeVideoDecoder(t) : nullptr;
            if (!dec) continue;           // unsupported codec: keep looking
            vdec_ = std::move(dec);
            videoTrackId_ = t.id;
            frameW_ = static_cast<int>(t.width);
            frameH_ = static_cast<int>(t.height);
            duration_ = t.durationNs;
            frameRate_ = t.frameRate;
        } else if (t.kind == TrackKind::Audio && audioTrackId_ == 0) {
            // Audio is optional: a file whose audio codec this backend can't
            // handle still plays, silently.
            auto dec = backend.makeAudioDecoder ? backend.makeAudioDecoder(t) : nullptr;
            if (!dec) continue;
            adec_ = std::move(dec);
            audioTrackId_ = t.id;
            audioRate_ = t.sampleRate;
            audioChannels_ = t.channels;
        }
    }

    if (!vdec_) {
        adec_.reset();
        return false;
    }
    // This source pumps video only — audio plays from a second demuxer (see
    // ElVideo). Asking for just the video track stops us reading and copying
    // audio packets we immediately drop.
    source->setActiveTracks({videoTrackId_});
    source_ = std::move(source);
    return true;
}

bool VideoPipeline::open(const std::string& path) {
    // Highest-priority backend that both recognises the container and can
    // decode a video track inside it wins. A host application registering an
    // ffmpeg backend therefore takes over transparently, and with none
    // registered this is exactly the old WebM/VP9/Opus path.
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

void VideoPipeline::play() { if (clock_) clock_->setPlaying(true); }
void VideoPipeline::pause() { if (clock_) clock_->setPlaying(false); }
bool VideoPipeline::isPlaying() const { return clock_ && clock_->isPlaying(); }

void VideoPipeline::setRate(double rate) {
    if (clock_) clock_->setRate(rate);
}

// A forward seek shorter than this decodes on from where we are instead of
// restarting the demuxer. A demuxer seek lands on the keyframe at or BEFORE
// the target — which for a target just ahead of the playhead is usually
// BEHIND the playhead, so it decodes a whole GOP to reach a picture that was
// only a few frames away. Frame stepping is the extreme case: one frame
// forward cost an entire GOP.
static constexpr TimeNs kForwardNudgeNs = 500 * 1000000LL;   // 0.5 s

void VideoPipeline::seekTo(TimeNs pts) {
    if (!source_) return;

    const bool nudgeForward = cur_.valid && cur_.pts >= 0 && pts >= cur_.pts &&
                              (pts - cur_.pts) <= kForwardNudgeNs;
    if (!nudgeForward) restartAt(pts);
    if (clock_) clock_->seekTo(pts);
    advanceTo(pts);
}

// Point the demuxer at the keyframe at or before `target` and throw away
// everything decoded from the old position — frames still in flight belong
// there, not here.
void VideoPipeline::restartAt(TimeNs target) {
    source_->seekTo(target);
    if (vdec_) vdec_->flush();
    if (adec_) adec_->flush();
    while (!staged_.empty()) { recycle(std::move(staged_.front())); staged_.pop_front(); }
    recycle(std::move(cur_));
    endOfStream_ = false;
}

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
            restartAt(target);
            advanceTo(was - 1);

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

    // Forward: whatever picture comes after the one on screen. Decode until
    // there is one — the staged queue already holds it during playback.
    while (staged_.empty() && !endOfStream_) pumpOne(cur_.pts, nullptr);
    if (staged_.empty()) return false;

    const TimeNs t = staged_.front().pts;
    if (clock_) clock_->seekTo(t);
    advanceTo(t);
    return true;
}

VideoPipeline::Picture VideoPipeline::takePicture() {
    if (pool_.empty()) return Picture{};
    Picture p = std::move(pool_.back());
    pool_.pop_back();
    return p;
}

void VideoPipeline::recycle(Picture&& p) {
    p.valid = false;
    p.pts = -1;
    if (pool_.size() < 4) pool_.push_back(std::move(p));
    p = Picture{};
}

// Copy a decoded frame's planes into a picture with tight strides.
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

    rgba_.resize(ySize * 4);
    const uint8_t* y = cur_.yuv.data();
    i420ToRgba(y, y + ySize, y + ySize + cSize,
               cur_.w, cw, cw, cur_.w, cur_.h, rgba_.data(), cur_.w * 4);
}

bool VideoPipeline::decodePacket(const MediaPacket& pkt, TimeNs nowNs) {
    if (pkt.trackId != videoTrackId_ || !vdec_) {
        // Audio packets are dropped here — audio playback is driven by a
        // separate demuxer instance (see ElVideo::openAudioTrack), which
        // predecodes the whole audio track into one clip up front rather than
        // pumping packets in lockstep with video. audioDecoder() is exposed
        // for callers that want to decode Opus packets themselves.
        return false;
    }
    if (!vdec_->decode(pkt)) return false;

    VideoFrame frame;
    bool changed = false;
    while (vdec_->nextFrame(frame)) {
        // Keep the picture, don't convert it. A seek walks the whole GOP to
        // reach one frame; converting all of them cost 3.3 ms each and was
        // the entire reason scrubbing felt broken.
        //
        // The FRAME's pts, not the packet's. A codec with B-frames emits
        // pictures in a different order than the packets that carried them,
        // so the frame coming out of decode() is generally not the one the
        // packet just fed in. VP8/VP9 don't reorder, which is why the packet
        // pts was indistinguishable until now.
        if (cur_.valid && frame.pts > nowNs) {
            Picture p = takePicture();
            storeFrame(p, frame);
            staged_.push_back(std::move(p));
        } else {
            Picture p = takePicture();
            storeFrame(p, frame);
            recycle(std::move(cur_));
            cur_ = std::move(p);
            rgbaStale_ = true;
            changed = true;
        }
    }
    return changed;
}

bool VideoPipeline::pumpOne(TimeNs nowNs, bool* changed) {
    MediaPacket pkt;
    if (!source_->readPacket(pkt)) {
        endOfStream_ = true;
        return false;
    }
    if (pkt.trackId != videoTrackId_) return true;   // not ours; keep going
    if (decodePacket(pkt, nowNs) && changed) *changed = true;
    return true;
}

bool VideoPipeline::advance() {
    if (!clock_) return false;
    return advanceTo(clock_->nowNs());
}

bool VideoPipeline::advanceTo(TimeNs nowNs) {
    if (!source_ || !vdec_) return false;
    bool changed = false;

    // A staged picture becomes the displayed one as soon as its time comes.
    while (!staged_.empty() && staged_.front().pts <= nowNs) {
        recycle(std::move(cur_));
        cur_ = std::move(staged_.front());
        staged_.pop_front();
        rgbaStale_ = true;
        changed = true;
    }

    // Then decode until something is staged — proof that the picture now on
    // screen really is the one `now` falls inside, rather than merely the
    // newest we happen to have decoded.
    while (staged_.empty() && !endOfStream_) {
        if (!pumpOne(nowNs, &changed)) break;
    }

    // One colour conversion per advance, however many frames were decoded to
    // get here.
    if (changed) refreshRgba();
    return changed;
}

} // namespace bro::video
