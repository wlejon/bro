// JS bindings — brosoundml audio ops: spectral / FFT core, STFT / iSTFT, the
// 1D convolution family, vocoder + codec activations, codec quantization, 1D
// resampling, log/exp/round elementwise, and the autoregressive logit sampler.
// See tensor_bindings.cpp for the architectural overview.
//
// These are the building blocks of Whisper / TTS / neural-codec / vocoder
// pipelines. Complex spectra are carried as ordinary FP32 tensors with the bin
// axis stored interleaved [re, im, re, im, ...] — an (R, 2*C) tensor — so no
// new dtype is involved. brotensor implements this family on the CPU and both
// GPU backends; `sampleLogits` is the one exception (no CUDA kernel — it
// throws on a CUDA build, runs on Metal).

#ifdef BROTENSOR_HAS_GPU

#include "js/tensor_bindings_internal.h"

namespace bro::js {

#define ENSURE_INIT() BROTENSOR_ENSURE_INIT()
#define GT(name, idx, label) BROTENSOR_GT(name, idx, label)

// ─── Spectral / FFT core ──────────────────────────────────────────────────
//
// fft / ifft / rfft / irfft transform one signal per tensor row. fft and ifft
// have no explicit backward (the adjoint is the other transform plus a
// scalar); rfft / irfft do — their adjoints carry bin weighting that is easy
// to get wrong by hand.

static JSValue js_fft(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 2) return JS_ThrowTypeError(ctx, "fft(x,y)");
    ENSURE_INIT();
    GT(x, 0, "fft"); GT(y, 1, "fft");
    nngpu::fft(*x, *y);
    return JS_UNDEFINED;
}

static JSValue js_ifft(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 2) return JS_ThrowTypeError(ctx, "ifft(x,y)");
    ENSURE_INIT();
    GT(x, 0, "ifft"); GT(y, 1, "ifft");
    nngpu::ifft(*x, *y);
    return JS_UNDEFINED;
}

static JSValue js_rfft(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 2) return JS_ThrowTypeError(ctx, "rfft(x,y)");
    ENSURE_INIT();
    GT(x, 0, "rfft"); GT(y, 1, "rfft");
    nngpu::rfft(*x, *y);
    return JS_UNDEFINED;
}

static JSValue js_irfft(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 3) return JS_ThrowTypeError(ctx, "irfft(x,L,y)");
    ENSURE_INIT();
    GT(x, 0, "irfft");
    int32_t L = 0; JS_ToInt32(ctx, &L, argv[1]);
    GT(y, 2, "irfft");
    nngpu::irfft(*x, L, *y);
    return JS_UNDEFINED;
}

static JSValue js_rfftBackward(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 3) return JS_ThrowTypeError(ctx, "rfftBackward(dY,L,dX)");
    ENSURE_INIT();
    GT(dY, 0, "rfftBackward");
    int32_t L = 0; JS_ToInt32(ctx, &L, argv[1]);
    GT(dX, 2, "rfftBackward");
    nngpu::rfft_backward(*dY, L, *dX);
    return JS_UNDEFINED;
}

static JSValue js_irfftBackward(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 2) return JS_ThrowTypeError(ctx, "irfftBackward(dY,dX)");
    ENSURE_INIT();
    GT(dY, 0, "irfftBackward"); GT(dX, 1, "irfftBackward");
    nngpu::irfft_backward(*dY, *dX);
    return JS_UNDEFINED;
}

static JSValue js_complexMul(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 3) return JS_ThrowTypeError(ctx, "complexMul(a,b,y)");
    ENSURE_INIT();
    GT(a, 0, "complexMul"); GT(b, 1, "complexMul"); GT(y, 2, "complexMul");
    nngpu::complex_mul(*a, *b, *y);
    return JS_UNDEFINED;
}

static JSValue js_complexMulBackward(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 5) return JS_ThrowTypeError(ctx, "complexMulBackward(a,b,dY,dA,dB)");
    ENSURE_INIT();
    GT(a, 0, "complexMulBackward"); GT(b, 1, "complexMulBackward");
    GT(dY, 2, "complexMulBackward"); GT(dA, 3, "complexMulBackward");
    GT(dB, 4, "complexMulBackward");
    nngpu::complex_mul_backward(*a, *b, *dY, *dA, *dB);
    return JS_UNDEFINED;
}

static JSValue js_complexAbs(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 2) return JS_ThrowTypeError(ctx, "complexAbs(z,y)");
    ENSURE_INIT();
    GT(z, 0, "complexAbs"); GT(y, 1, "complexAbs");
    nngpu::complex_abs(*z, *y);
    return JS_UNDEFINED;
}

static JSValue js_complexAbsBackward(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 3) return JS_ThrowTypeError(ctx, "complexAbsBackward(z,dY,dZ)");
    ENSURE_INIT();
    GT(z, 0, "complexAbsBackward"); GT(dY, 1, "complexAbsBackward");
    GT(dZ, 2, "complexAbsBackward");
    nngpu::complex_abs_backward(*z, *dY, *dZ);
    return JS_UNDEFINED;
}

static JSValue js_complexAngle(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 2) return JS_ThrowTypeError(ctx, "complexAngle(z,y)");
    ENSURE_INIT();
    GT(z, 0, "complexAngle"); GT(y, 1, "complexAngle");
    nngpu::complex_angle(*z, *y);
    return JS_UNDEFINED;
}

static JSValue js_complexFromPolar(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 3) return JS_ThrowTypeError(ctx, "complexFromPolar(mag,phase,y)");
    ENSURE_INIT();
    GT(mag, 0, "complexFromPolar"); GT(phase, 1, "complexFromPolar");
    GT(y, 2, "complexFromPolar");
    nngpu::complex_from_polar(*mag, *phase, *y);
    return JS_UNDEFINED;
}

// ─── STFT / iSTFT ─────────────────────────────────────────────────────────
//
// Short-time Fourier transform and its inverse. The spectrogram is
// interleaved-complex (N*frames, 2*(n_fft/2+1)); stft and istft are linear
// but NOT mutual adjoints once the window and COLA normalisation are folded
// in, so both backward ops are explicit.

