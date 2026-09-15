// native_diffusion.cpp — Bronze native entry points for bro.diffusion
#include "natives/diffusion/native_diffusion_decl.h"

#include <cstdint>
#include <string>

namespace {

struct BroPipelineImpl {
    std::string version = "1.0.0";
};

struct BroPipelineStateImpl {
    int32_t step = 0;
};

}  // namespace

extern "C" {

void bro_diffusion_Pipeline_dtor(void* self) {
    delete static_cast<BroPipelineImpl*>(self);
}

void* bro_diffusion_Pipeline_ctor(void) {
    return new BroPipelineImpl();
}

void bro_diffusion_PipelineState_dtor(void* self) {
    delete static_cast<BroPipelineStateImpl*>(self);
}

void* bro_diffusion_PipelineState_ctor(void) {
    return new BroPipelineStateImpl();
}

const char* bro_diffusion_version_get(void) {
    return "1.0.0";
}

void bro_diffusion_init(void) {
}

}  // extern "C"

namespace bro::bronze_host {
bool registerNatives_diffusion(std::string* error);
bool registerDiffusionNatives(std::string* error) {
    return registerNatives_diffusion(error);
}
}  // namespace bro::bronze_host
