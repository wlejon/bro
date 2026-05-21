// =============================================================================
// bro.tensor — GPU tensor + ops (brotensor: CUDA or Metal)
// =============================================================================
//
// Wraps the brotensor sibling library. brotensor exposes one unified tensor
// type with a runtime Device tag and device-neutral ops; bro.tensor is the
// GPU-resident face of it. The op surface is identical across CUDA (NVIDIA)
// and Metal (Apple) backends — code written against bro.tensor runs unchanged
// on either. Mirrors brotensor's CPU op surface over device-resident tensor
// buffers, plus a broad set of training- and inference-only ops for
// transformers, diffusion U-Nets, and W8A16 quant.
//
// Availability:
//   bro.tensor.available  // boolean
//   bro.tensor.backend    // "cuda" | "metal" | "unknown" (only when available)
//
// `available` is true when bro was built against a brotensor with a GPU
// backend enabled (-DBROGAMEAGENT_WITH_CUDA=ON or _WITH_METAL=ON). When
// false, only `available` is exposed — guard everything else behind the
// `available` check.
//
// Dtypes:
//   bro.tensor.dtype.fp32   // 0
//   bro.tensor.dtype.fp16   // 1
//   bro.tensor.dtype.int8   // 2
//
// Tensors carry a dtype tag. Arithmetic ops dispatch on the input dtype
// (FP32 or FP16); FP16 ops accumulate internally in FP32. INT8 is only
// carried by weight-only quantised ops. createTensor() / .resize() accept
// either the string ("fp32" | "fp16" | "int8") or the numeric enum.
//
// Synchronisation:
//   Every op runs on the default stream and is asynchronous on the GPU.
//   Before reading results back to host, call bro.tensor.sync() or use
//   .download(), which auto-syncs internally.
//
// Mask convention:
//   Wherever the CPU API takes a Float32Array mask (host pointer), the GPU
//   API takes a *device* mask: pass either `null`/`undefined` (no mask) OR
//   a GpuTensor whose `.data` is the device pointer. Passing a Float32Array
//   throws TypeError.
//
// Int32 indices (embedding):
//   `idx` arguments are GpuTensors whose `.data` is reinterpreted as an
//   int32_t* device buffer (size == B). This keeps indices resident on
//   device across calls.
//
// =============================================================================


const gpu = bro.tensor;

// -----------------------------------------------------------------------------
// Runtime
// -----------------------------------------------------------------------------

/**
 * `true` when a GPU backend is compiled in. Always check before accessing
 * other `gpu.*` symbols.
 * @type {boolean}
 */
gpu.available;

/** "cuda" | "metal" | "unknown" — the active backend identifier. */
gpu.backend;

/**
 * Idempotent device init. CUDA: selects device 0 (or BROTENSOR_CUDA_DEVICE).
 * Metal: opens the default MTLDevice. Most ops auto-init; call once at
 * startup to surface device errors early.
 */
gpu.init();

/** Block until all queued kernels on the default stream have completed. */
gpu.sync();


// -----------------------------------------------------------------------------
// GpuTensor
// -----------------------------------------------------------------------------

/**
 * Allocate an owning device tensor. dtype defaults to FP32. Storage is
 * uninitialised — call `.zero()` if you need zeros.
 * @param {number} rows
 * @param {number} [cols=1]
 * @param {string|number} [dtype="fp32"]  "fp32" | "fp16" | "int8" or enum
 * @returns {GpuTensor}
 */
const t   = gpu.createTensor(3, 4);
const t16 = gpu.createTensor(3, 4, "fp16");
const q8  = gpu.createTensor(8, 16, "int8");

t.rows;          // 3
t.cols;          // 4
t.size;          // 12
t.bytes;         // 48  (3*4*sizeof(fp32))
t.dtype();       // "fp32"

t.zero();                       // device memset to 0
t.resize(2, 6);                 // reallocates, default fp32
t.resize(2, 6, "fp16");         // reallocates AND switches dtype to FP16
const dup = t.clone();          // owning device-side copy (same dtype)

/**
 * Upload host → device (FP32).
 * @param {AITensor|Float32Array} src
 */
t.upload(new Float32Array([1, 2, 3, 4, 5, 6]));

/**
 * Download device → host (FP32). Auto-syncs.
 *   download()       → returns a fresh Float32Array
 *   download(dst)    → copies into the AITensor `dst` (resizes if needed)
 */
const arr = t.download();
const ht  = bro.ai.game.nn.createTensor(t.rows, t.cols);
t.download(ht);

/**
 * FP16 upload / download — pass a Uint16Array holding the IEEE binary16 bit
 * pattern. Use this when staging diffusion-style FP16 weights.
 *
 *   t16.uploadFp16(uint16Array);    // resizes destination to (n, 1) FP16
 *   const u16 = t16.downloadFp16(); // returns fresh Uint16Array (bit pattern)
 */

