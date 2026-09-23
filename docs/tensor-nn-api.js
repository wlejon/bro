// =============================================================================
// bro.tensor, part 2: attention, convolution, quantisation, audio
// =============================================================================
//
// The second half of the bro.tensor surface (brotensor). The core — runtime,
// dtypes, GpuTensor, RNG, safetensors, dense/elementwise ops, the norms,
// matmul, RoPE, reductions, the batched (B, D) family, embedding, losses,
// concat/split and the optimisers — is in **docs/tensor-api.js**. Read that
// file first: everything here assumes its conventions.
//
// Conventions carried over from tensor-api.js:
//   - `mask|null` is a device-resident FP32 GpuTensor (1 valid / 0 masked) or
//     null. Host Float32Arrays are rejected.
//   - INT32 buffers (cuSeq*, posT/posH/posW, pooling Idx, seqBounds,
//     ropeQkvPackedInplace's pos) are GpuTensors; GpuTensor.prototype.uploadInt32
//     builds one. Their values address memory the kernels do not bounds-check,
//     so the binding reads them back to the host (one device sync per call)
//     and throws on an out-of-range entry. Every one except pooling Idx (the
//     forward's INT32 output) also accepts whole-number FP32. The sampler's
//     `indices` is an INT32 output and stays on the device.
//   - NCHW / NCTHW / NCL activations are packed as 2D tensors — (N, C*H*W),
//     (N, C*T*H*W), (N, C*L) — with the spatial dims passed as ints.
//   - Outputs are resized + dtype-set to their input unless a comment says
//     otherwise. Gradients marked "accumulate" require the caller to zero them.
//   - Ops report failure by throwing an Error at the call site, including
//     "not implemented" when the active backend has no kernel for them.
//
// GPU-only groups: the FP16 / INT8-W8A16 / GGUF k-quant families below have no
// CPU kernels (quantizeInt8PerRowHost is the exception, a pure host helper).
//
// =============================================================================

const gpu = bro.tensor;


// -----------------------------------------------------------------------------
// Single-head + multi-head attention (FP32 training)
// -----------------------------------------------------------------------------

/**
 * Single-head SDPA self-attention. The forward fills the Q/K/V/Attn/Y_pre_Wo
 * caches so the backward recomputes nothing.
 */
gpu.attentionForward(X, Wq, Wk, Wv, Wo, /*mask|null*/ null,
                     Q, K, V, Attn, Y_pre_Wo, O);
gpu.attentionBackward(dO, X, Q, K, V, Attn, Y_pre_Wo,
                      Wq, Wk, Wv, Wo, /*mask|null*/ null,
                      dX, dWq, dWk, dWv, dWo);

/**
 * Multi-head self-attention. Wq/Wk/Wv/Wo are (D, D), split into `numHeads`
 * heads of head_dim = D/numHeads. Caches: Qh/Kh/Vh (h*L, head_dim),
 * Attnh (h*L, L), Yconcat (L, D), O (L, D).
 */
gpu.mhaForward(X, Wq, Wk, Wv, Wo, /*mask|null*/ null, numHeads,
               Qh, Kh, Vh, Attnh, Yconcat, O);
gpu.mhaBackward(dO, X, Qh, Kh, Vh, Attnh, Yconcat,
                Wq, Wk, Wv, Wo, /*mask|null*/ null, numHeads,
                dX, dWq, dWk, dWv, dWo);


// -----------------------------------------------------------------------------
// Self / cross attention (inference + training variants)
// -----------------------------------------------------------------------------

/** Inference self-attention, no caches exposed. Dispatched on X.dtype. */
gpu.selfAttentionForward(X, Wq, Wk, Wv, Wo, /*mask|null*/ null, numHeads, O);

/** FP32 training self-attention with caches, plus its backward. */
gpu.selfAttentionForwardTrain(X, Wq, Wk, Wv, Wo, /*mask|null*/ null, numHeads,
                              Qh, Kh, Vh, Attnh, Yconcat, O);
gpu.selfAttentionBackward(dO, X, Qh, Kh, Vh, Attnh, Yconcat,
                          Wq, Wk, Wv, Wo, /*mask|null*/ null, numHeads,
                          dX, dWq, dWk, dWv, dWo);

/**
 * Cross-attention: K/V projected from a separate context tensor (a diffusion
 * U-Net's text conditioning). Wk/Wv may be rectangular (D, D_ctx).
 */
gpu.crossAttentionForward(X, Ctx, Wq, Wk, Wv, Wo, /*mask|null*/ null, numHeads, O);

/**
 * Cross-attention that also emits the head-averaged attention map AttnAvg, and
 * takes an optional additive pre-softmax logit bias. For inspecting or
 * steering a diffusion model's attention (Cross-Attention Tree Search).
 */
gpu.crossAttentionForwardWithAttn(X, Ctx, Wq, Wk, Wv, Wo,
                                  /*mask|null*/ null, /*attnLogitBias|null*/ null,
                                  numHeads, O, AttnAvg);

gpu.crossAttentionForwardTrain(X, Ctx, Wq, Wk, Wv, Wo, /*mask|null*/ null, numHeads,
                               Qh, Kh, Vh, Attnh, Yconcat, O);
gpu.crossAttentionBackward(dO, X, Ctx, Qh, Kh, Vh, Attnh, Yconcat,
                           Wq, Wk, Wv, Wo, /*mask|null*/ null, numHeads,
                           dX, dCtx, dWq, dWk, dWv, dWo);

/**
 * Spatial moments of a cross-attention map (Lq, Lk). For each context token k:
 *   mass[k]     = sum_q Attn[q, k]
 *   centroid[k] = the mass-weighted (y, x) over the (h_lat, w_lat) image grid.
 * A reward primitive over diffusion attention maps.
 */
gpu.attentionTokenMoments(Attn, h_lat, w_lat, mass, centroid);

/** Causal mask helper: mask[k] = (k <= q) ? 1 : 0, sized to (L, 1). */
gpu.buildCausalMaskRow(L, q, mask);


// -----------------------------------------------------------------------------
// T5-style self-attention with relative-position bias
// -----------------------------------------------------------------------------
//
// Scaled self-attention with an optional additive per-head bias on the
// pre-softmax scores: the encoder attention of a T5 text encoder. Unlike the
// other attention ops it takes an explicit `scale` (T5 does NOT scale the QK
// dot, so pass 1.0) and an additive `attnBias`.
//
//   X:           (L, D); O resized + dtype-matched to X.
//   Wq/Wk/Wv/Wo: (D, D), same dtype as X.
//   mask:        optional length-L device key mask; also gates padded queries.
//   attnBias:    optional (numHeads*L, L) FP32 tensor — row h*L+q holds head
//                h's length-L bias for query q. FP32 on every backend, whatever
//                X.dtype is. T5's bucketed bias is built host-side and uploaded.
//   scale:       the QK-dot multiplier, applied before the bias (default 1.0).
//
// Scores are materialised (L, L) per head, so this is for encoder-length
// sequences (T5 <= 512), not for long decoders.
gpu.selfAttentionBiasForward(X, Wq, Wk, Wv, Wo,
                             /*mask|null*/ null, /*attnBias|null*/ bias,
                             numHeads, /*scale*/ 1.0, O);

/**
 * The device-side producer of that bias for the FastConformer / Conformer /
 * Transformer-XL family, where the position term is a second dot product
 * against a projected relative positional encoding:
 *   Bias[h*T + q, k] = sum_d Qv[q, h*headDim + d] * Pk[(T-1-q) + k, h*headDim + d]
 * (NeMo's rel_shift of Qv . Pk^T, per head). Qv is the position-term query
 * (q-projection + pos_bias_v, already scaled by 1/sqrt(headDim)); Pk the
 * relative-key projection of the (2T-1, D) encoding.
 *   Qv: (T, D) FP32.  Pk: (2T-1, D) FP32.  D = numHeads*headDim.
 *   Bias: (numHeads*T, T) FP32, resized — feed it to selfAttentionBiasForward.
 */
