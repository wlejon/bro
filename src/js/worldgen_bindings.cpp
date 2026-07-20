#include "js/worldgen_bindings.h"
#if BRO_WITH_DIFFUSION  // modular-build feature gate

#include <qjsbind/qjsbind.h>

#include "js/async_job.h"

#include <brodiffusion/terrain/world_pipeline.h>
#include <brotensor/runtime.h>
#include <api/api.h>  // brokit::api::resolveAssetPath

#include <atomic>
#include <cstdint>
#include <cmath>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace bro::js {
namespace {

namespace td = brodiffusion::terrain;

// Same shape as the other model bindings' local helper: a string argument, or
// a clean false so the caller can throw its own typed error.
bool argStr(JSContext* ctx, JSValueConst v, std::string& out) {
    if (!JS_IsString(v)) return false;
    const char* s = JS_ToCString(ctx, v);
    if (!s) return false;
    out = s;
    JS_FreeCString(ctx, s);
    return true;
}

// ─── handle ──────────────────────────────────────────────────────────────────

struct WorldWrapper {
    std::unique_ptr<td::WorldPipeline> pipe;

    // A pipeline memoises tiles across calls and that cache is not thread-safe,
    // so exactly one request may be in flight per world. This is claimed on the
    // JS thread before the job is launched and released in `done`, which also
    // runs on the JS thread — so it is only ever touched from one thread and the
    // atomic is belt-and-braces rather than load-bearing.
    std::atomic<bool> busy{false};
};

// The default request margin, in cells. See the header: elevation() pads its
// reconstruction outward and crops, but that pad is finite, so a band at the
// edge of any request is reconstructed from truncated support. Measured on the
// 30 m checkpoint, the disagreement between a region read alone and the same
// region read inside a larger one is confined to the outermost FOUR cells
// (0.18 m, 0.13 m, 0.08 m, 0.03 m) and is then bit-exactly zero further in.
//
// So over-requesting by this much and cropping makes independently generated
// tiles agree EXACTLY where they meet, which is what stops a chunked consumer
// from showing a seam at every tile boundary. Eight is double the measured
// depth — the extra cells cost area, not correctness, and the measurement is
// checkpoint-specific.
constexpr std::int64_t kDefaultMargin = 8;

// ─── shared request state ────────────────────────────────────────────────────

// Written by the background thread, read by the JS thread after the job ends.
// The handoff is the async runner's own completion ordering: `done` is only
// invoked once the work thread has been joined, so no additional publication is
// needed.
struct ElevJob {
    WorldWrapper* w = nullptr;
    std::int64_t  i1 = 0, j1 = 0, i2 = 0, j2 = 0;
    std::int64_t  margin = kDefaultMargin;
    td::TileBuffer out;

    JSValue onDone  = JS_UNDEFINED;
    JSValue onError = JS_UNDEFINED;
    bool    hasDone = false, hasError = false;
    JSValue worldRef = JS_UNDEFINED;   // keeps the world alive for the job
};


// Crop `margin` cells off every side of a (1, h, w) tile, in place.
td::TileBuffer cropMargin(const td::TileBuffer& src, std::int64_t margin) {
    if (margin <= 0) return src;
    const std::int64_t h = src.shape[1], w = src.shape[2];
    const std::int64_t oh = h - 2 * margin, ow = w - 2 * margin;

    td::TileBuffer out;
    out.shape = {1, oh, ow};
    out.data.resize(static_cast<std::size_t>(oh) * ow);
    for (std::int64_t z = 0; z < oh; ++z) {
        const float* row = src.data.data() + (z + margin) * w + margin;
        std::memcpy(out.data.data() + static_cast<std::size_t>(z) * ow, row,
                    static_cast<std::size_t>(ow) * sizeof(float));
    }
    return out;
}

// Build { width, height, cellSize, data: Float32Array } from a (1, h, w) tile.
JSValue makeElevResult(JSContext* ctx, const td::TileBuffer& tile,
                       double cellSize) {
    // elevation() is documented to return (1, h, w). Anything else means the
    // pipeline changed shape under us; surfacing that is better than indexing
    // into it and producing a plausible-looking map of nothing.
    if (tile.shape.size() != 3 || tile.shape[0] != 1) {
        return JS_ThrowInternalError(
            ctx, "worldgen: elevation returned an unexpected shape");
    }
    const std::int64_t h = tile.shape[1];
    const std::int64_t w = tile.shape[2];

    JSValue lenVal = JS_NewInt64(ctx, static_cast<std::int64_t>(tile.data.size()));
    JSValue arr = JS_NewTypedArray(ctx, 1, &lenVal, JS_TYPED_ARRAY_FLOAT32);
    JS_FreeValue(ctx, lenVal);
    if (JS_IsException(arr)) return arr;

    size_t byteOff = 0, viewLen = 0;
    JSValue abuf = JS_GetTypedArrayBuffer(ctx, arr, &byteOff, &viewLen, nullptr);
    if (JS_IsException(abuf)) { JS_FreeValue(ctx, arr); return abuf; }
    size_t abufLen = 0;
    uint8_t* ptr = JS_GetArrayBuffer(ctx, &abufLen, abuf);
    if (ptr) {
        std::memcpy(ptr + byteOff, tile.data.data(),
                    tile.data.size() * sizeof(float));
    }
    JS_FreeValue(ctx, abuf);

    JSValue res = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, res, "width",    JS_NewInt64(ctx, w));
    JS_SetPropertyStr(ctx, res, "height",   JS_NewInt64(ctx, h));
    JS_SetPropertyStr(ctx, res, "cellSize", JS_NewFloat64(ctx, cellSize));
    JS_SetPropertyStr(ctx, res, "data",     arr);
    return res;
}