/**
 * INT8 upload — pass an Int8Array of raw int8 weight bytes, typically the
 * `weights` field of quantizeInt8PerRowHost(). This is the staging path for
 * W8A16 weights: the only way to get quantised weights onto the device.
 *
 *   q8.uploadInt8(quant.weights);   // resizes destination to (rows, cols) INT8
 *
 * Keeps the destination's existing (rows, cols) when their product matches
 * the element count; otherwise falls back to (n, 1). Pair the resulting
 * tensor with an FP32 (out, 1) scales tensor for the matmulInt8wFp16 family.
 */


// -----------------------------------------------------------------------------
// Dense + elementwise
// -----------------------------------------------------------------------------

gpu.linearForward(W, b, x, y);              // y = W*x + b
gpu.linearBackward(W, x, dY, dX, dW, dB);   // dW, dB accumulated; dX overwritten

gpu.reluForward(x, y);     gpu.reluBackward(x, dY, dX);
gpu.tanhForward(x, y);     gpu.tanhBackward(y, dY, dX);    // uses cached y
gpu.sigmoidForward(x, y);  gpu.sigmoidBackward(y, dY, dX); // uses cached y

gpu.addInplace(y, x);            // y[i] += x[i]
gpu.addScalarInplace(y, 0.5);    // y[i] += s
gpu.scaleInplace(y, 2.0);        // y[i] *= s
gpu.mulInplace(y, x);            // y[i] *= x[i]   (FP32/FP16)
gpu.clamp(y, -1.0, 1.0);         // y[i] = clip(y[i], lo, hi)

/**
 * Build a slot-validity mask on-device:
 *   mask[k] = (x[offset + k*stride] > 0.5) ? 1 : 0   for k in [0, K)
 * Resizes mask to (K, 1). Used to avoid host syncs when constructing
 * per-slot validity masks.
 */
gpu.buildSlotMask(x, offset, K, stride, mask);

/**
 * Device-to-device chunk copy: copies `n` flat floats from src starting at
 * `srcOff` into dst starting at `dstOff`. Treats both tensors as flat
 * buffers regardless of shape.
 */
gpu.copyD2D(src, srcOff, dst, dstOff, n);

/**
 * Dtype cast: dst = src converted to outDtype. dst is resized (and dtype-set)
 * to match src's shape, on src's device. Supports the FP32 <-> FP16 pair plus
 * a same-dtype passthrough copy; other pairs throw. The standard
 * mixed-precision primitive (FP16 weight <-> FP32 master copy).
 *   outDtype: "fp32" | "fp16" | "int8" or a bro.tensor.dtype.* enum value.
 */
gpu.cast(src, dst, "fp16");


// -----------------------------------------------------------------------------
// Modern activations (transformer + diffusion stack)
// -----------------------------------------------------------------------------
//
// All forward kernels take (x, y); all backward kernels take (x, dY, dX) and
// read the *raw forward input x*, not the cached forward output y. FP32 and
// FP16 are dispatched on x.dtype.

gpu.siluForward(x, y);          gpu.siluBackward(x, dY, dX);    // x * sigmoid(x)
gpu.geluForward(x, y);          gpu.geluBackward(x, dY, dX);    // tanh approx
gpu.geluExactForward(x, y);     gpu.geluExactBackward(x, dY, dX); // erf-based
gpu.quickGeluForward(x, y);     gpu.quickGeluBackward(x, dY, dX); // x * sigmoid(1.702 x)

// Gated FFN activations. Input shape (B, 2*D) split into halves A, B_half;
// output (B, D). swiglu: silu(A) * B_half. geglu: A * gelu(B_half). Same
// API for the exact-erf GEGLU.
gpu.swigluForward(X, Y);        gpu.swigluBackward(X, dY, dX);
gpu.gegluForward(X, Y);         gpu.gegluBackward(X, dY, dX);
gpu.gegluExactForward(X, Y);    gpu.gegluExactBackward(X, dY, dX);


// -----------------------------------------------------------------------------
// Softmax
// -----------------------------------------------------------------------------

gpu.softmaxForward(logits, probs, /*mask|null*/ null);
gpu.softmaxBackward(probs, dProbs, dLogits);  // full Jacobian


// -----------------------------------------------------------------------------
// LayerNorm + batched-inference LayerNorm
// -----------------------------------------------------------------------------

/**
 * Single-vector LayerNorm. Returns the mean/rstd scalars for the backward
 * to consume without recomputation.
 */
const ln = gpu.layernormForward(x, gamma, beta, y, xhat, 1e-5);
gpu.layernormBackward(dY, xhat, gamma, ln.rstd, dX, dGamma, dBeta);

/**
 * Inference-only batched LayerNorm (no cache, no sync). One block per row.
 *   X_RD:  (R, D)
 *   gamma: (D,)
 *   beta:  (D,)
 *   Y_RD:  (R, D)  — resized if mis-shaped
 */
