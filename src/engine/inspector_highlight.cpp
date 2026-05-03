#include "engine/inspector_highlight.h"
#include "dom/element.h"
#include "render/renderer.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

namespace bro::engine {

namespace {

using render::Color;

// Devtools-style colors. Margin/padding/content rings are translucent so
// stacked layers visibly mix where they overlap.
constexpr Color kMarginColor  {246, 178, 107, 110};
constexpr Color kBorderColor  {255, 229, 153, 140};
constexpr Color kPaddingColor {147, 196, 125, 130};
constexpr Color kContentColor {111, 168, 220, 130};
constexpr Color kOutlineColor {  0, 200, 255, 230};
constexpr Color kGuideColor   {  0, 200, 255,  90};  // viewport guides
constexpr Color kLabelBg      { 28,  28,  32, 235};
constexpr Color kLabelFg      {255, 255, 255, 255};
constexpr Color kLabelTagFg   {108, 158, 248, 255};
constexpr Color kLabelDimFg   {200, 200, 200, 255};
constexpr Color kEdgeNumFg    { 30,  30,  30, 235};

constexpr float kLabelFontSize = 11.0f;
constexpr float kEdgeFontSize  = 10.0f;

std::string formatPx(float v) {
    char buf[32];
    // CSS-style: drop trailing zeros, no decimal for whole pixels.
    float rounded = std::round(v * 10.0f) / 10.0f;
    if (rounded == std::round(rounded)) {
        std::snprintf(buf, sizeof(buf), "%d", static_cast<int>(rounded));
    } else {
        std::snprintf(buf, sizeof(buf), "%.1f", rounded);
    }
    return std::string(buf);
}

// Fill an outer rect minus an inner rect — the four-strip "donut" that gives
// devtools box-model overlays their layered look. Caller passes outer/inner
// rects in the same coordinate space.
void drawDonut(render::Renderer* r,
               float ox, float oy, float ow, float oh,
               float ix, float iy, float iw, float ih,
               Color c) {
    if (ow <= 0 || oh <= 0) return;
    // top strip
    if (iy > oy)         r->fillRect(ox, oy, ow, iy - oy, c);
    // bottom strip
    if (iy + ih < oy + oh)
                         r->fillRect(ox, iy + ih, ow, (oy + oh) - (iy + ih), c);
    // left strip (between inner top and bottom only)
    if (ix > ox)         r->fillRect(ox, iy, ix - ox, ih, c);
    // right strip
    if (ix + iw < ox + ow)
                         r->fillRect(ix + iw, iy, (ox + ow) - (ix + iw), ih, c);
}

} // namespace

void drawInspectorHighlight(render::Renderer* renderer, dom::Element* el,
                            float scrollY, int insetLeft, int insetTop,
                            int viewportW, int viewportH) {
    if (!renderer || !el) return;
    const auto& box = el->layoutBox();
    if (box.contentRect.width <= 0 && box.contentRect.height <= 0) return;

    // Accumulate ancestor offsets up to the document root. Each layout parent
    // contributes its content-rect origin minus its scrollTop.
    float ox = 0, oy = 0;
    for (auto* p = el->layoutParent(); p; p = p->layoutParent()) {
        const auto& pb = p->layoutBox();
        ox += pb.contentRect.x;
        oy += pb.contentRect.y;
        oy -= p->scrollTopValue();
    }
    // Translate into the same coordinate system the app draw pass uses:
    // (insetLeft, insetTop) translation, minus the document scroll.
    ox += static_cast<float>(insetLeft);
    oy += static_cast<float>(insetTop) - scrollY;

    // Box rects (in the accumulated screen space).
    float cx = ox + box.contentRect.x;
    float cy = oy + box.contentRect.y;
    float cw = box.contentRect.width;
    float ch = box.contentRect.height;

    float pX = cx - box.padding.left;
    float pY = cy - box.padding.top;
    float pW = cw + box.padding.left + box.padding.right;
    float pH = ch + box.padding.top + box.padding.bottom;

    float bX = pX - box.border.left;
    float bY = pY - box.border.top;
    float bW = pW + box.border.left + box.border.right;
    float bH = pH + box.border.top + box.border.bottom;

    float mX = bX - box.margin.left;
    float mY = bY - box.margin.top;
    float mW = bW + box.margin.left + box.margin.right;
    float mH = bH + box.margin.top + box.margin.bottom;

    // Margin (outermost) → border → padding rings, content fill in the middle.
    drawDonut(renderer, mX, mY, mW, mH, bX, bY, bW, bH, kMarginColor);
    drawDonut(renderer, bX, bY, bW, bH, pX, pY, pW, pH, kBorderColor);
    drawDonut(renderer, pX, pY, pW, pH, cx, cy, cw, ch, kPaddingColor);
    renderer->fillRect(cx, cy, cw, ch, kContentColor);

    // Crisp 1px outline on the border rect makes the selected element easy
    // to track on light backgrounds where the translucent fills wash out.
    renderer->drawRect(bX, bY, bW, bH, kOutlineColor);

    // Viewport guide lines from each border-rect edge to the viewport edges.
    // 1-pixel translucent cyan, plus the same color stroked across the rect
    // edges themselves — gives a visual anchor without being noisy.
    if (viewportW > 0 && viewportH > 0) {
        float vRight  = static_cast<float>(insetLeft + viewportW);
        float vBottom = static_cast<float>(insetTop + viewportH);
        float vLeft   = static_cast<float>(insetLeft);
        float vTop    = static_cast<float>(insetTop);
        // Vertical guides at left/right edges of the border rect.
        renderer->drawLine(bX,        vTop,    bX,        vBottom, kGuideColor, 1.0f);
        renderer->drawLine(bX + bW,   vTop,    bX + bW,   vBottom, kGuideColor, 1.0f);
        // Horizontal guides at top/bottom edges.
        renderer->drawLine(vLeft, bY,      vRight, bY,        kGuideColor, 1.0f);
        renderer->drawLine(vLeft, bY + bH, vRight, bY + bH,   kGuideColor, 1.0f);
    }

    // Numeric edge labels for margin and padding sides — only drawn when the
    // strip is fat enough to actually fit the digits without overflowing into
    // the next ring. Skia caches typeface lookups so creating per-frame is OK.
    uint64_t edgeFont = renderer->createFont("Consolas", kEdgeFontSize, 400, false);
    auto strLabel = [&](float x, float y, const std::string& s, render::Color c) {
        renderer->drawText(s, x, y, edgeFont, c);
    };
    auto edgeLabel = [&](float v, float cx_, float cy_, float minDim) {
        if (v <= 0.5f) return;
        std::string s = formatPx(v);
        auto m = renderer->measureText(s, edgeFont);
        if (m.width + 4 > minDim) return; // doesn't fit
        // Faint white pill behind the digits so the value reads against any
        // background, then the dark digits on top.
        renderer->fillRect(cx_ - m.width * 0.5f - 2.0f,
                           cy_ - m.height * 0.5f - 1.0f,
                           m.width + 4.0f, m.height + 2.0f,
                           render::Color{255, 255, 255, 220});
        strLabel(cx_ - m.width * 0.5f, cy_ + m.ascent * 0.5f, s, kEdgeNumFg);
    };
    // Margin strips (between border rect and margin rect).
    edgeLabel(box.margin.top,    bX + bW * 0.5f, mY + box.margin.top * 0.5f, bW);
    edgeLabel(box.margin.bottom, bX + bW * 0.5f,
              bY + bH + box.margin.bottom * 0.5f, bW);
    edgeLabel(box.margin.left,   mX + box.margin.left * 0.5f, bY + bH * 0.5f,
              bH);
    edgeLabel(box.margin.right,
              bX + bW + box.margin.right * 0.5f, bY + bH * 0.5f, bH);
    // Padding strips (between content rect and padding rect, inside the border).
    edgeLabel(box.padding.top,    cx + cw * 0.5f, pY + box.padding.top * 0.5f, cw);
    edgeLabel(box.padding.bottom, cx + cw * 0.5f,
              cy + ch + box.padding.bottom * 0.5f, cw);
    edgeLabel(box.padding.left,   pX + box.padding.left * 0.5f, cy + ch * 0.5f,
              ch);
    edgeLabel(box.padding.right,
              cx + cw + box.padding.right * 0.5f, cy + ch * 0.5f, ch);

    // Header label: <tag.classes#id>  W × H px. Anchored above the border
    // rect when there's room, otherwise below it.
    uint64_t labelFont = renderer->createFont("Consolas", kLabelFontSize, 600, false);
    std::string tag = el->tagName();
    for (auto& c : tag) c = static_cast<char>(std::tolower(c));
    std::string idAttr = el->getAttribute("id");
    std::string cls = el->getAttribute("class");
    std::string tagText = tag;
    if (!idAttr.empty()) tagText += "#" + idAttr;
    if (!cls.empty()) {
        // Show only the first class to keep the label compact.
        size_t sp = cls.find(' ');
        std::string firstCls = sp == std::string::npos ? cls : cls.substr(0, sp);
        tagText += "." + firstCls;
    }
    std::string dimText = "  " + formatPx(box.contentRect.width) + " \xC3\x97 "
                          + formatPx(box.contentRect.height);
    auto tagM = renderer->measureText(tagText, labelFont);
    auto dimM = renderer->measureText(dimText, labelFont);
    float labelW = tagM.width + dimM.width + 14.0f;
    float labelH = std::max(tagM.height, dimM.height) + 6.0f;
    float labelX = bX;
    float labelY = bY - labelH - 2.0f;
    if (labelY < static_cast<float>(insetTop)) {
        labelY = bY + bH + 2.0f;       // not enough room above; place below
    }
    if (labelX + labelW > static_cast<float>(insetLeft + std::max(viewportW, 1))) {
        labelX = static_cast<float>(insetLeft + std::max(viewportW, 1)) - labelW;
    }
    if (labelX < static_cast<float>(insetLeft)) {
        labelX = static_cast<float>(insetLeft);
    }
    renderer->fillRect(labelX, labelY, labelW, labelH, kLabelBg);
    float textY = labelY + labelH * 0.5f + tagM.ascent * 0.5f - 1.0f;
    renderer->drawText(tagText, labelX + 7.0f, textY, labelFont, kLabelTagFg);
    renderer->drawText(dimText, labelX + 7.0f + tagM.width, textY,
                       labelFont, kLabelDimFg);

    renderer->deleteFont(labelFont);
    renderer->deleteFont(edgeFont);
}

} // namespace bro::engine