gpu.relPosBiasXlForward(Qv, Pk, numHeads, headDim, Bias);


// -----------------------------------------------------------------------------
// Flash attention
// -----------------------------------------------------------------------------

/**
 * Bare FlashAttention forward: Q/K/V already projected, no Wo. Tiled online
 * softmax, arbitrary Lq / Lk. `causal` enables autoregressive masking (which
 * needs Lq === Lk).
 */
gpu.flashAttentionForward(Q, K, V, /*mask|null*/ null, numHeads, /*causal*/ false, O);

/**
 * Sliding-window causal self-attention. Q/K/V already projected as
 * (L, numHeads*headDim). Always causal: the Lq queries sit at the last Lq
 * positions of a length-Lk sequence (q_offset = Lk - Lq) and each attends keys
 * [max(0, pos-window+1), pos]. `window <= 0` is unbounded causal.
 *   - Lq === Lk: prefill / codec sliding window.
 *   - Lq  <  Lk: incremental decode of an Lq-token block over a K/V cache.
 * Supports GQA (K/V may carry fewer heads than Q). Inference-only; the local
 * attention streaming codecs use (Qwen3-TTS / Mimi).
 */
gpu.flashAttentionWindowedForward(Q, K, V, /*mask|null*/ null, numHeads,
                                  /*window*/ 0, O);

/**
 * Bare FlashAttention backward, recompute-based: the forward saves no caches.
 * dQ / dK / dV are overwritten, not accumulated.
 */
gpu.flashAttentionBackward(Q, K, V, O, dO, /*mask|null*/ null, numHeads,
                           /*causal*/ false, dQ, dK, dV);

/**
 * Fused flash attention with all four projection matmuls inside the kernel.
 * Pass Ctx = null for self-attention, or a (Lk, D_ctx) tensor for cross.
 * Every bias may be null.
 */
gpu.flashAttentionQkvoForward(X, /*Ctx|null*/ null,
                              Wq, /*bq|null*/ null,
                              Wk, /*bk|null*/ null,
                              Wv, /*bv|null*/ null,
                              Wo, /*bo|null*/ null,
                              /*mask|null*/ null,
                              numHeads, /*causal*/ false, O);

/**
 * Its backward. Recompute-style: nothing is consumed from the forward, so pass
 * the same inputs the forward saw. An options object, because the positional
 * form would take 24 arguments.
 *
 * Required: X, Wq, Wk, Wv, Wo, dO, numHeads, dX, dWq, dWk, dWv, dWo
 * Optional (null / false by default): Ctx, bq, bk, bv, bo, mask, causal,
 *                                     dCtx, dbq, dbk, dbv, dbo
 * dCtx must be non-null iff Ctx was; each dbX iff the matching bX was.
 */
gpu.flashAttentionQkvoBackward({
    X, Ctx, Wq, bq, Wk, bk, Wv, bv, Wo, bo,
    mask, numHeads, causal: false, dO,
    dX, dCtx, dWq, dbq, dWk, dbk, dWv, dbv, dWo, dbo,
});

/**
 * Pre-project a context tensor through Wk/Wv. A diffusion step's text context
 * is fixed across the whole denoise, so projecting once amortises it over
 * every step. K_out, V_out: (Lk, D).
 */
gpu.flashAttentionProjectKv(ctx, Wk, /*bk|null*/ null,
                                 Wv, /*bv|null*/ null, K_out, V_out);

/**
 * Flash attention with X projected to Q on the fly but K/V supplied
 * pre-projected (typically by flashAttentionProjectKv). Equivalent to the
 * cached path of flashAttentionQkvoForward.
 */
gpu.flashAttentionQWithKvCachedForward(X, K, V, Wq, /*bq|null*/ null,
                                       Wo, /*bo|null*/ null,
                                       /*mask|null*/ null,
                                       numHeads, /*causal*/ false, O);

/**
 * Causal flash attention against a partially filled KV cache: reads only the
 * first `validLen` rows of K_cache / V_cache. Query position
 * p_q = validLen - L_q + i attends cache positions [0, p_q]; L_q === 1 for
 * token-by-token decoding.
 *
 * @param {number} [numKvHeads=numHeads]  GQA: must divide numHeads — query head
 *   h reads KV head h/(numHeads/numKvHeads). Omit for plain MHA.
 * @param {number} [attnSoftcap=0]  > 0 applies Gemma-2 tanh logit soft-capping
 *   (s -> cap*tanh(s/cap)) before the causal / window mask.
 * @param {number} [window=0]  > 0 is sliding-window causal: query at absolute
 *   position p attends key j only when j <= p AND j > p-window.
 * All three default to the pre-GQA behaviour.
 */
gpu.flashAttentionDecode(Q, K_cache, V_cache, validLen, numHeads, O,
                         numKvHeads, attnSoftcap, window);

/**
 * The CUDA-graph-capturable twin of flashAttentionDecode. K_cache / V_cache are
 * always read at their full (L_max, .) shape so the launch shape never changes
 * as generation advances, and validity comes from `dMask` — a device-resident
 * FP32 tensor of length L_max the caller updates between graph replays —
 * instead of a validLen scalar. Q must be a single query row (L_q === 1). With
 * dMask = [1]*validLen + [0]*(L_max-validLen) the result matches
 * flashAttentionDecode bit for bit. Same numKvHeads / attnSoftcap / window
 * extensions.
 */
gpu.flashAttentionDecodeMasked(Q, K_cache, V_cache, dMask, numHeads, O,
                               numKvHeads, attnSoftcap, window);

/**
 * Append L_new freshly projected K/V rows into rows [curLen, curLen+L_new) of
 * K_cache / V_cache. The caches must be pre-sized.
 */
gpu.kvCacheAppend(K_new, V_new, curLen, K_cache, V_cache);

/**
 * Grouped-query self-attention over pre-projected Q/K/V, causal or fully
 * bidirectional (the encoder prefill of an LLM2Vec-style bidirectional
 * decoder). Q carries numQHeads, K/V carry numKvHeads (which must divide
 * numQHeads; equal is plain MHA). Tiled online softmax, FP32 accumulation.
 *   Q: (Lq, numQHeads*headDim);  K, V: (Lk, numKvHeads*headDim); one dtype
 *   (FP32/FP16/BF16).  Lk >= Lq; causal additionally needs Lq === Lk.
 *   mask: optional length-Lk FP32 key mask.   O: (Lq, numQHeads*headDim).
 */
gpu.flashAttentionGqaForward(Q, K, V, /*mask|null*/ null, numQHeads, numKvHeads,
                             /*causal*/ false, O);


// -----------------------------------------------------------------------------
// Specialised attention (SAM rel-pos, packed varlen, gated delta rule, M-RoPE)
// -----------------------------------------------------------------------------

/**
 * SAM / ViTDet decomposed 2D relative-position self-attention. Token t maps to
 * grid coords (t/gridW, t%gridW) over a gridH*gridW patch grid, so
 * X.rows === gridH*gridW. The bias reads the projected query and is factored
 * into two tables rather than a materialised L x L bias:
 *   relPosH: (2*gridH-1, headDim)   relPosW: (2*gridW-1, headDim)
 * `scale` multiplies the Q.K dot only (typically 1/sqrt(headDim)). Each of
 * bq/bk/bv/bo may be null.
 */
gpu.selfAttentionDecomposedRelPosForward(
    X, Wq, bq, Wk, bk, Wv, bv, Wo, bo, relPosH, relPosW,
    numHeads, gridH, gridW, /*scale*/ 1 / Math.sqrt(headDim), O);

/**
 * The windowed block: window x window tiles run independently, the grid is
 * zero-padded up to a multiple of `window` and cropped back. relPosH / relPosW
 * are sized for the WINDOW here — (2*window-1, headDim).
 */
gpu.selfAttentionDecomposedRelPosWindowedForward(
    X, Wq, bq, Wk, bk, Wv, bv, Wo, bo, relPosH, relPosW,
    numHeads, gridH, gridW, window, /*scale*/ 1 / Math.sqrt(headDim), O);

