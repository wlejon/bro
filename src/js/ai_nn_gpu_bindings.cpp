// JS bindings for brogameagent::nn::gpu — CUDA backend.
//
// Installed onto bro.ai.game.nn.gpu by installNNGpuBindings(), which is called
// from ai_nn_bindings.cpp after the host nn namespace is set up.
//
// Compilation gating:
//   When brogameagent is built with BROGAMEAGENT_WITH_CUDA=ON, the public
//   header propagates BGA_HAS_CUDA=1. We use that to conditionally compile
//   the real bindings; otherwise installNNGpuBindings publishes a stub
//   namespace with `available: false` so JS code can detect availability.

#include "js/ai_bindings.h"

#include <qjsbind/qjsbind.h>

#ifdef BGA_HAS_CUDA

#include <brogameagent/nn/tensor.h>
#include <brogameagent/nn/gpu/runtime.h>
#include <brogameagent/nn/gpu/tensor.h>
#include <brogameagent/nn/gpu/ops.h>

#include <cstdint>
#include <cstring>
#include <vector>

namespace bro::js {

namespace nn    = brogameagent::nn;
namespace nngpu = brogameagent::nn::gpu;

// ─── Wrapper struct (owning) ───────────────────────────────────────────────
struct GpuTensorData { nngpu::GpuTensor t; };

// We deliberately do NOT expose GpuTensor::view() — non-owning views would
// invite lifetime traps from the JS side. Wrapping is owning-only.

// ─── Small helpers (duplicated from ai_nn_bindings.cpp by design) ──────────
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

// Unwrap the host AITensor wrapper. We don't have its struct definition here;
// instead we duplicate just enough to peek at it using qjsbind's class id by
// name. The simplest correct route is to declare the same wrapper struct
// shape — but that would create a different class id. Instead we use the
// fact that ai_nn_bindings.cpp's tensorFromJS is internal; we replicate it by
// using qjsbind::unwrap with a *forward* declaration of the same struct here.
//
// Per project convention (duplicated helpers ok across TUs), declare a local
// alias type pinned to the same registered class. qjsbind keys classes by the
// C++ type passed to Class<T>, so a local struct here would be a different
// type. To stay safe, we rely on ai_nn_bindings.cpp exposing tensors only via
// methods on the AITensor JS class, and we accept Tensor input via the
// Float32Array path or via direct AITensor methods invoked from JS.
//
// For upload(AITensor)/download(AITensor) we therefore call into JS-level
// `toArray()` / `fromArray()` accessors? No — that would be a host roundtrip.
// Instead, we reach into the AITensor instance via qjsbind::unwrap with a
// matching struct definition. qjsbind uses a JS class id keyed on a static
// per-T slot, so we MUST share the type. We declare an opaque-but-identical
// shape under the same fully qualified name:
//
//   bro::js::TensorData { nn::Tensor t; }
//
// matches the definition in ai_nn_bindings.cpp. Forward-declared here so the
// TU shares the same type identity as the one in the sibling TU.
struct TensorData { nn::Tensor t; };

static nn::Tensor* tensorFromJS(JSContext* ctx, JSValueConst v) {
    auto* d = qjsbind::unwrap<TensorData>(ctx, v);
    return d ? &d->t : nullptr;
}

static nngpu::GpuTensor* gpuTensorFromJSLocal(JSContext* ctx, JSValueConst v) {
    auto* d = qjsbind::unwrap<GpuTensorData>(ctx, v);
    return d ? &d->t : nullptr;
}

// Resolve an optional mask argument: null/undefined → nullptr, GpuTensor →
// device pointer (its .data). Anything else throws TypeError (caller checks
// JS_IsException via the returned thrown flag).
//
// Returns true on success (out_ptr possibly nullptr); false if a TypeError
// was thrown. Note: device-pointer mask, NOT a host Float32Array — this
// differs from the CPU softmax/attention mask convention.
static bool resolveDeviceMask(JSContext* ctx, JSValueConst v, const float*& out_ptr,
                              JSValue& thrown_err) {
    if (JS_IsUndefined(v) || JS_IsNull(v)) { out_ptr = nullptr; return true; }
    auto* gt = gpuTensorFromJSLocal(ctx, v);
    if (!gt) {
        thrown_err = JS_ThrowTypeError(ctx, "mask must be null or a GpuTensor (device pointer)");
        return false;
    }
    out_ptr = gt->data;
    return true;
}

// ═══════════════════════════════════════════════════════════════════════════
// Class registration
// ═══════════════════════════════════════════════════════════════════════════

static void registerClasses(JSContext* ctx) {
    qjsbind::Class<GpuTensorData>(ctx, "AIGpuTensor", qjsbind::NoGlobal)
        .get("rows", [](GpuTensorData* d) -> int { return d->t.rows; })
        .get("cols", [](GpuTensorData* d) -> int { return d->t.cols; })
        .get("size", [](GpuTensorData* d) -> int { return d->t.size(); })
        .method("zero",   [](GpuTensorData* d) { d->t.zero(); })
        .method("resize", [](GpuTensorData* d, int r, int c) { d->t.resize(r, c); })
        .method_raw("clone",
            [](JSContext* ctx, JSValueConst this_val, int, JSValueConst*) -> JSValue {
                auto* d = qjsbind::unwrap<GpuTensorData>(ctx, this_val);
                if (!d) return JS_ThrowTypeError(ctx, "clone() on non-GpuTensor");
                auto* nd = new GpuTensorData();
                nd->t = d->t.clone();
                return qjsbind::wrap<GpuTensorData>(ctx, nd);
            }, 0)
        // upload(src): src is an AITensor (host) or a Float32Array.
        .method_raw("upload",
            [](JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) -> JSValue {
                auto* d = qjsbind::unwrap<GpuTensorData>(ctx, this_val);
                if (!d || argc < 1) return JS_ThrowTypeError(ctx, "upload(src) — expected Tensor or Float32Array");
                // Try host Tensor first.
                if (auto* ht = tensorFromJS(ctx, argv[0])) {
                    nngpu::upload(*ht, d->t);
                    return JS_UNDEFINED;
                }
                // Float32Array path.
                size_t n = 0;
                float* src = getFloatArrayPtr(ctx, argv[0], n);
                if (!src) return JS_ThrowTypeError(ctx, "upload(src) — expected Tensor or Float32Array");
                // Build a temporary host Tensor matching this GpuTensor's shape if known,
                // else use (n, 1). Then upload (which resizes dst on shape mismatch).
                int r = d->t.rows, c = d->t.cols;
                if (r * c != (int)n) { r = (int)n; c = 1; }
                nn::Tensor host(r, c);
                std::memcpy(host.ptr(), src, n * sizeof(float));
                nngpu::upload(host, d->t);
                return JS_UNDEFINED;
            }, 1)
        // download(dst?): if dst is AITensor, download into it (auto-syncs);
        // else sync, download to a temp host Tensor and return Float32Array.
        .method_raw("download",
            [](JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) -> JSValue {
                auto* d = qjsbind::unwrap<GpuTensorData>(ctx, this_val);
                if (!d) return JS_ThrowTypeError(ctx, "download() on non-GpuTensor");
                nngpu::cuda_sync();
                if (argc >= 1 && !JS_IsUndefined(argv[0]) && !JS_IsNull(argv[0])) {
                    if (auto* ht = tensorFromJS(ctx, argv[0])) {
                        nngpu::download(d->t, *ht);
                        return JS_UNDEFINED;
                    }
                    return JS_ThrowTypeError(ctx, "download(dst): dst must be a Tensor");
                }
                nn::Tensor host(d->t.rows, d->t.cols);
                nngpu::download(d->t, host);
                return makeFloat32ArrayCopy(ctx, host.ptr(), host.size());
            }, 1);
}

// ═══════════════════════════════════════════════════════════════════════════
// Free-function helpers
// ═══════════════════════════════════════════════════════════════════════════

#define ENSURE_INIT() do { nngpu::cuda_init(); } while (0)

static JSValue js_init(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    nngpu::cuda_init();
    return JS_UNDEFINED;
}

static JSValue js_sync(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    nngpu::cuda_sync();
    return JS_UNDEFINED;
}

static JSValue js_createTensor(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    ENSURE_INIT();
    int r = 0, c = 1;
    if (argc >= 1) JS_ToInt32(ctx, &r, argv[0]);
    if (argc >= 2) JS_ToInt32(ctx, &c, argv[1]);
    if (r < 0 || c < 0) return JS_ThrowRangeError(ctx, "negative dim");
    auto* d = new GpuTensorData();
    if (r > 0 && c > 0) d->t = nngpu::GpuTensor(r, c);
    return qjsbind::wrap<GpuTensorData>(ctx, d);
}

// Macro shortcuts to fetch arg #i as GpuTensor* into name. On failure throws.
#define GT(name, idx, label) \
    auto* name = gpuTensorFromJSLocal(ctx, argv[idx]); \
    if (!name) return JS_ThrowTypeError(ctx, label " — expected GpuTensors")

// ─── Dense + elementwise ──────────────────────────────────────────────────

static JSValue js_linearForward(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 4) return JS_ThrowTypeError(ctx, "linearForward(W,b,x,y)");
    ENSURE_INIT();
    GT(W, 0, "linearForward(W,b,x,y)");
    GT(b, 1, "linearForward(W,b,x,y)");
    GT(x, 2, "linearForward(W,b,x,y)");
    GT(y, 3, "linearForward(W,b,x,y)");
    nngpu::linear_forward_gpu(*W, *b, *x, *y);
    return JS_UNDEFINED;
}

