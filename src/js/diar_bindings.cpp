// JS bindings for brosoundml speaker diarization — streaming Sortformer.
//
// Installed onto bro.diar.* by installDiarBindings(). The model
// (nvidia/diar_streaming_sortformer_4spk-v2.1: a NEST/FastConformer acoustic
// encoder feeding an 18-layer Transformer head that emits, per 80 ms frame, an
// independent sigmoid activity probability for up to four speakers, with labels
// in arrival-time order) lives behind an opaque qjsbind handle. ~450 MB.
//
// The model runs on GPU by default — the loader places it on CUDA when a GPU
// backend is available (pass opts.device 'cpu' to force CPU). Two ways to drive
// it:
//
//   * Offline — bro.diar.diarize(model, audio, opts?) runs the whole clip in
//     one pass on a background thread (cancellable, onDone(result, info)).
//     model.diarize(audio) is the synchronous variant for short clips / tests.
//
//   * Streaming — model.createSession() gives a per-stream Arrival-Order
//     Speaker Cache; session.feed(audio, isLast) accumulates 16 kHz PCM and,
//     on isLast, runs the AOSC streaming loop over the buffered audio,
//     continuing the session's cache so speaker labels stay stable across
//     calls. A live consumer flushes a short window each tick (isLast=true)
//     for rolling, low-latency diarization.
//
// Every result is { numFrames, numSpeakers, frameSeconds, probs } where probs is
// a Float32Array row-major (numFrames, numSpeakers) of activity probabilities in
// [0, 1]; frame t starts at t * frameSeconds seconds.

#include "js/diar_bindings.h"
#include "util/interrupt.h"
#include "js/async_job.h"
#include "js/model_gate.h"
#include "js/marshal.h"

#include <qjsbind/qjsbind.h>

#include <brosoundml/sortformer.h>
#include <brosoundml/cluster_diarizer.h>
#include <brosoundml/audio.h>

#include <brotensor/runtime.h>
#include <brotensor/tensor.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <memory>
#include <string>
#include <vector>

