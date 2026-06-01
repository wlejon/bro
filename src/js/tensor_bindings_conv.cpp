// JS bindings — conv2d (forward + per-input/weight/bias backward), 2x
// up/downsample (forward + backward), and NCHW↔sequence transpose. See
// tensor_bindings.cpp for the architectural overview.

#ifdef BROTENSOR_HAS_GPU

#include "js/tensor_bindings_internal.h"

namespace bro::js {

#define ENSURE_INIT() BROTENSOR_ENSURE_INIT()
#define GT(name, idx, label) BROTENSOR_GT(name, idx, label)

// ─── Conv2D forward / backward ────────────────────────────────────────────
//
// conv2dForward(X, Wt, bias|null, N, C_in, H, W, C_out, kH, kW,
//               strideH, strideW, padH, padW, dilH, dilW, groups, Y)
//
// 18 positional args. The 18-arg version exposes the explicit `groups`
// parameter. groups defaults to 1; pass 1 explicitly when in doubt.
static JSValue js_conv2dForward(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 18) return JS_ThrowTypeError(ctx,
        "conv2dForward(X,Wt,bias|null,N,C_in,H,W,C_out,kH,kW,sH,sW,pH,pW,dH,dW,groups,Y)");
    ENSURE_INIT();
    GT(X,  0, "conv2dForward"); GT(Wt, 1, "conv2dForward");
    JSValue err = JS_UNDEFINED;
    const nngpu::Tensor* bias = nullptr;
    if (!resolveOptionalConstGpuTensor(ctx, argv[2], bias, err, "bias")) return err;
    int32_t N = 0, Cin = 0, H = 0, W = 0, Cout = 0, kH = 0, kW = 0;
    int32_t sH = 1, sW = 1, pH = 0, pW = 0, dH = 1, dW = 1, groups = 1;
    JS_ToInt32(ctx, &N,    argv[3]);
    JS_ToInt32(ctx, &Cin,  argv[4]);
    JS_ToInt32(ctx, &H,    argv[5]);
    JS_ToInt32(ctx, &W,    argv[6]);
    JS_ToInt32(ctx, &Cout, argv[7]);
    JS_ToInt32(ctx, &kH,   argv[8]);
    JS_ToInt32(ctx, &kW,   argv[9]);
    JS_ToInt32(ctx, &sH,   argv[10]);
    JS_ToInt32(ctx, &sW,   argv[11]);
    JS_ToInt32(ctx, &pH,   argv[12]);
    JS_ToInt32(ctx, &pW,   argv[13]);
    JS_ToInt32(ctx, &dH,   argv[14]);
    JS_ToInt32(ctx, &dW,   argv[15]);
    JS_ToInt32(ctx, &groups,argv[16]);
    GT(Y, 17, "conv2dForward");
    nngpu::conv2d_forward(*X, *Wt, bias, N, Cin, H, W, Cout, kH, kW,
                              sH, sW, pH, pW, dH, dW, groups, *Y);
    return JS_UNDEFINED;
}

static JSValue js_conv2dBackwardInput(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 17) return JS_ThrowTypeError(ctx,
        "conv2dBackwardInput(Wt,dY,N,C_in,H,W,C_out,kH,kW,sH,sW,pH,pW,dH,dW,groups,dX)");
    ENSURE_INIT();
    GT(Wt, 0, "conv2dBackwardInput"); GT(dY, 1, "conv2dBackwardInput");
    int32_t N = 0, Cin = 0, H = 0, W = 0, Cout = 0, kH = 0, kW = 0;
    int32_t sH = 1, sW = 1, pH = 0, pW = 0, dH = 1, dW = 1, groups = 1;
    JS_ToInt32(ctx, &N,    argv[2]);
    JS_ToInt32(ctx, &Cin,  argv[3]);
    JS_ToInt32(ctx, &H,    argv[4]);
    JS_ToInt32(ctx, &W,    argv[5]);
    JS_ToInt32(ctx, &Cout, argv[6]);
    JS_ToInt32(ctx, &kH,   argv[7]);
    JS_ToInt32(ctx, &kW,   argv[8]);
    JS_ToInt32(ctx, &sH,   argv[9]);
    JS_ToInt32(ctx, &sW,   argv[10]);
    JS_ToInt32(ctx, &pH,   argv[11]);
    JS_ToInt32(ctx, &pW,   argv[12]);
    JS_ToInt32(ctx, &dH,   argv[13]);
    JS_ToInt32(ctx, &dW,   argv[14]);
    JS_ToInt32(ctx, &groups,argv[15]);
    GT(dX, 16, "conv2dBackwardInput");
    nngpu::conv2d_backward_input(*Wt, *dY, N, Cin, H, W, Cout, kH, kW,
                                     sH, sW, pH, pW, dH, dW, groups, *dX);
    return JS_UNDEFINED;
}