// Read the four half-open cell bounds. They are int64 and may be negative, so
// they go through JS_ToInt64 rather than the int32 helpers.
bool readBounds(JSContext* ctx, int argc, JSValueConst* argv,
                std::int64_t& i1, std::int64_t& j1,
                std::int64_t& i2, std::int64_t& j2,
                std::int64_t& margin) {
    if (argc < 4) {
        JS_ThrowTypeError(ctx, "elevation(i1, j1, i2, j2, opts?): four bounds required");
        return false;
    }
    if (JS_ToInt64(ctx, &i1, argv[0]) || JS_ToInt64(ctx, &j1, argv[1]) ||
        JS_ToInt64(ctx, &i2, argv[2]) || JS_ToInt64(ctx, &j2, argv[3])) {
        return false;
    }
    if (i2 <= i1 || j2 <= j1) {
        JS_ThrowRangeError(ctx,
            "elevation: bounds are half-open, so i2 > i1 and j2 > j1 required");
        return false;
    }

    // opts.margin overrides the default. 0 asks for the pipeline's raw output,
    // which is what the parity gates compare against; anything >= 4 makes
    // neighbouring tiles agree exactly.
    margin = kDefaultMargin;
    JSValueConst opts = argc > 4 ? argv[4] : JS_UNDEFINED;
    if (JS_IsObject(opts)) {
        JSValue m = JS_GetPropertyStr(ctx, opts, "margin");
        if (!JS_IsUndefined(m)) {
            double mv = 0;
            JS_ToFloat64(ctx, &mv, m);
            margin = static_cast<std::int64_t>(mv);
            if (margin < 0) margin = 0;
        }
        JS_FreeValue(ctx, m);
    }
    return true;
}

// ─── world.coarse(i1, j1, i2, j2) ────────────────────────────────────────────
//
// The coarse net's elevation channel, in metres, at 7.68 km per cell — 256x
// coarser than elevation() and covering 65536x the area for comparable work.
// This is the stage to draw a horizon or a view from orbit with; asking
// elevation() for terrain 100 km away is the wrong mechanism, not merely a slow
// one.
//
// Channel 0 is ALREADY the signed square root of metres — it does NOT take the
// coarse_means/coarse_stds normalisation, which applies to the network's own
// working domain rather than to this output. Undoing only the square root gives
// values that track the 30 m data averaged to the same cells at r = 0.9972,
// mean absolute error 58 m. Applying the normalisation as well inflates
// everything by a factor of ~1550, which still looks like plausible terrain and
// is exactly the kind of wrong that renders fine.
JSValue js_world_coarse(JSContext* ctx, JSValueConst this_val,
                        int argc, JSValueConst* argv) {
    auto* w = qjsbind::unwrap<WorldWrapper>(ctx, this_val);
    if (!w || !w->pipe) return JS_ThrowTypeError(ctx, "coarse: world is destroyed");

    std::int64_t i1, j1, i2, j2, margin;
    if (!readBounds(ctx, argc, argv, i1, j1, i2, j2, margin)) return JS_EXCEPTION;

    bool expected = false;
    if (!w->busy.compare_exchange_strong(expected, true)) {
        return JS_ThrowInternalError(ctx, "coarse: this world is already generating");
    }
    td::TileBuffer tile;
    try {
        tile = w->pipe->coarse_normalized(i1, j1, i2, j2);
    } catch (const std::exception& e) {
        w->busy.store(false);
        return JS_ThrowInternalError(ctx, "coarse: %s", e.what());
    }
    w->busy.store(false);

    // coarse_normalized is (6, h, w); take channel 0 and square it back.
    if (tile.shape.size() != 3 || tile.shape[0] < 1) {
        return JS_ThrowInternalError(ctx, "coarse: unexpected shape");
    }
    const std::int64_t h = tile.shape[1], w2 = tile.shape[2];
    td::TileBuffer out;
    out.shape = {1, h, w2};
    out.data.resize(static_cast<std::size_t>(h) * w2);
    for (std::size_t i = 0; i < out.data.size(); ++i) {
        const float v = tile.data[i];
        out.data[i] = std::copysign(v * v, v);
    }
    const double cell = w->pipe->config().native_resolution *
                        w->pipe->config().latent_compression * 32.0;
    return makeElevResult(ctx, out, cell);
}

