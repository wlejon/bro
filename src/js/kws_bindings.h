#pragma once

extern "C" {
#include "quickjs.h"
}

namespace broaudio { class Engine; }
namespace bro::engine { class AudioInference; }

namespace bro::js {

// Install the `bro.kws` namespace — open-vocabulary streaming keyword spotting
// via brosoundml::PhonemeSpotter (the open-vocab analogue of bro.wake: enroll
// any phrase as a phoneme-sequence template instead of training one keyword
// in). bro.kws is a thin tenant of the engine's audio-inference subsystem with
// the same three-thread split as bro.wake:
//   - audio thread: the tap callback copies resampled mic samples into a
//     lock-free ring. Nothing else — no model, no GPU, no heap. (No AGC: the
//     spotter's PCEN mel front-end is loudness-robust by construction.)
//   - inference thread (engine::AudioInference worker; headless: the calling
//     thread via stepInline): drains the ring, runs PhonemeSpotter::feed(),
//     and publishes fired events into a lock-free SPSC slot ring.
//   - main thread: tickKws() drains the slots and invokes onSpot(name, conf).
//
// PhonemeSpotter's mutators (enroll/remove/clear/reset) are single-producer
// with feed(), so the binding only permits them while NOT listening — enroll
// templates first, then listen(); stop() to re-enroll.
void installKwsBindings(JSContext* ctx, broaudio::Engine* audioEngine,
                        engine::AudioInference* inference);

// Per-frame result delivery. Drains the SPSC event slots published by the
// inference thread and invokes the stored JS onSpot callback once per event.
// Cheap when nothing is pending. Call from the main thread per frame.
void tickKws(JSContext* ctx);

// Symmetric cleanup hook. Detaches the mic tap, unregisters the inference
// task, drops the spotter, and frees the stored JS onSpot reference. Safe to
// call multiple times.
void cleanupKwsBindings(JSContext* ctx);

}  // namespace bro::js
