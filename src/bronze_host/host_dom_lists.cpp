// The DOM's list interfaces and list objects (host_dom_lists.h).
//
// The prototypes carry what the web puts there: NodeList's item / forEach /
// entries / keys / values / [Symbol.iterator], HTMLCollection's item /
// namedItem / [Symbol.iterator]. The iteration members are Array.prototype's,
// generic over anything with a length and indices (host_list_iteration.h), so
// they serve a static list and a live one alike, and item / namedItem are
// written over `this` the same way, so `NodeList.prototype.item.call(list, 0)`
// works on either.

#include "bronze_host/host_dom_lists.h"
#include "bronze_host/host_builder.h"
#include "bronze_host/host_element.h"
#include "bronze_host/host_list_iteration.h"
#include "bronze_host/host_node.h"
#include "bronze_host/host_node_sweep.h"
#include "bronze_host/host_proxy.h"
#include "bronze_host/host_values.h"
#include "dom/element.h"
#include "dom/node.h"

#include <memory>
#include <string>

namespace bro::bronze_host {

namespace {

HostClass g_nodeListClass;
HostClass g_htmlCollectionClass;
HostClass g_htmlOptionsCollectionClass;
HostClass g_htmlFormControlsCollectionClass;
HostClass g_domTokenListClass;
HostClass g_domStringMapClass;
HostClass g_mediaQueryListClass;

Value illegalListConstructor(Value, std::span<const Value>) {
    return ev::throwTypeError("Illegal constructor");
}

// A canonical array index ("0", "17"; not "01", "-1", "1.5").
bool indexKey(const std::string& key, size_t& out) {
    if (key.empty() || key.size() > 9) return false;
    if (key.size() > 1 && key[0] == '0') return false;
    size_t v = 0;
    for (char c : key) {
        if (c < '0' || c > '9') return false;
        v = v * 10 + static_cast<size_t>(c - '0');
    }
    out = v;
    return true;
}

uint32_t listLength(Value self) {
    uint32_t n = 0;
    lengthWithin(ev::toDouble(ev::getProperty(self, "length")), kMaxHostListLength, n);
    return n;
}

// item(index): the node at `index`, or null.
Value listItem(Value self, std::span<const Value> a) {
    const double d = ev::toDouble(argAt(a, 0));
    if (!(d >= 0) || d >= static_cast<double>(listLength(self))) return ev::null();
    Value v = ev::getProperty(self, std::to_string(static_cast<uint32_t>(d)));
    return ev::isUndefined(v) ? ev::null() : v;
}

// An element's own name for a collection's named properties: its id, or (an
// HTML element's) name attribute.
bool elementNamed(const dom::Element* el, const std::string& name) {
    if (!el || name.empty()) return false;
    if (el->getAttribute("id") == name) return true;
    return el->ns() == dom::Element::Namespace::HTML && el->getAttribute("name") == name;
}

// namedItem(name): the first element whose id, or whose name, is `name`.
Value listNamedItem(Value self, std::span<const Value> a) {
    if (a.empty() || ev::isUndefined(a[0]) || ev::isObject(a[0])) return ev::null();
    const std::string name = ev::toUtf8(a[0]);
    const uint32_t n = listLength(self);
    for (uint32_t i = 0; i < n; ++i) {
        dom::Node* node = hostNodeOf(ev::getElement(self, i));
        if (node && node->nodeType() == dom::NodeType::Element &&
            elementNamed(static_cast<dom::Element*>(node), name)) {
            return hostNodeValue(node);
        }
    }
    return ev::null();
}

void decorateNodeListProto(ObjectBuilder& b) {
    b.def("item", 1, listItem);
    installArrayLikeIteration(b.get(), /*withForEach=*/true);
}

void decorateCollectionProto(ObjectBuilder& b) {
    b.def("item", 1, listItem);
    b.def("namedItem", 1, listNamedItem);
    installArrayLikeIteration(b.get(), /*withForEach=*/false);
}

std::vector<std::string> indexKeys(size_t n) {
    std::vector<std::string> keys;
    keys.reserve(n);
    for (size_t i = 0; i < n; ++i) keys.push_back(std::to_string(i));
    return keys;
}

const HostClass& collectionClass(CollectionKind kind) {
    switch (kind) {
        case CollectionKind::Options: return g_htmlOptionsCollectionClass;
        case CollectionKind::FormControls: return g_htmlFormControlsCollectionClass;
        case CollectionKind::HTMLCollection: break;
    }
    return g_htmlCollectionClass;
}

}  // namespace

void installDomListInterfaces() {
    g_nodeListClass.install("NodeList", 0, illegalListConstructor, decorateNodeListProto);
    g_htmlCollectionClass.install("HTMLCollection", 0, illegalListConstructor, decorateCollectionProto);
    g_htmlOptionsCollectionClass.install("HTMLOptionsCollection", 0, illegalListConstructor, nullptr);
    g_htmlOptionsCollectionClass.inherit(g_htmlCollectionClass);
    g_htmlFormControlsCollectionClass.install("HTMLFormControlsCollection", 0, illegalListConstructor,
                                              nullptr);
    g_htmlFormControlsCollectionClass.inherit(g_htmlCollectionClass);
    g_domTokenListClass.install("DOMTokenList", 0, illegalListConstructor, nullptr);
    g_domStringMapClass.install("DOMStringMap", 0, illegalListConstructor, nullptr);
    g_mediaQueryListClass.install("MediaQueryList", 0, illegalListConstructor, nullptr);
}

Value domTokenListProto() { return g_domTokenListClass.prototype(); }
Value domStringMapProto() { return g_domStringMapClass.prototype(); }
Value mediaQueryListProto() { return g_mediaQueryListClass.prototype(); }

Value makeStaticNodeList(const std::vector<dom::Element*>& nodes) {
    ev::Persistent list(ev::createObject());
    Value proto = g_nodeListClass.prototype();
    if (ev::isObject(proto)) ev::setPrototype(list.get(), proto);
    for (size_t i = 0; i < nodes.size(); ++i) {
        Value v = hostElementValue(nodes[i]);
        list.set(ev::setElement(list.get(), static_cast<uint32_t>(i), v));
    }
    ev::PropertyAttributes lengthAttrs{false, false, true};
    list.set(ev::defineProperty(list.get(), "length", ev::fromDouble(static_cast<double>(nodes.size())),
                                lengthAttrs));
    return list.get();
}

Value makeLiveNodeList(std::function<std::vector<dom::Node*>()> items) {
    auto src = std::make_shared<std::function<std::vector<dom::Node*>()>>(std::move(items));
    HostProxyTraps traps;
    traps.proto = g_nodeListClass.prototype();
    traps.get = [src](const std::string& key, Value& out) -> bool {
        if (key == "length") {
            out = ev::fromDouble(static_cast<double>((*src)().size()));
            return true;
        }
        size_t idx = 0;
        if (!indexKey(key, idx)) return false;
        std::vector<dom::Node*> nodes = (*src)();
        out = idx < nodes.size() ? hostNodeValue(nodes[idx]) : ev::undefined();
        return true;
    };
    traps.has = [src](const std::string& key) -> bool {
        if (key == "length") return true;
        size_t idx = 0;
        return indexKey(key, idx) && idx < (*src)().size();
    };
    traps.ownKeys = [src]() { return indexKeys((*src)().size()); };
    return makeHostProxy(std::move(traps));
}

Value makeLiveCollection(std::function<std::vector<dom::Element*>()> items, CollectionKind kind) {
    auto src = std::make_shared<std::function<std::vector<dom::Element*>()>>(std::move(items));
    HostProxyTraps traps;
    traps.proto = collectionClass(kind).prototype();
    traps.get = [src](const std::string& key, Value& out) -> bool {
        if (key == "length") {
            out = ev::fromDouble(static_cast<double>((*src)().size()));
            return true;
        }
        std::vector<dom::Element*> els = (*src)();
        size_t idx = 0;
        if (indexKey(key, idx)) {
            out = idx < els.size() ? hostElementValue(els[idx]) : ev::undefined();
            return true;
        }
        for (dom::Element* el : els) {
            if (elementNamed(el, key)) {
                out = hostElementValue(el);
                return true;
            }
        }
        return false;
    };
    traps.has = [src](const std::string& key) -> bool {
        if (key == "length") return true;
        std::vector<dom::Element*> els = (*src)();
        size_t idx = 0;
        if (indexKey(key, idx)) return idx < els.size();
        for (dom::Element* el : els)
            if (elementNamed(el, key)) return true;
        return false;
    };
    traps.ownKeys = [src]() { return indexKeys((*src)().size()); };
    if (kind == CollectionKind::Options) {
        // HTMLOptionsCollection's length setter: the options past the new
        // length leave their parents.
        traps.set = [src](const std::string& key, Value v) {
            if (key != "length") return;
            const double d = ev::toDouble(v);
            if (!(d >= 0)) return;
            std::vector<dom::Element*> els = (*src)();
            for (size_t i = els.size(); i-- > static_cast<size_t>(d);) {
                if (dom::Node* p = els[i]->parentNode()) p->removeChild(els[i]);
            }
        };
    }
    return makeHostProxy(std::move(traps));
}

Value hostChildNodesList(HostNodeState* st) {
    if (!st || !st->node) return ev::undefined();
    if (ev::isUndefined(st->childNodesObj.get())) {
        StateRef ref(st);
        st->childNodesObj.set(makeLiveNodeList([ref]() -> std::vector<dom::Node*> {
            if (!ref->node) return {};
            return ref->node->childNodes();
        }));
    }
    return st->childNodesObj.get();
}

Value hostChildrenCollection(HostNodeState* st) {
    if (!st || !st->node) return ev::undefined();
    if (ev::isUndefined(st->childrenObj.get())) {
        StateRef ref(st);
        st->childrenObj.set(makeLiveCollection([ref]() -> std::vector<dom::Element*> {
            std::vector<dom::Element*> out;
            if (!ref->node) return out;
            for (dom::Node* n : ref->node->childNodes())
                if (n->nodeType() == dom::NodeType::Element) out.push_back(static_cast<dom::Element*>(n));
            return out;
        }));
    }
    return st->childrenObj.get();
}

}  // namespace bro::bronze_host
