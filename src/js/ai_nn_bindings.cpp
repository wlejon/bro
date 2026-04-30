// JS bindings for brogameagent::nn — the neural network subsystem.
//
// Installed onto bro.ai.game.nn. Provides:
//   - Tensor (owned float buffer)
//   - Circuits: Linear, Relu, Tanh, DeepSetsEncoder, ValueHead, FactoredPolicyHead
//   - SingleHeroNet (composed net used by the learn adapters)
//   - WeightsHandle (hot-swap weights between trainer and evaluator)
//   - Primitive ops (linear/relu/tanh/softmax/xent forwards & backwards, xavier init)
//
// Call installNNBindings(ctx, gameObj) from ai_bindings.cpp after gameObj is built.

#include "js/ai_bindings.h"

#include <qjsbind/qjsbind.h>
#include <brogameagent/nn/tensor.h>
#include <brogameagent/nn/ops.h>
#include <brogameagent/nn/circuits.h>
#include <brogameagent/nn/encoder.h>
#include <brogameagent/nn/heads.h>
#include <brogameagent/nn/net.h>
#include <brogameagent/nn/policy_value_net.h>
#include <brogameagent/nn/factored.h>

#include <cctype>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace bro::js {

namespace nn = brogameagent::nn;

// ─── Wrapper structs ───────────────────────────────────────────────────────
struct TensorData                { nn::Tensor t; };
struct LinearData                { nn::Linear l; };
struct ReluData                  { nn::Relu r; };
struct TanhData                  { nn::Tanh t; };
struct DeepSetsEncoderData       { nn::DeepSetsEncoder e; };
struct ValueHeadData             { nn::ValueHead v; };
struct FactoredPolicyHeadData    { nn::FactoredPolicyHead h; };
struct SingleHeroNetData         { std::shared_ptr<nn::SingleHeroNet> net; };
struct PolicyValueNetData        { std::shared_ptr<nn::PolicyValueNet> net; };
struct WeightsHandleData         { std::shared_ptr<nn::WeightsHandle> handle; };

// ─── Small helpers ─────────────────────────────────────────────────────────
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
static uint64_t readSeed(JSContext* ctx, JSValueConst v, uint64_t def) {
    if (JS_IsUndefined(v) || JS_IsNull(v)) return def;
    int64_t s = 0;
    if (JS_ToBigInt64(ctx, &s, v) == 0) return (uint64_t)s;
    double d = 0;
    if (JS_ToFloat64(ctx, &d, v) == 0) return (uint64_t)(int64_t)d;
    return def;
}

// Pull a raw float* from a Float32Array or ArrayBuffer; returns nullptr on mismatch.
static float* getFloatArrayPtr(JSContext* ctx, JSValueConst arr, size_t& outCount) {
    if (JS_IsUndefined(arr) || JS_IsNull(arr)) { outCount = 0; return nullptr; }
    size_t byteOff = 0, viewLen = 0;
    JSValue abuf = JS_GetTypedArrayBuffer(ctx, arr, &byteOff, &viewLen, nullptr);
    if (JS_IsException(abuf)) { JS_FreeValue(ctx, JS_GetException(ctx)); outCount = 0; return nullptr; }
    size_t abufLen = 0;
    uint8_t* ptr = JS_GetArrayBuffer(ctx, &abufLen, abuf);
    JS_FreeValue(ctx, abuf);
    if (!ptr) { outCount = 0; return nullptr; }
    outCount = viewLen / sizeof(float);
    return reinterpret_cast<float*>(ptr + byteOff);
}

static JSValue makeFloat32ArrayCopy(JSContext* ctx, const float* data, int count) {
    size_t bytes = (size_t)count * sizeof(float);
    JSValue abuf = JS_NewArrayBufferCopy(ctx, (const uint8_t*)data, bytes);
    JSValue args[3] = { abuf, JS_UNDEFINED, JS_UNDEFINED };
    JSValue arr = JS_NewTypedArray(ctx, 1, args, JS_TYPED_ARRAY_FLOAT32);
    JS_FreeValue(ctx, abuf);
    return arr;
}

static JSValue makeUint8ArrayCopy(JSContext* ctx, const uint8_t* data, size_t count) {
    JSValue abuf = JS_NewArrayBufferCopy(ctx, data, count);
    JSValue args[3] = { abuf, JS_UNDEFINED, JS_UNDEFINED };
    JSValue arr = JS_NewTypedArray(ctx, 1, args, JS_TYPED_ARRAY_UINT8);
    JS_FreeValue(ctx, abuf);
    return arr;
}

// Unwrap TensorData* from JS value (returns nullptr on mismatch).
static nn::Tensor* tensorFromJS(JSContext* ctx, JSValueConst v) {
    auto* d = qjsbind::unwrap<TensorData>(ctx, v);
    return d ? &d->t : nullptr;
}

// ═══════════════════════════════════════════════════════════════════════════
// Class registration
// ═══════════════════════════════════════════════════════════════════════════