namespace bro::js {

// ═══════════════════════════════════════════════════════════════════════════
// Wrapper structs
// ═══════════════════════════════════════════════════════════════════════════

// The model + its single-owner gate are held by shared_ptr so they outlive the
// JS model handle whenever a session is still alive, and so the async offline
// diarize and the synchronous feed/diarize over ONE model all serialize on the
// same busy flag — brosoundml's tier here is SERIALIZED (the streaming forward
// re-runs the encoder each step and shares the model's single GPU stream).
struct SortformerWrapper {
    std::shared_ptr<brosoundml::Sortformer> model;
    brotensor::Device device = brotensor::Device::CPU;   // captured at load
    // Set while an async bro.diar.diarize() runs on a background thread; the
    // synchronous diarize()/session.feed() reject if it is set. shared_ptr so
    // sessions share this exact gate with the model.
    ModelGate busy;
};

// A Sortformer streaming session over shared weights: its own Arrival-Order
// Speaker Cache + FIFO, one per stream. Holds the model + busy gate alive by
// shared_ptr (the model JS handle may be dropped while a session lives) and the
// move-only brosoundml::SortformerSession.
struct SortformerSessionWrapper {
    std::shared_ptr<brosoundml::Sortformer> model;
    ModelGate busy;   // shared with the model
    brotensor::Device                       device = brotensor::Device::CPU;
    brosoundml::SortformerSession           session;
};

// createSession() factory, registered on the model class but defined down in the
// session section.
static JSValue js_sortformer_createSession(JSContext*, JSValueConst, int, JSValueConst*);

// ═══════════════════════════════════════════════════════════════════════════
// Helpers (TU-local — same shape as the stt/tts bindings)
// ═══════════════════════════════════════════════════════════════════════════

static bool argStr(JSContext* ctx, JSValueConst v, std::string& out) {
    if (!JS_IsString(v)) return false;
    const char* s = JS_ToCString(ctx, v);
    if (!s) return false;
    out = s;
    JS_FreeCString(ctx, s);
    return true;
}

// Pick the default device — CUDA, then Metal, then CPU. brotensor::init() must
// have run beforehand so the GPU backend probes have fired.
static brotensor::Device autoDevice() {
    if (brotensor::is_available(brotensor::Device::CUDA))  return brotensor::Device::CUDA;
    if (brotensor::is_available(brotensor::Device::Metal)) return brotensor::Device::Metal;
    return brotensor::Device::CPU;
}

static const char* deviceName(brotensor::Device d) {
    switch (d) {
        case brotensor::Device::CUDA:  return "CUDA";
        case brotensor::Device::Metal: return "Metal";
        case brotensor::Device::CPU:   return "CPU";
    }
    return "?";
}

// Parse opts.device. On success writes the device into `out` and returns true.
// On a missing key leaves `out` untouched and returns true. On an unknown value
// sets `err` and returns false.
static bool parseDeviceOpt(JSContext* ctx, JSValueConst opts,
                           brotensor::Device& out, std::string& err) {
    if (!JS_IsObject(opts)) return true;
    JSValue v = JS_GetPropertyStr(ctx, opts, "device");
    if (JS_IsUndefined(v) || JS_IsNull(v)) { JS_FreeValue(ctx, v); return true; }
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


// Build the JS result object from a Diarization: { numFrames, numSpeakers,
// frameSeconds, probs: Float32Array }.
static JSValue makeDiarization(JSContext* c,
                               const brosoundml::Sortformer::Diarization& d) {
    JSValue obj = JS_NewObject(c);
    JS_SetPropertyStr(c, obj, "numFrames",    JS_NewInt32(c, d.num_frames));
    JS_SetPropertyStr(c, obj, "numSpeakers",  JS_NewInt32(c, d.num_speakers));
    JS_SetPropertyStr(c, obj, "frameSeconds", JS_NewFloat64(c, d.frame_seconds));
    JS_SetPropertyStr(c, obj, "probs",        qjsbind::make_float32_array(c, d.probs));
    return obj;
}

// Same result shape for the clustering diarizer (probs is one-hot per speech
// frame; numSpeakers is the discovered count).
static JSValue makeDiarization(JSContext* c,
                               const brosoundml::ClusterDiarizer::Diarization& d) {
    JSValue obj = JS_NewObject(c);
    JS_SetPropertyStr(c, obj, "numFrames",    JS_NewInt32(c, d.num_frames));
    JS_SetPropertyStr(c, obj, "numSpeakers",  JS_NewInt32(c, d.num_speakers));
    JS_SetPropertyStr(c, obj, "frameSeconds", JS_NewFloat64(c, d.frame_seconds));
    JS_SetPropertyStr(c, obj, "probs",        qjsbind::make_float32_array(c, d.probs));
    return obj;
}

// Parse a ClusterDiarizer::Config from a JS opts object (any missing key keeps
// its default). camelCase mirrors the C++ Config fields.
static void parseClusterConfig(JSContext* ctx, JSValueConst opts,
                               brosoundml::ClusterDiarizer::Config& cfg) {
    if (!JS_IsObject(opts)) return;
    auto numf = [&](const char* k, float& dst) {
        JSValue v = JS_GetPropertyStr(ctx, opts, k);
        if (JS_IsNumber(v)) { double t = dst; JS_ToFloat64(ctx, &t, v); dst = (float)t; }
        JS_FreeValue(ctx, v);
    };
    auto numi = [&](const char* k, int& dst) {
        JSValue v = JS_GetPropertyStr(ctx, opts, k);
        if (JS_IsNumber(v)) { int32_t t = dst; JS_ToInt32(ctx, &t, v); dst = t; }
        JS_FreeValue(ctx, v);
    };
    numf("clusterThreshold",  cfg.cluster_threshold);
    numf("vadThreshold",      cfg.vad_threshold);
    numf("windowSeconds",     cfg.window_seconds);
    numf("hopSeconds",        cfg.hop_seconds);
    numf("minWindowSeconds",  cfg.min_window_seconds);
    numf("minSpeakerSeconds", cfg.min_speaker_seconds);
    numi("maxSpeakers",       cfg.max_speakers);
}

// ═══════════════════════════════════════════════════════════════════════════
// Sortformer methods
// ═══════════════════════════════════════════════════════════════════════════

static SortformerWrapper* sortformerSelf(JSContext* ctx, JSValueConst this_val) {
    return qjsbind::unwrap<SortformerWrapper>(ctx, this_val);
}

// diarize(audio) -> { numFrames, numSpeakers, frameSeconds, probs }
//   audio: Float32Array @ 16 kHz, OR { samples, sampleRate } object. Synchronous
//   offline forward over the whole clip — blocks the JS thread, so prefer the
//   async bro.diar.diarize(model, audio, { onDone }) for anything but short
//   clips / tests.
static JSValue js_sortformer_diarize(JSContext* ctx, JSValueConst this_val,
                                     int argc, JSValueConst* argv) {
    auto* w = sortformerSelf(ctx, this_val);
    if (!w) return JS_ThrowTypeError(ctx, "diarize: not a Sortformer");
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "diarize(audio): audio required");
    if (w->busy.isBusy())
        return JS_ThrowInternalError(ctx,
            "diarize: an operation is already in flight on this model");

    brosoundml::AudioBuffer audio;
    std::string err;
    if (!readAudioBuffer(ctx, argv[0], audio, err))
        return JS_ThrowTypeError(ctx, "diarize: %s", err.c_str());

    try {
        brotensor::DeviceScope scope(w->device);
        auto out = w->model->diarize(audio);
        return makeDiarization(ctx, out);
    } catch (const std::exception& e) {
        return JS_ThrowInternalError(ctx, "diarize: %s", e.what());
    }
}

static void registerSortformerClass(JSContext* ctx) {
    qjsbind::Class<SortformerWrapper>(ctx, "Sortformer", qjsbind::NoGlobal)
        .get("loaded",       [](SortformerWrapper* w) { return w->model->loaded(); })
        .get("sampleRate",   [](SortformerWrapper* w) { return w->model->config().sample_rate; })
        .get("numSpeakers",  [](SortformerWrapper* w) { return w->model->config().num_spks; })
        .get("frameSeconds", [](SortformerWrapper* w) { return w->model->config().frame_seconds(); })
        .get("fcDModel",     [](SortformerWrapper* w) { return w->model->config().fc_d_model; })
        .get("tfDModel",     [](SortformerWrapper* w) { return w->model->config().tf_d_model; })
        .method_raw("diarize",       js_sortformer_diarize,       1)
        .method_raw("createSession", js_sortformer_createSession, 0);
}

// ═══════════════════════════════════════════════════════════════════════════
// bro.diar free functions
// ═══════════════════════════════════════════════════════════════════════════

static JSValue js_init(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    try {
        brotensor::init();
    } catch (const std::exception& e) {
        return JS_ThrowInternalError(ctx, "bro.diar.init: %s", e.what());
    }
    return JS_UNDEFINED;
}

// Build + load the Sortformer model from a checkpoint dir. Heavy + blocking
// (file IO + GPU upload); shared by the sync and async loadSortformer paths.
// Throws on error.
static void buildSortformer(const std::string& dir, brotensor::Device dev,
                            std::unique_ptr<SortformerWrapper>& w_out) {
    auto w = std::make_unique<SortformerWrapper>();
    w->device = dev;
    w->model  = std::make_unique<brosoundml::Sortformer>();
    {
        brotensor::DeviceScope scope(dev);
        w->model->load(dir, dev);
    }
    std::fprintf(stderr, "[INFO] [diar] Sortformer loaded on %s\n", deviceName(dev));
    w_out = std::move(w);
}

// State for an async loadSortformer.
struct SortformerLoadState {
    std::string                        dir;
    brotensor::Device                  dev = brotensor::Device::CPU;
    std::unique_ptr<SortformerWrapper> w;
    JSValue onReady = JS_UNDEFINED;
    JSValue onError = JS_UNDEFINED;
    bool    hasReady = false, hasError = false;
};

// bro.diar.loadSortformer(modelDir, opts?) -> Sortformer      (sync)
//                                          -> AsyncHandle     (async, if opts.onReady)
//   modelDir contains config.json + model.safetensors (convert with
//   brosoundml/scripts/convert-sortformer.py).
//   opts.device: 'cuda' | 'cpu' — defaults to CUDA when available, else CPU.
//   opts.onReady(model) / opts.onError(message): when onReady is a function the
//   load runs on a background thread (non-blocking) and these fire on the JS
//   thread.
static JSValue js_loadSortformer(JSContext* ctx, JSValueConst,
                                 int argc, JSValueConst* argv) {
    std::string dir;
    if (argc < 1 || !argStr(ctx, argv[0], dir))
        return JS_ThrowTypeError(ctx, "loadSortformer(modelDir, opts?): path required");

    brotensor::init();
    brotensor::Device dev = autoDevice();
    if (argc >= 2) {
        std::string err;
        if (!parseDeviceOpt(ctx, argv[1], dev, err))
            return JS_ThrowTypeError(ctx, "loadSortformer: %s", err.c_str());
    }

    const bool haveOpts = (argc >= 2) && JS_IsObject(argv[1]);
    JSValue onReady = haveOpts ? JS_GetPropertyStr(ctx, argv[1], "onReady") : JS_UNDEFINED;
    JSValue onError = haveOpts ? JS_GetPropertyStr(ctx, argv[1], "onError") : JS_UNDEFINED;
    const bool async = JS_IsFunction(ctx, onReady);

    // ── Sync path ──
    if (!async) {
        JS_FreeValue(ctx, onReady);
        JS_FreeValue(ctx, onError);
        try {
            std::unique_ptr<SortformerWrapper> w;
            buildSortformer(dir, dev, w);
            return qjsbind::wrap<SortformerWrapper>(ctx, w.release());
        } catch (const std::exception& e) {
            return JS_ThrowInternalError(ctx, "loadSortformer: %s", e.what());
        }
    }

    // ── Async path ──
    auto ls = std::make_shared<SortformerLoadState>();
    ls->dir      = dir;
    ls->dev      = dev;
    ls->hasReady = true;
    ls->onReady  = JS_DupValue(ctx, onReady);
    ls->hasError = JS_IsFunction(ctx, onError);
    ls->onError  = ls->hasError ? JS_DupValue(ctx, onError) : JS_UNDEFINED;
    JS_FreeValue(ctx, onReady);
    JS_FreeValue(ctx, onError);

    auto work = [ls](const std::atomic<bool>&) {
        buildSortformer(ls->dir, ls->dev, ls->w);   // throws -> error
    };
    auto done = [ls](JSContext* c, bool /*cancelled*/, const std::string& error) {
        if (!error.empty() || !ls->w) {
            if (ls->hasError) {
                JSValue e = JS_NewString(c, error.empty() ? "loadSortformer failed"
                                                          : error.c_str());
                JSValue r = JS_Call(c, ls->onError, JS_UNDEFINED, 1, &e);
                if (JS_IsException(r)) JS_FreeValue(c, JS_GetException(c));
                JS_FreeValue(c, r);
                JS_FreeValue(c, e);
            }
        } else {
            JSValue out = qjsbind::wrap<SortformerWrapper>(c, ls->w.release());
            JSValue r = JS_Call(c, ls->onReady, JS_UNDEFINED, 1, &out);
            if (JS_IsException(r)) JS_FreeValue(c, JS_GetException(c));
            JS_FreeValue(c, r);
            JS_FreeValue(c, out);
        }
        if (ls->hasReady) JS_FreeValue(c, ls->onReady);
        if (ls->hasError) JS_FreeValue(c, ls->onError);
    };
    return launchAsyncJob(ctx, std::move(work), nullptr, std::move(done));
}

// ═══════════════════════════════════════════════════════════════════════════
// Async offline diarize — bro.diar.diarize(model, audio, opts?)
// ═══════════════════════════════════════════════════════════════════════════
//
// Runs the whole-clip forward on a background thread so the JS thread stays
// responsive. Returns an AsyncHandle with .cancel(); opts.onDone(result, info)
// fires once on the JS thread with info = { cancelled, error? }. (The forward is
// monolithic — .cancel() drops the result rather than interrupting mid-clip.)

struct DiarJob {
    brosoundml::AudioBuffer                 audio;
    brosoundml::Sortformer::Diarization     result;   // filled by work()
    JSValue onDone   = JS_UNDEFINED;
    JSValue modelRef = JS_UNDEFINED;
    bool    hasOnDone = false;
};

static JSValue js_diar_diarize(JSContext* ctx, JSValueConst,
                               int argc, JSValueConst* argv) {
    if (argc < 2)
        return JS_ThrowTypeError(ctx,
            "diarize(model, audio, opts?): model and audio required");
    auto* w = qjsbind::unwrap<SortformerWrapper>(ctx, argv[0]);
    if (!w) return JS_ThrowTypeError(ctx, "diarize: first arg must be a Sortformer");

    auto job = std::make_shared<DiarJob>();
    std::string err;
    if (!readAudioBuffer(ctx, argv[1], job->audio, err))
        return JS_ThrowTypeError(ctx, "diarize: %s", err.c_str());

    JSValue onDone = JS_UNDEFINED;
    if (argc >= 3 && JS_IsObject(argv[2]))
        onDone = JS_GetPropertyStr(ctx, argv[2], "onDone");

    // Claim the model (single-owner; one op in flight across the model + its
    // sessions).
    if (!w->busy.tryClaim()) {
        JS_FreeValue(ctx, onDone);
        return JS_ThrowInternalError(ctx,
            "diarize: an operation is already in flight on this model");
    }

    job->hasOnDone = JS_IsFunction(ctx, onDone);
    job->onDone    = job->hasOnDone ? JS_DupValue(ctx, onDone) : JS_UNDEFINED;
    job->modelRef  = JS_DupValue(ctx, argv[0]);   // keep the model alive
    JS_FreeValue(ctx, onDone);

    SortformerWrapper* mw = w;

    auto work = [job, mw](const std::atomic<bool>&) {
        brotensor::DeviceScope scope(mw->device);
        job->result = mw->model->diarize(job->audio);
    };

    auto done = [job, mw](JSContext* c, bool cancelled, const std::string& error) {
        // Release the single-owner lock BEFORE invoking onDone so a callback that
        // synchronously starts the next op on this model succeeds instead of
        // tripping the in-flight guard (matches the stt/tts bindings).
        mw->busy.release();
        if (job->hasOnDone) {
            JSValue res = (error.empty() && !cancelled)
                              ? makeDiarization(c, job->result)
                              : JS_NULL;
            JSValue info = JS_NewObject(c);
            JS_SetPropertyStr(c, info, "cancelled", JS_NewBool(c, cancelled));
            if (!error.empty())
                JS_SetPropertyStr(c, info, "error", JS_NewString(c, error.c_str()));
            JSValue args[2] = { res, info };
            JSValue r = JS_Call(c, job->onDone, JS_UNDEFINED, 2, args);
            if (JS_IsException(r)) JS_FreeValue(c, JS_GetException(c));
            JS_FreeValue(c, r);
            JS_FreeValue(c, res);
            JS_FreeValue(c, info);
        }
        if (job->hasOnDone) JS_FreeValue(c, job->onDone);
        JS_FreeValue(c, job->modelRef);
    };

    return launchAsyncJob(ctx, std::move(work), nullptr, std::move(done));
}

// ═══════════════════════════════════════════════════════════════════════════
// Streaming session — model.createSession() + session.feed()/reset()
// ═══════════════════════════════════════════════════════════════════════════
//
// One loaded model behind N per-stream sessions (each its own Arrival-Order
// Speaker Cache) over ONE shared weight set. feed() runs SYNCHRONOUSLY on the JS
// thread: a live window is small and the forward is ~20x realtime on GPU, so it
// does not block noticeably — the same discipline as bro.stt's QwenAsrStream
// feed(). Sessions isolate STATE; all forwards over one model share the single
// GPU stream, so a session.feed() rejects while the async diarize holds the gate.

static SortformerSessionWrapper* sessionSelf(JSContext* ctx, JSValueConst v) {
    return qjsbind::unwrap<SortformerSessionWrapper>(ctx, v);
}

// model.createSession() -> SortformerSession
static JSValue js_sortformer_createSession(JSContext* ctx, JSValueConst this_val,
                                           int, JSValueConst*) {
    auto* w = sortformerSelf(ctx, this_val);
    if (!w) return JS_ThrowTypeError(ctx, "createSession: not a Sortformer");
    if (!w->model || !w->model->loaded())
        return JS_ThrowInternalError(ctx, "createSession: model is not loaded");
    try {
        brotensor::DeviceScope scope(w->device);
        auto sw = std::make_unique<SortformerSessionWrapper>();
        sw->model   = w->model;   // share weights (outlives the model handle)
        sw->busy    = w->busy;    // share the single-owner gate
        sw->device  = w->device;
        sw->session = w->model->make_session();
        return qjsbind::wrap<SortformerSessionWrapper>(ctx, sw.release());
    } catch (const std::exception& e) {
        return JS_ThrowInternalError(ctx, "createSession: %s", e.what());
    }
}

// session.feed(audio, isLast=false) -> { numFrames, numSpeakers, frameSeconds, probs }
//   audio: Float32Array @ 16 kHz, OR { samples, sampleRate } object. The session
//   accumulates audio and, on isLast=true, runs the AOSC streaming loop over the
//   buffered PCM — continuing this session's speaker cache so labels stay stable
//   across calls — and returns the activity for the finalized frames. With
//   isLast=false it only buffers and returns an empty result (numFrames=0). A
//   live consumer flushes a short window each tick (isLast=true) for rolling,
//   low-latency diarization.
static JSValue js_session_feed(JSContext* ctx, JSValueConst this_val,
                               int argc, JSValueConst* argv) {
    auto* sw = sessionSelf(ctx, this_val);
    if (!sw) return JS_ThrowTypeError(ctx, "feed: not a SortformerSession");
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "feed(audio, isLast?): audio required");
    if (sw->busy.isBusy())
        return JS_ThrowInternalError(ctx,
            "feed: an operation is already in flight on this model");

