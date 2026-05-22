// JS bindings for brotensor — CUDA or Metal GPU tensor + ops.
//
// Installed onto bro.tensor.* by installTensorBindings(). The op surface is
// split across several TUs to keep individual files in the few-hundred-LOC
// range:
//
//   tensor_bindings.cpp            — class, runtime, factory; orchestrates the
//                                    cluster installers; "core" ops (linear,
//                                    relu/tanh/sigmoid, softmax, layernorm,
//                                    attention/mha, optim, batched, embedding,
//                                    concat/split, masked-mean-pool, mse, fused
//                                    softmax-xent, reductions, batched-train
//                                    backwards, copy_d2d / scale / clamp /
//                                    build_slot_mask / mul_inplace /
//                                    layernorm_inference_batched(+fp16),
//                                    softmax_xent_fused_batched,
//                                    mse_vec_per_sample,
//                                    linear_forward_batched_fp16,
//                                    concat_batched_rows,
//                                    concat_nchw_channels(+backward) — too
//                                    interrelated with concat_rows to split)
//   tensor_bindings_activations.cpp — silu, gelu, gelu_exact, quick_gelu,
//                                    swiglu, geglu, geglu_exact F/B;
//                                    rms_norm F/B, group_norm F/B;
//                                    matmul, matmul_backward; rope F/B;
//                                    build_causal_mask_row.
//   tensor_bindings_attention.cpp  — self/cross attention (fp16+fp32 train);
//                                    flash attention family;
//                                    attention_token_moments; kv_cache_append;
//                                    resblock F/B.
//   tensor_bindings_conv.cpp       — conv2d F/B; up/downsample 2x F/B;
//                                    nchw↔seq transpose.
//   tensor_bindings_diffusion.cpp  — ddim_step, euler_step, dpmpp_2m_step,
//                                    timestep_embedding.
//   tensor_bindings_int8.cpp       — INT8 quant helpers + W8A16 ops.
//
// Compilation gating:
//   brotensor publishes BROTENSOR_HAS_GPU=1 when either CUDA or Metal is
//   enabled (BROTENSOR_HAS_CUDA / BROTENSOR_HAS_METAL identify the specific
//   backend). The public op surface is identical across backends, so we gate
//   on the umbrella define here. Without GPU support installTensorBindings
//   publishes a stub namespace with `available: false` so JS code can detect
//   it.

#include "js/ai_bindings.h"

#include <qjsbind/qjsbind.h>

#ifdef BROTENSOR_HAS_GPU

#include "js/tensor_bindings_internal.h"

#include <cstdint>
#include <cstring>
#include <vector>

