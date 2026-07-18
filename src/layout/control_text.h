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
// Snaps to whole clusters: clicking the left half of a cluster puts the caret
// before it, the right half after it — what every text field does — and never
// lands inside one, where there is no glyph geometry to draw a caret against.
int caretOffsetForX(const std::string& text, size_t start, size_t end,
                    float x, const render::FontRef& fr, render::Renderer* r);

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
                  size_t offset, const render::FontRef& fr, render::Renderer* r);

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
                int pos, const render::FontRef& fr, render::Renderer* r);
int clusterNext(const std::string& text, size_t start, size_t end,
                int pos, const render::FontRef& fr, render::Renderer* r);

// The logical line (between hard newlines) containing `pos`. Clusters never
// span a line break, and a newline is not a character any shaper should be
// asked about, so caret stepping shapes one line at a time.
inline void logicalLineBounds(const std::string& text, int pos,
                              size_t& lo, size_t& hi) {
    const size_t n = text.size();
    size_t p = static_cast<size_t>(std::clamp(pos, 0, static_cast<int>(n)));
    size_t a = text.rfind('\n', p == 0 ? 0 : p - 1);
    lo = (a == std::string::npos || p == 0) ? 0 : a + 1;
    size_t b = text.find('\n', p);
    hi = (b == std::string::npos) ? n : b;
    if (hi < lo) hi = lo;
}

// The wash painted behind selected text. Themed off the control's accent so it
// reads in both color schemes; the text keeps its own color rather than
// inverting, which stays legible without a second text pass.
inline bromath::Color selectionFill(const bromath::Color& accent) {
    return bromath::Color{accent.r, accent.g, accent.b, 0.35f};
}

} // namespace bro::layout
