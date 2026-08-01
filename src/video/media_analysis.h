#pragma once

#include "video/media_packet.h"

#include <cstdint>
#include <string>
#include <vector>

namespace bro::video {

// Whole-file summaries of a media file, for the two things every timeline
// needs and nothing in the DOM can express: a waveform to see the sound in,
// and a filmstrip to see the picture in.
//
// Both go through the media backend registry, so a host that registered an
// ffmpeg backend gets these for every format it can open, and with none
// registered they work on the built-in WebM path.
//
// Both are synchronous — this is analysis, not playback. Cost is in the docs
// for each.

// Which part of the file to summarise. The default is all of it, which is what
// every caller wanted while every file was one somebody had already downloaded.
// A file reached over the network is the case that made a span worth naming: a
// two-hour recording read from the start to fill a lane you are looking one
// minute of is the whole recording pulled down the link to draw a strip of it,
// and the strip you are looking at is a hundredth of what was read. So a caller
// that knows which part it is showing can ask for that part, and the buckets
// and the thumbnails spread across the span rather than across the file.
//
// `toNs` of 0 means "to the end", so `{from, 0}` is a tail and `{}` is
// everything. Both are clamped to the file, and a span that ends up empty is a
// refusal rather than a silently whole-file read.
struct Window {
    TimeNs fromNs = 0;
    TimeNs toNs = 0;
};

struct AudioPeaks {
    uint32_t sampleRate = 0;
    uint32_t channels = 0;
    TimeNs durationNs = 0;      // of the FILE, whatever span was asked for

    // The span the buckets actually cover, after clamping — the whole file
    // unless a Window said otherwise. A caller drawing a partial lane needs
    // both this and `durationNs`, and inferring one from the other is exactly
    // the arithmetic that puts a waveform in the wrong place.
    TimeNs fromNs = 0;
    TimeNs toNs = 0;

    // One entry per bucket, spanning [fromNs, toNs) evenly. min/max are the
    // envelope (what a waveform is drawn from), rms the perceived loudness
    // (what a filled body is drawn from). All in [-1, 1].
    std::vector<float> minv;
    std::vector<float> maxv;
    std::vector<float> rms;
};

// Decode the audio track over `window` and reduce it to `buckets` columns.
// Returns false when the file has no audio track this build can decode, or
// when the window is empty.
//
// Cost is one audio decode of the span — around 300 ms for five minutes of
// AAC. Everything else about a timeline is cheap; this is the part worth doing
// once and keeping.
bool analyzeAudioPeaks(const std::string& path, int buckets, AudioPeaks& out,
                       Window window = {});

struct ThumbnailStrip {
    int width = 0;      // of one thumbnail
    int height = 0;
    int count = 0;
    // What the frames had to be turned by to come out the right way up: 0, 90,
    // 180 or 270, from TrackInfo::rotationDegrees. Already APPLIED to the
    // pixels below — this reports what was done, so a caller can tell a phone
    // clip from an upright one without opening the file again. `width` is the
    // width of the turned picture, so it is the SHORT side of a sideways clip.
    int rotationDegrees = 0;
    std::vector<TimeNs> times;    // when each thumbnail is actually from
    // count thumbnails side by side in one image, (width*count) x height
    // RGBA8. One image because that is one texture upload and one putImageData
    // rather than `count` of each.
    std::vector<uint8_t> rgba;
};

// Grab `count` frames spread evenly across `window`, scaled to `height`
// (width follows the frame's aspect). Returns false when there is no video
// track this build can decode, or when the window is empty.
//
// Each grab seeks and decodes the keyframe it lands on rather than decoding
// forward to an exact time — which is both what makes this fast and what a
// filmstrip wants, since a keyframe is where the picture changed. `times`
// reports what was actually grabbed.
//
// **Thumbnails come out the right way up.** A phone records landscape frames
// and writes the correction into the container, so a strip taken at the coded
// size is a row of frames lying on their side under a portrait picture. Unlike
// <video> — which turns the quad it draws and leaves the pixels alone — there
// is nothing downstream of a strip to do the turning: it is a baked RGBA
// image, so the turn happens here, in the same pass that scales.
bool grabThumbnails(const std::string& path, int count, int height,
                    ThumbnailStrip& out, Window window = {});

} // namespace bro::video
