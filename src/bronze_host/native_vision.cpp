// native_vision.cpp — Bronze native entry points for bro.vision
#include "natives/vision/native_vision_decl.h"

#include <cstdint>
#include <string>

namespace {

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

}  // namespace

extern "C" {

// DepthEstimator
void bro_vision_DepthEstimator_dtor(void* self) {
    delete static_cast<BroDepthEstimatorImpl*>(self);
}
void* bro_vision_DepthEstimator_ctor(void) {
    return new BroDepthEstimatorImpl();
}
const char* bro_vision_DepthEstimator_device_get(void* self) {
    return self ? static_cast<BroDepthEstimatorImpl*>(self)->device.c_str() : "CPU";
}

// Sam
void bro_vision_Sam_dtor(void* self) {
    delete static_cast<BroSamImpl*>(self);
}
void* bro_vision_Sam_ctor(void) {
    return new BroSamImpl();
}
const char* bro_vision_Sam_device_get(void* self) {
    return self ? static_cast<BroSamImpl*>(self)->device.c_str() : "CPU";
}
bool bro_vision_Sam_hasImage_get(void* self) {
    return self && static_cast<BroSamImpl*>(self)->hasImage;
}

// NormalEstimator
void bro_vision_NormalEstimator_dtor(void* self) {
    delete static_cast<BroNormalEstimatorImpl*>(self);
}
void* bro_vision_NormalEstimator_ctor(void) {
    return new BroNormalEstimatorImpl();
}
const char* bro_vision_NormalEstimator_device_get(void* self) {
    return self ? static_cast<BroNormalEstimatorImpl*>(self)->device.c_str() : "CPU";
}

// Hed
void bro_vision_Hed_dtor(void* self) {
    delete static_cast<BroHedImpl*>(self);
}
void* bro_vision_Hed_ctor(void) {
    return new BroHedImpl();
}
const char* bro_vision_Hed_device_get(void* self) {
    return self ? static_cast<BroHedImpl*>(self)->device.c_str() : "CPU";
}

// Lineart
void bro_vision_Lineart_dtor(void* self) {
    delete static_cast<BroLineartImpl*>(self);
}
void* bro_vision_Lineart_ctor(void) {
    return new BroLineartImpl();
}
const char* bro_vision_Lineart_device_get(void* self) {
    return self ? static_cast<BroLineartImpl*>(self)->device.c_str() : "CPU";
}

// Mlsd
void bro_vision_Mlsd_dtor(void* self) {
    delete static_cast<BroMlsdImpl*>(self);
}
void* bro_vision_Mlsd_ctor(void) {
    return new BroMlsdImpl();
}
const char* bro_vision_Mlsd_device_get(void* self) {
    return self ? static_cast<BroMlsdImpl*>(self)->device.c_str() : "CPU";
}

// Openpose
void bro_vision_Openpose_dtor(void* self) {
    delete static_cast<BroOpenposeImpl*>(self);
}
void* bro_vision_Openpose_ctor(void) {
    return new BroOpenposeImpl();
}
const char* bro_vision_Openpose_device_get(void* self) {
    return self ? static_cast<BroOpenposeImpl*>(self)->device.c_str() : "CPU";
}

// Segformer
void bro_vision_Segformer_dtor(void* self) {
    delete static_cast<BroSegformerImpl*>(self);
}
void* bro_vision_Segformer_ctor(void) {
    return new BroSegformerImpl();
}
const char* bro_vision_Segformer_device_get(void* self) {
    return self ? static_cast<BroSegformerImpl*>(self)->device.c_str() : "CPU";
}

// Birefnet
void bro_vision_Birefnet_dtor(void* self) {
    delete static_cast<BroBirefnetImpl*>(self);
}
void* bro_vision_Birefnet_ctor(void) {
    return new BroBirefnetImpl();
}
const char* bro_vision_Birefnet_device_get(void* self) {
    return self ? static_cast<BroBirefnetImpl*>(self)->device.c_str() : "CPU";
}

// StyleGAN3
void bro_vision_StyleGAN3_dtor(void* self) {
    delete static_cast<BroStyleGAN3Impl*>(self);
}
void* bro_vision_StyleGAN3_ctor(void) {
    return new BroStyleGAN3Impl();
}
const char* bro_vision_StyleGAN3_device_get(void* self) {
    return self ? static_cast<BroStyleGAN3Impl*>(self)->device.c_str() : "CPU";
}
int32_t bro_vision_StyleGAN3_zDim_get(void* self) {
    return self ? static_cast<BroStyleGAN3Impl*>(self)->zDim : 512;
}
int32_t bro_vision_StyleGAN3_cDim_get(void* self) {
    return self ? static_cast<BroStyleGAN3Impl*>(self)->cDim : 0;
}
int32_t bro_vision_StyleGAN3_wDim_get(void* self) {
    return self ? static_cast<BroStyleGAN3Impl*>(self)->wDim : 512;
}
int32_t bro_vision_StyleGAN3_imgResolution_get(void* self) {
    return self ? static_cast<BroStyleGAN3Impl*>(self)->imgResolution : 1024;
}
int32_t bro_vision_StyleGAN3_imgChannels_get(void* self) {
    return self ? static_cast<BroStyleGAN3Impl*>(self)->imgChannels : 3;
}

// Dinov2
void bro_vision_Dinov2_dtor(void* self) {
    delete static_cast<BroDinov2Impl*>(self);
}
void* bro_vision_Dinov2_ctor(void) {
    return new BroDinov2Impl();
}
const char* bro_vision_Dinov2_device_get(void* self) {
    return self ? static_cast<BroDinov2Impl*>(self)->device.c_str() : "CPU";
}

// Dinov3
void bro_vision_Dinov3_dtor(void* self) {
    delete static_cast<BroDinov3Impl*>(self);
}
void* bro_vision_Dinov3_ctor(void) {
    return new BroDinov3Impl();
}
const char* bro_vision_Dinov3_device_get(void* self) {
    return self ? static_cast<BroDinov3Impl*>(self)->device.c_str() : "CPU";
}

// Namespace
void bro_vision_init(void) {
}

}  // extern "C"

namespace bro::bronze_host {
bool registerNatives_vision(std::string* error);
bool registerVisionNatives(std::string* error) {
    return registerNatives_vision(error);
}
}  // namespace bro::bronze_host