namespace bro::js {

using qjsbind::make_float32_array;

// ═══════════════════════════════════════════════════════════════════════════
// Dtype helpers (FP32 / FP16 / INT8)
// ═══════════════════════════════════════════════════════════════════════════

static nngpu::Dtype parseDtype(JSContext* ctx, JSValueConst v, nngpu::Dtype def) {
    if (JS_IsUndefined(v) || JS_IsNull(v)) return def;
    if (JS_IsString(v)) {
        const char* s = JS_ToCString(ctx, v);
        nngpu::Dtype out = def;
        if (s) {
            if      (!std::strcmp(s, "fp32") || !std::strcmp(s, "f32")) out = nngpu::Dtype::FP32;
            else if (!std::strcmp(s, "fp16") || !std::strcmp(s, "f16")) out = nngpu::Dtype::FP16;
            else if (!std::strcmp(s, "bf16"))                          out = nngpu::Dtype::BF16;
            else if (!std::strcmp(s, "int8") || !std::strcmp(s, "i8"))  out = nngpu::Dtype::INT8;
            JS_FreeCString(ctx, s);
        }
        return out;
    }
    int32_t i = static_cast<int32_t>(def);
    JS_ToInt32(ctx, &i, v);
    return static_cast<nngpu::Dtype>(i);
}

static const char* dtypeName(nngpu::Dtype dt) {
    switch (dt) {
        case nngpu::Dtype::FP32:  return "fp32";
        case nngpu::Dtype::FP16:  return "fp16";
        case nngpu::Dtype::BF16:  return "bf16";
        case nngpu::Dtype::INT8:  return "int8";
        case nngpu::Dtype::INT32: return "int32";
    }
    return "fp32";
}

// ═══════════════════════════════════════════════════════════════════════════
// Class registration
// ═══════════════════════════════════════════════════════════════════════════

static void registerClasses(JSContext* ctx) {
    qjsbind::Class<GpuTensorData>(ctx, "AIGpuTensor", qjsbind::NoGlobal)
        .get("rows", [](GpuTensorData* d) -> int { return d->t.rows; })
        .get("cols", [](GpuTensorData* d) -> int { return d->t.cols; })
        .get("size", [](GpuTensorData* d) -> int { return d->t.size(); })
        .get("bytes", [](GpuTensorData* d) -> int { return static_cast<int>(d->t.bytes()); })
        .method("zero", [](GpuTensorData* d) { d->t.zero(); })
        // resize(r, c, dtype?) — dtype defaults to "fp32".
        .method_raw("resize",
            [](JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) -> JSValue {
                auto* d = qjsbind::unwrap<GpuTensorData>(ctx, this_val);
                if (!d || argc < 2) return JS_ThrowTypeError(ctx, "resize(rows, cols, dtype?)");
                int32_t r = 0, c = 0;
                JS_ToInt32(ctx, &r, argv[0]);
                JS_ToInt32(ctx, &c, argv[1]);
                nngpu::Dtype dt = nngpu::Dtype::FP32;
                if (argc >= 3) dt = parseDtype(ctx, argv[2], nngpu::Dtype::FP32);
                d->t.resize(r, c, dt);
                return JS_UNDEFINED;
            }, 3)
        // dtype getter returns the string name.
        .method_raw("dtype",
            [](JSContext* ctx, JSValueConst this_val, int, JSValueConst*) -> JSValue {
                auto* d = qjsbind::unwrap<GpuTensorData>(ctx, this_val);
                if (!d) return JS_ThrowTypeError(ctx, "dtype() on non-GpuTensor");
                return JS_NewString(ctx, dtypeName(d->t.dtype));
            }, 0)
        .method_raw("clone",
            [](JSContext* ctx, JSValueConst this_val, int, JSValueConst*) -> JSValue {
                auto* d = qjsbind::unwrap<GpuTensorData>(ctx, this_val);
                if (!d) return JS_ThrowTypeError(ctx, "clone() on non-GpuTensor");
                auto* nd = new GpuTensorData();
                nd->t = d->t.clone();
                return qjsbind::wrap<GpuTensorData>(ctx, nd);
            }, 0)
        // upload(src): src is an AITensor (host, FP32) or a Float32Array.
        .method_raw("upload",
            [](JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) -> JSValue {
                auto* d = qjsbind::unwrap<GpuTensorData>(ctx, this_val);
                if (!d || argc < 1) return JS_ThrowTypeError(ctx, "upload(src) — expected Tensor or Float32Array");
                if (auto* ht = tensorFromJS(ctx, argv[0])) {
                    d->t = nngpu::Tensor::from_host(ht->ptr(), ht->rows, ht->cols);
                    return JS_UNDEFINED;
                }
                size_t n = 0;
                float* src = getFloatArrayPtr(ctx, argv[0], n);
                if (!src) return JS_ThrowTypeError(ctx, "upload(src) — expected Tensor or Float32Array");
                int r = d->t.rows, c = d->t.cols;
                if (r * c != (int)n) { r = (int)n; c = 1; }
                d->t = nngpu::Tensor::from_host(src, r, c);
                return JS_UNDEFINED;
            }, 1)
        // download(dst?): if dst is AITensor, download into it (auto-syncs);
        // else sync, download to a Float32Array. FP32 only — use downloadFp16
        // for FP16 tensors.
        .method_raw("download",
            [](JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) -> JSValue {
                auto* d = qjsbind::unwrap<GpuTensorData>(ctx, this_val);
                if (!d) return JS_ThrowTypeError(ctx, "download() on non-GpuTensor");
                nngpu::sync_all();
                if (argc >= 1 && !JS_IsUndefined(argv[0]) && !JS_IsNull(argv[0])) {
                    if (auto* ht = tensorFromJS(ctx, argv[0])) {
                        if (ht->rows != d->t.rows || ht->cols != d->t.cols) {
                            ht->resize(d->t.rows, d->t.cols);
                        }
                        d->t.copy_to_host(ht->ptr());
                        return JS_UNDEFINED;
                    }
                    return JS_ThrowTypeError(ctx, "download(dst): dst must be a Tensor");
                }
                std::vector<float> host(static_cast<size_t>(d->t.size()));
                d->t.copy_to_host(host.data());
                return make_float32_array(ctx, host.data(), static_cast<int>(host.size()));
            }, 1)
        // uploadFp16(uint16Array): src holds the IEEE binary16 bit pattern.
        // Resizes the destination to (rows, cols, FP16). If the destination
        // already has matching r*c, those are used; otherwise (n, 1) is used.
        .method_raw("uploadFp16",
            [](JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) -> JSValue {
                auto* d = qjsbind::unwrap<GpuTensorData>(ctx, this_val);
                if (!d || argc < 1) return JS_ThrowTypeError(ctx, "uploadFp16(uint16Array)");
                size_t bytes = 0;
                uint8_t* p = getTypedArrayBytePtr(ctx, argv[0], bytes);
                if (!p) return JS_ThrowTypeError(ctx, "uploadFp16(uint16Array) — expected Uint16Array");
                int n = static_cast<int>(bytes / sizeof(uint16_t));
                int r = d->t.rows, c = d->t.cols;
                if (r * c != n) { r = n; c = 1; }
                d->t = nngpu::Tensor::from_host_fp16(reinterpret_cast<const uint16_t*>(p), r, c);
                return JS_UNDEFINED;
            }, 1)
        // uploadInt8(int8Array): src holds raw int8 weight bytes — e.g. the
        // `weights` field returned by quantizeInt8PerRowHost. Resizes the
        // destination to (rows, cols, INT8); keeps the existing r*c when it
        // matches the element count, otherwise falls back to (n, 1).
        .method_raw("uploadInt8",
            [](JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) -> JSValue {
                auto* d = qjsbind::unwrap<GpuTensorData>(ctx, this_val);
                if (!d || argc < 1) return JS_ThrowTypeError(ctx, "uploadInt8(int8Array)");
                size_t bytes = 0;
                uint8_t* p = getTypedArrayBytePtr(ctx, argv[0], bytes);
                if (!p) return JS_ThrowTypeError(ctx, "uploadInt8(int8Array) — expected Int8Array");
                int n = static_cast<int>(bytes);  // INT8 → 1 byte per element
                int r = d->t.rows, c = d->t.cols;
                if (r * c != n) { r = n; c = 1; }
                d->t = nngpu::Tensor::from_host_int8(reinterpret_cast<const int8_t*>(p), r, c);
                return JS_UNDEFINED;
            }, 1)
        // downloadFp16(): sync + download FP16 bits into a fresh Uint16Array.
        .method_raw("downloadFp16",
            [](JSContext* ctx, JSValueConst this_val, int, JSValueConst*) -> JSValue {
                auto* d = qjsbind::unwrap<GpuTensorData>(ctx, this_val);
                if (!d) return JS_ThrowTypeError(ctx, "downloadFp16() on non-GpuTensor");
                nngpu::sync_all();
                std::vector<uint16_t> host(static_cast<size_t>(d->t.size()));
                d->t.copy_to_host_fp16(host.data());
                JSValue abuf = JS_NewArrayBufferCopy(
                    ctx, reinterpret_cast<const uint8_t*>(host.data()),
                    host.size() * sizeof(uint16_t));
                JSValue args[3] = { abuf, JS_UNDEFINED, JS_UNDEFINED };
                JSValue arr = JS_NewTypedArray(ctx, 1, args, JS_TYPED_ARRAY_UINT16);
                JS_FreeValue(ctx, abuf);
                return arr;
            }, 0);
}

// ═══════════════════════════════════════════════════════════════════════════
// Free-function helpers
// ═══════════════════════════════════════════════════════════════════════════

#define ENSURE_INIT() BROTENSOR_ENSURE_INIT()
#define GT(name, idx, label) BROTENSOR_GT(name, idx, label)

static JSValue js_init(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    nngpu::init();
    return JS_UNDEFINED;
}

static JSValue js_sync(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    nngpu::sync_all();
    return JS_UNDEFINED;
}

// createTensor(rows, cols, dtype?): dtype is one of "fp32" | "fp16" | "int8"
// or a numeric Dtype enum value. Defaults to FP32.
static JSValue js_createTensor(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    ENSURE_INIT();
    int r = 0, c = 1;
    if (argc >= 1) JS_ToInt32(ctx, &r, argv[0]);
    if (argc >= 2) JS_ToInt32(ctx, &c, argv[1]);
    if (r < 0 || c < 0) return JS_ThrowRangeError(ctx, "negative dim");
    nngpu::Dtype dt = nngpu::Dtype::FP32;
    if (argc >= 3) dt = parseDtype(ctx, argv[2], nngpu::Dtype::FP32);
    auto* d = new GpuTensorData();
    if (r > 0 && c > 0) d->t = nngpu::Tensor::zeros(r, c, dt);
    return qjsbind::wrap<GpuTensorData>(ctx, d);
}

// ─── Dense + elementwise ──────────────────────────────────────────────────

static JSValue js_linearForward(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 4) return JS_ThrowTypeError(ctx, "linearForward(W,b,x,y)");
    ENSURE_INIT();
    GT(W, 0, "linearForward"); GT(b, 1, "linearForward");
    GT(x, 2, "linearForward"); GT(y, 3, "linearForward");
    nngpu::linear_forward(*W, *b, *x, *y);
    return JS_UNDEFINED;
}

static JSValue js_linearBackward(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 6) return JS_ThrowTypeError(ctx, "linearBackward(W,x,dY,dX,dW,dB)");
    ENSURE_INIT();
    GT(W,  0, "linearBackward"); GT(x,  1, "linearBackward");
    GT(dY, 2, "linearBackward"); GT(dX, 3, "linearBackward");
    GT(dW, 4, "linearBackward"); GT(dB, 5, "linearBackward");
    nngpu::linear_backward(*W, *x, *dY, *dX, *dW, *dB);
    return JS_UNDEFINED;
}

