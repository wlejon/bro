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

static JSValue js_interp2dBackward(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 9) return JS_ThrowTypeError(ctx,
        "interp2dBackward(dY,N,C,H_in,W_in,H_out,W_out,mode,dX)");
    ENSURE_INIT();
    GT(dY, 0, "interp2dBackward");
    int32_t N=0,C=0,Hin=0,Win=0,Hout=0,Wout=0,mode=1;
    JS_ToInt32(ctx, &N,    argv[1]); JS_ToInt32(ctx, &C,    argv[2]);
    JS_ToInt32(ctx, &Hin,  argv[3]); JS_ToInt32(ctx, &Win,  argv[4]);
    JS_ToInt32(ctx, &Hout, argv[5]); JS_ToInt32(ctx, &Wout, argv[6]);
    JS_ToInt32(ctx, &mode, argv[7]);
    GT(dX, 8, "interp2dBackward");
    nngpu::interp2d_backward(*dY, N, C, Hin, Win, Hout, Wout, mode, *dX);
    return JS_UNDEFINED;
}

// ─── pad2d / slice2d (NCHW spatial pad + crop, fwd/bwd) ────────────────────
// pad2dForward(X,N,C,H,W,padT,padB,padL,padR,mode,Y) — mode 0 zero/1 reflect/2 replicate.
static JSValue js_pad2dForward(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 11) return JS_ThrowTypeError(ctx,
        "pad2dForward(X,N,C,H,W,padT,padB,padL,padR,mode,Y)");
    ENSURE_INIT();
    GT(X, 0, "pad2dForward");
    int32_t N=0,C=0,H=0,W=0,pt=0,pb=0,pl=0,pr=0,mode=0;
    JS_ToInt32(ctx, &N, argv[1]); JS_ToInt32(ctx, &C, argv[2]);
    JS_ToInt32(ctx, &H, argv[3]); JS_ToInt32(ctx, &W, argv[4]);
    JS_ToInt32(ctx, &pt, argv[5]); JS_ToInt32(ctx, &pb, argv[6]);
    JS_ToInt32(ctx, &pl, argv[7]); JS_ToInt32(ctx, &pr, argv[8]);
    JS_ToInt32(ctx, &mode, argv[9]);
    GT(Y, 10, "pad2dForward");
    nngpu::pad2d_forward(*X, N, C, H, W, pt, pb, pl, pr, mode, *Y);
    return JS_UNDEFINED;
}
static JSValue js_pad2dBackward(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 11) return JS_ThrowTypeError(ctx,
        "pad2dBackward(dY,N,C,H,W,padT,padB,padL,padR,mode,dX)");
    ENSURE_INIT();
    GT(dY, 0, "pad2dBackward");
    int32_t N=0,C=0,H=0,W=0,pt=0,pb=0,pl=0,pr=0,mode=0;
    JS_ToInt32(ctx, &N, argv[1]); JS_ToInt32(ctx, &C, argv[2]);
    JS_ToInt32(ctx, &H, argv[3]); JS_ToInt32(ctx, &W, argv[4]);
    JS_ToInt32(ctx, &pt, argv[5]); JS_ToInt32(ctx, &pb, argv[6]);
    JS_ToInt32(ctx, &pl, argv[7]); JS_ToInt32(ctx, &pr, argv[8]);
    JS_ToInt32(ctx, &mode, argv[9]);
    GT(dX, 10, "pad2dBackward");
    nngpu::pad2d_backward(*dY, N, C, H, W, pt, pb, pl, pr, mode, *dX);
    return JS_UNDEFINED;
}
// slice2dForward(X,N,C,H,W,h0,w0,H_out,W_out,Y)
static JSValue js_slice2dForward(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 10) return JS_ThrowTypeError(ctx,
        "slice2dForward(X,N,C,H,W,h0,w0,H_out,W_out,Y)");
    ENSURE_INIT();
    GT(X, 0, "slice2dForward");
    int32_t N=0,C=0,H=0,W=0,h0=0,w0=0,Ho=0,Wo=0;
    JS_ToInt32(ctx, &N, argv[1]); JS_ToInt32(ctx, &C, argv[2]);
    JS_ToInt32(ctx, &H, argv[3]); JS_ToInt32(ctx, &W, argv[4]);
    JS_ToInt32(ctx, &h0, argv[5]); JS_ToInt32(ctx, &w0, argv[6]);
    JS_ToInt32(ctx, &Ho, argv[7]); JS_ToInt32(ctx, &Wo, argv[8]);
    GT(Y, 9, "slice2dForward");
    nngpu::slice2d_forward(*X, N, C, H, W, h0, w0, Ho, Wo, *Y);
    return JS_UNDEFINED;
}
static JSValue js_slice2dBackward(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 10) return JS_ThrowTypeError(ctx,
        "slice2dBackward(dY,N,C,H,W,h0,w0,H_out,W_out,dX)");
    ENSURE_INIT();
    GT(dY, 0, "slice2dBackward");
    int32_t N=0,C=0,H=0,W=0,h0=0,w0=0,Ho=0,Wo=0;
    JS_ToInt32(ctx, &N, argv[1]); JS_ToInt32(ctx, &C, argv[2]);
    JS_ToInt32(ctx, &H, argv[3]); JS_ToInt32(ctx, &W, argv[4]);
    JS_ToInt32(ctx, &h0, argv[5]); JS_ToInt32(ctx, &w0, argv[6]);
    JS_ToInt32(ctx, &Ho, argv[7]); JS_ToInt32(ctx, &Wo, argv[8]);
    GT(dX, 9, "slice2dBackward");
    nngpu::slice2d_backward(*dY, N, C, H, W, h0, w0, Ho, Wo, *dX);
    return JS_UNDEFINED;
}

