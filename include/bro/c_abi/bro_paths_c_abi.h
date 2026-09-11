// =============================================================================
// bro_paths_c_abi.h — Pure C-ABI declarations for bro.paths
// Generated automatically by brosurface (gen/emit_c_abi.mjs).
// Zero dynamic boxing, direct native CPU register call.
// =============================================================================

#ifndef BRO_PATHS_C_ABI_H
#define BRO_PATHS_C_ABI_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// --- Namespace bro.paths ---
const char* bro_paths_get_appDir(void);
const char* bro_paths_get_userDataDir(void);
const char* bro_paths_resolvePath(const char* path);
const char* bro_paths_resolveWritePath(const char* path);

#ifdef __cplusplus
}
#endif

#endif // BRO_PATHS_C_ABI_H
