// JS bindings — activations, norms, matmul, rope. See tensor_bindings.cpp
// for the architectural overview.

#ifdef BROTENSOR_HAS_GPU

#include "js/tensor_bindings_internal.h"

namespace bro::js {

#define ENSURE_INIT() BROTENSOR_ENSURE_INIT()
#define GT(name, idx, label) BROTENSOR_GT(name, idx, label)

// ─── Unary activations: forward (x, y) ────────────────────────────────────
//
// Generates: js_<name>Forward + js_<name>Backward for the common
// (x,y) / (x,dY,dX) shape used by silu/gelu/gelu_exact/quick_gelu. The
// backward reads the raw forward input x (NOT the cached forward output y).
#define UNARY_FB(jsName, fwdFn, bwdFn)                                          \
    static JSValue js_##jsName##Forward(JSContext* ctx, JSValueConst, int argc, \
                                        JSValueConst* argv) {                   \
        if (argc < 2) return JS_ThrowTypeError(ctx, #jsName "Forward(x,y)");    \
        ENSURE_INIT();                                                          \
        GT(x, 0, #jsName "Forward"); GT(y, 1, #jsName "Forward");               \
        nngpu::fwdFn(*x, *y);                                                   \
        return JS_UNDEFINED;                                                    \
    }                                                                           \
    static JSValue js_##jsName##Backward(JSContext* ctx, JSValueConst, int argc,\
                                         JSValueConst* argv) {                  \
        if (argc < 3) return JS_ThrowTypeError(ctx, #jsName "Backward(x,dY,dX)");\
        ENSURE_INIT();                                                          \
        GT(x, 0, #jsName "Backward"); GT(dY, 1, #jsName "Backward");            \
        GT(dX, 2, #jsName "Backward");                                          \
        nngpu::bwdFn(*x, *dY, *dX);                                             \
        return JS_UNDEFINED;                                                    \
    }

UNARY_FB(silu,      silu_forward,        silu_backward)
UNARY_FB(gelu,      gelu_forward,        gelu_backward)
UNARY_FB(geluExact, gelu_exact_forward,  gelu_exact_backward)
UNARY_FB(quickGelu, quick_gelu_forward,  quick_gelu_backward)

#undef UNARY_FB

// ─── Gated activations: forward (X, Y), backward (X, dY, dX) ──────────────
//
// swiglu / geglu / geglu_exact split the last dim of X internally and write
// dX back as a concat of the two halves' gradients.
#define GATED_FB(jsName, fwdFn, bwdFn)                                          \
    static JSValue js_##jsName##Forward(JSContext* ctx, JSValueConst, int argc, \
                                        JSValueConst* argv) {                   \
        if (argc < 2) return JS_ThrowTypeError(ctx, #jsName "Forward(X,Y)");    \
        ENSURE_INIT();                                                          \
        GT(X, 0, #jsName "Forward"); GT(Y, 1, #jsName "Forward");               \
        nngpu::fwdFn(*X, *Y);                                                   \
        return JS_UNDEFINED;                                                    \
    }                                                                           \
    static JSValue js_##jsName##Backward(JSContext* ctx, JSValueConst, int argc,\
                                         JSValueConst* argv) {                  \
        if (argc < 3) return JS_ThrowTypeError(ctx, #jsName "Backward(X,dY,dX)");\
        ENSURE_INIT();                                                          \
        GT(X, 0, #jsName "Backward"); GT(dY, 1, #jsName "Backward");            \
        GT(dX, 2, #jsName "Backward");                                          \
        nngpu::bwdFn(*X, *dY, *dX);                                             \
        return JS_UNDEFINED;                                                    \
    }

GATED_FB(swiglu,     swiglu_forward,      swiglu_backward)
GATED_FB(geglu,      geglu_forward,       geglu_backward)
GATED_FB(gegluExact, geglu_exact_forward, geglu_exact_backward)

#undef GATED_FB

// ─── RMSNorm ──────────────────────────────────────────────────────────────

static JSValue js_rmsNormForward(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 4) return JS_ThrowTypeError(ctx, "rmsNormForward(X,gamma,eps,Y)");
    ENSURE_INIT();
    GT(X, 0, "rmsNormForward"); GT(g, 1, "rmsNormForward");
    double eps = 1e-5; JS_ToFloat64(ctx, &eps, argv[2]);
    GT(Y, 3, "rmsNormForward");
    nngpu::rms_norm_forward(*X, *g, (float)eps, *Y);
    return JS_UNDEFINED;
}

static JSValue js_rmsNormBackward(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 6) return JS_ThrowTypeError(ctx,
        "rmsNormBackward(X,gamma,dY,eps,dX,dGamma)");
    ENSURE_INIT();
    GT(X, 0, "rmsNormBackward"); GT(g, 1, "rmsNormBackward");
    GT(dY, 2, "rmsNormBackward");
    double eps = 1e-5; JS_ToFloat64(ctx, &eps, argv[3]);
    GT(dX, 4, "rmsNormBackward"); GT(dG, 5, "rmsNormBackward");
    nngpu::rms_norm_backward(*X, *g, *dY, (float)eps, *dX, *dG);
    return JS_UNDEFINED;
}

// ─── GroupNorm ────────────────────────────────────────────────────────────

static JSValue js_groupNormForward(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 9) return JS_ThrowTypeError(ctx,
        "groupNormForward(X,gamma,beta,N,C,H,W,numGroups,eps,Y) — pass eps then Y, 10 args total");
    // Note: the spec is (X, gamma, beta, N, C, H, W, num_groups, eps, Y).
    // Allow 10-arg form; the early throw above tolerates a min of 9 for
    // permissive callers passing eps via numGroups slot. Tighten:
    if (argc < 10) return JS_ThrowTypeError(ctx,
        "groupNormForward(X,gamma,beta,N,C,H,W,numGroups,eps,Y)");
    ENSURE_INIT();
    GT(X, 0, "groupNormForward"); GT(g, 1, "groupNormForward");
    GT(b, 2, "groupNormForward");
    int32_t N = 0, C = 0, H = 0, W = 0, ng = 32;
    JS_ToInt32(ctx, &N,  argv[3]);
    JS_ToInt32(ctx, &C,  argv[4]);
    JS_ToInt32(ctx, &H,  argv[5]);
    JS_ToInt32(ctx, &W,  argv[6]);
    JS_ToInt32(ctx, &ng, argv[7]);
    double eps = 1e-5; JS_ToFloat64(ctx, &eps, argv[8]);
    GT(Y, 9, "groupNormForward");
    nngpu::group_norm_forward(*X, *g, *b, N, C, H, W, ng, (float)eps, *Y);
    return JS_UNDEFINED;
}

static JSValue js_groupNormBackward(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 12) return JS_ThrowTypeError(ctx,
        "groupNormBackward(X,gamma,dY,N,C,H,W,numGroups,eps,dX,dGamma,dBeta)");
    ENSURE_INIT();
    GT(X,  0, "groupNormBackward"); GT(g,  1, "groupNormBackward");
    GT(dY, 2, "groupNormBackward");
    int32_t N = 0, C = 0, H = 0, W = 0, ng = 32;
    JS_ToInt32(ctx, &N,  argv[3]);
    JS_ToInt32(ctx, &C,  argv[4]);
    JS_ToInt32(ctx, &H,  argv[5]);
    JS_ToInt32(ctx, &W,  argv[6]);
    JS_ToInt32(ctx, &ng, argv[7]);
    double eps = 1e-5; JS_ToFloat64(ctx, &eps, argv[8]);
    GT(dX, 9, "groupNormBackward"); GT(dG, 10, "groupNormBackward");
    GT(dB, 11, "groupNormBackward");
    nngpu::group_norm_backward(*X, *g, *dY, N, C, H, W, ng, (float)eps,
                                   *dX, *dG, *dB);
    return JS_UNDEFINED;
}

// ─── Matmul ───────────────────────────────────────────────────────────────

static JSValue js_matmul(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 3) return JS_ThrowTypeError(ctx, "matmul(A,B,C)");
    ENSURE_INIT();
    GT(A, 0, "matmul"); GT(B, 1, "matmul"); GT(C, 2, "matmul");
    nngpu::matmul(*A, *B, *C);
    return JS_UNDEFINED;
}

static JSValue js_matmulBackward(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 5) return JS_ThrowTypeError(ctx, "matmulBackward(A,B,dC,dA,dB)");
    ENSURE_INIT();
    GT(A,  0, "matmulBackward"); GT(B,  1, "matmulBackward");
    GT(dC, 2, "matmulBackward"); GT(dA, 3, "matmulBackward");
    GT(dB, 4, "matmulBackward");
    nngpu::matmul_backward(*A, *B, *dC, *dA, *dB);
    return JS_UNDEFINED;
}

// ─── RoPE ─────────────────────────────────────────────────────────────────

static JSValue js_ropeForward(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 6) return JS_ThrowTypeError(ctx,
        "ropeForward(X,headDim,numHeads,seqOffset,thetaBase,Y)");
    ENSURE_INIT();
    GT(X, 0, "ropeForward");
    int32_t headDim = 0, numHeads = 0, seqOffset = 0;
    JS_ToInt32(ctx, &headDim,   argv[1]);
    JS_ToInt32(ctx, &numHeads,  argv[2]);
    JS_ToInt32(ctx, &seqOffset, argv[3]);
    double thetaBase = 10000.0; JS_ToFloat64(ctx, &thetaBase, argv[4]);
    GT(Y, 5, "ropeForward");
    nngpu::rope_forward(*X, headDim, numHeads, seqOffset, (float)thetaBase, *Y);
    return JS_UNDEFINED;
}

