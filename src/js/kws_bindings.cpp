#if BRO_WITH_SOUNDML
#include "js/kws_bindings.h"
#include "audio_inference/audio_inference.h"
#include "js/listen_host.h"
#include <broaudio/engine.h>
#include <broaudio/mic_tap.h>
#include <brosoundml/phoneme_model.h>
#include <brosoundml/phoneme_spotter.h>
#include <brotensor/runtime.h>
#include <brotensor/tensor.h>
#include <qjsbind/qjsbind.h>
#include <quickjs.h>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

extern "C" {
#include "quickjs.h"
}

namespace bro::js {

namespace {

using engine::AudioInference;

// Fired events cross inference -> main as (template index, confidence) pairs
// in a fixed SPSC slot ring. 64 slots is generous u2014 spot events arrive at
// human speech cadence; on overflow the newest event is dropped.
constexpr std::uint64_t kEventSlots = 64;

// u2500u2500u2500 One stream's keyword-spotting tenant u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500
//
// Address-stable (held by unique_ptr in g_kws.tenants), so the inference-thread
// onSpots closure can capture a raw KwsTenant* and publish into its ring.
struct KwsTenant {
    StreamId streamId = kInvalidStream;

    // This stream's spotter, built over the shared net. Owned by the tenant
    // across listen()/stop(); the inference task closure holds a second strong
    // ref while listening.
    std::shared_ptr<brosoundml::PhonemeSpotter> spotter;

    // Template-name snapshot taken at listen() u2014 index i names eventIdx[i].
    // The enrolled set cannot change while listening, so the snapshot is stable
    // for the task's lifetime. Main thread reads it in tickKws.
    std::vector<std::string> names;

    JSValue onSpot = JS_UNDEFINED;

    // SPSC event ring: the inference thread writes slot produced%kEventSlots
    // then bumps produced; tickKws (main) drains up to produced and advances
    // drained. The producer drops events when the ring is full.
    int                        eventIdx[kEventSlots]   = {};
    float                      eventConf[kEventSlots]  = {};
    std::int64_t               eventStart[kEventSlots] = {};   // matched-span frames
    std::int64_t               eventEnd[kEventSlots]   = {};   //   (absolute, frames axis)
    std::atomic<std::uint64_t> produced{0};
    std::atomic<std::uint64_t> drained{0};

    // Gates onSpot delivery, never audio processing (the posterior stream and
    // every template's DP state keep rolling so resume never faces a cold
    // matcher). Written by suspend()/resume() (main), read by the inference
    // thread.
    std::atomic<bool> suspended{false};

    bool listening = false;
};

// u2500u2500u2500 Namespace-level state (the shared net + the per-stream tenants) u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500
struct KwsNamespace {
    broaudio::Engine* audioEngine = nullptr;
    AudioInference*   inference   = nullptr;
    JSContext*        ctx         = nullptr;

    // The weights, loaded once and shared by every stream's spotter.
    std::shared_ptr<const brosoundml::PhonemeNet> net;
    brotensor::Device                             device = brotensor::Device::CPU;
    // Detector-policy defaults set at load(); applied to each new tenant spotter
    // and as the base for per-template enroll overrides.
    brosoundml::SpotterConfig defaultPolicy;

