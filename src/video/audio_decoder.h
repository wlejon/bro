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

    // Ask for PCM in a specific shape — the audio engine's sample rate and a
    // channel count it can mix — instead of whatever the stream happens to
    // carry. Returns false if the decoder can't convert, in which case it
    // keeps emitting its native format.
    //
    // This is what makes STREAMING playback possible. Converting rates chunk
    // by chunk needs a resampler that carries filter state across calls;
    // bro's own is a one-shot offline function, so a decoder that can't do
    // this leaves the caller to decode the whole track up front and convert
    // it in one pass. A decoder wrapping libswresample can, and streams.
    virtual bool setOutputFormat(uint32_t sampleRate, uint32_t channels) {
        (void)sampleRate; (void)channels;
        return false;
    }

    // Feed one compressed packet and return the decoded PCM. Single
    // packet in → single frame out (Opus/Vorbis don't fan out).
    virtual bool decode(const MediaPacket& pkt, AudioFrame& out) = 0;

    // Drop buffered decoder state after the source has jumped. See
    // VideoDecoder::flush().
    virtual void flush() {}
};

// Opus decoder (libopus). `channels` and `sampleRate` come from the
// container; sampleRate must be one of {8k,12k,16k,24k,48k}.
std::unique_ptr<AudioDecoder> createOpusDecoder(uint32_t sampleRate, uint32_t channels);

} // namespace bro::video
