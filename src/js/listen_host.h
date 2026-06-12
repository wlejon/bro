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
// bro.kws and bro.sense used to each own a mic tap, a PcmRing, and an
// AudioInference task — parallel copies of the same plumbing computing the
// same PCEN mel per tenant. The host collapses that to ONE raw (no-AGC)
// 16 kHz tap + ONE ring + ONE inference task driving a
// brosoundml::ListenBus, with the tenants' consumers (SensorHub,
// PhonemeSpotter) attached as members: one feature pass, one PhonemeNet
// forward, N listeners. bro.wake stays on its own tap — its model is trained
// on AGC'd audio, so its input stream is genuinely different until retrained.
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

}  // namespace bro::js