    std::unordered_map<StreamId, std::unique_ptr<KwsTenant>> tenants;
};

KwsNamespace g_kws;

// A per-stream view object: `stream.kws`. Carries only the stream id; the ops
// resolve their tenant through it. (No JSValue fields u2014 nothing for the GC to
// mark; the onSpot callback lives in the C++ KwsTenant, manually managed.)
struct KwsView {
    StreamId streamId = kInvalidStream;
};

// u2500u2500u2500 Helpers (same shapes as wake_bindings) u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500

bool getStr(JSContext* ctx, JSValueConst obj, const char* key, std::string& out) {
    JSValue v = JS_GetPropertyStr(ctx, obj, key);
    bool ok = false;
    if (JS_IsString(v)) {
        const char* s = JS_ToCString(ctx, v);
        if (s) { out = s; JS_FreeCString(ctx, s); ok = true; }
    }
    JS_FreeValue(ctx, v);
    return ok;
}

bool getNum(JSContext* ctx, JSValueConst obj, const char* key, double& dst) {
    JSValue v = JS_GetPropertyStr(ctx, obj, key);
    bool ok = false;
    if (JS_IsNumber(v)) { JS_ToFloat64(ctx, &dst, v); ok = true; }
    JS_FreeValue(ctx, v);
    return ok;
}

bool getInt(JSContext* ctx, JSValueConst obj, const char* key, int& dst) {
    JSValue v = JS_GetPropertyStr(ctx, obj, key);
    bool ok = false;
    if (JS_IsNumber(v)) {
        int32_t t = dst;
        JS_ToInt32(ctx, &t, v);
        dst = t;
        ok = true;
    }
    JS_FreeValue(ctx, v);
    return ok;
}

brotensor::Device autoDevice() {
    if (brotensor::is_available(brotensor::Device::CUDA))  return brotensor::Device::CUDA;
    if (brotensor::is_available(brotensor::Device::Metal)) return brotensor::Device::Metal;
    return brotensor::Device::CPU;
}

bool parseDeviceOpt(JSContext* ctx, JSValueConst opts,
                    brotensor::Device& out, std::string& err) {
    if (!JS_IsObject(opts)) return true;
    JSValue v = JS_GetPropertyStr(ctx, opts, "device");
    if (JS_IsUndefined(v) || JS_IsNull(v)) {
        JS_FreeValue(ctx, v);
        return true;
    }
    if (!JS_IsString(v)) {
        JS_FreeValue(ctx, v);
        err = "opts.device must be a string ('cpu', 'cuda', or 'metal')";
        return false;
    }
    const char* s = JS_ToCString(ctx, v);
    std::string sv = s ? s : "";
    if (s) JS_FreeCString(ctx, s);
    JS_FreeValue(ctx, v);
    if (sv == "cpu")   { out = brotensor::Device::CPU;   return true; }
    if (sv == "cuda")  { out = brotensor::Device::CUDA;  return true; }
    if (sv == "metal") { out = brotensor::Device::Metal; return true; }
    err = "opts.device must be 'cpu', 'cuda', or 'metal' (got '" + sv + "')";
    return false;
}

const char* deviceName(brotensor::Device d) {
    switch (d.type) {
        case brotensor::DeviceType::CUDA:  return "CUDA";
        case brotensor::DeviceType::Metal: return "Metal";
        case brotensor::DeviceType::CPU:   return "CPU";
    }
    return "?";
}

// Overlay detector-policy keys present on `obj` onto `cfg`:
//   threshold, refractoryMs, minPhonemes, entrySilenceFrames, emissionFloor,
//   minCoverage, scoreNorm, enrollGaps, gapMinFrames, gapTolerance,
//   smoothing: { hits, window }.
// Used for the global defaults (load) and per-template overrides (enroll).
void readPolicy(JSContext* ctx, JSValueConst obj, brosoundml::SpotterConfig& cfg) {
    if (!JS_IsObject(obj)) return;
    double d;
    if (getNum(ctx, obj, "threshold", d))     cfg.threshold      = (float)d;
    if (getNum(ctx, obj, "emissionFloor", d)) cfg.emission_floor = (float)d;
    // Proportional coverage gate: a completion must have at least
    // ceil(minCoverage * L) of the template's phonemes ACTUALLY emitted (not
    // merely riding the emission floor). This is what stops a long phrase from
    // firing on a short suffix u2014 "what is the first" completing on just
    // "first", with the leading phonemes floored. 0 (default) = absolute
    // min_phonemes gate only. See SpotterConfig::min_coverage_frac.
    if (getNum(ctx, obj, "minCoverage", d))   cfg.min_coverage_frac = (float)d;
    // Competition-normalization strength [0,1]: puts templates of differing
    // phoneme make-up on one score scale so a single threshold transfers
    // across them. 0 (default) = raw posterior. See SpotterConfig::score_norm.
    if (getNum(ctx, obj, "scoreNorm", d))     cfg.score_norm     = (float)d;
    if (getNum(ctx, obj, "gapTolerance", d))  cfg.gap_tolerance  = (float)d;
    getInt(ctx, obj, "refractoryMs",       cfg.refractory_ms);
    getInt(ctx, obj, "minPhonemes",        cfg.min_phonemes);
    getInt(ctx, obj, "entrySilenceFrames", cfg.entry_silence_frames);
    getInt(ctx, obj, "gapMinFrames",       cfg.gap_min_frames);
    JSValue gv = JS_GetPropertyStr(ctx, obj, "enrollGaps");
    if (!JS_IsUndefined(gv) && !JS_IsNull(gv))
        cfg.enroll_gaps = JS_ToBool(ctx, gv) > 0;
    JS_FreeValue(ctx, gv);
    JSValue sm = JS_GetPropertyStr(ctx, obj, "smoothing");
    if (JS_IsObject(sm)) {
        getInt(ctx, sm, "hits",   cfg.smoothing_hits);
        getInt(ctx, sm, "window", cfg.smoothing_window);
    }
    JS_FreeValue(ctx, sm);
}

bool readPolicyOverride(JSContext* ctx, JSValueConst obj,
                        const brosoundml::PhonemeSpotter& spotter,
                        brosoundml::SpotterConfig& out) {
    if (!JS_IsObject(obj)) return false;
    out = spotter.config();
    readPolicy(ctx, obj, out);
    return true;
}

// Read int ids from an Int32Array or number[].
std::vector<int> readIds(JSContext* ctx, JSValueConst v) {
    std::vector<int> out;
    size_t byteOff = 0, viewLen = 0, bpe = 0;
    JSValue abuf = JS_GetTypedArrayBuffer(ctx, v, &byteOff, &viewLen, &bpe);
    if (!JS_IsException(abuf)) {
        size_t abufLen = 0;
        std::uint8_t* p = JS_GetArrayBuffer(ctx, &abufLen, abuf);
        JS_FreeValue(ctx, abuf);
        if (p && bpe == sizeof(int32_t)) {
            const auto* ids = reinterpret_cast<const int32_t*>(p + byteOff);
            out.assign(ids, ids + viewLen / sizeof(int32_t));
            return out;
        }
    } else {
        JS_FreeValue(ctx, JS_GetException(ctx));
    }
    if (JS_IsArray(v)) {
        std::uint32_t n = 0;
        JSValue lv = JS_GetPropertyStr(ctx, v, "length");
        JS_ToUint32(ctx, &n, lv);
        JS_FreeValue(ctx, lv);
        out.reserve(n);
        for (std::uint32_t i = 0; i < n; ++i) {
            JSValue e = JS_GetPropertyUint32(ctx, v, i);
            int32_t t = 0;
            JS_ToInt32(ctx, &t, e);
            out.push_back(t);
            JS_FreeValue(ctx, e);
        }
    }
    return out;
}

// Read a Float32Array's element pointer + count (nullptr if not one).
const float* readFloats(JSContext* ctx, JSValueConst v, int& count) {
    count = 0;
    size_t byteOff = 0, viewLen = 0, bpe = 0;
    JSValue abuf = JS_GetTypedArrayBuffer(ctx, v, &byteOff, &viewLen, &bpe);
    if (JS_IsException(abuf)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        return nullptr;
    }
    size_t abufLen = 0;
    std::uint8_t* p = JS_GetArrayBuffer(ctx, &abufLen, abuf);
    JS_FreeValue(ctx, abuf);
    if (!p || bpe != sizeof(float)) return nullptr;
    count = static_cast<int>(viewLen / sizeof(float));
    return reinterpret_cast<const float*>(p + byteOff);
}

int nameIndexOf(const std::vector<std::string>& names, const std::string& n) {
    for (std::size_t i = 0; i < names.size(); ++i)
        if (names[i] == n) return static_cast<int>(i);
    return -1;
}

// Publish one fired event into a tenant's SPSC ring (inference thread). Drops
// the newest event on overflow u2014 never corrupts the ring.
void publishEvent(KwsTenant* t, int nameIdx, float confidence,
                  std::int64_t startFrame, std::int64_t endFrame) {
    const std::uint64_t p = t->produced.load(std::memory_order_relaxed);
    if (p - t->drained.load(std::memory_order_acquire) >= kEventSlots) return;
    t->eventIdx[p % kEventSlots]   = nameIdx;
    t->eventConf[p % kEventSlots]  = confidence;
    t->eventStart[p % kEventSlots] = startFrame;
    t->eventEnd[p % kEventSlots]   = endFrame;
    t->produced.store(p + 1, std::memory_order_release);
}

// u2500u2500u2500 Tenant registry u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500

KwsTenant* findTenant(StreamId id) {
    auto it = g_kws.tenants.find(id);
    return it == g_kws.tenants.end() ? nullptr : it->second.get();
}

// Get-or-create the tenant for `id`, building its spotter over the shared net.
// Returns nullptr if no net is loaded.
KwsTenant* ensureTenant(StreamId id) {
    if (id == kInvalidStream || !g_kws.net) return nullptr;
    if (KwsTenant* t = findTenant(id)) return t;
    auto t = std::make_unique<KwsTenant>();
    t->streamId = id;
    t->spotter  = std::make_shared<brosoundml::PhonemeSpotter>(g_kws.net);
    t->spotter->set_config(g_kws.defaultPolicy);
    KwsTenant* p = t.get();
    g_kws.tenants[id] = std::move(t);
    return p;
}

// Stop a tenant's spotting: detach its spotter from its stream (a host barrier),
// free its onSpot, and clear the live snapshot. Keeps the spotter + templates so
// listening can resume without re-enrolling.
void stopListening(KwsTenant* t) {
    if (!t->listening) return;
    // Detach from the listen host. The host replaces (or tears down) the
    // stream's inference task; any other member (bro.sense) keeps rolling.
    listenStreamSetSpotter(t->streamId, nullptr, brotensor::Device::CPU, nullptr);
    if (g_kws.ctx && !JS_IsUndefined(t->onSpot)) {
        JS_FreeValue(g_kws.ctx, t->onSpot);
        t->onSpot = JS_UNDEFINED;
    }
    t->produced.store(0, std::memory_order_relaxed);
    t->drained.store(0, std::memory_order_relaxed);
    t->suspended.store(false, std::memory_order_relaxed);
    t->names.clear();
    t->listening = false;
}

// Drop every tenant (stop + free) and the shared net.
void unloadAll() {
    for (auto& kv : g_kws.tenants) stopListening(kv.second.get());
    g_kws.tenants.clear();
    g_kws.net.reset();
}

// Reject single-producer mutators while the inference thread owns this stream's
// feed().
JSValue throwIfListening(JSContext* ctx, const KwsTenant* t, const char* what) {
    if (!t || !t->listening) return JS_UNDEFINED;
    return JS_ThrowInternalError(ctx,
        "bro.kws.%s: not allowed while this stream is listening (enroll/remove/"
        "clear/reset share the spotter's feed thread u2014 stop() first)", what);
}

// u2500u2500u2500 Tenant resolution (the dual-home seam) u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500

// The stream `this` addresses: a KwsView u2192 its stream; bro.kws u2192 default mic.
StreamId streamOf(JSContext* ctx, JSValueConst this_val) {
    if (KwsView* v = qjsbind::unwrap<KwsView>(ctx, this_val)) return v->streamId;
    return listenHostDefaultMicId();
}

// u2500u2500u2500 JS-callable functions u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500

// bro.kws.load({ weights, device?, threshold?, refractoryMs?, smoothing?,
//                minPhonemes?, entrySilenceFrames?, emissionFloor?,
//                minCoverage?, scoreNorm?, enrollGaps?, gapMinFrames?,
//                gapTolerance? })
// Load the PhonemeNet checkpoint (+ its embedded class map) ONCE into the shared
// net and set the global detector-policy defaults. Drops any existing tenants
// (their spotters referenced the old net). Enroll templates next, then listen().
// Namespace op u2014 not stream-scoped.
JSValue js_load(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 1 || !JS_IsObject(argv[0])) {
        return JS_ThrowTypeError(ctx,
            "bro.kws.load(opts): opts object required (weights, ...)");
    }
    std::string weights;
    if (!getStr(ctx, argv[0], "weights", weights) || weights.empty()) {
        return JS_ThrowTypeError(ctx,
            "bro.kws.load: opts.weights (PhonemeNet checkpoint path) required");
    }
    // init() BEFORE the device probe u2014 the GPU backends only register on the
    // driver probe, so autoDevice() before init() silently lands on CPU.
    try {
        brotensor::init();
    } catch (const std::exception& e) {
        return JS_ThrowInternalError(ctx, "bro.kws.load: %s", e.what());
    }
    brotensor::Device dev = autoDevice();
    {
        std::string err;
        if (!parseDeviceOpt(ctx, argv[0], dev, err))
            return JS_ThrowTypeError(ctx, "bro.kws.load: %s", err.c_str());
    }

