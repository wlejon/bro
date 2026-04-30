// JS bindings for brogameagent::grid::* — the grid-world / platformer
// training kit. Installs onto bro.ai.game.grid via installGridBindings().
//
// Surface mirrors docs/ai-game-api.js bro.ai.game.grid.* section.

#include "js/ai_bindings.h"

#include <qjsbind/qjsbind.h>

#include <brogameagent/grid/best_crop.h>
#include <brogameagent/grid/bc_ingest.h>
#include <brogameagent/grid/failure_tape.h>
#include <brogameagent/grid/frame_stack.h>
#include <brogameagent/grid/generic_recorder.h>
#include <brogameagent/grid/harness.h>
#include <brogameagent/grid/obs_window.h>
#include <brogameagent/grid/shaping.h>
#include <brogameagent/learn/generic_replay_buffer.h>
#include <brogameagent/generic_mcts.h>

#include <any>
#include <cstring>
#include <memory>
#include <random>
#include <string>
#include <vector>

namespace bro::js {

namespace grid    = brogameagent::grid;
namespace learn   = brogameagent::learn;
namespace mcts_ns = brogameagent::mcts;

namespace {

// ─── Small JSValue holder for std::any storage ─────────────────────────────
struct JsValueHolder {
    JSContext* ctx{nullptr};
    JSValue    v{JS_UNDEFINED};
    JsValueHolder() = default;
    JsValueHolder(JSContext* c, JSValueConst val) : ctx(c), v(JS_DupValue(c, val)) {}
    JsValueHolder(const JsValueHolder& o) : ctx(o.ctx), v(JS_DupValue(o.ctx, o.v)) {}
    JsValueHolder& operator=(const JsValueHolder& o) {
        if (this != &o) { if (ctx) JS_FreeValue(ctx, v);
                          ctx = o.ctx; v = JS_DupValue(o.ctx, o.v); }
        return *this;
    }
    JsValueHolder(JsValueHolder&& o) noexcept : ctx(o.ctx), v(o.v) {
        o.ctx = nullptr; o.v = JS_UNDEFINED;
    }
    JsValueHolder& operator=(JsValueHolder&& o) noexcept {
        if (this != &o) { if (ctx) JS_FreeValue(ctx, v);
                          ctx = o.ctx; v = o.v; o.ctx = nullptr; o.v = JS_UNDEFINED; }
        return *this;
    }
    ~JsValueHolder() { if (ctx) JS_FreeValue(ctx, v); }
};

// ─── Typed array marshalling ───────────────────────────────────────────────
//
// Hot paths: ObsWindow tile/layer samplers can return plain JS arrays like
// [1, 0], not Float32Arrays. We must avoid calling JS_GetTypedArrayBuffer on
// non-typed-array values: that path constructs and throws a TypeError every
// call, and even when caught and freed it churns the heap with Error objects
// and stack frames. Check JS_IsArray first; fall through to the typed-array
// path only when it isn't a plain array. Any leftover exception (the value
// is neither a plain array nor a typed array) is captured-and-freed properly
// — `JS_GetException(ctx)` returns a strong ref the caller owns.
std::vector<float> readFloat32Array(JSContext* ctx, JSValueConst val) {
    std::vector<float> out;
    if (JS_IsUndefined(val) || JS_IsNull(val)) return out;
    if (JS_IsArray(val)) {
        JSValue lv = JS_GetPropertyStr(ctx, val, "length");
        uint32_t n = 0; JS_ToUint32(ctx, &n, lv); JS_FreeValue(ctx, lv);
        out.reserve(n);
        for (uint32_t i = 0; i < n; ++i) {
            JSValue e = JS_GetPropertyUint32(ctx, val, i);
            double d = 0; JS_ToFloat64(ctx, &d, e);
            JS_FreeValue(ctx, e);
            out.push_back(static_cast<float>(d));
        }
        return out;
    }
    size_t off = 0, len = 0;
    JSValue ab = JS_GetTypedArrayBuffer(ctx, val, &off, &len, nullptr);
    if (JS_IsException(ab)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        return out;
    }
    size_t ab_len = 0;
    uint8_t* raw = JS_GetArrayBuffer(ctx, &ab_len, ab);
    JS_FreeValue(ctx, ab);
    if (!raw) return out;
    size_t n = len / sizeof(float);
    out.resize(n);
    if (n) std::memcpy(out.data(), raw + off, n * sizeof(float));
    return out;
}

std::vector<int> readIntArray(JSContext* ctx, JSValueConst val) {
    std::vector<int> out;
    if (JS_IsUndefined(val) || JS_IsNull(val)) return out;
    if (JS_IsArray(val)) {
        JSValue lv = JS_GetPropertyStr(ctx, val, "length");
        uint32_t n = 0; JS_ToUint32(ctx, &n, lv); JS_FreeValue(ctx, lv);
        out.reserve(n);
        for (uint32_t i = 0; i < n; ++i) {
            JSValue e = JS_GetPropertyUint32(ctx, val, i);
            int32_t x = 0; JS_ToInt32(ctx, &x, e); JS_FreeValue(ctx, e);
            out.push_back(x);
        }
        return out;
    }
    size_t off = 0, len = 0;
    JSValue ab = JS_GetTypedArrayBuffer(ctx, val, &off, &len, nullptr);
    if (JS_IsException(ab)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        return out;
    }
    size_t ab_len = 0;
    uint8_t* raw = JS_GetArrayBuffer(ctx, &ab_len, ab);
    JS_FreeValue(ctx, ab);
    if (!raw) return out;
    size_t n = len / sizeof(int32_t);
    out.resize(n);
    if (n) std::memcpy(out.data(), raw + off, n * sizeof(int32_t));
    return out;
}

using qjsbind::make_float32_array;
static inline JSValue make_int32_array_from_ints(JSContext* ctx, const std::vector<int>& v) {
    return qjsbind::make_int32_array(ctx, reinterpret_cast<const int32_t*>(v.data()), v.size());
}

double getDouble(JSContext* ctx, JSValueConst obj, const char* k, double d) {
    JSValue v = JS_GetPropertyStr(ctx, obj, k);
    double out = d;
    if (JS_IsNumber(v)) JS_ToFloat64(ctx, &out, v);
    JS_FreeValue(ctx, v);
    return out;
}
int32_t getInt(JSContext* ctx, JSValueConst obj, const char* k, int32_t d) {
    JSValue v = JS_GetPropertyStr(ctx, obj, k);
    int32_t out = d;
    if (JS_IsNumber(v)) JS_ToInt32(ctx, &out, v);
    JS_FreeValue(ctx, v);
    return out;
}
std::string getString(JSContext* ctx, JSValueConst obj, const char* k, const char* d) {
    JSValue v = JS_GetPropertyStr(ctx, obj, k);
    std::string out = d ? d : "";
    if (JS_IsString(v)) {
        const char* s = JS_ToCString(ctx, v);
        if (s) { out = s; JS_FreeCString(ctx, s); }
    }
    JS_FreeValue(ctx, v);
    return out;
}
uint64_t getU64(JSContext* ctx, JSValueConst obj, const char* k, uint64_t d) {
    JSValue v = JS_GetPropertyStr(ctx, obj, k);
    uint64_t out = d;
    if (!JS_IsUndefined(v) && !JS_IsNull(v)) {
        int64_t s = 0;
        if (JS_ToBigInt64(ctx, &s, v) == 0)      out = static_cast<uint64_t>(s);
        else { double dv = 0; JS_ToFloat64(ctx, &dv, v); out = static_cast<uint64_t>(dv); }
    }
    JS_FreeValue(ctx, v);
    return out;
}

// ─── ObsWindow wrapper ─────────────────────────────────────────────────────

struct ObsWindowData {
    JSContext* ctx{nullptr};
    std::unique_ptr<grid::ObsWindow> win;
    // Hold JS function refs alive.
    JSValue tile_fn = JS_UNDEFINED;
    std::vector<JSValue> enumerate_fns;
    std::vector<JSValue> sample_fns;
    ~ObsWindowData() {
        if (!ctx) return;
        JS_FreeValue(ctx, tile_fn);
        for (auto& v : enumerate_fns) JS_FreeValue(ctx, v);
        for (auto& v : sample_fns)    JS_FreeValue(ctx, v);
    }
};

static JSValue js_createObsWindow(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 1 || !JS_IsObject(argv[0])) {
        return JS_ThrowTypeError(ctx, "createObsWindow(opts): expected an object");
    }
    JSValueConst opts = argv[0];

