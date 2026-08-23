// ── Classes & Interfaces ─────────────────────────────────────────────────────

/**
 * =============================================================================
 * bro.tensor — GPU tensor + ops (brotensor: CUDA or Metal)
 * =============================================================================
 *
 * Wraps the brotensor sibling library. brotensor exposes one unified tensor
 * type with a runtime Device tag and device-neutral ops; bro.tensor is the
 * GPU-resident face of it. The op surface is identical across CUDA (NVIDIA)
 * and Metal (Apple) backends.
 * @example
 * if (bro.tensor.available) {
 *     bro.tensor.init();
 *     const t = bro.tensor.createTensor(3, 4);
 *     t.zero();
 *     const data = t.download();
 *   }
 */
class GpuTensor {

  /**
   * @readonly
   * @type {number}
   */
  rows;

  /**
   * @readonly
   * @type {number}
   */
  cols;

  /**
   * @readonly
   * @type {number}
   */
  size;

  /**
   * @readonly
   * @type {number}
   */
  bytes;

  zero() {}

  /**
   * @param {number} rows
   * @param {number} cols
   * @param {(string|number)} [dtype]
   */
  resize(rows, cols, dtype) {}

  /**
   * @returns {string}
   */
  dtype() {}

  /**
   * @returns {GpuTensor}
   */
  clone() {}

  /**
   * @param {(Float32Array|Object)} src
   */
  upload(src) {}

  /**
   * @param {Object} [dst]
   * @returns {(Float32Array|void)}
   */
  download(dst) {}

  /**
   * @param {Uint16Array} data
   */
  uploadFp16(data) {}

  /**
   * @returns {Uint16Array}
   */
  downloadFp16() {}

  /**
   * @param {Int8Array} data
   */
  uploadInt8(data) {}

}

// ── Namespaces ───────────────────────────────────────────────────────────────

/**
 * @readonly
 * @type {boolean}
 */
bro.tensor.available;

/**
 * @readonly
 * @type {string}
 */
bro.tensor.backend;

bro.tensor.init = function() {};

bro.tensor.sync = function() {};

/**
 * @param {number} rows
 * @param {number} [cols=1]
 * @param {(string|number)} [dtype="fp32"]
 * @returns {GpuTensor}
 */
bro.tensor.createTensor = function(rows, cols, dtype) {};

/**
 * @param {GpuTensor} W
 * @param {GpuTensor} b
 * @param {GpuTensor} x
 * @param {GpuTensor} y
 */
bro.tensor.linearForward = function(W, b, x, y) {};

/**
 * @param {GpuTensor} W
 * @param {GpuTensor} x
 * @param {GpuTensor} dY
 * @param {GpuTensor} dX
 * @param {GpuTensor} dW
 * @param {GpuTensor} dB
 */
bro.tensor.linearBackward = function(W, x, dY, dX, dW, dB) {};

/**
 * @param {GpuTensor} x
 * @param {GpuTensor} y
 */
bro.tensor.reluForward = function(x, y) {};

/**
 * @param {GpuTensor} x
 * @param {GpuTensor} dY
 * @param {GpuTensor} dX
 */
bro.tensor.reluBackward = function(x, dY, dX) {};

/**
 * @param {GpuTensor} x
 * @param {GpuTensor} y
 */
bro.tensor.tanhForward = function(x, y) {};

/**
 * @param {GpuTensor} y
 * @param {GpuTensor} dY
 * @param {GpuTensor} dX
 */
bro.tensor.tanhBackward = function(y, dY, dX) {};

/**
 * @param {GpuTensor} x
 * @param {GpuTensor} y
 */
bro.tensor.sigmoidForward = function(x, y) {};

/**
 * @param {GpuTensor} y
 * @param {GpuTensor} dY
 * @param {GpuTensor} dX
 */
bro.tensor.sigmoidBackward = function(y, dY, dX) {};

/**
 * @param {GpuTensor} y
 * @param {GpuTensor} x
 */
bro.tensor.addInplace = function(y, x) {};

/**
 * @param {GpuTensor} y
 * @param {number} s
 */
bro.tensor.addScalarInplace = function(y, s) {};

/**
 * @param {GpuTensor} y
 * @param {number} s
 */
bro.tensor.scaleInplace = function(y, s) {};

/**
 * @param {GpuTensor} y
 * @param {GpuTensor} x
 */
bro.tensor.mulInplace = function(y, x) {};

/**
 * @param {GpuTensor} y
 * @param {number} lo
 * @param {number} hi
 */
bro.tensor.clamp = function(y, lo, hi) {};

/**
 * @param {GpuTensor} x
 * @param {number} offset
 * @param {number} K
 * @param {number} stride
 * @param {GpuTensor} mask
 */
bro.tensor.buildSlotMask = function(x, offset, K, stride, mask) {};