    try {
        // Load the net once and share it (read-only) across every stream's
        // spotter. The weights live here exactly once.
        std::shared_ptr<const brosoundml::PhonemeNet> net;
        {
            brotensor::DeviceScope scope(dev);
            net = std::make_shared<const brosoundml::PhonemeNet>(
                brosoundml::PhonemeNet::load(weights, dev));
        }

        // Replacing the net invalidates existing tenant spotters u2014 drop them.
        unloadAll();

        brosoundml::SpotterConfig cfg;            // struct defaults
        readPolicy(ctx, argv[0], cfg);

        g_kws.net           = std::move(net);
        g_kws.device        = dev;
        g_kws.defaultPolicy = cfg;
        std::fprintf(stderr,
            "[INFO] [kws] PhonemeNet loaded on %s (K=%d, %d Hz, threshold=%.2f), "
            "shared across streams\n",
            deviceName(dev), g_kws.net->class_map().num_classes,
            g_kws.net->config().sample_rate, cfg.threshold);
        return JS_UNDEFINED;
    } catch (const std::exception& e) {
        return JS_ThrowInternalError(ctx, "bro.kws.load: %s", e.what());
    }
}

JSValue js_unload(JSContext*, JSValueConst, int, JSValueConst*) {
    unloadAll();
    return JS_UNDEFINED;
}

// bro.kws.enroll(name, phonemeIds, policy?) -> template length
//   phonemeIds: Int32Array/number[] of Kokoro phoneme ids u2014 exactly what
//   bro.tts.phonemize(text) returns. Silence/suprasegmental ids are dropped
//   and duplicate adjacent classes collapsed by the library. Enrolls on the
//   tenant for THIS stream (bro.kws u2192 default mic; stream.kws u2192 that stream).
JSValue js_enroll(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    KwsTenant* t = ensureTenant(streamOf(ctx, this_val));
    if (!t)
        return JS_ThrowInternalError(ctx, "bro.kws.enroll: call bro.kws.load first");
    JSValue guard = throwIfListening(ctx, t, "enroll");
    if (JS_IsException(guard)) return guard;
    std::string name;
    if (argc < 2 || !JS_IsString(argv[0])) {
        return JS_ThrowTypeError(ctx,
            "bro.kws.enroll(name, phonemeIds, policy?): name and ids required");
    }
    {
        const char* s = JS_ToCString(ctx, argv[0]);
        if (!s) return JS_EXCEPTION;
        name = s;
        JS_FreeCString(ctx, s);
    }
    std::vector<int> ids = readIds(ctx, argv[1]);
    if (ids.empty()) {
        return JS_ThrowTypeError(ctx,
            "bro.kws.enroll: phonemeIds must be a non-empty Int32Array or "
            "number[] (use bro.tts.phonemize(text))");
    }
    try {
        brosoundml::SpotterConfig pol;
        const bool hasPol = (argc >= 3) && readPolicyOverride(ctx, argv[2],
                                                              *t->spotter, pol);
        const int len = t->spotter->enroll(name, ids, hasPol ? &pol : nullptr);
        if (len <= 0) {
            return JS_ThrowInternalError(ctx,
                "bro.kws.enroll: '%s' produced an empty template (only "
                "silence/suprasegmental ids?)", name.c_str());
        }
        return JS_NewInt32(ctx, len);
    } catch (const std::exception& e) {
        return JS_ThrowInternalError(ctx, "bro.kws.enroll: %s", e.what());
    }
}

// bro.kws.enrollFromAudio(name, samples, policy?) -> template length
//   samples: Float32Array of mono PCM at bro.kws.sampleRate() u2014 runs the model
//   over the reference audio and uses the argmax class sequence as the template.
JSValue js_enrollFromAudio(JSContext* ctx, JSValueConst this_val, int argc,
                           JSValueConst* argv) {
    KwsTenant* t = ensureTenant(streamOf(ctx, this_val));
    if (!t || !t->spotter->loaded())
        return JS_ThrowInternalError(ctx,
            "bro.kws.enrollFromAudio: call bro.kws.load first");
    JSValue guard = throwIfListening(ctx, t, "enrollFromAudio");
    if (JS_IsException(guard)) return guard;
    if (argc < 2 || !JS_IsString(argv[0])) {
        return JS_ThrowTypeError(ctx,
            "bro.kws.enrollFromAudio(name, samples, policy?): name and samples "
            "required");
    }
    const char* s = JS_ToCString(ctx, argv[0]);
    if (!s) return JS_EXCEPTION;
    std::string name = s;
    JS_FreeCString(ctx, s);

    int n = 0;
    const float* p = readFloats(ctx, argv[1], n);
    if (!p || n <= 0) {
        return JS_ThrowTypeError(ctx,
            "bro.kws.enrollFromAudio: samples must be a non-empty Float32Array");
    }
    try {
        brosoundml::SpotterConfig pol;
        const bool hasPol = (argc >= 3) && readPolicyOverride(ctx, argv[2],
                                                              *t->spotter, pol);
        brotensor::DeviceScope scope(g_kws.device);
        const int len = t->spotter->enroll_from_audio(name, p, n,
                                                      hasPol ? &pol : nullptr);
        if (len <= 0) {
            return JS_ThrowInternalError(ctx,
                "bro.kws.enrollFromAudio: '%s' produced an empty template "
                "(silence-only audio?)", name.c_str());
        }
        return JS_NewInt32(ctx, len);
    } catch (const std::exception& e) {
        return JS_ThrowInternalError(ctx, "bro.kws.enrollFromAudio: %s", e.what());
    }
}

// bro.kws.enrollFromClasses(name, classIds, policy?) -> template length
//   classIds: Int32Array/number[] of phoneme-CLASS ids (already in [0,K) u2014 the
//   matcher's own alphabet, e.g. an edited sequence from bro.kws.inspect). The
//   library drops the silence class (0) and collapses adjacent duplicates. This
//   is the re-enroll path for an edited template.
JSValue js_enrollFromClasses(JSContext* ctx, JSValueConst this_val, int argc,
                             JSValueConst* argv) {
    KwsTenant* t = ensureTenant(streamOf(ctx, this_val));
    if (!t)
        return JS_ThrowInternalError(ctx,
            "bro.kws.enrollFromClasses: call bro.kws.load first");
    JSValue guard = throwIfListening(ctx, t, "enrollFromClasses");
    if (JS_IsException(guard)) return guard;
    if (argc < 2 || !JS_IsString(argv[0])) {
        return JS_ThrowTypeError(ctx,
            "bro.kws.enrollFromClasses(name, classIds, policy?): name and "
            "classIds required");
    }
    const char* s = JS_ToCString(ctx, argv[0]);
    if (!s) return JS_EXCEPTION;
    std::string name = s;
    JS_FreeCString(ctx, s);

    std::vector<int> ids = readIds(ctx, argv[1]);
    if (ids.empty()) {
        return JS_ThrowTypeError(ctx,
            "bro.kws.enrollFromClasses: classIds must be a non-empty "
            "Int32Array or number[]");
    }
    try {
        brosoundml::SpotterConfig pol;
        const bool hasPol = (argc >= 3) && readPolicyOverride(ctx, argv[2],
                                                              *t->spotter, pol);
        const int len = t->spotter->enroll_from_classes(name, ids,
                                                        hasPol ? &pol : nullptr);
        if (len <= 0) {
            return JS_ThrowInternalError(ctx,
                "bro.kws.enrollFromClasses: '%s' produced an empty template "
                "(only silence-class ids?)", name.c_str());
        }
        return JS_NewInt32(ctx, len);
    } catch (const std::exception& e) {
        return JS_ThrowInternalError(ctx, "bro.kws.enrollFromClasses: %s", e.what());
    }
}

JSValue js_remove(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    KwsTenant* t = findTenant(streamOf(ctx, this_val));
    if (!t)
        return JS_ThrowInternalError(ctx, "bro.kws.remove: nothing enrolled");
    JSValue guard = throwIfListening(ctx, t, "remove");
    if (JS_IsException(guard)) return guard;
    if (argc < 1 || !JS_IsString(argv[0]))
        return JS_ThrowTypeError(ctx, "bro.kws.remove(name): name required");
    const char* s = JS_ToCString(ctx, argv[0]);
    if (!s) return JS_EXCEPTION;
    const bool removed = t->spotter->remove(s);
    JS_FreeCString(ctx, s);
    return JS_NewBool(ctx, removed);
}

JSValue js_clear(JSContext* ctx, JSValueConst this_val, int, JSValueConst*) {
    KwsTenant* t = findTenant(streamOf(ctx, this_val));
    if (!t) return JS_UNDEFINED;
    JSValue guard = throwIfListening(ctx, t, "clear");
    if (JS_IsException(guard)) return guard;
    t->spotter->clear();
    return JS_UNDEFINED;
}

JSValue js_templates(JSContext* ctx, JSValueConst this_val, int, JSValueConst*) {
    JSValue arr = JS_NewArray(ctx);
    KwsTenant* t = findTenant(streamOf(ctx, this_val));
    if (!t) return arr;
    // While listening, return the snapshot (the live template list shares the
    // feed thread); otherwise read the spotter directly.
    const std::vector<std::string> names =
        t->listening ? t->names : t->spotter->templates();
    for (std::uint32_t i = 0; i < names.size(); ++i) {
        JS_SetPropertyUint32(ctx, arr, i, JS_NewString(ctx, names[i].c_str()));
    }
    return arr;
}

// bro.kws.inspect(name) -> { name, threshold, frameMs, hasGaps, states:[...] }
//   or null if no such template. Safe to call while listening (reads immutable
//   per-template structure).
JSValue js_inspect(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    KwsTenant* t = findTenant(streamOf(ctx, this_val));
    if (!t) return JS_NULL;
    if (argc < 1 || !JS_IsString(argv[0]))
        return JS_ThrowTypeError(ctx, "bro.kws.inspect(name): name required");
    const char* s = JS_ToCString(ctx, argv[0]);
    if (!s) return JS_EXCEPTION;
    std::string name = s;
    JS_FreeCString(ctx, s);

    brosoundml::TemplateView view;
    if (!t->spotter->inspect(name, view)) return JS_NULL;

    const auto& cm = t->spotter->class_map();
    const int   K  = cm.num_classes;

    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "name", JS_NewString(ctx, view.name.c_str()));
    JS_SetPropertyStr(ctx, obj, "threshold", JS_NewFloat64(ctx, view.threshold));
    JS_SetPropertyStr(ctx, obj, "frameMs", JS_NewFloat64(ctx, view.frame_ms));
    JS_SetPropertyStr(ctx, obj, "hasGaps", JS_NewBool(ctx, view.has_gaps));
    JSValue arr = JS_NewArray(ctx);
    for (std::uint32_t i = 0; i < view.states.size(); ++i) {
        const brosoundml::TemplateState& st = view.states[i];
        JSValue tv = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, tv, "cls", JS_NewInt32(ctx, st.cls));
        const char* label =
            st.gap ? "gap"
                   : (st.cls >= 0 && st.cls < K &&
                      st.cls < (int)cm.class_names.size())
                         ? cm.class_names[(std::size_t)st.cls].c_str()
                         : "?";
        JS_SetPropertyStr(ctx, tv, "label", JS_NewString(ctx, label));
        JS_SetPropertyStr(ctx, tv, "gap", JS_NewBool(ctx, st.gap));
        JS_SetPropertyStr(ctx, tv, "gapLo", JS_NewInt32(ctx, st.gap_lo));
        JS_SetPropertyStr(ctx, tv, "gapHi", JS_NewInt32(ctx, st.gap_hi));
        JS_SetPropertyUint32(ctx, arr, i, tv);
    }
    JS_SetPropertyStr(ctx, obj, "states", arr);
    return obj;
}