    // Spec
    grid::ObsWindowSpec spec{};
    JSValue specV = JS_GetPropertyStr(ctx, opts, "spec");
    JSValueConst sp = JS_IsObject(specV) ? specV : opts;
    spec.cols_behind     = getInt(ctx, sp, "colsBehind", 0);
    spec.cols_ahead      = getInt(ctx, sp, "colsAhead",  0);
    spec.rows_up         = getInt(ctx, sp, "rowsUp",     0);
    spec.rows_down       = getInt(ctx, sp, "rowsDown",   0);
    spec.tile_channels   = getInt(ctx, sp, "tileChannels", 1);
    spec.self_block_size = getInt(ctx, sp, "selfBlockSize", 0);
    JS_FreeValue(ctx, specV);

    auto* d = new ObsWindowData();
    d->ctx = ctx;

    // Tile sampler
    JSValue tileV = JS_GetPropertyStr(ctx, opts, "tile");
    if (JS_IsObject(tileV)) {
        spec.tile_channels = getInt(ctx, tileV, "channels", spec.tile_channels);
        // tile.normalize / tile.oob
        JSValue normV = JS_GetPropertyStr(ctx, tileV, "normalize");
        spec.tile_normalize = readFloat32Array(ctx, normV);
        JS_FreeValue(ctx, normV);
        JSValue oobV = JS_GetPropertyStr(ctx, tileV, "oob");
        spec.oob_tile = readFloat32Array(ctx, oobV);
        JS_FreeValue(ctx, oobV);

        JSValue sf = JS_GetPropertyStr(ctx, tileV, "sample");
        if (JS_IsFunction(ctx, sf)) d->tile_fn = JS_DupValue(ctx, sf);
        JS_FreeValue(ctx, sf);
    }
    JS_FreeValue(ctx, tileV);

    grid::TileSampleFn tile_fn;
    if (JS_IsFunction(ctx, d->tile_fn)) {
        ObsWindowData* dd = d;
        int TC = spec.tile_channels;
        tile_fn = [dd, ctx, TC](int col, int row, float* out) -> bool {
            JSValue cv = JS_NewInt32(ctx, col), rv = JS_NewInt32(ctx, row);
            JSValueConst args[2] = { cv, rv };
            JSValue r = JS_Call(ctx, dd->tile_fn, JS_UNDEFINED, 2, args);
            JS_FreeValue(ctx, cv); JS_FreeValue(ctx, rv);
            if (JS_IsException(r)) { JS_FreeValue(ctx, JS_GetException(ctx)); return false; }
            if (JS_IsBool(r)) {
                bool b = JS_ToBool(ctx, r) > 0;
                JS_FreeValue(ctx, r);
                for (int i = 0; i < TC; ++i) out[i] = b ? 1.0f : 0.0f;
                return true;
            }
            if (JS_IsNumber(r)) {
                double x = 0; JS_ToFloat64(ctx, &x, r); JS_FreeValue(ctx, r);
                for (int i = 0; i < TC; ++i) out[i] = static_cast<float>(x);
                return true;
            }
            // Array / typed array
            auto vec = readFloat32Array(ctx, r);
            JS_FreeValue(ctx, r);
            int n = std::min<int>(TC, static_cast<int>(vec.size()));
            for (int i = 0; i < n; ++i) out[i] = vec[static_cast<size_t>(i)];
            for (int i = n; i < TC; ++i) out[i] = 0.0f;
            return !vec.empty();
        };
    } else {
        tile_fn = [](int, int, float*) { return false; };
    }

    // Layers
    std::vector<grid::EntityLayerSpec> layers;
    JSValue layersV = JS_GetPropertyStr(ctx, opts, "layers");
    if (JS_IsArray(layersV)) {
        JSValue lv = JS_GetPropertyStr(ctx, layersV, "length");
        uint32_t n = 0; JS_ToUint32(ctx, &n, lv); JS_FreeValue(ctx, lv);
        for (uint32_t i = 0; i < n; ++i) {
            JSValue lo = JS_GetPropertyUint32(ctx, layersV, i);
            grid::EntityLayerSpec L;
            L.channels  = getInt(ctx, lo, "channels", 1);
            L.overwrite = false;
            JSValue ow = JS_GetPropertyStr(ctx, lo, "overwrite");
            if (!JS_IsUndefined(ow)) L.overwrite = JS_ToBool(ctx, ow) > 0;
            JS_FreeValue(ctx, ow);
            JSValue normV = JS_GetPropertyStr(ctx, lo, "normalize");
            L.normalize = readFloat32Array(ctx, normV);
            JS_FreeValue(ctx, normV);

            JSValue ev = JS_GetPropertyStr(ctx, lo, "enumerate");
            JSValue sv = JS_GetPropertyStr(ctx, lo, "sample");
            JSValue eDup = JS_IsFunction(ctx, ev) ? JS_DupValue(ctx, ev) : JS_UNDEFINED;
            JSValue sDup = JS_IsFunction(ctx, sv) ? JS_DupValue(ctx, sv) : JS_UNDEFINED;
            JS_FreeValue(ctx, ev);
            JS_FreeValue(ctx, sv);
            d->enumerate_fns.push_back(eDup);
            d->sample_fns.push_back(sDup);
            int idx = static_cast<int>(d->enumerate_fns.size()) - 1;
            int chan = L.channels;
            ObsWindowData* dd = d;
            L.enumerate_fn = [dd, ctx, idx]() -> size_t {
                JSValue fn = dd->enumerate_fns[static_cast<size_t>(idx)];
                if (!JS_IsFunction(ctx, fn)) return 0;
                JSValue r = JS_Call(ctx, fn, JS_UNDEFINED, 0, nullptr);
                if (JS_IsException(r)) { JS_FreeValue(ctx, JS_GetException(ctx)); return 0; }
                int32_t n = 0; JS_ToInt32(ctx, &n, r); JS_FreeValue(ctx, r);
                return n > 0 ? static_cast<size_t>(n) : 0;
            };
            L.sample_fn = [dd, ctx, idx, chan](size_t i) -> grid::EntityCell {
                grid::EntityCell c;
                JSValue fn = dd->sample_fns[static_cast<size_t>(idx)];
                if (!JS_IsFunction(ctx, fn)) return c;
                JSValue iv = JS_NewInt32(ctx, static_cast<int32_t>(i));
                JSValueConst args[1] = { iv };
                JSValue r = JS_Call(ctx, fn, JS_UNDEFINED, 1, args);
                JS_FreeValue(ctx, iv);
                if (JS_IsException(r)) { JS_FreeValue(ctx, JS_GetException(ctx)); return c; }
                c.col = getInt(ctx, r, "col", 0);
                c.row = getInt(ctx, r, "row", 0);
                JSValue vals = JS_GetPropertyStr(ctx, r, "values");
                if (!JS_IsUndefined(vals) && !JS_IsNull(vals)) c.values = readFloat32Array(ctx, vals);
                else {
                    // Single 'value' field shortcut.
                    double v = getDouble(ctx, r, "value", 1.0);
                    c.values.assign(static_cast<size_t>(chan), 0.0f);
                    c.values[0] = static_cast<float>(v);
                }
                JS_FreeValue(ctx, vals);
                JS_FreeValue(ctx, r);
                return c;
            };

            layers.push_back(std::move(L));
            JS_FreeValue(ctx, lo);
        }
    }
    JS_FreeValue(ctx, layersV);