static void registerClasses(JSContext* ctx) {
    // ── Tensor ────────────────────────────────────────────────────────────
    {
        qjsbind::Class<TensorData>(ctx, "AITensor", qjsbind::NoGlobal)
            .get("rows", [](TensorData* d) -> int { return d->t.rows; })
            .get("cols", [](TensorData* d) -> int { return d->t.cols; })
            .get("size", [](TensorData* d) -> int { return d->t.size(); })
            .method("zero", [](TensorData* d) { d->t.zero(); })
            .method("resize", [](TensorData* d, int r, int c) { d->t.resize(r, c); })
            .method("get",
                [](TensorData* d, int r, int c) -> double {
                    if (r < 0 || r >= d->t.rows || c < 0 || c >= d->t.cols) return 0.0;
                    return d->t(r, c);
                })
            .method("set",
                [](TensorData* d, int r, int c, double v) {
                    if (r < 0 || r >= d->t.rows || c < 0 || c >= d->t.cols) return;
                    d->t(r, c) = (float)v;
                })
            .method("toArray",
                [](TensorData* d, JSContext* ctx) -> JSValue {
                    return makeFloat32ArrayCopy(ctx, d->t.ptr(), d->t.size());
                })
            .method_raw("fromArray",
                [](JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) -> JSValue {
                    auto* d = qjsbind::unwrap<TensorData>(ctx, this_val);
                    if (!d || argc < 1) return JS_UNDEFINED;
                    size_t n = 0;
                    float* src = getFloatArrayPtr(ctx, argv[0], n);
                    if (!src) return JS_ThrowTypeError(ctx, "expected Float32Array");
                    int copy = (int)std::min<size_t>(n, (size_t)d->t.size());
                    std::memcpy(d->t.ptr(), src, (size_t)copy * sizeof(float));
                    return JS_UNDEFINED;
                }, 1)
            .method_raw("copyFrom",
                [](JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) -> JSValue {
                    auto* d = qjsbind::unwrap<TensorData>(ctx, this_val);
                    if (!d || argc < 1) return JS_UNDEFINED;
                    auto* src = qjsbind::unwrap<TensorData>(ctx, argv[0]);
                    if (!src) return JS_ThrowTypeError(ctx, "expected Tensor");
                    d->t = src->t;
                    return JS_UNDEFINED;
                }, 1);
    }

    // ── Linear ─────────────────────────────────────────────────────────────
    {
        qjsbind::Class<LinearData>(ctx, "AILinear", qjsbind::NoGlobal)
            .get("name",      [](LinearData* d) -> std::string { return d->l.name(); })
            .get("inDim",     [](LinearData* d) -> int { return d->l.in_dim(); })
            .get("outDim",    [](LinearData* d) -> int { return d->l.out_dim(); })
            .get("numParams", [](LinearData* d) -> int { return d->l.num_params(); })
            .method("init",
                [](LinearData* d, JSContext* ctx, int inDim, int outDim, JSValueConst seedV) -> JSValue {
                    uint64_t s = readSeed(ctx, seedV, 0xC0DE1234ULL);
                    d->l.init(inDim, outDim, s);
                    return JS_NewBigInt64(ctx, (int64_t)s);
                })
            .method("forward",
                [](LinearData* d, JSContext* ctx, JSValueConst x, JSValueConst y) -> JSValue {
                    auto* xt = tensorFromJS(ctx, x);
                    auto* yt = tensorFromJS(ctx, y);
                    if (!xt || !yt) return JS_ThrowTypeError(ctx, "Linear.forward(x,y) expects Tensors");
                    d->l.forward(*xt, *yt);
                    return JS_UNDEFINED;
                })
            .method("backward",
                [](LinearData* d, JSContext* ctx, JSValueConst dY, JSValueConst dX) -> JSValue {
                    auto* dYt = tensorFromJS(ctx, dY);
                    auto* dXt = tensorFromJS(ctx, dX);
                    if (!dYt || !dXt) return JS_ThrowTypeError(ctx, "Linear.backward(dY,dX) expects Tensors");
                    d->l.backward(*dYt, *dXt);
                    return JS_UNDEFINED;
                })
            .method("zeroGrad",    [](LinearData* d) { d->l.zero_grad(); })
            .method("sgdStep",     [](LinearData* d, double lr, double m) { d->l.sgd_step((float)lr, (float)m); })
            .method("save",
                [](LinearData* d, JSContext* ctx) -> JSValue {
                    std::vector<uint8_t> bytes;
                    d->l.save_to(bytes);
                    return makeUint8ArrayCopy(ctx, bytes.data(), bytes.size());
                })
            .method_raw("load",
                [](JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) -> JSValue {
                    auto* d = qjsbind::unwrap<LinearData>(ctx, this_val);
                    if (!d || argc < 1) return JS_UNDEFINED;
                    size_t len = 0;
                    uint8_t* ptr = (uint8_t*)getFloatArrayPtr(ctx, argv[0], len);
                    // getFloatArrayPtr treats lengths in floats; redo as raw bytes.
                    size_t byteOff = 0, viewLen = 0;
                    JSValue abuf = JS_GetTypedArrayBuffer(ctx, argv[0], &byteOff, &viewLen, nullptr);
                    if (JS_IsException(abuf)) { JS_FreeValue(ctx, JS_GetException(ctx)); return JS_ThrowTypeError(ctx, "expected TypedArray"); }
                    size_t abufLen = 0;
                    uint8_t* raw = JS_GetArrayBuffer(ctx, &abufLen, abuf);
                    JS_FreeValue(ctx, abuf);
                    if (!raw) return JS_ThrowTypeError(ctx, "bad buffer");
                    size_t off = 0;
                    d->l.load_from(raw + byteOff, off, viewLen);
                    (void)ptr; (void)len;
                    return JS_UNDEFINED;
                }, 1)
            // Parameter tensors — returned as unowned views sharing this LinearData's lifetime.
            .get("W",  [](LinearData* d, JSContext* ctx) -> JSValue {
                auto* t = new TensorData{ d->l.W() };  // copy view; users mutate via returned tensor
                return qjsbind::wrap<TensorData>(ctx, t);
            })
            .get("b",  [](LinearData* d, JSContext* ctx) -> JSValue {
                auto* t = new TensorData{ d->l.b() };
                return qjsbind::wrap<TensorData>(ctx, t);
            })
            .get("dW", [](LinearData* d, JSContext* ctx) -> JSValue {
                auto* t = new TensorData{ d->l.dW() };
                return qjsbind::wrap<TensorData>(ctx, t);
            })
            .get("dB", [](LinearData* d, JSContext* ctx) -> JSValue {
                auto* t = new TensorData{ d->l.dB() };
                return qjsbind::wrap<TensorData>(ctx, t);
            });
    }

    // ── Relu / Tanh ────────────────────────────────────────────────────────
    {
        qjsbind::Class<ReluData>(ctx, "AIRelu", qjsbind::NoGlobal)
            .get("name",      [](ReluData* d) -> std::string { return d->r.name(); })
            .get("numParams", [](ReluData* d) -> int { return d->r.num_params(); })
            .method("forward",
                [](ReluData* d, JSContext* ctx, JSValueConst x, JSValueConst y) -> JSValue {
                    auto* xt = tensorFromJS(ctx, x); auto* yt = tensorFromJS(ctx, y);
                    if (!xt || !yt) return JS_ThrowTypeError(ctx, "Relu.forward(x,y)");
                    d->r.forward(*xt, *yt); return JS_UNDEFINED;
                })
            .method("backward",
                [](ReluData* d, JSContext* ctx, JSValueConst dY, JSValueConst dX) -> JSValue {
                    auto* dYt = tensorFromJS(ctx, dY); auto* dXt = tensorFromJS(ctx, dX);
                    if (!dYt || !dXt) return JS_ThrowTypeError(ctx, "Relu.backward(dY,dX)");
                    d->r.backward(*dYt, *dXt); return JS_UNDEFINED;
                })
            .method("zeroGrad", [](ReluData*){})
            .method("sgdStep",  [](ReluData*, double, double){});
    }
    {
        qjsbind::Class<TanhData>(ctx, "AITanh", qjsbind::NoGlobal)
            .get("name",      [](TanhData* d) -> std::string { return d->t.name(); })
            .get("numParams", [](TanhData* d) -> int { return d->t.num_params(); })
            .method("forward",
                [](TanhData* d, JSContext* ctx, JSValueConst x, JSValueConst y) -> JSValue {
                    auto* xt = tensorFromJS(ctx, x); auto* yt = tensorFromJS(ctx, y);
                    if (!xt || !yt) return JS_ThrowTypeError(ctx, "Tanh.forward(x,y)");
                    d->t.forward(*xt, *yt); return JS_UNDEFINED;
                })
            .method("backward",
                [](TanhData* d, JSContext* ctx, JSValueConst dY, JSValueConst dX) -> JSValue {
                    auto* dYt = tensorFromJS(ctx, dY); auto* dXt = tensorFromJS(ctx, dX);
                    if (!dYt || !dXt) return JS_ThrowTypeError(ctx, "Tanh.backward(dY,dX)");
                    d->t.backward(*dYt, *dXt); return JS_UNDEFINED;
                })
            .method("zeroGrad", [](TanhData*){})
            .method("sgdStep",  [](TanhData*, double, double){});
    }

    // ── DeepSetsEncoder ────────────────────────────────────────────────────
    {
        qjsbind::Class<DeepSetsEncoderData>(ctx, "AIDeepSetsEncoder", qjsbind::NoGlobal)
            .get("name",      [](DeepSetsEncoderData* d) -> std::string { return d->e.name(); })
            .get("outDim",    [](DeepSetsEncoderData* d) -> int { return d->e.out_dim(); })
            .get("numParams", [](DeepSetsEncoderData* d) -> int { return d->e.num_params(); })
            .method("init",
                [](DeepSetsEncoderData* d, JSContext* ctx, JSValueConst cfgV, JSValueConst seedV) -> JSValue {
                    nn::DeepSetsEncoder::Config cfg{};
                    if (JS_IsObject(cfgV)) {
                        cfg.hidden    = getInt(ctx, cfgV, "hidden", cfg.hidden);
                        cfg.embed_dim = getInt(ctx, cfgV, "embedDim", cfg.embed_dim);
                    }
                    uint64_t s = readSeed(ctx, seedV, 0xC0DE1234ULL);
                    d->e.init(cfg, s);
                    return JS_NewBigInt64(ctx, (int64_t)s);
                })
            .method("forward",
                [](DeepSetsEncoderData* d, JSContext* ctx, JSValueConst x, JSValueConst y) -> JSValue {
                    auto* xt = tensorFromJS(ctx, x); auto* yt = tensorFromJS(ctx, y);
                    if (!xt || !yt) return JS_ThrowTypeError(ctx, "DeepSetsEncoder.forward(x,y)");
                    d->e.forward(*xt, *yt); return JS_UNDEFINED;
                })
            .method("backward",
                [](DeepSetsEncoderData* d, JSContext* ctx, JSValueConst dY, JSValueConst dX) -> JSValue {
                    auto* dYt = tensorFromJS(ctx, dY); auto* dXt = tensorFromJS(ctx, dX);
                    if (!dYt || !dXt) return JS_ThrowTypeError(ctx, "DeepSetsEncoder.backward(dY,dX)");
                    d->e.backward(*dYt, *dXt); return JS_UNDEFINED;
                })
            .method("zeroGrad", [](DeepSetsEncoderData* d) { d->e.zero_grad(); })
            .method("sgdStep",  [](DeepSetsEncoderData* d, double lr, double m) { d->e.sgd_step((float)lr, (float)m); })
            .method("save",
                [](DeepSetsEncoderData* d, JSContext* ctx) -> JSValue {
                    std::vector<uint8_t> bytes;
                    d->e.save_to(bytes);
                    return makeUint8ArrayCopy(ctx, bytes.data(), bytes.size());
                })
            .method_raw("load",
                [](JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) -> JSValue {
                    auto* d = qjsbind::unwrap<DeepSetsEncoderData>(ctx, this_val);
                    if (!d || argc < 1) return JS_UNDEFINED;
                    size_t byteOff = 0, viewLen = 0;
                    JSValue abuf = JS_GetTypedArrayBuffer(ctx, argv[0], &byteOff, &viewLen, nullptr);
                    if (JS_IsException(abuf)) { JS_FreeValue(ctx, JS_GetException(ctx)); return JS_ThrowTypeError(ctx, "expected TypedArray"); }
                    size_t abufLen = 0;
                    uint8_t* raw = JS_GetArrayBuffer(ctx, &abufLen, abuf);
                    JS_FreeValue(ctx, abuf);
                    if (!raw) return JS_ThrowTypeError(ctx, "bad buffer");
                    size_t off = 0;
                    d->e.load_from(raw + byteOff, off, viewLen);
                    return JS_UNDEFINED;
                }, 1);
    }

    // ── ValueHead ──────────────────────────────────────────────────────────
    {
        qjsbind::Class<ValueHeadData>(ctx, "AIValueHead", qjsbind::NoGlobal)
            .get("name",      [](ValueHeadData* d) -> std::string { return d->v.name(); })
            .get("numParams", [](ValueHeadData* d) -> int { return d->v.num_params(); })
            .method("init",
                [](ValueHeadData* d, JSContext* ctx, int embedDim, int hidden, JSValueConst seedV) -> JSValue {
                    uint64_t s = readSeed(ctx, seedV, 0xC0DE1234ULL);
                    d->v.init(embedDim, hidden, s);
                    return JS_NewBigInt64(ctx, (int64_t)s);
                })
            .method("forward",
                [](ValueHeadData* d, JSContext* ctx, JSValueConst embed) -> JSValue {
                    auto* et = tensorFromJS(ctx, embed);
                    if (!et) return JS_ThrowTypeError(ctx, "ValueHead.forward(embed)");
                    float v = 0.0f;
                    d->v.forward(*et, v);
                    return JS_NewFloat64(ctx, v);
                })
            .method("backward",
                [](ValueHeadData* d, JSContext* ctx, double dValue, JSValueConst dEmbed) -> JSValue {
                    auto* de = tensorFromJS(ctx, dEmbed);
                    if (!de) return JS_ThrowTypeError(ctx, "ValueHead.backward(dValue,dEmbed)");
                    d->v.backward((float)dValue, *de);
                    return JS_UNDEFINED;
                })
            .method("zeroGrad", [](ValueHeadData* d) { d->v.zero_grad(); })
            .method("sgdStep",  [](ValueHeadData* d, double lr, double m) { d->v.sgd_step((float)lr, (float)m); })
            .method("save",
                [](ValueHeadData* d, JSContext* ctx) -> JSValue {
                    std::vector<uint8_t> b; d->v.save_to(b);
                    return makeUint8ArrayCopy(ctx, b.data(), b.size());
                })
            .method_raw("load",
                [](JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) -> JSValue {
                    auto* d = qjsbind::unwrap<ValueHeadData>(ctx, this_val);
                    if (!d || argc < 1) return JS_UNDEFINED;
                    size_t byteOff = 0, viewLen = 0;
                    JSValue abuf = JS_GetTypedArrayBuffer(ctx, argv[0], &byteOff, &viewLen, nullptr);
                    if (JS_IsException(abuf)) { JS_FreeValue(ctx, JS_GetException(ctx)); return JS_ThrowTypeError(ctx, "expected TypedArray"); }
                    size_t abufLen = 0;
                    uint8_t* raw = JS_GetArrayBuffer(ctx, &abufLen, abuf);
                    JS_FreeValue(ctx, abuf);
                    if (!raw) return JS_ThrowTypeError(ctx, "bad buffer");
                    size_t off = 0;
                    d->v.load_from(raw + byteOff, off, viewLen);
                    return JS_UNDEFINED;
                }, 1);
    }

    // ── FactoredPolicyHead ─────────────────────────────────────────────────
    {
        qjsbind::Class<FactoredPolicyHeadData>(ctx, "AIFactoredPolicyHead", qjsbind::NoGlobal)
            .get("name",        [](FactoredPolicyHeadData* d) -> std::string { return d->h.name(); })
            .get("numParams",   [](FactoredPolicyHeadData* d) -> int { return d->h.num_params(); })
            .get("totalLogits", [](FactoredPolicyHeadData* d) -> int { return d->h.total_logits(); })
            .method("init",
                [](FactoredPolicyHeadData* d, JSContext* ctx, int embedDim, JSValueConst seedV) -> JSValue {
                    uint64_t s = readSeed(ctx, seedV, 0xC0DE1234ULL);
                    d->h.init(embedDim, s);
                    return JS_NewBigInt64(ctx, (int64_t)s);
                })
            .method("forward",
                [](FactoredPolicyHeadData* d, JSContext* ctx, JSValueConst embed, JSValueConst logits) -> JSValue {
                    auto* et = tensorFromJS(ctx, embed); auto* lt = tensorFromJS(ctx, logits);
                    if (!et || !lt) return JS_ThrowTypeError(ctx, "FactoredPolicyHead.forward(embed,logits)");
                    d->h.forward(*et, *lt); return JS_UNDEFINED;
                })
            .method("backward",
                [](FactoredPolicyHeadData* d, JSContext* ctx, JSValueConst dLogits, JSValueConst dEmbed) -> JSValue {
                    auto* dl = tensorFromJS(ctx, dLogits); auto* de = tensorFromJS(ctx, dEmbed);
                    if (!dl || !de) return JS_ThrowTypeError(ctx, "FactoredPolicyHead.backward(dLogits,dEmbed)");
                    d->h.backward(*dl, *de); return JS_UNDEFINED;
                })
            .method("zeroGrad", [](FactoredPolicyHeadData* d) { d->h.zero_grad(); })
            .method("sgdStep",  [](FactoredPolicyHeadData* d, double lr, double m) { d->h.sgd_step((float)lr, (float)m); })
            .method("save",
                [](FactoredPolicyHeadData* d, JSContext* ctx) -> JSValue {
                    std::vector<uint8_t> b; d->h.save_to(b);
                    return makeUint8ArrayCopy(ctx, b.data(), b.size());
                })
            .method_raw("load",
                [](JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) -> JSValue {
                    auto* d = qjsbind::unwrap<FactoredPolicyHeadData>(ctx, this_val);
                    if (!d || argc < 1) return JS_UNDEFINED;
                    size_t byteOff = 0, viewLen = 0;
                    JSValue abuf = JS_GetTypedArrayBuffer(ctx, argv[0], &byteOff, &viewLen, nullptr);
                    if (JS_IsException(abuf)) { JS_FreeValue(ctx, JS_GetException(ctx)); return JS_ThrowTypeError(ctx, "expected TypedArray"); }
                    size_t abufLen = 0;
                    uint8_t* raw = JS_GetArrayBuffer(ctx, &abufLen, abuf);
                    JS_FreeValue(ctx, abuf);
                    if (!raw) return JS_ThrowTypeError(ctx, "bad buffer");
                    size_t off = 0;
                    d->h.load_from(raw + byteOff, off, viewLen);
                    return JS_UNDEFINED;
                }, 1);
    }

    // ── SingleHeroNet ──────────────────────────────────────────────────────
    {
        qjsbind::Class<SingleHeroNetData>(ctx, "AISingleHeroNet", qjsbind::NoGlobal)
            .get("embedDim",     [](SingleHeroNetData* d) -> int { return d->net ? d->net->embed_dim() : 0; })
            .get("trunkDim",     [](SingleHeroNetData* d) -> int { return d->net ? d->net->trunk_dim() : 0; })
            .get("policyLogits", [](SingleHeroNetData* d) -> int { return d->net ? d->net->policy_logits() : 0; })
            .get("numParams",    [](SingleHeroNetData* d) -> int { return d->net ? d->net->num_params() : 0; })
            .method("forward",
                [](SingleHeroNetData* d, JSContext* ctx, JSValueConst x, JSValueConst logits) -> JSValue {
                    if (!d->net) return JS_ThrowInternalError(ctx, "net not initialized");
                    auto* xt = tensorFromJS(ctx, x); auto* lt = tensorFromJS(ctx, logits);
                    if (!xt || !lt) return JS_ThrowTypeError(ctx, "SingleHeroNet.forward(x,logits)");
                    float v = 0.0f;
                    d->net->forward(*xt, v, *lt);
                    return JS_NewFloat64(ctx, v);
                })
            .method("backward",
                [](SingleHeroNetData* d, JSContext* ctx, double dValue, JSValueConst dLogits) -> JSValue {
                    if (!d->net) return JS_ThrowInternalError(ctx, "net not initialized");
                    auto* dl = tensorFromJS(ctx, dLogits);
                    if (!dl) return JS_ThrowTypeError(ctx, "SingleHeroNet.backward(dValue,dLogits)");
                    d->net->backward((float)dValue, *dl);
                    return JS_UNDEFINED;
                })
            .method("zeroGrad", [](SingleHeroNetData* d) { if (d->net) d->net->zero_grad(); })
            .method("sgdStep",  [](SingleHeroNetData* d, double lr, double m) { if (d->net) d->net->sgd_step((float)lr, (float)m); })
            .method("save",
                [](SingleHeroNetData* d, JSContext* ctx) -> JSValue {
                    if (!d->net) return makeUint8ArrayCopy(ctx, nullptr, 0);
                    auto blob = d->net->save();
                    return makeUint8ArrayCopy(ctx, blob.data(), blob.size());
                })
            .method_raw("load",
                [](JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) -> JSValue {
                    auto* d = qjsbind::unwrap<SingleHeroNetData>(ctx, this_val);
                    if (!d || !d->net || argc < 1) return JS_UNDEFINED;
                    size_t byteOff = 0, viewLen = 0;
                    JSValue abuf = JS_GetTypedArrayBuffer(ctx, argv[0], &byteOff, &viewLen, nullptr);
                    if (JS_IsException(abuf)) { JS_FreeValue(ctx, JS_GetException(ctx)); return JS_ThrowTypeError(ctx, "expected TypedArray"); }
                    size_t abufLen = 0;
                    uint8_t* raw = JS_GetArrayBuffer(ctx, &abufLen, abuf);
                    JS_FreeValue(ctx, abuf);
                    if (!raw) return JS_ThrowTypeError(ctx, "bad buffer");
                    std::vector<uint8_t> blob(raw + byteOff, raw + byteOff + viewLen);
                    d->net->load(blob);
                    return JS_UNDEFINED;
                }, 1);
    }

    // ── PolicyValueNet ─────────────────────────────────────────────────────
    {
        qjsbind::Class<PolicyValueNetData>(ctx, "AIPolicyValueNet", qjsbind::NoGlobal)
            .get("inDim",       [](PolicyValueNetData* d) -> int { return d->net ? d->net->in_dim() : 0; })
            .get("numActions",  [](PolicyValueNetData* d) -> int { return d->net ? d->net->num_actions() : 0; })
            .get("trunkDim",    [](PolicyValueNetData* d) -> int { return d->net ? d->net->trunk_dim() : 0; })
            .get("numParams",   [](PolicyValueNetData* d) -> int { return d->net ? d->net->num_params() : 0; })
            .get("numHeads",    [](PolicyValueNetData* d) -> int { return d->net ? d->net->num_heads() : 0; })
            .method("headSizes",
                [](PolicyValueNetData* d, JSContext* ctx) -> JSValue {
                    if (!d->net) return JS_NewArray(ctx);
                    const auto& sizes = d->net->head_sizes();
                    JSValue arr = JS_NewArray(ctx);
                    for (size_t i = 0; i < sizes.size(); ++i)
                        JS_SetPropertyUint32(ctx, arr, (uint32_t)i, JS_NewInt32(ctx, sizes[i]));
                    return arr;
                })
            .method("headOffsets",
                [](PolicyValueNetData* d, JSContext* ctx) -> JSValue {
                    if (!d->net) return JS_NewArray(ctx);
                    const auto& offs = d->net->head_offsets();
                    JSValue arr = JS_NewArray(ctx);
                    for (size_t i = 0; i < offs.size(); ++i)
                        JS_SetPropertyUint32(ctx, arr, (uint32_t)i, JS_NewInt32(ctx, offs[i]));
                    return arr;
                })
            .method("forward",
                [](PolicyValueNetData* d, JSContext* ctx, JSValueConst x, JSValueConst logits) -> JSValue {
                    if (!d->net) return JS_ThrowInternalError(ctx, "net not initialized");
                    auto* xt = tensorFromJS(ctx, x); auto* lt = tensorFromJS(ctx, logits);
                    if (!xt || !lt) return JS_ThrowTypeError(ctx, "PolicyValueNet.forward(x,logits)");
                    float v = 0.0f;
                    d->net->forward(*xt, v, *lt);
                    return JS_NewFloat64(ctx, v);
                })
            .method("backward",
                [](PolicyValueNetData* d, JSContext* ctx, double dValue, JSValueConst dLogits) -> JSValue {
                    if (!d->net) return JS_ThrowInternalError(ctx, "net not initialized");
                    auto* dl = tensorFromJS(ctx, dLogits);
                    if (!dl) return JS_ThrowTypeError(ctx, "PolicyValueNet.backward(dValue,dLogits)");
                    d->net->backward((float)dValue, *dl);
                    return JS_UNDEFINED;
                })
            .method("zeroGrad", [](PolicyValueNetData* d) { if (d->net) d->net->zero_grad(); })
            .method("sgdStep",  [](PolicyValueNetData* d, double lr, double m) { if (d->net) d->net->sgd_step((float)lr, (float)m); })
            // Device migration. Pass "gpu" or "cpu" (case-insensitive). Throws
            // a JS error on a CPU-only build when "gpu" is requested.
            .method("to",
                [](PolicyValueNetData* d, JSContext* ctx, JSValueConst devV) -> JSValue {
                    if (!d->net) return JS_ThrowInternalError(ctx, "net not initialized");
                    const char* s = JS_ToCString(ctx, devV);
                    std::string dev = s ? s : "";
                    if (s) JS_FreeCString(ctx, s);
                    for (auto& c : dev) c = (char)std::tolower((unsigned char)c);
                    nn::Device target = (dev == "gpu") ? nn::Device::GPU : nn::Device::CPU;
                    try { d->net->to(target); }
                    catch (const std::exception& e) { return JS_ThrowInternalError(ctx, "%s", e.what()); }
                    return JS_UNDEFINED;
                })
            .get("device",
                [](PolicyValueNetData* d) -> std::string {
                    if (!d->net) return std::string("cpu");
                    return d->net->device() == nn::Device::GPU ? std::string("gpu") : std::string("cpu");
                })
            .method("save",
                [](PolicyValueNetData* d, JSContext* ctx) -> JSValue {
                    if (!d->net) return makeUint8ArrayCopy(ctx, nullptr, 0);
                    auto blob = d->net->save();
                    return makeUint8ArrayCopy(ctx, blob.data(), blob.size());
                })
            .method_raw("load",
                [](JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) -> JSValue {
                    auto* d = qjsbind::unwrap<PolicyValueNetData>(ctx, this_val);
                    if (!d || !d->net || argc < 1) return JS_UNDEFINED;
                    size_t byteOff = 0, viewLen = 0;
                    JSValue abuf = JS_GetTypedArrayBuffer(ctx, argv[0], &byteOff, &viewLen, nullptr);
                    if (JS_IsException(abuf)) { JS_FreeValue(ctx, JS_GetException(ctx)); return JS_ThrowTypeError(ctx, "expected TypedArray"); }
                    size_t abufLen = 0;
                    uint8_t* raw = JS_GetArrayBuffer(ctx, &abufLen, abuf);
                    JS_FreeValue(ctx, abuf);
                    if (!raw) return JS_ThrowTypeError(ctx, "bad buffer");
                    std::vector<uint8_t> blob(raw + byteOff, raw + byteOff + viewLen);
                    d->net->load(blob);
                    return JS_UNDEFINED;
                }, 1);
    }

    // ── WeightsHandle ──────────────────────────────────────────────────────
    {
        qjsbind::Class<WeightsHandleData>(ctx, "AIWeightsHandle", qjsbind::NoGlobal)
            .method_raw("publish",
                [](JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) -> JSValue {
                    auto* d = qjsbind::unwrap<WeightsHandleData>(ctx, this_val);
                    if (!d || !d->handle || argc < 2) return JS_ThrowTypeError(ctx, "publish(blob, version)");
                    size_t byteOff = 0, viewLen = 0;
                    JSValue abuf = JS_GetTypedArrayBuffer(ctx, argv[0], &byteOff, &viewLen, nullptr);
                    if (JS_IsException(abuf)) { JS_FreeValue(ctx, JS_GetException(ctx)); return JS_ThrowTypeError(ctx, "expected TypedArray"); }
                    size_t abufLen = 0;
                    uint8_t* raw = JS_GetArrayBuffer(ctx, &abufLen, abuf);
                    JS_FreeValue(ctx, abuf);
                    if (!raw) return JS_ThrowTypeError(ctx, "bad buffer");
                    int64_t version = 0;
                    JS_ToBigInt64(ctx, &version, argv[1]);  // also accepts numbers
                    std::vector<uint8_t> blob(raw + byteOff, raw + byteOff + viewLen);
                    d->handle->publish(std::move(blob), (uint64_t)version);
                    return JS_UNDEFINED;
                }, 2)
            .method("snapshot",
                [](WeightsHandleData* d, JSContext* ctx) -> JSValue {
                    if (!d->handle) return JS_NULL;
                    uint64_t version = 0;
                    auto sp = d->handle->snapshot(&version);
                    if (!sp) return JS_NULL;
                    JSValue obj = JS_NewObject(ctx);
                    JS_SetPropertyStr(ctx, obj, "blob",
                        makeUint8ArrayCopy(ctx, sp->data(), sp->size()));
                    JS_SetPropertyStr(ctx, obj, "version", JS_NewBigInt64(ctx, (int64_t)version));
                    return obj;
                })
            .method("version",
                [](WeightsHandleData* d, JSContext* ctx) -> JSValue {
                    if (!d->handle) return JS_NewBigInt64(ctx, 0);
                    return JS_NewBigInt64(ctx, (int64_t)d->handle->version());
                });
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// Factory + free-function bindings
// ═══════════════════════════════════════════════════════════════════════════

static JSValue js_createTensor(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    int r = 0, c = 1;
    if (argc >= 1) JS_ToInt32(ctx, &r, argv[0]);
    if (argc >= 2) JS_ToInt32(ctx, &c, argv[1]);
    if (r < 0 || c < 0) return JS_ThrowRangeError(ctx, "negative dim");
    return qjsbind::wrap<TensorData>(ctx, new TensorData{ nn::Tensor(r, c) });
}

static JSValue js_createLinear(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    auto* d = new LinearData();
    if (argc >= 2) {
        int inDim = 0, outDim = 0;
        JS_ToInt32(ctx, &inDim, argv[0]);
        JS_ToInt32(ctx, &outDim, argv[1]);
        uint64_t s = argc >= 3 ? readSeed(ctx, argv[2], 0xC0DE1234ULL) : 0xC0DE1234ULL;
        d->l.init(inDim, outDim, s);
    }
    return qjsbind::wrap<LinearData>(ctx, d);
}

static JSValue js_createRelu(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    return qjsbind::wrap<ReluData>(ctx, new ReluData());
}

static JSValue js_createTanh(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    return qjsbind::wrap<TanhData>(ctx, new TanhData());
}

static JSValue js_createDeepSetsEncoder(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    auto* d = new DeepSetsEncoderData();
    nn::DeepSetsEncoder::Config cfg{};
    if (argc >= 1 && JS_IsObject(argv[0])) {
        cfg.hidden    = getInt(ctx, argv[0], "hidden", cfg.hidden);
        cfg.embed_dim = getInt(ctx, argv[0], "embedDim", cfg.embed_dim);
    }
    uint64_t s = argc >= 2 ? readSeed(ctx, argv[1], 0xC0DE1234ULL) : 0xC0DE1234ULL;
    d->e.init(cfg, s);
    return qjsbind::wrap<DeepSetsEncoderData>(ctx, d);
}

static JSValue js_createValueHead(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    auto* d = new ValueHeadData();
    if (argc >= 2) {
        int embedDim = 0, hidden = 0;
        JS_ToInt32(ctx, &embedDim, argv[0]);
        JS_ToInt32(ctx, &hidden, argv[1]);
        uint64_t s = argc >= 3 ? readSeed(ctx, argv[2], 0xC0DE1234ULL) : 0xC0DE1234ULL;
        d->v.init(embedDim, hidden, s);
    }
    return qjsbind::wrap<ValueHeadData>(ctx, d);
}

static JSValue js_createFactoredPolicyHead(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    auto* d = new FactoredPolicyHeadData();
    if (argc >= 1) {
        int embedDim = 0;
        JS_ToInt32(ctx, &embedDim, argv[0]);
        uint64_t s = argc >= 2 ? readSeed(ctx, argv[1], 0xC0DE1234ULL) : 0xC0DE1234ULL;
        d->h.init(embedDim, s);
    }
    return qjsbind::wrap<FactoredPolicyHeadData>(ctx, d);
}

static JSValue js_createSingleHeroNet(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    nn::SingleHeroNet::Config cfg{};
    if (argc >= 1 && JS_IsObject(argv[0])) {
        JSValue encV = JS_GetPropertyStr(ctx, argv[0], "enc");
        if (JS_IsObject(encV)) {
            cfg.enc.hidden    = getInt(ctx, encV, "hidden", cfg.enc.hidden);
            cfg.enc.embed_dim = getInt(ctx, encV, "embedDim", cfg.enc.embed_dim);
        }
        JS_FreeValue(ctx, encV);
        cfg.trunk_hidden = getInt(ctx, argv[0], "trunkHidden", cfg.trunk_hidden);
        cfg.value_hidden = getInt(ctx, argv[0], "valueHidden", cfg.value_hidden);
        JSValue seedV = JS_GetPropertyStr(ctx, argv[0], "seed");
        cfg.seed = readSeed(ctx, seedV, cfg.seed);
        JS_FreeValue(ctx, seedV);
    }
    auto net = std::make_shared<nn::SingleHeroNet>();
    net->init(cfg);
    return qjsbind::wrap<SingleHeroNetData>(ctx, new SingleHeroNetData{ std::move(net) });
}

static JSValue js_createWeightsHandle(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    return qjsbind::wrap<WeightsHandleData>(ctx,
        new WeightsHandleData{ std::make_shared<nn::WeightsHandle>() });
}

// Read an array of positive ints from prop `name` on `obj`. Empty/absent
// → empty vector; entries ≤ 0 are skipped silently.
static std::vector<int> readIntArray(JSContext* ctx, JSValueConst obj, const char* name) {
    std::vector<int> out;
    JSValue v = JS_GetPropertyStr(ctx, obj, name);
    if (JS_IsArray(v)) {
        uint32_t len = 0;
        JSValue lv = JS_GetPropertyStr(ctx, v, "length");
        JS_ToUint32(ctx, &len, lv);
        JS_FreeValue(ctx, lv);
        out.reserve(len);
        for (uint32_t i = 0; i < len; ++i) {
            JSValue ev = JS_GetPropertyUint32(ctx, v, i);
            int32_t w = 0;
            JS_ToInt32(ctx, &w, ev);
            JS_FreeValue(ctx, ev);
            if (w > 0) out.push_back(w);
        }
    }
    JS_FreeValue(ctx, v);
    return out;
}

static JSValue js_createPolicyValueNet(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    nn::PolicyValueNet::Config cfg{};
    if (argc >= 1 && JS_IsObject(argv[0])) {
        cfg.in_dim       = getInt(ctx, argv[0], "inDim",       cfg.in_dim);
        cfg.num_actions  = getInt(ctx, argv[0], "numActions",  cfg.num_actions);
        cfg.value_hidden = getInt(ctx, argv[0], "valueHidden", cfg.value_hidden);

        auto hidden = readIntArray(ctx, argv[0], "hidden");
        if (!hidden.empty()) cfg.hidden = std::move(hidden);

        // headSizes (optional): factored policy heads. When provided,
        // numActions auto-derives from sum if 0; otherwise must match.
        cfg.head_sizes = readIntArray(ctx, argv[0], "headSizes");

        JSValue seedV = JS_GetPropertyStr(ctx, argv[0], "seed");
        cfg.seed = readSeed(ctx, seedV, cfg.seed);
        JS_FreeValue(ctx, seedV);
    }
    // numActions can be 0 if headSizes provided — init() will derive it.
    const bool actions_ok = cfg.num_actions > 0 || !cfg.head_sizes.empty();
    if (cfg.in_dim <= 0 || !actions_ok || cfg.hidden.empty() || cfg.value_hidden <= 0) {
        return JS_ThrowTypeError(ctx,
            "createPolicyValueNet({inDim,numActions|headSizes,hidden:[...],valueHidden,seed?}) — "
            "inDim, hidden[], valueHidden are required, plus one of numActions or headSizes");
    }
    auto net = std::make_shared<nn::PolicyValueNet>();
    net->init(cfg);
    return qjsbind::wrap<PolicyValueNetData>(ctx, new PolicyValueNetData{ std::move(net) });
}

// ─── Ops ───────────────────────────────────────────────────────────────────

static JSValue js_linearForward(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 4) return JS_ThrowTypeError(ctx, "linearForward(W,b,x,y)");
    auto* W = tensorFromJS(ctx, argv[0]);
    auto* b = tensorFromJS(ctx, argv[1]);
    auto* x = tensorFromJS(ctx, argv[2]);
    auto* y = tensorFromJS(ctx, argv[3]);
    if (!W || !b || !x || !y) return JS_ThrowTypeError(ctx, "expected Tensors");
    nn::linear_forward(*W, *b, *x, *y);
    return JS_UNDEFINED;
}

static JSValue js_linearBackward(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 6) return JS_ThrowTypeError(ctx, "linearBackward(W,x,dY,dX,dW,dB)");
    auto* W = tensorFromJS(ctx, argv[0]);
    auto* x = tensorFromJS(ctx, argv[1]);
    auto* dY = tensorFromJS(ctx, argv[2]);
    auto* dX = tensorFromJS(ctx, argv[3]);
    auto* dW = tensorFromJS(ctx, argv[4]);
    auto* dB = tensorFromJS(ctx, argv[5]);
    if (!W || !x || !dY || !dX || !dW || !dB) return JS_ThrowTypeError(ctx, "expected Tensors");
    nn::linear_backward(*W, *x, *dY, *dX, *dW, *dB);
    return JS_UNDEFINED;
}