/**
 * @param {GpuTensor} src
 * @param {number} srcOff
 * @param {GpuTensor} dst
 * @param {number} dstOff
 * @param {number} n
 */
bro.tensor.copyD2D = function(src, srcOff, dst, dstOff, n) {};

/**
 * @param {GpuTensor} src
 * @param {GpuTensor} dst
 * @param {(string|number)} outDtype
 */
bro.tensor.cast = function(src, dst, outDtype) {};

/**
 * @param {GpuTensor} x
 * @param {GpuTensor} y
 */
bro.tensor.siluForward = function(x, y) {};

/**
 * @param {GpuTensor} x
 * @param {GpuTensor} dY
 * @param {GpuTensor} dX
 */
bro.tensor.siluBackward = function(x, dY, dX) {};

/**
 * @param {GpuTensor} x
 * @param {GpuTensor} y
 */
bro.tensor.geluForward = function(x, y) {};

/**
 * @param {GpuTensor} x
 * @param {GpuTensor} dY
 * @param {GpuTensor} dX
 */
bro.tensor.geluBackward = function(x, dY, dX) {};

/**
 * @param {GpuTensor} x
 * @param {GpuTensor} y
 */
bro.tensor.geluExactForward = function(x, y) {};

/**
 * @param {GpuTensor} x
 * @param {GpuTensor} dY
 * @param {GpuTensor} dX
 */
bro.tensor.geluExactBackward = function(x, dY, dX) {};

/**
 * @param {GpuTensor} x
 * @param {GpuTensor} y
 */
bro.tensor.quickGeluForward = function(x, y) {};

/**
 * @param {GpuTensor} x
 * @param {GpuTensor} dY
 * @param {GpuTensor} dX
 */
bro.tensor.quickGeluBackward = function(x, dY, dX) {};

/**
 * @param {GpuTensor} X
 * @param {GpuTensor} Y
 */
bro.tensor.swigluForward = function(X, Y) {};

/**
 * @param {GpuTensor} X
 * @param {GpuTensor} dY
 * @param {GpuTensor} dX
 */
bro.tensor.swigluBackward = function(X, dY, dX) {};

/**
 * @param {GpuTensor} X
 * @param {GpuTensor} Y
 */
bro.tensor.gegluForward = function(X, Y) {};

/**
 * @param {GpuTensor} X
 * @param {GpuTensor} dY
 * @param {GpuTensor} dX
 */
bro.tensor.gegluBackward = function(X, dY, dX) {};

/**
 * @param {GpuTensor} X
 * @param {GpuTensor} Y
 */
bro.tensor.gegluExactForward = function(X, Y) {};

/**
 * @param {GpuTensor} X
 * @param {GpuTensor} dY
 * @param {GpuTensor} dX
 */
bro.tensor.gegluExactBackward = function(X, dY, dX) {};

/**
 * @param {GpuTensor} logits
 * @param {GpuTensor} probs
 * @param {GpuTensor|null} [mask]
 */
bro.tensor.softmaxForward = function(logits, probs, mask) {};

/**
 * @param {GpuTensor} probs
 * @param {GpuTensor} dProbs
 * @param {GpuTensor} dLogits
 */
bro.tensor.softmaxBackward = function(probs, dProbs, dLogits) {};

/**
 * @param {GpuTensor} x
 * @param {GpuTensor} gamma
 * @param {GpuTensor} beta
 * @param {GpuTensor} y
 * @param {GpuTensor} xhat
 * @param {number} [eps=0.00001]
 * @returns {Object}
 */
bro.tensor.layernormForward = function(x, gamma, beta, y, xhat, eps) {};

/**
 * @param {GpuTensor} dY
 * @param {GpuTensor} xhat
 * @param {GpuTensor} gamma
 * @param {number} rstd
 * @param {GpuTensor} dX
 * @param {GpuTensor} dGamma
 * @param {GpuTensor} dBeta
 */
bro.tensor.layernormBackward = function(dY, xhat, gamma, rstd, dX, dGamma, dBeta) {};

/**
 * @param {GpuTensor} X_RD
 * @param {GpuTensor} gamma
 * @param {GpuTensor} beta
 * @param {GpuTensor} Y_RD
 * @param {number} [eps=0.00001]
 */
bro.tensor.layernormForwardInferenceBatched = function(X_RD, gamma, beta, Y_RD, eps) {};

/**
 * @param {GpuTensor} X_RD
 * @param {GpuTensor} gamma
 * @param {GpuTensor} beta
 * @param {GpuTensor} Y_RD
 * @param {number} [eps=0.00001]
 */
bro.tensor.layernormForwardInferenceBatchedFp16 = function(X_RD, gamma, beta, Y_RD, eps) {};

/**
 * @param {GpuTensor} X
 * @param {GpuTensor} gamma
 * @param {number} eps
 * @param {GpuTensor} Y
 */