/**
 * Packed variable-length flash attention (Qwen-VL window attention). Q is
 * (totalTokensQ, numHeads*headDim), K/V are (totalTokensK, ...); the
 * per-sequence boundaries are the INT32 prefix sums cuSeqQ / cuSeqK, each of
 * length batch+1, in an INT32 (or whole-number FP32) GpuTensor (null only
 * with batch === 0). They must be non-decreasing within [0, totalTokens], and
 * no sequence may be longer than maxQ / maxK; anything else throws. No
 * cross-sequence attention.
 *
 * The backward is recompute-based — it consumes no forward caches (O is taken
 * for API symmetry) — and OVERWRITES dQ / dK / dV.
 */
gpu.flashAttentionVarlenForward(Q, K, V, cuSeqQ, cuSeqK,
                                batch, maxQ, maxK, numHeads, headDim, causal, O);
gpu.flashAttentionVarlenBackward(Q, K, V, O, dO, cuSeqQ, cuSeqK,
                                 batch, maxQ, maxK, numHeads, headDim, causal,
                                 dQ, dK, dV);

/**
 * Packed bidirectional self-attention straight off a fused QKV projection:
 * many independent sequences of any length packed back to back, no padding,
 * one launch per layer (encoder batching).
 *   QKV: (L, 3*numHeads*headDim), each row [q heads | k heads | v heads].
 *        FP16/BF16/FP32 on the GPU (headDim 64 takes the tensor-core kernel),
 *        FP32 on the CPU.
 *   seqBounds: (L, 2) INT32 (or whole-number FP32): row r's sequence is rows
 *        [seqBounds[r,0], seqBounds[r,1]), which must lie inside [0, L) and
 *        contain r. Checked on the host before the launch (the call syncs).
 *   window > 0: row r attends keys j of its sequence with |r - j| <= window/2;
 *   window <= 0: its whole sequence.
 *   O: (L, numHeads*headDim), QKV's dtype.
 * The backward is recompute-based (no forward caches), takes the same bounds
 * and window, and OVERWRITES dQKV (L, 3*numHeads*headDim) = [dQ | dK | dV].
 */
gpu.flashAttentionPackedQkvForward(QKV, seqBounds, numHeads, /*window*/ 0, O);
gpu.flashAttentionPackedQkvBackward(QKV, dO, seqBounds, numHeads, /*window*/ 0, dQKV);

/**
 * In-place RoPE over the Q and K sections of that packed QKV buffer, with a
 * per-row position id so each packed sequence restarts at 0. Same interleaved
 * pair rotation as ropeApply; the V section is untouched.
 *   cosTbl / sinTbl: (P, headDim/2) FP32.
 *   pos: (L, 1) INT32 (or whole-number FP32), every entry in [0, P) — checked
 *        on the host.
 */
gpu.ropeQkvPackedInplace(QKV, cosTbl, sinTbl, pos, numHeads, headDim);

/**
 * Gated delta rule, the linear attention of Qwen3-Next.
 *   Q/K: (L, numHeads*d_k)   V: (L, numHeads*d_v)   O: (L, numHeads*d_v)
 *   aRaw / beta: (L, numHeads) FP32 RAW gate inputs — softplus / sigmoid are
 *                applied inside the op, so do not pre-activate them.
 *   logA:  (numHeads, 1)
 *   state: (numHeads, d_v*d_k) FP32 recurrent memory, read AND updated in place.
 * `Chunked` runs a whole prefill block; `Step` advances L_step new tokens
 * against an existing state.
 */
gpu.gatedDeltaRuleChunked(Q, K, V, aRaw, beta, logA, numHeads, d_k, d_v, state, O);
gpu.gatedDeltaRuleStep(Q, K, V, aRaw, beta, logA, numHeads, d_k, d_v, state, O);

/**
 * M-RoPE, the Qwen2.5-VL / Qwen3-VL multimodal rotary. headDim splits into
 * three contiguous sub-ranges of widths 2*d_t, 2*d_h, 2*d_w (in that order),
 * each rotated by its own position stream.
 *   X, Y: (L, numHeads*headDim).  cos_a / sin_a: (maxPos_a, d_a) FP32.
 *   posT / posH / posW: length-L INT32 (or whole-number FP32) GpuTensors,
 *   every entry in [0, maxPos_a), or null for an axis whose d_a is 0.
 */
gpu.ropeApplyMrope(X, cosT, sinT, cosH, sinH, cosW, sinW,
                   posT, posH, posW, headDim, numHeads, d_t, d_h, d_w, Y);


// -----------------------------------------------------------------------------
// Conv2D (NCHW)
// -----------------------------------------------------------------------------
//
// Weights are OIHW: (C_out, (C_in/groups)*kH*kW). The stride / pad / dilation /
// groups tail may be omitted or 0, which falls back to stride 1, pad 0,
// dilation 1, groups 1. FP32 and FP16 dispatched on X.dtype.
//
//   H_out = (H + 2*pH - dH*(kH-1) - 1) / sH + 1
//   W_out = (W + 2*pW - dW*(kW-1) - 1) / sW + 1

gpu.conv2dForward(X, Wt, /*bias|null*/ null,
                  N, C_in, H, W, C_out, kH, kW, sH, sW, pH, pW, dH, dW, groups, Y);

gpu.conv2dBackwardInput(Wt, dY, N, C_in, H, W, C_out, kH, kW,
                        sH, sW, pH, pW, dH, dW, groups, dX);    // overwrite
gpu.conv2dBackwardWeight(X, dY, N, C_in, H, W, C_out, kH, kW,
                         sH, sW, pH, pW, dH, dW, groups, dWt);  // accumulate
gpu.conv2dBackwardBias(dY, N, C_out, H_out, W_out, dB);         // accumulate

/**
 * Transposed (fractionally strided) conv2d. Wt is input-channel-major:
 * (C_in, (C_out/groups)*kH*kW). `opH`/`opW` (< stride) disambiguate the output
 * size, exactly torch's output_padding. dWt / dB accumulate.
 */
gpu.convTranspose2dForward(X, Wt, /*bias|null*/ null,
                           N, C_in, H, W, C_out, kH, kW,
                           sH, sW, pH, pW, opH, opW, dH, dW, groups, Y);
gpu.convTranspose2dBackwardInput(Wt, dY, N, C_in, H, W, C_out, kH, kW,
                                 sH, sW, pH, pW, opH, opW, dH, dW, groups, dX);
gpu.convTranspose2dBackwardWeight(X, dY, N, C_in, H, W, C_out, kH, kW,
                                  sH, sW, pH, pW, opH, opW, dH, dW, groups, dWt);
gpu.convTranspose2dBackwardBias(dY, N, C_out, H_out, W_out, dB);

/**
 * conv3d over NCTHW, forward only. Wt is OICTHW:
 * (C_out, (C_in/groups)*kT*kH*kW). The video / patch-embed convolution.
 */
gpu.conv3dForward(X, Wt, /*bias|null*/ null,
                  N, C_in, T, H, W, C_out, kT, kH, kW,
                  sT, sH, sW, pT, pH, pW, dT, dH, dW, groups, Y);

/**
 * Modulated deformable conv2d (torchvision deform_conv2d / DCNv2), forward
 * only. Each output pixel's kH x kW taps are shifted by a learned offset and
 * optionally reweighted by a learned modulator, bilinearly sampled with zero
 * padding outside the input. FP32 / FP16 (CPU FP32); every operand shares
 * X's dtype.
 *   offset: (N, deformGroups*2*kH*kW * H_out*W_out); channel
 *           g*(2*kH*kW) + 2*(kh*kW+kw) is the ROW (y) offset, +1 the COL (x).
 *   mask:   (N, deformGroups*kH*kW * H_out*W_out) or null (all 1).
 *   Wt:     OIHW (C_out, (C_in/groups)*kH*kW) like conv2dForward.
 * groups divides C_in and C_out; deformGroups divides C_in. H_out / W_out
 * follow the conv2d formula above.
 */
