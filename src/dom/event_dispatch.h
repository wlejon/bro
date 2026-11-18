#pragma once

#include <vector>

namespace bro::dom {

class Document;
class Element;
class Event;
class ShadowRoot;

/// Dispatches a DOM event through standard DOM event propagation:
/// window (capture) -> ancestors (capture) -> target (at_target) -> ancestors (bubble) -> window (bubble).
/// Invokes native listeners registered via Element::addEventListener and Document::windowListeners.
void dispatchDomEvent(Element* target, Event& event);

/// The same dispatch with a shadow root as the target (shadowRoot.dispatchEvent):
/// the root, then its host's path when the event is composed.
void dispatchDomEventAtRoot(ShadowRoot* root, Event& event);

// An element's event handler content attribute (`onclick="..."`, HTML
// §8.1.8.1) runs through this hook, set by the script layer: called at the
// target and in the bubbling phase, before the element's listeners, for an
// element carrying an `on<type>` attribute. The layer compiles it, and skips
// it where script set the handler property instead.
using AttributeHandlerHook = void (*)(Element* el, Event& event);
void setAttributeHandlerHook(AttributeHandlerHook hook);

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