    d->win = std::make_unique<grid::ObsWindow>(spec, tile_fn, std::move(layers));
    return qjsbind::wrap<ObsWindowData>(ctx, d);
}

// ─── FrameStack wrapper ────────────────────────────────────────────────────
struct FrameStackData {
    std::unique_ptr<grid::FrameStack> fs;
};

static JSValue js_createFrameStack(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 1 || !JS_IsObject(argv[0])) {
        return JS_ThrowTypeError(ctx, "createFrameStack(opts): expected object");
    }
    int innerDim = getInt(ctx, argv[0], "innerDim", 0);
    int k        = getInt(ctx, argv[0], "k",        1);
    if (innerDim <= 0 || k <= 0) {
        return JS_ThrowTypeError(ctx, "createFrameStack: innerDim and k must be > 0");
    }
    auto* d = new FrameStackData();
    d->fs = std::make_unique<grid::FrameStack>(innerDim, k);
    return qjsbind::wrap<FrameStackData>(ctx, d);
}

// ─── FailureTape wrapper ───────────────────────────────────────────────────
struct FailureTapeData {
    std::unique_ptr<grid::FailureTape> tape;
};

static JSValue js_createFailureTape(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    grid::FailureTapeConfig cfg;
    if (argc >= 1 && JS_IsObject(argv[0])) {
        cfg.tape_depth    = getInt(ctx, argv[0], "tapeDepth",    cfg.tape_depth);
        cfg.ring_capacity = getInt(ctx, argv[0], "ringCapacity", cfg.ring_capacity);
        cfg.penalty       = static_cast<float>(getDouble(ctx, argv[0], "penalty", cfg.penalty));
        cfg.floor         = static_cast<float>(getDouble(ctx, argv[0], "floor",   cfg.floor));
    }
    auto* d = new FailureTapeData();
    d->tape = std::make_unique<grid::FailureTape>(cfg);
    return qjsbind::wrap<FailureTapeData>(ctx, d);
}

// ─── BestCrop wrapper ──────────────────────────────────────────────────────
struct BestCropData {
    std::unique_ptr<grid::BestCrop> pool;
    std::mt19937_64                 rng{0xC0DE1234ULL};
};

static JSValue js_createBestCrop(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    grid::BestCropConfig cfg;
    if (argc >= 1 && JS_IsObject(argv[0])) {
        cfg.capacity     = getInt(ctx, argv[0], "capacity",   cfg.capacity);
        cfg.depth_bonus  = static_cast<float>(getDouble(ctx, argv[0], "depthBonus", cfg.depth_bonus));
        cfg.age_decay    = static_cast<float>(getDouble(ctx, argv[0], "ageDecay",   cfg.age_decay));
        cfg.seed_top_k   = getInt(ctx, argv[0], "seedTopK",   cfg.seed_top_k);
    }
    auto* d = new BestCropData();
    d->pool = std::make_unique<grid::BestCrop>(cfg);
    uint64_t seed = (argc >= 1 && JS_IsObject(argv[0])) ? getU64(ctx, argv[0], "seed", 0xC0DE1234ULL) : 0xC0DE1234ULL;
    d->rng.seed(seed);
    return qjsbind::wrap<BestCropData>(ctx, d);
}

// ─── PotentialShaper / StallDetector wrappers ──────────────────────────────
struct PotentialShaperData {
    std::unique_ptr<grid::PotentialShaper> sh;
};
struct StallDetectorData {
    std::unique_ptr<grid::StallDetector> det;
};

// ─── Generic env from JS callbacks (BC ingestion) ──────────────────────────
struct JsEnvCallbacks {
    JSContext* ctx;
    JSValue env_obj;
    JSValue snapshot_fn;
    JSValue restore_fn;
    JSValue step_fn;
    JSValue legal_fn;
    JSValue observe_fn;
    int     num_actions;
    void release() {
        if (!ctx) return;
        JS_FreeValue(ctx, env_obj);
        JS_FreeValue(ctx, snapshot_fn);
        JS_FreeValue(ctx, restore_fn);
        JS_FreeValue(ctx, step_fn);
        JS_FreeValue(ctx, legal_fn);
        JS_FreeValue(ctx, observe_fn);
    }
};

static mcts_ns::GenericEnv buildEnvFromJs(JsEnvCallbacks* cb) {
    JSContext* ctx = cb->ctx;
    mcts_ns::GenericEnv env;
    env.num_actions = cb->num_actions;
    env.snapshot_fn = [cb, ctx]() -> std::any {
        JSValue r = JS_Call(ctx, cb->snapshot_fn, cb->env_obj, 0, nullptr);
        if (JS_IsException(r)) { JS_FreeValue(ctx, JS_GetException(ctx)); return std::any{}; }
        std::any out{ JsValueHolder(ctx, r) };
        JS_FreeValue(ctx, r);
        return out;
    };
    env.restore_fn = [cb, ctx](const std::any& s) {
        if (!s.has_value()) return;
        const auto* h = std::any_cast<JsValueHolder>(&s);
        if (!h) return;
        JSValueConst args[1] = { h->v };
        JSValue r = JS_Call(ctx, cb->restore_fn, cb->env_obj, 1, args);
        if (JS_IsException(r)) JS_FreeValue(ctx, JS_GetException(ctx));
        JS_FreeValue(ctx, r);
    };
    env.step_fn = [cb, ctx](int action) -> mcts_ns::GenericStepResult {
        JSValue av = JS_NewInt32(ctx, action);
        JSValueConst args[1] = { av };
        JSValue r = JS_Call(ctx, cb->step_fn, cb->env_obj, 1, args);
        JS_FreeValue(ctx, av);
        mcts_ns::GenericStepResult out{};
        if (JS_IsException(r)) { JS_FreeValue(ctx, JS_GetException(ctx)); return out; }
        if (JS_IsObject(r)) {
            out.reward = static_cast<float>(getDouble(ctx, r, "reward", 0.0));
            JSValue dv = JS_GetPropertyStr(ctx, r, "done");
            out.done = JS_ToBool(ctx, dv) > 0;
            JS_FreeValue(ctx, dv);
        }
        JS_FreeValue(ctx, r);
        return out;
    };
    env.legal_actions_fn = [cb, ctx]() -> std::vector<int> {
        JSValue r = JS_Call(ctx, cb->legal_fn, cb->env_obj, 0, nullptr);
        if (JS_IsException(r)) { JS_FreeValue(ctx, JS_GetException(ctx)); return {}; }
        auto out = readIntArray(ctx, r);
        JS_FreeValue(ctx, r);
        return out;
    };
    env.observe_fn = [cb, ctx]() -> std::vector<float> {
        JSValue r = JS_Call(ctx, cb->observe_fn, cb->env_obj, 0, nullptr);
        if (JS_IsException(r)) { JS_FreeValue(ctx, JS_GetException(ctx)); return {}; }
        auto out = readFloat32Array(ctx, r);
        JS_FreeValue(ctx, r);
        return out;
    };
    return env;
}

static JSValue situationToJs(JSContext* ctx, const learn::GenericSituation& s) {
    JSValue o = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, o, "obs",          make_float32_array(ctx, s.obs));
    JS_SetPropertyStr(ctx, o, "policyTarget", make_float32_array(ctx, s.policy_target));
    JS_SetPropertyStr(ctx, o, "actionMask",   make_float32_array(ctx, s.action_mask));
    JS_SetPropertyStr(ctx, o, "valueTarget",  JS_NewFloat64(ctx, s.value_target));
    return o;
}

static learn::GenericSituation situationFromJs(JSContext* ctx, JSValueConst v) {
    learn::GenericSituation s;
    if (!JS_IsObject(v)) return s;
    JSValue obs = JS_GetPropertyStr(ctx, v, "obs");
    s.obs = readFloat32Array(ctx, obs); JS_FreeValue(ctx, obs);
    JSValue pt = JS_GetPropertyStr(ctx, v, "policyTarget");
    s.policy_target = readFloat32Array(ctx, pt); JS_FreeValue(ctx, pt);
    JSValue am = JS_GetPropertyStr(ctx, v, "actionMask");
    s.action_mask = readFloat32Array(ctx, am); JS_FreeValue(ctx, am);
    s.value_target = static_cast<float>(getDouble(ctx, v, "valueTarget", 0.0));
    return s;
}