static JSValue js_linearBackward(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 6) return JS_ThrowTypeError(ctx, "linearBackward(W,x,dY,dX,dW,dB)");
    ENSURE_INIT();
    GT(W,  0, "linearBackward");
    GT(x,  1, "linearBackward");
    GT(dY, 2, "linearBackward");
    GT(dX, 3, "linearBackward");
    GT(dW, 4, "linearBackward");
    GT(dB, 5, "linearBackward");
    nngpu::linear_backward_gpu(*W, *x, *dY, *dX, *dW, *dB);
    return JS_UNDEFINED;
}

static JSValue js_reluForward(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 2) return JS_ThrowTypeError(ctx, "reluForward(x,y)");
    ENSURE_INIT();
    GT(x, 0, "reluForward"); GT(y, 1, "reluForward");
    nngpu::relu_forward_gpu(*x, *y);
    return JS_UNDEFINED;
}
static JSValue js_reluBackward(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 3) return JS_ThrowTypeError(ctx, "reluBackward(x,dY,dX)");
    ENSURE_INIT();
    GT(x, 0, "reluBackward"); GT(dY, 1, "reluBackward"); GT(dX, 2, "reluBackward");
    nngpu::relu_backward_gpu(*x, *dY, *dX);
    return JS_UNDEFINED;
}
static JSValue js_tanhForward(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 2) return JS_ThrowTypeError(ctx, "tanhForward(x,y)");
    ENSURE_INIT();
    GT(x, 0, "tanhForward"); GT(y, 1, "tanhForward");
    nngpu::tanh_forward_gpu(*x, *y);
    return JS_UNDEFINED;
}
static JSValue js_tanhBackward(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 3) return JS_ThrowTypeError(ctx, "tanhBackward(y,dY,dX)");
    ENSURE_INIT();
    GT(y, 0, "tanhBackward"); GT(dY, 1, "tanhBackward"); GT(dX, 2, "tanhBackward");
    nngpu::tanh_backward_gpu(*y, *dY, *dX);
    return JS_UNDEFINED;
}
static JSValue js_sigmoidForward(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 2) return JS_ThrowTypeError(ctx, "sigmoidForward(x,y)");
    ENSURE_INIT();
    GT(x, 0, "sigmoidForward"); GT(y, 1, "sigmoidForward");
    nngpu::sigmoid_forward_gpu(*x, *y);
    return JS_UNDEFINED;
}
static JSValue js_sigmoidBackward(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 3) return JS_ThrowTypeError(ctx, "sigmoidBackward(y,dY,dX)");
    ENSURE_INIT();
    GT(y, 0, "sigmoidBackward"); GT(dY, 1, "sigmoidBackward"); GT(dX, 2, "sigmoidBackward");
    nngpu::sigmoid_backward_gpu(*y, *dY, *dX);
    return JS_UNDEFINED;
}

