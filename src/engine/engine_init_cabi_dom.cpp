#include "engine/engine_init_cabi_dom.h"
#include "bro/c_abi/bro_engine_c_abi.h"

#include <string>
#include <vector>
#include <unordered_map>
#include <algorithm>

namespace bro::engine {
namespace {

struct EngineCustomElementRegistry {
    std::unordered_map<std::string, void*> elements;
};
static EngineCustomElementRegistry s_engine_registry;

struct EngineHTMLElement {
    int32_t id = 0;
};

struct EngineIframeElement {
    std::string src;
    std::string width = "300";
    std::string height = "150";
    void* contentDocument = nullptr;
    void* contentWindow = nullptr;
};

struct EngineMediaQueryList {
    std::string media = "all";
    bool matches = true;
    void* onchange = nullptr;
    std::vector<void*> listeners;
};

} // namespace

void bro_engine_register_cabi_dom_bridges() {
    static BroCustomElementsBridge s_engine_custom_elements_bridge = {
        .getRegistry = []() -> void* { return &s_engine_registry; },
        .define = [](void* self, const char* name, void* constructor, void* /*options*/) {
            auto* r = self ? static_cast<EngineCustomElementRegistry*>(self) : &s_engine_registry;
            if (name) r->elements[name] = constructor;
        },
        .get = [](void* self, const char* name) -> void* {
            auto* r = self ? static_cast<EngineCustomElementRegistry*>(self) : &s_engine_registry;
            if (name) {
                auto it = r->elements.find(name);
                if (it != r->elements.end()) return it->second;
            }
            return nullptr;
        },
        .whenDefined = [](void* /*self*/, const char* /*name*/) -> void* { return nullptr; },
        .upgrade = [](void* /*self*/, void* /*root*/) {},
        .createElement = []() -> void* { return new EngineHTMLElement(); },
        .destroyElement = [](void* self) { delete static_cast<EngineHTMLElement*>(self); }
    };
    bro_set_custom_elements_bridge(&s_engine_custom_elements_bridge);

    static BroIframeBridge s_engine_iframe_bridge = {
        .create = []() -> void* { return new EngineIframeElement(); },
        .destroy = [](void* self) { delete static_cast<EngineIframeElement*>(self); },
        .getSrc = [](void* self) -> const char* {
            auto* f = static_cast<EngineIframeElement*>(self);
            return f ? f->src.c_str() : "";
        },
        .setSrc = [](void* self, const char* val) {
            auto* f = static_cast<EngineIframeElement*>(self);
            if (f) f->src = val ? val : "";
        },
        .getWidth = [](void* self) -> const char* {
            auto* f = static_cast<EngineIframeElement*>(self);
            return f ? f->width.c_str() : "300";
        },
        .setWidth = [](void* self, const char* val) {
            auto* f = static_cast<EngineIframeElement*>(self);
            if (f) f->width = val ? val : "";
        },
        .getHeight = [](void* self) -> const char* {
            auto* f = static_cast<EngineIframeElement*>(self);
            return f ? f->height.c_str() : "150";
        },
        .setHeight = [](void* self, const char* val) {
            auto* f = static_cast<EngineIframeElement*>(self);
            if (f) f->height = val ? val : "";
        },
        .getContentDocument = [](void* self) -> void* {
            auto* f = static_cast<EngineIframeElement*>(self);
            return f ? f->contentDocument : nullptr;
        },
        .getContentWindow = [](void* self) -> void* {
            auto* f = static_cast<EngineIframeElement*>(self);
            return f ? f->contentWindow : nullptr;
        },
        .reload = [](void* /*self*/) {},
        .capture = [](void* /*self*/) -> void* { return nullptr; }
    };
    bro_set_iframe_bridge(&s_engine_iframe_bridge);

    static BroMatchMediaBridge s_engine_matchmedia_bridge = {
        .create = [](const char* query) -> void* {
            auto* mql = new EngineMediaQueryList();
            mql->media = (query && query[0] != '\0') ? query : "all";
            mql->matches = true;
            return mql;
        },
        .destroy = [](void* self) { delete static_cast<EngineMediaQueryList*>(self); },
        .getMatches = [](void* self) -> bool {
            auto* m = static_cast<EngineMediaQueryList*>(self);
            return m ? m->matches : false;
        },
        .getMedia = [](void* self) -> const char* {
            auto* m = static_cast<EngineMediaQueryList*>(self);
            return m ? m->media.c_str() : "all";
        },
        .getOnchange = [](void* self) -> void* {
            auto* m = static_cast<EngineMediaQueryList*>(self);
            return m ? m->onchange : nullptr;
        },
        .setOnchange = [](void* self, void* cb) {
            auto* m = static_cast<EngineMediaQueryList*>(self);
            if (m) m->onchange = cb;
        },
        .addEventListener = [](void* self, const char* /*type*/, void* listener, void* /*options*/) {
            auto* m = static_cast<EngineMediaQueryList*>(self);
            if (m && listener) m->listeners.push_back(listener);
        },
        .removeEventListener = [](void* self, const char* /*type*/, void* listener, void* /*options*/) {
            auto* m = static_cast<EngineMediaQueryList*>(self);
            if (m && listener) {
                auto it = std::find(m->listeners.begin(), m->listeners.end(), listener);
                if (it != m->listeners.end()) m->listeners.erase(it);
            }
        },
        .addListener = [](void* self, void* listener) {
            auto* m = static_cast<EngineMediaQueryList*>(self);
            if (m && listener) m->listeners.push_back(listener);
        },
        .removeListener = [](void* self, void* listener) {
            auto* m = static_cast<EngineMediaQueryList*>(self);
            if (m && listener) {
                auto it = std::find(m->listeners.begin(), m->listeners.end(), listener);
                if (it != m->listeners.end()) m->listeners.erase(it);
            }
        },
        .matchMedia = [](const char* query) -> void* {
            auto* mql = new EngineMediaQueryList();
            mql->media = (query && query[0] != '\0') ? query : "all";
            mql->matches = true;
            return mql;
        }
    };
    bro_set_matchmedia_bridge(&s_engine_matchmedia_bridge);

    static BroVendorGlobalsBridge s_engine_vendor_globals_bridge = {
        .getSignals = []() -> void* { return nullptr; },
        .getCodeMirror = []() -> void* { return nullptr; },
        .getAcorn = []() -> void* { return nullptr; },
        .getTern = []() -> void* { return nullptr; },
        .getEsprima = []() -> void* { return nullptr; },
        .getJsonlint = []() -> void* { return nullptr; },
        .getDracoEncoder = []() -> void* { return nullptr; }
    };
    bro_set_vendor_globals_bridge(&s_engine_vendor_globals_bridge);
}

} // namespace bro::engine
