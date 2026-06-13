// The engine's shared listening front-ends — see listen_host.h for the design.

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
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

namespace bro::js {

namespace {

using engine::AudioInference;
using engine::PcmRing;

// ── Stream retention ring ─────────────────────────────────────────────────────
// Captures the raw samples driving a stream (source-agnostic). Single producer
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

// ── One independent listening pipeline ────────────────────────────────────────
struct ListenStream {
    StreamId     id = kInvalidStream;
    ListenSource source;
    bool         keepInfra = false;   // explicitly-opened streams hold their
                                      // source up across membership changes;
                                      // the default mic stream is tenant-gated.

    broaudio::Engine* audioEngine = nullptr;
    AudioInference*   inference   = nullptr;

    // Infra: created when the stream goes active, dropped when it goes idle.
    std::shared_ptr<brosoundml::ListenBus>     bus;
    std::shared_ptr<PcmRing>                   ring;
    AudioInference::TaskId taskId = AudioInference::kInvalidTask;
    broaudio::MicTapId     tapId  = broaudio::kInvalidMicTapId;          // mic source
    std::unique_ptr<broaudio::LoopbackCapture> loopback;                 // loopback source

    // Members (main-thread copies used to build each generation's closure).
    std::shared_ptr<brosoundml::SensorHub>      hub;
    std::shared_ptr<brosoundml::PhonemeSpotter> spotter;
    std::shared_ptr<brosoundml::WakeWord>       wake;
    std::shared_ptr<brosoundml::GestureSpotter> gesture;
    brotensor::Device spotterDevice = brotensor::Device::CPU;
    brotensor::Device wakeDevice    = brotensor::Device::CPU;
    ListenSpotsFn     onSpots;
    ListenWakeFn      onWake;
    ListenGesturesFn  onGestures;

    Retention retention;
    int       retentionSeconds = 0;

    brotensor::Device scopeDevice() const {
        return spotter ? spotterDevice : wakeDevice;
    }
    bool hasMember() const { return hub || spotter || wake || gesture; }
    bool wantActive() const { return hasMember() || keepInfra; }

    void removeTaskIfAny() {
        if (inference && taskId != AudioInference::kInvalidTask) {
            inference->removeTask(taskId);
        }
        taskId = AudioInference::kInvalidTask;
    }

    // Bring up source + ring + bus. Throws on source failure.
    void ensureInfra() {
        if (bus) return;
        if (!audioEngine || !inference) {
            throw std::runtime_error("listen host: audio subsystems not available");
        }
        auto newBus = std::make_shared<brosoundml::ListenBus>();   // KWS recipe
        const int rate = newBus->sample_rate();
        auto newRing = std::make_shared<PcmRing>(
            static_cast<std::size_t>(rate) * 2u);
        auto ringRef = newRing;

        if (source.kind == ListenSource::Kind::Mic) {
            // One raw tap (no AGC — the PCEN front-end is loudness-robust and
            // the tier-0 level sensor needs absolute loudness). Callback only
            // writes the ring.
            broaudio::MicTapConfig tapCfg;
            tapCfg.targetRate  = rate;
            tapCfg.chunkFrames = 0;
            tapCfg.agc         = false;
            const broaudio::MicTapId tap = audioEngine->addMicTap(
                tapCfg,
                [ringRef](const float* s, int n) { ringRef->write(s, n); });
            if (tap == broaudio::kInvalidMicTapId) {
                throw std::runtime_error("listen host: addMicTap failed");
            }
            if (!audioEngine->isMicCapturing()) audioEngine->startMicCapture();
            tapId = tap;
        } else {
            // Render-side loopback. The capture runs its own thread and writes
            // the same ring the mic tap would, downmixed + resampled to `rate`.
            broaudio::LoopbackConfig cfg;
            cfg.mode = (source.kind == ListenSource::Kind::SystemLoopback)
                           ? broaudio::LoopbackMode::SystemOutput
                       : source.exclude
                           ? broaudio::LoopbackMode::ProcessExclude
                           : broaudio::LoopbackMode::ProcessInclude;
            cfg.pid        = source.pid;
            cfg.targetRate = rate;
            cfg.mono       = true;   // channel-select reserved (source.channel)
            auto cap = std::make_unique<broaudio::LoopbackCapture>();
            const bool ok = cap->start(
                cfg, [ringRef](const float* s, int n) { ringRef->write(s, n); });
            if (!ok) {
                throw std::runtime_error("listen host: loopback start failed");
            }
            loopback = std::move(cap);
        }

        bus  = std::move(newBus);
        ring = std::move(newRing);

        if (retentionSeconds > 0) {
            retention.configure(retentionSeconds, bus->sample_rate(),
                                bus->config().hop_length);
        }
    }

