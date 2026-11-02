#pragma once

// Documents other than the one the engine is showing: DOMParser
// (host_parser.cpp) and the document surface bound to any dom::Document
// (dom_globals.cpp).

#include "embed/embed.h"

#include <string>
#include <vector>

namespace bro::dom {
class Document;
}  // namespace bro::dom

namespace bro::bronze_host {

namespace ev = bronze::embed;
using Value = bronze::Value;

struct SweepGroup;

void installParserGlobal();

// A detached document parsed from `html` (DOMParser /
// document.implementation.createHTMLDocument); owned by the parser registry
// until the detached-DOM sweep proves neither its Document wrapper nor any
// Node wrapper inside it is reachable from JavaScript.
dom::Document* parseIntoNewDocument(const std::string& html);

// A full document surface — the queries, the factories, the element accessors —
// bound to `doc` rather than to whatever the engine is currently showing. The
// `document` global is the one wrapper NOT built this way; dom_globals.cpp's
// documentFor() says why. Defined there, beside the builder it shares.
Value makeDocumentValue(dom::Document* fixed);
Value hostDocumentValue(dom::Document* doc);
ev::Persistent* hostDocumentWrapperSlot(dom::Document* doc);

// Parsed-document hooks used by the detached-DOM sweep (host_node_sweep.cpp).
bool isParsedDocument(const dom::Document* doc);
void takeNewParsedDocuments(std::vector<dom::Document*>& out);
void collectAllParsedDocuments(std::vector<dom::Document*>& out);
SweepGroup* parsedDocumentSweepGroup(const dom::Document* doc);
void setParsedDocumentSweepGroup(dom::Document* doc, SweepGroup* g);
bool parsedDocumentSweepAlone(const dom::Document* doc);
void setParsedDocumentSweepAlone(dom::Document* doc, bool alone);
void destroyParsedDocument(dom::Document* doc);

}  // namespace bro::bronze_host