JSValue js_reset(JSContext* ctx, JSValueConst this_val, int, JSValueConst*) {
    KwsTenant* t = findTenant(streamOf(ctx, this_val));
    if (!t) return JS_UNDEFINED;
    JSValue guard = throwIfListening(ctx, t, "reset");
    if (JS_IsException(guard)) return guard;
    t->spotter->reset();
    return JS_UNDEFINED;
}

// bro.kws.listen({ onSpot }) u2014 start live spotting on THIS stream. Requires a
// loaded net and at least one enrolled template on this stream.
JSValue js_listen(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    if (!g_kws.audioEngine)
        return JS_ThrowInternalError(ctx, "bro.kws.listen: audio engine not available");
    if (!g_kws.inference)
        return JS_ThrowInternalError(ctx,
            "bro.kws.listen: audio-inference subsystem not available");
    const StreamId sid = streamOf(ctx, this_val);
    KwsTenant* t = ensureTenant(sid);
    if (!t || !t->spotter->loaded())
        return JS_ThrowInternalError(ctx, "bro.kws.listen: call bro.kws.load first");
    if (t->listening)
        return JS_ThrowInternalError(ctx,
            "bro.kws.listen: this stream is already listening (stop() first)");
    if (argc < 1 || !JS_IsObject(argv[0]))
        return JS_ThrowTypeError(ctx, "bro.kws.listen(opts): opts object required");

    JSValue onSpotVal = JS_GetPropertyStr(ctx, argv[0], "onSpot");
    if (!JS_IsFunction(ctx, onSpotVal)) {
        JS_FreeValue(ctx, onSpotVal);
        return JS_ThrowTypeError(ctx,
            "bro.kws.listen: opts.onSpot (function) required");
    }

    const std::vector<std::string> names = t->spotter->templates();
    if (names.empty()) {
        JS_FreeValue(ctx, onSpotVal);
        return JS_ThrowInternalError(ctx,
            "bro.kws.listen: no templates enrolled on this stream "
            "(bro.kws.enroll first)");
    }

    try {
        t->names  = names;
        t->onSpot = JS_DupValue(ctx, onSpotVal);
        t->produced.store(0, std::memory_order_relaxed);
        t->drained.store(0, std::memory_order_relaxed);
        t->suspended.store(false, std::memory_order_relaxed);
        JS_FreeValue(ctx, onSpotVal);

        // Join the listen host on this stream. The stream's single task drives
        // the bus (mel -> one PhonemeNet forward) and calls this hook on the
        // inference thread with whatever fired. Captures the tenant pointer
        // (address-stable in g_kws.tenants) and the name snapshot (the enrolled
        // set cannot change while listening, so name -> index lookups stay
        // valid); the tenant's atomics live until it is dropped.
        listenStreamSetSpotter(
            sid, t->spotter, g_kws.device,
            [t, names](const std::vector<brosoundml::SpotEvent>& events) {
                if (t->suspended.load(std::memory_order_relaxed)) return;
                for (const auto& ev : events) {
                    const int idx = nameIndexOf(names, ev.name);
                    if (idx >= 0)
                        publishEvent(t, idx, ev.confidence,
                                     ev.start_frame, ev.end_frame);
                }
            });
        t->listening = true;

        std::fprintf(stderr,
            "[INFO] [kws] listening on stream %u (device=%s, %zu template%s, "
            "model=%d Hz)\n",
            sid, deviceName(g_kws.device), names.size(),
            names.size() == 1 ? "" : "s", t->spotter->sample_rate());
        return JS_UNDEFINED;
    } catch (const std::exception& e) {
        t->listening = true;   // let stopListening clear the partial state
        stopListening(t);
        return JS_ThrowInternalError(ctx, "bro.kws.listen: %s", e.what());
    }
}

