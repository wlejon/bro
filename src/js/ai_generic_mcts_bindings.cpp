// JS bindings for brogameagent::mcts::GenericMcts — env-agnostic PUCT
// search exposed as `bro.ai.game.createGenericMcts({env, ...})`.
//
// The C++ class takes std::function callbacks for snapshot/restore/step/
// legalActions/observe (+ optional prior/value); the binding wraps each
// JS-side method into one of those std::functions and routes calls back
// across the FFI. State is held inside std::any as a duplicated JSValue
// reference so the caller's snapshot type can be any JS value.
//
// Installed onto bro.ai.game by installGenericMctsBindings().

#include "js/ai_bindings.h"

#include <qjsbind/qjsbind.h>
#include <brogameagent/generic_mcts.h>

#include <any>
#include <cstring>
#include <memory>
#include <vector>

namespace bro::js {

namespace mcts = brogameagent::mcts;

// ─── JSValue lifetime holder ───────────────────────────────────────────────
// std::any requires copy-constructibility, so we wrap each held JSValue in
// a small refcounting helper that DupValues on copy and FreeValues on dtor.
// Used as the storage type inside std::any for env snapshots.
namespace {

struct JsValueHolder {
    JSContext* ctx{nullptr};
    JSValue    v{JS_UNDEFINED};

    JsValueHolder() = default;
    JsValueHolder(JSContext* c, JSValueConst val) : ctx(c), v(JS_DupValue(c, val)) {}
    JsValueHolder(const JsValueHolder& other) : ctx(other.ctx), v(JS_DupValue(other.ctx, other.v)) {}
    JsValueHolder& operator=(const JsValueHolder& other) {
        if (this != &other) {
            if (ctx) JS_FreeValue(ctx, v);
            ctx = other.ctx;
            v   = JS_DupValue(other.ctx, other.v);
        }
        return *this;
    }
    JsValueHolder(JsValueHolder&& other) noexcept : ctx(other.ctx), v(other.v) {
        other.ctx = nullptr;
        other.v   = JS_UNDEFINED;
    }
    JsValueHolder& operator=(JsValueHolder&& other) noexcept {
        if (this != &other) {
            if (ctx) JS_FreeValue(ctx, v);
            ctx       = other.ctx;
            v         = other.v;
            other.ctx = nullptr;
            other.v   = JS_UNDEFINED;
        }
        return *this;
    }
    ~JsValueHolder() {
        if (ctx) JS_FreeValue(ctx, v);
    }
};

// ─── Helpers for marshalling typed-array results out of JS ─────────────────

// Read a Float32Array (or any view backed by floats) into a std::vector<float>.
// Returns empty vector if the value isn't a typed-array view.
std::vector<float> readFloat32Array(JSContext* ctx, JSValueConst val) {
    std::vector<float> out;
    if (JS_IsUndefined(val) || JS_IsNull(val)) return out;
    size_t byte_off = 0, view_len = 0;
    JSValue abuf = JS_GetTypedArrayBuffer(ctx, val, &byte_off, &view_len, nullptr);
    if (JS_IsException(abuf)) { JS_GetException(ctx); return out; }
    size_t abuf_len = 0;
    uint8_t* raw = JS_GetArrayBuffer(ctx, &abuf_len, abuf);
    JS_FreeValue(ctx, abuf);
    if (!raw) return out;
    const size_t n = view_len / sizeof(float);
    out.resize(n);
    if (n > 0) std::memcpy(out.data(), raw + byte_off, n * sizeof(float));
    return out;
}

// Read either a typed integer array (Int32Array) or a plain JS array of
// numbers into std::vector<int>.
std::vector<int> readIntArray(JSContext* ctx, JSValueConst val) {
    std::vector<int> out;
    if (JS_IsUndefined(val) || JS_IsNull(val)) return out;

    // Try typed array first.
    size_t byte_off = 0, view_len = 0;
    JSValue abuf = JS_GetTypedArrayBuffer(ctx, val, &byte_off, &view_len, nullptr);
    if (!JS_IsException(abuf)) {
        size_t abuf_len = 0;
        uint8_t* raw = JS_GetArrayBuffer(ctx, &abuf_len, abuf);
        JS_FreeValue(ctx, abuf);
        if (raw) {
            const size_t n = view_len / sizeof(int32_t);
            out.resize(n);
            if (n > 0) std::memcpy(out.data(), raw + byte_off, n * sizeof(int32_t));
            return out;
        }
    } else {
        JS_GetException(ctx);
    }

    // Fall back to plain Array<number>.
    if (!JS_IsArray(val)) return out;
    JSValue len_v = JS_GetPropertyStr(ctx, val, "length");
    uint32_t len = 0;
    JS_ToUint32(ctx, &len, len_v);
    JS_FreeValue(ctx, len_v);
    out.reserve(len);
    for (uint32_t i = 0; i < len; ++i) {
        JSValue elem = JS_GetPropertyUint32(ctx, val, i);
        int32_t x = 0;
        JS_ToInt32(ctx, &x, elem);
        JS_FreeValue(ctx, elem);
        out.push_back(x);
    }
    return out;
}

JSValue makeFloat32Array(JSContext* ctx, const std::vector<float>& v) {
    JSValue abuf = JS_NewArrayBufferCopy(
        ctx, reinterpret_cast<const uint8_t*>(v.data()), v.size() * sizeof(float));
    JSValue args[3] = { abuf, JS_UNDEFINED, JS_UNDEFINED };
    JSValue arr = JS_NewTypedArray(ctx, 1, args, JS_TYPED_ARRAY_FLOAT32);
    JS_FreeValue(ctx, abuf);
    return arr;
}

JSValue makeInt32Array(JSContext* ctx, const std::vector<int>& v) {
    JSValue abuf = JS_NewArrayBufferCopy(
        ctx, reinterpret_cast<const uint8_t*>(v.data()), v.size() * sizeof(int32_t));
    JSValue args[3] = { abuf, JS_UNDEFINED, JS_UNDEFINED };
    JSValue arr = JS_NewTypedArray(ctx, 1, args, JS_TYPED_ARRAY_INT32);
    JS_FreeValue(ctx, abuf);
    return arr;
}

double getDouble(JSContext* ctx, JSValueConst obj, const char* k, double def) {
    JSValue v = JS_GetPropertyStr(ctx, obj, k);
    double out = def;
    if (JS_IsNumber(v)) JS_ToFloat64(ctx, &out, v);
    JS_FreeValue(ctx, v);
    return out;
}
int32_t getInt(JSContext* ctx, JSValueConst obj, const char* k, int32_t def) {
    JSValue v = JS_GetPropertyStr(ctx, obj, k);
    int32_t out = def;
    if (JS_IsNumber(v)) JS_ToInt32(ctx, &out, v);
    JS_FreeValue(ctx, v);
    return out;
}

} // namespace

// ─── Wrapper data ──────────────────────────────────────────────────────────
// Owns the C++ MCTS plus DupValue'd refs to every JS callback. The
// GenericEnv stored inside the MCTS captures these by value (copy of
// JSValueConst), so we keep them alive as long as the wrapper is.

struct GenericMctsData {
    JSContext* ctx{nullptr};
    std::unique_ptr<mcts::GenericMcts> m;

