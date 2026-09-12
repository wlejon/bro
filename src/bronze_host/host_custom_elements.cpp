#include "bronze_host/host_globals_internal.h"
#include "bronze_host/gl_internal.h"
#include "dom/element.h"
#include "util/log.h"
#include <algorithm>
#include <cctype>
#include <unordered_map>

namespace bro::bronze_host {

namespace {

struct CustomElementDef {
    std::string tagName;
    ev::Persistent ctor;
    std::vector<std::string> observedAttributes;
};

static std::unordered_map<std::string, CustomElementDef> s_registry;
static thread_local dom::Element* s_activeConstructingElement = nullptr;
static thread_local Value s_activeCtor = ev::undefined();

static std::string toLowerStr(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    return out;
}

} // namespace

void onCustomElementConnected(dom::Element* el) {
    if (!el) return;
    std::string tag = toLowerStr(el->tagName());
    auto it = s_registry.find(tag);
    if (it == s_registry.end()) return;
    Value elVal = hostElementValue(el);
    Value cb = ev::getProperty(elVal, "connectedCallback");
    if (ev::isFunction(cb)) {
        ev::call(cb, elVal, {});
    }
}

void onCustomElementDisconnected(dom::Element* el) {
    if (!el) return;
    std::string tag = toLowerStr(el->tagName());
    auto it = s_registry.find(tag);
    if (it == s_registry.end()) return;
    Value elVal = hostElementValue(el);
    Value cb = ev::getProperty(elVal, "disconnectedCallback");
    if (ev::isFunction(cb)) {
        ev::call(cb, elVal, {});
    }
}

void onCustomElementAttributeChanged(dom::Element* el, const std::string& name,
                                     const char* oldValue, const char* newValue) {
    if (!el) return;
    std::string tag = toLowerStr(el->tagName());
    auto it = s_registry.find(tag);
    if (it == s_registry.end()) return;
    const auto& def = it->second;
    bool observed = false;
    for (const auto& attr : def.observedAttributes) {
        if (attr == name) { observed = true; break; }
    }
    if (!observed) return;

    Value elVal = hostElementValue(el);
    Value cb = ev::getProperty(elVal, "attributeChangedCallback");
    if (ev::isFunction(cb)) {
        Value nameVal = ev::fromUtf8(name);
        Value oldVal = oldValue ? ev::fromUtf8(oldValue) : ev::null();
        Value newVal = newValue ? ev::fromUtf8(newValue) : ev::null();
        const Value args[3] = { nameVal, oldVal, newVal };
        ev::call(cb, elVal, std::span<const Value>(args, 3));
    }
}

Value constructCustomElement(dom::Element* el, const std::string& tagName) {
    std::string tag = toLowerStr(tagName);
    auto it = s_registry.find(tag);
    if (it == s_registry.end()) return ev::undefined();

    s_activeConstructingElement = el;
    s_activeCtor = it->second.ctor.get();
    ev::CallResult res = ev::construct(s_activeCtor, {});
    s_activeConstructingElement = nullptr;
    s_activeCtor = ev::undefined();
    if (res.thrown) {
        return ev::throwValue(res.value);
    }
    Value customVal = res.value;
    if (ev::isObject(customVal)) {
        noteHostElementValue(el, customVal);
        HostNodeState* st = hostNodeStateFor(el);
        if (st) {
            ev::setProperty(customVal, "__bro_node_id__",
                            ev::fromDouble(static_cast<double>(reinterpret_cast<uintptr_t>(st))));
        }
        ev::setProperty(customVal, "nodeType", ev::fromDouble(1));
        ev::setProperty(customVal, "tagName", ev::fromUtf8(el->tagName()));
        ev::setProperty(customVal, "nodeName", ev::fromUtf8(el->tagName()));
    }
    return customVal;
}

Value constructCustomElementBase() {
    if (s_activeConstructingElement) {
        return hostElementValue(s_activeConstructingElement);
    }
    return ev::throwTypeError("Illegal constructor");
}

void installCustomElementsGlobals() {
    ObjectBuilder ce;
    ce.def("define", 2, [](Value, std::span<const Value> a) -> Value {
        if (a.size() < 2 || !ev::isString(a[0]) || !ev::isFunction(a[1])) {
            return ev::throwTypeError("customElements.define requires (name, constructor)");
        }
        std::string name = toLowerStr(ev::toUtf8(a[0]));
        if (name.find('-') == std::string::npos) {
            return ev::throwTypeError("customElements.define: name must contain a hyphen");
        }
        if (s_registry.find(name) != s_registry.end()) {
            return ev::throwTypeError("customElements.define: name already defined");
        }

        CustomElementDef def;
        def.tagName = name;
        def.ctor.set(a[1]);

        Value obs = ev::getProperty(a[1], "observedAttributes");
        if (ev::isObject(obs)) {
            Value lenV = ev::getProperty(obs, "length");
            if (ev::isNumber(lenV)) {
                uint32_t len = static_cast<uint32_t>(ev::toDouble(lenV));
                for (uint32_t i = 0; i < len; i++) {
                    Value item = ev::getElement(obs, i);
                    if (ev::isString(item)) {
                        def.observedAttributes.push_back(ev::toUtf8(item));
                    }
                }
            }
        }
        s_registry[name] = std::move(def);
        return ev::undefined();
    });

    ce.def("get", 1, [](Value, std::span<const Value> a) -> Value {
        if (a.empty() || !ev::isString(a[0])) return ev::undefined();
        std::string name = toLowerStr(ev::toUtf8(a[0]));
        auto it = s_registry.find(name);
        return it != s_registry.end() ? it->second.ctor.get() : ev::undefined();
    });

    ce.def("whenDefined", 1, [](Value, std::span<const Value>) -> Value {
        Value p = ev::createPromise();
        ev::resolvePromise(p, ev::undefined());
        return p;
    });

    ce.def("upgrade", 1, [](Value, std::span<const Value>) -> Value {
        return ev::undefined();
    });

    Value customElements = ce.get();
    ev::registerGlobal("customElements", customElements);
    auto g = ev::globalValue("globalThis");
    if (g.found && ev::isObject(g.value)) {
        ev::setProperty(g.value, "customElements", customElements);
    }
}

} // namespace bro::bronze_host
