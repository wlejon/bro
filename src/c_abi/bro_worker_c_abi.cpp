// =============================================================================
// bro_worker_c_abi.cpp — C++ forwarding implementations for bro.worker
// Generated automatically by brosurface (gen/emit_c_abi.mjs).
// =============================================================================

#include "bro/c_abi/bro_worker_c_abi.h"
#include <string>

struct BroWorkerImpl {
    std::string scriptURL;
    bool terminated = false;
    void* onmessage = nullptr;
    void* onerror = nullptr;
    void* onmessageerror = nullptr;
};

extern "C" {

void* bro_Worker_create(const char* scriptURL, void* /*options*/) {
    auto* w = new BroWorkerImpl();
    if (scriptURL) w->scriptURL = scriptURL;
    return w;
}

void bro_Worker_destroy(void* self) {
    delete static_cast<BroWorkerImpl*>(self);
}

void bro_Worker_postMessage(void* /*self*/, void* /*message*/, void* /*transfer*/) {
}

void bro_Worker_terminate(void* self) {
    if (self) static_cast<BroWorkerImpl*>(self)->terminated = true;
}

void* bro_Worker_get_onmessage(void* self) {
    return self ? static_cast<BroWorkerImpl*>(self)->onmessage : nullptr;
}

void bro_Worker_set_onmessage(void* self, void* val) {
    if (self) static_cast<BroWorkerImpl*>(self)->onmessage = val;
}

void* bro_Worker_get_onerror(void* self) {
    return self ? static_cast<BroWorkerImpl*>(self)->onerror : nullptr;
}

void bro_Worker_set_onerror(void* self, void* val) {
    if (self) static_cast<BroWorkerImpl*>(self)->onerror = val;
}

void* bro_Worker_get_onmessageerror(void* self) {
    return self ? static_cast<BroWorkerImpl*>(self)->onmessageerror : nullptr;
}

void bro_Worker_set_onmessageerror(void* self, void* val) {
    if (self) static_cast<BroWorkerImpl*>(self)->onmessageerror = val;
}

} // extern "C"
