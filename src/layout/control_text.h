#pragma once

#include "render/renderer.h"

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>
#include <vector>

// Text-editing primitives shared by ElInput and ElTextarea: mapping a mouse x
// back to a caret offset, stepping over UTF-8 characters, word boundaries, and
// the anchor/caret selection both controls carry.
//
// Both controls draw their value as a single measured text run per line, so the
// inverse of a click is the same in both: walk the run's UTF-8 boundaries and
// pick the one whose caret sits nearest the pointer.

namespace bro::layout {

// True if byte `i` starts a UTF-8 code point (the end of the string counts).
inline bool isUtf8Boundary(const std::string& s, size_t i) {
    if (i >= s.size()) return true;
    return (static_cast<unsigned char>(s[i]) & 0xC0) != 0x80;
}

// The offset one character before/after `pos`. Stepping by bytes would let a
// Backspace chop one byte off a multi-byte character and leave invalid UTF-8.
inline int utf8Prev(const std::string& s, int pos) {
    int i = std::clamp(pos, 0, static_cast<int>(s.size()));
    if (i <= 0) return 0;
    --i;
    while (i > 0 && !isUtf8Boundary(s, static_cast<size_t>(i))) --i;
    return i;
}

inline int utf8Next(const std::string& s, int pos) {
    const int n = static_cast<int>(s.size());
    int i = std::clamp(pos, 0, n);
    if (i >= n) return n;
    ++i;
    while (i < n && !isUtf8Boundary(s, static_cast<size_t>(i))) ++i;
    return i;
}

// Round an offset onto a UTF-8 character boundary — back to the character's
// first byte, or forward past its last. Offsets normally arrive
// boundary-aligned (the JS binding converts UTF-16 code units to byte offsets
// on whole characters), but snapping keeps a stray mid-character offset from
// splitting one and leaving invalid UTF-8 behind.
inline int utf8SnapBack(const std::string& s, int pos) {
    int i = std::clamp(pos, 0, static_cast<int>(s.size()));
    while (i > 0 && i < static_cast<int>(s.size()) &&
           !isUtf8Boundary(s, static_cast<size_t>(i))) --i;
    return i;
}

inline int utf8SnapFwd(const std::string& s, int pos) {
    const int n = static_cast<int>(s.size());
    int i = std::clamp(pos, 0, n);
    while (i < n && !isUtf8Boundary(s, static_cast<size_t>(i))) ++i;
    return i;
}

// Byte offset of codepoint index `cp` in `s` (clamped; cp < 0 means "end").
// SDL's TEXT_EDITING composition cursor counts UTF-8 characters, while the
// controls store byte offsets.
inline int utf8ByteForCodepoint(const std::string& s, int cp) {
    if (cp < 0) return static_cast<int>(s.size());
    int b = 0;
    while (cp > 0 && b < static_cast<int>(s.size())) { b = utf8Next(s, b); --cp; }
    return b;
}

// Character class for word selection. Non-ASCII bytes count as word characters
// so a double-click on an accented or CJK word takes the whole run.
inline bool isWordChar(unsigned char c) {
    return std::isalnum(c) || c == '_' || c >= 0x80;
}
inline bool isSpaceChar(unsigned char c) { return c == ' ' || c == '\t'; }

// The word surrounding `pos`, for double-click select. A run of word characters
// is one word; a click in a run of spaces takes that run, and a click in
// punctuation takes the punctuation run — which is how browsers behave.
inline void wordBoundsAt(const std::string& s, int pos, int& lo, int& hi) {
    const int n = static_cast<int>(s.size());
    if (n == 0) { lo = hi = 0; return; }
    pos = std::clamp(pos, 0, n);

    // A caret at the very end of the text belongs to the run behind it.
    const int probe = (pos == n) ? pos - 1 : pos;
    const unsigned char c = static_cast<unsigned char>(s[probe]);

    enum class Cls { Word, Space, Punct };
    auto classOf = [](unsigned char ch) {
        if (isWordChar(ch)) return Cls::Word;
        if (isSpaceChar(ch)) return Cls::Space;
        return Cls::Punct;
    };
    const Cls cls = classOf(c);

    lo = probe;
    while (lo > 0 && classOf(static_cast<unsigned char>(s[lo - 1])) == cls) --lo;
    hi = probe;
    while (hi < n && classOf(static_cast<unsigned char>(s[hi])) == cls) ++hi;
}

// A control's selection: `anchor` is the fixed end (pinned where the drag or
// shift-extend began), `caret` is the moving end and doubles as the cursor.
// Collapsed when the two coincide.
struct TextRange {
    int anchor = 0;
    int caret = 0;

    int start() const { return std::min(anchor, caret); }
    int end() const { return std::max(anchor, caret); }
    bool collapsed() const { return anchor == caret; }

    void collapseTo(int p) { anchor = caret = p; }
    void set(int s, int e) { anchor = s; caret = e; }
    void clampTo(int len) {
        anchor = std::clamp(anchor, 0, len);
        caret = std::clamp(caret, 0, len);
    }
};

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

// Width of text[start, offset) — how far into its run a caret sits.
inline float runWidthTo(const std::string& text, size_t start, size_t offset,
                        const render::FontRef& fr, render::Renderer* r) {
    if (!r || offset <= start) return 0.0f;
    return r->measureText(std::string_view(text).substr(start, offset - start), fr).width;
}

// The wash painted behind selected text. Themed off the control's accent so it
// reads in both color schemes; the text keeps its own color rather than
// inverting, which stays legible without a second text pass.
inline bromath::Color selectionFill(const bromath::Color& accent) {
    return bromath::Color{accent.r, accent.g, accent.b, 0.35f};
}

} // namespace bro::layout
