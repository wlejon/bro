// =============================================================================
// bro_gpu_c_abi.cpp — C++ forwarding implementations for bro.gpu
// Generated automatically by brosurface (gen/emit_c_abi.mjs).
// =============================================================================

#include "bro/c_abi/bro_gpu_c_abi.h"
#include "bro/c_abi/bro_engine_c_abi.h"
#include <string>

extern "C" {

bool bro_gpu_get_available(void) {
    const auto* b = bro_get_gpu_bridge();
    if (b && b->getAvailable) return b->getAvailable();
    return false;
}

const char* bro_gpu_get_backend(void) {
    const auto* b = bro_get_gpu_bridge();
    if (b && b->getBackend) return b->getBackend();
    return "cpu";
}

void* bro_gpu_get_devices(void) {
    const auto* b = bro_get_gpu_bridge();
    if (b && b->getDevices) return b->getDevices();
    return nullptr;
}

int32_t bro_gpu_deviceCount(const char* device) {
    const auto* b = bro_get_gpu_bridge();
    if (b && b->deviceCount) return b->deviceCount(device);
    return (!device || std::string(device) == "cpu") ? 1 : 0;
}

void* bro_gpu_get_compiledBackends(void) {
    const auto* b = bro_get_gpu_bridge();
    if (b && b->getCompiledBackends) return b->getCompiledBackends();
    return nullptr;
}

void* bro_gpu_memoryInfo(const char* device) {
    const auto* b = bro_get_gpu_bridge();
    if (b && b->memoryInfo) return b->memoryInfo(device);
    return nullptr;
}

const char* bro_gpu_deviceName(const char* device) {
    const auto* b = bro_get_gpu_bridge();
    if (b && b->deviceName) return b->deviceName(device);
    return "cpu";
}

bool bro_gpu_trim(const char* device, uint64_t keepBytes) {
    const auto* b = bro_get_gpu_bridge();
    if (b && b->trim) return b->trim(device, keepBytes);
    return false;
}

} // extern "C"
