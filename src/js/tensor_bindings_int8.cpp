// JS bindings — INT8 weight-only quantisation helpers and W8A16 ops.
// See tensor_bindings.cpp for the architectural overview.

#ifdef BROTENSOR_HAS_GPU

#include "js/tensor_bindings_internal.h"

#include <cstdint>
#include <vector>

namespace bro::js {

#define ENSURE_INIT() BROTENSOR_ENSURE_INIT()
#define GT(name, idx, label) BROTENSOR_GT(name, idx, label)

// ─── Host helper: quantise FP16 → INT8 + per-row FP32 scales ──────────────
//
// quantizeInt8PerRowHost(W_fp16_uint16Array, out, in) ->
//   { weights: Int8Array (out*in), scales: Float32Array (out) }
//
// Pure CPU helper. Used by callers preparing W8A16 weights once at load
// time. The output typed arrays are fresh copies.
static JSValue js_quantizeInt8PerRowHost(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 3) return JS_ThrowTypeError(ctx,
        "quantizeInt8PerRowHost(W_fp16:Uint16Array, out, in)");
    size_t bytes = 0;
    uint8_t* p = getTypedArrayBytePtr(ctx, argv[0], bytes);
    if (!p) return JS_ThrowTypeError(ctx,
        "quantizeInt8PerRowHost: W_fp16 must be Uint16Array (binary16 bit pattern)");
    int32_t out = 0, in_ = 0;
    JS_ToInt32(ctx, &out, argv[1]);
    JS_ToInt32(ctx, &in_, argv[2]);
    if (out <= 0 || in_ <= 0) return JS_ThrowRangeError(ctx, "out, in must be > 0");
    if (bytes / sizeof(uint16_t) < static_cast<size_t>(out) * static_cast<size_t>(in_))
        return JS_ThrowRangeError(ctx, "W_fp16 view too small for (out, in)");

    std::vector<int8_t> qw(static_cast<size_t>(out) * static_cast<size_t>(in_));
    std::vector<float>  sc(static_cast<size_t>(out));
    nngpu::quantize_int8_per_row_host(
        reinterpret_cast<const uint16_t*>(p), out, in_, qw.data(), sc.data());

    // Build Int8Array (typed view of Int8).
    JSValue qbuf = JS_NewArrayBufferCopy(
        ctx, reinterpret_cast<const uint8_t*>(qw.data()), qw.size());
    JSValue qargs[3] = { qbuf, JS_UNDEFINED, JS_UNDEFINED };
    JSValue qarr = JS_NewTypedArray(ctx, 1, qargs, JS_TYPED_ARRAY_INT8);
    JS_FreeValue(ctx, qbuf);

    JSValue sarr = qjsbind::make_float32_array(ctx, sc.data(), sc.size());

    JSValue ret = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, ret, "weights", qarr);
    JS_SetPropertyStr(ctx, ret, "scales",  sarr);
    return ret;
}

// ─── W8A16 matmul ─────────────────────────────────────────────────────────

static JSValue js_matmulInt8wFp16(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 4) return JS_ThrowTypeError(ctx,
        "matmulInt8wFp16(W_int8,scales,X,Y)");
    ENSURE_INIT();
    GT(W, 0, "matmulInt8wFp16"); GT(S, 1, "matmulInt8wFp16");
    GT(X, 2, "matmulInt8wFp16"); GT(Y, 3, "matmulInt8wFp16");
    nngpu::matmul_int8w_fp16_gpu(*W, *S, *X, *Y);
    return JS_UNDEFINED;
}