// ─── world.stage(name, i1, j1, i2, j2) ───────────────────────────────────────
//
// Every intermediate the pipeline computes on the way to elevation(), as a
// planar multi-channel buffer. elevation() is the product; this is the DAG that
// produces it, and each stage is meaningful on its own.
//
// The point is diagnostic rather than decorative. The pipeline is three nets in
// series, and when the output is wrong the only question worth asking is which
// stage it was already wrong in. Reading a stage alone halves the composition
// depth below it, which is what distinguishes error accumulating per step from
// error arriving already-formed out of the coarse net.
//
// SYNCHRONOUS, like coarse() and elevationSync(): a large request is seconds of
// work, so call it from a Worker. elevation() is the async form and is the one
// a frame loop should use.
//
// Channel semantics are upstream's (CHANNEL_NAMES in random_sampler.py) and are
// reported alongside the data rather than left for the caller to remember. The
// units that upstream does NOT state are labelled as unknown rather than
// guessed — a mislabelled climate field renders perfectly and is silently
// wrong, which is the failure mode this whole surface exists to catch.
struct StageSpec {
    const char* name;
    int         channels;
    const char* chanNames[6];
    const char* chanUnits[6];
};

// Physical units where the source states them, "?" where it does not.
//
// coarse ch0/ch1 are the SIGNED SQUARE ROOT of metres and are squared back
// below. ch1 is p5, the 5th-percentile fine elevation within the coarse cell;
// the net predicts elev - p5 and the pipeline rebuilds it, which is why its
// normalisation constants look nothing like ch0's.
//
// Note ch0 is the mean of signed-sqrt elevation over the cell, so squaring it
// gives the square of a mean of roots, NOT the mean elevation in metres. Close
// enough to draw and to pick biomes with; not a number to quote.
constexpr StageSpec kStages[] = {
    {"coarse", 6,
     {"elevation", "p5", "temperature", "temperatureSeasonality",
      "precipitation", "precipitationSeasonality"},
     {"m", "m", "degC", "?", "mm/yr", "?"}},
    // Channels 0..3 are an opaque learned code — no name or unit appears
    // anywhere in brodiffusion or upstream, so they get none here. Channel 4 is
    // the low-frequency elevation band elevation() reconstructs against.
    {"latent", 5,
     {"latent0", "latent1", "latent2", "latent3", "lowFrequency"},
     {"", "", "", "", "m"}},
    // The first of the latent stage's two TrigFlow steps: same five channels,
    // sampled from zero at a much higher noise level. Strictly noisier than
    // `latent`, and exposed so a discrepancy can be attributed to one step
    // rather than to the pair.
    {"latentInit", 5,
     {"latent0", "latent1", "latent2", "latent3", "lowFrequency"},
     {"", "", "", "", "m"}},
    // A high-pass Laplacian residual in the network's own standardised domain.
    // NOT metres, and not convertible to metres on its own: it needs the latent
    // low band, a denoise round-trip and a bilinear upsample, which is exactly
    // what elevation() does. Left raw rather than half-converted.
    {"residual", 1, {"residual"}, {"standardised"}},
    {"elevation", 1, {"elevation"}, {"m"}},
};

// The transformed result of one stage request. Plain data, no JSValues — the
// work runs on a background thread, which may not touch the JS heap.
struct StageOut {
    const StageSpec*   spec = nullptr;
    std::vector<float> data;
    std::int64_t       channels = 0, width = 0, height = 0;
    double             cellSize = 0.0;
};

