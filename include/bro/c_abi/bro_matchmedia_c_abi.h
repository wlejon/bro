// =============================================================================
// bro_matchmedia_c_abi.h — Pure C-ABI declarations for bro.matchmedia
// Generated automatically by brosurface (gen/emit_c_abi.mjs).
// Zero dynamic boxing, direct native CPU register call.
// =============================================================================

#ifndef BRO_MATCHMEDIA_C_ABI_H
#define BRO_MATCHMEDIA_C_ABI_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// --- Interface bro.matchmedia.MediaQueryList ---
void* bro_MediaQueryList_create(void);
void  bro_MediaQueryList_destroy(void* self);
void* bro_matchMedia(void* source);
bool bro_MediaQueryList_get_matches(void* self);
const char* bro_MediaQueryList_get_media(void* self);
void* bro_MediaQueryList_get_onchange(void* self);
void bro_MediaQueryList_set_onchange(void* self, void* val);
void bro_MediaQueryList_addEventListener(void* self, const char* type, void* listener, void* options);
void bro_MediaQueryList_removeEventListener(void* self, const char* type, void* listener, void* options);
void bro_MediaQueryList_addListener(void* self, void* listener);
void bro_MediaQueryList_removeListener(void* self, void* listener);

#ifdef __cplusplus
}
#endif

#endif // BRO_MATCHMEDIA_C_ABI_H
