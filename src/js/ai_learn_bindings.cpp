// JS bindings for brogameagent::learn — neural adapters, replay buffer,
// search-trace target extraction, Gumbel noise, and the ExIt trainer.
//
// Installed onto bro.ai.game.learn. Requires ai_nn_bindings for SingleHeroNet
// and WeightsHandle wrappers, and ai_bindings for Agent/World/Mcts accessors.

#include "js/ai_bindings.h"

#include <qjsbind/qjsbind.h>
#include <brogameagent/brogameagent.h>
#include <brogameagent/learn/neural_adapters.h>
#include <brogameagent/learn/replay_buffer.h>
#include <brogameagent/learn/search_trace.h>
#include <brogameagent/learn/gumbel.h>
#include <brogameagent/learn/trainer.h>
#include <brogameagent/learn/generic_replay_buffer.h>
#include <brogameagent/learn/generic_trainer.h>
#include <brogameagent/nn/net.h>
#include <brogameagent/nn/policy_value_net.h>

#include <cctype>
#include <cstring>
#include <memory>
#include <random>
#include <string>
#include <vector>

namespace bro::js {

namespace nn    = brogameagent::nn;
namespace mcts  = brogameagent::mcts;
namespace learn = brogameagent::learn;

// ─── Wrapper structs ───────────────────────────────────────────────────────
struct ReplayBufferData {
    std::shared_ptr<learn::ReplayBuffer> buf;
};

struct NeuralEvaluatorData {
    std::shared_ptr<learn::NeuralEvaluator> ev;
    std::shared_ptr<nn::SingleHeroNet>      netRef;   // keep alive
};

struct NeuralPriorData {
    std::shared_ptr<learn::NeuralPrior>     pr;
    std::shared_ptr<nn::SingleHeroNet>      netRef;
};

struct GumbelNoisePriorData {
    std::shared_ptr<learn::GumbelNoisePrior> pr;
    std::shared_ptr<mcts::IPrior>            innerRef;
};

struct ExItTrainerData {
    std::unique_ptr<learn::ExItTrainer>     trainer;
    std::shared_ptr<nn::SingleHeroNet>      netRef;
    std::shared_ptr<nn::WeightsHandle>      handleRef;
    std::shared_ptr<learn::ReplayBuffer>    bufRef;
};

struct GenericReplayBufferData {
    std::shared_ptr<learn::GenericReplayBuffer> buf;
};

struct GenericExItTrainerData {
    std::unique_ptr<learn::GenericExItTrainer>      trainer;
    std::shared_ptr<nn::PolicyValueNet>             netRef;
    std::shared_ptr<nn::WeightsHandle>              handleRef;
    std::shared_ptr<learn::GenericReplayBuffer>     bufRef;
};

// ─── Helpers ───────────────────────────────────────────────────────────────
static double getDouble(JSContext* ctx, JSValueConst obj, const char* k, double def) {
    JSValue v = JS_GetPropertyStr(ctx, obj, k);
    double out = def;
    if (JS_IsNumber(v)) JS_ToFloat64(ctx, &out, v);
    JS_FreeValue(ctx, v);
    return out;
}
static int32_t getInt(JSContext* ctx, JSValueConst obj, const char* k, int32_t def) {
    JSValue v = JS_GetPropertyStr(ctx, obj, k);
    int32_t out = def;
    if (JS_IsNumber(v)) JS_ToInt32(ctx, &out, v);
    JS_FreeValue(ctx, v);
    return out;
}

static JSValue makeFloat32ArrayCopy(JSContext* ctx, const float* data, int count) {
    size_t bytes = (size_t)count * sizeof(float);
    JSValue abuf = JS_NewArrayBufferCopy(ctx, (const uint8_t*)data, bytes);
    JSValue args[3] = { abuf, JS_UNDEFINED, JS_UNDEFINED };
    JSValue arr = JS_NewTypedArray(ctx, 1, args, JS_TYPED_ARRAY_FLOAT32);
    JS_FreeValue(ctx, abuf);
    return arr;
}

// Copy floats out of a typed-array property into a destination array.
// Returns true iff the source was a Float32Array (or compatible) with at least
// `count` elements.
static bool copyFloatProp(JSContext* ctx, JSValueConst obj, const char* key,
                          float* dst, int count) {
    JSValue v = JS_GetPropertyStr(ctx, obj, key);
    if (JS_IsUndefined(v) || JS_IsNull(v)) { JS_FreeValue(ctx, v); return false; }
    size_t byteOff = 0, viewLen = 0;
    JSValue abuf = JS_GetTypedArrayBuffer(ctx, v, &byteOff, &viewLen, nullptr);
    if (JS_IsException(abuf)) { JS_FreeValue(ctx, JS_GetException(ctx)); JS_FreeValue(ctx, v); return false; }
    size_t abufLen = 0;
    uint8_t* raw = JS_GetArrayBuffer(ctx, &abufLen, abuf);
    JS_FreeValue(ctx, abuf);
    JS_FreeValue(ctx, v);
    if (!raw) return false;
    int have = (int)(viewLen / sizeof(float));
    int copy = have < count ? have : count;
    std::memcpy(dst, raw + byteOff, (size_t)copy * sizeof(float));
    return copy == count;
}

// ─── Situation <-> JS object marshalling ──────────────────────────────────
static JSValue makeSituationObject(JSContext* ctx, const learn::Situation& s) {
    JSValue o = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, o, "obs",
        makeFloat32ArrayCopy(ctx, s.obs.data(), (int)s.obs.size()));
    JS_SetPropertyStr(ctx, o, "atkMask",
        makeFloat32ArrayCopy(ctx, s.atk_mask.data(), (int)s.atk_mask.size()));
    JS_SetPropertyStr(ctx, o, "abilMask",
        makeFloat32ArrayCopy(ctx, s.abil_mask.data(), (int)s.abil_mask.size()));
    JS_SetPropertyStr(ctx, o, "targetMove",
        makeFloat32ArrayCopy(ctx, s.target_move.data(), (int)s.target_move.size()));
    JS_SetPropertyStr(ctx, o, "targetAttack",
        makeFloat32ArrayCopy(ctx, s.target_attack.data(), (int)s.target_attack.size()));
    JS_SetPropertyStr(ctx, o, "targetAbility",
        makeFloat32ArrayCopy(ctx, s.target_ability.data(), (int)s.target_ability.size()));
    JS_SetPropertyStr(ctx, o, "valueTarget", JS_NewFloat64(ctx, s.value_target));
    return o;
}