// ─── max_pool2d / adaptive_avg_pool2d (NCHW, fwd/bwd) ──────────────────────
// maxPool2dForward(X,N,C,H,W,kH,kW,sH,sW,padH,padW,Y,Idx) — Idx (INT32) feeds backward.
static JSValue js_maxPool2dForward(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 13) return JS_ThrowTypeError(ctx,
        "maxPool2dForward(X,N,C,H,W,kH,kW,sH,sW,padH,padW,Y,Idx)");
    ENSURE_INIT();
    GT(X, 0, "maxPool2dForward");
    int32_t N=0,C=0,H=0,W=0,kH=0,kW=0,sH=1,sW=1,pH=0,pW=0;
    JS_ToInt32(ctx, &N, argv[1]); JS_ToInt32(ctx, &C, argv[2]);
    JS_ToInt32(ctx, &H, argv[3]); JS_ToInt32(ctx, &W, argv[4]);
    JS_ToInt32(ctx, &kH, argv[5]); JS_ToInt32(ctx, &kW, argv[6]);
    JS_ToInt32(ctx, &sH, argv[7]); JS_ToInt32(ctx, &sW, argv[8]);
    JS_ToInt32(ctx, &pH, argv[9]); JS_ToInt32(ctx, &pW, argv[10]);
    GT(Y, 11, "maxPool2dForward"); GT(Idx, 12, "maxPool2dForward");
    nngpu::max_pool2d_forward(*X, N, C, H, W, kH, kW, sH, sW, pH, pW, *Y, *Idx);
    return JS_UNDEFINED;
}
static JSValue js_maxPool2dBackward(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 9) return JS_ThrowTypeError(ctx,
        "maxPool2dBackward(dY,Idx,N,C,H,W,H_out,W_out,dX)");
    ENSURE_INIT();
    GT(dY, 0, "maxPool2dBackward"); GT(Idx, 1, "maxPool2dBackward");
    int32_t N=0,C=0,H=0,W=0,Ho=0,Wo=0;
    JS_ToInt32(ctx, &N, argv[2]); JS_ToInt32(ctx, &C, argv[3]);
    JS_ToInt32(ctx, &H, argv[4]); JS_ToInt32(ctx, &W, argv[5]);
    JS_ToInt32(ctx, &Ho, argv[6]); JS_ToInt32(ctx, &Wo, argv[7]);
    GT(dX, 8, "maxPool2dBackward");
    nngpu::max_pool2d_backward(*dY, *Idx, N, C, H, W, Ho, Wo, *dX);
    return JS_UNDEFINED;
}
// adaptiveAvgPool2dForward(X,N,C,H,W,H_out,W_out,Y)
static JSValue js_adaptiveAvgPool2dForward(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 8) return JS_ThrowTypeError(ctx,
        "adaptiveAvgPool2dForward(X,N,C,H,W,H_out,W_out,Y)");
    ENSURE_INIT();
    GT(X, 0, "adaptiveAvgPool2dForward");
    int32_t N=0,C=0,H=0,W=0,Ho=0,Wo=0;
    JS_ToInt32(ctx, &N, argv[1]); JS_ToInt32(ctx, &C, argv[2]);
    JS_ToInt32(ctx, &H, argv[3]); JS_ToInt32(ctx, &W, argv[4]);
    JS_ToInt32(ctx, &Ho, argv[5]); JS_ToInt32(ctx, &Wo, argv[6]);
    GT(Y, 7, "adaptiveAvgPool2dForward");
    nngpu::adaptive_avg_pool2d_forward(*X, N, C, H, W, Ho, Wo, *Y);
    return JS_UNDEFINED;
}
static JSValue js_adaptiveAvgPool2dBackward(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 8) return JS_ThrowTypeError(ctx,
        "adaptiveAvgPool2dBackward(dY,N,C,H,W,H_out,W_out,dX)");
    ENSURE_INIT();
    GT(dY, 0, "adaptiveAvgPool2dBackward");
    int32_t N=0,C=0,H=0,W=0,Ho=0,Wo=0;
    JS_ToInt32(ctx, &N, argv[1]); JS_ToInt32(ctx, &C, argv[2]);
    JS_ToInt32(ctx, &H, argv[3]); JS_ToInt32(ctx, &W, argv[4]);
    JS_ToInt32(ctx, &Ho, argv[5]); JS_ToInt32(ctx, &Wo, argv[6]);
    GT(dX, 7, "adaptiveAvgPool2dBackward");
    nngpu::adaptive_avg_pool2d_backward(*dY, N, C, H, W, Ho, Wo, *dX);
    return JS_UNDEFINED;
}