gpu.layernormForwardInferenceBatched(X_RD, gamma, beta, Y_RD, 1e-5);
gpu.layernormForwardInferenceBatchedFp16(X_RD, gamma, beta, Y_RD, 1e-5);


// -----------------------------------------------------------------------------
// RMSNorm (Llama-style)
// -----------------------------------------------------------------------------

/**
 * Per-row: rms[b] = sqrt(mean_j x[b,j]^2 + eps); y[b,j] = x[b,j]*gamma[j]/rms[b]
 *   X:     (B, D)
 *   gamma: (D, 1)
 *   Y:     (B, D), resized + dtype-matched
 */
gpu.rmsNormForward(X, gamma, 1e-5, Y);
gpu.rmsNormBackward(X, gamma, dY, 1e-5, dX, dGamma);  // dGamma accumulated


// -----------------------------------------------------------------------------
// GroupNorm (diffusion / vision)
// -----------------------------------------------------------------------------

/**
 * NCHW GroupNorm. numGroups must divide C. Mean/var computed over
 * (C/numGroups, H, W) per (n, group) tile. FP32 and FP16 dispatched on
 * X.dtype.
 */
gpu.groupNormForward(X, gamma, beta, N, C, H, W, numGroups, 1e-5, Y);
gpu.groupNormBackward(X, gamma, dY, N, C, H, W, numGroups, 1e-5,
                      dX, dGamma, dBeta);  // dGamma/dBeta accumulated


// -----------------------------------------------------------------------------
// Matmul (raw, no bias)
// -----------------------------------------------------------------------------

/** C(M,N) = A(M,K) @ B(K,N). Dtype dispatched on A.dtype. */
gpu.matmul(A, B, C);

/**
 * Backward. dA, dB are *accumulated* into (caller zeros). dC is read-only.
 *   dA += dC @ B^T   ;   dB += A^T @ dC
 */
gpu.matmulBackward(A, B, dC, dA, dB);


// -----------------------------------------------------------------------------
// RoPE (rotary position embedding)
// -----------------------------------------------------------------------------

/**
 * Per-head pair rotation:
 *   x_{2i}   ←  x_{2i}   * cos(θ) - x_{2i+1} * sin(θ)
 *   x_{2i+1} ←  x_{2i}   * sin(θ) + x_{2i+1} * cos(θ)
 *   θ = pos * thetaBase^(-2i/headDim);  pos = seqOffset + row.
 *
 *   X / Y / dY / dX: (L, numHeads * headDim)
 *   headDim must be even.
 */
gpu.ropeForward(X, headDim, numHeads, seqOffset, 10000.0, Y);
gpu.ropeBackward(dY, headDim, numHeads, seqOffset, 10000.0, dX);


// -----------------------------------------------------------------------------
// Reductions
// -----------------------------------------------------------------------------

gpu.sumRows(X, Y);     // Y(M,1) = sum_n X[m, n]
gpu.sumCols(X, Y);     // Y(1,N) = sum_m X[m, n]
gpu.argmaxRows(X, Idx); // Idx(M,1) FP32; integer index stored as float


// -----------------------------------------------------------------------------
// Single-head + multi-head attention
// -----------------------------------------------------------------------------

/**
 * Single-head SDPA self-attention. Caches Q/K/V/Attn/Y_pre_Wo are filled by
 * the forward so the backward needs no recomputation.
 */
gpu.attentionForward(X, Wq, Wk, Wv, Wo, mask, Q, K, V, Attn, Y_pre_Wo, O);
gpu.attentionBackward(dO, X, Q, K, V, Attn, Y_pre_Wo,
                      Wq, Wk, Wv, Wo, mask,
                      dX, dWq, dWk, dWv, dWo);

/**
 * Multi-head self-attention. Wq/Wk/Wv/Wo are (D,D); split into h heads of
 * head_dim = D/h. Caches Qh/Kh/Vh: (h*K, head_dim); Attnh: (h*K, K);
 * Yconcat: (K, D); O: (K, D).
 */
gpu.mhaForward(X, Wq, Wk, Wv, Wo, mask, numHeads,
               Qh, Kh, Vh, Attnh, Yconcat, O);
gpu.mhaBackward(dO, X, Qh, Kh, Vh, Attnh, Yconcat,
                Wq, Wk, Wv, Wo, mask, numHeads,
                dX, dWq, dWk, dWv, dWo);


// -----------------------------------------------------------------------------
// Self / cross attention (training-aware variants)
// -----------------------------------------------------------------------------

/** Inference self-attention (no caches exposed). FP16 path via flash-attn. */
gpu.selfAttentionForward(X, Wq, Wk, Wv, Wo, mask, numHeads, O);

/** FP32 training self-attention with caches (thin wrapper over mhaForward). */
gpu.selfAttentionForwardTrain(X, Wq, Wk, Wv, Wo, mask, numHeads,
                              Qh, Kh, Vh, Attnh, Yconcat, O);
