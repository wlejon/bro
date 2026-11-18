// The document as a node, and the documents a page can make.
//
// A dom::Document is not a dom::Node: its children — the doctype and the
// document element — have no parentNode in the dom tree. This layer answers
// the DOM's shape over that: `document.childNodes` / `firstChild` /
// `lastChild` / `doctype`, `documentElement.parentNode === document`, the
// siblings between the two, and appendChild / insertBefore / removeChild /
// replaceChild on a document (one element, one doctype before it, as DOM §4.2.3
// pre-insertion validity has it). DocumentType nodes (`<!DOCTYPE>`) are
// dom::DocumentType.
//
// document.implementation is a DOMImplementation: createHTMLDocument(title),
// createDocument(namespace, qualifiedName, doctype) (an XMLDocument, its
// content type by namespace), createDocumentType, hasFeature. `new Document()`
// is an empty XML document; document.cloneNode(deep) a new document of the
// same kind with its doctype and element cloned.

#include "bronze_host/host_builder.h"
#include "bronze_host/host_document.h"
#include "bronze_host/host_dom_lists.h"
#include "bronze_host/host_element.h"
#include "bronze_host/host_events.h"
#include "bronze_host/host_globals_internal.h"
#include "bronze_host/host_html_interfaces.h"
#include "bronze_host/host_node.h"
#include "bronze_host/host_node_sweep.h"
#include "bronze_host/host_runtime.h"
#include "bronze_host/host_values.h"

#include "dom/document.h"
#include "dom/document_type.h"
#include "dom/element.h"
#include "engine/engine.h"

#include <span>
#include <string>
#include <vector>