    brosoundml::AudioBuffer audio;
    std::string err;
    if (!readAudioBuffer(ctx, argv[0], audio, err))
        return JS_ThrowTypeError(ctx, "feed: %s", err.c_str());
    const bool isLast = (argc >= 2) && (JS_ToBool(ctx, argv[1]) == 1);

    try {
        brotensor::DeviceScope scope(sw->device);
        auto out = sw->model->feed(sw->session, audio, isLast);
        return makeDiarization(ctx, out);
    } catch (const std::exception& e) {
        return JS_ThrowInternalError(ctx, "feed: %s", e.what());
    }
}

// session.reset() — clear the Arrival-Order Speaker Cache for a fresh stream.
static JSValue js_session_reset(JSContext* ctx, JSValueConst this_val,
                                int, JSValueConst*) {
    auto* sw = sessionSelf(ctx, this_val);
    if (!sw) return JS_ThrowTypeError(ctx, "reset: not a SortformerSession");
    if (sw->busy.isBusy())
        return JS_ThrowInternalError(ctx, "reset: an operation is in flight on this model");
    try {
        brotensor::DeviceScope scope(sw->device);
        sw->model->reset(sw->session);
    } catch (const std::exception& e) {
        return JS_ThrowInternalError(ctx, "reset: %s", e.what());
    }
    return JS_UNDEFINED;
}

