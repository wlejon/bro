// =============================================================================
// bro_tts_c_abi.cpp — C++ forwarding implementations for bro.tts
// Generated automatically by brosurface (gen/emit_c_abi.mjs).
// =============================================================================

#include "bro/c_abi/bro_tts_c_abi.h"
#include "bro/c_abi/bro_engine_c_abi.h"
#include <cstdint>
#include <string>

struct BroKokoroModelImpl {
    bool loaded = true;
    std::string device = "CPU";
};

struct BroVoiceImpl {
    bool loaded = true;
    std::string name = "default";
};

struct BroKokoroSessionImpl {
    bool loaded = true;
};

struct BroQwenTtsModelImpl {
    bool loaded = true;
    std::string device = "CPU";
    std::string variant = "base";
};

struct BroQwenTtsSessionImpl {
    bool loaded = true;
    std::string variant = "base";
};

struct BroSupertonicModelImpl {
    bool loaded = true;
    std::string device = "CPU";
};

struct BroSupertonicVoiceImpl {
    bool loaded = true;
    std::string name = "default";
};

struct BroSpeakerEncoderImpl {
    bool loaded = true;
    std::string device = "CPU";
};

extern "C" {

// --- Interface bro.tts.KokoroModel ---
void* bro_KokoroModel_create(void) {
    return new BroKokoroModelImpl();
}

void bro_KokoroModel_destroy(void* self) {
    delete static_cast<BroKokoroModelImpl*>(self);
}

bool bro_KokoroModel_get_loaded(void* self) {
    auto* m = static_cast<BroKokoroModelImpl*>(self);
    return m ? m->loaded : false;
}

const char* bro_KokoroModel_get_device(void* self) {
    auto* m = static_cast<BroKokoroModelImpl*>(self);
    return m ? m->device.c_str() : "CPU";
}

void* bro_KokoroModel_encodePhonemes(void* /*self*/, const char* /*ipa*/) {
    return nullptr;
}

void* bro_KokoroModel_loadVoice(void* /*self*/, const char* /*path*/) {
    return new BroVoiceImpl();
}

void* bro_KokoroModel_createSession(void* /*self*/) {
    return new BroKokoroSessionImpl();
}

// --- Interface bro.tts.Voice ---
void* bro_Voice_create(void) {
    return new BroVoiceImpl();
}

void bro_Voice_destroy(void* self) {
    delete static_cast<BroVoiceImpl*>(self);
}

bool bro_Voice_get_loaded(void* self) {
    auto* v = static_cast<BroVoiceImpl*>(self);
    return v ? v->loaded : false;
}

const char* bro_Voice_get_name(void* self) {
    auto* v = static_cast<BroVoiceImpl*>(self);
    return v ? v->name.c_str() : "default";
}

// --- Interface bro.tts.KokoroSession ---
void* bro_KokoroSession_create(void) {
    return new BroKokoroSessionImpl();
}

void bro_KokoroSession_destroy(void* self) {
    delete static_cast<BroKokoroSessionImpl*>(self);
}

bool bro_KokoroSession_get_loaded(void* self) {
    auto* s = static_cast<BroKokoroSessionImpl*>(self);
    return s ? s->loaded : false;
}

void* bro_KokoroSession_synthesize(void* /*self*/, void* /*phonemes*/, void* /*voice*/, void* /*opts*/) {
    return nullptr;
}

void bro_KokoroSession_reset(void* /*self*/) {
}

// --- Interface bro.tts.QwenTtsModel ---
void* bro_QwenTtsModel_create(void) {
    return new BroQwenTtsModelImpl();
}

void bro_QwenTtsModel_destroy(void* self) {
    delete static_cast<BroQwenTtsModelImpl*>(self);
}

bool bro_QwenTtsModel_get_loaded(void* self) {
    auto* m = static_cast<BroQwenTtsModelImpl*>(self);
    return m ? m->loaded : false;
}

const char* bro_QwenTtsModel_get_device(void* self) {
    auto* m = static_cast<BroQwenTtsModelImpl*>(self);
    return m ? m->device.c_str() : "CPU";
}

const char* bro_QwenTtsModel_get_variant(void* self) {
    auto* m = static_cast<BroQwenTtsModelImpl*>(self);
    return m ? m->variant.c_str() : "base";
}

void* bro_QwenTtsModel_createSession(void* /*self*/) {
    return new BroQwenTtsSessionImpl();
}

void* bro_QwenTtsModel_encodeAudio(void* /*self*/, void* /*audio*/) {
    return nullptr;
}

void* bro_QwenTtsModel_decodeCodes(void* /*self*/, void* /*codes*/) {
    return nullptr;
}

// --- Interface bro.tts.QwenTtsSession ---
void* bro_QwenTtsSession_create(void) {
    return new BroQwenTtsSessionImpl();
}

void bro_QwenTtsSession_destroy(void* self) {
    delete static_cast<BroQwenTtsSessionImpl*>(self);
}

bool bro_QwenTtsSession_get_loaded(void* self) {
    auto* s = static_cast<BroQwenTtsSessionImpl*>(self);
    return s ? s->loaded : false;
}

const char* bro_QwenTtsSession_get_variant(void* self) {
    auto* s = static_cast<BroQwenTtsSessionImpl*>(self);
    return s ? s->variant.c_str() : "base";
}

void* bro_QwenTtsSession_synthesize(void* /*self*/, const char* /*text*/, void* /*opts*/) {
    return nullptr;
}

void bro_QwenTtsSession_reset(void* /*self*/) {
}

// --- Interface bro.tts.SupertonicModel ---
void* bro_SupertonicModel_create(void) {
    return new BroSupertonicModelImpl();
}

void bro_SupertonicModel_destroy(void* self) {
    delete static_cast<BroSupertonicModelImpl*>(self);
}

bool bro_SupertonicModel_get_loaded(void* self) {
    auto* m = static_cast<BroSupertonicModelImpl*>(self);
    return m ? m->loaded : false;
}

const char* bro_SupertonicModel_get_device(void* self) {
    auto* m = static_cast<BroSupertonicModelImpl*>(self);
    return m ? m->device.c_str() : "CPU";
}

void* bro_SupertonicModel_loadVoiceStyle(void* /*self*/, const char* /*path*/) {
    return new BroSupertonicVoiceImpl();
}

// --- Interface bro.tts.SupertonicVoice ---
void* bro_SupertonicVoice_create(void) {
    return new BroSupertonicVoiceImpl();
}

void bro_SupertonicVoice_destroy(void* self) {
    delete static_cast<BroSupertonicVoiceImpl*>(self);
}

bool bro_SupertonicVoice_get_loaded(void* self) {
    auto* v = static_cast<BroSupertonicVoiceImpl*>(self);
    return v ? v->loaded : false;
}

const char* bro_SupertonicVoice_get_name(void* self) {
    auto* v = static_cast<BroSupertonicVoiceImpl*>(self);
    return v ? v->name.c_str() : "default";
}

// --- Interface bro.tts.SpeakerEncoder ---
void* bro_SpeakerEncoder_create(void) {
    return new BroSpeakerEncoderImpl();
}

void bro_SpeakerEncoder_destroy(void* self) {
    delete static_cast<BroSpeakerEncoderImpl*>(self);
}

bool bro_SpeakerEncoder_get_loaded(void* self) {
    auto* e = static_cast<BroSpeakerEncoderImpl*>(self);
    return e ? e->loaded : false;
}

const char* bro_SpeakerEncoder_get_device(void* self) {
    auto* e = static_cast<BroSpeakerEncoderImpl*>(self);
    return e ? e->device.c_str() : "CPU";
}

void* bro_SpeakerEncoder_embedSpeaker(void* /*self*/, void* /*audio*/, void* /*opts*/) {
    return nullptr;
}

// --- Namespace bro.tts ---
void bro_tts_init(void) {
    const auto* b = bro_get_tts_bridge();
    if (b && b->init) b->init();
}

void* bro_tts_loadKokoro(const char* dir, void* opts) {
    const auto* b = bro_get_tts_bridge();
    if (b && b->loadKokoro) return b->loadKokoro(dir, opts);
    return new BroKokoroModelImpl();
}

void* bro_tts_loadQwen(const char* dir, void* opts) {
    const auto* b = bro_get_tts_bridge();
    if (b && b->loadQwen) return b->loadQwen(dir, opts);
    return new BroQwenTtsModelImpl();
}

void* bro_tts_loadSupertonic(const char* dir, void* opts) {
    const auto* b = bro_get_tts_bridge();
    if (b && b->loadSupertonic) return b->loadSupertonic(dir, opts);
    return new BroSupertonicModelImpl();
}

void* bro_tts_loadSpeakerEncoder(const char* dir, void* opts) {
    const auto* b = bro_get_tts_bridge();
    if (b && b->loadSpeakerEncoder) return b->loadSpeakerEncoder(dir, opts);
    return new BroSpeakerEncoderImpl();
}

void* bro_tts_phonemize(const char* text, void* opts) {
    const auto* b = bro_get_tts_bridge();
    if (b && b->phonemize) return b->phonemize(text, opts);
    return nullptr;
}

void bro_tts_setAssetRoot(const char* dir) {
    const auto* b = bro_get_tts_bridge();
    if (b && b->setAssetRoot) b->setAssetRoot(dir);
}

void bro_tts_setAssets(void* opts) {
    const auto* b = bro_get_tts_bridge();
    if (b && b->setAssets) b->setAssets(opts);
}

void* bro_tts_synthesize(void* model, void* textOrPhonemes, void* voiceOrOpts, void* opts) {
    const auto* b = bro_get_tts_bridge();
    if (b && b->synthesize) return b->synthesize(model, textOrPhonemes, voiceOrOpts, opts);
    return nullptr;
}

void* bro_tts_synthesizeStream(void* model, void* textOrChunks, void* voiceOrOpts, void* opts) {
    const auto* b = bro_get_tts_bridge();
    if (b && b->synthesizeStream) return b->synthesizeStream(model, textOrChunks, voiceOrOpts, opts);
    return nullptr;
}

void* bro_tts_decodeFrom(void* kokoro, void* voice, void* asr, void* F0, void* N, int32_t nPhonemes, void* opts) {
    const auto* b = bro_get_tts_bridge();
    if (b && b->decodeFrom) return b->decodeFrom(kokoro, voice, asr, F0, N, nPhonemes, opts);
    return nullptr;
}

}