static JSValue js_addInplace(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 2) return JS_ThrowTypeError(ctx, "addInplace(y,x)");
    ENSURE_INIT();
    GT(y, 0, "addInplace"); GT(x, 1, "addInplace");
    nngpu::add_inplace_gpu(*y, *x);
    return JS_UNDEFINED;
}
static JSValue js_addScalarInplace(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 2) return JS_ThrowTypeError(ctx, "addScalarInplace(y,s)");
    ENSURE_INIT();
    GT(y, 0, "addScalarInplace");
    double s = 0; JS_ToFloat64(ctx, &s, argv[1]);
    nngpu::add_scalar_inplace_gpu(*y, (float)s);
    return JS_UNDEFINED;
}

// ─── Softmax ──────────────────────────────────────────────────────────────

static JSValue js_softmaxForward(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 2) return JS_ThrowTypeError(ctx, "softmaxForward(logits,probs,mask?)");
    ENSURE_INIT();
    GT(l, 0, "softmaxForward"); GT(p, 1, "softmaxForward");
    const float* mask = nullptr;
    if (argc >= 3) {
        JSValue err = JS_UNDEFINED;
        if (!resolveDeviceMask(ctx, argv[2], mask, err)) return err;
    }
    nngpu::softmax_forward_gpu(*l, *p, mask);
    return JS_UNDEFINED;
}

static JSValue js_softmaxBackward(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 3) return JS_ThrowTypeError(ctx, "softmaxBackward(probs,dProbs,dLogits)");
    ENSURE_INIT();
    GT(p, 0, "softmaxBackward"); GT(dp, 1, "softmaxBackward"); GT(dl, 2, "softmaxBackward");
    nngpu::softmax_backward_gpu(*p, *dp, *dl);
    return JS_UNDEFINED;
}

// ─── LayerNorm ────────────────────────────────────────────────────────────

static JSValue js_layernormForward(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 6) return JS_ThrowTypeError(ctx, "layernormForward(x,gamma,beta,y,xhat,eps)");
    ENSURE_INIT();
    GT(x,    0, "layernormForward");
    GT(g,    1, "layernormForward");
    GT(b,    2, "layernormForward");
    GT(y,    3, "layernormForward");
    GT(xhat, 4, "layernormForward");
    double eps = 1e-5; JS_ToFloat64(ctx, &eps, argv[5]);
    float mean = 0.f, rstd = 0.f;
    nngpu::layernorm_forward_gpu(*x, *g, *b, *y, *xhat, mean, rstd, (float)eps);
    JSValue out = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, out, "mean", JS_NewFloat64(ctx, (double)mean));
    JS_SetPropertyStr(ctx, out, "rstd", JS_NewFloat64(ctx, (double)rstd));
    return out;
}