static bool readSituation(JSContext* ctx, JSValueConst v, learn::Situation& out) {
    if (!JS_IsObject(v)) return false;
    copyFloatProp(ctx, v, "obs",           out.obs.data(),           (int)out.obs.size());
    copyFloatProp(ctx, v, "atkMask",       out.atk_mask.data(),      (int)out.atk_mask.size());
    copyFloatProp(ctx, v, "abilMask",      out.abil_mask.data(),     (int)out.abil_mask.size());
    copyFloatProp(ctx, v, "targetMove",    out.target_move.data(),   (int)out.target_move.size());
    copyFloatProp(ctx, v, "targetAttack",  out.target_attack.data(), (int)out.target_attack.size());
    copyFloatProp(ctx, v, "targetAbility", out.target_ability.data(),(int)out.target_ability.size());
    out.value_target = (float)getDouble(ctx, v, "valueTarget", 0.0);
    return true;
}

// ─── GenericSituation <-> JS object marshalling ───────────────────────────
//
// JS shape: { obs: Float32Array, policyTarget: Float32Array,
//             actionMask?: Float32Array, valueTarget: number }

static JSValue makeFloat32ArrayCopyVec(JSContext* ctx, const std::vector<float>& v) {
    return makeFloat32ArrayCopy(ctx, v.data(), (int)v.size());
}

static bool readFloatVec(JSContext* ctx, JSValueConst obj, const char* key,
                         std::vector<float>& out, bool required) {
    JSValue v = JS_GetPropertyStr(ctx, obj, key);
    if (JS_IsUndefined(v) || JS_IsNull(v)) {
        JS_FreeValue(ctx, v);
        if (required) return false;
        out.clear();
        return true;
    }
    size_t byteOff = 0, viewLen = 0;
    JSValue abuf = JS_GetTypedArrayBuffer(ctx, v, &byteOff, &viewLen, nullptr);
    if (JS_IsException(abuf)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        JS_FreeValue(ctx, v);
        return !required;
    }
    size_t abufLen = 0;
    uint8_t* raw = JS_GetArrayBuffer(ctx, &abufLen, abuf);
    JS_FreeValue(ctx, abuf);
    JS_FreeValue(ctx, v);
    if (!raw) return !required;
    int n = (int)(viewLen / sizeof(float));
    out.assign(reinterpret_cast<float*>(raw + byteOff),
               reinterpret_cast<float*>(raw + byteOff) + n);
    return true;
}

static JSValue makeGenericSituationObject(JSContext* ctx, const learn::GenericSituation& s) {
    JSValue o = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, o, "obs",          makeFloat32ArrayCopyVec(ctx, s.obs));
    JS_SetPropertyStr(ctx, o, "policyTarget", makeFloat32ArrayCopyVec(ctx, s.policy_target));
    JS_SetPropertyStr(ctx, o, "actionMask",   makeFloat32ArrayCopyVec(ctx, s.action_mask));
    JS_SetPropertyStr(ctx, o, "valueTarget",  JS_NewFloat64(ctx, s.value_target));
    return o;
}