static void registerSortformerSessionClass(JSContext* ctx) {
    qjsbind::Class<SortformerSessionWrapper>(ctx, "SortformerSession", qjsbind::NoGlobal)
        .get("loaded",       [](SortformerSessionWrapper* w) { return w->model && w->model->loaded(); })
        .get("numSpeakers",  [](SortformerSessionWrapper* w) { return w->model->config().num_spks; })
        .get("frameSeconds", [](SortformerSessionWrapper* w) { return w->model->config().frame_seconds(); })
        .method_raw("feed",  js_session_feed,  2)
        .method_raw("reset", js_session_reset, 0);
}

// ═══════════════════════════════════════════════════════════════════════════
// ClusterDiarizer — Sortformer VAD + ECAPA x-vectors + centered-cosine clustering
// ═══════════════════════════════════════════════════════════════════════════
//
// The path for telling apart acoustically similar voices Sortformer's 4-slot
// head collapses (e.g. two women in the same pitch range). Owns its own
// Sortformer (VAD) + speaker encoder. Offline only for now: a whole-clip pass on
// a background thread, or a synchronous variant for short clips / tests. Same
// single-owner busy gate discipline as the Sortformer wrapper.

struct ClusterDiarizerWrapper {
    std::shared_ptr<brosoundml::ClusterDiarizer> model;
    brotensor::Device device = brotensor::Device::CPU;
    ModelGate busy;
};