namespace bro::bronze_host {

namespace {

constexpr const char* kXhtmlNs = "http://www.w3.org/1999/xhtml";
constexpr const char* kSvgNs = "http://www.w3.org/2000/svg";

Value hierarchyError(const std::string& what) {
    return ev::throwValue(hostMakeDomError("HierarchyRequestError", what));
}

std::string optionalString(Value v) {
    return (ev::isUndefined(v) || ev::isNull(v)) ? std::string() : ev::toUtf8(v);
}

// Pre-insertion validity for a document parent (DOM §4.2.3 steps 2-6): an
// element or a doctype, at most one of each, the doctype before the element.
// `replacing` is the child a replaceChild takes out; `before` the reference
// child. Returns an error message, or empty when the insertion is valid.
std::string documentInsertionError(dom::Document* doc, dom::Node* node, dom::Node* before, dom::Node* replacing) {
    dom::Element* el = doc->documentElement();
    dom::DocumentType* dt = doc->doctype();
    if (replacing == el) el = nullptr;
    if (replacing == dt) dt = nullptr;
    if (node == el) el = nullptr;
    if (node == dt) dt = nullptr;
    switch (node->nodeType()) {
        case dom::NodeType::Element:
            if (el) return "Only one element on document allowed.";
            if (before && before == doc->doctype() && !replacing) return "Can't insert an element before a doctype.";
            return {};
        case dom::NodeType::DocumentType:
            if (dt) return "Only one doctype on document allowed.";
            if (el && (!before || before != el)) return "Can't insert a doctype after an element.";
            return {};
        default:
            return "Nodes of type '" + node->nodeName() + "' may not be inserted inside nodes of type '#document'.";
    }
}

// Put `node` into `doc` as its doctype or its element, moving it out of
// wherever it was (another document included) first.
void attachToDocument(dom::Document* doc, dom::Node* node) {
    hostDetachFromDocument(node);
    if (dom::Node* p = node->parentNode()) p->removeChild(node);
    if (node->document() != doc) node = doc->adoptNode(node);
    if (node->nodeType() == dom::NodeType::Element) doc->setDocumentElement(static_cast<dom::Element*>(node));
    else doc->setDoctype(static_cast<dom::DocumentType*>(node));
}

// A document of the given kind: an HTML document parsed from markup, or an
// empty XML one with `contentType`.
dom::Document* emptyXmlDocument(const std::string& contentType) {
    dom::Document* doc = newEmptyDocument();
    doc->setXml(true);
    doc->setContentType(contentType);
    doc->setUrl("about:blank");
    return doc;
}

Value createHTMLDocument(Value, std::span<const Value> a) {
    dom::Document* doc = parseIntoNewDocument("<!DOCTYPE html><html><head></head><body></body></html>");
    doc->setUrl("about:blank");
    Value titleV = argAt(a, 0);
    if (!ev::isUndefined(titleV)) {
        dom::Element* head = doc->querySelector("head");
        dom::Element* title = doc->createElement("title");
        title->appendChild(doc->createTextNode(ev::toUtf8(titleV)));
        if (head) head->appendChild(title);
    }
    return hostDocumentValue(doc);
}

Value implementationFor(dom::Document* fixed);

}  // namespace

bool hostIsDocumentChild(const dom::Node* n) {
    if (!n || n->parentNode()) return false;
    dom::Document* doc = n->document();
    return doc && (n == doc->documentElement() || n == doc->doctype());
}

std::vector<dom::Node*> hostDocumentChildren(dom::Document* doc) {
    std::vector<dom::Node*> out;
    if (!doc) return out;
    if (doc->doctype()) out.push_back(doc->doctype());
    if (doc->documentElement()) out.push_back(doc->documentElement());
    return out;
}

void hostDetachFromDocument(dom::Node* n) {
    if (!hostIsDocumentChild(n)) return;
    dom::Document* doc = n->document();
    if (n == doc->documentElement()) doc->setDocumentElement(nullptr);
    else doc->setDoctype(nullptr);
}

Value hostDocumentObject(dom::Document* doc) {
    if (!doc) return ev::null();
    if (hostEngine() && doc == hostEngine()->document()) {
        ev::GlobalValue g = ev::globalValue("document");
        return g.found ? g.value : ev::null();
    }
    return hostDocumentValue(doc);
}

Value makeDocumentTypeValue(dom::Node* node) {
    HostNodeState* st = hostNodeStateFor(node);
    ObjectBuilder b(documentTypeHostClass().make(st, stateHandleDtor(st)));
    b.set("nodeType", ev::fromDouble(10));
    return b.get();
}

void decorateDocumentTypeProto(ObjectBuilder& b) {
    auto field = [&b](const char* name, const std::string& (dom::DocumentType::*get)() const) {
        b.accessor(name, [get](Value self_, std::span<const Value>) {
            HostNodeState* st = hostNodeStateOfValue(self_);
            if (!st || !st->node || st->node->nodeType() != dom::NodeType::DocumentType) return ev::undefined();
            return ev::fromUtf8((static_cast<dom::DocumentType*>(st->node)->*get)());
        }, nullptr);
    };
    field("name", &dom::DocumentType::name);
    field("nodeName", &dom::DocumentType::name);
    field("publicId", &dom::DocumentType::publicId);
    field("systemId", &dom::DocumentType::systemId);
    b.accessor("textContent", [](Value, std::span<const Value>) { return ev::null(); },
               [](Value, std::span<const Value>) { return ev::undefined(); });
    b.accessor("nodeValue", [](Value, std::span<const Value>) { return ev::null(); },
               [](Value, std::span<const Value>) { return ev::undefined(); });
}

void decorateDocumentNode(ObjectBuilder& b, dom::Document* fixed) {
    b.set("nodeName", ev::fromUtf8("#document"));
    b.set("nodeValue", ev::null());
    b.accessor("textContent", [](Value, std::span<const Value>) { return ev::null(); },
               [](Value, std::span<const Value>) { return ev::undefined(); });
    for (const char* name : {"parentNode", "parentElement", "ownerDocument", "previousSibling", "nextSibling"}) {
        b.accessor(name, [](Value, std::span<const Value>) { return ev::null(); }, nullptr);
    }
    b.accessor("isConnected", [](Value, std::span<const Value>) { return ev::fromBool(true); }, nullptr);
    b.accessor("doctype", [fixed](Value, std::span<const Value>) {
        dom::Document* doc = documentFor(fixed);
        return doc ? hostNodeValue(doc->doctype()) : ev::null();
    }, nullptr);
    b.accessor("contentType", [fixed](Value, std::span<const Value>) {
        dom::Document* doc = documentFor(fixed);
        return ev::fromUtf8(doc ? doc->contentType() : std::string("text/html"));
    }, nullptr);
    b.accessor("childNodes", [fixed](Value, std::span<const Value>) {
        return makeLiveNodeList([fixed]() { return hostDocumentChildren(documentFor(fixed)); });
    }, nullptr);
    auto edge = [&b, fixed](const char* name, bool first) {
        b.accessor(name, [fixed, first](Value, std::span<const Value>) {
            std::vector<dom::Node*> kids = hostDocumentChildren(documentFor(fixed));
            if (kids.empty()) return ev::null();
            return hostNodeValue(first ? kids.front() : kids.back());
        }, nullptr);
    };
    edge("firstChild", true);
    edge("lastChild", false);
    b.def("hasChildNodes", 0, [fixed](Value, std::span<const Value>) {
        return ev::fromBool(!hostDocumentChildren(documentFor(fixed)).empty());
    });
    b.def("getRootNode", 0, [](Value self_, std::span<const Value>) { return self_; });

    // insertBefore(node, ref) / appendChild(node): a fragment's children
    // one by one, each validated.
    auto insert = [fixed](Value nodeV, dom::Node* ref, dom::Node* replacing) -> Value {
        dom::Document* doc = documentFor(fixed);
        dom::Node* node = hostNodeOf(nodeV);
        if (!doc || !node) return ev::throwTypeError("parameter 1 is not of type 'Node'.");
        std::vector<dom::Node*> nodes;
        if (node->nodeType() == dom::NodeType::DocumentFragment) nodes = node->childNodes();
        else nodes.push_back(node);
        for (dom::Node* n : nodes) {
            std::string err = documentInsertionError(doc, n, ref, replacing);
            if (!err.empty()) return hierarchyError("Failed to execute 'insertBefore' on 'Node': " + err);
        }
        if (replacing) hostDetachFromDocument(replacing);
        for (dom::Node* n : nodes) attachToDocument(doc, n);
        return nodeV;
    };
    b.def("appendChild", 1, [insert](Value, std::span<const Value> a) { return insert(argAt(a, 0), nullptr, nullptr); });
    b.def("insertBefore", 2, [insert, fixed](Value, std::span<const Value> a) {
        dom::Node* ref = hostNodeOf(argAt(a, 1));
        if (ref && !(hostIsDocumentChild(ref) && ref->document() == documentFor(fixed))) {
            return ev::throwValue(hostMakeDomError("NotFoundError", "The node before which the new node is to be inserted is not a child of this node."));
        }
        return insert(argAt(a, 0), ref, nullptr);
    });
    b.def("replaceChild", 2, [insert, fixed](Value, std::span<const Value> a) {
        dom::Node* old = hostNodeOf(argAt(a, 1));
        if (!old || !(hostIsDocumentChild(old) && old->document() == documentFor(fixed))) {
            return ev::throwValue(hostMakeDomError("NotFoundError", "The node to be replaced is not a child of this node."));
        }
        insert(argAt(a, 0), old, old);
        return argAt(a, 1);
    });
    b.def("removeChild", 1, [fixed](Value, std::span<const Value> a) {
        dom::Node* node = hostNodeOf(argAt(a, 0));
        if (!node || !(hostIsDocumentChild(node) && node->document() == documentFor(fixed))) {
            return ev::throwValue(hostMakeDomError("NotFoundError", "The node to be removed is not a child of this node."));
        }
        hostDetachFromDocument(node);
        return argAt(a, 0);
    });
    b.def("append", 1, [insert](Value, std::span<const Value> a) {
        for (const Value& v : a) insert(v, nullptr, nullptr);
        return ev::undefined();
    });
    b.def("contains", 1, [fixed](Value, std::span<const Value> a) {
        dom::Node* n = hostNodeOf(argAt(a, 0));
        dom::Document* doc = documentFor(fixed);
        if (!n || !doc) return ev::fromBool(false);
        while (n->parentNode()) n = n->parentNode();
        return ev::fromBool(n->document() == doc && hostIsDocumentChild(n));
    });
    b.def("cloneNode", 1, [fixed](Value, std::span<const Value> a) {
        dom::Document* src = documentFor(fixed);
        if (!src) return ev::null();
        const bool deep = ev::toBool(argAt(a, 0));
        dom::Document* doc = newEmptyDocument();
        doc->setXml(src->isXml());
        doc->setContentType(src->contentType());
        doc->setUrl(isParsedDocument(src) ? src->url() : documentUrl(src));
        if (deep) {
            for (dom::Node* kid : hostDocumentChildren(src)) attachToDocument(doc, doc->cloneNode(kid, true));
        }
        return hostDocumentValue(doc);
    });
    b.set("implementation", implementationFor(fixed));
}

namespace {

Value implementationFor(dom::Document* fixed) {
    ObjectBuilder impl(ev::createObject());
    impl.def("createHTMLDocument", 0, createHTMLDocument);
    impl.def("hasFeature", 0, [](Value, std::span<const Value>) { return ev::fromBool(true); });
    impl.def("createDocumentType", 3, [fixed](Value, std::span<const Value> a) {
        dom::Document* doc = documentFor(fixed);
        if (!doc) return ev::null();
        if (a.size() < 3) return ev::throwTypeError("Failed to execute 'createDocumentType' on 'DOMImplementation': 3 arguments required.");
        return hostNodeValue(doc->createDocumentType(ev::toUtf8(argAt(a, 0)), ev::toUtf8(argAt(a, 1)),
                                                     ev::toUtf8(argAt(a, 2))));
    });
    impl.def("createDocument", 2, [](Value, std::span<const Value> a) {
        const std::string ns = optionalString(argAt(a, 0));
        const std::string qname = optionalString(argAt(a, 1));
        dom::Node* dt = hostNodeOf(argAt(a, 2));
        if (dt && dt->nodeType() != dom::NodeType::DocumentType) {
            return ev::throwTypeError("Failed to execute 'createDocument' on 'DOMImplementation': parameter 3 is not of type 'DocumentType'.");
        }
        const char* type = ns == kXhtmlNs ? "application/xhtml+xml" : ns == kSvgNs ? "image/svg+xml" : "application/xml";
        dom::Document* doc = emptyXmlDocument(type);
        const ev::Persistent docV(hostDocumentValue(doc));
        if (dt) attachToDocument(doc, dt);
        if (!qname.empty()) {
            const ev::Persistent create(ev::getProperty(docV.get(), "createElementNS"));
            const ev::Persistent nsV(ns.empty() ? ev::null() : ev::fromUtf8(ns));
            const ev::Persistent qV(ev::fromUtf8(qname));
            Value args[2] = {nsV.get(), qV.get()};
            ev::CallResult r = ev::call(create.get(), docV.get(), std::span<const Value>(args, 2));
            if (r.thrown) return ev::throwValue(r.value);
            if (dom::Node* el = hostNodeOf(r.value)) attachToDocument(doc, el);
        }
        return docV.get();
    });
    return ev::setPrototype(impl.get(), domImplementationHostClass().prototype());
}

}  // namespace

// `new Document()`: an empty XML document whose interface is Document itself.
Value documentConstructor(Value, std::span<const Value>) {
    dom::Document* doc = emptyXmlDocument("application/xml");
    const ev::Persistent v(hostDocumentValue(doc));
    return ev::setPrototype(v.get(), documentHostClass().prototype());
}

}  // namespace bro::bronze_host
