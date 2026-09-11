// =============================================================================
// bro_triposplat_c_abi.cpp — C++ forwarding implementations for bro.triposplat
// Generated automatically by brosurface (gen/emit_c_abi.mjs).
// =============================================================================

#include "bro/c_abi/bro_triposplat_c_abi.h"
#include <cstdint>
#include <string>

struct BroTripoSplatPipelineImpl {
    std::string device = "CPU";
};

extern "C" {

// --- Interface bro.triposplat.TripoSplatPipeline ---
void* bro_TripoSplatPipeline_create(void) {
    return new BroTripoSplatPipelineImpl();
}

void bro_TripoSplatPipeline_destroy(void* self) {
    delete static_cast<BroTripoSplatPipelineImpl*>(self);
}

// --- Namespace bro.triposplat ---
void bro_triposplat_init(void) {
}

void* bro_triposplat_load(void* /*config*/) {
    return new BroTripoSplatPipelineImpl();
}

void bro_triposplat_cancel(void) {
}

}
