#include "video/webm_demuxer.h"

#include <mkvparser/mkvparser.h>
#include <mkvparser/mkvreader.h>

#include <cmath>
#include <cstring>

namespace bro::video {

namespace {

Codec codecFromWebmId(const char* id) {
    if (!id) return Codec::Unknown;
    if (std::strcmp(id, "V_VP9") == 0) return Codec::VP9;
    if (std::strcmp(id, "V_VP8") == 0) return Codec::VP8;
    if (std::strcmp(id, "A_OPUS") == 0) return Codec::Opus;
    if (std::strcmp(id, "A_VORBIS") == 0) return Codec::Vorbis;
    return Codec::Unknown;
}

// Matroska keeps a rotation in the Video Projection element's ProjectionPoseRoll:
// a clockwise roll about the forward vector, which is exactly how far the
// decoded picture has to be turned clockwise to be the right way up. Only a
// rectangular projection with no yaw and no pitch means "this picture is
// sideways" — anything else is a spherical or cube-mapped video, which is not
// a rotation at all and must not be read as one.
//
// Only quarter turns are taken. A container may say 89.9 (the field is a
// float) and a size can only be swapped or not, so anything off a quarter turn
// is reported as no rotation rather than as an angle nothing can honour.
//
// "Absent" is spelt as FLT_MAX by libwebm, but its kValueNotPresent constant
// is not exported by the vcpkg unofficial-libwebm build and referencing it
// fails to link — the same thing the encoder hit with the codec-id literals.
// A finite angle inside a plausible range is the same question asked of the
// value itself, and it does not depend on a symbol being exported.
int rotationFromProjection(const mkvparser::Projection* p) {
    if (!p || p->type != mkvparser::Projection::kRectangular) return 0;
    auto stated = [](float v) { return std::isfinite(v) && std::fabs(v) < 1.0e6f; };
    if (stated(p->pose_yaw)   && p->pose_yaw   != 0.0f) return 0;
    if (stated(p->pose_pitch) && p->pose_pitch != 0.0f) return 0;
    if (!stated(p->pose_roll)) return 0;

    const float roll = p->pose_roll;
    const int quarter = static_cast<int>(roll < 0 ? roll / 90.0f - 0.5f
                                                  : roll / 90.0f + 0.5f);
    if (roll - quarter * 90.0f > 0.5f || roll - quarter * 90.0f < -0.5f) return 0;
    return ((quarter % 4) + 4) % 4 * 90;
}

} // namespace

WebMDemuxer::WebMDemuxer() = default;
WebMDemuxer::~WebMDemuxer() = default;

bool WebMDemuxer::open(const std::string& path) {
    reader_ = std::make_unique<mkvparser::MkvReader>();
    if (reader_->Open(path.c_str()) != 0) {
        lastError_ = "failed to open file: " + path;
        reader_.reset();
        return false;
    }

    long long pos = 0;
    mkvparser::EBMLHeader ebml;
    if (ebml.Parse(reader_.get(), pos) < 0) {
        lastError_ = "not a valid EBML file";
        return false;
    }

    mkvparser::Segment* rawSegment = nullptr;
    if (mkvparser::Segment::CreateInstance(reader_.get(), pos, rawSegment) < 0) {
        lastError_ = "failed to create segment";
        return false;
    }
    segment_.reset(rawSegment);

    if (segment_->Load() < 0) {
        lastError_ = "failed to load segment";
        return false;
    }

    const mkvparser::Tracks* trackList = segment_->GetTracks();
    if (!trackList) {
        lastError_ = "no tracks in segment";
        return false;
    }

    const mkvparser::SegmentInfo* info = segment_->GetInfo();
    const TimeNs durationNs = info ? info->GetDuration() : 0;

    for (unsigned long i = 0; i < trackList->GetTracksCount(); ++i) {
        const mkvparser::Track* track = trackList->GetTrackByIndex(i);
        if (!track) continue;

        TrackInfo ti;
        ti.id = static_cast<uint32_t>(track->GetNumber());
        ti.codec = codecFromWebmId(track->GetCodecId());
        ti.durationNs = durationNs;

        if (track->GetType() == mkvparser::Track::kVideo) {
            const auto* vt = static_cast<const mkvparser::VideoTrack*>(track);
            ti.kind = TrackKind::Video;
            ti.width = static_cast<uint32_t>(vt->GetWidth());
            ti.height = static_cast<uint32_t>(vt->GetHeight());
            ti.rotationDegrees = rotationFromProjection(vt->GetProjection());
        } else if (track->GetType() == mkvparser::Track::kAudio) {
            const auto* at = static_cast<const mkvparser::AudioTrack*>(track);
            ti.kind = TrackKind::Audio;
            ti.sampleRate = static_cast<uint32_t>(at->GetSamplingRate());
            ti.channels = static_cast<uint32_t>(at->GetChannels());
        } else {
            continue;
        }

        size_t cpSize = 0;
        const unsigned char* cp = track->GetCodecPrivate(cpSize);
        if (cp && cpSize > 0) {
            ti.codecPrivate.assign(cp, cp + cpSize);
        }

        tracks_.push_back(std::move(ti));
    }

    cluster_ = segment_->GetFirst();
    return true;
}

bool WebMDemuxer::advanceToNextBlock() {
    while (cluster_ && !cluster_->EOS()) {
        if (!blockEntry_) {
            if (cluster_->GetFirst(blockEntry_) < 0 || !blockEntry_) {
                cluster_ = segment_->GetNext(cluster_);
                continue;
            }
            blockFrameIndex_ = 0;
        }

        if (blockEntry_ && !blockEntry_->EOS()) {
            const mkvparser::Block* block = blockEntry_->GetBlock();
            if (block && blockFrameIndex_ < block->GetFrameCount()) {
                return true;
            }
        }

        // Exhausted this entry — advance.
        if (cluster_->GetNext(blockEntry_, blockEntry_) < 0 || !blockEntry_ || blockEntry_->EOS()) {
            cluster_ = segment_->GetNext(cluster_);
            blockEntry_ = nullptr;
            continue;
        }
        blockFrameIndex_ = 0;
    }
    return false;
}

bool WebMDemuxer::readPacket(MediaPacket& out) {
    if (!segment_) return false;
    if (!advanceToNextBlock()) return false;

    const mkvparser::Block* block = blockEntry_->GetBlock();
    const mkvparser::Block::Frame& frame = block->GetFrame(blockFrameIndex_);

    auto buf = std::make_shared<std::vector<uint8_t>>(frame.len);
    if (frame.Read(reader_.get(), buf->data()) != 0) {
        lastError_ = "frame read failed";
        return false;
    }

    out.trackId = static_cast<uint32_t>(block->GetTrackNumber());
    out.keyframe = block->IsKey();
    out.pts = block->GetTime(cluster_);  // already ns
    out.duration = 0;
    out.data = std::move(buf);

    // Resolve codec/kind from the track list.
    for (const auto& t : tracks_) {
        if (t.id == out.trackId) {
            out.codec = t.codec;
            out.kind = t.kind;
            break;
        }
    }

    ++blockFrameIndex_;
    return true;
}

bool WebMDemuxer::seekTo(TimeNs pts) {
    if (!segment_) return false;

    // Find the first video track for cue-point lookup; fall back to any track.
    const mkvparser::Tracks* trackList = segment_->GetTracks();
    if (!trackList) return false;

    const mkvparser::Track* seekTrack = nullptr;
    for (unsigned long i = 0; i < trackList->GetTracksCount(); ++i) {
        const mkvparser::Track* t = trackList->GetTrackByIndex(i);
        if (t && t->GetType() == mkvparser::Track::kVideo) { seekTrack = t; break; }
    }
    if (!seekTrack) seekTrack = trackList->GetTrackByIndex(0);
    if (!seekTrack) return false;

    const mkvparser::Cues* cues = segment_->GetCues();
    if (cues) {
        while (!cues->DoneParsing()) cues->LoadCuePoint();
        const mkvparser::CuePoint* cp = nullptr;
        const mkvparser::CuePoint::TrackPosition* tp = nullptr;
        if (cues->Find(pts, seekTrack, cp, tp) && cp && tp) {
            cluster_ = segment_->FindOrPreloadCluster(tp->m_pos);
            blockEntry_ = nullptr;
            blockFrameIndex_ = 0;
            return cluster_ != nullptr;
        }
    }

    // No cues — fall back to linear scan from start.
    cluster_ = segment_->GetFirst();
    blockEntry_ = nullptr;
    blockFrameIndex_ = 0;
    return true;
}

} // namespace bro::video