bro.tensor.rmsNormForward = function(X, gamma, eps, Y) {};

/**
 * @param {GpuTensor} X
 * @param {GpuTensor} gamma
 * @param {GpuTensor} dY
 * @param {number} eps
 * @param {GpuTensor} dX
 * @param {GpuTensor} dGamma
 */
bro.tensor.rmsNormBackward = function(X, gamma, dY, eps, dX, dGamma) {};

/**
 * @param {GpuTensor} X
 * @param {GpuTensor} gamma
 * @param {GpuTensor} beta
 * @param {number} N
 * @param {number} C
 * @param {number} H
 * @param {number} W
 * @param {number} numGroups
 * @param {number} eps
 * @param {GpuTensor} Y
 */
bro.tensor.groupNormForward = function(X, gamma, beta, N, C, H, W, numGroups, eps, Y) {};

/**
 * @param {GpuTensor} X
 * @param {GpuTensor} gamma
 * @param {GpuTensor} dY
 * @param {number} N
 * @param {number} C
 * @param {number} H
 * @param {number} W
 * @param {number} numGroups
 * @param {number} eps
 * @param {GpuTensor} dX
 * @param {GpuTensor} dGamma
 * @param {GpuTensor} dBeta
 */
bro.tensor.groupNormBackward = function(X, gamma, dY, N, C, H, W, numGroups, eps, dX, dGamma, dBeta) {};

/**
 * @param {GpuTensor} A
 * @param {GpuTensor} B
 * @param {GpuTensor} C
 */
bro.tensor.matmul = function(A, B, C) {};

/**
 * @param {GpuTensor} A
 * @param {GpuTensor} B
 * @param {GpuTensor} dC
 * @param {GpuTensor} dA
 * @param {GpuTensor} dB
 */
bro.tensor.matmulBackward = function(A, B, dC, dA, dB) {};

/**
 * @param {GpuTensor} X
 * @param {number} headDim
 * @param {number} numHeads
 * @param {number} seqOffset
 * @param {number} thetaBase
 * @param {GpuTensor} Y
 */
bro.tensor.ropeForward = function(X, headDim, numHeads, seqOffset, thetaBase, Y) {};

/**
 * @param {GpuTensor} dY
 * @param {number} headDim
 * @param {number} numHeads
 * @param {number} seqOffset
 * @param {number} thetaBase
 * @param {GpuTensor} dX
 */
bro.tensor.ropeBackward = function(dY, headDim, numHeads, seqOffset, thetaBase, dX) {};

/**
 * @param {GpuTensor} X
 * @param {GpuTensor} cosTbl
 * @param {GpuTensor} sinTbl
 * @param {number} headDim
 * @param {number} numHeads
 * @param {GpuTensor} Y
 */
bro.tensor.ropeApply = function(X, cosTbl, sinTbl, headDim, numHeads, Y) {};

/**
 * @param {GpuTensor} dY
 * @param {number} headDim
 * @param {number} numHeads
 * @param {GpuTensor} dX
 */
bro.tensor.ropeApplyBackward = function(dY, headDim, numHeads, dX) {};

/**
 * @param {GpuTensor} X
 * @param {GpuTensor} scale
 * @param {GpuTensor} shift
 * @param {GpuTensor} Y
 */
bro.tensor.modulate = function(X, scale, shift, Y) {};

/**
 * @param {GpuTensor} X
 * @param {GpuTensor} v
 * @param {GpuTensor} Y
 */
bro.tensor.broadcastMul = function(X, v, Y) {};

/**
 * @param {GpuTensor} X
 * @param {GpuTensor} Y
 */
bro.tensor.sumRows = function(X, Y) {};

/**
 * @param {GpuTensor} X
 * @param {GpuTensor} Y
 */
bro.tensor.sumCols = function(X, Y) {};

/**
 * @param {GpuTensor} X
 * @param {GpuTensor} Idx
 */
bro.tensor.argmaxRows = function(X, Idx) {};

/**
 * @param {GpuTensor} X
 * @param {GpuTensor} Wq
 * @param {GpuTensor} Wk
 * @param {GpuTensor} Wv
 * @param {GpuTensor} Wo
 * @param {GpuTensor|null} mask
 * @param {GpuTensor} Q
 * @param {GpuTensor} K
 * @param {GpuTensor} V
 * @param {GpuTensor} Attn
 * @param {GpuTensor} Y_pre_Wo
 * @param {GpuTensor} O
 */
bro.tensor.attentionForward = function(X, Wq, Wk, Wv, Wo, mask, Q, K, V, Attn, Y_pre_Wo, O) {};

