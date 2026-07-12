#pragma once

#include "render/renderer.h"

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

// Mapping a mouse x back to a caret offset, shared by ElInput and ElTextarea.
// Both controls draw their value as a single measured text run per line, so the
// inverse is the same in both: walk the run's UTF-8 boundaries and pick the one
// whose caret sits nearest the click.

namespace bro::layout {

// True if byte `i` starts a UTF-8 code point (the end of the string counts).
inline bool isUtf8Boundary(const std::string& s, size_t i) {
    if (i >= s.size()) return true;
    return (static_cast<unsigned char>(s[i]) & 0xC0) != 0x80;
}

// Caret offset (byte index into `text`) nearest to `x` within the run
// text[start, end). `x` is measured from the run's left edge, so callers must
// already have subtracted the draw origin and added back any scroll.
//
// Snaps to whole glyphs: clicking the left half of a character puts the caret
// before it, the right half after it — what every text field does.
inline int caretOffsetForX(const std::string& text, size_t start, size_t end,
                           float x, const render::FontRef& fr,
                           render::Renderer* r) {
    if (!r || end <= start) return static_cast<int>(start);
    if (x <= 0.0f) return static_cast<int>(start);

    // Candidate caret positions: every UTF-8 boundary in the run.
    std::vector<size_t> bounds;
    bounds.push_back(start);
    for (size_t i = start + 1; i < end; ++i) {
        if (isUtf8Boundary(text, i)) bounds.push_back(i);
    }
    bounds.push_back(end);

    auto widthTo = [&](size_t b) {
        return r->measureText(std::string_view(text).substr(start, b - start), fr).width;
    };

    // Prefix width is monotonic in the offset, so binary-search the last
    // boundary still left of x rather than measuring every prefix.
    size_t lo = 0, hi = bounds.size() - 1;
    if (x >= widthTo(bounds[hi])) return static_cast<int>(bounds[hi]);
    while (hi - lo > 1) {
        size_t mid = (lo + hi) / 2;
        if (widthTo(bounds[mid]) <= x) lo = mid;
        else hi = mid;
    }
    float wl = widthTo(bounds[lo]);
    float wh = widthTo(bounds[hi]);
    return static_cast<int>((x - wl <= wh - x) ? bounds[lo] : bounds[hi]);
}

} // namespace bro::layout
