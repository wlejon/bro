#pragma once

#include "video/media_packet.h"
#include <cstdint>
#include <memory>
#include <vector>

namespace bro::video {

// Decoded PCM chunk in interleaved float32. libopus decodes to int16 or
// float; we pick float so downstream broaudio mixing never has to convert.
struct AudioFrame {
    uint32_t sampleRate = 0;
    uint32_t channels = 0;
    TimeNs pts = 0;
    std::vector<float> samples;  // length = frames * channels
};

class AudioDecoder {
public:
    virtual ~AudioDecoder() = default;

    // Feed one compressed packet and return the decoded PCM. Single
    // packet in → single frame out (Opus/Vorbis don't fan out).
    virtual bool decode(const MediaPacket& pkt, AudioFrame& out) = 0;
};

// Opus decoder (libopus). `channels` and `sampleRate` come from the
// container; sampleRate must be one of {8k,12k,16k,24k,48k}.
std::unique_ptr<AudioDecoder> createOpusDecoder(uint32_t sampleRate, uint32_t channels);

} // namespace bro::video
