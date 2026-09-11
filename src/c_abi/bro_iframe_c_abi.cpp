// =============================================================================
// bro_iframe_c_abi.cpp — C++ forwarding implementations for bro.iframe
// Generated automatically by brosurface (gen/emit_c_abi.mjs).
// =============================================================================

#include "bro/c_abi/bro_iframe_c_abi.h"
#include "bro/c_abi/bro_engine_c_abi.h"
#include <string>

struct BroHTMLIFrameElementImpl {
    std::string src;
    std::string width = "300";
    std::string height = "150";
    void* contentDocument = nullptr;
    void* contentWindow = nullptr;
};

extern "C" {

void* bro_HTMLIFrameElement_create(void) {
    const auto* b = bro_get_iframe_bridge();
    if (b && b->create) {
        return b->create();
    }
    return new BroHTMLIFrameElementImpl();
}

void bro_HTMLIFrameElement_destroy(void* self) {
    const auto* b = bro_get_iframe_bridge();
    if (b && b->destroy) {
        b->destroy(self);
        return;
    }
    delete static_cast<BroHTMLIFrameElementImpl*>(self);
}

const char* bro_HTMLIFrameElement_get_src(void* self) {
    const auto* b = bro_get_iframe_bridge();
    if (b && b->getSrc) {
        return b->getSrc(self);
    }
    auto* f = static_cast<BroHTMLIFrameElementImpl*>(self);
    return f ? f->src.c_str() : "";
}

void bro_HTMLIFrameElement_set_src(void* self, const char* val) {
    const auto* b = bro_get_iframe_bridge();
    if (b && b->setSrc) {
        b->setSrc(self, val);
        return;
    }
    auto* f = static_cast<BroHTMLIFrameElementImpl*>(self);
    if (f) {
        f->src = val ? val : "";
    }
}

const char* bro_HTMLIFrameElement_get_width(void* self) {
    const auto* b = bro_get_iframe_bridge();
    if (b && b->getWidth) {
        return b->getWidth(self);
    }
    auto* f = static_cast<BroHTMLIFrameElementImpl*>(self);
    return f ? f->width.c_str() : "";
}

void bro_HTMLIFrameElement_set_width(void* self, const char* val) {
    const auto* b = bro_get_iframe_bridge();
    if (b && b->setWidth) {
        b->setWidth(self, val);
        return;
    }
    auto* f = static_cast<BroHTMLIFrameElementImpl*>(self);
    if (f) {
        f->width = val ? val : "";
    }
}

const char* bro_HTMLIFrameElement_get_height(void* self) {
    const auto* b = bro_get_iframe_bridge();
    if (b && b->getHeight) {
        return b->getHeight(self);
    }
    auto* f = static_cast<BroHTMLIFrameElementImpl*>(self);
    return f ? f->height.c_str() : "";
}

void bro_HTMLIFrameElement_set_height(void* self, const char* val) {
    const auto* b = bro_get_iframe_bridge();
    if (b && b->setHeight) {
        b->setHeight(self, val);
        return;
    }
    auto* f = static_cast<BroHTMLIFrameElementImpl*>(self);
    if (f) {
        f->height = val ? val : "";
    }
}

void* bro_HTMLIFrameElement_get_contentDocument(void* self) {
    const auto* b = bro_get_iframe_bridge();
    if (b && b->getContentDocument) {
        return b->getContentDocument(self);
    }
    auto* f = static_cast<BroHTMLIFrameElementImpl*>(self);
    return f ? f->contentDocument : nullptr;
}

void* bro_HTMLIFrameElement_get_contentWindow(void* self) {
    const auto* b = bro_get_iframe_bridge();
    if (b && b->getContentWindow) {
        return b->getContentWindow(self);
    }
    auto* f = static_cast<BroHTMLIFrameElementImpl*>(self);
    return f ? f->contentWindow : nullptr;
}

void bro_HTMLIFrameElement_reload(void* self) {
    const auto* b = bro_get_iframe_bridge();
    if (b && b->reload) {
        b->reload(self);
        return;
    }
    (void)self;
}

void* bro_HTMLIFrameElement_capture(void* self) {
    const auto* b = bro_get_iframe_bridge();
    if (b && b->capture) {
        return b->capture(self);
    }
    (void)self;
    return nullptr;
}

} // extern "C"
