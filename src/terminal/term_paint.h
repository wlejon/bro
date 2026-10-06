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
// Paint order, over the whole frame: the default background, images under
// the cell backgrounds, the cell background runs, images between the
// backgrounds and the text, image cells (sixel, iTerm2, kitty placeholders),
// the highlight overlays (selection, search matches), the glyphs, the
// decorations (underline in its five styles, strikethrough, overline),
// images over the text, the hovered link, the cursor, and the IME preedit.
//
// Images (bropty::Frame::images) are placed in cells, so they land on the
// same grid as the text at any cell size or device scale; their pixels are
// drawn by reference (render::SharedPixels), uploaded once per buffer.
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
    float cellW = 8.0f;     // CSS px: the advance plus letter-spacing, snapped to device px
    float cellH = 16.0f;    // CSS px, snapped to device px
    float advance = 8.0f;   // the font's own advance (CSS px)
    // Added after each glyph of a batched run, so a run of N glyphs spans
    // exactly N cells: cellW - advance.
    float letterSpacing = 0.0f;
    float scale = 1.0f;     // device px per CSS px the metrics were snapped for
    float ascent = 12.0f;
    float descent = 4.0f;
    float baseline = 12.0f;     // from the cell top
    float lineThickness = 1.0f;
    float underlineY = 14.0f;   // from the cell top, the line's centre
    float strikeY = 8.0f;
    float overlineY = 0.5f;

    // From `font`'s metrics as `renderer` measures them. `lineHeightPx` > 0
    // overrides the cell height (CSS line-height); the glyph box is centred.
    // `letterSpacingPx` widens every cell (CSS letter-spacing). The cell size
    // and the baseline are snapped to whole device pixels at `scale` (the
    // render scale: 2 on a Retina display), so every cell edge of a layer
    // drawn at that scale lands on a pixel and the grid stays crisp.
    // `keepAdvance` keeps the font's own advance as the cell width instead
    // (no snapping, no spacing): what ligatures across cells need.
    static CellMetrics measure(render::Renderer* renderer, const render::FontRef& font,
                               float lineHeightPx = 0.0f, float letterSpacingPx = 0.0f, float scale = 1.0f,
                               bool keepAdvance = false);
    // The cell in device pixels (what the PTY's window size and SGR-pixel
    // mouse reports count in).
    [[nodiscard]] int pixelWidth() const;
    [[nodiscard]] int pixelHeight() const;
    bool operator==(const CellMetrics&) const = default;
};

// How cell colours are chosen beyond the SGR rules.
struct ColorPolicy {
    // Bold text in one of the eight base colours draws in its bright twin
    // (xterm's boldColors); off by default, as kitty and Ghostty do.
    bool boldIsBright = false;
    // WCAG contrast ratio every glyph keeps against its background, the
    // foreground's lightness moved (hue kept, Oklch) when it is lower; 1 = off.
    float minimumContrast = 1.0f;
    bool operator==(const ColorPolicy&) const = default;
};

// The overlay colours (drawn over the backgrounds, under the text).
struct HighlightColors {
    bromath::Color selection{90.0f / 255, 140.0f / 255, 230.0f / 255, 0.45f};
    bromath::Color match{230.0f / 255, 200.0f / 255, 60.0f / 255, 0.35f};
    bromath::Color currentMatch{240.0f / 255, 140.0f / 255, 40.0f / 255, 0.55f};
};

struct PaintOptions {
    render::FontRef font;           // the regular face; bold/italic are derived
    bool focused = false;
    bool blinkOn = true;            // blink phase: false hides a blinking cursor
    std::string preedit;            // IME composition, drawn at the cursor
    ColorPolicy colors;
    HighlightColors highlights;
    // Programming ligatures across cells (calt/liga in coding fonts). Off:
    // every cell shows its own character.
    bool ligatures = false;
};

// A cell's colours after the palette, inverse, reverse video, dim and
// invisible are applied: exactly what the painter fills and draws with.
struct ResolvedCell {
    bropty::Rgb fg;
    bropty::Rgb bg;
    bool hidden = false;  // SGR 8: no glyph, no decorations
};
ResolvedCell resolveCell(const bropty::Style& st, const bropty::Palette& pal, bool reverseVideo,
                         const ColorPolicy& policy = {});
// `fg` with its lightness moved until it has `ratio` contrast against `bg`.
bropty::Rgb ensureContrast(bropty::Rgb fg, bropty::Rgb bg, float ratio);
// The default background (what the frame is filled with) and foreground.
bropty::Rgb defaultBg(const bropty::Palette& pal, bool reverseVideo);
bropty::Rgb defaultFg(const bropty::Palette& pal, bool reverseVideo);

bromath::Color toColor(bropty::Rgb c, float alpha = 1.0f);

// Overlay colours for the highlight kinds (drawn over the backgrounds,
// under the text). Hover is drawn as an underline instead (alpha 0 here).
bromath::Color highlightColor(bropty::HighlightKind kind, const HighlightColors& colors = {});

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
    static RowOps buildRow(const bropty::FrameRow& row, const bropty::Palette& pal, bool reverseVideo,
                           const ColorPolicy& policy = {});

private:
    const RowOps& rowOps(const bropty::FrameRow& row, const bropty::Palette& pal, bool rv, const ColorPolicy& cp);
    void drawHover(render::Renderer* r, const bropty::Frame& f, float x, float y, const CellMetrics& m,
                   const bropty::Palette& pal, bool rv);
    void drawDeco(render::Renderer* r, const DecoOp& d, float x, float rowTop, const CellMetrics& m);
    void drawCursor(render::Renderer* r, const bropty::Frame& f, float x, float y, const CellMetrics& m,
                    const PaintOptions& opts, const bropty::Palette& pal, bool rv);

public:
    // The frame's images on one plane, in the frame's order (term_paint_images.cpp).
    static void drawImages(render::Renderer* r, const bropty::Frame& f, bropty::ImagePlane plane, float x, float y,
                           const CellMetrics& m);
    // Where an image lands, in CSS px, with the frame's top-left cell at (x, y).
    struct ImageRect {
        float x = 0, y = 0, w = 0, h = 0;
    };
    static ImageRect imageRect(const bropty::FrameImage& im, float x, float y, const CellMetrics& m);

private:

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
    ColorPolicy cachePolicy_;
    uint64_t paintCount_ = 0;
    int rowsBuilt_ = 0;
};

} // namespace bro::terminal
