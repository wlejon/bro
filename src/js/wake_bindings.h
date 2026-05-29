#pragma once

extern "C" {
#include "quickjs.h"
}

namespace broaudio { class Engine; }
namespace bro::engine { class AudioInference; }

namespace bro::js {

// Install the `bro.wake` namespace — streaming wake-word detection via
// brosoundml::WakeWord. bro.wake is a thin tenant of the engine's audio-
// inference subsystem: listen() registers the model + a lock-free PCM ring
// with `inference`, and a broaudio mic tap writes the ring on the audio thread.
//
// All three concerns live in their own thread:
//   - audio thread: the tap callback copies resampled + AGC'd mic samples into
//     the ring. Nothing else — no model, no GPU, no heap.
//   - inference thread (engine::AudioInference worker; or the calling thread in
//     headless via stepInline): drains the ring and runs WakeWord::feed(),
//     publishing a fire count via an atomic.
//   - main thread: tickWake() drains that atomic and invokes onFire.
//
// The engine owns both broaudio::Engine and engine::AudioInference; the binding
// holds raw pointers for the lifetime of the JS runtime. Both outlive the JS
// context and are torn down (inference worker joined first) by Engine's
// destructor.
void installWakeBindings(JSContext* ctx, broaudio::Engine* audioEngine,
                         engine::AudioInference* inference);

// Per-frame pump. Drains the atomic fire counter published by the inference
// thread and invokes the stored JS onFire callback once per pending fire.
// Cheap when nothing is pending. Call from the main thread per frame, after the
// inference subsystem has been pumped (signalPump in windowed, stepInline in
// headless).
void tickWake(JSContext* ctx);

// Symmetric cleanup hook. Detaches the mic tap, unregisters the inference task,
// and frees the stored JS onFire reference. Safe to call multiple times.
void cleanupWakeBindings(JSContext* ctx);

}  // namespace bro::js