// ─── W8A16 conv2d forward ─────────────────────────────────────────────────
//
// conv2dInt8wFp16Forward(X, W_int8, scales, bias|null,
//                        N, C_in, H, W, C_out, kH, kW,
//                        sH, sW, pH, pW, dH, dW, groups, Y)
static JSValue js_conv2dInt8wFp16Forward(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 19) return JS_ThrowTypeError(ctx,
        "conv2dInt8wFp16Forward(X,W_int8,scales,bias|null,N,C_in,H,W,C_out,kH,kW,sH,sW,pH,pW,dH,dW,groups,Y)");
    ENSURE_INIT();
    GT(X,   0, "conv2dInt8wFp16Forward");
    GT(Wt,  1, "conv2dInt8wFp16Forward");
    GT(Sc,  2, "conv2dInt8wFp16Forward");
    JSValue err = JS_UNDEFINED;
    const nngpu::GpuTensor* bias = nullptr;
    if (!resolveOptionalConstGpuTensor(ctx, argv[3], bias, err, "bias")) return err;
    int32_t N=0,Cin=0,Hin=0,Win=0,Cout=0,kH=0,kW=0;
    int32_t sH=1,sW=1,pH=0,pW=0,dH=1,dW=1,groups=1;
    JS_ToInt32(ctx, &N,    argv[4]);
    JS_ToInt32(ctx, &Cin,  argv[5]);
    JS_ToInt32(ctx, &Hin,  argv[6]);
    JS_ToInt32(ctx, &Win,  argv[7]);
    JS_ToInt32(ctx, &Cout, argv[8]);
    JS_ToInt32(ctx, &kH,   argv[9]);
    JS_ToInt32(ctx, &kW,   argv[10]);
    JS_ToInt32(ctx, &sH,   argv[11]);
    JS_ToInt32(ctx, &sW,   argv[12]);
    JS_ToInt32(ctx, &pH,   argv[13]);
    JS_ToInt32(ctx, &pW,   argv[14]);
    JS_ToInt32(ctx, &dH,   argv[15]);
    JS_ToInt32(ctx, &dW,   argv[16]);
    JS_ToInt32(ctx, &groups,argv[17]);
    GT(Y, 18, "conv2dInt8wFp16Forward");
    nngpu::conv2d_int8w_fp16_forward_gpu(*X, *Wt, *Sc, bias,
                                         N, Cin, Hin, Win, Cout, kH, kW,
                                         sH, sW, pH, pW, dH, dW, groups, *Y);
    return JS_UNDEFINED;
}

// ─── W8A16 batched linear ─────────────────────────────────────────────────

// linearForwardBatchedInt8wFp16(W_int8, scales, bias|null, X_BD, Y_BD)
static JSValue js_linearForwardBatchedInt8wFp16(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 5) return JS_ThrowTypeError(ctx,
        "linearForwardBatchedInt8wFp16(W_int8,scales,bias|null,X_BD,Y_BD)");
    ENSURE_INIT();
    GT(W, 0, "linearForwardBatchedInt8wFp16");
    GT(S, 1, "linearForwardBatchedInt8wFp16");
    JSValue err = JS_UNDEFINED;
    const nngpu::GpuTensor* bias = nullptr;
    if (!resolveOptionalConstGpuTensor(ctx, argv[2], bias, err, "bias")) return err;
    GT(X, 3, "linearForwardBatchedInt8wFp16");
    GT(Y, 4, "linearForwardBatchedInt8wFp16");
    nngpu::linear_forward_batched_int8w_fp16_gpu(*W, *S, bias, *X, *Y);
    return JS_UNDEFINED;
}