// ─── conv_transpose2d (NCHW, fwd + per-input/weight/bias bwd) ──────────────
// convTranspose2dForward(X,Wt,bias|null,N,C_in,H,W,C_out,kH,kW,sH,sW,pH,pW,opH,opW,dH,dW,groups,Y)
static JSValue js_convTranspose2dForward(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 20) return JS_ThrowTypeError(ctx,
        "convTranspose2dForward(X,Wt,bias|null,N,C_in,H,W,C_out,kH,kW,sH,sW,pH,pW,opH,opW,dH,dW,groups,Y)");
    ENSURE_INIT();
    GT(X, 0, "convTranspose2dForward"); GT(Wt, 1, "convTranspose2dForward");
    JSValue err = JS_UNDEFINED; const nngpu::Tensor* bias = nullptr;
    if (!resolveOptionalConstGpuTensor(ctx, argv[2], bias, err, "bias")) return err;
    int32_t N=0,Cin=0,H=0,W=0,Cout=0,kH=0,kW=0,sH=1,sW=1,pH=0,pW=0,opH=0,opW=0,dH=1,dW=1,groups=1;
    JS_ToInt32(ctx, &N, argv[3]);   JS_ToInt32(ctx, &Cin, argv[4]);
    JS_ToInt32(ctx, &H, argv[5]);   JS_ToInt32(ctx, &W, argv[6]);
    JS_ToInt32(ctx, &Cout, argv[7]);JS_ToInt32(ctx, &kH, argv[8]);
    JS_ToInt32(ctx, &kW, argv[9]);  JS_ToInt32(ctx, &sH, argv[10]);
    JS_ToInt32(ctx, &sW, argv[11]); JS_ToInt32(ctx, &pH, argv[12]);
    JS_ToInt32(ctx, &pW, argv[13]); JS_ToInt32(ctx, &opH, argv[14]);
    JS_ToInt32(ctx, &opW, argv[15]);JS_ToInt32(ctx, &dH, argv[16]);
    JS_ToInt32(ctx, &dW, argv[17]); JS_ToInt32(ctx, &groups, argv[18]);
    GT(Y, 19, "convTranspose2dForward");
    nngpu::conv_transpose2d_forward(*X, *Wt, bias, N, Cin, H, W, Cout, kH, kW,
                                    sH, sW, pH, pW, opH, opW, dH, dW, groups, *Y);
    return JS_UNDEFINED;
}
static JSValue js_convTranspose2dBackwardInput(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 19) return JS_ThrowTypeError(ctx,
        "convTranspose2dBackwardInput(Wt,dY,N,C_in,H,W,C_out,kH,kW,sH,sW,pH,pW,opH,opW,dH,dW,groups,dX)");
    ENSURE_INIT();
    GT(Wt, 0, "convTranspose2dBackwardInput"); GT(dY, 1, "convTranspose2dBackwardInput");
    int32_t N=0,Cin=0,H=0,W=0,Cout=0,kH=0,kW=0,sH=1,sW=1,pH=0,pW=0,opH=0,opW=0,dH=1,dW=1,groups=1;
    JS_ToInt32(ctx, &N, argv[2]);   JS_ToInt32(ctx, &Cin, argv[3]);
    JS_ToInt32(ctx, &H, argv[4]);   JS_ToInt32(ctx, &W, argv[5]);
    JS_ToInt32(ctx, &Cout, argv[6]);JS_ToInt32(ctx, &kH, argv[7]);
    JS_ToInt32(ctx, &kW, argv[8]);  JS_ToInt32(ctx, &sH, argv[9]);
    JS_ToInt32(ctx, &sW, argv[10]); JS_ToInt32(ctx, &pH, argv[11]);
    JS_ToInt32(ctx, &pW, argv[12]); JS_ToInt32(ctx, &opH, argv[13]);
    JS_ToInt32(ctx, &opW, argv[14]);JS_ToInt32(ctx, &dH, argv[15]);
    JS_ToInt32(ctx, &dW, argv[16]); JS_ToInt32(ctx, &groups, argv[17]);
    GT(dX, 18, "convTranspose2dBackwardInput");
    nngpu::conv_transpose2d_backward_input(*Wt, *dY, N, Cin, H, W, Cout, kH, kW,
                                           sH, sW, pH, pW, opH, opW, dH, dW, groups, *dX);
    return JS_UNDEFINED;
}
static JSValue js_convTranspose2dBackwardWeight(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 19) return JS_ThrowTypeError(ctx,
        "convTranspose2dBackwardWeight(X,dY,N,C_in,H,W,C_out,kH,kW,sH,sW,pH,pW,opH,opW,dH,dW,groups,dWt)");
    ENSURE_INIT();
    GT(X, 0, "convTranspose2dBackwardWeight"); GT(dY, 1, "convTranspose2dBackwardWeight");
    int32_t N=0,Cin=0,H=0,W=0,Cout=0,kH=0,kW=0,sH=1,sW=1,pH=0,pW=0,opH=0,opW=0,dH=1,dW=1,groups=1;
    JS_ToInt32(ctx, &N, argv[2]);   JS_ToInt32(ctx, &Cin, argv[3]);
    JS_ToInt32(ctx, &H, argv[4]);   JS_ToInt32(ctx, &W, argv[5]);
    JS_ToInt32(ctx, &Cout, argv[6]);JS_ToInt32(ctx, &kH, argv[7]);
    JS_ToInt32(ctx, &kW, argv[8]);  JS_ToInt32(ctx, &sH, argv[9]);
    JS_ToInt32(ctx, &sW, argv[10]); JS_ToInt32(ctx, &pH, argv[11]);
    JS_ToInt32(ctx, &pW, argv[12]); JS_ToInt32(ctx, &opH, argv[13]);
    JS_ToInt32(ctx, &opW, argv[14]);JS_ToInt32(ctx, &dH, argv[15]);
    JS_ToInt32(ctx, &dW, argv[16]); JS_ToInt32(ctx, &groups, argv[17]);
    GT(dWt, 18, "convTranspose2dBackwardWeight");
    nngpu::conv_transpose2d_backward_weight(*X, *dY, N, Cin, H, W, Cout, kH, kW,
                                            sH, sW, pH, pW, opH, opW, dH, dW, groups, *dWt);
    return JS_UNDEFINED;
}
static JSValue js_convTranspose2dBackwardBias(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 6) return JS_ThrowTypeError(ctx,
        "convTranspose2dBackwardBias(dY,N,C_out,H_out,W_out,dB)");
    ENSURE_INIT();
    GT(dY, 0, "convTranspose2dBackwardBias");
    int32_t N=0,Cout=0,Ho=0,Wo=0;
    JS_ToInt32(ctx, &N, argv[1]); JS_ToInt32(ctx, &Cout, argv[2]);
    JS_ToInt32(ctx, &Ho, argv[3]); JS_ToInt32(ctx, &Wo, argv[4]);
    GT(dB, 5, "convTranspose2dBackwardBias");
    nngpu::conv_transpose2d_backward_bias(*dY, N, Cout, Ho, Wo, *dB);
    return JS_UNDEFINED;
}

