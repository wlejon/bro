// JS bindings for brosoundml::Rave — RAVE neural audio autoencoder inference.
//
// Installed onto bro.rave.* by installRaveBindings(). A converted RAVE v2 model
// (encoder + decoder + PQMF + latent PCA, <20M params) lives behind an opaque
// qjsbind handle. encode() compresses a waveform to a (nLatent x frames) latent
// whose PCA-sorted axes (loudness / pitch / timbre) are the editable curves a
// lab plots; decode() resynthesises a waveform from a (possibly edited) latent.
//
// encode/decode are fast (faster-than-realtime) so the methods are synchronous;
// the heavy file-IO + GPU-upload load step has an async form (opts.onReady).

#include "js/rave_bindings.h"
#include "js/async_job.h"

#include <qjsbind/qjsbind.h>

#include <brosoundml/rave.h>
#include <brosoundml/audio.h>

#include <brotensor/runtime.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace bro::js {

// ─── wrapper ─────────────────────────────────────────────────────────────────

struct RaveWrapper {
    std::unique_ptr<brosoundml::Rave> rave;
    brotensor::Device device = brotensor::Device::CPU;   // captured at load
    // Set while an async op runs on this model's thread; rejects a second
    // concurrent op (the model is single-owner).
    std::atomic<bool> busy{false};
};

// ─── helpers ─────────────────────────────────────────────────────────────────

static bool argStr(JSContext* ctx, JSValueConst v, std::string& out) {
    if (!JS_IsString(v)) return false;
    const char* s = JS_ToCString(ctx, v);
    if (!s) return false;
    out = s;
    JS_FreeCString(ctx, s);
    return true;
}

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

// ─── Rave methods ────────────────────────────────────────────────────────────

static RaveWrapper* raveSelf(JSContext* ctx, JSValueConst this_val) {
    return qjsbind::unwrap<RaveWrapper>(ctx, this_val);
}

// rave.encode(audio) -> { latent: Float32Array, nLatent, frames }
//   audio: mono Float32Array at rave.sampleRate. The latent is channel-major,
//   latent[c*frames + t] — nLatent time-series of length frames. Deterministic.
static JSValue js_rave_encode(JSContext* ctx, JSValueConst this_val,
                              int argc, JSValueConst* argv) {
    auto* w = raveSelf(ctx, this_val);
    if (!w) return JS_ThrowTypeError(ctx, "encode: not a Rave");
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "encode(audio): audio Float32Array required");
    std::vector<float> audio = qjsbind::read_float32_array(ctx, argv[0]);
    if (audio.empty())
        return JS_ThrowTypeError(ctx, "encode: audio must be a non-empty Float32Array");
    try {
        brotensor::DeviceScope scope(w->device);
        brosoundml::RaveLatent z = w->rave->encode(audio);
        JSValue obj = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, obj, "latent", qjsbind::make_float32_array(ctx, z.data));
        JS_SetPropertyStr(ctx, obj, "nLatent", JS_NewInt32(ctx, z.n_latent));
        JS_SetPropertyStr(ctx, obj, "frames",  JS_NewInt32(ctx, z.frames));
        return obj;
    } catch (const std::exception& e) {
        return JS_ThrowInternalError(ctx, "encode: %s", e.what());
    }
}

