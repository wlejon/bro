// =============================================================================
// bro_vendor_globals_c_abi.cpp — C++ forwarding implementations for bro.vendor_globals
// Generated automatically by brosurface (gen/emit_c_abi.mjs).
// =============================================================================

#include "bro/c_abi/bro_vendor_globals_c_abi.h"
#include "bro/c_abi/bro_engine_c_abi.h"

extern "C" {

void* bro_vendor_globals_get_signals(void) {
    const auto* b = bro_get_vendor_globals_bridge();
    if (b && b->getSignals) return b->getSignals();
    return nullptr;
}

void* bro_vendor_globals_get_CodeMirror(void) {
    const auto* b = bro_get_vendor_globals_bridge();
    if (b && b->getCodeMirror) return b->getCodeMirror();
    return nullptr;
}

void* bro_vendor_globals_get_acorn(void) {
    const auto* b = bro_get_vendor_globals_bridge();
    if (b && b->getAcorn) return b->getAcorn();
    return nullptr;
}

void* bro_vendor_globals_get_tern(void) {
    const auto* b = bro_get_vendor_globals_bridge();
    if (b && b->getTern) return b->getTern();
    return nullptr;
}

void* bro_vendor_globals_get_esprima(void) {
    const auto* b = bro_get_vendor_globals_bridge();
    if (b && b->getEsprima) return b->getEsprima();
    return nullptr;
}

void* bro_vendor_globals_get_jsonlint(void) {
    const auto* b = bro_get_vendor_globals_bridge();
    if (b && b->getJsonlint) return b->getJsonlint();
    return nullptr;
}

void* bro_vendor_globals_get_draco_encoder(void) {
    const auto* b = bro_get_vendor_globals_bridge();
    if (b && b->getDracoEncoder) return b->getDracoEncoder();
    return nullptr;
}

} // extern "C"
