// =============================================================================
// bro_media_c_abi.cpp — C++ forwarding implementations for bro.media
// Generated automatically by brosurface (gen/emit_c_abi.mjs).
// =============================================================================

#include "bro/c_abi/bro_media_c_abi.h"
#include "bro/c_abi/bro_engine_c_abi.h"

extern "C" {

bool bro_media_get_available(void) {
    const auto* b = bro_get_media_bridge();
    if (b && b->getAvailable) return b->getAvailable();
    return true;
}

void* bro_media_peaks(const char* path, void* options) {
    const auto* b = bro_get_media_bridge();
    if (b && b->peaks) return b->peaks(path, options);
    return nullptr;
}

void* bro_media_thumbnails(const char* path, void* options) {
    const auto* b = bro_get_media_bridge();
    if (b && b->thumbnails) return b->thumbnails(path, options);
    return nullptr;
}

// VideoEncoder
void* bro_VideoEncoder_create(void* /*config*/) { return nullptr; }
void  bro_VideoEncoder_destroy(void* /*self*/) {}
int32_t bro_VideoEncoder_get_width(void* /*self*/) { return 0; }
int32_t bro_VideoEncoder_get_height(void* /*self*/) { return 0; }
int32_t bro_VideoEncoder_get_framesWritten(void* /*self*/) { return 0; }
const char* bro_VideoEncoder_get_lastError(void* /*self*/) { return ""; }
void bro_VideoEncoder_addFrameRGBA(void* /*self*/, void* /*pixels*/, int32_t /*stride*/) {}
void bro_VideoEncoder_addCanvasFrame(void* /*self*/, void* /*canvas*/) {}
void bro_VideoEncoder_addViewportFrame(void* /*self*/) {}
void bro_VideoEncoder_addAudioFramesPCM(void* /*self*/, void* /*pcm*/) {}
void bro_VideoEncoder_finish(void* /*self*/) {}

// GifEncoder
void* bro_GifEncoder_create(void* /*config*/) { return nullptr; }
void  bro_GifEncoder_destroy(void* /*self*/) {}
int32_t bro_GifEncoder_get_width(void* /*self*/) { return 0; }
int32_t bro_GifEncoder_get_height(void* /*self*/) { return 0; }
int32_t bro_GifEncoder_get_framesWritten(void* /*self*/) { return 0; }
const char* bro_GifEncoder_get_lastError(void* /*self*/) { return ""; }
void bro_GifEncoder_addFrameRGBA(void* /*self*/, void* /*pixels*/, int32_t /*stride*/) {}
void bro_GifEncoder_addCanvasFrame(void* /*self*/, void* /*canvas*/) {}
void bro_GifEncoder_addViewportFrame(void* /*self*/) {}
void bro_GifEncoder_setNextFrameDelayCs(void* /*self*/, int32_t /*delayCs*/) {}
void bro_GifEncoder_finish(void* /*self*/) {}

} // extern "C"