static JSValue js_conv2dBackwardWeight(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 17) return JS_ThrowTypeError(ctx,
        "conv2dBackwardWeight(X,dY,N,C_in,H,W,C_out,kH,kW,sH,sW,pH,pW,dH,dW,groups,dWt)");
    ENSURE_INIT();
    GT(X, 0, "conv2dBackwardWeight"); GT(dY, 1, "conv2dBackwardWeight");
    int32_t N = 0, Cin = 0, H = 0, W = 0, Cout = 0, kH = 0, kW = 0;
    int32_t sH = 1, sW = 1, pH = 0, pW = 0, dH = 1, dW = 1, groups = 1;
    JS_ToInt32(ctx, &N,    argv[2]);
    JS_ToInt32(ctx, &Cin,  argv[3]);
    JS_ToInt32(ctx, &H,    argv[4]);
    JS_ToInt32(ctx, &W,    argv[5]);
    JS_ToInt32(ctx, &Cout, argv[6]);
    JS_ToInt32(ctx, &kH,   argv[7]);
    JS_ToInt32(ctx, &kW,   argv[8]);
    JS_ToInt32(ctx, &sH,   argv[9]);
    JS_ToInt32(ctx, &sW,   argv[10]);
    JS_ToInt32(ctx, &pH,   argv[11]);
    JS_ToInt32(ctx, &pW,   argv[12]);
    JS_ToInt32(ctx, &dH,   argv[13]);
    JS_ToInt32(ctx, &dW,   argv[14]);
    JS_ToInt32(ctx, &groups,argv[15]);
    GT(dWt, 16, "conv2dBackwardWeight");
    nngpu::conv2d_backward_weight(*X, *dY, N, Cin, H, W, Cout, kH, kW,
                                      sH, sW, pH, pW, dH, dW, groups, *dWt);
    return JS_UNDEFINED;
}

static JSValue js_conv2dBackwardBias(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 6) return JS_ThrowTypeError(ctx,
        "conv2dBackwardBias(dY,N,C_out,H_out,W_out,dB)");
    ENSURE_INIT();
    GT(dY, 0, "conv2dBackwardBias");
    int32_t N = 0, Cout = 0, Hout = 0, Wout = 0;
    JS_ToInt32(ctx, &N,    argv[1]);
    JS_ToInt32(ctx, &Cout, argv[2]);
    JS_ToInt32(ctx, &Hout, argv[3]);
    JS_ToInt32(ctx, &Wout, argv[4]);
    GT(dB, 5, "conv2dBackwardBias");
    nngpu::conv2d_backward_bias(*dY, N, Cout, Hout, Wout, *dB);
    return JS_UNDEFINED;
}

