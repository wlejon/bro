// =============================================================================
// bro_kws_c_abi.cpp — C++ forwarding implementations for bro.kws
// Generated automatically by brosurface (gen/emit_c_abi.mjs).
// =============================================================================

#include "bro/c_abi/bro_kws_c_abi.h"
#include "bro/c_abi/bro_engine_c_abi.h"
#include <cstdint>
#include <string>

struct BroKwsStreamViewImpl {
    bool active = false;
    bool suspended = false;
    bool loaded = true;
    int32_t sampleRate = 16000;
};

extern "C" {

// --- Interface bro.kws.KwsStreamView ---
void* bro_KwsStreamView_create(void) {
    return new BroKwsStreamViewImpl();
}

void bro_KwsStreamView_destroy(void* self) {
    delete static_cast<BroKwsStreamViewImpl*>(self);
}

bool bro_KwsStreamView_get_active(void* self) {
    auto* v = static_cast<BroKwsStreamViewImpl*>(self);
    return v ? v->active : false;
}

int32_t bro_KwsStreamView_enroll(void* /*self*/, const char* /*name*/, void* /*phonemeIds*/, void* /*policy*/) {
    return 0;
}

int32_t bro_KwsStreamView_enrollFromAudio(void* /*self*/, const char* /*name*/, void* /*samples*/, void* /*policy*/) {
    return 0;
}

int32_t bro_KwsStreamView_enrollFromClasses(void* /*self*/, const char* /*name*/, void* /*classIds*/, void* /*policy*/) {
    return 0;
}

void* bro_KwsStreamView_inspect(void* /*self*/, const char* /*name*/) {
    return nullptr;
}

bool bro_KwsStreamView_remove(void* /*self*/, const char* /*name*/) {
    return false;
}

void bro_KwsStreamView_clear(void* /*self*/) {
}

void* bro_KwsStreamView_templates(void* /*self*/) {
    return nullptr;
}

void bro_KwsStreamView_reset(void* /*self*/) {
}

void bro_KwsStreamView_listen(void* self, void* /*opts*/) {
    auto* v = static_cast<BroKwsStreamViewImpl*>(self);
    if (v) v->active = true;
}

void bro_KwsStreamView_stop(void* self) {
    auto* v = static_cast<BroKwsStreamViewImpl*>(self);
    if (v) v->active = false;
}

void bro_KwsStreamView_suspend(void* self) {
    auto* v = static_cast<BroKwsStreamViewImpl*>(self);
    if (v) v->suspended = true;
}

void bro_KwsStreamView_resume(void* self) {
    auto* v = static_cast<BroKwsStreamViewImpl*>(self);
    if (v) v->suspended = false;
}

bool bro_KwsStreamView_isActive(void* self) {
    auto* v = static_cast<BroKwsStreamViewImpl*>(self);
    return v ? v->active : false;
}

bool bro_KwsStreamView_isSuspended(void* self) {
    auto* v = static_cast<BroKwsStreamViewImpl*>(self);
    return v ? v->suspended : false;
}

bool bro_KwsStreamView_isLoaded(void* self) {
    auto* v = static_cast<BroKwsStreamViewImpl*>(self);
    return v ? v->loaded : false;
}

int32_t bro_KwsStreamView_sampleRate(void* self) {
    auto* v = static_cast<BroKwsStreamViewImpl*>(self);
    return v ? v->sampleRate : 16000;
}

double bro_KwsStreamView_prefixProgress(void* /*self*/) {
    return 0.0;
}

void* bro_KwsStreamView_progress(void* /*self*/) {
    return nullptr;
}

void* bro_KwsStreamView_posterior(void* /*self*/, int32_t /*topK*/) {
    return nullptr;
}

void* bro_KwsStreamView_stats(void* /*self*/) {
    return nullptr;
}

void* bro_KwsStreamView_feed(void* /*self*/, void* /*samples*/) {
    return nullptr;
}

// --- Namespace bro.kws ---
void bro_kws_init(void) {
    const auto* b = bro_get_kws_bridge();
    if (b && b->init) b->init();
}

void bro_kws_load(void* opts) {
    const auto* b = bro_get_kws_bridge();
    if (b && b->load) b->load(opts);
}

void bro_kws_unload(void) {
    const auto* b = bro_get_kws_bridge();
    if (b && b->unload) b->unload();
}

int32_t bro_kws_enroll(const char* name, void* phonemeIds, void* policy) {
    const auto* b = bro_get_kws_bridge();
    if (b && b->enroll) return b->enroll(name, phonemeIds, policy);
    return 0;
}

int32_t bro_kws_enrollFromAudio(const char* name, void* samples, void* policy) {
    const auto* b = bro_get_kws_bridge();
    if (b && b->enrollFromAudio) return b->enrollFromAudio(name, samples, policy);
    return 0;
}

int32_t bro_kws_enrollFromClasses(const char* name, void* classIds, void* policy) {
    const auto* b = bro_get_kws_bridge();
    if (b && b->enrollFromClasses) return b->enrollFromClasses(name, classIds, policy);
    return 0;
}

void* bro_kws_inspect(const char* name) {
    const auto* b = bro_get_kws_bridge();
    if (b && b->inspect) return b->inspect(name);
    return nullptr;
}

bool bro_kws_remove(const char* name) {
    const auto* b = bro_get_kws_bridge();
    if (b && b->remove) return b->remove(name);
    return false;
}

void bro_kws_clear(void) {
    const auto* b = bro_get_kws_bridge();
    if (b && b->clear) b->clear();
}

void* bro_kws_templates(void) {
    const auto* b = bro_get_kws_bridge();
    if (b && b->templates) return b->templates();
    return nullptr;
}

void bro_kws_reset(void) {
    const auto* b = bro_get_kws_bridge();
    if (b && b->reset) b->reset();
}

void bro_kws_listen(void* opts) {
    const auto* b = bro_get_kws_bridge();
    if (b && b->listen) b->listen(opts);
}

void bro_kws_stop(void) {
    const auto* b = bro_get_kws_bridge();
    if (b && b->stop) b->stop();
}

void bro_kws_suspend(void) {
    const auto* b = bro_get_kws_bridge();
    if (b && b->suspend) b->suspend();
}

void bro_kws_resume(void) {
    const auto* b = bro_get_kws_bridge();
    if (b && b->resume) b->resume();
}

bool bro_kws_isActive(void) {
    const auto* b = bro_get_kws_bridge();
    if (b && b->isActive) return b->isActive();
    return false;
}

bool bro_kws_isSuspended(void) {
    const auto* b = bro_get_kws_bridge();
    if (b && b->isSuspended) return b->isSuspended();
    return false;
}

bool bro_kws_isLoaded(void) {
    const auto* b = bro_get_kws_bridge();
    if (b && b->isLoaded) return b->isLoaded();
    return true;
}

int32_t bro_kws_sampleRate(void) {
    const auto* b = bro_get_kws_bridge();
    if (b && b->sampleRate) return b->sampleRate();
    return 16000;
}

double bro_kws_prefixProgress(void) {
    const auto* b = bro_get_kws_bridge();
    if (b && b->prefixProgress) return b->prefixProgress();
    return 0.0;
}

void* bro_kws_progress(void) {
    const auto* b = bro_get_kws_bridge();
    if (b && b->progress) return b->progress();
    return nullptr;
}

void* bro_kws_posterior(int32_t topK) {
    const auto* b = bro_get_kws_bridge();
    if (b && b->posterior) return b->posterior(topK);
    return nullptr;
}

void* bro_kws_stats(void) {
    const auto* b = bro_get_kws_bridge();
    if (b && b->stats) return b->stats();
    return nullptr;
}

void* bro_kws_feed(void* samples) {
    const auto* b = bro_get_kws_bridge();
    if (b && b->feed) return b->feed(samples);
    return nullptr;
}

}