gpu.deformConv2dForward(X, offset, /*mask|null*/ null, Wt, /*bias|null*/ null,
                        N, C_in, H, W, C_out, kH, kW, sH, sW, pH, pW, dH, dW,
                        groups, deformGroups, Y);


// -----------------------------------------------------------------------------
// 2x up/downsample, pooling, pad / crop (NCHW)
// -----------------------------------------------------------------------------
//
// H and W are always the *input* dims. FP32 / FP16 dispatched on the input.

gpu.upsampleNearest2xForward(X,  N, C, H, W, Y);    // 2x nearest neighbour
gpu.upsampleNearest2xBackward(dY, N, C, H, W, dX);
gpu.upsampleBilinear2xForward(X,  N, C, H, W, Y);   // align_corners = false
gpu.upsampleBilinear2xBackward(dY, N, C, H, W, dX);
gpu.downsampleAvg2xForward(X,  N, C, H, W, Y);      // 2x2 average pool
gpu.downsampleAvg2xBackward(dY, N, C, H, W, dX);

/** Max pool. Idx is an INT32 tensor shaped like Y, carrying the argmax offsets. */
gpu.maxPool2dForward(X, N, C, H, W, kH, kW, sH, sW, padH, padW, Y, Idx);
gpu.maxPool2dBackward(dY, Idx, N, C, H, W, H_out, W_out, dX);   // overwrite

/** Adaptive average pool; the per-pixel region follows PyTorch's formula. */
gpu.adaptiveAvgPool2dForward(X, N, C, H, W, H_out, W_out, Y);
gpu.adaptiveAvgPool2dBackward(dY, N, C, H, W, H_out, W_out, dX);

/** Spatial pad / crop. pad2d mode: 0 zero, 1 reflect, 2 replicate. */
gpu.pad2dForward(X, N, C, H, W, padT, padB, padL, padR, /*mode*/ 0, Y);
gpu.pad2dBackward(dY, N, C, H, W, padT, padB, padL, padR, /*mode*/ 0, dX);
gpu.slice2dForward(X, N, C, H, W, h0, w0, H_out, W_out, Y);     // crop region
gpu.slice2dBackward(dY, N, C, H, W, h0, w0, H_out, W_out, dX);  // zero + scatter


// -----------------------------------------------------------------------------
// NCHW <-> sequence, resample, unfold, normalise (vision post-processing)
// -----------------------------------------------------------------------------

// Lets transformer-shaped ops consume tensors produced by NCHW primitives.
gpu.nchwToSequence(X, N, C, H, W, Y);   // (N, C*H*W)     -> (N*H*W, C)
gpu.sequenceToNchw(X, N, C, H, W, Y);   // (N*H*W, C)     -> (N, C*H*W)

/**
 * Resample H_in x W_in -> H_out x W_out with half-pixel mapping.
 *   mode 0 nearest, 1 bilinear, 2 bicubic a=-0.5 (PIL), 3 bicubic a=-0.75
 *   (torch / OpenCV). Bicubic is FP32-only; other modes throw.
 * The backward supports modes 0 and 1 only (bicubic upsamplers are
 * inference-only in practice) and OVERWRITES dX.
 */
gpu.interp2dForward(X, N, C, H_in, W_in, H_out, W_out, /*mode*/ 1, Y);
gpu.interp2dBackward(dY, N, C, H_in, W_in, H_out, W_out, /*mode*/ 1, dX);

/**
 * The corner-aligned mapping instead (torch align_corners=True), the
 * convention DPT / Depth-Anything fusion and final upsamples use. Modes 0/1/2,
 * forward only. Registered on CPU and CUDA; the Metal slot is intentionally null.
 */
gpu.interp2dAlignCornersForward(X, N, C, H_in, W_in, H_out, W_out, /*mode*/ 1, Y);

/**
 * Neighbourhood unfold (spatial-preserving im2col): each output pixel gathers
 * its kH x kW window into a channel block. mode: 0 zero, 1 reflect,
 * 2 replicate. Y: (N, C*kH*kW * H_out*W_out); stride 1 with pad (k-1)/2 keeps
 * the grid size.
 */
gpu.unfold2dForward(X, N, C, H, W, kH, kW, sH, sW, padT, padB, padL, padR,
                    /*mode*/ 0, Y);

/**
 * Per-pixel L2 normalise over the channel axis (a unit-length direction field):
 *   Y[n,:,h,w] = X[n,:,h,w] / max(||X[n,:,h,w]||, eps)
 * The per-head variant (l2NormForward) is in tensor-api.js.
 */
gpu.l2NormalizeNchwForward(X, N, C, H, W, /*eps*/ 1e-12, Y);

/**
 * RAFT-style convex (learned-mask) upsample: each low-res pixel expands to a
 * scale x scale block, every fine pixel a softmax-weighted blend of the 3x3
 * low-res neighbourhood. Mask: (N, 9*scale*scale*H*W), torch (N,9,k,k,H,W).
 * Y: (N, C*(scale*H)*(scale*W)).
 */
gpu.convexUpsampleForward(X, Mask, N, C, H, W, /*scale*/ 8, Y);

/**
 * Window partition / reverse (Swin, SAM). windowReverseForward is the exact
 * inverse — and the adjoint — of windowPartitionForward; H and W must be
 * multiples of `window`.
 */
gpu.windowPartitionForward(X, N, C, H, W, window, Y);
gpu.windowReverseForward(X, N, C, H, W, window, Y);

/**
 * 2x2 spatial token merge: (N,C,H,W) -> (N,4C,H/2,W/2). `channelMajor`
 * defaults to false = block-major, c_out = block*C + c_in (the Qwen-VL patch
 * merger); true gives c_out = c_in*4 + block (torch pixel_unshuffle, the
 * Flux.2 VAE tail).
 */
gpu.spatialMerge2x2Forward(X, N, C, H, W, Y, /*channelMajor*/ false);

/**
 * The DC-AE up-shortcut (diffusers DCUpBlock2d, interpolate mode): channel
 * repeat_interleave then a 2x pixel shuffle, fused into one gather:
 *   Y[n, c, 2h+i, 2w+j] = X[n, (4c + 2i + j) / repeats, h, w],
 *   repeats = 4*C_out/C_in (C_in must divide 4*C_out).
 * C_in === 4*C_out is a plain pixel shuffle; C_in === C_out a 2x nearest
 * upsample. X: (N, C_in*H*W); Y: (N, C_out*2H*2W). Inference-only.
 */
gpu.pixelShuffleUpsample2xForward(X, N, C_in, H, W, C_out, Y);

/**
 * DiT unpatchify: token rows back to an NCHW image, dropping trailing
 * channels (PixArt's learned-variance half).
 *   tokens: (hp*wp, P*P*C_total), row i*wp + j is grid cell (i, j). Within a
 *           row, channelMajor=false reads col = block*C_total + c, true reads
 *           col = c*P*P + block, with block = py*P + px.
 *   Y:      (1, C_keep*(hp*P)*(wp*P)); channels [C_keep, C_total) dropped.
 */
gpu.patchUnpackForward(tokens, hp, wp, P, C_total, C_keep, /*channelMajor*/ false, Y);


// -----------------------------------------------------------------------------
// BatchNorm + image preprocessing
// -----------------------------------------------------------------------------

/**
 * Training forward: runningMean / runningVar are updated in place, and
 * savedMean / savedRstd ((C,1) each) feed the backward. eps defaults to 1e-5,
 * momentum to 0.1.
 */
gpu.batchNormForward(X, gamma, beta, runningMean, runningVar,
                     N, C, H, W, /*eps*/ 1e-5, /*momentum*/ 0.1,
                     Y, savedMean, savedRstd);
/** dX is overwritten; dGamma / dBeta ACCUMULATE — zero them first. */
gpu.batchNormBackward(X, gamma, savedMean, savedRstd, dY,
                      N, C, H, W, dX, dGamma, dBeta);
