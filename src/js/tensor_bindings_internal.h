// Shared helpers between tensor_bindings_*.cpp TUs.
//
// Only included when BROTENSOR_HAS_GPU is set; consumers are expected to
// guard their own #include with the same macro. The header defines the
// GpuTensorData / TensorData wrapper shapes used across all TUs (qjsbind
// keys classes by C++ type, so the *exact* struct definition must match the
// one registered in tensor_bindings.cpp), plus the small parsing helpers
// that every op binding repeats.
//
// Each per-cluster binding file (tensor_bindings_activations.cpp,
// tensor_bindings_attention.cpp, etc.) defines a single free function
// `installTensor<Cluster>Ops(JSContext*, JSValue gpuObj)` that attaches its
// ops onto the already-created bro.tensor namespace object. The main
// installTensorBindings() in tensor_bindings.cpp orchestrates the call
// order.

#pragma once

#ifdef BROTENSOR_HAS_GPU

#include "js/ai_bindings.h"

#include <qjsbind/qjsbind.h>

#include <brogameagent/nn/tensor.h>
#include <brotensor/runtime.h>
#include <brotensor/tensor.h>
#include <brotensor/ops.h>

#include <cstdint>
#include <vector>

namespace bro::js {

namespace nn    = brogameagent::nn;
namespace nngpu = brotensor;

// ─── Wrapper structs (must match the registered class identity) ────────────
//
// qjsbind::Class<T> keys its JS class id by the type T. Every TU that wants
// to unwrap an AIGpuTensor / AITensor must use the *same* struct definition
// here. tensor_bindings.cpp registers the AIGpuTensor class; other TUs only
// unwrap.
struct GpuTensorData { nngpu::GpuTensor t; };
struct TensorData    { nn::Tensor t; };

// ─── Float32Array helpers ──────────────────────────────────────────────────
inline float* getFloatArrayPtr(JSContext* ctx, JSValueConst arr, size_t& outCount) {
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

// Get raw byte view of any typed array (Uint8/Int8/Uint16/Float16/Int32/Float32).
// Returns the *byte length* of the view (not element count). Used by upload_fp16
// (Uint16Array as bit-pattern) and int8 quant helpers.
inline uint8_t* getTypedArrayBytePtr(JSContext* ctx, JSValueConst arr, size_t& outBytes) {
    if (JS_IsUndefined(arr) || JS_IsNull(arr)) { outBytes = 0; return nullptr; }
    size_t byteOff = 0, viewLen = 0;
    JSValue abuf = JS_GetTypedArrayBuffer(ctx, arr, &byteOff, &viewLen, nullptr);
    if (JS_IsException(abuf)) { JS_FreeValue(ctx, JS_GetException(ctx)); outBytes = 0; return nullptr; }
    size_t abufLen = 0;
    uint8_t* ptr = JS_GetArrayBuffer(ctx, &abufLen, abuf);
    JS_FreeValue(ctx, abuf);
    if (!ptr) { outBytes = 0; return nullptr; }
    outBytes = viewLen;
    return ptr + byteOff;
}

// ─── Unwrap helpers ────────────────────────────────────────────────────────
inline nn::Tensor* tensorFromJS(JSContext* ctx, JSValueConst v) {
    auto* d = qjsbind::unwrap<TensorData>(ctx, v);
    return d ? &d->t : nullptr;
}

inline nngpu::GpuTensor* gpuTensorFromJSLocal(JSContext* ctx, JSValueConst v) {
    auto* d = qjsbind::unwrap<GpuTensorData>(ctx, v);
    return d ? &d->t : nullptr;
}

// ─── Optional-arg resolvers ────────────────────────────────────────────────

// Resolve an optional device-pointer mask. null/undefined → nullptr.
// GpuTensor → its .data. Anything else throws TypeError (returns false +
// thrown_err set with the exception value).
inline bool resolveDeviceMask(JSContext* ctx, JSValueConst v,
                              const float*& out_ptr, JSValue& thrown_err) {
    if (JS_IsUndefined(v) || JS_IsNull(v)) { out_ptr = nullptr; return true; }
    auto* gt = gpuTensorFromJSLocal(ctx, v);
    if (!gt) {
        thrown_err = JS_ThrowTypeError(ctx, "mask must be null or a GpuTensor (device pointer)");
        return false;
    }
    out_ptr = gt->data;
    return true;
}

// Resolve an optional GpuTensor arg (bias, t_emb_shift, attn_logit_bias, …).
// null/undefined → nullptr. GpuTensor → pointer to the underlying tensor.
// Anything else throws TypeError.
inline bool resolveOptionalGpuTensor(JSContext* ctx, JSValueConst v,
                                     nngpu::GpuTensor*& out_ptr,
                                     JSValue& thrown_err,
                                     const char* label) {
    if (JS_IsUndefined(v) || JS_IsNull(v)) { out_ptr = nullptr; return true; }
    auto* gt = gpuTensorFromJSLocal(ctx, v);
    if (!gt) {
        thrown_err = JS_ThrowTypeError(ctx, "%s must be null or a GpuTensor", label);
        return false;
    }
    out_ptr = gt;
    return true;
}

// Const variant for read-only optional args (e.g. forward inputs in backward
// signatures that take `const GpuTensor*`).
inline bool resolveOptionalConstGpuTensor(JSContext* ctx, JSValueConst v,
                                          const nngpu::GpuTensor*& out_ptr,
                                          JSValue& thrown_err,
                                          const char* label) {
    nngpu::GpuTensor* m = nullptr;
    bool ok = resolveOptionalGpuTensor(ctx, v, m, thrown_err, label);
    out_ptr = m;
    return ok;
}

// Boolean arg with default.
inline bool getBool(JSContext* ctx, JSValueConst v, bool def) {
    if (JS_IsUndefined(v) || JS_IsNull(v)) return def;
    return JS_ToBool(ctx, v) ? true : false;
}

// Read a JS Array of GpuTensors. Returns true on success.
inline bool readGpuTensorArray(JSContext* ctx, JSValueConst v,
                               std::vector<const nngpu::GpuTensor*>& outConst,
                               std::vector<nngpu::GpuTensor*>* outMut) {
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

// Read a JS Array of integers into a std::vector<int>. Returns true on success.
inline bool readIntArray(JSContext* ctx, JSValueConst v, std::vector<int>& out) {
    if (!JS_IsArray(v)) return false;
    uint32_t len = 0;
    JSValue lv = JS_GetPropertyStr(ctx, v, "length");
    JS_ToUint32(ctx, &len, lv);
    JS_FreeValue(ctx, lv);
    out.clear();
    out.reserve(len);
    for (uint32_t i = 0; i < len; ++i) {
        JSValue ev = JS_GetPropertyUint32(ctx, v, i);
        int32_t x = 0;
        JS_ToInt32(ctx, &x, ev);
        JS_FreeValue(ctx, ev);
        out.push_back(x);
    }
    return true;
}

// ─── Cluster installers (free functions; called from installTensorBindings) ─
void installTensorActivationOps(JSContext* ctx, JSValue gpuObj);
void installTensorAttentionOps(JSContext* ctx, JSValue gpuObj);
void installTensorConvOps(JSContext* ctx, JSValue gpuObj);
void installTensorDiffusionOps(JSContext* ctx, JSValue gpuObj);
void installTensorInt8Ops(JSContext* ctx, JSValue gpuObj);

// ─── Common macros used by every TU ────────────────────────────────────────
// (defined here rather than in each TU so the conventions stay synchronised)

#define BROTENSOR_ENSURE_INIT() do { ::brotensor::cuda_init(); } while (0)

// Fetch GpuTensor* from argv[idx] into `name`. Throws TypeError on failure.
#define BROTENSOR_GT(name, idx, label)                                      \
    auto* name = ::bro::js::gpuTensorFromJSLocal(ctx, argv[idx]);           \
    if (!name) return JS_ThrowTypeError(ctx, label " — expected GpuTensors")

} // namespace bro::js

#endif // BROTENSOR_HAS_GPU