static JSValue js_ropeBackward(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 6) return JS_ThrowTypeError(ctx,
        "ropeBackward(dY,headDim,numHeads,seqOffset,thetaBase,dX)");
    ENSURE_INIT();
    GT(dY, 0, "ropeBackward");
    int32_t headDim = 0, numHeads = 0, seqOffset = 0;
    JS_ToInt32(ctx, &headDim,   argv[1]);
    JS_ToInt32(ctx, &numHeads,  argv[2]);
    JS_ToInt32(ctx, &seqOffset, argv[3]);
    double thetaBase = 10000.0; JS_ToFloat64(ctx, &thetaBase, argv[4]);
    GT(dX, 5, "ropeBackward");
    nngpu::rope_backward(*dY, headDim, numHeads, seqOffset, (float)thetaBase, *dX);
    return JS_UNDEFINED;
}

// ─── Causal mask helper ───────────────────────────────────────────────────

static JSValue js_buildCausalMaskRow(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 3) return JS_ThrowTypeError(ctx, "buildCausalMaskRow(L,q,mask)");
    ENSURE_INIT();
    int32_t L = 0, q = 0;
    JS_ToInt32(ctx, &L, argv[0]);
    JS_ToInt32(ctx, &q, argv[1]);
    GT(mask, 2, "buildCausalMaskRow");
    nngpu::build_causal_mask_row(L, q, *mask);
    return JS_UNDEFINED;
}

