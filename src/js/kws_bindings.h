#pragma once

extern "C" {
#include "quickjs.h"
}

#include <cstdint>

namespace broaudio { class Engine; }
namespace bro::engine { class AudioInference; }

namespace bro::js {

// Install the `bro.kws` namespace — open-vocabulary streaming keyword spotting
// via brosoundml::PhonemeSpotter (the open-vocab analogue of bro.wake: enroll
// any phrase as a phoneme-sequence template instead of training one keyword
// in). bro.kws is a tenant of the engine's audio-inference subsystem with the
// same three-thread split as bro.wake:
//   - audio thread: the source callback (mic tap or loopback) copies resampled
//     samples into a lock-free ring. Nothing else — no model, no GPU, no heap.
//     (No AGC: the spotter's PCEN mel front-end is loudness-robust.)
//   - inference thread (engine::AudioInference worker; headless: the calling
//     thread via stepInline): drains the ring, runs PhonemeSpotter::feed(),
//     and publishes fired events into a lock-free SPSC slot ring.
//   - main thread: tickKws() drains the slots and invokes onSpot(name, conf).
//
// Weights load ONCE: bro.kws.load() reads the PhonemeNet checkpoint into a
// shared, read-only net. Each listened-on stream gets its OWN PhonemeSpotter
// (templates + streaming session + matcher) built over that shared net, so the
// same vocabulary can be spotted on N asynchronous streams (mic + system audio
// + a specific app) without copying weights. The per-stream spotters/templates
// are independent — enrolling on one stream never touches another.
//
// Dual-homed surface: the enroll/listen/… ops live on BOTH `bro.kws` (which
// targets the shared default-microphone stream) and `stream.kws` (the view a
// bro.listen.open() handle exposes, which targets that handle's stream). One
// implementation backs both; it resolves its per-stream tenant from `this`.
//
// PhonemeSpotter's mutators (enroll/remove/clear/reset) are single-producer
// with feed(), so the binding only permits them on a stream while that stream
// is NOT listening — enroll templates first, then listen(); stop() to re-enroll.
void installKwsBindings(JSContext* ctx, broaudio::Engine* audioEngine,
                        engine::AudioInference* inference);

// Build a `stream.kws` view object bound to stream `id` (a brosoundml StreamId).
// The view carries the same enroll/listen/… methods as bro.kws but resolves its
// tenant to `id`. Called by the bro.listen.open() handle to expose its .kws
// sub-object. Requires installKwsBindings() to have registered the view class.
JSValue kwsViewFor(JSContext* ctx, std::uint32_t id);

// Per-frame result delivery. Drains every listening stream's SPSC event slots
// and invokes its onSpot callback once per event; prunes tenants whose stream
// has closed. Cheap when nothing is pending. Call from the main thread per frame.
void tickKws(JSContext* ctx);

// Symmetric cleanup hook. Stops every stream's spotting, drops the shared net
// and all per-stream tenants, and frees stored JS onSpot references. Safe to
// call multiple times.
void cleanupKwsBindings(JSContext* ctx);

}  // namespace bro::js
