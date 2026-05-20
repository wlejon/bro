// JS bindings — self/cross attention, flash-attention family, kv-cache,
// resblock. See tensor_bindings.cpp for the architectural overview.

#ifdef BROTENSOR_HAS_GPU

#include "js/tensor_bindings_internal.h"

namespace bro::js {

#define ENSURE_INIT() BROTENSOR_ENSURE_INIT()
#define GT(name, idx, label) BROTENSOR_GT(name, idx, label)

// ─── FP32 training-side self / cross attention ────────────────────────────

// Inference convenience: self_attention_forward (no caches).
static JSValue js_selfAttentionForward(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 8) return JS_ThrowTypeError(ctx,
        "selfAttentionForward(X,Wq,Wk,Wv,Wo,mask|null,numHeads,O)");
    ENSURE_INIT();
    GT(X,  0, "selfAttentionForward"); GT(Wq, 1, "selfAttentionForward");
    GT(Wk, 2, "selfAttentionForward"); GT(Wv, 3, "selfAttentionForward");
    GT(Wo, 4, "selfAttentionForward");
    const float* mask = nullptr; JSValue err = JS_UNDEFINED;
    if (!resolveDeviceMask(ctx, argv[5], mask, err)) return err;
    int32_t numHeads = 1; JS_ToInt32(ctx, &numHeads, argv[6]);
    GT(O, 7, "selfAttentionForward");
    nngpu::self_attention_forward(*X, *Wq, *Wk, *Wv, *Wo, mask, numHeads, *O);
    return JS_UNDEFINED;
}

static JSValue js_selfAttentionForwardTrain(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 13) return JS_ThrowTypeError(ctx,
        "selfAttentionForwardTrain(X,Wq,Wk,Wv,Wo,mask|null,numHeads,Qh,Kh,Vh,Attnh,Yconcat,O)");
    ENSURE_INIT();
    GT(X,  0, "selfAttentionForwardTrain"); GT(Wq, 1, "selfAttentionForwardTrain");
    GT(Wk, 2, "selfAttentionForwardTrain"); GT(Wv, 3, "selfAttentionForwardTrain");
    GT(Wo, 4, "selfAttentionForwardTrain");
    const float* mask = nullptr; JSValue err = JS_UNDEFINED;
    if (!resolveDeviceMask(ctx, argv[5], mask, err)) return err;
    int32_t numHeads = 1; JS_ToInt32(ctx, &numHeads, argv[6]);
    GT(Qh,    7,  "selfAttentionForwardTrain"); GT(Kh,    8,  "selfAttentionForwardTrain");
    GT(Vh,    9,  "selfAttentionForwardTrain"); GT(Attnh, 10, "selfAttentionForwardTrain");
    GT(Yc,    11, "selfAttentionForwardTrain"); GT(O,     12, "selfAttentionForwardTrain");
    nngpu::self_attention_forward_train(*X, *Wq, *Wk, *Wv, *Wo, mask, numHeads,
                                            *Qh, *Kh, *Vh, *Attnh, *Yc, *O);
    return JS_UNDEFINED;
}

static JSValue js_selfAttentionBackward(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 18) return JS_ThrowTypeError(ctx,
        "selfAttentionBackward(dO,X,Qh,Kh,Vh,Attnh,Yconcat,Wq,Wk,Wv,Wo,mask|null,numHeads,dX,dWq,dWk,dWv,dWo)");
    ENSURE_INIT();
    GT(dO,    0,  "selfAttentionBackward"); GT(X,     1,  "selfAttentionBackward");
    GT(Qh,    2,  "selfAttentionBackward"); GT(Kh,    3,  "selfAttentionBackward");
    GT(Vh,    4,  "selfAttentionBackward"); GT(Attnh, 5,  "selfAttentionBackward");
    GT(Yc,    6,  "selfAttentionBackward"); GT(Wq,    7,  "selfAttentionBackward");
    GT(Wk,    8,  "selfAttentionBackward"); GT(Wv,    9,  "selfAttentionBackward");
    GT(Wo,   10,  "selfAttentionBackward");
    const float* mask = nullptr; JSValue err = JS_UNDEFINED;
    if (!resolveDeviceMask(ctx, argv[11], mask, err)) return err;
    int32_t numHeads = 1; JS_ToInt32(ctx, &numHeads, argv[12]);
    GT(dX,   13, "selfAttentionBackward"); GT(dWq,  14, "selfAttentionBackward");
    GT(dWk,  15, "selfAttentionBackward"); GT(dWv,  16, "selfAttentionBackward");
    GT(dWo,  17, "selfAttentionBackward");
    nngpu::self_attention_backward(*dO, *X, *Qh, *Kh, *Vh, *Attnh, *Yc,
                                       *Wq, *Wk, *Wv, *Wo, mask, numHeads,
                                       *dX, *dWq, *dWk, *dWv, *dWo);
    return JS_UNDEFINED;
}

