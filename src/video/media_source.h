#pragma once

#include "video/media_packet.h"
#include <memory>
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

    // Backend-private handle, attached by the MediaSource that produced this
    // track and read back by the SAME backend's decoder factory.
    //
    // codecPrivate is a flat byte blob, which is all the built-in WebM path
    // needs. A richer backend needs more: an ffmpeg decoder wants the whole
    // AVCodecParameters, and flattening and re-parsing that would lose
    // exactly the details (extradata layout, pixel format, colour space) that
    // make an H.264 stream decodable. So a backend can hand its own object
    // through instead.
    //
    // Opaque to the engine — it is only ever moved along, never inspected.
    // Not valid across backends: only the backend that set it knows the type.
    std::shared_ptr<void> backendPrivate;

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