static JSValue js_reluForward(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 2) return JS_ThrowTypeError(ctx, "reluForward(x,y)");
    ENSURE_INIT();
    GT(x, 0, "reluForward"); GT(y, 1, "reluForward");
    nngpu::relu_forward(*x, *y);
    return JS_UNDEFINED;
}
static JSValue js_reluBackward(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 3) return JS_ThrowTypeError(ctx, "reluBackward(x,dY,dX)");
    ENSURE_INIT();
    GT(x, 0, "reluBackward"); GT(dY, 1, "reluBackward"); GT(dX, 2, "reluBackward");
    nngpu::relu_backward(*x, *dY, *dX);
    return JS_UNDEFINED;
}
static JSValue js_tanhForward(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 2) return JS_ThrowTypeError(ctx, "tanhForward(x,y)");
    ENSURE_INIT();
    GT(x, 0, "tanhForward"); GT(y, 1, "tanhForward");
    nngpu::tanh_forward(*x, *y);
    return JS_UNDEFINED;
}
static JSValue js_tanhBackward(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 3) return JS_ThrowTypeError(ctx, "tanhBackward(y,dY,dX)");
    ENSURE_INIT();
    GT(y, 0, "tanhBackward"); GT(dY, 1, "tanhBackward"); GT(dX, 2, "tanhBackward");
    nngpu::tanh_backward(*y, *dY, *dX);
    return JS_UNDEFINED;
}
static JSValue js_sigmoidForward(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 2) return JS_ThrowTypeError(ctx, "sigmoidForward(x,y)");
    ENSURE_INIT();
    GT(x, 0, "sigmoidForward"); GT(y, 1, "sigmoidForward");
    nngpu::sigmoid_forward(*x, *y);
    return JS_UNDEFINED;
}
static JSValue js_sigmoidBackward(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 3) return JS_ThrowTypeError(ctx, "sigmoidBackward(y,dY,dX)");
    ENSURE_INIT();
    GT(y, 0, "sigmoidBackward"); GT(dY, 1, "sigmoidBackward"); GT(dX, 2, "sigmoidBackward");
    nngpu::sigmoid_backward(*y, *dY, *dX);
    return JS_UNDEFINED;
}

static JSValue js_addInplace(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 2) return JS_ThrowTypeError(ctx, "addInplace(y,x)");
    ENSURE_INIT();
    GT(y, 0, "addInplace"); GT(x, 1, "addInplace");
    nngpu::add_inplace(*y, *x);
    return JS_UNDEFINED;
}
static JSValue js_addScalarInplace(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 2) return JS_ThrowTypeError(ctx, "addScalarInplace(y,s)");
    ENSURE_INIT();
    GT(y, 0, "addScalarInplace");
    double s = 0; JS_ToFloat64(ctx, &s, argv[1]);
    nngpu::add_scalar_inplace(*y, (float)s);
    return JS_UNDEFINED;
}
static JSValue js_scaleInplace(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 2) return JS_ThrowTypeError(ctx, "scaleInplace(y,s)");
    ENSURE_INIT();
    GT(y, 0, "scaleInplace");
    double s = 0; JS_ToFloat64(ctx, &s, argv[1]);
    nngpu::scale_inplace(*y, (float)s);
    return JS_UNDEFINED;
}
static JSValue js_mulInplace(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 2) return JS_ThrowTypeError(ctx, "mulInplace(y,x)");
    ENSURE_INIT();
    GT(y, 0, "mulInplace"); GT(x, 1, "mulInplace");
    nngpu::mul_inplace(*y, *x);
    return JS_UNDEFINED;
}
static JSValue js_clamp(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 3) return JS_ThrowTypeError(ctx, "clamp(y,lo,hi)");
    ENSURE_INIT();
    GT(y, 0, "clamp");
    double lo = 0, hi = 0;
    JS_ToFloat64(ctx, &lo, argv[1]);
    JS_ToFloat64(ctx, &hi, argv[2]);
    nngpu::clamp(*y, (float)lo, (float)hi);
    return JS_UNDEFINED;
}
static JSValue js_buildSlotMask(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 5) return JS_ThrowTypeError(ctx, "buildSlotMask(x,offset,K,stride,mask)");
    ENSURE_INIT();
    GT(x, 0, "buildSlotMask");
    int32_t offset = 0, K = 0, stride = 1;
    JS_ToInt32(ctx, &offset, argv[1]);
    JS_ToInt32(ctx, &K,      argv[2]);
    JS_ToInt32(ctx, &stride, argv[3]);
    GT(mask, 4, "buildSlotMask");
    nngpu::build_slot_mask(*x, offset, K, stride, *mask);
    return JS_UNDEFINED;
}
static JSValue js_copyD2D(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 5) return JS_ThrowTypeError(ctx, "copyD2D(src,srcOff,dst,dstOff,n)");
    ENSURE_INIT();
    GT(src, 0, "copyD2D");
    int32_t srcOff = 0, dstOff = 0, n = 0;
    JS_ToInt32(ctx, &srcOff, argv[1]);
    GT(dst, 2, "copyD2D");
    JS_ToInt32(ctx, &dstOff, argv[3]);
    JS_ToInt32(ctx, &n,      argv[4]);
    nngpu::copy_d2d(*src, srcOff, *dst, dstOff, n);
    return JS_UNDEFINED;
}

// cast(src, dst, outDtype): dst = src converted to outDtype. dst is resized
// (and dtype-set) to (src.rows, src.cols, outDtype) and lands on src's device.
// Supports the FP32 <-> FP16 pair plus a same-dtype passthrough copy. outDtype
// is a dtype string ("fp32" | "fp16" | "int8") or numeric Dtype enum value.
static JSValue js_cast(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 3) return JS_ThrowTypeError(ctx, "cast(src,dst,outDtype)");
    ENSURE_INIT();
    GT(src, 0, "cast"); GT(dst, 1, "cast");
    nngpu::Dtype dt = parseDtype(ctx, argv[2], nngpu::Dtype::FP32);
    nngpu::cast(*src, *dst, dt);
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
    nngpu::softmax_forward(*l, *p, mask);
    return JS_UNDEFINED;
}

static JSValue js_softmaxBackward(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 3) return JS_ThrowTypeError(ctx, "softmaxBackward(probs,dProbs,dLogits)");
    ENSURE_INIT();
    GT(p, 0, "softmaxBackward"); GT(dp, 1, "softmaxBackward"); GT(dl, 2, "softmaxBackward");
    nngpu::softmax_backward(*p, *dp, *dl);
    return JS_UNDEFINED;
}

// ─── LayerNorm ────────────────────────────────────────────────────────────

static JSValue js_layernormForward(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 6) return JS_ThrowTypeError(ctx, "layernormForward(x,gamma,beta,y,xhat,eps)");
    ENSURE_INIT();
    GT(x,    0, "layernormForward"); GT(g,    1, "layernormForward");
    GT(b,    2, "layernormForward"); GT(y,    3, "layernormForward");
    GT(xhat, 4, "layernormForward");
    double eps = 1e-5; JS_ToFloat64(ctx, &eps, argv[5]);
    float mean = 0.f, rstd = 0.f;
    nngpu::layernorm_forward(*x, *g, *b, *y, *xhat, mean, rstd, (float)eps);
    JSValue out = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, out, "mean", JS_NewFloat64(ctx, (double)mean));
    JS_SetPropertyStr(ctx, out, "rstd", JS_NewFloat64(ctx, (double)rstd));
    return out;
}