// ─── W8A16 resblock forward (options object) ──────────────────────────────
//
// resblockForwardInt8wFp16(opts):
//   required: X, gamma1, beta1, W1_int8, s1, gamma2, beta2, W2_int8, s2, Y,
//             N, C_in, C_out, H, W
//   optional: b1, t_emb_shift, b2, Wskip_int8, sskip, bskip,
//             numGroups (32), eps (1e-5)
static const nngpu::GpuTensor* getTensorPropCR(JSContext* ctx, JSValueConst obj,
                                               const char* key, bool& ok, JSValue& err) {
    JSValue v = JS_GetPropertyStr(ctx, obj, key);
    if (JS_IsUndefined(v) || JS_IsNull(v)) { JS_FreeValue(ctx, v); ok = true; return nullptr; }
    auto* gt = gpuTensorFromJSLocal(ctx, v);
    JS_FreeValue(ctx, v);
    if (!gt) {
        err = JS_ThrowTypeError(ctx, "options.%s must be a GpuTensor or null", key);
        ok = false;
        return nullptr;
    }
    ok = true;
    return gt;
}
static nngpu::GpuTensor* getTensorPropMR(JSContext* ctx, JSValueConst obj,
                                         const char* key, bool& ok, JSValue& err) {
    JSValue v = JS_GetPropertyStr(ctx, obj, key);
    if (JS_IsUndefined(v) || JS_IsNull(v)) { JS_FreeValue(ctx, v); ok = true; return nullptr; }
    auto* gt = gpuTensorFromJSLocal(ctx, v);
    JS_FreeValue(ctx, v);
    if (!gt) {
        err = JS_ThrowTypeError(ctx, "options.%s must be a GpuTensor or null", key);
        ok = false;
        return nullptr;
    }
    ok = true;
    return gt;
}
static int32_t getIntPropI8(JSContext* ctx, JSValueConst obj, const char* key, int32_t def) {
    JSValue v = JS_GetPropertyStr(ctx, obj, key);
    int32_t out = def;
    if (JS_IsNumber(v)) JS_ToInt32(ctx, &out, v);
    JS_FreeValue(ctx, v);
    return out;
}

static JSValue js_resblockForwardInt8wFp16(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 1 || !JS_IsObject(argv[0])) return JS_ThrowTypeError(ctx,
        "resblockForwardInt8wFp16(opts) — see docs");
    ENSURE_INIT();
    JSValueConst o = argv[0];
    JSValue err = JS_UNDEFINED;
    bool ok = true;
    const nngpu::GpuTensor* X    = getTensorPropCR(ctx, o, "X",    ok, err); if (!ok) return err;
    const nngpu::GpuTensor* g1   = getTensorPropCR(ctx, o, "gamma1", ok, err); if (!ok) return err;
    const nngpu::GpuTensor* b1g  = getTensorPropCR(ctx, o, "beta1",  ok, err); if (!ok) return err;
    const nngpu::GpuTensor* W1   = getTensorPropCR(ctx, o, "W1_int8", ok, err); if (!ok) return err;
    const nngpu::GpuTensor* s1   = getTensorPropCR(ctx, o, "s1",     ok, err); if (!ok) return err;
    const nngpu::GpuTensor* b1   = getTensorPropCR(ctx, o, "b1",     ok, err); if (!ok) return err;
    const nngpu::GpuTensor* tem  = getTensorPropCR(ctx, o, "t_emb_shift", ok, err); if (!ok) return err;
    const nngpu::GpuTensor* g2   = getTensorPropCR(ctx, o, "gamma2", ok, err); if (!ok) return err;
    const nngpu::GpuTensor* b2g  = getTensorPropCR(ctx, o, "beta2",  ok, err); if (!ok) return err;
    const nngpu::GpuTensor* W2   = getTensorPropCR(ctx, o, "W2_int8", ok, err); if (!ok) return err;
    const nngpu::GpuTensor* s2   = getTensorPropCR(ctx, o, "s2",     ok, err); if (!ok) return err;
    const nngpu::GpuTensor* b2   = getTensorPropCR(ctx, o, "b2",     ok, err); if (!ok) return err;
    const nngpu::GpuTensor* Wsk  = getTensorPropCR(ctx, o, "Wskip_int8", ok, err); if (!ok) return err;
    const nngpu::GpuTensor* ssk  = getTensorPropCR(ctx, o, "sskip",  ok, err); if (!ok) return err;
    const nngpu::GpuTensor* bsk  = getTensorPropCR(ctx, o, "bskip",  ok, err); if (!ok) return err;
    nngpu::GpuTensor* Y          = getTensorPropMR(ctx, o, "Y",      ok, err); if (!ok) return err;
    if (!X || !g1 || !b1g || !W1 || !s1 || !g2 || !b2g || !W2 || !s2 || !Y) {
        return JS_ThrowTypeError(ctx,
            "resblockForwardInt8wFp16: required X,gamma1,beta1,W1_int8,s1,gamma2,beta2,W2_int8,s2,Y missing");
    }
    int32_t N    = getIntPropI8(ctx, o, "N",    0);
    int32_t Cin  = getIntPropI8(ctx, o, "C_in", 0);
    int32_t Cout = getIntPropI8(ctx, o, "C_out",0);
    int32_t H    = getIntPropI8(ctx, o, "H",    0);
    int32_t W    = getIntPropI8(ctx, o, "W",    0);
    int32_t ng   = getIntPropI8(ctx, o, "numGroups", 32);
    JSValue epsV = JS_GetPropertyStr(ctx, o, "eps");
    double eps = 1e-5; if (JS_IsNumber(epsV)) JS_ToFloat64(ctx, &eps, epsV);
    JS_FreeValue(ctx, epsV);
    nngpu::resblock_forward_int8w_fp16_gpu(*X, *g1, *b1g, *W1, *s1, b1, tem,
                                           *g2, *b2g, *W2, *s2, b2, Wsk, ssk, bsk,
                                           N, Cin, Cout, H, W, ng, (float)eps, *Y);
    return JS_UNDEFINED;
}

