#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace bro::video {

// Animated GIF89a encoder. RGBA in, file out. Per-frame 256-color palette via
// median-cut quantization (alpha snapped to 0/255 — fully transparent pixels
// become the GIF transparent color, partial alpha is treated as opaque).
//
// Sized for sprite/animation export, not photographic content. Quality at
// 256 colors is fine for pixel art; for natural photos consider WebM instead.
class GifEncoder {
public:
    struct Config {
        int width  = 0;
        int height = 0;
        // Frame delay in centiseconds (1/100 of a second). 4 cs ≈ 25 fps.
        // Construct from fps with: delayCs = round(100 / fps).
        int delayCs = 4;
        // Loop forever (0 = infinite, 1 = play once, N = repeat N times).
        int loopCount = 0;
        // 1..8. Lower = fewer colors, smaller file. 8 = 256 colors max.
        int paletteBits = 8;
    };

    static std::unique_ptr<GifEncoder> create(const std::string& path,
                                              const Config& cfg,
                                              std::string* err = nullptr);

    virtual ~GifEncoder() = default;

    // Push one RGBA frame, top-down. srcStride may be 0 to use width*4.
    virtual bool addFrameRGBA(const uint8_t* rgba, int srcStride) = 0;

    // Override the per-frame delay for the next addFrameRGBA call.
    // Reverts to Config::delayCs after each call.
    virtual void setNextFrameDelayCs(int delayCs) = 0;

    virtual bool finish() = 0;
    virtual const std::string& lastError() const = 0;
    virtual int framesWritten() const = 0;
};

} // namespace bro::video