// Fetch a stage and put it in the domain kStages advertises. Throws on failure,
// like the pipeline calls it wraps; both entry points below catch.
StageOut computeStage(td::WorldPipeline& pipe, const StageSpec& spec,
                      std::int64_t i1, std::int64_t j1,
                      std::int64_t i2, std::int64_t j2) {
    const std::string name = spec.name;
    td::TileBuffer tile;
    if      (name == "coarse")     tile = pipe.coarse_normalized(i1, j1, i2, j2);
    else if (name == "latent")     tile = pipe.latent_normalized(i1, j1, i2, j2);
    else if (name == "latentInit") tile = pipe.latent_init(i1, j1, i2, j2);
    else if (name == "residual")   tile = pipe.residual_normalized(i1, j1, i2, j2);
    else                           tile = pipe.elevation(i1, j1, i2, j2);

    if (tile.shape.size() != 3)
        throw std::runtime_error("unexpected shape");

    StageOut out;
    out.spec   = &spec;
    out.height = tile.shape[1];
    out.width  = tile.shape[2];
    const std::int64_t ch = tile.shape[0];
    const std::size_t plane = static_cast<std::size_t>(out.height) * out.width;

    // latent_init returns the WEIGHTED form (n+1 channels, last is the weight
    // sum) because it is a raw DAG node rather than one of the _normalized
    // accessors. Divide here so every stage hands JS the same thing: a plain
    // planar buffer whose channel count matches the spec.
    if (name == "latentInit" && ch == spec.channels + 1) {
        const float* wgt = tile.data.data() + static_cast<std::size_t>(spec.channels) * plane;
        out.data.resize(static_cast<std::size_t>(spec.channels) * plane);
        for (int c = 0; c < spec.channels; ++c)
            for (std::size_t p = 0; p < plane; ++p) {
                const float d = wgt[p];
                out.data[static_cast<std::size_t>(c) * plane + p] =
                    (d != 0.0f) ? tile.data[static_cast<std::size_t>(c) * plane + p] / d : 0.0f;
            }
        out.channels = spec.channels;
    } else {
        out.data     = std::move(tile.data);
        out.channels = ch;
    }

    // Undo the signed square root wherever the channel is elevation in
    // disguise, so "m" in chanUnits means metres for real. Everything else is
    // handed over exactly as the pipeline produced it.
    auto square = [&](std::int64_t c) {
        if (c >= out.channels) return;
        float* p = out.data.data() + static_cast<std::size_t>(c) * plane;
        for (std::size_t i = 0; i < plane; ++i) p[i] = std::copysign(p[i] * p[i], p[i]);
    };
    if (name == "coarse") { square(0); square(1); }
    if (name == "latent" || name == "latentInit") {
        // Standardised at storage; these two constants are hardcoded in both
        // the port and upstream rather than living in config.json.
        if (out.channels > 4) {
            float* p = out.data.data() + 4 * plane;
            for (std::size_t i = 0; i < plane; ++i) p[i] = p[i] * 38.6f - 31.4f;
        }
        square(4);
    }

    // Coarse cells are native * compression * 32, latent cells
    // native * compression, and residual/elevation are native.
    const auto& cfg = pipe.config();
    out.cellSize = cfg.native_resolution;
    if (name == "coarse")
        out.cellSize = cfg.native_resolution * cfg.latent_compression * 32.0;
    else if (name == "latent" || name == "latentInit")
        out.cellSize = cfg.native_resolution * cfg.latent_compression;
    return out;
}

