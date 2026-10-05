#include "terminal/term_paint.h"

#include <bropty/cell.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <utility>

namespace bro::terminal {

namespace {

constexpr char32_t kImageCell = 0x10EEEE;  // bropty's image placeholder (drawn by T2)

bool sameRgb(bropty::Rgb a, bropty::Rgb b) { return a == b; }

void appendUtf8(std::string& out, char32_t cp) { bropty::append_utf8(out, cp); }

} // namespace

// ---------------------------------------------------------------------------
// Batching

bool batchableCodepoint(char32_t cp) {
    if (cp >= 0x21 && cp <= 0x7E) return true;     // ASCII
    if (cp >= 0xA1 && cp <= 0x24F) return cp != 0xAD;  // Latin-1, Latin Extended A/B (not SHY)
    if (cp >= 0x370 && cp <= 0x3FF) return true;    // Greek
    if (cp >= 0x400 && cp <= 0x482) return true;    // Cyrillic letters (not the combining marks)
    if (cp >= 0x48A && cp <= 0x52F) return true;
    if (cp >= 0x2500 && cp <= 0x259F) return true;  // box drawing, block elements
    return false;
}

// ---------------------------------------------------------------------------
// Metrics

CellMetrics CellMetrics::measure(render::Renderer* r, const render::FontRef& font, float lineHeightPx,
                                 float letterSpacingPx, float scale, bool keepAdvance) {
    CellMetrics m;
    const float s = scale > 0.0f && std::isfinite(scale) ? scale : 1.0f;
    m.scale = s;
    if (!r) return m;
    // Whole device pixels: round to nearest, and up (for heights that must
    // hold the glyph box), with at least one pixel.
    auto snap = [s](float v) { return std::max(1.0f, std::round(v * s)) / s; };
    auto snapUp = [s](float v) { return std::max(1.0f, std::ceil(v * s - 1e-3f)) / s; };
    const render::TextMetrics wide = r->measureText("MMMMMMMMMM", font);
    const render::TextMetrics line = r->measureText("", font);
    m.advance = wide.width > 0 ? wide.width / 10.0f : font.size * 0.6f;
    m.cellW = snap(m.advance + letterSpacingPx);
    m.letterSpacing = m.cellW - m.advance;
    m.ascent = line.ascent > 0 ? line.ascent : font.size * 0.8f;
    m.descent = line.descent > 0 ? line.descent : font.size * 0.2f;
    if (keepAdvance) {
        // Ligatures across cells need the font's own advance per cell.
        m.cellW = std::max(1.0f / s, m.advance + letterSpacingPx);
        m.letterSpacing = letterSpacingPx;
    }
    const float natural = snapUp(m.ascent + m.descent);
    m.cellH = lineHeightPx > 0 ? snap(lineHeightPx) : natural;
    m.baseline = std::round(((m.cellH - (m.ascent + m.descent)) * 0.5f + m.ascent) * s) / s;
    m.lineThickness = std::max(1.0f, std::round(font.size / 14.0f));
    m.underlineY = std::min(m.cellH - m.lineThickness * 0.5f,
                            m.baseline + std::max(m.lineThickness, m.descent * 0.35f));
    const float xh = line.xHeight > 0 ? line.xHeight : m.ascent * 0.55f;
    m.strikeY = m.baseline - xh * 0.5f;
    m.overlineY = m.lineThickness * 0.5f;
    return m;
}

int CellMetrics::pixelWidth() const { return std::max(1, int(std::lround(cellW * scale))); }
int CellMetrics::pixelHeight() const { return std::max(1, int(std::lround(cellH * scale))); }

// ---------------------------------------------------------------------------
// Row preparation

TermPainter::RowOps TermPainter::buildRow(const bropty::FrameRow& row, const bropty::Palette& pal, bool rv,
                                          const ColorPolicy& policy) {
    RowOps ops;
    const bropty::RowView v = row.view();
    if (!v.cells) return ops;
    const bropty::Rgb base = defaultBg(pal, rv);

    int textRun = -1;      // index of the open batched run in ops.text
    int runEnd = -1;       // the column the open run continues at
    int pendingSpaces = 0; // blank cells inside the open run, not yet appended
    struct OpenDeco {
        bool open = false;
        DecoOp op;
    } deco[3];

    auto closeDeco = [&](int k) {
        if (deco[k].open) ops.deco.push_back(deco[k].op);
        deco[k].open = false;
    };
    auto extendDeco = [&](int k, int col0, int col1, bropty::Underline style, bropty::Rgb color) {
        OpenDeco& d = deco[k];
        if (d.open && d.op.col1 == col0 && d.op.style == style && sameRgb(d.op.color, color)) {
            d.op.col1 = col1;
            return;
        }
        closeDeco(k);
        d.open = true;
        d.op.col0 = col0;
        d.op.col1 = col1;
        d.op.kind = DecoOp::Kind(k);
        d.op.style = style;
        d.op.color = color;
    };

    for (int col = 0; col < v.cols; ++col) {
        const bropty::Cell& cell = v[col];
        if (cell.wide() == bropty::Wide::SpacerTail) continue;
        const int width = cell.wide() == bropty::Wide::Lead ? 2 : 1;
        const int end = std::min(v.cols, col + width);
        const bropty::Style& st = v.style(col);
        const ResolvedCell rc = resolveCell(st, pal, rv, policy);

        // Background runs (merged by colour across style changes).
        if (!sameRgb(rc.bg, base)) {
            if (!ops.bg.empty() && ops.bg.back().col1 == col && sameRgb(ops.bg.back().color, rc.bg))
                ops.bg.back().col1 = end;
            else
                ops.bg.push_back(RectOp{col, end, rc.bg});
        }

        // Decorations (blank cells carry them too: an underlined space is a line).
        const bool lines = !rc.hidden;
        if (lines && st.underline != bropty::Underline::None) {
            const bropty::Rgb uc = st.underline_color.is_default() ? rc.fg : pal.resolve_fg(st.underline_color);
            extendDeco(DecoOp::Under, col, end, st.underline, uc);
        } else {
            closeDeco(DecoOp::Under);
        }
        if (lines && st.has(bropty::Attr_Strike)) extendDeco(DecoOp::Strike, col, end, bropty::Underline::Single, rc.fg);
        else closeDeco(DecoOp::Strike);
        if (lines && st.has(bropty::Attr_Overline)) extendDeco(DecoOp::Over, col, end, bropty::Underline::Single, rc.fg);
        else closeDeco(DecoOp::Over);

        // Glyphs.
        const char32_t cp = cell.cp();
        const bool glyph = cp != 0 && cp != U' ' && cp != kImageCell && !rc.hidden;
        if (!glyph) {
            if (textRun >= 0 && runEnd == col && width == 1) {
                ++pendingSpaces;
                runEnd = col + 1;
            } else {
                textRun = -1;
            }
            continue;
        }
        const bool bold = st.has(bropty::Attr_Bold);
        const bool italic = st.has(bropty::Attr_Italic);
        const bool batch = cell.wide() == bropty::Wide::Narrow && !cell.has_cluster() && batchableCodepoint(cp);
        if (batch && textRun >= 0 && runEnd == col) {
            TextOp& t = ops.text[size_t(textRun)];
            if (sameRgb(t.fg, rc.fg) && t.bold == bold && t.italic == italic) {
                t.text.append(size_t(pendingSpaces), ' ');
                pendingSpaces = 0;
                appendUtf8(t.text, cp);
                runEnd = col + 1;
                continue;
            }
        }
        pendingSpaces = 0;
        TextOp t;
        t.col = col;
        appendUtf8(t.text, cp);
        if (cell.has_cluster() && v.clusters) {
            for (char32_t c : v.clusters->find(col)) appendUtf8(t.text, c);
        }
        t.fg = rc.fg;
        t.bold = bold;
        t.italic = italic;
        ops.text.push_back(std::move(t));
        textRun = batch ? int(ops.text.size()) - 1 : -1;
        runEnd = col + width;
    }
    for (int k = 0; k < 3; ++k) closeDeco(k);
    return ops;
}

const TermPainter::RowOps& TermPainter::rowOps(const bropty::FrameRow& row, const bropty::Palette& pal, bool rv,
                                               const ColorPolicy& cp) {
    auto it = cache_.find(row.serial);
    if (it == cache_.end()) {
        ++rowsBuilt_;
        it = cache_.emplace(row.serial, Entry{buildRow(row, pal, rv, cp), 0}).first;
    }
    it->second.usedIn = paintCount_;
    return it->second.ops;
}

// ---------------------------------------------------------------------------
// Painting

namespace {

render::FontRef faceFor(const render::FontRef& base, bool bold, bool italic) {
    render::FontRef f = base;
    if (bold) f.weight = std::max(700, base.weight);
    if (italic) f.italic = true;
    return f;
}

std::string fmt(float v) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.2f", double(v));
    return buf;
}

} // namespace

