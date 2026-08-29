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

    // Nominal frames per second, 0 when the container doesn't say. An
    // average: a variable-frame-rate file still reports one, so this is for
    // display (timecode, "29.97 fps" in an info panel) and never for deciding
    // where a frame boundary is. Stepping uses the frames' own timestamps.
    double frameRate = 0.0;

    // How far the decoded picture has to be turned CLOCKWISE to be the right
    // way up, in degrees: 0, 90, 180 or 270, and 0 when the container says
    // nothing. Phones record sideways and write the correction into the
    // container rather than rotating the pixels, so a portrait clip whose
    // frames are 1920x1080 is shown 1080x1920.
    //
    // This is metadata about the picture, not a property of it: width and
    // height stay the size of the frames the decoder produces, and turning
    // them is presentation. VideoPipeline::displayWidth/displayHeight are
    // the swapped pair, and <video> rotates the quad it draws rather than
    // the pixels it decoded.
    //
    // Anything that is not a quarter turn is refused by whoever reads the
    // container and reported as 0 — an arbitrary angle is a transform this
    // element cannot express as a size, and claiming a size that is wrong is
    // worse than showing the picture as it was stored.
    int rotationDegrees = 0;

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

    // Can the next readPacket answer without waiting?
    //
    // A demuxer always can: the packet is bytes on a disk or in a socket
    // buffer, and reading it is the only way to find out whether it is there.
    // A source that *makes* its packets cannot — a render being played while
    // it is made needs real time to produce the next block, and the only
    // answer readPacket has for "not yet" is to wait, because false means the
    // stream ended.
    //
    // That wait lands on whichever thread asked, and one of them is the UI
    // thread: pumpStreamingAudio tops the audio ring up once a frame from
    // pumpEvents. A source that waits four seconds for its first block is a
    // window that stops for four seconds — measured at 5.9 s on a thirteen-cut
    // mix of four six-hour recordings, the whole of it inside one frame.
    //
    // So a caller on a thread that must keep moving asks this first and comes
    // back next frame. Waiting never made the sound arrive sooner; it only
    // decided who stood still while it did.
    virtual bool packetReady() const { return true; }

    // Optional: seek to the nearest keyframe at or before `pts`. Returns
    // false if the source doesn't support seeking (network live).
    virtual bool seekTo(TimeNs pts) { (void)pts; return false; }

    // Only deliver packets for these tracks. A player opens the same file
    // twice — once pumping video, once pumping audio — and without this each
    // instance reads, copies and discards the other's packets, which for
    // 1080p is several MB/s of pure waste. Ignored by sources that can't
    // filter; readPacket callers must still check trackId.
    virtual void setActiveTracks(const std::vector<uint32_t>& trackIds) {
        (void)trackIds;
    }
};

} // namespace bro::video
