// The SVG paint traversal: walks an <svg> subtree and emits Renderer
// primitives. Paint servers live in svg_paint_server.cpp, filters in
// svg_filter.cpp, text layout in svg_text.cpp.

#include "layout/svg_paint.h"
#include "layout/svg_paint_internal.h"
#include "layout/svg_common.h"
#include "layout/svg_image.h"
#include "layout/svg_text.h"
#include "layout/css_shadow.h"
#include "layout/draw_traversal_internal.h"   // parseCSSFilter
#include "dom/element.h"
#include "render/image_store.h"
#include "render/renderer.h"
#include "render/shaped_run.h"

#include <include/core/SkMatrix.h>
#include <include/core/SkPath.h>
#include <include/core/SkPathBuilder.h>
#include <include/core/SkRect.h>
#include <include/core/SkString.h>
#include <include/pathops/SkPathOps.h>
#include <include/utils/SkParsePath.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <string>
#include <vector>

namespace bro::layout {

using namespace svgcommon;
using render::PathFillRule;
using render::StrokeStyle;

namespace {

void applyMatrix(render::Renderer* r, const SkMatrix& m) {
    if (m.isIdentity()) return;
    r->concat(m.getScaleX(), m.getSkewY(), m.getSkewX(),
              m.getScaleY(), m.getTranslateX(), m.getTranslateY());
}

uint8_t toByte(float a) {
    return static_cast<uint8_t>(std::lround(std::clamp(a, 0.0f, 1.0f) * 255.0f));
}

std::string toD(const SkPath& p) { return std::string(SkParsePath::ToSVGString(p).c_str()); }

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

StrokeStyle strokeStyleOf(const dom::Element* el) {
    StrokeStyle ss;
    ss.width = styleFloat(el, "stroke-width", 1.0f);
    ss.cap = parseCap(styleOr(el, "stroke-linecap", "butt"));
    ss.join = parseJoin(styleOr(el, "stroke-linejoin", "miter"));
    ss.miterLimit = styleFloat(el, "stroke-miterlimit", 4.0f);
    ss.dashOffset = styleFloat(el, "stroke-dashoffset", 0.0f);
    return ss;
}

bool isHidden(const dom::Element* el) {
    const std::string v = styleOr(el, "visibility", "visible");
    return v == "hidden" || v == "collapse";
}

bool isGroupTag(const std::string& tag) {
    return tag == "g" || tag == "a" || tag == "svg" || tag == "switch";
}

bool displayNone(const dom::Element* el) {
    const auto& cs = el->computedStyle();
    auto it = cs.find("display");
    return it != cs.end() && it->second == "none";
}

// A <use>'s target with how it is instantiated: a symbol or svg becomes a
// viewport of the use's width/height.
SkMatrix useTransform(const dom::Element* use, const dom::Element* target) {
    SkMatrix m = SkMatrix::Translate(attrFloat(use, "x"), attrFloat(use, "y"));
    const std::string ttag = lower(target->tagName());
    if (ttag == "symbol" || ttag == "svg") {
        float w = attrFloat(use, "width", -1.0f), h = attrFloat(use, "height", -1.0f);
        if (w < 0) w = attrFloat(target, "width", 100.0f);
        if (h < 0) h = attrFloat(target, "height", 100.0f);
        const std::string& vb = attrOf(target, "viewBox");
        if (w > 0 && h > 0 && !vb.empty())
            m.preConcat(viewportMatrix(w, h, vb, attrOf(target, "preserveAspectRatio")));
    }
    return m;
}

// ---- text -------------------------------------------------------------------

render::FontRef fontRefOf(const SvgTextRun& run) { return svgFontRef(run.font); }

// The glyph outlines of a run, in the <text> element's user space: along the
// baseline, or each cluster placed and rotated onto the run's textPath.
SkPath runOutline(render::Renderer* r, const SvgTextRun& run) {
    SkPathBuilder b;
    const render::ShapedRun* shaped = r->shapeText(run.text, fontRefOf(run), false, run.direction);
    if (!shaped) return b.detach();
    const auto clusters = shaped->clusterOutlines();
    if (run.pathD.empty()) {
        for (const auto& c : clusters) b.addPath(c.path, run.x + c.x, run.baseline);
        return b.detach();
    }
    SvgPathMeasure pm(run.pathD);
    for (const auto& c : clusters) {
        const float mid = run.x + c.x + c.advance * 0.5f;
        SkPoint pos;
        SkVector tan;
        if (!pm.place(mid, pos, tan)) continue;
        SkMatrix m = SkMatrix::Translate(pos.fX, pos.fY);
        m.preRotate(std::atan2(tan.fY, tan.fX) * 57.29577951f);
        m.preTranslate(-c.advance * 0.5f, run.baseline);
        b.addPath(c.path, m);
    }
    return b.detach();
}

} // namespace

SkRect svgObjectBounds(SvgPaintCtx& ctx, const dom::Element* el) {
    if (ctx.depth > SvgPaintCtx::kMaxDepth) return SkRect::MakeEmpty();
    const std::string tag = lower(el->tagName());
    if (isGroupTag(tag)) {
        SkRect r = SkRect::MakeEmpty();
        ++ctx.depth;
        for (const dom::Element* c : el->children()) {
            const std::string ctag = lower(c->tagName());
            if (isNonRendered(ctag) || displayNone(c)) continue;
            SkRect cb = svgObjectBounds(ctx, c);
            if (cb.isEmpty()) continue;
            ownTransform(c, ctag).mapRect(&cb);
            r.join(cb);
        }
        --ctx.depth;
        return r;
    }
    if (tag == "use") {
        const dom::Element* target = hrefTarget(el, ctx.root);
        if (!target) return SkRect::MakeEmpty();
        ++ctx.depth;
        SkRect tb;
        const std::string ttag = lower(target->tagName());
        if (ttag == "symbol") {
            tb = SkRect::MakeEmpty();
            for (const dom::Element* c : target->children()) {
                SkRect cb = svgObjectBounds(ctx, c);
                if (cb.isEmpty()) continue;
                ownTransform(c, lower(c->tagName())).mapRect(&cb);
                tb.join(cb);
            }
        } else {
            tb = svgObjectBounds(ctx, target);
            ownTransform(target, ttag).mapRect(&tb);
        }
        --ctx.depth;
        useTransform(el, target).mapRect(&tb);
        return tb;
    }
    if (tag == "text") {
        RendererTextMeasurer meas(ctx.r);
        SkRect tb;
        return svgTextElementBounds(el, meas, tb) ? tb : SkRect::MakeEmpty();
    }
    if (tag == "image" || tag == "foreignobject") {
        return SkRect::MakeXYWH(attrFloat(el, "x"), attrFloat(el, "y"),
                                attrFloat(el, "width"), attrFloat(el, "height"));
    }
    SkRect b = SkRect::MakeEmpty();
    pathBounds(shapeToPathData(el), b);
    return b;
}

namespace {

// ---- clip-path ----------------------------------------------------------------

// Add a clipPath child's geometry (a shape, text, or a <use> of either) to
// `out` under `m`.
void addClipGeometry(SvgPaintCtx& ctx, const dom::Element* c, const SkMatrix& m, SkPathBuilder& out,
                     int depth) {
    if (depth > 4 || displayNone(c) || isHidden(c)) return;
    const std::string tag = lower(c->tagName());
    SkMatrix cm = m;
    cm.preConcat(parseTransformList(attrOf(c, "transform")));
    if (tag == "use") {
        const dom::Element* target = hrefTarget(c, ctx.root);
        if (!target) return;
        cm.preConcat(SkMatrix::Translate(attrFloat(c, "x"), attrFloat(c, "y")));
        addClipGeometry(ctx, target, cm, out, depth + 1);
        return;
    }
    if (tag == "text") {
        RendererTextMeasurer meas(ctx.r);
        for (const SvgTextRun& run : layoutSvgText(c, meas))
            out.addPath(runOutline(ctx.r, run), cm);
        return;
    }
    const std::string d = shapeToPathData(c);
    if (d.empty()) return;
    auto p = SkParsePath::FromSVGString(d.c_str());
    if (!p) return;
    if (styleOr(c, "clip-rule", "nonzero") == "evenodd") p->setFillType(SkPathFillType::kEvenOdd);
    // Union of the children: each simplified to its own fill rule first so
    // the combined path can be filled nonzero.
    if (p->getFillType() == SkPathFillType::kEvenOdd) {
        SkPath simple;
        if (Simplify(*p, &simple)) *p = simple;
    }
    out.addPath(p->makeTransform(cm));
}

// Resolve a computed clip-path into a path in the clipped element's local
// frame. False when there is no (resolvable) clip. `outD` is the clip as path
// data: the one child's own when the clip is a single untransformed shape
// (kept as written, arcs included), else `out` serialized.
bool resolveClip(SvgPaintCtx& ctx, const dom::Element* el, SkPath& out, std::string& outD,
                 PathFillRule& outRule) {
    const std::string ref = urlRef(styleOr(el, "clip-path", "none"));
    if (ref.empty()) return false;
    const dom::Element* clipEl = findById(ctx.root, ref);
    if (!clipEl || lower(clipEl->tagName()) != "clippath") return false;

    SkMatrix base = parseTransformList(attrOf(clipEl, "transform"));
    outRule = PathFillRule::NonZero;
    const bool obbUnits = attrOf(clipEl, "clipPathUnits") == "objectBoundingBox";
    if (!obbUnits && base.isIdentity()) {
        const dom::Element* only = nullptr;
        int count = 0;
        for (const dom::Element* c : clipEl->children()) {
            if (displayNone(c) || isHidden(c)) continue;
            only = c;
            ++count;
        }
        if (count == 1 && attrOf(only, "transform").empty()) {
            const std::string tag = lower(only->tagName());
            if (tag != "use" && tag != "text") {
                outD = shapeToPathData(only);
                if (outD.empty() || !SkParsePath::FromSVGString(outD.c_str(), &out)) {
                    out = SkPath();
                    return true;   // a clip with no geometry clips everything away
                }
                if (styleOr(only, "clip-rule", "nonzero") == "evenodd") outRule = PathFillRule::EvenOdd;
                return true;
            }
        }
    }
    if (obbUnits) {
        SkRect bb = svgObjectBounds(ctx, el);
        if (bb.isEmpty()) { out = SkPath(); return true; }   // clips everything away
        base.preConcat(SkMatrix::MakeAll(bb.width(), 0, bb.fLeft, 0, bb.height(), bb.fTop, 0, 0, 1));
    }
    SkPathBuilder builder;
    for (const dom::Element* c : clipEl->children()) addClipGeometry(ctx, c, base, builder, 0);
    out = builder.detach();
    out.setFillType(SkPathFillType::kWinding);
    outD = toD(out);
    return true;
}

// ---- shapes and text --------------------------------------------------------------

void paintShape(SvgPaintCtx& ctx, const dom::Element* el, const std::string& d) {
    if (isHidden(el)) return;
    const std::string fillV = styleOr(el, "fill", "black");
    const std::string strokeV = styleOr(el, "stroke", "none");
    // The geometry is parsed only for what needs it: a paint server's
    // bounding box or a pattern's area. Solid paint records the data as is.
    SkPath path;
    SkRect bbox = SkRect::MakeEmpty();
    if (fillV.find("url(") != std::string::npos || strokeV.find("url(") != std::string::npos) {
        if (!SkParsePath::FromSVGString(d.c_str(), &path)) return;
        bbox = path.computeTightBounds();
    }
    const bromath::Color cur = currentColorOf(el);
    SvgPaint fill = resolveSvgPaint(ctx, fillV, styleFloat(el, "fill-opacity", 1.0f), bbox, cur);
    SvgPaint stroke = resolveSvgPaint(ctx, strokeV, styleFloat(el, "stroke-opacity", 1.0f), bbox, cur);
    const PathFillRule rule = styleOr(el, "fill-rule", "nonzero") == "evenodd" ? PathFillRule::EvenOdd
                                                                                : PathFillRule::NonZero;
    paintSvgGeometry(ctx, path, rule, fill, stroke, strokeStyleOf(el),
                     parseDash(styleOr(el, "stroke-dasharray", "none")), bbox, &d);
}

// Paint a <text> subtree. The shared pen-walk (svg_text) positions every run.
// A run with a plain color fill and no stroke draws as text; anything else (a
// stroke, a gradient or pattern, a textPath) paints its glyph outlines as
// geometry.
void paintText(SvgPaintCtx& ctx, const dom::Element* textEl) {
    RendererTextMeasurer meas(ctx.r);
    std::vector<SvgTextRun> runs = layoutSvgText(textEl, meas);
    SkRect textBox = SkRect::MakeEmpty();
    bool haveBox = false;
    for (const auto& run : runs) {
        if (run.text.empty() || isHidden(run.styleEl)) continue;
        const dom::Element* s = run.styleEl;
        const bromath::Color cur = currentColorOf(s);
        const std::string fillV = styleOr(s, "fill", "black");
        const std::string strokeV = styleOr(s, "stroke", "none");
        const bool plain = run.pathD.empty() && strokeV == "none" && fillV.rfind("url(", 0) != 0;
        if (!haveBox && !plain) {
            svgTextElementBounds(textEl, meas, textBox);
            haveBox = true;
        }
        SvgPaint fill = resolveSvgPaint(ctx, fillV, styleFloat(s, "fill-opacity", 1.0f), textBox, cur);
        if (plain) {
            if (fill.kind == SvgPaint::Kind::Solid)
                ctx.r->drawText(run.text, run.x, run.baseline, fontRefOf(run), fill.paint.color, run.direction);
            continue;
        }
        SvgPaint stroke = resolveSvgPaint(ctx, strokeV, styleFloat(s, "stroke-opacity", 1.0f), textBox, cur);
        const auto direct = [](const SvgPaint& p) { return p.kind != SvgPaint::Kind::Pattern; };
        if (run.pathD.empty() && direct(fill) && direct(stroke)) {
            // Glyphs drawn as text with the paint: gradients and strokes keep
            // the glyph caches.
            static const std::vector<render::ColorStop> kNoStops;
            render::GradientPaint none;
            none.kind = render::GradientPaint::Kind::None;
            const bool f = fill.kind != SvgPaint::Kind::None, st = stroke.kind != SvgPaint::Kind::None;
            ctx.r->drawSvgText(run.text, run.x, run.baseline, fontRefOf(run), run.direction,
                               f ? fill.paint : none, f ? fill.stops : kNoStops,
                               st ? stroke.paint : none, st ? stroke.stops : kNoStops, strokeStyleOf(s),
                               parseDash(styleOr(s, "stroke-dasharray", "none")));
            continue;
        }
        SkPath outline = runOutline(ctx.r, run);
        paintSvgGeometry(ctx, outline, PathFillRule::NonZero, fill, stroke, strokeStyleOf(s),
                         parseDash(styleOr(s, "stroke-dasharray", "none")), textBox);
    }
}

// ---- markers -------------------------------------------------------------------

SkVector unitVec(SkVector v) {
    float len = v.length();
    return len < 1e-4f ? SkVector{0, 0} : SkVector{v.fX / len, v.fY / len};
}

// First of up to three candidate directions with non-degenerate length.
SkVector nonzeroDir(SkVector a, SkVector b, SkVector c) {
    if (a.length() > 1e-4f) return a;
    if (b.length() > 1e-4f) return b;
    return c;
}

// A marker vertex: position plus incoming/outgoing unit tangents (either may be
// zero at path ends). Markers sit at every path command endpoint.
struct MarkerPt {
    SkPoint p{0, 0};
    SkVector in{0, 0};
    SkVector out{0, 0};
};

std::vector<MarkerPt> markerVertices(const std::string& d) {
    std::vector<MarkerPt> out;
    SkPath path;
    if (!SkParsePath::FromSVGString(d.c_str(), &path)) return out;
    SkPath::Iter it(path, false);
    SkPoint pts[4];
    MarkerPt cur;
    bool have = false;
    SkPoint subStart{0, 0};
    auto flush = [&]() { if (have) { out.push_back(cur); have = false; } };
    for (SkPath::Verb v = it.next(pts); v != SkPath::kDone_Verb; v = it.next(pts)) {
        switch (v) {
        case SkPath::kMove_Verb:
            flush();
            cur = MarkerPt{}; cur.p = pts[0]; have = true; subStart = pts[0];
            break;
        case SkPath::kLine_Verb: {
            SkVector d0 = unitVec(pts[1] - pts[0]);
            cur.out = d0; flush();
            cur = MarkerPt{}; cur.p = pts[1]; cur.in = d0; have = true;
            break;
        }
        case SkPath::kQuad_Verb:
        case SkPath::kConic_Verb: {
            SkVector s = unitVec(nonzeroDir(pts[1] - pts[0], pts[2] - pts[0], pts[2] - pts[0]));
            SkVector e = unitVec(nonzeroDir(pts[2] - pts[1], pts[2] - pts[0], pts[2] - pts[0]));
            cur.out = s; flush();
            cur = MarkerPt{}; cur.p = pts[2]; cur.in = e; have = true;
            break;
        }
        case SkPath::kCubic_Verb: {
            SkVector s = unitVec(nonzeroDir(pts[1] - pts[0], pts[2] - pts[0], pts[3] - pts[0]));
            SkVector e = unitVec(nonzeroDir(pts[3] - pts[2], pts[3] - pts[1], pts[3] - pts[0]));
            cur.out = s; flush();
            cur = MarkerPt{}; cur.p = pts[3]; cur.in = e; have = true;
            break;
        }
        case SkPath::kClose_Verb: {
            SkVector d0 = unitVec(subStart - cur.p);
            cur.out = d0; flush();
            cur = MarkerPt{}; cur.p = subStart; cur.in = d0; have = true;
            break;
        }
        default: break;
        }
    }
    flush();
    return out;
}

// orient angle (auto handled by caller): number with optional deg/grad/rad/turn.
float parseAngleDeg(const std::string& s) {
    if (s.empty()) return 0.0f;
    char* end = nullptr;
    float f = std::strtof(s.c_str(), &end);
    if (end == s.c_str()) return 0.0f;
    std::string u(end);
    if (size_t a = u.find_first_not_of(" \t"); a != std::string::npos) u = u.substr(a);
    if (u.rfind("grad", 0) == 0) return f * 0.9f;
    if (u.rfind("rad", 0) == 0)  return f * 57.29577951f;
    if (u.rfind("turn", 0) == 0) return f * 360.0f;
    return f; // deg (default) or unitless
}

// role: 0=start, 1=mid, 2=end. Returns the auto-orient angle in degrees.
float autoAngleDeg(const MarkerPt& m, int role) {
    SkVector dir{0, 0};
    if (role == 0)      dir = m.out.length() > 0 ? m.out : m.in;
    else if (role == 2) dir = m.in.length() > 0 ? m.in : m.out;
    else {  // mid: bisector of incoming/outgoing
        if (m.in.length() == 0)       dir = m.out;
        else if (m.out.length() == 0) dir = m.in;
        else {
            dir = unitVec(SkVector{m.in.fX + m.out.fX, m.in.fY + m.out.fY});
            if (dir.length() == 0) dir = m.out;  // exact reversal → keep travel dir
        }
    }
    if (dir.length() == 0) return 0.0f;
    return std::atan2(dir.fY, dir.fX) * 57.29577951f;
}

std::string trimStr(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\n\r");
    if (a == std::string::npos) return {};
    return s.substr(a, s.find_last_not_of(" \t\n\r") - a + 1);
}

// Resolve marker-start/-mid/-end (or the `marker` shorthand attribute) → element.
const dom::Element* resolveMarkerRef(SvgPaintCtx& ctx, const dom::Element* el, const char* prop) {
    std::string v = styleOr(el, prop, "none");
    if (v == "none" || v.empty()) v = attrOf(el, "marker");  // shorthand fallback
    std::string ref = urlRef(trimStr(v));
    if (ref.empty()) return nullptr;
    const dom::Element* m = findById(ctx.root, ref);
    return (m && lower(m->tagName()) == "marker") ? m : nullptr;
}

// Paint one marker instance at vertex v (role selects start/mid/end semantics).
void paintMarker(SvgPaintCtx& ctx, const dom::Element* mk, const MarkerPt& v, int role, float strokeW) {
    float mw = attrFloat(mk, "markerWidth", 3.0f);
    float mh = attrFloat(mk, "markerHeight", 3.0f);
    if (mw <= 0 || mh <= 0) return;

    std::string orient = trimStr(attrOf(mk, "orient"));
    float angleDeg;
    if (orient == "auto" || orient == "auto-start-reverse") {
        angleDeg = autoAngleDeg(v, role);
        if (role == 0 && orient == "auto-start-reverse") angleDeg += 180.0f;
    } else {
        angleDeg = parseAngleDeg(orient);
    }

    // viewBox → marker viewport (markerWidth × markerHeight); identity when absent
    // (refX/refY then in marker units directly).
    SkMatrix content = viewportMatrix(mw, mh, attrOf(mk, "viewBox"), attrOf(mk, "preserveAspectRatio"));
    SkPoint refVp = content.mapPoint({attrFloat(mk, "refX", 0.0f), attrFloat(mk, "refY", 0.0f)});

    std::string units = attrOf(mk, "markerUnits");
    bool strokeUnits = units.empty() || units == "strokeWidth";

    // Place the marker viewport at the vertex: translate → rotate → (strokeWidth
    // scale) → align refX/refY onto the vertex.
    SkMatrix place = SkMatrix::Translate(v.p.fX, v.p.fY);
    place.preRotate(angleDeg);
    if (strokeUnits) place.preScale(strokeW, strokeW);
    place.preTranslate(-refVp.fX, -refVp.fY);

    render::Renderer* r = ctx.r;
    r->save();
    applyMatrix(r, place);
    // overflow:hidden (UA default) clips content to the marker viewport rect,
    // which lives in this pre-content space.
    std::string ov = styleOr(mk, "overflow", "hidden");
    if (ov != "visible" && ov != "auto") r->setClip(0, 0, mw, mh);
    applyMatrix(r, content);
    ++ctx.depth;
    for (const dom::Element* c : mk->children()) svgPaintNode(ctx, c);
    --ctx.depth;
    r->restore();
}

// Draw markers at every vertex of a path/line/polyline/polygon.
void paintMarkers(SvgPaintCtx& ctx, const dom::Element* el, const std::string& d) {
    if (ctx.depth >= SvgPaintCtx::kMaxDepth) return;
    const dom::Element* mStart = resolveMarkerRef(ctx, el, "marker-start");
    const dom::Element* mMid   = resolveMarkerRef(ctx, el, "marker-mid");
    const dom::Element* mEnd   = resolveMarkerRef(ctx, el, "marker-end");
    if (!mStart && !mMid && !mEnd) return;

    std::vector<MarkerPt> verts = markerVertices(d);
    if (verts.empty()) return;
    float strokeW = styleFloat(el, "stroke-width", 1.0f);

    for (size_t i = 0; i < verts.size(); ++i) {
        int role = (i == 0) ? 0 : (i + 1 == verts.size()) ? 2 : 1;
        const dom::Element* mk = role == 0 ? mStart : role == 2 ? mEnd : mMid;
        if (mk) paintMarker(ctx, mk, verts[i], role, strokeW);
    }
}

// ---- mask ------------------------------------------------------------------------

const dom::Element* resolveMask(SvgPaintCtx& ctx, const dom::Element* el) {
    const std::string ref = urlRef(styleOr(el, "mask", "none"));
    if (ref.empty()) return nullptr;
    const dom::Element* m = findById(ctx.root, ref);
    return (m && lower(m->tagName()) == "mask") ? m : nullptr;
}

// Inside the element's group layer, after its content: the mask's content
// drawn into a mask layer, which keeps the content where the mask is.
void paintMaskLayer(SvgPaintCtx& ctx, const dom::Element* mask, const SkRect& bbox) {
    render::Renderer* r = ctx.r;
    std::string type = attrOf(mask, "mask-type");
    if (type.empty()) type = styleOr(mask, "mask-type", "luminance");
    r->saveLayerMask(type == "alpha" ? render::MaskMode::Alpha : render::MaskMode::Luminance);

    // The mask region: objectBoundingBox fractions (default -10% / 120%) or
    // user-space lengths. Outside it the mask is empty.
    const bool obb = attrOf(mask, "maskUnits") != "userSpaceOnUse";
    auto len = [&](const char* n, const char* def, float bbStart, float bbSize, float vp, bool size) {
        std::string v = attrOf(mask, n);
        if (v.empty()) v = def;
        char* end = nullptr;
        float f = std::strtof(v.c_str(), &end);
        const bool pct = end && *end == '%';
        if (obb) return (size ? 0.0f : bbStart) + (pct ? f / 100.0f : f) * bbSize;
        return pct ? f / 100.0f * vp : f;
    };
    const float mx = len("x", "-10%", bbox.fLeft, bbox.width(), ctx.vpW, false);
    const float my = len("y", "-10%", bbox.fTop, bbox.height(), ctx.vpH, false);
    const float mw = len("width", "120%", 0, bbox.width(), ctx.vpW, true);
    const float mh = len("height", "120%", 0, bbox.height(), ctx.vpH, true);
    const bool empty = !(mw > 0) || !(mh > 0) || (obb && bbox.isEmpty());
    if (!empty && ctx.depth < SvgPaintCtx::kMaxDepth) {
        r->setClip(mx, my, mw, mh);
        if (attrOf(mask, "maskContentUnits") == "objectBoundingBox")
            r->concat(bbox.width(), 0, 0, bbox.height(), bbox.fLeft, bbox.fTop);
        ++ctx.depth;
        for (const dom::Element* c : mask->children()) svgPaintNode(ctx, c);
        --ctx.depth;
    }
    r->restore();
}

// ---- <image> -------------------------------------------------------------------------

void paintImage(SvgPaintCtx& ctx, const dom::Element* el) {
    if (isHidden(el) || !ctx.images || !*ctx.images) return;
    const std::string& href = hrefOf(el);
    if (href.empty()) return;
    std::shared_ptr<const render::DecodedImage> img = (*ctx.images)(href);
    if (!img) return;
    const float x = attrFloat(el, "x"), y = attrFloat(el, "y");
    float w = attrFloat(el, "width", -1.0f), h = attrFloat(el, "height", -1.0f);
    const float iw = static_cast<float>(img->width), ih = static_cast<float>(img->height);
    if (w < 0) w = iw;
    if (h < 0) h = ih;
    if (w <= 0 || h <= 0) return;

    render::Renderer* r = ctx.r;
    r->save();
    r->setClip(x, y, w, h);
    if (img->isSvg) {
        paintSvgImage(r, *img, x, y, w, h);
    } else if (iw > 0 && ih > 0 && !img->rgba.empty()) {
        // preserveAspectRatio fits the picture into the viewport like a viewBox.
        char vb[96];
        std::snprintf(vb, sizeof vb, "0 0 %g %g", iw, ih);
        SkMatrix m = viewportMatrix(w, h, vb, attrOf(el, "preserveAspectRatio"));
        const SkRect dst = m.mapRect(SkRect::MakeWH(iw, ih)).makeOffset(x, y);
        r->drawSharedPixels(render::sharedPixelsOf(img), 0, 0, iw, ih, dst.fLeft, dst.fTop, dst.width(),
                            dst.height());
    }
    r->restore();
}

// ---- the node walk ---------------------------------------------------------------

// The first direct child of a <switch> whose conditions hold. Only
// systemLanguage is evaluated (against English); requiredExtensions fails.
const dom::Element* switchChoice(const dom::Element* sw) {
    for (const dom::Element* c : sw->children()) {
        const std::string tag = lower(c->tagName());
        if (isNonRendered(tag) || displayNone(c)) continue;
        if (!attrOf(c, "requiredExtensions").empty()) continue;
        const std::string& lang = attrOf(c, "systemLanguage");
        if (!lang.empty() && lang.find("en") == std::string::npos) continue;
        return c;
    }
    return nullptr;
}

void paintContent(SvgPaintCtx& ctx, const dom::Element* el, const std::string& tag) {
    if (tag == "switch") {
        if (const dom::Element* c = switchChoice(el)) svgPaintNode(ctx, c);
    } else if (isGroupTag(tag)) {
        // A nested <svg> is a new viewport for percentages.
        const float saveW = ctx.vpW, saveH = ctx.vpH;
        if (tag == "svg") {
            ctx.vpW = attrFloat(el, "width", ctx.vpW);
            ctx.vpH = attrFloat(el, "height", ctx.vpH);
            std::vector<float> vb = parseNumberList(attrOf(el, "viewBox"));
            if (vb.size() == 4 && vb[2] > 0 && vb[3] > 0) { ctx.vpW = vb[2]; ctx.vpH = vb[3]; }
        }
        for (const dom::Element* c : el->children()) svgPaintNode(ctx, c);
        ctx.vpW = saveW;
        ctx.vpH = saveH;
    } else if (tag == "text") {
        paintText(ctx, el);   // walks its own tspans; do not recurse
    } else if (tag == "use") {
        const dom::Element* target = hrefTarget(el, ctx.root);
        if (!target || ctx.depth >= SvgPaintCtx::kMaxDepth) return;
        ctx.r->save();
        applyMatrix(ctx.r, useTransform(el, target));
        ++ctx.depth;
        UseInstanceScope instance(target, el);
        if (lower(target->tagName()) == "symbol") {
            for (const dom::Element* c : target->children()) svgPaintNode(ctx, c);
        } else {
            svgPaintNode(ctx, target);
        }
        --ctx.depth;
        ctx.r->restore();
    } else if (tag == "image") {
        paintImage(ctx, el);
    } else {
        std::string d = shapeToPathData(el);
        if (!d.empty()) {
            paintShape(ctx, el, d);
            if (tag == "path" || tag == "line" || tag == "polyline" || tag == "polygon")
                paintMarkers(ctx, el, d);
        }
    }
}

} // namespace

void svgPaintNode(SvgPaintCtx& ctx, const dom::Element* el) {
    const std::string tag = lower(el->tagName());
    if (isNonRendered(tag) || displayNone(el)) return;
    render::Renderer* r = ctx.r;

    const float op = std::clamp(styleFloat(el, "opacity", 1.0f), 0.0f, 1.0f);
    if (op <= 0.0f) return;
    const dom::Element* mask = ctx.depth < SvgPaintCtx::kMaxDepth ? resolveMask(ctx, el) : nullptr;

    // Filter: an SVG <filter> by url(), else CSS filter functions.
    std::vector<render::SvgFilterPrimitive> prims;
    std::vector<float> fdata;
    SkRect region = SkRect::MakeEmpty();
    std::vector<render::CssFilterParams> cssFilters;
    const std::string filterV = styleOr(el, "filter", "none");
    bool svgFilter = false;
    SkRect bbox = SkRect::MakeEmpty();
    bool haveBox = false;
    auto box = [&]() -> const SkRect& {
        if (!haveBox) { bbox = svgObjectBounds(ctx, el); haveBox = true; }
        return bbox;
    };
    if (filterV != "none") {
        if (filterV.rfind("url(", 0) == 0) {
            svgFilter = buildSvgFilter(ctx, el, box(), prims, fdata, region);
            if (svgFilter && region.isEmpty()) return;   // nothing survives the filter
        } else {
            CssLengthContext lengths;
            lengths.fontSize = styleFloat(el, "font-size", 16.0f);
            lengths.viewportW = ctx.vpW;
            lengths.viewportH = ctx.vpH;
            cssFilters = parseCSSFilter(filterV, currentColorOf(el), lengths);
        }
    }

    r->save();
    applyMatrix(r, ownTransform(el, tag));

    SkPath clip;
    std::string clipD;
    PathFillRule clipRule = PathFillRule::NonZero;
    if (resolveClip(ctx, el, clip, clipD, clipRule)) {
        if (clip.isEmpty()) { r->restore(); return; }   // clipped away entirely
        r->clipSvgPath(clipD, clipRule);
    }

    // The group: opacity, and the layer a mask keeps part of.
    const bool group = op < 0.999f || mask;
    if (group) r->saveLayerAlpha(toByte(op));
    if (svgFilter) {
        r->saveLayerWithSvgFilter(prims, fdata, region.fLeft, region.fTop, region.width(), region.height());
    } else if (!cssFilters.empty()) {
        const SkRect& b = box();
        r->saveLayerWithFilter(cssFilters, b.fLeft, b.fTop, b.width(), b.height());
    }

    paintContent(ctx, el, tag);

    if (svgFilter || !cssFilters.empty()) r->restore();
    if (mask) paintMaskLayer(ctx, mask, box());
    if (group) r->restore();
    r->restore();
}

void paintSvgSubtree(render::Renderer* r, const dom::Element* svgRoot,
                     float x, float y, float w, float h, const SvgImageResolver& images) {
    if (!r || !svgRoot || w <= 0 || h <= 0) return;
    SvgPaintCtx ctx;
    ctx.r = r;
    ctx.root = svgRoot;
    ctx.images = &images;
    ctx.vpW = w;
    ctx.vpH = h;
    std::vector<float> vb = parseNumberList(attrOf(svgRoot, "viewBox"));
    if (vb.size() == 4 && vb[2] > 0 && vb[3] > 0) { ctx.vpW = vb[2]; ctx.vpH = vb[3]; }

    r->save();
    r->translate(x, y);
    // The viewBox maps user units into the content rect; identity (user==px)
    // when absent, which matches the CSS-sized replaced box.
    applyMatrix(r, viewportMatrix(w, h, attrOf(svgRoot, "viewBox"), attrOf(svgRoot, "preserveAspectRatio")));
    for (const dom::Element* c : svgRoot->children()) svgPaintNode(ctx, c);
    r->restore();
}

} // namespace bro::layout
