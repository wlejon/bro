// =============================================================================
// bro_imagebitmap_c_abi.h — Pure C-ABI declarations for bro.imagebitmap
// Generated automatically by brosurface (gen/emit_c_abi.mjs).
// Zero dynamic boxing, direct native CPU register call.
// =============================================================================

#ifndef BRO_IMAGEBITMAP_C_ABI_H
#define BRO_IMAGEBITMAP_C_ABI_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// --- Interface bro.imagebitmap.ImageBitmap ---
void* bro_ImageBitmap_create(void);
void  bro_ImageBitmap_destroy(void* self);
void* bro_createImageBitmap(void* source);
int32_t bro_ImageBitmap_get_width(void* self);
int32_t bro_ImageBitmap_get_height(void* self);
void bro_ImageBitmap_close(void* self);

// --- Interface bro.imagebitmap.ImageData ---
void* bro_ImageData_create(int32_t width, int32_t height);
void  bro_ImageData_destroy(void* self);
int32_t bro_ImageData_get_width(void* self);
int32_t bro_ImageData_get_height(void* self);
void* bro_ImageData_get_data(void* self);

#ifdef __cplusplus
}
#endif

#endif // BRO_IMAGEBITMAP_C_ABI_H