static ClusterDiarizerWrapper* clusterSelf(JSContext* ctx, JSValueConst v) {
    return qjsbind::unwrap<ClusterDiarizerWrapper>(ctx, v);
}

// model.diarize(audio, opts?) -> { numFrames, numSpeakers, frameSeconds, probs }
//   Synchronous whole-clip diarization (blocks the JS thread — prefer the async
//   bro.diar.clusterDiarize for anything but short clips / tests). opts may carry
//   clusterThreshold / windowSeconds / hopSeconds / vadThreshold / maxSpeakers...
static JSValue js_cluster_diarize_method(JSContext* ctx, JSValueConst this_val,
                                         int argc, JSValueConst* argv) {
    auto* w = clusterSelf(ctx, this_val);
    if (!w) return JS_ThrowTypeError(ctx, "diarize: not a ClusterDiarizer");
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "diarize(audio, opts?): audio required");
    if (w->busy.isBusy())
        return JS_ThrowInternalError(ctx,
            "diarize: an operation is already in flight on this model");

    brosoundml::AudioBuffer audio;
    std::string err;
    if (!readAudioBuffer(ctx, argv[0], audio, err))
        return JS_ThrowTypeError(ctx, "diarize: %s", err.c_str());
    brosoundml::ClusterDiarizer::Config cfg;
    if (argc >= 2) parseClusterConfig(ctx, argv[1], cfg);

    try {
        brotensor::DeviceScope scope(w->device);
        auto out = w->model->diarize(audio, cfg);
        return makeDiarization(ctx, out);
    } catch (const std::exception& e) {
        return JS_ThrowInternalError(ctx, "diarize: %s", e.what());
    }
}

