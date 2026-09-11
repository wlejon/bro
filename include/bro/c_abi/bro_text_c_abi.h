// =============================================================================
// bro_text_c_abi.h — Pure C-ABI declarations for bro.text
// Generated automatically by brosurface (gen/emit_c_abi.mjs).
// Zero dynamic boxing, direct native CPU register call.
// =============================================================================

#ifndef BRO_TEXT_C_ABI_H
#define BRO_TEXT_C_ABI_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// --- Namespace bro.text ---
bool bro_text_get_bidiAvailable(void);
void* bro_text_shape(const char* text, void* options);
void* bro_text_byteOffsetToX(const char* text, void* options, int32_t byteOffset);
int32_t bro_text_xToByteOffset(const char* text, void* options, double x);
void* bro_text_clusterRange(const char* text, void* options, int32_t byteOffset);
void* bro_text_cacheStats(void);
void* bro_text_bidi(const char* text, const char* base, bool override);
void* bro_text_bidiReorder(void* levels);

#ifdef __cplusplus
}
#endif

#endif // BRO_TEXT_C_ABI_H
