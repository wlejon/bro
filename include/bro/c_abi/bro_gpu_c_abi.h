// =============================================================================
// bro_gpu_c_abi.h — Pure C-ABI declarations for bro.gpu
// Generated automatically by brosurface (gen/emit_c_abi.mjs).
// Zero dynamic boxing, zero JSContext, direct native CPU register call.
// =============================================================================

#ifndef BRO_GPU_C_ABI_H
#define BRO_GPU_C_ABI_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// --- Namespace bro.gpu ---
bool bro_gpu_get_available(void);
const char* bro_gpu_get_backend(void);
void* bro_gpu_get_devices(void);
int32_t bro_gpu_deviceCount(const char* device);
void* bro_gpu_get_compiledBackends(void);
void* bro_gpu_memoryInfo(const char* device);
const char* bro_gpu_deviceName(const char* device);
bool bro_gpu_trim(const char* device, uint64_t keepBytes);

#ifdef __cplusplus
}
#endif

#endif // BRO_GPU_C_ABI_H