gpu.selfAttentionBackward(dO, X, Qh, Kh, Vh, Attnh, Yconcat,
                          Wq, Wk, Wv, Wo, mask, numHeads,
                          dX, dWq, dWk, dWv, dWo);

/**
 * Cross-attention: K/V projected from a separate context tensor (used in
 * diffusion U-Nets for text conditioning). Wk/Wv may be rectangular (D, D_ctx).
 * FP16 dispatch → flash-attn path (caches not exposed); FP32 → training path.
 */
gpu.crossAttentionForward(X, Ctx, Wq, Wk, Wv, Wo, mask, numHeads, O);

/**
 * FP16 cross-attention with head-averaged attention map AttnAvg and an
 * optional pre-softmax logit bias (Lq, Lk). Used by Cross-Attention Tree
 * Search to inspect / inject attention maps.
 */
gpu.crossAttentionForwardWithAttn(X, Ctx, Wq, Wk, Wv, Wo,
                                  mask, attnLogitBias /* GpuTensor|null */,
                                  numHeads, O, AttnAvg);

gpu.crossAttentionForwardTrain(X, Ctx, Wq, Wk, Wv, Wo, mask, numHeads,
                               Qh, Kh, Vh, Attnh, Yconcat, O);
gpu.crossAttentionBackward(dO, X, Ctx, Qh, Kh, Vh, Attnh, Yconcat,
                           Wq, Wk, Wv, Wo, mask, numHeads,
                           dX, dCtx, dWq, dWk, dWv, dWo);

/**
 * Spatial moments of a cross-attention map (Lq, Lk). For each text token k:
 *   mass[k] = sum_q Attn[q, k]
 *   centroid[k] = mass-weighted (y, x) of the image-token grid.
 * Used as an MCTS reward primitive over diffusion attention maps.
 */
gpu.attentionTokenMoments(Attn, h_lat, w_lat, mass, centroid);

/** Causal mask helper: mask[k] = (k <= q) ? 1 : 0; sized to (L, 1). */
gpu.buildCausalMaskRow(L, q, mask);


// -----------------------------------------------------------------------------
// T5-style self-attention with relative-position bias
// -----------------------------------------------------------------------------
//
// Scaled self-attention with an optional additive per-head bias on the
// pre-softmax scores — the encoder attention of a T5 text encoder. Unlike the
// other attention ops it takes an explicit `scale` (T5 does NOT scale the QK
// dot product, so pass 1.0) and an additive `attnBias`.
//
//   X:        (L, D) token activations; O resized + dtype-matched to X.
//   Wq/Wk/Wv/Wo: (D, D) projection weights, same dtype as X.
//   mask:     optional device key-validity mask (length L, 1 valid / 0 not),
//             or null. Also gates padded query rows.
//   attnBias: optional (numHeads*L, L) FP32 GpuTensor — row h*L+q holds head
//             h's length-L bias for query q. null → plain scaled attention.
//             T5's relative-position bias is built host-side (bucketed) and
//             uploaded here. FP32 on every backend, regardless of X.dtype.
//   scale:    QK-dot multiplier, applied before the bias.
//
// Dispatched on X.dtype (FP32 / FP16 / BF16); FP32 internal math. Scores are
// materialised (L, L) per head — intended for encoder-length sequences
// (T5 ≤ 512).
gpu.selfAttentionBiasForward(X, Wq, Wk, Wv, Wo,
                             /*mask|null*/ null, /*attnBias|null*/ bias,
                             numHeads, /*scale*/ 1.0, O);


// -----------------------------------------------------------------------------
// Flash-attention family (FP16, recompute-based backward)
// -----------------------------------------------------------------------------

/**
 * Bare FlashAttention forward: Q/K/V already projected, no Wo. Tiled online
 * softmax; works for arbitrary Lq/Lk. `causal` enables autoregressive
 * masking (requires Lq == Lk).
 */
gpu.flashAttentionForward(Q, K, V, mask, numHeads, /*causal*/ false, O);

/**
 * Bare FlashAttention backward — recompute-based; no per-call caches saved
 * by the forward. dQ/dK/dV are overwritten.
 */
gpu.flashAttentionBackward(Q, K, V, O, dO, mask, numHeads, false,
                           dQ, dK, dV);

/**
 * Fused flash-attention with all four projection matmuls inside the kernel.
 * Pass `Ctx = null` for self-attention (causal CLIP-text usage); pass a
 * (Lk, D_ctx) tensor for cross-attention. Each optional bias can be null.
 */
gpu.flashAttentionQkvoForward(X, /*Ctx|null*/ null,
                              Wq, /*bq|null*/ null,
                              Wk, /*bk|null*/ null,
                              Wv, /*bv|null*/ null,
                              Wo, /*bo|null*/ null,
                              /*mask|null*/ null,
                              numHeads, /*causal*/ false, O);

