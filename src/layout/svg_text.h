#pragma once

#include <string>
#include <vector>

#include <include/core/SkRect.h>

namespace bro::dom { class Element; }
namespace bro::render { class Renderer; }

// Shared SVG <text> layout. A single pen-walk over a <text> subtree produces
// positioned glyph runs; both the native paint traversal (svg_paint.cpp) and
// the getBoundingClientRect geometry (svg_geometry.cpp) consume the same
// routine so painted glyphs and reported bounds always agree.
//
// All coordinates are user units in the <text> element's own frame (the
// element's `transform` attribute and the enclosing viewBox are applied by the
// caller's CTM, not here). Measurement is abstracted behind SvgTextMeasurer so
// the paint side can drive it from a render::Renderer and the geometry side
// from an htmlayout TextMetrics — the same layout, two backends.
namespace bro::layout {

struct SvgFont {
    std::string family = "Arial";
    float       size = 16.0f;
    int         weight = 400;
    bool        italic = false;
};

// Abstract text measurement. Both implementations ultimately call the same
// Skia/DirectWrite backend, so advances and vertical metrics agree.
struct SvgTextMeasurer {
    virtual ~SvgTextMeasurer() = default;
    // Advance width of `text` at `f`, in user units.
    virtual float advance(const std::string& text, const SvgFont& f) = 0;
    // Font vertical metrics at `f`: ascent/descent (both positive, px above/
    // below the baseline) and x-height.
    virtual void vmetrics(const SvgFont& f, float& ascent, float& descent,
                          float& xHeight) = 0;
};

// Measures text through a render::Renderer using the font's raw (unrounded)
// metrics — SVG text geometry and painting both use these, so glyphs and
// getBoundingClientRect agree and match Chromium's raw-metric text bbox.
struct RendererTextMeasurer : SvgTextMeasurer {
    render::Renderer* r;
    explicit RendererTextMeasurer(render::Renderer* rr) : r(rr) {}
    float advance(const std::string& text, const SvgFont& f) override;
    void vmetrics(const SvgFont& f, float& ascent, float& descent,
                  float& xHeight) override;
};

struct SvgTextRun {
    std::string text;
    float x = 0;          // left edge after text-anchor, user units
    float baseline = 0;   // baseline y, user units
    float width = 0;
    float ascent = 0;
    float descent = 0;
    const dom::Element* styleEl = nullptr;  // element whose fill/font applies
    SvgFont font;
};

// Lay out a <text> element into anchored, positioned runs.
std::vector<SvgTextRun> layoutSvgText(const dom::Element* textEl,
                                      SvgTextMeasurer& m);

// Union bounds (user units, <text>-local frame) of the runs contributed by
// `el` — every run when `el` is the <text> root, or just the runs inside `el`
// when it is a <tspan>. Returns false if `el` is not within a <text> or
// contributes no runs.
bool svgTextElementBounds(const dom::Element* el, SvgTextMeasurer& m,
                          SkRect& out);

} // namespace bro::layout
