#pragma once
// TermPainter: draws a bropty::Frame through bro's Renderer.
//
// Text goes through Renderer::drawText, which is the same HarfBuzz /
// ShapedRun path every other piece of text in bro takes (font fallback for
// emoji and CJK included). The grid stays exact because nothing is left to
// the shaper's advances where they could drift: runs are batched only over
// narrow single-code-point cells of scripts that are drawn left to right
// with one glyph per character in a monospace face (ASCII, Latin, Greek,
// Cyrillic, box drawing and block elements); every other cluster (wide
// characters, emoji, combining sequences, anything bidi could reorder) is
// drawn on its own at its cell's x.
//
// Paint order, over the whole frame: the default background, the cell
// background runs, the highlight overlays (selection, search matches), the
// glyphs, the decorations (underline in its five styles, strikethrough,
// overline), the cursor, and the IME preedit.
//
// Rows are prepared once per content: each frame row carries a serial that
// changes whenever its content does (bropty's copy-on-write snapshot), and
// the prepared runs (resolved colours, batched text, decoration spans, in
// cell units) are cached by it, so a frame where three rows changed rebuilds
// three rows. The cache is dropped when the palette, reverse video or the
// emphasis policy changes.

#include "render/renderer.h"

#include <bropty/color.h>
#include <bropty/frame.h>
#include <bropty/style.h>

#include <bromath/color.h>

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace bro::terminal {

struct CellMetrics {
    float cellW = 8.0f;     // CSS px, fractional (the font's advance)
    float cellH = 16.0f;    // CSS px
    float ascent = 12.0f;
    float descent = 4.0f;
    float baseline = 12.0f;     // from the cell top
    float lineThickness = 1.0f;
    float underlineY = 14.0f;   // from the cell top, the line's centre
    float strikeY = 8.0f;
    float overlineY = 0.5f;

    // From `font`'s metrics as `renderer` measures them. `lineHeightPx` > 0
    // overrides the cell height (CSS line-height); the glyph box is centred.
    static CellMetrics measure(render::Renderer* renderer, const render::FontRef& font,
                               float lineHeightPx = 0.0f);
    bool operator==(const CellMetrics&) const = default;
};

struct PaintOptions {
    render::FontRef font;           // the regular face; bold/italic are derived
    bool focused = false;
    bool blinkOn = true;            // blink phase: false hides a blinking cursor
    std::string preedit;            // IME composition, drawn at the cursor
};

// A cell's colours after the palette, inverse, reverse video, dim and
// invisible are applied: exactly what the painter fills and draws with.
struct ResolvedCell {
    bropty::Rgb fg;
    bropty::Rgb bg;
    bool hidden = false;  // SGR 8: no glyph, no decorations
};
ResolvedCell resolveCell(const bropty::Style& st, const bropty::Palette& pal, bool reverseVideo);
// The default background (what the frame is filled with) and foreground.
bropty::Rgb defaultBg(const bropty::Palette& pal, bool reverseVideo);
bropty::Rgb defaultFg(const bropty::Palette& pal, bool reverseVideo);

bromath::Color toColor(bropty::Rgb c, float alpha = 1.0f);

// Overlay colours for the highlight kinds (drawn over the backgrounds,
// under the text).
bromath::Color highlightColor(bropty::HighlightKind kind);

// Whether a cell's glyph may share a drawText run with its neighbours.
bool batchableCodepoint(char32_t cp);

class TermPainter {
public:
    // Paint `frame` with its top-left cell at (x, y), clipped to (x, y, w, h).
    void paint(render::Renderer* r, const bropty::Frame& frame, float x, float y, float w, float h,
               const CellMetrics& m, const PaintOptions& opts);

    // Rows whose runs were (re)built by the last paint() (cache misses).
    int rowsBuilt() const { return rowsBuilt_; }

    struct TextOp {
        int col = 0;
        std::string text;           // UTF-8
        bropty::Rgb fg;
        bool bold = false;
        bool italic = false;
    };
    struct RectOp {
        int col0 = 0, col1 = 0;     // [col0, col1)
        bropty::Rgb color;
    };
    struct DecoOp {
        int col0 = 0, col1 = 0;
        enum Kind : uint8_t { Under, Strike, Over } kind = Under;
        bropty::Underline style = bropty::Underline::Single;
        bropty::Rgb color;
    };
    struct RowOps {
        std::vector<RectOp> bg;     // runs that differ from the default background
        std::vector<TextOp> text;
        std::vector<DecoOp> deco;
    };
    static RowOps buildRow(const bropty::FrameRow& row, const bropty::Palette& pal, bool reverseVideo);

private:
    const RowOps& rowOps(const bropty::FrameRow& row, const bropty::Palette& pal, bool rv);
    void drawDeco(render::Renderer* r, const DecoOp& d, float x, float rowTop, const CellMetrics& m);
    void drawCursor(render::Renderer* r, const bropty::Frame& f, float x, float y, const CellMetrics& m,
                    const PaintOptions& opts, const bropty::Palette& pal, bool rv);

    struct Entry {
        RowOps ops;
        uint64_t usedIn = 0;
    };
    std::unordered_map<uint64_t, Entry> cache_;  // FrameRow::serial -> runs
    // The palette the cache was built with; the reference keeps its address
    // from being reused by a later palette while the cache is keyed on it.
    const bropty::Palette* cachePalette_ = nullptr;
    std::shared_ptr<const bropty::Palette> cachePaletteRef_;
    bool cacheReverse_ = false;
    uint64_t paintCount_ = 0;
    int rowsBuilt_ = 0;
};

} // namespace bro::terminal