static JSValue js_stft(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 9) return JS_ThrowTypeError(ctx,
        "stft(signal,window,N,nFft,hopLength,winLength,center,normalized,spec)");
    ENSURE_INIT();
    GT(signal, 0, "stft"); GT(window, 1, "stft");
    int32_t N = 0, nFft = 0, hop = 0, winLen = 0;
    JS_ToInt32(ctx, &N,      argv[2]);
    JS_ToInt32(ctx, &nFft,   argv[3]);
    JS_ToInt32(ctx, &hop,    argv[4]);
    JS_ToInt32(ctx, &winLen, argv[5]);
    bool center     = getBool(ctx, argv[6], false);
    bool normalized = getBool(ctx, argv[7], false);
    GT(spec, 8, "stft");
    nngpu::stft(*signal, *window, N, nFft, hop, winLen, center, normalized, *spec);
    return JS_UNDEFINED;
}

static JSValue js_stftBackward(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 10) return JS_ThrowTypeError(ctx,
        "stftBackward(dSpec,window,N,signalLen,nFft,hopLength,winLength,center,normalized,dSignal)");
    ENSURE_INIT();
    GT(dSpec, 0, "stftBackward"); GT(window, 1, "stftBackward");
    int32_t N = 0, sigLen = 0, nFft = 0, hop = 0, winLen = 0;
    JS_ToInt32(ctx, &N,      argv[2]);
    JS_ToInt32(ctx, &sigLen, argv[3]);
    JS_ToInt32(ctx, &nFft,   argv[4]);
    JS_ToInt32(ctx, &hop,    argv[5]);
    JS_ToInt32(ctx, &winLen, argv[6]);
    bool center     = getBool(ctx, argv[7], false);
    bool normalized = getBool(ctx, argv[8], false);
    GT(dSignal, 9, "stftBackward");
    nngpu::stft_backward(*dSpec, *window, N, sigLen, nFft, hop, winLen,
                         center, normalized, *dSignal);
    return JS_UNDEFINED;
}

static JSValue js_istft(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 10) return JS_ThrowTypeError(ctx,
        "istft(spec,window,N,signalLen,nFft,hopLength,winLength,center,normalized,signal)");
    ENSURE_INIT();
    GT(spec, 0, "istft"); GT(window, 1, "istft");
    int32_t N = 0, sigLen = 0, nFft = 0, hop = 0, winLen = 0;
    JS_ToInt32(ctx, &N,      argv[2]);
    JS_ToInt32(ctx, &sigLen, argv[3]);
    JS_ToInt32(ctx, &nFft,   argv[4]);
    JS_ToInt32(ctx, &hop,    argv[5]);
    JS_ToInt32(ctx, &winLen, argv[6]);
    bool center     = getBool(ctx, argv[7], false);
    bool normalized = getBool(ctx, argv[8], false);
    GT(signal, 9, "istft");
    nngpu::istft(*spec, *window, N, sigLen, nFft, hop, winLen,
                 center, normalized, *signal);
    return JS_UNDEFINED;
}

static JSValue js_istftBackward(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 10) return JS_ThrowTypeError(ctx,
        "istftBackward(dSignal,window,N,signalLen,nFft,hopLength,winLength,center,normalized,dSpec)");
    ENSURE_INIT();
    GT(dSignal, 0, "istftBackward"); GT(window, 1, "istftBackward");
    int32_t N = 0, sigLen = 0, nFft = 0, hop = 0, winLen = 0;
    JS_ToInt32(ctx, &N,      argv[2]);
    JS_ToInt32(ctx, &sigLen, argv[3]);
    JS_ToInt32(ctx, &nFft,   argv[4]);
    JS_ToInt32(ctx, &hop,    argv[5]);
    JS_ToInt32(ctx, &winLen, argv[6]);
    bool center     = getBool(ctx, argv[7], false);
    bool normalized = getBool(ctx, argv[8], false);
    GT(dSpec, 9, "istftBackward");
    nngpu::istft_backward(*dSignal, *window, N, sigLen, nFft, hop, winLen,
                          center, normalized, *dSpec);
    return JS_UNDEFINED;
}

// ─── 1D convolution family (NCL) ──────────────────────────────────────────
//
// Activations are (N, C * L); weights are OIL. conv1d / its backward halves /
// the W8A16 conv1d are conv2d wrappers with the height axis collapsed;
// conv_transpose1d, causal_conv1d_update and pad1d are genuine 1D kernels.

static JSValue js_pad1dForward(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 8) return JS_ThrowTypeError(ctx,
        "pad1dForward(X,N,C,L,padLeft,padRight,mode,Y)");
    ENSURE_INIT();
    GT(X, 0, "pad1dForward");
    int32_t N = 0, C = 0, L = 0, padL = 0, padR = 0, mode = 0;
    JS_ToInt32(ctx, &N,    argv[1]);
    JS_ToInt32(ctx, &C,    argv[2]);
    JS_ToInt32(ctx, &L,    argv[3]);
    JS_ToInt32(ctx, &padL, argv[4]);
    JS_ToInt32(ctx, &padR, argv[5]);
    JS_ToInt32(ctx, &mode, argv[6]);
    GT(Y, 7, "pad1dForward");
    nngpu::pad1d_forward(*X, N, C, L, padL, padR, mode, *Y);
    return JS_UNDEFINED;
}

static JSValue js_pad1dBackward(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 8) return JS_ThrowTypeError(ctx,
        "pad1dBackward(dY,N,C,L,padLeft,padRight,mode,dX)");
    ENSURE_INIT();
    GT(dY, 0, "pad1dBackward");
    int32_t N = 0, C = 0, L = 0, padL = 0, padR = 0, mode = 0;
    JS_ToInt32(ctx, &N,    argv[1]);
    JS_ToInt32(ctx, &C,    argv[2]);
    JS_ToInt32(ctx, &L,    argv[3]);
    JS_ToInt32(ctx, &padL, argv[4]);
    JS_ToInt32(ctx, &padR, argv[5]);
    JS_ToInt32(ctx, &mode, argv[6]);
    GT(dX, 7, "pad1dBackward");
    nngpu::pad1d_backward(*dY, N, C, L, padL, padR, mode, *dX);
    return JS_UNDEFINED;
}

