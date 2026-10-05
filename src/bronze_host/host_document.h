#pragma once

// Documents other than the one the engine is showing: DOMParser
// (host_parser.cpp) and the document surface bound to any dom::Document
// (dom_globals.cpp).

#include "embed/embed.h"

#include <string>

namespace bro::dom {
class Document;
}  // namespace bro::dom

namespace bro::bronze_host {

namespace ev = bronze::embed;
using Value = bronze::Value;

void installParserGlobal();

// A detached document parsed from `html`, owned for the life of the process
// like every DOMParser result; what document.implementation.createHTMLDocument
// hands back as well.
dom::Document* parseIntoNewDocument(const std::string& html);

// A full document surface — the queries, the factories, the element accessors —
// bound to `doc` rather than to whatever the engine is currently showing. The
// `document` global is the one wrapper NOT built this way; dom_globals.cpp's
// documentFor() says why. Defined there, beside the builder it shares.
Value makeDocumentValue(dom::Document* fixed);
Value hostDocumentValue(dom::Document* doc);

}  // namespace bro::bronze_host