JSValue makeStageResult(JSContext* ctx, const StageOut& s) {
    JSValue lenVal = JS_NewInt64(ctx, static_cast<std::int64_t>(s.data.size()));
    JSValue arr = JS_NewTypedArray(ctx, 1, &lenVal, JS_TYPED_ARRAY_FLOAT32);
    JS_FreeValue(ctx, lenVal);
    if (JS_IsException(arr)) return arr;
    size_t byteOff = 0, viewLen = 0;
    JSValue abuf = JS_GetTypedArrayBuffer(ctx, arr, &byteOff, &viewLen, nullptr);
    if (JS_IsException(abuf)) { JS_FreeValue(ctx, arr); return abuf; }
    size_t abufLen = 0;
    uint8_t* ptr = JS_GetArrayBuffer(ctx, &abufLen, abuf);
    if (ptr) std::memcpy(ptr + byteOff, s.data.data(), s.data.size() * sizeof(float));
    JS_FreeValue(ctx, abuf);

    JSValue names = JS_NewArray(ctx);
    JSValue units = JS_NewArray(ctx);
    for (std::int64_t c = 0; c < s.channels && c < s.spec->channels; ++c) {
        JS_SetPropertyUint32(ctx, names, static_cast<uint32_t>(c),
                             JS_NewString(ctx, s.spec->chanNames[c]));
        JS_SetPropertyUint32(ctx, units, static_cast<uint32_t>(c),
                             JS_NewString(ctx, s.spec->chanUnits[c]));
    }

    JSValue res = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, res, "stage",    JS_NewString(ctx, s.spec->name));
    JS_SetPropertyStr(ctx, res, "width",    JS_NewInt64(ctx, s.width));
    JS_SetPropertyStr(ctx, res, "height",   JS_NewInt64(ctx, s.height));
    JS_SetPropertyStr(ctx, res, "channels", JS_NewInt64(ctx, s.channels));
    JS_SetPropertyStr(ctx, res, "cellSize", JS_NewFloat64(ctx, s.cellSize));
    JS_SetPropertyStr(ctx, res, "names",    names);
    JS_SetPropertyStr(ctx, res, "units",    units);
    JS_SetPropertyStr(ctx, res, "data",     arr);
    return res;
}

// Shared front half of both entry points: name -> spec, then the bounds.
const StageSpec* readStageArgs(JSContext* ctx, int argc, JSValueConst* argv,
                               std::int64_t& i1, std::int64_t& j1,
                               std::int64_t& i2, std::int64_t& j2) {
    std::string name;
    if (argc < 1 || !argStr(ctx, argv[0], name)) {
        JS_ThrowTypeError(ctx, "stage(name, i1, j1, i2, j2): name must be a string");
        return nullptr;
    }
    for (const auto& s : kStages)
        if (name == s.name) {
            std::int64_t margin = 0;
            if (!readBounds(ctx, argc - 1, argv + 1, i1, j1, i2, j2, margin))
                return nullptr;
            return &s;
        }
    JS_ThrowRangeError(ctx,
        "stage: unknown stage '%s' (coarse, latent, latentInit, residual, "
        "elevation)", name.c_str());
    return nullptr;
}

struct StageJob {
    WorldWrapper*    w = nullptr;
    const StageSpec* spec = nullptr;
    std::int64_t     i1 = 0, j1 = 0, i2 = 0, j2 = 0;
    StageOut         out;

    JSValue onDone  = JS_UNDEFINED;
    JSValue onError = JS_UNDEFINED;
    bool    hasDone = false, hasError = false;
    JSValue worldRef = JS_UNDEFINED;
};

// world.stage(name, i1, j1, i2, j2, { onDone, onError }) — ASYNC, like
// elevation(), so a frame loop stays live while a stage generates. Without
// this a viz that draws stages would freeze for the seconds each one takes.
JSValue js_world_stage(JSContext* ctx, JSValueConst this_val,
                       int argc, JSValueConst* argv) {
    auto* w = qjsbind::unwrap<WorldWrapper>(ctx, this_val);
    if (!w || !w->pipe) return JS_ThrowTypeError(ctx, "stage: world is destroyed");

    std::int64_t i1, j1, i2, j2;
    const StageSpec* spec = readStageArgs(ctx, argc, argv, i1, j1, i2, j2);
    if (!spec) return JS_EXCEPTION;

    bool expected = false;
    if (!w->busy.compare_exchange_strong(expected, true)) {
        return JS_ThrowInternalError(ctx,
            "stage: this world is already generating. One request at a time "
            "— the tile cache is not thread-safe. Queue in JS, or load a second "
            "world for the same seed.");
    }

    auto job = std::make_shared<StageJob>();
    job->w = w; job->spec = spec;
    job->i1 = i1; job->j1 = j1; job->i2 = i2; job->j2 = j2;
    job->worldRef = JS_DupValue(ctx, this_val);

    JSValueConst opts = argc > 5 ? argv[5] : JS_UNDEFINED;
    if (JS_IsObject(opts)) {
        JSValue od = JS_GetPropertyStr(ctx, opts, "onDone");
        JSValue oe = JS_GetPropertyStr(ctx, opts, "onError");
        job->hasDone  = JS_IsFunction(ctx, od);
        job->hasError = JS_IsFunction(ctx, oe);
        job->onDone   = job->hasDone  ? JS_DupValue(ctx, od) : JS_UNDEFINED;
        job->onError  = job->hasError ? JS_DupValue(ctx, oe) : JS_UNDEFINED;
        JS_FreeValue(ctx, od);
        JS_FreeValue(ctx, oe);
    }

    auto work = [job](const std::atomic<bool>&) {
        job->out = computeStage(*job->w->pipe, *job->spec,
                                job->i1, job->j1, job->i2, job->j2);
    };

    auto done = [job](JSContext* c, bool cancelled, const std::string& error) {
        job->w->busy.store(false);
        if (!cancelled && error.empty()) {
            if (job->hasDone) {
                JSValue res = makeStageResult(c, job->out);
                if (JS_IsException(res)) {
                    JS_FreeValue(c, JS_GetException(c));
                } else {
                    JSValue r = JS_Call(c, job->onDone, JS_UNDEFINED, 1, &res);
                    if (JS_IsException(r)) JS_FreeValue(c, JS_GetException(c));
                    JS_FreeValue(c, r);
                }
                JS_FreeValue(c, res);
            }
        } else if (!cancelled && job->hasError) {
            JSValue e = JS_NewString(c, error.empty() ? "stage failed" : error.c_str());
            JSValue r = JS_Call(c, job->onError, JS_UNDEFINED, 1, &e);
            if (JS_IsException(r)) JS_FreeValue(c, JS_GetException(c));
            JS_FreeValue(c, r);
            JS_FreeValue(c, e);
        }
        if (job->hasDone)  JS_FreeValue(c, job->onDone);
        if (job->hasError) JS_FreeValue(c, job->onError);
        JS_FreeValue(c, job->worldRef);
    };

    return launchAsyncJob(ctx, std::move(work), nullptr, std::move(done));
}