static JSValue js_conv1d(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 13) return JS_ThrowTypeError(ctx,
        "conv1d(X,Wt,bias|null,N,C_in,L,C_out,kL,stride,padding,dilation,groups,Y)");
    ENSURE_INIT();
    GT(X, 0, "conv1d"); GT(Wt, 1, "conv1d");
    JSValue err = JS_UNDEFINED;
    const nngpu::Tensor* bias = nullptr;
    if (!resolveOptionalConstGpuTensor(ctx, argv[2], bias, err, "bias")) return err;
    int32_t N = 0, Cin = 0, L = 0, Cout = 0, kL = 0;
    int32_t stride = 1, padding = 0, dilation = 1, groups = 1;
    JS_ToInt32(ctx, &N,        argv[3]);
    JS_ToInt32(ctx, &Cin,      argv[4]);
    JS_ToInt32(ctx, &L,        argv[5]);
    JS_ToInt32(ctx, &Cout,     argv[6]);
    JS_ToInt32(ctx, &kL,       argv[7]);
    JS_ToInt32(ctx, &stride,   argv[8]);
    JS_ToInt32(ctx, &padding,  argv[9]);
    JS_ToInt32(ctx, &dilation, argv[10]);
    JS_ToInt32(ctx, &groups,   argv[11]);
    GT(Y, 12, "conv1d");
    nngpu::conv1d(*X, *Wt, bias, N, Cin, L, Cout, kL,
                  stride, padding, dilation, groups, *Y);
    return JS_UNDEFINED;
}

static JSValue js_conv1dBackwardInput(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 12) return JS_ThrowTypeError(ctx,
        "conv1dBackwardInput(Wt,dY,N,C_in,L,C_out,kL,stride,padding,dilation,groups,dX)");
    ENSURE_INIT();
    GT(Wt, 0, "conv1dBackwardInput"); GT(dY, 1, "conv1dBackwardInput");
    int32_t N = 0, Cin = 0, L = 0, Cout = 0, kL = 0;
    int32_t stride = 1, padding = 0, dilation = 1, groups = 1;
    JS_ToInt32(ctx, &N,        argv[2]);
    JS_ToInt32(ctx, &Cin,      argv[3]);
    JS_ToInt32(ctx, &L,        argv[4]);
    JS_ToInt32(ctx, &Cout,     argv[5]);
    JS_ToInt32(ctx, &kL,       argv[6]);
    JS_ToInt32(ctx, &stride,   argv[7]);
    JS_ToInt32(ctx, &padding,  argv[8]);
    JS_ToInt32(ctx, &dilation, argv[9]);
    JS_ToInt32(ctx, &groups,   argv[10]);
    GT(dX, 11, "conv1dBackwardInput");
    nngpu::conv1d_backward_input(*Wt, *dY, N, Cin, L, Cout, kL,
                                 stride, padding, dilation, groups, *dX);
    return JS_UNDEFINED;
}

static JSValue js_conv1dBackwardWeight(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 12) return JS_ThrowTypeError(ctx,
        "conv1dBackwardWeight(X,dY,N,C_in,L,C_out,kL,stride,padding,dilation,groups,dWt)");
    ENSURE_INIT();
    GT(X, 0, "conv1dBackwardWeight"); GT(dY, 1, "conv1dBackwardWeight");
    int32_t N = 0, Cin = 0, L = 0, Cout = 0, kL = 0;
    int32_t stride = 1, padding = 0, dilation = 1, groups = 1;
    JS_ToInt32(ctx, &N,        argv[2]);
    JS_ToInt32(ctx, &Cin,      argv[3]);
    JS_ToInt32(ctx, &L,        argv[4]);
    JS_ToInt32(ctx, &Cout,     argv[5]);
    JS_ToInt32(ctx, &kL,       argv[6]);
    JS_ToInt32(ctx, &stride,   argv[7]);
    JS_ToInt32(ctx, &padding,  argv[8]);
    JS_ToInt32(ctx, &dilation, argv[9]);
    JS_ToInt32(ctx, &groups,   argv[10]);
    GT(dWt, 11, "conv1dBackwardWeight");
    nngpu::conv1d_backward_weight(*X, *dY, N, Cin, L, Cout, kL,
                                  stride, padding, dilation, groups, *dWt);
    return JS_UNDEFINED;
}

static JSValue js_conv1dBackwardBias(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 5) return JS_ThrowTypeError(ctx,
        "conv1dBackwardBias(dY,N,C_out,L_out,dB)");
    ENSURE_INIT();
    GT(dY, 0, "conv1dBackwardBias");
    int32_t N = 0, Cout = 0, Lout = 0;
    JS_ToInt32(ctx, &N,    argv[1]);
    JS_ToInt32(ctx, &Cout, argv[2]);
    JS_ToInt32(ctx, &Lout, argv[3]);
    GT(dB, 4, "conv1dBackwardBias");
    nngpu::conv1d_backward_bias(*dY, N, Cout, Lout, *dB);
    return JS_UNDEFINED;
}

static JSValue js_conv1dInt8wFp16(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 14) return JS_ThrowTypeError(ctx,
        "conv1dInt8wFp16(X,W_int8,scales,bias|null,N,C_in,L,C_out,kL,stride,padding,dilation,groups,Y)");
    ENSURE_INIT();
    GT(X, 0, "conv1dInt8wFp16"); GT(W, 1, "conv1dInt8wFp16");
    GT(scales, 2, "conv1dInt8wFp16");
    JSValue err = JS_UNDEFINED;
    const nngpu::Tensor* bias = nullptr;
    if (!resolveOptionalConstGpuTensor(ctx, argv[3], bias, err, "bias")) return err;
    int32_t N = 0, Cin = 0, L = 0, Cout = 0, kL = 0;
    int32_t stride = 1, padding = 0, dilation = 1, groups = 1;
    JS_ToInt32(ctx, &N,        argv[4]);
    JS_ToInt32(ctx, &Cin,      argv[5]);
    JS_ToInt32(ctx, &L,        argv[6]);
    JS_ToInt32(ctx, &Cout,     argv[7]);
    JS_ToInt32(ctx, &kL,       argv[8]);
    JS_ToInt32(ctx, &stride,   argv[9]);
    JS_ToInt32(ctx, &padding,  argv[10]);
    JS_ToInt32(ctx, &dilation, argv[11]);
    JS_ToInt32(ctx, &groups,   argv[12]);
    GT(Y, 13, "conv1dInt8wFp16");
    nngpu::conv1d_int8w_fp16(*X, *W, *scales, bias, N, Cin, L, Cout, kL,
                             stride, padding, dilation, groups, *Y);
    return JS_UNDEFINED;
}

