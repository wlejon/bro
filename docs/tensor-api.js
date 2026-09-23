// =============================================================================
// bro.tensor, GPU tensor + ops (brotensor: CUDA, Metal or CPU)
// =============================================================================
//
// Wraps the brotensor sibling library. brotensor exposes one unified tensor
// type with a runtime Device tag and device-neutral ops; bro.tensor is the
// device-resident face of it. The op surface is identical across the CUDA
// (NVIDIA), Metal (Apple) and CPU backends, so code written against
// bro.tensor runs unchanged on any of them; a handful of FP16 / INT8 fast
// paths are GPU-only and say so.
//
// This file documents the CORE surface: runtime + dtypes, GpuTensor, the RNG
// and initialisers, safetensors IO, dense / elementwise / activation ops, the
// norms, matmul, RoPE, reductions, the batched (B, D) family, embedding,
// losses, concat/split and the optimisers.
//
// The rest of bro.tensor lives in **docs/tensor-nn-api.js**:
//   attention (single-head, MHA, self/cross, T5 bias, flash, KV-cache decode,
//   SAM rel-pos, packed varlen, gated delta rule, M-RoPE), conv2d/conv3d and
//   the whole spatial family, ResBlock, diffusion sampler steps, INT8 (W8A16)
//   and GGUF k-quant inference, and the audio / codec ops (FFT, STFT, conv1d,
//   snake/elu, VQ/FSQ, resample, logit sampling).
//
// Availability:
//   bro.tensor.available   // boolean
//   bro.tensor.backend     // "cpu" | "cuda" | "metal" (lowercased device name,
//                          //  "cuda:1" on a non-default CUDA device)
//
// `available` is true whenever bro was built with BRO_WITH_TENSOR (the `full`
// profile). When bro is built without it, bro.tensor is the compiled-out stub
// `{ available: false }` and nothing else is there, so guard every use. With
// tensor compiled in but no GPU backend, `backend` reads "cpu" and the ops
// still run, just on the CPU. Gate heavy ML work on `backend !== "cpu"` (or
// on `bro.gpu.available`), never on `available` alone.
//
// Dtypes (bro.tensor.dtype):
//   fp32 0   fp16 1   int8 2   int32 3   bf16 4   f64 5
//
// Tensors carry a dtype tag and ops dispatch on the input's dtype (FP16 / BF16
// kernels accumulate internally in FP32). INT8 is carried by the weight-only
// quantised ops, INT32 by index / offset buffers. Everywhere a `dtype`
// argument is taken you may pass the name ("fp32"/"f32", "fp16"/"f16",
// "bf16", "int8"/"i8", "int32"/"i32", "f64") or the numeric enum; anything
// unrecognised falls back to FP32.
//
// Synchronisation:
//   Ops queue on the default stream and are asynchronous on a GPU. Call
//   bro.tensor.sync() before timing or before reading device memory by any
//   route other than download()/downloadFp16()/downloadInt8(), which sync
//   internally.
//
// Mask convention:
//   A `mask|null` slot takes a device-resident FP32 GpuTensor (1 = valid,
//   0 = masked) or null/undefined for "no mask". Host Float32Arrays are
//   rejected with a TypeError; the one exception is softmaxXentSegment, which
//   is a host-buffer op throughout.
//
// INT32 index / offset buffers:
//   Index and offset operands (`idx`, `Idx`, `headOffsets`, `cuSeq*`,
//   `posT/posH/posW`) are GpuTensors. GpuTensor.prototype.uploadInt32 builds
//   one; plain upload() lands FP32, and embedding / gatherRows /
//   scatterRowsAdd / cuSeq* / pos* accept that too when every value is a
//   whole number. `headOffsets` and pooling `Idx` must be INT32 (pooling
//   `Idx` is the forward's own output). Because these values address memory
//   the kernels do not bounds-check, the binding reads each one back to the
//   host and throws on an out-of-range entry, which costs one device sync
//   per call. The sampler's `indices` is an INT32 output and never leaves
//   the device.
//
// Errors:
//   A native never throws across the bronze ABI: it records a message the JS
//   wrapper reads back and re-throws as an Error right after the call. So a
//   failed op surfaces as a normal JS exception at the call site, and an op
//   that is not implemented on the active backend throws "not implemented".
//
// =============================================================================

const gpu = bro.tensor;


// -----------------------------------------------------------------------------
// Runtime
// -----------------------------------------------------------------------------

/**
 * `true` when brotensor is compiled in. Check before touching anything else.
 * @type {boolean}
 */
gpu.available;

/** Active device, lowercased: "cpu" | "cuda" | "metal" | "cuda:<n>". */
gpu.backend;

/**
 * Idempotent device init. CUDA: selects device 0 (or BROTENSOR_CUDA_DEVICE).
 * Metal: opens the default MTLDevice. Ops auto-init, but calling this once at
 * startup surfaces device errors early. A no-op when `available` is false.
 *
 * NOTE (ordering): init() before reading `backend` or probing a device, or a
 * pre-init probe answers "cpu" and the whole app silently runs on the CPU.
 */
gpu.init();

/** Block until every queued kernel on the default stream has completed. */
gpu.sync();

