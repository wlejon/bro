// The engine's shared listening front-end — see listen_host.h for the design.

#include "js/listen_host.h"

#include "audio_inference/audio_inference.h"

#include <broaudio/engine.h>
#include <brotensor/runtime.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <stdexcept>
#include <utility>
#include <vector>

namespace bro::js {

namespace {

using engine::AudioInference;
using engine::PcmRing;

// ── Stream retention ring ─────────────────────────────────────────────────────
// Captures the raw samples driving the bus (source-agnostic). Single producer
// (feed thread) appends; the main thread reads a behind-the-head window. `cap`
// is fixed once configured so slots are only reused after a full period — a read
// of data comfortably behind `written` never races the writer.
struct Retention {
    std::vector<float>        buf;            // ring; size == cap
    std::size_t               cap = 0;        // 0 = disabled
    int                       seconds = 0;
    int                       rate = 16000;
    int                       hop = 160;
    std::atomic<std::int64_t> written{0};     // total samples ever written

    void configure(int secs, int rate_, int hop_) {
        rate = rate_ > 0 ? rate_ : 16000;
        hop  = hop_  > 0 ? hop_  : 160;
        seconds = secs > 0 ? secs : 0;
        cap = static_cast<std::size_t>(seconds) * static_cast<std::size_t>(rate);
        buf.assign(cap, 0.0f);
        written.store(0, std::memory_order_release);
    }
    void disable() {
        cap = 0; seconds = 0;
        buf.clear(); buf.shrink_to_fit();
        written.store(0, std::memory_order_release);
    }
    void restart() {                          // fresh stream: keep config, rewind
        if (cap) std::fill(buf.begin(), buf.end(), 0.0f);
        written.store(0, std::memory_order_release);
    }

    // Producer thread. Append n samples.
    void write(const float* s, int n) {
        if (cap == 0 || n <= 0) return;
        const std::int64_t w = written.load(std::memory_order_relaxed);
        std::size_t pos = static_cast<std::size_t>(w % static_cast<std::int64_t>(cap));
        std::size_t rem = static_cast<std::size_t>(n);
        const float* src = s;
        while (rem > 0) {
            const std::size_t chunk = std::min(rem, cap - pos);
            std::memcpy(&buf[pos], src, chunk * sizeof(float));
            src += chunk; pos += chunk; if (pos == cap) pos = 0; rem -= chunk;
        }
        written.store(w + n, std::memory_order_release);
    }

    // Main thread. Copy absolute sample range [a, b) clamped to the held window.
    int readSamples(std::int64_t a, std::int64_t b, std::vector<float>& out) {
        out.clear();
        if (cap == 0) return 0;
        const std::int64_t w  = written.load(std::memory_order_acquire);
        const std::int64_t lo = std::max<std::int64_t>(0, w - static_cast<std::int64_t>(cap));
        a = std::max(a, lo);
        b = std::min(b, w);
        if (b <= a) return 0;
        const std::size_t count = static_cast<std::size_t>(b - a);
        out.resize(count);
        std::size_t pos = static_cast<std::size_t>(a % static_cast<std::int64_t>(cap));
        std::size_t rem = count, off = 0;
        while (rem > 0) {
            const std::size_t chunk = std::min(rem, cap - pos);
            std::memcpy(out.data() + off, &buf[pos], chunk * sizeof(float));
            off += chunk; pos += chunk; if (pos == cap) pos = 0; rem -= chunk;
        }
        return static_cast<int>(count);
    }

    std::int64_t streamFrame() const {
        return written.load(std::memory_order_acquire) / static_cast<std::int64_t>(hop);
    }
    std::int64_t heldFrames() const {
        const std::int64_t w = written.load(std::memory_order_acquire);
        const std::int64_t held = std::min<std::int64_t>(w, static_cast<std::int64_t>(cap));
        return held / static_cast<std::int64_t>(hop);
    }
};

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
    std::shared_ptr<brosoundml::WakeWord>       wake;
    std::shared_ptr<brosoundml::GestureSpotter> gesture;
    brotensor::Device spotterDevice = brotensor::Device::CPU;
    brotensor::Device wakeDevice    = brotensor::Device::CPU;
    ListenSpotsFn     onSpots;
    ListenWakeFn      onWake;
    ListenGesturesFn  onGestures;

    // Opt-in raw-audio retention over the shared stream (see header). Captured
    // at the bus chokepoint; survives membership changes (samples keep flowing),
    // rewound when the whole stream tears down + restarts.
    Retention retention;
    int       retentionSeconds = 0;   // requested depth; applied on infra create