// ─── W8A16 flash-attention triplet ────────────────────────────────────────

// flashAttentionProjectKvInt8wFp16(ctx, Wk_int8, sk, bk|null, Wv_int8, sv,
//                                  bv|null, K_out, V_out)
static JSValue js_flashAttentionProjectKvInt8wFp16(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 9) return JS_ThrowTypeError(ctx,
        "flashAttentionProjectKvInt8wFp16(ctx,Wk_int8,sk,bk|null,Wv_int8,sv,bv|null,K_out,V_out)");
    ENSURE_INIT();
    GT(C,  0, "flashAttentionProjectKvInt8wFp16");
    GT(Wk, 1, "flashAttentionProjectKvInt8wFp16");
    GT(sk, 2, "flashAttentionProjectKvInt8wFp16");
    JSValue err = JS_UNDEFINED;
    const nngpu::GpuTensor* bk = nullptr;
    if (!resolveOptionalConstGpuTensor(ctx, argv[3], bk, err, "bk")) return err;
    GT(Wv, 4, "flashAttentionProjectKvInt8wFp16");
    GT(sv, 5, "flashAttentionProjectKvInt8wFp16");
    const nngpu::GpuTensor* bv = nullptr;
    if (!resolveOptionalConstGpuTensor(ctx, argv[6], bv, err, "bv")) return err;
    GT(Ko, 7, "flashAttentionProjectKvInt8wFp16");
    GT(Vo, 8, "flashAttentionProjectKvInt8wFp16");
    nngpu::flash_attention_project_kv_int8w_fp16_gpu(*C, *Wk, *sk, bk, *Wv, *sv, bv,
                                                     *Ko, *Vo);
    return JS_UNDEFINED;
}