static JSValue js_layernormBackward(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 7) return JS_ThrowTypeError(ctx, "layernormBackward(dY,xhat,gamma,rstd,dX,dGamma,dBeta)");
    ENSURE_INIT();
    GT(dY,     0, "layernormBackward");
    GT(xhat,   1, "layernormBackward");
    GT(g,      2, "layernormBackward");
    double rstd = 0; JS_ToFloat64(ctx, &rstd, argv[3]);
    GT(dX,     4, "layernormBackward");
    GT(dGamma, 5, "layernormBackward");
    GT(dBeta,  6, "layernormBackward");
    nngpu::layernorm_backward_gpu(*dY, *xhat, *g, (float)rstd, *dX, *dGamma, *dBeta);
    return JS_UNDEFINED;
}

// ─── Single-head attention ────────────────────────────────────────────────

static JSValue js_attentionForward(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 12) return JS_ThrowTypeError(ctx,
        "attentionForward(X,Wq,Wk,Wv,Wo,mask|null,Q,K,V,Attn,Y_pre_Wo,O)");
    ENSURE_INIT();
    GT(X,  0, "attentionForward");
    GT(Wq, 1, "attentionForward");
    GT(Wk, 2, "attentionForward");
    GT(Wv, 3, "attentionForward");
    GT(Wo, 4, "attentionForward");
    const float* mask = nullptr;
    JSValue err = JS_UNDEFINED;
    if (!resolveDeviceMask(ctx, argv[5], mask, err)) return err;
    GT(Q,  6, "attentionForward");
    GT(K,  7, "attentionForward");
    GT(V,  8, "attentionForward");
    GT(A,  9, "attentionForward");
    GT(Yp, 10, "attentionForward");
    GT(O,  11, "attentionForward");
    nngpu::attention_forward_gpu(*X, *Wq, *Wk, *Wv, *Wo, mask, *Q, *K, *V, *A, *Yp, *O);
    return JS_UNDEFINED;
}

static JSValue js_attentionBackward(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 17) return JS_ThrowTypeError(ctx,
        "attentionBackward(dO,X,Q,K,V,Attn,Y_pre_Wo,Wq,Wk,Wv,Wo,mask|null,dX,dWq,dWk,dWv,dWo)");
    ENSURE_INIT();
    GT(dO, 0,  "attentionBackward");
    GT(X,  1,  "attentionBackward");
    GT(Q,  2,  "attentionBackward");
    GT(K,  3,  "attentionBackward");
    GT(V,  4,  "attentionBackward");
    GT(A,  5,  "attentionBackward");
    GT(Yp, 6,  "attentionBackward");
    GT(Wq, 7,  "attentionBackward");
    GT(Wk, 8,  "attentionBackward");
    GT(Wv, 9,  "attentionBackward");
    GT(Wo, 10, "attentionBackward");
    const float* mask = nullptr;
    JSValue err = JS_UNDEFINED;
    if (!resolveDeviceMask(ctx, argv[11], mask, err)) return err;
    GT(dX,  12, "attentionBackward");
    GT(dWq, 13, "attentionBackward");
    GT(dWk, 14, "attentionBackward");
    GT(dWv, 15, "attentionBackward");
    GT(dWo, 16, "attentionBackward");
    nngpu::attention_backward_gpu(*dO, *X, *Q, *K, *V, *A, *Yp,
                                  *Wq, *Wk, *Wv, *Wo, mask,
                                  *dX, *dWq, *dWk, *dWv, *dWo);
    return JS_UNDEFINED;
}

// ─── Multi-head attention ─────────────────────────────────────────────────

static JSValue js_mhaForward(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 13) return JS_ThrowTypeError(ctx,
        "mhaForward(X,Wq,Wk,Wv,Wo,mask|null,numHeads,Qh,Kh,Vh,Attnh,Yconcat,O)");
    ENSURE_INIT();
    GT(X,  0, "mhaForward");
    GT(Wq, 1, "mhaForward");
    GT(Wk, 2, "mhaForward");
    GT(Wv, 3, "mhaForward");
    GT(Wo, 4, "mhaForward");
    const float* mask = nullptr;
    JSValue err = JS_UNDEFINED;
    if (!resolveDeviceMask(ctx, argv[5], mask, err)) return err;
    int32_t numHeads = 1; JS_ToInt32(ctx, &numHeads, argv[6]);
    GT(Qh,    7,  "mhaForward");
    GT(Kh,    8,  "mhaForward");
    GT(Vh,    9,  "mhaForward");
    GT(Attnh, 10, "mhaForward");
    GT(Yc,    11, "mhaForward");
    GT(O,     12, "mhaForward");
    nngpu::mha_forward_gpu(*X, *Wq, *Wk, *Wv, *Wo, mask, numHeads,
                           *Qh, *Kh, *Vh, *Attnh, *Yc, *O);
    return JS_UNDEFINED;
}

