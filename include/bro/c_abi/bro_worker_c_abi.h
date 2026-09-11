// =============================================================================
// bro_worker_c_abi.h — Pure C-ABI declarations for bro.worker
// Generated automatically by brosurface (gen/emit_c_abi.mjs).
// Zero dynamic boxing, direct native CPU register call.
// =============================================================================

#ifndef BRO_WORKER_C_ABI_H
#define BRO_WORKER_C_ABI_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// --- Interface bro.worker.Worker ---
void* bro_Worker_create(const char* scriptURL, void* options);
void  bro_Worker_destroy(void* self);
void bro_Worker_postMessage(void* self, void* message, void* transfer);
void bro_Worker_terminate(void* self);
void* bro_Worker_get_onmessage(void* self);
void bro_Worker_set_onmessage(void* self, void* val);
void* bro_Worker_get_onerror(void* self);
void bro_Worker_set_onerror(void* self, void* val);
void* bro_Worker_get_onmessageerror(void* self);
void bro_Worker_set_onmessageerror(void* self, void* val);

#ifdef __cplusplus
}
#endif

#endif // BRO_WORKER_C_ABI_H
