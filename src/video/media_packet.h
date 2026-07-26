#pragma once

#include <cstdint>
#include <memory>
#include <vector>

namespace bro::video {

// Codec identifier on the wire / in containers.
//
// The engine itself still decodes only VP8/VP9 + Opus — the rest are here
// because a registered MediaBackend (see media_backend.h) reports what it
// found, and that value reaches JS and the UI. A player wants to display
// "h264" whether or not bro is the thing decoding it.
//
// `Other` is the escape hatch: a backend handling something not named here
// reports Other and keeps its own identification in the TrackInfo it built.
// Nothing in the engine switches on these beyond the built-in WebM backend
// matching its own three.
enum class Codec : uint8_t {
    Unknown = 0,
    // Video
    VP8,
    VP9,
    AV1,
    H264,
    H265,
    MPEG2Video,
    MPEG4,
    ProRes,
    // Audio
    Opus,
    Vorbis,
    AAC,
    MP3,
    FLAC,
    AC3,
    EAC3,
    PCM,
    // Handled by a backend, not named above.
    Other,
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
