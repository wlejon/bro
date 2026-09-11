// =============================================================================
// bro_listen_c_abi.h — Pure C-ABI declarations for bro.listen
// Generated automatically by brosurface (gen/emit_c_abi.mjs).
// Zero dynamic boxing, zero JSContext, direct native CPU register call.
// =============================================================================

#ifndef BRO_LISTEN_C_ABI_H
#define BRO_LISTEN_C_ABI_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// --- Interface bro.listen.ListenStream ---
void* bro_ListenStream_create(void);
void  bro_ListenStream_destroy(void* self);
uint32_t bro_ListenStream_get_id(void* self);
const char* bro_ListenStream_get_kind(void* self);
bool bro_ListenStream_get_valid(void* self);
void* bro_ListenStream_get_wake(void* self);
void* bro_ListenStream_get_kws(void* self);
void* bro_ListenStream_get_sense(void* self);
void* bro_ListenStream_get_gesture(void* self);
void bro_ListenStream_retain(void* self, int32_t seconds);
void* bro_ListenStream_audio(void* self, int64_t startFrame, int64_t endFrame);
int64_t bro_ListenStream_frame(void* self);
void* bro_ListenStream_info(void* self);
void bro_ListenStream_feed(void* self, void* samples);
void bro_ListenStream_close(void* self);

// --- Namespace bro.listen ---
void* bro_listen_open(void* source);
bool bro_listen_supported(void);
void* bro_listen_apps(void);
void bro_listen_retain(int32_t seconds);
void* bro_listen_audio(int64_t startFrame, int64_t endFrame);
int64_t bro_listen_frame(void);
void* bro_listen_info(void);

#ifdef __cplusplus
}
#endif

#endif // BRO_LISTEN_C_ABI_H