// The same, blocking. For headless tests and Workers, mirroring elevationSync.
JSValue js_world_stage_sync(JSContext* ctx, JSValueConst this_val,
                            int argc, JSValueConst* argv) {
    auto* w = qjsbind::unwrap<WorldWrapper>(ctx, this_val);
    if (!w || !w->pipe) return JS_ThrowTypeError(ctx, "stageSync: world is destroyed");

    std::int64_t i1, j1, i2, j2;
    const StageSpec* spec = readStageArgs(ctx, argc, argv, i1, j1, i2, j2);
    if (!spec) return JS_EXCEPTION;

    bool expected = false;
    if (!w->busy.compare_exchange_strong(expected, true))
        return JS_ThrowInternalError(ctx, "stageSync: this world is already generating");

    StageOut out;
    try {
        out = computeStage(*w->pipe, *spec, i1, j1, i2, j2);
    } catch (const std::exception& e) {
        w->busy.store(false);
        return JS_ThrowInternalError(ctx, "stageSync(%s): %s", spec->name, e.what());
    }
    w->busy.store(false);
    return makeStageResult(ctx, out);
}

// ─── world.elevation(i1, j1, i2, j2, opts) ───────────────────────────────────

JSValue js_world_elevation(JSContext* ctx, JSValueConst this_val,
                           int argc, JSValueConst* argv) {
    auto* w = qjsbind::unwrap<WorldWrapper>(ctx, this_val);
    if (!w || !w->pipe) return JS_ThrowTypeError(ctx, "elevation: world is destroyed");

    std::int64_t i1, j1, i2, j2, margin;
    if (!readBounds(ctx, argc, argv, i1, j1, i2, j2, margin)) return JS_EXCEPTION;

    bool expected = false;
    if (!w->busy.compare_exchange_strong(expected, true)) {
        return JS_ThrowInternalError(ctx,
            "elevation: this world is already generating. One request at a time "
            "— the tile cache is not thread-safe. Queue in JS, or load a second "
            "world for the same seed.");
    }

    auto job = std::make_shared<ElevJob>();
    job->w = w;
    job->i1 = i1; job->j1 = j1; job->i2 = i2; job->j2 = j2;
    job->margin = margin;
    job->worldRef = JS_DupValue(ctx, this_val);

    JSValueConst opts = argc > 4 ? argv[4] : JS_UNDEFINED;
    if (JS_IsObject(opts)) {
        JSValue od = JS_GetPropertyStr(ctx, opts, "onDone");
        JSValue oe = JS_GetPropertyStr(ctx, opts, "onError");
        job->hasDone  = JS_IsFunction(ctx, od);
        job->hasError = JS_IsFunction(ctx, oe);
        job->onDone   = job->hasDone  ? JS_DupValue(ctx, od) : JS_UNDEFINED;
        job->onError  = job->hasError ? JS_DupValue(ctx, oe) : JS_UNDEFINED;
        JS_FreeValue(ctx, od);
        JS_FreeValue(ctx, oe);
    }

    auto work = [job](const std::atomic<bool>&) {
        // Monolithic: there is no step boundary to observe a cancel at, so a
        // cancelled request runs to completion and its result is dropped.
        const std::int64_t m = job->margin;
        job->out = cropMargin(
            job->w->pipe->elevation(job->i1 - m, job->j1 - m,
                                    job->i2 + m, job->j2 + m), m);
    };

    auto done = [job](JSContext* c, bool cancelled, const std::string& error) {
        job->w->busy.store(false);

        if (!cancelled && error.empty()) {
            if (job->hasDone) {
                JSValue res = makeElevResult(
                    c, job->out, job->w->pipe->config().native_resolution);
                if (JS_IsException(res)) {
                    JS_FreeValue(c, JS_GetException(c));
                } else {
                    JSValue r = JS_Call(c, job->onDone, JS_UNDEFINED, 1, &res);
                    if (JS_IsException(r)) JS_FreeValue(c, JS_GetException(c));
                    JS_FreeValue(c, r);
                }
                JS_FreeValue(c, res);
            }
        } else if (!cancelled && job->hasError) {
            JSValue e = JS_NewString(c, error.empty() ? "elevation failed"
                                                      : error.c_str());
            JSValue r = JS_Call(c, job->onError, JS_UNDEFINED, 1, &e);
            if (JS_IsException(r)) JS_FreeValue(c, JS_GetException(c));
            JS_FreeValue(c, r);
            JS_FreeValue(c, e);
        }

        if (job->hasDone)  JS_FreeValue(c, job->onDone);
        if (job->hasError) JS_FreeValue(c, job->onError);
        JS_FreeValue(c, job->worldRef);
    };

    return launchAsyncJob(ctx, std::move(work), nullptr, std::move(done));
}