static JSValue js_layernormBackward(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 7) return JS_ThrowTypeError(ctx,
        "layernormBackward(dY,xhat,gamma,rstd,dX,dGamma,dBeta)");
    ENSURE_INIT();
    GT(dY,     0, "layernormBackward"); GT(xhat,   1, "layernormBackward");
    GT(g,      2, "layernormBackward");
    double rstd = 0; JS_ToFloat64(ctx, &rstd, argv[3]);
    GT(dX,     4, "layernormBackward"); GT(dGamma, 5, "layernormBackward");
    GT(dBeta,  6, "layernormBackward");
    nngpu::layernorm_backward(*dY, *xhat, *g, (float)rstd, *dX, *dGamma, *dBeta);
    return JS_UNDEFINED;
}

// FP32 batched inference.
static JSValue js_layernormForwardInferenceBatched(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 5) return JS_ThrowTypeError(ctx,
        "layernormForwardInferenceBatched(X_RD,gamma,beta,Y_RD,eps)");
    ENSURE_INIT();
    GT(X, 0, "layernormForwardInferenceBatched");
    GT(g, 1, "layernormForwardInferenceBatched");
    GT(b, 2, "layernormForwardInferenceBatched");
    GT(Y, 3, "layernormForwardInferenceBatched");
    double eps = 1e-5; JS_ToFloat64(ctx, &eps, argv[4]);
    nngpu::layernorm_forward_inference_batched(*X, *g, *b, *Y, (float)eps);
    return JS_UNDEFINED;
}
static JSValue js_layernormForwardInferenceBatchedFp16(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 5) return JS_ThrowTypeError(ctx,
        "layernormForwardInferenceBatchedFp16(X_RD,gamma,beta,Y_RD,eps)");
    ENSURE_INIT();
    GT(X, 0, "layernormForwardInferenceBatchedFp16");
    GT(g, 1, "layernormForwardInferenceBatchedFp16");
    GT(b, 2, "layernormForwardInferenceBatchedFp16");
    GT(Y, 3, "layernormForwardInferenceBatchedFp16");
    double eps = 1e-5; JS_ToFloat64(ctx, &eps, argv[4]);
    nngpu::layernorm_forward_inference_batched_fp16(*X, *g, *b, *Y, (float)eps);
    return JS_UNDEFINED;
}

// ─── Single-head attention ────────────────────────────────────────────────

static JSValue js_attentionForward(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 12) return JS_ThrowTypeError(ctx,
        "attentionForward(X,Wq,Wk,Wv,Wo,mask|null,Q,K,V,Attn,Y_pre_Wo,O)");
    ENSURE_INIT();
    GT(X,  0, "attentionForward"); GT(Wq, 1, "attentionForward");
    GT(Wk, 2, "attentionForward"); GT(Wv, 3, "attentionForward");
    GT(Wo, 4, "attentionForward");
    const float* mask = nullptr; JSValue err = JS_UNDEFINED;
    if (!resolveDeviceMask(ctx, argv[5], mask, err)) return err;
    GT(Q,  6, "attentionForward"); GT(K,  7, "attentionForward");
    GT(V,  8, "attentionForward"); GT(A,  9, "attentionForward");
    GT(Yp, 10, "attentionForward"); GT(O,  11, "attentionForward");
    nngpu::attention_forward(*X, *Wq, *Wk, *Wv, *Wo, mask, *Q, *K, *V, *A, *Yp, *O);
    return JS_UNDEFINED;
}

static JSValue js_attentionBackward(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 17) return JS_ThrowTypeError(ctx,
        "attentionBackward(dO,X,Q,K,V,Attn,Y_pre_Wo,Wq,Wk,Wv,Wo,mask|null,dX,dWq,dWk,dWv,dWo)");
    ENSURE_INIT();
    GT(dO, 0,  "attentionBackward"); GT(X,  1,  "attentionBackward");
    GT(Q,  2,  "attentionBackward"); GT(K,  3,  "attentionBackward");
    GT(V,  4,  "attentionBackward"); GT(A,  5,  "attentionBackward");
    GT(Yp, 6,  "attentionBackward"); GT(Wq, 7,  "attentionBackward");
    GT(Wk, 8,  "attentionBackward"); GT(Wv, 9,  "attentionBackward");
    GT(Wo, 10, "attentionBackward");
    const float* mask = nullptr; JSValue err = JS_UNDEFINED;
    if (!resolveDeviceMask(ctx, argv[11], mask, err)) return err;
    GT(dX,  12, "attentionBackward"); GT(dWq, 13, "attentionBackward");
    GT(dWk, 14, "attentionBackward"); GT(dWv, 15, "attentionBackward");
    GT(dWo, 16, "attentionBackward");
    nngpu::attention_backward(*dO, *X, *Q, *K, *V, *A, *Yp,
                                  *Wq, *Wk, *Wv, *Wo, mask,
                                  *dX, *dWq, *dWk, *dWv, *dWo);
    return JS_UNDEFINED;
}

// ─── Multi-head attention ─────────────────────────────────────────────────

static JSValue js_mhaForward(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 13) return JS_ThrowTypeError(ctx,
        "mhaForward(X,Wq,Wk,Wv,Wo,mask|null,numHeads,Qh,Kh,Vh,Attnh,Yconcat,O)");
    ENSURE_INIT();
    GT(X,  0, "mhaForward"); GT(Wq, 1, "mhaForward");
    GT(Wk, 2, "mhaForward"); GT(Wv, 3, "mhaForward");
    GT(Wo, 4, "mhaForward");
    const float* mask = nullptr; JSValue err = JS_UNDEFINED;
    if (!resolveDeviceMask(ctx, argv[5], mask, err)) return err;
    int32_t numHeads = 1; JS_ToInt32(ctx, &numHeads, argv[6]);
    GT(Qh,    7,  "mhaForward"); GT(Kh,    8,  "mhaForward");
    GT(Vh,    9,  "mhaForward"); GT(Attnh, 10, "mhaForward");
    GT(Yc,    11, "mhaForward"); GT(O,     12, "mhaForward");
    nngpu::mha_forward(*X, *Wq, *Wk, *Wv, *Wo, mask, numHeads,
                           *Qh, *Kh, *Vh, *Attnh, *Yc, *O);
    return JS_UNDEFINED;
}

