#pragma once

#include <string>
#include <unordered_map>

namespace htmlayout::css { using ComputedStyle = std::unordered_map<std::string, std::string>; }
namespace bro::dom { class Element; }

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

} // namespace bro::engine
