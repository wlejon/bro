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
}

} // namespace bro::js

#else // !BROTENSOR_HAS_GPU

#include <qjsbind/qjsbind.h>
namespace bro::js {
void installTensorConvOps(JSContext*, JSValue) {}
} // namespace bro::js

#endif // BROTENSOR_HAS_GPU