// ─── world.elevationSync(i1, j1, i2, j2) ─────────────────────────────────────

JSValue js_world_elevation_sync(JSContext* ctx, JSValueConst this_val,
                                int argc, JSValueConst* argv) {
    auto* w = qjsbind::unwrap<WorldWrapper>(ctx, this_val);
    if (!w || !w->pipe) return JS_ThrowTypeError(ctx, "elevationSync: world is destroyed");

    std::int64_t i1, j1, i2, j2, margin;
    if (!readBounds(ctx, argc, argv, i1, j1, i2, j2, margin)) return JS_EXCEPTION;

    bool expected = false;
    if (!w->busy.compare_exchange_strong(expected, true)) {
        return JS_ThrowInternalError(ctx,
            "elevationSync: this world is already generating");
    }
    td::TileBuffer tile;
    try {
        tile = cropMargin(w->pipe->elevation(i1 - margin, j1 - margin,
                                             i2 + margin, j2 + margin), margin);
    } catch (const std::exception& e) {
        w->busy.store(false);
        return JS_ThrowInternalError(ctx, "elevationSync: %s", e.what());
    }
    w->busy.store(false);
    return makeElevResult(ctx, tile, w->pipe->config().native_resolution);
}

// ─── class ───────────────────────────────────────────────────────────────────

void registerWorldClass(JSContext* ctx) {
    qjsbind::Class<WorldWrapper>(ctx, "World", qjsbind::NoGlobal)
        .get("seed", [](WorldWrapper* w) -> double {
            // Seeds are uint64 and JS numbers are doubles; above 2^53 this is
            // lossy, which is why the seed is echoed rather than round-tripped.
            return w->pipe ? static_cast<double>(w->pipe->seed()) : 0.0;
        })
        .get("cellSize", [](WorldWrapper* w) -> double {
            return w->pipe ? w->pipe->config().native_resolution : 0.0;
        })
        .get("generating", [](WorldWrapper* w) -> bool {
            return w->busy.load();
        })
        .get("latentCellSize", [](WorldWrapper* w) -> double {
            return w->pipe ? w->pipe->config().native_resolution *
                             w->pipe->config().latent_compression : 0.0;
        })
        .get("coarseCellSize", [](WorldWrapper* w) -> double {
            return w->pipe ? w->pipe->config().native_resolution *
                             w->pipe->config().latent_compression * 32.0 : 0.0;
        })
        .method_raw("elevation", js_world_elevation, 5)
        .method_raw("elevationSync", js_world_elevation_sync, 4)
        .method_raw("coarse", js_world_coarse, 4)
        .method_raw("stage", js_world_stage, 6)
        .method_raw("stageSync", js_world_stage_sync, 5)
        .method("clearCache", [](WorldWrapper* w) {
            if (w->pipe) w->pipe->clear_cache();
        });
}