// ─── conv3d (NCDHW-ish: N,C,T,H,W) forward ─────────────────────────────────
// conv3dForward(X,Wt,bias|null,N,C_in,T,H,W,C_out,kT,kH,kW,sT,sH,sW,pT,pH,pW,dT,dH,dW,groups,Y)
static JSValue js_conv3dForward(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 23) return JS_ThrowTypeError(ctx,
        "conv3dForward(X,Wt,bias|null,N,C_in,T,H,W,C_out,kT,kH,kW,sT,sH,sW,pT,pH,pW,dT,dH,dW,groups,Y)");
    ENSURE_INIT();
    GT(X, 0, "conv3dForward"); GT(Wt, 1, "conv3dForward");
    JSValue err = JS_UNDEFINED; const nngpu::Tensor* bias = nullptr;
    if (!resolveOptionalConstGpuTensor(ctx, argv[2], bias, err, "bias")) return err;
    int32_t N=0,Cin=0,T=0,H=0,W=0,Cout=0,kT=0,kH=0,kW=0;
    int32_t sT=1,sH=1,sW=1,pT=0,pH=0,pW=0,dT=1,dH=1,dW=1,groups=1;
    JS_ToInt32(ctx, &N, argv[3]);   JS_ToInt32(ctx, &Cin, argv[4]);
    JS_ToInt32(ctx, &T, argv[5]);   JS_ToInt32(ctx, &H, argv[6]);
    JS_ToInt32(ctx, &W, argv[7]);   JS_ToInt32(ctx, &Cout, argv[8]);
    JS_ToInt32(ctx, &kT, argv[9]);  JS_ToInt32(ctx, &kH, argv[10]);
    JS_ToInt32(ctx, &kW, argv[11]); JS_ToInt32(ctx, &sT, argv[12]);
    JS_ToInt32(ctx, &sH, argv[13]); JS_ToInt32(ctx, &sW, argv[14]);
    JS_ToInt32(ctx, &pT, argv[15]); JS_ToInt32(ctx, &pH, argv[16]);
    JS_ToInt32(ctx, &pW, argv[17]); JS_ToInt32(ctx, &dT, argv[18]);
    JS_ToInt32(ctx, &dH, argv[19]); JS_ToInt32(ctx, &dW, argv[20]);
    JS_ToInt32(ctx, &groups, argv[21]);
    GT(Y, 22, "conv3dForward");
    nngpu::conv3d_forward(*X, *Wt, bias, N, Cin, T, H, W, Cout, kT, kH, kW,
                          sT, sH, sW, pT, pH, pW, dT, dH, dW, groups, *Y);
    return JS_UNDEFINED;
}

