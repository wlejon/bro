#pragma once

#include <cstdint>
#include <memory>
#include <vector>

namespace bro::video {

// Codec identifier on the wire / in containers. Kept narrow on purpose:
// the engine commits to a single pipeline (VP9 video + Opus audio in WebM).
// Additional entries should be added only when a second pipeline is supported.
enum class Codec : uint8_t {
    Unknown = 0,
    VP8,
    VP9,
    Opus,
    Vorbis,
};

enum class TrackKind : uint8_t {
    Video,
    Audio,
};

// Timestamps are in nanoseconds from the start of the stream. WebM's native
// unit is also nanoseconds (via timecode scale), and using ns matches the
// precision needed for a/v sync without forcing a rational-number type.
using TimeNs = int64_t;

// A single compressed unit: one video frame or one audio packet.
// Ownership of `data` is shared so packets can be cheaply routed across
// thread boundaries without copying when a queue fans out.
struct MediaPacket {
    uint32_t trackId = 0;
    Codec codec = Codec::Unknown;
    TrackKind kind = TrackKind::Video;
    bool keyframe = false;

    TimeNs pts = 0;         // presentation timestamp
    TimeNs duration = 0;    // 0 if unknown

    std::shared_ptr<std::vector<uint8_t>> data;
};

} // namespace bro::video
