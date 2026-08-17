// DOMParser: turn a string of HTML into a tree you can pick apart and take
// pieces from.
//
// This is the one DOM entry point that makes a SECOND document exist in a
// bronze program, and almost everything below is a consequence of that.
//
// WHY AN APP WANTS IT. Markup arrives as text — a fetched fragment, a template
// baked into the app, a chunk of generated HTML — and the alternative to
// parsing it is building the same tree by hand with createElement, which is
// both slower to write and slower to run for anything past a few nodes. The
// half of this that already worked is `innerHTML`, which parses INTO an element
// that is already in the tree; DOMParser is the other half, where the app wants
// to look at what it parsed and take only some of it.
//
// LIFETIME: PARSED DOCUMENTS ARE NEVER FREED. `g_parsed` below owns every one
// of them for the life of the process. That is a deliberate choice over the
// obvious alternative — a handle finalizer that deletes the Document when its
// wrapper is collected — and it is worth saying why, because the obvious
// alternative is what the QuickJS side does (js/dom_bindings.cpp's
// wrapDetachedDocument) and it does not port:
//
//   * ~Document severs wrappers through nodeDestroyingCb_, which is a single
//     callback slot the JS realm owns, and it visits elements only. The
//     freed-node observer LIST that this layer's registry depends on — the one
//     documented in document.h as existing precisely "for a wrapper layer that
//     is not the JS realm" — is not fired from ~Document at all. A finalizer
//     that deleted a parsed document would therefore leave a live
//     HostNodeState pointing into released storage for every node of it this
//     layer had ever wrapped.
//
//   * Even with that fixed, a node wrapper can outlive the document wrapper it
//     came from: `const el = parser.parseFromString(s).body.firstChild` drops
//     the document on the same line. On the web the node keeps the document
//     alive; here the only way to express that would be for every node wrapper
//     to root its document — and registry entries are never freed, so rooting
//     one would pin the document forever anyway. The finalizer buys nothing
//     and costs a class of silent wrong answers.
//
// So the cost is a Document husk per parse, and it is a real cost — an app
// parsing a fragment every frame grows without bound. Two things keep it small
// rather than absent: adoption moves every node the app actually KEEPS out of
// the parsed document and into the live one (dom/element.cpp), so what is
// retained is the husk and whatever the app did not want; and parsing is
// normally a startup or a per-interaction act rather than a per-frame one.
// The fix wants `~Document` to fire the freed-node observer list (which exists
// in dom/document.h for a wrapper layer that is not the JS realm, and which
// nothing fires today) plus a registry entry that can actually be released;
// src/bronze_host/README.md carries both halves.
//
// ADOPTION IS WHAT MAKES THE RESULT USABLE. `liveEl.appendChild(parsed)` has to
// move `parsed` out of the parser document's ownership, or the live tree ends
// up holding a node someone else will destroy. That step is not here: it is in
// Node::appendChild, where every inserter gets it, which is a change this file
// forced and the DOM layer wanted anyway.
//
// TYPE IS IGNORED. bro has no XML parser, so 'text/html', 'text/xml' and
// 'image/svg+xml' all go through gumbo. The interpreted side made the same
// choice for the same reason (js/document_bindings.cpp).

#include "bronze_host/bronze_host.h"
#include "bronze_host/gl_internal.h"  // ObjectBuilder, argAt
#include "bronze_host/host_internal.h"

#include "dom/document.h"

#include <memory>
#include <span>
#include <string>
#include <vector>

namespace bro::bronze_host {

namespace {

// Every document this layer has parsed, owned for the life of the process. A
// pointer into it is stable — unique_ptr, so growing the vector does not move
// the documents — which is what the wrappers capture.
//
// Heap-allocated and never deleted, the same convention the timer and observer
// tables follow: a static destructor running at process exit would tear down
// documents whose wrappers may still be reachable.
std::vector<std::unique_ptr<dom::Document>>* g_parsed = nullptr;

dom::Document* parseIntoNewDocument(const std::string& html) {
    if (!g_parsed) g_parsed = new std::vector<std::unique_ptr<dom::Document>>();
    g_parsed->push_back(std::make_unique<dom::Document>());
    dom::Document* doc = g_parsed->back().get();
    // No author or UA stylesheet: nothing lays this tree out. An app that
    // wants the parsed nodes styled gets that when it appends them, from the
    // cascade of the document it appends them into.
    doc->parse(html);
    return doc;
}

Value parserParseFromString(Value, std::span<const Value> a) {
    Value htmlV = argAt(a, 0);
    // An object argument is the mistake worth catching: `parseFromString(obj)`
    // would otherwise parse the string "[object Object]" into an empty tree and
    // answer a document the app then finds mysteriously blank.
    if (ev::isObject(htmlV))
        return ev::throwTypeError("parseFromString: markup must be a string");
    const std::string html = ev::isUndefined(htmlV) ? std::string() : ev::toUtf8(htmlV);
    // Argument 1 (the mime type) is read and discarded — see the file comment.
    dom::Document* doc = parseIntoNewDocument(html);
    return hostDocumentValue(doc);
}

Value makeParserValue() {
    ObjectBuilder b;
    b.def("parseFromString", 2, parserParseFromString);
    return b.get();
}

}  // namespace

void installParserGlobal() {
    // Same construct story as XMLHttpRequest and Image: bronze_construct
    // replaces the plain instance with whatever the body returns, so `new
    // DOMParser()` and `DOMParser()` answer the same object and `instanceof` is
    // false. Real code only ever writes the `new` form and only ever calls one
    // method on the result.
    Value ctor = ev::makeFunction(
        [](Value, std::span<const Value>) { return makeParserValue(); }, 0);
    ev::registerGlobal("DOMParser", ctor);
}

}  // namespace bro::bronze_host