/** The dtype enum: { fp32:0, fp16:1, int8:2, int32:3, bf16:4, f64:5 }. */
gpu.dtype;


// -----------------------------------------------------------------------------
// GpuTensor
// -----------------------------------------------------------------------------

/**
 * Allocate an owning device tensor, zero-filled. dtype defaults to FP32.
 * Throws if brotensor is compiled out, or RangeError on negative dims.
 *
 * @param {number} rows
 * @param {number} [cols=1]
 * @param {string|number} [dtype="fp32"]
 * @returns {GpuTensor}
 */
const t   = gpu.createTensor(3, 4);
const t16 = gpu.createTensor(3, 4, "fp16");
const q8  = gpu.createTensor(8, 16, gpu.dtype.int8);

/**
 * The class itself, for `x instanceof bro.tensor.GpuTensor`. It is not
 * constructible: instances only come from createTensor(), clone(),
 * SafetensorsFile#get() and the other ops that return one.
 */
gpu.GpuTensor;

t.rows;          // 3
t.cols;          // 4
t.size;          // 12   (rows * cols)
t.bytes;         // 48   (size * sizeof(dtype))
t.dtype();       // "fp32" | "fp16" | "bf16" | "int8" | "int32" | "f64"

t.zero();                   // device memset to 0
t.resize(2, 6);             // reallocates, dtype defaults back to fp32
t.resize(2, 6, "fp16");     // reallocates AND switches dtype to FP16
const dup = t.clone();      // owning device-side copy (same shape + dtype)

/**
 * Upload host -> device as FP32. Accepts a Float32Array or anything
 * Float32Array.from() takes (a plain array). The destination keeps its
 * (rows, cols) when their product equals the element count, otherwise it
 * becomes (n, 1).
 * @param {Float32Array|number[]} src
 */
t.upload(new Float32Array([1, 2, 3, 4, 5, 6]));

/**
 * Download device -> host as FP32. Syncs, and converts from whatever dtype the
 * tensor carries.
 *   download()     -> a fresh Float32Array of `size` elements.
 *   download(dst)  -> fills `dst` in place and returns it. `dst` must be a
 *                     Float32Array with dst.length >= size (RangeError
 *                     otherwise), which is how a render/inference loop reads
 *                     results back without allocating per frame.
 * @param {Float32Array} [dst]
 * @returns {Float32Array}
 */
const arr = t.download();
const reuse = new Float32Array(t.size);
t.download(reuse);          // same array back, filled

/**
 * FP16 staging: a Uint16Array of IEEE binary16 bit patterns.
 *   uploadFp16(data)  — same reshape rule as upload(); dtype becomes FP16.
 *   downloadFp16()    — fresh Uint16Array, converting from any dtype.
 */
t16.uploadFp16(new Uint16Array([0x3c00, 0x4000]));
const u16 = t16.downloadFp16();

/**
 * INT8 staging, the only way to get W8A16 weights onto the device. Typically
 * the `weights` field of quantizeInt8PerRowHost() (see tensor-nn-api.js).
 *   uploadInt8(data)  — Int8Array (or plain array); same reshape rule.
 *   downloadInt8()    — the tensor's raw storage bytes as a fresh Int8Array,
 *                       `bytes` long (not `size` long for a non-INT8 dtype).
 * Pair the result with an FP32 (out, 1) scales tensor for the Int8wFp16 family.
 */
q8.uploadInt8(quant.weights);
const bytes = q8.downloadInt8();

/**
 * INT32 staging, for the index / position / bound / token-grid operands.
 *   uploadInt32(data)  — Int32Array (or plain array of ints); same reshape
 *                        rule as upload(); dtype becomes INT32.
 *   downloadInt32()    — a fresh Int32Array of an INT32 tensor's values
 *                        (throws for any other dtype). How rowsCountAbove
 *                        counts, maskedDiffusionScores `pred`, topKRows Idx
 *                        etc. come back to the host.
 * Ops that only READ an index operand also accept an FP32 tensor of whole
 * numbers (what upload() produces) and convert it; operands an op writes in
 * place (maskedDiffusionCommit's token grid) must already be INT32.
 */
const pos = gpu.createTensor(4, 1, "int32");
pos.uploadInt32([0, 1, 0, 1]);
const back = pos.downloadInt32();   // Int32Array [0, 1, 0, 1]


// -----------------------------------------------------------------------------
// Counter-based RNG (Philox) + initialisers
// -----------------------------------------------------------------------------
//
// Deterministic in (key, counter): the same pair always fills the same values,
// on every backend. key / counter are Numbers or BigInts, never tensors; Y is
// an FP32 GpuTensor the caller pre-sized (it is filled, not resized).

gpu.randUniform(key, counter, Y);              // U[0, 1)
gpu.randn(key, counter, Y);                    // standard normal
gpu.randBernoulli(p, key, counter, Y);         // 1 with probability p, else 0
gpu.randnTruncated(lo, hi, key, counter, Y);   // normal truncated to [lo, hi]

/**
 * Xavier-uniform fill of W, using and advancing a counter-based RNG state.
 * Returns the advanced state (a Number) — thread it through successive inits
 * so a whole model initialises from one seed.
 * @param {GpuTensor} W
 * @param {number|bigint} rngState
 * @returns {number}
 */