// rave.decode(latent, frames, opts?) -> { samples, sampleRate, channels }
//   latent: Float32Array of nLatent*frames, channel-major (latent[c*frames + t]).
//   Produces frames * totalRatio samples per channel. nLatent is inferred as
//   latent.length / frames and must equal rave.nLatent.
//   opts (optional):
//     addNoise?: bool   run RAVE's stochastic FFT noise-synth branch (breathy /
//                       unvoiced texture).
//     seed?:     number pins the white noise + stereo latent pad so the output
//                       is reproducible (default deterministic, no noise).
//     channels?: number >1 returns an INTERLEAVED multi-channel buffer
//                       (samples[t*channels + c]); RAVE's stereo decode runs the
//                       mono decoder once per channel.
//     stereoWidth?: number  std of the independent N(0,1) pad on the discarded
//                       latent dims per channel — the source of L/R decorrelation
//                       (RAVE-native = 1.0; defaults to 1.0 when channels>1).
//   `channels` in the result is the channel count (1 = plain mono).
static JSValue js_rave_decode(JSContext* ctx, JSValueConst this_val,
                              int argc, JSValueConst* argv) {
    auto* w = raveSelf(ctx, this_val);
    if (!w) return JS_ThrowTypeError(ctx, "decode: not a Rave");
    if (argc < 2)
        return JS_ThrowTypeError(ctx, "decode(latent, frames): latent and frames required");
    std::vector<float> latent = qjsbind::read_float32_array(ctx, argv[0]);
    if (latent.empty())
        return JS_ThrowTypeError(ctx, "decode: latent must be a non-empty Float32Array");
    int32_t frames = 0;
    JS_ToInt32(ctx, &frames, argv[1]);
    if (frames <= 0 || latent.size() % static_cast<size_t>(frames) != 0)
        return JS_ThrowTypeError(ctx, "decode: latent.length must be a positive multiple of frames");
    const int n_latent = static_cast<int>(latent.size() / static_cast<size_t>(frames));

    brosoundml::RaveDecodeOptions opts;
    bool widthSet = false;
    if (argc >= 3 && JS_IsObject(argv[2])) {
        JSValue an = JS_GetPropertyStr(ctx, argv[2], "addNoise");
        opts.add_noise = JS_ToBool(ctx, an) > 0;
        JS_FreeValue(ctx, an);
        JSValue sv = JS_GetPropertyStr(ctx, argv[2], "seed");
        if (!JS_IsUndefined(sv)) {
            int64_t s = 0;
            JS_ToInt64(ctx, &s, sv);
            opts.seed = static_cast<uint64_t>(s);
        }
        JS_FreeValue(ctx, sv);
        JSValue cv = JS_GetPropertyStr(ctx, argv[2], "channels");
        if (!JS_IsUndefined(cv)) {
            int32_t c = 1;
            JS_ToInt32(ctx, &c, cv);
            opts.channels = c < 1 ? 1 : c;
        }
        JS_FreeValue(ctx, cv);
        JSValue wv = JS_GetPropertyStr(ctx, argv[2], "stereoWidth");
        if (!JS_IsUndefined(wv)) {
            double width = 1.0;
            JS_ToFloat64(ctx, &width, wv);
            opts.latent_pad_std = static_cast<float>(width);
            widthSet = true;
        }
        JS_FreeValue(ctx, wv);
    }
    // Stereo with no explicit width: use RAVE's native unit-variance pad so the
    // channels actually decorrelate (otherwise both channels would be identical).
    if (opts.channels > 1 && !widthSet) opts.latent_pad_std = 1.0f;

    try {
        brotensor::DeviceScope scope(w->device);
        if (opts.channels <= 1) {
            brosoundml::AudioBuffer buf = w->rave->decode(latent.data(), n_latent, frames, opts);
            JSValue obj = JS_NewObject(ctx);
            JS_SetPropertyStr(ctx, obj, "samples", qjsbind::make_float32_array(ctx, buf.samples));
            JS_SetPropertyStr(ctx, obj, "sampleRate", JS_NewInt32(ctx, buf.sample_rate));
            JS_SetPropertyStr(ctx, obj, "channels", JS_NewInt32(ctx, 1));
            return obj;
        }
        brosoundml::RaveMultiBuffer buf =
            w->rave->decode_multi(latent.data(), n_latent, frames, opts);
        JSValue obj = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, obj, "samples", qjsbind::make_float32_array(ctx, buf.samples));
        JS_SetPropertyStr(ctx, obj, "sampleRate", JS_NewInt32(ctx, buf.sample_rate));
        JS_SetPropertyStr(ctx, obj, "channels", JS_NewInt32(ctx, buf.channels));
        return obj;
    } catch (const std::exception& e) {
        return JS_ThrowInternalError(ctx, "decode: %s", e.what());
    }
}

static void registerRaveClass(JSContext* ctx) {
    qjsbind::Class<RaveWrapper>(ctx, "Rave", qjsbind::NoGlobal)
        .get("loaded",      [](RaveWrapper* w) { return w->rave->loaded(); })
        .get("sampleRate",  [](RaveWrapper* w) { return w->rave->config().sampling_rate; })
        .get("nLatent",     [](RaveWrapper* w) { return w->rave->config().cropped_latent_size; })
        .get("fullLatent",  [](RaveWrapper* w) { return w->rave->config().full_latent_size; })
        .get("nBand",       [](RaveWrapper* w) { return w->rave->config().n_band; })
        .get("totalRatio",  [](RaveWrapper* w) { return w->rave->config().total_ratio; })
        .method_raw("encode", js_rave_encode, 1)
        .method_raw("decode", js_rave_decode, 2);
}

// ─── loader ──────────────────────────────────────────────────────────────────