/** Inference: the frozen running stats, no update, no caches. */
gpu.batchNormInference(X, gamma, beta, runningMean, runningVar,
                       N, C, H, W, /*eps*/ 1e-5, Y);

/** Per-channel (X - mean[c]) / std[c] on NCHW; mean / std are (C,1). */
gpu.imageNormalize(X, mean, std, N, C, H, W, Y);

/**
 * Packed uint8 HWC bytes (a decoder's output, a host Uint8Array of N*H*W*C
 * bytes) into FP32 NCHW in one scale+bias pass: Y = src*scale + bias.
 *   [0,255] -> [0,1]:   scale 1/255, bias 0
 *   [0,255] -> [-1,1]:  scale 2/255, bias -1
 */
gpu.imageU8ToF32NhwcToNchw(srcUint8, N, H, W, C, /*scale*/ 1 / 255, /*bias*/ 0, Y);


// -----------------------------------------------------------------------------
// ResBlock (fused SD U-Net residual block)
// -----------------------------------------------------------------------------
//
// An options object: too many optional tensors for a positional call.
//
// Required: X, gamma1, beta1, W1, gamma2, beta2, W2, Y, N, C_in, C_out, H, W
// Optional: b1, t_emb_shift, b2, Wskip, bskip, numGroups (32), eps (1e-5)
//
// Forward:
//   h = silu(group_norm(X, gamma1, beta1))
//   h = conv2d_3x3_same(h, W1, b1)
//   if (t_emb_shift) h += broadcast(t_emb_shift)
//   h = silu(group_norm(h, gamma2, beta2))
//   h = conv2d_3x3_same(h, W2, b2)
//   Y = h + (C_in === C_out ? X : conv2d_1x1(X, Wskip, bskip))

gpu.resblockForward({
    X, gamma1, beta1, W1, b1, t_emb_shift,
    gamma2, beta2, W2, b2,
    Wskip: null, bskip: null,
    N, C_in, C_out, H, W, numGroups: 32, eps: 1e-5,
    Y,
});

/**
 * ResBlock backward: recomputes the forward intermediates internally, so pass
 * the same forward inputs plus dY and the gradient slots. dX is overwritten;
 * the weight / gamma / beta gradients accumulate. db1, dt_emb_shift, db2,
 * dWskip and dbskip are optional and written only when given.
 */
gpu.resblockBackward({
    X, gamma1, beta1, W1, b1, t_emb_shift,
    gamma2, beta2, W2, b2, Wskip, bskip,
    N, C_in, C_out, H, W, numGroups: 32, eps: 1e-5,
    dY, dX,
    dGamma1, dBeta1, dW1, db1, dt_emb_shift,
    dGamma2, dBeta2, dW2, db2,
    dWskip, dbskip,
});


// -----------------------------------------------------------------------------
// Diffusion sampler steps
// -----------------------------------------------------------------------------
//
// The scheduler keeps its alpha / sigma / log-SNR coefficients host-side; these
// kernels just apply one elementwise step. x_t and eps_pred share a shape and
// the outputs are resized to match.

/**
 *   x0_pred = (x_t - sqrt(1-alpha_t)*eps_pred) / sqrt(alpha_t)
 *   x_prev  = sqrt(alpha_prev)*x0_pred + sqrt(1-alpha_prev-sigma_t^2)*eps_pred
 * sigma_t = 0 is deterministic DDIM.
 */
gpu.ddimStep(x_t, eps_pred, alpha_t, alpha_prev, sigma_t, x_prev);

/**
 * Euler: x_prev = x_t + (sigma_prev - sigma_t) * eps_pred. The kernel never
 * interprets eps_pred, so this covers both the eps / k-diffusion derivative
 * form and flow-matching velocity.
 */
gpu.eulerStep(x_t, eps_pred, sigma_t, sigma_prev, x_prev);

/**
 * DPM-Solver++ 2M. The caller keeps a running x0 cache; the coefficients come
 * from the scheduler host-side.
 *   x0_t   = x_t - sigma_t*eps_pred
 *   x_prev = c_xt*x_t + c_x0t*x0_t + c_x0prev*x0_prev
 *   x0_out = x0_t   (copy into x0_prev for the next step)
 * The first step has no x0_prev: use eulerStep for it.
 */
gpu.dpmpp2mStep(x_t, eps_pred, x0_prev, sigma_t,
                c_xt, c_x0t, c_x0prev, x_prev, x0_out);

/**
 * Sinusoidal timestep embedding, cos half first (the diffusers
 * flip_sin_to_cos=true layout, SD/SDXL default).
 *   timesteps: (N, 1) FP32.  Y: (N, dim) FP32.  maxPeriod defaults to 10000.
 */
gpu.timestepEmbedding(timesteps, dim, /*maxPeriod*/ 10000, Y);


// -----------------------------------------------------------------------------
// StyleGAN3-R generator primitives
// -----------------------------------------------------------------------------
//
// modulated conv, upfirdn2d, bias_act and filtered_lrelu, mirroring the NVlabs
// `_ref` implementations. NCHW packed (N, C*H*W); FP32 / FP16 / BF16 with FP32
// math (CPU FP32). The Fourier-feature sin / cos, the demod rsqrt and the
// mapping network's pixelNorm are in tensor-api.js.

/**
 * Fused per-channel bias + activation + gain + clamp:
 *   t = X + b[c];  y = gain * act(t);  clamp >= 0 clips y to [-clamp, clamp].
 *   X: (N, C*HW).  b: (C, 1) or null.  act: 0 linear, 1 leaky ReLU (alpha).
 *   Defaults: alpha 0.2, gain sqrt(2) for lrelu (1 for linear), clamp -1 (off).
 * The backward overwrites dX and ACCUMULATES dB (caller zeros; null skips);
 * the gradient is cut where the pre-clamp |y| exceeded clamp.
 */
gpu.biasActForward(X, /*b|null*/ b, N, C, HW, /*act*/ 1, /*alpha*/ 0.2, Math.SQRT2, /*clamp*/ -1, Y);
gpu.biasActBackward(dY, X, b, N, C, HW, 1, 0.2, Math.SQRT2, -1, dX, /*dB|null*/ dB);

/**
 * upfirdn2d: zero-insert upsample -> pad / crop (negative pads crop) -> 2D FIR
 * with the constant depthwise filter f (fH, fW), same dtype as X -> downsample
 * -> gain.  flipFilter false = true convolution, true = plain correlation.
 *   H_out = (H*upY + padY0 + padY1 - fH) / downY + 1   (W_out likewise)
 * The backward takes the SAME forward arguments (H, W the forward INPUT dims)
 * and overwrites dX (N, C*H*W). No gradient reaches f.
 */
gpu.upfirdn2dForward(X, f, N, C, H, W, fH, fW, upX, upY, downX, downY,
                     padX0, padX1, padY0, padY1, /*flipFilter*/ false, /*gain*/ 4, Y);
gpu.upfirdn2dBackward(dY, f, N, C, H, W, fH, fW, upX, upY, downX, downY,
                      padX0, padX1, padY0, padY1, false, 4, dX);

/**
 * The synthesis-layer conv: per-sample style modulation of shared weights,
 * optional demodulation, then a stride-1 conv2d per sample.
 *   X: (N, C_in*H*W).  W: (C_out, C_in*kH*kW).  s: (N, C_in) styles.
 *   w' = W * s[n];  dcoef[n,o] = demodulate ? rsqrt(sum w'^2 + eps) : 1
 *   dcoef: (N, C_out) FP32 out, kept for the backward.
 *   H_out = H + 2*padH - (kH-1)   (W_out likewise).
 * Backward: dX and ds overwritten; dW ACCUMULATES (caller zeros) — or pass
 * null to skip the weight gradient entirely (inversion with frozen weights).
 */
