// =============================================================================
// bro_text_c_abi.cpp — C++ forwarding implementations for bro.text
// Generated automatically by brosurface (gen/emit_c_abi.mjs).
// =============================================================================

#include "bro/c_abi/bro_text_c_abi.h"
#include "bro/c_abi/bro_engine_c_abi.h"

extern "C" {

bool bro_text_get_bidiAvailable(void) {
    const auto* b = bro_get_text_bridge();
    if (b && b->getBidiAvailable) return b->getBidiAvailable();
    return false;
}

void* bro_text_shape(const char* text, void* options) {
    const auto* b = bro_get_text_bridge();
    if (b && b->shape) return b->shape(text, options);
    return nullptr;
}

void* bro_text_byteOffsetToX(const char* text, void* options, int32_t byteOffset) {
    const auto* b = bro_get_text_bridge();
    if (b && b->byteOffsetToX) return b->byteOffsetToX(text, options, byteOffset);
    return nullptr;
}

int32_t bro_text_xToByteOffset(const char* text, void* options, double x) {
    const auto* b = bro_get_text_bridge();
    if (b && b->xToByteOffset) return b->xToByteOffset(text, options, x);
    return 0;
}

void* bro_text_clusterRange(const char* text, void* options, int32_t byteOffset) {
    const auto* b = bro_get_text_bridge();
    if (b && b->clusterRange) return b->clusterRange(text, options, byteOffset);
    return nullptr;
}

void* bro_text_cacheStats(void) {
    const auto* b = bro_get_text_bridge();
    if (b && b->cacheStats) return b->cacheStats();
    return nullptr;
}

void* bro_text_bidi(const char* text, const char* base, bool override) {
    const auto* b = bro_get_text_bridge();
    if (b && b->bidi) return b->bidi(text, base, override);
    return nullptr;
}

void* bro_text_bidiReorder(void* levels) {
    const auto* b = bro_get_text_bridge();
    if (b && b->bidiReorder) return b->bidiReorder(levels);
    return nullptr;
}

} // extern "C"