// ─── Cross-attention ──────────────────────────────────────────────────────

// Inference (FP16) cross-attention — no caches exposed.
static JSValue js_crossAttentionForward(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 9) return JS_ThrowTypeError(ctx,
        "crossAttentionForward(X,Ctx,Wq,Wk,Wv,Wo,mask|null,numHeads,O)");
    ENSURE_INIT();
    GT(X,   0, "crossAttentionForward"); GT(C,  1, "crossAttentionForward");
    GT(Wq,  2, "crossAttentionForward"); GT(Wk, 3, "crossAttentionForward");
    GT(Wv,  4, "crossAttentionForward"); GT(Wo, 5, "crossAttentionForward");
    const float* mask = nullptr; JSValue err = JS_UNDEFINED;
    if (!resolveDeviceMask(ctx, argv[6], mask, err)) return err;
    int32_t numHeads = 1; JS_ToInt32(ctx, &numHeads, argv[7]);
    GT(O, 8, "crossAttentionForward");
    nngpu::cross_attention_forward(*X, *C, *Wq, *Wk, *Wv, *Wo, mask, numHeads, *O);
    return JS_UNDEFINED;
}

// FP16 cross-attention with attention-map output + optional logit bias.
static JSValue js_crossAttentionForwardWithAttn(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 11) return JS_ThrowTypeError(ctx,
        "crossAttentionForwardWithAttn(X,Ctx,Wq,Wk,Wv,Wo,mask|null,attnLogitBias|null,numHeads,O,AttnAvg)");
    ENSURE_INIT();
    GT(X,   0, "crossAttentionForwardWithAttn"); GT(C,  1, "crossAttentionForwardWithAttn");
    GT(Wq,  2, "crossAttentionForwardWithAttn"); GT(Wk, 3, "crossAttentionForwardWithAttn");
    GT(Wv,  4, "crossAttentionForwardWithAttn"); GT(Wo, 5, "crossAttentionForwardWithAttn");
    const float* mask = nullptr; JSValue err = JS_UNDEFINED;
    if (!resolveDeviceMask(ctx, argv[6], mask, err)) return err;
    const nngpu::Tensor* bias = nullptr;
    if (!resolveOptionalConstGpuTensor(ctx, argv[7], bias, err, "attnLogitBias")) return err;
    int32_t numHeads = 1; JS_ToInt32(ctx, &numHeads, argv[8]);
    GT(O, 9, "crossAttentionForwardWithAttn"); GT(A, 10, "crossAttentionForwardWithAttn");
    nngpu::cross_attention_forward_with_attn(*X, *C, *Wq, *Wk, *Wv, *Wo,
                                                 mask, bias, numHeads, *O, *A);
    return JS_UNDEFINED;
}

// FP32 training cross-attention with caches.
static JSValue js_crossAttentionForwardTrain(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 14) return JS_ThrowTypeError(ctx,
        "crossAttentionForwardTrain(X,Ctx,Wq,Wk,Wv,Wo,mask|null,numHeads,Qh,Kh,Vh,Attnh,Yconcat,O)");
    ENSURE_INIT();
    GT(X,   0, "crossAttentionForwardTrain"); GT(C,  1, "crossAttentionForwardTrain");
    GT(Wq,  2, "crossAttentionForwardTrain"); GT(Wk, 3, "crossAttentionForwardTrain");
    GT(Wv,  4, "crossAttentionForwardTrain"); GT(Wo, 5, "crossAttentionForwardTrain");
    const float* mask = nullptr; JSValue err = JS_UNDEFINED;
    if (!resolveDeviceMask(ctx, argv[6], mask, err)) return err;
    int32_t numHeads = 1; JS_ToInt32(ctx, &numHeads, argv[7]);
    GT(Qh,    8,  "crossAttentionForwardTrain"); GT(Kh,    9,  "crossAttentionForwardTrain");
    GT(Vh,    10, "crossAttentionForwardTrain"); GT(Attnh, 11, "crossAttentionForwardTrain");
    GT(Yc,    12, "crossAttentionForwardTrain"); GT(O,     13, "crossAttentionForwardTrain");
    nngpu::cross_attention_forward_train(*X, *C, *Wq, *Wk, *Wv, *Wo, mask, numHeads,
                                             *Qh, *Kh, *Vh, *Attnh, *Yc, *O);
    return JS_UNDEFINED;
}

