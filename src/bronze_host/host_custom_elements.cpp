#include "bronze_host/host_globals_internal.h"
#include "bronze_host/host_class.h"
#include "bronze_host/host_runtime.h"
#include "bronze_host/host_values.h"
#include "bronze_host/host_node.h"
#include "bronze_host/host_element.h"
#include "bronze_host/host_builder.h"
#include "bronze_host/host_realm_scope.h"
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

// One registry per realm scope (host_realm_scope.h): a window's
// CustomElementRegistry is its own, so a frame defining <x-row> neither
// defines it for its parent nor collides with the parent's <x-row>, and a
// constructor one frame registered is never run against another's elements.
// Dropped with the scope (clearCustomElementsForScope).
using Registry = std::unordered_map<std::string, CustomElementDef>;
static std::unordered_map<uint64_t, Registry> s_registries;
static std::unordered_map<uint64_t, std::unordered_map<std::string, std::vector<ev::Persistent>>>
    s_pendingWhenDefined;
static thread_local dom::Element* s_activeConstructingElement = nullptr;
// The constructor being run, by its definition's root: it is read again from
// inside the construct call (constructCustomElementBase), after user code has
// allocated, so a raw Value copy here would be stale by then.
static thread_local const ev::Persistent* s_activeCtor = nullptr;

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

// The registry of the realm the script now running belongs to.
static Registry& currentRegistry() { return s_registries[currentRealmScope()]; }

// The registry of the window `el` lives in, or null when it has none: an
// element is upgraded by its own document's definitions, whichever realm's
// script inserted it.
static Registry* registryFor(dom::Element* el) {
    if (s_registries.empty()) return nullptr;
    dom::Document* doc = el->document();
    auto it = s_registries.find(doc ? scopeIdForDocument(doc) : currentRealmScope());
    return it == s_registries.end() ? nullptr : &it->second;
}

static const CustomElementDef* findDefinition(dom::Element* el, const std::string& lowerTag) {
    Registry* reg = registryFor(el);
    if (!reg) return nullptr;
    auto it = reg->find(lowerTag);
    return it == reg->end() ? nullptr : &it->second;
}

static const CustomElementDef* definitionFor(dom::Element* el) {
    const std::string& tag = el->tagName();
    if (tag.find('-') == std::string::npos) return nullptr;
    return findDefinition(el, toLowerStr(tag));
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
    // Rooted across the callback read, which may run a getter.
    ev::Persistent elVal(hostElementValue(el));
    Value cb = ev::getProperty(elVal.get(), name);
    if (ev::isFunction(cb)) {
        ev::CallResult r = ev::call(cb, elVal.get(), {});
        if (r.thrown) {
            // thrownValueText: an Error is an object, and toUtf8 of an
            // object is a hard error in the embed API.
            LOG_WARN("customElements: <%s> %s threw: %s", toLowerStr(el->tagName()).c_str(), name,
                     thrownValueText(r.value).c_str());
        }
    }
}

// Run the class constructor on an element that was parsed rather than
// created. The constructor's super() lands on the element's existing
// wrapper (constructCustomElementBase), so any reference the app already
// holds keeps its identity and gains the class prototype.
static ev::CallResult runCustomElementConstructor(dom::Element* el, const CustomElementDef& def) {
    s_activeConstructingElement = el;
    s_activeCtor = &def.ctor;
    ev::CallResult res = ev::construct(def.ctor.get(), {});
    s_activeConstructingElement = nullptr;
    s_activeCtor = nullptr;
    if (res.thrown) return res;
    if (ev::isObject(res.value)) {
        // Rooted: setProperty and fromUtf8 allocate, and `res.value` is
        // what the caller reads back afterwards.
        ev::Persistent customVal(res.value);
        noteHostElementValue(el, customVal.get());
        HostNodeState* st = hostNodeStateFor(el);
        if (st) {
            ev::setProperty(customVal.get(), "__bro_node_id__",
                            ev::fromDouble(static_cast<double>(reinterpret_cast<uintptr_t>(st))));
        }
        ev::setProperty(customVal.get(), "nodeType", ev::fromDouble(1));
        ev::Persistent tagName(ev::fromUtf8(el->domTagName()));
        ev::setProperty(customVal.get(), "tagName", tagName.get());
        ev::setProperty(customVal.get(), "nodeName", tagName.get());
        ev::setProperty(customVal.get(), kUpgradedMarker, ev::fromBool(true));
        res.value = customVal.get();
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
                 thrownValueText(res.value).c_str());
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
    if (s_registries.empty()) return;
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
    if (!el || s_registries.empty()) return;
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
    const CustomElementDef* found = findDefinition(el, toLowerStr(el->tagName()));
    if (!found) return;
    const auto& def = *found;
    bool observed = false;
    for (const auto& attr : def.observedAttributes) {
        if (attr == name) { observed = true; break; }
    }
    if (!observed) return;

    // Each fromUtf8 allocates, so the element, the callback and the earlier
    // strings are rooted before the next one is made.
    ev::Persistent elVal(hostElementValue(el));
    ev::Persistent cb(ev::getProperty(elVal.get(), "attributeChangedCallback"));
    if (ev::isFunction(cb.get())) {
        ev::Persistent nameVal(ev::fromUtf8(name));
        ev::Persistent oldVal(oldValue ? ev::fromUtf8(oldValue) : ev::null());
        ev::Persistent newVal(newValue ? ev::fromUtf8(newValue) : ev::null());
        const Value args[3] = { nameVal.get(), oldVal.get(), newVal.get() };
        ev::call(cb.get(), elVal.get(), std::span<const Value>(args, 3));
    }
}