static bool readGenericSituation(JSContext* ctx, JSValueConst v, learn::GenericSituation& out) {
    if (!JS_IsObject(v)) return false;
    if (!readFloatVec(ctx, v, "obs", out.obs, /*required*/ true)) return false;
    if (!readFloatVec(ctx, v, "policyTarget", out.policy_target, /*required*/ true)) return false;
    readFloatVec(ctx, v, "actionMask", out.action_mask, /*required*/ false);
    out.value_target = (float)getDouble(ctx, v, "valueTarget", 0.0);
    return true;
}

// ─── Extract shared_ptr<IPrior> from any wrapped prior JS value ────────────
std::shared_ptr<mcts::IPrior> extractPriorShared(JSContext* ctx, JSValueConst v) {
    if (auto* np = qjsbind::unwrap<NeuralPriorData>(ctx, v))     return np->pr;
    if (auto* gp = qjsbind::unwrap<GumbelNoisePriorData>(ctx, v)) return gp->pr;
    return {};
}

std::shared_ptr<mcts::IEvaluator> extractHeroEvaluatorShared(JSContext* ctx, JSValueConst v) {
    if (auto* ne = qjsbind::unwrap<NeuralEvaluatorData>(ctx, v)) return ne->ev;
    return {};
}

// ═══════════════════════════════════════════════════════════════════════════
// Class registration
// ═══════════════════════════════════════════════════════════════════════════