// flashAttentionQWithKvCachedInt8wFp16(X, K, V, Wq_int8, sq, bq|null,
//                                      Wo_int8, so, bo|null, mask|null,
//                                      numHeads, causal, O)
static JSValue js_flashAttentionQWithKvCachedInt8wFp16(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 13) return JS_ThrowTypeError(ctx,
        "flashAttentionQWithKvCachedInt8wFp16(X,K,V,Wq_int8,sq,bq|null,Wo_int8,so,bo|null,mask|null,numHeads,causal,O)");
    ENSURE_INIT();
    GT(X,  0, "flashAttentionQWithKvCachedInt8wFp16");
    GT(K,  1, "flashAttentionQWithKvCachedInt8wFp16");
    GT(V,  2, "flashAttentionQWithKvCachedInt8wFp16");
    GT(Wq, 3, "flashAttentionQWithKvCachedInt8wFp16");
    GT(sq, 4, "flashAttentionQWithKvCachedInt8wFp16");
    JSValue err = JS_UNDEFINED;
    const nngpu::GpuTensor* bq = nullptr;
    if (!resolveOptionalConstGpuTensor(ctx, argv[5], bq, err, "bq")) return err;
    GT(Wo, 6, "flashAttentionQWithKvCachedInt8wFp16");
    GT(so, 7, "flashAttentionQWithKvCachedInt8wFp16");
    const nngpu::GpuTensor* bo = nullptr;
    if (!resolveOptionalConstGpuTensor(ctx, argv[8], bo, err, "bo")) return err;
    const float* mask = nullptr;
    if (!resolveDeviceMask(ctx, argv[9], mask, err)) return err;
    int32_t numHeads = 1; JS_ToInt32(ctx, &numHeads, argv[10]);
    bool causal = getBool(ctx, argv[11], false);
    GT(O, 12, "flashAttentionQWithKvCachedInt8wFp16");
    nngpu::flash_attention_q_with_kv_cached_int8w_fp16_gpu(
        *X, *K, *V, *Wq, *sq, bq, *Wo, *so, bo, mask, numHeads, causal, *O);
    return JS_UNDEFINED;
}

// flashAttentionQkvoInt8wFp16(opts):
//   required: X, Wq_int8, sq, Wk_int8, sk, Wv_int8, sv, Wo_int8, so, O,
//             numHeads
//   optional: Ctx, bq, bk, bv, bo, mask, causal (default false)
static JSValue js_flashAttentionQkvoInt8wFp16(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 1 || !JS_IsObject(argv[0])) return JS_ThrowTypeError(ctx,
        "flashAttentionQkvoInt8wFp16(opts) — see docs");
    ENSURE_INIT();
    JSValueConst o = argv[0];
    JSValue err = JS_UNDEFINED;
    bool ok = true;
    const nngpu::GpuTensor* X    = getTensorPropCR(ctx, o, "X",       ok, err); if (!ok) return err;
    const nngpu::GpuTensor* C    = getTensorPropCR(ctx, o, "Ctx",     ok, err); if (!ok) return err;
    const nngpu::GpuTensor* Wq   = getTensorPropCR(ctx, o, "Wq_int8", ok, err); if (!ok) return err;
    const nngpu::GpuTensor* sq   = getTensorPropCR(ctx, o, "sq",      ok, err); if (!ok) return err;
    const nngpu::GpuTensor* bq   = getTensorPropCR(ctx, o, "bq",      ok, err); if (!ok) return err;
    const nngpu::GpuTensor* Wk   = getTensorPropCR(ctx, o, "Wk_int8", ok, err); if (!ok) return err;
    const nngpu::GpuTensor* sk   = getTensorPropCR(ctx, o, "sk",      ok, err); if (!ok) return err;
    const nngpu::GpuTensor* bk   = getTensorPropCR(ctx, o, "bk",      ok, err); if (!ok) return err;
    const nngpu::GpuTensor* Wv   = getTensorPropCR(ctx, o, "Wv_int8", ok, err); if (!ok) return err;
    const nngpu::GpuTensor* sv   = getTensorPropCR(ctx, o, "sv",      ok, err); if (!ok) return err;
    const nngpu::GpuTensor* bv   = getTensorPropCR(ctx, o, "bv",      ok, err); if (!ok) return err;
    const nngpu::GpuTensor* Wo   = getTensorPropCR(ctx, o, "Wo_int8", ok, err); if (!ok) return err;
    const nngpu::GpuTensor* so   = getTensorPropCR(ctx, o, "so",      ok, err); if (!ok) return err;
    const nngpu::GpuTensor* bo   = getTensorPropCR(ctx, o, "bo",      ok, err); if (!ok) return err;
    nngpu::GpuTensor* Outt       = getTensorPropMR(ctx, o, "O",       ok, err); if (!ok) return err;

    if (!X || !Wq || !sq || !Wk || !sk || !Wv || !sv || !Wo || !so || !Outt) {
        return JS_ThrowTypeError(ctx,
            "flashAttentionQkvoInt8wFp16: required X,Wq_int8,sq,Wk_int8,sk,Wv_int8,sv,Wo_int8,so,O missing");
    }

    JSValue maskV = JS_GetPropertyStr(ctx, o, "mask");
    const float* mask = nullptr;
    bool maskOk = resolveDeviceMask(ctx, maskV, mask, err);
    JS_FreeValue(ctx, maskV);
    if (!maskOk) return err;

    int32_t numHeads = getIntPropI8(ctx, o, "numHeads", 1);
    JSValue cV = JS_GetPropertyStr(ctx, o, "causal");
    bool causal = false;
    if (!JS_IsUndefined(cV) && !JS_IsNull(cV)) causal = JS_ToBool(ctx, cV) ? true : false;
    JS_FreeValue(ctx, cV);

    nngpu::flash_attention_qkvo_int8w_fp16_gpu(
        *X, C, *Wq, *sq, bq, *Wk, *sk, bk, *Wv, *sv, bv, *Wo, *so, bo,
        mask, numHeads, causal, *Outt);
    return JS_UNDEFINED;
}