Value constructCustomElement(dom::Element* el, const std::string& tagName) {
    const CustomElementDef* def = findDefinition(el, toLowerStr(tagName));
    if (!def) return ev::undefined();
    ev::CallResult res = runCustomElementConstructor(el, *def);
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
        if (ev::isObject(wrapper.get()) && s_activeCtor && ev::isFunction(s_activeCtor->get())) {
            Value proto = ev::getProperty(s_activeCtor->get(), "prototype");
            if (ev::isObject(proto)) return ev::setPrototype(wrapper.get(), proto);
        }
        return wrapper.get();
    }

    if (!ev::isObject(newObject)) return ev::throwTypeError("Illegal constructor");
    ev::Persistent self(newObject);
    Value tagVal = ev::getProperty(self.get(), kTagMarker);
    if (!ev::isString(tagVal)) return ev::throwTypeError("Illegal constructor");
    std::string tag = toLowerStr(ev::toUtf8(tagVal));
    if (currentRegistry().count(tag) == 0) return ev::throwTypeError("Illegal constructor");

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
    ev::Persistent tagName(ev::fromUtf8(el->domTagName()));
    ev::setProperty(self.get(), "tagName", tagName.get());
    ev::setProperty(self.get(), "nodeName", tagName.get());
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
        if (currentRegistry().count(name) != 0) {
            return ev::throwTypeError("customElements.define: name already defined");
        }

        CustomElementDef def;
        def.tagName = name;
        def.ctor.set(a[1]);

        const Rooted obs(ev::getProperty(a[1], "observedAttributes"));
        if (ev::isObject(obs)) {
            Value lenV = ev::getProperty(obs, "length");
            if (ev::isNumber(lenV)) {
                uint32_t len = satCast<uint32_t>(ev::toDouble(lenV));
                for (uint32_t i = 0; i < len; i++) {
                    Value item = ev::getElement(obs, i);
                    if (ev::isString(item)) {
                        def.observedAttributes.push_back(ev::toUtf8(item));
                    }
                }
            }
        }
        const uint64_t scope = currentRealmScope();
        s_registries[scope][name] = std::move(def);

        // Resolve any promises this realm has awaiting this element name.
        auto& pending = s_pendingWhenDefined[scope];
        auto pendIt = pending.find(name);
        if (pendIt != pending.end()) {
            std::vector<ev::Persistent> waiting = std::move(pendIt->second);
            pending.erase(pendIt);
            for (auto& p : waiting) {
                ev::resolvePromise(p.get(), a[1]);
            }
        }

        // Stamp the tag on the class's prototype so `new MyElement()` can find
        // it again through the receiver (constructCustomElementBase).
        ev::Persistent proto(ev::getProperty(a[1], "prototype"));
        if (ev::isObject(proto.get())) {
            ev::Persistent tag(ev::fromUtf8(name));
            ev::setProperty(proto.get(), kTagMarker, tag.get());
        }

        // Elements already parsed under this name are upgraded now, in
        // document order, and those in the document get connectedCallback:
        // markup routinely precedes the script that defines its components.
        if (dom::Document* doc = currentHostDocument()) {
            const CustomElementDef& stored = s_registries[scope][name];
            for (dom::Element* el : doc->querySelectorAll(name)) {
                if (upgradeElement(el, stored) && isConnected(el)) fireLifecycle(el, "connectedCallback");
            }
        }
        return ev::undefined();
    });

    ce.def("get", 1, [](Value, std::span<const Value> a) -> Value {
        if (a.empty() || !ev::isString(a[0])) return ev::undefined();
        std::string name = toLowerStr(ev::toUtf8(a[0]));
        Registry& reg = currentRegistry();
        auto it = reg.find(name);
        return it != reg.end() ? it->second.ctor.get() : ev::undefined();
    });

    ce.def("whenDefined", 1, [](Value, std::span<const Value> a) -> Value {
        if (a.empty() || !ev::isString(a[0])) {
            return ev::throwTypeError("customElements.whenDefined requires a name string");
        }
        std::string name = toLowerStr(ev::toUtf8(a[0]));
        if (name.find('-') == std::string::npos) {
            ev::Persistent p(ev::createPromise());
            ev::Persistent err(hostMakeDomError("SyntaxError", "customElements.whenDefined: name must contain a hyphen"));
            ev::rejectPromise(p.get(), err.get());
            return p.get();
        }
        Registry& reg = currentRegistry();
        auto it = reg.find(name);
        if (it != reg.end()) {
            ev::Persistent p(ev::createPromise());
            ev::resolvePromise(p.get(), it->second.ctor.get());
            return p.get();
        }
        Value p = ev::createPromise();
        auto& waiting = s_pendingWhenDefined[currentRealmScope()][name];
        waiting.emplace_back(p);
        return waiting.back().get();
    });

    ce.def("upgrade", 1, [](Value, std::span<const Value> a) -> Value {
        HostNodeState* st = a.empty() ? nullptr : hostNodeStateOfValue(a[0]);
        dom::Node* root = st ? (st->shadowRoot ? static_cast<dom::Node*>(st->shadowRoot) : st->node) : nullptr;
        if (root) upgradeAndConnect(root, true);
        return ev::undefined();
    });

    ev::registerGlobal("customElements", ce.get());
    auto g = ev::globalValue("globalThis");
    if (g.found && ev::isObject(g.value)) {
        ev::setProperty(g.value, "customElements", ce.get());
    }
}

void clearCustomElementsForScope(uint64_t scopeId) {
    s_registries.erase(scopeId);
    s_pendingWhenDefined.erase(scopeId);
}

void resetCustomElementsRegistry() {
    s_registries.clear();
    s_pendingWhenDefined.clear();
    s_activeConstructingElement = nullptr;
    s_activeCtor = nullptr;
}

} // namespace bro::bronze_host