static JSValue js_mhaBackward(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 18) return JS_ThrowTypeError(ctx,
        "mhaBackward(dO,X,Qh,Kh,Vh,Attnh,Yconcat,Wq,Wk,Wv,Wo,mask|null,numHeads,dX,dWq,dWk,dWv,dWo)");
    ENSURE_INIT();
    GT(dO,    0, "mhaBackward"); GT(X,     1, "mhaBackward");
    GT(Qh,    2, "mhaBackward"); GT(Kh,    3, "mhaBackward");
    GT(Vh,    4, "mhaBackward"); GT(Attnh, 5, "mhaBackward");
    GT(Yc,    6, "mhaBackward"); GT(Wq,    7, "mhaBackward");
    GT(Wk,    8, "mhaBackward"); GT(Wv,    9, "mhaBackward");
    GT(Wo,   10, "mhaBackward");
    const float* mask = nullptr; JSValue err = JS_UNDEFINED;
    if (!resolveDeviceMask(ctx, argv[11], mask, err)) return err;
    int32_t numHeads = 1; JS_ToInt32(ctx, &numHeads, argv[12]);
    GT(dX,   13, "mhaBackward"); GT(dWq,  14, "mhaBackward");
    GT(dWk,  15, "mhaBackward"); GT(dWv,  16, "mhaBackward");
    GT(dWo,  17, "mhaBackward");
    nngpu::mha_backward(*dO, *X, *Qh, *Kh, *Vh, *Attnh, *Yc,
                            *Wq, *Wk, *Wv, *Wo, mask, numHeads,
                            *dX, *dWq, *dWk, *dWv, *dWo);
    return JS_UNDEFINED;
}

// ─── Pooling, losses, embedding, concat ───────────────────────────────────

static JSValue js_maskedMeanPoolForward(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 3) return JS_ThrowTypeError(ctx, "maskedMeanPoolForward(X,mask|null,y)");
    ENSURE_INIT();
    GT(X, 0, "maskedMeanPoolForward");
    const float* mask = nullptr; JSValue err = JS_UNDEFINED;
    if (!resolveDeviceMask(ctx, argv[1], mask, err)) return err;
    GT(y, 2, "maskedMeanPoolForward");
    nngpu::masked_mean_pool_forward(*X, mask, *y);
    return JS_UNDEFINED;
}
static JSValue js_maskedMeanPoolBackward(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 4) return JS_ThrowTypeError(ctx, "maskedMeanPoolBackward(dY,mask|null,K,dX)");
    ENSURE_INIT();
    GT(dY, 0, "maskedMeanPoolBackward");
    const float* mask = nullptr; JSValue err = JS_UNDEFINED;
    if (!resolveDeviceMask(ctx, argv[1], mask, err)) return err;
    int32_t K = 0; JS_ToInt32(ctx, &K, argv[2]);
    GT(dX, 3, "maskedMeanPoolBackward");
    nngpu::masked_mean_pool_backward(*dY, mask, K, *dX);
    return JS_UNDEFINED;
}

static JSValue js_mseVecForward(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 2) return JS_ThrowTypeError(ctx, "mseVecForward(pred,target)");
    ENSURE_INIT();
    GT(p, 0, "mseVecForward"); GT(t, 1, "mseVecForward");
    float loss = nngpu::mse_vec_forward(*p, *t);
    return JS_NewFloat64(ctx, (double)loss);
}
static JSValue js_mseVecBackward(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 3) return JS_ThrowTypeError(ctx, "mseVecBackward(pred,target,dPred)");
    ENSURE_INIT();
    GT(p, 0, "mseVecBackward"); GT(t, 1, "mseVecBackward"); GT(dp, 2, "mseVecBackward");
    nngpu::mse_vec_backward(*p, *t, *dp);
    return JS_UNDEFINED;
}

// Per-sample MSE (training).
static JSValue js_mseVecPerSample(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 4) return JS_ThrowTypeError(ctx,
        "mseVecPerSample(pred,target,dPred,lossPerSample)");
    ENSURE_INIT();
    GT(p,    0, "mseVecPerSample"); GT(t,    1, "mseVecPerSample");
    GT(dp,   2, "mseVecPerSample"); GT(lps,  3, "mseVecPerSample");
    nngpu::mse_vec_per_sample(*p, *t, *dp, *lps);
    return JS_UNDEFINED;
}

static JSValue js_softmaxXentFused(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 5) return JS_ThrowTypeError(ctx,
        "softmaxXentFused(logits,target,mask|null,probs,dLogits)");
    ENSURE_INIT();
    GT(l, 0, "softmaxXentFused"); GT(t, 1, "softmaxXentFused");
    const float* mask = nullptr; JSValue err = JS_UNDEFINED;
    if (!resolveDeviceMask(ctx, argv[2], mask, err)) return err;
    GT(p, 3, "softmaxXentFused"); GT(dl, 4, "softmaxXentFused");
    float loss = nngpu::softmax_xent_fused(*l, *t, mask, *p, *dl);
    return JS_NewFloat64(ctx, (double)loss);
}

// Batched fused softmax + cross-entropy across (sample, head) tiles. The
// head_offsets GpuTensor is reinterpreted as an int32 device buffer
// (cumulative length n_heads + 1).
static JSValue js_softmaxXentFusedBatched(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 8) return JS_ThrowTypeError(ctx,
        "softmaxXentFusedBatched(logits_BL,target_BL,mask|null,headOffsets,nHeads,probs_BL,dLogits_BL,lossPerSample)");
    ENSURE_INIT();
    GT(L,  0, "softmaxXentFusedBatched"); GT(T,  1, "softmaxXentFusedBatched");
    const float* mask = nullptr; JSValue err = JS_UNDEFINED;
    if (!resolveDeviceMask(ctx, argv[2], mask, err)) return err;
    GT(headOff, 3, "softmaxXentFusedBatched");
    int32_t nHeads = 1; JS_ToInt32(ctx, &nHeads, argv[4]);
    GT(P,   5, "softmaxXentFusedBatched"); GT(dL,  6, "softmaxXentFusedBatched");
    GT(lps, 7, "softmaxXentFusedBatched");
    const int* d_head = reinterpret_cast<const int*>(headOff->data);
    nngpu::softmax_xent_fused_batched(*L, *T, mask, d_head, nHeads, *P, *dL, *lps);
    return JS_UNDEFINED;
}

// idx is a GpuTensor whose `.data` is reinterpreted as int32_t* (its size
// must equal B). Convention: device-resident integer buffer wrapped as a
// GpuTensor.
static JSValue js_embeddingLookupForward(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 4) return JS_ThrowTypeError(ctx,
        "embeddingLookupForward(table,idx,B,out) — idx is a GpuTensor viewed as int32 storage");
    ENSURE_INIT();
    GT(tbl, 0, "embeddingLookupForward"); GT(idx, 1, "embeddingLookupForward");
    int32_t B = 0; JS_ToInt32(ctx, &B, argv[2]);
    GT(out, 3, "embeddingLookupForward");
    const int32_t* d_idx = reinterpret_cast<const int32_t*>(idx->data);
    nngpu::embedding_lookup_forward(*tbl, d_idx, B, *out);
    return JS_UNDEFINED;
}
static JSValue js_embeddingLookupBackward(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 4) return JS_ThrowTypeError(ctx,
        "embeddingLookupBackward(dOut,idx,B,dTable) — idx is a GpuTensor viewed as int32 storage");
    ENSURE_INIT();
    GT(dOut, 0, "embeddingLookupBackward"); GT(idx,  1, "embeddingLookupBackward");
    int32_t B = 0; JS_ToInt32(ctx, &B, argv[2]);
    GT(dTbl, 3, "embeddingLookupBackward");
    const int32_t* d_idx = reinterpret_cast<const int32_t*>(idx->data);
    nngpu::embedding_lookup_backward(*dOut, d_idx, B, *dTbl);
    return JS_UNDEFINED;
}

