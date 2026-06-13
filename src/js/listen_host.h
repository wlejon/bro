#pragma once

#include <broaudio/mic_tap.h>
#include <brosoundml/listen_bus.h>
#include <brotensor/tensor.h>

#include <functional>
#include <memory>
#include <vector>

namespace broaudio {
class Engine;
}

namespace bro::engine {
class AudioInference;
}

namespace bro::js {

// ─── ListenHost — the engine's one shared listening front-end ───────────────
//
// bro.kws, bro.sense, and bro.wake used to each own a mic tap, a PcmRing,
// and an AudioInference task — parallel copies of the same plumbing
// computing the same PCEN mel per tenant. The host collapses that to ONE
// raw (no-AGC) 16 kHz tap + ONE ring + ONE inference task driving a
// brosoundml::ListenBus, with the tenants' consumers (SensorHub,
// PhonemeSpotter, WakeWord) attached as members: one feature pass, one
// forward per model, N listeners. bro.wake joined once its model was
// retrained on the AGC-free recipe (random presentation level — the
// detector is level-invariant, so it hears the same raw stream).
//
// Threading / membership: the bus is single-producer and is ONLY ever touched
// from inference-task closures. A membership change replaces the whole task
// (removeTask + addTask): AudioInference applies both commands, in order, on
// the worker between pumps, so the old closure (whose captures keep the old
// members alive) never overlaps the new one and the main thread never touches
// the bus. No locks anywhere. The tap + ring persist across membership
// changes — samples keep accumulating in the ring, so attaching or detaching
// one tenant drops no audio for the others.
//
// onSpots runs on the INFERENCE thread; tenants publish into their own SPSC
// delivery (bro.kws's event ring) from it.

using ListenSpotsFn =
    std::function<void(const std::vector<brosoundml::SpotEvent>&)>;
// Invoked on the inference thread after EVERY bus feed while a wake member
// is attached (not only on fires) so the tenant can publish per-block score
// telemetry alongside the fire flag.
using ListenWakeFn = std::function<void(bool fired)>;
// Invoked on the inference thread after a bus feed when a gesture member is
// attached and at least one gesture fired this block.
using ListenGesturesFn =
    std::function<void(const std::vector<brosoundml::GestureEvent>&)>;

// Wire the host to the engine's subsystems (engine init) / drop everything
// (engine teardown — also detaches any remaining members).
void installListenHost(broaudio::Engine* audio,
                       engine::AudioInference* inference);
void shutdownListenHost();

// Attach / detach members (pass nullptr to detach). The first attach creates
// the bus + ring + tap; the last detach tears them down (a fresh attach later
// starts a fresh stream). Throws std::runtime_error on a front-end framing
// mismatch or tap failure. Main thread only.
void listenHostSetHub(std::shared_ptr<brosoundml::SensorHub> hub);
void listenHostSetSpotter(std::shared_ptr<brosoundml::PhonemeSpotter> spotter,
                          brotensor::Device device, ListenSpotsFn onSpots);
void listenHostSetWake(std::shared_ptr<brosoundml::WakeWord> wake,
                       brotensor::Device device, ListenWakeFn onWake);
void listenHostSetGesture(std::shared_ptr<brosoundml::GestureSpotter> gesture,
                          ListenGesturesFn onGestures);

// The shared tap (kInvalidMicTapId when no member is attached) — for the
// tenants' stats() surfaces.
broaudio::MicTapId listenHostTapId();

// Manual feed for tests / scripted scenarios — the tenants' usual mode split.
// Threaded: write the shared ring (results surface via each tenant's normal
// delivery). Headless: run the bus synchronously on this thread for ALL
// attached members — it is ONE stream, so a feed from any tenant advances
// every tenant — delivering spots through onSpots exactly as the live path
// would, and returning the result.
void listenHostWriteRing(const float* samples, int n);
brosoundml::ListenFeedResult listenHostFeedInline(const float* samples, int n);

// ─── Stream retention (opt-in) ──────────────────────────────────────────────
//
// A ring of the RAW samples driving the shared stream, so recent audio can be
// scrubbed / replayed by frame range (e.g. to hear exactly what a match fired
// on). Source-AGNOSTIC by design: it captures whatever feeds the bus — the live
// mic tap, a scripted feed(), and any future non-mic source (system-audio
// loopback) that drives this host — so it works for every input the stack ever
// listens to, not just the mic. Off by default (no memory cost until enabled).
//
// Single-producer writer (the feed thread, at the bus chokepoint); the reader
// (main thread) copies a behind-the-head window, race-free without a lock
// because a slot is only overwritten after a full retention period.
//
// Frame axis: samples-consumed / hop — the same axis bro.sense reports (sense
// is the host's first/always member). A fresh stream (first attach after a full
// teardown) restarts the axis at 0, mirroring the tenants.

// Enable/resize retention to `seconds` of audio (0 disables + frees it). Takes
// effect immediately if the stream is up, else on the next attach. Main thread.
void listenHostSetRetention(int seconds);

// Current stream frame (total samples consumed / hop) — the retention axis.
std::int64_t listenHostStreamFrame();

// Copy retained raw PCM for the inclusive frame range [startFrame, endFrame]
// into `out`. Returns samples copied (0 if disabled or the range fell outside
// the retained window). Main thread.
int listenHostReadAudio(std::int64_t startFrame, std::int64_t endFrame,
                        std::vector<float>& out);

struct ListenRetentionInfo {
    bool         active = false;   // retention enabled
    int          seconds = 0;      // configured depth
    int          rate = 0;         // PCM sample rate
    int          hop = 0;          // samples per frame (frame->sample factor)
    std::int64_t streamFrame = 0;  // current stream frame
    std::int64_t heldFrames = 0;   // frames currently available to read back
};
ListenRetentionInfo listenHostRetentionInfo();

}  // namespace bro::js
