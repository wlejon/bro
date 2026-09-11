// =============================================================================
// bro_tensor_c_abi.h — Pure C-ABI declarations for bro.tensor
// Generated automatically by brosurface (gen/emit_c_abi.mjs).
// Zero dynamic boxing, direct native CPU register call.
// =============================================================================

#ifndef BRO_TENSOR_C_ABI_H
#define BRO_TENSOR_C_ABI_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// --- Interface bro.tensor.GpuTensor ---
void* bro_GpuTensor_create(void);
void  bro_GpuTensor_destroy(void* self);
int32_t bro_GpuTensor_get_rows(void* self);
int32_t bro_GpuTensor_get_cols(void* self);
int32_t bro_GpuTensor_get_size(void* self);
int32_t bro_GpuTensor_get_bytes(void* self);
void bro_GpuTensor_zero(void* self);
void bro_GpuTensor_resize(void* self, int32_t rows, int32_t cols, void* dtype);
const char* bro_GpuTensor_dtype(void* self);
void* bro_GpuTensor_clone(void* self);
void bro_GpuTensor_upload(void* self, void* src);
void* bro_GpuTensor_download(void* self, void* dst);
void bro_GpuTensor_uploadFp16(void* self, void* data);
void* bro_GpuTensor_downloadFp16(void* self);
void bro_GpuTensor_uploadInt8(void* self, void* data);

