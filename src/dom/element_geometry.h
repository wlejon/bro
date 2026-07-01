#pragma once
#include "css/transform.h"

namespace bro::dom { class Element; }

namespace bro::dom {

/// A single absolute (document/screen-space) point.
struct AbsolutePoint {
    float x = 0, y = 0;
};

/// A single absolute (document/screen-space) axis-aligned rect.
struct AbsoluteRect {
    float x = 0, y = 0, width = 0, height = 0;
};

/// Composed CSS transform/perspective for an element's ancestor chain, plus
/// the untransformed accumulated content-box origin. This is the shared
/// building block behind getBoundingClientRect()'s CSSOM-correct math —
/// callers that only need a point or an axis-aligned rect should use the
/// wrappers below instead of consuming this directly.
///
/// `hasTransform` is false whenever no ancestor (including `el` itself) has
/// a CSS `transform` or `perspective` — the overwhelmingly common case —
/// letting callers skip matrix work entirely.
struct AbsoluteFrame {
    float ox = 0, oy = 0;
    htmlayout::css::Matrix3D transform;
    bool hasTransform = false;
};

/// Walks `el`'s layoutParent() (composed-tree) ancestor chain, accumulating
/// each ancestor's content-box origin (matching layoutBox().contentRect,
/// which is expressed in the parent's content-area coordinates) and
/// composing every ancestor's CSS transform + perspective into one 4x4
/// matrix. Returns an all-zero, untransformed frame for a null element.
AbsoluteFrame computeAbsoluteFrame(const Element* el);

/// Absolute, transform-correct top-left of `el`'s content box. This is the
/// point most engine-side callers actually need (gizmo/highlight/mouse
/// placement) — cheaper than the full AABB wrappers below when width/height
/// aren't required.
AbsolutePoint absoluteContentOrigin(const Element* el);

/// Absolute, transform-correct axis-aligned bounding box of `el`'s border
/// box. Matches CSSOM `getBoundingClientRect()` semantics: projects all four
/// border-box corners through the composed ancestor transform and returns
/// their AABB (exact for translate/scale, an approximation under
/// rotation/skew — same tradeoff browsers make). Returns {0,0,0,0} for a
/// null element or one with `display: none`.
AbsoluteRect absoluteBorderBox(const Element* el);

/// Same AABB projection as absoluteBorderBox(), but for the content box
/// (no padding/border) — for canvas/webgl element layout callbacks and
/// gizmo placement, which need content-box width/height rather than
/// border-box.
AbsoluteRect absoluteContentBox(const Element* el);

} // namespace bro::dom