// ─── window partition / reverse / 2x2 spatial merge (Swin/SAM/Qwen-VL) ─────
static JSValue js_windowPartitionForward(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 7) return JS_ThrowTypeError(ctx, "windowPartitionForward(X,N,C,H,W,window,Y)");
    ENSURE_INIT();
    GT(X, 0, "windowPartitionForward");
    int32_t N=0,C=0,H=0,W=0,win=0;
    JS_ToInt32(ctx, &N, argv[1]); JS_ToInt32(ctx, &C, argv[2]);
    JS_ToInt32(ctx, &H, argv[3]); JS_ToInt32(ctx, &W, argv[4]);
    JS_ToInt32(ctx, &win, argv[5]);
    GT(Y, 6, "windowPartitionForward");
    nngpu::window_partition_forward(*X, N, C, H, W, win, *Y);
    return JS_UNDEFINED;
}
static JSValue js_windowReverseForward(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 7) return JS_ThrowTypeError(ctx, "windowReverseForward(X,N,C,H,W,window,Y)");
    ENSURE_INIT();
    GT(X, 0, "windowReverseForward");
    int32_t N=0,C=0,H=0,W=0,win=0;
    JS_ToInt32(ctx, &N, argv[1]); JS_ToInt32(ctx, &C, argv[2]);
    JS_ToInt32(ctx, &H, argv[3]); JS_ToInt32(ctx, &W, argv[4]);
    JS_ToInt32(ctx, &win, argv[5]);
    GT(Y, 6, "windowReverseForward");
    nngpu::window_reverse_forward(*X, N, C, H, W, win, *Y);
    return JS_UNDEFINED;
}
static JSValue js_spatialMerge2x2Forward(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 6) return JS_ThrowTypeError(ctx, "spatialMerge2x2Forward(X,N,C,H,W,Y,channelMajor?)");
    ENSURE_INIT();
    GT(X, 0, "spatialMerge2x2Forward");
    int32_t N=0,C=0,H=0,W=0;
    JS_ToInt32(ctx, &N, argv[1]); JS_ToInt32(ctx, &C, argv[2]);
    JS_ToInt32(ctx, &H, argv[3]); JS_ToInt32(ctx, &W, argv[4]);
    GT(Y, 5, "spatialMerge2x2Forward");
    // Optional ordering flag: false (default) = block-major c_out=block*C+c_in
    // (Qwen-VL); true = channel-major c_out=c_in*4+block (torch pixel_unshuffle).
    bool channelMajor = (argc > 6) && (JS_ToBool(ctx, argv[6]) == 1);
    nngpu::spatial_merge_2x2_forward(*X, N, C, H, W, channelMajor, *Y);
    return JS_UNDEFINED;
}