static JSValue js_crossAttentionBackward(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 20) return JS_ThrowTypeError(ctx,
        "crossAttentionBackward(dO,X,Ctx,Qh,Kh,Vh,Attnh,Yconcat,Wq,Wk,Wv,Wo,mask|null,numHeads,dX,dCtx,dWq,dWk,dWv,dWo)");
    ENSURE_INIT();
    GT(dO,    0,  "crossAttentionBackward"); GT(X,     1,  "crossAttentionBackward");
    GT(C,     2,  "crossAttentionBackward"); GT(Qh,    3,  "crossAttentionBackward");
    GT(Kh,    4,  "crossAttentionBackward"); GT(Vh,    5,  "crossAttentionBackward");
    GT(Attnh, 6,  "crossAttentionBackward"); GT(Yc,    7,  "crossAttentionBackward");
    GT(Wq,    8,  "crossAttentionBackward"); GT(Wk,    9,  "crossAttentionBackward");
    GT(Wv,    10, "crossAttentionBackward"); GT(Wo,    11, "crossAttentionBackward");
    const float* mask = nullptr; JSValue err = JS_UNDEFINED;
    if (!resolveDeviceMask(ctx, argv[12], mask, err)) return err;
    int32_t numHeads = 1; JS_ToInt32(ctx, &numHeads, argv[13]);
    GT(dX,    14, "crossAttentionBackward"); GT(dC,    15, "crossAttentionBackward");
    GT(dWq,   16, "crossAttentionBackward"); GT(dWk,   17, "crossAttentionBackward");
    GT(dWv,   18, "crossAttentionBackward"); GT(dWo,   19, "crossAttentionBackward");
    nngpu::cross_attention_backward(*dO, *X, *C, *Qh, *Kh, *Vh, *Attnh, *Yc,
                                        *Wq, *Wk, *Wv, *Wo, mask, numHeads,
                                        *dX, *dC, *dWq, *dWk, *dWv, *dWo);
    return JS_UNDEFINED;
}

// ─── Attention spatial moments ────────────────────────────────────────────

static JSValue js_attentionTokenMoments(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 5) return JS_ThrowTypeError(ctx,
        "attentionTokenMoments(Attn,hLat,wLat,mass,centroid)");
    ENSURE_INIT();
    GT(A, 0, "attentionTokenMoments");
    int32_t hLat = 0, wLat = 0;
    JS_ToInt32(ctx, &hLat, argv[1]);
    JS_ToInt32(ctx, &wLat, argv[2]);
    GT(mass, 3, "attentionTokenMoments"); GT(cen, 4, "attentionTokenMoments");
    nngpu::attention_token_moments(*A, hLat, wLat, *mass, *cen);
    return JS_UNDEFINED;
}

// ─── Flash attention ──────────────────────────────────────────────────────

static JSValue js_flashAttentionForward(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 7) return JS_ThrowTypeError(ctx,
        "flashAttentionForward(Q,K,V,mask|null,numHeads,causal,O)");
    ENSURE_INIT();
    GT(Q, 0, "flashAttentionForward"); GT(K, 1, "flashAttentionForward");
    GT(V, 2, "flashAttentionForward");
    const float* mask = nullptr; JSValue err = JS_UNDEFINED;
    if (!resolveDeviceMask(ctx, argv[3], mask, err)) return err;
    int32_t numHeads = 1; JS_ToInt32(ctx, &numHeads, argv[4]);
    bool causal = getBool(ctx, argv[5], false);
    GT(O, 6, "flashAttentionForward");
    nngpu::flash_attention_forward(*Q, *K, *V, mask, numHeads, causal, *O);
    return JS_UNDEFINED;
}

static JSValue js_flashAttentionBackward(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 10) return JS_ThrowTypeError(ctx,
        "flashAttentionBackward(Q,K,V,O,dO,mask|null,numHeads,causal,dQ,dK,dV)");
    ENSURE_INIT();
    GT(Q, 0, "flashAttentionBackward"); GT(K, 1, "flashAttentionBackward");
    GT(V, 2, "flashAttentionBackward"); GT(O, 3, "flashAttentionBackward");
    GT(dO, 4, "flashAttentionBackward");
    const float* mask = nullptr; JSValue err = JS_UNDEFINED;
    if (!resolveDeviceMask(ctx, argv[5], mask, err)) return err;
    int32_t numHeads = 1; JS_ToInt32(ctx, &numHeads, argv[6]);
    bool causal = getBool(ctx, argv[7], false);
    GT(dQ, 8, "flashAttentionBackward"); GT(dK, 9, "flashAttentionBackward");
    if (argc < 11) return JS_ThrowTypeError(ctx,
        "flashAttentionBackward(...,dQ,dK,dV)");
    GT(dV, 10, "flashAttentionBackward");
    nngpu::flash_attention_backward(*Q, *K, *V, *O, *dO, mask, numHeads, causal,
                                        *dQ, *dK, *dV);
    return JS_UNDEFINED;
}

