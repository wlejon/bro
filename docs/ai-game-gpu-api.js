// =============================================================================
// bro.ai.game.nn.gpu — CUDA-backed neural-net primitives
// =============================================================================
//
// CUDA backend for brogameagent's neural-net layer. Mirrors the CPU API on
// `bro.ai.game.nn` over device-resident GpuTensor buffers, plus a few extra
// ops (sigmoid, layernorm, single/multi-head attention, masked mean pool,
// MSE/cross-entropy losses, embedding lookup, concat/split, SGD/Adam, and
// batched inference variants).
//
// Availability:
//   bro.ai.game.nn.gpu.available
// is `true` only when bro was built against a brogameagent that defined
// BGA_HAS_CUDA=1 (i.e. configured with -DBROGAMEAGENT_WITH_CUDA=ON). When
// the flag is `false`, the rest of the namespace is absent — guard everything
// behind the `available` check.
//
// Synchronisation model:
//   Every op is launched on the default CUDA stream and is asynchronous on
//   the GPU. Before reading results back to the host, call
//     gpu.sync();
//   or use gpu.<tensor>.download(...), which auto-syncs internally.
//
// Mask convention:
//   Wherever the CPU API takes a Float32Array mask (host pointer), the GPU
//   API takes a *device* mask: pass either `null`/`undefined` (no mask) OR a
//   GpuTensor whose `.data` is the device pointer. Passing a Float32Array
//   throws TypeError. This avoids per-call host→device copies.
//
// =============================================================================


// -----------------------------------------------------------------------------
// Runtime
// -----------------------------------------------------------------------------

/**
 * `true` when the GPU backend is compiled in. Always check before using any
 * other `gpu.*` symbol — otherwise you'll get `undefined` accesses.
 * @type {boolean}
 */
bro.ai.game.nn.gpu.available;

/**
 * Idempotent. Selects CUDA device 0 (or the index in the env var
 * BGA_CUDA_DEVICE if set). Safe to call multiple times. Most ops auto-init,
 * but it's fine to call once at startup to surface device-init errors early.
 */
bro.ai.game.nn.gpu.init();

/**
 * Wraps cudaDeviceSynchronize. Blocks until all queued kernels on the default
 * stream have completed. Throws on CUDA error.
 */
bro.ai.game.nn.gpu.sync();


// -----------------------------------------------------------------------------
// GpuTensor — device-resident float32 buffer
// -----------------------------------------------------------------------------

/**
 * Allocate an owning device tensor of shape (rows, cols), filled with
 * uninitialised storage. Call `.zero()` if you need zeros.
 * @param {number} rows
 * @param {number} [cols=1]
 * @returns {GpuTensor}
 */
const t = bro.ai.game.nn.gpu.createTensor(3, 4);

t.rows;          // 3
t.cols;          // 4
t.size;          // 12

t.zero();        // cudaMemset to 0
t.resize(2, 6);  // reallocates if shape differs; contents undefined after
const dup = t.clone();  // owning device-side copy

/**
 * Copy data from host into this tensor.
 *
 * src may be either:
 *   - an AITensor (host nn::Tensor), copied directly to device
 *   - a Float32Array, uploaded as either (rows, cols) (if shape matches the
 *     length) or (length, 1) otherwise (this tensor is resized to match)
 *
 * @param {AITensor|Float32Array} src
 */
t.upload(new Float32Array([1, 2, 3, 4, 5, 6]));

/**
 * Copy data from this tensor back to host. Always issues a `gpu.sync()`
 * first.
 *
 * Modes:
 *   - download() with no args: returns a fresh Float32Array (host copy).
 *   - download(dst): dst is an AITensor; copies into it (resized if needed)
 *     and returns undefined.
 *
 * @param {AITensor} [dst]
 * @returns {Float32Array | undefined}
 */
const arr = t.download();
const ht = bro.ai.game.nn.createTensor(t.rows, t.cols);
t.download(ht);


// -----------------------------------------------------------------------------
// Dense + elementwise (per-vector)
// -----------------------------------------------------------------------------

