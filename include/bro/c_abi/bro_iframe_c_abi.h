// =============================================================================
// bro_iframe_c_abi.h — Pure C-ABI declarations for bro.iframe
// Generated automatically by brosurface (gen/emit_c_abi.mjs).
// Zero dynamic boxing, direct native CPU register call.
// =============================================================================

#ifndef BRO_IFRAME_C_ABI_H
#define BRO_IFRAME_C_ABI_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// --- Interface bro.iframe.HTMLIFrameElement ---
void* bro_HTMLIFrameElement_create(void);
void  bro_HTMLIFrameElement_destroy(void* self);
const char* bro_HTMLIFrameElement_get_src(void* self);
void bro_HTMLIFrameElement_set_src(void* self, const char* val);
const char* bro_HTMLIFrameElement_get_width(void* self);
void bro_HTMLIFrameElement_set_width(void* self, const char* val);
const char* bro_HTMLIFrameElement_get_height(void* self);
void bro_HTMLIFrameElement_set_height(void* self, const char* val);
void* bro_HTMLIFrameElement_get_contentDocument(void* self);
void* bro_HTMLIFrameElement_get_contentWindow(void* self);
void bro_HTMLIFrameElement_reload(void* self);
void* bro_HTMLIFrameElement_capture(void* self);

#ifdef __cplusplus
}
#endif

#endif // BRO_IFRAME_C_ABI_H
