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

void VideoPipeline::seekTo(TimeNs pts) {
    if (!source_) return;
    source_->seekTo(pts);
    if (clock_) clock_->seekTo(pts);
    pendingVideoValid_ = false;
    endOfStream_ = false;
    hasFrame_ = false;
    currentPts_ = -1;
    advanceTo(pts);
}

bool VideoPipeline::decodePacket(const MediaPacket& pkt) {
    if (pkt.trackId == videoTrackId_ && vdec_) {
        if (!vdec_->decode(pkt)) return false;
        VideoFrame frame;
        bool got = false;
        while (vdec_->nextFrame(frame)) {
            frameW_ = static_cast<int>(frame.width);
            frameH_ = static_cast<int>(frame.height);
            rgba_.resize(static_cast<size_t>(frameW_) * frameH_ * 4);
            i420ToRgba(frame.y, frame.u, frame.v,
                       frame.strideY, frame.strideU, frame.strideV,
                       frameW_, frameH_, rgba_.data(), frameW_ * 4);
            currentPts_ = pkt.pts;
            hasFrame_ = true;
            got = true;
        }
        return got;
    }
    // Audio packets are dropped here — audio playback is driven by a
    // separate demuxer instance (see ElVideo::openAudioTrack), which
    // predecodes the whole audio track into one clip up front rather than
    // pumping packets in lockstep with video. audioDecoder() is exposed
    // for callers that want to decode Opus packets themselves.
    return false;
}

bool VideoPipeline::advance() {
    if (!clock_) return false;
    return advanceTo(clock_->nowNs());
}

bool VideoPipeline::advanceTo(TimeNs nowNs) {
    if (!source_ || !vdec_) return false;
    bool changed = false;

    // Walk packets until we've consumed anything with pts <= now. The
    // pending slot holds a demuxed-but-not-yet-due packet across calls.
    while (!endOfStream_) {
        MediaPacket pkt;
        bool have = false;
        if (pendingVideoValid_) {
            pkt = std::move(pendingVideo_);
            pendingVideoValid_ = false;
            have = true;
        } else {
            have = source_->readPacket(pkt);
            if (!have) { endOfStream_ = true; break; }
        }

        if (pkt.trackId != videoTrackId_) {
            // Audio packets on this source are ignored — see decodePacket().
            continue;
        }

        if (pkt.pts > nowNs && hasFrame_) {
            // Stash and wait until time catches up.
            pendingVideo_ = std::move(pkt);
            pendingVideoValid_ = true;
            break;
        }

        if (decodePacket(pkt)) changed = true;
    }

    return changed;
}

} // namespace bro::video