static JSValue js_convTranspose1dForward(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 14) return JS_ThrowTypeError(ctx,
        "convTranspose1dForward(X,Wt,bias|null,N,C_in,L,C_out,kL,stride,padding,outputPadding,dilation,groups,Y)");
    ENSURE_INIT();
    GT(X, 0, "convTranspose1dForward"); GT(Wt, 1, "convTranspose1dForward");
    JSValue err = JS_UNDEFINED;
    const nngpu::Tensor* bias = nullptr;
    if (!resolveOptionalConstGpuTensor(ctx, argv[2], bias, err, "bias")) return err;
    int32_t N = 0, Cin = 0, L = 0, Cout = 0, kL = 0;
    int32_t stride = 1, padding = 0, outPad = 0, dilation = 1, groups = 1;
    JS_ToInt32(ctx, &N,        argv[3]);
    JS_ToInt32(ctx, &Cin,      argv[4]);
    JS_ToInt32(ctx, &L,        argv[5]);
    JS_ToInt32(ctx, &Cout,     argv[6]);
    JS_ToInt32(ctx, &kL,       argv[7]);
    JS_ToInt32(ctx, &stride,   argv[8]);
    JS_ToInt32(ctx, &padding,  argv[9]);
    JS_ToInt32(ctx, &outPad,   argv[10]);
    JS_ToInt32(ctx, &dilation, argv[11]);
    JS_ToInt32(ctx, &groups,   argv[12]);
    GT(Y, 13, "convTranspose1dForward");
    nngpu::conv_transpose1d_forward(*X, *Wt, bias, N, Cin, L, Cout, kL,
                                    stride, padding, outPad, dilation, groups, *Y);
    return JS_UNDEFINED;
}

static JSValue js_convTranspose1dBackwardInput(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 13) return JS_ThrowTypeError(ctx,
        "convTranspose1dBackwardInput(Wt,dY,N,C_in,L,C_out,kL,stride,padding,outputPadding,dilation,groups,dX)");
    ENSURE_INIT();
    GT(Wt, 0, "convTranspose1dBackwardInput"); GT(dY, 1, "convTranspose1dBackwardInput");
    int32_t N = 0, Cin = 0, L = 0, Cout = 0, kL = 0;
    int32_t stride = 1, padding = 0, outPad = 0, dilation = 1, groups = 1;
    JS_ToInt32(ctx, &N,        argv[2]);
    JS_ToInt32(ctx, &Cin,      argv[3]);
    JS_ToInt32(ctx, &L,        argv[4]);
    JS_ToInt32(ctx, &Cout,     argv[5]);
    JS_ToInt32(ctx, &kL,       argv[6]);
    JS_ToInt32(ctx, &stride,   argv[7]);
    JS_ToInt32(ctx, &padding,  argv[8]);
    JS_ToInt32(ctx, &outPad,   argv[9]);
    JS_ToInt32(ctx, &dilation, argv[10]);
    JS_ToInt32(ctx, &groups,   argv[11]);
    GT(dX, 12, "convTranspose1dBackwardInput");
    nngpu::conv_transpose1d_backward_input(*Wt, *dY, N, Cin, L, Cout, kL,
                                           stride, padding, outPad, dilation,
                                           groups, *dX);
    return JS_UNDEFINED;
}

static JSValue js_convTranspose1dBackwardWeight(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 13) return JS_ThrowTypeError(ctx,
        "convTranspose1dBackwardWeight(X,dY,N,C_in,L,C_out,kL,stride,padding,outputPadding,dilation,groups,dWt)");
    ENSURE_INIT();
    GT(X, 0, "convTranspose1dBackwardWeight"); GT(dY, 1, "convTranspose1dBackwardWeight");
    int32_t N = 0, Cin = 0, L = 0, Cout = 0, kL = 0;
    int32_t stride = 1, padding = 0, outPad = 0, dilation = 1, groups = 1;
    JS_ToInt32(ctx, &N,        argv[2]);
    JS_ToInt32(ctx, &Cin,      argv[3]);
    JS_ToInt32(ctx, &L,        argv[4]);
    JS_ToInt32(ctx, &Cout,     argv[5]);
    JS_ToInt32(ctx, &kL,       argv[6]);
    JS_ToInt32(ctx, &stride,   argv[7]);
    JS_ToInt32(ctx, &padding,  argv[8]);
    JS_ToInt32(ctx, &outPad,   argv[9]);
    JS_ToInt32(ctx, &dilation, argv[10]);
    JS_ToInt32(ctx, &groups,   argv[11]);
    GT(dWt, 12, "convTranspose1dBackwardWeight");
    nngpu::conv_transpose1d_backward_weight(*X, *dY, N, Cin, L, Cout, kL,
                                            stride, padding, outPad, dilation,
                                            groups, *dWt);
    return JS_UNDEFINED;
}

static JSValue js_convTranspose1dBackwardBias(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 5) return JS_ThrowTypeError(ctx,
        "convTranspose1dBackwardBias(dY,N,C_out,L_out,dB)");
    ENSURE_INIT();
    GT(dY, 0, "convTranspose1dBackwardBias");
    int32_t N = 0, Cout = 0, Lout = 0;
    JS_ToInt32(ctx, &N,    argv[1]);
    JS_ToInt32(ctx, &Cout, argv[2]);
    JS_ToInt32(ctx, &Lout, argv[3]);
    GT(dB, 4, "convTranspose1dBackwardBias");
    nngpu::conv_transpose1d_backward_bias(*dY, N, Cout, Lout, *dB);
    return JS_UNDEFINED;
}

