// =============================================================================
// bro_matchmedia_c_abi.cpp — C++ forwarding implementations for bro.matchmedia
// Generated automatically by brosurface (gen/emit_c_abi.mjs).
// =============================================================================

#include "bro/c_abi/bro_matchmedia_c_abi.h"
#include "bro/c_abi/bro_engine_c_abi.h"
#include <string>
#include <vector>
#include <algorithm>

struct BroMediaQueryListImpl {
    std::string media = "all";
    bool matches = true;
    void* onchange = nullptr;
    std::vector<void*> listeners;
};

extern "C" {

void* bro_MediaQueryList_create(void) {
    const auto* b = bro_get_matchmedia_bridge();
    if (b && b->create) {
        return b->create("all");
    }
    return new BroMediaQueryListImpl();
}

void bro_MediaQueryList_destroy(void* self) {
    const auto* b = bro_get_matchmedia_bridge();
    if (b && b->destroy) {
        b->destroy(self);
        return;
    }
    delete static_cast<BroMediaQueryListImpl*>(self);
}

void* bro_matchMedia(void* /*source*/) {
    const auto* b = bro_get_matchmedia_bridge();
    if (b && b->matchMedia) {
        return b->matchMedia("all");
    }
    auto* mql = new BroMediaQueryListImpl();
    mql->media = "all";
    mql->matches = true;
    return mql;
}

bool bro_MediaQueryList_get_matches(void* self) {
    const auto* b = bro_get_matchmedia_bridge();
    if (b && b->getMatches) {
        return b->getMatches(self);
    }
    auto* m = static_cast<BroMediaQueryListImpl*>(self);
    return m ? m->matches : false;
}

const char* bro_MediaQueryList_get_media(void* self) {
    const auto* b = bro_get_matchmedia_bridge();
    if (b && b->getMedia) {
        return b->getMedia(self);
    }
    auto* m = static_cast<BroMediaQueryListImpl*>(self);
    return m ? m->media.c_str() : "all";
}

void* bro_MediaQueryList_get_onchange(void* self) {
    const auto* b = bro_get_matchmedia_bridge();
    if (b && b->getOnchange) {
        return b->getOnchange(self);
    }
    auto* m = static_cast<BroMediaQueryListImpl*>(self);
    return m ? m->onchange : nullptr;
}

void bro_MediaQueryList_set_onchange(void* self, void* val) {
    const auto* b = bro_get_matchmedia_bridge();
    if (b && b->setOnchange) {
        b->setOnchange(self, val);
        return;
    }
    auto* m = static_cast<BroMediaQueryListImpl*>(self);
    if (m) {
        m->onchange = val;
    }
}

void bro_MediaQueryList_addEventListener(void* self, const char* type, void* listener, void* options) {
    const auto* b = bro_get_matchmedia_bridge();
    if (b && b->addEventListener) {
        b->addEventListener(self, type, listener, options);
        return;
    }
    (void)type;
    (void)options;
    auto* m = static_cast<BroMediaQueryListImpl*>(self);
    if (m && listener) {
        m->listeners.push_back(listener);
    }
}

void bro_MediaQueryList_removeEventListener(void* self, const char* type, void* listener, void* options) {
    const auto* b = bro_get_matchmedia_bridge();
    if (b && b->removeEventListener) {
        b->removeEventListener(self, type, listener, options);
        return;
    }
    (void)type;
    (void)options;
    auto* m = static_cast<BroMediaQueryListImpl*>(self);
    if (m && listener) {
        auto it = std::find(m->listeners.begin(), m->listeners.end(), listener);
        if (it != m->listeners.end()) {
            m->listeners.erase(it);
        }
    }
}

void bro_MediaQueryList_addListener(void* self, void* listener) {
    const auto* b = bro_get_matchmedia_bridge();
    if (b && b->addListener) {
        b->addListener(self, listener);
        return;
    }
    auto* m = static_cast<BroMediaQueryListImpl*>(self);
    if (m && listener) {
        m->listeners.push_back(listener);
    }
}

void bro_MediaQueryList_removeListener(void* self, void* listener) {
    const auto* b = bro_get_matchmedia_bridge();
    if (b && b->removeListener) {
        b->removeListener(self, listener);
        return;
    }
    auto* m = static_cast<BroMediaQueryListImpl*>(self);
    if (m && listener) {
        auto it = std::find(m->listeners.begin(), m->listeners.end(), listener);
        if (it != m->listeners.end()) {
            m->listeners.erase(it);
        }
    }
}

} // extern "C"