static JSValue js_mhaBackward(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 18) return JS_ThrowTypeError(ctx,
        "mhaBackward(dO,X,Qh,Kh,Vh,Attnh,Yconcat,Wq,Wk,Wv,Wo,mask|null,numHeads,dX,dWq,dWk,dWv,dWo)");
    ENSURE_INIT();
    GT(dO,    0, "mhaBackward");
    GT(X,     1, "mhaBackward");
    GT(Qh,    2, "mhaBackward");
    GT(Kh,    3, "mhaBackward");
    GT(Vh,    4, "mhaBackward");
    GT(Attnh, 5, "mhaBackward");
    GT(Yc,    6, "mhaBackward");
    GT(Wq,    7, "mhaBackward");
    GT(Wk,    8, "mhaBackward");
    GT(Wv,    9, "mhaBackward");
    GT(Wo,   10, "mhaBackward");
    const float* mask = nullptr;
    JSValue err = JS_UNDEFINED;
    if (!resolveDeviceMask(ctx, argv[11], mask, err)) return err;
    int32_t numHeads = 1; JS_ToInt32(ctx, &numHeads, argv[12]);
    GT(dX,   13, "mhaBackward");
    GT(dWq,  14, "mhaBackward");
    GT(dWk,  15, "mhaBackward");
    GT(dWv,  16, "mhaBackward");
    GT(dWo,  17, "mhaBackward");
    nngpu::mha_backward_gpu(*dO, *X, *Qh, *Kh, *Vh, *Attnh, *Yc,
                            *Wq, *Wk, *Wv, *Wo, mask, numHeads,
                            *dX, *dWq, *dWk, *dWv, *dWo);
    return JS_UNDEFINED;
}

// ─── Pooling, losses, embedding, concat ───────────────────────────────────

static JSValue js_maskedMeanPoolForward(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 3) return JS_ThrowTypeError(ctx, "maskedMeanPoolForward(X,mask|null,y)");
    ENSURE_INIT();
    GT(X, 0, "maskedMeanPoolForward");
    const float* mask = nullptr;
    JSValue err = JS_UNDEFINED;
    if (!resolveDeviceMask(ctx, argv[1], mask, err)) return err;
    GT(y, 2, "maskedMeanPoolForward");
    nngpu::masked_mean_pool_forward_gpu(*X, mask, *y);
    return JS_UNDEFINED;
}

static JSValue js_maskedMeanPoolBackward(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 4) return JS_ThrowTypeError(ctx, "maskedMeanPoolBackward(dY,mask|null,K,dX)");
    ENSURE_INIT();
    GT(dY, 0, "maskedMeanPoolBackward");
    const float* mask = nullptr;
    JSValue err = JS_UNDEFINED;
    if (!resolveDeviceMask(ctx, argv[1], mask, err)) return err;
    int32_t K = 0; JS_ToInt32(ctx, &K, argv[2]);
    GT(dX, 3, "maskedMeanPoolBackward");
    nngpu::masked_mean_pool_backward_gpu(*dY, mask, K, *dX);
    return JS_UNDEFINED;
}

static JSValue js_mseVecForward(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 2) return JS_ThrowTypeError(ctx, "mseVecForward(pred,target)");
    ENSURE_INIT();
    GT(p, 0, "mseVecForward"); GT(t, 1, "mseVecForward");
    float loss = nngpu::mse_vec_forward_gpu(*p, *t);
    return JS_NewFloat64(ctx, (double)loss);
}

static JSValue js_mseVecBackward(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 3) return JS_ThrowTypeError(ctx, "mseVecBackward(pred,target,dPred)");
    ENSURE_INIT();
    GT(p, 0, "mseVecBackward"); GT(t, 1, "mseVecBackward"); GT(dp, 2, "mseVecBackward");
    nngpu::mse_vec_backward_gpu(*p, *t, *dp);
    return JS_UNDEFINED;
}

static JSValue js_softmaxXentFused(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 5) return JS_ThrowTypeError(ctx,
        "softmaxXentFused(logits,target,mask|null,probs,dLogits)");
    ENSURE_INIT();
    GT(l, 0, "softmaxXentFused"); GT(t, 1, "softmaxXentFused");
    const float* mask = nullptr;
    JSValue err = JS_UNDEFINED;
    if (!resolveDeviceMask(ctx, argv[2], mask, err)) return err;
    GT(p, 3, "softmaxXentFused"); GT(dl, 4, "softmaxXentFused");
    float loss = nngpu::softmax_xent_fused_gpu(*l, *t, mask, *p, *dl);
    return JS_NewFloat64(ctx, (double)loss);
}

