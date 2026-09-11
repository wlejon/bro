// =============================================================================
// bro_dialogs_c_abi.h — Pure C-ABI declarations for bro.dialogs
// Generated automatically by brosurface (gen/emit_c_abi.mjs).
// Zero dynamic boxing, direct native CPU register call.
// =============================================================================

#ifndef BRO_DIALOGS_C_ABI_H
#define BRO_DIALOGS_C_ABI_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// --- Namespace bro.dialogs ---
void bro_dialogs_alert(const char* message);
bool bro_dialogs_confirm(const char* message);
const char* bro_dialogs_prompt(const char* message, const char* defaultText);
const char* bro_dialogs_showSaveFileDialog(const char* filter, const char* defaultName);
const char* bro_dialogs_showOpenFileDialog(const char* filter, bool allowMultiple);
const char* bro_dialogs_showOpenFolderDialog(const char* defaultLocation, bool allowMultiple);

#ifdef __cplusplus
}
#endif

#endif // BRO_DIALOGS_C_ABI_H