    // The DeviceScope for the whole bus feed. Both model consumers run under
    // one scope; when only the wake member is attached, use its device.
    brotensor::Device scopeDevice() const {
        return spotter ? spotterDevice : wakeDevice;
    }
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
    // The stream is gone; rewind retention so a fresh attach restarts its axis
    // at 0 (mirroring the tenants' frame counters) and stale audio isn't read.
    g_listen.retention.restart();
}

// Replace the inference task with one for the CURRENT membership (or tear
// everything down when the membership emptied). AudioInference applies the
// remove + add commands in order on the worker between pumps, so the bus
// only ever runs under one closure at a time.
void rebuildTask() {
    removeTaskIfAny();
    if (!g_listen.hub && !g_listen.spotter && !g_listen.wake &&
        !g_listen.gesture) {
        teardownInfra();
        return;
    }

    auto bus        = g_listen.bus;
    auto hub        = g_listen.hub;
    auto spotter    = g_listen.spotter;
    auto wake       = g_listen.wake;
    auto gesture    = g_listen.gesture;
    auto device     = g_listen.scopeDevice();
    auto onSpots    = g_listen.onSpots;
    auto onWake     = g_listen.onWake;
    auto onGestures = g_listen.onGestures;
    g_listen.taskId = g_listen.inference->addTask(
        g_listen.ring,
        [bus, hub, spotter, wake, gesture, device, onSpots, onWake, onGestures](
            const float* samples, int n) {
            // Retain the raw stream (no-op when disabled). Producer thread; the
            // retention object lives for the host's life.
            g_listen.retention.write(samples, n);
            brosoundml::ListenFeedResult r;
            try {
                // DeviceScope so the model forwards run on their own stream,
                // not the host-syncing default.
                brotensor::DeviceScope scope(device);
                r = bus->feed(samples, n, hub.get(), spotter.get(),
                              wake.get(), gesture.get());
            } catch (const std::exception& e) {
                std::fprintf(stderr, "[ERROR] [listen] feed: %s\n", e.what());
                return;
            }
            if (onSpots && !r.spots.empty()) onSpots(r.spots);
            if (onWake && wake) onWake(r.wake_fired);
            if (onGestures && !r.gestures.empty()) onGestures(r.gestures);
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

    // Fresh stream: (re)configure retention if the app requested it.
    if (g_listen.retentionSeconds > 0) {
        g_listen.retention.configure(g_listen.retentionSeconds,
                                     g_listen.bus->sample_rate(),
                                     g_listen.bus->config().hop_length);
    }
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
    g_listen.wake.reset();
    g_listen.gesture.reset();
    g_listen.onSpots    = nullptr;
    g_listen.onWake     = nullptr;
    g_listen.onGestures = nullptr;
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

void listenHostSetWake(std::shared_ptr<brosoundml::WakeWord> wake,
                       brotensor::Device device, ListenWakeFn onWake) {
    if (wake) {
        ensureInfra();
        g_listen.bus->check_compatible(*wake);
    }
    g_listen.wake       = std::move(wake);
    g_listen.wakeDevice = device;
    g_listen.onWake     = std::move(onWake);
    rebuildTask();
}

void listenHostSetGesture(std::shared_ptr<brosoundml::GestureSpotter> gesture,
                          ListenGesturesFn onGestures) {
    // The gesture spotter reads the SensorHub's per-frame snapshot — it needs
    // no model device and no front-end of its own (it rides the hub bro.sense
    // already runs). Compatibility is implicit: it consumes whatever the hub
    // publishes, and does nothing on frames where no hub is attached.
    if (gesture) ensureInfra();
    g_listen.gesture    = std::move(gesture);
    g_listen.onGestures = std::move(onGestures);
    rebuildTask();
}

broaudio::MicTapId listenHostTapId() { return g_listen.tapId; }

void listenHostWriteRing(const float* samples, int n) {
    if (g_listen.ring) g_listen.ring->write(samples, n);
}

brosoundml::ListenFeedResult listenHostFeedInline(const float* samples, int n) {
    brosoundml::ListenFeedResult r;
    if (!g_listen.bus) return r;
    g_listen.retention.write(samples, n);
    brotensor::DeviceScope scope(g_listen.scopeDevice());
    r = g_listen.bus->feed(samples, n,
                           g_listen.hub.get(), g_listen.spotter.get(),
                           g_listen.wake.get(), g_listen.gesture.get());
    if (g_listen.onSpots && !r.spots.empty()) g_listen.onSpots(r.spots);
    if (g_listen.onWake && g_listen.wake) g_listen.onWake(r.wake_fired);
    if (g_listen.onGestures && !r.gestures.empty())
        g_listen.onGestures(r.gestures);
    return r;
}

// ─── Retention API ───────────────────────────────────────────────────────────

void listenHostSetRetention(int seconds) {
    g_listen.retentionSeconds = seconds > 0 ? seconds : 0;
    if (!g_listen.bus) return;                 // applied on the next attach
    if (g_listen.retentionSeconds > 0) {
        g_listen.retention.configure(g_listen.retentionSeconds,
                                     g_listen.bus->sample_rate(),
                                     g_listen.bus->config().hop_length);
    } else {
        g_listen.retention.disable();
    }
}

std::int64_t listenHostStreamFrame() {
    return g_listen.retention.streamFrame();
}

int listenHostReadAudio(std::int64_t startFrame, std::int64_t endFrame,
                        std::vector<float>& out) {
    out.clear();
    if (g_listen.retention.cap == 0) return 0;
    const int hop = g_listen.retention.hop;
    if (startFrame < 0) startFrame = 0;
    if (endFrame < startFrame) return 0;
    // Inclusive frame range -> [start*hop, (end+1)*hop) sample span.
    const std::int64_t a = startFrame * hop;
    const std::int64_t b = (endFrame + 1) * hop;
    return g_listen.retention.readSamples(a, b, out);
}

ListenRetentionInfo listenHostRetentionInfo() {
    ListenRetentionInfo info;
    info.active      = g_listen.retention.cap != 0;
    info.seconds     = g_listen.retention.seconds;
    info.rate        = g_listen.retention.rate;
    info.hop         = g_listen.retention.hop;
    info.streamFrame = g_listen.retention.streamFrame();
    info.heldFrames  = g_listen.retention.heldFrames();
    return info;
}

}  // namespace bro::js