static JSValue js_concatRows(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 2) return JS_ThrowTypeError(ctx, "concatRows([parts...], out)");
    ENSURE_INIT();
    std::vector<const nngpu::Tensor*> parts;
    if (!readGpuTensorArray(ctx, argv[0], parts, nullptr))
        return JS_ThrowTypeError(ctx, "concatRows: first arg must be array of GpuTensors");
    GT(out, 1, "concatRows");
    nngpu::concat_rows(parts, *out);
    return JS_UNDEFINED;
}

static JSValue js_splitRows(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 2) return JS_ThrowTypeError(ctx, "splitRows(in, [parts...])");
    ENSURE_INIT();
    GT(in, 0, "splitRows");
    std::vector<const nngpu::Tensor*> partsConst;
    std::vector<nngpu::Tensor*> partsMut;
    if (!readGpuTensorArray(ctx, argv[1], partsConst, &partsMut))
        return JS_ThrowTypeError(ctx, "splitRows: second arg must be array of GpuTensors");
    nngpu::split_rows(*in, partsMut);
    return JS_UNDEFINED;
}

// Batched column-block concat: parts are each (B, d_i), out is (B, sum d_i).
static JSValue js_concatBatchedRows(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 2) return JS_ThrowTypeError(ctx, "concatBatchedRows([parts...], out)");
    ENSURE_INIT();
    std::vector<const nngpu::Tensor*> parts;
    if (!readGpuTensorArray(ctx, argv[0], parts, nullptr))
        return JS_ThrowTypeError(ctx, "concatBatchedRows: first arg must be array of GpuTensors");
    GT(out, 1, "concatBatchedRows");
    nngpu::concat_batched_rows(parts, *out);
    return JS_UNDEFINED;
}

// concatNchwChannels(parts, N, H, W, C_per_part[], out)
static JSValue js_concatNchwChannels(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 6) return JS_ThrowTypeError(ctx,
        "concatNchwChannels([parts...], N, H, W, [C_per_part...], out)");
    ENSURE_INIT();
    std::vector<const nngpu::Tensor*> parts;
    if (!readGpuTensorArray(ctx, argv[0], parts, nullptr))
        return JS_ThrowTypeError(ctx, "concatNchwChannels: parts must be array of GpuTensors");
    int32_t N = 0, H = 0, W = 0;
    JS_ToInt32(ctx, &N, argv[1]);
    JS_ToInt32(ctx, &H, argv[2]);
    JS_ToInt32(ctx, &W, argv[3]);
    std::vector<int> Cpp;
    if (!readIntArray(ctx, argv[4], Cpp))
        return JS_ThrowTypeError(ctx, "concatNchwChannels: C_per_part must be array of ints");
    GT(out, 5, "concatNchwChannels");
    nngpu::concat_nchw_channels(parts, N, H, W, Cpp, *out);
    return JS_UNDEFINED;
}

static JSValue js_concatNchwChannelsBackward(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 6) return JS_ThrowTypeError(ctx,
        "concatNchwChannelsBackward(dY, N, H, W, [C_per_part...], [parts...])");
    ENSURE_INIT();
    GT(dY, 0, "concatNchwChannelsBackward");
    int32_t N = 0, H = 0, W = 0;
    JS_ToInt32(ctx, &N, argv[1]);
    JS_ToInt32(ctx, &H, argv[2]);
    JS_ToInt32(ctx, &W, argv[3]);
    std::vector<int> Cpp;
    if (!readIntArray(ctx, argv[4], Cpp))
        return JS_ThrowTypeError(ctx, "concatNchwChannelsBackward: C_per_part must be array of ints");
    std::vector<const nngpu::Tensor*> partsConst;
    std::vector<nngpu::Tensor*> partsMut;
    if (!readGpuTensorArray(ctx, argv[5], partsConst, &partsMut))
        return JS_ThrowTypeError(ctx, "concatNchwChannelsBackward: parts must be array of GpuTensors");
    nngpu::concat_nchw_channels_backward(*dY, N, H, W, Cpp, partsMut);
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
    nngpu::sgd_step(*p, *g, *v, (float)lr, (float)m);
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
    nngpu::adam_step(*p, *g, *m, *v, (float)lr, (float)b1, (float)b2, (float)eps, step);
    return JS_UNDEFINED;
}

// ─── Batched forwards ─────────────────────────────────────────────────────

static JSValue js_linearForwardBatched(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 4) return JS_ThrowTypeError(ctx, "linearForwardBatched(W,bias,X_BD,Y_BD)");
    ENSURE_INIT();
    GT(W, 0, "linearForwardBatched"); GT(b, 1, "linearForwardBatched");
    GT(X, 2, "linearForwardBatched"); GT(Y, 3, "linearForwardBatched");
    nngpu::linear_forward_batched(*W, *b, *X, *Y);
    return JS_UNDEFINED;
}
static JSValue js_linearForwardBatchedFp16(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 4) return JS_ThrowTypeError(ctx,
        "linearForwardBatchedFp16(W,bias|null,X_BD,Y_BD)");
    ENSURE_INIT();
    GT(W, 0, "linearForwardBatchedFp16");
    const nngpu::Tensor* bias = nullptr; JSValue err = JS_UNDEFINED;
    if (!resolveOptionalConstGpuTensor(ctx, argv[1], bias, err, "bias")) return err;
    GT(X, 2, "linearForwardBatchedFp16"); GT(Y, 3, "linearForwardBatchedFp16");
    nngpu::linear_forward_batched_fp16(*W, bias, *X, *Y);
    return JS_UNDEFINED;
}
static JSValue js_reluForwardBatched(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 2) return JS_ThrowTypeError(ctx, "reluForwardBatched(X_BD,Y_BD)");
    ENSURE_INIT();
    GT(X, 0, "reluForwardBatched"); GT(Y, 1, "reluForwardBatched");
    nngpu::relu_forward_batched(*X, *Y);
    return JS_UNDEFINED;
}
static JSValue js_tanhForwardBatched(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 2) return JS_ThrowTypeError(ctx, "tanhForwardBatched(X_BD,Y_BD)");
    ENSURE_INIT();
    GT(X, 0, "tanhForwardBatched"); GT(Y, 1, "tanhForwardBatched");
    nngpu::tanh_forward_batched(*X, *Y);
    return JS_UNDEFINED;
}
static JSValue js_addInplaceBatched(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 2) return JS_ThrowTypeError(ctx, "addInplaceBatched(Y_BD,X_BD)");
    ENSURE_INIT();
    GT(Y, 0, "addInplaceBatched"); GT(X, 1, "addInplaceBatched");
    nngpu::add_inplace_batched(*Y, *X);
    return JS_UNDEFINED;
}

// ─── Batched-training backwards ───────────────────────────────────────────