// ─── batch_norm (NCHW, fwd/bwd/inference) ──────────────────────────────────
// batchNormForward(X,gamma,beta,runningMean,runningVar,N,C,H,W,eps,momentum,Y,savedMean,savedRstd)
static JSValue js_batchNormForward(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 14) return JS_ThrowTypeError(ctx,
        "batchNormForward(X,gamma,beta,runningMean,runningVar,N,C,H,W,eps,momentum,Y,savedMean,savedRstd)");
    ENSURE_INIT();
    GT(X, 0, "batchNormForward"); GT(gamma, 1, "batchNormForward"); GT(beta, 2, "batchNormForward");
    GT(rmean, 3, "batchNormForward"); GT(rvar, 4, "batchNormForward");
    int32_t N=0,C=0,H=0,W=0; double eps=1e-5, mom=0.1;
    JS_ToInt32(ctx, &N, argv[5]); JS_ToInt32(ctx, &C, argv[6]);
    JS_ToInt32(ctx, &H, argv[7]); JS_ToInt32(ctx, &W, argv[8]);
    JS_ToFloat64(ctx, &eps, argv[9]); JS_ToFloat64(ctx, &mom, argv[10]);
    GT(Y, 11, "batchNormForward"); GT(smean, 12, "batchNormForward"); GT(srstd, 13, "batchNormForward");
    nngpu::batch_norm_forward(*X, *gamma, *beta, *rmean, *rvar, N, C, H, W,
                              (float)eps, (float)mom, *Y, *smean, *srstd);
    return JS_UNDEFINED;
}
static JSValue js_batchNormBackward(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 12) return JS_ThrowTypeError(ctx,
        "batchNormBackward(X,gamma,savedMean,savedRstd,dY,N,C,H,W,dX,dGamma,dBeta)");
    ENSURE_INIT();
    GT(X, 0, "batchNormBackward"); GT(gamma, 1, "batchNormBackward");
    GT(smean, 2, "batchNormBackward"); GT(srstd, 3, "batchNormBackward"); GT(dY, 4, "batchNormBackward");
    int32_t N=0,C=0,H=0,W=0;
    JS_ToInt32(ctx, &N, argv[5]); JS_ToInt32(ctx, &C, argv[6]);
    JS_ToInt32(ctx, &H, argv[7]); JS_ToInt32(ctx, &W, argv[8]);
    GT(dX, 9, "batchNormBackward"); GT(dGamma, 10, "batchNormBackward"); GT(dBeta, 11, "batchNormBackward");
    nngpu::batch_norm_backward(*X, *gamma, *smean, *srstd, *dY, N, C, H, W, *dX, *dGamma, *dBeta);
    return JS_UNDEFINED;
}
static JSValue js_batchNormInference(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 11) return JS_ThrowTypeError(ctx,
        "batchNormInference(X,gamma,beta,runningMean,runningVar,N,C,H,W,eps,Y)");
    ENSURE_INIT();
    GT(X, 0, "batchNormInference"); GT(gamma, 1, "batchNormInference"); GT(beta, 2, "batchNormInference");
    GT(rmean, 3, "batchNormInference"); GT(rvar, 4, "batchNormInference");
    int32_t N=0,C=0,H=0,W=0; double eps=1e-5;
    JS_ToInt32(ctx, &N, argv[5]); JS_ToInt32(ctx, &C, argv[6]);
    JS_ToInt32(ctx, &H, argv[7]); JS_ToInt32(ctx, &W, argv[8]);
    JS_ToFloat64(ctx, &eps, argv[9]);
    GT(Y, 10, "batchNormInference");
    nngpu::batch_norm_inference(*X, *gamma, *beta, *rmean, *rvar, N, C, H, W, (float)eps, *Y);
    return JS_UNDEFINED;
}