/**
 * @param {GpuTensor} dO
 * @param {GpuTensor} X
 * @param {GpuTensor} Q
 * @param {GpuTensor} K
 * @param {GpuTensor} V
 * @param {GpuTensor} Attn
 * @param {GpuTensor} Y_pre_Wo
 * @param {GpuTensor} Wq
 * @param {GpuTensor} Wk
 * @param {GpuTensor} Wv
 * @param {GpuTensor} Wo
 * @param {GpuTensor|null} mask
 * @param {GpuTensor} dX
 * @param {GpuTensor} dWq
 * @param {GpuTensor} dWk
 * @param {GpuTensor} dWv
 * @param {GpuTensor} dWo
 */
bro.tensor.attentionBackward = function(dO, X, Q, K, V, Attn, Y_pre_Wo, Wq, Wk, Wv, Wo, mask, dX, dWq, dWk, dWv, dWo) {};

/**
 * @param {GpuTensor} X
 * @param {GpuTensor} Wq
 * @param {GpuTensor} Wk
 * @param {GpuTensor} Wv
 * @param {GpuTensor} Wo
 * @param {GpuTensor|null} mask
 * @param {number} numHeads
 * @param {GpuTensor} Qh
 * @param {GpuTensor} Kh
 * @param {GpuTensor} Vh
 * @param {GpuTensor} Attnh
 * @param {GpuTensor} Yconcat
 * @param {GpuTensor} O
 */
bro.tensor.mhaForward = function(X, Wq, Wk, Wv, Wo, mask, numHeads, Qh, Kh, Vh, Attnh, Yconcat, O) {};

/**
 * @param {GpuTensor} dO
 * @param {GpuTensor} X
 * @param {GpuTensor} Qh
 * @param {GpuTensor} Kh
 * @param {GpuTensor} Vh
 * @param {GpuTensor} Attnh
 * @param {GpuTensor} Yconcat
 * @param {GpuTensor} Wq
 * @param {GpuTensor} Wk
 * @param {GpuTensor} Wv
 * @param {GpuTensor} Wo
 * @param {GpuTensor|null} mask
 * @param {number} numHeads
 * @param {GpuTensor} dX
 * @param {GpuTensor} dWq
 * @param {GpuTensor} dWk
 * @param {GpuTensor} dWv
 * @param {GpuTensor} dWo
 */
bro.tensor.mhaBackward = function(dO, X, Qh, Kh, Vh, Attnh, Yconcat, Wq, Wk, Wv, Wo, mask, numHeads, dX, dWq, dWk, dWv, dWo) {};

/**
 * @param {GpuTensor} X
 * @param {GpuTensor} Wq
 * @param {GpuTensor} Wk
 * @param {GpuTensor} Wv
 * @param {GpuTensor} Wo
 * @param {GpuTensor|null} mask
 * @param {number} numHeads
 * @param {GpuTensor} O
 */
bro.tensor.selfAttentionForward = function(X, Wq, Wk, Wv, Wo, mask, numHeads, O) {};

/**
 * @param {GpuTensor} X
 * @param {GpuTensor} Wq
 * @param {GpuTensor} Wk
 * @param {GpuTensor} Wv
 * @param {GpuTensor} Wo
 * @param {GpuTensor|null} mask
 * @param {number} numHeads
 * @param {GpuTensor} Qh
 * @param {GpuTensor} Kh
 * @param {GpuTensor} Vh
 * @param {GpuTensor} Attnh
 * @param {GpuTensor} Yconcat
 * @param {GpuTensor} O
 */
bro.tensor.selfAttentionForwardTrain = function(X, Wq, Wk, Wv, Wo, mask, numHeads, Qh, Kh, Vh, Attnh, Yconcat, O) {};

/**
 * @param {GpuTensor} dO
 * @param {GpuTensor} X
 * @param {GpuTensor} Qh
 * @param {GpuTensor} Kh
 * @param {GpuTensor} Vh
 * @param {GpuTensor} Attnh
 * @param {GpuTensor} Yconcat
 * @param {GpuTensor} Wq
 * @param {GpuTensor} Wk
 * @param {GpuTensor} Wv
 * @param {GpuTensor} Wo
 * @param {GpuTensor|null} mask
 * @param {number} numHeads
 * @param {GpuTensor} dX
 * @param {GpuTensor} dWq
 * @param {GpuTensor} dWk
 * @param {GpuTensor} dWv
 * @param {GpuTensor} dWo
 */
bro.tensor.selfAttentionBackward = function(dO, X, Qh, Kh, Vh, Attnh, Yconcat, Wq, Wk, Wv, Wo, mask, numHeads, dX, dWq, dWk, dWv, dWo) {};

/**
 * @param {GpuTensor} X
 * @param {GpuTensor} Ctx
 * @param {GpuTensor} Wq
 * @param {GpuTensor} Wk
 * @param {GpuTensor} Wv
 * @param {GpuTensor} Wo
 * @param {GpuTensor|null} mask
 * @param {number} numHeads
 * @param {GpuTensor} O
 */