void TermPainter::drawDeco(render::Renderer* r, const DecoOp& d, float x, float rowTop, const CellMetrics& m) {
    const float x0 = x + float(d.col0) * m.cellW;
    const float x1 = x + float(d.col1) * m.cellW;
    const float t = m.lineThickness;
    const bromath::Color c = toColor(d.color);
    if (d.kind == DecoOp::Strike) {
        r->drawLine(x0, rowTop + m.strikeY, x1, rowTop + m.strikeY, c, t);
        return;
    }
    if (d.kind == DecoOp::Over) {
        r->drawLine(x0, rowTop + m.overlineY, x1, rowTop + m.overlineY, c, t);
        return;
    }
    const float y = rowTop + m.underlineY;
    switch (d.style) {
        case bropty::Underline::None:
            break;
        case bropty::Underline::Single:
            r->drawLine(x0, y, x1, y, c, t);
            break;
        case bropty::Underline::Double: {
            const float y0 = std::max(rowTop + m.baseline + t * 0.5f, y - t);
            const float y1 = std::min(rowTop + m.cellH - t * 0.5f, y0 + t * 2.0f);
            r->drawLine(x0, y0, x1, y0, c, t);
            r->drawLine(x0, y1, x1, y1, c, t);
            break;
        }
        case bropty::Underline::Dotted:
        case bropty::Underline::Dashed: {
            // One segment per period, starting at the span's left edge.
            const bool dotted = d.style == bropty::Underline::Dotted;
            const float on = dotted ? t : std::max(2.0f * t, m.cellW * 0.5f);
            const float period = dotted ? 2.0f * t : on + std::max(t, m.cellW * 0.25f);
            for (float sx = x0; sx < x1; sx += period)
                r->drawLine(sx, y, std::min(x1, sx + on), y, c, t);
            break;
        }
        case bropty::Underline::Curly: {
            // A wave of one period per cell, amplitude kept inside the cell.
            const float amp = std::max(t, std::min(m.cellH - m.underlineY - t, m.descent * 0.4f));
            const float cy = std::min(y, rowTop + m.cellH - amp - t * 0.5f);
            std::string path = "M" + fmt(x0) + " " + fmt(cy);
            const float half = m.cellW * 0.5f;
            bool up = true;
            for (float sx = x0; sx + 0.01f < x1; sx += half) {
                const float ex = std::min(x1, sx + half);
                path += " Q" + fmt((sx + ex) * 0.5f) + " " + fmt(up ? cy - amp * 2.0f : cy + amp * 2.0f) + " " +
                        fmt(ex) + " " + fmt(cy);
                up = !up;
            }
            r->drawPath(path, bromath::Color{0, 0, 0, 0}, c, t);
            break;
        }
    }
}