static JSValue js_reluForward(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 2) return JS_ThrowTypeError(ctx, "reluForward(x,y)");
    auto* x = tensorFromJS(ctx, argv[0]); auto* y = tensorFromJS(ctx, argv[1]);
    if (!x || !y) return JS_ThrowTypeError(ctx, "expected Tensors");
    nn::relu_forward(*x, *y);
    return JS_UNDEFINED;
}
static JSValue js_reluBackward(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 3) return JS_ThrowTypeError(ctx, "reluBackward(x,dY,dX)");
    auto* x = tensorFromJS(ctx, argv[0]); auto* dY = tensorFromJS(ctx, argv[1]); auto* dX = tensorFromJS(ctx, argv[2]);
    if (!x || !dY || !dX) return JS_ThrowTypeError(ctx, "expected Tensors");
    nn::relu_backward(*x, *dY, *dX);
    return JS_UNDEFINED;
}
static JSValue js_tanhForward(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 2) return JS_ThrowTypeError(ctx, "tanhForward(x,y)");
    auto* x = tensorFromJS(ctx, argv[0]); auto* y = tensorFromJS(ctx, argv[1]);
    if (!x || !y) return JS_ThrowTypeError(ctx, "expected Tensors");
    nn::tanh_forward(*x, *y);
    return JS_UNDEFINED;
}
static JSValue js_tanhBackward(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 3) return JS_ThrowTypeError(ctx, "tanhBackward(y,dY,dX)");
    auto* y = tensorFromJS(ctx, argv[0]); auto* dY = tensorFromJS(ctx, argv[1]); auto* dX = tensorFromJS(ctx, argv[2]);
    if (!y || !dY || !dX) return JS_ThrowTypeError(ctx, "expected Tensors");
    nn::tanh_backward(*y, *dY, *dX);
    return JS_UNDEFINED;
}