// flashAttentionQkvoForward(X, Ctx|null, Wq, bq|null, Wk, bk|null, Wv, bv|null,
//                           Wo, bo|null, mask|null, numHeads, causal, O)
static JSValue js_flashAttentionQkvoForward(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 14) return JS_ThrowTypeError(ctx,
        "flashAttentionQkvoForward(X,Ctx|null,Wq,bq|null,Wk,bk|null,Wv,bv|null,Wo,bo|null,mask|null,numHeads,causal,O)");
    ENSURE_INIT();
    GT(X, 0, "flashAttentionQkvoForward");
    JSValue err = JS_UNDEFINED;
    const nngpu::Tensor* C = nullptr;
    if (!resolveOptionalConstGpuTensor(ctx, argv[1], C, err, "Ctx")) return err;
    GT(Wq, 2, "flashAttentionQkvoForward");
    const nngpu::Tensor* bq = nullptr;
    if (!resolveOptionalConstGpuTensor(ctx, argv[3], bq, err, "bq")) return err;
    GT(Wk, 4, "flashAttentionQkvoForward");
    const nngpu::Tensor* bk = nullptr;
    if (!resolveOptionalConstGpuTensor(ctx, argv[5], bk, err, "bk")) return err;
    GT(Wv, 6, "flashAttentionQkvoForward");
    const nngpu::Tensor* bv = nullptr;
    if (!resolveOptionalConstGpuTensor(ctx, argv[7], bv, err, "bv")) return err;
    GT(Wo, 8, "flashAttentionQkvoForward");
    const nngpu::Tensor* bo = nullptr;
    if (!resolveOptionalConstGpuTensor(ctx, argv[9], bo, err, "bo")) return err;
    const float* mask = nullptr;
    if (!resolveDeviceMask(ctx, argv[10], mask, err)) return err;
    int32_t numHeads = 1; JS_ToInt32(ctx, &numHeads, argv[11]);
    bool causal = getBool(ctx, argv[12], false);
    GT(O, 13, "flashAttentionQkvoForward");
    nngpu::flash_attention_qkvo_forward(*X, C, *Wq, bq, *Wk, bk, *Wv, bv, *Wo, bo,
                                            mask, numHeads, causal, *O);
    return JS_UNDEFINED;
}