static void registerClusterDiarizerClass(JSContext* ctx) {
    qjsbind::Class<ClusterDiarizerWrapper>(ctx, "ClusterDiarizer", qjsbind::NoGlobal)
        .get("loaded", [](ClusterDiarizerWrapper* w) { return w->model && w->model->loaded(); })
        .method_raw("diarize", js_cluster_diarize_method, 2);
}

// Build + load (Sortformer VAD dir + speaker-encoder dir). Heavy + blocking.
static void buildClusterDiarizer(const std::string& sortformerDir,
                                 const std::string& encoderDir,
                                 brotensor::Device dev,
                                 std::unique_ptr<ClusterDiarizerWrapper>& w_out) {
    auto w = std::make_unique<ClusterDiarizerWrapper>();
    w->device = dev;
    w->model  = std::make_unique<brosoundml::ClusterDiarizer>();
    {
        brotensor::DeviceScope scope(dev);
        w->model->load(sortformerDir, encoderDir, dev);
    }
    std::fprintf(stderr, "[INFO] [diar] ClusterDiarizer loaded on %s\n", deviceName(dev));
    w_out = std::move(w);
}

struct ClusterLoadState {
    std::string sortformerDir, encoderDir;
    brotensor::Device dev = brotensor::Device::CPU;
    std::unique_ptr<ClusterDiarizerWrapper> w;
    JSValue onReady = JS_UNDEFINED, onError = JS_UNDEFINED;
    bool hasReady = false, hasError = false;
};

