#include "video/audio_decoder.h"

#include <opus/opus.h>

namespace bro::video {

namespace {

class OpusAudioDecoder final : public AudioDecoder {
public:
    OpusAudioDecoder(uint32_t sampleRate, uint32_t channels)
        : sampleRate_(sampleRate), channels_(channels) {}

    ~OpusAudioDecoder() override {
        if (dec_) opus_decoder_destroy(dec_);
    }

    bool init() {
        int err = 0;
        dec_ = opus_decoder_create(static_cast<opus_int32>(sampleRate_),
                                   static_cast<int>(channels_), &err);
        return err == OPUS_OK && dec_ != nullptr;
    }

    bool decode(const MediaPacket& pkt, AudioFrame& out) override {
        if (!dec_ && !init()) return false;
        if (!pkt.data || pkt.data->empty()) return false;

        // Opus packets are at most 120 ms; at 48 kHz that's 5760 samples per
        // channel. Allocate the max to avoid a second decode attempt.
        constexpr int kMaxFramesPerChannel = 5760;
        out.samples.resize(static_cast<size_t>(kMaxFramesPerChannel) * channels_);

        const int decoded = opus_decode_float(
            dec_,
            pkt.data->data(),
            static_cast<opus_int32>(pkt.data->size()),
            out.samples.data(),
            kMaxFramesPerChannel,
            /*decode_fec=*/0);
        if (decoded < 0) return false;

        out.samples.resize(static_cast<size_t>(decoded) * channels_);
        out.sampleRate = sampleRate_;
        out.channels = channels_;
        out.pts = pkt.pts;
        return true;
    }

private:
    uint32_t sampleRate_;
    uint32_t channels_;
    OpusDecoder* dec_ = nullptr;
};

} // namespace

std::unique_ptr<AudioDecoder> createOpusDecoder(uint32_t sampleRate, uint32_t channels) {
    return std::make_unique<OpusAudioDecoder>(sampleRate, channels);
}

} // namespace bro::video
