// =============================================================================
// bro_vision_c_abi.cpp — C++ forwarding implementations for bro.vision
// Generated automatically by brosurface (gen/emit_c_abi.mjs).
// =============================================================================

#include "bro/c_abi/bro_vision_c_abi.h"
#include "bro/c_abi/bro_engine_c_abi.h"
#include <cstdint>
#include <string>

struct BroDepthEstimatorImpl {
    std::string device = "CPU";
};

struct BroSamImpl {
    std::string device = "CPU";
    bool hasImage = false;
};

struct BroNormalEstimatorImpl {
    std::string device = "CPU";
};

struct BroHedImpl {
    std::string device = "CPU";
};

struct BroLineartImpl {
    std::string device = "CPU";
};

struct BroMlsdImpl {
    std::string device = "CPU";
};

struct BroOpenposeImpl {
    std::string device = "CPU";
};

struct BroSegformerImpl {
    std::string device = "CPU";
};

struct BroBirefnetImpl {
    std::string device = "CPU";
};

struct BroStyleGAN3Impl {
    std::string device = "CPU";
    int32_t zDim = 512;
    int32_t cDim = 0;
    int32_t wDim = 512;
    int32_t imgResolution = 1024;
    int32_t imgChannels = 3;
};

struct BroDinov2Impl {
    std::string device = "CPU";
};

struct BroDinov3Impl {
    std::string device = "CPU";
};

