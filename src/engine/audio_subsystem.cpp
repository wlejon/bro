#include "engine/audio_subsystem.h"

#include "js/wake_bindings.h"
#include "js/kws_bindings.h"
#include "js/gesture_bindings.h"
#include "js/mic_bindings.h"

namespace bro::engine {

void tickAudioInferenceSubsystem(JSContext* ctx) {
#if BRO_WITH_SOUNDML
    if (!ctx) return;
    js::tickWake(ctx);
    js::tickKws(ctx);
    js::tickGesture(ctx);
#else
    (void)ctx;
#endif
}

void tickMicSubsystem(JSContext* ctx) {
    if (!ctx) return;
    js::tickMic(ctx);
}

} // namespace bro::engine
