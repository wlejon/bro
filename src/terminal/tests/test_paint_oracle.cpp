// The paint oracle: what TermPainter draws, decoded back into cells from the
// captured renderer calls, against what bropty's own snapshot of the same
// terminal says each cell is. The expectation side reads only the Frame
// (cells, styles, clusters, palette, modes) and spells the SGR colour rules
// out itself; the decoding side reads only the captured calls. The two meet
// cell by cell: glyph text, foreground, face (bold/italic), the stack of
// fills under the cell's centre (default background, cell background,
// selection), and each decoration (underline style and colour,
// strikethrough, overline).

#include "check.h"
#include "paint_fixture.h"
#include "tests.h"

#include <bropty/cell.h>
#include <bropty/search_regex.h>
#include <bropty/unicode.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <map>
#include <optional>
#include <set>
#include <vector>

namespace bro::terminal::test {

namespace {

constexpr char32_t kImageCell = 0x10EEEE;

std::string utf8(std::u32string_view s) {
    std::string out;
    for (char32_t c : s) bropty::append_utf8(out, c);
    return out;
}

std::u32string decodeUtf8(std::string_view s) {
    std::u32string out;
    for (size_t i = 0; i < s.size();) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        int n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : 4;
        char32_t cp = n == 1 ? c : n == 2 ? (c & 0x1F) : n == 3 ? (c & 0x0F) : (c & 0x07);
        for (int k = 1; k < n && i + size_t(k) < s.size(); ++k) cp = (cp << 6) | (char32_t(s[i + size_t(k)]) & 0x3F);
        out.push_back(cp);
        i += size_t(n);
    }
    return out;
}

// Split drawn text into grapheme-ish clusters: a code point joins the one
// before it when it is zero-width (combining marks, variation selectors,
// ZWJ), follows a ZWJ, is an emoji modifier, or completes a regional
// indicator pair. Enough for the corpus below.
std::vector<std::u32string> clustersOf(const std::u32string& s) {
    std::vector<std::u32string> out;
    bool afterZwj = false;
    int riRun = 0;
    for (char32_t cp : s) {
        const bool ri = bropty::unicode::is_regional_indicator(cp);
        const bool join = !out.empty() &&
                          (bropty::unicode::is_zero_width(cp) || afterZwj ||
                           bropty::unicode::is_emoji_modifier(cp) || (ri && riRun % 2 == 1));
        if (join) out.back().push_back(cp);
        else out.push_back(std::u32string(1, cp));
        afterZwj = cp == 0x200D;
        riRun = ri ? riRun + 1 : 0;
    }
    return out;
}

// What one cell looks like, from either side.
struct CellLook {
    std::u32string text;            // empty: no glyph
    bromath::Color fg{};
    bool bold = false;
    bool italic = false;
    std::vector<bromath::Color> fills;  // bottom to top
    bropty::Underline underline = bropty::Underline::None;
    bromath::Color underlineColor{};
    std::optional<bromath::Color> strike;
    std::optional<bromath::Color> over;
};

using Grid = std::vector<std::vector<CellLook>>;

// ---- expectation: the Frame ------------------------------------------------

// `highlighted`: the overlay each highlighted cell carries (selection, match).
Grid expected(const bropty::Frame& f, const std::map<std::pair<int, int>, bropty::HighlightKind>& highlighted,
              bool boldIsBright = false) {
    const bropty::Palette& pal = *f.palette;
    const bool rv = f.modes.reverse_video;
    const bropty::Rgb defFg = rv ? pal.background : pal.foreground;
    const bropty::Rgb defBg = rv ? pal.foreground : pal.background;
    Grid g(size_t(f.rows), std::vector<CellLook>(size_t(f.cols)));
    for (int y = 0; y < f.rows; ++y) {
        const bropty::RowView v = f.lines[size_t(y)]->view();
        for (int x = 0; x < f.cols; ++x) {
            CellLook& c = g[size_t(y)][size_t(x)];
            const bool tail = v[x].wide() == bropty::Wide::SpacerTail && x > 0;
            const int sx = tail ? x - 1 : x;  // the cell whose style paints here
            const bropty::Style& st = v.style(sx);
            bropty::Color fgc = st.fg;
            // xterm's boldColors: bold in one of the eight base colours is its bright twin.
            if (boldIsBright && st.has(bropty::Attr_Bold) && fgc.is_indexed() && fgc.index() < 8)
                fgc = bropty::Color::indexed(uint8_t(fgc.index() + 8));
            bropty::Rgb fg = fgc.is_default() ? defFg : pal.resolve_fg(fgc);
            bropty::Rgb bg = st.bg.is_default() ? defBg : pal.resolve_bg(st.bg);
            if (st.has(bropty::Attr_Inverse)) std::swap(fg, bg);
            if (st.has(bropty::Attr_Dim)) {
                auto half = [](uint8_t a, uint8_t b) { return uint8_t(std::lround((int(a) + int(b)) / 2.0)); };
                fg = bropty::Rgb{half(fg.r, bg.r), half(fg.g, bg.g), half(fg.b, bg.b)};
            }
            const bool hidden = st.has(bropty::Attr_Invisible);

            c.fills.push_back(rgbColor(defBg));
            if (!(bg == defBg)) c.fills.push_back(rgbColor(bg));
            if (auto h = highlighted.find({y, x}); h != highlighted.end()) c.fills.push_back(highlightColor(h->second));

            const char32_t cp = v[sx].cp();
            if (!tail && cp != 0 && cp != U' ' && cp != kImageCell && !hidden) {
                std::u32string cl = v.cluster(x);
                c.text = cl.empty() ? std::u32string(1, cp) : cl;
                c.fg = rgbColor(fg);
                c.bold = st.has(bropty::Attr_Bold);
                c.italic = st.has(bropty::Attr_Italic);
            }
            if (!hidden) {
                c.underline = st.underline;
                if (st.underline != bropty::Underline::None)
                    c.underlineColor = rgbColor(st.underline_color.is_default() ? fg : pal.resolve_fg(st.underline_color));
                if (st.has(bropty::Attr_Strike)) c.strike = rgbColor(fg);
                if (st.has(bropty::Attr_Overline)) c.over = rgbColor(fg);
            }
        }
    }
    return g;
}

// ---- decoding: the captured calls --------------------------------------------

struct Decoded {
    Grid grid;
    std::vector<std::string> errors;  // calls that do not land on the grid
};

Decoded decode(const std::vector<Op>& ops, int cols, int rows, const CellMetrics& m) {
    Decoded d;
    d.grid.assign(size_t(rows), std::vector<CellLook>(size_t(cols)));
    auto cellOk = [&](int y, int x) { return y >= 0 && y < rows && x >= 0 && x < cols; };
    std::set<std::pair<int, int>> glyphSeen;

    struct UnderPiece {
        float y, len;
        bromath::Color color;
    };
    std::map<std::pair<int, int>, std::vector<UnderPiece>> under;
    std::map<std::pair<int, int>, float> underCovered;

    for (const Op& op : ops) {
        switch (op.kind) {
            case Op::Fill: {
                for (int y = 0; y < rows; ++y)
                    for (int x = 0; x < cols; ++x) {
                        const float cx = kOriginX + (float(x) + 0.5f) * m.cellW;
                        const float cy = kOriginY + (float(y) + 0.5f) * m.cellH;
                        if (cx >= op.x && cx < op.x + op.w && cy >= op.y && cy < op.y + op.h)
                            d.grid[size_t(y)][size_t(x)].fills.push_back(op.color);
                    }
                break;
            }
            case Op::Text: {
                const float fx = (op.x - kOriginX) / m.cellW;
                const float fy = (op.y - kOriginY - m.baseline) / m.cellH;
                int x = int(std::lround(fx));
                const int y = int(std::lround(fy));
                if (std::fabs(fx - float(x)) > 1e-3f || std::fabs(fy - float(y)) > 1e-3f) {
                    d.errors.push_back("text off the cell grid: " + op.text);
                    break;
                }
                for (const std::u32string& cl : clustersOf(decodeUtf8(op.text))) {
                    const int w = std::max(1, bropty::unicode::cluster_width(cl));
                    if (!cellOk(y, x)) {
                        d.errors.push_back("text outside the grid: " + op.text);
                        break;
                    }
                    if (!(cl.size() == 1 && cl[0] == U' ')) {
                        if (!glyphSeen.insert({y, x}).second) d.errors.push_back("cell drawn twice: " + utf8(cl));
                        CellLook& c = d.grid[size_t(y)][size_t(x)];
                        c.text = cl;
                        c.fg = op.color;
                        c.bold = op.weight >= 700;
                        c.italic = op.italic;
                        if (op.family != "TestMono" || op.size != 14.0f)
                            d.errors.push_back("text not in the element's font: " + op.family);
                    }
                    x += w;
                }
                break;
            }
            case Op::Line: {
                if (std::fabs(op.y - op.y2) > 1e-4f) {
                    d.errors.push_back("non-horizontal line");
                    break;
                }
                const int y = int(std::floor((op.y - kOriginY) / m.cellH));
                const float rel = op.y - (kOriginY + float(y) * m.cellH);
                const float x0 = std::min(op.x, op.x2), x1 = std::max(op.x, op.x2);
                for (int x = 0; x < cols; ++x) {
                    const float cx0 = kOriginX + float(x) * m.cellW, cx1 = cx0 + m.cellW;
                    const float len = std::min(x1, cx1) - std::max(x0, cx0);
                    if (len <= 1e-3f || !cellOk(y, x)) continue;
                    CellLook& c = d.grid[size_t(y)][size_t(x)];
                    if (std::fabs(rel - m.strikeY) < 1e-3f) c.strike = op.color;
                    else if (std::fabs(rel - m.overlineY) < 1e-3f) c.over = op.color;
                    else {
                        under[{y, x}].push_back(UnderPiece{op.y, len, op.color});
                        underCovered[{y, x}] += len;
                    }
                }
                break;
            }
            case Op::Path: {
                // "M x0 y Q ... x1 y": the wave spans [x0, x1).
                std::vector<float> nums;
                const char* p = op.text.c_str();
                while (*p) {
                    if ((*p >= '0' && *p <= '9') || *p == '-' || *p == '.') {
                        char* end = nullptr;
                        nums.push_back(std::strtof(p, &end));
                        p = end;
                    } else {
                        ++p;
                    }
                }
                if (nums.size() < 4) break;
                const float x0 = nums[0], y0 = nums[1], x1 = nums[nums.size() - 2];
                const int y = int(std::floor((y0 - kOriginY) / m.cellH));
                for (int x = 0; x < cols; ++x) {
                    const float cx = kOriginX + (float(x) + 0.5f) * m.cellW;
                    if (cx < x0 || cx >= x1 || !cellOk(y, x)) continue;
                    CellLook& c = d.grid[size_t(y)][size_t(x)];
                    c.underline = bropty::Underline::Curly;
                    c.underlineColor = op.color;
                }
                break;
            }
            default:
                break;
        }
    }
    for (auto& [pos, pieces] : under) {
        CellLook& c = d.grid[size_t(pos.first)][size_t(pos.second)];
        std::set<int> ys;
        float maxLen = 0;
        for (const UnderPiece& u : pieces) {
            ys.insert(int(std::lround(u.y * 100)));
            maxLen = std::max(maxLen, u.len);
        }
        c.underlineColor = pieces.front().color;
        if (ys.size() >= 2) c.underline = bropty::Underline::Double;
        else if (underCovered[pos] >= m.cellW - 1e-3f) c.underline = bropty::Underline::Single;
        else if (maxLen <= m.lineThickness * 1.5f) c.underline = bropty::Underline::Dotted;
        else c.underline = bropty::Underline::Dashed;
    }
    return d;
}

// ---- comparison --------------------------------------------------------------

void compare(const char* name, const Grid& want, const Decoded& got) {
    for (const std::string& e : got.errors) CHECK_MSG(false, std::string(name) + ": " + e);
    int mismatches = 0;
    auto fail = [&](int y, int x, const std::string& what) {
        if (++mismatches > 12) return;
        CHECK_MSG(false, std::string(name) + " cell (" + std::to_string(y) + "," + std::to_string(x) + "): " + what);
    };
    for (size_t y = 0; y < want.size(); ++y) {
        for (size_t x = 0; x < want[y].size(); ++x) {
            const CellLook& w = want[y][x];
            const CellLook& g = got.grid[y][x];
            const int yi = int(y), xi = int(x);
            if (w.text != g.text) fail(yi, xi, "glyph '" + utf8(g.text) + "', want '" + utf8(w.text) + "'");
            else if (!w.text.empty()) {
                if (!sameColor(w.fg, g.fg)) fail(yi, xi, "fg " + describe(g.fg) + ", want " + describe(w.fg));
                if (w.bold != g.bold) fail(yi, xi, "bold mismatch");
                if (w.italic != g.italic) fail(yi, xi, "italic mismatch");
            }
            bool fillsOk = w.fills.size() == g.fills.size();
            for (size_t i = 0; fillsOk && i < w.fills.size(); ++i) fillsOk = sameColor(w.fills[i], g.fills[i]);
            if (!fillsOk) {
                std::string s = "fills [";
                for (auto& c : g.fills) s += describe(c);
                s += "] want [";
                for (auto& c : w.fills) s += describe(c);
                fail(yi, xi, s + "]");
            }
            if (w.underline != g.underline)
                fail(yi, xi, "underline style " + std::to_string(int(g.underline)) + ", want " +
                                 std::to_string(int(w.underline)));
            else if (w.underline != bropty::Underline::None && !sameColor(w.underlineColor, g.underlineColor))
                fail(yi, xi, "underline colour " + describe(g.underlineColor) + ", want " + describe(w.underlineColor));
            if (w.strike.has_value() != g.strike.has_value() || (w.strike && !sameColor(*w.strike, *g.strike)))
                fail(yi, xi, "strikethrough mismatch");
            if (w.over.has_value() != g.over.has_value() || (w.over && !sameColor(*w.over, *g.over)))
                fail(yi, xi, "overline mismatch");
        }
    }
    if (mismatches > 12) CHECK_MSG(false, std::string(name) + ": " + std::to_string(mismatches) + " mismatches in all");
    if (mismatches == 0) CHECK(true);
}

struct Case {
    const char* name;
    std::string bytes;
    int cols = 40;
    int rows = 6;
    bool boldIsBright = false;
    std::optional<bropty::Palette> palette;  // set as the base palette after the bytes
};

// A theme the way the element sets one from CSS or JS: every base colour and
// the defaults moved off the standard palette.
bropty::Palette themedPalette() {
    bropty::Palette p = bropty::Palette::standard();
    for (int i = 0; i < 16; ++i) p.colors[size_t(i)] = bropty::Rgb{uint8_t(20 + i * 13), uint8_t(200 - i * 9), uint8_t(i * 15)};
    p.colors[200] = bropty::Rgb{1, 2, 3};
    p.foreground = bropty::Rgb{230, 220, 200};
    p.background = bropty::Rgb{40, 30, 60};
    return p;
}

std::vector<Case> corpus() {
    std::vector<Case> c;
    c.push_back({"plain", "hello world\r\nsecond  line, spaced   out\r\n\ttab"});
    {
        std::string s;
        for (int i = 30; i <= 37; ++i) s += "\x1b[" + std::to_string(i) + "mF" + std::to_string(i - 30);
        s += "\x1b[0m\r\n";
        for (int i = 90; i <= 97; ++i) s += "\x1b[" + std::to_string(i) + "mB" + std::to_string(i - 90);
        s += "\x1b[0m\r\n";
        for (int i = 40; i <= 47; ++i) s += "\x1b[" + std::to_string(i) + "m b";
        s += "\x1b[0m\r\n";
        for (int i = 100; i <= 107; ++i) s += "\x1b[" + std::to_string(i) + "m B";
        s += "\x1b[0m\r\n\x1b[1;31mbold red is not bright\x1b[0m";
        c.push_back({"sgr16", s});
    }
    {
        std::string s;
        for (int n : {0, 9, 16, 21, 46, 124, 196, 202, 231, 232, 244, 255})
            s += "\x1b[38;5;" + std::to_string(n) + "mX\x1b[48;5;" + std::to_string(n) + "m \x1b[0m";
        s += "\r\n\x1b[38:5:208mcolon form\x1b[0m";
        c.push_back({"sgr256", s});
    }
    c.push_back({"truecolor",
                 "\x1b[38;2;255;128;0morange\x1b[0m \x1b[48;2;0;64;128mon blue\x1b[0m "
                 "\x1b[38;2;1;2;3;48;2;250;251;252mboth\x1b[0m\r\n\x1b[38:2::10:20:30mcolon\x1b[0m"});
    c.push_back({"attributes",
                 "\x1b[1mbold\x1b[0m \x1b[3mitalic\x1b[0m \x1b[1;3mboth\x1b[0m \x1b[2mdim\x1b[0m "
                 "\x1b[2;31mdimred\x1b[0m\r\n\x1b[7minverse\x1b[0m \x1b[7;32;44minvcol\x1b[0m "
                 "\x1b[8msecret\x1b[0m|\x1b[9mstrike\x1b[0m \x1b[53mover\x1b[0m\r\n"
                 "\x1b[2;7mdiminv\x1b[0m \x1b[8;4;9mhidden lines\x1b[0m"});
    c.push_back({"underlines",
                 "\x1b[4msingle\x1b[0m \x1b[4:2mdouble\x1b[0m \x1b[21mdbl21\x1b[0m\r\n"
                 "\x1b[4:3mcurly wave\x1b[0m \x1b[4:4mdotted\x1b[0m\r\n\x1b[4:5mdashed line\x1b[0m "
                 "\x1b[4;58;2;255;0;0mred under\x1b[0m\r\n\x1b[4:3;58;5;46mgreen curly\x1b[0m "
                 "\x1b[4;33mfg under\x1b[0m\r\n\x1b[4m  \x1b[0m<-underlined blanks"});
    c.push_back({"wide and combining",
                 "CJK \xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e end\r\n"
                 "emoji \xf0\x9f\x98\x80 flag \xf0\x9f\x87\xaf\xf0\x9f\x87\xb5 x\r\n"
                 "zwj \xf0\x9f\x91\xa8\xe2\x80\x8d\xf0\x9f\x91\xa9\xe2\x80\x8d\xf0\x9f\x91\xa7 y\r\n"
                 "comb e\xcc\x81 a\xcc\x88o\xcc\x83 heart \xe2\x9d\xa4\xef\xb8\x8f z\r\n"
                 "\x1b[4;41m\xe5\xae\xbd\xe5\xad\x97\x1b[0m wide styled"});
    c.push_back({"batching",
                 "ab\x1b[31mcd\x1b[0mef \x1b[1mgh\x1b[0m\r\n\xe2\x94\x8c\xe2\x94\x80\xe2\x94\x80\xe2\x94\x90 box "
                 "\xce\xb1\xce\xb2\xce\xb3 \xd0\xb4\xd0\xb0 caf\xc3\xa9\r\nx      y"});
    c.push_back({"reverse video",
                 "\x1b[?5hdefault \x1b[31mred\x1b[0m \x1b[44mbluebg\x1b[0m \x1b[7minv\x1b[0m"});
    c.push_back({"scrolled", "1\r\n2\r\n3\r\n4\r\n5\r\n6\r\n7\r\n\x1b[42m8 green\x1b[0m\r\n9", 20, 4});
    const std::string themed =
        "\x1b[31mred\x1b[0m \x1b[1;32mboldgreen\x1b[0m \x1b[1;38;5;9mbold9\x1b[0m\r\n"
        "\x1b[1;34;43mbold blue on yellow\x1b[0m \x1b[38;5;200mx200\x1b[0m\r\n"
        "\x1b[1;7;35minv bold\x1b[0m \x1b[1;2;36mdim bold\x1b[0m \x1b[1;38;2;1;2;3mrgb\x1b[0m plain";
    c.push_back({"palette", themed, 40, 4, false, themedPalette()});
    c.push_back({"bold is bright", themed, 40, 4, true});
    c.push_back({"bold is bright, palette", themed, 40, 4, true, themedPalette()});
    return c;
}

void runCase(const Case& c) {
    Fixture fx(c.cols, c.rows, c.bytes);
    if (c.palette) fx.term->set_base_palette(*c.palette);
    auto frame = fx.frame();
    if (c.palette) {
        CHECK_MSG(frame->palette->colors[1] == c.palette->colors[1] &&
                      frame->palette->background == c.palette->background,
                  std::string(c.name) + ": the frame carries the base palette");
    }
    CaptureRenderer r;
    TermPainter painter;
    PaintOptions opts;
    opts.font = fixedFont();
    opts.focused = false;  // the cursor is a stroked rect, out of the cell decoding's way
    opts.colors.boldIsBright = c.boldIsBright;
    const CellMetrics m = fixedMetrics();
    painter.paint(&r, *frame, kOriginX, kOriginY, float(c.cols) * m.cellW, float(c.rows) * m.cellH, m, opts);
    compare(c.name, expected(*frame, {}, c.boldIsBright), decode(r.ops, frame->cols, frame->rows, m));
}

// WCAG 2 contrast ratio, spelled out here rather than taken from brothemes.
// bromath colours are linear light already (cfromColor8 decodes sRGB).
double contrastRatio(const bromath::Color& a, const bromath::Color& b) {
    auto lum = [](const bromath::Color& c) { return 0.2126 * c.r + 0.7152 * c.g + 0.0722 * c.b; };
    const double la = lum(a), lb = lum(b);
    return (std::max(la, lb) + 0.05) / (std::min(la, lb) + 0.05);
}

void runMinimumContrast() {
    section("paint oracle: minimum contrast");
    // Low-contrast pairs (lifted) beside ones already readable (kept).
    Fixture fx(40, 3,
               "\x1b[38;2;40;40;40;48;2;30;30;30mdark on dark\x1b[0m \x1b[38;2;200;200;210;48;2;230;230;230mpale\x1b[0m\r\n"
               "\x1b[38;2;250;250;250;48;2;0;0;0mwhite on black\x1b[0m \x1b[34;44msame\x1b[0m");
    auto frame = fx.frame();
    const CellMetrics m = fixedMetrics();
    for (float ratio : {4.5f, 7.0f}) {
        CaptureRenderer off, on;
        TermPainter p0, p1;
        PaintOptions opts;
        opts.font = fixedFont();
        p0.paint(&off, *frame, kOriginX, kOriginY, 40 * m.cellW, 3 * m.cellH, m, opts);
        opts.colors.minimumContrast = ratio;
        p1.paint(&on, *frame, kOriginX, kOriginY, 40 * m.cellW, 3 * m.cellH, m, opts);
        const Decoded plain = decode(off.ops, frame->cols, frame->rows, m);
        const Decoded lifted = decode(on.ops, frame->cols, frame->rows, m);
        int glyphs = 0, changed = 0;
        for (int y = 0; y < frame->rows; ++y)
            for (int x = 0; x < frame->cols; ++x) {
                const CellLook& a = plain.grid[size_t(y)][size_t(x)];
                const CellLook& b = lifted.grid[size_t(y)][size_t(x)];
                if (b.text.empty()) continue;
                ++glyphs;
                const bromath::Color bg = b.fills.back();
                const double before = contrastRatio(a.fg, bg), after = contrastRatio(b.fg, bg);
                const std::string where = "(" + std::to_string(y) + "," + std::to_string(x) + ") ratio " +
                                          std::to_string(ratio);
                CHECK_MSG(a.text == b.text && b.fills.size() == a.fills.size(), "same glyphs and fills " + where);
                // Where the ratio is out of reach (7:1 on a mid blue), the best there is: white or black.
                const double reach = std::min(double(ratio), std::max(contrastRatio({1, 1, 1, 1}, bg),
                                                                      contrastRatio({0, 0, 0, 1}, bg)));
                CHECK_MSG(after >= reach - 0.05, "lifted to " + std::to_string(after) + " " + where +
                                                             " fg " + describe(a.fg) + " -> " + describe(b.fg));
                if (before >= double(ratio)) CHECK_MSG(sameColor(a.fg, b.fg), "readable colours kept " + where);
                else ++changed;
            }
        CHECK_MSG(glyphs == 30 && changed >= 14, std::to_string(glyphs) + " glyphs, " + std::to_string(changed) + " lifted");
    }
}

void runLetterSpacing() {
    section("paint oracle: letter-spacing and device-pixel cells");
    CaptureRenderer measure;
    measure.advance = 8.3f;
    // letter-spacing 1.5px at a render scale of 2: the cell is the advance
    // plus the spacing, snapped to half-pixels; the glyphs still sit one per cell.
    const CellMetrics m = CellMetrics::measure(&measure, fixedFont(), 0.0f, 1.5f, 2.0f);
    CHECK_MSG(std::fabs(m.cellW * 2.0f - std::round(m.cellW * 2.0f)) < 1e-4f && std::fabs(m.cellW - 9.8f) <= 0.25f,
              "cellW " + std::to_string(m.cellW));
    CHECK(std::fabs(m.cellH * 2.0f - std::round(m.cellH * 2.0f)) < 1e-4f);
    CHECK_MSG(std::fabs(m.letterSpacing - (m.cellW - m.advance)) < 1e-4f, "spacing " + std::to_string(m.letterSpacing));
    CHECK(m.pixelWidth() == int(std::lround(m.cellW * 2.0f)));
    Fixture fx(30, 2, "spaced \x1b[31mletters\x1b[0m here\r\n\x1b[1mbold\x1b[0m and \xe6\x97\xa5\xe6\x9c\xac wide");
    auto frame = fx.frame();
    CaptureRenderer r;
    TermPainter painter;
    PaintOptions opts;
    opts.font = fixedFont();
    painter.paint(&r, *frame, kOriginX, kOriginY, 30 * m.cellW, 2 * m.cellH, m, opts);
    compare("letter-spacing", expected(*frame, {}), decode(r.ops, frame->cols, frame->rows, m));
}

void runSearchHighlights() {
    section("paint oracle: search matches");
    Fixture fx(30, 4, "find the cat, the CAT\r\nand concatenate cat\r\nno match here");
    auto& search = fx.view->search();
    std::string err;
    search.start(bropty::RegexMatcher::create("cat", bropty::RegexSearchOptions{}, &err));
    while (search.step()) {
    }
    CHECK_MSG(search.size() == 4, "matches " + std::to_string(search.size()) + err);  // smart case: CAT too
    fx.view->search_next(false);
    auto frame = fx.frame();
    std::map<std::pair<int, int>, bropty::HighlightKind> lit;
    int current = 0;
    for (const bropty::Highlight& h : frame->highlights)
        for (int x = h.col0; x < h.col1; ++x) {
            lit[{h.y, x}] = h.kind;
            if (h.kind == bropty::HighlightKind::CurrentMatch) ++current;
        }
    // The oracle's own scan of the screen text: every "cat", any case.
    std::set<std::pair<int, int>> want;
    for (int y = 0; y < frame->rows; ++y) {
        std::string row = frame->lines[size_t(y)]->view().text();
        for (char& ch : row) ch = char(std::tolower(static_cast<unsigned char>(ch)));
        for (size_t at = row.find("cat"); at != std::string::npos; at = row.find("cat", at + 1))
            for (int k = 0; k < 3; ++k) want.insert({y, int(at) + k});
    }
    std::set<std::pair<int, int>> got;
    for (auto& [cell, kind] : lit) got.insert(cell);
    CHECK_MSG(got == want, "highlighted " + std::to_string(got.size()) + " cells, want " + std::to_string(want.size()));
    CHECK_MSG(current == 3, "one current match of three cells: " + std::to_string(current));
    CaptureRenderer r;
    TermPainter painter;
    PaintOptions opts;
    opts.font = fixedFont();
    const CellMetrics m = fixedMetrics();
    painter.paint(&r, *frame, kOriginX, kOriginY, 30 * m.cellW, 4 * m.cellH, m, opts);
    compare("search matches", expected(*frame, lit), decode(r.ops, frame->cols, frame->rows, m));
    // Custom highlight colours reach the fills.
    CaptureRenderer r2;
    opts.highlights.match = bromath::Color{0.0f, 1.0f, 0.0f, 0.5f};
    opts.highlights.currentMatch = bromath::Color{1.0f, 0.0f, 1.0f, 0.5f};
    painter.paint(&r2, *frame, kOriginX, kOriginY, 30 * m.cellW, 4 * m.cellH, m, opts);
    const Decoded d = decode(r2.ops, frame->cols, frame->rows, m);
    for (auto& [cell, kind] : lit) {
        const bromath::Color want2 = kind == bropty::HighlightKind::CurrentMatch ? opts.highlights.currentMatch
                                                                                 : opts.highlights.match;
        CHECK_MSG(sameColor(d.grid[size_t(cell.first)][size_t(cell.second)].fills.back(), want2),
                  "match colour at (" + std::to_string(cell.first) + "," + std::to_string(cell.second) + ")");
    }
}

void runSelection() {
    section("paint oracle: selection");
    Fixture fx(30, 4, "select some of this\r\nand \x1b[44mthis\x1b[0m too");
    const int64_t top = fx.term->screen_top_row();
    fx.view->selection().select_range(bropty::RowRange{{top, 7}, {top + 1, 8}});
    auto frame = fx.frame();
    CHECK(!frame->highlights.empty());
    // Where a selection ends on a line (its text, or the full width) is
    // bropty's call: take its highlight spans as the selected cells, and
    // check the ends that are not in doubt.
    std::set<std::pair<int, int>> sel;
    for (const bropty::Highlight& h : frame->highlights)
        for (int x = h.col0; x < h.col1; ++x) sel.insert({h.y, x});
    CHECK(sel.count({0, 7}) && sel.count({1, 7}) && !sel.count({1, 8}) && !sel.count({0, 6}));
    CaptureRenderer r;
    TermPainter painter;
    PaintOptions opts;
    opts.font = fixedFont();
    const CellMetrics m = fixedMetrics();
    painter.paint(&r, *frame, kOriginX, kOriginY, 30 * m.cellW, 4 * m.cellH, m, opts);
    std::map<std::pair<int, int>, bropty::HighlightKind> lit;
    for (const auto& cell : sel) lit[cell] = bropty::HighlightKind::Selection;
    compare("selection", expected(*frame, lit), decode(r.ops, frame->cols, frame->rows, m));
}

void runRowCache() {
    section("paint: rows rebuilt only when they change");
    Fixture fx(20, 5, "one\r\ntwo\r\nthree\r\nfour");
    TermPainter painter;
    PaintOptions opts;
    opts.font = fixedFont();
    const CellMetrics m = fixedMetrics();
    CaptureRenderer r;
    painter.paint(&r, *fx.frame(), 0, 0, 200, 100, m, opts);
    CHECK(painter.rowsBuilt() == 5);
    painter.paint(&r, *fx.frame(), 0, 0, 200, 100, m, opts);
    CHECK(painter.rowsBuilt() == 0);
    fx.term->feed("\x1b[2;1Hchanged");
    painter.paint(&r, *fx.frame(), 0, 0, 200, 100, m, opts);
    CHECK_MSG(painter.rowsBuilt() == 1, "rebuilt " + std::to_string(painter.rowsBuilt()));
    // A palette change (OSC 4) invalidates every row.
    fx.term->feed("\x1b]4;1;rgb:ff/00/ff\x1b\\");
    painter.paint(&r, *fx.frame(), 0, 0, 200, 100, m, opts);
    CHECK_MSG(painter.rowsBuilt() == 5, "rebuilt " + std::to_string(painter.rowsBuilt()));
}

} // namespace

void run_paint_oracle_tests() {
    for (const Case& c : corpus()) {
        section(std::string("paint oracle: ") + c.name);
        runCase(c);
    }
    runSelection();
    runSearchHighlights();
    runMinimumContrast();
    runLetterSpacing();
    runRowCache();
}

} // namespace bro::terminal::test
