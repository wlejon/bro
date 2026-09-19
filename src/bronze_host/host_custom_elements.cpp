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

// The tag a registered class builds, stamped on that class's PROTOTYPE by
// customElements.define. It is how the HTMLElement constructor learns which
// element `new MyElement()` means: bronze's NativeFn boundary carries no
// new.target, and the receiver's prototype chain is the one thing at hand that
// names the class — see constructCustomElementBase.
constexpr const char* kTagMarker = "__bro_ce_tag__";

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
            LOG_WARN("customElements: <%s> %s threw: %s", toLowerStr(el->tagName()).c_str(), name,
                     ev::toUtf8(r.value).c_str());
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
        LOG_WARN("customElements: <%s> constructor threw during upgrade: %s", def.tagName.c_str(),
                 ev::toUtf8(res.value).c_str());
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

// The HTMLElement constructor (HTML §4.13.4, "HTML element constructors").
//
// TWO CALLERS, and the difference is which object already exists.
//
// An UPGRADE starts from an element that is already in the tree: the element
// being constructed IS the return value, so the derived constructor continues
// on the wrapper the app may already hold, and its prototype becomes the
// class's — NewTarget.prototype in the spec, the registered constructor's here
// — which turns a plain wrapper into an instance of the class without changing
// which object it is.
//
// `new MyElement()` starts from nothing: the spec says to create an element
// with the tag the registry holds against NewTarget and adopt the object the
// `new` produced as its wrapper. The port had no answer for this case and
// threw "Illegal constructor" for every defined element — so a component
// library's own `new Row()` (the spelling that avoids a createElement +
// setAttribute round trip) could not build a single instance.
//
// The tag comes off the object's prototype chain. customElements.define stamps
// kTagMarker on each registered class's prototype, so reading it through the
// receiver resolves to the NEAREST registered class above it — which is the
// right answer for `class Fancy extends Plain` where both are defined.
Value constructCustomElementBase(Value newObject) {
    if (s_activeConstructingElement) {
        // Rooted across the prototype read, which may allocate the class's
        // prototype object on first touch.
        ev::Persistent wrapper(hostElementValue(s_activeConstructingElement));
        if (ev::isObject(wrapper.get()) && ev::isFunction(s_activeCtor)) {
            Value proto = ev::getProperty(s_activeCtor, "prototype");
            if (ev::isObject(proto)) return ev::setPrototype(wrapper.get(), proto);
        }
        return wrapper.get();
    }

    if (!ev::isObject(newObject)) return ev::throwTypeError("Illegal constructor");
    ev::Persistent self(newObject);
    Value tagVal = ev::getProperty(self.get(), kTagMarker);
    if (!ev::isString(tagVal)) return ev::throwTypeError("Illegal constructor");
    std::string tag = toLowerStr(ev::toUtf8(tagVal));
    if (s_registry.find(tag) == s_registry.end())
        return ev::throwTypeError("Illegal constructor");

    dom::Document* doc = currentHostDocument();
    if (!doc) return ev::throwError("new <custom element>: the engine has no document");
    dom::Element* el = doc->createElement(tag);
    if (!el) return ev::throwError("new <custom element>: the document refused <" + tag + ">");

    // The object `new` produced becomes this element's wrapper, so the two are
    // one identity from here on: `document.querySelector(tag) === inst` once
    // it is appended, and every Element member resolves through the node id.
    HostNodeState* st = hostNodeStateFor(el);
    noteHostElementValue(el, self.get());
    if (st) {
        ev::setProperty(self.get(), "__bro_node_id__",
                        ev::fromDouble(static_cast<double>(reinterpret_cast<uintptr_t>(st))));
    }
    ev::setProperty(self.get(), "nodeType", ev::fromDouble(1));
    ev::setProperty(self.get(), "tagName", ev::fromUtf8(el->tagName()));
    ev::setProperty(self.get(), "nodeName", ev::fromUtf8(el->tagName()));
    ev::setProperty(self.get(), kUpgradedMarker, ev::fromBool(true));
    return self.get();
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

        // Stamp the tag on the class's prototype so `new MyElement()` can find
        // it again through the receiver (constructCustomElementBase).
        Value proto = ev::getProperty(a[1], "prototype");
        if (ev::isObject(proto)) {
            ev::setProperty(proto, kTagMarker, ev::fromUtf8(name));
        }

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
