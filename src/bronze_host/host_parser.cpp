// DOMParser & XMLSerializer — bronze_host translation unit.

#include "bronze_host/bronze_host.h"
#include "bronze_host/host_builder.h"
#include "bronze_host/host_element.h"
#include "bronze_host/host_document.h"
#include "bronze_host/host_node.h"
#include "dom/document.h"
#include "dom/element.h"
#include "dom/serialize.h"
#include "dom/text_node.h"

#include <memory>
#include <span>
#include <string>
#include <vector>

#include "bronze_host/host_compartment.h"
#include "bronze_host/host_node_sweep.h"

namespace bro::bronze_host {

namespace {

struct ParsedDocEntry {
    std::unique_ptr<dom::Document> doc;
    uint64_t compartmentId = 0;
    SweepGroup* sweepGroup = nullptr;
    bool sweepAlone = false;
    bool newlyCreated = true;
};

std::vector<ParsedDocEntry> s_parsedDocs;

ParsedDocEntry* findParsedDoc(const dom::Document* doc) {
    if (!doc) return nullptr;
    for (auto& e : s_parsedDocs) {
        if (e.doc.get() == doc) return &e;
    }
    return nullptr;
}

void teardownParsedDoc(dom::Document* doc) {
    if (!doc) return;
    hostSweepForgetDocument(doc);
    clearHostElementsForDocument(doc);
    clearHostDocument(doc);
    forgetDocumentOwner(doc);
}

}  // namespace

dom::Document* parseIntoNewDocument(const std::string& html) {
    auto doc = std::make_unique<dom::Document>();
    dom::Document* ptr = doc.get();
    doc->parse(html);
    // The parsing compartment's: its code alone may wrap and walk it.
    noteDocumentOwner(ptr, currentCompartment());
    s_parsedDocs.push_back(ParsedDocEntry{std::move(doc), currentCompartmentId()});
    return ptr;
}

dom::Document* newEmptyDocument() {
    auto doc = std::make_unique<dom::Document>();
    dom::Document* ptr = doc.get();
    noteDocumentOwner(ptr, currentCompartment());
    s_parsedDocs.push_back(ParsedDocEntry{std::move(doc), currentCompartmentId()});
    return ptr;
}

namespace {
// An XML document for DOMParser's XML types; a parse error leaves the
// <parsererror> document Document::parseXml builds.
dom::Document* parseXmlIntoNewDocument(const std::string& xml) {
    auto doc = std::make_unique<dom::Document>();
    dom::Document* ptr = doc.get();
    doc->parseXml(xml);
    noteDocumentOwner(ptr, currentCompartment());
    s_parsedDocs.push_back(ParsedDocEntry{std::move(doc), currentCompartmentId()});
    return ptr;
}
}  // namespace

bool isParsedDocument(const dom::Document* doc) {
    return findParsedDoc(doc) != nullptr;
}

void takeNewParsedDocuments(std::vector<dom::Document*>& out) {
    out.clear();
    for (auto& e : s_parsedDocs) {
        if (e.doc && e.newlyCreated) {
            e.newlyCreated = false;
            out.push_back(e.doc.get());
        }
    }
}

void collectAllParsedDocuments(std::vector<dom::Document*>& out) {
    out.clear();
    for (auto& e : s_parsedDocs) {
        if (e.doc) {
            e.newlyCreated = false;
            out.push_back(e.doc.get());
        }
    }
}

SweepGroup* parsedDocumentSweepGroup(const dom::Document* doc) {
    ParsedDocEntry* e = findParsedDoc(doc);
    return e ? e->sweepGroup : nullptr;
}

void setParsedDocumentSweepGroup(dom::Document* doc, SweepGroup* g) {
    if (ParsedDocEntry* e = findParsedDoc(doc)) e->sweepGroup = g;
}

bool parsedDocumentSweepAlone(const dom::Document* doc) {
    ParsedDocEntry* e = findParsedDoc(doc);
    return e && e->sweepAlone;
}

void setParsedDocumentSweepAlone(dom::Document* doc, bool alone) {
    if (ParsedDocEntry* e = findParsedDoc(doc)) e->sweepAlone = alone;
}

void destroyParsedDocument(dom::Document* doc) {
    if (!doc) return;
    for (auto it = s_parsedDocs.begin(); it != s_parsedDocs.end(); ++it) {
        if (it->doc.get() == doc) {
            teardownParsedDoc(doc);
            s_parsedDocs.erase(it);
            return;
        }
    }
}

void clearParsedDocuments() {
    for (auto& entry : s_parsedDocs) {
        if (entry.doc) teardownParsedDoc(entry.doc.get());
    }
    s_parsedDocs.clear();
}

void clearParsedDocumentsForCompartment(uint64_t compartmentId) {
    for (auto it = s_parsedDocs.begin(); it != s_parsedDocs.end(); ) {
        if (it->compartmentId == compartmentId) {
            if (it->doc) teardownParsedDoc(it->doc.get());
            it = s_parsedDocs.erase(it);
        } else {
            ++it;
        }
    }
}

namespace {

Value parserParseFromString(Value, std::span<const Value> a) {
    Value htmlV = argAt(a, 0);
    if (ev::isObject(htmlV))
        return ev::throwTypeError("parseFromString: markup must be a string");
    Value mimeV = argAt(a, 1);
    if (!ev::isString(mimeV))
        return ev::throwTypeError("parseFromString: mimeType must be a string");
    std::string mime = ev::toUtf8(mimeV);
    if (mime != "text/html" &&
        mime != "text/xml" &&
        mime != "application/xml" &&
        mime != "application/xhtml+xml" &&
        mime != "image/svg+xml") {
        return ev::throwTypeError("parseFromString: '" + mime + "' is not a supported mimeType");
    }
    const std::string html = ev::isUndefined(htmlV) ? std::string() : ev::toUtf8(htmlV);
    dom::Document* doc = mime == "text/html" ? parseIntoNewDocument(html)
                                             : parseXmlIntoNewDocument(html);
    doc->setContentType(mime);
    return hostDocumentValue(doc);
}

Value makeParserValue() {
    ObjectBuilder b;
    b.def("parseFromString", 2, parserParseFromString);
    return b.get();
}

// The XML serialization of a node, or of a document (found through its
// document element or doctype, a document object having no node of its own).
Value xmlSerializerSerializeToString(Value, std::span<const Value> a) {
    const Value nodeVal = argAt(a, 0);
    if (dom::Node* node = hostNodeOf(nodeVal)) return ev::fromUtf8(dom::serializeXml(node));
    if (!ev::isObject(nodeVal) || ev::toDouble(ev::getProperty(nodeVal, "nodeType")) != 9.0) {
        return ev::throwTypeError("Failed to execute 'serializeToString' on 'XMLSerializer': parameter 1 is not of type 'Node'.");
    }
    dom::Node* child = hostNodeOf(ev::getProperty(nodeVal, "documentElement"));
    if (!child) child = hostNodeOf(ev::getProperty(nodeVal, "doctype"));
    return ev::fromUtf8(child ? dom::serializeXml(nullptr, child->document()) : std::string());
}

Value makeXMLSerializerValue() {
    ObjectBuilder b;
    b.def("serializeToString", 1, xmlSerializerSerializeToString);
    return b.get();
}

}  // namespace

// ---------------------------------------------------------------------------
// install
// ---------------------------------------------------------------------------

void installParserGlobal() {
    // Rooted: each is used again after later allocations.
    const Rooted ctor(ev::makeFunction(
        [](Value, std::span<const Value>) { return makeParserValue(); }, 0));
    ev::registerGlobal("DOMParser", ctor);

    const Rooted xmlSerializerCtor(ev::makeFunction(
        [](Value, std::span<const Value>) { return makeXMLSerializerValue(); }, 0));
    ev::registerGlobal("XMLSerializer", xmlSerializerCtor);

    ev::GlobalValue gt = ev::globalValue("globalThis");
    if (gt.found && ev::isObject(gt.value)) {
        const Rooted global(gt.value);
        ev::setProperty(global, "DOMParser", ctor);
        ev::setProperty(global, "XMLSerializer", xmlSerializerCtor);
    }
}

}  // namespace bro::bronze_host
