#if BRO_WITH_TENSOR && defined(BROTENSOR_HAS_GPU)
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
    nngpu::matmul_int8w_fp16(*W, *S, *X, *Y);
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
    const nngpu::Tensor* bias = nullptr;
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
    nngpu::conv2d_int8w_fp16_forward(*X, *Wt, *Sc, bias,
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
    const nngpu::Tensor* bias = nullptr;
    if (!resolveOptionalConstGpuTensor(ctx, argv[2], bias, err, "bias")) return err;
    GT(X, 3, "linearForwardBatchedInt8wFp16");
    GT(Y, 4, "linearForwardBatchedInt8wFp16");
    nngpu::linear_forward_batched_int8w_fp16(*W, *S, bias, *X, *Y);
    return JS_UNDEFINED;
}

// ─── W8A16 resblock forward (options object) ──────────────────────────────
//
// resblockForwardInt8wFp16(opts):
//   required: X, gamma1, beta1, W1_int8, s1, gamma2, beta2, W2_int8, s2, Y,
//             N, C_in, C_out, H, W
//   optional: b1, t_emb_shift, b2, Wskip_int8, sskip, bskip,
//             numGroups (32), eps (1e-5)
static const nngpu::Tensor* getTensorPropCR(JSContext* ctx, JSValueConst obj,
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
static nngpu::Tensor* getTensorPropMR(JSContext* ctx, JSValueConst obj,
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
    const nngpu::Tensor* X    = getTensorPropCR(ctx, o, "X",    ok, err); if (!ok) return err;
    const nngpu::Tensor* g1   = getTensorPropCR(ctx, o, "gamma1", ok, err); if (!ok) return err;
    const nngpu::Tensor* b1g  = getTensorPropCR(ctx, o, "beta1",  ok, err); if (!ok) return err;
    const nngpu::Tensor* W1   = getTensorPropCR(ctx, o, "W1_int8", ok, err); if (!ok) return err;
    const nngpu::Tensor* s1   = getTensorPropCR(ctx, o, "s1",     ok, err); if (!ok) return err;
    const nngpu::Tensor* b1   = getTensorPropCR(ctx, o, "b1",     ok, err); if (!ok) return err;
    const nngpu::Tensor* tem  = getTensorPropCR(ctx, o, "t_emb_shift", ok, err); if (!ok) return err;
    const nngpu::Tensor* g2   = getTensorPropCR(ctx, o, "gamma2", ok, err); if (!ok) return err;
    const nngpu::Tensor* b2g  = getTensorPropCR(ctx, o, "beta2",  ok, err); if (!ok) return err;
    const nngpu::Tensor* W2   = getTensorPropCR(ctx, o, "W2_int8", ok, err); if (!ok) return err;
    const nngpu::Tensor* s2   = getTensorPropCR(ctx, o, "s2",     ok, err); if (!ok) return err;
    const nngpu::Tensor* b2   = getTensorPropCR(ctx, o, "b2",     ok, err); if (!ok) return err;
    const nngpu::Tensor* Wsk  = getTensorPropCR(ctx, o, "Wskip_int8", ok, err); if (!ok) return err;
    const nngpu::Tensor* ssk  = getTensorPropCR(ctx, o, "sskip",  ok, err); if (!ok) return err;
    const nngpu::Tensor* bsk  = getTensorPropCR(ctx, o, "bskip",  ok, err); if (!ok) return err;
    nngpu::Tensor* Y          = getTensorPropMR(ctx, o, "Y",      ok, err); if (!ok) return err;
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
    nngpu::resblock_forward_int8w_fp16(*X, *g1, *b1g, *W1, *s1, b1, tem,
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
    const nngpu::Tensor* bk = nullptr;
    if (!resolveOptionalConstGpuTensor(ctx, argv[3], bk, err, "bk")) return err;
    GT(Wv, 4, "flashAttentionProjectKvInt8wFp16");
    GT(sv, 5, "flashAttentionProjectKvInt8wFp16");
    const nngpu::Tensor* bv = nullptr;
    if (!resolveOptionalConstGpuTensor(ctx, argv[6], bv, err, "bv")) return err;
    GT(Ko, 7, "flashAttentionProjectKvInt8wFp16");
    GT(Vo, 8, "flashAttentionProjectKvInt8wFp16");
    nngpu::flash_attention_project_kv_int8w_fp16(*C, *Wk, *sk, bk, *Wv, *sv, bv,
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
    const nngpu::Tensor* bq = nullptr;
    if (!resolveOptionalConstGpuTensor(ctx, argv[5], bq, err, "bq")) return err;
    GT(Wo, 6, "flashAttentionQWithKvCachedInt8wFp16");
    GT(so, 7, "flashAttentionQWithKvCachedInt8wFp16");
    const nngpu::Tensor* bo = nullptr;
    if (!resolveOptionalConstGpuTensor(ctx, argv[8], bo, err, "bo")) return err;
    const float* mask = nullptr;
    if (!resolveDeviceMask(ctx, argv[9], mask, err)) return err;
    int32_t numHeads = 1; JS_ToInt32(ctx, &numHeads, argv[10]);
    bool causal = getBool(ctx, argv[11], false);
    GT(O, 12, "flashAttentionQWithKvCachedInt8wFp16");
    nngpu::flash_attention_q_with_kv_cached_int8w_fp16(
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
    const nngpu::Tensor* X    = getTensorPropCR(ctx, o, "X",       ok, err); if (!ok) return err;
    const nngpu::Tensor* C    = getTensorPropCR(ctx, o, "Ctx",     ok, err); if (!ok) return err;
    const nngpu::Tensor* Wq   = getTensorPropCR(ctx, o, "Wq_int8", ok, err); if (!ok) return err;
    const nngpu::Tensor* sq   = getTensorPropCR(ctx, o, "sq",      ok, err); if (!ok) return err;
    const nngpu::Tensor* bq   = getTensorPropCR(ctx, o, "bq",      ok, err); if (!ok) return err;
    const nngpu::Tensor* Wk   = getTensorPropCR(ctx, o, "Wk_int8", ok, err); if (!ok) return err;
    const nngpu::Tensor* sk   = getTensorPropCR(ctx, o, "sk",      ok, err); if (!ok) return err;
    const nngpu::Tensor* bk   = getTensorPropCR(ctx, o, "bk",      ok, err); if (!ok) return err;
    const nngpu::Tensor* Wv   = getTensorPropCR(ctx, o, "Wv_int8", ok, err); if (!ok) return err;
    const nngpu::Tensor* sv   = getTensorPropCR(ctx, o, "sv",      ok, err); if (!ok) return err;
    const nngpu::Tensor* bv   = getTensorPropCR(ctx, o, "bv",      ok, err); if (!ok) return err;
    const nngpu::Tensor* Wo   = getTensorPropCR(ctx, o, "Wo_int8", ok, err); if (!ok) return err;
    const nngpu::Tensor* so   = getTensorPropCR(ctx, o, "so",      ok, err); if (!ok) return err;
    const nngpu::Tensor* bo   = getTensorPropCR(ctx, o, "bo",      ok, err); if (!ok) return err;
    nngpu::Tensor* Outt       = getTensorPropMR(ctx, o, "O",       ok, err); if (!ok) return err;

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

    nngpu::flash_attention_qkvo_int8w_fp16(
        *X, C, *Wq, *sq, bq, *Wk, *sk, bk, *Wv, *sv, bv, *Wo, *so, bo,
        mask, numHeads, causal, *Outt);
    return JS_UNDEFINED;
}

// ─── W8A16 T5-style bias attention ────────────────────────────────────────
//
// selfAttentionBiasInt8wFp16(X, Wq_int8, sq, Wk_int8, sk, Wv_int8, sv,
//                            Wo_int8, so, mask|null, attnBias|null,
//                            numHeads, scale, O)
//
// W8A16 variant of selfAttentionBiasForward — the quantised T5 encoder
// attention. Each projection weight is an INT8 (D, D) matrix paired with an
// FP32 (D, 1) per-output-row dequant scale; activations stay FP16.
static JSValue js_selfAttentionBiasInt8wFp16(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 14) return JS_ThrowTypeError(ctx,
        "selfAttentionBiasInt8wFp16(X,Wq_int8,sq,Wk_int8,sk,Wv_int8,sv,Wo_int8,so,mask|null,attnBias|null,numHeads,scale,O)");
    ENSURE_INIT();
    GT(X,  0, "selfAttentionBiasInt8wFp16");
    GT(Wq, 1, "selfAttentionBiasInt8wFp16"); GT(sq, 2, "selfAttentionBiasInt8wFp16");
    GT(Wk, 3, "selfAttentionBiasInt8wFp16"); GT(sk, 4, "selfAttentionBiasInt8wFp16");
    GT(Wv, 5, "selfAttentionBiasInt8wFp16"); GT(sv, 6, "selfAttentionBiasInt8wFp16");
    GT(Wo, 7, "selfAttentionBiasInt8wFp16"); GT(so, 8, "selfAttentionBiasInt8wFp16");
    JSValue err = JS_UNDEFINED;
    const float* mask = nullptr;
    if (!resolveDeviceMask(ctx, argv[9], mask, err)) return err;
    const nngpu::Tensor* attnBias = nullptr;
    if (!resolveOptionalConstGpuTensor(ctx, argv[10], attnBias, err, "attnBias")) return err;
    int32_t numHeads = 1; JS_ToInt32(ctx, &numHeads, argv[11]);
    double scale = 1.0; JS_ToFloat64(ctx, &scale, argv[12]);
    GT(O, 13, "selfAttentionBiasInt8wFp16");
    nngpu::self_attention_bias_int8w_fp16(*X, *Wq, *sq, *Wk, *sk, *Wv, *sv, *Wo, *so,
                                          mask, attnBias, numHeads, (float)scale, *O);
    return JS_UNDEFINED;
}

// ─── GGUF k-quant dequant (Q4_K / Q6_K / Q8_0 → FP16) ──────────────────────
#define DEQUANT_OP(jsName, fn)                                                  \
    static JSValue js_##jsName(JSContext* ctx, JSValueConst, int argc,          \
                               JSValueConst* argv) {                            \
        if (argc < 2) return JS_ThrowTypeError(ctx, #jsName "(W_q, W_fp16)");   \
        ENSURE_INIT();                                                          \
        GT(Wq, 0, #jsName); GT(Wf, 1, #jsName);                                 \
        nngpu::fn(*Wq, *Wf);                                                    \
        return JS_UNDEFINED;                                                    \
    }
DEQUANT_OP(dequantQ4kToFp16,  dequant_q4k_to_fp16)
DEQUANT_OP(dequantQ6kToFp16,  dequant_q6k_to_fp16)
DEQUANT_OP(dequantQ8_0ToFp16, dequant_q8_0_to_fp16)
#undef DEQUANT_OP

// ─── k-quant weight-only linear (W=GGUF k-quant, X/Y FP16) ─────────────────
// linearForwardQ*kFp16(W_q, bias|null, x, y) — single-token (x: D, y: Dout).
// linearForwardBatchedQ*kFp16(W_q, bias|null, X_BD, Y_BD) — batched rows.
#define QLINEAR_OP(jsName, fn)                                                  \
    static JSValue js_##jsName(JSContext* ctx, JSValueConst, int argc,          \
                               JSValueConst* argv) {                            \
        if (argc < 4) return JS_ThrowTypeError(ctx, #jsName "(W_q,bias|null,X,Y)"); \
        ENSURE_INIT();                                                          \
        GT(Wq, 0, #jsName);                                                     \
        JSValue err = JS_UNDEFINED; const nngpu::Tensor* bias = nullptr;        \
        if (!resolveOptionalConstGpuTensor(ctx, argv[1], bias, err, "bias")) return err; \
        GT(X, 2, #jsName); GT(Y, 3, #jsName);                                   \
        nngpu::fn(*Wq, bias, *X, *Y);                                           \
        return JS_UNDEFINED;                                                    \
    }
QLINEAR_OP(linearForwardQ4kFp16,         linear_forward_q4k_fp16)
QLINEAR_OP(linearForwardQ6kFp16,         linear_forward_q6k_fp16)
QLINEAR_OP(linearForwardQ8_0Fp16,        linear_forward_q8_0_fp16)
QLINEAR_OP(linearForwardBatchedQ4kFp16,  linear_forward_batched_q4k_fp16)
QLINEAR_OP(linearForwardBatchedQ6kFp16,  linear_forward_batched_q6k_fp16)
QLINEAR_OP(linearForwardBatchedQ8_0Fp16, linear_forward_batched_q8_0_fp16)
#undef QLINEAR_OP

// ─── conv3d W8A16 (INT8 weight, FP16 activation), N,C,T,H,W ─────────────────
static JSValue js_conv3dInt8wFp16Forward(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 24) return JS_ThrowTypeError(ctx,
        "conv3dInt8wFp16Forward(X,W_int8,scales,bias|null,N,C_in,T,H,W,C_out,kT,kH,kW,sT,sH,sW,pT,pH,pW,dT,dH,dW,groups,Y)");
    ENSURE_INIT();
    GT(X, 0, "conv3dInt8wFp16Forward"); GT(Wq, 1, "conv3dInt8wFp16Forward");
    GT(scales, 2, "conv3dInt8wFp16Forward");
    JSValue err = JS_UNDEFINED; const nngpu::Tensor* bias = nullptr;
    if (!resolveOptionalConstGpuTensor(ctx, argv[3], bias, err, "bias")) return err;
    int32_t N=0,Cin=0,T=0,H=0,W=0,Cout=0,kT=0,kH=0,kW=0;
    int32_t sT=1,sH=1,sW=1,pT=0,pH=0,pW=0,dT=1,dH=1,dW=1,groups=1;
    JS_ToInt32(ctx, &N, argv[4]);   JS_ToInt32(ctx, &Cin, argv[5]);
    JS_ToInt32(ctx, &T, argv[6]);   JS_ToInt32(ctx, &H, argv[7]);
    JS_ToInt32(ctx, &W, argv[8]);   JS_ToInt32(ctx, &Cout, argv[9]);
    JS_ToInt32(ctx, &kT, argv[10]); JS_ToInt32(ctx, &kH, argv[11]);
    JS_ToInt32(ctx, &kW, argv[12]); JS_ToInt32(ctx, &sT, argv[13]);
    JS_ToInt32(ctx, &sH, argv[14]); JS_ToInt32(ctx, &sW, argv[15]);
    JS_ToInt32(ctx, &pT, argv[16]); JS_ToInt32(ctx, &pH, argv[17]);
    JS_ToInt32(ctx, &pW, argv[18]); JS_ToInt32(ctx, &dT, argv[19]);
    JS_ToInt32(ctx, &dH, argv[20]); JS_ToInt32(ctx, &dW, argv[21]);
    JS_ToInt32(ctx, &groups, argv[22]);
    GT(Y, 23, "conv3dInt8wFp16Forward");
    nngpu::conv3d_int8w_fp16_forward(*X, *Wq, *scales, bias, N, Cin, T, H, W,
                                     Cout, kT, kH, kW, sT, sH, sW, pT, pH, pW,
                                     dT, dH, dW, groups, *Y);
    return JS_UNDEFINED;
}

#undef GT
#undef ENSURE_INIT

void installTensorInt8Ops(JSContext* ctx, JSValue gpuObj) {
    JS_SetPropertyStr(ctx, gpuObj, "dequantQ4kToFp16",  JS_NewCFunction(ctx, js_dequantQ4kToFp16,  "dequantQ4kToFp16",  2));
    JS_SetPropertyStr(ctx, gpuObj, "dequantQ6kToFp16",  JS_NewCFunction(ctx, js_dequantQ6kToFp16,  "dequantQ6kToFp16",  2));
    JS_SetPropertyStr(ctx, gpuObj, "dequantQ8_0ToFp16", JS_NewCFunction(ctx, js_dequantQ8_0ToFp16, "dequantQ8_0ToFp16", 2));
    JS_SetPropertyStr(ctx, gpuObj, "linearForwardQ4kFp16",         JS_NewCFunction(ctx, js_linearForwardQ4kFp16,         "linearForwardQ4kFp16",         4));
    JS_SetPropertyStr(ctx, gpuObj, "linearForwardQ6kFp16",         JS_NewCFunction(ctx, js_linearForwardQ6kFp16,         "linearForwardQ6kFp16",         4));
    JS_SetPropertyStr(ctx, gpuObj, "linearForwardQ8_0Fp16",        JS_NewCFunction(ctx, js_linearForwardQ8_0Fp16,        "linearForwardQ8_0Fp16",        4));
    JS_SetPropertyStr(ctx, gpuObj, "linearForwardBatchedQ4kFp16",  JS_NewCFunction(ctx, js_linearForwardBatchedQ4kFp16,  "linearForwardBatchedQ4kFp16",  4));
    JS_SetPropertyStr(ctx, gpuObj, "linearForwardBatchedQ6kFp16",  JS_NewCFunction(ctx, js_linearForwardBatchedQ6kFp16,  "linearForwardBatchedQ6kFp16",  4));
    JS_SetPropertyStr(ctx, gpuObj, "linearForwardBatchedQ8_0Fp16", JS_NewCFunction(ctx, js_linearForwardBatchedQ8_0Fp16, "linearForwardBatchedQ8_0Fp16", 4));
    JS_SetPropertyStr(ctx, gpuObj, "conv3dInt8wFp16Forward",       JS_NewCFunction(ctx, js_conv3dInt8wFp16Forward,       "conv3dInt8wFp16Forward",      24));

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
    JS_SetPropertyStr(ctx, gpuObj, "selfAttentionBiasInt8wFp16",
        JS_NewCFunction(ctx, js_selfAttentionBiasInt8wFp16, "selfAttentionBiasInt8wFp16", 14));
}

} // namespace bro::js

#else // !BROTENSOR_HAS_GPU

#include <qjsbind/qjsbind.h>
namespace bro::js {
void installTensorInt8Ops(JSContext*, JSValue) {}
} // namespace bro::js

#endif // BROTENSOR_HAS_GPU

#endif  // BRO_WITH_TENSOR