/**
 * Backward partner of flashAttentionQkvoForward. Recompute-style: no caches
 * consumed from the forward. Pass the same inputs the forward saw. Uses an
 * options object because the positional form has 22 args.
 *
 * Required keys: X, Wq, Wk, Wv, Wo, dO, numHeads, dX, dWq, dWk, dWv, dWo
 * Optional keys (default null/false):
 *   Ctx, bq, bk, bv, bo, mask, causal, dCtx, dbq, dbk, dbv, dbo
 *
 * dCtx must be non-null iff Ctx was non-null in the forward. Bias-grad
 * symmetry: dbX is required iff the corresponding bX was supplied.
 */
gpu.flashAttentionQkvoBackward({
    X, Ctx, Wq, bq, Wk, bk, Wv, bv, Wo, bo,
    mask, numHeads, causal: false, dO,
    dX, dCtx, dWq, dbq, dWk, dbk, dWv, dbv, dWo, dbo,
});

/**
 * Pre-project a context tensor through Wk/Wv. Useful for diffusion: the
 * text context is fixed across denoising steps, so projecting once amortises
 * the cost over all steps.
 *   K_out, V_out:  (Lk, D)  FP16
 */
gpu.flashAttentionProjectKv(ctx, Wk, /*bk|null*/ null,
                                Wv, /*bv|null*/ null,
                                K_out, V_out);

/**
 * Flash-attention with X projected to Q on the fly but K/V supplied
 * pre-projected (typically via flashAttentionProjectKv). Bitwise-equivalent
 * to the cached path of flashAttentionQkvoForward.
 */
gpu.flashAttentionQWithKvCachedForward(X, K, V,
                                       Wq, /*bq|null*/ null,
                                       Wo, /*bo|null*/ null,
                                       /*mask|null*/ null,
                                       numHeads, /*causal*/ false, O);

/**
 * Causal flash-attention against a partially-filled KV cache. Reads only
 * the first `validLen` rows of K_cache / V_cache. Query position
 *   p_q = validLen - L_q + i
 * attends to cache positions [0, p_q]. Typical L_q == 1 for token-by-token
 * decoding.
 */
gpu.flashAttentionDecode(Q, K_cache, V_cache, validLen, numHeads, O);

/**
 * Append L_new freshly-projected K/V rows into rows [curLen, curLen+L_new)
 * of K_cache / V_cache. Caches must be pre-sized.
 */
gpu.kvCacheAppend(K_new, V_new, curLen, K_cache, V_cache);


// -----------------------------------------------------------------------------
// Conv2D (NCHW) — forward + per-input/weight/bias backwards
// -----------------------------------------------------------------------------
//
// All NCHW tensors are stored as 2D (N, C * H * W); the spatial dims are
// passed as integer arguments. The weight tensor uses OIHW layout:
// (C_out, (C_in/groups) * kH * kW). `groups` is required (pass 1 for the
// standard full-channel convolution). FP32 and FP16 are both supported and
// dispatched on X.dtype.
//
// Output dims:
//   H_out = (H + 2*pH - dH * (kH - 1) - 1) / sH + 1
//   W_out = (W + 2*pW - dW * (kW - 1) - 1) / sW + 1

gpu.conv2dForward(X, Wt, /*bias|null*/ null,
                  N, C_in, H, W, C_out, kH, kW,
                  sH, sW, pH, pW, dH, dW, groups, Y);

gpu.conv2dBackwardInput(Wt, dY,
                        N, C_in, H, W, C_out, kH, kW,
                        sH, sW, pH, pW, dH, dW, groups, dX);  // overwrite

gpu.conv2dBackwardWeight(X, dY,
                         N, C_in, H, W, C_out, kH, kW,
                         sH, sW, pH, pW, dH, dW, groups, dWt); // accumulate

gpu.conv2dBackwardBias(dY, N, C_out, H_out, W_out, dB);        // accumulate


// -----------------------------------------------------------------------------
// 2x up/downsample (NCHW)
// -----------------------------------------------------------------------------
//
// Forward and backward share the same (N, C, H, W) convention — H and W are
// the *input* dims (post-upsample dims = 2H, 2W; pre-downsample dims for the
// down family). FP32 and FP16 dispatched on the input dtype.

gpu.upsampleNearest2xForward(X,  N, C, H, W, Y);    // 2x nearest-neighbour
gpu.upsampleNearest2xBackward(dY, N, C, H, W, dX);

gpu.upsampleBilinear2xForward(X,  N, C, H, W, Y);   // align_corners=false
gpu.upsampleBilinear2xBackward(dY, N, C, H, W, dX);

gpu.downsampleAvg2xForward(X,  N, C, H, W, Y);      // 2x2 avg-pool
gpu.downsampleAvg2xBackward(dY, N, C, H, W, dX);


