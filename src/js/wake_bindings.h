#pragma once

extern "C" {
#include "quickjs.h"
}

namespace broaudio { class Engine; }
namespace bro::engine { class AudioInference; }

namespace bro::js {

// Install the `bro.wake` namespace — streaming wake-word detection via
// brosoundml::WakeWord. bro.wake is a tenant of the engine's SHARED listen
// host (listen_host.h): listen() attaches the model as a ListenBus member,
// so one raw (no-AGC) tap + one PCEN mel pass serve it alongside
// bro.kws / bro.sense. The AGC-free trained model is level-invariant — it
// hears the same raw stream as the rest of the stack.
//
// All three concerns live in their own thread:
//   - audio thread: the host's tap callback copies resampled raw mic samples
//     into the shared ring. Nothing else — no model, no GPU, no heap.
//   - inference thread (engine::AudioInference worker; or the calling thread
//     in headless via stepInline): the host's task drains the ring and runs
//     the bus (mel → WakeWord::feed_mel), publishing a fire count via an
//     atomic through this binding's hook.
//   - main thread: tickWake() drains that atomic and invokes onFire.
//
// The engine owns both broaudio::Engine and engine::AudioInference; the binding
// holds raw pointers for the lifetime of the JS runtime. Both outlive the JS
// context and are torn down (inference worker joined first) by Engine's
// destructor.
void installWakeBindings(JSContext* ctx, broaudio::Engine* audioEngine,
                         engine::AudioInference* inference);

// Per-frame result delivery. Drains the atomic fire counter published by the
// inference thread and invokes the stored JS onFire callback once per pending
// fire. Cheap when nothing is pending. Call from the main thread per frame. This
// only delivers fires — the self-paced inference worker (windowed) drains the
// mic ring and runs the model on its own clock; in headless, stepInline() drives
// the pump on the calling thread.
void tickWake(JSContext* ctx);

// Symmetric cleanup hook. Detaches from the listen host (which tears down
// the shared tap/task when bro.wake was the last member) and frees the stored
// JS onFire reference. Safe to call multiple times.
void cleanupWakeBindings(JSContext* ctx);

}  // namespace bro::js