// flashAttentionQkvoBackward — opts encoded as a single options object to
// keep the call site sane (22 args otherwise). Required keys:
//   X, Wq, Wk, Wv, Wo, dO, numHeads, dX, dWq, dWk, dWv, dWo
// Optional keys (default null/false):
//   Ctx, bq, bk, bv, bo, mask, causal, dCtx, dbq, dbk, dbv, dbo
static const nngpu::Tensor* getTensorProp(JSContext* ctx, JSValueConst obj,
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
static nngpu::Tensor* getTensorPropMut(JSContext* ctx, JSValueConst obj,
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
static int32_t getIntProp(JSContext* ctx, JSValueConst obj, const char* key, int32_t def) {
    JSValue v = JS_GetPropertyStr(ctx, obj, key);
    int32_t out = def;
    if (JS_IsNumber(v)) JS_ToInt32(ctx, &out, v);
    JS_FreeValue(ctx, v);
    return out;
}
static bool getBoolProp(JSContext* ctx, JSValueConst obj, const char* key, bool def) {
    JSValue v = JS_GetPropertyStr(ctx, obj, key);
    bool out = def;
    if (!JS_IsUndefined(v) && !JS_IsNull(v)) out = JS_ToBool(ctx, v) ? true : false;
    JS_FreeValue(ctx, v);
    return out;
}
static bool getMaskProp(JSContext* ctx, JSValueConst obj, const char* key,
                        const float*& out, JSValue& err) {
    JSValue v = JS_GetPropertyStr(ctx, obj, key);
    bool ok = resolveDeviceMask(ctx, v, out, err);
    JS_FreeValue(ctx, v);
    return ok;
}

static JSValue js_flashAttentionQkvoBackward(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 1 || !JS_IsObject(argv[0])) return JS_ThrowTypeError(ctx,
        "flashAttentionQkvoBackward(opts) — opts is an object; see docs");
    ENSURE_INIT();
    JSValueConst opts = argv[0];
    JSValue err = JS_UNDEFINED;
    bool ok = true;

    const nngpu::Tensor* X   = getTensorProp(ctx, opts, "X",   ok, err); if (!ok) return err;
    const nngpu::Tensor* C   = getTensorProp(ctx, opts, "Ctx", ok, err); if (!ok) return err;
    const nngpu::Tensor* Wq  = getTensorProp(ctx, opts, "Wq",  ok, err); if (!ok) return err;
    const nngpu::Tensor* bq  = getTensorProp(ctx, opts, "bq",  ok, err); if (!ok) return err;
    const nngpu::Tensor* Wk  = getTensorProp(ctx, opts, "Wk",  ok, err); if (!ok) return err;
    const nngpu::Tensor* bk  = getTensorProp(ctx, opts, "bk",  ok, err); if (!ok) return err;
    const nngpu::Tensor* Wv  = getTensorProp(ctx, opts, "Wv",  ok, err); if (!ok) return err;
    const nngpu::Tensor* bv  = getTensorProp(ctx, opts, "bv",  ok, err); if (!ok) return err;
    const nngpu::Tensor* Wo  = getTensorProp(ctx, opts, "Wo",  ok, err); if (!ok) return err;
    const nngpu::Tensor* bo  = getTensorProp(ctx, opts, "bo",  ok, err); if (!ok) return err;
    const nngpu::Tensor* dO  = getTensorProp(ctx, opts, "dO",  ok, err); if (!ok) return err;
    nngpu::Tensor* dX        = getTensorPropMut(ctx, opts, "dX",  ok, err); if (!ok) return err;
    nngpu::Tensor* dCtx      = getTensorPropMut(ctx, opts, "dCtx",ok, err); if (!ok) return err;
    nngpu::Tensor* dWq       = getTensorPropMut(ctx, opts, "dWq", ok, err); if (!ok) return err;
    nngpu::Tensor* dbq       = getTensorPropMut(ctx, opts, "dbq", ok, err); if (!ok) return err;
    nngpu::Tensor* dWk       = getTensorPropMut(ctx, opts, "dWk", ok, err); if (!ok) return err;
    nngpu::Tensor* dbk       = getTensorPropMut(ctx, opts, "dbk", ok, err); if (!ok) return err;
    nngpu::Tensor* dWv       = getTensorPropMut(ctx, opts, "dWv", ok, err); if (!ok) return err;
    nngpu::Tensor* dbv       = getTensorPropMut(ctx, opts, "dbv", ok, err); if (!ok) return err;
    nngpu::Tensor* dWo       = getTensorPropMut(ctx, opts, "dWo", ok, err); if (!ok) return err;
    nngpu::Tensor* dbo       = getTensorPropMut(ctx, opts, "dbo", ok, err); if (!ok) return err;

    const float* mask = nullptr;
    if (!getMaskProp(ctx, opts, "mask", mask, err)) return err;

    if (!X || !Wq || !Wk || !Wv || !Wo || !dO || !dX || !dWq || !dWk || !dWv || !dWo) {
        return JS_ThrowTypeError(ctx, "flashAttentionQkvoBackward: required tensors missing (X,Wq,Wk,Wv,Wo,dO,dX,dWq,dWk,dWv,dWo)");
    }

    int32_t numHeads = getIntProp(ctx, opts, "numHeads", 1);
    bool causal = getBoolProp(ctx, opts, "causal", false);

    nngpu::flash_attention_qkvo_backward(
        *X, C, *Wq, bq, *Wk, bk, *Wv, bv, *Wo, bo,
        mask, numHeads, causal,
        *dO, *dX, dCtx, *dWq, dbq, *dWk, dbk, *dWv, dbv, *dWo, dbo);
    return JS_UNDEFINED;
}

// flashAttentionProjectKv(ctx, Wk, bk|null, Wv, bv|null, K_out, V_out)
static JSValue js_flashAttentionProjectKv(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 7) return JS_ThrowTypeError(ctx,
        "flashAttentionProjectKv(ctx,Wk,bk|null,Wv,bv|null,K_out,V_out)");
    ENSURE_INIT();
    GT(C,  0, "flashAttentionProjectKv");
    GT(Wk, 1, "flashAttentionProjectKv");
    JSValue err = JS_UNDEFINED;
    const nngpu::Tensor* bk = nullptr;
    if (!resolveOptionalConstGpuTensor(ctx, argv[2], bk, err, "bk")) return err;
    GT(Wv, 3, "flashAttentionProjectKv");
    const nngpu::Tensor* bv = nullptr;
    if (!resolveOptionalConstGpuTensor(ctx, argv[4], bv, err, "bv")) return err;
    GT(Ko, 5, "flashAttentionProjectKv"); GT(Vo, 6, "flashAttentionProjectKv");
    nngpu::flash_attention_project_kv(*C, *Wk, bk, *Wv, bv, *Ko, *Vo);
    return JS_UNDEFINED;
}

// flashAttentionQWithKvCachedForward(X, K, V, Wq, bq|null, Wo, bo|null,
//                                    mask|null, numHeads, causal, O)
static JSValue js_flashAttentionQWithKvCachedForward(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 11) return JS_ThrowTypeError(ctx,
        "flashAttentionQWithKvCachedForward(X,K,V,Wq,bq|null,Wo,bo|null,mask|null,numHeads,causal,O)");
    ENSURE_INIT();
    GT(X, 0, "flashAttentionQWithKvCachedForward");
    GT(K, 1, "flashAttentionQWithKvCachedForward");
    GT(V, 2, "flashAttentionQWithKvCachedForward");
    GT(Wq, 3, "flashAttentionQWithKvCachedForward");
    JSValue err = JS_UNDEFINED;
    const nngpu::Tensor* bq = nullptr;
    if (!resolveOptionalConstGpuTensor(ctx, argv[4], bq, err, "bq")) return err;
    GT(Wo, 5, "flashAttentionQWithKvCachedForward");
    const nngpu::Tensor* bo = nullptr;
    if (!resolveOptionalConstGpuTensor(ctx, argv[6], bo, err, "bo")) return err;
    const float* mask = nullptr;
    if (!resolveDeviceMask(ctx, argv[7], mask, err)) return err;
    int32_t numHeads = 1; JS_ToInt32(ctx, &numHeads, argv[8]);
    bool causal = getBool(ctx, argv[9], false);
    GT(O, 10, "flashAttentionQWithKvCachedForward");
    nngpu::flash_attention_q_with_kv_cached_forward(*X, *K, *V, *Wq, bq, *Wo, bo,
                                                        mask, numHeads, causal, *O);
    return JS_UNDEFINED;
}