bro.tensor.crossAttentionForward = function(X, Ctx, Wq, Wk, Wv, Wo, mask, numHeads, O) {};

/**
 * @param {GpuTensor} X
 * @param {GpuTensor} Ctx
 * @param {GpuTensor} Wq
 * @param {GpuTensor} Wk
 * @param {GpuTensor} Wv
 * @param {GpuTensor} Wo
 * @param {GpuTensor|null} mask
 * @param {GpuTensor|null} attnLogitBias
 * @param {number} numHeads
 * @param {GpuTensor} O
 * @param {GpuTensor} AttnAvg
 */
bro.tensor.crossAttentionForwardWithAttn = function(X, Ctx, Wq, Wk, Wv, Wo, mask, attnLogitBias, numHeads, O, AttnAvg) {};

/**
 * @param {GpuTensor} X
 * @param {GpuTensor} Ctx
 * @param {GpuTensor} Wq
 * @param {GpuTensor} Wk
 * @param {GpuTensor} Wv
 * @param {GpuTensor} Wo
 * @param {GpuTensor|null} mask
 * @param {number} numHeads
 * @param {GpuTensor} Qh
 * @param {GpuTensor} Kh
 * @param {GpuTensor} Vh
 * @param {GpuTensor} Attnh
 * @param {GpuTensor} Yconcat
 * @param {GpuTensor} O
 */
bro.tensor.crossAttentionForwardTrain = function(X, Ctx, Wq, Wk, Wv, Wo, mask, numHeads, Qh, Kh, Vh, Attnh, Yconcat, O) {};

/**
 * @param {GpuTensor} dO
 * @param {GpuTensor} X
 * @param {GpuTensor} Ctx
 * @param {GpuTensor} Qh
 * @param {GpuTensor} Kh
 * @param {GpuTensor} Vh
 * @param {GpuTensor} Attnh
 * @param {GpuTensor} Yconcat
 * @param {GpuTensor} Wq
 * @param {GpuTensor} Wk
 * @param {GpuTensor} Wv
 * @param {GpuTensor} Wo
 * @param {GpuTensor|null} mask
 * @param {number} numHeads
 * @param {GpuTensor} dX
 * @param {GpuTensor} dCtx
 * @param {GpuTensor} dWq
 * @param {GpuTensor} dWk
 * @param {GpuTensor} dWv
 * @param {GpuTensor} dWo
 */
bro.tensor.crossAttentionBackward = function(dO, X, Ctx, Qh, Kh, Vh, Attnh, Yconcat, Wq, Wk, Wv, Wo, mask, numHeads, dX, dCtx, dWq, dWk, dWv, dWo) {};

/**
 * @param {GpuTensor} Attn
 * @param {number} h_lat
 * @param {number} w_lat
 * @param {GpuTensor} mass
 * @param {GpuTensor} centroid
 */
bro.tensor.attentionTokenMoments = function(Attn, h_lat, w_lat, mass, centroid) {};

/**
 * @param {number} L
 * @param {number} q
 * @param {GpuTensor} mask
 */
bro.tensor.buildCausalMaskRow = function(L, q, mask) {};

/**
 * @param {GpuTensor} X
 * @param {GpuTensor} Wq
 * @param {GpuTensor} Wk
 * @param {GpuTensor} Wv
 * @param {GpuTensor} Wo
 * @param {GpuTensor|null} mask
 * @param {GpuTensor|null} attnBias
 * @param {number} numHeads
 * @param {number} scale
 * @param {GpuTensor} O
 */
bro.tensor.selfAttentionBiasForward = function(X, Wq, Wk, Wv, Wo, mask, attnBias, numHeads, scale, O) {};

/**
 * @param {GpuTensor} Q
 * @param {GpuTensor} K
 * @param {GpuTensor} V
 * @param {GpuTensor|null} mask
 * @param {number} numHeads
 * @param {boolean} causal
 * @param {GpuTensor} O
 */
bro.tensor.flashAttentionForward = function(Q, K, V, mask, numHeads, causal, O) {};

/**
 * @param {GpuTensor} Q
 * @param {GpuTensor} K
 * @param {GpuTensor} V
 * @param {GpuTensor|null} mask
 * @param {number} numHeads
 * @param {number} window
 * @param {GpuTensor} O
 */
bro.tensor.flashAttentionWindowedForward = function(Q, K, V, mask, numHeads, window, O) {};

/**
 * @param {GpuTensor} Q
 * @param {GpuTensor} K
 * @param {GpuTensor} V
 * @param {GpuTensor} O
 * @param {GpuTensor} dO
 * @param {GpuTensor|null} mask
 * @param {number} numHeads
 * @param {boolean} causal
 * @param {GpuTensor} dQ
 * @param {GpuTensor} dK
 * @param {GpuTensor} dV
 */
