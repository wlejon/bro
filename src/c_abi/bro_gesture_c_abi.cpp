// =============================================================================
// bro_gesture_c_abi.cpp — C++ forwarding implementations for bro.gesture
// Generated automatically by brosurface (gen/emit_c_abi.mjs).
// =============================================================================

#include "bro/c_abi/bro_gesture_c_abi.h"
#include "bro/c_abi/bro_engine_c_abi.h"
#include <cstdint>
#include <string>

struct BroGestureStreamViewImpl {
    bool active = false;
    int32_t sampleRate = 16000;
};

extern "C" {

// --- Interface bro.gesture.GestureStreamView ---
void* bro_GestureStreamView_create(void) {
    return new BroGestureStreamViewImpl();
}

void bro_GestureStreamView_destroy(void* self) {
    delete static_cast<BroGestureStreamViewImpl*>(self);
}

bool bro_GestureStreamView_get_active(void* self) {
    auto* v = static_cast<BroGestureStreamViewImpl*>(self);
    return v ? v->active : false;
}

int32_t bro_GestureStreamView_enrollFromAudio(void* /*self*/, const char* /*name*/, void* /*samples*/, void* /*policy*/) {
    return 0;
}

bool bro_GestureStreamView_remove(void* /*self*/, const char* /*name*/) {
    return false;
}

void bro_GestureStreamView_clear(void* /*self*/) {
}

void* bro_GestureStreamView_templates(void* /*self*/) {
    return nullptr;
}

void* bro_GestureStreamView_inspect(void* /*self*/, const char* /*name*/) {
    return nullptr;
}

void bro_GestureStreamView_reset(void* /*self*/) {
}

void bro_GestureStreamView_listen(void* self, void* /*opts*/) {
    auto* v = static_cast<BroGestureStreamViewImpl*>(self);
    if (v) v->active = true;
}

void bro_GestureStreamView_stop(void* self) {
    auto* v = static_cast<BroGestureStreamViewImpl*>(self);
    if (v) v->active = false;
}

bool bro_GestureStreamView_isActive(void* self) {
    auto* v = static_cast<BroGestureStreamViewImpl*>(self);
    return v ? v->active : false;
}

int32_t bro_GestureStreamView_sampleRate(void* self) {
    auto* v = static_cast<BroGestureStreamViewImpl*>(self);
    return v ? v->sampleRate : 16000;
}

// --- Namespace bro.gesture ---
void bro_gesture_init(void) {
    const auto* b = bro_get_gesture_bridge();
    if (b && b->init) b->init();
}

int32_t bro_gesture_enrollFromAudio(const char* name, void* samples, void* policy) {
    const auto* b = bro_get_gesture_bridge();
    if (b && b->enrollFromAudio) return b->enrollFromAudio(name, samples, policy);
    return 0;
}

bool bro_gesture_remove(const char* name) {
    const auto* b = bro_get_gesture_bridge();
    if (b && b->remove) return b->remove(name);
    return false;
}

void bro_gesture_clear(void) {
    const auto* b = bro_get_gesture_bridge();
    if (b && b->clear) b->clear();
}

void* bro_gesture_templates(void) {
    const auto* b = bro_get_gesture_bridge();
    if (b && b->templates) return b->templates();
    return nullptr;
}

void* bro_gesture_inspect(const char* name) {
    const auto* b = bro_get_gesture_bridge();
    if (b && b->inspect) return b->inspect(name);
    return nullptr;
}

void bro_gesture_reset(void) {
    const auto* b = bro_get_gesture_bridge();
    if (b && b->reset) b->reset();
}

void bro_gesture_listen(void* opts) {
    const auto* b = bro_get_gesture_bridge();
    if (b && b->listen) b->listen(opts);
}

void bro_gesture_stop(void) {
    const auto* b = bro_get_gesture_bridge();
    if (b && b->stop) b->stop();
}

bool bro_gesture_isActive(void) {
    const auto* b = bro_get_gesture_bridge();
    if (b && b->isActive) return b->isActive();
    return false;
}

int32_t bro_gesture_sampleRate(void) {
    const auto* b = bro_get_gesture_bridge();
    if (b && b->sampleRate) return b->sampleRate();
    return 16000;
}

}