static void registerClasses(JSContext* ctx) {
    // ── ReplayBuffer ───────────────────────────────────────────────────────
    {
        qjsbind::Class<ReplayBufferData>(ctx, "AIReplayBuffer", qjsbind::NoGlobal)
            .get("size",     [](ReplayBufferData* d) -> int { return d->buf ? (int)d->buf->size() : 0; })
            .get("capacity", [](ReplayBufferData* d) -> int { return d->buf ? (int)d->buf->capacity() : 0; })
            .method_raw("push",
                [](JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) -> JSValue {
                    auto* d = qjsbind::unwrap<ReplayBufferData>(ctx, this_val);
                    if (!d || !d->buf || argc < 1) return JS_UNDEFINED;
                    learn::Situation s{};
                    if (!readSituation(ctx, argv[0], s))
                        return JS_ThrowTypeError(ctx, "push(situation): expected object");
                    d->buf->push(s);
                    return JS_UNDEFINED;
                }, 1)
            .method("clear", [](ReplayBufferData* d) { if (d->buf) d->buf->clear(); })
            .method_raw("sample",
                [](JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) -> JSValue {
                    auto* d = qjsbind::unwrap<ReplayBufferData>(ctx, this_val);
                    if (!d || !d->buf) return JS_NewArray(ctx);
                    int32_t n = 0;
                    if (argc >= 1) JS_ToInt32(ctx, &n, argv[0]);
                    if (n < 0) n = 0;
                    // Use a thread-local rng; sampling is cheap.
                    static thread_local std::mt19937_64 rng{std::random_device{}()};
                    auto batch = d->buf->sample((size_t)n, rng);
                    JSValue arr = JS_NewArray(ctx);
                    for (size_t i = 0; i < batch.size(); ++i)
                        JS_SetPropertyUint32(ctx, arr, (uint32_t)i, makeSituationObject(ctx, batch[i]));
                    return arr;
                }, 1)
            .method("all",
                [](ReplayBufferData* d, JSContext* ctx) -> JSValue {
                    JSValue arr = JS_NewArray(ctx);
                    if (!d->buf) return arr;
                    const auto& v = d->buf->all();
                    for (size_t i = 0; i < v.size(); ++i)
                        JS_SetPropertyUint32(ctx, arr, (uint32_t)i, makeSituationObject(ctx, v[i]));
                    return arr;
                });
    }

    // ── NeuralEvaluator ────────────────────────────────────────────────────
    {
        qjsbind::Class<NeuralEvaluatorData>(ctx, "AINeuralEvaluator", qjsbind::NoGlobal)
            .method("evaluate",
                [](NeuralEvaluatorData* d, JSContext* ctx, JSValueConst worldV, int heroId) -> JSValue {
                    auto* w = worldFromJS(ctx, worldV);
                    if (!w || !d->ev) return JS_NewFloat64(ctx, 0.0);
                    return JS_NewFloat64(ctx, d->ev->evaluate(*w, heroId));
                });
    }

    // ── NeuralPrior ────────────────────────────────────────────────────────
    {
        qjsbind::Class<NeuralPriorData>(ctx, "AINeuralPrior", qjsbind::NoGlobal)
            .method("setTemperature", [](NeuralPriorData* d, double t) { if (d->pr) d->pr->set_temperature((float)t); })
            .method("setUniformMix",  [](NeuralPriorData* d, double m) { if (d->pr) d->pr->set_uniform_mix((float)m); });
    }

    // ── GumbelNoisePrior ──────────────────────────────────────────────────
    {
        qjsbind::Class<GumbelNoisePriorData>(ctx, "AIGumbelNoisePrior", qjsbind::NoGlobal)
            .method_raw("reseed",
                [](JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) -> JSValue {
                    auto* d = qjsbind::unwrap<GumbelNoisePriorData>(ctx, this_val);
                    if (!d || !d->pr || argc < 1) return JS_UNDEFINED;
                    int64_t s = 0;
                    if (JS_ToBigInt64(ctx, &s, argv[0]) != 0) {
                        double ds = 0; JS_ToFloat64(ctx, &ds, argv[0]); s = (int64_t)ds;
                    }
                    d->pr->reseed((uint64_t)s);
                    return JS_UNDEFINED;
                }, 1)
            .method("setScale", [](GumbelNoisePriorData* d, double s) { if (d->pr) d->pr->set_scale((float)s); });
    }

    // ── ExItTrainer ────────────────────────────────────────────────────────
    {
        qjsbind::Class<ExItTrainerData>(ctx, "AIExItTrainer", qjsbind::NoGlobal)
            .get("totalSteps",     [](ExItTrainerData* d) -> int { return d->trainer ? d->trainer->total_steps() : 0; })
            .get("totalPublishes", [](ExItTrainerData* d) -> int { return d->trainer ? d->trainer->total_publishes() : 0; })
            .method_raw("setNet",
                [](JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) -> JSValue {
                    auto* d = qjsbind::unwrap<ExItTrainerData>(ctx, this_val);
                    if (!d || !d->trainer || argc < 1) return JS_UNDEFINED;
                    auto* net = nnSingleHeroNetFromJS(ctx, argv[0]);
                    if (!net) return JS_ThrowTypeError(ctx, "setNet(net): expected SingleHeroNet");
                    d->trainer->set_net(net);
                    JS_SetPropertyStr(ctx, this_val, "__net", JS_DupValue(ctx, argv[0]));
                    return JS_UNDEFINED;
                }, 1)
            .method_raw("setBuffer",
                [](JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) -> JSValue {
                    auto* d = qjsbind::unwrap<ExItTrainerData>(ctx, this_val);
                    if (!d || !d->trainer || argc < 1) return JS_UNDEFINED;
                    auto* bd = qjsbind::unwrap<ReplayBufferData>(ctx, argv[0]);
                    if (!bd || !bd->buf) return JS_ThrowTypeError(ctx, "setBuffer(buf): expected ReplayBuffer");
                    d->bufRef = bd->buf;
                    d->trainer->set_buffer(bd->buf.get());
                    JS_SetPropertyStr(ctx, this_val, "__buf", JS_DupValue(ctx, argv[0]));
                    return JS_UNDEFINED;
                }, 1)
            .method_raw("setWeightsHandle",
                [](JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) -> JSValue {
                    auto* d = qjsbind::unwrap<ExItTrainerData>(ctx, this_val);
                    if (!d || !d->trainer || argc < 1) return JS_UNDEFINED;
                    auto* h = nnWeightsHandleFromJS(ctx, argv[0]);
                    if (!h) return JS_ThrowTypeError(ctx, "setWeightsHandle(handle): expected WeightsHandle");
                    d->trainer->set_weights_handle(h);
                    JS_SetPropertyStr(ctx, this_val, "__handle", JS_DupValue(ctx, argv[0]));
                    return JS_UNDEFINED;
                }, 1)
            .method("setConfig",
                [](ExItTrainerData* d, JSContext* ctx, JSValueConst cfgV) {
                    if (!d->trainer || !JS_IsObject(cfgV)) return;
                    learn::TrainerConfig c = d->trainer->config();
                    c.lr            = (float)getDouble(ctx, cfgV, "lr", c.lr);
                    c.momentum      = (float)getDouble(ctx, cfgV, "momentum", c.momentum);
                    c.batch         = getInt(ctx, cfgV, "batch", c.batch);
                    c.policy_weight = (float)getDouble(ctx, cfgV, "policyWeight", c.policy_weight);
                    c.value_weight  = (float)getDouble(ctx, cfgV, "valueWeight", c.value_weight);
                    c.publish_every = getInt(ctx, cfgV, "publishEvery", c.publish_every);
                    JSValue sv = JS_GetPropertyStr(ctx, cfgV, "rngSeed");
                    if (!JS_IsUndefined(sv) && !JS_IsNull(sv)) {
                        int64_t s = 0;
                        if (JS_ToBigInt64(ctx, &s, sv) != 0) {
                            double ds = 0; JS_ToFloat64(ctx, &ds, sv); s = (int64_t)ds;
                        }
                        c.rng_seed = (uint64_t)s;
                    }
                    JS_FreeValue(ctx, sv);
                    d->trainer->set_config(c);
                })
            .method("step",
                [](ExItTrainerData* d, JSContext* ctx) -> JSValue {
                    JSValue obj = JS_NewObject(ctx);
                    if (!d->trainer) return obj;
                    auto s = d->trainer->step();
                    JS_SetPropertyStr(ctx, obj, "lossValue",  JS_NewFloat64(ctx, s.loss_value));
                    JS_SetPropertyStr(ctx, obj, "lossPolicy", JS_NewFloat64(ctx, s.loss_policy));
                    JS_SetPropertyStr(ctx, obj, "lossTotal",  JS_NewFloat64(ctx, s.loss_total));
                    JS_SetPropertyStr(ctx, obj, "samples",    JS_NewInt32(ctx, s.samples));
                    return obj;
                })
            .method("stepN",
                [](ExItTrainerData* d, JSContext* ctx, int n) -> JSValue {
                    JSValue obj = JS_NewObject(ctx);
                    if (!d->trainer) return obj;
                    auto s = d->trainer->step_n(n);
                    JS_SetPropertyStr(ctx, obj, "lossValue",  JS_NewFloat64(ctx, s.loss_value));
                    JS_SetPropertyStr(ctx, obj, "lossPolicy", JS_NewFloat64(ctx, s.loss_policy));
                    JS_SetPropertyStr(ctx, obj, "lossTotal",  JS_NewFloat64(ctx, s.loss_total));
                    JS_SetPropertyStr(ctx, obj, "samples",    JS_NewInt32(ctx, s.samples));
                    return obj;
                });
    }

    // ── GenericReplayBuffer ────────────────────────────────────────────────
    {
        qjsbind::Class<GenericReplayBufferData>(ctx, "AIGenericReplayBuffer", qjsbind::NoGlobal)
            .get("size",     [](GenericReplayBufferData* d) -> int { return d->buf ? (int)d->buf->size() : 0; })
            .get("capacity", [](GenericReplayBufferData* d) -> int { return d->buf ? (int)d->buf->capacity() : 0; })
            .method_raw("push",
                [](JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) -> JSValue {
                    auto* d = qjsbind::unwrap<GenericReplayBufferData>(ctx, this_val);
                    if (!d || !d->buf || argc < 1) return JS_UNDEFINED;
                    learn::GenericSituation s{};
                    if (!readGenericSituation(ctx, argv[0], s))
                        return JS_ThrowTypeError(ctx, "push(situation): expected {obs, policyTarget, valueTarget, actionMask?}");
                    d->buf->push(std::move(s));
                    return JS_UNDEFINED;
                }, 1)
            .method("clear", [](GenericReplayBufferData* d) { if (d->buf) d->buf->clear(); })
            .method_raw("sample",
                [](JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) -> JSValue {
                    auto* d = qjsbind::unwrap<GenericReplayBufferData>(ctx, this_val);
                    if (!d || !d->buf) return JS_NewArray(ctx);
                    int32_t n = 0;
                    if (argc >= 1) JS_ToInt32(ctx, &n, argv[0]);
                    if (n < 0) n = 0;
                    static thread_local std::mt19937_64 rng{std::random_device{}()};
                    auto batch = d->buf->sample((size_t)n, rng);
                    JSValue arr = JS_NewArray(ctx);
                    for (size_t i = 0; i < batch.size(); ++i)
                        JS_SetPropertyUint32(ctx, arr, (uint32_t)i, makeGenericSituationObject(ctx, batch[i]));
                    return arr;
                }, 1)
            .method("all",
                [](GenericReplayBufferData* d, JSContext* ctx) -> JSValue {
                    JSValue arr = JS_NewArray(ctx);
                    if (!d->buf) return arr;
                    const auto& v = d->buf->all();
                    for (size_t i = 0; i < v.size(); ++i)
                        JS_SetPropertyUint32(ctx, arr, (uint32_t)i, makeGenericSituationObject(ctx, v[i]));
                    return arr;
                });
    }

    // ── GenericExItTrainer ─────────────────────────────────────────────────
    {
        qjsbind::Class<GenericExItTrainerData>(ctx, "AIGenericExItTrainer", qjsbind::NoGlobal)
            .get("totalSteps",     [](GenericExItTrainerData* d) -> int { return d->trainer ? d->trainer->total_steps() : 0; })
            .get("totalPublishes", [](GenericExItTrainerData* d) -> int { return d->trainer ? d->trainer->total_publishes() : 0; })
            .method_raw("setNet",
                [](JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) -> JSValue {
                    auto* d = qjsbind::unwrap<GenericExItTrainerData>(ctx, this_val);
                    if (!d || !d->trainer || argc < 1) return JS_UNDEFINED;
                    auto netShared = nnPolicyValueNetSharedFromJS(ctx, argv[0]);
                    if (!netShared) return JS_ThrowTypeError(ctx, "setNet(net): expected PolicyValueNet");
                    d->netRef = netShared;
                    d->trainer->set_net(netShared.get());
                    JS_SetPropertyStr(ctx, this_val, "__net", JS_DupValue(ctx, argv[0]));
                    return JS_UNDEFINED;
                }, 1)
            .method_raw("setBuffer",
                [](JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) -> JSValue {
                    auto* d = qjsbind::unwrap<GenericExItTrainerData>(ctx, this_val);
                    if (!d || !d->trainer || argc < 1) return JS_UNDEFINED;
                    auto* bd = qjsbind::unwrap<GenericReplayBufferData>(ctx, argv[0]);
                    if (!bd || !bd->buf) return JS_ThrowTypeError(ctx, "setBuffer(buf): expected GenericReplayBuffer");
                    d->bufRef = bd->buf;
                    d->trainer->set_buffer(bd->buf.get());
                    JS_SetPropertyStr(ctx, this_val, "__buf", JS_DupValue(ctx, argv[0]));
                    return JS_UNDEFINED;
                }, 1)
            .method_raw("setWeightsHandle",
                [](JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) -> JSValue {
                    auto* d = qjsbind::unwrap<GenericExItTrainerData>(ctx, this_val);
                    if (!d || !d->trainer || argc < 1) return JS_UNDEFINED;
                    auto* h = nnWeightsHandleFromJS(ctx, argv[0]);
                    if (!h) return JS_ThrowTypeError(ctx, "setWeightsHandle(handle): expected WeightsHandle");
                    d->trainer->set_weights_handle(h);
                    JS_SetPropertyStr(ctx, this_val, "__handle", JS_DupValue(ctx, argv[0]));
                    return JS_UNDEFINED;
                }, 1)
            .method("setConfig",
                [](GenericExItTrainerData* d, JSContext* ctx, JSValueConst cfgV) {
                    if (!d->trainer || !JS_IsObject(cfgV)) return;
                    learn::GenericTrainerConfig c = d->trainer->config();
                    c.lr            = (float)getDouble(ctx, cfgV, "lr", c.lr);
                    c.momentum      = (float)getDouble(ctx, cfgV, "momentum", c.momentum);
                    c.batch         = getInt(ctx, cfgV, "batch", c.batch);
                    c.policy_weight = (float)getDouble(ctx, cfgV, "policyWeight", c.policy_weight);
                    c.value_weight  = (float)getDouble(ctx, cfgV, "valueWeight", c.value_weight);
                    c.publish_every = getInt(ctx, cfgV, "publishEvery", c.publish_every);
                    JSValue sv = JS_GetPropertyStr(ctx, cfgV, "rngSeed");
                    if (!JS_IsUndefined(sv) && !JS_IsNull(sv)) {
                        int64_t s = 0;
                        if (JS_ToBigInt64(ctx, &s, sv) != 0) {
                            double ds = 0; JS_ToFloat64(ctx, &ds, sv); s = (int64_t)ds;
                        }
                        c.rng_seed = (uint64_t)s;
                    }
                    JS_FreeValue(ctx, sv);
                    // Where compute happens. "gpu" requires net.to('gpu') first.
                    JSValue dv = JS_GetPropertyStr(ctx, cfgV, "device");
                    if (JS_IsString(dv)) {
                        const char* s = JS_ToCString(ctx, dv);
                        std::string dev = s ? s : "";
                        if (s) JS_FreeCString(ctx, s);
                        for (auto& ch : dev) ch = (char)std::tolower((unsigned char)ch);
                        c.device = (dev == "gpu") ? brogameagent::nn::Device::GPU
                                                  : brogameagent::nn::Device::CPU;
                    }
                    JS_FreeValue(ctx, dv);
                    d->trainer->set_config(c);
                })
            .method("step",
                [](GenericExItTrainerData* d, JSContext* ctx) -> JSValue {
                    JSValue obj = JS_NewObject(ctx);
                    if (!d->trainer) return obj;
                    auto s = d->trainer->step();
                    JS_SetPropertyStr(ctx, obj, "lossValue",  JS_NewFloat64(ctx, s.loss_value));
                    JS_SetPropertyStr(ctx, obj, "lossPolicy", JS_NewFloat64(ctx, s.loss_policy));
                    JS_SetPropertyStr(ctx, obj, "lossTotal",  JS_NewFloat64(ctx, s.loss_total));
                    JS_SetPropertyStr(ctx, obj, "samples",    JS_NewInt32(ctx, s.samples));
                    return obj;
                })
            .method("stepN",
                [](GenericExItTrainerData* d, JSContext* ctx, int n) -> JSValue {
                    JSValue obj = JS_NewObject(ctx);
                    if (!d->trainer) return obj;
                    auto s = d->trainer->step_n(n);
                    JS_SetPropertyStr(ctx, obj, "lossValue",  JS_NewFloat64(ctx, s.loss_value));
                    JS_SetPropertyStr(ctx, obj, "lossPolicy", JS_NewFloat64(ctx, s.loss_policy));
                    JS_SetPropertyStr(ctx, obj, "lossTotal",  JS_NewFloat64(ctx, s.loss_total));
                    JS_SetPropertyStr(ctx, obj, "samples",    JS_NewInt32(ctx, s.samples));
                    return obj;
                });
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// Factory functions
// ═══════════════════════════════════════════════════════════════════════════

static JSValue js_createReplayBuffer(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    size_t cap = 4096;
    if (argc >= 1) {
        int32_t v = 0; JS_ToInt32(ctx, &v, argv[0]);
        if (v > 0) cap = (size_t)v;
    }
    auto* d = new ReplayBufferData{ std::make_shared<learn::ReplayBuffer>(cap) };
    return qjsbind::wrap<ReplayBufferData>(ctx, d);
}

static JSValue js_createNeuralEvaluator(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 1) return JS_ThrowTypeError(ctx, "createNeuralEvaluator(net, handle?)");
    auto net = nnSingleHeroNetSharedFromJS(ctx, argv[0]);
    if (!net) return JS_ThrowTypeError(ctx, "net must be a SingleHeroNet");
    nn::WeightsHandle* handle = nullptr;
    if (argc >= 2 && !JS_IsUndefined(argv[1]) && !JS_IsNull(argv[1])) {
        handle = nnWeightsHandleFromJS(ctx, argv[1]);
    }
    auto ev = std::make_shared<learn::NeuralEvaluator>(net, handle);
    auto* d = new NeuralEvaluatorData{ ev, net };
    JSValue obj = qjsbind::wrap<NeuralEvaluatorData>(ctx, d);
    // Hold JS ref to net (and handle if provided) so they live as long as this evaluator.
    JS_SetPropertyStr(ctx, obj, "__net", JS_DupValue(ctx, argv[0]));
    if (argc >= 2) JS_SetPropertyStr(ctx, obj, "__handle", JS_DupValue(ctx, argv[1]));
    return obj;
}

static JSValue js_createNeuralPrior(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 1) return JS_ThrowTypeError(ctx, "createNeuralPrior(net, handle?)");
    auto net = nnSingleHeroNetSharedFromJS(ctx, argv[0]);
    if (!net) return JS_ThrowTypeError(ctx, "net must be a SingleHeroNet");
    nn::WeightsHandle* handle = nullptr;
    if (argc >= 2 && !JS_IsUndefined(argv[1]) && !JS_IsNull(argv[1])) {
        handle = nnWeightsHandleFromJS(ctx, argv[1]);
    }
    auto pr = std::make_shared<learn::NeuralPrior>(net, handle);
    auto* d = new NeuralPriorData{ pr, net };
    JSValue obj = qjsbind::wrap<NeuralPriorData>(ctx, d);
    JS_SetPropertyStr(ctx, obj, "__net", JS_DupValue(ctx, argv[0]));
    if (argc >= 2) JS_SetPropertyStr(ctx, obj, "__handle", JS_DupValue(ctx, argv[1]));
    return obj;
}

