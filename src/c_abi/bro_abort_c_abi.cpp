// =============================================================================
// bro_abort_c_abi.cpp — C++ forwarding implementations for bro.abort
// Generated automatically by brosurface (gen/emit_c_abi.mjs).
// =============================================================================

#include "bro/c_abi/bro_abort_c_abi.h"
#include <cstdint>
#include <atomic>

struct BroAbortSignalImpl {
    std::atomic<uint32_t> refCount{1};
    bool aborted = false;
    void* reason = nullptr;
    void* onabort = nullptr;
};

struct BroAbortControllerImpl {
    BroAbortSignalImpl* signal = nullptr;

    BroAbortControllerImpl() {
        signal = new BroAbortSignalImpl();
    }
    ~BroAbortControllerImpl() {
        if (signal && --signal->refCount == 0) {
            delete signal;
        }
    }
};

extern "C" {

// AbortSignal
void* bro_AbortSignal_create(void) {
    return new BroAbortSignalImpl();
}

void bro_AbortSignal_destroy(void* self) {
    auto* s = static_cast<BroAbortSignalImpl*>(self);
    if (s && --s->refCount == 0) {
        delete s;
    }
}

bool bro_AbortSignal_get_aborted(void* self) {
    return self ? static_cast<BroAbortSignalImpl*>(self)->aborted : false;
}

void* bro_AbortSignal_get_reason(void* self) {
    return self ? static_cast<BroAbortSignalImpl*>(self)->reason : nullptr;
}

void* bro_AbortSignal_get_onabort(void* self) {
    return self ? static_cast<BroAbortSignalImpl*>(self)->onabort : nullptr;
}

void bro_AbortSignal_set_onabort(void* self, void* val) {
    if (self) static_cast<BroAbortSignalImpl*>(self)->onabort = val;
}

void bro_AbortSignal_addEventListener(void* /*self*/, const char* /*type*/, void* /*listener*/) {}
void bro_AbortSignal_removeEventListener(void* /*self*/, const char* /*type*/, void* /*listener*/) {}
bool bro_AbortSignal_dispatchEvent(void* /*self*/, void* /*event*/) { return true; }
void bro_AbortSignal_throwIfAborted(void* /*self*/) {}

void* bro_AbortSignal_abort(void* reason) {
    auto* s = new BroAbortSignalImpl();
    s->aborted = true;
    s->reason = reason;
    return s;
}

void* bro_AbortSignal_timeout(uint64_t /*milliseconds*/) {
    return new BroAbortSignalImpl();
}

void* bro_AbortSignal_any(void* /*signals*/) {
    return new BroAbortSignalImpl();
}

// AbortController
void* bro_AbortController_create(void) {
    return new BroAbortControllerImpl();
}

void bro_AbortController_destroy(void* self) {
    delete static_cast<BroAbortControllerImpl*>(self);
}

void* bro_AbortController_get_signal(void* self) {
    if (!self) return nullptr;
    auto* c = static_cast<BroAbortControllerImpl*>(self);
    if (c->signal) {
        c->signal->refCount++;
        return c->signal;
    }
    return nullptr;
}

void bro_AbortController_abort(void* self, void* reason) {
    if (!self) return;
    auto* c = static_cast<BroAbortControllerImpl*>(self);
    if (c->signal) {
        c->signal->aborted = true;
        c->signal->reason = reason;
    }
}

} // extern "C"