void TermPainter::paint(render::Renderer* r, const bropty::Frame& f, float x, float y, float w, float h,
                        const CellMetrics& m, const PaintOptions& opts) {
    if (!r || !f.palette) return;
    const bropty::Palette& pal = *f.palette;
    const bool rv = f.modes.reverse_video;
    ++paintCount_;
    rowsBuilt_ = 0;
    if (cachePalette_ != f.palette.get() || cacheReverse_ != rv || !(cachePolicy_ == opts.colors)) {
        cache_.clear();
        cachePalette_ = f.palette.get();
        cachePaletteRef_ = f.palette;
        cacheReverse_ = rv;
        cachePolicy_ = opts.colors;
    }

    r->save();
    r->setClip(x, y, w, h);
    r->fillRect(x, y, w, h, toColor(defaultBg(pal, rv)));

    const int rows = std::min<int>(f.rows, int(f.lines.size()));
    std::vector<const RowOps*> ops(size_t(std::max(0, rows)), nullptr);
    for (int row = 0; row < rows; ++row) {
        if (f.lines[size_t(row)]) ops[size_t(row)] = &rowOps(*f.lines[size_t(row)], pal, rv, opts.colors);
    }
    auto rowTop = [&](int row) { return y + float(row) * m.cellH; };
    // Cell x edges snap to whole device pixels, so neighbouring fills meet
    // without an antialiased seam however fractional the origin is.
    const float s = m.scale > 0.0f ? m.scale : 1.0f;
    auto colX = [&](int col) { return std::round((x + float(col) * m.cellW) * s) / s; };
    render::FontRef textFont = opts.font;
    textFont.ligatures = opts.ligatures;

    // 1. backgrounds
    for (int row = 0; row < rows; ++row) {
        if (!ops[size_t(row)]) continue;
        for (const RectOp& b : ops[size_t(row)]->bg) {
            const float x0 = colX(b.col0);
            r->fillRect(x0, rowTop(row), colX(b.col1) - x0, m.cellH, toColor(b.color));
        }
    }
    // 2. highlights
    for (const bropty::Highlight& hl : f.highlights) {
        if (hl.y < 0 || hl.y >= rows || hl.col1 <= hl.col0) continue;
        const bromath::Color c = highlightColor(hl.kind, opts.highlights);
        if (c.a <= 0.0f) continue;
        const float x0 = colX(hl.col0);
        r->fillRect(x0, rowTop(hl.y), colX(hl.col1) - x0, m.cellH, c);
    }
    // 3. glyphs. A batched run gets the cell's spacing after every glyph, so
    // its glyph i lands exactly on cell col + i whatever the font's advance.
    for (int row = 0; row < rows; ++row) {
        if (!ops[size_t(row)]) continue;
        const float base = rowTop(row) + m.baseline;
        for (const TextOp& t : ops[size_t(row)]->text) {
            const render::FontRef face = faceFor(textFont, t.bold, t.italic);
            const float tx = x + float(t.col) * m.cellW;
            if (m.letterSpacing != 0.0f && t.text.size() > 1)
                r->drawTextEx(t.text, tx, base, face, toColor(t.fg), m.letterSpacing, 0.0f);
            else
                r->drawText(t.text, tx, base, face, toColor(t.fg));
        }
    }
    // 4. decorations, and the underline of the hovered link
    for (int row = 0; row < rows; ++row) {
        if (!ops[size_t(row)]) continue;
        for (const DecoOp& d : ops[size_t(row)]->deco) drawDeco(r, d, x, rowTop(row), m);
    }
    drawHover(r, f, x, y, m, pal, rv);
    // 5. cursor, 6. preedit
    drawCursor(r, f, x, y, m, opts, pal, rv);

    r->restore();

    // Forget rows this frame no longer shows.
    for (auto it = cache_.begin(); it != cache_.end();) {
        if (it->second.usedIn != paintCount_) it = cache_.erase(it);
        else ++it;
    }
}

