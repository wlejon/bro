#include "layout/svg_paint.h"
#include "layout/svg_common.h"
#include "layout/svg_text.h"
#include "layout/draw_traversal.h"   // DrawTraversal::tryParseColor
#include "dom/element.h"
#include "render/renderer.h"

#include <include/core/SkMatrix.h>
#include <include/core/SkPath.h>
#include <include/core/SkPathBuilder.h>
#include <include/core/SkRect.h>
#include <include/core/SkString.h>
#include <include/utils/SkParsePath.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <string>
#include <vector>

namespace bro::layout {

using svgcommon::lower;
using svgcommon::attrOf;
using svgcommon::attrFloat;
using svgcommon::parseNumberList;
using svgcommon::isNonRendered;
using svgcommon::parseTransformList;
using svgcommon::ownTransform;
using svgcommon::viewportMatrix;
using svgcommon::findById;
using svgcommon::shapeToPathData;
using svgcommon::pathBounds;

namespace {

using render::GradientPaint;
using render::StrokeStyle;
using render::ColorStop;
using render::PathFillRule;

// ---- computed-style accessors (fall back to the SVG property initial) --------

std::string styleOr(const dom::Element* el, const char* prop, const char* fallback) {
    const auto& cs = el->computedStyle();
    auto it = cs.find(prop);
    if (it != cs.end() && !it->second.empty()) return it->second;
    return fallback;
}

float styleFloat(const dom::Element* el, const char* prop, float fallback) {
    const auto& cs = el->computedStyle();
    auto it = cs.find(prop);
    if (it == cs.end() || it->second.empty()) return fallback;
    char* end = nullptr;
    float f = std::strtof(it->second.c_str(), &end);
    return end == it->second.c_str() ? fallback : f;
}

bool ieq(const std::string& a, const char* b) {
    size_t n = 0;
    for (; b[n]; ++n) {
        if (n >= a.size()) return false;
        if (std::tolower(static_cast<unsigned char>(a[n])) !=
            std::tolower(static_cast<unsigned char>(b[n]))) return false;
    }
    return n == a.size();
}

// Resolve a color keyword/function, honoring `currentColor`. Returns false for
// none/unparsable (caller treats as no paint).
bool resolveColor(const std::string& v, const bromath::Color& cur, bromath::Color& out) {
    if (v.empty() || ieq(v, "none")) return false;
    if (ieq(v, "currentcolor")) { out = cur; return true; }
    return DrawTraversal::tryParseColor(v, out);
}

// A gradient/objectBoundingBox coordinate: "50%" -> 0.5, "0.5"/"1" -> as-is.
float coordFrac(const std::string& v, float def) {
    if (v.empty()) return def;
    char* end = nullptr;
    float f = std::strtof(v.c_str(), &end);
    if (end == v.c_str()) return def;
    if (*end == '%') return f / 100.0f;
    return f;
}

void applyMatrix(render::Renderer* r, const SkMatrix& m) {
    if (m.isIdentity()) return;
    r->concat(m.getScaleX(), m.getSkewY(), m.getSkewX(),
              m.getScaleY(), m.getTranslateX(), m.getTranslateY());
}

uint8_t toByte(float a) {
    return static_cast<uint8_t>(std::lround(std::clamp(a, 0.0f, 1.0f) * 255.0f));
}

StrokeStyle::Cap parseCap(const std::string& s) {
    if (s == "round") return StrokeStyle::Cap::Round;
    if (s == "square") return StrokeStyle::Cap::Square;
    return StrokeStyle::Cap::Butt;
}

StrokeStyle::Join parseJoin(const std::string& s) {
    if (s == "round") return StrokeStyle::Join::Round;
    if (s == "bevel") return StrokeStyle::Join::Bevel;
    return StrokeStyle::Join::Miter;
}

// stroke-dasharray -> even-length interval list (empty for "none"/invalid).
std::vector<float> parseDash(const std::string& s) {
    if (s.empty() || s == "none") return {};
    std::vector<float> d = parseNumberList(s);
    bool allZero = true;
    for (float v : d) { if (v < 0) return {}; if (v > 0) allZero = false; }
    if (d.empty() || allZero) return {};
    if (d.size() % 2 == 1) d.insert(d.end(), d.begin(), d.end()); // SVG: odd list is doubled
    return d;
}

// Collect gradient stops (following xlink:href/href when the gradient has none).
void collectStops(const dom::Element* grad, const dom::Element* svgRoot,
                  const bromath::Color& cur, std::vector<ColorStop>& out, int depth) {
    if (depth > 8) return;
    float lastOff = 0.0f;
    bool any = false;
    for (const dom::Element* c : grad->children()) {
        if (lower(c->tagName()) != "stop") continue;
        any = true;
        float off = std::clamp(coordFrac(attrOf(c, "offset"), 0.0f), 0.0f, 1.0f);
        if (off < lastOff) off = lastOff;   // offsets are non-decreasing
        lastOff = off;
        bromath::Color col{0, 0, 0, 1};
        resolveColor(styleOr(c, "stop-color", "black"), cur, col);
        col.a *= std::clamp(styleFloat(c, "stop-opacity", 1.0f), 0.0f, 1.0f);
        out.push_back({off, col});
    }
    if (!any) {
        std::string href = attrOf(grad, "href");
        if (href.empty()) href = attrOf(grad, "xlink:href");
        if (href.size() > 1 && href[0] == '#') {
            if (const dom::Element* ref = findById(svgRoot, href.substr(1)))
                collectStops(ref, svgRoot, cur, out, depth + 1);
        }
    }
}

struct PaintResult {
    GradientPaint paint;
    std::vector<ColorStop> stops;
};

// Build a gradient paint from a <linearGradient>/<radialGradient> element.
bool buildGradient(const dom::Element* grad, const dom::Element* svgRoot,
                   const SkRect& bbox, const bromath::Color& cur, PaintResult& out) {
    std::string gtag = lower(grad->tagName());
    bool radial = (gtag == "radialgradient");
    if (!radial && gtag != "lineargradient") return false;

    collectStops(grad, svgRoot, cur, out.stops, 0);
    if (out.stops.empty()) return false;

    std::string units = attrOf(grad, "gradientUnits");
    bool obb = units.empty() || units == "objectBoundingBox";
    auto mapX = [&](float f) { return obb ? bbox.fLeft + f * bbox.width() : f; };
    auto mapY = [&](float f) { return obb ? bbox.fTop + f * bbox.height() : f; };

    std::string spread = attrOf(grad, "spreadMethod");
    out.paint.spread = spread == "reflect" ? GradientPaint::Spread::Reflect
                     : spread == "repeat"  ? GradientPaint::Spread::Repeat
                                           : GradientPaint::Spread::Pad;

    if (!radial) {
        out.paint.kind = GradientPaint::Kind::Linear;
        out.paint.p0[0] = mapX(coordFrac(attrOf(grad, "x1"), 0.0f));
        out.paint.p0[1] = mapY(coordFrac(attrOf(grad, "y1"), 0.0f));
        out.paint.p1[0] = mapX(coordFrac(attrOf(grad, "x2"), 1.0f));
        out.paint.p1[1] = mapY(coordFrac(attrOf(grad, "y2"), 0.0f));
    } else {
        out.paint.kind = GradientPaint::Kind::Radial;
        float cx = coordFrac(attrOf(grad, "cx"), 0.5f);
        float cy = coordFrac(attrOf(grad, "cy"), 0.5f);
        float rr = coordFrac(attrOf(grad, "r"), 0.5f);
        out.paint.center[0] = mapX(cx);
        out.paint.center[1] = mapY(cy);
        // In objectBoundingBox the unit square scales to the bbox; approximate
        // the anisotropic radius with the bbox diagonal / sqrt(2).
        out.paint.radius = obb ? rr * std::hypot(bbox.width(), bbox.height()) / 1.41421356f : rr;
        std::string fxs = attrOf(grad, "fx"), fys = attrOf(grad, "fy");
        if (!fxs.empty() || !fys.empty()) {
            out.paint.hasFocal = true;
            out.paint.focal[0] = mapX(coordFrac(fxs, cx));
            out.paint.focal[1] = mapY(coordFrac(fys, cy));
        }
    }

    std::string gt = attrOf(grad, "gradientTransform");
    SkMatrix gm = gt.empty() ? SkMatrix::I() : parseTransformList(gt);
    out.paint.transform[0] = gm.getScaleX(); out.paint.transform[1] = gm.getSkewY();
    out.paint.transform[2] = gm.getSkewX();  out.paint.transform[3] = gm.getScaleY();
    out.paint.transform[4] = gm.getTranslateX(); out.paint.transform[5] = gm.getTranslateY();
    return true;
}

// Resolve a fill/stroke value into a paint server (solid, gradient, or none).
PaintResult resolvePaint(const std::string& value, float opacity,
                         const dom::Element* svgRoot, const SkRect& bbox,
                         const bromath::Color& cur) {
    PaintResult res;
    std::string v = value;
    // trim
    size_t a = v.find_first_not_of(" \t\n\r");
    size_t b = v.find_last_not_of(" \t\n\r");
    if (a == std::string::npos) { res.paint.kind = GradientPaint::Kind::None; return res; }
    v = v.substr(a, b - a + 1);

    if (v.rfind("url(", 0) == 0) {
        auto close = v.find(')');
        std::string ref = v.substr(4, close == std::string::npos ? std::string::npos : close - 4);
        // strip quotes and leading '#'
        ref.erase(std::remove(ref.begin(), ref.end(), '"'), ref.end());
        ref.erase(std::remove(ref.begin(), ref.end(), '\''), ref.end());
        size_t h = ref.find('#');
        if (h != std::string::npos) ref = ref.substr(h + 1);
        // trim
        size_t ra = ref.find_first_not_of(" \t");
        size_t rb = ref.find_last_not_of(" \t");
        if (ra != std::string::npos) ref = ref.substr(ra, rb - ra + 1);
        const dom::Element* grad = ref.empty() ? nullptr : findById(svgRoot, ref);
        if (grad && buildGradient(grad, svgRoot, bbox, cur, res)) {
            for (auto& s : res.stops) s.color.a *= std::clamp(opacity, 0.0f, 1.0f);
            return res;
        }
        res.paint.kind = GradientPaint::Kind::None;   // unresolved paint server
        return res;
    }

    bromath::Color c{0, 0, 0, 1};
    if (resolveColor(v, cur, c)) {
        c.a *= std::clamp(opacity, 0.0f, 1.0f);
        res.paint.kind = GradientPaint::Kind::Solid;
        res.paint.color = c;
    } else {
        res.paint.kind = GradientPaint::Kind::None;
    }
    return res;
}

bool isGroupTag(const std::string& tag) {
    return tag == "g" || tag == "a" || tag == "svg" || tag == "switch";
}

// Bounds of an element's drawn geometry in its OWN local frame (i.e. excluding
// the element's own transform) — the coordinate space objectBoundingBox units
// resolve against. Groups union their children under each child's transform.
SkRect localBounds(const dom::Element* el, const dom::Element* svgRoot, int depth) {
    if (depth > 16) return SkRect::MakeEmpty();
    std::string tag = lower(el->tagName());
    if (isNonRendered(tag)) return SkRect::MakeEmpty();
    if (isGroupTag(tag)) {
        SkRect r = SkRect::MakeEmpty();
        for (const dom::Element* c : el->children()) {
            std::string ctag = lower(c->tagName());
            if (isNonRendered(ctag)) continue;
            SkRect cb = localBounds(c, svgRoot, depth + 1);
            if (cb.isEmpty()) continue;
            ownTransform(c, ctag).mapRect(&cb);
            r.join(cb);
        }
        return r;
    }
    if (tag == "use") return SkRect::MakeEmpty();
    SkRect b = SkRect::MakeEmpty();
    pathBounds(shapeToPathData(el), b);
    return b;
}

// Resolve a computed clip-path into a composed path 'd' in the clipped
// element's local frame. Returns false when there is no (resolvable) clip.
bool resolveClip(const dom::Element* el, const dom::Element* svgRoot,
                 std::string& outD, PathFillRule& outRule) {
    std::string cp = styleOr(el, "clip-path", "none");
    if (cp.empty() || cp == "none" || cp.rfind("url(", 0) != 0) return false;

    auto close = cp.find(')');
    std::string ref = cp.substr(4, close == std::string::npos ? std::string::npos : close - 4);
    ref.erase(std::remove(ref.begin(), ref.end(), '"'), ref.end());
    ref.erase(std::remove(ref.begin(), ref.end(), '\''), ref.end());
    if (size_t h = ref.find('#'); h != std::string::npos) ref = ref.substr(h + 1);
    if (size_t ra = ref.find_first_not_of(" \t"); ra != std::string::npos)
        ref = ref.substr(ra, ref.find_last_not_of(" \t") - ra + 1);
    const dom::Element* clipEl = ref.empty() ? nullptr : findById(svgRoot, ref);
    if (!clipEl || lower(clipEl->tagName()) != "clippath") return false;

    SkMatrix base = parseTransformList(attrOf(clipEl, "transform"));
    std::string units = attrOf(clipEl, "clipPathUnits");
    if (units == "objectBoundingBox") {
        SkRect bb = localBounds(el, svgRoot, 0);
        if (bb.isEmpty()) return false;
        base.preConcat(SkMatrix::MakeAll(bb.width(), 0, bb.fLeft,
                                         0, bb.height(), bb.fTop, 0, 0, 1));
    }

    SkPathBuilder builder;
    int shapeCount = 0;
    bool lastEvenOdd = false;
    for (const dom::Element* c : clipEl->children()) {
        std::string d = shapeToPathData(c);   // ignores <use>/<text>/non-shapes
        if (d.empty()) continue;
        auto p = SkParsePath::FromSVGString(d.c_str());
        if (!p) continue;
        SkMatrix m = base;
        m.preConcat(parseTransformList(attrOf(c, "transform")));
        builder.addPath(p->makeTransform(m));
        lastEvenOdd = styleOr(c, "clip-rule", "nonzero") == "evenodd";
        ++shapeCount;
    }
    SkPath combined = builder.detach();
    if (combined.isEmpty()) return false;
    // A single child may carry clip-rule:evenodd; a union of several is nonzero.
    outRule = (shapeCount == 1 && lastEvenOdd) ? PathFillRule::EvenOdd
                                               : PathFillRule::NonZero;
    outD = std::string(SkParsePath::ToSVGString(combined).c_str());
    return !outD.empty();
}

bool isHidden(const dom::Element* el) {
    const auto& cs = el->computedStyle();
    auto it = cs.find("visibility");
    return it != cs.end() && (it->second == "hidden" || it->second == "collapse");
}

// Paint one shape leaf.
void paintShape(render::Renderer* r, const dom::Element* el,
                const dom::Element* svgRoot, const std::string& d) {
    if (isHidden(el)) return;

    bromath::Color cur{0, 0, 0, 1};
    resolveColor(styleOr(el, "color", "black"), bromath::Color{0, 0, 0, 1}, cur);

    SkRect bbox = SkRect::MakeEmpty();
    pathBounds(d, bbox);

    PaintResult fill = resolvePaint(styleOr(el, "fill", "black"),
                                    styleFloat(el, "fill-opacity", 1.0f), svgRoot, bbox, cur);
    PaintResult stroke = resolvePaint(styleOr(el, "stroke", "none"),
                                      styleFloat(el, "stroke-opacity", 1.0f), svgRoot, bbox, cur);

    StrokeStyle ss;
    ss.width = styleFloat(el, "stroke-width", 1.0f);
    ss.cap = parseCap(styleOr(el, "stroke-linecap", "butt"));
    ss.join = parseJoin(styleOr(el, "stroke-linejoin", "miter"));
    ss.miterLimit = styleFloat(el, "stroke-miterlimit", 4.0f);
    ss.dashOffset = styleFloat(el, "stroke-dashoffset", 0.0f);
    std::vector<float> dash = parseDash(styleOr(el, "stroke-dasharray", "none"));

    PathFillRule rule = styleOr(el, "fill-rule", "nonzero") == "evenodd"
                            ? PathFillRule::EvenOdd : PathFillRule::NonZero;

    r->drawSvgPath(d, rule, fill.paint, fill.stops, stroke.paint, stroke.stops, ss, dash);
}

// Paint a <text> subtree. The shared pen-walk (svg_text) positions every run;
// here we resolve each run's solid fill and emit drawText at its baseline.
void paintText(render::Renderer* r, const dom::Element* textEl,
               const dom::Element* svgRoot) {
    RendererTextMeasurer meas(r);
    std::vector<SvgTextRun> runs = layoutSvgText(textEl, meas);
    SkRect noBox = SkRect::MakeEmpty();
    for (const auto& run : runs) {
        if (run.text.empty() || isHidden(run.styleEl)) continue;
        bromath::Color cur{0, 0, 0, 1};
        resolveColor(styleOr(run.styleEl, "color", "black"),
                     bromath::Color{0, 0, 0, 1}, cur);
        PaintResult fill = resolvePaint(styleOr(run.styleEl, "fill", "black"),
                                        styleFloat(run.styleEl, "fill-opacity", 1.0f),
                                        svgRoot, noBox, cur);
        if (fill.paint.kind != GradientPaint::Kind::Solid) continue;  // none / gradient (gated out)
        render::FontRef fref{run.font.family, run.font.size, run.font.weight, run.font.italic};
        r->drawText(run.text, run.x, run.baseline, fref, fill.paint.color);
    }
}

void paintNode(render::Renderer* r, const dom::Element* el, const dom::Element* svgRoot) {
    std::string tag = lower(el->tagName());
    if (isNonRendered(tag)) return;
    const auto& cs = el->computedStyle();
    if (auto it = cs.find("display"); it != cs.end() && it->second == "none") return;

    float op = styleFloat(el, "opacity", 1.0f);
    bool layer = op < 0.999f;

    r->save();
    applyMatrix(r, ownTransform(el, tag));

    // clip-path (url(#clipPath)) — the clip is composed in this element's local
    // frame, so it must be applied after the element's own transform and stay
    // in effect (bounded by the outer save) while the element is drawn.
    std::string clipD;
    PathFillRule clipRule = PathFillRule::NonZero;
    if (resolveClip(el, svgRoot, clipD, clipRule)) r->clipSvgPath(clipD, clipRule);

    if (layer) r->saveLayerAlpha(toByte(op));

    if (tag == "g" || tag == "a" || tag == "svg" || tag == "switch") {
        for (const dom::Element* c : el->children()) paintNode(r, c, svgRoot);
    } else if (tag == "text") {
        paintText(r, el, svgRoot);   // walks its own tspans; do not recurse
    } else if (tag == "use") {
        std::string href = attrOf(el, "href");
        if (href.empty()) href = attrOf(el, "xlink:href");
        if (href.size() > 1 && href[0] == '#') {
            if (const dom::Element* target = findById(svgRoot, href.substr(1));
                target && target != el) {
                float ux = attrFloat(el, "x"), uy = attrFloat(el, "y");
                r->save();
                if (ux != 0 || uy != 0) r->translate(ux, uy);
                paintNode(r, target, svgRoot);
                r->restore();
            }
        }
    } else {
        std::string d = shapeToPathData(el);
        if (!d.empty()) paintShape(r, el, svgRoot, d);
    }

    if (layer) r->restore();
    r->restore();
}

bool nodeSupported(const dom::Element* el) {
    std::string tag = lower(el->tagName());
    static const char* kUnsupported[] = {
        "textpath", "tref", "foreignobject", "image",
        "symbol", "marker", "mask", "pattern", "filter",
    };
    for (const char* u : kUnsupported) if (tag == u) return false;

    // Native text paints solid/currentColor fills only. A gradient/pattern fill
    // or any stroke on text falls back to SkSVGDOM (which handles those).
    if (tag == "text" || tag == "tspan") {
        std::string fill = styleOr(el, "fill", "black");
        if (fill.rfind("url(", 0) == 0) return false;
        std::string stroke = styleOr(el, "stroke", "none");
        if (!stroke.empty() && stroke != "none") return false;
    }

    const auto& cs = el->computedStyle();
    for (const char* p : {"filter", "mask", "marker-start", "marker-mid", "marker-end"}) {
        auto it = cs.find(p);
        if (it != cs.end() && !it->second.empty() && it->second != "none") return false;
    }
    for (const dom::Element* c : el->children())
        if (!nodeSupported(c)) return false;
    return true;
}

} // namespace

bool svgSubtreeNativelySupported(const dom::Element* svgRoot) {
    return svgRoot && nodeSupported(svgRoot);
}

void paintSvgSubtree(render::Renderer* r, const dom::Element* svgRoot,
                     float x, float y, float w, float h) {
    if (!r || !svgRoot || w <= 0 || h <= 0) return;

    r->save();
    r->translate(x, y);
    // The viewBox maps user units into the content rect; identity (user==px)
    // when absent, which matches the CSS-sized replaced box.
    applyMatrix(r, viewportMatrix(w, h, attrOf(svgRoot, "viewBox"),
                                  attrOf(svgRoot, "preserveAspectRatio")));
    for (const dom::Element* c : svgRoot->children())
        paintNode(r, c, svgRoot);
    r->restore();
}

} // namespace bro::layout