#undef GT
#undef ENSURE_INIT

void installTensorInt8Ops(JSContext* ctx, JSValue gpuObj) {
    JS_SetPropertyStr(ctx, gpuObj, "quantizeInt8PerRowHost",
        JS_NewCFunction(ctx, js_quantizeInt8PerRowHost, "quantizeInt8PerRowHost", 3));
    JS_SetPropertyStr(ctx, gpuObj, "matmulInt8wFp16",
        JS_NewCFunction(ctx, js_matmulInt8wFp16, "matmulInt8wFp16", 4));
    JS_SetPropertyStr(ctx, gpuObj, "conv2dInt8wFp16Forward",
        JS_NewCFunction(ctx, js_conv2dInt8wFp16Forward, "conv2dInt8wFp16Forward", 19));
    JS_SetPropertyStr(ctx, gpuObj, "linearForwardBatchedInt8wFp16",
        JS_NewCFunction(ctx, js_linearForwardBatchedInt8wFp16, "linearForwardBatchedInt8wFp16", 5));
    JS_SetPropertyStr(ctx, gpuObj, "resblockForwardInt8wFp16",
        JS_NewCFunction(ctx, js_resblockForwardInt8wFp16, "resblockForwardInt8wFp16", 1));
    JS_SetPropertyStr(ctx, gpuObj, "flashAttentionProjectKvInt8wFp16",
        JS_NewCFunction(ctx, js_flashAttentionProjectKvInt8wFp16, "flashAttentionProjectKvInt8wFp16", 9));
    JS_SetPropertyStr(ctx, gpuObj, "flashAttentionQWithKvCachedInt8wFp16",
        JS_NewCFunction(ctx, js_flashAttentionQWithKvCachedInt8wFp16, "flashAttentionQWithKvCachedInt8wFp16", 13));
    JS_SetPropertyStr(ctx, gpuObj, "flashAttentionQkvoInt8wFp16",
        JS_NewCFunction(ctx, js_flashAttentionQkvoInt8wFp16, "flashAttentionQkvoInt8wFp16", 1));
}

} // namespace bro::js

#else // !BROTENSOR_HAS_GPU

#include <qjsbind/qjsbind.h>
namespace bro::js {
void installTensorInt8Ops(JSContext*, JSValue) {}
} // namespace bro::js

#endif // BROTENSOR_HAS_GPU
