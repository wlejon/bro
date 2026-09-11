// =============================================================================
// bro_wake_c_abi.cpp — C++ forwarding implementations for bro.wake
// Generated automatically by brosurface (gen/emit_c_abi.mjs).
// =============================================================================

#include "bro/c_abi/bro_wake_c_abi.h"
#include "bro/c_abi/bro_engine_c_abi.h"
#include <cstdint>
#include <string>

struct BroWakeStreamViewImpl {
    bool active = false;
    bool suspended = false;
    bool loaded = true;
    double lastScore = 0.0;
};

extern "C" {

// --- Interface bro.wake.WakeStreamView ---
void* bro_WakeStreamView_create(void) {
    return new BroWakeStreamViewImpl();
}

void bro_WakeStreamView_destroy(void* self) {
    delete static_cast<BroWakeStreamViewImpl*>(self);
}

bool bro_WakeStreamView_get_active(void* self) {
    auto* v = static_cast<BroWakeStreamViewImpl*>(self);
    return v ? v->active : false;
}

void bro_WakeStreamView_listen(void* self, void* /*opts*/) {
    auto* v = static_cast<BroWakeStreamViewImpl*>(self);
    if (v) v->active = true;
}

void bro_WakeStreamView_stop(void* self) {
    auto* v = static_cast<BroWakeStreamViewImpl*>(self);
    if (v) v->active = false;
}

void bro_WakeStreamView_suspend(void* self) {
    auto* v = static_cast<BroWakeStreamViewImpl*>(self);
    if (v) v->suspended = true;
}

void bro_WakeStreamView_resume(void* self) {
    auto* v = static_cast<BroWakeStreamViewImpl*>(self);
    if (v) v->suspended = false;
}

double bro_WakeStreamView_lastScore(void* self) {
    auto* v = static_cast<BroWakeStreamViewImpl*>(self);
    return v ? v->lastScore : 0.0;
}

bool bro_WakeStreamView_isActive(void* self) {
    auto* v = static_cast<BroWakeStreamViewImpl*>(self);
    return v ? v->active : false;
}

bool bro_WakeStreamView_isSuspended(void* self) {
    auto* v = static_cast<BroWakeStreamViewImpl*>(self);
    return v ? v->suspended : false;
}

bool bro_WakeStreamView_isLoaded(void* self) {
    auto* v = static_cast<BroWakeStreamViewImpl*>(self);
    return v ? v->loaded : false;
}

void bro_WakeStreamView_setThreshold(void* /*self*/, double /*threshold*/) {
}

void* bro_WakeStreamView_stats(void* /*self*/) {
    return nullptr;
}

void* bro_WakeStreamView_feed(void* /*self*/, void* /*samples*/, int32_t /*sampleRate*/) {
    return nullptr;
}

// --- Namespace bro.wake ---
void bro_wake_init(void) {
    const auto* b = bro_get_wake_bridge();
    if (b && b->init) b->init();
}

void bro_wake_load(void* opts) {
    const auto* b = bro_get_wake_bridge();
    if (b && b->load) b->load(opts);
}

void bro_wake_unload(void) {
    const auto* b = bro_get_wake_bridge();
    if (b && b->unload) b->unload();
}

void bro_wake_listen(void* opts) {
    const auto* b = bro_get_wake_bridge();
    if (b && b->listen) b->listen(opts);
}

void bro_wake_stop(void) {
    const auto* b = bro_get_wake_bridge();
    if (b && b->stop) b->stop();
}

void bro_wake_suspend(void) {
    const auto* b = bro_get_wake_bridge();
    if (b && b->suspend) b->suspend();
}

void bro_wake_resume(void) {
    const auto* b = bro_get_wake_bridge();
    if (b && b->resume) b->resume();
}

double bro_wake_lastScore(void) {
    const auto* b = bro_get_wake_bridge();
    if (b && b->lastScore) return b->lastScore();
    return 0.0;
}

bool bro_wake_isActive(void) {
    const auto* b = bro_get_wake_bridge();
    if (b && b->isActive) return b->isActive();
    return false;
}

bool bro_wake_isSuspended(void) {
    const auto* b = bro_get_wake_bridge();
    if (b && b->isSuspended) return b->isSuspended();
    return false;
}

bool bro_wake_isLoaded(void) {
    const auto* b = bro_get_wake_bridge();
    if (b && b->isLoaded) return b->isLoaded();
    return true;
}

void bro_wake_setThreshold(double threshold) {
    const auto* b = bro_get_wake_bridge();
    if (b && b->setThreshold) b->setThreshold(threshold);
}

void* bro_wake_stats(void) {
    const auto* b = bro_get_wake_bridge();
    if (b && b->stats) return b->stats();
    return nullptr;
}

void* bro_wake_feed(void* samples, int32_t sampleRate) {
    const auto* b = bro_get_wake_bridge();
    if (b && b->feed) return b->feed(samples, sampleRate);
    return nullptr;
}

}