static JSValue js_linearBackwardBatched(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 6) return JS_ThrowTypeError(ctx,
        "linearBackwardBatched(W,X_BD,dY_BD,dX_BD,dW,dB)");
    ENSURE_INIT();
    GT(W,   0, "linearBackwardBatched"); GT(X,   1, "linearBackwardBatched");
    GT(dY,  2, "linearBackwardBatched"); GT(dX,  3, "linearBackwardBatched");
    GT(dW,  4, "linearBackwardBatched"); GT(dB,  5, "linearBackwardBatched");
    nngpu::linear_backward_batched(*W, *X, *dY, *dX, *dW, *dB);
    return JS_UNDEFINED;
}
static JSValue js_reluBackwardBatched(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 3) return JS_ThrowTypeError(ctx, "reluBackwardBatched(X_BD,dY_BD,dX_BD)");
    ENSURE_INIT();
    GT(X, 0, "reluBackwardBatched"); GT(dY, 1, "reluBackwardBatched");
    GT(dX, 2, "reluBackwardBatched");
    nngpu::relu_backward_batched(*X, *dY, *dX);
    return JS_UNDEFINED;
}
static JSValue js_tanhBackwardBatched(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 3) return JS_ThrowTypeError(ctx, "tanhBackwardBatched(Y_BD,dY_BD,dX_BD)");
    ENSURE_INIT();
    GT(Y, 0, "tanhBackwardBatched"); GT(dY, 1, "tanhBackwardBatched");
    GT(dX, 2, "tanhBackwardBatched");
    nngpu::tanh_backward_batched(*Y, *dY, *dX);
    return JS_UNDEFINED;
}

// ─── Reductions ───────────────────────────────────────────────────────────

static JSValue js_sumRows(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 2) return JS_ThrowTypeError(ctx, "sumRows(X,Y)");
    ENSURE_INIT();
    GT(X, 0, "sumRows"); GT(Y, 1, "sumRows");
    nngpu::sum_rows(*X, *Y);
    return JS_UNDEFINED;
}
static JSValue js_sumCols(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 2) return JS_ThrowTypeError(ctx, "sumCols(X,Y)");
    ENSURE_INIT();
    GT(X, 0, "sumCols"); GT(Y, 1, "sumCols");
    nngpu::sum_cols(*X, *Y);
    return JS_UNDEFINED;
}
static JSValue js_argmaxRows(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 2) return JS_ThrowTypeError(ctx, "argmaxRows(X,Idx)");
    ENSURE_INIT();
    GT(X, 0, "argmaxRows"); GT(I, 1, "argmaxRows");
    nngpu::argmax_rows(*X, *I);
    return JS_UNDEFINED;
}

// Dtype enum constants exposed as bro.tensor.dtype.{fp32, fp16, int8}.
static void installDtypeEnum(JSContext* ctx, JSValue gpuObj) {
    JSValue dt = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, dt, "fp32", JS_NewInt32(ctx, (int)nngpu::Dtype::FP32));
    JS_SetPropertyStr(ctx, dt, "fp16", JS_NewInt32(ctx, (int)nngpu::Dtype::FP16));
    JS_SetPropertyStr(ctx, dt, "int8", JS_NewInt32(ctx, (int)nngpu::Dtype::INT8));
    JS_SetPropertyStr(ctx, gpuObj, "dtype", dt);
}

#undef GT
#undef ENSURE_INIT

// ═══════════════════════════════════════════════════════════════════════════
// Install
// ═══════════════════════════════════════════════════════════════════════════