gpu.modulatedConv2dForward(X, W, s, N, C_in, H, Wd, C_out, kH, kW, padH, padW,
                           /*demodulate*/ true, /*eps*/ 1e-8, dcoef, Y);
gpu.modulatedConv2dBackward(X, W, s, dcoef, dY, N, C_in, H, Wd, C_out, kH, kW,
                            padH, padW, true, 1e-8, dX, /*dW|null*/ dW, ds);

/**
 * The alias-free nonlinearity: bias -> upfirdn2d(fu, up, pads, gain up^2) ->
 * leaky ReLU (gain, slope, clamp) -> upfirdn2d(fd, down). CUDA runs a fused
 * kernel; elsewhere (and for configs it does not cover) the composite.
 *   X: (N, C*H*W).  fu / fd: the up / down filters.  b: (C, 1) or null.
 *   upBuf / actBuf: cache outputs (post-upsample / post-lrelu). The fused
 *   kernel may leave them empty.
 *   out_w = (W*up + padX0 + padX1 - (fuW-1) - (fdW-1) + (down-1)) / down.
 * The backward chains the sub-backwards: dX overwritten, dB accumulated
 * (null skips). upBuf is optional — null or an empty tensor recomputes it
 * from X.
 */
gpu.filteredLreluForward(X, fu, fd, /*b|null*/ null, N, C, H, W, /*up*/ 2, /*down*/ 2,
                         padX0, padX1, padY0, padY1, /*gain*/ Math.SQRT2, /*slope*/ 0.2,
                         /*clamp*/ 256, upBuf, actBuf, Y);
gpu.filteredLreluBackward(dY, X, fu, fd, null, N, C, H, W, 2, 2,
                          padX0, padX1, padY0, padY1, Math.SQRT2, 0.2, 256,
                          /*upBuf|null*/ upBuf, dX, /*dB|null*/ null);


// -----------------------------------------------------------------------------
// LSTM (training forward + full BPTT)
// -----------------------------------------------------------------------------
//
// A single-layer, single-direction LSTM over a length-T sequence of batch B,
// PyTorch nn.LSTM weight layout and gate order [input, forget, cell, output].
// FP32; CPU, CUDA and Metal. Bidirectional / stacked networks are built by
// wrapping the cell (reverse the sequence; feed one layer's Y to the next).
//   X: (T*B, I), row t*B + b.   W_ih: (4H, I).   W_hh: (4H, H).
//   b_ih / b_hh: (4H, 1) or null.   h0 / c0: (B, H) or null (zeros).
//   Y: (T*B, H) hidden states.   gates: (T*B, 4H) post-activation [i|f|g|o]
//   and C: (T*B, H) cell states are the backward's cache — pass them back
//   unchanged.   hT / cT: (B, H) final states, or null.

gpu.lstmForwardTrain(X, W_ih, W_hh, /*b_ih*/ null, /*b_hh*/ null, /*h0*/ null, /*c0*/ null,
                     T, B, Y, gates, C, /*hT|null*/ hT, /*cT|null*/ cT);

/**
 * BPTT. dY: (T*B, H) upstream (zero all but the last step for a last-step
 * loss). dX (T*B, I) is overwritten; dW_ih (4H, I), dW_hh (4H, H), db_ih and
 * db_hh (4H, 1) ACCUMULATE (caller zeros; the two bias grads are equal, as in
 * nn.LSTM). dh0 / dc0 (B, H) are overwritten when given. db_* / dh0 / dc0 may
 * be null.
 */
gpu.lstmBackward(X, W_ih, W_hh, null, null, Y, gates, C, dY, T, B,
                 dX, dW_ih, dW_hh, /*db_ih|null*/ db_ih, /*db_hh|null*/ db_hh,
                 /*dh0|null*/ null, /*dc0|null*/ null);


// -----------------------------------------------------------------------------
// INT8 weight-only quantisation (W8A16)
// -----------------------------------------------------------------------------
//
// Activations stay FP16; weights are per-output-row symmetric INT8 with FP32
// dequant scales. Inference only — there is no backward, the weights are
// frozen. GPU-only, except the host quantiser.

/**
 * Host helper, no device involved. Quantises an FP16 weight matrix (a
 * Uint16Array of IEEE binary16 bit patterns) to INT8 plus per-output-row FP32
 * scales:  scale[row] = max(|w|)/127 (0 for an all-zero row), and
 * w_q = clamp(round(w/scale), -127, 127).
 * @returns {{weights: Int8Array, scales: Float32Array}} fresh copies, lengths
 *   out*in and out. Upload `weights` with GpuTensor#uploadInt8 and `scales`
 *   into an FP32 (out, 1) tensor.
 */
const { weights, scales } = gpu.quantizeInt8PerRowHost(W_fp16_u16, out, inDim);

/** Y(out,B) = dequant(W_int8, scales)(out,in) @ X(in,B); X / Y FP16. */
gpu.matmulInt8wFp16(W_int8, scales, X, Y);

/** Y_BD(B,out) = X_BD(B,in) @ dequant(W_int8)^T + bias. */
gpu.linearForwardBatchedInt8wFp16(W_int8, scales, /*bias|null*/ null, X_BD, Y_BD);

/** W8A16 conv2d / conv3d, same shape contract as conv2dForward / conv3dForward. */
gpu.conv2dInt8wFp16Forward(X, W_int8, scales, /*bias|null*/ null,
                           N, C_in, H, W, C_out, kH, kW,
                           sH, sW, pH, pW, dH, dW, groups, Y);
gpu.conv3dInt8wFp16Forward(X, W_int8, scales, /*bias|null*/ null,
                           N, C_in, T, H, W, C_out, kT, kH, kW,
                           sT, sH, sW, pT, pH, pW, dT, dH, dW, groups, Y);

/**
 * W8A16 ResBlock forward: the same options shape as resblockForward, with each
 * conv weight replaced by its INT8 tensor + FP32 scales pair.
 * Required: X, gamma1, beta1, W1_int8, s1, gamma2, beta2, W2_int8, s2, Y,
 *           N, C_in, C_out, H, W
 * Optional: b1, t_emb_shift, b2, Wskip_int8, sskip, bskip, numGroups (32),
 *           eps (1e-5)
 */
gpu.resblockForwardInt8wFp16({
    X, gamma1, beta1, W1_int8, s1, b1, t_emb_shift,
    gamma2, beta2, W2_int8, s2, b2,
    Wskip_int8: null, sskip: null, bskip: null,
    N, C_in, C_out, H, W, numGroups: 32, eps: 1e-5, Y,
});

/** W8A16 KV projection: ctx through (Wk, sk, bk?) and (Wv, sv, bv?). */
gpu.flashAttentionProjectKvInt8wFp16(ctx, Wk_int8, sk, /*bk|null*/ null,
                                          Wv_int8, sv, /*bv|null*/ null,
                                          K_out, V_out);

/** W8A16 Q-with-pre-projected-KV flash attention. */
gpu.flashAttentionQWithKvCachedInt8wFp16(X, K, V,
                                         Wq_int8, sq, /*bq|null*/ null,
                                         Wo_int8, so, /*bo|null*/ null,
                                         /*mask|null*/ null,
                                         numHeads, /*causal*/ false, O);

/**
 * W8A16 fused-QKVO flash attention (options object).
 * Required: X, Wq_int8, sq, Wk_int8, sk, Wv_int8, sv, Wo_int8, so, O
 * Optional: Ctx, bq, bk, bv, bo, mask, numHeads (1), causal (false)
 */
gpu.flashAttentionQkvoInt8wFp16({
    X, Ctx: null,
    Wq_int8, sq, bq: null,
    Wk_int8, sk, bk: null,
    Wv_int8, sv, bv: null,
    Wo_int8, so, bo: null,
    mask: null, numHeads, causal: false, O,
});

/**
 * W8A16 twin of selfAttentionBiasForward — the quantised T5 encoder attention.
 * Each (D,D) INT8 weight carries an FP32 (D,1) per-output-row scale;
 * activations stay FP16. attnBias is an optional (numHeads*L, L) FP32 additive
 * pre-softmax bias, `scale` the pre-bias QK multiplier (1.0 for T5).
 */
