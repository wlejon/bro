// DOMParser — bronze_host translation unit.

#include "bronze_host/bronze_host.h"
#include "bronze_host/gl_internal.h"
#include "bronze_host/host_internal.h"

#include "bronze_host/gl_internal.h"
#include "bronze_host/host_internal.h"
#include "dom/document.h"
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace bro::bronze_host {

namespace {

HostClass g_dom_parserClass;

std::vector<std::unique_ptr<dom::Document>>* g_parsed = nullptr;

dom::Document* parseIntoNewDocument(const std::string& html) {
    if (!g_parsed) g_parsed = new std::vector<std::unique_ptr<dom::Document>>();
    g_parsed->push_back(std::make_unique<dom::Document>());
    dom::Document* doc = g_parsed->back().get();
    doc->parse(html);
    return doc;
}

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

}  // namespace

// ---------------------------------------------------------------------------
// install
// ---------------------------------------------------------------------------

void installParserGlobal() {
    Value ctor = ev::makeFunction(
        [](Value, std::span<const Value>) { return makeParserValue(); }, 0);
    ev::registerGlobal("DOMParser", ctor);
}

}  // namespace bro::bronze_host