static JSValue js_createGumbelNoisePrior(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 1) return JS_ThrowTypeError(ctx, "createGumbelNoisePrior(innerPrior, scale?)");
    auto inner = extractPriorShared(ctx, argv[0]);
    if (!inner) return JS_ThrowTypeError(ctx, "innerPrior must be a bound prior");
    float scale = 1.0f;
    if (argc >= 2) { double s = 0; JS_ToFloat64(ctx, &s, argv[1]); scale = (float)s; }
    auto pr = std::make_shared<learn::GumbelNoisePrior>(inner, scale);
    auto* d = new GumbelNoisePriorData{ pr, inner };
    JSValue obj = qjsbind::wrap<GumbelNoisePriorData>(ctx, d);
    JS_SetPropertyStr(ctx, obj, "__inner", JS_DupValue(ctx, argv[0]));
    return obj;
}

static JSValue js_createExItTrainer(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    auto* d = new ExItTrainerData();
    d->trainer = std::make_unique<learn::ExItTrainer>();
    return qjsbind::wrap<ExItTrainerData>(ctx, d);
}

static JSValue js_createGenericReplayBuffer(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    size_t cap = 4096;
    if (argc >= 1) {
        int32_t v = 0; JS_ToInt32(ctx, &v, argv[0]);
        if (v > 0) cap = (size_t)v;
    }
    auto* d = new GenericReplayBufferData{ std::make_shared<learn::GenericReplayBuffer>(cap) };
    return qjsbind::wrap<GenericReplayBufferData>(ctx, d);
}

