#pragma once

// Shared fixture of the paint tests: a real bropty Terminal fed a byte
// string, its frame as TerminalView builds it, and fixed cell metrics.

#include "capture_renderer.h"
#include "terminal/term_paint.h"

#include <bropty/terminal.h>
#include <bropty/view.h>

#include <bromath/color.h>

#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <string_view>

namespace bro::terminal::test {

// Origin and metrics every paint test uses. A fractional advance, so the
// oracle also sees that glyphs sit at col * advance exactly while fills snap.
inline constexpr float kOriginX = 10.0f;
inline constexpr float kOriginY = 20.0f;

inline CellMetrics fixedMetrics() {
    CellMetrics m;
    m.cellW = 8.5f;
    m.cellH = 17.0f;
    m.ascent = 12.0f;
    m.descent = 4.0f;
    m.baseline = 13.0f;
    m.lineThickness = 1.0f;
    m.underlineY = 15.0f;
    m.strikeY = 9.5f;
    m.overlineY = 0.5f;
    return m;
}

inline render::FontRef fixedFont() { return render::FontRef{"TestMono", 14.0f, 400, false}; }

struct Fixture {
    std::unique_ptr<bropty::Terminal> term;
    std::unique_ptr<bropty::TerminalView> view;

    Fixture(int cols, int rows, std::string_view bytes) {
        term = std::make_unique<bropty::Terminal>(cols, rows, 100);
        term->feed(bytes);
        view = std::make_unique<bropty::TerminalView>(*term);
    }
    std::shared_ptr<const bropty::Frame> frame() { return view->snapshot(); }
};

inline bromath::Color rgbColor(bropty::Rgb c, float a = 1.0f) {
    bromath::Color out = bromath::cfromColor8({c.r, c.g, c.b, 255});
    out.a = a;
    return out;
}

inline bool sameColor(const bromath::Color& a, const bromath::Color& b) {
    return std::fabs(a.r - b.r) < 1e-4f && std::fabs(a.g - b.g) < 1e-4f && std::fabs(a.b - b.b) < 1e-4f &&
           std::fabs(a.a - b.a) < 1e-4f;
}

inline std::string describe(const bromath::Color& c) {
    char buf[96];
    std::snprintf(buf, sizeof buf, "(%.3f %.3f %.3f a%.2f)", double(c.r), double(c.g), double(c.b), double(c.a));
    return buf;
}

} // namespace bro::terminal::test
