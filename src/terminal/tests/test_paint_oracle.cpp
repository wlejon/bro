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
#include <bropty/unicode.h>

#include <algorithm>
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

Grid expected(const bropty::Frame& f, const std::set<std::pair<int, int>>& selected) {
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
            bropty::Rgb fg = st.fg.is_default() ? defFg : pal.resolve_fg(st.fg);
            bropty::Rgb bg = st.bg.is_default() ? defBg : pal.resolve_bg(st.bg);
            if (st.has(bropty::Attr_Inverse)) std::swap(fg, bg);
            if (st.has(bropty::Attr_Dim)) {
                auto half = [](uint8_t a, uint8_t b) { return uint8_t(std::lround((int(a) + int(b)) / 2.0)); };
                fg = bropty::Rgb{half(fg.r, bg.r), half(fg.g, bg.g), half(fg.b, bg.b)};
            }
            const bool hidden = st.has(bropty::Attr_Invisible);

            c.fills.push_back(rgbColor(defBg));
            if (!(bg == defBg)) c.fills.push_back(rgbColor(bg));
            if (selected.count({y, x})) c.fills.push_back(highlightColor(bropty::HighlightKind::Selection));

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
};

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
    return c;
}

void runCase(const Case& c) {
    Fixture fx(c.cols, c.rows, c.bytes);
    auto frame = fx.frame();
    CaptureRenderer r;
    TermPainter painter;
    PaintOptions opts;
    opts.font = fixedFont();
    opts.focused = false;  // the cursor is a stroked rect, out of the cell decoding's way
    const CellMetrics m = fixedMetrics();
    painter.paint(&r, *frame, kOriginX, kOriginY, float(c.cols) * m.cellW, float(c.rows) * m.cellH, m, opts);
    compare(c.name, expected(*frame, {}), decode(r.ops, frame->cols, frame->rows, m));
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
    compare("selection", expected(*frame, sel), decode(r.ops, frame->cols, frame->rows, m));
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
    runRowCache();
}

} // namespace bro::terminal::test