static JSValue js_createGenericExItTrainer(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    auto* d = new GenericExItTrainerData();
    d->trainer = std::make_unique<learn::GenericExItTrainer>();
    return qjsbind::wrap<GenericExItTrainerData>(ctx, d);
}

// ─── Free functions ────────────────────────────────────────────────────────

static JSValue js_targetsFromMcts(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 1) return JS_ThrowTypeError(ctx, "targetsFromMcts(mcts)");
    auto* m = mctsFromJS(ctx, argv[0]);
    if (!m) return JS_ThrowTypeError(ctx, "expected Mcts");
    const mcts::Node* root = m->last_root();
    if (!root) return JS_NULL;

    float tm[nn::FactoredPolicyHead::N_MOVE] = {};
    float ta[nn::FactoredPolicyHead::N_ATTACK] = {};
    float tb[nn::FactoredPolicyHead::N_ABILITY] = {};
    learn::targets_from_root(*root, tm, ta, tb);
    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "move",    makeFloat32ArrayCopy(ctx, tm, nn::FactoredPolicyHead::N_MOVE));
    JS_SetPropertyStr(ctx, obj, "attack",  makeFloat32ArrayCopy(ctx, ta, nn::FactoredPolicyHead::N_ATTACK));
    JS_SetPropertyStr(ctx, obj, "ability", makeFloat32ArrayCopy(ctx, tb, nn::FactoredPolicyHead::N_ABILITY));
    return obj;
}

