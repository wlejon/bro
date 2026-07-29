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
// Both are synchronous and read the file from the start — this is analysis,
// not playback. Cost is in the docs for each.

struct AudioPeaks {
    uint32_t sampleRate = 0;
    uint32_t channels = 0;
    TimeNs durationNs = 0;

    // One entry per bucket, spanning the file evenly. min/max are the
    // envelope (what a waveform is drawn from), rms the perceived loudness
    // (what a filled body is drawn from). All in [-1, 1].
    std::vector<float> minv;
    std::vector<float> maxv;
    std::vector<float> rms;
};

// Decode the whole audio track and reduce it to `buckets` columns. Returns
// false when the file has no audio track this build can decode.
//
// Cost is one full audio decode — around 300 ms for five minutes of AAC.
// Everything else about a timeline is cheap; this is the part worth doing
// once and keeping.
bool analyzeAudioPeaks(const std::string& path, int buckets, AudioPeaks& out);

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

// Grab `count` frames spread evenly across the file, scaled to `height`
// (width follows the frame's aspect). Returns false when there is no video
// track this build can decode.
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
                    ThumbnailStrip& out);

} // namespace bro::video