let rngState = 1234;
rngState = gpu.xavierInit(W1, rngState);
rngState = gpu.xavierInit(W2, rngState);


// -----------------------------------------------------------------------------
// safetensors, load / save
// -----------------------------------------------------------------------------
//
// The huggingface safetensors container. The reader mmap's the file: opening a
// multi-GB checkpoint and reading only its header() is cheap, no payload is
// faulted in until get() uploads a tensor. Paths go through bro's path
// resolver, so an app-relative path works.

/**
 * Open a .safetensors file.
 * @param {string} path
 * @returns {SafetensorsFile}  throws TypeError if missing or malformed.
 */
const f = gpu.openSafetensors('weights/model.safetensors');

/** The class, for instanceof. Not constructible; openSafetensors returns one. */
gpu.SafetensorsFile;

/** Number of tensors in the file. @type {number} */
f.count;

/** Tensor names in file order. @returns {string[]} */
f.names();

/**
 * Per-tensor metadata, header only, so it stays cheap on huge files.
 * @returns {Object<string, {dtype: string, shape: number[], nbytes: number}>}
 *   dtype is the safetensors spelling: "F32" | "F16" | "BF16" | "I8" | ...
 */
const hdr = f.header();
hdr['model.embed_tokens.weight'].shape;   // e.g. [151936, 1024]

/**
 * Upload one tensor as a GpuTensor. brotensor tensors are 2D: with rows/cols
 * omitted an N-D source flattens to (shape[0], numel/shape[0]).
 *
 * The dtype string selects how the source is uploaded, and may be passed in
 * place of, or after, rows/cols:
 *   "native"  (default) keep the file's dtype (F32/F16/BF16).
 *   "compute"           the backend's compute dtype: FP16 on a GPU build,
 *                       FP32 on CPU. BF16 sources are converted. The
 *                       model-loader path.
 *   "fp16"              always FP16, converting an F32 source.
 *
 * @param {string} name
 * @param {number|string} [rows]
 * @param {number} [cols]
 * @param {string} [dtype="native"]
 * @returns {GpuTensor}   RangeError on an unknown name, TypeError on a bad read.
 */
const W  = f.get('model.layers.0.self_attn.q_proj.weight');            // native
const Wc = f.get('model.layers.0.self_attn.q_proj.weight', 'compute'); // FP16 on GPU
const We = f.get('embedding.weight', 1024, 768, 'compute');            // explicit 2D

/** Release the mmap early. GC releases it otherwise; calls after it throw. */
f.close();

/**
 * Write GpuTensors to a .safetensors file. FP32 and FP16 values only (anything
 * else throws); shape is stored as the tensor's (rows, cols).
 * @param {string} path
 * @param {Object<string, GpuTensor>} tensors
 */
gpu.saveSafetensors('out/checkpoint.safetensors', { weight: W, bias: b });


// -----------------------------------------------------------------------------
// Dense + elementwise
// -----------------------------------------------------------------------------

gpu.linearForward(W, b, x, y);              // y = W*x + b
gpu.linearBackward(W, x, dY, dX, dW, dB);   // dW, dB accumulate; dX overwritten

gpu.reluForward(x, y);     gpu.reluBackward(x, dY, dX);      // reads x
gpu.tanhForward(x, y);     gpu.tanhBackward(y, dY, dX);      // reads cached y
gpu.sigmoidForward(x, y);  gpu.sigmoidBackward(y, dY, dX);   // reads cached y

gpu.addInplace(y, x);            // y[i] += x[i]
gpu.addScalarInplace(y, 0.5);    // y[i] += s
gpu.scaleInplace(y, 2.0);        // y[i] *= s
gpu.mulInplace(y, x);            // y[i] *= x[i]
gpu.clamp(y, -1.0, 1.0);         // y[i] = clip(y[i], lo, hi)

/**
 * y[i] = a*y[i] + b*x[i], with the arithmetic in FP32 whatever the storage
 * dtype (FP32/FP16/BF16; x and y share shape and dtype). The classifier-free
 * guidance blend v = s*v_cond - (s-1)*v_uncond, which cancels catastrophically
 * if combined at half precision.
 */
gpu.axpbyInplace(y, x, /*a*/ 2.0, /*b*/ -1.0);

/**
 * Broadcast bias adds, in place, bias sharing y's dtype.
 *   addChannelBiasInplace: y is channel-major (C, L), y[c*L+i] += bias[c];
 *                          y must hold exactly C*L elements, bias C.
 *   addRowBiasInplace:     Y is row-major (R, D), Y[r, d] += bias[d];
 *                          bias must hold exactly Y.cols elements.
 */
gpu.addChannelBiasInplace(y, bias, C, L);
gpu.addRowBiasInplace(Y, bias);

/**
 * Byte mask: Y[i] = X[i] > t ? 1 : 0 (strict >). X is FP32 or FP16; Y is
 * resized to INT8 (read it with downloadInt8()). Not differentiable.
 */
gpu.thresholdU8(X, /*t*/ 0.0, Y);

/**
 * Build a slot-validity mask on-device, without a host sync:
 *   mask[k] = (x[offset + k*stride] > 0.5) ? 1 : 0,  k in [0, K)
 * Resizes mask to (K, 1).
 */