/**
 * y = W*x + b. Shapes: W=(out,in), b=(out,1), x=(in,1), y=(out,1) (resized).
 * @param {GpuTensor} W
 * @param {GpuTensor} b
 * @param {GpuTensor} x
 * @param {GpuTensor} y
 */
gpu.linearForward(W, b, x, y);

/**
 * Linear backward. dW and dB are *accumulated into* — caller must zero them
 * between minibatches. dX is overwritten.
 */
gpu.linearBackward(W, x, dY, dX, dW, dB);

gpu.reluForward(x, y);
gpu.reluBackward(x, dY, dX);

gpu.tanhForward(x, y);
gpu.tanhBackward(y, dY, dX);   // takes cached forward output `y`, not `x`

gpu.sigmoidForward(x, y);
gpu.sigmoidBackward(y, dY, dX); // same: pass cached `y`

gpu.addInplace(y, x);              // y[i] += x[i]
gpu.addScalarInplace(y, 0.5);      // y[i] += s


// -----------------------------------------------------------------------------
// Softmax
// -----------------------------------------------------------------------------

/**
 * Numerically stable softmax over a flat length-N tensor.
 *
 * @param {GpuTensor} logits
 * @param {GpuTensor} probs
 * @param {GpuTensor|null} [mask] - optional length-N device mask (1 valid,
 *    0 invalid). MUST be a GpuTensor or null (NOT a Float32Array — the GPU
 *    op needs a device pointer).
 */
gpu.softmaxForward(logits, probs, null);
gpu.softmaxForward(logits, probs, deviceMaskTensor);

/** Full Jacobian softmax backward. dLogits resized to match. */
gpu.softmaxBackward(probs, dProbs, dLogits);


// -----------------------------------------------------------------------------
// LayerNorm (single-vector)
// -----------------------------------------------------------------------------

/**
 * y = gamma * (x - mean) / sqrt(var + eps) + beta. xhat caches the normalised
 * value; mean/rstd returned for backward to consume without recomputation.
 *
 * @returns {{mean:number, rstd:number}} cached scalars to feed back
 */
const cache = gpu.layernormForward(x, gamma, beta, y, xhat, 1e-5);
gpu.layernormBackward(dY, xhat, gamma, cache.rstd, dX, dGamma, dBeta);


// -----------------------------------------------------------------------------
// Single-head scaled dot-product self-attention
// -----------------------------------------------------------------------------

/**
 * Forward: O = Wo * (Attn * V), with Q/K/V derived by Wq/Wk/Wv from X.
 *
 * Shapes:
 *   X:   (N, D)
 *   Wq, Wk, Wv, Wo: (D, D)
 *   Q, K, V: (N, D)   - filled
 *   Attn: (N, N)      - post-softmax weights
 *   Y_pre_Wo: (N, D)  - Attn @ V
 *   O: (N, D)         - final output
 *
 * @param {GpuTensor|null} mask - length-N device mask (or null).
 */
gpu.attentionForward(X, Wq, Wk, Wv, Wo, mask, Q, K, V, Attn, Y_pre_Wo, O);

/**
 * Backward; dWq/dWk/dWv/dWo are accumulated into (caller zeros), dX is
 * overwritten.
 */
gpu.attentionBackward(dO, X, Q, K, V, Attn, Y_pre_Wo,
                      Wq, Wk, Wv, Wo, mask,
                      dX, dWq, dWk, dWv, dWo);


// -----------------------------------------------------------------------------
// Multi-head attention
// -----------------------------------------------------------------------------

/**
 * Multi-head self-attention. Wq/Wk/Wv/Wo are each (D, D); h heads of
 * head_dim = D / h are taken row-stripe-wise from the projection weights.
 *
 * @param {GpuTensor} X        (K, D) input
 * @param {GpuTensor|null} mask length-K device mask
 * @param {number} numHeads
 * Caches: Qh/Kh/Vh = (h*K, head_dim), Attnh = (h*K, K), Yconcat = (K, D),
 * O = (K, D).
 */