void TermPainter::drawHover(render::Renderer* r, const bropty::Frame& f, float x, float y, const CellMetrics& m,
                            const bropty::Palette& pal, bool rv) {
    const float s = m.scale > 0.0f ? m.scale : 1.0f;
    for (const bropty::Highlight& hl : f.highlights) {
        if (hl.kind != bropty::HighlightKind::Hover || hl.y < 0 || hl.y >= f.rows || hl.col1 <= hl.col0) continue;
        // The link's own colour where its first cell has one, else the
        // default foreground: a solid line under the whole link.
        bropty::Rgb c = defaultFg(pal, rv);
        if (size_t(hl.y) < f.lines.size() && f.lines[size_t(hl.y)]) {
            const bropty::RowView v = f.lines[size_t(hl.y)]->view();
            if (v.cells && hl.col0 < v.cols) c = resolveCell(v.style(hl.col0), pal, rv).fg;
        }
        const float x0 = std::round((x + float(hl.col0) * m.cellW) * s) / s;
        const float x1 = std::round((x + float(hl.col1) * m.cellW) * s) / s;
        const float uy = y + float(hl.y) * m.cellH + m.underlineY;
        r->drawLine(x0, uy, x1, uy, toColor(c), m.lineThickness);
    }
}

void TermPainter::drawCursor(render::Renderer* r, const bropty::Frame& f, float x, float y, const CellMetrics& m,
                             const PaintOptions& opts, const bropty::Palette& pal, bool rv) {
    if (f.cursor_y < 0 || f.cursor_y >= f.rows || f.cols <= 0) return;
    const float top = y + float(f.cursor_y) * m.cellH;
    int col = std::clamp(f.cursor.col, 0, f.cols - 1);
    const bropty::FrameRow* row = size_t(f.cursor_y) < f.lines.size() ? f.lines[size_t(f.cursor_y)].get() : nullptr;
    bropty::RowView v = row ? row->view() : bropty::RowView{};
    if (v.cells && col > 0 && v[col].wide() == bropty::Wide::SpacerTail) --col;
    const int width = (v.cells && v[col].wide() == bropty::Wide::Lead && col + 1 < f.cols) ? 2 : 1;
    const float cx = std::round(x + float(col) * m.cellW);
    const float cw = std::round(x + float(col + width) * m.cellW) - cx;

    if (!opts.preedit.empty()) {
        // The composition replaces the cursor: on the default background, in
        // the default foreground, underlined, starting at the cursor cell.
        const bropty::Rgb fg = defaultFg(pal, rv);
        const float tw = r->measureText(opts.preedit, opts.font).width;
        r->fillRect(cx, top, std::max(cw, std::ceil(tw)), m.cellH, toColor(defaultBg(pal, rv)));
        r->drawText(opts.preedit, x + float(col) * m.cellW, top + m.baseline, opts.font, toColor(fg));
        r->drawLine(cx, top + m.underlineY, cx + std::max(cw, tw), top + m.underlineY, toColor(fg), m.lineThickness);
        return;
    }

    if (!f.cursor.visible) return;
    const bool blinking = f.cursor.blink || f.modes.cursor_blink;
    if (opts.focused && blinking && !opts.blinkOn) return;
    const bromath::Color cc = toColor(pal.cursor);
    const float t = std::max(1.0f, m.lineThickness);

    if (!opts.focused) {
        // Unfocused: a hollow block whatever the shape.
        r->drawRect(cx + 0.5f, top + 0.5f, cw - 1.0f, m.cellH - 1.0f, cc);
        return;
    }
    switch (f.cursor.shape) {
        case bropty::CursorShape::Block: {
            r->fillRect(cx, top, cw, m.cellH, cc);
            // The glyph under it, in the cell's background colour.
            if (v.cells) {
                const bropty::Cell& cell = v[col];
                const bropty::Style& st = v.style(col);
                const ResolvedCell rc = resolveCell(st, pal, rv);
                const char32_t cp = cell.cp();
                if (cp != 0 && cp != U' ' && cp != kImageCell && !rc.hidden) {
                    std::string text;
                    appendUtf8(text, cp);
                    if (cell.has_cluster() && v.clusters)
                        for (char32_t c : v.clusters->find(col)) appendUtf8(text, c);
                    r->drawText(text, x + float(col) * m.cellW, top + m.baseline,
                                faceFor(opts.font, st.has(bropty::Attr_Bold), st.has(bropty::Attr_Italic)),
                                toColor(rc.bg));
                }
            }
            break;
        }
        case bropty::CursorShape::Underline: {
            const float uh = std::max(t, std::round(m.cellH / 10.0f));
            r->fillRect(cx, top + m.cellH - uh, cw, uh, cc);
            break;
        }
        case bropty::CursorShape::Bar: {
            const float bw = std::max(t, std::round(m.cellW / 8.0f));
            r->fillRect(cx, top, bw, m.cellH, cc);
            break;
        }
    }
}

} // namespace bro::terminal
