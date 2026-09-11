// =============================================================================
// bro_motion_c_abi.cpp — C++ forwarding implementations for bro.motion
// Generated automatically by brosurface (gen/emit_c_abi.mjs).
// =============================================================================

#include "bro/c_abi/bro_motion_c_abi.h"
#include "bro/c_abi/bro_engine_c_abi.h"
#include <cstdint>
#include <string>

struct BroArdyMotionPipelineImpl {
    std::string device = "CPU";
};

extern "C" {

// --- Interface bro.motion.ArdyMotionPipeline ---
void* bro_ArdyMotionPipeline_create(void) {
    return new BroArdyMotionPipelineImpl();
}

void bro_ArdyMotionPipeline_destroy(void* self) {
    delete static_cast<BroArdyMotionPipelineImpl*>(self);
}

const char* bro_ArdyMotionPipeline_get_device(void* self) {
    auto* p = static_cast<BroArdyMotionPipelineImpl*>(self);
    return p ? p->device.c_str() : "CPU";
}

void* bro_ArdyMotionPipeline_generate(void* /*self*/, const char* /*text*/, void* /*opts*/) {
    return nullptr;
}

// --- Namespace bro.motion ---
void bro_motion_init(void) {
    const auto* b = bro_get_motion_bridge();
    if (b && b->init) {
        b->init();
        return;
    }
}

void* bro_motion_load(void* opts) {
    const auto* b = bro_get_motion_bridge();
    if (b && b->load) {
        return b->load(opts);
    }
    return new BroArdyMotionPipelineImpl();
}

}
