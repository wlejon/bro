// Smoke test for the WebM/VP9 encoder. Encodes a synthetic gradient at the
// requested size/length, then decodes it back through the existing demuxer +
// VpxDecoder and verifies that we get the same number of frames out as we
// pushed in. Prints the resulting file size and decoded resolution. Not part
// of the shipped binary — built only via the bro_videoencodetest target.

#include "video/video_decoder.h"
#include "video/webm_demuxer.h"
#include "video/webm_encoder.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace bro::video;

namespace {

void drawFrame(uint8_t* rgba, int w, int h, int frame, int totalFrames) {
    // Diagonal moving stripes — every cell varies in space and time so a
    // dropped frame or off-by-one in the encoder is visually obvious if we
    // ever pipe this into a viewer.
    const int phase = (frame * 8) % 256;
    for (int y = 0; y < h; ++y) {
        uint8_t* row = rgba + y * w * 4;
        for (int x = 0; x < w; ++x) {
            row[x * 4 + 0] = static_cast<uint8_t>((x * 256 / w + phase) & 0xff);
            row[x * 4 + 1] = static_cast<uint8_t>((y * 256 / h - phase) & 0xff);
            row[x * 4 + 2] = static_cast<uint8_t>(((x ^ y) + phase) & 0xff);
            row[x * 4 + 3] = 255;
        }
    }
    (void)totalFrames;
}

long long fileSize(const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return -1;
    std::fseek(f, 0, SEEK_END);
    const long long sz = std::ftell(f);
    std::fclose(f);
    return sz;
}

} // namespace

int main(int argc, char** argv) {
    std::string outPath = "encode_test.webm";
    int width = 320, height = 240, fps = 30, frames = 30;
    if (argc > 1) outPath = argv[1];
    if (argc > 2) width = std::atoi(argv[2]);
    if (argc > 3) height = std::atoi(argv[3]);
    if (argc > 4) fps = std::atoi(argv[4]);
    if (argc > 5) frames = std::atoi(argv[5]);

    std::printf("encode: %s %dx%d @ %dfps × %d frames\n",
                outPath.c_str(), width, height, fps, frames);

    WebmEncoder::Config cfg;
    cfg.width = width;
    cfg.height = height;
    cfg.fpsNum = fps;
    cfg.fpsDen = 1;
    cfg.quality = WebmEncoder::Quality::Realtime;

    std::string err;
    auto enc = WebmEncoder::create(outPath, cfg, &err);
    if (!enc) {
        std::fprintf(stderr, "encoder open failed: %s\n", err.c_str());
        return 1;
    }

    std::vector<uint8_t> frame(static_cast<size_t>(width) * height * 4);
    for (int i = 0; i < frames; ++i) {
        drawFrame(frame.data(), width, height, i, frames);
        if (!enc->addFrameRGBA(frame.data(), width * 4)) {
            std::fprintf(stderr, "addFrameRGBA(%d) failed: %s\n", i, enc->lastError().c_str());
            return 1;
        }
    }
    if (!enc->finish()) {
        std::fprintf(stderr, "finish failed: %s\n", enc->lastError().c_str());
        return 1;
    }
    const int written = enc->framesWritten();
    enc.reset();

    const long long sz = fileSize(outPath);
    std::printf("encoded: %d packets, %lld bytes\n", written, sz);

    // Decode back and count.
    WebMDemuxer demux;
    if (!demux.open(outPath)) {
        std::fprintf(stderr, "demux open failed\n");
        return 1;
    }
    auto dec = createVpxDecoder(Codec::VP9, false);
    int decoded = 0;
    int decW = 0, decH = 0;
    MediaPacket pkt;
    while (demux.readPacket(pkt)) {
        if (pkt.kind != TrackKind::Video) continue;
        if (!dec->decode(pkt)) {
            std::fprintf(stderr, "decode failed at frame %d\n", decoded);
            return 1;
        }
        VideoFrame vf;
        while (dec->nextFrame(vf)) {
            ++decoded;
            decW = vf.width;
            decH = vf.height;
        }
    }

    std::printf("decoded: %d frames, %dx%d\n", decoded, decW, decH);
    if (decoded != frames) {
        std::fprintf(stderr, "FAIL: encoded %d frames but decoded %d\n",
                     frames, decoded);
        return 1;
    }
    if (decW != width || decH != height) {
        std::fprintf(stderr, "FAIL: size mismatch %dx%d != %dx%d\n",
                     decW, decH, width, height);
        return 1;
    }
    std::printf("OK\n");
    return 0;
}