// ─── image preprocessing ───────────────────────────────────────────────────
// imageNormalize(X, mean, std, N, C, H, W, Y) — per-channel (x-mean)/std, NCHW.
static JSValue js_imageNormalize(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 8) return JS_ThrowTypeError(ctx, "imageNormalize(X,mean,std,N,C,H,W,Y)");
    ENSURE_INIT();
    GT(X, 0, "imageNormalize"); GT(mean, 1, "imageNormalize"); GT(std_, 2, "imageNormalize");
    int32_t N=0,C=0,H=0,W=0;
    JS_ToInt32(ctx, &N, argv[3]); JS_ToInt32(ctx, &C, argv[4]);
    JS_ToInt32(ctx, &H, argv[5]); JS_ToInt32(ctx, &W, argv[6]);
    GT(Y, 7, "imageNormalize");
    nngpu::image_normalize(*X, *mean, *std_, N, C, H, W, *Y);
    return JS_UNDEFINED;
}
// imageU8ToF32NhwcToNchw(srcUint8Array, N, H, W, C, scale, bias, Y)
static JSValue js_imageU8ToF32NhwcToNchw(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 8) return JS_ThrowTypeError(ctx,
        "imageU8ToF32NhwcToNchw(srcUint8,N,H,W,C,scale,bias,Y)");
    ENSURE_INIT();
    size_t nbytes = 0;
    uint8_t* src = getTypedArrayBytePtr(ctx, argv[0], nbytes);
    if (!src) return JS_ThrowTypeError(ctx, "imageU8ToF32NhwcToNchw — src must be a Uint8Array");
    int32_t N=0,H=0,W=0,C=0; double scale=1.0, bias=0.0;
    JS_ToInt32(ctx, &N, argv[1]); JS_ToInt32(ctx, &H, argv[2]);
    JS_ToInt32(ctx, &W, argv[3]); JS_ToInt32(ctx, &C, argv[4]);
    JS_ToFloat64(ctx, &scale, argv[5]); JS_ToFloat64(ctx, &bias, argv[6]);
    GT(Y, 7, "imageU8ToF32NhwcToNchw");
    nngpu::image_u8_to_f32_nhwc_to_nchw(src, N, H, W, C, (float)scale, (float)bias, *Y);
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
    JS_SetPropertyStr(ctx, gpuObj, "interp2dBackward",            JS_NewCFunction(ctx, js_interp2dBackward,            "interp2dBackward",             9));
    JS_SetPropertyStr(ctx, gpuObj, "unfold2dForward",             JS_NewCFunction(ctx, js_unfold2dForward,             "unfold2dForward",             15));
    JS_SetPropertyStr(ctx, gpuObj, "l2NormalizeNchwForward",      JS_NewCFunction(ctx, js_l2NormalizeNchwForward,      "l2NormalizeNchwForward",       7));
    JS_SetPropertyStr(ctx, gpuObj, "convexUpsampleForward",       JS_NewCFunction(ctx, js_convexUpsampleForward,       "convexUpsampleForward",        8));

    JS_SetPropertyStr(ctx, gpuObj, "pad2dForward",   JS_NewCFunction(ctx, js_pad2dForward,   "pad2dForward",   11));
    JS_SetPropertyStr(ctx, gpuObj, "pad2dBackward",  JS_NewCFunction(ctx, js_pad2dBackward,  "pad2dBackward",  11));
    JS_SetPropertyStr(ctx, gpuObj, "slice2dForward", JS_NewCFunction(ctx, js_slice2dForward, "slice2dForward", 10));
    JS_SetPropertyStr(ctx, gpuObj, "slice2dBackward",JS_NewCFunction(ctx, js_slice2dBackward,"slice2dBackward",10));

    JS_SetPropertyStr(ctx, gpuObj, "maxPool2dForward",          JS_NewCFunction(ctx, js_maxPool2dForward,          "maxPool2dForward",          13));
    JS_SetPropertyStr(ctx, gpuObj, "maxPool2dBackward",         JS_NewCFunction(ctx, js_maxPool2dBackward,         "maxPool2dBackward",          9));
    JS_SetPropertyStr(ctx, gpuObj, "adaptiveAvgPool2dForward",  JS_NewCFunction(ctx, js_adaptiveAvgPool2dForward,  "adaptiveAvgPool2dForward",   8));
    JS_SetPropertyStr(ctx, gpuObj, "adaptiveAvgPool2dBackward", JS_NewCFunction(ctx, js_adaptiveAvgPool2dBackward, "adaptiveAvgPool2dBackward",  8));

    JS_SetPropertyStr(ctx, gpuObj, "convTranspose2dForward",        JS_NewCFunction(ctx, js_convTranspose2dForward,        "convTranspose2dForward",        20));
    JS_SetPropertyStr(ctx, gpuObj, "convTranspose2dBackwardInput",  JS_NewCFunction(ctx, js_convTranspose2dBackwardInput,  "convTranspose2dBackwardInput",  19));
    JS_SetPropertyStr(ctx, gpuObj, "convTranspose2dBackwardWeight", JS_NewCFunction(ctx, js_convTranspose2dBackwardWeight, "convTranspose2dBackwardWeight", 19));
    JS_SetPropertyStr(ctx, gpuObj, "convTranspose2dBackwardBias",   JS_NewCFunction(ctx, js_convTranspose2dBackwardBias,   "convTranspose2dBackwardBias",    6));
    JS_SetPropertyStr(ctx, gpuObj, "conv3dForward",                 JS_NewCFunction(ctx, js_conv3dForward,                 "conv3dForward",                 23));

    JS_SetPropertyStr(ctx, gpuObj, "windowPartitionForward", JS_NewCFunction(ctx, js_windowPartitionForward, "windowPartitionForward", 7));
    JS_SetPropertyStr(ctx, gpuObj, "windowReverseForward",   JS_NewCFunction(ctx, js_windowReverseForward,   "windowReverseForward",   7));
    JS_SetPropertyStr(ctx, gpuObj, "spatialMerge2x2Forward", JS_NewCFunction(ctx, js_spatialMerge2x2Forward, "spatialMerge2x2Forward", 6));

    JS_SetPropertyStr(ctx, gpuObj, "batchNormForward",   JS_NewCFunction(ctx, js_batchNormForward,   "batchNormForward",   14));
    JS_SetPropertyStr(ctx, gpuObj, "batchNormBackward",  JS_NewCFunction(ctx, js_batchNormBackward,  "batchNormBackward",  12));
    JS_SetPropertyStr(ctx, gpuObj, "batchNormInference", JS_NewCFunction(ctx, js_batchNormInference, "batchNormInference", 11));

    JS_SetPropertyStr(ctx, gpuObj, "imageNormalize",         JS_NewCFunction(ctx, js_imageNormalize,         "imageNormalize",         8));
    JS_SetPropertyStr(ctx, gpuObj, "imageU8ToF32NhwcToNchw", JS_NewCFunction(ctx, js_imageU8ToF32NhwcToNchw, "imageU8ToF32NhwcToNchw", 8));
}

} // namespace bro::js

#else // !BROTENSOR_HAS_GPU

#include <qjsbind/qjsbind.h>
namespace bro::js {
void installTensorConvOps(JSContext*, JSValue) {}
} // namespace bro::js

#endif // BROTENSOR_HAS_GPU
