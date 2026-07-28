#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace bro::video {

// Software VP9 encoder + WebM muxer. Single video track, RGBA in, file out.
//
// Frames are encoded in submission order with a monotonically increasing
// timestamp derived from (frameIndex / fps). Forced keyframes happen at the
// configured interval; the encoder may insert additional keyframes if its
// rate-control logic decides one is needed.
//
// Hardware encode is intentionally absent — VP9 hardware support is sparse
// (no NVENC, no D3D12 Video Encode). When that becomes a bottleneck, add a
// sibling D3D12VideoEncoder/H264Encoder behind the same shape; the muxer
// stays the same.
class WebmEncoder {
public:
    enum class Quality {
        Realtime,  // VPX_DL_REALTIME — lowest latency, lowest quality/bitrate
        Good,      // VPX_DL_GOOD_QUALITY — balanced default
        Best,      // VPX_DL_BEST_QUALITY — slowest, smallest file at target bitrate
    };

    struct Config {
        int width  = 0;
        int height = 0;
        int fpsNum = 30;
        int fpsDen = 1;
        // 0 = auto: ~width*height*fps*0.07 bits/pixel/frame, capped to 8000.
        int targetBitrateKbps = 0;
        int keyframeIntervalSec = 2;
        Quality quality = Quality::Good;
        // 0 = libvpx default (typically 1).
        int threads = 0;

        // How far a player has to turn the picture clockwise to show it the
        // right way up: 0, 90, 180 or 270. The pixels are written as they are
        // given; this is metadata, so a portrait clip encoded from landscape
        // frames stays landscape in the file and is presented portrait. 0
        // writes no Projection element at all, which is why every file this
        // encoder wrote before is byte-for-byte what it was.
        int rotationDegrees = 0;

        // Audio. 0 = no audio track. Opus only accepts 8/12/16/24/48 kHz.
        int audioSampleRate = 0;
        int audioChannels   = 2;
        int audioBitrateKbps = 96;
    };

    // Open a WebM file for writing. Returns null on init failure (codec
    // refused config, file open failed, etc) and writes a human-readable
    // reason into *err if provided.
    static std::unique_ptr<WebmEncoder> create(const std::string& path,
                                               const Config& cfg,
                                               std::string* err = nullptr);

    virtual ~WebmEncoder() = default;

    // Push one RGBA frame. `srcStride` may be 0 to use width*4.
    // Width/height of `rgba` must match the configured size. Returns false
    // on encode error; call lastError() for details.
    virtual bool addFrameRGBA(const uint8_t* rgba, int srcStride) = 0;

    // Push interleaved float PCM. `frameCount` is samples per channel.
    // Channel count must match Config::audioChannels. Samples are buffered
    // and encoded in 20 ms Opus packets; trailing partial chunks are
    // zero-padded inside finish(). No-op if no audio track was configured.
    virtual bool addAudioFramesPCM(const float* interleaved, int frameCount) = 0;

    // Flush remaining frames out of the encoder, write the WebM trailer,
    // close the file. Safe to call multiple times; subsequent calls no-op.
    // Destructor calls finish() if still open.
    virtual bool finish() = 0;

    virtual const std::string& lastError() const = 0;
    virtual int framesWritten() const = 0;
};

} // namespace bro::video
