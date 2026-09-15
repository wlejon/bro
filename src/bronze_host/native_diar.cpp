// native_diar.cpp — Bronze native entry points for bro.diar
#include "natives/diar/native_diar_decl.h"

#include <cstdint>
#include <string>

namespace {

struct BroSortformerImpl {
    std::string device = "CPU";
    bool busy = false;
};

struct BroSortformerSessionImpl {
    std::string device = "CPU";
    bool busy = false;
};

struct BroClusterDiarizerImpl {
    std::string device = "CPU";
    bool busy = false;
};

}  // namespace

extern "C" {

void bro_diar_Sortformer_dtor(void* self) {
    delete static_cast<BroSortformerImpl*>(self);
}

void* bro_diar_Sortformer_ctor(void) {
    return new BroSortformerImpl();
}

void bro_diar_SortformerSession_dtor(void* self) {
    delete static_cast<BroSortformerSessionImpl*>(self);
}

void* bro_diar_SortformerSession_ctor(void) {
    return new BroSortformerSessionImpl();
}

void bro_diar_ClusterDiarizer_dtor(void* self) {
    delete static_cast<BroClusterDiarizerImpl*>(self);
}

void* bro_diar_ClusterDiarizer_ctor(void) {
    return new BroClusterDiarizerImpl();
}

void* bro_diar_Sortformer_createSession(void* /*self*/) {
    return new BroSortformerSessionImpl();
}

const char* bro_diar_Sortformer_device_get(void* self) {
    return self ? static_cast<BroSortformerImpl*>(self)->device.c_str() : "CPU";
}

bool bro_diar_Sortformer_busy_get(void* self) {
    return self && static_cast<BroSortformerImpl*>(self)->busy;
}

void bro_diar_SortformerSession_reset(void* /*self*/) {
}

const char* bro_diar_SortformerSession_device_get(void* self) {
    return self ? static_cast<BroSortformerSessionImpl*>(self)->device.c_str() : "CPU";
}

bool bro_diar_SortformerSession_busy_get(void* self) {
    return self && static_cast<BroSortformerSessionImpl*>(self)->busy;
}

const char* bro_diar_ClusterDiarizer_device_get(void* self) {
    return self ? static_cast<BroClusterDiarizerImpl*>(self)->device.c_str() : "CPU";
}

bool bro_diar_ClusterDiarizer_busy_get(void* self) {
    return self && static_cast<BroClusterDiarizerImpl*>(self)->busy;
}

void bro_diar_init(void) {
}

}  // extern "C"

namespace bro::bronze_host {
bool registerNatives_diar(std::string* error);
bool registerDiarNatives(std::string* error) {
    return registerNatives_diar(error);
}
}  // namespace bro::bronze_host
