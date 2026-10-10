#pragma once

// ComputedStyle is a map alias, so it cannot be forward-declared — and spelling
// the alias out here by hand silently duplicates htmlayout's definition, which
// then has to be kept in step with it by luck (it wasn't: the map is keyed
// heterogeneously now, and this copy was not).
#include "css/cascade.h"
#include "dom/element_scroll.h"   // max / clamped scroll offsets, per axis
#include "engine/scrollbar.h"

#include <vector>

namespace bro::dom { class Element; }

namespace bro::engine {

/// The used vertical / horizontal overflow value for an element
/// (dom::usedOverflow: the longhand, falling back to the shorthand, with the
/// visible→auto pairing rule applied).
std::string getOverflowY(const htmlayout::css::ComputedStyle& style);
std::string getOverflowX(const htmlayout::css::ComputedStyle& style);

/// Whether an element clips overflowing content (hidden, scroll, or auto).
bool overflowClips(const std::string& ov);

/// Whether an element is user-scrollable (scroll or auto only, not hidden).
bool overflowScrollable(const std::string& ov);

/// Write `el`'s scroll offset on one axis, clamped into [0, max]. Returns
/// whether the stored offset moved; the caller owns the `scroll` event and the
/// repaint, since who gets told differs by document (app, host window, system
/// panel).
bool setScrollOffsetClamped(dom::Element* el, bool horizontal, float offset);

/// Browser-style wheel chaining on one axis: from `target` up the composed
/// tree, the first user scroller on that axis (overflow auto/scroll) that can
/// still move in the delta's direction takes the whole delta. `delta` is in
/// scroll-offset px, positive toward the end (down / right). Returns the
/// element that took it (nullptr if none did), and sets `moved` when its
/// offset actually changed.
dom::Element* wheelScrollChain(dom::Element* target, bool horizontal, float delta,
                               bool& moved);

/// Re-clamp every scroller's stored scrollTop into its post-layout range,
/// walking the composed tree from `root`. Call this after layout: content that
/// shrank (a tab panel swapped for a shorter one, a collapsed fold, a filtered
/// list) leaves the stored offset above the new max, and nothing else brings it
/// back down — the wheel handler skips a scroller whose content now fits
/// (maxST == 0), so the stale offset would persist indefinitely. Paint clamps
/// on read, so the frame *looks* right while hit testing reads the raw value
/// and lands the pointer somewhere else entirely.
///
/// display:none subtrees are skipped: their boxes are stale, so clamping there
/// would zero a remembered offset using a height that is no longer meaningful.
/// Returns true if any offset moved, so the caller can dispatch scroll events.
bool clampScrollOffsets(dom::Element* root,
                        std::vector<dom::Element*>* changed = nullptr);

/// Walk up the composed tree (crosses shadow boundaries via host element).
dom::Element* composedParent(dom::Element* el);

/// The overlay scrollbars of one element: `v` (right edge) and `h` (bottom
/// edge), each visible only when the element is a user scroller on that axis
/// whose content overflows. When both show, each stops short of the corner
/// the other one occupies. `contentX/contentY` are the element's content-box
/// origin in the space the bars are wanted in. The offsets are the clamped
/// ones paint uses, so the thumbs sit where the content is.
struct ElementScrollbarLayout {
    ScrollbarMetrics v, h;
};
ElementScrollbarLayout layoutElementScrollbars(dom::Element* elem,
                                               float contentX, float contentY,
                                               const Scrollbar& scrollbar);

/// A press on `el`'s scrollbar track `m` outside the thumb: page one view
/// toward the press. `along` is the press coordinate on the bar's axis, in the
/// space `m` was laid out in. Returns whether the offset moved.
bool pageElementScrollbar(dom::Element* el, const Scrollbar& scrollbar,
                          const ScrollbarMetrics& m, float along);

/// A thumb drag in progress on `el` (scrollbar.isDragging()): move the offset
/// on the dragged bar's axis to follow `along`, the pointer coordinate on that
/// axis in the space the drag began in. Returns whether the offset moved.
bool dragElementScrollbar(dom::Element* el, const Scrollbar& scrollbar, float along);

/// Walk the composed tree from `root` to find the deepest overflow element
/// whose scrollbar area contains (x, y). Returns nullptr if none.
/// `outMetrics.horizontal` says which of its two bars was hit.
/// `offsetX/offsetY` map layout box coordinates to the document's draw space
/// at `root` — the app doc passes -scrollY_ (content space, matching its
/// content-sized layer surfaces; callers fold the engine inset out of the
/// mouse y before comparing); system panel docs pass 0 (window space).
/// On a hit, `outMetrics` receives the scrollbar layout so the caller can
/// run thumbHitTest / beginDrag / scrollToPosition against the same rect.
dom::Element* findElementScrollbarHit(
    dom::Element* root, float x, float y,
    float offsetX, float offsetY,
    Scrollbar& scrollbar, ScrollbarMetrics& outMetrics);

} // namespace bro::engine
