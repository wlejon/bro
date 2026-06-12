// The engine's shared listening front-end — see listen_host.h for the design.

#include "js/listen_host.h"

#include "audio_inference/audio_inference.h"

#include <broaudio/engine.h>
#include <brotensor/runtime.h>

#include <cstdio>
#include <exception>
#include <stdexcept>
#include <utility>

namespace bro::js {

namespace {

using engine::AudioInference;
using engine::PcmRing;

struct ListenHostState {
    broaudio::Engine* audioEngine = nullptr;
    AudioInference*   inference   = nullptr;

    // Created on the first member attach, dropped on the last detach.
    std::shared_ptr<brosoundml::ListenBus> bus;
    std::shared_ptr<PcmRing>               ring;
    AudioInference::TaskId taskId = AudioInference::kInvalidTask;
    broaudio::MicTapId     tapId  = broaudio::kInvalidMicTapId;

    // Current membership — main-thread copies used to build each generation's
    // task closure (which captures its own strong refs).
    std::shared_ptr<brosoundml::SensorHub>      hub;
    std::shared_ptr<brosoundml::PhonemeSpotter> spotter;
    brotensor::Device spotterDevice = brotensor::Device::CPU;
    ListenSpotsFn     onSpots;
};

ListenHostState g_listen;

void removeTaskIfAny() {
    if (g_listen.inference && g_listen.taskId != AudioInference::kInvalidTask) {
        g_listen.inference->removeTask(g_listen.taskId);
    }
    g_listen.taskId = AudioInference::kInvalidTask;
}

void teardownInfra() {
    // Producer first, so no more samples enter the ring.
    if (g_listen.audioEngine && g_listen.tapId != broaudio::kInvalidMicTapId) {
        g_listen.audioEngine->removeMicTap(g_listen.tapId);
    }
    g_listen.tapId = broaudio::kInvalidMicTapId;
    removeTaskIfAny();
    g_listen.ring.reset();
    g_listen.bus.reset();
}

// Replace the inference task with one for the CURRENT membership (or tear
// everything down when the membership emptied). AudioInference applies the
// remove + add commands in order on the worker between pumps, so the bus
// only ever runs under one closure at a time.
void rebuildTask() {
    removeTaskIfAny();
    if (!g_listen.hub && !g_listen.spotter) {
        teardownInfra();
        return;
    }

    auto bus     = g_listen.bus;
    auto hub     = g_listen.hub;
    auto spotter = g_listen.spotter;
    auto device  = g_listen.spotterDevice;
    auto onSpots = g_listen.onSpots;
    g_listen.taskId = g_listen.inference->addTask(
        g_listen.ring,
        [bus, hub, spotter, device, onSpots](const float* samples, int n) {
            brosoundml::ListenFeedResult r;
            try {
                // DeviceScope so the spotter's per-block forward runs on its
                // own stream, not the host-syncing default (cf. bro.wake).
                brotensor::DeviceScope scope(device);
                r = bus->feed(samples, n, hub.get(), spotter.get());
            } catch (const std::exception& e) {
                std::fprintf(stderr, "[ERROR] [listen] feed: %s\n", e.what());
                return;
            }
            if (onSpots && !r.spots.empty()) onSpots(r.spots);
        });
}

// First member attach: create bus + ring + tap. Throws on tap failure.
void ensureInfra() {
    if (g_listen.bus) return;
    if (!g_listen.audioEngine || !g_listen.inference) {
        throw std::runtime_error(
            "listen host: audio subsystems not available");
    }
    auto bus = std::make_shared<brosoundml::ListenBus>();   // KWS recipe
    const int rate = bus->sample_rate();
    auto ring = std::make_shared<PcmRing>(static_cast<std::size_t>(rate) * 2u);

    // One raw tap for the whole stack: no AGC — the PCEN front-end is
    // loudness-robust by construction, and the tier-0 level sensor NEEDS
    // absolute loudness. The callback only writes the ring.
    broaudio::MicTapConfig tapCfg;
    tapCfg.targetRate  = rate;
    tapCfg.chunkFrames = 0;
    tapCfg.agc         = false;
    auto ringRef = ring;
    const broaudio::MicTapId tap = g_listen.audioEngine->addMicTap(
        tapCfg,
        [ringRef](const float* samples, int n) { ringRef->write(samples, n); });
    if (tap == broaudio::kInvalidMicTapId) {
        throw std::runtime_error("listen host: addMicTap failed");
    }
    if (!g_listen.audioEngine->isMicCapturing()) {
        g_listen.audioEngine->startMicCapture();
    }
    g_listen.bus   = std::move(bus);
    g_listen.ring  = std::move(ring);
    g_listen.tapId = tap;
}

}  // namespace

void installListenHost(broaudio::Engine* audio,
                       engine::AudioInference* inference) {
    g_listen.audioEngine = audio;
    g_listen.inference   = inference;
}

void shutdownListenHost() {
    g_listen.hub.reset();
    g_listen.spotter.reset();
    g_listen.onSpots = nullptr;
    teardownInfra();
    g_listen.audioEngine = nullptr;
    g_listen.inference   = nullptr;
}

void listenHostSetHub(std::shared_ptr<brosoundml::SensorHub> hub) {
    if (hub) {
        ensureInfra();
        g_listen.bus->check_compatible(*hub);
    }
    g_listen.hub = std::move(hub);
    rebuildTask();
}

void listenHostSetSpotter(std::shared_ptr<brosoundml::PhonemeSpotter> spotter,
                          brotensor::Device device, ListenSpotsFn onSpots) {
    if (spotter) {
        ensureInfra();
        g_listen.bus->check_compatible(*spotter);
    }
    g_listen.spotter       = std::move(spotter);
    g_listen.spotterDevice = device;
    g_listen.onSpots       = std::move(onSpots);
    rebuildTask();
}

broaudio::MicTapId listenHostTapId() { return g_listen.tapId; }

void listenHostWriteRing(const float* samples, int n) {
    if (g_listen.ring) g_listen.ring->write(samples, n);
}

brosoundml::ListenFeedResult listenHostFeedInline(const float* samples, int n) {
    brosoundml::ListenFeedResult r;
    if (!g_listen.bus) return r;
    brotensor::DeviceScope scope(g_listen.spotterDevice);
    r = g_listen.bus->feed(samples, n,
                           g_listen.hub.get(), g_listen.spotter.get());
    if (g_listen.onSpots && !r.spots.empty()) g_listen.onSpots(r.spots);
    return r;
}

}  // namespace bro::js