void installTensorBindings(JSContext* ctx) {
    registerClasses(ctx);

    JSValue gpuObj = JS_NewObject(ctx);

    JS_SetPropertyStr(ctx, gpuObj, "available", JS_TRUE);

    // Backend identification
#ifdef BROTENSOR_HAS_CUDA
    JS_SetPropertyStr(ctx, gpuObj, "backend", JS_NewString(ctx, "cuda"));
#elif defined(BROTENSOR_HAS_METAL)
    JS_SetPropertyStr(ctx, gpuObj, "backend", JS_NewString(ctx, "metal"));
#else
    JS_SetPropertyStr(ctx, gpuObj, "backend", JS_NewString(ctx, "unknown"));
#endif

    installDtypeEnum(ctx, gpuObj);

    // Runtime
    JS_SetPropertyStr(ctx, gpuObj, "init", JS_NewCFunction(ctx, js_init, "init", 0));
    JS_SetPropertyStr(ctx, gpuObj, "sync", JS_NewCFunction(ctx, js_sync, "sync", 0));

    // Factory
    JS_SetPropertyStr(ctx, gpuObj, "createTensor",
        JS_NewCFunction(ctx, js_createTensor, "createTensor", 3));

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
    JS_SetPropertyStr(ctx, gpuObj, "scaleInplace",       JS_NewCFunction(ctx, js_scaleInplace,       "scaleInplace",       2));
    JS_SetPropertyStr(ctx, gpuObj, "mulInplace",         JS_NewCFunction(ctx, js_mulInplace,         "mulInplace",         2));
    JS_SetPropertyStr(ctx, gpuObj, "clamp",              JS_NewCFunction(ctx, js_clamp,              "clamp",              3));
    JS_SetPropertyStr(ctx, gpuObj, "buildSlotMask",      JS_NewCFunction(ctx, js_buildSlotMask,      "buildSlotMask",      5));
    JS_SetPropertyStr(ctx, gpuObj, "copyD2D",            JS_NewCFunction(ctx, js_copyD2D,            "copyD2D",            5));
    JS_SetPropertyStr(ctx, gpuObj, "cast",               JS_NewCFunction(ctx, js_cast,               "cast",               3));

    // Softmax
    JS_SetPropertyStr(ctx, gpuObj, "softmaxForward",     JS_NewCFunction(ctx, js_softmaxForward,     "softmaxForward",     3));
    JS_SetPropertyStr(ctx, gpuObj, "softmaxBackward",    JS_NewCFunction(ctx, js_softmaxBackward,    "softmaxBackward",    3));

    // LayerNorm
    JS_SetPropertyStr(ctx, gpuObj, "layernormForward",                       JS_NewCFunction(ctx, js_layernormForward,                       "layernormForward",                       6));
    JS_SetPropertyStr(ctx, gpuObj, "layernormBackward",                      JS_NewCFunction(ctx, js_layernormBackward,                      "layernormBackward",                      7));
    JS_SetPropertyStr(ctx, gpuObj, "layernormForwardInferenceBatched",       JS_NewCFunction(ctx, js_layernormForwardInferenceBatched,       "layernormForwardInferenceBatched",       5));
    JS_SetPropertyStr(ctx, gpuObj, "layernormForwardInferenceBatchedFp16",   JS_NewCFunction(ctx, js_layernormForwardInferenceBatchedFp16,   "layernormForwardInferenceBatchedFp16",   5));

    // Single-head attention
    JS_SetPropertyStr(ctx, gpuObj, "attentionForward",   JS_NewCFunction(ctx, js_attentionForward,   "attentionForward",   12));
    JS_SetPropertyStr(ctx, gpuObj, "attentionBackward",  JS_NewCFunction(ctx, js_attentionBackward,  "attentionBackward",  17));

    // MHA
    JS_SetPropertyStr(ctx, gpuObj, "mhaForward",         JS_NewCFunction(ctx, js_mhaForward,         "mhaForward",         13));
    JS_SetPropertyStr(ctx, gpuObj, "mhaBackward",        JS_NewCFunction(ctx, js_mhaBackward,        "mhaBackward",        18));

    // Pooling, losses, embedding, concat
    JS_SetPropertyStr(ctx, gpuObj, "maskedMeanPoolForward",       JS_NewCFunction(ctx, js_maskedMeanPoolForward,       "maskedMeanPoolForward",       3));
    JS_SetPropertyStr(ctx, gpuObj, "maskedMeanPoolBackward",      JS_NewCFunction(ctx, js_maskedMeanPoolBackward,      "maskedMeanPoolBackward",      4));
    JS_SetPropertyStr(ctx, gpuObj, "mseVecForward",               JS_NewCFunction(ctx, js_mseVecForward,               "mseVecForward",               2));
    JS_SetPropertyStr(ctx, gpuObj, "mseVecBackward",              JS_NewCFunction(ctx, js_mseVecBackward,              "mseVecBackward",              3));
    JS_SetPropertyStr(ctx, gpuObj, "mseVecPerSample",             JS_NewCFunction(ctx, js_mseVecPerSample,             "mseVecPerSample",             4));
    JS_SetPropertyStr(ctx, gpuObj, "softmaxXentFused",            JS_NewCFunction(ctx, js_softmaxXentFused,            "softmaxXentFused",            5));
    JS_SetPropertyStr(ctx, gpuObj, "softmaxXentFusedBatched",     JS_NewCFunction(ctx, js_softmaxXentFusedBatched,     "softmaxXentFusedBatched",     8));
    JS_SetPropertyStr(ctx, gpuObj, "embeddingLookupForward",      JS_NewCFunction(ctx, js_embeddingLookupForward,      "embeddingLookupForward",      4));
    JS_SetPropertyStr(ctx, gpuObj, "embeddingLookupBackward",     JS_NewCFunction(ctx, js_embeddingLookupBackward,     "embeddingLookupBackward",     4));
    JS_SetPropertyStr(ctx, gpuObj, "concatRows",                  JS_NewCFunction(ctx, js_concatRows,                  "concatRows",                  2));
    JS_SetPropertyStr(ctx, gpuObj, "splitRows",                   JS_NewCFunction(ctx, js_splitRows,                   "splitRows",                   2));
    JS_SetPropertyStr(ctx, gpuObj, "concatBatchedRows",           JS_NewCFunction(ctx, js_concatBatchedRows,           "concatBatchedRows",           2));
    JS_SetPropertyStr(ctx, gpuObj, "concatNchwChannels",          JS_NewCFunction(ctx, js_concatNchwChannels,          "concatNchwChannels",          6));
    JS_SetPropertyStr(ctx, gpuObj, "concatNchwChannelsBackward",  JS_NewCFunction(ctx, js_concatNchwChannelsBackward,  "concatNchwChannelsBackward",  6));

    // Optimisers
    JS_SetPropertyStr(ctx, gpuObj, "sgdStep",   JS_NewCFunction(ctx, js_sgdStep,   "sgdStep",   5));
    JS_SetPropertyStr(ctx, gpuObj, "adamStep",  JS_NewCFunction(ctx, js_adamStep,  "adamStep",  9));

    // Batched (inference + training)
    JS_SetPropertyStr(ctx, gpuObj, "linearForwardBatched",       JS_NewCFunction(ctx, js_linearForwardBatched,       "linearForwardBatched",       4));
    JS_SetPropertyStr(ctx, gpuObj, "linearForwardBatchedFp16",   JS_NewCFunction(ctx, js_linearForwardBatchedFp16,   "linearForwardBatchedFp16",   4));
    JS_SetPropertyStr(ctx, gpuObj, "reluForwardBatched",         JS_NewCFunction(ctx, js_reluForwardBatched,         "reluForwardBatched",         2));
    JS_SetPropertyStr(ctx, gpuObj, "tanhForwardBatched",         JS_NewCFunction(ctx, js_tanhForwardBatched,         "tanhForwardBatched",         2));
    JS_SetPropertyStr(ctx, gpuObj, "addInplaceBatched",          JS_NewCFunction(ctx, js_addInplaceBatched,          "addInplaceBatched",          2));
    JS_SetPropertyStr(ctx, gpuObj, "linearBackwardBatched",      JS_NewCFunction(ctx, js_linearBackwardBatched,      "linearBackwardBatched",      6));
    JS_SetPropertyStr(ctx, gpuObj, "reluBackwardBatched",        JS_NewCFunction(ctx, js_reluBackwardBatched,        "reluBackwardBatched",        3));
    JS_SetPropertyStr(ctx, gpuObj, "tanhBackwardBatched",        JS_NewCFunction(ctx, js_tanhBackwardBatched,        "tanhBackwardBatched",        3));

    // Reductions
    JS_SetPropertyStr(ctx, gpuObj, "sumRows",    JS_NewCFunction(ctx, js_sumRows,    "sumRows",    2));
    JS_SetPropertyStr(ctx, gpuObj, "sumCols",    JS_NewCFunction(ctx, js_sumCols,    "sumCols",    2));
    JS_SetPropertyStr(ctx, gpuObj, "argmaxRows", JS_NewCFunction(ctx, js_argmaxRows, "argmaxRows", 2));

    // Delegate the other clusters.
    installTensorActivationOps(ctx, gpuObj);
    installTensorAttentionOps(ctx, gpuObj);
    installTensorConvOps(ctx, gpuObj);
    installTensorDiffusionOps(ctx, gpuObj);
    installTensorInt8Ops(ctx, gpuObj);
    installTensorSafetensorsOps(ctx, gpuObj);
    installTensorAudioOps(ctx, gpuObj);

    // Install onto bro.tensor.
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue broObj = JS_GetPropertyStr(ctx, global, "bro");
    if (JS_IsUndefined(broObj) || JS_IsException(broObj)) {
        JS_FreeValue(ctx, broObj);
        broObj = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, global, "bro", JS_DupValue(ctx, broObj));
    }
    JS_SetPropertyStr(ctx, broObj, "tensor", gpuObj);
    JS_FreeValue(ctx, broObj);
    JS_FreeValue(ctx, global);
}

// Cross-binding unwrap helper (declared in ai_bindings.h).
nngpu::Tensor* gpuTensorFromJS(JSContext* ctx, JSValueConst v) {
    return gpuTensorFromJSLocal(ctx, v);
}

} // namespace bro::js

#else // !BROTENSOR_HAS_GPU

namespace brotensor { struct Tensor; }

namespace bro::js {

void installTensorBindings(JSContext* ctx) {
    JSValue gpuObj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, gpuObj, "available", JS_FALSE);

    JSValue global = JS_GetGlobalObject(ctx);
    JSValue broObj = JS_GetPropertyStr(ctx, global, "bro");
    if (JS_IsUndefined(broObj) || JS_IsException(broObj)) {
        JS_FreeValue(ctx, broObj);
        broObj = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, global, "bro", JS_DupValue(ctx, broObj));
    }
    JS_SetPropertyStr(ctx, broObj, "tensor", gpuObj);
    JS_FreeValue(ctx, broObj);
    JS_FreeValue(ctx, global);
}

brotensor::Tensor* gpuTensorFromJS(JSContext*, JSValueConst) {
    return nullptr;
}

} // namespace bro::js

#endif // BROTENSOR_HAS_GPU
