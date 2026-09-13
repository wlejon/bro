#include "bronze_host/host_shadow_dom.h"
#include "bronze_host/bronze_host.h"
#include "bronze_host/host_html_interfaces.h"
#include "bronze_host/gl_internal.h"
#include "dom/shadow_root.h"
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
    b.accessor("nodeType", [](Value, std::span<const Value>) -> Value {
        return ev::fromDouble(11);
    }, nullptr);

    b.accessor("nodeName", [](Value, std::span<const Value>) -> Value {
        return ev::fromUtf8("#shadow-root");
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
            return ev::undefined();
        });

    b.def("querySelector", 1, [](Value self_, std::span<const Value> a) -> Value {
        HostNodeState* st = hostNodeStateOfValue(self_);
        if (!st || !st->shadowRoot) return ev::undefined();
        Value selV = argAt(a, 0);
        if (ev::isObject(selV) || ev::isUndefined(selV)) return ev::null();
        return hostElementValue(st->shadowRoot->querySelector(ev::toUtf8(selV)));
    });

    b.def("querySelectorAll", 1, [](Value self_, std::span<const Value> a) -> Value {
        HostNodeState* st = hostNodeStateOfValue(self_);
        if (!st || !st->shadowRoot) return ev::undefined();
        Value selV = argAt(a, 0);
        if (ev::isObject(selV) || ev::isUndefined(selV)) {
            return hostArrayOf(0, [](size_t) { return ev::undefined(); });
        }
        std::vector<dom::Element*> found = st->shadowRoot->querySelectorAll(ev::toUtf8(selV));
        return hostArrayOf(found.size(), [&found](size_t i) {
            return hostElementValue(found[i]);
        });
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
        std::vector<dom::Element*> kids;
        for (dom::Node* n : st->shadowRoot->childNodes()) {
            if (n->nodeType() == dom::NodeType::Element) {
                kids.push_back(static_cast<dom::Element*>(n));
            }
        }
        return hostArrayOf(kids.size(), [&kids](size_t i) {
            return hostElementValue(kids[i]);
        });
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
            for (auto* child : st->shadowRoot->childNodes()) {
                child->setParent(nullptr);
                if (doc) doc->freeNode(child);
            }
            st->shadowRoot->childNodes().clear();
            st->shadowRoot->invalidateSlots();
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

}  // namespace

const HostClass& shadowRootHostClass() {
    return g_shadowRootClass;
}

void installShadowRootClass() {
    static bool s_installed = false;
    if (s_installed) return;
    s_installed = true;

    g_shadowRootClass.install("ShadowRoot", 0, illegalConstructor, decorateShadowRootProto);
    g_shadowRootClass.inherit(nodeHostClass());

    ev::registerGlobal("ShadowRoot", g_shadowRootClass.constructor());
    ev::GlobalValue gt = ev::globalValue("globalThis");
    if (gt.found && !gt.value.isUndefined() && ev::isObject(gt.value)) {
        ev::setProperty(gt.value, "ShadowRoot", g_shadowRootClass.constructor());
    }
}

Value hostShadowRootValue(dom::ShadowRoot* sr) {
    if (!sr) return ev::null();
    HostNodeState* st = hostNodeStateFor(sr);
    if (!st->shadowRoot) st->shadowRoot = sr;
    Value existing = st->jsObj.get();
    if (!ev::isUndefined(existing)) return existing;

    Value v = shadowRootHostClass().make(st, [](void*) {});
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
}

}  // namespace bro::bronze_host
