// Borders: the border-collapse table grid, the <fieldset> legend gap, and the
// separated-border painters (uniform rects, rounded strokes, per-side colour
// wedges, trapezoids, and the dashed / dotted / double / 3D styles).
// border-image is in draw_traversal_border_image.cpp.

#include "layout/draw_traversal_internal.h"
#include "dom/node.h"

#include <algorithm>
#include <cmath>
#include <span>

namespace bro::layout {

// Return true if `elem` is a table-cell whose enclosing table has
// `border-collapse: collapse`. In that case, borders are painted centered on
// the cell's border-box edge so adjacent cells share a single grid line.
static bool isCellInCollapsedTable(dom::Element* elem) {
    if (!elem) return false;
    auto& cs = elem->computedStyle();
    auto dIt = cs.find("display");
    if (dIt == cs.end() || dIt->second != "table-cell") return false;
    // Walk up looking for the enclosing table.
    auto* p = elem->layoutParent();
    while (p) {
        auto& ps = p->computedStyle();
        auto pdIt = ps.find("display");
        const std::string& pd = (pdIt != ps.end()) ? pdIt->second : std::string{};
        if (pd == "table" || pd == "inline-table") {
            auto bcIt = ps.find("border-collapse");
            return (bcIt != ps.end() && bcIt->second == "collapse");
        }
        p = p->layoutParent();
    }
    return false;
}

// Find the enclosing collapsed-mode table for a cell. Returns nullptr if not
// in a collapsed table.
static dom::Element* enclosingCollapsedTable(dom::Element* elem) {
    if (!elem) return nullptr;
    auto* p = elem->layoutParent();
    while (p) {
        auto& ps = p->computedStyle();
        auto pdIt = ps.find("display");
        const std::string& pd = (pdIt != ps.end()) ? pdIt->second : std::string{};
        if (pd == "table" || pd == "inline-table") {
            auto bcIt = ps.find("border-collapse");
            if (bcIt != ps.end() && bcIt->second == "collapse") return p;
            return nullptr;
        }
        p = p->layoutParent();
    }
    return nullptr;
}

float DrawTraversal::fieldsetTopShift(dom::Element* elem, float x, float y,
                                      float* gapX0, float* gapX1) {
    std::string tag = elem->tagName();
    if (tag != "fieldset" && tag != "FIELDSET") return 0.0f;
    for (auto* child : elem->composedChildNodes()) {
        if (!child || child->nodeType() != dom::NodeType::Element) continue;
        auto* le = static_cast<dom::Element*>(child);
        std::string t = le->tagName();
        if (t != "legend" && t != "LEGEND") continue;
        auto& cs = le->computedStyle();
        auto dIt = cs.find("display");
        if (dIt != cs.end() && dIt->second == "none") break;
        auto& box = elem->layoutBox();
        auto& lb = le->layoutBox();
        float contentX = x + box.border.left + box.padding.left;
        float contentY = y + box.border.top + box.padding.top;
        float lx = contentX + lb.contentRect.x - lb.padding.left - lb.border.left;
        float ly = contentY + lb.contentRect.y - lb.padding.top - lb.border.top;
        float centerY = ly + lb.fullHeight() * 0.5f;
        if (gapX0) *gapX0 = lx;
        if (gapX1) *gapX1 = lx + lb.fullWidth();
        return std::max(0.0f, (centerY - box.border.top * 0.5f) - y);
    }
    return 0.0f;
}

void DrawTraversal::drawBorders(dom::Element* elem, float x, float y, float w, float h) {
    auto& box = elem->layoutBox();
    auto& style = elem->computedStyle();
    render::Radii radii = getRadii(style, w, h);
    bool rounded = !radii.isZero();

    // --- border-collapse: collapse painting ---------------------------------
    // Table cells in collapsed mode paint each side centered on the cell's
    // border-box edge so adjacent cells share a single grid line. The table
    // itself paints its outer border the same way (its layout box.border has
    // been zeroed by the table layout — we read widths from the style).
    bool cellCollapse  = isCellInCollapsedTable(elem);
    bool tableCollapse = isCollapsedTable(elem);
    if (cellCollapse || tableCollapse) {
        struct Side { float wpx; std::string color; std::string st; };
        const char* widthProps[4] = {"border-top-width", "border-right-width",
                                     "border-bottom-width", "border-left-width"};
        const char* styleProps2[4] = {"border-top-style", "border-right-style",
                                      "border-bottom-style", "border-left-style"};
        const char* colorProps2[4] = {"border-top-color", "border-right-color",
                                      "border-bottom-color", "border-left-color"};
        Side sides4[4];
        for (int i = 0; i < 4; ++i) {
            auto stIt = style.find(styleProps2[i]);
            sides4[i].st = (stIt == style.end()) ? "solid" : stIt->second;
            sides4[i].wpx = (sides4[i].st == "none") ? 0.0f
                                                     : styleLengthPx(style, widthProps[i]);
            auto cIt = style.find(colorProps2[i]);
            sides4[i].color = (cIt == style.end()) ? "" : cIt->second;
        }
        // Cells in collapsed mode: the layout `box.border` for cells is still
        // their full unmerged border. Use box values to keep sub-px parity
        // with what the cell would have drawn pre-collapse-fix.
        if (cellCollapse) {
            sides4[0].wpx = box.border.top;
            sides4[1].wpx = box.border.right;
            sides4[2].wpx = box.border.bottom;
            sides4[3].wpx = box.border.left;
        }
        auto parseColor = [&](const std::string& s) -> bromath::Color {
            bromath::Color c = cfromColor8({0, 0, 0, 255});
            if (!s.empty()) tryParseColor(s, c);
            return c;
        };
        // Each side: rect spans from outer half-line to inner half-line,
        // centered on the cell's border-box edge.
        float T = sides4[0].wpx, R = sides4[1].wpx,
              B = sides4[2].wpx, L = sides4[3].wpx;

        auto paintStripe = [&](int idx, float sx, float sy, float sw, float sh) {
            if (sides4[idx].st == "none" || sides4[idx].wpx <= 0) return;
            bromath::Color c = parseColor(sides4[idx].color);
            const std::string& st = sides4[idx].st;
            bool horizontal = (idx == 0 || idx == 2);
            float w0 = horizontal ? sh : sw; // border thickness
            if (st == "double") {
                // Two strokes each ~floor(width/3), separated by a gap.
                float t = std::max(1.0f, std::floor(w0 / 3.0f));
                if (horizontal) {
                    renderer_->fillRect(sx, sy, sw, t, c);
                    renderer_->fillRect(sx, sy + sh - t, sw, t, c);
                } else {
                    renderer_->fillRect(sx, sy, t, sh, c);
                    renderer_->fillRect(sx + sw - t, sy, t, sh, c);
                }
                return;
            }
            // Solid (and unknown-style fallback). Collapsed-border mode does
            // not render dashed/dotted stripes; they paint as solid.
            renderer_->fillRect(sx, sy, sw, sh, c);
        };

        // For cells: only paint top + left. The shared bottom/right edges with
        // neighbors are painted by the next row/column's top/left — painting
        // both sides would double the gridline. The table's outer bottom/right
        // are painted by the table itself (post-children pass). The cell's
        // outer top/left edges are also handled by the table when this cell
        // sits flush against the table's content area, so we suppress those
        // to avoid stacking with the table's outer paint.
        // For tables: paint all four outer sides.
        bool suppressTop  = false;
        bool suppressLeft = false;
        if (cellCollapse) {
            if (auto* tbl = enclosingCollapsedTable(elem)) {
                const auto& tcr = tbl->layoutBox().contentRect;
                const float eps = 0.5f;
                if (std::abs(y - tcr.y) < eps) suppressTop = true;
                if (std::abs(x - tcr.x) < eps) suppressLeft = true;
            }
        }
        // Top
        if (!suppressTop)
            paintStripe(0, x - L * 0.5f, y - T * 0.5f, w + (L + R) * 0.5f, T);
        // Left
        if (!suppressLeft)
            paintStripe(3, x - L * 0.5f, y - T * 0.5f, L, h + (T + B) * 0.5f);
        // Tables: also paint bottom + right outer edges (cells never paint these).
        if (tableCollapse) {
            paintStripe(2, x - L * 0.5f, y + h - B * 0.5f, w + (L + R) * 0.5f, B);
            paintStripe(1, x + w - R * 0.5f, y - T * 0.5f, R, h + (T + B) * 0.5f);
        }
        return;
    }
    // --- end border-collapse painting ---------------------------------------

    // CSS border-image: when border-image-source names a loaded image it
    // REPLACES the normal border painting for this element (Backgrounds-3
    // §6). Absent, `none`, or failed sources fall through to the normal
    // border paint below.
    if (drawBorderImage(elem, x, y, w, h)) return;

    // <fieldset>: the painted border box starts at the legend's vertical
    // center and the top border skips the legend's horizontal extent.
    fieldsetGapActive_ = false;
    {
        float gx0 = 0, gx1 = 0;
        float shift = fieldsetTopShift(elem, x, y, &gx0, &gx1);
        if (shift > 0 || gx1 > gx0) {
            y += shift;
            h -= shift;
            fieldsetGapX0_ = gx0;
            fieldsetGapX1_ = gx1;
            fieldsetGapActive_ = true;
        }
    }

    auto getBorderColor = [&](const char* prop) -> bromath::Color {
        bromath::Color c = cfromColor8({0, 0, 0, 255});
        auto it = style.find(prop);
        // border-*-color initial value is currentcolor: resolve against the
        // element's color (e.g. the UA hr rule tints its border via color).
        if (it == style.end() || it->second == "currentcolor" ||
            it->second == "currentColor") {
            auto cIt = style.find("color");
            if (cIt != style.end()) tryParseColor(cIt->second, c);
            return c;
        }
        tryParseColor(it->second, c);
        return c;
    };
    auto isBorderVisible = [&](const char* styleProp) -> bool {
        auto it = style.find(styleProp);
        return it == style.end() || it->second != "none";
    };
    auto getBorderStyle = [&](const char* styleProp) -> std::string {
        auto it = style.find(styleProp);
        if (it == style.end()) return "solid";
        return it->second;
    };
    const char* sideStyleProps[4] = {"border-top-style", "border-right-style",
                                     "border-bottom-style", "border-left-style"};
    bool anyNonSolid = false;
    for (int i = 0; i < 4; ++i) {
        std::string s = getBorderStyle(sideStyleProps[i]);
        if (s != "solid" && s != "none" && !s.empty()) { anyNonSolid = true; break; }
    }

    // When all four borders have the same color AND the same width, draw as a
    // single rounded/rect stroke.  If only some sides are present (or widths
    // differ), fall through to per-side drawing — otherwise the stroke would
    // paint phantom borders on the missing sides.
    bool allSameColor = true;
    bool allSameWidth = true;
    bool allFourVisible = true;
    bool anyVisible = false;
    bromath::Color firstColor = cfromColor8({0, 0, 0, 255});
    float firstWidth = 0.0f;
    float sides[] = {box.border.top, box.border.right, box.border.bottom, box.border.left};
    const char* colorProps[] = {"border-top-color", "border-right-color",
                                "border-bottom-color", "border-left-color"};
    const char* styleProps[] = {"border-top-style", "border-right-style",
                                "border-bottom-style", "border-left-style"};
    bool firstSet = false;
    for (int i = 0; i < 4; ++i) {
        bool visible = (sides[i] > 0 && isBorderVisible(styleProps[i]));
        if (!visible) { allFourVisible = false; continue; }
        auto c = getBorderColor(colorProps[i]);
        if (!firstSet) { firstColor = c; firstWidth = sides[i]; firstSet = true; }
        else {
            if (c.r != firstColor.r || c.g != firstColor.g ||
                c.b != firstColor.b || c.a != firstColor.a) {
                allSameColor = false;
            }
            if (std::abs(sides[i] - firstWidth) > 0.01f) allSameWidth = false;
        }
        anyVisible = true;
    }

    if (!anyVisible) return;

    if (rounded && allSameColor && allSameWidth && allFourVisible && !anyNonSolid &&
        !fieldsetGapActive_) {
        // Draw a single rounded rect outline. Inset by half the (averaged)
        // border width so the centerline of the stroke lies on the border box
        // edge, matching CSS border placement.
        float avgWidth = 0; int count = 0;
        for (float s : sides) { if (s > 0) { avgWidth += s; ++count; } }
        if (count > 0) avgWidth /= count;
        float half = avgWidth / 2;
        // Shrink each corner radius by half the border width so the stroke
        // centerline traces a path with the requested outer radius.
        render::Radii inner = radii;
        for (int i = 0; i < 4; ++i) {
            inner.x[i] = std::max(0.0f, radii.x[i] - half);
            inner.y[i] = std::max(0.0f, radii.y[i] - half);
        }
        renderer_->drawRoundRectRadii(x + half, y + half, w - avgWidth, h - avgWidth,
                                      inner, avgWidth, firstColor);
        return;
    }

    float L = box.border.left;
    float R = box.border.right;
    float T = box.border.top;
    float B = box.border.bottom;

    // Rounded borders whose sides differ only in COLOR (uniform width, all four
    // visible, all solid): stroke the full rounded rect once per side, clipped
    // to that side's outer-corner→inner-corner wedge. Each side shows its own
    // color and the corners split along the diagonal — matching CSS. (The
    // all-same-color rounded fast path above already returned; this handles the
    // per-side-colored ring, e.g. a CSS loading spinner.)
    if (rounded && allSameWidth && allFourVisible && !anyNonSolid &&
        !fieldsetGapActive_) {
        float avgWidth = firstWidth;
        float half = avgWidth / 2;
        render::Radii inner = radii;
        for (int i = 0; i < 4; ++i) {
            inner.x[i] = std::max(0.0f, radii.x[i] - half);
            inner.y[i] = std::max(0.0f, radii.y[i] - half);
        }
        float ox0 = x,         oy0 = y;
        float ox1 = x + w,     oy1 = y + h;

        // How far each side's wedge reaches INWARD along the two corner split
        // lines, as a multiple of the border width. A square box needs exactly
        // 1 — the wedge is then the familiar outer-corner→inner-corner
        // trapezoid. A rounded corner needs more: its split line stays inside
        // the corner's rx×ry box for `min(rx/w, ry/w)` widths, and everything
        // the side owns of that arc lies along it. Clipping to the flat
        // trapezoid instead is what turned a spinner into four disconnected
        // chords with the corners missing — the arcs sweep a full radius deep
        // while the trapezoid is only one border-width tall.
        //
        // Never past where the two split lines cross, or the quad folds into a
        // bowtie and the clip means nothing. On a circle the crossing IS the
        // centre and the wedge is the exact quadrant, which is the whole point.
        // depth = the side's own width (how far in the split line travels per
        // unit); along = the adjacent side's width (how far sideways).
        auto wedgeK = [](float rxA, float ryA, float alongA,
                         float rxB, float ryB, float alongB,
                         float depth, float extent) {
            auto exitAt = [](float rx, float ry, float alongW, float depthW) -> float {
                if (alongW <= 0.0f || depthW <= 0.0f) return 0.0f;
                return std::min(rx / alongW, ry / depthW);
            };
            float needed = std::max(exitAt(rxA, ryA, alongA, depth),
                                    exitAt(rxB, ryB, alongB, depth));
            float cross = (alongA + alongB) > 0.0f ? extent / (alongA + alongB)
                                                   : 1.0f;
            return std::min(cross, std::max(1.0f, needed));
        };
        // radii index order is TL, TR, BR, BL.
        float kT = wedgeK(radii.x[0], radii.y[0], L, radii.x[1], radii.y[1], R, T, w);
        float kR = wedgeK(radii.x[1], radii.y[1], T, radii.x[2], radii.y[2], B, R, h);
        float kB = wedgeK(radii.x[3], radii.y[3], L, radii.x[2], radii.y[2], R, B, w);
        float kL = wedgeK(radii.x[0], radii.y[0], T, radii.x[3], radii.y[3], B, L, h);

        struct Wedge { const char* colorProp; render::PointF p[4]; };
        Wedge wedges[4] = {
            {"border-top-color",    {{ox0, oy0}, {ox1, oy0},
                                     {ox1 - R * kT, oy0 + T * kT},
                                     {ox0 + L * kT, oy0 + T * kT}}},
            {"border-right-color",  {{ox1, oy0}, {ox1, oy1},
                                     {ox1 - R * kR, oy1 - B * kR},
                                     {ox1 - R * kR, oy0 + T * kR}}},
            {"border-bottom-color", {{ox1, oy1}, {ox0, oy1},
                                     {ox0 + L * kB, oy1 - B * kB},
                                     {ox1 - R * kB, oy1 - B * kB}}},
            {"border-left-color",   {{ox0, oy1}, {ox0, oy0},
                                     {ox0 + L * kL, oy0 + T * kL},
                                     {ox0 + L * kL, oy1 - B * kL}}},
        };
        for (auto& wd : wedges) {
            auto c = getBorderColor(wd.colorProp);
            renderer_->save();
            renderer_->setClipPolygon(std::span<const render::PointF>(wd.p, 4));
            renderer_->drawRoundRectRadii(x + half, y + half, w - avgWidth, h - avgWidth,
                                          inner, avgWidth, c);
            renderer_->restore();
        }
        return;
    }

    // Uniform non-rounded borders (all four sides same color, same width):
    // emit axis-aligned rects to match prior antialiasing exactly. Trapezoid
    // edges along the corner diagonals would otherwise produce subtle AA
    // seams across the table/box-grid corpus.
    if (!rounded && allSameColor && allSameWidth && allFourVisible && !anyNonSolid &&
        !fieldsetGapActive_) {
        if (T > 0) renderer_->fillRect(x, y, w, T, firstColor);
        if (B > 0) renderer_->fillRect(x, y + h - B, w, B, firstColor);
        if (L > 0) renderer_->fillRect(x, y + T, L, h - T - B, firstColor);
        if (R > 0) renderer_->fillRect(x + w - R, y + T, R, h - T - B, firstColor);
        return;
    }

    // General case: draw each border as a trapezoid quad spanning from the two
    // outer corners of the border-box edge to the corresponding two inner
    // corners (i.e., padding-box corners). This matches CSS spec: when
    // adjacent sides have different colors/widths the seam runs diagonally
    // from outer corner to inner corner. With width/height collapsed to 0
    // (the classic CSS triangle trick) this yields the expected triangles.
    float ox0 = x,       oy0 = y;
    float ox1 = x + w,   oy1 = y + h;
    float ix0 = x + L,   iy0 = y + T;
    float ix1 = x + w - R, iy1 = y + h - B;

    // For non-solid styles we draw axis-aligned stamps along the side's
    // bounding rect (between outer and inner edges). Side index: 0=top,
    // 1=right, 2=bottom, 3=left. The trapezoid corners are still passed for
    // the solid path. Adjacent sides will overlap on the diagonal, but for
    // matching colors/widths this is invisible; for the styles tested in
    // conformance (uniform per-side style) this produces correct dashes/
    // dots/doubles.
    auto drawSide = [&](const char* colorProp, const char* styleProp,
                        int sideIndex, float w0,
                        render::PointF p0, render::PointF p1,
                        render::PointF p2, render::PointF p3) {
        if (w0 <= 0 || !isBorderVisible(styleProp)) return;
        auto c = getBorderColor(colorProp);
        std::string st = getBorderStyle(styleProp);

        // Axis-aligned stamp rect for this side (outer extent).
        float sx, sy, sw, sh;
        bool horizontal = (sideIndex == 0 || sideIndex == 2);
        if (sideIndex == 0)      { sx = x;          sy = y;          sw = w;  sh = w0; }
        else if (sideIndex == 2) { sx = x;          sy = y + h - w0; sw = w;  sh = w0; }
        else if (sideIndex == 3) { sx = x;          sy = y;          sw = w0; sh = h;  }
        else                     { sx = x + w - w0; sy = y;          sw = w0; sh = h;  }

        // Fill helper that skips the fieldset legend gap on the top side.
        auto stampRect = [&](float rx, float ry, float rw, float rh,
                             bromath::Color cc) {
            if (sideIndex == 0 && fieldsetGapActive_) {
                float gx0 = std::max(rx, fieldsetGapX0_);
                float gx1 = std::min(rx + rw, fieldsetGapX1_);
                if (gx1 > gx0) {
                    if (gx0 > rx) renderer_->fillRect(rx, ry, gx0 - rx, rh, cc);
                    if (rx + rw > gx1)
                        renderer_->fillRect(gx1, ry, rx + rw - gx1, rh, cc);
                    return;
                }
            }
            renderer_->fillRect(rx, ry, rw, rh, cc);
        };

        if (st == "solid" || st.empty()) {
            if (sideIndex == 0 && fieldsetGapActive_) {
                stampRect(sx, sy, sw, sh, c);
                return;
            }
            render::PointF pts[4] = {p0, p1, p2, p3};
            renderer_->drawPolygon(std::span<const render::PointF>(pts, 4),
                                   c, cfromColor8({0, 0, 0, 0}), 0.0f);
            return;
        }

        // 3D shaded styles. WebKit/Blink shading: the "dark" variant scales
        // the sRGB-encoded channels by ~2/3 (Color values here are linear, so
        // encode/scale/decode). inset: top/left dark, bottom/right base;
        // outset is the inverse. groove carves (outer half inset-shaded,
        // inner half outset-shaded); ridge embosses (the inverse).
        if (st == "groove" || st == "ridge" || st == "inset" || st == "outset") {
            auto darken = [](bromath::Color cc) {
                cc.r = bromath::csrgbToLinear(bromath::clinearToSrgb(cc.r) * 2.0f / 3.0f);
                cc.g = bromath::csrgbToLinear(bromath::clinearToSrgb(cc.g) * 2.0f / 3.0f);
                cc.b = bromath::csrgbToLinear(bromath::clinearToSrgb(cc.b) * 2.0f / 3.0f);
                return cc;
            };
            bool topLeft = (sideIndex == 0 || sideIndex == 3);
            if (st == "inset" || st == "outset") {
                bool dark = (topLeft == (st == "inset"));
                stampRect(sx, sy, sw, sh, dark ? darken(c) : c);
                return;
            }
            bool outerDark = (topLeft == (st == "groove"));
            bromath::Color oc = outerDark ? darken(c) : c;
            bromath::Color ic = outerDark ? c : darken(c);
            float t = w0 * 0.5f;
            switch (sideIndex) {
            case 0: stampRect(sx, sy, sw, t, oc);
                    stampRect(sx, sy + t, sw, sh - t, ic); break;
            case 2: stampRect(sx, sy + sh - t, sw, t, oc);
                    stampRect(sx, sy, sw, sh - t, ic); break;
            case 3: stampRect(sx, sy, t, sh, oc);
                    stampRect(sx + t, sy, sw - t, sh, ic); break;
            default: stampRect(sx + sw - t, sy, t, sh, oc);
                     stampRect(sx, sy, sw - t, sh, ic); break;
            }
            return;
        }

        if (st == "double") {
            // Two strokes each ~floor(width/3), separated by a gap.
            float t = std::max(1.0f, std::floor(w0 / 3.0f));
            if (horizontal) {
                renderer_->fillRect(sx, sy, sw, t, c);
                renderer_->fillRect(sx, sy + sh - t, sw, t, c);
            } else {
                renderer_->fillRect(sx, sy, t, sh, c);
                renderer_->fillRect(sx + sw - t, sy, t, sh, c);
            }
            return;
        }

        if (st == "dashed" || st == "dotted") {
            // Chromium dashed: stamp = 2*w, period = 3*w. Dotted: round dot,
            // diameter = w, period = 2*w. Center the row of stamps.
            float length = horizontal ? sw : sh;
            if (length <= 0) return;
            bool dotted = (st == "dotted");
            float stamp = dotted ? w0 : 2.0f * w0;
            float period = dotted ? 2.0f * w0 : 3.0f * w0;
            int count = std::max(1, (int)std::round((length + (period - stamp)) / period));
            float totalStamps = count * stamp + (count - 1) * (period - stamp);
            float startOffset = (length - totalStamps) * 0.5f;
            for (int i = 0; i < count; ++i) {
                float off = startOffset + i * period;
                if (dotted) {
                    float cx, cy;
                    if (horizontal) { cx = sx + off + stamp * 0.5f; cy = sy + sh * 0.5f; }
                    else            { cx = sx + sw * 0.5f;          cy = sy + off + stamp * 0.5f; }
                    renderer_->drawCircle(cx, cy, w0 * 0.5f, c, cfromColor8({0, 0, 0, 0}), 0.0f);
                } else {
                    if (horizontal) renderer_->fillRect(sx + off, sy, stamp, sh, c);
                    else            renderer_->fillRect(sx, sy + off, sw, stamp, c);
                }
            }
            return;
        }

        // Unknown style — fall back to solid trapezoid.
        render::PointF pts[4] = {p0, p1, p2, p3};
        renderer_->drawPolygon(std::span<const render::PointF>(pts, 4),
                               c, cfromColor8({0, 0, 0, 0}), 0.0f);
    };

    // Top: outer TL, outer TR, inner TR, inner TL
    drawSide("border-top-color", "border-top-style", 0, T,
             {ox0, oy0}, {ox1, oy0}, {ix1, iy0}, {ix0, iy0});
    // Right: outer TR, outer BR, inner BR, inner TR
    drawSide("border-right-color", "border-right-style", 1, R,
             {ox1, oy0}, {ox1, oy1}, {ix1, iy1}, {ix1, iy0});
    // Bottom: outer BR, outer BL, inner BL, inner BR
    drawSide("border-bottom-color", "border-bottom-style", 2, B,
             {ox1, oy1}, {ox0, oy1}, {ix0, iy1}, {ix1, iy1});
    // Left: outer BL, outer TL, inner TL, inner BL
    drawSide("border-left-color", "border-left-style", 3, L,
             {ox0, oy1}, {ox0, oy0}, {ix0, iy0}, {ix0, iy1});
    fieldsetGapActive_ = false;
}

} // namespace bro::layout