// causalConv1d(X,Wt,bias|null,N,C_in,L,C_out,kL,stride,dilation,groups,scratch,Y)
// `scratch` is a caller-owned GpuTensor reused as the left-padded-input buffer.
static JSValue js_causalConv1d(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 13) return JS_ThrowTypeError(ctx,
        "causalConv1d(X,Wt,bias|null,N,C_in,L,C_out,kL,stride,dilation,groups,scratch,Y)");
    ENSURE_INIT();
    GT(X, 0, "causalConv1d"); GT(Wt, 1, "causalConv1d");
    JSValue err = JS_UNDEFINED;
    const nngpu::Tensor* bias = nullptr;
    if (!resolveOptionalConstGpuTensor(ctx, argv[2], bias, err, "bias")) return err;
    int32_t N = 0, Cin = 0, L = 0, Cout = 0, kL = 0;
    int32_t stride = 1, dilation = 1, groups = 1;
    JS_ToInt32(ctx, &N,        argv[3]);
    JS_ToInt32(ctx, &Cin,      argv[4]);
    JS_ToInt32(ctx, &L,        argv[5]);
    JS_ToInt32(ctx, &Cout,     argv[6]);
    JS_ToInt32(ctx, &kL,       argv[7]);
    JS_ToInt32(ctx, &stride,   argv[8]);
    JS_ToInt32(ctx, &dilation, argv[9]);
    JS_ToInt32(ctx, &groups,   argv[10]);
    GT(scratch, 11, "causalConv1d");
    GT(Y, 12, "causalConv1d");
    nngpu::causal_conv1d(*X, *Wt, bias, N, Cin, L, Cout, kL,
                         stride, dilation, groups, *scratch, *Y);
    return JS_UNDEFINED;
}

// causalConv1dUpdate(X,Wt,bias|null,N,C,L_step,kL,dilation,state,Y)
// `state` is the rolling (kL-1)*dilation-sample history — read AND overwritten.
static JSValue js_causalConv1dUpdate(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 10) return JS_ThrowTypeError(ctx,
        "causalConv1dUpdate(X,Wt,bias|null,N,C,L_step,kL,dilation,state,Y)");
    ENSURE_INIT();
    GT(X, 0, "causalConv1dUpdate"); GT(Wt, 1, "causalConv1dUpdate");
    JSValue err = JS_UNDEFINED;
    const nngpu::Tensor* bias = nullptr;
    if (!resolveOptionalConstGpuTensor(ctx, argv[2], bias, err, "bias")) return err;
    int32_t N = 0, C = 0, Lstep = 0, kL = 0, dilation = 1;
    JS_ToInt32(ctx, &N,        argv[3]);
    JS_ToInt32(ctx, &C,        argv[4]);
    JS_ToInt32(ctx, &Lstep,    argv[5]);
    JS_ToInt32(ctx, &kL,       argv[6]);
    JS_ToInt32(ctx, &dilation, argv[7]);
    GT(state, 8, "causalConv1dUpdate");
    GT(Y, 9, "causalConv1dUpdate");
    nngpu::causal_conv1d_update(*X, *Wt, bias, N, C, Lstep, kL, dilation,
                                *state, *Y);
    return JS_UNDEFINED;
}

// ─── Vocoder / codec activations ──────────────────────────────────────────
//
// NCL layout. snake carries per-channel learnable alpha (and optional beta);
// elu / leaky_relu are plain elementwise maps with a scalar parameter.

static JSValue js_snakeForward(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 7) return JS_ThrowTypeError(ctx,
        "snakeForward(X,alpha,beta|null,N,C,L,Y)");
    ENSURE_INIT();
    GT(X, 0, "snakeForward"); GT(alpha, 1, "snakeForward");
    JSValue err = JS_UNDEFINED;
    const nngpu::Tensor* beta = nullptr;
    if (!resolveOptionalConstGpuTensor(ctx, argv[2], beta, err, "beta")) return err;
    int32_t N = 0, C = 0, L = 0;
    JS_ToInt32(ctx, &N, argv[3]);
    JS_ToInt32(ctx, &C, argv[4]);
    JS_ToInt32(ctx, &L, argv[5]);
    GT(Y, 6, "snakeForward");
    nngpu::snake_forward(*X, *alpha, beta, N, C, L, *Y);
    return JS_UNDEFINED;
}

// snakeBackward(X,alpha,beta|null,dY,N,C,L,dX,dAlpha,dBeta|null)
// dBeta must be non-null exactly when beta is non-null.
static JSValue js_snakeBackward(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 10) return JS_ThrowTypeError(ctx,
        "snakeBackward(X,alpha,beta|null,dY,N,C,L,dX,dAlpha,dBeta|null)");
    ENSURE_INIT();
    GT(X, 0, "snakeBackward"); GT(alpha, 1, "snakeBackward");
    JSValue err = JS_UNDEFINED;
    const nngpu::Tensor* beta = nullptr;
    if (!resolveOptionalConstGpuTensor(ctx, argv[2], beta, err, "beta")) return err;
    GT(dY, 3, "snakeBackward");
    int32_t N = 0, C = 0, L = 0;
    JS_ToInt32(ctx, &N, argv[4]);
    JS_ToInt32(ctx, &C, argv[5]);
    JS_ToInt32(ctx, &L, argv[6]);
    GT(dX, 7, "snakeBackward"); GT(dAlpha, 8, "snakeBackward");
    nngpu::Tensor* dBeta = nullptr;
    if (!resolveOptionalGpuTensor(ctx, argv[9], dBeta, err, "dBeta")) return err;
    nngpu::snake_backward(*X, *alpha, beta, *dY, N, C, L, *dX, *dAlpha, dBeta);
    return JS_UNDEFINED;
}

static JSValue js_eluForward(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 3) return JS_ThrowTypeError(ctx, "eluForward(x,alpha,y)");
    ENSURE_INIT();
    GT(x, 0, "eluForward");
    double alpha = 1.0; JS_ToFloat64(ctx, &alpha, argv[1]);
    GT(y, 2, "eluForward");
    nngpu::elu_forward(*x, (float)alpha, *y);
    return JS_UNDEFINED;
}