// ─── AdaLN modulation (DiT / SD3 / Flux) ──────────────────────────────────
//
// modulate: Y = X*(1+scale) + shift, scale/shift broadcast across token rows
// — the affine step every DiT block applies after norm(). broadcastMul is the
// DiT residual gate: Y[l,d] = X[l,d]*v[d]. Both dispatch on X.dtype.

static JSValue js_modulate(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 4) return JS_ThrowTypeError(ctx, "modulate(X,scale,shift,Y)");
    ENSURE_INIT();
    GT(X, 0, "modulate"); GT(scale, 1, "modulate");
    GT(shift, 2, "modulate"); GT(Y, 3, "modulate");
    nngpu::modulate(*X, *scale, *shift, *Y);
    return JS_UNDEFINED;
}

static JSValue js_broadcastMul(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 3) return JS_ThrowTypeError(ctx, "broadcastMul(X,v,Y)");
    ENSURE_INIT();
    GT(X, 0, "broadcastMul"); GT(v, 1, "broadcastMul"); GT(Y, 2, "broadcastMul");
    nngpu::broadcast_mul(*X, *v, *Y);
    return JS_UNDEFINED;
}

// ─── RoPE with precomputed cos/sin tables ─────────────────────────────────
//
// Unlike ropeForward (which derives θ from seqOffset + thetaBase), ropeApply
// takes caller-supplied cos/sin tables — one (row, headDim/2) entry per token.
// Dispatched on X.dtype.

static JSValue js_ropeApply(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 6) return JS_ThrowTypeError(ctx,
        "ropeApply(X,cosTbl,sinTbl,headDim,numHeads,Y)");
    ENSURE_INIT();
    GT(X, 0, "ropeApply"); GT(cosTbl, 1, "ropeApply"); GT(sinTbl, 2, "ropeApply");
    int32_t headDim = 0, numHeads = 0;
    JS_ToInt32(ctx, &headDim,  argv[3]);
    JS_ToInt32(ctx, &numHeads, argv[4]);
    GT(Y, 5, "ropeApply");
    nngpu::rope_apply(*X, *cosTbl, *sinTbl, headDim, numHeads, *Y);
    return JS_UNDEFINED;
}

static JSValue js_ropeApplyBackward(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 6) return JS_ThrowTypeError(ctx,
        "ropeApplyBackward(dY,cosTbl,sinTbl,headDim,numHeads,dX)");
    ENSURE_INIT();
    GT(dY, 0, "ropeApplyBackward"); GT(cosTbl, 1, "ropeApplyBackward");
    GT(sinTbl, 2, "ropeApplyBackward");
    int32_t headDim = 0, numHeads = 0;
    JS_ToInt32(ctx, &headDim,  argv[3]);
    JS_ToInt32(ctx, &numHeads, argv[4]);
    GT(dX, 5, "ropeApplyBackward");
    nngpu::rope_apply_backward(*dY, *cosTbl, *sinTbl, headDim, numHeads, *dX);
    return JS_UNDEFINED;
}

#undef GT
#undef ENSURE_INIT

