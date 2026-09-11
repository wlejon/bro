#pragma once

// The RESOLVED value of a CSS property on an element — what getComputedStyle()
// answers, as distinct from what the cascade stored.
//
// The two are not the same thing, and the difference is most of this file's
// reason to exist. The cascade stores `width: auto`, `margin: 1em`,
// `color: red`, `line-height: 1.4`; a computed-style read has to answer
// `831px`, `16px`, `rgb(255, 0, 0)`, `22.4px` — used values off the layout box,
// absolutised lengths, serialised colours, line-height resolved through the
// real font metrics. Every one of those needs something the style map alone
// does not have.
//
// The resolution lives beside the layout it reads from so all consumers have
// a single consistent computed style representation.

#include "dom/element.h"

#include <string>

namespace bro::layout {

class SkiaTextMetrics;

// `prop` is the CSS spelling ("background-color", not "backgroundColor").
// `metrics` may be null: only line-height resolution consults it, and it falls
// back to the metric-free approximation when there is none — which is what a
// caller with no engine (a test, a layout-thread read) gets.
//
// The caller owes a layout flush first for anything geometric: this reads
// el->layoutBox() as it finds it and does not itself force a pass, because the
// binding layers already have the document handy and the engine's flush is
// theirs to schedule (Engine::flushLayoutForRead).
std::string computedProperty(dom::Element* el, const std::string& prop,
                             SkiaTextMetrics* metrics);

}  // namespace bro::layout