// idx is a GpuTensor whose `.data` is reinterpreted as int32_t* (its size
// must equal B). This keeps the convention symmetric with mask args:
// device-resident integer buffer wrapped as a GpuTensor.
static JSValue js_embeddingLookupForward(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 4) return JS_ThrowTypeError(ctx,
        "embeddingLookupForward(table,idx,B,out) — idx is a GpuTensor viewed as int32 storage");
    ENSURE_INIT();
    GT(tbl, 0, "embeddingLookupForward");
    GT(idx, 1, "embeddingLookupForward");
    int32_t B = 0; JS_ToInt32(ctx, &B, argv[2]);
    GT(out, 3, "embeddingLookupForward");
    const int32_t* d_idx = reinterpret_cast<const int32_t*>(idx->data);
    nngpu::embedding_lookup_forward_gpu(*tbl, d_idx, B, *out);
    return JS_UNDEFINED;
}

static JSValue js_embeddingLookupBackward(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 4) return JS_ThrowTypeError(ctx,
        "embeddingLookupBackward(dOut,idx,B,dTable) — idx is a GpuTensor viewed as int32 storage");
    ENSURE_INIT();
    GT(dOut, 0, "embeddingLookupBackward");
    GT(idx,  1, "embeddingLookupBackward");
    int32_t B = 0; JS_ToInt32(ctx, &B, argv[2]);
    GT(dTbl, 3, "embeddingLookupBackward");
    const int32_t* d_idx = reinterpret_cast<const int32_t*>(idx->data);
    nngpu::embedding_lookup_backward_gpu(*dOut, d_idx, B, *dTbl);
    return JS_UNDEFINED;
}

// Read a JS array of GpuTensors. Returns true on success.
static bool readGpuTensorArray(JSContext* ctx, JSValueConst v,
                               std::vector<const nngpu::GpuTensor*>& outConst,
                               std::vector<nngpu::GpuTensor*>* outMut /* may be null */) {
    if (!JS_IsArray(v)) return false;
    uint32_t len = 0;
    JSValue lv = JS_GetPropertyStr(ctx, v, "length");
    JS_ToUint32(ctx, &len, lv);
    JS_FreeValue(ctx, lv);
    outConst.clear();
    outConst.reserve(len);
    if (outMut) { outMut->clear(); outMut->reserve(len); }
    for (uint32_t i = 0; i < len; ++i) {
        JSValue ev = JS_GetPropertyUint32(ctx, v, i);
        auto* gt = gpuTensorFromJSLocal(ctx, ev);
        JS_FreeValue(ctx, ev);
        if (!gt) return false;
        outConst.push_back(gt);
        if (outMut) outMut->push_back(gt);
    }
    return true;
}

static JSValue js_concatRows(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 2) return JS_ThrowTypeError(ctx, "concatRows([parts...], out)");
    ENSURE_INIT();
    std::vector<const nngpu::GpuTensor*> parts;
    if (!readGpuTensorArray(ctx, argv[0], parts, nullptr))
        return JS_ThrowTypeError(ctx, "concatRows: first arg must be array of GpuTensors");
    GT(out, 1, "concatRows");
    nngpu::concat_rows_gpu(parts, *out);
    return JS_UNDEFINED;
}

static JSValue js_splitRows(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 2) return JS_ThrowTypeError(ctx, "splitRows(in, [parts...])");
    ENSURE_INIT();
    GT(in, 0, "splitRows");
    std::vector<const nngpu::GpuTensor*> partsConst;
    std::vector<nngpu::GpuTensor*> partsMut;
    if (!readGpuTensorArray(ctx, argv[1], partsConst, &partsMut))
        return JS_ThrowTypeError(ctx, "splitRows: second arg must be array of GpuTensors");
    nngpu::split_rows_gpu(*in, partsMut);
    return JS_UNDEFINED;
}

// ─── Optimisers ───────────────────────────────────────────────────────────

static JSValue js_sgdStep(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 5) return JS_ThrowTypeError(ctx, "sgdStep(param,grad,velocity,lr,momentum)");
    ENSURE_INIT();
    GT(p, 0, "sgdStep"); GT(g, 1, "sgdStep"); GT(v, 2, "sgdStep");
    double lr = 0, m = 0;
    JS_ToFloat64(ctx, &lr, argv[3]);
    JS_ToFloat64(ctx, &m, argv[4]);
    nngpu::sgd_step_gpu(*p, *g, *v, (float)lr, (float)m);
    return JS_UNDEFINED;
}

static JSValue js_adamStep(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 9) return JS_ThrowTypeError(ctx,
        "adamStep(param,grad,m,v,lr,beta1,beta2,eps,step)");
    ENSURE_INIT();
    GT(p, 0, "adamStep"); GT(g, 1, "adamStep"); GT(m, 2, "adamStep"); GT(v, 3, "adamStep");
    double lr = 0, b1 = 0, b2 = 0, eps = 0;
    JS_ToFloat64(ctx, &lr,  argv[4]);
    JS_ToFloat64(ctx, &b1,  argv[5]);
    JS_ToFloat64(ctx, &b2,  argv[6]);
    JS_ToFloat64(ctx, &eps, argv[7]);
    int32_t step = 1; JS_ToInt32(ctx, &step, argv[8]);
    nngpu::adam_step_gpu(*p, *g, *m, *v, (float)lr, (float)b1, (float)b2, (float)eps, step);
    return JS_UNDEFINED;
}