extern "C" {

// --- Interface bro.vision.DepthEstimator ---
void* bro_DepthEstimator_create(void) {
    return new BroDepthEstimatorImpl();
}

void bro_DepthEstimator_destroy(void* self) {
    delete static_cast<BroDepthEstimatorImpl*>(self);
}

const char* bro_DepthEstimator_get_device(void* self) {
    auto* e = static_cast<BroDepthEstimatorImpl*>(self);
    return e ? e->device.c_str() : "CPU";
}

void* bro_DepthEstimator_estimate(void* /*self*/, void* /*image*/, void* /*opts*/) {
    return nullptr;
}

// --- Interface bro.vision.Sam ---
void* bro_Sam_create(void) {
    return new BroSamImpl();
}

void bro_Sam_destroy(void* self) {
    delete static_cast<BroSamImpl*>(self);
}

const char* bro_Sam_get_device(void* self) {
    auto* s = static_cast<BroSamImpl*>(self);
    return s ? s->device.c_str() : "CPU";
}

bool bro_Sam_get_hasImage(void* self) {
    auto* s = static_cast<BroSamImpl*>(self);
    return s ? s->hasImage : false;
}

void* bro_Sam_setImage(void* self, void* /*image*/, void* /*opts*/) {
    auto* s = static_cast<BroSamImpl*>(self);
    if (s) s->hasImage = true;
    return nullptr;
}

void* bro_Sam_segment(void* /*self*/, void* /*opts*/) {
    return nullptr;
}

void* bro_Sam_segmentEverything(void* /*self*/, void* /*image*/, void* /*opts*/) {
    return nullptr;
}

// --- Interface bro.vision.NormalEstimator ---
void* bro_NormalEstimator_create(void) {
    return new BroNormalEstimatorImpl();
}

void bro_NormalEstimator_destroy(void* self) {
    delete static_cast<BroNormalEstimatorImpl*>(self);
}

const char* bro_NormalEstimator_get_device(void* self) {
    auto* e = static_cast<BroNormalEstimatorImpl*>(self);
    return e ? e->device.c_str() : "CPU";
}

void* bro_NormalEstimator_estimate(void* /*self*/, void* /*image*/, void* /*opts*/) {
    return nullptr;
}

// --- Interface bro.vision.Hed ---
void* bro_Hed_create(void) {
    return new BroHedImpl();
}

void bro_Hed_destroy(void* self) {
    delete static_cast<BroHedImpl*>(self);
}

const char* bro_Hed_get_device(void* self) {
    auto* h = static_cast<BroHedImpl*>(self);
    return h ? h->device.c_str() : "CPU";
}

void* bro_Hed_estimate(void* /*self*/, void* /*image*/, void* /*opts*/) {
    return nullptr;
}

// --- Interface bro.vision.Lineart ---
void* bro_Lineart_create(void) {
    return new BroLineartImpl();
}

void bro_Lineart_destroy(void* self) {
    delete static_cast<BroLineartImpl*>(self);
}

const char* bro_Lineart_get_device(void* self) {
    auto* l = static_cast<BroLineartImpl*>(self);
    return l ? l->device.c_str() : "CPU";
}

void* bro_Lineart_estimate(void* /*self*/, void* /*image*/, void* /*opts*/) {
    return nullptr;
}

// --- Interface bro.vision.Mlsd ---
void* bro_Mlsd_create(void) {
    return new BroMlsdImpl();
}

void bro_Mlsd_destroy(void* self) {
    delete static_cast<BroMlsdImpl*>(self);
}

const char* bro_Mlsd_get_device(void* self) {
    auto* m = static_cast<BroMlsdImpl*>(self);
    return m ? m->device.c_str() : "CPU";
}

void* bro_Mlsd_estimate(void* /*self*/, void* /*image*/, void* /*opts*/) {
    return nullptr;
}

// --- Interface bro.vision.Openpose ---
void* bro_Openpose_create(void) {
    return new BroOpenposeImpl();
}

void bro_Openpose_destroy(void* self) {
    delete static_cast<BroOpenposeImpl*>(self);
}

const char* bro_Openpose_get_device(void* self) {
    auto* o = static_cast<BroOpenposeImpl*>(self);
    return o ? o->device.c_str() : "CPU";
}

void* bro_Openpose_estimate(void* /*self*/, void* /*image*/, void* /*opts*/) {
    return nullptr;
}

// --- Interface bro.vision.Segformer ---
void* bro_Segformer_create(void) {
    return new BroSegformerImpl();
}

void bro_Segformer_destroy(void* self) {
    delete static_cast<BroSegformerImpl*>(self);
}

const char* bro_Segformer_get_device(void* self) {
    auto* s = static_cast<BroSegformerImpl*>(self);
    return s ? s->device.c_str() : "CPU";
}

void* bro_Segformer_estimate(void* /*self*/, void* /*image*/, void* /*opts*/) {
    return nullptr;
}

// --- Interface bro.vision.Birefnet ---
void* bro_Birefnet_create(void) {
    return new BroBirefnetImpl();
}

void bro_Birefnet_destroy(void* self) {
    delete static_cast<BroBirefnetImpl*>(self);
}

const char* bro_Birefnet_get_device(void* self) {
    auto* b = static_cast<BroBirefnetImpl*>(self);
    return b ? b->device.c_str() : "CPU";
}

void* bro_Birefnet_estimate(void* /*self*/, void* /*image*/, void* /*opts*/) {
    return nullptr;
}

// --- Interface bro.vision.StyleGAN3 ---
void* bro_StyleGAN3_create(void) {
    return new BroStyleGAN3Impl();
}

void bro_StyleGAN3_destroy(void* self) {
    delete static_cast<BroStyleGAN3Impl*>(self);
}

const char* bro_StyleGAN3_get_device(void* self) {
    auto* s = static_cast<BroStyleGAN3Impl*>(self);
    return s ? s->device.c_str() : "CPU";
}

int32_t bro_StyleGAN3_get_zDim(void* self) {
    auto* s = static_cast<BroStyleGAN3Impl*>(self);
    return s ? s->zDim : 512;
}

int32_t bro_StyleGAN3_get_cDim(void* self) {
    auto* s = static_cast<BroStyleGAN3Impl*>(self);
    return s ? s->cDim : 0;
}

int32_t bro_StyleGAN3_get_wDim(void* self) {
    auto* s = static_cast<BroStyleGAN3Impl*>(self);
    return s ? s->wDim : 512;
}

int32_t bro_StyleGAN3_get_imgResolution(void* self) {
    auto* s = static_cast<BroStyleGAN3Impl*>(self);
    return s ? s->imgResolution : 1024;
}

int32_t bro_StyleGAN3_get_imgChannels(void* self) {
    auto* s = static_cast<BroStyleGAN3Impl*>(self);
    return s ? s->imgChannels : 3;
}

void* bro_StyleGAN3_generate(void* /*self*/, void* /*z*/, void* /*opts*/) {
    return nullptr;
}

// --- Interface bro.vision.Dinov2 ---
void* bro_Dinov2_create(void) {
    return new BroDinov2Impl();
}

void bro_Dinov2_destroy(void* self) {
    delete static_cast<BroDinov2Impl*>(self);
}

const char* bro_Dinov2_get_device(void* self) {
    auto* d = static_cast<BroDinov2Impl*>(self);
    return d ? d->device.c_str() : "CPU";
}

void* bro_Dinov2_estimate(void* /*self*/, void* /*image*/, void* /*opts*/) {
    return nullptr;
}

// --- Interface bro.vision.Dinov3 ---
void* bro_Dinov3_create(void) {
    return new BroDinov3Impl();
}

void bro_Dinov3_destroy(void* self) {
    delete static_cast<BroDinov3Impl*>(self);
}

const char* bro_Dinov3_get_device(void* self) {
    auto* d = static_cast<BroDinov3Impl*>(self);
    return d ? d->device.c_str() : "CPU";
}

void* bro_Dinov3_estimate(void* /*self*/, void* /*image*/, void* /*opts*/) {
    return nullptr;
}

// --- Namespace bro.vision ---
void bro_vision_init(void) {
    const auto* b = bro_get_vision_bridge();
    if (b && b->init) b->init();
}

void* bro_vision_loadDepth(const char* modelDir, void* opts) {
    const auto* b = bro_get_vision_bridge();
    if (b && b->loadDepth) return b->loadDepth(modelDir, opts);
    return new BroDepthEstimatorImpl();
}

void* bro_vision_loadSam(const char* /*modelDir*/, void* /*opts*/) {
    return new BroSamImpl();
}

void* bro_vision_loadNormal(const char* /*modelDir*/, void* /*opts*/) {
    return new BroNormalEstimatorImpl();
}

void* bro_vision_loadHed(const char* /*modelDir*/, void* /*opts*/) {
    return new BroHedImpl();
}

void* bro_vision_loadLineart(const char* /*modelDir*/, void* /*opts*/) {
    return new BroLineartImpl();
}

void* bro_vision_loadMlsd(const char* /*modelDir*/, void* /*opts*/) {
    return new BroMlsdImpl();
}

void* bro_vision_loadOpenpose(const char* /*modelDir*/, void* /*opts*/) {
    return new BroOpenposeImpl();
}

void* bro_vision_loadSegformer(const char* /*modelDir*/, void* /*opts*/) {
    return new BroSegformerImpl();
}

void* bro_vision_loadBirefnet(const char* /*modelDir*/, void* /*opts*/) {
    return new BroBirefnetImpl();
}

void* bro_vision_loadStyleGAN3(const char* /*modelDir*/, void* /*opts*/) {
    return new BroStyleGAN3Impl();
}

void* bro_vision_loadDinov2(const char* /*modelDir*/, void* /*opts*/) {
    return new BroDinov2Impl();
}

void* bro_vision_loadDinov3(const char* /*modelDir*/, void* /*opts*/) {
    return new BroDinov3Impl();
}

}