static JSValue js_eluBackward(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 4) return JS_ThrowTypeError(ctx, "eluBackward(x,dY,alpha,dX)");
    ENSURE_INIT();
    GT(x, 0, "eluBackward"); GT(dY, 1, "eluBackward");
    double alpha = 1.0; JS_ToFloat64(ctx, &alpha, argv[2]);
    GT(dX, 3, "eluBackward");
    nngpu::elu_backward(*x, *dY, (float)alpha, *dX);
    return JS_UNDEFINED;
}

static JSValue js_leakyReluForward(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 3) return JS_ThrowTypeError(ctx, "leakyReluForward(x,negativeSlope,y)");
    ENSURE_INIT();
    GT(x, 0, "leakyReluForward");
    double slope = 0.01; JS_ToFloat64(ctx, &slope, argv[1]);
    GT(y, 2, "leakyReluForward");
    nngpu::leaky_relu_forward(*x, (float)slope, *y);
    return JS_UNDEFINED;
}

static JSValue js_leakyReluBackward(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 4) return JS_ThrowTypeError(ctx, "leakyReluBackward(x,dY,negativeSlope,dX)");
    ENSURE_INIT();
    GT(x, 0, "leakyReluBackward"); GT(dY, 1, "leakyReluBackward");
    double slope = 0.01; JS_ToFloat64(ctx, &slope, argv[2]);
    GT(dX, 3, "leakyReluBackward");
    nngpu::leaky_relu_backward(*x, *dY, (float)slope, *dX);
    return JS_UNDEFINED;
}

// ─── Codec quantization ───────────────────────────────────────────────────
//
// VQ-VAE residual-VQ and FSQ bottlenecks. The straight-through estimator
// makes both backward ops a plain identity passthrough.

static JSValue js_vqEncodeForward(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 4) return JS_ThrowTypeError(ctx,
        "vqEncodeForward(x,codebook,indices,quantized)");
    ENSURE_INIT();
    GT(x, 0, "vqEncodeForward"); GT(codebook, 1, "vqEncodeForward");
    GT(indices, 2, "vqEncodeForward"); GT(quantized, 3, "vqEncodeForward");
    nngpu::vq_encode_forward(*x, *codebook, *indices, *quantized);
    return JS_UNDEFINED;
}

static JSValue js_vqEncodeBackward(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 2) return JS_ThrowTypeError(ctx, "vqEncodeBackward(dQuantized,dX)");
    ENSURE_INIT();
    GT(dQuantized, 0, "vqEncodeBackward"); GT(dX, 1, "vqEncodeBackward");
    nngpu::vq_encode_backward(*dQuantized, *dX);
    return JS_UNDEFINED;
}

static JSValue js_fsqQuantizeForward(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 4) return JS_ThrowTypeError(ctx,
        "fsqQuantizeForward(x,levels,quantized,packedIndices)");
    ENSURE_INIT();
    GT(x, 0, "fsqQuantizeForward"); GT(levels, 1, "fsqQuantizeForward");
    GT(quantized, 2, "fsqQuantizeForward"); GT(packed, 3, "fsqQuantizeForward");
    nngpu::fsq_quantize_forward(*x, *levels, *quantized, *packed);
    return JS_UNDEFINED;
}

static JSValue js_fsqQuantizeBackward(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 2) return JS_ThrowTypeError(ctx, "fsqQuantizeBackward(dQuantized,dX)");
    ENSURE_INIT();
    GT(dQuantized, 0, "fsqQuantizeBackward"); GT(dX, 1, "fsqQuantizeBackward");
    nngpu::fsq_quantize_backward(*dQuantized, *dX);
    return JS_UNDEFINED;
}

// ─── 1D resampling ────────────────────────────────────────────────────────
//
// Arbitrary-scale resampling along the length axis of an NCL audio tensor.
// mode: 0 = nearest, 1 = linear.

static JSValue js_resample1dForward(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 7) return JS_ThrowTypeError(ctx,
        "resample1dForward(X,N,C,L_in,L_out,mode,Y)");
    ENSURE_INIT();
    GT(X, 0, "resample1dForward");
    int32_t N = 0, C = 0, Lin = 0, Lout = 0, mode = 0;
    JS_ToInt32(ctx, &N,    argv[1]);
    JS_ToInt32(ctx, &C,    argv[2]);
    JS_ToInt32(ctx, &Lin,  argv[3]);
    JS_ToInt32(ctx, &Lout, argv[4]);
    JS_ToInt32(ctx, &mode, argv[5]);
    GT(Y, 6, "resample1dForward");
    nngpu::resample1d_forward(*X, N, C, Lin, Lout, mode, *Y);
    return JS_UNDEFINED;
}

static JSValue js_resample1dBackward(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 7) return JS_ThrowTypeError(ctx,
        "resample1dBackward(dY,N,C,L_in,L_out,mode,dX)");
    ENSURE_INIT();
    GT(dY, 0, "resample1dBackward");
    int32_t N = 0, C = 0, Lin = 0, Lout = 0, mode = 0;
    JS_ToInt32(ctx, &N,    argv[1]);
    JS_ToInt32(ctx, &C,    argv[2]);
    JS_ToInt32(ctx, &Lin,  argv[3]);
    JS_ToInt32(ctx, &Lout, argv[4]);
    JS_ToInt32(ctx, &mode, argv[5]);
    GT(dX, 6, "resample1dBackward");
    nngpu::resample1d_backward(*dY, N, C, Lin, Lout, mode, *dX);
    return JS_UNDEFINED;
}

// ─── log / exp / round elementwise ────────────────────────────────────────
//
// FP32 elementwise scalar maps. log/exp backward read the raw forward input;
// round backward is the STE identity and needs only dY.

