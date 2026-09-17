#include "bronze_host/host_globals_internal.h"
#include "bronze_host/gl_internal.h"
#include "dom/document.h"
#include "dom/element.h"
#include "dom/shadow_root.h"
#include "util/log.h"
#include <algorithm>
#include <cctype>
#include <unordered_map>
#include <vector>

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

// The marker constructCustomElement leaves on an element's wrapper once the
// class constructor has run on it. A registered tag whose wrapper lacks it
// is parsed markup still waiting for its upgrade; the marker (not the
// registry) is what decides whether a lifecycle callback may fire, because
// only an upgraded wrapper sits on the class prototype that defines them.
constexpr const char* kUpgradedMarker = "__bro_ce_upgraded__";

static const CustomElementDef* definitionFor(dom::Element* el) {
    const std::string& tag = el->tagName();
    if (tag.find('-') == std::string::npos) return nullptr;
    auto it = s_registry.find(toLowerStr(tag));
    return it == s_registry.end() ? nullptr : &it->second;
}

static bool isUpgraded(dom::Element* el) {
    HostNodeState* st = hostNodeStateFor(el);
    if (!st) return false;
    Value v = st->jsObj.get();
    return ev::isObject(v) && ev::toBool(ev::getProperty(v, kUpgradedMarker));
}

// Connected in the DOM sense: a parent chain (crossing shadow roots to
// their hosts) whose top is the document element. bro's Document is not a
// Node, so the chain ends at <html> rather than at a document node.
static bool isConnected(dom::Node* n) {
    while (n) {
        if (n->nodeType() == dom::NodeType::DocumentFragment) {
            auto* sr = dynamic_cast<dom::ShadowRoot*>(n);
            n = sr ? sr->host() : nullptr;
            continue;
        }
        dom::Node* parent = n->parentNode();
        if (!parent) {
            dom::Document* doc = n->document();
            return doc && n->nodeType() == dom::NodeType::Element && doc->documentElement() == n;
        }
        n = parent;
    }
    return false;
}

static void fireLifecycle(dom::Element* el, const char* name) {
    if (!isUpgraded(el)) return;
    Value elVal = hostElementValue(el);
    Value cb = ev::getProperty(elVal, name);
    if (ev::isFunction(cb)) {
        ev::CallResult r = ev::call(cb, elVal, {});
        if (r.thrown) {
            LOG_WARN("customElements: <{}> {} threw: {}", toLowerStr(el->tagName()), name,
                     ev::toUtf8(r.value));
        }
    }
}

// Run the class constructor on an element that was parsed rather than
// created. The constructor's super() lands on the element's existing
// wrapper (constructCustomElementBase), so any reference the app already
// holds keeps its identity and gains the class prototype.
static ev::CallResult runCustomElementConstructor(dom::Element* el, const CustomElementDef& def) {
    s_activeConstructingElement = el;
    s_activeCtor = def.ctor.get();
    ev::CallResult res = ev::construct(s_activeCtor, {});
    s_activeConstructingElement = nullptr;
    s_activeCtor = ev::undefined();
    if (res.thrown) return res;
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
        ev::setProperty(customVal, kUpgradedMarker, ev::fromBool(true));
    }
    return res;
}

static bool upgradeElement(dom::Element* el, const CustomElementDef& def) {
    if (isUpgraded(el)) return true;
    // An upgrade failure is reported, not raised: the spec reports the
    // exception and leaves the element un-upgraded, and the caller here is
    // an innerHTML setter or appendChild that has nothing to do with it.
    ev::CallResult res = runCustomElementConstructor(el, def);
    if (res.thrown) {
        LOG_WARN("customElements: <{}> constructor threw during upgrade: {}", def.tagName,
                 ev::toUtf8(res.value));
        return false;
    }
    // Attributes present at upgrade time are reported the way the spec's
    // upgrade step does: one attributeChangedCallback per observed attribute.
    for (const auto& attr : def.observedAttributes) {
        if (!el->hasAttribute(attr)) continue;
        std::string val = el->getAttribute(attr);
        onCustomElementAttributeChanged(el, attr, nullptr, val.c_str());
    }
    return true;
}

// Depth-first over the element and its light-DOM descendants. An element
// that was just upgraded owns whatever its constructor put in its shadow
// root; the walk does not descend into shadow trees.
template <typename Fn>
static void forEachElementInSubtree(dom::Node* root, bool includeRoot, Fn&& fn) {
    if (!root) return;
    if (includeRoot && root->nodeType() == dom::NodeType::Element) {
        fn(static_cast<dom::Element*>(root));
    }
    // Snapshot: a callback may mutate the tree it is being walked over.
    std::vector<dom::Node*> kids(root->childNodes().begin(), root->childNodes().end());
    for (dom::Node* kid : kids) {
        if (kid->nodeType() != dom::NodeType::Element) continue;
        forEachElementInSubtree(kid, true, fn);
    }
}

static void upgradeAndConnect(dom::Node* root, bool includeRoot) {
    if (s_registry.empty()) return;
    const bool connected = isConnected(root);
    forEachElementInSubtree(root, includeRoot, [&](dom::Element* el) {
        const CustomElementDef* def = definitionFor(el);
        if (!def) return;
        if (!upgradeElement(el, *def)) return;
        if (connected) fireLifecycle(el, "connectedCallback");
    });
}

} // namespace

void onCustomElementConnected(dom::Element* el) {
    if (!el) return;
    upgradeAndConnect(el, true);
}

void onCustomElementDisconnected(dom::Element* el) {
    if (!el || s_registry.empty()) return;
    forEachElementInSubtree(el, true, [](dom::Element* e) {
        if (definitionFor(e)) fireLifecycle(e, "disconnectedCallback");
    });
}

void upgradeCustomElementsInSubtree(dom::Node* root) {
    upgradeAndConnect(root, false);
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
    ev::CallResult res = runCustomElementConstructor(el, it->second);
    if (res.thrown) return ev::throwValue(res.value);
    return res.value;
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

        // Elements already parsed under this name are upgraded now, in
        // document order, and those in the document get connectedCallback:
        // markup routinely precedes the script that defines its components.
        if (dom::Document* doc = currentHostDocument()) {
            const CustomElementDef& stored = s_registry[name];
            for (dom::Element* el : doc->querySelectorAll(name)) {
                if (upgradeElement(el, stored) && isConnected(el)) fireLifecycle(el, "connectedCallback");
            }
        }
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

    ce.def("upgrade", 1, [](Value, std::span<const Value> a) -> Value {
        HostNodeState* st = a.empty() ? nullptr : hostNodeStateOfValue(a[0]);
        dom::Node* root = st ? (st->shadowRoot ? static_cast<dom::Node*>(st->shadowRoot) : st->node) : nullptr;
        if (root) upgradeAndConnect(root, true);
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
