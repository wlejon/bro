// =============================================================================
// bro_rave_c_abi.cpp — C++ forwarding implementations for bro.rave
// Generated automatically by brosurface (gen/emit_c_abi.mjs).
// =============================================================================

#include "bro/c_abi/bro_rave_c_abi.h"
#include "bro/c_abi/bro_engine_c_abi.h"
#include <cstdint>
#include <string>

struct BroRaveImpl {
    bool loaded = true;
    int32_t sampleRate = 48000;
    int32_t nLatent = 8;
    int32_t fullLatent = 16;
    int32_t nBand = 16;
    int32_t totalRatio = 2048;
};

extern "C" {

// --- Interface bro.rave.Rave ---
void* bro_Rave_create(void) {
    return new BroRaveImpl();
}

void bro_Rave_destroy(void* self) {
    delete static_cast<BroRaveImpl*>(self);
}

bool bro_Rave_get_loaded(void* self) {
    auto* r = static_cast<BroRaveImpl*>(self);
    return r ? r->loaded : false;
}

int32_t bro_Rave_get_sampleRate(void* self) {
    auto* r = static_cast<BroRaveImpl*>(self);
    return r ? r->sampleRate : 48000;
}

int32_t bro_Rave_get_nLatent(void* self) {
    auto* r = static_cast<BroRaveImpl*>(self);
    return r ? r->nLatent : 8;
}

int32_t bro_Rave_get_fullLatent(void* self) {
    auto* r = static_cast<BroRaveImpl*>(self);
    return r ? r->fullLatent : 16;
}

int32_t bro_Rave_get_nBand(void* self) {
    auto* r = static_cast<BroRaveImpl*>(self);
    return r ? r->nBand : 16;
}

int32_t bro_Rave_get_totalRatio(void* self) {
    auto* r = static_cast<BroRaveImpl*>(self);
    return r ? r->totalRatio : 2048;
}

void* bro_Rave_encode(void* /*self*/, void* /*audio*/) {
    return nullptr;
}

void* bro_Rave_decode(void* /*self*/, void* /*latent*/, int32_t /*frames*/, void* /*opts*/) {
    return nullptr;
}

// --- Namespace bro.rave ---
void bro_rave_init(void) {
    const auto* b = bro_get_rave_bridge();
    if (b && b->init) b->init();
}

void* bro_rave_loadRave(const char* modelDir, void* opts) {
    const auto* b = bro_get_rave_bridge();
    if (b && b->loadRave) return b->loadRave(modelDir, opts);
    return new BroRaveImpl();
}

}