// ─── loader ──────────────────────────────────────────────────────────────────

JSValue js_init(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    try {
        brotensor::init();
    } catch (const std::exception& e) {
        return JS_ThrowInternalError(ctx, "bro.worldgen.init: %s", e.what());
    }
    return JS_UNDEFINED;
}

struct LoadJob {
    std::string   dir;
    std::uint64_t seed = 0;
    std::unique_ptr<WorldWrapper> w;

    JSValue onReady = JS_UNDEFINED;
    JSValue onError = JS_UNDEFINED;
    bool    hasReady = false, hasError = false;
};

// bro.worldgen.loadWorld(dir, { seed, onReady, onError })
JSValue js_loadWorld(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    std::string dir;
    if (argc < 1 || !argStr(ctx, argv[0], dir)) {
        return JS_ThrowTypeError(ctx,
            "loadWorld(dir, opts): dir must be a converted checkpoint directory");
    }

    // Resolve relative to the app directory, as the other model loaders do,
    // so an app can say "weights/terrain" without knowing the launch CWD.
    dir = brokit::api::resolveAssetPath(ctx, dir);

    auto ls = std::make_shared<LoadJob>();
    ls->dir = dir;

    JSValueConst opts = argc > 1 ? argv[1] : JS_UNDEFINED;
    if (JS_IsObject(opts)) {
        double sd = 0;
        JSValue s = JS_GetPropertyStr(ctx, opts, "seed");
        if (!JS_IsUndefined(s)) JS_ToFloat64(ctx, &sd, s);
        JS_FreeValue(ctx, s);
        ls->seed = static_cast<std::uint64_t>(sd);

        JSValue orv = JS_GetPropertyStr(ctx, opts, "onReady");
        JSValue oev = JS_GetPropertyStr(ctx, opts, "onError");
        ls->hasReady = JS_IsFunction(ctx, orv);
        ls->hasError = JS_IsFunction(ctx, oev);
        ls->onReady  = ls->hasReady ? JS_DupValue(ctx, orv) : JS_UNDEFINED;
        ls->onError  = ls->hasError ? JS_DupValue(ctx, oev) : JS_UNDEFINED;
        JS_FreeValue(ctx, orv);
        JS_FreeValue(ctx, oev);
    }

    auto work = [ls](const std::atomic<bool>&) {
        auto w = std::make_unique<WorldWrapper>();
        w->pipe = std::make_unique<td::WorldPipeline>(ls->dir, ls->seed);
        ls->w = std::move(w);
    };

    auto done = [ls](JSContext* c, bool /*cancelled*/, const std::string& error) {
        if (!error.empty() || !ls->w) {
            if (ls->hasError) {
                JSValue e = JS_NewString(c, error.empty() ? "loadWorld failed"
                                                          : error.c_str());
                JSValue r = JS_Call(c, ls->onError, JS_UNDEFINED, 1, &e);
                if (JS_IsException(r)) JS_FreeValue(c, JS_GetException(c));
                JS_FreeValue(c, r);
                JS_FreeValue(c, e);
            }
        } else if (ls->hasReady) {
            JSValue out = qjsbind::wrap<WorldWrapper>(c, ls->w.release());
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

}  // namespace

// ─── install ─────────────────────────────────────────────────────────────────

void installWorldgenBindings(JSContext* ctx) {
    registerWorldClass(ctx);

    JSValue global = JS_GetGlobalObject(ctx);
    JSValue broObj = JS_GetPropertyStr(ctx, global, "bro");
    if (JS_IsUndefined(broObj) || JS_IsException(broObj)) {
        JS_FreeValue(ctx, broObj);
        broObj = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, global, "bro", JS_DupValue(ctx, broObj));
    }

    JSValue wg = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, wg, "available", JS_TRUE);
    JS_SetPropertyStr(ctx, wg, "init",
        JS_NewCFunction(ctx, js_init, "init", 0));
    JS_SetPropertyStr(ctx, wg, "loadWorld",
        JS_NewCFunction(ctx, js_loadWorld, "loadWorld", 2));
    JS_SetPropertyStr(ctx, broObj, "worldgen", wg);

    JS_FreeValue(ctx, broObj);
    JS_FreeValue(ctx, global);
}

void cleanupWorldgenBindings(JSContext*) {}

}  // namespace bro::js

#endif  // BRO_WITH_DIFFUSION