gpu.buildSlotMask(x, offset, K, stride, mask);

/**
 * Device-to-device chunk copy: `n` flat elements from src[srcOff] into
 * dst[dstOff]. Both tensors are treated as flat buffers, whatever their shape.
 */
gpu.copyD2D(src, srcOff, dst, dstOff, n);

/**
 * Strided row-block copy: `height` rows of `width` elements, row r read from
 * src[srcOff + r*srcPitch] and written to dst[dstOff + r*dstPitch] (offsets
 * and pitches in elements, pitch >= width). One call replaces a loop of
 * copyD2D calls, e.g. padding / unpadding the W axis of an NCHW activation.
 * src and dst share a dtype; dst is not resized, so both must already cover
 * every row the copy touches (an Error otherwise).
 */
gpu.copyD2DStrided(src, srcOff, srcPitch, dst, dstOff, dstPitch, width, height);

/**
 * Dtype cast: dst = src converted to outDtype, resized + dtype-set to src's
 * shape on src's device. FP32 <-> FP16 <-> BF16 plus a same-dtype passthrough
 * copy; other pairs throw. The mixed-precision primitive (FP16 working copy
 * against an FP32 master weight).
 * @param {GpuTensor} src
 * @param {GpuTensor} dst
 * @param {string|number} outDtype
 */
gpu.cast(src, dst, "fp16");


// -----------------------------------------------------------------------------
// Modern activations (transformer + diffusion stack)
// -----------------------------------------------------------------------------
//
// Forwards take (x, y); backwards take (x, dY, dX) and read the *raw forward
// input* x, not the cached output. Dispatched on x.dtype.

gpu.siluForward(x, y);        gpu.siluBackward(x, dY, dX);       // x*sigmoid(x)
gpu.geluForward(x, y);        gpu.geluBackward(x, dY, dX);       // tanh approx
gpu.geluExactForward(x, y);   gpu.geluExactBackward(x, dY, dX);  // erf-based
gpu.quickGeluForward(x, y);   gpu.quickGeluBackward(x, dY, dX);  // x*sigmoid(1.702x)

// Gated FFN activations. Input (B, 2*D) splits into halves A and B_half,
// output is (B, D). swiglu: silu(A)*B_half. geglu: A*gelu(B_half), with an
// exact-erf variant.
gpu.swigluForward(X, Y);      gpu.swigluBackward(X, dY, dX);
gpu.gegluForward(X, Y);       gpu.gegluBackward(X, dY, dX);
gpu.gegluExactForward(X, Y);  gpu.gegluExactBackward(X, dY, dX);

/**
 * FP32-only elementwise maps (StyleGAN3's Fourier features and demod
 * reciprocal-sqrt). sin / cos backwards read the forward INPUT x; rsqrt's
 * reads the forward OUTPUT y (dX = -0.5 * dY * y^3). rsqrt does not guard
 * x > 0. Backwards overwrite dX; dY must match the forward's size.
 */
gpu.sinForward(x, y);     gpu.sinBackward(x, dY, dX);     // dX = dY*cos(x)
gpu.cosForward(x, y);     gpu.cosBackward(x, dY, dX);     // dX = -dY*sin(x)
gpu.rsqrtForward(x, y);   gpu.rsqrtBackward(y, dY, dX);   // y = 1/sqrt(x)


// -----------------------------------------------------------------------------
// Softmax
// -----------------------------------------------------------------------------

/**
 * Row softmax. The third argument is either:
 *   a GpuTensor / null — a length-N FP32 device key mask (1 valid, 0 masked),
 *   or a number        — a temperature: the logits are scaled by 1/temp first
 *                        (temp === 1 or <= 0 is a plain softmax).
 * @param {GpuTensor} logits
 * @param {GpuTensor} probs
 * @param {GpuTensor|number|null} [maskOrTemp]
 */
gpu.softmaxForward(logits, probs, /*mask|null*/ null);
gpu.softmaxForward(logits, probs, /*temperature*/ 0.7);

/** Full-Jacobian backward: dLogits = (diag(p) - p p^T) dProbs. */
gpu.softmaxBackward(probs, dProbs, dLogits);

/**
 * Row-batched softmax in one launch: Y[r, :] = softmax(X[r, :]) for `rows`
 * rows of `cols` (X must hold rows*cols elements; rows / cols default to X's
 * shape). FP32 / FP16 / BF16. Y may be X (in place). Inference only.
 */
gpu.softmaxRowsForward(X, Y, rows, cols);

/**
 * Fused softmax + cross-entropy over the flat N elements of `logits`, the
 * CPU-style argument order of softmaxXentFused. Returns the loss
 * -sum target*log(p); writes probs and dLogits = probs - target (both resized).
 * All FP32; target holds N elements; the optional mask is an FP32 length-N
 * legal-action mask (masked entries get probability 0 and no gradient).
 * @returns {number}
 */
const xent = gpu.softmaxXent(logits, target, probs, dLogits, /*mask|null*/ null);


// -----------------------------------------------------------------------------
// LayerNorm
// -----------------------------------------------------------------------------

/**
 * Single-vector LayerNorm. Returns the two scalar caches the backward wants,
 * so it never recomputes them.
 * @returns {{mean: number, rstd: number}}
 */
