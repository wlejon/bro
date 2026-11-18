#pragma once

#include <vector>

namespace bro::dom {

class Document;
class Element;
class Event;

/// Dispatches a DOM event through standard DOM event propagation:
/// window (capture) -> ancestors (capture) -> target (at_target) -> ancestors (bubble) -> window (bubble).
/// Invokes native listeners registered via Element::addEventListener and Document::windowListeners.
void dispatchDomEvent(Element* target, Event& event);

/// Dispatches an event directly at window listeners on the given document.
void dispatchWindowEvent(Document* doc, Event& event);

/// Sets `doc`'s readiness (Document::readyState) to `state` and fires
/// `readystatechange` at the document, as HTML's "update the current document
/// readiness" does. Nothing happens when the document is already in `state`.
void advanceDocumentReadiness(Document* doc, const char* state);

/// The end of a document's load (HTML "the end"): readiness "interactive" if
/// it is still loading, DOMContentLoaded at the document (bubbling to the
/// window), then readiness "complete" and `load` at the window. Nothing
/// happens for a document that is already complete.
void completeDocumentLoad(Document* doc);

} // namespace bro::dom