    // DupValue'd JS callable references — freed in dtor.
    JSValue env_obj    = JS_UNDEFINED;
    JSValue snapshot_fn= JS_UNDEFINED;
    JSValue restore_fn = JS_UNDEFINED;
    JSValue step_fn    = JS_UNDEFINED;
    JSValue legal_fn   = JS_UNDEFINED;
    JSValue observe_fn = JS_UNDEFINED;
    JSValue prior_fn   = JS_UNDEFINED;
    JSValue value_fn   = JS_UNDEFINED;

    int num_actions = 0;

    GenericMctsData() = default;
    GenericMctsData(const GenericMctsData&) = delete;
    GenericMctsData& operator=(const GenericMctsData&) = delete;
    ~GenericMctsData() {
        if (!ctx) return;
        JS_FreeValue(ctx, env_obj);
        JS_FreeValue(ctx, snapshot_fn);
        JS_FreeValue(ctx, restore_fn);
        JS_FreeValue(ctx, step_fn);
        JS_FreeValue(ctx, legal_fn);
        JS_FreeValue(ctx, observe_fn);
        JS_FreeValue(ctx, prior_fn);
        JS_FreeValue(ctx, value_fn);
    }
};

// ─── Build the C++ env from JS callables ───────────────────────────────────

static mcts::GenericEnv makeGenericEnv(GenericMctsData* d) {
    JSContext* ctx = d->ctx;
    mcts::GenericEnv env;
    env.num_actions = d->num_actions;

    env.snapshot_fn = [d, ctx]() -> std::any {
        JSValue r = JS_Call(ctx, d->snapshot_fn, d->env_obj, 0, nullptr);
        if (JS_IsException(r)) {
            JS_GetException(ctx);
            return std::any{};
        }
        std::any out{ JsValueHolder(ctx, r) };
        JS_FreeValue(ctx, r);
        return out;
    };

    env.restore_fn = [d, ctx](const std::any& s) {
        if (!s.has_value()) return;
        const auto* h = std::any_cast<JsValueHolder>(&s);
        if (!h) return;
        JSValueConst args[1] = { h->v };
        JSValue r = JS_Call(ctx, d->restore_fn, d->env_obj, 1, args);
        if (JS_IsException(r)) JS_GetException(ctx);
        JS_FreeValue(ctx, r);
    };

    env.step_fn = [d, ctx](int action) -> mcts::GenericStepResult {
        JSValue av = JS_NewInt32(ctx, action);
        JSValueConst args[1] = { av };
        JSValue r = JS_Call(ctx, d->step_fn, d->env_obj, 1, args);
        JS_FreeValue(ctx, av);
        mcts::GenericStepResult out{};
        if (JS_IsException(r)) { JS_GetException(ctx); return out; }
        if (JS_IsObject(r)) {
            out.reward = static_cast<float>(getDouble(ctx, r, "reward", 0.0));
            JSValue dv = JS_GetPropertyStr(ctx, r, "done");
            out.done = JS_ToBool(ctx, dv) > 0;
            JS_FreeValue(ctx, dv);
        }
        JS_FreeValue(ctx, r);
        return out;
    };

    env.legal_actions_fn = [d, ctx]() -> std::vector<int> {
        JSValue r = JS_Call(ctx, d->legal_fn, d->env_obj, 0, nullptr);
        if (JS_IsException(r)) { JS_GetException(ctx); return {}; }
        auto out = readIntArray(ctx, r);
        JS_FreeValue(ctx, r);
        return out;
    };

    env.observe_fn = [d, ctx]() -> std::vector<float> {
        JSValue r = JS_Call(ctx, d->observe_fn, d->env_obj, 0, nullptr);
        if (JS_IsException(r)) { JS_GetException(ctx); return {}; }
        auto out = readFloat32Array(ctx, r);
        JS_FreeValue(ctx, r);
        return out;
    };

    return env;
}

// Set or replace the prior_fn on the underlying MCTS. Caller has already
// stashed the JSValue ref on `d->prior_fn` (or set it to JS_UNDEFINED to
// clear).
static void rewirePrior(GenericMctsData* d) {
    JSContext* ctx = d->ctx;
    if (JS_IsFunction(ctx, d->prior_fn)) {
        d->m->set_prior_fn([d, ctx](const std::vector<float>& obs,
                                     const std::vector<int>& legal)
                           -> std::vector<float> {
            JSValue obs_v = makeFloat32Array(ctx, obs);
            JSValue leg_v = makeInt32Array(ctx, legal);
            JSValueConst args[2] = { obs_v, leg_v };
            JSValue r = JS_Call(ctx, d->prior_fn, JS_UNDEFINED, 2, args);
            JS_FreeValue(ctx, obs_v);
            JS_FreeValue(ctx, leg_v);
            if (JS_IsException(r)) { JS_GetException(ctx); return {}; }
            auto out = readFloat32Array(ctx, r);
            JS_FreeValue(ctx, r);
            return out;
        });
    } else {
        d->m->set_prior_fn(nullptr);
    }
}

static void rewireValue(GenericMctsData* d) {
    JSContext* ctx = d->ctx;
    if (JS_IsFunction(ctx, d->value_fn)) {
        d->m->set_value_fn([d, ctx](const std::vector<float>& obs) -> float {
            JSValue obs_v = makeFloat32Array(ctx, obs);
            JSValueConst args[1] = { obs_v };
            JSValue r = JS_Call(ctx, d->value_fn, JS_UNDEFINED, 1, args);
            JS_FreeValue(ctx, obs_v);
            if (JS_IsException(r)) { JS_GetException(ctx); return 0.0f; }
            double x = 0.0;
            JS_ToFloat64(ctx, &x, r);
            JS_FreeValue(ctx, r);
            return static_cast<float>(x);
        });
    } else {
        d->m->set_value_fn(nullptr);
    }
}

// ─── Factory: bro.ai.game.createGenericMcts({...}) ─────────────────────────
//
// Required env shape:
//   {
//     numActions:    int,
//     snapshot():    any,                 // opaque JS state object
//     restore(snap): void,
//     step(action):  { reward: number, done: boolean },
//     legalActions(): Int32Array | number[],
//     observe():     Float32Array,
//   }
// Plus optional top-level config keys (cPuct, gamma, rolloutDepth, iterations,
// dirichletAlpha, dirichletEpsilon, seed) and optional priorFn / valueFn.

static JSValue js_createGenericMcts(JSContext* ctx, JSValueConst,
                                    int argc, JSValueConst* argv) {
    if (argc < 1 || !JS_IsObject(argv[0])) {
        return JS_ThrowTypeError(ctx,
            "createGenericMcts(opts): expected an object");
    }
    JSValueConst opts = argv[0];

    // Pull the env. Either opts.env or opts itself is treated as the env.
    JSValue env_v = JS_GetPropertyStr(ctx, opts, "env");
    if (JS_IsUndefined(env_v) || JS_IsNull(env_v)) {
        JS_FreeValue(ctx, env_v);
        env_v = JS_DupValue(ctx, opts);   // env is opts
    }
    if (!JS_IsObject(env_v)) {
        JS_FreeValue(ctx, env_v);
        return JS_ThrowTypeError(ctx,
            "createGenericMcts: env must be an object");
    }

    auto* d = new GenericMctsData();
    d->ctx     = ctx;
    d->env_obj = JS_DupValue(ctx, env_v);

    auto getMethod = [&](const char* name) -> JSValue {
        JSValue fn = JS_GetPropertyStr(ctx, env_v, name);
        if (!JS_IsFunction(ctx, fn)) { JS_FreeValue(ctx, fn); return JS_UNDEFINED; }
        return fn;
    };
    d->snapshot_fn = getMethod("snapshot");
    d->restore_fn  = getMethod("restore");
    d->step_fn     = getMethod("step");
    d->legal_fn    = getMethod("legalActions");
    d->observe_fn  = getMethod("observe");

    d->num_actions = getInt(ctx, env_v, "numActions", 0);
    if (d->num_actions <= 0) {
        // Allow numActions to live on opts when env was passed inline.
        d->num_actions = getInt(ctx, opts, "numActions", 0);
    }

    JS_FreeValue(ctx, env_v);

    if (!JS_IsFunction(ctx, d->snapshot_fn) ||
        !JS_IsFunction(ctx, d->restore_fn)  ||
        !JS_IsFunction(ctx, d->step_fn)     ||
        !JS_IsFunction(ctx, d->legal_fn)    ||
        !JS_IsFunction(ctx, d->observe_fn)) {
        delete d;
        return JS_ThrowTypeError(ctx,
            "createGenericMcts: env must define snapshot/restore/step/legalActions/observe");
    }
    if (d->num_actions <= 0) {
        delete d;
        return JS_ThrowTypeError(ctx,
            "createGenericMcts: numActions must be a positive integer");
    }

    d->m = std::make_unique<mcts::GenericMcts>(makeGenericEnv(d));

    // Optional inline config + priorFn/valueFn on the same opts object.
    mcts::GenericMctsConfig cfg = d->m->config();
    cfg.iterations        = getInt   (ctx, opts, "iterations",        cfg.iterations);
    cfg.c_puct            = (float)getDouble(ctx, opts, "cPuct",      cfg.c_puct);
    cfg.gamma             = (float)getDouble(ctx, opts, "gamma",      cfg.gamma);
    cfg.rollout_depth     = getInt   (ctx, opts, "rolloutDepth",      cfg.rollout_depth);
    cfg.dirichlet_alpha   = (float)getDouble(ctx, opts, "dirichletAlpha",   cfg.dirichlet_alpha);
    cfg.dirichlet_epsilon = (float)getDouble(ctx, opts, "dirichletEpsilon", cfg.dirichlet_epsilon);
    {
        JSValue sv = JS_GetPropertyStr(ctx, opts, "seed");
        if (!JS_IsUndefined(sv) && !JS_IsNull(sv)) {
            int64_t s = 0;
            if (JS_ToBigInt64(ctx, &s, sv) != 0) {
                double ds = 0; JS_ToFloat64(ctx, &ds, sv); s = (int64_t)ds;
            }
            cfg.seed = (uint64_t)s;
        }
        JS_FreeValue(ctx, sv);
    }
    d->m->set_config(cfg);

    {
        JSValue pv = JS_GetPropertyStr(ctx, opts, "priorFn");
        if (JS_IsFunction(ctx, pv)) d->prior_fn = JS_DupValue(ctx, pv);
        JS_FreeValue(ctx, pv);
        rewirePrior(d);
    }
    {
        JSValue vv = JS_GetPropertyStr(ctx, opts, "valueFn");
        if (JS_IsFunction(ctx, vv)) d->value_fn = JS_DupValue(ctx, vv);
        JS_FreeValue(ctx, vv);
        rewireValue(d);
    }

    return qjsbind::wrap<GenericMctsData>(ctx, d);
}

// ─── Install ───────────────────────────────────────────────────────────────

void installGenericMctsBindings(JSContext* ctx, JSValue gameObj) {
    qjsbind::Class<GenericMctsData>(ctx, "AIGenericMcts", qjsbind::NoGlobal)
        .get("numActions", [](GenericMctsData* d) -> int { return d ? d->num_actions : 0; })
        .method_raw("search",
            [](JSContext* ctx, JSValueConst this_val, int /*argc*/, JSValueConst* /*argv*/) -> JSValue {
                auto* d = qjsbind::unwrap<GenericMctsData>(ctx, this_val);
                if (!d || !d->m) return JS_NewInt32(ctx, -1);
                return JS_NewInt32(ctx, d->m->search());
            }, 0)
        .method("rootVisits",
            [](GenericMctsData* d, JSContext* ctx) -> JSValue {
                if (!d || !d->m) return makeFloat32Array(ctx, {});
                return makeFloat32Array(ctx, d->m->root_visits());
            })
        .method("advanceRoot",
            [](GenericMctsData* d, int action) {
                if (d && d->m) d->m->advance_root(action);
            })
        .method("reset",
            [](GenericMctsData* d) {
                if (d && d->m) d->m->reset();
            })
        .method("setConfig",
            [](GenericMctsData* d, JSContext* ctx, JSValueConst cfgV) {
                if (!d || !d->m || !JS_IsObject(cfgV)) return;
                mcts::GenericMctsConfig c = d->m->config();
                c.iterations        = getInt   (ctx, cfgV, "iterations",        c.iterations);
                c.c_puct            = (float)getDouble(ctx, cfgV, "cPuct",      c.c_puct);
                c.gamma             = (float)getDouble(ctx, cfgV, "gamma",      c.gamma);
                c.rollout_depth     = getInt   (ctx, cfgV, "rolloutDepth",      c.rollout_depth);
                c.dirichlet_alpha   = (float)getDouble(ctx, cfgV, "dirichletAlpha",   c.dirichlet_alpha);
                c.dirichlet_epsilon = (float)getDouble(ctx, cfgV, "dirichletEpsilon", c.dirichlet_epsilon);
                JSValue sv = JS_GetPropertyStr(ctx, cfgV, "seed");
                if (!JS_IsUndefined(sv) && !JS_IsNull(sv)) {
                    int64_t s = 0;
                    if (JS_ToBigInt64(ctx, &s, sv) != 0) {
                        double ds = 0; JS_ToFloat64(ctx, &ds, sv); s = (int64_t)ds;
                    }
                    c.seed = (uint64_t)s;
                }
                JS_FreeValue(ctx, sv);
                d->m->set_config(c);
            })
        .method_raw("setPriorFn",
            [](JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) -> JSValue {
                auto* d = qjsbind::unwrap<GenericMctsData>(ctx, this_val);
                if (!d) return JS_UNDEFINED;
                JS_FreeValue(ctx, d->prior_fn);
                d->prior_fn = (argc >= 1 && JS_IsFunction(ctx, argv[0]))
                    ? JS_DupValue(ctx, argv[0])
                    : JS_UNDEFINED;
                rewirePrior(d);
                return JS_UNDEFINED;
            }, 1)
        .method_raw("setValueFn",
            [](JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) -> JSValue {
                auto* d = qjsbind::unwrap<GenericMctsData>(ctx, this_val);
                if (!d) return JS_UNDEFINED;
                JS_FreeValue(ctx, d->value_fn);
                d->value_fn = (argc >= 1 && JS_IsFunction(ctx, argv[0]))
                    ? JS_DupValue(ctx, argv[0])
                    : JS_UNDEFINED;
                rewireValue(d);
                return JS_UNDEFINED;
            }, 1)
        .method("lastStats",
            [](GenericMctsData* d, JSContext* ctx) -> JSValue {
                JSValue o = JS_NewObject(ctx);
                if (!d || !d->m) return o;
                const auto& s = d->m->last_stats();
                JS_SetPropertyStr(ctx, o, "iterations", JS_NewInt32(ctx, s.iterations));
                JS_SetPropertyStr(ctx, o, "treeSize",   JS_NewInt32(ctx, s.tree_size));
                JS_SetPropertyStr(ctx, o, "bestVisits", JS_NewInt32(ctx, s.best_visits));
                JS_SetPropertyStr(ctx, o, "bestAction", JS_NewInt32(ctx, s.best_action));
                return o;
            });

    JS_SetPropertyStr(ctx, gameObj, "createGenericMcts",
        JS_NewCFunction(ctx, js_createGenericMcts, "createGenericMcts", 1));
}

} // namespace bro::js