JSValue js_stop(JSContext* ctx, JSValueConst this_val, int, JSValueConst*) {
    if (KwsTenant* t = findTenant(streamOf(ctx, this_val))) stopListening(t);
    return JS_UNDEFINED;
}

JSValue js_suspend(JSContext* ctx, JSValueConst this_val, int, JSValueConst*) {
    if (KwsTenant* t = findTenant(streamOf(ctx, this_val)))
        t->suspended.store(true, std::memory_order_relaxed);
    return JS_UNDEFINED;
}

JSValue js_resume(JSContext* ctx, JSValueConst this_val, int, JSValueConst*) {
    if (KwsTenant* t = findTenant(streamOf(ctx, this_val)))
        t->suspended.store(false, std::memory_order_relaxed);
    return JS_UNDEFINED;
}

JSValue js_isActive(JSContext* ctx, JSValueConst this_val, int, JSValueConst*) {
    KwsTenant* t = findTenant(streamOf(ctx, this_val));
    return JS_NewBool(ctx, t && t->listening);
}

JSValue js_isSuspended(JSContext* ctx, JSValueConst this_val, int, JSValueConst*) {
    KwsTenant* t = findTenant(streamOf(ctx, this_val));
    return JS_NewBool(ctx,
        t && t->suspended.load(std::memory_order_relaxed));
}