// ─── Batched (inference-only) variants ────────────────────────────────────

static JSValue js_linearForwardBatched(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 4) return JS_ThrowTypeError(ctx, "linearForwardBatched(W,bias,X_BD,Y_BD)");
    ENSURE_INIT();
    GT(W, 0, "linearForwardBatched");
    GT(b, 1, "linearForwardBatched");
    GT(X, 2, "linearForwardBatched");
    GT(Y, 3, "linearForwardBatched");
    nngpu::linear_forward_batched_gpu(*W, *b, *X, *Y);
    return JS_UNDEFINED;
}
static JSValue js_reluForwardBatched(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 2) return JS_ThrowTypeError(ctx, "reluForwardBatched(X_BD,Y_BD)");
    ENSURE_INIT();
    GT(X, 0, "reluForwardBatched"); GT(Y, 1, "reluForwardBatched");
    nngpu::relu_forward_batched_gpu(*X, *Y);
    return JS_UNDEFINED;
}
static JSValue js_tanhForwardBatched(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 2) return JS_ThrowTypeError(ctx, "tanhForwardBatched(X_BD,Y_BD)");
    ENSURE_INIT();
    GT(X, 0, "tanhForwardBatched"); GT(Y, 1, "tanhForwardBatched");
    nngpu::tanh_forward_batched_gpu(*X, *Y);
    return JS_UNDEFINED;
}
static JSValue js_addInplaceBatched(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 2) return JS_ThrowTypeError(ctx, "addInplaceBatched(Y_BD,X_BD)");
    ENSURE_INIT();
    GT(Y, 0, "addInplaceBatched"); GT(X, 1, "addInplaceBatched");
    nngpu::add_inplace_batched_gpu(*Y, *X);
    return JS_UNDEFINED;
}

#undef GT
#undef ENSURE_INIT

// ═══════════════════════════════════════════════════════════════════════════
// Install
// ═══════════════════════════════════════════════════════════════════════════

