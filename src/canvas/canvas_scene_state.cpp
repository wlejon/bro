// CanvasScene's drawing state: the style setters and getters, font resolution
// and shaping, the paint each draw records with (globalAlpha, composite op,
// shadow and filter), and text placement, measurement and recording.

#include "canvas/canvas_scene.h"
#include "render/filter_chain.h"
#include "render/font_family.h"
#include "render/system_font_mgr.h"

#include <include/core/SkBlendMode.h>
#include <include/core/SkFontMetrics.h>
#include <include/core/SkFontMgr.h>
#include <include/core/SkFontStyle.h>
#include <include/core/SkSamplingOptions.h>
#include <include/effects/SkDashPathEffect.h>
#include <include/effects/SkImageFilters.h>

#include <algorithm>
#include <cmath>

namespace bro::canvas {

// ---------------------------------------------------------------------------
// State setters
// ---------------------------------------------------------------------------

void CanvasScene::setFillColor(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    state_.fillPaint.setColor(SkColorSetARGB(a, r, g, b));
    // Canvas 2D spec: assigning a solid color to fillStyle replaces any
    // gradient/pattern shader that was previously there.
    state_.fillPaint.setShader(nullptr);
    state_.fillPattern.reset();
}

void CanvasScene::getFillColor(uint8_t& r, uint8_t& g, uint8_t& b, uint8_t& a) const {
    SkColor c = state_.fillPaint.getColor();
    a = SkColorGetA(c); r = SkColorGetR(c); g = SkColorGetG(c); b = SkColorGetB(c);
}

void CanvasScene::setStrokeColor(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    state_.strokePaint.setColor(SkColorSetARGB(a, r, g, b));
    state_.strokePaint.setShader(nullptr);
    state_.strokePattern.reset();
}

void CanvasScene::getStrokeColor(uint8_t& r, uint8_t& g, uint8_t& b, uint8_t& a) const {
    SkColor c = state_.strokePaint.getColor();
    a = SkColorGetA(c); r = SkColorGetR(c); g = SkColorGetG(c); b = SkColorGetB(c);
}

void CanvasScene::setFillShader(sk_sp<SkShader> shader) {
    state_.fillPaint.setShader(std::move(shader));
    state_.fillPattern.reset();
}

void CanvasScene::setStrokeShader(sk_sp<SkShader> shader) {
    state_.strokePaint.setShader(std::move(shader));
    state_.strokePattern.reset();
}

void CanvasScene::setFillPattern(std::shared_ptr<CanvasPatternData> pattern) {
    state_.fillPaint.setShader(nullptr);
    state_.fillPattern = std::move(pattern);
}

void CanvasScene::setStrokePattern(std::shared_ptr<CanvasPatternData> pattern) {
    state_.strokePaint.setShader(nullptr);
    state_.strokePattern = std::move(pattern);
}

void CanvasScene::setLineWidth(float w) {
    state_.lineWidthVal = w;
    state_.strokePaint.setStrokeWidth(w);
}

float CanvasScene::lineWidth() const { return state_.lineWidthVal; }

void CanvasScene::setGlobalAlpha(float a) {
    state_.globalAlphaVal = std::clamp(a, 0.0f, 1.0f);
}

float CanvasScene::globalAlpha() const { return state_.globalAlphaVal; }

void CanvasScene::setLineCap(int cap) {
    state_.lineCapVal = cap;
    static const SkPaint::Cap caps[] = { SkPaint::kButt_Cap, SkPaint::kRound_Cap, SkPaint::kSquare_Cap };
    if (cap >= 0 && cap <= 2) state_.strokePaint.setStrokeCap(caps[cap]);
}

int CanvasScene::lineCap() const { return state_.lineCapVal; }

void CanvasScene::setLineJoin(int join) {
    state_.lineJoinVal = join;
    static const SkPaint::Join joins[] = { SkPaint::kMiter_Join, SkPaint::kRound_Join, SkPaint::kBevel_Join };
    if (join >= 0 && join <= 2) state_.strokePaint.setStrokeJoin(joins[join]);
}

int CanvasScene::lineJoin() const { return state_.lineJoinVal; }

void CanvasScene::setMiterLimit(float limit) {
    state_.miterLimitVal = limit;
    state_.strokePaint.setStrokeMiter(limit);
}

float CanvasScene::miterLimit() const { return state_.miterLimitVal; }

static SkBlendMode blendModeFromOp(int op) {
    switch (op) {
    case 0:  return SkBlendMode::kSrcOver;
    case 1:  return SkBlendMode::kSrcIn;
    case 2:  return SkBlendMode::kSrcOut;
    case 3:  return SkBlendMode::kSrcATop;
    case 4:  return SkBlendMode::kDstOver;
    case 5:  return SkBlendMode::kDstIn;
    case 6:  return SkBlendMode::kDstOut;
    case 7:  return SkBlendMode::kDstATop;
    case 8:  return SkBlendMode::kLighten;
    case 9:  return SkBlendMode::kDarken;
    case 10: return SkBlendMode::kXor;
    case 11: return SkBlendMode::kPlus;
    case 12: return SkBlendMode::kMultiply;
    case 13: return SkBlendMode::kScreen;
    case 14: return SkBlendMode::kOverlay;
    case 15: return SkBlendMode::kColorDodge;
    case 16: return SkBlendMode::kColorBurn;
    case 17: return SkBlendMode::kHardLight;
    case 18: return SkBlendMode::kSoftLight;
    case 19: return SkBlendMode::kDifference;
    case 20: return SkBlendMode::kExclusion;
    default: return SkBlendMode::kSrcOver;
    }
}

void CanvasScene::setGlobalCompositeOperation(int op) {
    state_.compositeOp = op;
}

int CanvasScene::globalCompositeOperation() const { return state_.compositeOp; }

void CanvasScene::setFont(const std::string& fontStr) {
    state_.fontStr = fontStr;
    applyFont();
}

void CanvasScene::setTextAlign(int align) {
    state_.textAlignVal = align;
}

void CanvasScene::setTextBaseline(int bl) {
    state_.textBaselineVal = bl;
}

void CanvasScene::setDirection(int dir) {
    state_.directionVal = dir;
}

void CanvasScene::setShadowBlur(float blur) {
    if (std::isfinite(blur) && blur >= 0.0f) state_.shadowBlurVal = blur;
}

void CanvasScene::setShadowColor(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    state_.shadowR = r; state_.shadowG = g; state_.shadowB = b; state_.shadowA = a;
}

void CanvasScene::getShadowColor(uint8_t& r, uint8_t& g, uint8_t& b, uint8_t& a) const {
    r = state_.shadowR; g = state_.shadowG; b = state_.shadowB; a = state_.shadowA;
}

void CanvasScene::setShadowOffsetX(float x) { if (std::isfinite(x)) state_.shadowOX = x; }
void CanvasScene::setShadowOffsetY(float y) { if (std::isfinite(y)) state_.shadowOY = y; }

void CanvasScene::setImageSmoothingEnabled(bool v) {
    state_.imgSmooth = v;
}

void CanvasScene::setImageSmoothingQuality(int q) {
    if (q >= 0 && q <= 2) state_.smoothQuality = q;
}

bool CanvasScene::setFilter(const std::string& filter, FilterColor currentColor,
                            FilterLengthContext lengths) {
    std::vector<render::CssFilterParams> list;
    // em in a filter is the context's font-size when the filter is set.
    lengths.fontSize = parseCSSFont(state_.fontStr).size;
    if (!parseCanvasFilter(filter, list, currentColor, lengths)) return false;
    state_.filterStr = filter;
    // "none" parses to an empty list, and an empty chain is a null filter, so
    // a filter that does nothing costs a draw nothing.
    state_.filter = list.empty() ? nullptr : render::BuildSkImageFilterChain(list);
    return true;
}

// ---------------------------------------------------------------------------
// Font management
// ---------------------------------------------------------------------------

SkFontMgr* CanvasScene::ensureFontMgr() {
    if (fontMgr_) return fontMgr_.get();
    fontMgr_ = sk_ref_sp(render::systemFontMgr());
    return fontMgr_.get();
}

void CanvasScene::applyFont() {
    const std::string& fontStr = state_.fontStr;
    auto it = fontCache_.find(fontStr);
    if (it != fontCache_.end()) {
        font_ = it->second.font;
        fontFamily_ = it->second.family;
        fontStyle_ = it->second.style;
        return;
    }

    auto pf = parseCSSFont(fontStr);

    SkFontStyle style(
        pf.weight,
        SkFontStyle::kNormal_Width,
        pf.italic ? SkFontStyle::kItalic_Slant : SkFontStyle::kUpright_Slant);

    // font-family is a comma-separated fallback list (e.g. "system-ui,
    // -apple-system, Segoe UI, sans-serif") — resolve it the same way every
    // other text path does (bro::render::resolveFontFamilyList), so a
    // multi-family list can't silently resolve to no glyphs.
    sk_sp<SkTypeface> tf = bro::render::resolveFontFamilyList(pf.family, style, ensureFontMgr());

    SkFont f(tf, pf.size);
    f.setEdging(SkFont::Edging::kSubpixelAntiAlias);
    // Subpixel advances, for the same reason the renderers set it: HarfBuzz
    // asks the SkFont for each glyph's width and rounds to a whole pixel
    // unless the font opts in, which quantizes every advance and drifts a
    // string's measured width away from what the font specifies.
    f.setSubpixel(true);

    fontCache_[fontStr] = {tf, f, pf.family, style};
    font_ = f;
    fontFamily_ = pf.family;
    fontStyle_ = style;
}

render::TextDirection CanvasScene::baseDirection() const {
    // `inherit` should take the canvas element's computed CSS direction. The
    // scene reaches its backing element only through opaque callbacks, so it
    // cannot read a computed style from here; `inherit` therefore behaves as
    // `ltr`, which is what it resolves to for any document that has not set
    // direction. Scripts that need RTL on a canvas set ctx.direction = "rtl"
    // explicitly, which is honoured exactly.
    return state_.directionVal == 1 ? render::TextDirection::RTL : render::TextDirection::LTR;
}

const render::ShapedRun* CanvasScene::shapeCurrent(std::string_view text) {
    if (text.empty()) return nullptr;
    return shaper_.shape(text, font_, fontFamily_, fontStyle_,
                         ensureFontMgr(), fallbackCache_, baseDirection());
}

// ---------------------------------------------------------------------------
// Paint helpers
// ---------------------------------------------------------------------------

bool CanvasScene::shadowActive() const {
    return state_.shadowA > 0 &&
           (state_.shadowBlurVal > 0.0f || state_.shadowOX != 0.0f || state_.shadowOY != 0.0f);
}

bool CanvasScene::drawIsLayered() const {
    return state_.filter || shadowActive();
}

sk_sp<SkImageFilter> CanvasScene::shadowFilter() const {
    if (!shadowActive()) return nullptr;
    const float sigma = state_.shadowBlurVal / 2.0f;
    const SkColor color = SkColorSetARGB(state_.shadowA, state_.shadowR, state_.shadowG, state_.shadowB);
    // The filter's output is what the shadow is cast from, so it is the
    // shadow's input; null means the draw itself.
    return SkImageFilters::DropShadowOnly(state_.shadowOX, state_.shadowOY, sigma, sigma,
                                          color, state_.filter);
}

float CanvasScene::drawAlpha() const {
    return drawIsLayered() ? 1.0f : state_.globalAlphaVal;
}

SkBlendMode CanvasScene::drawBlend() const {
    return drawIsLayered() ? SkBlendMode::kSrcOver : blendModeFromOp(state_.compositeOp);
}

// imageSmoothingQuality follows what Chromium maps its three levels to:
// `low` is bilinear, `medium` adds mipmaps (so a large downscale averages
// rather than aliasing), `high` is a cubic resampler. With smoothing off the
// quality is irrelevant: nearest neighbour.
SkSamplingOptions CanvasScene::imageSampling() const {
    if (!state_.imgSmooth) {
        return SkSamplingOptions(SkFilterMode::kNearest, SkMipmapMode::kNone);
    }
    switch (state_.smoothQuality) {
    case 2:  return SkSamplingOptions(SkCubicResampler::Mitchell());
    case 1:  return SkSamplingOptions(SkFilterMode::kLinear, SkMipmapMode::kLinear);
    default: return SkSamplingOptions(SkFilterMode::kLinear, SkMipmapMode::kNone);
    }
}

void CanvasScene::applyPattern(SkPaint& p, const std::shared_ptr<CanvasPatternData>& pat) const {
    if (!pat || !pat->image) return;
    p.setShader(pat->image->makeShader(pat->tileX, pat->tileY, imageSampling(), &pat->transform));
}

SkPaint CanvasScene::makeFillPaint() const {
    SkPaint p = state_.fillPaint;
    applyPattern(p, state_.fillPattern);
    // Skia multiplies the paint's color alpha into the shader output. The
    // Canvas 2D spec says a gradient/pattern carries its own colors and only
    // globalAlpha modulates them — but setFillShader leaves whatever alpha
    // the previous `fillStyle = "rgba(...)"` assignment baked into the
    // paint color, which would silently dim the first gradient draw after
    // any low-alpha solid fill.
    if (p.getShader()) p.setAlphaf(drawAlpha());
    else               p.setAlphaf(p.getAlphaf() * drawAlpha());
    p.setBlendMode(drawBlend());
    return p;
}

SkPaint CanvasScene::makeImagePaint() const {
    SkPaint p;
    p.setAlphaf(drawAlpha());
    p.setBlendMode(drawBlend());
    return p;
}

void CanvasScene::recordDraw(CanvasCmd&& cmd) {
    if (drawIsLayered()) {
        cmd.filter = state_.filter;
        cmd.shadow = shadowFilter();
        cmd.layerAlpha = state_.globalAlphaVal;
        cmd.layerBlend = blendModeFromOp(state_.compositeOp);
    }
    commands_.push_back(std::move(cmd));
    dirty_ = true;
    snapshotValid_ = false;
    snapshotImageValid_ = false;
}

SkPaint CanvasScene::makeStrokePaint() const {
    SkPaint p = state_.strokePaint;
    applyPattern(p, state_.strokePattern);
    if (p.getShader()) p.setAlphaf(drawAlpha());
    else               p.setAlphaf(p.getAlphaf() * drawAlpha());
    p.setBlendMode(drawBlend());
    if (!state_.lineDash.empty()) {
        // HTML5 spec: odd-length segment arrays are doubled.
        std::vector<float> segs = state_.lineDash;
        if (segs.size() % 2 == 1) segs.insert(segs.end(), state_.lineDash.begin(), state_.lineDash.end());
        p.setPathEffect(SkDashPathEffect::Make(
            SkSpan<const SkScalar>(segs.data(), segs.size()),
            state_.lineDashOffset));
    }
    return p;
}

void CanvasScene::setLineDash(const std::vector<float>& segments) {
    // A negative or non-finite entry makes the whole call a no-op: the spec
    // returns before touching the dash list, so the previous dash survives.
    for (float v : segments) {
        if (!(v >= 0) || !std::isfinite(v)) return;
    }
    state_.lineDash = segments;
}

const std::vector<float>& CanvasScene::lineDash() const { return state_.lineDash; }

void CanvasScene::setLineDashOffset(float off) {
    if (std::isfinite(off)) state_.lineDashOffset = off;
}

float CanvasScene::lineDashOffset() const { return state_.lineDashOffset; }

float CanvasScene::adjustTextX(float x, float textWidth) const {
    // `start` and `end` name the direction-relative edges: in RTL text the
    // start edge is the right one. `left` and `right` are absolute and do not
    // move. Getting this wrong is invisible in LTR, which is why the two pairs
    // were conflated before there was a direction to resolve them against.
    const bool rtl = baseDirection() == render::TextDirection::RTL;
    switch (state_.textAlignVal) {
    case 1: return x - textWidth / 2.0f;             // center
    case 2: return x - textWidth;                    // right
    case 0: return rtl ? x - textWidth : x;          // start
    case 3: return rtl ? x : x - textWidth;          // end
    default: return x;                                // left
    }
}

// The em box, normalized: the font's OS/2 typographic ascender and descender
// scaled so they sum to the font size.
//
// This is not the same as SkFontMetrics' fAscent/fDescent, which come from hhea
// and describe the font's LINE box — for Arial that is 0.905em of ascent, well
// above the em square. `top`, `middle` and `bottom` name the em box, not the
// line box, so using hhea for them put text about 4px off at 32px versus every
// browser. Chromium calls the same quantity the normalized typo ascent.
//
// Falls back to the hhea ratio when the face has no usable OS/2 table, which
// keeps a bitmap or synthesized face from collapsing to zero.
bool CanvasScene::typoMetrics(float& ascent, float& descent) const {
    const SkTypeface* tf = font_.getTypeface();
    const float size = font_.getSize();
    if (tf) {
        // OS/2: sTypoAscender is at byte 68, sTypoDescender at 70, both int16
        // in font design units. A table shorter than that is not version 0.
        constexpr SkFourByteTag kOS2 = SkSetFourByteTag('O', 'S', '/', '2');
        uint8_t buf[74];
        const size_t n = tf->getTableData(kOS2, 0, sizeof(buf), buf);
        const int upem = tf->getUnitsPerEm();
        if (n >= 72 && upem > 0) {
            auto be16 = [&](size_t off) -> int16_t {
                return static_cast<int16_t>((buf[off] << 8) | buf[off + 1]);
            };
            const int asc = be16(68);
            const int desc = be16(70);   // negative below the baseline
            const float span = static_cast<float>(asc) - static_cast<float>(desc);
            if (span > 0.0f && asc > 0) {
                ascent = size * static_cast<float>(asc) / span;
                descent = size - ascent;
                return true;
            }
        }
    }
    SkFontMetrics fm;
    font_.getMetrics(&fm);
    const float span = (-fm.fAscent) + fm.fDescent;
    if (span <= 0.0f) { ascent = size * 0.8f; descent = size * 0.2f; return false; }
    ascent = size * (-fm.fAscent) / span;
    descent = size - ascent;
    return false;
}

float CanvasScene::adjustTextY(float y) const {
    SkFontMetrics metrics;
    font_.getMetrics(&metrics);
    float emAsc = 0.0f, emDesc = 0.0f;
    typoMetrics(emAsc, emDesc);
    switch (state_.textBaselineVal) {
    case 1: return y + emAsc;                              // top of the em box
    case 2: return y + emAsc - font_.getSize() / 2.0f;     // middle of it
    case 3: return y - emDesc;                             // bottom of it
    case 4: return y - metrics.fAscent * 0.8f;    // hanging (approximation)
    case 5: return y - emDesc;                    // ideographic ≈ bottom
    default: return y;                             // alphabetic (baseline)
    }
}

// ---------------------------------------------------------------------------
// Text drawing
// ---------------------------------------------------------------------------

// fillText and strokeText differ only in which paint they build. Both shape
// here, on the JS thread, and record the resulting blob: replay then
// replays glyphs without shaping, and without re-deriving font fallback for
// every frame the way drawSimpleText did.
void CanvasScene::recordText(bool stroke, const std::string& text, float x, float y, float maxWidth) {
    if (text.empty()) return;
    std::string utf8Scratch;
    std::string_view t = render::ensureValidUtf8(text, utf8Scratch);

    CanvasCmd cmd;
    cmd.type = stroke ? CanvasCmd::kStrokeText : CanvasCmd::kFillText;
    cmd.paint = stroke ? makeStrokePaint() : makeFillPaint();

    float tw = 0.0f;
    if (const render::ShapedRun* run = shapeCurrent(t)) {
        tw = run->width();
        cmd.blob = run->makeBlob();
    } else {
        // Shaping produced nothing (empty after validation, or a build with
        // BRO_WITH_TEXT_SHAPING off and no glyphs). Fall back to the raw
        // string so the replay path still has something to draw.
        tw = font_.measureText(t.data(), t.size(), SkTextEncoding::kUTF8);
    }

    if (maxWidth > 0.0f && tw > maxWidth) {
        cmd.p[0] = adjustTextX(0.0f, tw);
        cmd.p[1] = adjustTextY(y);
        cmd.p[2] = maxWidth / tw;
        cmd.p[3] = x;
    } else {
        cmd.p[0] = adjustTextX(x, tw);
        cmd.p[1] = adjustTextY(y);
        cmd.p[2] = 1.0f;
        cmd.p[3] = 0.0f;
    }
    cmd.text = std::string(t);
    cmd.font = font_;
    recordDraw(std::move(cmd));
}

void CanvasScene::fillText(const std::string& text, float x, float y, float maxWidth) {
    recordText(/*stroke=*/false, text, x, y, maxWidth);
}

void CanvasScene::strokeText(const std::string& text, float x, float y, float maxWidth) {
    recordText(/*stroke=*/true, text, x, y, maxWidth);
}

CanvasTextMetrics CanvasScene::measureText(const std::string& text) {
    SkFontMetrics fm;
    font_.getMetrics(&fm);
    std::string utf8Scratch;
    std::string_view t = render::ensureValidUtf8(text, utf8Scratch);

    CanvasTextMetrics m;
    SkRect ink = SkRect::MakeEmpty();
    if (const render::ShapedRun* run = shapeCurrent(t)) {
        m.width = run->width();
        ink = run->inkBounds();
    }

    // Everything below is relative to the alignment point. `anchorX` is where
    // the text's left edge lands relative to it, and `baseOff` is where the
    // alphabetic baseline lands (positive = below). Both come from the very
    // functions fillText uses to place the text, so the reported box is
    // exactly the box that would be drawn.
    const float anchorX = adjustTextX(0.0f, m.width);
    const float baseOff = adjustTextY(0.0f);

    m.actualLeft  = -(ink.fLeft + anchorX);
    m.actualRight = ink.fRight + anchorX;
    // Skia's glyph bounds are y-down from the baseline: fTop is negative for
    // ink above it. Ascent is reported positive upward, hence the negation.
    m.actualAscent  = -(baseOff + ink.fTop);
    m.actualDescent = baseOff + ink.fBottom;

    m.fontAscent  = -(baseOff + fm.fAscent);
    m.fontDescent = baseOff + fm.fDescent;

    // The em square, from the face's OS/2 typographic metrics — the same
    // quantity `textBaseline: top` and `bottom` name, so these agree with
    // where the text would actually be placed.
    float emAsc = 0.0f, emDesc = 0.0f;
    typoMetrics(emAsc, emDesc);
    m.emAscent  = emAsc - baseOff;
    m.emDescent = emDesc + baseOff;

    m.alphabeticBaseline  = -baseOff;
    // No face here exposes a BASE table, so the hanging and ideographic
    // baselines use the conventional fractions of the ascent and descent that
    // browsers fall back to for fonts without one.
    m.hangingBaseline     = -baseOff + 0.8f * (-fm.fAscent);
    m.ideographicBaseline = -baseOff - fm.fDescent;
    return m;
}

} // namespace bro::canvas
