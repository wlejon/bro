#include "video/video_pipeline.h"

#include "video/webm_demuxer.h"
#include "video/yuv_to_rgb.h"

namespace bro::video {

VideoPipeline::VideoPipeline() {
    clock_ = std::make_unique<FileClock>();
}

VideoPipeline::~VideoPipeline() = default;

bool VideoPipeline::open(const std::string& path) {
    auto demux = std::make_unique<WebMDemuxer>();
    if (!demux->open(path)) return false;

    Codec videoCodec = Codec::Unknown;
    for (const auto& t : demux->tracks()) {
        if (t.kind == TrackKind::Video && videoTrackId_ == 0) {
            videoTrackId_ = t.id;
            videoCodec = t.codec;
            frameW_ = static_cast<int>(t.width);
            frameH_ = static_cast<int>(t.height);
            duration_ = t.durationNs;
        } else if (t.kind == TrackKind::Audio && audioTrackId_ == 0) {
            audioTrackId_ = t.id;
            audioRate_ = t.sampleRate;
            audioChannels_ = t.channels;
            adec_ = createOpusDecoder(audioRate_, audioChannels_);
        }
    }

    if (videoTrackId_ == 0 ||
        (videoCodec != Codec::VP9 && videoCodec != Codec::VP8)) {
        return false;
    }

    vdec_ = createVpxDecoder(videoCodec, /*lowLatency=*/false);
    source_ = std::move(demux);
    return true;
}

void VideoPipeline::play() { if (clock_) clock_->setPlaying(true); }
void VideoPipeline::pause() { if (clock_) clock_->setPlaying(false); }
bool VideoPipeline::isPlaying() const { return clock_ && clock_->isPlaying(); }

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
    // Audio packets are dropped here; calling code pulls them via
    // audioDecoder() + a separate demux pump when the audio pipeline
    // lands. This isolates the video-first integration.
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
            // Non-video packets not handled in this first integration.
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
