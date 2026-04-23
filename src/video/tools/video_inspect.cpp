// Smoke test for the bro video pipeline. Reads a WebM file, lists its
// tracks, decodes a handful of video frames and audio packets, and
// prints what it saw. Not part of the shipped binary — built only via
// the bro_videoinspect target.

#include "video/audio_decoder.h"
#include "video/video_decoder.h"
#include "video/webm_demuxer.h"

#include <cstdio>
#include <cstdlib>
#include <string>

using namespace bro::video;

namespace {

const char* codecName(Codec c) {
    switch (c) {
        case Codec::VP8:    return "VP8";
        case Codec::VP9:    return "VP9";
        case Codec::Opus:   return "Opus";
        case Codec::Vorbis: return "Vorbis";
        default:            return "unknown";
    }
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <file.webm> [max_video_frames=16] [max_audio_packets=16]\n", argv[0]);
        return 2;
    }
    const std::string path = argv[1];
    const int maxVideo = argc > 2 ? std::atoi(argv[2]) : 16;
    const int maxAudio = argc > 3 ? std::atoi(argv[3]) : 16;

    WebMDemuxer demux;
    if (!demux.open(path)) {
        std::fprintf(stderr, "open failed: %s\n", demux.lastError().c_str());
        return 1;
    }

    std::printf("file: %s\n", path.c_str());
    std::printf("tracks: %zu\n", demux.tracks().size());

    uint32_t videoTrackId = 0;
    uint32_t audioTrackId = 0;
    Codec videoCodec = Codec::Unknown;
    uint32_t audioRate = 0, audioChannels = 0;

    for (const auto& t : demux.tracks()) {
        if (t.kind == TrackKind::Video) {
            std::printf("  #%u video  %s  %ux%u  dur=%.3fs\n",
                        t.id, codecName(t.codec), t.width, t.height,
                        t.durationNs / 1e9);
            if (!videoTrackId) { videoTrackId = t.id; videoCodec = t.codec; }
        } else {
            std::printf("  #%u audio  %s  %u Hz  %u ch  dur=%.3fs\n",
                        t.id, codecName(t.codec), t.sampleRate, t.channels,
                        t.durationNs / 1e9);
            if (!audioTrackId) { audioTrackId = t.id; audioRate = t.sampleRate; audioChannels = t.channels; }
        }
    }

    std::unique_ptr<VideoDecoder> vdec;
    if (videoTrackId) vdec = createVpxDecoder(videoCodec, /*lowLatency=*/false);

    std::unique_ptr<AudioDecoder> adec;
    if (audioTrackId) adec = createOpusDecoder(audioRate, audioChannels);

    int videoSeen = 0, audioSeen = 0;
    MediaPacket pkt;
    while ((videoSeen < maxVideo || audioSeen < maxAudio) && demux.readPacket(pkt)) {
        if (pkt.trackId == videoTrackId && vdec) {
            if (!vdec->decode(pkt)) {
                std::printf("  video decode error at pts=%.3fs\n", pkt.pts / 1e9);
                continue;
            }
            VideoFrame frame;
            while (vdec->nextFrame(frame) && videoSeen < maxVideo) {
                std::printf("  V[%d] pts=%.3fs %ux%u keyframe=%d\n",
                            videoSeen, frame.pts / 1e9, frame.width, frame.height, pkt.keyframe);
                ++videoSeen;
            }
        } else if (pkt.trackId == audioTrackId && adec) {
            AudioFrame frame;
            if (adec->decode(pkt, frame) && audioSeen < maxAudio) {
                std::printf("  A[%d] pts=%.3fs %zu samples (%u ch, %u Hz)\n",
                            audioSeen, frame.pts / 1e9, frame.samples.size() / frame.channels,
                            frame.channels, frame.sampleRate);
                ++audioSeen;
            }
        }
    }

    std::printf("decoded: %d video frame(s), %d audio packet(s)\n", videoSeen, audioSeen);
    return 0;
}
