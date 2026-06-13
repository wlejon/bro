#pragma once

extern "C" {
#include "quickjs.h"
}

#include <cstdint>

namespace broaudio { class Engine; }
namespace bro::engine { class AudioInference; }

namespace bro::js {

// Install the `bro.gesture` namespace — open-vocabulary NON-SPEECH gesture
// matching via brosoundml::GestureSpotter, the tier-0 analogue of bro.kws.
// Where bro.kws aligns phoneme templates (speech), bro.gesture matches enrolled
// rhythm (onset-interval) and tone (sustained-pitch) gestures against the same
// shared listen host's tier-0 SensorHub stream — so clicks, taps, and whistles
// that the speech model can only hear as garbage tokens fire reliably here.
//
// Same three-thread split as bro.kws (tap -> ring -> inference -> SPSC slots ->
// tickGesture -> onGesture(name, confidence, kind)), and the same single-
// producer rule: enroll/remove/clear/reset only while NOT listening. The
// matcher consumes the SensorHub's per-frame snapshot, so it only fires while
// bro.sense is also a live member of the host.
// Each stream gets its OWN GestureSpotter, riding that stream's SensorHub:
// bro.gesture targets the shared default-mic stream; stream.gesture (the view a
// bro.listen.open() handle exposes) targets that handle's stream.
void installGestureBindings(JSContext* ctx, broaudio::Engine* audioEngine,
                            engine::AudioInference* inference);

// Build a `stream.gesture` view object bound to stream `id` (a brosoundml
// StreamId). Same enroll/listen/… surface as bro.gesture but scoped to `id`.
// Called by the bro.listen.open() handle to expose its .gesture sub-object.
JSValue gestureViewFor(JSContext* ctx, std::uint32_t id);

// Drain every listening stream's gesture event slots and invoke its onGesture
// callback once per event; prune tenants whose stream has closed. Main thread,
// per frame.
void tickGesture(JSContext* ctx);

// Detach from the host, drop the spotter, free the JS callback. Idempotent.
void cleanupGestureBindings(JSContext* ctx);

}  // namespace bro::js