const ln = gpu.layernormForward(x, gamma, beta, y, xhat, /*eps*/ 1e-5);
gpu.layernormBackward(dY, xhat, gamma, ln.rstd, dX, dGamma, dBeta);

/**
 * Inference-only batched LayerNorm, one block per row, no caches, no sync.
 *   X_RD: (R, D)   gamma: (D,)   beta: (D,)   Y_RD: (R, D), resized if needed.
 */
gpu.layernormForwardInferenceBatched(X_RD, gamma, beta, Y_RD, 1e-5);
gpu.layernormForwardInferenceBatchedFp16(X_RD, gamma, beta, Y_RD, 1e-5);

/**
 * Batched LayerNorm that saves the caches for an exact training backward.
 *   X (R,D), gamma/beta (D,), Y/Xhat (R,D) resized, Mean/Rstd (R,1) FP32
 *   whatever X's dtype is.
 * dX is overwritten; dGamma / dBeta accumulate into (D,) tensors the caller
 * sized and zeroed (they are not resized).
 */
gpu.layernormForwardBatchedWithCaches(X, gamma, beta, Y, Xhat, Mean, Rstd, 1e-5);
gpu.layernormBackwardBatchedWithCaches(dY, Xhat, gamma, Rstd, dX, dGamma, dBeta);


// -----------------------------------------------------------------------------
// RMSNorm / GroupNorm / per-head L2 norm
// -----------------------------------------------------------------------------

/**
 * Llama-style RMSNorm, per row:
 *   rms[b] = sqrt(mean_j x[b,j]^2 + eps);  y[b,j] = x[b,j]*gamma[j]/rms[b]
 *   X: (B, D)   gamma: (D, 1)   Y: (B, D), resized + dtype-matched.
 */
gpu.rmsNormForward(X, gamma, 1e-5, Y);
gpu.rmsNormBackward(X, gamma, dY, 1e-5, dX, dGamma);   // dGamma accumulates

/**
 * NCHW GroupNorm; numGroups must divide C. Mean/var over each
 * (C/numGroups, H, W) tile. FP32 / FP16 dispatched on X.dtype.
 */
gpu.groupNormForward(X, gamma, beta, N, C, H, W, numGroups, 1e-5, Y);
gpu.groupNormBackward(X, gamma, dY, N, C, H, W, numGroups, 1e-5,
                      dX, dGamma, dBeta);              // dGamma/dBeta accumulate

/**
 * Per-head last-dim L2 normalise over an (L, numHeads*headDim) layout, the
 * QK-norm of gated-deltanet attention: each head's headDim slice is divided by
 * sqrt(sum of squares + eps). eps defaults to 1e-6.
 * Distinct from l2NormalizeNchwForward (channel axis of an NCHW tensor), which
 * lives in tensor-nn-api.js.
 */
gpu.l2NormForward(X, headDim, numHeads, /*eps*/ 1e-6, Y);
gpu.l2NormBackward(X, headDim, numHeads, /*eps*/ 1e-6, dY, dX);

/**
 * Pixel norm (StyleGAN's normalize_2nd_moment): per row of an (N, C) FP32
 * tensor, Y = X * rsqrt(mean_c(X^2) + eps). Root-MEAN-square — the 1/C is
 * included, unlike the L2 norms above. No parameters; the backward reads the
 * raw forward input X and overwrites dX.
 */
gpu.pixelNormForward(X, /*eps*/ 1e-8, Y);
gpu.pixelNormBackward(X, dY, /*eps*/ 1e-8, dX);


// -----------------------------------------------------------------------------
// Matmul
// -----------------------------------------------------------------------------

/** C(M,N) = A(M,K) @ B(K,N). Dtype dispatched on A.dtype. */
gpu.matmul(A, B, C);

/**
 * Backward. dA and dB are *accumulated* into (the caller zeros them); dC is
 * read-only.
 *   dA += dC @ B^T   ;   dB += A^T @ dC
 */
gpu.matmulBackward(A, B, dC, dA, dB);

/**
 * Batched A @ B^T, 16-bit (FP16 or BF16, shared by A, B, C and bias) with FP32
 * accumulation: for b in [0, batch), C[b](M,N) = A[b](M,K) @ B[b](N,K)^T,
 * slices `stride*` elements apart (defaults: tightly packed M*K, N*K, M*N; a
 * stride of 0 reuses one slice for every b). bias (N) may be null; act is a
 * gpu.LinearActivation value fused into the store. C is NOT resized — it must
 * already hold every slice the call writes (an Error otherwise).
 */
gpu.matmulAbt(A, B, C, batch, M, N, K, strideA, strideB, strideC,
              /*bias|null*/ null, gpu.LinearActivation.none);


// -----------------------------------------------------------------------------
// RoPE (rotary position embedding)
// -----------------------------------------------------------------------------

/**
 * Per-head pair rotation:
 *   x_{2i}   <- x_{2i}*cos(t) - x_{2i+1}*sin(t)
 *   x_{2i+1} <- x_{2i}*sin(t) + x_{2i+1}*cos(t)
 *   t = pos * thetaBase^(-2i/headDim),  pos = seqOffset + row.
 *   X / Y / dY / dX: (L, numHeads*headDim).  headDim must be even.
 */
