// =============================================================================
// bro_domparser_c_abi.cpp — C++ forwarding implementations for bro.domparser
// Generated automatically by brosurface (gen/emit_c_abi.mjs).
// =============================================================================

#include "bro/c_abi/bro_domparser_c_abi.h"
struct BroDOMParserImpl {
    int dummy = 0;
};

extern "C" {

void* bro_DOMParser_create(void) {
    return new BroDOMParserImpl();
}

void bro_DOMParser_destroy(void* self) {
    delete static_cast<BroDOMParserImpl*>(self);
}

void* bro_DOMParser_parseFromString(void* /*self*/, const char* /*str*/, const char* /*type*/) {
    return nullptr;
}

} // extern "C"