static JSValue js_softmaxForward(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 2) return JS_ThrowTypeError(ctx, "softmaxForward(logits,probs,mask?)");
    auto* l = tensorFromJS(ctx, argv[0]); auto* p = tensorFromJS(ctx, argv[1]);
    if (!l || !p) return JS_ThrowTypeError(ctx, "expected Tensors");
    size_t mn = 0;
    float* mask = argc >= 3 ? getFloatArrayPtr(ctx, argv[2], mn) : nullptr;
    nn::softmax_forward(*l, *p, mask);
    return JS_UNDEFINED;
}

static JSValue js_softmaxBackward(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 3) return JS_ThrowTypeError(ctx, "softmaxBackward(probs,dProbs,dLogits)");
    auto* p = tensorFromJS(ctx, argv[0]); auto* dp = tensorFromJS(ctx, argv[1]); auto* dl = tensorFromJS(ctx, argv[2]);
    if (!p || !dp || !dl) return JS_ThrowTypeError(ctx, "expected Tensors");
    nn::softmax_backward(*p, *dp, *dl);
    return JS_UNDEFINED;
}

static JSValue js_softmaxXent(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 4) return JS_ThrowTypeError(ctx, "softmaxXent(logits,target,probs,dLogits,mask?)");
    auto* l = tensorFromJS(ctx, argv[0]); auto* t = tensorFromJS(ctx, argv[1]);
    auto* p = tensorFromJS(ctx, argv[2]); auto* dl = tensorFromJS(ctx, argv[3]);
    if (!l || !t || !p || !dl) return JS_ThrowTypeError(ctx, "expected Tensors");
    size_t mn = 0;
    float* mask = argc >= 5 ? getFloatArrayPtr(ctx, argv[4], mn) : nullptr;
    float loss = nn::softmax_xent(*l, *t, *p, *dl, mask);
    return JS_NewFloat64(ctx, loss);
}