gpu.ropeForward(X, headDim, numHeads, seqOffset, /*thetaBase*/ 10000.0, Y);
gpu.ropeBackward(dY, headDim, numHeads, seqOffset, 10000.0, dX);

/**
 * RoPE against caller-supplied tables: same rotation, but each row reads its
 * angles from cosTbl / sinTbl instead of deriving them from seqOffset and
 * thetaBase. Use it when the position schedule is irregular (packed sequences,
 * 2D/3D RoPE) or shared across calls.
 *   X / Y: (L, numHeads*headDim).  cosTbl / sinTbl: (L, headDim/2).
 * The backward rotates dY by the inverse angles, so it takes the SAME tables
 * the forward used: dX = R(-t) * dY.
 */
gpu.ropeApply(X, cosTbl, sinTbl, headDim, numHeads, Y);
gpu.ropeApplyBackward(dY, cosTbl, sinTbl, headDim, numHeads, dX);

/**
 * ropeApply with PER-HEAD tables: every (row, head) pair carries its own
 * angles (content-dependent rotary, e.g. TripoSplat's RePo3D).
 *   X / Y: (L, numHeads*headDim), FP32/FP16/BF16.
 *   cosTbl / sinTbl: (L*numHeads, headDim/2) FP32, head-minor within a row.
 * Inference-only (no backward).
 */
gpu.ropeApplyPerhead(X, cosTbl, sinTbl, headDim, numHeads, Y);

// ropeQkvPackedInplace, the in-place rotary over a packed (L, 3*D) QKV buffer,
// is with flashAttentionPackedQkvForward in tensor-nn-api.js.

// The Qwen-VL multimodal variant, ropeApplyMrope, is in tensor-nn-api.js.


// -----------------------------------------------------------------------------
// AdaLN modulation (DiT / SD3 / Flux)
// -----------------------------------------------------------------------------

/**
 * Adaptive-LayerNorm modulation: Y = X * (1 + scale) + shift, with the
 * per-channel scale / shift broadcast across every token row — the affine step
 * every DiT block applies after norm().
 *   X, Y: (L, D) token activations.
 *   scale, shift: length-D vectors ((1,D) or (D,1)), same dtype/device as X.
 * Y is resized + dtype-set to X.
 */
gpu.modulate(X, scale, shift, Y);

/**
 * Broadcast channel-wise multiply: Y[l,d] = X[l,d] * v[d]. The DiT residual
 * gate (`x = x + broadcastMul(sublayerOut, gate)`) and any per-channel rescale.
 */
gpu.broadcastMul(X, v, Y);


// -----------------------------------------------------------------------------
// Reductions, row gather / scatter / top-k
// -----------------------------------------------------------------------------

gpu.sumRows(X, Y);       // Y(M,1) = sum_n X[m,n]
gpu.sumCols(X, Y);       // Y(1,N) = sum_m X[m,n]
gpu.argmaxRows(X, Idx);  // Idx(M,1) FP32; the integer index stored as a float

/**
 * Row gather / its adjoint. Idx is an (M,1) index tensor — INT32 as topKRows
 * writes it, or an FP32 tensor of whole numbers, which the op converts. Out of
 * range values are the caller's problem: these do not bounds-check.
 *   gatherRows:    Y[i] = X[Idx[i]]
 *   scatterRowsAdd: dX is (R, C), zeroed and then scatter-added into. R is the
 *                   forward X's row count, which dY and Idx alone don't give.
 */
gpu.gatherRows(X, Idx, Y);
gpu.scatterRowsAdd(dY, Idx, R, dX);

/**
 * Per-row top-k, descending, ties broken toward the smaller column index.
 *   Vals: (R, k) FP32 and Idx: (R, k) INT32, both resized + dtype-set.
 */
gpu.topKRows(X, k, Vals, Idx);

/**
 * Scatter-OVERWRITE rows in place: X[Idx[m], :] = Y[m, :]. Rows Idx does not
 * name keep their contents; X is never resized. Idx is (Y.rows, 1), INT32 or
 * whole-number FP32, and every entry must lie in [0, X.rows) — the binding
 * reads Idx on the host and throws otherwise (so this call syncs). X and Y
 * share dtype and column count; duplicate indices race.
 */
gpu.scatterRows(Y, Idx, X);

/**
 * Two above-threshold counts per row in one pass (strict >):
 *   counts[r] = [ #{X[r][c] > tLo}, #{X[r][c] > tHi} ]
 * X is (R, C) FP32 or FP16; counts is resized to (R, 2) INT32
 * (downloadInt32()). SAM's stability score without downloading the logits.
 */
gpu.rowsCountAbove(X, tLo, tHi, counts);

/**
 * Softmax summary of variable-length segments of a logit column. Segment s is
 * rows [segOffsets[s], segOffsets[s+1]); with p its softmax and
 * k = max(2, length):
 *   out[s] = [ top1, top1 - top2, entropy(p)/log(k), k/255 ]
 * (top2 = 0 for a one-row segment; an empty segment writes zeros).
 *   logits: (K, 1) FP32/FP16/BF16.   out: (S, 4), logits' dtype, resized.
 *   segOffsets: (S+1, 1) INT32 (or whole-number FP32), non-decreasing within
 *               [0, K] — checked on the host before the launch.
 */
