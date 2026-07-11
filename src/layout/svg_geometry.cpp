#include "layout/svg_geometry.h"
#include "layout/svg_common.h"
#include "layout/svg_text.h"
#include "dom/element.h"

#include <include/core/SkMatrix.h>
#include <include/core/SkPath.h>
#include <include/core/SkRect.h>
#include <include/utils/SkParsePath.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

namespace bro::layout {

// Shared SVG parsing helpers (transform/viewBox/attr/shape) live in svg_common.
using svgcommon::lower;
using svgcommon::attrOf;
using svgcommon::attrFloat;
using svgcommon::parseNumberList;
using svgcommon::isNonRendered;
using svgcommon::parseTransformList;
using svgcommon::viewportMatrix;
using svgcommon::ownTransform;
using svgcommon::findById;

namespace {

// Min/max join that keeps degenerate (zero-width/height) geometry such as
// horizontal/vertical lines — SkRect::join discards "empty" rects, which
// would drop them.
struct FillBounds {
    bool valid = false;
    float l = 0, t = 0, r = 0, b = 0;
    void join(float x0, float y0, float x1, float y1) {
        if (!valid) { l = x0; t = y0; r = x1; b = y1; valid = true; return; }
        l = std::min(l, x0); t = std::min(t, y0);
        r = std::max(r, x1); b = std::max(b, y1);
    }
    void joinRect(const SkRect& rc) { join(rc.fLeft, rc.fTop, rc.fRight, rc.fBottom); }
};

// Fill (object) bounding box of `el` in its own user space — geometry only,
// no stroke, and NOT including el's own `transform` attribute (the caller
// applies that when mapping into the parent's space). Returns false when the
// element has no geometry (empty container, unresolved <use>, <text> without
// metrics, non-rendered element).
bool localFillBounds(const dom::Element* el, const dom::Element* idScope,
                     SvgTextMeasurer* meas, FillBounds& out, int depth) {
    if (depth > 24) return false;
    std::string tag = lower(el->tagName());
    if (isNonRendered(tag)) return false;

    if (tag == "text" || tag == "tspan") {
        if (!meas) return false;   // no font backend → no measurable box
        SkRect tb;
        if (!svgTextElementBounds(el, *meas, tb)) return false;
        out.joinRect(tb);
        return true;
    }
    if (tag == "rect" || tag == "image") {
        float w = attrFloat(el, "width"), h = attrFloat(el, "height");
        if (w < 0 || h < 0) return false;
        float x = attrFloat(el, "x"), y = attrFloat(el, "y");
        out.join(x, y, x + w, y + h);
        return true;
    }
    if (tag == "circle") {
        float r = attrFloat(el, "r");
        if (r < 0) return false;
        float cx = attrFloat(el, "cx"), cy = attrFloat(el, "cy");
        out.join(cx - r, cy - r, cx + r, cy + r);
        return true;
    }
    if (tag == "ellipse") {
        float rx = attrFloat(el, "rx", -1.0f), ry = attrFloat(el, "ry", -1.0f);
        if (rx < 0) rx = ry; // rx="auto" tracks ry and vice versa
        if (ry < 0) ry = rx;
        if (rx < 0 || ry < 0) return false;
        float cx = attrFloat(el, "cx"), cy = attrFloat(el, "cy");
        out.join(cx - rx, cy - ry, cx + rx, cy + ry);
        return true;
    }
    if (tag == "line") {
        float x1 = attrFloat(el, "x1"), y1 = attrFloat(el, "y1");
        float x2 = attrFloat(el, "x2"), y2 = attrFloat(el, "y2");
        out.join(std::min(x1, x2), std::min(y1, y2), std::max(x1, x2), std::max(y1, y2));
        return true;
    }
    if (tag == "polyline" || tag == "polygon") {
        std::vector<float> pts = parseNumberList(attrOf(el, "points"));
        if (pts.size() < 2) return false;
        for (size_t i = 0; i + 1 < pts.size(); i += 2)
            out.join(pts[i], pts[i + 1], pts[i], pts[i + 1]);
        return true;
    }
    if (tag == "path") {
        const std::string& d = attrOf(el, "d");
        if (d.empty()) return false;
        SkPath path;
        if (!SkParsePath::FromSVGString(d.c_str(), &path) || path.countPoints() == 0)
            return false;
        out.joinRect(path.computeTightBounds());
        return true;
    }
    if (tag == "use") {
        std::string href = attrOf(el, "href");
        if (href.empty()) href = attrOf(el, "xlink:href");
        if (href.size() < 2 || href[0] != '#') return false;
        const dom::Element* target = findById(idScope, href.substr(1));
        if (!target || target == el) return false;
        std::string ttag = lower(target->tagName());
        if (ttag == "symbol" || ttag == "svg") {
            // A referenced symbol/svg becomes a nested viewport at the
            // use's x/y, sized by the use's width/height and mapped through
            // the symbol's viewBox. (isNonRendered would reject the symbol
            // in the plain path below — it only renders when used.)
            FillBounds cb;
            bool any = false;
            for (const dom::Element* child : target->children()) {
                std::string ctag = lower(child->tagName());
                if (isNonRendered(ctag)) continue;
                FillBounds c1;
                if (!localFillBounds(child, idScope, meas, c1, depth + 1)) continue;
                SkRect mapped = ownTransform(child, ctag)
                                    .mapRect(SkRect::MakeLTRB(c1.l, c1.t, c1.r, c1.b));
                cb.joinRect(mapped);
                any = true;
            }
            if (!any) return false;
            float uw = attrFloat(el, "width", -1.0f);
            float uh = attrFloat(el, "height", -1.0f);
            const std::string& vb = attrOf(target, "viewBox");
            SkMatrix m = SkMatrix::I();
            if (uw > 0 && uh > 0 && !vb.empty())
                m = viewportMatrix(uw, uh, vb, attrOf(target, "preserveAspectRatio"));
            m.postTranslate(attrFloat(el, "x"), attrFloat(el, "y"));
            out.joinRect(m.mapRect(SkRect::MakeLTRB(cb.l, cb.t, cb.r, cb.b)));
            return true;
        }
        FillBounds tb;
        if (!localFillBounds(target, idScope, meas, tb, depth + 1)) return false;
        // The referenced element is rendered as if cloned under <use>: its
        // own transform applies, then the use's x/y translation.
        SkMatrix m = ownTransform(target, ttag);
        m.postTranslate(attrFloat(el, "x"), attrFloat(el, "y"));
        SkRect mapped = m.mapRect(SkRect::MakeLTRB(tb.l, tb.t, tb.r, tb.b));
        out.joinRect(mapped);
        return true;
    }
    if (tag == "g" || tag == "a" || tag == "svg" || tag == "switch") {
        bool any = false;
        for (const dom::Element* child : el->children()) {
            std::string ctag = lower(child->tagName());
            if (isNonRendered(ctag)) continue;
            auto& cs = child->computedStyle();
            auto it = cs.find("display");
            if (it != cs.end() && it->second == "none") continue;
            FillBounds cb;
            if (!localFillBounds(child, idScope, meas, cb, depth + 1)) continue;
            SkRect mapped = ownTransform(child, ctag)
                                .mapRect(SkRect::MakeLTRB(cb.l, cb.t, cb.r, cb.b));
            out.joinRect(mapped);
            any = true;
        }
        return any;
    }
    // Unknown elements have no box.
    return false;
}

} // namespace

bool svgChildBoundingClientRect(const dom::Element* el, dom::AbsoluteRect& out,
                                render::Renderer* renderer) {
    if (!el) return false;

    // Outermost <svg> ancestor: the CSS-laid-out replaced element whose
    // content box is the SVG viewport. No <svg> ancestor (including when el
    // itself is that root) → not an SVG child, use the layout-box path.
    const dom::Element* svgRoot = nullptr;
    for (const dom::Element* p = el->parentElement(); p; p = p->parentElement())
        if (lower(p->tagName()) == "svg") svgRoot = p;
    if (!svgRoot) return false;

    out = {};

    // Non-rendered elements/subtrees and display:none report all-zeros.
    for (const dom::Element* p = el; p != svgRoot; p = p->parentElement()) {
        if (isNonRendered(lower(p->tagName()))) return true;
        auto& cs = p->computedStyle();
        auto it = cs.find("display");
        if (it != cs.end() && it->second == "none") return true;
    }
    {
        auto& cs = svgRoot->computedStyle();
        auto it = cs.find("display");
        if (it != cs.end() && it->second == "none") return true;
    }

    RendererTextMeasurer meas(renderer);
    FillBounds fb;
    if (!localFillBounds(el, svgRoot, &meas, fb, 0)) return true; // no geometry → zeros

    // CTM: viewport(viewBox) ∘ ancestor transforms ∘ el's own transform.
    auto& box = svgRoot->layoutBox();
    SkMatrix ctm = viewportMatrix(box.contentRect.width, box.contentRect.height,
                                  attrOf(svgRoot, "viewBox"),
                                  attrOf(svgRoot, "preserveAspectRatio"));
    std::vector<const dom::Element*> chain;
    for (const dom::Element* p = el; p != svgRoot; p = p->parentElement())
        chain.push_back(p);
    for (auto it = chain.rbegin(); it != chain.rend(); ++it)
        ctm.preConcat(ownTransform(*it, lower((*it)->tagName())));

    SkRect mapped = ctm.mapRect(SkRect::MakeLTRB(fb.l, fb.t, fb.r, fb.b));

    // Offset by the svg's content-box position and project through any CSS
    // transforms on its ancestor chain.
    dom::AbsoluteFrame frame = dom::computeAbsoluteFrame(svgRoot);
    out = dom::projectRectThroughAncestors(svgRoot,
                                           frame.ox + mapped.fLeft,
                                           frame.oy + mapped.fTop,
                                           mapped.width(), mapped.height());
    return true;
}

} // namespace bro::layout