static JSValue js_makeSituation(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 3) return JS_ThrowTypeError(ctx, "makeSituation(mcts, hero, world)");
    auto* m  = mctsFromJS(ctx, argv[0]);
    auto* a  = agentFromJS(ctx, argv[1]);
    auto* w  = worldFromJS(ctx, argv[2]);
    if (!m || !a || !w) return JS_ThrowTypeError(ctx, "bad args");
    const mcts::Node* root = m->last_root();
    if (!root) return JS_NULL;
    auto s = learn::make_situation(*w, *a, *root);
    return makeSituationObject(ctx, s);
}

static JSValue js_gumbelImprovedPolicy(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 1) return JS_ThrowTypeError(ctx, "gumbelImprovedPolicy(mcts)");
    auto* m = mctsFromJS(ctx, argv[0]);
    if (!m) return JS_ThrowTypeError(ctx, "expected Mcts");
    const mcts::Node* root = m->last_root();
    if (!root) return JS_NULL;
    float tm[9] = {};
    float ta[6] = {};  // N_ENEMY_SLOTS + 1
    float tb[9] = {};  // N_ABILITY_SLOTS + 1 (MAX_ABILITIES=8)
    learn::gumbel_improved_policy(*root, tm, ta, tb);
    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "move",    makeFloat32ArrayCopy(ctx, tm, 9));
    JS_SetPropertyStr(ctx, obj, "attack",  makeFloat32ArrayCopy(ctx, ta, 6));
    JS_SetPropertyStr(ctx, obj, "ability", makeFloat32ArrayCopy(ctx, tb, 9));
    return obj;
}

