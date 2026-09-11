// =============================================================================
// bro_settings_c_abi.h — Pure C-ABI declarations for bro.settings
// Generated automatically by brosurface (gen/emit_c_abi.mjs).
// Zero dynamic boxing, direct native CPU register call.
// =============================================================================

#ifndef BRO_SETTINGS_C_ABI_H
#define BRO_SETTINGS_C_ABI_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// --- Namespace bro.settings ---
void bro_settings_load(void);
void bro_settings_save(void);
const char* bro_settings_get(const char* key);
void bro_settings_set(const char* key, const char* val);
void bro_settings_reset(const char* category);
bool bro_settings_isActionPressed(const char* action);
double bro_settings_getActionStrength(const char* action);

#ifdef __cplusplus
}
#endif

#endif // BRO_SETTINGS_C_ABI_H
