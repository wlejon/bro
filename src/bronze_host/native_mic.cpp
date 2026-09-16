#include "natives/mic/native_mic_decl.h"
#include "bronze_host/host_internal.h"
#include "bronze_host/gl_internal.h"
#include "engine/engine.h"
#include <broaudio/engine.h>
#include <broaudio/mic_tap.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace {

constexpr int kMicRing = 4096;

struct MicStatsSlot {
    double framesDelivered = 0.0;
    double samplesDelivered = 0.0;
    double rollingPeak = 0.0;
    double chunkCount = 0.0;
    double dropped = 0.0;
    int32_t chunkFrames = 512;
};

struct MicState {
    std::atomic<int> peakRingX10000[kMicRing];
    std::atomic<int> rmsRingX10000[kMicRing];
    std::atomic<uint64_t> writeCount{0};
    std::atomic<uint64_t> dropped{0};

    bool wantSamples = false;
    std::vector<float> sampleRing;

    uint64_t lastFired = 0;
    int chunkFrames = 0;
    broaudio::MicTapId tapId = broaudio::kInvalidMicTapId;
    bool active = false;

    bronze::embed::Persistent onChunk;
};

static MicState g_mic;
static thread_local MicStatsSlot tl_micStats;
static thread_local std::vector<double> tl_levels;

void shutdownActiveMic() {
    if (!g_mic.active) return;
    auto* eng = bro::bronze_host::hostEngine();
    auto* e = eng ? eng->audioEngine() : nullptr;
    if (e && g_mic.tapId != broaudio::kInvalidMicTapId) {
        e->removeMicTap(g_mic.tapId);
    }
    g_mic.tapId = broaudio::kInvalidMicTapId;
    g_mic.onChunk.set(bronze::embed::undefined());
    g_mic.writeCount.store(0, std::memory_order_relaxed);
    g_mic.dropped.store(0, std::memory_order_relaxed);
    g_mic.lastFired = 0;
    g_mic.chunkFrames = 0;
    g_mic.wantSamples = false;
    g_mic.sampleRing.clear();
    g_mic.sampleRing.shrink_to_fit();
    g_mic.active = false;
}

} // namespace

namespace bro::bronze_host {

void drainMicChunks() {
    if (!g_mic.active) return;
    uint64_t w = g_mic.writeCount.load(std::memory_order_acquire);
    if (w == g_mic.lastFired) return;
    if (bronze::embed::isUndefined(g_mic.onChunk.get()) || !bronze::embed::isFunction(g_mic.onChunk.get())) {
        g_mic.lastFired = w;
        return;
    }

    uint64_t start = g_mic.lastFired;
    if (w - start > static_cast<uint64_t>(kMicRing)) {
        g_mic.dropped.fetch_add(w - start - kMicRing, std::memory_order_relaxed);
        start = w - kMicRing;
    }
    for (uint64_t i = start; i < w; ++i) {
        int slot = static_cast<int>(i % kMicRing);
        int pk = g_mic.peakRingX10000[slot].load(std::memory_order_relaxed);
        int rms = g_mic.rmsRingX10000[slot].load(std::memory_order_relaxed);

        ObjectBuilder o;
        o.set("index", bronze::embed::fromDouble(static_cast<double>(i)));
        o.set("peak", bronze::embed::fromDouble(pk / 10000.0));
        o.set("rms", bronze::embed::fromDouble(rms / 10000.0));
        if (g_mic.wantSamples && g_mic.chunkFrames > 0) {
            const float* src = g_mic.sampleRing.data() + static_cast<size_t>(slot) * static_cast<size_t>(g_mic.chunkFrames);
            o.set("samples", makeFloat32Array(src, static_cast<size_t>(g_mic.chunkFrames)));
        }
        bronze::embed::Value chunkVal = o.get();
        bronze::embed::call(g_mic.onChunk.get(), bronze::embed::undefined(), std::span<const bronze::embed::Value>(&chunkVal, 1));
    }
    g_mic.lastFired = w;
}

} // namespace bro::bronze_host