gpu.selfAttentionBiasInt8wFp16(X, Wq_int8, sq, Wk_int8, sk,
                               Wv_int8, sv, Wo_int8, so,
                               /*mask|null*/ null, /*attnBias|null*/ bias,
                               numHeads, /*scale*/ 1.0, O);


// -----------------------------------------------------------------------------
// GGUF k-quant weight inference (Q4_K / Q6_K / Q8_0)
// -----------------------------------------------------------------------------
//
// Either dequantise a block-quantised weight to FP16 once (for tests, or when
// one weight is reused across many matmuls), or run a weight-only-quant linear
// with the weight left k-quant and X / Y FP16. GPU-only.

gpu.dequantQ4kToFp16(W_q4k, W_fp16);
gpu.dequantQ6kToFp16(W_q6k, W_fp16);
gpu.dequantQ8_0ToFp16(W_q8_0, W_fp16);

// Single token: x is (in,1) FP16, y is (out,1) FP16; bias optional.
gpu.linearForwardQ4kFp16(W_q4k, /*bias|null*/ null, x, y);
gpu.linearForwardQ6kFp16(W_q6k, /*bias|null*/ null, x, y);
gpu.linearForwardQ8_0Fp16(W_q8_0, /*bias|null*/ null, x, y);

// Batched: X_BD is (B,in) FP16, Y_BD is (B,out) FP16.
gpu.linearForwardBatchedQ4kFp16(W_q4k, /*bias|null*/ null, X_BD, Y_BD);
gpu.linearForwardBatchedQ6kFp16(W_q6k, /*bias|null*/ null, X_BD, Y_BD);
gpu.linearForwardBatchedQ8_0Fp16(W_q8_0, /*bias|null*/ null, X_BD, Y_BD);


// =============================================================================
// Audio / codec ops
// =============================================================================
//
// The building blocks of Whisper / STT, TTS, neural-codec and vocoder models.
// Backend-neutral: CPU, CUDA and Metal each have a kernel (the W8A16 conv1d
// excepted).
//
// Complex tensor layout: there is no complex dtype. A complex spectrum of C
// bins over R rows is an ordinary FP32 (R, 2*C) tensor with the bin axis
// interleaved [re, im, re, im, ...]; real tensors keep the natural (R, C).


// -----------------------------------------------------------------------------
// Spectral / FFT core
// -----------------------------------------------------------------------------
//
// One signal per row, "backward" normalisation (the numpy default): the
// forward transform is unscaled, the inverse scaled by 1/N. fft / ifft have no
// explicit backward — the adjoint of one is the other times a scalar. rfft and
// irfft DO have backwards: their adjoints carry bin weighting that is easy to
// get wrong by hand.

gpu.fft(x, y);              // complex (R,2*N) -> complex (R,2*N)
gpu.ifft(x, y);             // complex -> complex, scaled by 1/N
gpu.rfft(x, y);             // real (R,len) -> half spectrum (R, 2*(len/2+1))
gpu.irfft(x, len, y);       // half spectrum -> real (R,len); len disambiguates
gpu.rfftBackward(dY, len, dX);  // adjoint of rfft; dX real (R,len)
gpu.irfftBackward(dY, dX);      // adjoint of irfft; dX complex

gpu.complexMul(a, b, y);                    // y = a*b per bin
gpu.complexMulBackward(a, b, dY, dA, dB);   // dA/dB accumulate (pre-zero them)
gpu.complexAbs(z, y);                       // y = |z|, REAL (R,C)
gpu.complexAbsBackward(z, dY, dZ);          // dZ overwritten
gpu.complexAngle(z, y);                     // y = atan2(im, re); no backward
gpu.complexFromPolar(mag, phase, y);        // y = mag * exp(i*phase)


// -----------------------------------------------------------------------------
// STFT / iSTFT
// -----------------------------------------------------------------------------
//
// A length-L real signal is one row of an (N, L) tensor; the spectrogram is
// interleaved-complex (N*frames, 2*(nFft/2+1)) with each signal's frame block
// stacked in order. `window` is a real (1, winLength) tensor, winLength <= nFft.
// `center` reflect-pads the signal by nFft/2 each side (torch center=true);
// `normalized` also scales by 1/sqrt(nFft). stft and istft are linear but NOT
// mutual adjoints once the window and COLA normalisation are folded in, so
// both backwards are explicit.

gpu.stft(signal, window, N, nFft, hopLength, winLength, center, normalized, spec);
gpu.istft(spec, window, N, signalLen, nFft, hopLength, winLength,
          center, normalized, signal);
gpu.stftBackward(dSpec, window, N, signalLen, nFft, hopLength, winLength,
                 center, normalized, dSignal);
gpu.istftBackward(dSignal, window, N, signalLen, nFft, hopLength, winLength,
                  center, normalized, dSpec);


// -----------------------------------------------------------------------------
// 1D convolution family (NCL)
// -----------------------------------------------------------------------------
//
// Activations are (N, C*L) — NCHW with the height axis dropped — and weights
// are OIL: (C_out, (C_in/groups)*kL). Defaults for the tail: stride 1,
// padding 0, dilation 1, groups 1, outputPadding 0.
//
// pad1d mode: 0 zero, 1 reflect, 2 replicate.

gpu.pad1dForward(X, N, C, L, padLeft, padRight, /*mode*/ 0, Y);
gpu.pad1dBackward(dY, N, C, L, padLeft, padRight, /*mode*/ 0, dX);

gpu.conv1d(X, Wt, /*bias|null*/ null, N, C_in, L, C_out, kL,
           stride, padding, dilation, groups, Y);
gpu.conv1dBackwardInput(Wt, dY, N, C_in, L, C_out, kL,
                        stride, padding, dilation, groups, dX);    // overwrite
gpu.conv1dBackwardWeight(X, dY, N, C_in, L, C_out, kL,
                         stride, padding, dilation, groups, dWt);  // accumulate
gpu.conv1dBackwardBias(dY, N, C_out, L_out, dB);                   // accumulate

/** W8A16 1D conv: FP16 activations, OIL INT8 weights, (C_out,1) FP32 scales. */
gpu.conv1dInt8wFp16(X, W_int8, scales, /*bias|null*/ null, N, C_in, L, C_out, kL,
                    stride, padding, dilation, groups, Y);

/**
 * 1D transposed convolution, the upsampling primitive of every neural vocoder
 * (HiFi-GAN, EnCodec / DAC decoders). Weights are input-channel-major:
 * (C_in, (C_out/groups)*kL). `outputPadding` (< stride) disambiguates the
 * output length, exactly torch's ConvTranspose1d argument.
 */
gpu.convTranspose1dForward(X, Wt, /*bias|null*/ null, N, C_in, L, C_out, kL,
                           stride, padding, outputPadding, dilation, groups, Y);
gpu.convTranspose1dBackwardInput(Wt, dY, N, C_in, L, C_out, kL, stride, padding,
                                 outputPadding, dilation, groups, dX);
gpu.convTranspose1dBackwardWeight(X, dY, N, C_in, L, C_out, kL, stride, padding,
                                  outputPadding, dilation, groups, dWt);
gpu.convTranspose1dBackwardBias(dY, N, C_out, L_out, dB);

/**
 * Causal 1D conv: left-pads the length axis by dilation*(kL-1) then runs a
 * valid conv1d, so every output sample depends only on inputs at or before it.
 * `scratch` is a caller-owned GpuTensor reused as the left-padded input buffer
 * (resized internally), which keeps the call allocation-free.
 */
gpu.causalConv1d(X, Wt, /*bias|null*/ null, N, C_in, L, C_out, kL,
                 stride, dilation, groups, scratch, Y);

