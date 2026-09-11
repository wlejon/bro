// =============================================================================
// bro_web_animations_c_abi.cpp — C++ forwarding implementations for bro.web_animations
// Generated automatically by brosurface (gen/emit_c_abi.mjs).
// =============================================================================

#include "bro/c_abi/bro_web_animations_c_abi.h"
#include <cstdint>
#include <string>

struct BroAnimationImpl {
    double currentTime = 0.0;
    double playbackRate = 1.0;
    std::string playState = "idle";
    bool pending = false;
    void* onfinish = nullptr;
    void* oncancel = nullptr;
};

struct BroWebAnimationsImpl {
};

extern "C" {

// --- Interface bro.web_animations.Animation ---
void* bro_Animation_create(void) {
    return new BroAnimationImpl();
}

void bro_Animation_destroy(void* self) {
    delete static_cast<BroAnimationImpl*>(self);
}

double bro_Animation_get_currentTime(void* self) {
    auto* a = static_cast<BroAnimationImpl*>(self);
    return a ? a->currentTime : 0.0;
}

void bro_Animation_set_currentTime(void* self, double val) {
    auto* a = static_cast<BroAnimationImpl*>(self);
    if (a) a->currentTime = val;
}

double bro_Animation_get_playbackRate(void* self) {
    auto* a = static_cast<BroAnimationImpl*>(self);
    return a ? a->playbackRate : 1.0;
}

void bro_Animation_set_playbackRate(void* self, double val) {
    auto* a = static_cast<BroAnimationImpl*>(self);
    if (a) a->playbackRate = val;
}

const char* bro_Animation_get_playState(void* self) {
    auto* a = static_cast<BroAnimationImpl*>(self);
    return a ? a->playState.c_str() : "idle";
}

bool bro_Animation_get_pending(void* self) {
    auto* a = static_cast<BroAnimationImpl*>(self);
    return a ? a->pending : false;
}

void* bro_Animation_get_finished(void* /*self*/) {
    return nullptr;
}

void* bro_Animation_get_ready(void* /*self*/) {
    return nullptr;
}

void* bro_Animation_get_onfinish(void* self) {
    auto* a = static_cast<BroAnimationImpl*>(self);
    return a ? a->onfinish : nullptr;
}

void bro_Animation_set_onfinish(void* self, void* val) {
    auto* a = static_cast<BroAnimationImpl*>(self);
    if (a) a->onfinish = val;
}

void* bro_Animation_get_oncancel(void* self) {
    auto* a = static_cast<BroAnimationImpl*>(self);
    return a ? a->oncancel : nullptr;
}

void bro_Animation_set_oncancel(void* self, void* val) {
    auto* a = static_cast<BroAnimationImpl*>(self);
    if (a) a->oncancel = val;
}

void bro_Animation_play(void* self) {
    auto* a = static_cast<BroAnimationImpl*>(self);
    if (a) a->playState = "running";
}

void bro_Animation_pause(void* self) {
    auto* a = static_cast<BroAnimationImpl*>(self);
    if (a) a->playState = "paused";
}

void bro_Animation_finish(void* self) {
    auto* a = static_cast<BroAnimationImpl*>(self);
    if (a) a->playState = "finished";
}

void bro_Animation_cancel(void* self) {
    auto* a = static_cast<BroAnimationImpl*>(self);
    if (a) a->playState = "idle";
}

void bro_Animation_reverse(void* self) {
    auto* a = static_cast<BroAnimationImpl*>(self);
    if (a) {
        a->playbackRate = -a->playbackRate;
        a->playState = "running";
    }
}

// --- Interface bro.web_animations.WebAnimations ---
void* bro_WebAnimations_create(void) {
    return new BroWebAnimationsImpl();
}

void bro_WebAnimations_destroy(void* self) {
    delete static_cast<BroWebAnimationsImpl*>(self);
}

}