extern "C" {

void bro_mic_start(bool opts_chunkFrames_given, int32_t opts_chunkFrames,
                   bool opts_targetRate_given, int32_t opts_targetRate,
                   bool opts_agc_given, bool opts_agc,
                   bool opts_live_given, bool opts_live,
                   bool opts_samples_given, bool opts_samples,
                   uint64_t opts_onChunk,
                   bool opts_targetPeak_given, double opts_targetPeak,
                   bool opts_halfLifeSec_given, double opts_halfLifeSec,
                   bool opts_noiseGate_given, double opts_noiseGate,
                   bool opts_maxGain_given, double opts_maxGain) {
    auto* eng = bro::bronze_host::hostEngine();
    auto* e = eng ? eng->audioEngine() : nullptr;
    if (!e) {
        bronze::embed::throwError("bro.mic.start: audio engine not available");
        return;
    }

    int chunkFrames = opts_chunkFrames_given ? opts_chunkFrames : 160;
    int targetRate = opts_targetRate_given ? opts_targetRate : 16000;
    bool agc = opts_agc_given ? opts_agc : false;
    bool live = opts_live_given ? opts_live : true;
    bool samples = opts_samples_given ? opts_samples : false;

    if (chunkFrames < 0 || targetRate < 0) {
        bronze::embed::throwError("bro.mic.start: chunkFrames and targetRate must be >= 0");
        return;
    }
    if (samples && chunkFrames <= 0) {
        bronze::embed::throwError("bro.mic.start: opts.samples needs a fixed chunkFrames (> 0) — the sample ring is sized once at start");
        return;
    }

    shutdownActiveMic();

    broaudio::MicTapConfig cfg;
    cfg.chunkFrames = chunkFrames;
    cfg.targetRate = targetRate;
    cfg.agc = agc;
    if (opts_targetPeak_given) cfg.agcCfg.targetPeak = static_cast<float>(opts_targetPeak);
    if (opts_halfLifeSec_given) cfg.agcCfg.halfLifeSec = static_cast<float>(opts_halfLifeSec);
    if (opts_noiseGate_given) cfg.agcCfg.noiseGate = static_cast<float>(opts_noiseGate);
    if (opts_maxGain_given) cfg.agcCfg.maxGain = static_cast<float>(opts_maxGain);

    g_mic.chunkFrames = chunkFrames;
    g_mic.wantSamples = samples;
    if (opts_onChunk != 0) {
        bronze::embed::Value fnVal(opts_onChunk);
        if (bronze::embed::isFunction(fnVal)) {
            g_mic.onChunk.set(fnVal);
        }
    }
    if (samples) {
        g_mic.sampleRing.assign(static_cast<size_t>(kMicRing) * static_cast<size_t>(chunkFrames), 0.0f);
    }
    g_mic.writeCount.store(0, std::memory_order_relaxed);
    g_mic.dropped.store(0, std::memory_order_relaxed);
    g_mic.lastFired = 0;

    g_mic.tapId = e->addMicTap(cfg, [](const float* s, int n) {
        if (n <= 0) return;
        float peak = 0.0f, sumSq = 0.0f;
        for (int i = 0; i < n; ++i) {
            float a = std::fabs(s[i]);
            if (a > peak) peak = a;
            sumSq += s[i] * s[i];
        }
        float rms = std::sqrt(sumSq / static_cast<float>(n));
        uint64_t idx = g_mic.writeCount.load(std::memory_order_relaxed);
        int slot = static_cast<int>(idx % kMicRing);
        g_mic.peakRingX10000[slot].store(static_cast<int>(peak * 10000.0f), std::memory_order_relaxed);
        g_mic.rmsRingX10000[slot].store(static_cast<int>(rms * 10000.0f), std::memory_order_relaxed);
        if (g_mic.wantSamples) {
            int cf = g_mic.chunkFrames;
            int m = n < cf ? n : cf;
            float* dst = g_mic.sampleRing.data() + static_cast<size_t>(slot) * static_cast<size_t>(cf);
            std::memcpy(dst, s, static_cast<size_t>(m) * sizeof(float));
            if (m < cf) {
                std::memset(dst + m, 0, static_cast<size_t>(cf - m) * sizeof(float));
            }
        }
        g_mic.writeCount.store(idx + 1, std::memory_order_release);
    });

    if (g_mic.tapId == broaudio::kInvalidMicTapId) {
        shutdownActiveMic();
        bronze::embed::throwError("bro.mic.start: addMicTap failed");
        return;
    }

    if (live && !e->isMicCapturing()) {
        e->startMicCapture();
    }
    g_mic.active = true;
}

void bro_mic_stop(void) {
    shutdownActiveMic();
}

bool bro_mic_isActive(void) {
    return g_mic.active;
}

int32_t bro_mic_engineRate(void) {
    auto* eng = bro::bronze_host::hostEngine();
    auto* e = eng ? eng->audioEngine() : nullptr;
    return e ? e->sampleRate() : 0;
}

bool bro_mic_stats(void) {
    auto* eng = bro::bronze_host::hostEngine();
    auto* e = eng ? eng->audioEngine() : nullptr;
    if (!g_mic.active || !e || g_mic.tapId == broaudio::kInvalidMicTapId) {
        return false;
    }
    auto s = e->getMicTapStats(g_mic.tapId);
    tl_micStats.framesDelivered = static_cast<double>(s.framesDelivered);
    tl_micStats.samplesDelivered = static_cast<double>(s.samplesDelivered);
    tl_micStats.rollingPeak = s.rollingPeak;
    tl_micStats.chunkCount = static_cast<double>(g_mic.writeCount.load(std::memory_order_acquire));
    tl_micStats.dropped = static_cast<double>(g_mic.dropped.load(std::memory_order_relaxed));
    tl_micStats.chunkFrames = g_mic.chunkFrames;
    return true;
}

double bro_mic_stats_framesDelivered(void) {
    return tl_micStats.framesDelivered;
}

double bro_mic_stats_samplesDelivered(void) {
    return tl_micStats.samplesDelivered;
}

double bro_mic_stats_rollingPeak(void) {
    return tl_micStats.rollingPeak;
}

double bro_mic_stats_chunkCount(void) {
    return tl_micStats.chunkCount;
}

double bro_mic_stats_dropped(void) {
    return tl_micStats.dropped;
}

int32_t bro_mic_stats_chunkFrames(void) {
    return tl_micStats.chunkFrames;
}

void bro_mic_levels(bool maxCount_given, int32_t maxCount, bronze_native_buffer* out) {
    if (!out) return;
    int limit = kMicRing;
    if (maxCount_given && maxCount >= 0 && maxCount < limit) {
        limit = maxCount;
    }
    uint64_t w = g_mic.writeCount.load(std::memory_order_acquire);
    int avail = static_cast<int>(w < static_cast<uint64_t>(kMicRing) ? w : static_cast<uint64_t>(kMicRing));
    int count = avail < limit ? avail : limit;

    tl_levels.resize(count);
    for (int k = 0; k < count; ++k) {
        uint64_t i = w - static_cast<uint64_t>(count) + static_cast<uint64_t>(k);
        int pk = g_mic.peakRingX10000[i % kMicRing].load(std::memory_order_relaxed);
        tl_levels[k] = pk / 10000.0;
    }
    out->data = tl_levels.data();
    out->length = static_cast<uint32_t>(tl_levels.size());
    out->release = nullptr;
    out->ctx = nullptr;
}

void bro_mic_feed(const float* samples, uint32_t samples_len, bool sampleRate_given, int32_t sampleRate) {
    if (!g_mic.active) {
        bronze::embed::throwError("bro.mic.feed: not started");
        return;
    }
    auto* eng = bro::bronze_host::hostEngine();
    auto* e = eng ? eng->audioEngine() : nullptr;
    if (!e) {
        bronze::embed::throwError("bro.mic.feed: audio engine not available");
        return;
    }
    if (e->isMicCapturing()) {
        bronze::embed::throwError("bro.mic.feed: cannot feed while live mic capture is active (feed is for headless/offline use)");
        return;
    }
    if (!samples || samples_len == 0) {
        return;
    }
    int engineRate = e->sampleRate();
    if (sampleRate_given && sampleRate > 0 && sampleRate != engineRate) {
        std::string err = "bro.mic.feed: sampleRate=" + std::to_string(sampleRate) +
                          " must equal the engine mic rate=" + std::to_string(engineRate);
        bronze::embed::throwError(err.c_str());
        return;
    }
    e->injectMicSamples(samples, static_cast<int>(samples_len));
}

} // extern "C"

namespace bro::bronze_host {
bool registerNatives_mic(std::string* error);
bool registerMicNatives(std::string* error) {
    return registerNatives_mic(error);
}
} // namespace bro::bronze_host