static JSValue js_mseScalar(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 2) return JS_ThrowTypeError(ctx, "mseScalar(pred,target)");
    double pred = 0, target = 0;
    JS_ToFloat64(ctx, &pred, argv[0]);
    JS_ToFloat64(ctx, &target, argv[1]);
    float dPred = 0.0f;
    float loss = nn::mse_scalar((float)pred, (float)target, dPred);
    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "loss", JS_NewFloat64(ctx, loss));
    JS_SetPropertyStr(ctx, obj, "dPred", JS_NewFloat64(ctx, dPred));
    return obj;
}

static JSValue js_addInplace(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 2) return JS_ThrowTypeError(ctx, "addInplace(y,x)");
    auto* y = tensorFromJS(ctx, argv[0]); auto* x = tensorFromJS(ctx, argv[1]);
    if (!x || !y) return JS_ThrowTypeError(ctx, "expected Tensors");
    nn::add_inplace(*y, *x);
    return JS_UNDEFINED;
}

static JSValue js_addScalarInplace(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 2) return JS_ThrowTypeError(ctx, "addScalarInplace(y,s)");
    auto* y = tensorFromJS(ctx, argv[0]);
    if (!y) return JS_ThrowTypeError(ctx, "expected Tensor");
    double s = 0; JS_ToFloat64(ctx, &s, argv[1]);
    nn::add_scalar_inplace(*y, (float)s);
    return JS_UNDEFINED;
}

