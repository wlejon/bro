#pragma once

extern "C" {
#include "quickjs.h"
}

namespace bro::dom { class Element; class Event; }

namespace bro::js {

/// Dispatch a DOM event through the JS listener system with bubbling.
/// Shared by both the windowed Engine and the Headless tool.
void dispatchDomEvent(JSContext* ctx, bro::dom::Element* target, bro::dom::Event& event);

} // namespace bro::js