// bro.diar.loadClusterDiarizer(sortformerDir, speakerEncoderDir, opts?)
//   -> ClusterDiarizer (sync) | AsyncHandle (async, if opts.onReady)
static JSValue js_loadClusterDiarizer(JSContext* ctx, JSValueConst,
                                      int argc, JSValueConst* argv) {
    std::string sortformerDir, encoderDir;
    if (argc < 2 || !argStr(ctx, argv[0], sortformerDir) || !argStr(ctx, argv[1], encoderDir))
        return JS_ThrowTypeError(ctx,
            "loadClusterDiarizer(sortformerDir, speakerEncoderDir, opts?): two paths required");

    brotensor::init();
    brotensor::Device dev = autoDevice();
    if (argc >= 3) {
        std::string err;
        if (!parseDeviceOpt(ctx, argv[2], dev, err))
            return JS_ThrowTypeError(ctx, "loadClusterDiarizer: %s", err.c_str());
    }

    const bool haveOpts = (argc >= 3) && JS_IsObject(argv[2]);
    JSValue onReady = haveOpts ? JS_GetPropertyStr(ctx, argv[2], "onReady") : JS_UNDEFINED;
    JSValue onError = haveOpts ? JS_GetPropertyStr(ctx, argv[2], "onError") : JS_UNDEFINED;
    const bool async = JS_IsFunction(ctx, onReady);

    if (!async) {
        JS_FreeValue(ctx, onReady);
        JS_FreeValue(ctx, onError);
        try {
            std::unique_ptr<ClusterDiarizerWrapper> w;
            buildClusterDiarizer(sortformerDir, encoderDir, dev, w);
            return qjsbind::wrap<ClusterDiarizerWrapper>(ctx, w.release());
        } catch (const std::exception& e) {
            return JS_ThrowInternalError(ctx, "loadClusterDiarizer: %s", e.what());
        }
    }

    auto ls = std::make_shared<ClusterLoadState>();
    ls->sortformerDir = sortformerDir;
    ls->encoderDir    = encoderDir;
    ls->dev           = dev;
    ls->hasReady      = true;
    ls->onReady       = JS_DupValue(ctx, onReady);
    ls->hasError      = JS_IsFunction(ctx, onError);
    ls->onError       = ls->hasError ? JS_DupValue(ctx, onError) : JS_UNDEFINED;
    JS_FreeValue(ctx, onReady);
    JS_FreeValue(ctx, onError);

    auto work = [ls](const std::atomic<bool>&) {
        buildClusterDiarizer(ls->sortformerDir, ls->encoderDir, ls->dev, ls->w);
    };
    auto done = [ls](JSContext* c, bool /*cancelled*/, const std::string& error) {
        if (!error.empty() || !ls->w) {
            if (ls->hasError) {
                JSValue e = JS_NewString(c, error.empty() ? "loadClusterDiarizer failed"
                                                          : error.c_str());
                JSValue r = JS_Call(c, ls->onError, JS_UNDEFINED, 1, &e);
                if (JS_IsException(r)) JS_FreeValue(c, JS_GetException(c));
                JS_FreeValue(c, r);
                JS_FreeValue(c, e);
            }
        } else {
            JSValue out = qjsbind::wrap<ClusterDiarizerWrapper>(c, ls->w.release());
            JSValue r = JS_Call(c, ls->onReady, JS_UNDEFINED, 1, &out);
            if (JS_IsException(r)) JS_FreeValue(c, JS_GetException(c));
            JS_FreeValue(c, r);
            JS_FreeValue(c, out);
        }
        if (ls->hasReady) JS_FreeValue(c, ls->onReady);
        if (ls->hasError) JS_FreeValue(c, ls->onError);
    };
    return launchAsyncJob(ctx, std::move(work), nullptr, std::move(done));
}

// Async whole-clip clustering diarization on a background thread.
// bro.diar.clusterDiarize(model, audio, opts?) -> AsyncHandle (.cancel())
//   opts.onDone(result|null, { cancelled, error? }); opts also carries the Config
//   knobs (clusterThreshold, windowSeconds, ...).
struct ClusterDiarJob {
    brosoundml::AudioBuffer                   audio;
    brosoundml::ClusterDiarizer::Config       cfg;
    brosoundml::ClusterDiarizer::Diarization  result;
    JSValue onDone   = JS_UNDEFINED;
    JSValue modelRef = JS_UNDEFINED;
    bool    hasOnDone = false;
};

