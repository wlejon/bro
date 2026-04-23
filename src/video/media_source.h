#pragma once

#include "video/media_packet.h"
#include <string>
#include <vector>

namespace bro::video {

struct TrackInfo {
    uint32_t id = 0;
    TrackKind kind = TrackKind::Video;
    Codec codec = Codec::Unknown;

    // Video
    uint32_t width = 0;
    uint32_t height = 0;

    // Audio
    uint32_t sampleRate = 0;
    uint32_t channels = 0;

    // Codec-private data (e.g. Opus head, VP9 codec config). Demuxer fills,
    // decoder consumes at init.
    std::vector<uint8_t> codecPrivate;

    // Total stream duration for seekable sources; 0 for live.
    TimeNs durationNs = 0;
};

// Produces MediaPackets. File demuxers and network receivers both implement
// this so the decode pipeline doesn't branch on input type. Single-threaded
// by contract: callers own the serialization; implementations are not
// required to be reentrant.
class MediaSource {
public:
    virtual ~MediaSource() = default;

    virtual const std::vector<TrackInfo>& tracks() const = 0;

    // Pull the next packet on any track. Returns false at end-of-stream.
    // Packets are emitted in container order; callers route by trackId.
    virtual bool readPacket(MediaPacket& out) = 0;

    // Optional: seek to the nearest keyframe at or before `pts`. Returns
    // false if the source doesn't support seeking (network live).
    virtual bool seekTo(TimeNs pts) { (void)pts; return false; }
};

} // namespace bro::video
