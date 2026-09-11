// =============================================================================
// bro_media_c_abi.h — Pure C-ABI declarations for bro.media
// Generated automatically by brosurface (gen/emit_c_abi.mjs).
// Zero dynamic boxing, direct native CPU register call.
// =============================================================================

#ifndef BRO_MEDIA_C_ABI_H
#define BRO_MEDIA_C_ABI_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// --- Interface bro.media.VideoEncoder ---
void* bro_VideoEncoder_create(void* config);
void  bro_VideoEncoder_destroy(void* self);
int32_t bro_VideoEncoder_get_width(void* self);
int32_t bro_VideoEncoder_get_height(void* self);
int32_t bro_VideoEncoder_get_framesWritten(void* self);
const char* bro_VideoEncoder_get_lastError(void* self);
void bro_VideoEncoder_addFrameRGBA(void* self, void* pixels, int32_t stride);
void bro_VideoEncoder_addCanvasFrame(void* self, void* canvas);
void bro_VideoEncoder_addViewportFrame(void* self);
void bro_VideoEncoder_addAudioFramesPCM(void* self, void* pcm);
void bro_VideoEncoder_finish(void* self);

// --- Interface bro.media.GifEncoder ---
void* bro_GifEncoder_create(void* config);
void  bro_GifEncoder_destroy(void* self);
int32_t bro_GifEncoder_get_width(void* self);
int32_t bro_GifEncoder_get_height(void* self);
int32_t bro_GifEncoder_get_framesWritten(void* self);
const char* bro_GifEncoder_get_lastError(void* self);
void bro_GifEncoder_addFrameRGBA(void* self, void* pixels, int32_t stride);
void bro_GifEncoder_addCanvasFrame(void* self, void* canvas);
void bro_GifEncoder_addViewportFrame(void* self);
void bro_GifEncoder_setNextFrameDelayCs(void* self, int32_t delayCs);
void bro_GifEncoder_finish(void* self);

// --- Namespace bro.media ---
bool bro_media_get_available(void);
void* bro_media_peaks(const char* path, void* options);
void* bro_media_thumbnails(const char* path, void* options);

#ifdef __cplusplus
}
#endif

#endif // BRO_MEDIA_C_ABI_H
