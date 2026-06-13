#pragma once

extern "C" {
#include "quickjs.h"
}

#include <cstdint>

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
// The audio plumbing is the SHARED listen host's (listen_host.h): one raw
// no-AGC tap + ring + inference task drive a brosoundml::ListenBus the hub
// joins as a member (alongside bro.kws's spotter) — one PCEN feature pass for
// the whole stack. bro.sense itself has no result ring and no tick — the
// hub's snapshot is the lock-free cross-thread surface:
//   - audio thread: the host's tap callback copies resampled mic samples into
//     the shared lock-free ring. No AGC — the level sensor is the stack's one
//     absolute-loudness signal, and the PCEN-derived sensors are gain-robust.
//   - inference thread (engine::AudioInference worker; headless: the calling
//     thread): the host's task runs the bus, which drives the hub per frame.
//   - main thread (or any thread): bro.sense.snapshot() — a seqlock read.
//     Momentary booleans are paired with monotonic counters, so polling at
//     frame rate still observes every onset/voice/tonal event.
// Each stream gets its OWN SensorHub: bro.sense targets the shared default-mic
// stream; stream.sense (the view a bro.listen.open() handle exposes) targets
// that handle's stream. One implementation resolves its tenant from `this`.
void installSenseBindings(JSContext* ctx, broaudio::Engine* audioEngine,
                          engine::AudioInference* inference);

// Build a `stream.sense` view object bound to stream `id` (a brosoundml
// StreamId). Same start/snapshot/… surface as bro.sense but scoped to `id`.
// Called by the bro.listen.open() handle to expose its .sense sub-object.
JSValue senseViewFor(JSContext* ctx, std::uint32_t id);

// Symmetric cleanup hook. Detaches the hub from the listen host (the host
// tears down the shared tap/task when its last member leaves) and drops the
// hub. Safe to call multiple times.
void cleanupSenseBindings(JSContext* ctx);

}  // namespace bro::js