    void teardownInfra() {
        // Producer first, so no more samples enter the ring.
        if (audioEngine && tapId != broaudio::kInvalidMicTapId) {
            audioEngine->removeMicTap(tapId);
        }
        tapId = broaudio::kInvalidMicTapId;
        if (loopback) { loopback->stop(); loopback.reset(); }
        removeTaskIfAny();
        ring.reset();
        bus.reset();
        // Stream gone: rewind retention so a fresh activation restarts at 0.
        retention.restart();
    }

    // Replace the inference task with one for the CURRENT membership, or tear
    // the infra down when the stream went idle (no members and not pinned).
    void rebuildTask() {
        removeTaskIfAny();
        if (!wantActive()) {
            teardownInfra();
            return;
        }
        ensureInfra();

        Retention* ret      = &retention;
        auto        b        = bus;
        auto        h        = hub;
        auto        sp       = spotter;
        auto        wk       = wake;
        auto        ge       = gesture;
        auto        device   = scopeDevice();
        auto        onS      = onSpots;
        auto        onW      = onWake;
        auto        onG      = onGestures;
        taskId = inference->addTask(
            ring,
            [ret, b, h, sp, wk, ge, device, onS, onW, onG](
                const float* samples, int n) {
                ret->write(samples, n);   // retain raw stream (no-op if disabled)
                brosoundml::ListenFeedResult r;
                try {
                    brotensor::DeviceScope scope(device);
                    r = b->feed(samples, n, h.get(), sp.get(), wk.get(), ge.get());
                } catch (const std::exception& e) {
                    std::fprintf(stderr, "[ERROR] [listen] feed: %s\n", e.what());
                    return;
                }
                if (onS && !r.spots.empty()) onS(r.spots);
                if (onW && wk) onW(r.wake_fired);
                if (onG && !r.gestures.empty()) onG(r.gestures);
            });
    }

    brosoundml::ListenFeedResult feedInline(const float* samples, int n) {
        brosoundml::ListenFeedResult r;
        if (!bus) return r;
        retention.write(samples, n);
        brotensor::DeviceScope scope(scopeDevice());
        r = bus->feed(samples, n, hub.get(), spotter.get(), wake.get(),
                      gesture.get());
        if (onSpots && !r.spots.empty()) onSpots(r.spots);
        if (onWake && wake) onWake(r.wake_fired);
        if (onGestures && !r.gestures.empty()) onGestures(r.gestures);
        return r;
    }

    void applyRetention(int seconds) {
        retentionSeconds = seconds > 0 ? seconds : 0;
        if (!bus) return;                           // applied on the next activate
        if (retentionSeconds > 0) {
            retention.configure(retentionSeconds, bus->sample_rate(),
                                bus->config().hop_length);
        } else {
            retention.disable();
        }
    }

    int readAudio(std::int64_t startFrame, std::int64_t endFrame,
                  std::vector<float>& out) {
        out.clear();
        if (retention.cap == 0) return 0;
        const int hop = retention.hop;
        if (startFrame < 0) startFrame = 0;
        if (endFrame < startFrame) return 0;
        const std::int64_t a = startFrame * hop;
        const std::int64_t b = (endFrame + 1) * hop;
        return retention.readSamples(a, b, out);
    }
};

// ── Manager: owns the streams (main thread only) ──────────────────────────────
struct Manager {
    broaudio::Engine* audioEngine = nullptr;
    AudioInference*   inference   = nullptr;
    std::vector<std::unique_ptr<ListenStream>> streams;
    StreamId nextId    = 1;
    StreamId defaultMic = kInvalidStream;

    ListenStream* find(StreamId id) {
        if (id == kInvalidStream) return nullptr;
        for (auto& s : streams) if (s->id == id) return s.get();
        return nullptr;
    }

    ListenStream* create(const ListenSource& src, bool keepInfra) {
        auto s = std::make_unique<ListenStream>();
        s->id          = nextId++;
        s->source      = src;
        s->keepInfra   = keepInfra;
        s->audioEngine = audioEngine;
        s->inference   = inference;
        ListenStream* p = s.get();
        streams.push_back(std::move(s));
        return p;
    }

