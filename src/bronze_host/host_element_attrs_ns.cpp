// The namespaced attribute methods (DOM §4.9): setAttributeNS,
// getAttributeNS, hasAttributeNS, removeAttributeNS.
//
// The element keeps its attributes by qualified name (`xlink:href`, as the
// parser leaves an SVG one), so an attribute's namespace is its prefix's:
// none for an unprefixed name, the XLink / XML / XMLNS namespaces for those
// prefixes. Another prefix (`setAttributeNS('urn:x', 'x:a', v)`) stands for
// whatever namespace a lookup names, so long as it names one: the element
// does not record which. A lookup by (namespace, local name) matches the
// attribute whose local part and namespace both match, whatever its prefix.

#include "bronze_host/host_builder.h"
#include "bronze_host/host_element.h"
#include "bronze_host/host_globals_internal.h"
#include "bronze_host/host_node.h"
#include "bronze_host/host_values.h"

#include "dom/element.h"

#include <string>

namespace bro::bronze_host {

namespace {

constexpr const char* kXlinkNs = "http://www.w3.org/1999/xlink";
constexpr const char* kXmlNs = "http://www.w3.org/XML/1998/namespace";
constexpr const char* kXmlnsNs = "http://www.w3.org/2000/xmlns/";

// Whether the attribute named `qname` is in namespace `ns` ("" for none).
bool attributeInNamespace(const std::string& qname, const std::string& ns) {
    const size_t colon = qname.find(':');
    if (colon == std::string::npos) return ns == (qname == "xmlns" ? kXmlnsNs : "");
    const std::string prefix = qname.substr(0, colon);
    if (prefix == "xlink") return ns == kXlinkNs;
    if (prefix == "xml") return ns == kXmlNs;
    if (prefix == "xmlns") return ns == kXmlnsNs;
    return !ns.empty();
}

std::string localPart(const std::string& qname) {
    const size_t colon = qname.find(':');
    return colon == std::string::npos ? qname : qname.substr(colon + 1);
}

// The qualified name of `el`'s attribute (ns, local), or "" for none.
std::string findAttribute(const dom::Element* el, const std::string& ns, const std::string& local) {
    for (const auto& [name, value] : el->attributes()) {
        (void)value;
        if (localPart(name) == local && attributeInNamespace(name, ns)) return name;
    }
    if (ns.empty() && local == "style" && el->hasAttribute("style")) return "style";
    return {};
}

std::string nsArg(std::span<const Value> a, size_t i) {
    Value v = argAt(a, i);
    return ev::isNull(v) || ev::isUndefined(v) ? std::string() : ev::toUtf8(v);
}

}  // namespace

void decorateElementNamespacedAttributes(ObjectBuilder& b) {
    b.def("setAttributeNS", 3, [](Value self_, std::span<const Value> a) -> Value {
        HostNodeState* st = hostNodeStateOfValue(self_);
        if (!st || !st->el) return ev::undefined();
        const std::string ns = nsArg(a, 0);
        const std::string qname = ev::toUtf8(argAt(a, 1));
        const std::string value = hostNullableString(argAt(a, 2));
        const size_t colon = qname.find(':');
        if (qname.empty() || (colon != std::string::npos && ns.empty())) {
            return ev::throwValue(hostMakeDomError(
                "NamespaceError", "setAttributeNS: '" + qname + "' has a prefix and no namespace"));
        }
        // The attribute (ns, local) is replaced whatever prefix it had.
        const std::string existing = findAttribute(st->el, ns, localPart(qname));
        const bool had = !existing.empty();
        const std::string oldVal = had ? st->el->getAttribute(existing) : std::string();
        if (had && existing != qname) st->el->removeAttribute(existing);
        st->el->setAttribute(had ? existing : qname, value);
        onCustomElementAttributeChanged(st->el, localPart(qname), had ? oldVal.c_str() : nullptr, value.c_str());
        return ev::undefined();
    });
    b.def("getAttributeNS", 2, [](Value self_, std::span<const Value> a) -> Value {
        HostNodeState* st = hostNodeStateOfValue(self_);
        if (!st || !st->el) return ev::null();
        const std::string name = findAttribute(st->el, nsArg(a, 0), ev::toUtf8(argAt(a, 1)));
        return name.empty() ? ev::null() : ev::fromUtf8(st->el->getAttribute(name));
    });
    b.def("hasAttributeNS", 2, [](Value self_, std::span<const Value> a) -> Value {
        HostNodeState* st = hostNodeStateOfValue(self_);
        if (!st || !st->el) return ev::fromBool(false);
        return ev::fromBool(!findAttribute(st->el, nsArg(a, 0), ev::toUtf8(argAt(a, 1))).empty());
    });
    b.def("removeAttributeNS", 2, [](Value self_, std::span<const Value> a) -> Value {
        HostNodeState* st = hostNodeStateOfValue(self_);
        if (!st || !st->el) return ev::undefined();
        const std::string name = findAttribute(st->el, nsArg(a, 0), ev::toUtf8(argAt(a, 1)));
        if (name.empty()) return ev::undefined();
        const std::string oldVal = st->el->getAttribute(name);
        st->el->removeAttribute(name);
        onCustomElementAttributeChanged(st->el, localPart(name), oldVal.c_str(), nullptr);
        return ev::undefined();
    });
}

}  // namespace bro::bronze_host
