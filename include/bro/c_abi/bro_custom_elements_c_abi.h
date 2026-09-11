// =============================================================================
// bro_custom_elements_c_abi.h — Pure C-ABI declarations for bro.custom_elements
// Generated automatically by brosurface (gen/emit_c_abi.mjs).
// Zero dynamic boxing, direct native CPU register call.
// =============================================================================

#ifndef BRO_CUSTOM_ELEMENTS_C_ABI_H
#define BRO_CUSTOM_ELEMENTS_C_ABI_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// --- Interface bro.custom_elements.CustomElementRegistry ---
void* bro_CustomElementRegistry_create(void);
void  bro_CustomElementRegistry_destroy(void* self);
void* bro_customElements(void* source);
void bro_CustomElementRegistry_define(void* self, const char* name, void* constructor, void* options);
void* bro_CustomElementRegistry_get(void* self, const char* name);
void* bro_CustomElementRegistry_whenDefined(void* self, const char* name);
void bro_CustomElementRegistry_upgrade(void* self, void* root);

// --- Interface bro.custom_elements.HTMLElement ---
void* bro_HTMLElement_create(void);
void  bro_HTMLElement_destroy(void* self);

#ifdef __cplusplus
}
#endif

#endif // BRO_CUSTOM_ELEMENTS_C_ABI_H