gpu.segmentSoftmaxStats(logits, segOffsets, out);


// -----------------------------------------------------------------------------
// Batched dense family (B, D)
// -----------------------------------------------------------------------------
//
// B independent passes in one launch, for trainers and inference loops that
// share a minibatch. Tensors carrying B rows are (B, D) row-major: W is
// (out, in), bias (out, 1), X_BD (B, in), Y_BD (B, out) and resized. W may be
// FP32/FP16/BF16 while the activations stay FP32.

gpu.linearForwardBatched(W, bias, X_BD, Y_BD);
gpu.reluForwardBatched(X_BD, Y_BD);
gpu.tanhForwardBatched(X_BD, Y_BD);
gpu.addInplaceBatched(Y_BD, X_BD);

/**
 * 16-bit storage throughout (FP16 or BF16 W, bias, X and produced Y).
 * Inference-only, and GPU-only: the CPU backend registers no slot for it.
 * `bias` may be null.
 */
gpu.linearForwardBatchedFp16(W, /*bias|null*/ null, X_BD, Y_BD);

/**
 * Epilogue codes for the fused batched linears and matmulAbt:
 *   gpu.LinearActivation = { none: 0, relu: 1, geluTanh: 2, geluExact: 3,
 *                            silu: 4, quickGelu: 5 }
 *   gpu.LinearEpilogue   = { store: 0, accumulate: 1, geglu: 2, fastAccum: 16 }
 */
gpu.LinearActivation;
gpu.LinearEpilogue;

/**
 * linearForwardBatchedFp16 with bias + activation fused into the GEMM's store
 * (no separate bias / activation pass). FP16 or BF16 throughout; GPU only.
 */
gpu.linearForwardBatchedFp16Act(W, /*bias|null*/ null, X_BD, gpu.LinearActivation.geluTanh, Y_BD);

/**
 * Batched linear r = act(X_BD . W^T + bias) with a fused epilogue:
 *   store       Y = r                     (Y resized to (B, out))
 *   accumulate  Y += r (residual add)     (Y must already be (B, out))
 *   geglu       Y[:, j] = r[:, 2j] * gelu_exact(r[:, 2j+1]); act must be none,
 *               W's rows interleave the two halves; Y is (B, out/2)
 * OR `fastAccum` in to let FP16 accumulate each 16-deep k step in FP16 (faster
 * on consumer GPUs, bounded extra error; opt in per model). FP16/BF16 on the
 * GPU, FP32 on the CPU. `workspace` is an optional FP32 scratch tensor that
 * lets a short-B call split K across more blocks; reuse one across calls.
 */
gpu.linearForwardBatchedEx(W, /*bias|null*/ null, X_BD, gpu.LinearActivation.none,
                           gpu.LinearEpilogue.accumulate, /*workspace|null*/ null, Y_BD);

gpu.linearBackwardBatched(W, X_BD, dY_BD, dX_BD, dW, dB);  // dW/dB accumulate
gpu.reluBackwardBatched(X_BD, dY_BD, dX_BD);   // reads X:  dX = dY*(X>0)
gpu.tanhBackwardBatched(Y_BD, dY_BD, dX_BD);   // reads Y:  dX = dY*(1-Y*Y)


// -----------------------------------------------------------------------------
// Embedding
// -----------------------------------------------------------------------------

/**
 * Embedding lookup: out[b, :] = table[idx[b], :]. `idxAsInt32` is a GpuTensor
 * whose storage is read as an INT32 buffer of B entries, so token ids stay
 * device-resident across the whole decode step.
 */
gpu.embeddingLookupForward(table, idxAsInt32, B, out);
gpu.embeddingLookupBackward(dOut, idxAsInt32, B, dTable);   // dTable accumulates


// -----------------------------------------------------------------------------
// Pooling + losses
// -----------------------------------------------------------------------------

/**
 * Masked mean pool over the rows of X (a sequence -> one vector), with an
 * optional length-N device mask. The backward spreads dY over the K valid
 * rows; K is the forward's row count.
 */
gpu.maskedMeanPoolForward(X, /*mask|null*/ null, y);
gpu.maskedMeanPoolBackward(dY, /*mask|null*/ null, K, dX);

/** Vector MSE over the whole tensor. @returns {number} the scalar loss. */
const mseLoss = gpu.mseVecForward(pred, target);
gpu.mseVecBackward(pred, target, dPred);

/**
 * Per-sample MSE (loss = 0.5*d^2, dPred = d).
 *   pred, target, dPred, lossPerSample: (B, 1)
 */
gpu.mseVecPerSample(pred, target, dPred, lossPerSample);

/**
 * Host-scalar MSE, the value-head loss: loss = 0.5*(pred-target)^2 and
 * dPred = pred-target. Plain numbers, no tensors.
 * @returns {[number, number]} [loss, dPred]
 */
const [vLoss, vGrad] = gpu.mseScalar(pred, target);

/**
 * Fused softmax + cross-entropy. Writes probs and dLogits = probs - target on
 * valid entries (0 on masked ones).
 * @returns {number} the scalar loss.
 */
const xentLoss = gpu.softmaxXentFused(logits, target, /*mask|null*/ null,
                                      probs, dLogits);