    void erase(StreamId id) {
        for (auto it = streams.begin(); it != streams.end(); ++it) {
            if ((*it)->id == id) {
                (*it)->teardownInfra();
                streams.erase(it);
                return;
            }
        }
    }

    // Create-or-get the shared microphone stream the global bindings target.
    ListenStream* ensureDefaultMic() {
        if (ListenStream* s = find(defaultMic)) return s;
        ListenStream* s = create(ListenSource{}, /*keepInfra*/ false);
        defaultMic = s->id;
        return s;
    }
};

Manager g_mgr;

}  // namespace

// ─── Manager lifecycle ───────────────────────────────────────────────────────

void installListenHost(broaudio::Engine* audio,
                       engine::AudioInference* inference) {
    g_mgr.audioEngine = audio;
    g_mgr.inference   = inference;
}

void shutdownListenHost() {
    for (auto& s : g_mgr.streams) s->teardownInfra();
    g_mgr.streams.clear();
    g_mgr.defaultMic  = kInvalidStream;
    g_mgr.audioEngine = nullptr;
    g_mgr.inference   = nullptr;
}

// ─── Streams ───────────────────────────────────────────────────────────────

StreamId listenHostOpen(const ListenSource& src) {
    if (!g_mgr.audioEngine || !g_mgr.inference) return kInvalidStream;
    if (src.kind != ListenSource::Kind::Mic &&
        !broaudio::LoopbackCapture::isSupported()) {
        return kInvalidStream;
    }
    ListenStream* s = g_mgr.create(src, /*keepInfra*/ true);
    try {
        s->ensureInfra();   // start the source immediately
        s->rebuildTask();   // run the (member-less) feed so audio flows + retains
    } catch (const std::exception& e) {
        std::fprintf(stderr, "[ERROR] [listen] open: %s\n", e.what());
        g_mgr.erase(s->id);
        return kInvalidStream;
    }
    return s->id;
}

void listenHostClose(StreamId id) {
    if (id == g_mgr.defaultMic) g_mgr.defaultMic = kInvalidStream;
    g_mgr.erase(id);
}

bool listenHostValid(StreamId id) { return g_mgr.find(id) != nullptr; }

StreamId listenHostDefaultMicId() { return g_mgr.ensureDefaultMic()->id; }

bool listenHostLoopbackSupported() {
    return broaudio::LoopbackCapture::isSupported();
}

std::vector<broaudio::AudioProcess> listenHostEnumerateApps() {
    return broaudio::LoopbackCapture::enumerateProcesses();
}

// ─── Per-stream member attach / detach ───────────────────────────────────────

void listenStreamSetHub(StreamId id, std::shared_ptr<brosoundml::SensorHub> hub) {
    ListenStream* s = g_mgr.find(id);
    if (!s) return;
    if (hub) { s->ensureInfra(); s->bus->check_compatible(*hub); }
    s->hub = std::move(hub);
    s->rebuildTask();
}

void listenStreamSetSpotter(StreamId id,
                            std::shared_ptr<brosoundml::PhonemeSpotter> spotter,
                            brotensor::Device device, ListenSpotsFn onSpots) {
    ListenStream* s = g_mgr.find(id);
    if (!s) return;
    if (spotter) { s->ensureInfra(); s->bus->check_compatible(*spotter); }
    s->spotter       = std::move(spotter);
    s->spotterDevice = device;
    s->onSpots       = std::move(onSpots);
    s->rebuildTask();
}

void listenStreamSetWake(StreamId id, std::shared_ptr<brosoundml::WakeWord> wake,
                         brotensor::Device device, ListenWakeFn onWake) {
    ListenStream* s = g_mgr.find(id);
    if (!s) return;
    if (wake) { s->ensureInfra(); s->bus->check_compatible(*wake); }
    s->wake       = std::move(wake);
    s->wakeDevice = device;
    s->onWake     = std::move(onWake);
    s->rebuildTask();
}

void listenStreamSetGesture(StreamId id,
                            std::shared_ptr<brosoundml::GestureSpotter> gesture,
                            ListenGesturesFn onGestures) {
    ListenStream* s = g_mgr.find(id);
    if (!s) return;
    // The gesture spotter rides the SensorHub's per-frame snapshot — no model
    // device, no front-end of its own. Compatibility is implicit.
    if (gesture) s->ensureInfra();
    s->gesture    = std::move(gesture);
    s->onGestures = std::move(onGestures);
    s->rebuildTask();
}

broaudio::MicTapId listenStreamTapId(StreamId id) {
    ListenStream* s = g_mgr.find(id);
    return s ? s->tapId : broaudio::kInvalidMicTapId;
}

void listenStreamWriteRing(StreamId id, const float* samples, int n) {
    ListenStream* s = g_mgr.find(id);
    if (s && s->ring) s->ring->write(samples, n);
}

brosoundml::ListenFeedResult listenStreamFeedInline(StreamId id,
                                                    const float* samples, int n) {
    ListenStream* s = g_mgr.find(id);
    if (!s) return {};
    return s->feedInline(samples, n);
}

brosoundml::ListenFeedResult listenStreamFeed(StreamId id,
                                              const float* samples, int n) {
    ListenStream* s = g_mgr.find(id);
    if (!s) return {};
    // Threaded: hand the samples to the live ring; the inference worker drains
    // it and delivers any events through the attached members' callbacks.
    if (g_mgr.inference && g_mgr.inference->threaded()) {
        if (s->ring) s->ring->write(samples, n);
        return {};
    }
    // Headless / no worker: run the bus now on this thread for every member.
    return s->feedInline(samples, n);
}

void listenStreamSetRetention(StreamId id, int seconds) {
    if (ListenStream* s = g_mgr.find(id)) s->applyRetention(seconds);
}

std::int64_t listenStreamFrame(StreamId id) {
    ListenStream* s = g_mgr.find(id);
    return s ? s->retention.streamFrame() : 0;
}

int listenStreamReadAudio(StreamId id, std::int64_t startFrame,
                          std::int64_t endFrame, std::vector<float>& out) {
    ListenStream* s = g_mgr.find(id);
    if (!s) { out.clear(); return 0; }
    return s->readAudio(startFrame, endFrame, out);
}

ListenRetentionInfo listenStreamRetentionInfo(StreamId id) {
    ListenRetentionInfo info;
    ListenStream* s = g_mgr.find(id);
    if (!s) return info;
    info.active      = s->retention.cap != 0;
    info.seconds     = s->retention.seconds;
    info.rate        = s->retention.rate;
    info.hop         = s->retention.hop;
    info.streamFrame = s->retention.streamFrame();
    info.heldFrames  = s->retention.heldFrames();
    return info;
}

// ─── Default mic stream (migration scaffold) ─────────────────────────────────

void listenHostSetHub(std::shared_ptr<brosoundml::SensorHub> hub) {
    listenStreamSetHub(g_mgr.ensureDefaultMic()->id, std::move(hub));
}
void listenHostSetSpotter(std::shared_ptr<brosoundml::PhonemeSpotter> spotter,
                          brotensor::Device device, ListenSpotsFn onSpots) {
    listenStreamSetSpotter(g_mgr.ensureDefaultMic()->id, std::move(spotter),
                           device, std::move(onSpots));
}
void listenHostSetWake(std::shared_ptr<brosoundml::WakeWord> wake,
                       brotensor::Device device, ListenWakeFn onWake) {
    listenStreamSetWake(g_mgr.ensureDefaultMic()->id, std::move(wake), device,
                        std::move(onWake));
}
void listenHostSetGesture(std::shared_ptr<brosoundml::GestureSpotter> gesture,
                          ListenGesturesFn onGestures) {
    listenStreamSetGesture(g_mgr.ensureDefaultMic()->id, std::move(gesture),
                           std::move(onGestures));
}

broaudio::MicTapId listenHostTapId() {
    return listenStreamTapId(g_mgr.ensureDefaultMic()->id);
}
void listenHostWriteRing(const float* samples, int n) {
    listenStreamWriteRing(g_mgr.ensureDefaultMic()->id, samples, n);
}
brosoundml::ListenFeedResult listenHostFeedInline(const float* samples, int n) {
    return listenStreamFeedInline(g_mgr.ensureDefaultMic()->id, samples, n);
}

void listenHostSetRetention(int seconds) {
    listenStreamSetRetention(g_mgr.ensureDefaultMic()->id, seconds);
}
std::int64_t listenHostStreamFrame() {
    return listenStreamFrame(g_mgr.ensureDefaultMic()->id);
}
int listenHostReadAudio(std::int64_t startFrame, std::int64_t endFrame,
                        std::vector<float>& out) {
    return listenStreamReadAudio(g_mgr.ensureDefaultMic()->id, startFrame,
                                 endFrame, out);
}
ListenRetentionInfo listenHostRetentionInfo() {
    return listenStreamRetentionInfo(g_mgr.ensureDefaultMic()->id);
}

}  // namespace bro::js