/**
 * One streaming step of a causal depthwise conv against a rolling state cache
 * (streaming Whisper, real-time vocoders, Mamba's short conv). C channels in
 * and out, one length-kL filter per channel.
 *   X, Y:  (N, C*L_step)                 new samples in / outputs out
 *   Wt:    (C, kL)                       depthwise filter, one row per channel
 *   state: (N, C*(kL-1)*dilation)        rolling history, read AND overwritten
 *                                        (the caller zero-initialises it)
 */
gpu.causalConv1dUpdate(X, Wt, /*bias|null*/ null, N, C, L_step, kL, dilation,
                       state, Y);


// -----------------------------------------------------------------------------
// Vocoder / codec activations (NCL)
// -----------------------------------------------------------------------------

/**
 * Snake activation (BigVGAN / DAC). Per-channel learnable alpha and optional
 * beta, broadcast across the (n, l) plane:
 *   beta === null:  y = x + (1/alpha_c)*sin^2(alpha_c*x)
 *   beta !== null:  y = x + (1/beta_c) *sin^2(alpha_c*x)
 *   alpha / beta: (C,1) or (1,C). dAlpha / dBeta accumulate (pre-zero them),
 *   and dBeta must be non-null exactly when beta is.
 */
gpu.snakeForward(X, alpha, /*beta|null*/ null, N, C, L, Y);
gpu.snakeBackward(X, alpha, /*beta|null*/ null, dY, N, C, L,
                  dX, dAlpha, /*dBeta|null*/ null);

gpu.eluForward(x, /*alpha*/ 1.0, y);              // EnCodec: x>0 ? x : a*(e^x-1)
gpu.eluBackward(x, dY, /*alpha*/ 1.0, dX);
gpu.leakyReluForward(x, /*negativeSlope*/ 0.01, y);   // HiFi-GAN
gpu.leakyReluBackward(x, dY, /*negativeSlope*/ 0.01, dX);


// -----------------------------------------------------------------------------
// Codec quantisation
// -----------------------------------------------------------------------------
//
// The bottlenecks of neural audio codecs. Both backwards are the
// straight-through estimator: a plain identity passthrough (dX = dQuantized).

/**
 * Vector-quantisation encode (VQ-VAE / residual VQ). Each input row picks the
 * L2-nearest codeword, emitting its index and a copy of the codeword.
 *   x:         (N, D) FP32 input rows
 *   codebook:  (K, D) FP32 codewords
 *   indices:   (N, 1), output, dtype-set to INT32
 *   quantized: (N, D) FP32, output, codebook[indices[n]] per row
 */
gpu.vqEncodeForward(x, codebook, indices, quantized);
gpu.vqEncodeBackward(dQuantized, dX);

/**
 * Finite Scalar Quantisation (NanoCodec). Each coordinate snaps to one of L_d
 * evenly spaced levels; the per-dim indices are packed mixed-radix.
 *   x:             (N, D) FP32, assumed pre-bounded into [-1, 1]
 *   levels:        (D, 1) INT32 per-dimension level count (each >= 2)
 *   quantized:     (N, D) FP32, output, dequantised values in [-1, 1]
 *   packedIndices: (N, 1) INT32, output, the packed code per row
 */
gpu.fsqQuantizeForward(x, levels, quantized, packedIndices);
gpu.fsqQuantizeBackward(dQuantized, dX);


// -----------------------------------------------------------------------------
// 1D resampling, log / exp / round
// -----------------------------------------------------------------------------

/**
 * Arbitrary-scale resampling along the length axis of an NCL tensor (sample
 * rate conversion), align_corners=false. mode: 0 nearest, 1 linear. The
 * backward is the exact adjoint and overwrites dX.
 */
gpu.resample1dForward(X, N, C, L_in, L_out, /*mode*/ 1, Y);
gpu.resample1dBackward(dY, N, C, L_in, L_out, /*mode*/ 1, dX);

// FP32 elementwise scalar maps (log-mel spectrograms, log-domain losses). log
// and exp backward read the raw forward input x; round backward is the
// straight-through estimator and needs only dY. All backwards overwrite dX, and
// the x > 0 precondition for log is the caller's — it is not guarded.
gpu.logForward(x, y);      gpu.logBackward(x, dY, dX);
gpu.expForward(x, y);      gpu.expBackward(x, dY, dX);
gpu.roundForward(x, y);    gpu.roundBackward(dY, dX);   // round-half-to-even


// -----------------------------------------------------------------------------
// Autoregressive logit sampling
// -----------------------------------------------------------------------------

/**
 * Per-row next-token sampler over an (N, V) logit matrix. Applies, in order:
 * temperature scaling, softmax, top-k filter, top-p / nucleus filter,
 * renormalise, inverse-CDF draw. temperature === 0 is deterministic argmax.
 * `key` / `counter` seed the Philox 4x32-10 RNG — plain Numbers or BigInts,
 * not tensors. `indices` comes back (N, 1) INT32.
 */
gpu.sampleLogits(logits, /*temperature*/ 1.0, /*topK*/ 0, /*topP*/ 1.0,
                 key, counter, indices);

/**
 * The graph-capturable twin: the same draw, but every RNG-state and output
 * buffer is a caller-owned pre-sized GpuTensor touched only on-device, so a
 * whole decode step can be recorded into a CUDA graph and replayed with one
 * launch (no host counter read/write, no mid-capture allocation).
 *   counter: (>=1,) INT32 — counter[0] is the base offset, advanced in place by
 *            N each call, so every replay draws fresh values.
 *   scratch: FP32 with >= 3*N*V elements, sized once and reused.
 *   indices: (N, 1) INT32, pre-sized by the caller, written in place, never
 *            resized.
 */
gpu.sampleLogitsInto(logits, /*temperature*/ 1.0, /*topK*/ 0, /*topP*/ 1.0,
                     key, counterTensor, scratch, indices);


// -----------------------------------------------------------------------------
// Masked-diffusion token selection (OmniVoice-style codebook grids)
// -----------------------------------------------------------------------------
//
// One step of a masked-diffusion LM over a (C codebooks, T frames) grid with
// vocabulary V, where token id `maskId` marks a still-masked cell. Scores fuse
// classifier-free guidance, log-softmax, the per-cell prediction and the
// confidence every masked cell competes with; the host picks the k best cells
// (topKRows over `scores` viewed as one (1, C*T) row) and Commit writes them.
// FP32 only; CPU, CUDA and Metal. Noise is a counter hash of (seed, cell), so
// the same inputs and seed always give the same result on every backend.

/**
 * logits: (R, C*V) FP32, column c*V + v; R = 2T with guidance (rows [0,T)
 *         conditional, [T,2T) unconditional), else T.
 * tokens: (C, T) INT32 current grid (or whole-number FP32).
 * pred:   (C, T) INT32 out — the predicted id for every cell.
 * scores: (C, T) FP32 out — the selection score; -Infinity where the cell is
 *         already decided.
 * confidence: (C, T) FP32 out — the raw max log-prob of EVERY cell (no layer
 *         penalty, temperature, noise or masking), for per-step logging.
 * opts: { guidanceScale = 0, layerPenalty = 0 (score -= c*layerPenalty),
 *         positionTemperature = 0 (> 0 adds Gumbel noise to scores),
 *         classTemperature = 0 (> 0 samples pred from the top classTopFrac*V
 *         classes instead of argmax), classTopFrac = 1, seed = 0 (Number or
 *         BigInt) }
 */
gpu.maskedDiffusionScores(logits, tokens, T, C, V, maskId,
                          { guidanceScale: 2.0, positionTemperature: 5.0, seed: step },
                          pred, scores, confidence);

/**
 * Commit k chosen cells: for i < k, p = idx[i]: tokens[p] = pred[p],
 * unmaskStep[p] = step. idx holds flat cell indices c*T + t (topKRows' Idx);
 * an index outside [0, C*T) is ignored. pred / tokens / unmaskStep are
 * pre-sized (C, T) INT32 tensors (tokens and unmaskStep updated in place).
 */
gpu.maskedDiffusionCommit(pred, idx, k, step, tokens, unmaskStep);
