// =============================================================================
// bro_stt_c_abi.cpp — C++ forwarding implementations for bro.stt
// Generated automatically by brosurface (gen/emit_c_abi.mjs).
// =============================================================================

#include "bro/c_abi/bro_stt_c_abi.h"
#include "bro/c_abi/bro_engine_c_abi.h"
#include <cstdint>
#include <string>

struct BroWhisperTokenizerImpl {
    bool loaded = true;
};

struct BroWhisperModelImpl {
    bool loaded = true;
    std::string device = "CPU";
};

struct BroWhisperSessionImpl {
    bool loaded = true;
};

struct BroParakeetTokenizerImpl {
    bool loaded = true;
};

struct BroParakeetModelImpl {
    bool loaded = true;
    std::string device = "CPU";
};

struct BroParakeetSessionImpl {
    bool loaded = true;
};

struct BroQwenAsrModelImpl {
    bool loaded = true;
    std::string device = "CPU";
};

struct BroQwenAsrSessionImpl {
    bool loaded = true;
};

struct BroQwenAsrStreamImpl {
    bool loaded = true;
};

extern "C" {

// --- Interface bro.stt.WhisperTokenizer ---
void* bro_WhisperTokenizer_create(void) {
    return new BroWhisperTokenizerImpl();
}

void bro_WhisperTokenizer_destroy(void* self) {
    delete static_cast<BroWhisperTokenizerImpl*>(self);
}

bool bro_WhisperTokenizer_get_loaded(void* self) {
    auto* t = static_cast<BroWhisperTokenizerImpl*>(self);
    return t ? t->loaded : false;
}

void* bro_WhisperTokenizer_encode(void* /*self*/, const char* /*text*/) {
    return nullptr;
}

const char* bro_WhisperTokenizer_decode(void* /*self*/, void* /*tokenIds*/) {
    return "";
}

void* bro_WhisperTokenizer_buildPrompt(void* /*self*/, void* /*opts*/) {
    return nullptr;
}

// --- Interface bro.stt.WhisperModel ---
void* bro_WhisperModel_create(void) {
    return new BroWhisperModelImpl();
}

void bro_WhisperModel_destroy(void* self) {
    delete static_cast<BroWhisperModelImpl*>(self);
}

bool bro_WhisperModel_get_loaded(void* self) {
    auto* m = static_cast<BroWhisperModelImpl*>(self);
    return m ? m->loaded : false;
}

const char* bro_WhisperModel_get_device(void* self) {
    auto* m = static_cast<BroWhisperModelImpl*>(self);
    return m ? m->device.c_str() : "CPU";
}

void* bro_WhisperModel_transcribe(void* /*self*/, void* /*audio*/, void* /*promptOrOpts*/, void* /*opts*/) {
    return nullptr;
}

void* bro_WhisperModel_createSession(void* /*self*/) {
    return new BroWhisperSessionImpl();
}

// --- Interface bro.stt.WhisperSession ---
void* bro_WhisperSession_create(void) {
    return new BroWhisperSessionImpl();
}

void bro_WhisperSession_destroy(void* self) {
    delete static_cast<BroWhisperSessionImpl*>(self);
}

bool bro_WhisperSession_get_loaded(void* self) {
    auto* s = static_cast<BroWhisperSessionImpl*>(self);
    return s ? s->loaded : false;
}

void* bro_WhisperSession_transcribe(void* /*self*/, void* /*audio*/, void* /*promptOrOpts*/, void* /*opts*/) {
    return nullptr;
}

void bro_WhisperSession_reset(void* /*self*/) {
}

// --- Interface bro.stt.ParakeetTokenizer ---
void* bro_ParakeetTokenizer_create(void) {
    return new BroParakeetTokenizerImpl();
}

void bro_ParakeetTokenizer_destroy(void* self) {
    delete static_cast<BroParakeetTokenizerImpl*>(self);
}

bool bro_ParakeetTokenizer_get_loaded(void* self) {
    auto* t = static_cast<BroParakeetTokenizerImpl*>(self);
    return t ? t->loaded : false;
}

void* bro_ParakeetTokenizer_encode(void* /*self*/, const char* /*text*/) {
    return nullptr;
}

const char* bro_ParakeetTokenizer_decode(void* /*self*/, void* /*tokenIds*/) {
    return "";
}

// --- Interface bro.stt.ParakeetModel ---
void* bro_ParakeetModel_create(void) {
    return new BroParakeetModelImpl();
}

void bro_ParakeetModel_destroy(void* self) {
    delete static_cast<BroParakeetModelImpl*>(self);
}

bool bro_ParakeetModel_get_loaded(void* self) {
    auto* m = static_cast<BroParakeetModelImpl*>(self);
    return m ? m->loaded : false;
}

const char* bro_ParakeetModel_get_device(void* self) {
    auto* m = static_cast<BroParakeetModelImpl*>(self);
    return m ? m->device.c_str() : "CPU";
}

void* bro_ParakeetModel_transcribe(void* /*self*/, void* /*audio*/, void* /*opts*/) {
    return nullptr;
}

void* bro_ParakeetModel_createSession(void* /*self*/) {
    return new BroParakeetSessionImpl();
}

// --- Interface bro.stt.ParakeetSession ---
void* bro_ParakeetSession_create(void) {
    return new BroParakeetSessionImpl();
}

void bro_ParakeetSession_destroy(void* self) {
    delete static_cast<BroParakeetSessionImpl*>(self);
}

bool bro_ParakeetSession_get_loaded(void* self) {
    auto* s = static_cast<BroParakeetSessionImpl*>(self);
    return s ? s->loaded : false;
}

void* bro_ParakeetSession_transcribe(void* /*self*/, void* /*audio*/, void* /*opts*/) {
    return nullptr;
}

void bro_ParakeetSession_reset(void* /*self*/) {
}

// --- Interface bro.stt.QwenAsrModel ---
void* bro_QwenAsrModel_create(void) {
    return new BroQwenAsrModelImpl();
}

void bro_QwenAsrModel_destroy(void* self) {
    delete static_cast<BroQwenAsrModelImpl*>(self);
}

bool bro_QwenAsrModel_get_loaded(void* self) {
    auto* m = static_cast<BroQwenAsrModelImpl*>(self);
    return m ? m->loaded : false;
}

const char* bro_QwenAsrModel_get_device(void* self) {
    auto* m = static_cast<BroQwenAsrModelImpl*>(self);
    return m ? m->device.c_str() : "CPU";
}

void* bro_QwenAsrModel_transcribe(void* /*self*/, void* /*audio*/, void* /*opts*/) {
    return nullptr;
}

void* bro_QwenAsrModel_createSession(void* /*self*/) {
    return new BroQwenAsrSessionImpl();
}

// --- Interface bro.stt.QwenAsrSession ---
void* bro_QwenAsrSession_create(void) {
    return new BroQwenAsrSessionImpl();
}

void bro_QwenAsrSession_destroy(void* self) {
    delete static_cast<BroQwenAsrSessionImpl*>(self);
}

bool bro_QwenAsrSession_get_loaded(void* self) {
    auto* s = static_cast<BroQwenAsrSessionImpl*>(self);
    return s ? s->loaded : false;
}

void* bro_QwenAsrSession_transcribe(void* /*self*/, void* /*audio*/, void* /*opts*/) {
    return nullptr;
}

void bro_QwenAsrSession_reset(void* /*self*/) {
}

// --- Interface bro.stt.QwenAsrStream ---
void* bro_QwenAsrStream_create(void) {
    return new BroQwenAsrStreamImpl();
}

void bro_QwenAsrStream_destroy(void* self) {
    delete static_cast<BroQwenAsrStreamImpl*>(self);
}

bool bro_QwenAsrStream_get_loaded(void* self) {
    auto* s = static_cast<BroQwenAsrStreamImpl*>(self);
    return s ? s->loaded : false;
}

void bro_QwenAsrStream_feed(void* /*self*/, void* /*audio*/) {
}

void* bro_QwenAsrStream_finish(void* /*self*/) {
    return nullptr;
}

// --- Namespace bro.stt ---
void bro_stt_init(void) {
    const auto* b = bro_get_stt_bridge();
    if (b && b->init) b->init();
}

void* bro_stt_loadWhisper(const char* dir, void* opts) {
    const auto* b = bro_get_stt_bridge();
    if (b && b->loadWhisper) return b->loadWhisper(dir, opts);
    return new BroWhisperModelImpl();
}

void* bro_stt_loadTokenizer(void* opts) {
    const auto* b = bro_get_stt_bridge();
    if (b && b->loadTokenizer) return b->loadTokenizer(opts);
    return new BroWhisperTokenizerImpl();
}

void* bro_stt_loadParakeet(const char* dir, void* opts) {
    const auto* b = bro_get_stt_bridge();
    if (b && b->loadParakeet) return b->loadParakeet(dir, opts);
    return new BroParakeetModelImpl();
}

void* bro_stt_loadParakeetTokenizer(const char* path, void* opts) {
    const auto* b = bro_get_stt_bridge();
    if (b && b->loadParakeetTokenizer) return b->loadParakeetTokenizer(path, opts);
    return new BroParakeetTokenizerImpl();
}

void* bro_stt_loadQwenAsr(const char* dir, void* opts) {
    const auto* b = bro_get_stt_bridge();
    if (b && b->loadQwenAsr) return b->loadQwenAsr(dir, opts);
    return new BroQwenAsrModelImpl();
}

void* bro_stt_loadQwenAsrStream(const char* dir, void* opts) {
    const auto* b = bro_get_stt_bridge();
    if (b && b->loadQwenAsrStream) return b->loadQwenAsrStream(dir, opts);
    return new BroQwenAsrStreamImpl();
}

void* bro_stt_transcribe(void* model, void* audio, void* promptOrOpts, void* opts) {
    const auto* b = bro_get_stt_bridge();
    if (b && b->transcribe) return b->transcribe(model, audio, promptOrOpts, opts);
    return nullptr;
}

}