// --- Namespace bro.tensor ---
bool bro_tensor_get_available(void);
const char* bro_tensor_get_backend(void);
void bro_tensor_init(void);
void bro_tensor_sync(void);
void* bro_tensor_createTensor(int32_t rows, int32_t cols, void* dtype);
void bro_tensor_linearForward(void* W, void* b, void* x, void* y);
void bro_tensor_linearBackward(void* W, void* x, void* dY, void* dX, void* dW, void* dB);
void bro_tensor_reluForward(void* x, void* y);
void bro_tensor_reluBackward(void* x, void* dY, void* dX);
void bro_tensor_tanhForward(void* x, void* y);
void bro_tensor_tanhBackward(void* y, void* dY, void* dX);
void bro_tensor_sigmoidForward(void* x, void* y);
void bro_tensor_sigmoidBackward(void* y, void* dY, void* dX);
void bro_tensor_addInplace(void* y, void* x);
void bro_tensor_addScalarInplace(void* y, double s);
void bro_tensor_scaleInplace(void* y, double s);
void bro_tensor_mulInplace(void* y, void* x);
void bro_tensor_clamp(void* y, double lo, double hi);
void bro_tensor_buildSlotMask(void* x, int32_t offset, int32_t K, int32_t stride, void* mask);
void bro_tensor_copyD2D(void* src, int32_t srcOff, void* dst, int32_t dstOff, int32_t n);
void bro_tensor_cast(void* src, void* dst, void* outDtype);
void bro_tensor_siluForward(void* x, void* y);
void bro_tensor_siluBackward(void* x, void* dY, void* dX);
void bro_tensor_geluForward(void* x, void* y);
void bro_tensor_geluBackward(void* x, void* dY, void* dX);
void bro_tensor_geluExactForward(void* x, void* y);
void bro_tensor_geluExactBackward(void* x, void* dY, void* dX);
void bro_tensor_quickGeluForward(void* x, void* y);
void bro_tensor_quickGeluBackward(void* x, void* dY, void* dX);
void bro_tensor_swigluForward(void* X, void* Y);
void bro_tensor_swigluBackward(void* X, void* dY, void* dX);
void bro_tensor_gegluForward(void* X, void* Y);
void bro_tensor_gegluBackward(void* X, void* dY, void* dX);
void bro_tensor_gegluExactForward(void* X, void* Y);
void bro_tensor_gegluExactBackward(void* X, void* dY, void* dX);
void bro_tensor_softmaxForward(void* logits, void* probs, void* mask);
void bro_tensor_softmaxBackward(void* probs, void* dProbs, void* dLogits);
void* bro_tensor_layernormForward(void* x, void* gamma, void* beta, void* y, void* xhat, double eps);
void bro_tensor_layernormBackward(void* dY, void* xhat, void* gamma, double rstd, void* dX, void* dGamma, void* dBeta);
void bro_tensor_layernormForwardInferenceBatched(void* X_RD, void* gamma, void* beta, void* Y_RD, double eps);
void bro_tensor_layernormForwardInferenceBatchedFp16(void* X_RD, void* gamma, void* beta, void* Y_RD, double eps);
void bro_tensor_rmsNormForward(void* X, void* gamma, double eps, void* Y);
void bro_tensor_rmsNormBackward(void* X, void* gamma, void* dY, double eps, void* dX, void* dGamma);
void bro_tensor_groupNormForward(void* X, void* gamma, void* beta, int32_t N, int32_t C, int32_t H, int32_t W, int32_t numGroups, double eps, void* Y);
void bro_tensor_groupNormBackward(void* X, void* gamma, void* dY, int32_t N, int32_t C, int32_t H, int32_t W, int32_t numGroups, double eps, void* dX, void* dGamma, void* dBeta);
void bro_tensor_matmul(void* A, void* B, void* C);
void bro_tensor_matmulBackward(void* A, void* B, void* dC, void* dA, void* dB);
void bro_tensor_ropeForward(void* X, int32_t headDim, int32_t numHeads, int32_t seqOffset, double thetaBase, void* Y);
void bro_tensor_ropeBackward(void* dY, int32_t headDim, int32_t numHeads, int32_t seqOffset, double thetaBase, void* dX);
void bro_tensor_ropeApply(void* X, void* cosTbl, void* sinTbl, int32_t headDim, int32_t numHeads, void* Y);
void bro_tensor_ropeApplyBackward(void* dY, int32_t headDim, int32_t numHeads, void* dX);
void bro_tensor_modulate(void* X, void* scale, void* shift, void* Y);
void bro_tensor_broadcastMul(void* X, void* v, void* Y);
void bro_tensor_sumRows(void* X, void* Y);
void bro_tensor_sumCols(void* X, void* Y);
void bro_tensor_argmaxRows(void* X, void* Idx);
void bro_tensor_attentionForward(void* X, void* Wq, void* Wk, void* Wv, void* Wo, void* mask, void* Q, void* K, void* V, void* Attn, void* Y_pre_Wo, void* O);
void bro_tensor_attentionBackward(void* dO, void* X, void* Q, void* K, void* V, void* Attn, void* Y_pre_Wo, void* Wq, void* Wk, void* Wv, void* Wo, void* mask, void* dX, void* dWq, void* dWk, void* dWv, void* dWo);
void bro_tensor_mhaForward(void* X, void* Wq, void* Wk, void* Wv, void* Wo, void* mask, int32_t numHeads, void* Qh, void* Kh, void* Vh, void* Attnh, void* Yconcat, void* O);
void bro_tensor_mhaBackward(void* dO, void* X, void* Qh, void* Kh, void* Vh, void* Attnh, void* Yconcat, void* Wq, void* Wk, void* Wv, void* Wo, void* mask, int32_t numHeads, void* dX, void* dWq, void* dWk, void* dWv, void* dWo);
void bro_tensor_selfAttentionForward(void* X, void* Wq, void* Wk, void* Wv, void* Wo, void* mask, int32_t numHeads, void* O);
void bro_tensor_selfAttentionForwardTrain(void* X, void* Wq, void* Wk, void* Wv, void* Wo, void* mask, int32_t numHeads, void* Qh, void* Kh, void* Vh, void* Attnh, void* Yconcat, void* O);
void bro_tensor_selfAttentionBackward(void* dO, void* X, void* Qh, void* Kh, void* Vh, void* Attnh, void* Yconcat, void* Wq, void* Wk, void* Wv, void* Wo, void* mask, int32_t numHeads, void* dX, void* dWq, void* dWk, void* dWv, void* dWo);
void bro_tensor_crossAttentionForward(void* X, void* Ctx, void* Wq, void* Wk, void* Wv, void* Wo, void* mask, int32_t numHeads, void* O);
void bro_tensor_crossAttentionForwardWithAttn(void* X, void* Ctx, void* Wq, void* Wk, void* Wv, void* Wo, void* mask, void* attnLogitBias, int32_t numHeads, void* O, void* AttnAvg);
void bro_tensor_crossAttentionForwardTrain(void* X, void* Ctx, void* Wq, void* Wk, void* Wv, void* Wo, void* mask, int32_t numHeads, void* Qh, void* Kh, void* Vh, void* Attnh, void* Yconcat, void* O);
void bro_tensor_crossAttentionBackward(void* dO, void* X, void* Ctx, void* Qh, void* Kh, void* Vh, void* Attnh, void* Yconcat, void* Wq, void* Wk, void* Wv, void* Wo, void* mask, int32_t numHeads, void* dX, void* dCtx, void* dWq, void* dWk, void* dWv, void* dWo);
void bro_tensor_attentionTokenMoments(void* Attn, int32_t h_lat, int32_t w_lat, void* mass, void* centroid);
void bro_tensor_buildCausalMaskRow(int32_t L, int32_t q, void* mask);
void bro_tensor_selfAttentionBiasForward(void* X, void* Wq, void* Wk, void* Wv, void* Wo, void* mask, void* attnBias, int32_t numHeads, double scale, void* O);
void bro_tensor_flashAttentionForward(void* Q, void* K, void* V, void* mask, int32_t numHeads, bool causal, void* O);
void bro_tensor_flashAttentionWindowedForward(void* Q, void* K, void* V, void* mask, int32_t numHeads, int32_t window, void* O);
void bro_tensor_flashAttentionBackward(void* Q, void* K, void* V, void* O, void* dO, void* mask, int32_t numHeads, bool causal, void* dQ, void* dK, void* dV);
void bro_tensor_flashAttentionQkvoForward(void* X, void* Ctx, void* Wq, void* bq, void* Wk, void* bk, void* Wv, void* bv, void* Wo, void* bo, void* mask, int32_t numHeads, bool causal, void* O);
void bro_tensor_flashAttentionQkvoBackward(void* opts);
void bro_tensor_flashAttentionProjectKv(void* ctx, void* Wk, void* bk, void* Wv, void* bv, void* K_out, void* V_out);
void bro_tensor_flashAttentionQWithKvCachedForward(void* X, void* K, void* V, void* Wq, void* bq, void* Wo, void* bo, void* mask, int32_t numHeads, bool causal, void* O);
void bro_tensor_flashAttentionDecode(void* Q, void* K_cache, void* V_cache, int32_t validLen, int32_t numHeads, void* O, int32_t numKvHeads, double attnSoftcap, int32_t window);
void bro_tensor_flashAttentionDecodeMasked(void* Q, void* K_cache, void* V_cache, void* dMask, int32_t numHeads, void* O, int32_t numKvHeads, double attnSoftcap, int32_t window);
void bro_tensor_kvCacheAppend(void* K_new, void* V_new, int32_t curLen, void* K_cache, void* V_cache);
void bro_tensor_conv2dForward(void* X, void* Wt, void* bias, int32_t N, int32_t C_in, int32_t H, int32_t W, int32_t C_out, int32_t kH, int32_t kW, int32_t sH, int32_t sW, int32_t pH, int32_t pW, int32_t dH, int32_t dW, int32_t groups, void* Y);
void bro_tensor_conv2dBackwardInput(void* Wt, void* dY, int32_t N, int32_t C_in, int32_t H, int32_t W, int32_t C_out, int32_t kH, int32_t kW, int32_t sH, int32_t sW, int32_t pH, int32_t pW, int32_t dH, int32_t dW, int32_t groups, void* dX);
void bro_tensor_conv2dBackwardWeight(void* X, void* dY, int32_t N, int32_t C_in, int32_t H, int32_t W, int32_t C_out, int32_t kH, int32_t kW, int32_t sH, int32_t sW, int32_t pH, int32_t pW, int32_t dH, int32_t dW, int32_t groups, void* dWt);
void bro_tensor_conv2dBackwardBias(void* dY, int32_t N, int32_t C_out, int32_t H_out, int32_t W_out, void* dB);
void bro_tensor_upsampleNearest2xForward(void* X, int32_t N, int32_t C, int32_t H, int32_t W, void* Y);
void bro_tensor_upsampleNearest2xBackward(void* dY, int32_t N, int32_t C, int32_t H, int32_t W, void* dX);
void bro_tensor_upsampleBilinear2xForward(void* X, int32_t N, int32_t C, int32_t H, int32_t W, void* Y);
void bro_tensor_upsampleBilinear2xBackward(void* dY, int32_t N, int32_t C, int32_t H, int32_t W, void* dX);
void bro_tensor_downsampleAvg2xForward(void* X, int32_t N, int32_t C, int32_t H, int32_t W, void* Y);
void bro_tensor_downsampleAvg2xBackward(void* dY, int32_t N, int32_t C, int32_t H, int32_t W, void* dX);
void bro_tensor_nchwToSequence(void* X, int32_t N, int32_t C, int32_t H, int32_t W, void* Y);
void bro_tensor_sequenceToNchw(void* X, int32_t N, int32_t C, int32_t H, int32_t W, void* Y);
void bro_tensor_interp2dForward(void* X, int32_t N, int32_t C, int32_t H_in, int32_t W_in, int32_t H_out, int32_t W_out, int32_t mode, void* Y);
void bro_tensor_interp2dAlignCornersForward(void* X, int32_t N, int32_t C, int32_t H_in, int32_t W_in, int32_t H_out, int32_t W_out, int32_t mode, void* Y);
void bro_tensor_unfold2dForward(void* X, int32_t N, int32_t C, int32_t H, int32_t W, int32_t kH, int32_t kW, int32_t sH, int32_t sW, int32_t padT, int32_t padB, int32_t padL, int32_t padR, int32_t mode, void* Y);
void bro_tensor_l2NormalizeNchwForward(void* X, int32_t N, int32_t C, int32_t H, int32_t W, double eps, void* Y);
void bro_tensor_convexUpsampleForward(void* X, void* Mask, int32_t N, int32_t C, int32_t H, int32_t W, int32_t scale, void* Y);
void bro_tensor_resblockForward(void* opts);
void bro_tensor_resblockBackward(void* opts);
void bro_tensor_maskedMeanPoolForward(void* X, void* mask, void* y);
void bro_tensor_maskedMeanPoolBackward(void* dY, void* mask, int32_t K, void* dX);
double bro_tensor_mseVecForward(void* pred, void* target);
void bro_tensor_mseVecBackward(void* pred, void* target, void* dPred);
void bro_tensor_mseVecPerSample(void* pred, void* target, void* dPred, void* lossPerSample);
double bro_tensor_softmaxXentFused(void* logits, void* target, void* mask, void* probs, void* dLogits);
void bro_tensor_softmaxXentFusedBatched(void* logits_BL, void* target_BL, void* mask, void* headOffsets, int32_t n_heads, void* probs_BL, void* dLogits_BL, void* lossPerSample);
void bro_tensor_embeddingLookupForward(void* table, void* idxAsInt32, int32_t B, void* out);
void bro_tensor_embeddingLookupBackward(void* dOut, void* idxAsInt32, int32_t B, void* dTable);
void bro_tensor_concatRows(void* parts, void* out);
void bro_tensor_splitRows(void* in_, void* parts);
void bro_tensor_concatBatchedRows(void* parts, void* out);
void bro_tensor_concatNchwChannels(void* parts, int32_t N, int32_t H, int32_t W, void* C_per_part, void* out);
void bro_tensor_concatNchwChannelsBackward(void* dY, int32_t N, int32_t H, int32_t W, void* C_per_part, void* dParts);
void bro_tensor_sgdStep(void* param, void* grad, void* velocity, double lr, double momentum);
void bro_tensor_adamStep(void* param, void* grad, void* m, void* v, double lr, double beta1, double beta2, double eps, int32_t step);

#ifdef __cplusplus
}
#endif

#endif // BRO_TENSOR_C_ABI_H