JSValue js_isLoaded(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    return JS_NewBool(ctx, static_cast<bool>(g_kws.net));
}

JSValue js_sampleRate(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    return JS_NewInt32(ctx, g_kws.net ? g_kws.net->config().sample_rate : 0);
}

// Best current prefix progress across this stream's templates, [0,1] u2014 a
// lock-free library read, safe while the inference thread feeds. For UI meters.
JSValue js_prefixProgress(JSContext* ctx, JSValueConst this_val, int, JSValueConst*) {
    KwsTenant* t = findTenant(streamOf(ctx, this_val));
    if (!t) return JS_NewFloat64(ctx, 0.0);
    return JS_NewFloat64(ctx, t->spotter->prefix_progress());
}

// Per-template alignment telemetry for THIS stream u2014 the spotter's contribution
// to the fused listening surface. One coherent lock-free snapshot. Null until
// this stream has a spotter (i.e. something was enrolled / it listened).
JSValue js_progress(JSContext* ctx, JSValueConst this_val, int, JSValueConst*) {
    KwsTenant* t = findTenant(streamOf(ctx, this_val));
    if (!t) return JS_NULL;
    const brosoundml::ProgressSnapshot s = t->spotter->progress_snapshot();
    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "frames", JS_NewInt64(ctx, s.frames));
    JS_SetPropertyStr(ctx, obj, "generation", JS_NewUint32(ctx, s.generation));
    JSValue arr = JS_NewArray(ctx);
    for (int i = 0; i < s.count; ++i) {
        const brosoundml::TemplateProgress& e = s.templates[i];
        JSValue tv = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, tv, "name", JS_NewString(ctx, e.name));
        JS_SetPropertyStr(ctx, tv, "matched", JS_NewInt32(ctx, e.matched));
        JS_SetPropertyStr(ctx, tv, "length", JS_NewInt32(ctx, e.length));
        JS_SetPropertyStr(ctx, tv, "progress", JS_NewFloat64(ctx, e.progress));
        JS_SetPropertyStr(ctx, tv, "confidence", JS_NewFloat64(ctx, e.confidence));
        JS_SetPropertyStr(ctx, tv, "completions", JS_NewInt64(ctx, e.completions));
        JS_SetPropertyStr(ctx, tv, "lastAdvanceFrame",
                          JS_NewInt64(ctx, e.last_advance_frame));
        JS_SetPropertyStr(ctx, tv, "lastFireFrame",
                          JS_NewInt64(ctx, e.last_fire_frame));
        JS_SetPropertyUint32(ctx, arr, static_cast<std::uint32_t>(i), tv);
    }
    JS_SetPropertyStr(ctx, obj, "templates", arr);
    return obj;
}

// bro.kws.posterior(topK=3) -> { frame, top: [{ cls, label, p }, ...] } or null.
// The model's RAW per-frame readout for THIS stream u2014 what PhonemeNet is hearing
// now, independent of any template. last_posterior() is a lock-free seqlock
// read, safe while the inference thread feeds.
JSValue js_posterior(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    KwsTenant* t = findTenant(streamOf(ctx, this_val));
    if (!t || !t->spotter->loaded()) return JS_NULL;
    const std::vector<float> post = t->spotter->last_posterior();
    if (post.empty()) return JS_NULL;

    int topK = 3;
    if (argc >= 1 && JS_IsNumber(argv[0])) {
        int32_t k = 0;
        JS_ToInt32(ctx, &k, argv[0]);
        if (k > 0) topK = k;
    }
    const int K = static_cast<int>(post.size());
    if (topK > K) topK = K;

    const auto& cm = t->spotter->class_map();
    // Partial top-K by repeated argmax (K is small u2014 a few dozen classes).
    std::vector<int> taken(static_cast<std::size_t>(K), 0);
    JSValue arr = JS_NewArray(ctx);
    for (int r = 0; r < topK; ++r) {
        int best = -1;
        float bestP = -1.0f;
        for (int c = 0; c < K; ++c) {
            if (taken[(std::size_t)c]) continue;
            if (post[(std::size_t)c] > bestP) { bestP = post[(std::size_t)c]; best = c; }
        }
        if (best < 0) break;
        taken[(std::size_t)best] = 1;
        const char* label = (best < (int)cm.class_names.size())
                                ? cm.class_names[(std::size_t)best].c_str() : "?";
        JSValue e = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, e, "cls", JS_NewInt32(ctx, best));
        JS_SetPropertyStr(ctx, e, "label", JS_NewString(ctx, label));
        JS_SetPropertyStr(ctx, e, "p", JS_NewFloat64(ctx, bestP));
        JS_SetPropertyUint32(ctx, arr, static_cast<std::uint32_t>(r), e);
    }
    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "frame",
                      JS_NewInt64(ctx, t->spotter->progress_snapshot().frames));
    JS_SetPropertyStr(ctx, obj, "top", arr);
    return obj;
}

