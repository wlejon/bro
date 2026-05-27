#pragma once

extern "C" {
#include "quickjs.h"
}

namespace broaudio { class Engine; }

namespace bro::js {

// Install the `bro.wake` namespace — streaming wake-word detection via
// brosoundml::WakeWord. The detector runs inline on the broaudio mic
// callback thread (~0.26 ms / frame on CPU); detected events are queued
// to an atomic counter and drained on the main thread by tickWake() so
// the JS onFire callback always runs single-threaded.
//
// The engine owns the broaudio::Engine instance; the binding holds a raw
// pointer to it for the duration of the JS runtime. broaudio::Engine
// outlives the JS context — both are torn down by Engine's destructor in
// reverse construction order.
void installWakeBindings(JSContext* ctx, broaudio::Engine* audioEngine);

// Per-frame pump. Drains the atomic fire counter set by the audio-thread
// feed() and invokes the stored JS onFire callback once per pending fire.
// Cheap when nothing is pending. Call from the main thread per frame.
void tickWake(JSContext* ctx);

// Symmetric cleanup hook. Detaches any installed mic callback and frees
// the stored JS onFire reference. Safe to call multiple times.
void cleanupWakeBindings(JSContext* ctx);

}  // namespace bro::js