// -----------------------------------------------------------------------------
// NCHW ↔ sequence transpose
// -----------------------------------------------------------------------------
//
// Lets transformer-shaped ops (attention, layernorm-batched) consume tensors
// produced by NCHW primitives (conv2d, group_norm, resblock). Per-element
// gather; FP32 and FP16 supported.

// X: (N, C * H * W)  →  Y: (N * H * W, C)
gpu.nchwToSequence(X, N, C, H, W, Y);

// X: (N * H * W, C)  →  Y: (N, C * H * W)
gpu.sequenceToNchw(X, N, C, H, W, Y);


// -----------------------------------------------------------------------------
// ResBlock (fused SD U-Net residual block)
// -----------------------------------------------------------------------------
//
// Options-object call site: too many optional tensors for a positional API.
//
// Required:
//   X, gamma1, beta1, W1, gamma2, beta2, W2, Y,
//   N, C_in, C_out, H, W
// Optional (any can be null/undefined):
//   b1, t_emb_shift, b2, Wskip, bskip
//   numGroups (default 32), eps (default 1e-5)
//
// Math (forward):
//   h  = silu(group_norm(X, gamma1, beta1))
//   h  = conv2d_3x3_same(h, W1, b1)
//   if t_emb_shift: h += broadcast(t_emb_shift)
//   h  = silu(group_norm(h, gamma2, beta2))
//   h  = conv2d_3x3_same(h, W2, b2)
//   Y  = h + (C_in == C_out ? X : conv2d_1x1(X, Wskip, bskip))

gpu.resblockForward({
    X, gamma1, beta1, W1, b1, t_emb_shift,
    gamma2, beta2, W2, b2,
    Wskip: null, bskip: null,
    N, C_in, C_out, H, W,
    numGroups: 32, eps: 1e-5,
    Y,
});

/**
 * ResBlock backward. Recomputes the forward intermediates internally —
 * pass the same forward inputs plus dY and the gradient accumulators.
 * dWeights and dGammas/dBetas are accumulated; dX is overwritten.
 */
gpu.resblockBackward({
    X, gamma1, beta1, W1, b1, t_emb_shift,
    gamma2, beta2, W2, b2,
    Wskip, bskip,
    N, C_in, C_out, H, W, numGroups: 32, eps: 1e-5,
    dY,
    dX,
    dGamma1, dBeta1, dW1, db1, dt_emb_shift,
    dGamma2, dBeta2, dW2, db2,
    dWskip, dbskip,
});


// -----------------------------------------------------------------------------
// Pooling, losses, embedding, concat
// -----------------------------------------------------------------------------

gpu.maskedMeanPoolForward(X, /*mask|null*/ null, y);
gpu.maskedMeanPoolBackward(dY, mask, K, dX);

const mseLoss   = gpu.mseVecForward(pred, target);
gpu.mseVecBackward(pred, target, dPred);

/**
 * Per-sample MSE (CPU `mse_scalar` parity: loss = 0.5*d², dPred = d).
 *   pred, target:  (B, 1)
 *   dPred:         (B, 1)
 *   lossPerSample: (B, 1)
 */
gpu.mseVecPerSample(pred, target, dPred, lossPerSample);

/**
 * Fused softmax + cross-entropy. Returns the scalar loss; writes probs and
 * dLogits = probs - target on valid entries (0 on invalid).
 */
const xentLoss = gpu.softmaxXentFused(logits, target, mask, probs, dLogits);

/**
 * Batched fused softmax + cross-entropy across (sample, head) tiles. Used
 * by trainers that share a single (B, n_act_total) logits buffer across all
 * actor heads. `headOffsets` is a GpuTensor wrapping an int32 device buffer
 * of cumulative offsets, length n_heads + 1.
 *
 *   logits_BL, target_BL, probs_BL, dLogits_BL: (B, n_act_total)
 *   mask:        (B, n_act_total) device pointer or null
 *   lossPerSample: (B, 1) — overwritten with sum-over-heads loss
 */
gpu.softmaxXentFusedBatched(logits_BL, target_BL, /*mask|null*/ null,
                            headOffsets, n_heads,
                            probs_BL, dLogits_BL, lossPerSample);

/**
 * Embedding lookup. `idx` is a GpuTensor reinterpreted as an int32_t*
 * device buffer (its size must equal B). Avoids per-call host→device copies.
 *   out[b, :] = table[idx[b], :]
 */
gpu.embeddingLookupForward(table, idxAsInt32, B, out);
gpu.embeddingLookupBackward(dOut, idxAsInt32, B, dTable);  // accumulated

/** Concat flat tensors end-to-end. `parts` is a JS array of GpuTensors. */
gpu.concatRows([part0, part1, part2], out);

/** Inverse: scatter disjoint segments of `in` back into each `parts[i]`. */
gpu.splitRows(in_, [part0, part1, part2]);