bro.tensor.flashAttentionBackward = function(Q, K, V, O, dO, mask, numHeads, causal, dQ, dK, dV) {};

/**
 * @param {GpuTensor} X
 * @param {GpuTensor|null} Ctx
 * @param {GpuTensor} Wq
 * @param {GpuTensor|null} bq
 * @param {GpuTensor} Wk
 * @param {GpuTensor|null} bk
 * @param {GpuTensor} Wv
 * @param {GpuTensor|null} bv
 * @param {GpuTensor} Wo
 * @param {GpuTensor|null} bo
 * @param {GpuTensor|null} mask
 * @param {number} numHeads
 * @param {boolean} causal
 * @param {GpuTensor} O
 */
bro.tensor.flashAttentionQkvoForward = function(X, Ctx, Wq, bq, Wk, bk, Wv, bv, Wo, bo, mask, numHeads, causal, O) {};

/**
 * @param {Object} opts
 */
bro.tensor.flashAttentionQkvoBackward = function(opts) {};

/**
 * @param {GpuTensor} ctx
 * @param {GpuTensor} Wk
 * @param {GpuTensor|null} bk
 * @param {GpuTensor} Wv
 * @param {GpuTensor|null} bv
 * @param {GpuTensor} K_out
 * @param {GpuTensor} V_out
 */
bro.tensor.flashAttentionProjectKv = function(ctx, Wk, bk, Wv, bv, K_out, V_out) {};

/**
 * @param {GpuTensor} X
 * @param {GpuTensor} K
 * @param {GpuTensor} V
 * @param {GpuTensor} Wq
 * @param {GpuTensor|null} bq
 * @param {GpuTensor} Wo
 * @param {GpuTensor|null} bo
 * @param {GpuTensor|null} mask
 * @param {number} numHeads
 * @param {boolean} causal
 * @param {GpuTensor} O
 */
bro.tensor.flashAttentionQWithKvCachedForward = function(X, K, V, Wq, bq, Wo, bo, mask, numHeads, causal, O) {};

/**
 * @param {GpuTensor} Q
 * @param {GpuTensor} K_cache
 * @param {GpuTensor} V_cache
 * @param {number} validLen
 * @param {number} numHeads
 * @param {GpuTensor} O
 * @param {number} [numKvHeads]
 * @param {number} [attnSoftcap=0]
 * @param {number} [window=0]
 */
bro.tensor.flashAttentionDecode = function(Q, K_cache, V_cache, validLen, numHeads, O, numKvHeads, attnSoftcap, window) {};

/**
 * @param {GpuTensor} Q
 * @param {GpuTensor} K_cache
 * @param {GpuTensor} V_cache
 * @param {GpuTensor} dMask
 * @param {number} numHeads
 * @param {GpuTensor} O
 * @param {number} [numKvHeads]
 * @param {number} [attnSoftcap=0]
 * @param {number} [window=0]
 */
bro.tensor.flashAttentionDecodeMasked = function(Q, K_cache, V_cache, dMask, numHeads, O, numKvHeads, attnSoftcap, window) {};

/**
 * @param {GpuTensor} K_new
 * @param {GpuTensor} V_new
 * @param {number} curLen
 * @param {GpuTensor} K_cache
 * @param {GpuTensor} V_cache
 */
bro.tensor.kvCacheAppend = function(K_new, V_new, curLen, K_cache, V_cache) {};

/**
 * @param {GpuTensor} X
 * @param {GpuTensor} Wt
 * @param {GpuTensor|null} bias
 * @param {number} N
 * @param {number} C_in
 * @param {number} H
 * @param {number} W
 * @param {number} C_out
 * @param {number} kH
 * @param {number} kW
 * @param {number} sH
 * @param {number} sW
 * @param {number} pH
 * @param {number} pW
 * @param {number} dH
 * @param {number} dW
 * @param {number} groups
 * @param {GpuTensor} Y
 */
bro.tensor.conv2dForward = function(X, Wt, bias, N, C_in, H, W, C_out, kH, kW, sH, sW, pH, pW, dH, dW, groups, Y) {};

/**
 * @param {GpuTensor} Wt
 * @param {GpuTensor} dY
 * @param {number} N
 * @param {number} C_in
 * @param {number} H
 * @param {number} W
 * @param {number} C_out
 * @param {number} kH
 * @param {number} kW
 * @param {number} sH
 * @param {number} sW
 * @param {number} pH
 * @param {number} pW
 * @param {number} dH
 * @param {number} dW
 * @param {number} groups
 * @param {GpuTensor} dX
 */
bro.tensor.conv2dBackwardInput = function(Wt, dY, N, C_in, H, W, C_out, kH, kW, sH, sW, pH, pW, dH, dW, groups, dX) {};

