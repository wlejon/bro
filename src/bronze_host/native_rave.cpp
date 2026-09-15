#include "natives/rave/native_rave_decl.h"
#include <vector>
#include <cstdint>

namespace {

struct BroRaveImpl {
    bool loaded = true;
    int32_t sampleRate = 48000;
    int32_t nLatent = 8;
    int32_t fullLatent = 16;
    int32_t nBand = 16;
    int32_t totalRatio = 2048;
};

struct RaveEncodeSlot {
    std::vector<float> latent;
    int32_t nLatent = 8;
    int32_t frames = 0;
};

struct RaveDecodeSlot {
    std::vector<float> samples;
    int32_t sampleRate = 48000;
    int32_t channels = 1;
};

static thread_local RaveEncodeSlot tl_encodeSlot;
static thread_local RaveDecodeSlot tl_decodeSlot;

} // namespace

extern "C" {

void bro_rave_Rave_dtor(void* self) {
    delete static_cast<BroRaveImpl*>(self);
}

void* bro_rave_Rave_ctor(void) {
    return new BroRaveImpl();
}

bool bro_rave_Rave_loaded_get(void* self) {
    auto* r = static_cast<BroRaveImpl*>(self);
    return r ? r->loaded : false;
}

int32_t bro_rave_Rave_sampleRate_get(void* self) {
    auto* r = static_cast<BroRaveImpl*>(self);
    return r ? r->sampleRate : 48000;
}

int32_t bro_rave_Rave_nLatent_get(void* self) {
    auto* r = static_cast<BroRaveImpl*>(self);
    return r ? r->nLatent : 8;
}

int32_t bro_rave_Rave_fullLatent_get(void* self) {
    auto* r = static_cast<BroRaveImpl*>(self);
    return r ? r->fullLatent : 16;
}

int32_t bro_rave_Rave_nBand_get(void* self) {
    auto* r = static_cast<BroRaveImpl*>(self);
    return r ? r->nBand : 16;
}

int32_t bro_rave_Rave_totalRatio_get(void* self) {
    auto* r = static_cast<BroRaveImpl*>(self);
    return r ? r->totalRatio : 2048;
}

void bro_rave_Rave_encode(void* /*self*/, const float* /*audio*/, uint32_t /*audio_len*/) {
    tl_encodeSlot.latent.clear();
    tl_encodeSlot.nLatent = 8;
    tl_encodeSlot.frames = 0;
}

void bro_rave_Rave_encode_latent(bronze_native_buffer* out) {
    if (!out) return;
    out->data = tl_encodeSlot.latent.data();
    out->length = static_cast<uint32_t>(tl_encodeSlot.latent.size());
    out->release = nullptr;
    out->ctx = nullptr;
}

int32_t bro_rave_Rave_encode_nLatent(void) {
    return tl_encodeSlot.nLatent;
}

int32_t bro_rave_Rave_encode_frames(void) {
    return tl_encodeSlot.frames;
}

void bro_rave_Rave_decode(void* /*self*/, const float* /*latent*/, uint32_t /*latent_len*/,
                          int32_t frames, bool /*opts_addNoise*/, bool /*opts_seed_given*/,
                          double /*opts_seed*/, int32_t opts_channels, double /*opts_stereoWidth*/) {
    tl_decodeSlot.samples.clear();
    tl_decodeSlot.sampleRate = 48000;
    tl_decodeSlot.channels = opts_channels > 0 ? opts_channels : 1;
    (void)frames;
}

void bro_rave_Rave_decode_samples(bronze_native_buffer* out) {
    if (!out) return;
    out->data = tl_decodeSlot.samples.data();
    out->length = static_cast<uint32_t>(tl_decodeSlot.samples.size());
    out->release = nullptr;
    out->ctx = nullptr;
}

int32_t bro_rave_Rave_decode_sampleRate(void) {
    return tl_decodeSlot.sampleRate;
}

int32_t bro_rave_Rave_decode_channels(void) {
    return tl_decodeSlot.channels;
}

#if BRO_WITH_SOUNDML
void bro_rave_init(void) {}

void* bro_rave_loadRave(const char* /*modelDir*/, bool /*opts_device_given*/,
                        const char* /*opts_device*/, uint64_t /*opts_onReady*/,
                        uint64_t /*opts_onError*/) {
    return new BroRaveImpl();
}
#endif

} // extern "C"

namespace bro::bronze_host {
bool registerNatives_rave(std::string* error);
bool registerRaveNatives(std::string* error) {
    return registerNatives_rave(error);
}
} // namespace bro::bronze_host