// Diagnostic surface over THIS stream's mic tap (cf. bro.wake.stats). Null for a
// non-mic (loopback) stream u2014 there is no mic tap to report.
JSValue js_stats(JSContext* ctx, JSValueConst this_val, int, JSValueConst*) {
    KwsTenant* t = findTenant(streamOf(ctx, this_val));
    const broaudio::MicTapId tap =
        t ? listenStreamTapId(t->streamId) : broaudio::kInvalidMicTapId;
    if (!t || !t->listening || !g_kws.audioEngine ||
        tap == broaudio::kInvalidMicTapId) {
        return JS_NULL;
    }
    auto s = g_kws.audioEngine->getMicTapStats(tap);
    JSValue o = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, o, "framesDelivered",
        JS_NewInt64(ctx, static_cast<int64_t>(s.framesDelivered)));
    JS_SetPropertyStr(ctx, o, "samplesDelivered",
        JS_NewInt64(ctx, static_cast<int64_t>(s.samplesDelivered)));
    JS_SetPropertyStr(ctx, o, "rollingPeak", JS_NewFloat64(ctx, s.rollingPeak));
    return o;
}

JSValue makeEventArray(JSContext* ctx,
                       const std::vector<brosoundml::SpotEvent>& events) {
    JSValue arr = JS_NewArray(ctx);
    for (std::uint32_t i = 0; i < events.size(); ++i) {
        JSValue o = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, o, "name", JS_NewString(ctx, events[i].name.c_str()));
        JS_SetPropertyStr(ctx, o, "confidence",
                          JS_NewFloat64(ctx, events[i].confidence));
        JS_SetPropertyUint32(ctx, arr, i, o);
    }
    return arr;
}

// Manual feed for tests / scripted scenarios on THIS stream. Samples must
// already be at the spotter's rate. Mirrors bro.wake.feed's mode split:
//   - Headless (no inference worker): the stream's bus runs synchronously on
//     this (the inference) thread u2014 it is ONE stream, so the feed advances every
//     attached tenant (bro.sense included) u2014 and the fired events come back as
//     [{name, confidence}]. Suspended fires are still returned (the caller
//     asked) but not queued for onSpot.
//   - Threaded: samples go into the stream's live ring; events surface via
//     onSpot. Returns undefined.
// Refuses to run while live MIC capture is active (two-producer race on the
// ring). Loopback streams: prefer headless/inline for scripted feeds.
JSValue js_feed(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    KwsTenant* t = findTenant(streamOf(ctx, this_val));
    if (!t || !t->listening)
        return JS_ThrowInternalError(ctx, "bro.kws.feed: this stream is not listening");
    if (g_kws.audioEngine && g_kws.audioEngine->isMicCapturing()) {
        return JS_ThrowInternalError(ctx,
            "bro.kws.feed: cannot feed while live mic capture is active "
            "(feed is for headless/offline use; the live tap already writes "
            "the ring)");
    }
    int n = 0;
    const float* p = (argc >= 1) ? readFloats(ctx, argv[0], n) : nullptr;
    if (!p)
        return JS_ThrowTypeError(ctx, "bro.kws.feed(Float32Array)");

    if (g_kws.inference && g_kws.inference->threaded()) {
        listenStreamWriteRing(t->streamId, p, n);
        return JS_UNDEFINED;
    }

    brosoundml::ListenFeedResult r;
    try {
        // The host runs the device scope and delivers spots through the same
        // onSpots hook the live path uses (suspended-gated there).
        r = listenStreamFeedInline(t->streamId, p, n);
    } catch (const std::exception& e) {
        return JS_ThrowInternalError(ctx, "bro.kws.feed: %s", e.what());
    }
    return makeEventArray(ctx, r.spots);
}

// Drain one tenant's SPSC event ring into its onSpot callback (main thread).
void drainTenant(JSContext* ctx, KwsTenant* t) {
    if (!t->listening) return;
    const std::uint64_t produced = t->produced.load(std::memory_order_acquire);
    std::uint64_t drained = t->drained.load(std::memory_order_relaxed);
    if (drained >= produced || JS_IsUndefined(t->onSpot)) return;
    while (drained < produced) {
        const int          idx    = t->eventIdx[drained % kEventSlots];
        const float        conf   = t->eventConf[drained % kEventSlots];
        const std::int64_t startF = t->eventStart[drained % kEventSlots];
        const std::int64_t endF   = t->eventEnd[drained % kEventSlots];
        drained++;
        // Publish the consumption BEFORE the JS call: the producer only needs
        // the slot back, and onSpot may run for a while.
        t->drained.store(drained, std::memory_order_release);
        if (idx < 0 || idx >= (int)t->names.size()) continue;
        // 3rd arg: the matched span on the frames axis (align with
        // bro.kws.progress().frames / bro.sense frames). Backward-compatible u2014
        // existing onSpot(name, conf) handlers ignore it.
        JSValue span = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, span, "startFrame", JS_NewInt64(ctx, startF));
        JS_SetPropertyStr(ctx, span, "endFrame", JS_NewInt64(ctx, endF));
        JS_SetPropertyStr(ctx, span, "matchedFrames",
                          JS_NewInt64(ctx, endF >= startF ? endF - startF + 1 : 0));
        JSValue args[3] = {
            JS_NewString(ctx, t->names[(std::size_t)idx].c_str()),
            JS_NewFloat64(ctx, conf),
            span,
        };
        JSValue r = JS_Call(ctx, t->onSpot, JS_UNDEFINED, 3, args);
        if (JS_IsException(r)) {
            JSValue exc = JS_GetException(ctx);
            const char* s = JS_ToCString(ctx, exc);
            std::fprintf(stderr, "[ERROR] [kws] onSpot threw: %s\n", s ? s : "?");
            if (s) JS_FreeCString(ctx, s);
            JS_FreeValue(ctx, exc);
        }
        JS_FreeValue(ctx, r);
        JS_FreeValue(ctx, args[0]);
        JS_FreeValue(ctx, args[1]);
        JS_FreeValue(ctx, args[2]);
    }
}

// u2500u2500u2500 View class registration u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500u2500

