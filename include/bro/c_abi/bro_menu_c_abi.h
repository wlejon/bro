// =============================================================================
// bro_menu_c_abi.h — Pure C-ABI declarations for bro.menu
// Generated automatically by brosurface (gen/emit_c_abi.mjs).
// Zero dynamic boxing, direct native CPU register call.
// =============================================================================

#ifndef BRO_MENU_C_ABI_H
#define BRO_MENU_C_ABI_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// --- Namespace bro.menu ---
bool bro_menu_get_visible(void);
void bro_menu_show(void);
void bro_menu_hide(void);
void bro_menu_set(void* items);
bool bro_menu_addItem(const char* parentId, void* item, int32_t index);
bool bro_menu_updateItem(const char* id, void* props);
bool bro_menu_removeItem(const char* id);
void bro_menu_on(const char* id, void* callback);

#ifdef __cplusplus
}
#endif

#endif // BRO_MENU_C_ABI_H