// ═══════════════════════════════════════════════════════════════════════════
// Install
// ═══════════════════════════════════════════════════════════════════════════

void installLearnBindings(JSContext* ctx, JSValue gameObj) {
    registerClasses(ctx);

    JSValue learnObj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, learnObj, "createReplayBuffer",
        JS_NewCFunction(ctx, js_createReplayBuffer, "createReplayBuffer", 1));
    JS_SetPropertyStr(ctx, learnObj, "createNeuralEvaluator",
        JS_NewCFunction(ctx, js_createNeuralEvaluator, "createNeuralEvaluator", 2));
    JS_SetPropertyStr(ctx, learnObj, "createNeuralPrior",
        JS_NewCFunction(ctx, js_createNeuralPrior, "createNeuralPrior", 2));
    JS_SetPropertyStr(ctx, learnObj, "createGumbelNoisePrior",
        JS_NewCFunction(ctx, js_createGumbelNoisePrior, "createGumbelNoisePrior", 2));
    JS_SetPropertyStr(ctx, learnObj, "createExItTrainer",
        JS_NewCFunction(ctx, js_createExItTrainer, "createExItTrainer", 0));
    JS_SetPropertyStr(ctx, learnObj, "createGenericReplayBuffer",
        JS_NewCFunction(ctx, js_createGenericReplayBuffer, "createGenericReplayBuffer", 1));
    JS_SetPropertyStr(ctx, learnObj, "createGenericExItTrainer",
        JS_NewCFunction(ctx, js_createGenericExItTrainer, "createGenericExItTrainer", 0));
    JS_SetPropertyStr(ctx, learnObj, "targetsFromMcts",
        JS_NewCFunction(ctx, js_targetsFromMcts, "targetsFromMcts", 1));
    JS_SetPropertyStr(ctx, learnObj, "makeSituation",
        JS_NewCFunction(ctx, js_makeSituation, "makeSituation", 3));
    JS_SetPropertyStr(ctx, learnObj, "gumbelImprovedPolicy",
        JS_NewCFunction(ctx, js_gumbelImprovedPolicy, "gumbelImprovedPolicy", 1));

    JS_SetPropertyStr(ctx, gameObj, "learn", learnObj);
}

} // namespace bro::js
