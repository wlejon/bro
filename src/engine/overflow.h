#pragma once

#include <string>
#include <unordered_map>

namespace htmlayout::css { using ComputedStyle = std::unordered_map<std::string, std::string>; }
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
