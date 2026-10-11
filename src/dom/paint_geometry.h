#pragma once

#include "layout/box.h"

namespace bro::dom {

// Whether a new layout box paints differently from the one it replaces:
// what the draw traversal reads of a box (its rects and edges, the placed
// text runs, truncation and clamping, the scroll extent the clamped scroll
// offset depends on). A box laid out again to the same geometry keeps the
// paint caches above it (Element::notePaintChanged).
inline bool paintGeometryDiffers(const htmlayout::layout::LayoutBox& a,
                                 const htmlayout::layout::LayoutBox& b) {
    auto rectNe = [](const htmlayout::layout::Rect& p, const htmlayout::layout::Rect& q) {
        return p.x != q.x || p.y != q.y || p.width != q.width || p.height != q.height;
    };
    auto edgesNe = [](const htmlayout::layout::Edges& p, const htmlayout::layout::Edges& q) {
        return p.top != q.top || p.right != q.right || p.bottom != q.bottom || p.left != q.left;
    };
    if (rectNe(a.contentRect, b.contentRect) || edgesNe(a.padding, b.padding) ||
        edgesNe(a.border, b.border) || edgesNe(a.margin, b.margin))
        return true;
    if (a.naturalWidth != b.naturalWidth || a.naturalHeight != b.naturalHeight ||
        a.textTruncated != b.textTruncated || a.clampHidden != b.clampHidden ||
        a.baselineOffset != b.baselineOffset)
        return true;
    if (a.textRuns.size() != b.textRuns.size()) return true;
    for (size_t i = 0; i < a.textRuns.size(); ++i) {
        const auto& p = a.textRuns[i];
        const auto& q = b.textRuns[i];
        if (p.x != q.x || p.y != q.y || p.width != q.width || p.height != q.height ||
            p.srcStart != q.srcStart || p.srcEnd != q.srcEnd || p.text != q.text)
            return true;
    }
    return false;
}

}  // namespace bro::dom