// ─── BC ingestion ──────────────────────────────────────────────────────────

static JSValue js_generateBC(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 1 || !JS_IsObject(argv[0]))
        return JS_ThrowTypeError(ctx, "generateBC(opts): expected object");
    JSValueConst opts = argv[0];

    JSValue envV = JS_GetPropertyStr(ctx, opts, "env");
    if (!JS_IsObject(envV)) { JS_FreeValue(ctx, envV);
        return JS_ThrowTypeError(ctx, "generateBC: env must be an object"); }

    JsEnvCallbacks cb{};
    cb.ctx = ctx;
    cb.env_obj     = JS_DupValue(ctx, envV);
    cb.num_actions = getInt(ctx, envV, "numActions", 0);
    auto getMethod = [&](const char* n) -> JSValue {
        JSValue f = JS_GetPropertyStr(ctx, envV, n);
        if (!JS_IsFunction(ctx, f)) { JS_FreeValue(ctx, f); return JS_UNDEFINED; }
        return f;
    };
    cb.snapshot_fn = getMethod("snapshot");
    cb.restore_fn  = getMethod("restore");
    cb.step_fn     = getMethod("step");
    cb.legal_fn    = getMethod("legalActions");
    cb.observe_fn  = getMethod("observe");
    JS_FreeValue(ctx, envV);

    if (!JS_IsFunction(ctx, cb.snapshot_fn) || !JS_IsFunction(ctx, cb.restore_fn) ||
        !JS_IsFunction(ctx, cb.step_fn)     || !JS_IsFunction(ctx, cb.legal_fn) ||
        !JS_IsFunction(ctx, cb.observe_fn)  || cb.num_actions <= 0) {
        cb.release();
        return JS_ThrowTypeError(ctx, "generateBC: env missing snapshot/restore/step/legalActions/observe/numActions");
    }
    auto env = buildEnvFromJs(&cb);

    JSValue heuristicV = JS_GetPropertyStr(ctx, opts, "heuristic");
    if (!JS_IsFunction(ctx, heuristicV)) {
        cb.release(); JS_FreeValue(ctx, heuristicV);
        return JS_ThrowTypeError(ctx, "generateBC: heuristic must be a function");
    }

    grid::HeuristicPolicyFn policy = [ctx, heuristicV](const std::vector<float>& obs,
                                                       const std::vector<int>&   legal) {
        JSValue ov = make_float32_array(ctx, obs);
        JSValue lv = make_int32_array_from_ints(ctx, legal);
        JSValueConst args[2] = { ov, lv };
        JSValue r = JS_Call(ctx, heuristicV, JS_UNDEFINED, 2, args);
        JS_FreeValue(ctx, ov); JS_FreeValue(ctx, lv);
        int32_t a = -1;
        if (!JS_IsException(r)) JS_ToInt32(ctx, &a, r);
        else JS_FreeValue(ctx, JS_GetException(ctx));
        JS_FreeValue(ctx, r);
        return a;
    };

    grid::BCConfig cfg;
    cfg.min_return      = static_cast<float>(getDouble(ctx, opts, "minReturn",      cfg.min_return));
    cfg.rollout_horizon = getInt(ctx, opts, "rolloutHorizon", cfg.rollout_horizon);
    cfg.gamma           = static_cast<float>(getDouble(ctx, opts, "gamma",          cfg.gamma));
    {
        JSValue cv = JS_GetPropertyStr(ctx, opts, "clipValue");
        if (!JS_IsUndefined(cv)) cfg.clip_value = JS_ToBool(ctx, cv) > 0;
        JS_FreeValue(ctx, cv);
    }

    // We must restore env to each start, so we capture starts as JsValueHolder.
    std::vector<std::any> starts;
    JSValue startsV = JS_GetPropertyStr(ctx, opts, "starts");
    if (JS_IsArray(startsV)) {
        JSValue lv = JS_GetPropertyStr(ctx, startsV, "length");
        uint32_t n = 0; JS_ToUint32(ctx, &n, lv); JS_FreeValue(ctx, lv);
        starts.reserve(n);
        for (uint32_t i = 0; i < n; ++i) {
            JSValue e = JS_GetPropertyUint32(ctx, startsV, i);
            starts.push_back(std::any{ JsValueHolder(ctx, e) });
            JS_FreeValue(ctx, e);
        }
    }
    JS_FreeValue(ctx, startsV);

    auto sits = grid::generate_bc_situations(env, policy, starts, cfg);

    JS_FreeValue(ctx, heuristicV);
    cb.release();

    JSValue arr = JS_NewArray(ctx);
    for (size_t i = 0; i < sits.size(); ++i) {
        JS_SetPropertyUint32(ctx, arr, static_cast<uint32_t>(i), situationToJs(ctx, sits[i]));
    }
    return arr;
}

// ─── GenericRecorder / Reader wrappers ─────────────────────────────────────
struct GenericRecorderData {
    std::unique_ptr<grid::GenericRecorder> rec;
    std::vector<grid::FieldDef> roster, frame, events;
};
struct GenericReplayReaderData {
    std::unique_ptr<grid::GenericReplayReader> rr;
};

static std::vector<grid::FieldDef> readSchema(JSContext* ctx, JSValueConst arr) {
    std::vector<grid::FieldDef> out;
    if (!JS_IsArray(arr)) return out;
    JSValue lv = JS_GetPropertyStr(ctx, arr, "length");
    uint32_t n = 0; JS_ToUint32(ctx, &n, lv); JS_FreeValue(ctx, lv);
    for (uint32_t i = 0; i < n; ++i) {
        JSValue e = JS_GetPropertyUint32(ctx, arr, i);
        grid::FieldDef fd;
        fd.name = getString(ctx, e, "name", "");
        std::string t = getString(ctx, e, "type", "f32");
        if (t == "i32") fd.type = grid::FieldType::I32;
        else if (t == "i64") fd.type = grid::FieldType::I64;
        else if (t == "f64") fd.type = grid::FieldType::F64;
        else fd.type = grid::FieldType::F32;
        out.push_back(std::move(fd));
        JS_FreeValue(ctx, e);
    }
    return out;
}

static grid::Row rowFromJs(JSContext* ctx, JSValueConst arr,
                           const std::vector<grid::FieldDef>& schema) {
    grid::Row row;
    if (!JS_IsArray(arr)) return row;
    JSValue lv = JS_GetPropertyStr(ctx, arr, "length");
    uint32_t n = 0; JS_ToUint32(ctx, &n, lv); JS_FreeValue(ctx, lv);
    for (uint32_t i = 0; i < n; ++i) {
        JSValue e = JS_GetPropertyUint32(ctx, arr, i);
        grid::FieldType t = i < schema.size() ? schema[i].type : grid::FieldType::F32;
        switch (t) {
            case grid::FieldType::I32: { int32_t x = 0; JS_ToInt32(ctx, &x, e); row.push_back(x); break; }
            case grid::FieldType::I64: { int64_t x = 0; JS_ToBigInt64(ctx, &x, e);
                if (x == 0) { double d = 0; JS_ToFloat64(ctx, &d, e); x = static_cast<int64_t>(d); }
                row.push_back(x); break; }
            case grid::FieldType::F32: { double d = 0; JS_ToFloat64(ctx, &d, e); row.push_back(static_cast<float>(d)); break; }
            case grid::FieldType::F64: { double d = 0; JS_ToFloat64(ctx, &d, e); row.push_back(d); break; }
        }
        JS_FreeValue(ctx, e);
    }
    return row;
}

static JSValue rowToJs(JSContext* ctx, const grid::Row& row) {
    JSValue arr = JS_NewArray(ctx);
    for (size_t i = 0; i < row.size(); ++i) {
        const auto& v = row[i];
        JSValue elem;
        if      (auto* p = std::get_if<int32_t>(&v)) elem = JS_NewInt32(ctx, *p);
        else if (auto* p = std::get_if<int64_t>(&v)) elem = JS_NewBigInt64(ctx, *p);
        else if (auto* p = std::get_if<float>(&v))   elem = JS_NewFloat64(ctx, *p);
        else                                          elem = JS_NewFloat64(ctx, std::get<double>(v));
        JS_SetPropertyUint32(ctx, arr, static_cast<uint32_t>(i), elem);
    }
    return arr;
}

