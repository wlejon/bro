#pragma once

// ComputedStyle is a map alias, so it cannot be forward-declared — and spelling
// the alias out here by hand silently duplicates htmlayout's definition, which
// then has to be kept in step with it by luck (it wasn't: the map is keyed
// heterogeneously now, and this copy was not).
#include "css/cascade.h"

#include <vector>

namespace bro::dom { class Element; }
namespace bro::engine { struct ScrollbarMetrics; class Scrollbar; }

namespace bro::engine {

/// Get the effective vertical overflow value for an element.
/// Checks overflow-y first (more specific), then falls back to overflow.
std::string getOverflowY(const htmlayout::css::ComputedStyle& style);

/// Whether an element clips overflowing content (hidden, scroll, or auto).
bool overflowClips(const std::string& ov);

/// Whether an element is user-scrollable (scroll or auto only, not hidden).
bool overflowScrollable(const std::string& ov);

/// Compute the maximum scroll offset for an overflow element.
/// Returns 0 if content fits within the element's visible area.
float maxScrollTop(dom::Element* el);

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

/// Walk the composed tree from `root` to find the deepest overflow element
/// whose scrollbar area contains (x, y). Returns nullptr if none.
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
