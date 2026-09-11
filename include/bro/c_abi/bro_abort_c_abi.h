// =============================================================================
// bro_abort_c_abi.h — Pure C-ABI declarations for bro.abort
// Generated automatically by brosurface (gen/emit_c_abi.mjs).
// Zero dynamic boxing, direct native CPU register call.
// =============================================================================

#ifndef BRO_ABORT_C_ABI_H
#define BRO_ABORT_C_ABI_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// --- Interface bro.abort.AbortSignal ---
void* bro_AbortSignal_create(void);
void  bro_AbortSignal_destroy(void* self);
bool bro_AbortSignal_get_aborted(void* self);
void* bro_AbortSignal_get_reason(void* self);
void* bro_AbortSignal_get_onabort(void* self);
void bro_AbortSignal_set_onabort(void* self, void* val);
void bro_AbortSignal_addEventListener(void* self, const char* type, void* listener);
void bro_AbortSignal_removeEventListener(void* self, const char* type, void* listener);
bool bro_AbortSignal_dispatchEvent(void* self, void* event);
void bro_AbortSignal_throwIfAborted(void* self);
void* bro_AbortSignal_abort(void* reason);
void* bro_AbortSignal_timeout(uint64_t milliseconds);
void* bro_AbortSignal_any(void* signals);

// --- Interface bro.abort.AbortController ---
void* bro_AbortController_create(void);
void  bro_AbortController_destroy(void* self);
void* bro_AbortController_get_signal(void* self);
void bro_AbortController_abort(void* self, void* reason);

#ifdef __cplusplus
}
#endif

#endif // BRO_ABORT_C_ABI_H
