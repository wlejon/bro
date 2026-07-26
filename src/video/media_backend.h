#pragma once

#include "video/audio_decoder.h"
#include "video/media_source.h"
#include "video/video_decoder.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace bro::video {

/// One complete way of getting from a media file to decoded frames: a
/// demuxer plus the decoders for the tracks it finds.
///
/// The engine ships exactly one backend — WebM/VP9/Opus — and that is
/// deliberate: every additional container and codec is another dependency
/// with another license, and bro stays MIT. But the interfaces underneath
/// (MediaSource, VideoDecoder, AudioDecoder) were always codec-agnostic, so
/// the only thing standing between bro and "plays everything" was
/// VideoPipeline hardcoding which three classes to construct.
///
/// This is that seam. A host application that links its own demuxer and
/// decoders registers a backend here, and every path that plays media —
/// `<video>` included — picks it up without knowing it exists. ffmpeg-bro
/// links libavformat/libavcodec and registers one; anyone else can do the
/// same in their own executable without either project's license reaching
/// the other.
///
/// Decoders come from the SAME backend that opened the source. A TrackInfo's
/// codecPrivate is written by that demuxer and packets carry whatever
/// framing it produces, so pairing one backend's source with another's
/// decoder is not supported.
struct MediaBackend {
    /// Shown in logs when a backend claims a file. Keep it short: "webm",
    /// "ffmpeg".
    std::string name;

    /// Backends are tried highest-priority first. The built-in WebM backend
    /// registers at 0; a host that intends to take over registers above it.
    int priority = 0;

    /// Open `path`. Return nullptr — without logging — when this backend
    /// simply does not handle the format, so the next one gets its turn.
    /// Reserve logging for a file this backend recognised but could not read.
    std::function<std::unique_ptr<MediaSource>(const std::string& path)> open;

    /// Build a decoder for a track the source reported, or nullptr when the
    /// codec is unsupported. A null video decoder makes the pipeline reject
    /// the file and fall through to the next backend; a null audio decoder
    /// just means the file plays silently.
    std::function<std::unique_ptr<VideoDecoder>(const TrackInfo&)> makeVideoDecoder;
    std::function<std::unique_ptr<AudioDecoder>(const TrackInfo&)> makeAudioDecoder;
};

/// Register a backend. Call during host-application startup, before any
/// media is opened.
///
/// Not thread-safe, and not meant to be: registration is startup-time control
/// plane, the same as installing JS bindings.
void registerMediaBackend(MediaBackend backend);

/// Registered backends, highest priority first. The built-in WebM backend is
/// registered on first access, so a host that registers at priority 0 still
/// lands after it rather than racing a static initializer.
const std::vector<MediaBackend>& mediaBackends();

} // namespace bro::video