static JSValue js_xavierInit(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 1) return JS_ThrowTypeError(ctx, "xavierInit(W, seed?)");
    auto* W = tensorFromJS(ctx, argv[0]);
    if (!W) return JS_ThrowTypeError(ctx, "expected Tensor");
    uint64_t s = argc >= 2 ? readSeed(ctx, argv[1], 0xC0DE1234ULL) : 0xC0DE1234ULL;
    nn::xavier_init(*W, s);
    return JS_NewBigInt64(ctx, (int64_t)s);
}

static JSValue js_factoredSoftmax(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 2) return JS_ThrowTypeError(ctx, "factoredSoftmax(logits,probs,atkMask?,abilMask?)");
    auto* l = tensorFromJS(ctx, argv[0]); auto* p = tensorFromJS(ctx, argv[1]);
    if (!l || !p) return JS_ThrowTypeError(ctx, "expected Tensors");
    size_t amn = 0, bmn = 0;
    float* aMask = argc >= 3 ? getFloatArrayPtr(ctx, argv[2], amn) : nullptr;
    float* bMask = argc >= 4 ? getFloatArrayPtr(ctx, argv[3], bmn) : nullptr;
    nn::factored_softmax(*l, *p, aMask, bMask);
    return JS_UNDEFINED;
}