static JSValue js_init(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    try {
        brotensor::init();
    } catch (const std::exception& e) {
        return JS_ThrowInternalError(ctx, "bro.rave.init: %s", e.what());
    }
    return JS_UNDEFINED;
}

static void buildRave(const std::string& dir, brotensor::Device dev,
                      std::unique_ptr<RaveWrapper>& w_out) {
    auto w = std::make_unique<RaveWrapper>();
    w->device = dev;
    w->rave = std::make_unique<brosoundml::Rave>();
    {
        brotensor::DeviceScope scope(dev);
        w->rave->load(dir, dev);
    }
    std::fprintf(stderr, "[INFO] [rave] RAVE loaded on %s\n", deviceName(dev));
    w_out = std::move(w);
}

struct RaveLoadState {
    std::string                  dir;
    brotensor::Device            dev = brotensor::Device::CPU;
    std::unique_ptr<RaveWrapper> w;
    JSValue onReady = JS_UNDEFINED;
    JSValue onError = JS_UNDEFINED;
    bool    hasReady = false, hasError = false;
};

// bro.rave.loadRave(modelDir, opts?) -> Rave         (sync)
//                                    -> AsyncHandle   (async, if opts.onReady)
//   modelDir holds config.json + model.safetensors (scripts/convert-rave.py).
//   opts.device: 'cuda' | 'cpu' — defaults to CUDA when available, else CPU.
//   opts.onReady(rave) / opts.onError(message): when onReady is a function the
//   load runs on a background thread and these fire on the JS thread.
static JSValue js_loadRave(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    std::string dir;
    if (argc < 1 || !argStr(ctx, argv[0], dir))
        return JS_ThrowTypeError(ctx, "loadRave(modelDir, opts?): path required");

    brotensor::init();
    brotensor::Device dev = autoDevice();
    if (argc >= 2) {
        std::string err;
        if (!parseDeviceOpt(ctx, argv[1], dev, err))
            return JS_ThrowTypeError(ctx, "loadRave: %s", err.c_str());
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
            std::unique_ptr<RaveWrapper> w;
            buildRave(dir, dev, w);
            return qjsbind::wrap<RaveWrapper>(ctx, w.release());
        } catch (const std::exception& e) {
            return JS_ThrowInternalError(ctx, "loadRave: %s", e.what());
        }
    }

    // ── Async path ──
    auto ls = std::make_shared<RaveLoadState>();
    ls->dir      = dir;
    ls->dev      = dev;
    ls->hasReady = true;
    ls->onReady  = JS_DupValue(ctx, onReady);
    ls->hasError = JS_IsFunction(ctx, onError);
    ls->onError  = ls->hasError ? JS_DupValue(ctx, onError) : JS_UNDEFINED;
    JS_FreeValue(ctx, onReady);
    JS_FreeValue(ctx, onError);

    auto work = [ls](const std::atomic<bool>&) {
        buildRave(ls->dir, ls->dev, ls->w);   // throws -> error
    };
    auto done = [ls](JSContext* c, bool /*cancelled*/, const std::string& error) {
        if (!error.empty() || !ls->w) {
            if (ls->hasError) {
                JSValue e = JS_NewString(c, error.empty() ? "loadRave failed" : error.c_str());
                JSValue r = JS_Call(c, ls->onError, JS_UNDEFINED, 1, &e);
                if (JS_IsException(r)) JS_FreeValue(c, JS_GetException(c));
                JS_FreeValue(c, r);
                JS_FreeValue(c, e);
            }
        } else {
            JSValue out = qjsbind::wrap<RaveWrapper>(c, ls->w.release());
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

// ─── install ─────────────────────────────────────────────────────────────────

void installRaveBindings(JSContext* ctx) {
    registerRaveClass(ctx);

    JSValue global = JS_GetGlobalObject(ctx);
    JSValue broObj = JS_GetPropertyStr(ctx, global, "bro");
    if (JS_IsUndefined(broObj) || JS_IsException(broObj)) {
        JS_FreeValue(ctx, broObj);
        broObj = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, global, "bro", JS_DupValue(ctx, broObj));
    }

    JSValue rave = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, rave, "init",
        JS_NewCFunction(ctx, js_init, "init", 0));
    JS_SetPropertyStr(ctx, rave, "loadRave",
        JS_NewCFunction(ctx, js_loadRave, "loadRave", 2));
    JS_SetPropertyStr(ctx, broObj, "rave", rave);

    JS_FreeValue(ctx, broObj);
    JS_FreeValue(ctx, global);
}

void cleanupRaveBindings(JSContext* /*ctx*/) {}

}  // namespace bro::js