static std::vector<grid::Row> rowsFromJs(JSContext* ctx, JSValueConst arr,
                                         const std::vector<grid::FieldDef>& schema) {
    std::vector<grid::Row> out;
    if (!JS_IsArray(arr)) return out;
    JSValue lv = JS_GetPropertyStr(ctx, arr, "length");
    uint32_t n = 0; JS_ToUint32(ctx, &n, lv); JS_FreeValue(ctx, lv);
    for (uint32_t i = 0; i < n; ++i) {
        JSValue e = JS_GetPropertyUint32(ctx, arr, i);
        out.push_back(rowFromJs(ctx, e, schema));
        JS_FreeValue(ctx, e);
    }
    return out;
}

static JSValue js_createGenericRecorder(JSContext* ctx, JSValueConst, int /*argc*/, JSValueConst* /*argv*/) {
    auto* d = new GenericRecorderData();
    d->rec = std::make_unique<grid::GenericRecorder>();
    return qjsbind::wrap<GenericRecorderData>(ctx, d);
}

static JSValue js_createGenericReplayReader(JSContext* ctx, JSValueConst, int /*argc*/, JSValueConst* /*argv*/) {
    auto* d = new GenericReplayReaderData();
    d->rr = std::make_unique<grid::GenericReplayReader>();
    return qjsbind::wrap<GenericReplayReaderData>(ctx, d);
}

// ─── GridTrainer wrapper ───────────────────────────────────────────────────
struct GridTrainerData {
    JSContext* ctx{nullptr};
    std::unique_ptr<grid::GridTrainer> tr;
};

static JSValue js_createGridTrainer(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 1 || !JS_IsObject(argv[0]))
        return JS_ThrowTypeError(ctx, "createGridTrainer(opts): expected object");
    JSValueConst opts = argv[0];

    grid::GridTrainerConfig cfg;
    JSValue netV = JS_GetPropertyStr(ctx, opts, "net");
    JSValueConst nv = JS_IsObject(netV) ? netV : opts;
    cfg.net.in_dim       = getInt(ctx, nv, "inDim", 0);
    cfg.net.value_hidden = getInt(ctx, nv, "valueHidden", cfg.net.value_hidden);
    cfg.net.num_actions  = getInt(ctx, nv, "numActions", 0);
    cfg.net.seed         = getU64(ctx, nv, "seed", cfg.net.seed);
    JSValue hv = JS_GetPropertyStr(ctx, nv, "hidden");
    if (JS_IsArray(hv)) {
        cfg.net.hidden.clear();
        JSValue lv = JS_GetPropertyStr(ctx, hv, "length");
        uint32_t n = 0; JS_ToUint32(ctx, &n, lv); JS_FreeValue(ctx, lv);
        for (uint32_t i = 0; i < n; ++i) {
            JSValue e = JS_GetPropertyUint32(ctx, hv, i);
            int32_t x = 0; JS_ToInt32(ctx, &x, e); JS_FreeValue(ctx, e);
            cfg.net.hidden.push_back(x);
        }
    }
    JS_FreeValue(ctx, hv);
    JS_FreeValue(ctx, netV);

    JSValue bufV = JS_GetPropertyStr(ctx, opts, "buffer");
    if (JS_IsObject(bufV)) cfg.buffer_capacity = getInt(ctx, bufV, "capacity", cfg.buffer_capacity);
    JS_FreeValue(ctx, bufV);

    JSValue trV = JS_GetPropertyStr(ctx, opts, "trainer");
    if (JS_IsObject(trV)) {
        cfg.trainer.lr            = static_cast<float>(getDouble(ctx, trV, "lr",       cfg.trainer.lr));
        cfg.trainer.momentum      = static_cast<float>(getDouble(ctx, trV, "momentum", cfg.trainer.momentum));
        cfg.trainer.batch         = getInt(ctx, trV, "batch", cfg.trainer.batch);
        cfg.trainer.policy_weight = static_cast<float>(getDouble(ctx, trV, "policyWeight", cfg.trainer.policy_weight));
        cfg.trainer.value_weight  = static_cast<float>(getDouble(ctx, trV, "valueWeight",  cfg.trainer.value_weight));
        cfg.trainer.publish_every = getInt(ctx, trV, "publishEvery", cfg.trainer.publish_every);
        cfg.trainer.rng_seed      = getU64(ctx, trV, "rngSeed", cfg.trainer.rng_seed);
    }
    JS_FreeValue(ctx, trV);

    JSValue ckptV = JS_GetPropertyStr(ctx, opts, "ckpt");
    if (JS_IsObject(ckptV)) {
        cfg.ckpt_dir       = getString(ctx, ckptV, "dir",      cfg.ckpt_dir.c_str());
        cfg.ckpt_ring_size = getInt   (ctx, ckptV, "ringSize", cfg.ckpt_ring_size);
        cfg.best_window    = getInt   (ctx, ckptV, "bestWindow", cfg.best_window);
    }
    JS_FreeValue(ctx, ckptV);

    cfg.ingest_burst   = getInt(ctx, opts, "ingestBurst",  cfg.ingest_burst);
    cfg.steps_per_tick = getInt(ctx, opts, "stepsPerTick", cfg.steps_per_tick);

    if (cfg.net.in_dim <= 0 || cfg.net.num_actions <= 0) {
        return JS_ThrowTypeError(ctx, "createGridTrainer: net.inDim and net.numActions must be > 0");
    }

    auto* d = new GridTrainerData();
    d->ctx = ctx;
    d->tr  = std::make_unique<grid::GridTrainer>(std::move(cfg));
    return qjsbind::wrap<GridTrainerData>(ctx, d);
}

} // namespace (anonymous)

// ─── installGridBindings ───────────────────────────────────────────────────

