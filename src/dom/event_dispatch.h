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

} // namespace bro::dom
