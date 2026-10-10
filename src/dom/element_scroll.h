#pragma once

// Element scrolling as an engine operation rather than as binding code.
//
// `el.scrollTop = v` is not a field write. It clamps to the scrollable range,
// it has to survive the fact that layout is ASYNC (the classic
// `el.scrollTop = el.scrollHeight` runs in the same turn as the append it is
// following, when neither number is known yet), it dirties the document so the
// new offset is painted, and it fires a trusted `scroll` event. All four were
// inline in the old QuickJS binding and all four went missing when the binding
// was rewritten as a setter on dom::Element::setScrollTopValue.
//
// They live here, in the DOM layer, because they are properties of scrolling
// and not of who asked for it: scrollIntoView, a compiled program calling
// through the host, and engine C++ all owe the same behaviour.

#include "css/cascade.h"

#include <string>

namespace bro::dom {

class Element;

/// The used `overflow` on one axis: the longhand (falling back to the
/// shorthand, then `visible`), with CSS Overflow 3's pairing rule applied — if
/// the other axis scrolls, `visible` here computes to `auto` and `clip` to
/// `hidden`. So `overflow-y: auto` alone makes a box scroll horizontally too,
/// as it does in every browser.
std::string usedOverflow(const htmlayout::css::ComputedStyle& style, bool horizontal);

/// The largest scrollTop `el` can hold: its unclamped content height minus the
/// height actually shown. 0 for an element that does not overflow.
float maxScrollTopOf(const Element* el);

/// The largest scrollLeft `el` can hold: its unclamped content width minus the
/// width actually shown. 0 for an element that does not overflow.
float maxScrollLeftOf(const Element* el);

/// The scrollTop / scrollLeft `el` actually applies: the stored offset clamped
/// into [0, max]. The stored value can briefly run past the max (script wrote
/// it before layout caught up, or the content shrank); paint, geometry and hit
/// testing all read it through these so they agree on where the content is.
float clampedScrollTopOf(const Element* el);
float clampedScrollLeftOf(const Element* el);

/// Is `el` a scroll container at all — does its resolved overflow clip?
/// `visible` and `initial` spill rather than scroll, so moving them moves
/// nothing. elementClipsOverflow reads the vertical axis (overflow-y),
/// elementClipsOverflowX the horizontal one (overflow-x).
bool elementClipsOverflow(const Element* el);
bool elementClipsOverflowX(const Element* el);

/// `el.scrollTop = v`, with the whole operation: clamp, the deferred
/// scroll-to-bottom intent when the request asks for the end while a layout is
/// still pending, the document dirty mark, and the `scroll` event when the
/// offset actually moved.
void setElementScrollTop(Element* el, double v);

/// `el.scrollLeft = v`: clamp, mark document dirty, and fire `scroll` event if moved.
void setElementScrollLeft(Element* el, double v);

/// `el.scrollBy({top: delta})` — the same operation, relative.
void scrollElementBy(Element* el, double delta);

/// `el.scrollBy({left: delta})` — the horizontal relative scroll operation.
void scrollElementLeftBy(Element* el, double delta);

}  // namespace bro::dom
