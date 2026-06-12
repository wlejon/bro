#pragma once

extern "C" {
#include "quickjs.h"
}

namespace broaudio { class Engine; }
namespace bro::engine { class AudioInference; }

namespace bro::js {

// Install the `bro.sense` namespace — the tier-0 acoustic sensor bus
// (brosoundml::SensorHub): always-on, model-free, per-frame DSP sensors over
// one shared PCEN mel front-end. level/VAD, spectral-flux onset, and
// autocorrelation tonality, each published into a single lock-free snapshot.
//
// bro.sense is the fast layer of the listening stack: it tells the app
// "something is happening" (a transient, a voice, a whistle) within one mel
// frame, so heavier tenants (bro.wake, bro.kws, streaming STT) can be gated,
// confirmed, or fused against it.
//
// Same engine plumbing as bro.wake/bro.kws, minus the result ring — the hub's
// snapshot is itself the lock-free cross-thread surface, so there is no tick:
//   - audio thread: the tap callback copies resampled mic samples into a
//     lock-free ring. No AGC — the level sensor is the stack's one absolute-
//     loudness signal, and the PCEN-derived sensors are gain-robust anyway.
//   - inference thread (engine::AudioInference worker; headless: the calling
//     thread via stepInline): drains the ring, runs SensorHub::feed().
//   - main thread (or any thread): bro.sense.snapshot() — a seqlock read.
//     Momentary booleans are paired with monotonic counters, so polling at
//     frame rate still observes every onset/voice/tonal event.
void installSenseBindings(JSContext* ctx, broaudio::Engine* audioEngine,
                          engine::AudioInference* inference);

// Symmetric cleanup hook. Detaches the mic tap, unregisters the inference
// task, and drops the hub. Safe to call multiple times.
void cleanupSenseBindings(JSContext* ctx);

}  // namespace bro::js
