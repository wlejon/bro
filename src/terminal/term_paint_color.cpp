// Cell colours as painted: the SGR rules against the palette, plus the
// element's colour policy (bold-is-bright, minimum contrast), and the
// overlay colours of the highlights.

#include "terminal/term_paint.h"

#include <brothemes/contrast.h>

#include <algorithm>
#include <cmath>
#include <utility>

namespace bro::terminal {

namespace {

bropty::Rgb mix(bropty::Rgb a, bropty::Rgb b, float t) {
    auto ch = [t](uint8_t x, uint8_t y) {
        return uint8_t(std::lround(float(x) + (float(y) - float(x)) * t));
    };
    return bropty::Rgb{ch(a.r, b.r), ch(a.g, b.g), ch(a.b, b.b)};
}

} // namespace

bropty::Rgb defaultBg(const bropty::Palette& pal, bool rv) { return rv ? pal.foreground : pal.background; }
bropty::Rgb defaultFg(const bropty::Palette& pal, bool rv) { return rv ? pal.background : pal.foreground; }

bropty::Rgb ensureContrast(bropty::Rgb fg, bropty::Rgb bg, float ratio) {
    if (!(ratio > 1.0f)) return fg;
    const themes::Color f(fg.r, fg.g, fg.b);
    const themes::Color b(bg.r, bg.g, bg.b);
    if (themes::wcag_contrast_ratio(f, b) >= ratio) return fg;
    const themes::Color out = themes::adjust_contrast(f, b, std::min(ratio, 21.0f));
    return bropty::Rgb{out.r, out.g, out.b};
}

// SGR semantics as painted:
//  * default colours come from the palette's defaults; under reverse video
//    (DECSCNM) the two defaults trade places, explicit colours do not;
//  * bold (SGR 1) is a heavier face; with policy.boldIsBright it also moves
//    the first eight palette colours to their bright twins (xterm), which
//    otherwise it does not (kitty, Alacritty and Ghostty's default);
//  * inverse (SGR 7) swaps the cell's resolved foreground and background;
//  * dim (SGR 2) draws the foreground halfway to the background;
//  * invisible (SGR 8) keeps the background and draws no glyph and no lines;
//  * last, policy.minimumContrast lifts a foreground too close to its
//    background (not a hidden one, and not the background itself).
ResolvedCell resolveCell(const bropty::Style& st, const bropty::Palette& pal, bool rv, const ColorPolicy& policy) {
    ResolvedCell rc;
    bropty::Color fgc = st.fg;
    if (policy.boldIsBright && st.has(bropty::Attr_Bold) && fgc.is_indexed() && fgc.index() < 8)
        fgc = bropty::Color::indexed(uint8_t(fgc.index() + 8));
    rc.fg = fgc.is_default() ? defaultFg(pal, rv) : pal.resolve_fg(fgc);
    rc.bg = st.bg.is_default() ? defaultBg(pal, rv) : pal.resolve_bg(st.bg);
    if (st.has(bropty::Attr_Inverse)) std::swap(rc.fg, rc.bg);
    if (st.has(bropty::Attr_Dim)) rc.fg = mix(rc.fg, rc.bg, 0.5f);
    rc.hidden = st.has(bropty::Attr_Invisible);
    if (!rc.hidden && policy.minimumContrast > 1.0f) rc.fg = ensureContrast(rc.fg, rc.bg, policy.minimumContrast);
    return rc;
}

bromath::Color toColor(bropty::Rgb c, float alpha) {
    bromath::Color out = bromath::cfromColor8({c.r, c.g, c.b, 255});
    out.a = alpha;
    return out;
}

bromath::Color highlightColor(bropty::HighlightKind kind, const HighlightColors& colors) {
    switch (kind) {
        case bropty::HighlightKind::Selection: return colors.selection;
        case bropty::HighlightKind::Match: return colors.match;
        case bropty::HighlightKind::CurrentMatch: return colors.currentMatch;
        case bropty::HighlightKind::Hover: return bromath::Color{0, 0, 0, 0};
    }
    return bromath::Color{0, 0, 0, 0};
}

} // namespace bro::terminal