/**
 * @param {GpuTensor} X
 * @param {GpuTensor} dY
 * @param {number} N
 * @param {number} C_in
 * @param {number} H
 * @param {number} W
 * @param {number} C_out
 * @param {number} kH
 * @param {number} kW
 * @param {number} sH
 * @param {number} sW
 * @param {number} pH
 * @param {number} pW
 * @param {number} dH
 * @param {number} dW
 * @param {number} groups
 * @param {GpuTensor} dWt
 */
bro.tensor.conv2dBackwardWeight = function(X, dY, N, C_in, H, W, C_out, kH, kW, sH, sW, pH, pW, dH, dW, groups, dWt) {};

/**
 * @param {GpuTensor} dY
 * @param {number} N
 * @param {number} C_out
 * @param {number} H_out
 * @param {number} W_out
 * @param {GpuTensor} dB
 */
bro.tensor.conv2dBackwardBias = function(dY, N, C_out, H_out, W_out, dB) {};

/**
 * @param {GpuTensor} X
 * @param {number} N
 * @param {number} C
 * @param {number} H
 * @param {number} W
 * @param {GpuTensor} Y
 */
bro.tensor.upsampleNearest2xForward = function(X, N, C, H, W, Y) {};

/**
 * @param {GpuTensor} dY
 * @param {number} N
 * @param {number} C
 * @param {number} H
 * @param {number} W
 * @param {GpuTensor} dX
 */
bro.tensor.upsampleNearest2xBackward = function(dY, N, C, H, W, dX) {};

/**
 * @param {GpuTensor} X
 * @param {number} N
 * @param {number} C
 * @param {number} H
 * @param {number} W
 * @param {GpuTensor} Y
 */
bro.tensor.upsampleBilinear2xForward = function(X, N, C, H, W, Y) {};

/**
 * @param {GpuTensor} dY
 * @param {number} N
 * @param {number} C
 * @param {number} H
 * @param {number} W
 * @param {GpuTensor} dX
 */
bro.tensor.upsampleBilinear2xBackward = function(dY, N, C, H, W, dX) {};

/**
 * @param {GpuTensor} X
 * @param {number} N
 * @param {number} C
 * @param {number} H
 * @param {number} W
 * @param {GpuTensor} Y
 */
bro.tensor.downsampleAvg2xForward = function(X, N, C, H, W, Y) {};

/**
 * @param {GpuTensor} dY
 * @param {number} N
 * @param {number} C
 * @param {number} H
 * @param {number} W
 * @param {GpuTensor} dX
 */
bro.tensor.downsampleAvg2xBackward = function(dY, N, C, H, W, dX) {};

/**
 * @param {GpuTensor} X
 * @param {number} N
 * @param {number} C
 * @param {number} H
 * @param {number} W
 * @param {GpuTensor} Y
 */
bro.tensor.nchwToSequence = function(X, N, C, H, W, Y) {};

/**
 * @param {GpuTensor} X
 * @param {number} N
 * @param {number} C
 * @param {number} H
 * @param {number} W
 * @param {GpuTensor} Y
 */
bro.tensor.sequenceToNchw = function(X, N, C, H, W, Y) {};

/**
 * @param {GpuTensor} X
 * @param {number} N
 * @param {number} C
 * @param {number} H_in
 * @param {number} W_in
 * @param {number} H_out
 * @param {number} W_out
 * @param {number} mode
 * @param {GpuTensor} Y
 */
bro.tensor.interp2dForward = function(X, N, C, H_in, W_in, H_out, W_out, mode, Y) {};

/**
 * @param {GpuTensor} X
 * @param {number} N
 * @param {number} C
 * @param {number} H_in
 * @param {number} W_in
 * @param {number} H_out
 * @param {number} W_out
 * @param {number} mode
 * @param {GpuTensor} Y
 */
bro.tensor.interp2dAlignCornersForward = function(X, N, C, H_in, W_in, H_out, W_out, mode, Y) {};

/**
 * @param {GpuTensor} X
 * @param {number} N
 * @param {number} C
 * @param {number} H
 * @param {number} W
 * @param {number} kH
 * @param {number} kW
 * @param {number} sH
 * @param {number} sW
 * @param {number} padT
 * @param {number} padB
 * @param {number} padL
 * @param {number} padR
 * @param {number} mode
 * @param {GpuTensor} Y
 */
bro.tensor.unfold2dForward = function(X, N, C, H, W, kH, kW, sH, sW, padT, padB, padL, padR, mode, Y) {};

/**
 * @param {GpuTensor} X
 * @param {number} N
 * @param {number} C
 * @param {number} H
 * @param {number} W
 * @param {number} eps
 * @param {GpuTensor} Y
 */
bro.tensor.l2NormalizeNchwForward = function(X, N, C, H, W, eps, Y) {};

