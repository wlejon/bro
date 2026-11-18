#include "bronze_host/host_shadow_dom.h"
#include "bronze_host/host_dom_lists.h"
#include "bronze_host/bronze_host.h"
#include "bronze_host/host_compartment.h"
#include "bronze_host/host_html_interfaces.h"
#include "bronze_host/host_builder.h"
#include "bronze_host/host_values.h"
#include "bronze_host/host_globals_internal.h"
#include "bronze_host/host_node_sweep.h"
#include "bronze_host/host_events.h"
#include "bronze_host/host_runtime.h"
#include "dom/event.h"
#include "dom/node_handle.h"
#include "dom/shadow_root.h"
#include "engine/engine.h"
#include "dom/element.h"
#include "dom/document.h"
#include "dom/text_node.h"

#include <span>
#include <string>
#include <vector>

namespace bro::bronze_host {

namespace {

HostClass g_shadowRootClass;

Value illegalConstructor(Value, std::span<const Value>) {
    return ev::throwTypeError("Illegal constructor");
}

void decorateShadowRootProto(ObjectBuilder& b) {
    // An EventTarget on the composed path: listeners here hear events from
    // inside the shadow tree (and slotchange), dispatchEvent targets the root.
    installElementEventTarget(b, nullptr, "ShadowRoot");

    b.accessor("nodeType", [](Value, std::span<const Value>) -> Value {
        return ev::fromDouble(11);
    }, nullptr);

    // A ShadowRoot is a DocumentFragment, and every DocumentFragment reports
    // "#document-fragment" — that is what nodeName means for node type 11 and
    // what a serializer or a node-kind switch is written against.
    // "#shadow-root" is bro's own INTERNAL discriminator (dom::ShadowRoot::
    // nodeName, which the wrapper registry keys on) and was never a web-visible
    // name.
    b.accessor("nodeName", [](Value, std::span<const Value>) -> Value {
        return ev::fromUtf8("#document-fragment");
    }, nullptr);

    b.accessor("mode", [](Value self_, std::span<const Value>) -> Value {
        HostNodeState* st = hostNodeStateOfValue(self_);
        if (!st || !st->shadowRoot) return ev::undefined();
        return ev::fromUtf8(st->shadowRoot->modeString());
    }, nullptr);

    b.accessor("host", [](Value self_, std::span<const Value>) -> Value {
        HostNodeState* st = hostNodeStateOfValue(self_);
        if (!st || !st->shadowRoot) return ev::undefined();
        return hostElementValue(st->shadowRoot->host());
    }, nullptr);

    b.accessor("innerHTML",
        [](Value self_, std::span<const Value>) -> Value {
            HostNodeState* st = hostNodeStateOfValue(self_);
            if (!st || !st->shadowRoot) return ev::undefined();
            return ev::fromUtf8(st->shadowRoot->innerHTML());
        },
        [](Value self_, std::span<const Value> a) -> Value {
            HostNodeState* st = hostNodeStateOfValue(self_);
            if (!st || !st->shadowRoot) return ev::undefined();
            Value v = argAt(a, 0);
            std::string html = (ev::isUndefined(v) || ev::isNull(v)) ? "" : ev::toUtf8(v);
            dom::Document* doc = st->shadowRoot->document();
            if (!doc && st->shadowRoot->host()) doc = st->shadowRoot->host()->document();
            if (!doc) doc = currentHostDocument();
            st->shadowRoot->setInnerHTML(html, doc);
            upgradeCustomElementsInSubtree(st->shadowRoot);
            return ev::undefined();
        });

    b.def("querySelector", 1, [](Value self_, std::span<const Value> a) -> Value {
        HostNodeState* st = hostNodeStateOfValue(self_);
        if (!st || !st->shadowRoot) return ev::undefined();
        Value selV = argAt(a, 0);
        if (ev::isObject(selV) || ev::isUndefined(selV)) return ev::null();
        std::string sel = ev::toUtf8(selV);
        if (!dom::Element::isValidSelector(sel)) {
            return ev::throwValue(hostMakeDomError("SyntaxError", "'" + sel + "' is not a valid selector"));
        }
        return hostElementValue(st->shadowRoot->querySelector(sel));
    });

    b.def("querySelectorAll", 1, [](Value self_, std::span<const Value> a) -> Value {
        HostNodeState* st = hostNodeStateOfValue(self_);
        if (!st || !st->shadowRoot) return ev::undefined();
        Value selV = argAt(a, 0);
        if (ev::isObject(selV) || ev::isUndefined(selV)) return makeStaticNodeList({});
        std::string sel = ev::toUtf8(selV);
        if (!dom::Element::isValidSelector(sel)) {
            return ev::throwValue(hostMakeDomError("SyntaxError", "'" + sel + "' is not a valid selector"));
        }
        return makeStaticNodeList(st->shadowRoot->querySelectorAll(sel));
    });

    b.def("getElementById", 1, [](Value self_, std::span<const Value> a) -> Value {
        HostNodeState* st = hostNodeStateOfValue(self_);
        if (!st || !st->shadowRoot) return ev::undefined();
        Value idV = argAt(a, 0);
        if (ev::isObject(idV) || ev::isUndefined(idV)) return ev::null();
        return hostElementValue(st->shadowRoot->getElementById(ev::toUtf8(idV)));
    });

    b.accessor("children", [](Value self_, std::span<const Value>) -> Value {
        HostNodeState* st = hostNodeStateOfValue(self_);
        if (!st || !st->shadowRoot) return ev::undefined();
        return hostChildrenCollection(st);
    }, nullptr);

    b.accessor("firstElementChild", [](Value self_, std::span<const Value>) -> Value {
        HostNodeState* st = hostNodeStateOfValue(self_);
        if (!st || !st->shadowRoot) return ev::null();
        for (dom::Node* n : st->shadowRoot->childNodes()) {
            if (n->nodeType() == dom::NodeType::Element) {
                return hostElementValue(static_cast<dom::Element*>(n));
            }
        }
        return ev::null();
    }, nullptr);

    b.accessor("lastElementChild", [](Value self_, std::span<const Value>) -> Value {
        HostNodeState* st = hostNodeStateOfValue(self_);
        if (!st || !st->shadowRoot) return ev::null();
        const auto& kids = st->shadowRoot->childNodes();
        for (auto it = kids.rbegin(); it != kids.rend(); ++it) {
            if ((*it)->nodeType() == dom::NodeType::Element) {
                return hostElementValue(static_cast<dom::Element*>(*it));
            }
        }
        return ev::null();
    }, nullptr);

    b.accessor("childElementCount", [](Value self_, std::span<const Value>) -> Value {
        HostNodeState* st = hostNodeStateOfValue(self_);
        if (!st || !st->shadowRoot) return ev::fromDouble(0);
        size_t count = 0;
        for (dom::Node* n : st->shadowRoot->childNodes()) {
            if (n->nodeType() == dom::NodeType::Element) ++count;
        }
        return ev::fromDouble(count);
    }, nullptr);

    b.accessor("textContent",
        [](Value self_, std::span<const Value>) -> Value {
            HostNodeState* st = hostNodeStateOfValue(self_);
            if (!st || !st->shadowRoot) return ev::undefined();
            std::string text;
            for (dom::Node* n : st->shadowRoot->childNodes()) {
                if (n->nodeType() == dom::NodeType::Text) {
                    text += static_cast<dom::TextNode*>(n)->data();
                } else if (n->nodeType() == dom::NodeType::Element) {
                    text += static_cast<dom::Element*>(n)->textContent();
                }
            }
            return ev::fromUtf8(text);
        },
        [](Value self_, std::span<const Value> a) -> Value {
            HostNodeState* st = hostNodeStateOfValue(self_);
            if (!st || !st->shadowRoot) return ev::undefined();
            dom::Document* doc = st->shadowRoot->document();
            if (!doc && st->shadowRoot->host()) doc = st->shadowRoot->host()->document();
            if (!doc) doc = currentHostDocument();
            // (Marked as ShadowRoot::setInnerHTML marks it: the layout tree
            // holds the children until it is built again.)
            const bool hadChildren = !st->shadowRoot->childNodes().empty();
            for (auto* child : st->shadowRoot->childNodes()) {
                child->setParent(nullptr);
                if (doc) doc->freeNode(child);
            }
            st->shadowRoot->childNodes().clear();
            st->shadowRoot->invalidateSlots();
            if (hadChildren && st->shadowRoot->host()) st->shadowRoot->host()->markStructureDirty();
            Value v = argAt(a, 0);
            if (!ev::isUndefined(v) && !ev::isNull(v) && doc) {
                std::string text = ev::toUtf8(v);
                if (!text.empty()) {
                    st->shadowRoot->appendChild(doc->createTextNode(text));
                }
            }
            return ev::undefined();
        });

    b.def("append", 1, [](Value self_, std::span<const Value> a) -> Value {
        HostNodeState* st = hostNodeStateOfValue(self_);
        if (!st || !st->shadowRoot) return ev::undefined();
        for (const Value& v : a) {
            if (dom::Node* child = hostNodeOf(v)) {
                hostInsertNode(st->shadowRoot, child, nullptr);
            } else if (!ev::isObject(v) && !ev::isUndefined(v)) {
                dom::Document* doc = st->shadowRoot->document();
                if (!doc && st->shadowRoot->host()) doc = st->shadowRoot->host()->document();
                if (!doc) doc = currentHostDocument();
                if (doc) {
                    st->shadowRoot->appendChild(doc->createTextNode(ev::toUtf8(v)));
                }
            }
        }
        return ev::undefined();
    });
}

// ---- slotchange --------------------------------------------------------
// DOM "signal a slot change": a slot whose assigned nodes changed gets a
// `slotchange` (bubbling, not composed) at the next microtask checkpoint.
// The tree tells us which shadow roots' assignments went stale
// (ShadowRoot::setSlotChangeHook); the hosts are held by handle until then,
// and each root compares its slots with what it last reported.
struct PendingSlotChanges {
    std::vector<dom::NodeHandle<dom::Element>> hosts;
    bool scheduled = false;
};

PendingSlotChanges& pendingSlotChanges() {
    static auto* p = new PendingSlotChanges();
    return *p;
}

void flushSlotChanges() {
    PendingSlotChanges& p = pendingSlotChanges();
    p.scheduled = false;
    std::vector<dom::NodeHandle<dom::Element>> hosts;
    hosts.swap(p.hosts);
    engine::Engine* engine = hostEngine();
    for (const auto& h : hosts) {
        dom::Element* host = h.get();
        dom::ShadowRoot* sr = host ? host->shadowRoot() : nullptr;
        if (!sr) continue;
        std::vector<dom::NodeHandle<dom::Element>> slots;
        for (dom::Element* slot : sr->takeSlotChanges()) slots.emplace_back(slot->document(), slot);
        for (const auto& handle : slots) {
            dom::Element* slot = handle.get();
            if (!slot || !engine) continue;
            dom::Event evt("slotchange", /*bubbles=*/true, /*cancelable=*/false);
            evt.setIsTrusted(true);
            evt.setComposed(false);
            engine->dispatchElementEvent(slot, evt);
        }
    }
}

void onSlotsInvalidated(dom::ShadowRoot* sr) {
    dom::Element* host = sr->host();
    if (!host || !host->document()) return;
    PendingSlotChanges& p = pendingSlotChanges();
    p.hosts.emplace_back(host->document(), host);
    if (p.scheduled) return;
    p.scheduled = true;
    // A microtask (a resolved promise's reaction) where script can run; a
    // task where none is current (a document built by the engine itself).
    if (!ev::currentRealm()) {
        postHostTask(flushSlotChanges);
        return;
    }
    ev::Persistent promise(ev::createPromise());
    ev::resolvePromise(promise.get(), ev::undefined());
    ev::Persistent then(ev::getProperty(promise.get(), "then"));
    ev::Persistent reaction(ev::makeFunction(
        [](Value, std::span<const Value>) -> Value {
            flushSlotChanges();
            return ev::undefined();
        },
        1));
    if (!ev::isFunction(then.get())) {
        postHostTask(flushSlotChanges);
        return;
    }
    Value arg = reaction.get();
    ev::CallResult r = ev::call(then.get(), promise.get(), std::span<const Value>(&arg, 1));
    if (r.thrown) postHostTask(flushSlotChanges);
}

}  // namespace

const HostClass& shadowRootHostClass() {
    return g_shadowRootClass;
}

void installShadowRootClass() {
    if (!firstInRealm(&g_shadowRootClass)) return;

    dom::ShadowRoot::setSlotChangeHook(&onSlotsInvalidated);
    g_shadowRootClass.install("ShadowRoot", 0, illegalConstructor, decorateShadowRootProto);
    g_shadowRootClass.inherit(nodeHostClass());

    ev::registerGlobal("ShadowRoot", g_shadowRootClass.constructor());
}

Value hostShadowRootValue(dom::ShadowRoot* sr) {
    if (!sr) return ev::null();
    HostNodeState* st = hostNodeStateFor(sr);
    if (!st->shadowRoot) st->shadowRoot = sr;
    Value existing = st->jsObj.get();
    if (!ev::isUndefined(existing)) return existing;

    Value v = shadowRootHostClass().make(st, stateHandleDtor(st));
    st->jsObj.set(v);
    return v;
}

void decorateElementShadow(ObjectBuilder& b) {
    b.def("attachShadow", 1, [](Value self_, std::span<const Value> a) -> Value {
        dom::Element* el = hostElementOf(self_);
        if (!el) {
            return ev::throwTypeError("attachShadow: receiver is not an Element");
        }
        Value options = argAt(a, 0);
        if (!ev::isObject(options)) {
            return ev::throwTypeError("attachShadow: options must be an object");
        }
        Value modeVal = ev::getProperty(options, "mode");
        if (!ev::isString(modeVal)) {
            return ev::throwTypeError("attachShadow: mode must be 'open' or 'closed'");
        }
        std::string mode = ev::toUtf8(modeVal);
        if (mode != "open" && mode != "closed") {
            return ev::throwTypeError("attachShadow: mode must be 'open' or 'closed'");
        }
        if (el->shadowRoot()) {
            return ev::throwError("Shadow root already exists");
        }
        dom::ShadowRoot::Mode srMode = (mode == "closed")
            ? dom::ShadowRoot::Mode::Closed
            : dom::ShadowRoot::Mode::Open;
        dom::ShadowRoot* sr = el->attachShadow(srMode);
        if (!sr) {
            return ev::throwError("Failed to attach shadow root");
        }
        return hostShadowRootValue(sr);
    });

    b.accessor("shadowRoot",
               [](Value self_, std::span<const Value>) -> Value {
                   dom::Element* el = hostElementOf(self_);
                   if (!el) return ev::undefined();
                   dom::ShadowRoot* sr = el->shadowRoot();
                   if (sr && sr->mode() == dom::ShadowRoot::Mode::Open) {
                       return hostShadowRootValue(sr);
                   }
                   return ev::null();
               },
               nullptr);

    // ---- the slot API ------------------------------------------------------
    // `slot` is the plain reflection of the content attribute — the thing a
    // component's own markup writes to choose which <slot> it lands in.
    b.accessor("slot",
               [](Value self_, std::span<const Value>) -> Value {
                   dom::Element* el = hostElementOf(self_);
                   if (!el) return ev::fromUtf8("");
                   return ev::fromUtf8(el->getAttribute("slot"));
               },
               [](Value self_, std::span<const Value> a) -> Value {
                   dom::Element* el = hostElementOf(self_);
                   Value v = argAt(a, 0);
                   if (el && !ev::isObject(v)) el->setAttribute("slot", ev::toUtf8(v));
                   return ev::undefined();
               });

    // The other direction: which <slot> in the PARENT's shadow tree this light
    // DOM child was distributed into. Null for anything not slotted.
    b.accessor("assignedSlot",
               [](Value self_, std::span<const Value>) -> Value {
                   dom::Element* el = hostElementOf(self_);
                   if (!el) return ev::null();
                   dom::Element* parent = el->parentElement();
                   if (!parent || !parent->hasShadow()) return ev::null();
                   dom::ShadowRoot* sr = parent->shadowRoot();
                   if (!sr) return ev::null();
                   return hostElementValue(sr->assignedSlot(el));
               },
               nullptr);

    // And what a <slot> received. `assignedNodes` includes text, which is what
    // a component checks to find out whether it was given anything at all;
    // `assignedElements` is the element-only view.
    auto defAssigned = [&b](const char* name, bool elementsOnly) {
        b.def(name, 1, [elementsOnly](Value self_, std::span<const Value>) -> Value {
            dom::Element* el = hostElementOf(self_);
            auto empty = [] { return hostArrayOf(0, [](size_t) { return ev::undefined(); }); };
            if (!el) return empty();
            const std::string& tag = el->tagName();
            if (tag != "SLOT" && tag != "slot") return empty();
            dom::ShadowRoot* sr = el->containingShadowRoot();
            if (!sr) return empty();
            std::vector<dom::Node*> nodes = sr->assignedNodes(el);
            if (elementsOnly) {
                std::vector<dom::Element*> els;
                for (dom::Node* n : nodes) {
                    if (n->nodeType() == dom::NodeType::Element)
                        els.push_back(static_cast<dom::Element*>(n));
                }
                return hostArrayOf(els.size(),
                                   [&els](size_t i) { return hostElementValue(els[i]); });
            }
            return hostArrayOf(nodes.size(),
                               [&nodes](size_t i) { return hostNodeValue(nodes[i]); });
        });
    };
    defAssigned("assignedNodes", false);
    defAssigned("assignedElements", true);
}

}  // namespace bro::bronze_host