static JSValue js_diar_clusterDiarize(JSContext* ctx, JSValueConst,
                                      int argc, JSValueConst* argv) {
    if (argc < 2)
        return JS_ThrowTypeError(ctx,
            "clusterDiarize(model, audio, opts?): model and audio required");
    auto* w = clusterSelf(ctx, argv[0]);
    if (!w) return JS_ThrowTypeError(ctx, "clusterDiarize: first arg must be a ClusterDiarizer");

    auto job = std::make_shared<ClusterDiarJob>();
    std::string err;
    if (!readAudioBuffer(ctx, argv[1], job->audio, err))
        return JS_ThrowTypeError(ctx, "clusterDiarize: %s", err.c_str());
    if (argc >= 3) parseClusterConfig(ctx, argv[2], job->cfg);

    JSValue onDone = JS_UNDEFINED;
    if (argc >= 3 && JS_IsObject(argv[2]))
        onDone = JS_GetPropertyStr(ctx, argv[2], "onDone");

    if (!w->busy.tryClaim()) {
        JS_FreeValue(ctx, onDone);
        return JS_ThrowInternalError(ctx,
            "clusterDiarize: an operation is already in flight on this model");
    }

    job->hasOnDone = JS_IsFunction(ctx, onDone);
    job->onDone    = job->hasOnDone ? JS_DupValue(ctx, onDone) : JS_UNDEFINED;
    job->modelRef  = JS_DupValue(ctx, argv[0]);
    JS_FreeValue(ctx, onDone);

    ClusterDiarizerWrapper* mw = w;
    auto work = [job, mw](const std::atomic<bool>&) {
        brotensor::DeviceScope scope(mw->device);
        job->result = mw->model->diarize(job->audio, job->cfg);
    };
    auto done = [job, mw](JSContext* c, bool cancelled, const std::string& error) {
        mw->busy.release();
        if (job->hasOnDone) {
            JSValue res = (error.empty() && !cancelled)
                              ? makeDiarization(c, job->result) : JS_NULL;
            JSValue info = JS_NewObject(c);
            JS_SetPropertyStr(c, info, "cancelled", JS_NewBool(c, cancelled));
            if (!error.empty())
                JS_SetPropertyStr(c, info, "error", JS_NewString(c, error.c_str()));
            JSValue args[2] = { res, info };
            JSValue r = JS_Call(c, job->onDone, JS_UNDEFINED, 2, args);
            if (JS_IsException(r)) JS_FreeValue(c, JS_GetException(c));
            JS_FreeValue(c, r);
            JS_FreeValue(c, res);
            JS_FreeValue(c, info);
        }
        if (job->hasOnDone) JS_FreeValue(c, job->onDone);
        JS_FreeValue(c, job->modelRef);
    };
    return launchAsyncJob(ctx, std::move(work), nullptr, std::move(done));
}

// ═══════════════════════════════════════════════════════════════════════════
// Install
// ═══════════════════════════════════════════════════════════════════════════

void installDiarBindings(JSContext* ctx) {
    registerSortformerClass(ctx);
    registerSortformerSessionClass(ctx);
    registerClusterDiarizerClass(ctx);

    JSValue global = JS_GetGlobalObject(ctx);
    JSValue broObj = JS_GetPropertyStr(ctx, global, "bro");
    if (JS_IsUndefined(broObj) || JS_IsException(broObj)) {
        JS_FreeValue(ctx, broObj);
        broObj = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, global, "bro", JS_DupValue(ctx, broObj));
    }

    JSValue diar = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, diar, "init",
        JS_NewCFunction(ctx, js_init, "init", 0));
    JS_SetPropertyStr(ctx, diar, "loadSortformer",
        JS_NewCFunction(ctx, js_loadSortformer, "loadSortformer", 2));
    JS_SetPropertyStr(ctx, diar, "diarize",
        JS_NewCFunction(ctx, js_diar_diarize, "diarize", 3));
    JS_SetPropertyStr(ctx, diar, "loadClusterDiarizer",
        JS_NewCFunction(ctx, js_loadClusterDiarizer, "loadClusterDiarizer", 3));
    JS_SetPropertyStr(ctx, diar, "clusterDiarize",
        JS_NewCFunction(ctx, js_diar_clusterDiarize, "clusterDiarize", 3));
    JS_SetPropertyStr(ctx, broObj, "diar", diar);

    JS_FreeValue(ctx, broObj);
    JS_FreeValue(ctx, global);
}

void cleanupDiarBindings(JSContext* /*ctx*/) {}

}  // namespace bro::js