static JSValue js_factoredXent(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 6) return JS_ThrowTypeError(ctx, "factoredXent(logits,mTgt,aTgt,abTgt,probs,dLogits,atkMask?,abilMask?)");
    auto* l   = tensorFromJS(ctx, argv[0]);
    auto* mt  = tensorFromJS(ctx, argv[1]);
    auto* at  = tensorFromJS(ctx, argv[2]);
    auto* abt = tensorFromJS(ctx, argv[3]);
    auto* p   = tensorFromJS(ctx, argv[4]);
    auto* dl  = tensorFromJS(ctx, argv[5]);
    if (!l || !mt || !at || !abt || !p || !dl) return JS_ThrowTypeError(ctx, "expected Tensors");
    size_t amn = 0, bmn = 0;
    float* aMask = argc >= 7 ? getFloatArrayPtr(ctx, argv[6], amn) : nullptr;
    float* bMask = argc >= 8 ? getFloatArrayPtr(ctx, argv[7], bmn) : nullptr;
    float loss = nn::factored_xent(*l, *mt, *at, *abt, *p, *dl, aMask, bMask);
    return JS_NewFloat64(ctx, loss);
}

// ─── Factored multi-head policy helpers ──────────────────────────────────
//
// Wraps nn::factored_to_flat and friends. The JS layer passes head_sizes as
// a JS array of ints; offsets are derived internally to keep the call site
// simple. Logits / flat_prior / masks are Float32 arrays (TypedArray, not
// Tensor wrappers — this matches how the trainer worker shuffles data).

static std::vector<int> readJSIntArray(JSContext* ctx, JSValueConst arr) {
    std::vector<int> out;
    if (!JS_IsArray(arr)) return out;
    JSValue lv = JS_GetPropertyStr(ctx, arr, "length");
    uint32_t len = 0;
    JS_ToUint32(ctx, &len, lv);
    JS_FreeValue(ctx, lv);
    out.reserve(len);
    for (uint32_t i = 0; i < len; ++i) {
        JSValue ev = JS_GetPropertyUint32(ctx, arr, i);
        int32_t v = 0;
        JS_ToInt32(ctx, &v, ev);
        JS_FreeValue(ctx, ev);
        out.push_back(v);
    }
    return out;
}

static std::vector<int> offsetsFromSizes(const std::vector<int>& sizes) {
    std::vector<int> offs;
    offs.reserve(sizes.size() + 1);
    int o = 0;
    for (int s : sizes) { offs.push_back(o); o += s; }
    offs.push_back(o);
    return offs;
}

static JSValue js_factoredToFlat(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 3)
        return JS_ThrowTypeError(ctx, "factoredToFlat(logits, headSizes, flatPrior, headMasks?)");
    size_t lN = 0; float* logits = getFloatArrayPtr(ctx, argv[0], lN);
    if (!logits) return JS_ThrowTypeError(ctx, "logits must be Float32Array");
    auto sizes = readJSIntArray(ctx, argv[1]);
    if (sizes.empty()) return JS_ThrowTypeError(ctx, "headSizes must be a non-empty int array");
    auto offsets = offsetsFromSizes(sizes);
    if ((int)lN < offsets.back())
        return JS_ThrowTypeError(ctx, "logits shorter than sum(headSizes)");
    size_t fN = 0; float* flat = getFloatArrayPtr(ctx, argv[2], fN);
    if (!flat) return JS_ThrowTypeError(ctx, "flatPrior must be Float32Array");
    const int total = nn::flat_action_count(sizes);
    if ((int)fN < total)
        return JS_ThrowTypeError(ctx, "flatPrior shorter than prod(headSizes)");
    size_t mN = 0;
    float* masks = (argc >= 4 && !JS_IsUndefined(argv[3]) && !JS_IsNull(argv[3]))
                   ? getFloatArrayPtr(ctx, argv[3], mN) : nullptr;
    if (masks && (int)mN < offsets.back())
        return JS_ThrowTypeError(ctx, "headMasks shorter than sum(headSizes)");
    nn::factored_to_flat(logits, sizes, offsets, flat, masks);
    return JS_UNDEFINED;
}

static JSValue js_flatActionCount(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 1) return JS_ThrowTypeError(ctx, "flatActionCount(headSizes)");
    auto sizes = readJSIntArray(ctx, argv[0]);
    return JS_NewInt32(ctx, nn::flat_action_count(sizes));
}

static JSValue js_decodeFlatAction(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 2) return JS_ThrowTypeError(ctx, "decodeFlatAction(flat, headSizes)");
    int32_t flat = 0; JS_ToInt32(ctx, &flat, argv[0]);
    auto sizes = readJSIntArray(ctx, argv[1]);
    if (sizes.empty()) return JS_ThrowTypeError(ctx, "headSizes must be non-empty");
    auto strides = nn::head_strides(sizes);
    std::vector<int> out(sizes.size());
    nn::decode_flat_action(flat, sizes, strides, out.data());
    JSValue arr = JS_NewArray(ctx);
    for (size_t i = 0; i < out.size(); ++i)
        JS_SetPropertyUint32(ctx, arr, (uint32_t)i, JS_NewInt32(ctx, out[i]));
    return arr;
}

