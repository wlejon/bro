// =============================================================================
// bro_sense_c_abi.cpp — C++ forwarding implementations for bro.sense
// Generated automatically by brosurface (gen/emit_c_abi.mjs).
// =============================================================================

#include "bro/c_abi/bro_sense_c_abi.h"
#include "bro/c_abi/bro_engine_c_abi.h"
#include <cstdint>
#include <string>

struct BroSenseStreamViewImpl {
    bool active = false;
    int32_t sampleRate = 16000;
};

extern "C" {

// --- Interface bro.sense.SenseStreamView ---
void* bro_SenseStreamView_create(void) {
    return new BroSenseStreamViewImpl();
}

void bro_SenseStreamView_destroy(void* self) {
    delete static_cast<BroSenseStreamViewImpl*>(self);
}

bool bro_SenseStreamView_get_active(void* self) {
    auto* v = static_cast<BroSenseStreamViewImpl*>(self);
    return v ? v->active : false;
}

void bro_SenseStreamView_start(void* self, void* /*opts*/) {
    auto* v = static_cast<BroSenseStreamViewImpl*>(self);
    if (v) v->active = true;
}

void bro_SenseStreamView_stop(void* self) {
    auto* v = static_cast<BroSenseStreamViewImpl*>(self);
    if (v) v->active = false;
}

bool bro_SenseStreamView_isActive(void* self) {
    auto* v = static_cast<BroSenseStreamViewImpl*>(self);
    return v ? v->active : false;
}

void* bro_SenseStreamView_snapshot(void* /*self*/) {
    return nullptr;
}

int32_t bro_SenseStreamView_sampleRate(void* self) {
    auto* v = static_cast<BroSenseStreamViewImpl*>(self);
    return v ? v->sampleRate : 16000;
}

void* bro_SenseStreamView_stats(void* /*self*/) {
    return nullptr;
}

void* bro_SenseStreamView_feed(void* /*self*/, void* /*samples*/) {
    return nullptr;
}

void* bro_SenseStreamView_analyze(void* /*self*/, void* /*samples*/, void* /*opts*/) {
    return nullptr;
}

// --- Namespace bro.sense ---
void bro_sense_init(void) {
    const auto* b = bro_get_sense_bridge();
    if (b && b->init) b->init();
}

void bro_sense_start(void* opts) {
    const auto* b = bro_get_sense_bridge();
    if (b && b->start) b->start(opts);
}

void bro_sense_stop(void) {
    const auto* b = bro_get_sense_bridge();
    if (b && b->stop) b->stop();
}

bool bro_sense_isActive(void) {
    const auto* b = bro_get_sense_bridge();
    if (b && b->isActive) return b->isActive();
    return false;
}

void* bro_sense_snapshot(void) {
    const auto* b = bro_get_sense_bridge();
    if (b && b->snapshot) return b->snapshot();
    return nullptr;
}

int32_t bro_sense_sampleRate(void) {
    const auto* b = bro_get_sense_bridge();
    if (b && b->sampleRate) return b->sampleRate();
    return 16000;
}

void* bro_sense_stats(void) {
    const auto* b = bro_get_sense_bridge();
    if (b && b->stats) return b->stats();
    return nullptr;
}

void* bro_sense_feed(void* samples) {
    const auto* b = bro_get_sense_bridge();
    if (b && b->feed) return b->feed(samples);
    return nullptr;
}

void* bro_sense_analyze(void* samples, void* opts) {
    const auto* b = bro_get_sense_bridge();
    if (b && b->analyze) return b->analyze(samples, opts);
    return nullptr;
}

}