void installTensorActivationOps(JSContext* ctx, JSValue gpuObj) {
    JS_SetPropertyStr(ctx, gpuObj, "siluForward",      JS_NewCFunction(ctx, js_siluForward,      "siluForward",      2));
    JS_SetPropertyStr(ctx, gpuObj, "siluBackward",     JS_NewCFunction(ctx, js_siluBackward,     "siluBackward",     3));
    JS_SetPropertyStr(ctx, gpuObj, "geluForward",      JS_NewCFunction(ctx, js_geluForward,      "geluForward",      2));
    JS_SetPropertyStr(ctx, gpuObj, "geluBackward",     JS_NewCFunction(ctx, js_geluBackward,     "geluBackward",     3));
    JS_SetPropertyStr(ctx, gpuObj, "geluExactForward", JS_NewCFunction(ctx, js_geluExactForward, "geluExactForward", 2));
    JS_SetPropertyStr(ctx, gpuObj, "geluExactBackward",JS_NewCFunction(ctx, js_geluExactBackward,"geluExactBackward",3));
    JS_SetPropertyStr(ctx, gpuObj, "quickGeluForward", JS_NewCFunction(ctx, js_quickGeluForward, "quickGeluForward", 2));
    JS_SetPropertyStr(ctx, gpuObj, "quickGeluBackward",JS_NewCFunction(ctx, js_quickGeluBackward,"quickGeluBackward",3));

    JS_SetPropertyStr(ctx, gpuObj, "swigluForward",      JS_NewCFunction(ctx, js_swigluForward,      "swigluForward",      2));
    JS_SetPropertyStr(ctx, gpuObj, "swigluBackward",     JS_NewCFunction(ctx, js_swigluBackward,     "swigluBackward",     3));
    JS_SetPropertyStr(ctx, gpuObj, "gegluForward",       JS_NewCFunction(ctx, js_gegluForward,       "gegluForward",       2));
    JS_SetPropertyStr(ctx, gpuObj, "gegluBackward",      JS_NewCFunction(ctx, js_gegluBackward,      "gegluBackward",      3));
    JS_SetPropertyStr(ctx, gpuObj, "gegluExactForward",  JS_NewCFunction(ctx, js_gegluExactForward,  "gegluExactForward",  2));
    JS_SetPropertyStr(ctx, gpuObj, "gegluExactBackward", JS_NewCFunction(ctx, js_gegluExactBackward, "gegluExactBackward", 3));

    JS_SetPropertyStr(ctx, gpuObj, "rmsNormForward",    JS_NewCFunction(ctx, js_rmsNormForward,    "rmsNormForward",    4));
    JS_SetPropertyStr(ctx, gpuObj, "rmsNormBackward",   JS_NewCFunction(ctx, js_rmsNormBackward,   "rmsNormBackward",   6));

    JS_SetPropertyStr(ctx, gpuObj, "groupNormForward",  JS_NewCFunction(ctx, js_groupNormForward,  "groupNormForward",  10));
    JS_SetPropertyStr(ctx, gpuObj, "groupNormBackward", JS_NewCFunction(ctx, js_groupNormBackward, "groupNormBackward", 12));

    JS_SetPropertyStr(ctx, gpuObj, "matmul",            JS_NewCFunction(ctx, js_matmul,            "matmul",            3));
    JS_SetPropertyStr(ctx, gpuObj, "matmulBackward",    JS_NewCFunction(ctx, js_matmulBackward,    "matmulBackward",    5));

    JS_SetPropertyStr(ctx, gpuObj, "ropeForward",       JS_NewCFunction(ctx, js_ropeForward,       "ropeForward",       6));
    JS_SetPropertyStr(ctx, gpuObj, "ropeBackward",      JS_NewCFunction(ctx, js_ropeBackward,      "ropeBackward",      6));

    JS_SetPropertyStr(ctx, gpuObj, "buildCausalMaskRow",JS_NewCFunction(ctx, js_buildCausalMaskRow,"buildCausalMaskRow",3));

    JS_SetPropertyStr(ctx, gpuObj, "modulate",          JS_NewCFunction(ctx, js_modulate,          "modulate",          4));
    JS_SetPropertyStr(ctx, gpuObj, "broadcastMul",      JS_NewCFunction(ctx, js_broadcastMul,      "broadcastMul",      3));
    JS_SetPropertyStr(ctx, gpuObj, "ropeApply",         JS_NewCFunction(ctx, js_ropeApply,         "ropeApply",         6));
    JS_SetPropertyStr(ctx, gpuObj, "ropeApplyBackward", JS_NewCFunction(ctx, js_ropeApplyBackward, "ropeApplyBackward", 6));
}

} // namespace bro::js

#else // !BROTENSOR_HAS_GPU

#include <qjsbind/qjsbind.h>
namespace bro::js {
void installTensorActivationOps(JSContext*, JSValue) {}
} // namespace bro::js

#endif // BROTENSOR_HAS_GPU
