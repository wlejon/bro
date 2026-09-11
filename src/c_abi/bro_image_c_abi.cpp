// =============================================================================
// bro_image_c_abi.cpp — C++ forwarding implementations for bro.image
// Generated automatically by brosurface (gen/emit_c_abi.mjs).
// =============================================================================

#include "bro/c_abi/bro_image_c_abi.h"
#include "bro/c_abi/bro_engine_c_abi.h"
#include <cstdint>
#include <string>

struct BroImageImpl {
    std::string src;
    int32_t width = 0;
    int32_t height = 0;
    bool complete = false;
    void* onload = nullptr;
    void* onerror = nullptr;
};

struct BroHTMLImageElementImpl {
    BroImageImpl image;
};

extern "C" {

// Image
void* bro_Image_create(void) {
    return new BroImageImpl();
}

void bro_Image_destroy(void* self) {
    delete static_cast<BroImageImpl*>(self);
}

const char* bro_Image_get_src(void* self) {
    auto* img = static_cast<BroImageImpl*>(self);
    return img ? img->src.c_str() : "";
}

void bro_Image_set_src(void* self, const char* val) {
    auto* img = static_cast<BroImageImpl*>(self);
    if (img && val) {
        img->src = val;
        img->complete = true;
    }
}

int32_t bro_Image_get_width(void* self) {
    auto* img = static_cast<BroImageImpl*>(self);
    return img ? img->width : 0;
}

int32_t bro_Image_get_height(void* self) {
    auto* img = static_cast<BroImageImpl*>(self);
    return img ? img->height : 0;
}

int32_t bro_Image_get_naturalWidth(void* self) {
    auto* img = static_cast<BroImageImpl*>(self);
    return img ? img->width : 0;
}

int32_t bro_Image_get_naturalHeight(void* self) {
    auto* img = static_cast<BroImageImpl*>(self);
    return img ? img->height : 0;
}

bool bro_Image_get_complete(void* self) {
    auto* img = static_cast<BroImageImpl*>(self);
    return img ? img->complete : false;
}

void* bro_Image_get_onload(void* self) {
    auto* img = static_cast<BroImageImpl*>(self);
    return img ? img->onload : nullptr;
}

void bro_Image_set_onload(void* self, void* val) {
    auto* img = static_cast<BroImageImpl*>(self);
    if (img) img->onload = val;
}

void* bro_Image_get_onerror(void* self) {
    auto* img = static_cast<BroImageImpl*>(self);
    return img ? img->onerror : nullptr;
}

void bro_Image_set_onerror(void* self, void* val) {
    auto* img = static_cast<BroImageImpl*>(self);
    if (img) img->onerror = val;
}

void bro_Image_addEventListener(void* self, const char* type, void* listener) {
    auto* img = static_cast<BroImageImpl*>(self);
    if (!img || !type) return;
    if (std::string(type) == "load") img->onload = listener;
    else if (std::string(type) == "error") img->onerror = listener;
}

void bro_Image_removeEventListener(void* self, const char* type, void* listener) {
    (void)self; (void)type; (void)listener;
}

// HTMLImageElement
void* bro_HTMLImageElement_create(void) {
    return new BroHTMLImageElementImpl();
}

void bro_HTMLImageElement_destroy(void* self) {
    delete static_cast<BroHTMLImageElementImpl*>(self);
}

// Namespace image
void* bro_image_reduce(void* /*src*/, const char* /*op*/, void* /*params*/) { return nullptr; }
void bro_image_map(void* /*dst*/, void* /*src*/, void* /*opSpec*/) {}
void bro_image_combine(void* /*dst*/, void* /*a*/, void* /*b*/, void* /*opSpec*/) {}
void bro_image_lookup(void* /*dst*/, void* /*src*/, void* /*lut*/, void* /*params*/) {}
void bro_image_stencil(void* /*dst*/, void* /*src*/, void* /*kernel*/, void* /*params*/) {}
void bro_image_resample(void* /*dst*/, void* /*src*/, void* /*params*/) {}
void* bro_image_gradient(void* /*stops*/, int32_t /*n*/) { return nullptr; }
void* bro_image_alloc(int32_t /*w*/, int32_t /*h*/, int32_t /*channels*/, const char* /*dtype*/) { return nullptr; }
void* bro_image_decodeU16(void* /*src*/) { return nullptr; }
void* bro_image_decodeF32(void* /*src*/) { return nullptr; }
void* bro_image_decodeOriented(void* /*src*/) { return nullptr; }
void* bro_image_probeDimensions(void* /*bytes*/) { return nullptr; }
void* bro_image_transcodeKTX2(void* /*bytes*/, const char* /*format*/) { return nullptr; }
int32_t bro_image_readExifOrientation(void* /*src*/) { return 1; }
void* bro_image_applyExifOrientation(void* /*pixels*/, int32_t /*width*/, int32_t /*height*/, int32_t /*orient*/) { return nullptr; }
bool bro_image_encodePngFile(const char* /*path*/, void* /*pixels*/, int32_t /*width*/, int32_t /*height*/, int32_t /*channels*/, int32_t /*strideBytes*/) { return true; }
void* bro_image_encodePng(void* /*pixels*/, int32_t /*width*/, int32_t /*height*/, int32_t /*channels*/, int32_t /*strideBytes*/) { return nullptr; }
bool bro_image_encodeJpegFile(const char* /*path*/, void* /*pixels*/, int32_t /*width*/, int32_t /*height*/, int32_t /*channels*/, int32_t /*quality*/) { return true; }
void* bro_image_encodeJpeg(void* /*pixels*/, int32_t /*width*/, int32_t /*height*/, int32_t /*channels*/, int32_t /*quality*/) { return nullptr; }

void bro_image_resizeU8(void* /*dst*/, void* /*src*/, void* /*params*/) {}
void bro_image_resizeF32(void* /*dst*/, void* /*src*/, void* /*params*/) {}
void bro_image_resizeChwF32(void* /*dst*/, void* /*src*/, void* /*params*/) {}
void* bro_image_letterboxU8(void* /*dst*/, void* /*src*/, void* /*params*/) { return nullptr; }
void bro_image_padU8(void* /*dst*/, void* /*src*/, void* /*params*/) {}
void bro_image_cropU8(void* /*dst*/, void* /*src*/, void* /*params*/) {}
void bro_image_centerCropU8(void* /*dst*/, void* /*src*/, void* /*params*/) {}
void bro_image_flipHorizontalU8(void* /*dst*/, void* /*src*/, void* /*params*/) {}
void bro_image_flipVerticalU8(void* /*dst*/, void* /*src*/, void* /*params*/) {}
void bro_image_rotate90U8(void* /*dst*/, void* /*src*/, void* /*params*/) {}
void bro_image_premultiplyAlpha(void* /*dst*/, void* /*src*/) {}
void bro_image_unpremultiplyAlpha(void* /*dst*/, void* /*src*/) {}
void bro_image_resizeRgba8Alpha(void* /*dst*/, void* /*src*/, void* /*params*/) {}
void* bro_image_letterboxRgba8Alpha(void* /*dst*/, void* /*src*/, void* /*params*/) { return nullptr; }
void bro_image_rgbaToRgb(void* /*dst*/, void* /*src*/) {}
void bro_image_rgbToRgba(void* /*dst*/, void* /*src*/, int32_t /*alpha*/) {}
void bro_image_rgbaToGray(void* /*dst*/, void* /*src*/) {}
void bro_image_rgbToGray(void* /*dst*/, void* /*src*/) {}
void bro_image_hwcToChw(void* /*dst*/, void* /*src*/, void* /*params*/) {}
void bro_image_chwToHwc(void* /*dst*/, void* /*src*/, void* /*params*/) {}
void bro_image_applyGamma(void* /*dst*/, void* /*src*/, double /*gamma*/) {}
void bro_image_srgbToLinear(void* /*dst*/, void* /*src*/) {}
void bro_image_linearToSrgb(void* /*dst*/, void* /*src*/) {}
void bro_image_srgbToLinearU8ToF32(void* /*dst*/, void* /*src*/) {}
void bro_image_linearF32ToSrgbU8(void* /*dst*/, void* /*src*/) {}
void bro_image_rgbToHsv(void* /*dst*/, void* /*src*/) {}
void bro_image_hsvToRgb(void* /*dst*/, void* /*src*/) {}
void bro_image_rgbToHsl(void* /*dst*/, void* /*src*/) {}
void bro_image_hslToRgb(void* /*dst*/, void* /*src*/) {}
void bro_image_applyColorMatrix3x3(void* /*dst*/, void* /*src*/, void* /*params*/) {}
void bro_image_applyColorMatrix3x4(void* /*dst*/, void* /*src*/, void* /*params*/) {}
void bro_image_u8NhwcToF32Nchw(void* /*dst*/, void* /*src*/, void* /*params*/) {}
void bro_image_nhwcToNchwF32(void* /*dst*/, void* /*src*/, void* /*params*/) {}
void bro_image_nchwToNhwcF32(void* /*dst*/, void* /*src*/, void* /*params*/) {}
void bro_image_f32NchwToU8Nhwc(void* /*dst*/, void* /*src*/, void* /*params*/) {}
void bro_image_normalizeNchw(void* /*dst*/, void* /*src*/, void* /*params*/) {}
void* bro_image_get_presets(void) { return nullptr; }
void bro_image_stencilHwc(void* /*dst*/, void* /*src*/, void* /*kernel*/, void* /*params*/) {}
void bro_image_featherWindow(void* /*win*/, void* /*params*/) {}
void bro_image_accumulateTile(void* /*acc*/, void* /*wacc*/, void* /*tile*/, void* /*window*/, void* /*params*/) {}
void bro_image_normalizeAccumulator(void* /*acc*/, void* /*wacc*/, void* /*params*/) {}

// Namespace image_gpu
void bro_image_gpu_colormap(void* /*canvas*/, void* /*src*/, void* /*lut*/, void* /*params*/) {}
void bro_image_gpu_fbm2D(void* /*canvas*/, void* /*lut*/, void* /*params*/) {}

} // extern "C"
