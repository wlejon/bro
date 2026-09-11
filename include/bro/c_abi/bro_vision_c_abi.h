// =============================================================================
// bro_vision_c_abi.h — Pure C-ABI declarations for bro.vision
// Generated automatically by brosurface (gen/emit_c_abi.mjs).
// Zero dynamic boxing, direct native CPU register call.
// =============================================================================

#ifndef BRO_VISION_C_ABI_H
#define BRO_VISION_C_ABI_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// --- Interface bro.vision.DepthEstimator ---
void* bro_DepthEstimator_create(void);
void  bro_DepthEstimator_destroy(void* self);
const char* bro_DepthEstimator_get_device(void* self);
void* bro_DepthEstimator_estimate(void* self, void* image, void* opts);

// --- Interface bro.vision.Sam ---
void* bro_Sam_create(void);
void  bro_Sam_destroy(void* self);
const char* bro_Sam_get_device(void* self);
bool bro_Sam_get_hasImage(void* self);
void* bro_Sam_setImage(void* self, void* image, void* opts);
void* bro_Sam_segment(void* self, void* opts);
void* bro_Sam_segmentEverything(void* self, void* image, void* opts);

// --- Interface bro.vision.NormalEstimator ---
void* bro_NormalEstimator_create(void);
void  bro_NormalEstimator_destroy(void* self);
const char* bro_NormalEstimator_get_device(void* self);
void* bro_NormalEstimator_estimate(void* self, void* image, void* opts);

// --- Interface bro.vision.Hed ---
void* bro_Hed_create(void);
void  bro_Hed_destroy(void* self);
const char* bro_Hed_get_device(void* self);
void* bro_Hed_estimate(void* self, void* image, void* opts);

// --- Interface bro.vision.Lineart ---
void* bro_Lineart_create(void);
void  bro_Lineart_destroy(void* self);
const char* bro_Lineart_get_device(void* self);
void* bro_Lineart_estimate(void* self, void* image, void* opts);

// --- Interface bro.vision.Mlsd ---
void* bro_Mlsd_create(void);
void  bro_Mlsd_destroy(void* self);
const char* bro_Mlsd_get_device(void* self);
void* bro_Mlsd_estimate(void* self, void* image, void* opts);

// --- Interface bro.vision.Openpose ---
void* bro_Openpose_create(void);
void  bro_Openpose_destroy(void* self);
const char* bro_Openpose_get_device(void* self);
void* bro_Openpose_estimate(void* self, void* image, void* opts);

// --- Interface bro.vision.Segformer ---
void* bro_Segformer_create(void);
void  bro_Segformer_destroy(void* self);
const char* bro_Segformer_get_device(void* self);
void* bro_Segformer_estimate(void* self, void* image, void* opts);

// --- Interface bro.vision.Birefnet ---
void* bro_Birefnet_create(void);
void  bro_Birefnet_destroy(void* self);
const char* bro_Birefnet_get_device(void* self);
void* bro_Birefnet_estimate(void* self, void* image, void* opts);

// --- Interface bro.vision.StyleGAN3 ---
void* bro_StyleGAN3_create(void);
void  bro_StyleGAN3_destroy(void* self);
const char* bro_StyleGAN3_get_device(void* self);
int32_t bro_StyleGAN3_get_zDim(void* self);
int32_t bro_StyleGAN3_get_cDim(void* self);
int32_t bro_StyleGAN3_get_wDim(void* self);
int32_t bro_StyleGAN3_get_imgResolution(void* self);
int32_t bro_StyleGAN3_get_imgChannels(void* self);
void* bro_StyleGAN3_generate(void* self, void* z, void* opts);

// --- Interface bro.vision.Dinov2 ---
void* bro_Dinov2_create(void);
void  bro_Dinov2_destroy(void* self);
const char* bro_Dinov2_get_device(void* self);
void* bro_Dinov2_estimate(void* self, void* image, void* opts);

// --- Interface bro.vision.Dinov3 ---
void* bro_Dinov3_create(void);
void  bro_Dinov3_destroy(void* self);
const char* bro_Dinov3_get_device(void* self);
void* bro_Dinov3_estimate(void* self, void* image, void* opts);

// --- Namespace bro.vision ---
void bro_vision_init(void);
void* bro_vision_loadDepth(const char* modelDir, void* opts);
void* bro_vision_loadSam(const char* modelDir, void* opts);
void* bro_vision_loadNormal(const char* modelDir, void* opts);
void* bro_vision_loadHed(const char* modelDir, void* opts);
void* bro_vision_loadLineart(const char* modelDir, void* opts);
void* bro_vision_loadMlsd(const char* modelDir, void* opts);
void* bro_vision_loadOpenpose(const char* modelDir, void* opts);
void* bro_vision_loadSegformer(const char* modelDir, void* opts);
void* bro_vision_loadBirefnet(const char* modelDir, void* opts);
void* bro_vision_loadStyleGAN3(const char* modelDir, void* opts);
void* bro_vision_loadDinov2(const char* modelDir, void* opts);
void* bro_vision_loadDinov3(const char* modelDir, void* opts);

#ifdef __cplusplus
}
#endif

#endif // BRO_VISION_C_ABI_H