void installNNGpuBindings(JSContext* ctx, JSValue nnObj) {
    registerClasses(ctx);

    JSValue gpuObj = JS_NewObject(ctx);

    // Capability flag.
    JS_SetPropertyStr(ctx, gpuObj, "available", JS_TRUE);

    // Runtime
    JS_SetPropertyStr(ctx, gpuObj, "init", JS_NewCFunction(ctx, js_init, "init", 0));
    JS_SetPropertyStr(ctx, gpuObj, "sync", JS_NewCFunction(ctx, js_sync, "sync", 0));

    // Factory
    JS_SetPropertyStr(ctx, gpuObj, "createTensor",
        JS_NewCFunction(ctx, js_createTensor, "createTensor", 2));

    // Dense + elementwise
    JS_SetPropertyStr(ctx, gpuObj, "linearForward",      JS_NewCFunction(ctx, js_linearForward,      "linearForward",      4));
    JS_SetPropertyStr(ctx, gpuObj, "linearBackward",     JS_NewCFunction(ctx, js_linearBackward,     "linearBackward",     6));
    JS_SetPropertyStr(ctx, gpuObj, "reluForward",        JS_NewCFunction(ctx, js_reluForward,        "reluForward",        2));
    JS_SetPropertyStr(ctx, gpuObj, "reluBackward",       JS_NewCFunction(ctx, js_reluBackward,       "reluBackward",       3));
    JS_SetPropertyStr(ctx, gpuObj, "tanhForward",        JS_NewCFunction(ctx, js_tanhForward,        "tanhForward",        2));
    JS_SetPropertyStr(ctx, gpuObj, "tanhBackward",       JS_NewCFunction(ctx, js_tanhBackward,       "tanhBackward",       3));
    JS_SetPropertyStr(ctx, gpuObj, "sigmoidForward",     JS_NewCFunction(ctx, js_sigmoidForward,     "sigmoidForward",     2));
    JS_SetPropertyStr(ctx, gpuObj, "sigmoidBackward",    JS_NewCFunction(ctx, js_sigmoidBackward,    "sigmoidBackward",    3));
    JS_SetPropertyStr(ctx, gpuObj, "addInplace",         JS_NewCFunction(ctx, js_addInplace,         "addInplace",         2));
    JS_SetPropertyStr(ctx, gpuObj, "addScalarInplace",   JS_NewCFunction(ctx, js_addScalarInplace,   "addScalarInplace",   2));

    // Softmax
    JS_SetPropertyStr(ctx, gpuObj, "softmaxForward",     JS_NewCFunction(ctx, js_softmaxForward,     "softmaxForward",     3));
    JS_SetPropertyStr(ctx, gpuObj, "softmaxBackward",    JS_NewCFunction(ctx, js_softmaxBackward,    "softmaxBackward",    3));

    // LayerNorm
    JS_SetPropertyStr(ctx, gpuObj, "layernormForward",   JS_NewCFunction(ctx, js_layernormForward,   "layernormForward",   6));
    JS_SetPropertyStr(ctx, gpuObj, "layernormBackward",  JS_NewCFunction(ctx, js_layernormBackward,  "layernormBackward",  7));

    // Single-head attention
    JS_SetPropertyStr(ctx, gpuObj, "attentionForward",   JS_NewCFunction(ctx, js_attentionForward,   "attentionForward",   12));
    JS_SetPropertyStr(ctx, gpuObj, "attentionBackward",  JS_NewCFunction(ctx, js_attentionBackward,  "attentionBackward",  17));

    // MHA
    JS_SetPropertyStr(ctx, gpuObj, "mhaForward",         JS_NewCFunction(ctx, js_mhaForward,         "mhaForward",         13));
    JS_SetPropertyStr(ctx, gpuObj, "mhaBackward",        JS_NewCFunction(ctx, js_mhaBackward,        "mhaBackward",        18));

    // Pooling, losses, embedding, concat
    JS_SetPropertyStr(ctx, gpuObj, "maskedMeanPoolForward",  JS_NewCFunction(ctx, js_maskedMeanPoolForward,  "maskedMeanPoolForward",  3));
    JS_SetPropertyStr(ctx, gpuObj, "maskedMeanPoolBackward", JS_NewCFunction(ctx, js_maskedMeanPoolBackward, "maskedMeanPoolBackward", 4));
    JS_SetPropertyStr(ctx, gpuObj, "mseVecForward",          JS_NewCFunction(ctx, js_mseVecForward,          "mseVecForward",          2));
    JS_SetPropertyStr(ctx, gpuObj, "mseVecBackward",         JS_NewCFunction(ctx, js_mseVecBackward,         "mseVecBackward",         3));
    JS_SetPropertyStr(ctx, gpuObj, "softmaxXentFused",       JS_NewCFunction(ctx, js_softmaxXentFused,       "softmaxXentFused",       5));
    JS_SetPropertyStr(ctx, gpuObj, "embeddingLookupForward", JS_NewCFunction(ctx, js_embeddingLookupForward, "embeddingLookupForward", 4));
    JS_SetPropertyStr(ctx, gpuObj, "embeddingLookupBackward",JS_NewCFunction(ctx, js_embeddingLookupBackward,"embeddingLookupBackward",4));
    JS_SetPropertyStr(ctx, gpuObj, "concatRows",             JS_NewCFunction(ctx, js_concatRows,             "concatRows",             2));
    JS_SetPropertyStr(ctx, gpuObj, "splitRows",              JS_NewCFunction(ctx, js_splitRows,              "splitRows",              2));

    // Optimisers
    JS_SetPropertyStr(ctx, gpuObj, "sgdStep",   JS_NewCFunction(ctx, js_sgdStep,   "sgdStep",   5));
    JS_SetPropertyStr(ctx, gpuObj, "adamStep",  JS_NewCFunction(ctx, js_adamStep,  "adamStep",  9));

    // Batched (inference)
    JS_SetPropertyStr(ctx, gpuObj, "linearForwardBatched", JS_NewCFunction(ctx, js_linearForwardBatched, "linearForwardBatched", 4));
    JS_SetPropertyStr(ctx, gpuObj, "reluForwardBatched",   JS_NewCFunction(ctx, js_reluForwardBatched,   "reluForwardBatched",   2));
    JS_SetPropertyStr(ctx, gpuObj, "tanhForwardBatched",   JS_NewCFunction(ctx, js_tanhForwardBatched,   "tanhForwardBatched",   2));
    JS_SetPropertyStr(ctx, gpuObj, "addInplaceBatched",    JS_NewCFunction(ctx, js_addInplaceBatched,    "addInplaceBatched",    2));

    JS_SetPropertyStr(ctx, nnObj, "gpu", gpuObj);
}

// Cross-binding unwrap helper (declared in ai_bindings.h).
nngpu::GpuTensor* gpuTensorFromJS(JSContext* ctx, JSValueConst v) {
    return gpuTensorFromJSLocal(ctx, v);
}

} // namespace bro::js

#else // !BGA_HAS_CUDA

namespace brogameagent::nn::gpu { struct GpuTensor; }

namespace bro::js {

void installNNGpuBindings(JSContext* ctx, JSValue nnObj) {
    JSValue gpuObj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, gpuObj, "available", JS_FALSE);
    JS_SetPropertyStr(ctx, nnObj, "gpu", gpuObj);
}

brogameagent::nn::gpu::GpuTensor* gpuTensorFromJS(JSContext*, JSValueConst) {
    return nullptr;
}

} // namespace bro::js

#endif // BGA_HAS_CUDA