// ─── 2x up/downsample, NCHW ───────────────────────────────────────────────
//
// Macro generates F and B for each of the three families. Forward and
// backward share the same (N, C, H, W) integer convention: H and W are the
// *input* dims (post-upsample dims are 2H/2W for the forward, pre-downsample
// dims for the down family).
#define UPSAMPLE_FB(jsName, fwdFn, bwdFn)                                       \
    static JSValue js_##jsName##Forward(JSContext* ctx, JSValueConst, int argc, \
                                        JSValueConst* argv) {                   \
        if (argc < 6) return JS_ThrowTypeError(ctx,                             \
            #jsName "Forward(X,N,C,H,W,Y)");                                    \
        ENSURE_INIT();                                                          \
        GT(X, 0, #jsName "Forward");                                            \
        int32_t N=0,C=0,H=0,W=0;                                                \
        JS_ToInt32(ctx, &N, argv[1]);                                           \
        JS_ToInt32(ctx, &C, argv[2]);                                           \
        JS_ToInt32(ctx, &H, argv[3]);                                           \
        JS_ToInt32(ctx, &W, argv[4]);                                           \
        GT(Y, 5, #jsName "Forward");                                            \
        nngpu::fwdFn(*X, N, C, H, W, *Y);                                       \
        return JS_UNDEFINED;                                                    \
    }                                                                           \
    static JSValue js_##jsName##Backward(JSContext* ctx, JSValueConst, int argc,\
                                         JSValueConst* argv) {                  \
        if (argc < 6) return JS_ThrowTypeError(ctx,                             \
            #jsName "Backward(dY,N,C,H,W,dX)");                                 \
        ENSURE_INIT();                                                          \
        GT(dY, 0, #jsName "Backward");                                          \
        int32_t N=0,C=0,H=0,W=0;                                                \
        JS_ToInt32(ctx, &N, argv[1]);                                           \
        JS_ToInt32(ctx, &C, argv[2]);                                           \
        JS_ToInt32(ctx, &H, argv[3]);                                           \
        JS_ToInt32(ctx, &W, argv[4]);                                           \
        GT(dX, 5, #jsName "Backward");                                          \
        nngpu::bwdFn(*dY, N, C, H, W, *dX);                                     \
        return JS_UNDEFINED;                                                    \
    }

UPSAMPLE_FB(upsampleNearest2x,  upsample_nearest_2x,  upsample_nearest_2x_backward)
UPSAMPLE_FB(upsampleBilinear2x, upsample_bilinear_2x, upsample_bilinear_2x_backward)
UPSAMPLE_FB(downsampleAvg2x,    downsample_avg_2x,    downsample_avg_2x_backward)

#undef UPSAMPLE_FB

// ─── NCHW ↔ sequence transpose ────────────────────────────────────────────

static JSValue js_nchwToSequence(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 6) return JS_ThrowTypeError(ctx, "nchwToSequence(X,N,C,H,W,Y)");
    ENSURE_INIT();
    GT(X, 0, "nchwToSequence");
    int32_t N=0,C=0,H=0,W=0;
    JS_ToInt32(ctx, &N, argv[1]);
    JS_ToInt32(ctx, &C, argv[2]);
    JS_ToInt32(ctx, &H, argv[3]);
    JS_ToInt32(ctx, &W, argv[4]);
    GT(Y, 5, "nchwToSequence");
    nngpu::nchw_to_sequence(*X, N, C, H, W, *Y);
    return JS_UNDEFINED;
}

static JSValue js_sequenceToNchw(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 6) return JS_ThrowTypeError(ctx, "sequenceToNchw(X,N,C,H,W,Y)");
    ENSURE_INIT();
    GT(X, 0, "sequenceToNchw");
    int32_t N=0,C=0,H=0,W=0;
    JS_ToInt32(ctx, &N, argv[1]);
    JS_ToInt32(ctx, &C, argv[2]);
    JS_ToInt32(ctx, &H, argv[3]);
    JS_ToInt32(ctx, &W, argv[4]);
    GT(Y, 5, "sequenceToNchw");
    nngpu::sequence_to_nchw(*X, N, C, H, W, *Y);
    return JS_UNDEFINED;
}

// ─── interp2d (bilinear/bicubic resample, NCHW) ───────────────────────────
//
// interp2dForward(X, N, C, H_in, W_in, H_out, W_out, mode, Y)
// interp2dAlignCornersForward(X, N, C, H_in, W_in, H_out, W_out, mode, Y)
//
// mode: 0 nearest, 1 bilinear, 2 bicubic (a=-0.5, PIL), 3 bicubic (a=-0.75,
// torch/OpenCV). The align-corners variant uses the corner-aligned source
// mapping (torch align_corners=True) DPT-style depth/seg heads need; modes
// 0/1/2 only. Both are inference-only (no backward binding).
#define INTERP2D_FWD(jsName, fn)                                                 \
    static JSValue js_##jsName(JSContext* ctx, JSValueConst, int argc,           \
                               JSValueConst* argv) {                             \
        if (argc < 9) return JS_ThrowTypeError(ctx,                              \
            #jsName "(X,N,C,H_in,W_in,H_out,W_out,mode,Y)");                     \
        ENSURE_INIT();                                                           \
        GT(X, 0, #jsName);                                                       \
        int32_t N=0,C=0,Hin=0,Win=0,Hout=0,Wout=0,mode=1;                        \
        JS_ToInt32(ctx, &N,    argv[1]);                                         \
        JS_ToInt32(ctx, &C,    argv[2]);                                         \
        JS_ToInt32(ctx, &Hin,  argv[3]);                                         \
        JS_ToInt32(ctx, &Win,  argv[4]);                                         \
        JS_ToInt32(ctx, &Hout, argv[5]);                                         \
        JS_ToInt32(ctx, &Wout, argv[6]);                                         \
        JS_ToInt32(ctx, &mode, argv[7]);                                         \
        GT(Y, 8, #jsName);                                                       \
        nngpu::fn(*X, N, C, Hin, Win, Hout, Wout, mode, *Y);                     \
        return JS_UNDEFINED;                                                     \
    }

INTERP2D_FWD(interp2dForward,             interp2d_forward)
INTERP2D_FWD(interp2dAlignCornersForward, interp2d_align_corners_forward)

#undef INTERP2D_FWD

// ─── unfold2d (spatial-preserving neighborhood im2col, NCHW) ───────────────
//
// unfold2dForward(X, N, C, H, W, kH, kW, sH, sW, padT, padB, padL, padR, mode, Y)
// mode: 0 zero, 1 reflect, 2 replicate. Inference-only.
static JSValue js_unfold2dForward(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 15) return JS_ThrowTypeError(ctx,
        "unfold2dForward(X,N,C,H,W,kH,kW,sH,sW,padT,padB,padL,padR,mode,Y)");
    ENSURE_INIT();
    GT(X, 0, "unfold2dForward");
    int32_t N=0,C=0,H=0,W=0,kH=0,kW=0,sH=1,sW=1,pt=0,pb=0,pl=0,pr=0,mode=0;
    JS_ToInt32(ctx, &N,    argv[1]);
    JS_ToInt32(ctx, &C,    argv[2]);
    JS_ToInt32(ctx, &H,    argv[3]);
    JS_ToInt32(ctx, &W,    argv[4]);
    JS_ToInt32(ctx, &kH,   argv[5]);
    JS_ToInt32(ctx, &kW,   argv[6]);
    JS_ToInt32(ctx, &sH,   argv[7]);
    JS_ToInt32(ctx, &sW,   argv[8]);
    JS_ToInt32(ctx, &pt,   argv[9]);
    JS_ToInt32(ctx, &pb,   argv[10]);
    JS_ToInt32(ctx, &pl,   argv[11]);
    JS_ToInt32(ctx, &pr,   argv[12]);
    JS_ToInt32(ctx, &mode, argv[13]);
    GT(Y, 14, "unfold2dForward");
    nngpu::unfold2d_forward(*X, N, C, H, W, kH, kW, sH, sW, pt, pb, pl, pr, mode, *Y);
    return JS_UNDEFINED;
}

// ─── l2_normalize_nchw (per-pixel channel-axis unit normalize) ─────────────
//
// l2NormalizeNchwForward(X, N, C, H, W, eps, Y)
static JSValue js_l2NormalizeNchwForward(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 7) return JS_ThrowTypeError(ctx,
        "l2NormalizeNchwForward(X,N,C,H,W,eps,Y)");
    ENSURE_INIT();
    GT(X, 0, "l2NormalizeNchwForward");
    int32_t N=0,C=0,H=0,W=0;
    double eps = 1e-12;
    JS_ToInt32(ctx, &N, argv[1]);
    JS_ToInt32(ctx, &C, argv[2]);
    JS_ToInt32(ctx, &H, argv[3]);
    JS_ToInt32(ctx, &W, argv[4]);
    JS_ToFloat64(ctx, &eps, argv[5]);
    GT(Y, 6, "l2NormalizeNchwForward");
    nngpu::l2_normalize_nchw_forward(*X, N, C, H, W, static_cast<float>(eps), *Y);
    return JS_UNDEFINED;
}

// ─── convex_upsample (RAFT-style learned mask upsample, NCHW) ──────────────
//
// convexUpsampleForward(X, Mask, N, C, H, W, scale, Y)
// Mask: (N, 9*scale*scale*H*W), torch (N,9,k,k,H,W) layout. Inference-only.
static JSValue js_convexUpsampleForward(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 8) return JS_ThrowTypeError(ctx,
        "convexUpsampleForward(X,Mask,N,C,H,W,scale,Y)");
    ENSURE_INIT();
    GT(X, 0, "convexUpsampleForward"); GT(Mask, 1, "convexUpsampleForward");
    int32_t N=0,C=0,H=0,W=0,scale=1;
    JS_ToInt32(ctx, &N,     argv[2]);
    JS_ToInt32(ctx, &C,     argv[3]);
    JS_ToInt32(ctx, &H,     argv[4]);
    JS_ToInt32(ctx, &W,     argv[5]);
    JS_ToInt32(ctx, &scale, argv[6]);
    GT(Y, 7, "convexUpsampleForward");
    nngpu::convex_upsample_forward(*X, *Mask, N, C, H, W, scale, *Y);
    return JS_UNDEFINED;
}

#undef GT
#undef ENSURE_INIT

void installTensorConvOps(JSContext* ctx, JSValue gpuObj) {
    JS_SetPropertyStr(ctx, gpuObj, "conv2dForward",         JS_NewCFunction(ctx, js_conv2dForward,         "conv2dForward",         18));
    JS_SetPropertyStr(ctx, gpuObj, "conv2dBackwardInput",   JS_NewCFunction(ctx, js_conv2dBackwardInput,   "conv2dBackwardInput",   17));
    JS_SetPropertyStr(ctx, gpuObj, "conv2dBackwardWeight",  JS_NewCFunction(ctx, js_conv2dBackwardWeight,  "conv2dBackwardWeight",  17));
    JS_SetPropertyStr(ctx, gpuObj, "conv2dBackwardBias",    JS_NewCFunction(ctx, js_conv2dBackwardBias,    "conv2dBackwardBias",     6));

    JS_SetPropertyStr(ctx, gpuObj, "upsampleNearest2xForward",   JS_NewCFunction(ctx, js_upsampleNearest2xForward,   "upsampleNearest2xForward",   6));
    JS_SetPropertyStr(ctx, gpuObj, "upsampleNearest2xBackward",  JS_NewCFunction(ctx, js_upsampleNearest2xBackward,  "upsampleNearest2xBackward",  6));
    JS_SetPropertyStr(ctx, gpuObj, "upsampleBilinear2xForward",  JS_NewCFunction(ctx, js_upsampleBilinear2xForward,  "upsampleBilinear2xForward",  6));
    JS_SetPropertyStr(ctx, gpuObj, "upsampleBilinear2xBackward", JS_NewCFunction(ctx, js_upsampleBilinear2xBackward, "upsampleBilinear2xBackward", 6));
    JS_SetPropertyStr(ctx, gpuObj, "downsampleAvg2xForward",     JS_NewCFunction(ctx, js_downsampleAvg2xForward,     "downsampleAvg2xForward",     6));
    JS_SetPropertyStr(ctx, gpuObj, "downsampleAvg2xBackward",    JS_NewCFunction(ctx, js_downsampleAvg2xBackward,    "downsampleAvg2xBackward",    6));

    JS_SetPropertyStr(ctx, gpuObj, "nchwToSequence", JS_NewCFunction(ctx, js_nchwToSequence, "nchwToSequence", 6));
    JS_SetPropertyStr(ctx, gpuObj, "sequenceToNchw", JS_NewCFunction(ctx, js_sequenceToNchw, "sequenceToNchw", 6));

    JS_SetPropertyStr(ctx, gpuObj, "interp2dForward",             JS_NewCFunction(ctx, js_interp2dForward,             "interp2dForward",              9));
    JS_SetPropertyStr(ctx, gpuObj, "interp2dAlignCornersForward", JS_NewCFunction(ctx, js_interp2dAlignCornersForward, "interp2dAlignCornersForward",  9));
    JS_SetPropertyStr(ctx, gpuObj, "unfold2dForward",             JS_NewCFunction(ctx, js_unfold2dForward,             "unfold2dForward",             15));
    JS_SetPropertyStr(ctx, gpuObj, "l2NormalizeNchwForward",      JS_NewCFunction(ctx, js_l2NormalizeNchwForward,      "l2NormalizeNchwForward",       7));
    JS_SetPropertyStr(ctx, gpuObj, "convexUpsampleForward",       JS_NewCFunction(ctx, js_convexUpsampleForward,       "convexUpsampleForward",        8));
}

} // namespace bro::js

#else // !BROTENSOR_HAS_GPU

#include <qjsbind/qjsbind.h>
namespace bro::js {
void installTensorConvOps(JSContext*, JSValue) {}
} // namespace bro::js

#endif // BROTENSOR_HAS_GPU
