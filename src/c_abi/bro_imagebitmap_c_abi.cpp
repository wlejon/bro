// =============================================================================
// bro_imagebitmap_c_abi.cpp — C++ forwarding implementations for bro.imagebitmap
// Generated automatically by brosurface (gen/emit_c_abi.mjs).
// =============================================================================

#include "bro/c_abi/bro_imagebitmap_c_abi.h"
#include <cstdint>
#include <vector>

struct BroImageBitmapImpl {
    int32_t width = 0;
    int32_t height = 0;
    bool closed = false;
};

struct BroImageDataImpl {
    int32_t width = 0;
    int32_t height = 0;
    std::vector<uint8_t> data;
};

extern "C" {

// ImageBitmap
void* bro_ImageBitmap_create(void) {
    return new BroImageBitmapImpl();
}

void bro_ImageBitmap_destroy(void* self) {
    delete static_cast<BroImageBitmapImpl*>(self);
}

int32_t bro_ImageBitmap_get_width(void* self) {
    auto* b = static_cast<BroImageBitmapImpl*>(self);
    return (b && !b->closed) ? b->width : 0;
}

int32_t bro_ImageBitmap_get_height(void* self) {
    auto* b = static_cast<BroImageBitmapImpl*>(self);
    return (b && !b->closed) ? b->height : 0;
}

void bro_ImageBitmap_close(void* self) {
    auto* b = static_cast<BroImageBitmapImpl*>(self);
    if (b) {
        b->closed = true;
    }
}

// ImageData
void* bro_ImageData_create(int32_t width, int32_t height) {
    auto* d = new BroImageDataImpl();
    d->width = width > 0 ? width : 0;
    d->height = height > 0 ? height : 0;
    d->data.resize(static_cast<size_t>(d->width) * d->height * 4, 0);
    return d;
}

void bro_ImageData_destroy(void* self) {
    delete static_cast<BroImageDataImpl*>(self);
}

int32_t bro_ImageData_get_width(void* self) {
    auto* d = static_cast<BroImageDataImpl*>(self);
    return d ? d->width : 0;
}

int32_t bro_ImageData_get_height(void* self) {
    auto* d = static_cast<BroImageDataImpl*>(self);
    return d ? d->height : 0;
}

void* bro_ImageData_get_data(void* self) {
    auto* d = static_cast<BroImageDataImpl*>(self);
    return d ? d->data.data() : nullptr;
}

// Global createImageBitmap
void* bro_createImageBitmap(void* source) {
    auto* bmp = new BroImageBitmapImpl();
    if (source) {
        auto* d = static_cast<BroImageDataImpl*>(source);
        bmp->width = d->width;
        bmp->height = d->height;
    }
    return bmp;
}

} // extern "C"