// flashAttentionDecode(Q, K_cache, V_cache, validLen, numHeads, O)
static JSValue js_flashAttentionDecode(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 6) return JS_ThrowTypeError(ctx,
        "flashAttentionDecode(Q,K_cache,V_cache,validLen,numHeads,O)");
    ENSURE_INIT();
    GT(Q,  0, "flashAttentionDecode");
    GT(Kc, 1, "flashAttentionDecode");
    GT(Vc, 2, "flashAttentionDecode");
    int32_t validLen = 0, numHeads = 1;
    JS_ToInt32(ctx, &validLen, argv[3]);
    JS_ToInt32(ctx, &numHeads, argv[4]);
    GT(O, 5, "flashAttentionDecode");
    nngpu::flash_attention_decode(*Q, *Kc, *Vc, validLen, numHeads, *O);
    return JS_UNDEFINED;
}

// ─── KV cache ─────────────────────────────────────────────────────────────

static JSValue js_kvCacheAppend(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 5) return JS_ThrowTypeError(ctx,
        "kvCacheAppend(K_new,V_new,curLen,K_cache,V_cache)");
    ENSURE_INIT();
    GT(Kn, 0, "kvCacheAppend"); GT(Vn, 1, "kvCacheAppend");
    int32_t curLen = 0; JS_ToInt32(ctx, &curLen, argv[2]);
    GT(Kc, 3, "kvCacheAppend"); GT(Vc, 4, "kvCacheAppend");
    nngpu::kv_cache_append(*Kn, *Vn, curLen, *Kc, *Vc);
    return JS_UNDEFINED;
}

// ─── Resblock ─────────────────────────────────────────────────────────────
//
// Forward and backward take many optional tensors. The forward has 15 + 8
// (N,C_in,C_out,H,W,numGroups,eps) = effectively required > 15. Use options
// object for both.
//
// resblockForward(opts):
//   required: X, gamma1, beta1, W1, gamma2, beta2, W2, Y, N, C_in, C_out, H, W
//   optional: b1, t_emb_shift, b2, Wskip, bskip, numGroups (default 32), eps (default 1e-5)

static JSValue js_resblockForward(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 1 || !JS_IsObject(argv[0])) return JS_ThrowTypeError(ctx,
        "resblockForward(opts) — see docs");
    ENSURE_INIT();
    JSValueConst o = argv[0];
    JSValue err = JS_UNDEFINED;
    bool ok = true;
    const nngpu::Tensor* X    = getTensorProp(ctx, o, "X",    ok, err); if (!ok) return err;
    const nngpu::Tensor* g1   = getTensorProp(ctx, o, "gamma1", ok, err); if (!ok) return err;
    const nngpu::Tensor* b1g  = getTensorProp(ctx, o, "beta1",  ok, err); if (!ok) return err;
    const nngpu::Tensor* W1   = getTensorProp(ctx, o, "W1",   ok, err); if (!ok) return err;
    const nngpu::Tensor* b1   = getTensorProp(ctx, o, "b1",   ok, err); if (!ok) return err;
    const nngpu::Tensor* tem  = getTensorProp(ctx, o, "t_emb_shift", ok, err); if (!ok) return err;
    const nngpu::Tensor* g2   = getTensorProp(ctx, o, "gamma2", ok, err); if (!ok) return err;
    const nngpu::Tensor* b2g  = getTensorProp(ctx, o, "beta2",  ok, err); if (!ok) return err;
    const nngpu::Tensor* W2   = getTensorProp(ctx, o, "W2",   ok, err); if (!ok) return err;
    const nngpu::Tensor* b2   = getTensorProp(ctx, o, "b2",   ok, err); if (!ok) return err;
    const nngpu::Tensor* Wsk  = getTensorProp(ctx, o, "Wskip",ok, err); if (!ok) return err;
    const nngpu::Tensor* bsk  = getTensorProp(ctx, o, "bskip",ok, err); if (!ok) return err;
    nngpu::Tensor* Y          = getTensorPropMut(ctx, o, "Y", ok, err); if (!ok) return err;
    if (!X || !g1 || !b1g || !W1 || !g2 || !b2g || !W2 || !Y) {
        return JS_ThrowTypeError(ctx,
            "resblockForward: required X,gamma1,beta1,W1,gamma2,beta2,W2,Y missing");
    }
    int32_t N    = getIntProp(ctx, o, "N",    0);
    int32_t Cin  = getIntProp(ctx, o, "C_in", 0);
    int32_t Cout = getIntProp(ctx, o, "C_out",0);
    int32_t H    = getIntProp(ctx, o, "H",    0);
    int32_t W    = getIntProp(ctx, o, "W",    0);
    int32_t ng   = getIntProp(ctx, o, "numGroups", 32);
    JSValue epsV = JS_GetPropertyStr(ctx, o, "eps");
    double eps = 1e-5; if (JS_IsNumber(epsV)) JS_ToFloat64(ctx, &eps, epsV);
    JS_FreeValue(ctx, epsV);
    nngpu::resblock_forward(*X, *g1, *b1g, *W1, b1, tem,
                                *g2, *b2g, *W2, b2, Wsk, bsk,
                                N, Cin, Cout, H, W, ng, (float)eps, *Y);
    return JS_UNDEFINED;
}