// The per-stream ops shared by bro.kws and stream.kws. Registered as method_raw
// on the KwsView prototype; the SAME function pointers go on bro.kws below.
void registerKwsViewClass(JSContext* ctx) {
    qjsbind::Class<KwsView>(ctx, "KwsStreamView", qjsbind::NoGlobal)
        .get("listening", [](KwsView* v) {
            KwsTenant* t = findTenant(v->streamId);
            return t && t->listening;
        })
        .method_raw("enroll",            js_enroll, 3)
        .method_raw("enrollFromAudio",   js_enrollFromAudio, 3)
        .method_raw("enrollFromClasses", js_enrollFromClasses, 3)
        .method_raw("inspect",           js_inspect, 1)
        .method_raw("remove",            js_remove, 1)
        .method_raw("clear",             js_clear, 0)
        .method_raw("templates",         js_templates, 0)
        .method_raw("reset",             js_reset, 0)
        .method_raw("listen",            js_listen, 1)
        .method_raw("stop",              js_stop, 0)
        .method_raw("suspend",           js_suspend, 0)
        .method_raw("resume",            js_resume, 0)
        .method_raw("isActive",          js_isActive, 0)
        .method_raw("isSuspended",       js_isSuspended, 0)
        .method_raw("isLoaded",          js_isLoaded, 0)
        .method_raw("sampleRate",        js_sampleRate, 0)
        .method_raw("prefixProgress",    js_prefixProgress, 0)
        .method_raw("progress",          js_progress, 0)
        .method_raw("posterior",         js_posterior, 1)
        .method_raw("stats",             js_stats, 0)
        .method_raw("feed",              js_feed, 1);
}

} // namespace

JSValue kwsViewFor(JSContext* ctx, std::uint32_t id) {
    return qjsbind::wrap<KwsView>(ctx, new KwsView{static_cast<StreamId>(id)});
}

// ---------------------------------------------------------------------------
// Install
// ---------------------------------------------------------------------------

void installKwsBindings(JSContext* ctx, broaudio::Engine* audioEngine, engine::AudioInference* inference) {
    g_kws.audioEngine = audioEngine;
        g_kws.inference   = inference;
        g_kws.ctx         = ctx;
    
        registerKwsViewClass(ctx);
    
        JSValue global = JS_GetGlobalObject(ctx);
        JSValue broObj = JS_GetPropertyStr(ctx, global, "bro");
        if (JS_IsUndefined(broObj) || JS_IsException(broObj)) {
            JS_FreeValue(ctx, broObj);
            broObj = JS_NewObject(ctx);
            JS_SetPropertyStr(ctx, global, "bro", JS_DupValue(ctx, broObj));
        }
    
        JSValue kws = JS_NewObject(ctx);
        // Namespace ops (shared net u2014 not stream-scoped).
        JS_SetPropertyStr(ctx, kws, "load",
            JS_NewCFunction(ctx, js_load, "load", 1));
        JS_SetPropertyStr(ctx, kws, "unload",
            JS_NewCFunction(ctx, js_unload, "unload", 0));
        // Per-stream ops u2014 on bro.kws they target the shared default-mic stream;
        // the SAME functions are method_raw on KwsView for stream.kws.
        JS_SetPropertyStr(ctx, kws, "enroll",
            JS_NewCFunction(ctx, js_enroll, "enroll", 3));
        JS_SetPropertyStr(ctx, kws, "enrollFromAudio",
            JS_NewCFunction(ctx, js_enrollFromAudio, "enrollFromAudio", 3));
        JS_SetPropertyStr(ctx, kws, "enrollFromClasses",
            JS_NewCFunction(ctx, js_enrollFromClasses, "enrollFromClasses", 3));
        JS_SetPropertyStr(ctx, kws, "inspect",
            JS_NewCFunction(ctx, js_inspect, "inspect", 1));
        JS_SetPropertyStr(ctx, kws, "remove",
            JS_NewCFunction(ctx, js_remove, "remove", 1));
        JS_SetPropertyStr(ctx, kws, "clear",
            JS_NewCFunction(ctx, js_clear, "clear", 0));
        JS_SetPropertyStr(ctx, kws, "templates",
            JS_NewCFunction(ctx, js_templates, "templates", 0));
        JS_SetPropertyStr(ctx, kws, "reset",
            JS_NewCFunction(ctx, js_reset, "reset", 0));
        JS_SetPropertyStr(ctx, kws, "listen",
            JS_NewCFunction(ctx, js_listen, "listen", 1));
        JS_SetPropertyStr(ctx, kws, "stop",
            JS_NewCFunction(ctx, js_stop, "stop", 0));
        JS_SetPropertyStr(ctx, kws, "suspend",
            JS_NewCFunction(ctx, js_suspend, "suspend", 0));
        JS_SetPropertyStr(ctx, kws, "resume",
            JS_NewCFunction(ctx, js_resume, "resume", 0));
        JS_SetPropertyStr(ctx, kws, "isActive",
            JS_NewCFunction(ctx, js_isActive, "isActive", 0));
        JS_SetPropertyStr(ctx, kws, "isSuspended",
            JS_NewCFunction(ctx, js_isSuspended, "isSuspended", 0));
        JS_SetPropertyStr(ctx, kws, "isLoaded",
            JS_NewCFunction(ctx, js_isLoaded, "isLoaded", 0));
        JS_SetPropertyStr(ctx, kws, "sampleRate",
            JS_NewCFunction(ctx, js_sampleRate, "sampleRate", 0));
        JS_SetPropertyStr(ctx, kws, "prefixProgress",
            JS_NewCFunction(ctx, js_prefixProgress, "prefixProgress", 0));
        JS_SetPropertyStr(ctx, kws, "progress",
            JS_NewCFunction(ctx, js_progress, "progress", 0));
        JS_SetPropertyStr(ctx, kws, "posterior",
            JS_NewCFunction(ctx, js_posterior, "posterior", 1));
        JS_SetPropertyStr(ctx, kws, "stats",
            JS_NewCFunction(ctx, js_stats, "stats", 0));
        JS_SetPropertyStr(ctx, kws, "feed",
            JS_NewCFunction(ctx, js_feed, "feed", 1));
        JS_SetPropertyStr(ctx, broObj, "kws", kws);
    
        JS_FreeValue(ctx, broObj);
        JS_FreeValue(ctx, global);
}

void tickKws(JSContext* ctx) {
    for (auto it = g_kws.tenants.begin(); it != g_kws.tenants.end(); ) {
        KwsTenant* t = it->second.get();
        // Prune a tenant whose stream has closed (handle .close()'d or GC'd).
        // The stream's teardown removed its inference task (a barrier), so the
        // onSpots closure can no longer run u2014 safe to drop the tenant. Default
        // mic is never invalid, so its tenant is never pruned here.
        if (!listenHostValid(t->streamId)) {
            stopListening(t);   // frees onSpot (detach is a no-op u2014 stream gone)
            it = g_kws.tenants.erase(it);
            continue;
        }
        drainTenant(ctx, t);
        ++it;
    }
}

void cleanupKwsBindings(JSContext* /*ctx*/) {
    unloadAll();
    g_kws.audioEngine = nullptr;
    g_kws.inference   = nullptr;
    g_kws.ctx         = nullptr;
}


} // namespace bro::js

#endif  // BRO_WITH_SOUNDML
