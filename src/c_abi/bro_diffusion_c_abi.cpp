// =============================================================================
// bro_diffusion_c_abi.cpp — C++ forwarding implementations for bro.diffusion
// Generated automatically by brosurface (gen/emit_c_abi.mjs).
// =============================================================================

#include "bro/c_abi/bro_diffusion_c_abi.h"
#include "bro/c_abi/bro_engine_c_abi.h"
#include <cstdint>
#include <string>

struct BroPipelineImpl {
    std::string version = "1.0.0";
};

struct BroPipelineStateImpl {
    int32_t step = 0;
};

extern "C" {

// --- Interface bro.diffusion.Pipeline ---
void* bro_Pipeline_create(void) {
    return new BroPipelineImpl();
}

void bro_Pipeline_destroy(void* self) {
    delete static_cast<BroPipelineImpl*>(self);
}

// --- Interface bro.diffusion.PipelineState ---
void* bro_PipelineState_create(void) {
    return new BroPipelineStateImpl();
}

void bro_PipelineState_destroy(void* self) {
    delete static_cast<BroPipelineStateImpl*>(self);
}

// --- Namespace bro.diffusion ---
const char* bro_diffusion_get_version(void) {
    return "1.0.0";
}

void bro_diffusion_init(void) {
    const auto* b = bro_get_diffusion_bridge();
    if (b && b->init) b->init();
}

void* bro_diffusion_createPipeline(void* config) {
    const auto* b = bro_get_diffusion_bridge();
    if (b && b->createPipeline) return b->createPipeline(config);
    return new BroPipelineImpl();
}

void* bro_diffusion_loadModel(const char* dir, void* opts) {
    const auto* b = bro_get_diffusion_bridge();
    if (b && b->loadModel) return b->loadModel(dir, opts);
    return new BroPipelineImpl();
}

void* bro_diffusion_expandNoise(void* /*src*/, void* /*opts*/) {
    return nullptr;
}

}
