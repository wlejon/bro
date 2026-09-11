// =============================================================================
// bro_animation_c_abi.cpp — C++ forwarding implementations for bro.animation
// Generated automatically by brosurface (gen/emit_c_abi.mjs).
// =============================================================================

#include "bro/c_abi/bro_animation_c_abi.h"
#include <cstdint>
#include <string>

struct BroTweenImpl {
    bool isRunning = false;
    bool isPaused = false;
    int32_t loops = 1;
};

struct BroAnimationPlayerImpl {
    bool playing = false;
    double currentTime = 0.0;
    std::string currentClip;
    double speed = 1.0;
};

extern "C" {

// --- Interface bro.animation.Tween ---
void* bro_Tween_create(void) {
    return new BroTweenImpl();
}

void bro_Tween_destroy(void* self) {
    delete static_cast<BroTweenImpl*>(self);
}

void* bro_Tween_to(void* self, void* /*target*/, void* /*props*/, double /*duration*/, const char* /*easing*/) {
    return self;
}

void* bro_Tween_parallel(void* self, void* /*tweens*/) {
    return self;
}

void* bro_Tween_call(void* self, void* /*callback*/) {
    return self;
}

void* bro_Tween_loop(void* self, int32_t count) {
    auto* t = static_cast<BroTweenImpl*>(self);
    if (t) t->loops = count;
    return self;
}

void* bro_Tween_start(void* self) {
    auto* t = static_cast<BroTweenImpl*>(self);
    if (t) { t->isRunning = true; t->isPaused = false; }
    return self;
}

void* bro_Tween_stop(void* self) {
    auto* t = static_cast<BroTweenImpl*>(self);
    if (t) { t->isRunning = false; t->isPaused = false; }
    return self;
}

void* bro_Tween_pause(void* self) {
    auto* t = static_cast<BroTweenImpl*>(self);
    if (t) { t->isPaused = true; }
    return self;
}

void* bro_Tween_resume(void* self) {
    auto* t = static_cast<BroTweenImpl*>(self);
    if (t) { t->isPaused = false; }
    return self;
}

// --- Interface bro.animation.AnimationPlayer ---
void* bro_AnimationPlayer_create(void) {
    return new BroAnimationPlayerImpl();
}

void bro_AnimationPlayer_destroy(void* self) {
    delete static_cast<BroAnimationPlayerImpl*>(self);
}

void bro_AnimationPlayer_addClip(void* /*self*/, const char* /*name*/, void* /*clip*/) {
}

void bro_AnimationPlayer_clipDef(void* /*self*/, const char* /*name*/, void* /*def*/) {
}

void bro_AnimationPlayer_play(void* self, const char* clipName, void* /*opts*/) {
    auto* p = static_cast<BroAnimationPlayerImpl*>(self);
    if (p) {
        p->playing = true;
        p->currentClip = clipName ? clipName : "";
    }
}

void bro_AnimationPlayer_pause(void* self) {
    auto* p = static_cast<BroAnimationPlayerImpl*>(self);
    if (p) p->playing = false;
}

void bro_AnimationPlayer_resume(void* self) {
    auto* p = static_cast<BroAnimationPlayerImpl*>(self);
    if (p) p->playing = true;
}

void bro_AnimationPlayer_stop(void* self) {
    auto* p = static_cast<BroAnimationPlayerImpl*>(self);
    if (p) {
        p->playing = false;
        p->currentTime = 0.0;
    }
}

void bro_AnimationPlayer_seek(void* self, double time) {
    auto* p = static_cast<BroAnimationPlayerImpl*>(self);
    if (p) p->currentTime = time;
}

} // extern "C"