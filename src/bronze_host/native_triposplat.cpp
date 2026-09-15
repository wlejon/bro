// native_triposplat.cpp — Bronze native entry points for bro.triposplat
#include "natives/triposplat/native_triposplat_decl.h"

#include <string>

namespace {

struct BroTripoSplatPipelineImpl {
    std::string device = "CPU";
};

}  // namespace

extern "C" {

void bro_triposplat_TripoSplatPipeline_dtor(void* self) {
    delete static_cast<BroTripoSplatPipelineImpl*>(self);
}

void* bro_triposplat_TripoSplatPipeline_ctor(void) {
    return new BroTripoSplatPipelineImpl();
}

void bro_triposplat_init(void) {
}

void bro_triposplat_cancel(void) {
}

}  // extern "C"

namespace bro::bronze_host {
bool registerNatives_triposplat(std::string* error);
bool registerTriposplatNatives(std::string* error) {
    return registerNatives_triposplat(error);
}
}  // namespace bro::bronze_host