/**
 * Batched column-block concat. parts are each (B, d_i); out becomes
 * (B, sum d_i).
 *   out[b, off_i + j] = parts[i][b, j]
 */
gpu.concatBatchedRows([part0, part1, part2], out);

/**
 * Channel-axis concat over NCHW tensors. Each `parts[i]` is shape
 * (N, C_i * H * W); out becomes (N, sum_i C_i * H * W) with channel blocks
 * regrouped per sample.
 *   C_per_part: JS array of ints, same length as parts.
 */
gpu.concatNchwChannels([part0, part1], N, H, W, [C0, C1], out);

/**
 * Backward of channel-axis NCHW concat. Each parts[i] is overwritten with
 * the matching channel slice of dY.
 */
gpu.concatNchwChannelsBackward(dY, N, H, W, [C0, C1], [dPart0, dPart1]);


// -----------------------------------------------------------------------------
// Optimisers
// -----------------------------------------------------------------------------

/**
 *   velocity = momentum*velocity + grad
 *   param   -= lr * velocity
 */
gpu.sgdStep(param, grad, velocity, lr, momentum);

/**
 * Adam (1-based step counter for bias correction):
 *   m = beta1*m + (1 - beta1)*g
 *   v = beta2*v + (1 - beta2)*g^2
 *   param -= lr * (m / (1 - beta1^step)) / (sqrt(v / (1 - beta2^step)) + eps)
 */
gpu.adamStep(param, grad, m, v, lr, beta1, beta2, eps, step);


// -----------------------------------------------------------------------------
// Batched (inference + training)
// -----------------------------------------------------------------------------
//
// (B, D) row-major layout. Forward variants are launch-amortising
// single-kernel forwards. Backward variants partner the forwards for
// training paths that share a minibatch.

gpu.linearForwardBatched(W, bias, X_BD, Y_BD);
gpu.linearForwardBatchedFp16(W, /*bias|null*/ null, X_BD, Y_BD);  // FP16-only
gpu.reluForwardBatched(X_BD, Y_BD);
gpu.tanhForwardBatched(X_BD, Y_BD);
gpu.addInplaceBatched(Y_BD, X_BD);

gpu.linearBackwardBatched(W, X_BD, dY_BD, dX_BD, dW, dB);  // dW/dB accumulate
gpu.reluBackwardBatched(X_BD, dY_BD, dX_BD);   // reads X
gpu.tanhBackwardBatched(Y_BD, dY_BD, dX_BD);   // reads Y (forward output)


// -----------------------------------------------------------------------------
// Diffusion samplers (FP16, fwd-only)
// -----------------------------------------------------------------------------
//
// The scheduler keeps its α / σ / log-SNR coefficients host-side; the kernel
// just applies one elementwise step. x_t and eps_pred must share shape;
// outputs are resized to match.

/**
 *   x0_pred = (x_t - sqrt(1 - alpha_t) * eps_pred) / sqrt(alpha_t)
 *   dir     = sqrt(1 - alpha_prev - sigma_t^2) * eps_pred
 *   x_prev  = sqrt(alpha_prev) * x0_pred + dir
 */
gpu.ddimStep(x_t, eps_pred, alpha_t, alpha_prev, sigma_t, x_prev);

/** Euler (ε-prediction): x_prev = x_t + (sigma_prev - sigma_t) * eps_pred. */
gpu.eulerStep(x_t, eps_pred, sigma_t, sigma_prev, x_prev);

/**
 * DPM-Solver++ 2M, ε-prediction. The caller maintains a running x0 cache;
 * coefficients (c_xt, c_x0t, c_x0prev) are derived host-side from the
 * scheduler's σ / log-SNR schedule. First step has no x0_prev — use
 * eulerStep instead. x0_out is the new x0 estimate to copy into x0_prev for
 * the next step.
 */
gpu.dpmpp2mStep(x_t, eps_pred, x0_prev, sigma_t,
                c_xt, c_x0t, c_x0prev, x_prev, x0_out);

/**
 * Sinusoidal timestep embedding (SD/SDXL default: flip_sin_to_cos=true,
 * downscale_freq_shift=0).
 *   timesteps: (N, 1) FP32
 *   Y:         (N, dim) FP32
 */
gpu.timestepEmbedding(timesteps, dim, /*maxPeriod*/ 10000.0, Y);


// -----------------------------------------------------------------------------
// INT8 weight-only quantisation (W8A16)
// -----------------------------------------------------------------------------
//
// Activations stay FP16; weights are quantised to per-output-row symmetric
// INT8 with FP32 dequant scales. No backward — weights are frozen at
// inference time.

/**
 * Host helper. Quantises an FP16 weight matrix (passed as a Uint16Array of
 * IEEE binary16 bit patterns) to INT8 + per-output-row FP32 scales.
 * Returns fresh typed arrays:
 *   { weights: Int8Array of length out*in,
 *     scales:  Float32Array of length out }
 * The caller uploads these into GpuTensors with the int8/fp32 dtypes.
 */
