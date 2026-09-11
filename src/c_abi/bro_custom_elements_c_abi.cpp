// =============================================================================
// bro_custom_elements_c_abi.cpp — C++ forwarding implementations for bro.custom_elements
// Generated automatically by brosurface (gen/emit_c_abi.mjs).
// =============================================================================

#include "bro/c_abi/bro_custom_elements_c_abi.h"
#include "bro/c_abi/bro_engine_c_abi.h"
#include <string>
#include <unordered_map>

struct BroCustomElementRegistryImpl {
    std::unordered_map<std::string, void*> elements;
};

struct BroHTMLElementImpl {
    int32_t dummy = 0;
};

static BroCustomElementRegistryImpl s_default_custom_elements;

extern "C" {

void* bro_CustomElementRegistry_create(void) {
    return new BroCustomElementRegistryImpl();
}

void bro_CustomElementRegistry_destroy(void* self) {
    if (self && self != &s_default_custom_elements) {
        delete static_cast<BroCustomElementRegistryImpl*>(self);
    }
}

void* bro_customElements(void* /*source*/) {
    const auto* b = bro_get_custom_elements_bridge();
    if (b && b->getRegistry) {
        return b->getRegistry();
    }
    return &s_default_custom_elements;
}

void bro_CustomElementRegistry_define(void* self, const char* name, void* constructor, void* options) {
    const auto* b = bro_get_custom_elements_bridge();
    if (b && b->define) {
        b->define(self, name, constructor, options);
        return;
    }
    (void)options;
    auto* r = self ? static_cast<BroCustomElementRegistryImpl*>(self) : &s_default_custom_elements;
    if (name) {
        r->elements[name] = constructor;
    }
}

void* bro_CustomElementRegistry_get(void* self, const char* name) {
    const auto* b = bro_get_custom_elements_bridge();
    if (b && b->get) {
        return b->get(self, name);
    }
    auto* r = self ? static_cast<BroCustomElementRegistryImpl*>(self) : &s_default_custom_elements;
    if (name) {
        auto it = r->elements.find(name);
        if (it != r->elements.end()) return it->second;
    }
    return nullptr;
}

void* bro_CustomElementRegistry_whenDefined(void* self, const char* name) {
    const auto* b = bro_get_custom_elements_bridge();
    if (b && b->whenDefined) {
        return b->whenDefined(self, name);
    }
    (void)self;
    (void)name;
    return nullptr;
}

void bro_CustomElementRegistry_upgrade(void* self, void* root) {
    const auto* b = bro_get_custom_elements_bridge();
    if (b && b->upgrade) {
        b->upgrade(self, root);
        return;
    }
    (void)self;
    (void)root;
}

void* bro_HTMLElement_create(void) {
    const auto* b = bro_get_custom_elements_bridge();
    if (b && b->createElement) {
        return b->createElement();
    }
    return new BroHTMLElementImpl();
}

void bro_HTMLElement_destroy(void* self) {
    const auto* b = bro_get_custom_elements_bridge();
    if (b && b->destroyElement) {
        b->destroyElement(self);
        return;
    }
    delete static_cast<BroHTMLElementImpl*>(self);
}

} // extern "C"
