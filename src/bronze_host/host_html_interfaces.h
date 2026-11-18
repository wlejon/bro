#pragma once

#include "bronze_host/host_class.h"
#include "bronze_host/host_element.h"
#include <string>

namespace bro::bronze_host {

const HostClass& nodeHostClass();
const HostClass& documentHostClass();
const HostClass& elementHostClass();
const HostClass& htmlElementHostClass();
const HostClass& htmlImageElementClass();
// Brands for host-built plain objects: set as the prototype of every 2D
// context (host_canvas2d.cpp) and dataTransfer (host_dom_events.cpp).
const HostClass& canvasRenderingContext2DHostClass();
const HostClass& dataTransferHostClass();
const HostClass& svgElementHostClass();
const HostClass& characterDataHostClass();
const HostClass& textHostClass();
const HostClass& commentHostClass();
const HostClass& documentFragmentHostClass();
const HostClass& documentTypeHostClass();
const HostClass& domImplementationHostClass();

// host_document_node.cpp: the document's node surface (childNodes, doctype,
// the four mutators, cloneNode, implementation), DocumentType's members, and
// `new Document()`.
void decorateDocumentNode(ObjectBuilder& b, dom::Document* fixed);
void decorateDocumentTypeProto(ObjectBuilder& b);
Value documentConstructor(Value, std::span<const Value>);
// The interface prototype for a document object: HTMLDocument or XMLDocument.
Value documentInterfaceProto(const dom::Document* doc);

// The interface prototype an element is born on: by namespace, then tag.
Value elementInterfaceProto(const dom::Element* el);

void decorateDocumentProto(ObjectBuilder& b);

void installHtmlInterfaces();
// After brokit's EventTarget exists: Node.prototype inherits EventTarget.prototype.
void chainNodeOntoEventTarget();

}  // namespace bro::bronze_host