const { weights, scales } = gpu.quantizeInt8PerRowHost(W_fp16_uint16, out, in_);

/** Y = dequant(W_int8, scales) @ X.  W: (out,in) int8; scales: (out,1) FP32. */
gpu.matmulInt8wFp16(W_int8, scales, X, Y);

/** W8A16 conv2d forward. Same shape contract as conv2dForward. */
gpu.conv2dInt8wFp16Forward(X, W_int8, scales, /*bias|null*/ null,
                           N, C_in, H, W, C_out, kH, kW,
                           sH, sW, pH, pW, dH, dW, groups, Y);

/** W8A16 batched linear: Y(B, out) = X(B, in) @ dequant(W)^T + bias. */
gpu.linearForwardBatchedInt8wFp16(W_int8, scales, /*bias|null*/ null,
                                  X_BD, Y_BD);

/** W8A16 ResBlock forward — same options shape as resblockForward but each
 *  conv weight is replaced by its INT8 + FP32 scales pair. */
gpu.resblockForwardInt8wFp16({
    X, gamma1, beta1, W1_int8, s1, b1, t_emb_shift,
    gamma2, beta2, W2_int8, s2, b2,
    Wskip_int8: null, sskip: null, bskip: null,
    N, C_in, C_out, H, W, numGroups: 32, eps: 1e-5, Y,
});

/** W8A16 KV-projection: project ctx through (Wk, sk, bk?) and (Wv, sv, bv?). */
gpu.flashAttentionProjectKvInt8wFp16(ctx, Wk_int8, sk, /*bk|null*/ null,
                                          Wv_int8, sv, /*bv|null*/ null,
                                          K_out, V_out);

/** W8A16 Q-with-pre-projected-KV flash-attention forward. */
gpu.flashAttentionQWithKvCachedInt8wFp16(X, K, V,
                                         Wq_int8, sq, /*bq|null*/ null,
                                         Wo_int8, so, /*bo|null*/ null,
                                         /*mask|null*/ null,
                                         numHeads, /*causal*/ false, O);

/** W8A16 fused-QKVO flash-attention forward (options-object form). */
gpu.flashAttentionQkvoInt8wFp16({
    X, Ctx: null,
    Wq_int8, sq, bq: null,
    Wk_int8, sk, bk: null,
    Wv_int8, sv, bv: null,
    Wo_int8, so, bo: null,
    mask: null, numHeads, causal: false, O,
});

/**
 * W8A16 variant of selfAttentionBiasForward — the quantised T5 encoder
 * attention. Each projection weight is an INT8 (D, D) matrix paired with an
 * FP32 (D, 1) per-output-row dequant scale; activations stay FP16. `attnBias`
 * is FP32 (numHeads*L, L) or null; `scale` is the pre-bias QK multiplier.
 */
gpu.selfAttentionBiasInt8wFp16(X,
                               Wq_int8, sq, Wk_int8, sk,
                               Wv_int8, sv, Wo_int8, so,
                               /*mask|null*/ null, /*attnBias|null*/ bias,
                               numHeads, /*scale*/ 1.0, O);


// -----------------------------------------------------------------------------
// safetensors — load / save
// -----------------------------------------------------------------------------
//
// Read and write the huggingface safetensors container format. The reader
// mmap's the file: opening a multi-GB checkpoint and inspecting only its
// header() is cheap — no payload is faulted in until get() uploads a tensor.

/**
 * Open a .safetensors file. Returns an opaque handle that owns the mmap.
 * Throws if the file is missing or malformed.
 */
const f = gpu.openSafetensors('/path/to/model.safetensors');

/** Number of tensors in the file. */
f.count;

/** Tensor names, in file order. -> string[] */
f.names();

/**
 * Per-tensor metadata — no payload read, so this is cheap on huge files.
 * -> { name: { dtype: "F32"|"F16"|"BF16"|..., shape: number[], nbytes }, ... }
 */
const hdr = f.header();

/**
 * Upload one tensor as a GpuTensor. brotensor tensors are 2D; when rows/cols
 * are omitted the N-D source is flattened to (shape[0], numel/shape[0]).
 * Source dtype must be F16 or F32. Throws on an unknown name.
 */
const W = f.get('model.layers.0.self_attn.q_proj.weight');
const W2 = f.get('embedding.weight', 1024, 768);   // explicit 2D shape

/** Release the mmap early. Also released automatically on GC. */
f.close();

/**
 * Write a set of GpuTensors to a .safetensors file. Each value must be a
 * GpuTensor; FP32 and FP16 tensors are supported. Shape is stored as the
 * tensor's (rows, cols).
 */
gpu.saveSafetensors('/path/to/out.safetensors', { weight: W, bias: b });