gpu.mhaForward(X, Wq, Wk, Wv, Wo, mask, numHeads,
               Qh, Kh, Vh, Attnh, Yconcat, O);

gpu.mhaBackward(dO, X, Qh, Kh, Vh, Attnh, Yconcat,
                Wq, Wk, Wv, Wo, mask, numHeads,
                dX, dWq, dWk, dWv, dWo);


// -----------------------------------------------------------------------------
// Pooling, losses, embedding, concat
// -----------------------------------------------------------------------------

/**
 * Masked mean pool over rows of a (K, D) matrix.
 *   y[j] = (1 / numValid) * sum_{k : mask[k]==1} X[k, j]
 * If numValid == 0 the output is zeroed.
 */
gpu.maskedMeanPoolForward(X, mask /* GpuTensor|null */, y);

/**
 * Backward. dX is *overwritten* (not accumulated). Pass the row count K of
 * the original input (we don't carry X around).
 */
gpu.maskedMeanPoolBackward(dY, mask, K, dX);

/**
 * Vector MSE: loss = mean((pred - target)^2). Backward gives (2/N)*(p - t).
 * @returns {number} scalar loss
 */
const mseLoss = gpu.mseVecForward(pred, target);
gpu.mseVecBackward(pred, target, dPred);

/**
 * Fused softmax + cross-entropy. Returns scalar loss; writes probs and
 * dLogits = (probs - target) on valid entries (0 on invalid).
 *
 * @param {GpuTensor|null} mask
 * @returns {number} scalar loss
 */
const xentLoss = gpu.softmaxXentFused(logits, target, mask, probs, dLogits);

/**
 * Embedding lookup. `idx` is a GpuTensor whose `.data` is reinterpreted as
 * an int32_t* device buffer (its size must equal B). Use this convention
 * instead of uploading an Int32Array per call — keep indices resident on
 * device.
 *
 *   out[b, :] = table[idx[b], :]
 */
gpu.embeddingLookupForward(table, idxGpuTensorViewedAsInt32, B, out);

/**
 * Embedding lookup backward — scatter-accumulate via atomicAdd. dTable is
 * accumulated into (caller zeros).
 */
gpu.embeddingLookupBackward(dOut, idxGpuTensorViewedAsInt32, B, dTable);

/** Concat flat tensors end-to-end. `parts` is a JS array of GpuTensors. */
gpu.concatRows([part0, part1, part2], out);

/** Inverse: copy disjoint segments of `in` back into each `parts[i]`. */
gpu.splitRows(in_, [part0, part1, part2]);


// -----------------------------------------------------------------------------
// Optimisers
// -----------------------------------------------------------------------------

/**
 * SGD with momentum, in place:
 *   velocity = momentum*velocity + grad
 *   param   -= lr * velocity
 */
gpu.sgdStep(param, grad, velocity, lr, momentum);

/**
 * Adam, in place (1-based step counter for bias correction):
 *   m = beta1*m + (1 - beta1)*g
 *   v = beta2*v + (1 - beta2)*g^2
 *   param -= lr * (m / (1 - beta1^step)) / (sqrt(v / (1 - beta2^step)) + eps)
 */
gpu.adamStep(param, grad, m, v, lr, beta1, beta2, eps, step);


// -----------------------------------------------------------------------------
// Batched (inference-only) variants
// -----------------------------------------------------------------------------
//
// Run B independent forward passes in a single kernel. Forward-only — no
// backward provided. Layout: (B, D) row-major, so row b columns 0..D-1 hold
// the b'th sample.

/** Y_BD[b, :] = W * X_BD[b, :] + bias  for all b. */
gpu.linearForwardBatched(W, bias, X_BD, Y_BD);

gpu.reluForwardBatched(X_BD, Y_BD);
gpu.tanhForwardBatched(X_BD, Y_BD);
gpu.addInplaceBatched(Y_BD, X_BD);   // Y[i] += X[i]
