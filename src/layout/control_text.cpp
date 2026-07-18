#include "layout/control_text.h"

#include "layout/cluster_caret.h"
#include "render/shaped_run.h"

// The caret geometry that needs the shaper lives here rather than in the
// header: shaped_run.h pulls in Skia's headers, and control_text.h is included
// (through pseudo_style.h) by the DOM layer, which is deliberately built
// without them. Keeping these four definitions out of line keeps that
// separation intact.

namespace bro::layout {

namespace {

// Shape the run text[start, end) — the exact string the control draws as one
// piece — or null when there is no shaper or the cluster path is off. The run
// belongs to the renderer's cache and is invalidated by the next shaping miss,
// so callers use it and drop it.
const render::ShapedRun* shapedRunFor(const std::string& text,
                                      size_t start, size_t end,
                                      const render::FontRef& fr,
                                      render::Renderer* r) {
    if (!r || !clusterCaretEnabled() || end <= start || end > text.size()) return nullptr;
    return r->shapeText(std::string_view(text).substr(start, end - start), fr);
}

} // namespace

// Caret offset (byte index into `text`) nearest to `x` within the run
// text[start, end). `x` is measured from the run's left edge, so callers must
// already have subtracted the draw origin and added back any scroll.
//
// Snaps to whole glyphs: clicking the left half of a character puts the caret
// before it, the right half after it — what every text field does.
int caretOffsetForX(const std::string& text, size_t start, size_t end,
                           float x, const render::FontRef& fr,
                           render::Renderer* r) {
    if (!r || end <= start) return static_cast<int>(start);
    if (x <= 0.0f) return static_cast<int>(start);

    // With a shaper, the run's cluster map names every legal caret site
    // directly — including the ones prefix widths cannot see, like the two
    // edges of a ligature that has no interior geometry to point at.
    if (const render::ShapedRun* run = shapedRunFor(text, start, end, fr, r)) {
        return static_cast<int>(start + run->xToByteOffset(x));
    }

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

// How far into its run a caret at `offset` sits, as an x offset from the run's
// left edge.
//
// `end` is the end of the run as drawn, and it matters: the caret's x is a
// position *inside* one shaped piece of text, not the width of a shorter piece
// measured on its own. Those differ whenever a glyph's advance depends on its
// neighbour — the last character before the caret kerns against the one after
// it, and measuring the prefix alone drops that kern. Splitting a run at the
// caret and measuring the left half is exactly the mistake this signature
// exists to prevent.
float caretXInRun(const std::string& text, size_t start, size_t end,
                         size_t offset, const render::FontRef& fr,
                         render::Renderer* r) {
    if (!r || offset <= start) return 0.0f;
    end = std::min(end, text.size());
    if (offset >= end) offset = end;
    if (const render::ShapedRun* run = shapedRunFor(text, start, end, fr, r)) {
        return run->byteOffsetToX(offset - start).primary.x;
    }
    return r->measureText(std::string_view(text).substr(start, offset - start), fr).width;
}

// The caret offset one cluster before/after `pos` within the run
// text[start, end).
//
// A cluster, not a character: a ligature is one glyph for several characters
// and has no geometry inside it, so a caret placed there would be drawn at the
// cluster's leading edge — the arrow key would appear to do nothing, and a
// second press would appear to skip two. Stepping by cluster keeps movement
// and geometry telling the same story. Indic syllables and combining marks
// behave the same way for the same reason. Falls back to per-character steps
// when there is no shaper.
int clusterPrev(const std::string& text, size_t start, size_t end,
                       int pos, const render::FontRef& fr, render::Renderer* r) {
    const int lo = static_cast<int>(start);
    if (pos <= lo) return lo;
    if (const render::ShapedRun* run = shapedRunFor(text, start, end, fr, r)) {
        const int probe = std::min(pos, static_cast<int>(end)) - 1;
        auto span = run->clusterRange(static_cast<size_t>(probe) - start);
        const int s = lo + static_cast<int>(span.byteStart);
        if (s < pos) return s;
    }
    return utf8Prev(text, pos);
}

int clusterNext(const std::string& text, size_t start, size_t end,
                       int pos, const render::FontRef& fr, render::Renderer* r) {
    const int hi = static_cast<int>(std::min(end, text.size()));
    if (pos >= hi) return hi;
    if (const render::ShapedRun* run = shapedRunFor(text, start, end, fr, r)) {
        auto span = run->clusterRange(static_cast<size_t>(pos) - start);
        const int e = static_cast<int>(start) + static_cast<int>(span.byteEnd);
        if (e > pos) return std::min(e, hi);
    }
    return utf8Next(text, pos);
}

} // namespace bro::layout
