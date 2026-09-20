// DOMParser & XMLSerializer — bronze_host translation unit.

#include "bronze_host/bronze_host.h"
#include "bronze_host/gl_internal.h"
#include "bronze_host/host_internal.h"
#include "dom/document.h"
#include "dom/element.h"
#include "dom/text_node.h"

#include <memory>
#include <span>
#include <string>
#include <vector>

#include "bronze_host/host_realm_scope.h"

namespace bro::bronze_host {

namespace {

struct ParsedDocEntry {
    std::unique_ptr<dom::Document> doc;
    uint64_t scopeId = 0;
};

std::vector<ParsedDocEntry> s_parsedDocs;

}  // namespace

dom::Document* parseIntoNewDocument(const std::string& html) {
    auto doc = std::make_unique<dom::Document>();
    dom::Document* ptr = doc.get();
    doc->parse(html);
    const uint64_t scopeId = currentRealmScope();
    s_parsedDocs.push_back(ParsedDocEntry{std::move(doc), scopeId});
    return ptr;
}

void clearParsedDocuments() {
    for (auto& entry : s_parsedDocs) {
        if (entry.doc) {
            clearHostElementsForDocument(entry.doc.get());
            clearHostDocument(entry.doc.get());
        }
    }
    s_parsedDocs.clear();
}

void clearParsedDocumentsForScope(uint64_t scopeId) {
    for (auto it = s_parsedDocs.begin(); it != s_parsedDocs.end(); ) {
        if (it->scopeId == scopeId) {
            if (it->doc) {
                clearHostElementsForDocument(it->doc.get());
                clearHostDocument(it->doc.get());
            }
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
    const std::string html = ev::isUndefined(htmlV) ? std::string() : ev::toUtf8(htmlV);
    dom::Document* doc = parseIntoNewDocument(html);
    return hostDocumentValue(doc);
}

Value makeParserValue() {
    ObjectBuilder b;
    b.def("parseFromString", 2, parserParseFromString);
    return b.get();
}

Value xmlSerializerSerializeToString(Value, std::span<const Value> a) {
    if (a.empty()) return ev::fromUtf8("");
    Value nodeVal = a[0];
    if (ev::isNull(nodeVal) || ev::isUndefined(nodeVal)) return ev::fromUtf8("");

    if (dom::Element* el = hostElementOf(nodeVal)) {
        return ev::fromUtf8(el->outerHTML());
    }

    Value docElem = ev::getProperty(nodeVal, "documentElement");
    if (dom::Element* el = hostElementOf(docElem)) {
        return ev::fromUtf8(el->outerHTML());
    }

    Value body = ev::getProperty(nodeVal, "body");
    if (dom::Element* el = hostElementOf(body)) {
        return ev::fromUtf8(el->innerHTML());
    }

    Value inner = ev::getProperty(nodeVal, "innerHTML");
    if (ev::isString(inner)) return inner;

    Value text = ev::getProperty(nodeVal, "textContent");
    if (ev::isString(text)) return text;

    return ev::fromUtf8("");
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
    Value ctor = ev::makeFunction(
        [](Value, std::span<const Value>) { return makeParserValue(); }, 0);
    ev::registerGlobal("DOMParser", ctor);

    Value xmlSerializerCtor = ev::makeFunction(
        [](Value, std::span<const Value>) { return makeXMLSerializerValue(); }, 0);
    ev::registerGlobal("XMLSerializer", xmlSerializerCtor);

    ev::GlobalValue gt = ev::globalValue("globalThis");
    if (gt.found && ev::isObject(gt.value)) {
        ev::setProperty(gt.value, "DOMParser", ctor);
        ev::setProperty(gt.value, "XMLSerializer", xmlSerializerCtor);
    }
}

}  // namespace bro::bronze_host