static JSValue js_encodeFlatAction(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 2) return JS_ThrowTypeError(ctx, "encodeFlatAction(perHead, headSizes)");
    auto perHead = readJSIntArray(ctx, argv[0]);
    auto sizes = readJSIntArray(ctx, argv[1]);
    if (sizes.empty() || perHead.size() != sizes.size())
        return JS_ThrowTypeError(ctx, "perHead/headSizes length mismatch");
    auto strides = nn::head_strides(sizes);
    return JS_NewInt32(ctx, nn::encode_flat_action(perHead.data(), strides, (int)sizes.size()));
}

// ═══════════════════════════════════════════════════════════════════════════
// Install
// ═══════════════════════════════════════════════════════════════════════════

void installNNBindings(JSContext* ctx, JSValue gameObj) {
    registerClasses(ctx);

    JSValue nnObj = JS_NewObject(ctx);

    // Factories
    JS_SetPropertyStr(ctx, nnObj, "createTensor",             JS_NewCFunction(ctx, js_createTensor, "createTensor", 2));
    JS_SetPropertyStr(ctx, nnObj, "createLinear",             JS_NewCFunction(ctx, js_createLinear, "createLinear", 3));
    JS_SetPropertyStr(ctx, nnObj, "createRelu",               JS_NewCFunction(ctx, js_createRelu, "createRelu", 0));
    JS_SetPropertyStr(ctx, nnObj, "createTanh",               JS_NewCFunction(ctx, js_createTanh, "createTanh", 0));
    JS_SetPropertyStr(ctx, nnObj, "createDeepSetsEncoder",    JS_NewCFunction(ctx, js_createDeepSetsEncoder, "createDeepSetsEncoder", 2));
    JS_SetPropertyStr(ctx, nnObj, "createValueHead",          JS_NewCFunction(ctx, js_createValueHead, "createValueHead", 3));
    JS_SetPropertyStr(ctx, nnObj, "createFactoredPolicyHead", JS_NewCFunction(ctx, js_createFactoredPolicyHead, "createFactoredPolicyHead", 2));
    JS_SetPropertyStr(ctx, nnObj, "createSingleHeroNet",      JS_NewCFunction(ctx, js_createSingleHeroNet, "createSingleHeroNet", 1));
    JS_SetPropertyStr(ctx, nnObj, "createPolicyValueNet",     JS_NewCFunction(ctx, js_createPolicyValueNet, "createPolicyValueNet", 1));
    JS_SetPropertyStr(ctx, nnObj, "createWeightsHandle",      JS_NewCFunction(ctx, js_createWeightsHandle, "createWeightsHandle", 0));

    // Ops
    JS_SetPropertyStr(ctx, nnObj, "linearForward",    JS_NewCFunction(ctx, js_linearForward, "linearForward", 4));
    JS_SetPropertyStr(ctx, nnObj, "linearBackward",   JS_NewCFunction(ctx, js_linearBackward, "linearBackward", 6));
    JS_SetPropertyStr(ctx, nnObj, "reluForward",      JS_NewCFunction(ctx, js_reluForward, "reluForward", 2));
    JS_SetPropertyStr(ctx, nnObj, "reluBackward",     JS_NewCFunction(ctx, js_reluBackward, "reluBackward", 3));
    JS_SetPropertyStr(ctx, nnObj, "tanhForward",      JS_NewCFunction(ctx, js_tanhForward, "tanhForward", 2));
    JS_SetPropertyStr(ctx, nnObj, "tanhBackward",     JS_NewCFunction(ctx, js_tanhBackward, "tanhBackward", 3));
    JS_SetPropertyStr(ctx, nnObj, "softmaxForward",   JS_NewCFunction(ctx, js_softmaxForward, "softmaxForward", 3));
    JS_SetPropertyStr(ctx, nnObj, "softmaxBackward",  JS_NewCFunction(ctx, js_softmaxBackward, "softmaxBackward", 3));
    JS_SetPropertyStr(ctx, nnObj, "softmaxXent",      JS_NewCFunction(ctx, js_softmaxXent, "softmaxXent", 5));
    JS_SetPropertyStr(ctx, nnObj, "mseScalar",        JS_NewCFunction(ctx, js_mseScalar, "mseScalar", 2));
    JS_SetPropertyStr(ctx, nnObj, "addInplace",       JS_NewCFunction(ctx, js_addInplace, "addInplace", 2));
    JS_SetPropertyStr(ctx, nnObj, "addScalarInplace", JS_NewCFunction(ctx, js_addScalarInplace, "addScalarInplace", 2));
    JS_SetPropertyStr(ctx, nnObj, "xavierInit",       JS_NewCFunction(ctx, js_xavierInit, "xavierInit", 2));
    JS_SetPropertyStr(ctx, nnObj, "factoredSoftmax",  JS_NewCFunction(ctx, js_factoredSoftmax, "factoredSoftmax", 4));
    JS_SetPropertyStr(ctx, nnObj, "factoredXent",     JS_NewCFunction(ctx, js_factoredXent, "factoredXent", 8));

    // Multi-head policy helpers (PolicyValueNet head_sizes / nn::factored_to_flat).
    JS_SetPropertyStr(ctx, nnObj, "factoredToFlat",   JS_NewCFunction(ctx, js_factoredToFlat, "factoredToFlat", 4));
    JS_SetPropertyStr(ctx, nnObj, "flatActionCount",  JS_NewCFunction(ctx, js_flatActionCount, "flatActionCount", 1));
    JS_SetPropertyStr(ctx, nnObj, "decodeFlatAction", JS_NewCFunction(ctx, js_decodeFlatAction, "decodeFlatAction", 2));
    JS_SetPropertyStr(ctx, nnObj, "encodeFlatAction", JS_NewCFunction(ctx, js_encodeFlatAction, "encodeFlatAction", 2));

    // Constants (mirrored here for policy-head shape discovery)
    JS_SetPropertyStr(ctx, nnObj, "N_MOVE",    JS_NewInt32(ctx, nn::FactoredPolicyHead::N_MOVE));
    JS_SetPropertyStr(ctx, nnObj, "N_ATTACK",  JS_NewInt32(ctx, nn::FactoredPolicyHead::N_ATTACK));
    JS_SetPropertyStr(ctx, nnObj, "N_ABILITY", JS_NewInt32(ctx, nn::FactoredPolicyHead::N_ABILITY));

    // Install GPU sub-namespace (bro.ai.game.nn.gpu). Implemented in
    // ai_nn_gpu_bindings.cpp; falls back to `{ available: false }` when
    // brogameagent was built without CUDA.
    installNNGpuBindings(ctx, nnObj);

    JS_SetPropertyStr(ctx, gameObj, "nn", nnObj);
}

// Unwrap helpers used by other bindings (learn, belief).
nn::SingleHeroNet* nnSingleHeroNetFromJS(JSContext* ctx, JSValueConst v) {
    auto* d = qjsbind::unwrap<SingleHeroNetData>(ctx, v);
    return d && d->net ? d->net.get() : nullptr;
}
std::shared_ptr<nn::SingleHeroNet> nnSingleHeroNetSharedFromJS(JSContext* ctx, JSValueConst v) {
    auto* d = qjsbind::unwrap<SingleHeroNetData>(ctx, v);
    return d ? d->net : std::shared_ptr<nn::SingleHeroNet>{};
}
nn::WeightsHandle* nnWeightsHandleFromJS(JSContext* ctx, JSValueConst v) {
    auto* d = qjsbind::unwrap<WeightsHandleData>(ctx, v);
    return d && d->handle ? d->handle.get() : nullptr;
}
nn::PolicyValueNet* nnPolicyValueNetFromJS(JSContext* ctx, JSValueConst v) {
    auto* d = qjsbind::unwrap<PolicyValueNetData>(ctx, v);
    return d && d->net ? d->net.get() : nullptr;
}
std::shared_ptr<nn::PolicyValueNet> nnPolicyValueNetSharedFromJS(JSContext* ctx, JSValueConst v) {
    auto* d = qjsbind::unwrap<PolicyValueNetData>(ctx, v);
    return d ? d->net : std::shared_ptr<nn::PolicyValueNet>{};
}

} // namespace bro::js