static JSValue js_resblockBackward(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 1 || !JS_IsObject(argv[0])) return JS_ThrowTypeError(ctx,
        "resblockBackward(opts) — see docs");
    ENSURE_INIT();
    JSValueConst o = argv[0];
    JSValue err = JS_UNDEFINED;
    bool ok = true;
    const nngpu::Tensor* X    = getTensorProp(ctx, o, "X",    ok, err); if (!ok) return err;
    const nngpu::Tensor* g1   = getTensorProp(ctx, o, "gamma1", ok, err); if (!ok) return err;
    const nngpu::Tensor* b1g  = getTensorProp(ctx, o, "beta1",  ok, err); if (!ok) return err;
    const nngpu::Tensor* W1   = getTensorProp(ctx, o, "W1",   ok, err); if (!ok) return err;
    const nngpu::Tensor* b1   = getTensorProp(ctx, o, "b1",   ok, err); if (!ok) return err;
    const nngpu::Tensor* tem  = getTensorProp(ctx, o, "t_emb_shift", ok, err); if (!ok) return err;
    const nngpu::Tensor* g2   = getTensorProp(ctx, o, "gamma2", ok, err); if (!ok) return err;
    const nngpu::Tensor* b2g  = getTensorProp(ctx, o, "beta2",  ok, err); if (!ok) return err;
    const nngpu::Tensor* W2   = getTensorProp(ctx, o, "W2",   ok, err); if (!ok) return err;
    const nngpu::Tensor* b2   = getTensorProp(ctx, o, "b2",   ok, err); if (!ok) return err;
    const nngpu::Tensor* Wsk  = getTensorProp(ctx, o, "Wskip",ok, err); if (!ok) return err;
    const nngpu::Tensor* bsk  = getTensorProp(ctx, o, "bskip",ok, err); if (!ok) return err;
    const nngpu::Tensor* dY   = getTensorProp(ctx, o, "dY",   ok, err); if (!ok) return err;
    nngpu::Tensor* dX         = getTensorPropMut(ctx, o, "dX", ok, err); if (!ok) return err;
    nngpu::Tensor* dG1        = getTensorPropMut(ctx, o, "dGamma1", ok, err); if (!ok) return err;
    nngpu::Tensor* dB1        = getTensorPropMut(ctx, o, "dBeta1",  ok, err); if (!ok) return err;
    nngpu::Tensor* dW1        = getTensorPropMut(ctx, o, "dW1", ok, err); if (!ok) return err;
    nngpu::Tensor* db1        = getTensorPropMut(ctx, o, "db1", ok, err); if (!ok) return err;
    nngpu::Tensor* dtem       = getTensorPropMut(ctx, o, "dt_emb_shift", ok, err); if (!ok) return err;
    nngpu::Tensor* dG2        = getTensorPropMut(ctx, o, "dGamma2", ok, err); if (!ok) return err;
    nngpu::Tensor* dB2        = getTensorPropMut(ctx, o, "dBeta2",  ok, err); if (!ok) return err;
    nngpu::Tensor* dW2        = getTensorPropMut(ctx, o, "dW2", ok, err); if (!ok) return err;
    nngpu::Tensor* db2        = getTensorPropMut(ctx, o, "db2", ok, err); if (!ok) return err;
    nngpu::Tensor* dWsk       = getTensorPropMut(ctx, o, "dWskip", ok, err); if (!ok) return err;
    nngpu::Tensor* dbsk       = getTensorPropMut(ctx, o, "dbskip", ok, err); if (!ok) return err;
    if (!X || !g1 || !b1g || !W1 || !g2 || !b2g || !W2 || !dY
        || !dX || !dG1 || !dB1 || !dW1 || !dG2 || !dB2 || !dW2) {
        return JS_ThrowTypeError(ctx,
            "resblockBackward: required tensors missing");
    }
    int32_t N    = getIntProp(ctx, o, "N",    0);
    int32_t Cin  = getIntProp(ctx, o, "C_in", 0);
    int32_t Cout = getIntProp(ctx, o, "C_out",0);
    int32_t H    = getIntProp(ctx, o, "H",    0);
    int32_t W    = getIntProp(ctx, o, "W",    0);
    int32_t ng   = getIntProp(ctx, o, "numGroups", 32);
    JSValue epsV = JS_GetPropertyStr(ctx, o, "eps");
    double eps = 1e-5; if (JS_IsNumber(epsV)) JS_ToFloat64(ctx, &eps, epsV);
    JS_FreeValue(ctx, epsV);
    nngpu::resblock_backward(*X, *g1, *b1g, *W1, b1, tem,
                                 *g2, *b2g, *W2, b2, Wsk, bsk,
                                 N, Cin, Cout, H, W, ng, (float)eps,
                                 *dY, *dX, *dG1, *dB1, *dW1, db1, dtem,
                                 *dG2, *dB2, *dW2, db2, dWsk, dbsk);
    return JS_UNDEFINED;
}

