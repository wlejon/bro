// The cursor and the IME preedit as painted: each shape (DECSCUSR), blink
// phase, hidden (DECTCEM), the hollow block of an unfocused terminal, a
// cursor on a wide character, and the composition drawn in its place.

#include "check.h"
#include "paint_fixture.h"
#include "tests.h"

#include <cmath>
#include <vector>

namespace bro::terminal::test {

namespace {

struct Painted {
    std::vector<Op> ops;
    bropty::Rgb cursorRgb;
    bropty::Rgb bgRgb;
    bropty::Rgb fgRgb;
};

Painted paintWith(std::string_view bytes, bool focused, bool blinkOn = true, std::string preedit = {}) {
    Fixture fx(20, 3, bytes);
    auto frame = fx.frame();
    CaptureRenderer r;
    TermPainter p;
    PaintOptions o;
    o.font = fixedFont();
    o.focused = focused;
    o.blinkOn = blinkOn;
    o.preedit = std::move(preedit);
    const CellMetrics m = fixedMetrics();
    p.paint(&r, *frame, kOriginX, kOriginY, 20 * m.cellW, 3 * m.cellH, m, o);
    return Painted{std::move(r.ops), frame->palette->cursor, frame->palette->background, frame->palette->foreground};
}

std::vector<const Op*> cursorFills(const Painted& p) {
    std::vector<const Op*> out;
    for (const Op& op : p.ops)
        if (op.kind == Op::Fill && sameColor(op.color, rgbColor(p.cursorRgb))) out.push_back(&op);
    return out;
}

std::vector<const Op*> strokes(const Painted& p) {
    std::vector<const Op*> out;
    for (const Op& op : p.ops)
        if (op.kind == Op::Stroke) out.push_back(&op);
    return out;
}

float cellX(int col) { return std::round(kOriginX + float(col) * fixedMetrics().cellW); }
float rowY(int row) { return kOriginY + float(row) * fixedMetrics().cellH; }

} // namespace

void run_paint_cursor_tests() {
    const CellMetrics m = fixedMetrics();

    section("cursor: steady block on a glyph, focused");
    {
        Painted p = paintWith("abc\r\nxyz\x1b[2D\x1b[2 q", true);  // cursor on 'y' (row 1, col 1)
        auto fills = cursorFills(p);
        CHECK(fills.size() == 1);
        if (fills.size() == 1) {
            CHECK(fills[0]->x == cellX(1) && fills[0]->y == rowY(1));
            CHECK(fills[0]->w == cellX(2) - cellX(1) && fills[0]->h == m.cellH);
        }
        // The glyph is drawn again over the block, in the cell's background.
        const Op& last = p.ops[p.ops.size() - 2];  // before the final restore
        CHECK(last.kind == Op::Text && last.text == "y");
        CHECK(sameColor(last.color, rgbColor(p.bgRgb)));
        CHECK(std::fabs(last.x - (kOriginX + m.cellW)) < 1e-4f && last.y == rowY(1) + m.baseline);
    }

    section("cursor: underline and bar shapes");
    {
        Painted u = paintWith("ab\x1b[4 q", true);
        auto fu = cursorFills(u);
        CHECK(fu.size() == 1);
        if (fu.size() == 1) {
            CHECK(fu[0]->x == cellX(2) && fu[0]->h == 2.0f && fu[0]->y == rowY(0) + m.cellH - 2.0f);
        }
        Painted b = paintWith("ab\x1b[6 q", true);
        auto fb = cursorFills(b);
        CHECK(fb.size() == 1);
        if (fb.size() == 1) CHECK(fb[0]->x == cellX(2) && fb[0]->w == 1.0f && fb[0]->h == m.cellH);
    }

    section("cursor: unfocused is a hollow block");
    {
        Painted p = paintWith("ab\x1b[6 q", false);  // whatever the shape
        CHECK(cursorFills(p).empty());
        auto s = strokes(p);
        CHECK(s.size() == 1);
        if (s.size() == 1) CHECK(std::fabs(s[0]->x - (cellX(2) + 0.5f)) < 1e-4f && s[0]->h == m.cellH - 1.0f);
    }

    section("cursor: hidden (DECTCEM)");
    {
        Painted f = paintWith("ab\x1b[?25l", true);
        Painted u = paintWith("ab\x1b[?25l", false);
        CHECK(cursorFills(f).empty() && strokes(f).empty());
        CHECK(cursorFills(u).empty() && strokes(u).empty());
    }

    section("cursor: blink phase");
    {
        // The default cursor blinks: its off phase draws nothing ...
        CHECK(cursorFills(paintWith("ab", true, false)).empty());
        CHECK(cursorFills(paintWith("ab", true, true)).size() == 1);
        // ... a steady one ignores the phase ...
        CHECK(cursorFills(paintWith("ab\x1b[2 q", true, false)).size() == 1);
        // ... and an unfocused one never blinks.
        CHECK(strokes(paintWith("ab", false, false)).size() == 1);
    }

    section("cursor: on a wide character covers both cells");
    {
        Painted p = paintWith("\xe6\x97\xa5\xe6\x9c\xac\x1b[1;3H\x1b[2 q", true);  // on the second CJK char
        auto fills = cursorFills(p);
        CHECK(fills.size() == 1);
        if (fills.size() == 1) CHECK(fills[0]->x == cellX(2) && fills[0]->w == cellX(4) - cellX(2));
    }

    section("preedit replaces the cursor");
    {
        Painted p = paintWith("ab", true, true, "\xe3\x81\xab\xe3\x81\xbb");
        CHECK(cursorFills(p).empty());
        bool text = false, line = false;
        for (const Op& op : p.ops) {
            if (op.kind == Op::Text && op.text == "\xe3\x81\xab\xe3\x81\xbb") {
                text = std::fabs(op.x - (kOriginX + 2 * m.cellW)) < 1e-4f && sameColor(op.color, rgbColor(p.fgRgb));
            }
            if (op.kind == Op::Line && op.y == rowY(0) + m.underlineY && op.x == cellX(2)) line = true;
        }
        CHECK(text);
        CHECK(line);
    }
}

} // namespace bro::terminal::test
