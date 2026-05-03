#include "engine/inspector_highlight.h"
#include "dom/element.h"
#include "render/renderer.h"

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
                            float scrollY, int insetLeft, int insetTop) {
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
}

} // namespace bro::engine