#undef GT
#undef ENSURE_INIT

void installTensorAttentionOps(JSContext* ctx, JSValue gpuObj) {
    // Self-attention
    JS_SetPropertyStr(ctx, gpuObj, "selfAttentionForward",      JS_NewCFunction(ctx, js_selfAttentionForward,      "selfAttentionForward",       8));
    JS_SetPropertyStr(ctx, gpuObj, "selfAttentionForwardTrain", JS_NewCFunction(ctx, js_selfAttentionForwardTrain, "selfAttentionForwardTrain", 13));
    JS_SetPropertyStr(ctx, gpuObj, "selfAttentionBackward",     JS_NewCFunction(ctx, js_selfAttentionBackward,     "selfAttentionBackward",     18));

    // Cross-attention
    JS_SetPropertyStr(ctx, gpuObj, "crossAttentionForward",         JS_NewCFunction(ctx, js_crossAttentionForward,         "crossAttentionForward",          9));
    JS_SetPropertyStr(ctx, gpuObj, "crossAttentionForwardWithAttn", JS_NewCFunction(ctx, js_crossAttentionForwardWithAttn, "crossAttentionForwardWithAttn", 11));
    JS_SetPropertyStr(ctx, gpuObj, "crossAttentionForwardTrain",    JS_NewCFunction(ctx, js_crossAttentionForwardTrain,    "crossAttentionForwardTrain",    14));
    JS_SetPropertyStr(ctx, gpuObj, "crossAttentionBackward",        JS_NewCFunction(ctx, js_crossAttentionBackward,        "crossAttentionBackward",        20));

    JS_SetPropertyStr(ctx, gpuObj, "attentionTokenMoments",         JS_NewCFunction(ctx, js_attentionTokenMoments,         "attentionTokenMoments",          5));

    // Flash attention
    JS_SetPropertyStr(ctx, gpuObj, "flashAttentionForward",            JS_NewCFunction(ctx, js_flashAttentionForward,            "flashAttentionForward",             7));
    JS_SetPropertyStr(ctx, gpuObj, "flashAttentionBackward",           JS_NewCFunction(ctx, js_flashAttentionBackward,           "flashAttentionBackward",           11));
    JS_SetPropertyStr(ctx, gpuObj, "flashAttentionQkvoForward",        JS_NewCFunction(ctx, js_flashAttentionQkvoForward,        "flashAttentionQkvoForward",        14));
    JS_SetPropertyStr(ctx, gpuObj, "flashAttentionQkvoBackward",       JS_NewCFunction(ctx, js_flashAttentionQkvoBackward,       "flashAttentionQkvoBackward",        1));
    JS_SetPropertyStr(ctx, gpuObj, "flashAttentionProjectKv",          JS_NewCFunction(ctx, js_flashAttentionProjectKv,          "flashAttentionProjectKv",           7));
    JS_SetPropertyStr(ctx, gpuObj, "flashAttentionQWithKvCachedForward", JS_NewCFunction(ctx, js_flashAttentionQWithKvCachedForward, "flashAttentionQWithKvCachedForward", 11));
    JS_SetPropertyStr(ctx, gpuObj, "flashAttentionDecode",             JS_NewCFunction(ctx, js_flashAttentionDecode,             "flashAttentionDecode",              6));

    // KV cache
    JS_SetPropertyStr(ctx, gpuObj, "kvCacheAppend",                    JS_NewCFunction(ctx, js_kvCacheAppend,                    "kvCacheAppend",                     5));

    // Resblock
    JS_SetPropertyStr(ctx, gpuObj, "resblockForward",                  JS_NewCFunction(ctx, js_resblockForward,                  "resblockForward",                   1));
    JS_SetPropertyStr(ctx, gpuObj, "resblockBackward",                 JS_NewCFunction(ctx, js_resblockBackward,                 "resblockBackward",                  1));
}

} // namespace bro::js

#else // !BROTENSOR_HAS_GPU

#include <qjsbind/qjsbind.h>
namespace bro::js {
void installTensorAttentionOps(JSContext*, JSValue) {}
} // namespace bro::js

#endif // BROTENSOR_HAS_GPU