/**
 * @param {GpuTensor} X
 * @param {GpuTensor} Mask
 * @param {number} N
 * @param {number} C
 * @param {number} H
 * @param {number} W
 * @param {number} scale
 * @param {GpuTensor} Y
 */
bro.tensor.convexUpsampleForward = function(X, Mask, N, C, H, W, scale, Y) {};

/**
 * @param {Object} opts
 */
bro.tensor.resblockForward = function(opts) {};

/**
 * @param {Object} opts
 */
bro.tensor.resblockBackward = function(opts) {};

/**
 * @param {GpuTensor} X
 * @param {GpuTensor|null} mask
 * @param {GpuTensor} y
 */
bro.tensor.maskedMeanPoolForward = function(X, mask, y) {};

/**
 * @param {GpuTensor} dY
 * @param {GpuTensor|null} mask
 * @param {number} K
 * @param {GpuTensor} dX
 */
bro.tensor.maskedMeanPoolBackward = function(dY, mask, K, dX) {};

/**
 * @param {GpuTensor} pred
 * @param {GpuTensor} target
 * @returns {number}
 */
bro.tensor.mseVecForward = function(pred, target) {};

/**
 * @param {GpuTensor} pred
 * @param {GpuTensor} target
 * @param {GpuTensor} dPred
 */
bro.tensor.mseVecBackward = function(pred, target, dPred) {};

/**
 * @param {GpuTensor} pred
 * @param {GpuTensor} target
 * @param {GpuTensor} dPred
 * @param {GpuTensor} lossPerSample
 */
bro.tensor.mseVecPerSample = function(pred, target, dPred, lossPerSample) {};

/**
 * @param {GpuTensor} logits
 * @param {GpuTensor} target
 * @param {GpuTensor|null} mask
 * @param {GpuTensor} probs
 * @param {GpuTensor} dLogits
 * @returns {number}
 */
bro.tensor.softmaxXentFused = function(logits, target, mask, probs, dLogits) {};

/**
 * @param {GpuTensor} logits_BL
 * @param {GpuTensor} target_BL
 * @param {GpuTensor|null} mask
 * @param {GpuTensor} headOffsets
 * @param {number} n_heads
 * @param {GpuTensor} probs_BL
 * @param {GpuTensor} dLogits_BL
 * @param {GpuTensor} lossPerSample
 */
bro.tensor.softmaxXentFusedBatched = function(logits_BL, target_BL, mask, headOffsets, n_heads, probs_BL, dLogits_BL, lossPerSample) {};

/**
 * @param {GpuTensor} table
 * @param {GpuTensor} idxAsInt32
 * @param {number} B
 * @param {GpuTensor} out
 */
bro.tensor.embeddingLookupForward = function(table, idxAsInt32, B, out) {};

/**
 * @param {GpuTensor} dOut
 * @param {GpuTensor} idxAsInt32
 * @param {number} B
 * @param {GpuTensor} dTable
 */
bro.tensor.embeddingLookupBackward = function(dOut, idxAsInt32, B, dTable) {};

/**
 * @param {Array<GpuTensor>} parts
 * @param {GpuTensor} out
 */
bro.tensor.concatRows = function(parts, out) {};

/**
 * @param {GpuTensor} in_
 * @param {Array<GpuTensor>} parts
 */
bro.tensor.splitRows = function(in_, parts) {};

/**
 * @param {Array<GpuTensor>} parts
 * @param {GpuTensor} out
 */
bro.tensor.concatBatchedRows = function(parts, out) {};

/**
 * @param {Array<GpuTensor>} parts
 * @param {number} N
 * @param {number} H
 * @param {number} W
 * @param {Array<number>} C_per_part
 * @param {GpuTensor} out
 */
bro.tensor.concatNchwChannels = function(parts, N, H, W, C_per_part, out) {};

/**
 * @param {GpuTensor} dY
 * @param {number} N
 * @param {number} H
 * @param {number} W
 * @param {Array<number>} C_per_part
 * @param {Array<GpuTensor>} dParts
 */
bro.tensor.concatNchwChannelsBackward = function(dY, N, H, W, C_per_part, dParts) {};

/**
 * @param {GpuTensor} param
 * @param {GpuTensor} grad
 * @param {GpuTensor} velocity
 * @param {number} lr
 * @param {number} momentum
 */
bro.tensor.sgdStep = function(param, grad, velocity, lr, momentum) {};

/**
 * @param {GpuTensor} param
 * @param {GpuTensor} grad
 * @param {GpuTensor} m
 * @param {GpuTensor} v
 * @param {number} lr
 * @param {number} beta1
 * @param {number} beta2
 * @param {number} eps
 * @param {number} step
 */
bro.tensor.adamStep = function(param, grad, m, v, lr, beta1, beta2, eps, step) {};