#define ELEMENTWISE_FB(jsName, fwdFn, bwdFn)                                    \
    static JSValue js_##jsName##Forward(JSContext* ctx, JSValueConst, int argc, \
                                        JSValueConst* argv) {                   \
        if (argc < 2) return JS_ThrowTypeError(ctx, #jsName "Forward(x,y)");     \
        ENSURE_INIT();                                                          \
        GT(x, 0, #jsName "Forward"); GT(y, 1, #jsName "Forward");                \
        nngpu::fwdFn(*x, *y);                                                   \
        return JS_UNDEFINED;                                                    \
    }                                                                           \
    static JSValue js_##jsName##Backward(JSContext* ctx, JSValueConst, int argc,\
                                         JSValueConst* argv) {                  \
        if (argc < 3) return JS_ThrowTypeError(ctx, #jsName "Backward(x,dY,dX)");\
        ENSURE_INIT();                                                          \
        GT(x, 0, #jsName "Backward"); GT(dY, 1, #jsName "Backward");             \
        GT(dX, 2, #jsName "Backward");                                          \
        nngpu::bwdFn(*x, *dY, *dX);                                             \
        return JS_UNDEFINED;                                                    \
    }

ELEMENTWISE_FB(log, log_forward, log_backward)
ELEMENTWISE_FB(exp, exp_forward, exp_backward)

#undef ELEMENTWISE_FB

static JSValue js_roundForward(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 2) return JS_ThrowTypeError(ctx, "roundForward(x,y)");
    ENSURE_INIT();
    GT(x, 0, "roundForward"); GT(y, 1, "roundForward");
    nngpu::round_forward(*x, *y);
    return JS_UNDEFINED;
}

// round backward is the straight-through estimator: dX = dY (no x needed).
static JSValue js_roundBackward(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 2) return JS_ThrowTypeError(ctx, "roundBackward(dY,dX)");
    ENSURE_INIT();
    GT(dY, 0, "roundBackward"); GT(dX, 1, "roundBackward");
    nngpu::round_backward(*dY, *dX);
    return JS_UNDEFINED;
}

// ─── Autoregressive logit sampling ────────────────────────────────────────
//
// Per-row next-token sampler (temperature / top-k / top-p, Philox RNG).
// brotensor has no CUDA kernel for this op — the CUDA vtable slot is absent,
// so on a CUDA build the binding throws a clear error rather than dispatching
// into an undefined slot (which crashes the process). Runs on Metal.
//
// sampleLogits(logits, temperature, topK, topP, key, counter, indices)
//   key / counter seed the Philox 4x32-10 generator; passed as plain numbers.
static JSValue js_sampleLogits(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
#ifdef BROTENSOR_HAS_CUDA
    return JS_ThrowTypeError(ctx,
        "sampleLogits: brotensor has no CUDA kernel for sample_logits "
        "(available on the Metal backend only)");
#else
    if (argc < 7) return JS_ThrowTypeError(ctx,
        "sampleLogits(logits,temperature,topK,topP,key,counter,indices)");
    ENSURE_INIT();
    GT(logits, 0, "sampleLogits");
    double temperature = 1.0, topP = 1.0;
    int32_t topK = 0;
    int64_t key = 0, counter = 0;
    JS_ToFloat64(ctx, &temperature, argv[1]);
    JS_ToInt32(ctx, &topK, argv[2]);
    JS_ToFloat64(ctx, &topP, argv[3]);
    JS_ToInt64(ctx, &key, argv[4]);
    JS_ToInt64(ctx, &counter, argv[5]);
    GT(indices, 6, "sampleLogits");
    nngpu::sample_logits(*logits, (float)temperature, topK, (float)topP,
                         (uint64_t)key, (uint64_t)counter, *indices);
    return JS_UNDEFINED;
#endif // BROTENSOR_HAS_CUDA
}

#undef GT
#undef ENSURE_INIT

void installTensorAudioOps(JSContext* ctx, JSValue gpuObj) {
    // Spectral / FFT core
    JS_SetPropertyStr(ctx, gpuObj, "fft",                JS_NewCFunction(ctx, js_fft,                "fft",                2));
    JS_SetPropertyStr(ctx, gpuObj, "ifft",               JS_NewCFunction(ctx, js_ifft,               "ifft",               2));
    JS_SetPropertyStr(ctx, gpuObj, "rfft",               JS_NewCFunction(ctx, js_rfft,               "rfft",               2));
    JS_SetPropertyStr(ctx, gpuObj, "irfft",              JS_NewCFunction(ctx, js_irfft,              "irfft",              3));
    JS_SetPropertyStr(ctx, gpuObj, "rfftBackward",       JS_NewCFunction(ctx, js_rfftBackward,       "rfftBackward",       3));
    JS_SetPropertyStr(ctx, gpuObj, "irfftBackward",      JS_NewCFunction(ctx, js_irfftBackward,      "irfftBackward",      2));
    JS_SetPropertyStr(ctx, gpuObj, "complexMul",         JS_NewCFunction(ctx, js_complexMul,         "complexMul",         3));
    JS_SetPropertyStr(ctx, gpuObj, "complexMulBackward", JS_NewCFunction(ctx, js_complexMulBackward, "complexMulBackward", 5));
    JS_SetPropertyStr(ctx, gpuObj, "complexAbs",         JS_NewCFunction(ctx, js_complexAbs,         "complexAbs",         2));
    JS_SetPropertyStr(ctx, gpuObj, "complexAbsBackward", JS_NewCFunction(ctx, js_complexAbsBackward, "complexAbsBackward", 3));
    JS_SetPropertyStr(ctx, gpuObj, "complexAngle",       JS_NewCFunction(ctx, js_complexAngle,       "complexAngle",       2));
    JS_SetPropertyStr(ctx, gpuObj, "complexFromPolar",   JS_NewCFunction(ctx, js_complexFromPolar,   "complexFromPolar",   3));

    // STFT / iSTFT
    JS_SetPropertyStr(ctx, gpuObj, "stft",          JS_NewCFunction(ctx, js_stft,          "stft",          9));
    JS_SetPropertyStr(ctx, gpuObj, "stftBackward",  JS_NewCFunction(ctx, js_stftBackward,  "stftBackward",  10));
    JS_SetPropertyStr(ctx, gpuObj, "istft",         JS_NewCFunction(ctx, js_istft,         "istft",         10));
    JS_SetPropertyStr(ctx, gpuObj, "istftBackward", JS_NewCFunction(ctx, js_istftBackward, "istftBackward", 10));

    // 1D convolution family
    JS_SetPropertyStr(ctx, gpuObj, "pad1dForward",                   JS_NewCFunction(ctx, js_pad1dForward,                   "pad1dForward",                   8));
    JS_SetPropertyStr(ctx, gpuObj, "pad1dBackward",                  JS_NewCFunction(ctx, js_pad1dBackward,                  "pad1dBackward",                  8));
    JS_SetPropertyStr(ctx, gpuObj, "conv1d",                         JS_NewCFunction(ctx, js_conv1d,                         "conv1d",                         13));
    JS_SetPropertyStr(ctx, gpuObj, "conv1dBackwardInput",            JS_NewCFunction(ctx, js_conv1dBackwardInput,            "conv1dBackwardInput",            12));
    JS_SetPropertyStr(ctx, gpuObj, "conv1dBackwardWeight",           JS_NewCFunction(ctx, js_conv1dBackwardWeight,           "conv1dBackwardWeight",           12));
    JS_SetPropertyStr(ctx, gpuObj, "conv1dBackwardBias",             JS_NewCFunction(ctx, js_conv1dBackwardBias,             "conv1dBackwardBias",             5));
    JS_SetPropertyStr(ctx, gpuObj, "conv1dInt8wFp16",                JS_NewCFunction(ctx, js_conv1dInt8wFp16,                "conv1dInt8wFp16",                14));
    JS_SetPropertyStr(ctx, gpuObj, "convTranspose1dForward",         JS_NewCFunction(ctx, js_convTranspose1dForward,         "convTranspose1dForward",         14));
    JS_SetPropertyStr(ctx, gpuObj, "convTranspose1dBackwardInput",   JS_NewCFunction(ctx, js_convTranspose1dBackwardInput,   "convTranspose1dBackwardInput",   13));
    JS_SetPropertyStr(ctx, gpuObj, "convTranspose1dBackwardWeight",  JS_NewCFunction(ctx, js_convTranspose1dBackwardWeight,  "convTranspose1dBackwardWeight",  13));
    JS_SetPropertyStr(ctx, gpuObj, "convTranspose1dBackwardBias",    JS_NewCFunction(ctx, js_convTranspose1dBackwardBias,    "convTranspose1dBackwardBias",    5));
    JS_SetPropertyStr(ctx, gpuObj, "causalConv1d",                   JS_NewCFunction(ctx, js_causalConv1d,                   "causalConv1d",                   13));
    JS_SetPropertyStr(ctx, gpuObj, "causalConv1dUpdate",             JS_NewCFunction(ctx, js_causalConv1dUpdate,             "causalConv1dUpdate",             10));

    // Vocoder / codec activations
    JS_SetPropertyStr(ctx, gpuObj, "snakeForward",      JS_NewCFunction(ctx, js_snakeForward,      "snakeForward",      7));
    JS_SetPropertyStr(ctx, gpuObj, "snakeBackward",     JS_NewCFunction(ctx, js_snakeBackward,     "snakeBackward",     10));
    JS_SetPropertyStr(ctx, gpuObj, "eluForward",        JS_NewCFunction(ctx, js_eluForward,        "eluForward",        3));
    JS_SetPropertyStr(ctx, gpuObj, "eluBackward",       JS_NewCFunction(ctx, js_eluBackward,       "eluBackward",       4));
    JS_SetPropertyStr(ctx, gpuObj, "leakyReluForward",  JS_NewCFunction(ctx, js_leakyReluForward,  "leakyReluForward",  3));
    JS_SetPropertyStr(ctx, gpuObj, "leakyReluBackward", JS_NewCFunction(ctx, js_leakyReluBackward, "leakyReluBackward", 4));

    // Codec quantization
    JS_SetPropertyStr(ctx, gpuObj, "vqEncodeForward",     JS_NewCFunction(ctx, js_vqEncodeForward,     "vqEncodeForward",     4));
    JS_SetPropertyStr(ctx, gpuObj, "vqEncodeBackward",    JS_NewCFunction(ctx, js_vqEncodeBackward,    "vqEncodeBackward",    2));
    JS_SetPropertyStr(ctx, gpuObj, "fsqQuantizeForward",  JS_NewCFunction(ctx, js_fsqQuantizeForward,  "fsqQuantizeForward",  4));
    JS_SetPropertyStr(ctx, gpuObj, "fsqQuantizeBackward", JS_NewCFunction(ctx, js_fsqQuantizeBackward, "fsqQuantizeBackward", 2));

    // 1D resampling
    JS_SetPropertyStr(ctx, gpuObj, "resample1dForward",  JS_NewCFunction(ctx, js_resample1dForward,  "resample1dForward",  7));
    JS_SetPropertyStr(ctx, gpuObj, "resample1dBackward", JS_NewCFunction(ctx, js_resample1dBackward, "resample1dBackward", 7));

    // log / exp / round elementwise
    JS_SetPropertyStr(ctx, gpuObj, "logForward",    JS_NewCFunction(ctx, js_logForward,    "logForward",    2));
    JS_SetPropertyStr(ctx, gpuObj, "logBackward",   JS_NewCFunction(ctx, js_logBackward,   "logBackward",   3));
    JS_SetPropertyStr(ctx, gpuObj, "expForward",    JS_NewCFunction(ctx, js_expForward,    "expForward",    2));
    JS_SetPropertyStr(ctx, gpuObj, "expBackward",   JS_NewCFunction(ctx, js_expBackward,   "expBackward",   3));
    JS_SetPropertyStr(ctx, gpuObj, "roundForward",  JS_NewCFunction(ctx, js_roundForward,  "roundForward",  2));
    JS_SetPropertyStr(ctx, gpuObj, "roundBackward", JS_NewCFunction(ctx, js_roundBackward, "roundBackward", 2));

    // Autoregressive logit sampling
    JS_SetPropertyStr(ctx, gpuObj, "sampleLogits", JS_NewCFunction(ctx, js_sampleLogits, "sampleLogits", 7));
}

} // namespace bro::js

#else // !BROTENSOR_HAS_GPU

#include <qjsbind/qjsbind.h>
namespace bro::js {
void installTensorAudioOps(JSContext*, JSValue) {}
} // namespace bro::js

#endif // BROTENSOR_HAS_GPU