/**
 * Batched fused softmax + cross-entropy across (sample, head) tiles, for
 * trainers that share one (B, n_act_total) logits buffer across actor heads.
 *   logits_BL, target_BL, probs_BL, dLogits_BL: (B, n_act_total)
 *   mask:          (B, n_act_total) device mask or null
 *   headOffsets:   an INT32 GpuTensor, n_heads+1 non-decreasing offsets in
 *                  [0, n_act_total]
 *   lossPerSample: (B, 1), overwritten with the sum-over-heads loss
 */
gpu.softmaxXentFusedBatched(logits_BL, target_BL, /*mask|null*/ null,
                            headOffsets, n_heads,
                            probs_BL, dLogits_BL, lossPerSample);

/**
 * Host-buffer softmax cross-entropy over the first `n` elements of each
 * Float32Array, so a caller can run xent on a segment of a larger buffer with
 * no temporary tensors. probs and dLogits are written in place; `mask` is an
 * optional host Float32Array (null for none).
 * @returns {number} the scalar loss.
 */
const segLoss = gpu.softmaxXentSegment(logits, target, probs, dLogits, n,
                                       /*mask|null*/ null);

/**
 * Fused, numerically stable BCE-with-logits over a (B, L) grid. posWeight === 1
 * is standard unweighted BCE; `mask` is an optional (B, L) device mask.
 * lossPerSample is (B, 1), summed over L.
 */
gpu.bceWithLogitsFusedBatched(logits, target, /*mask|null*/ null, /*posWeight*/ 1.0,
                              probs, dLogits, lossPerSample);


// -----------------------------------------------------------------------------
// Concat / split
// -----------------------------------------------------------------------------

/** Concat flat tensors end to end. `parts` is a JS array of GpuTensors. */
gpu.concatRows([part0, part1, part2], out);

/** The inverse: scatter disjoint segments of `in` back into each parts[i]. */
gpu.splitRows(in_, [part0, part1, part2]);

/**
 * Batched column-block concat: parts are each (B, d_i), out becomes
 * (B, sum d_i) with out[b, off_i + j] = parts[i][b, j].
 */
gpu.concatBatchedRows([part0, part1, part2], out);

/**
 * Channel-axis concat over NCHW tensors. Each parts[i] is (N, C_i*H*W); out
 * becomes (N, sum_i C_i * H * W), channel blocks regrouped per sample.
 * C_per_part is a JS array (or Int32Array) of ints, same length as parts.
 */
gpu.concatNchwChannels([part0, part1], N, H, W, [C0, C1], out);

/** Its backward: each dParts[i] is overwritten with dY's channel slice. */
gpu.concatNchwChannelsBackward(dY, N, H, W, [C0, C1], [dPart0, dPart1]);


// -----------------------------------------------------------------------------
// Optimisers
// -----------------------------------------------------------------------------

/**
 * SGD with momentum:
 *   velocity = momentum*velocity + grad
 *   param   -= lr * velocity
 */
gpu.sgdStep(param, grad, velocity, lr, momentum);

/**
 * Adam, with a 1-based step counter for the bias correction:
 *   m = beta1*m + (1-beta1)*g
 *   v = beta2*v + (1-beta2)*g^2
 *   param -= lr * (m/(1-beta1^step)) / (sqrt(v/(1-beta2^step)) + eps)
 */
gpu.adamStep(param, grad, m, v, lr, beta1, beta2, eps, step);


// -----------------------------------------------------------------------------
// Worked example: a tiny training step
// -----------------------------------------------------------------------------
//
//   bro.tensor.init();
//   if (!bro.tensor.available) throw new Error('built without BRO_WITH_TENSOR');
//
//   const B = 32, D_in = 16, D_out = 4;
//   const W  = bro.tensor.createTensor(D_out, D_in);
//   const b  = bro.tensor.createTensor(D_out, 1);
//   const X  = bro.tensor.createTensor(B, D_in);
//   const Y  = bro.tensor.createTensor(B, D_out);
//   const T  = bro.tensor.createTensor(B, D_out);
//   const dY = bro.tensor.createTensor(B, D_out);
//   const dX = bro.tensor.createTensor(B, D_in);
//   const dW = bro.tensor.createTensor(D_out, D_in);
//   const dB = bro.tensor.createTensor(D_out, 1);
//   const mW = W.clone(), vW = W.clone();   // Adam moments, zeroed below
//   mW.zero(); vW.zero();
//
//   let rng = bro.tensor.xavierInit(W, 7);
//   bro.tensor.randUniform(rng, 0, X);
//   bro.tensor.randUniform(rng, 1, T);
//
//   for (let step = 1; step <= 100; step++) {
//       bro.tensor.linearForwardBatched(W, b, X, Y);
//       const loss = bro.tensor.mseVecForward(Y, T);
//       bro.tensor.mseVecBackward(Y, T, dY);
//       dW.zero(); dB.zero();
//       bro.tensor.linearBackwardBatched(W, X, dY, dX, dW, dB);
//       bro.tensor.adamStep(W, dW, mW, vW, 1e-3, 0.9, 0.999, 1e-8, step);
//       if (step % 20 === 0) console.log(step, loss);
//   }
//   bro.tensor.sync();
//   console.log(W.download().slice(0, 4));