void installGridBindings(JSContext* ctx, JSValue gameObj) {
    JSValue gridObj = JS_NewObject(ctx);

    // ObsWindow.
    //
    // gc_mark exposes the JS function refs we hold (tile_fn + per-layer
    // sample_fns / enumerate_fns) to QuickJS's cycle GC. Without it the
    // typical wiring forms an unbreakable cycle: the wrapper is held by a
    // module-scope `_win` whose closure scope is captured by the very
    // sample/enumerate functions we DupValue into the wrapper. Refcount-
    // only collection can't break that loop, and the runtime asserts on
    // a non-empty gc_obj_list during JS_FreeRuntime.
    qjsbind::Class<ObsWindowData>(ctx, "AIGridObsWindow", qjsbind::NoGlobal)
        .gc_mark([](ObsWindowData* d, JSRuntime* rt, JS_MarkFunc* mark) {
            if (!d) return;
            JS_MarkValue(rt, d->tile_fn, mark);
            for (auto& v : d->enumerate_fns) JS_MarkValue(rt, v, mark);
            for (auto& v : d->sample_fns)    JS_MarkValue(rt, v, mark);
        })
        .get("outDim", [](ObsWindowData* d) { return d && d->win ? d->win->out_dim() : 0; })
        .method("layout", [](ObsWindowData* d, JSContext* ctx) -> JSValue {
            JSValue o = JS_NewObject(ctx);
            if (!d || !d->win) return o;
            const auto& L = d->win->layout();
            JS_SetPropertyStr(ctx, o, "cols",         JS_NewInt32(ctx, L.cols));
            JS_SetPropertyStr(ctx, o, "rows",         JS_NewInt32(ctx, L.rows));
            JS_SetPropertyStr(ctx, o, "tileOffset",   JS_NewInt32(ctx, L.tile_offset));
            JS_SetPropertyStr(ctx, o, "tileChannels", JS_NewInt32(ctx, L.tile_channels));
            JS_SetPropertyStr(ctx, o, "tileSize",     JS_NewInt32(ctx, L.tile_size));
            JSValue layers = JS_NewArray(ctx);
            for (size_t i = 0; i < L.layers.size(); ++i) {
                JSValue li = JS_NewObject(ctx);
                JS_SetPropertyStr(ctx, li, "offset",   JS_NewInt32(ctx, L.layers[i].offset));
                JS_SetPropertyStr(ctx, li, "channels", JS_NewInt32(ctx, L.layers[i].channels));
                JS_SetPropertyStr(ctx, li, "size",     JS_NewInt32(ctx, L.layers[i].size));
                JS_SetPropertyUint32(ctx, layers, static_cast<uint32_t>(i), li);
            }
            JS_SetPropertyStr(ctx, o, "layers",     layers);
            JS_SetPropertyStr(ctx, o, "selfOffset", JS_NewInt32(ctx, L.self_offset));
            JS_SetPropertyStr(ctx, o, "selfSize",   JS_NewInt32(ctx, L.self_size));
            JS_SetPropertyStr(ctx, o, "total",      JS_NewInt32(ctx, L.total));
            return o;
        })
        .method_raw("build",
            [](JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) -> JSValue {
                auto* d = qjsbind::unwrap<ObsWindowData>(ctx, this_val);
                if (!d || !d->win || argc < 2) return JS_UNDEFINED;
                int32_t ec = 0, er = 0;
                JS_ToInt32(ctx, &ec, argv[0]);
                JS_ToInt32(ctx, &er, argv[1]);
                std::vector<float> self;
                if (argc >= 3) self = readFloat32Array(ctx, argv[2]);
                std::vector<float> out(static_cast<size_t>(d->win->out_dim()), 0.0f);
                d->win->build(ec, er, self.empty() ? nullptr : self.data(), self.size(), out.data());
                return make_float32_array(ctx, out);
            }, 3);

    JS_SetPropertyStr(ctx, gridObj, "createObsWindow",
        JS_NewCFunction(ctx, js_createObsWindow, "createObsWindow", 1));

    // FrameStack
    qjsbind::Class<FrameStackData>(ctx, "AIGridFrameStack", qjsbind::NoGlobal)
        .get("outDim",   [](FrameStackData* d) { return d && d->fs ? d->fs->out_dim()  : 0; })
        .get("innerDim", [](FrameStackData* d) { return d && d->fs ? d->fs->inner_dim(): 0; })
        .get("k",        [](FrameStackData* d) { return d && d->fs ? d->fs->k()        : 0; })
        .get("filled",   [](FrameStackData* d) { return d && d->fs ? d->fs->filled()   : 0; })
        .method("reset", [](FrameStackData* d) { if (d && d->fs) d->fs->reset(); })
        .method_raw("push",
            [](JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) -> JSValue {
                auto* d = qjsbind::unwrap<FrameStackData>(ctx, this_val);
                if (!d || !d->fs || argc < 1) return JS_UNDEFINED;
                auto v = readFloat32Array(ctx, argv[0]);
                if (static_cast<int>(v.size()) < d->fs->inner_dim()) v.resize(static_cast<size_t>(d->fs->inner_dim()), 0.0f);
                d->fs->push(v.data());
                return JS_UNDEFINED;
            }, 1)
        .method("read", [](FrameStackData* d, JSContext* ctx) -> JSValue {
            if (!d || !d->fs) return make_float32_array(ctx, {});
            return make_float32_array(ctx, d->fs->read());
        });
    JS_SetPropertyStr(ctx, gridObj, "createFrameStack",
        JS_NewCFunction(ctx, js_createFrameStack, "createFrameStack", 1));

    // FailureTape
    qjsbind::Class<FailureTapeData>(ctx, "AIGridFailureTape", qjsbind::NoGlobal)
        .get("size", [](FailureTapeData* d) { return d && d->tape ? d->tape->size() : 0; })
        .get("capacity", [](FailureTapeData* d) { return d && d->tape ? d->tape->capacity() : 0; })
        .method_raw("recordFailure",
            [](JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) -> JSValue {
                auto* d = qjsbind::unwrap<FailureTapeData>(ctx, this_val);
                if (!d || !d->tape || argc < 1 || !JS_IsArray(argv[0])) return JS_UNDEFINED;
                std::vector<grid::FailureStep> tail;
                JSValue lv = JS_GetPropertyStr(ctx, argv[0], "length");
                uint32_t n = 0; JS_ToUint32(ctx, &n, lv); JS_FreeValue(ctx, lv);
                for (uint32_t i = 0; i < n; ++i) {
                    JSValue e = JS_GetPropertyUint32(ctx, argv[0], i);
                    grid::FailureStep s;
                    s.sig    = getString(ctx, e, "sig", "");
                    s.action = getInt(ctx, e, "action", -1);
                    tail.push_back(std::move(s));
                    JS_FreeValue(ctx, e);
                }
                d->tape->record_failure(tail);
                return JS_UNDEFINED;
            }, 1)
        .method_raw("multipliers",
            [](JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) -> JSValue {
                auto* d = qjsbind::unwrap<FailureTapeData>(ctx, this_val);
                if (!d || !d->tape || argc < 2) return make_float32_array(ctx, {});
                const char* sigC = JS_ToCString(ctx, argv[0]);
                std::string sig = sigC ? sigC : ""; if (sigC) JS_FreeCString(ctx, sigC);
                int32_t n = 0; JS_ToInt32(ctx, &n, argv[1]);
                return make_float32_array(ctx, d->tape->multipliers(sig, n));
            }, 2)
        .method_raw("applyPriors",
            [](JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) -> JSValue {
                auto* d = qjsbind::unwrap<FailureTapeData>(ctx, this_val);
                if (!d || !d->tape || argc < 2) return make_float32_array(ctx, {});
                const char* sigC = JS_ToCString(ctx, argv[0]);
                std::string sig = sigC ? sigC : ""; if (sigC) JS_FreeCString(ctx, sigC);
                auto prior = readFloat32Array(ctx, argv[1]);
                d->tape->apply_priors(sig, prior.data(), static_cast<int>(prior.size()));
                return make_float32_array(ctx, prior);
            }, 2)
        .method("clear", [](FailureTapeData* d) { if (d && d->tape) d->tape->clear(); });
    JS_SetPropertyStr(ctx, gridObj, "createFailureTape",
        JS_NewCFunction(ctx, js_createFailureTape, "createFailureTape", 1));

    // BestCrop
    qjsbind::Class<BestCropData>(ctx, "AIGridBestCrop", qjsbind::NoGlobal)
        .get("size",     [](BestCropData* d) { return d && d->pool ? d->pool->size()     : 0; })
        .get("capacity", [](BestCropData* d) { return d && d->pool ? d->pool->capacity() : 0; })
        .method_raw("push",
            [](JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) -> JSValue {
                auto* d = qjsbind::unwrap<BestCropData>(ctx, this_val);
                if (!d || !d->pool || argc < 1 || !JS_IsObject(argv[0])) return JS_UNDEFINED;
                JSValue snap = JS_GetPropertyStr(ctx, argv[0], "snapshot");
                std::any sn{ JsValueHolder(ctx, snap) }; JS_FreeValue(ctx, snap);
                JSValue prefV = JS_GetPropertyStr(ctx, argv[0], "prefix");
                auto pref = readIntArray(ctx, prefV); JS_FreeValue(ctx, prefV);
                float score = static_cast<float>(getDouble(ctx, argv[0], "score", 0.0));
                int   depth = getInt(ctx, argv[0], "depth", 0);
                d->pool->push(std::move(sn), std::move(pref), score, depth);
                return JS_UNDEFINED;
            }, 1)
        .method("seed", [](BestCropData* d, JSContext* ctx) -> JSValue {
            JSValue o = JS_NewObject(ctx);
            if (!d || !d->pool || d->pool->empty()) return o;
            auto seed = d->pool->seed(d->rng);
            JSValue snap = JS_NULL;
            if (seed.snapshot.has_value()) {
                if (auto* h = std::any_cast<JsValueHolder>(&seed.snapshot)) {
                    snap = JS_DupValue(ctx, h->v);
                }
            }
            JS_SetPropertyStr(ctx, o, "snapshot", snap);
            JS_SetPropertyStr(ctx, o, "prefix", make_int32_array_from_ints(ctx, seed.prefix));
            return o;
        })
        .method("clear", [](BestCropData* d) { if (d && d->pool) d->pool->clear(); });
    JS_SetPropertyStr(ctx, gridObj, "createBestCrop",
        JS_NewCFunction(ctx, js_createBestCrop, "createBestCrop", 1));

    // PotentialShaper
    qjsbind::Class<PotentialShaperData>(ctx, "AIGridPotentialShaper", qjsbind::NoGlobal)
        .get("gamma", [](PotentialShaperData* d) -> double { return d && d->sh ? d->sh->gamma() : 0.0; })
        .method("reset", [](PotentialShaperData* d, double phi0) {
            if (d && d->sh) d->sh->reset(static_cast<float>(phi0));
        })
        .method("step", [](PotentialShaperData* d, double phi) -> double {
            return (d && d->sh) ? static_cast<double>(d->sh->step(static_cast<float>(phi))) : 0.0;
        });
    JS_SetPropertyStr(ctx, gridObj, "createPotentialShaper",
        JS_NewCFunction(ctx, [](JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) -> JSValue {
            float gamma = 0.99f;
            if (argc >= 1 && JS_IsObject(argv[0])) gamma = static_cast<float>(getDouble(ctx, argv[0], "gamma", 0.99));
            auto* d = new PotentialShaperData();
            d->sh = std::make_unique<grid::PotentialShaper>(gamma);
            return qjsbind::wrap<PotentialShaperData>(ctx, d);
        }, "createPotentialShaper", 1));

    // StallDetector
    qjsbind::Class<StallDetectorData>(ctx, "AIGridStallDetector", qjsbind::NoGlobal)
        .method("reset", [](StallDetectorData* d) { if (d && d->det) d->det->reset(); })
        .method("tick", [](StallDetectorData* d, double progress) -> bool {
            return d && d->det ? d->det->tick(static_cast<float>(progress)) : false;
        });
    JS_SetPropertyStr(ctx, gridObj, "createStallDetector",
        JS_NewCFunction(ctx, [](JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) -> JSValue {
            float eps = 0.0f; int patience = 60;
            if (argc >= 1 && JS_IsObject(argv[0])) {
                eps      = static_cast<float>(getDouble(ctx, argv[0], "epsilon", 0.0));
                patience = getInt(ctx, argv[0], "patience", 60);
            }
            auto* d = new StallDetectorData();
            d->det = std::make_unique<grid::StallDetector>(eps, patience);
            return qjsbind::wrap<StallDetectorData>(ctx, d);
        }, "createStallDetector", 1));

    // BC ingestion (free function)
    JS_SetPropertyStr(ctx, gridObj, "generateBC",
        JS_NewCFunction(ctx, js_generateBC, "generateBC", 1));

    // Generic recorder
    qjsbind::Class<GenericRecorderData>(ctx, "AIGridGenericRecorder", qjsbind::NoGlobal)
        .get("frameCount", [](GenericRecorderData* d) { return d && d->rec ? static_cast<int>(d->rec->frame_count()) : 0; })
        .method_raw("open",
            [](JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) -> JSValue {
                auto* d = qjsbind::unwrap<GenericRecorderData>(ctx, this_val);
                if (!d || !d->rec || argc < 5) return JS_FALSE;
                const char* path = JS_ToCString(ctx, argv[0]);
                int64_t ep = 0;  JS_ToBigInt64(ctx, &ep, argv[1]);
                int64_t sd = 0;  JS_ToBigInt64(ctx, &sd, argv[2]);
                double dt = 0;   JS_ToFloat64(ctx, &dt, argv[3]);
                JSValueConst schemas = argv[4];
                JSValue rs = JS_GetPropertyStr(ctx, schemas, "roster");
                JSValue fs = JS_GetPropertyStr(ctx, schemas, "frame");
                JSValue es = JS_GetPropertyStr(ctx, schemas, "events");
                d->roster = readSchema(ctx, rs);
                d->frame  = readSchema(ctx, fs);
                d->events = readSchema(ctx, es);
                JS_FreeValue(ctx, rs); JS_FreeValue(ctx, fs); JS_FreeValue(ctx, es);
                bool ok = d->rec->open(path ? path : "",
                    static_cast<uint64_t>(ep), static_cast<uint64_t>(sd),
                    static_cast<float>(dt), d->roster, d->frame, d->events);
                if (path) JS_FreeCString(ctx, path);
                return ok ? JS_TRUE : JS_FALSE;
            }, 5)
        .method_raw("writeRoster",
            [](JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) -> JSValue {
                auto* d = qjsbind::unwrap<GenericRecorderData>(ctx, this_val);
                if (!d || !d->rec || argc < 1) return JS_UNDEFINED;
                d->rec->write_roster(rowsFromJs(ctx, argv[0], d->roster));
                return JS_UNDEFINED;
            }, 1)
        .method_raw("recordFrame",
            [](JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) -> JSValue {
                auto* d = qjsbind::unwrap<GenericRecorderData>(ctx, this_val);
                if (!d || !d->rec || argc < 3) return JS_UNDEFINED;
                int64_t step = 0; JS_ToBigInt64(ctx, &step, argv[0]);
                double el = 0;    JS_ToFloat64(ctx, &el, argv[1]);
                auto rows = rowsFromJs(ctx, argv[2], d->frame);
                std::vector<grid::Row> events;
                if (argc >= 4) events = rowsFromJs(ctx, argv[3], d->events);
                d->rec->record_frame(static_cast<uint64_t>(step), static_cast<float>(el),
                                     rows, events);
                return JS_UNDEFINED;
            }, 4)
        .method("close", [](GenericRecorderData* d) -> bool {
            return d && d->rec ? d->rec->close() : false;
        });
    JS_SetPropertyStr(ctx, gridObj, "createGenericRecorder",
        JS_NewCFunction(ctx, js_createGenericRecorder, "createGenericRecorder", 0));

    // Generic replay reader
    qjsbind::Class<GenericReplayReaderData>(ctx, "AIGridGenericReplayReader", qjsbind::NoGlobal)
        .get("frameCount", [](GenericReplayReaderData* d) { return d && d->rr ? static_cast<int>(d->rr->frame_count()) : 0; })
        .method_raw("open",
            [](JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) -> JSValue {
                auto* d = qjsbind::unwrap<GenericReplayReaderData>(ctx, this_val);
                if (!d || !d->rr || argc < 1) return JS_FALSE;
                const char* path = JS_ToCString(ctx, argv[0]);
                bool ok = d->rr->open(path ? path : "");
                if (path) JS_FreeCString(ctx, path);
                return ok ? JS_TRUE : JS_FALSE;
            }, 1)
        .method("frame", [](GenericReplayReaderData* d, JSContext* ctx, int i) -> JSValue {
            JSValue o = JS_NewObject(ctx);
            if (!d || !d->rr) return o;
            auto fr = d->rr->frame(static_cast<size_t>(i));
            JS_SetPropertyStr(ctx, o, "stepIdx", JS_NewBigInt64(ctx, static_cast<int64_t>(fr.step_idx)));
            JS_SetPropertyStr(ctx, o, "elapsed", JS_NewFloat64(ctx, fr.elapsed));
            JSValue rows = JS_NewArray(ctx);
            for (size_t i = 0; i < fr.rows.size(); ++i)
                JS_SetPropertyUint32(ctx, rows, static_cast<uint32_t>(i), rowToJs(ctx, fr.rows[i]));
            JS_SetPropertyStr(ctx, o, "rows", rows);
            JSValue evs = JS_NewArray(ctx);
            for (size_t i = 0; i < fr.events.size(); ++i)
                JS_SetPropertyUint32(ctx, evs, static_cast<uint32_t>(i), rowToJs(ctx, fr.events[i]));
            JS_SetPropertyStr(ctx, o, "events", evs);
            return o;
        })
        .method_raw("trajectory",
            [](JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) -> JSValue {
                auto* d = qjsbind::unwrap<GenericReplayReaderData>(ctx, this_val);
                if (!d || !d->rr || argc < 2) return JS_NewArray(ctx);
                int32_t row = 0; JS_ToInt32(ctx, &row, argv[0]);
                const char* name = JS_ToCString(ctx, argv[1]);
                auto vals = d->rr->trajectory(static_cast<size_t>(row), name ? name : "");
                if (name) JS_FreeCString(ctx, name);
                JSValue arr = JS_NewArray(ctx);
                for (size_t i = 0; i < vals.size(); ++i) {
                    JSValue elem;
                    const auto& v = vals[i];
                    if      (auto* p = std::get_if<int32_t>(&v)) elem = JS_NewInt32(ctx, *p);
                    else if (auto* p = std::get_if<int64_t>(&v)) elem = JS_NewBigInt64(ctx, *p);
                    else if (auto* p = std::get_if<float>(&v))   elem = JS_NewFloat64(ctx, *p);
                    else                                          elem = JS_NewFloat64(ctx, std::get<double>(v));
                    JS_SetPropertyUint32(ctx, arr, static_cast<uint32_t>(i), elem);
                }
                return arr;
            }, 2);
    JS_SetPropertyStr(ctx, gridObj, "createGenericReplayReader",
        JS_NewCFunction(ctx, js_createGenericReplayReader, "createGenericReplayReader", 0));

    // GridTrainer
    qjsbind::Class<GridTrainerData>(ctx, "AIGridTrainer", qjsbind::NoGlobal)
        .get("running", [](GridTrainerData* d) { return d && d->tr ? d->tr->running() : false; })
        .method_raw("ingestSituation",
            [](JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) -> JSValue {
                auto* d = qjsbind::unwrap<GridTrainerData>(ctx, this_val);
                if (!d || !d->tr || argc < 1) return JS_UNDEFINED;
                d->tr->ingest_situation(situationFromJs(ctx, argv[0]));
                return JS_UNDEFINED;
            }, 1)
        .method_raw("ingestEpisode",
            [](JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) -> JSValue {
                auto* d = qjsbind::unwrap<GridTrainerData>(ctx, this_val);
                if (!d || !d->tr || argc < 1 || !JS_IsObject(argv[0])) return JS_UNDEFINED;
                grid::EpisodeSummary e;
                e.total_return = static_cast<float>(getDouble(ctx, argv[0], "totalReturn", 0.0));
                e.depth        = getInt(ctx, argv[0], "depth", 0);
                JSValue f = JS_GetPropertyStr(ctx, argv[0], "failed");
                e.failed = JS_ToBool(ctx, f) > 0;
                JS_FreeValue(ctx, f);
                JSValue snap = JS_GetPropertyStr(ctx, argv[0], "snapshot");
                if (!JS_IsUndefined(snap) && !JS_IsNull(snap))
                    e.start_snapshot = std::any{ JsValueHolder(ctx, snap) };
                JS_FreeValue(ctx, snap);
                JSValue pref = JS_GetPropertyStr(ctx, argv[0], "prefix");
                e.action_prefix = readIntArray(ctx, pref);
                JS_FreeValue(ctx, pref);
                JSValue tail = JS_GetPropertyStr(ctx, argv[0], "tail");
                if (JS_IsArray(tail)) {
                    JSValue lv = JS_GetPropertyStr(ctx, tail, "length");
                    uint32_t n = 0; JS_ToUint32(ctx, &n, lv); JS_FreeValue(ctx, lv);
                    for (uint32_t i = 0; i < n; ++i) {
                        JSValue se = JS_GetPropertyUint32(ctx, tail, i);
                        grid::FailureStep st;
                        st.sig    = getString(ctx, se, "sig", "");
                        st.action = getInt(ctx, se, "action", -1);
                        e.failure_tail.push_back(std::move(st));
                        JS_FreeValue(ctx, se);
                    }
                }
                JS_FreeValue(ctx, tail);
                d->tr->ingest_episode(std::move(e));
                return JS_UNDEFINED;
            }, 1)
        .method_raw("warmupWith",
            [](JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) -> JSValue {
                auto* d = qjsbind::unwrap<GridTrainerData>(ctx, this_val);
                if (!d || !d->tr || argc < 1 || !JS_IsArray(argv[0])) return JS_UNDEFINED;
                JSValue lv = JS_GetPropertyStr(ctx, argv[0], "length");
                uint32_t n = 0; JS_ToUint32(ctx, &n, lv); JS_FreeValue(ctx, lv);
                std::vector<learn::GenericSituation> sits;
                sits.reserve(n);
                for (uint32_t i = 0; i < n; ++i) {
                    JSValue s = JS_GetPropertyUint32(ctx, argv[0], i);
                    sits.push_back(situationFromJs(ctx, s));
                    JS_FreeValue(ctx, s);
                }
                d->tr->warmup_with(sits);
                return JS_UNDEFINED;
            }, 1)
        .method("start",      [](GridTrainerData* d) { if (d && d->tr) d->tr->start(); })
        .method("stop",       [](GridTrainerData* d) { if (d && d->tr) d->tr->stop();  })
        .method("stepSync",   [](GridTrainerData* d, int n) { if (d && d->tr) d->tr->step_sync(n); })
        .method("stats", [](GridTrainerData* d, JSContext* ctx) -> JSValue {
            JSValue o = JS_NewObject(ctx);
            if (!d || !d->tr) return o;
            auto s = d->tr->stats();
            JS_SetPropertyStr(ctx, o, "totalSteps",         JS_NewInt32(ctx, s.total_steps));
            JS_SetPropertyStr(ctx, o, "totalPublishes",     JS_NewInt32(ctx, s.total_publishes));
            JS_SetPropertyStr(ctx, o, "episodesIngested",   JS_NewInt32(ctx, s.episodes_ingested));
            JS_SetPropertyStr(ctx, o, "trailingMeanReturn", JS_NewFloat64(ctx, s.trailing_mean_return));
            JS_SetPropertyStr(ctx, o, "bestMeanReturn",     JS_NewFloat64(ctx, s.best_mean_return));
            JS_SetPropertyStr(ctx, o, "bufferSize",         JS_NewInt32(ctx, s.buffer_size));
            JS_SetPropertyStr(ctx, o, "running",            JS_NewBool(ctx, s.running));
            return o;
        })
        .method("pollEvents", [](GridTrainerData* d, JSContext* ctx) -> JSValue {
            JSValue arr = JS_NewArray(ctx);
            if (!d || !d->tr) return arr;
            auto evs = d->tr->poll_events();
            for (size_t i = 0; i < evs.size(); ++i) {
                const auto& e = evs[i];
                JSValue o = JS_NewObject(ctx);
                const char* k = "?";
                switch (e.kind) {
                    case grid::GridEvent::Kind::WeightsUpdated:  k = "weightsUpdated";  break;
                    case grid::GridEvent::Kind::BestRotated:     k = "bestRotated";     break;
                    case grid::GridEvent::Kind::EpisodeIngested: k = "episodeIngested"; break;
                }
                JS_SetPropertyStr(ctx, o, "kind", JS_NewString(ctx, k));
                JS_SetPropertyStr(ctx, o, "version",      JS_NewBigInt64(ctx, static_cast<int64_t>(e.version)));
                JS_SetPropertyStr(ctx, o, "totalSteps",   JS_NewInt32(ctx, e.total_steps));
                JS_SetPropertyStr(ctx, o, "episodeCount", JS_NewInt32(ctx, e.episode_count));
                JS_SetPropertyStr(ctx, o, "meanReturn",   JS_NewFloat64(ctx, e.mean_return));
                JS_SetPropertyStr(ctx, o, "path",         JS_NewString(ctx, e.path.c_str()));
                JS_SetPropertyUint32(ctx, arr, static_cast<uint32_t>(i), o);
            }
            return arr;
        });
    JS_SetPropertyStr(ctx, gridObj, "createGridTrainer",
        JS_NewCFunction(ctx, js_createGridTrainer, "createGridTrainer", 1));

    JS_SetPropertyStr(ctx, gameObj, "grid", gridObj);
}

} // namespace bro::js
