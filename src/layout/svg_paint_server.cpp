// SVG paint servers: solid colors, linear/radial gradients and patterns, and
// painting a path with any mix of them as fill and stroke.

#include "layout/svg_paint_internal.h"
#include "layout/svg_common.h"
#include "dom/element.h"

#include <include/core/SkMatrix.h>
#include <include/core/SkPaint.h>
#include <include/core/SkPathBuilder.h>
#include <include/core/SkPathUtils.h>
#include <include/core/SkString.h>
#include <include/effects/SkDashPathEffect.h>
#include <include/utils/SkParsePath.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace bro::layout {

using namespace svgcommon;
using render::ColorStop;
using render::GradientPaint;

namespace {

// A gradient/objectBoundingBox coordinate: "50%" -> 0.5, "0.5"/"1" -> as-is.
float coordFrac(const std::string& v, float def) {
    if (v.empty()) return def;
    char* end = nullptr;
    float f = std::strtof(v.c_str(), &end);
    if (end == v.c_str()) return def;
    if (*end == '%') return f / 100.0f;
    return f;
}

// An attribute of a gradient or pattern, inherited through its href chain
// when the element itself does not set it.
const std::string& inheritedAttr(const dom::Element* el, const dom::Element* root, const char* name) {
    static const std::string kEmpty;
    const dom::Element* cur = el;
    for (int depth = 0; cur && depth < 8; ++depth) {
        const std::string& v = attrOf(cur, name);
        if (!v.empty()) return v;
        cur = hrefTarget(cur, root);
    }
    return kEmpty;
}

// The first element in the href chain that has element children: whose
// stops (or pattern content) apply.
const dom::Element* contentSource(const dom::Element* el, const dom::Element* root, const char* childTag) {
    const dom::Element* cur = el;
    for (int depth = 0; cur && depth < 8; ++depth) {
        for (const dom::Element* c : cur->children())
            if (!childTag || lower(c->tagName()) == childTag) return cur;
        cur = hrefTarget(cur, root);
    }
    return el;
}

void collectStops(const dom::Element* grad, const bromath::Color& cur, std::vector<ColorStop>& out) {
    float lastOff = 0.0f;
    for (const dom::Element* c : grad->children()) {
        if (lower(c->tagName()) != "stop") continue;
        float off = std::clamp(coordFrac(attrOf(c, "offset"), 0.0f), 0.0f, 1.0f);
        if (off < lastOff) off = lastOff;   // offsets are non-decreasing
        lastOff = off;
        bromath::Color col{0, 0, 0, 1};
        resolveColor(styleOr(c, "stop-color", "black"), cur, col);
        col.a *= std::clamp(styleFloat(c, "stop-opacity", 1.0f), 0.0f, 1.0f);
        out.push_back({off, col});
    }
}

bool buildGradient(const SvgPaintCtx& ctx, const dom::Element* grad, const SkRect& bbox,
                   const bromath::Color& cur, SvgPaint& out) {
    const dom::Element* root = ctx.root;
    const std::string gtag = lower(grad->tagName());
    const bool radial = (gtag == "radialgradient");
    if (!radial && gtag != "lineargradient") return false;

    collectStops(contentSource(grad, root, "stop"), cur, out.stops);
    if (out.stops.empty()) return false;
    // One stop paints its color everywhere.
    if (out.stops.size() == 1) {
        out.kind = SvgPaint::Kind::Solid;
        out.paint.kind = GradientPaint::Kind::Solid;
        out.paint.color = out.stops[0].color;
        out.stops.clear();
        return true;
    }

    auto attr = [&](const char* n) -> const std::string& { return inheritedAttr(grad, root, n); };
    const std::string& units = attr("gradientUnits");
    const bool obb = units.empty() || units == "objectBoundingBox";
    // A bounding box with no area has no objectBoundingBox space.
    if (obb && (bbox.width() <= 0 || bbox.height() <= 0)) return false;

    const std::string& spread = attr("spreadMethod");
    out.paint.spread = spread == "reflect" ? GradientPaint::Spread::Reflect
                     : spread == "repeat"  ? GradientPaint::Spread::Repeat
                                           : GradientPaint::Spread::Pad;

    // objectBoundingBox: the gradient is defined in the unit square and mapped
    // onto the box by the transform, so a radial one stretches with it.
    SkMatrix m = SkMatrix::I();
    if (obb) m = SkMatrix::MakeAll(bbox.width(), 0, bbox.fLeft, 0, bbox.height(), bbox.fTop, 0, 0, 1);
    const std::string& gt = attr("gradientTransform");
    if (!gt.empty()) m.preConcat(parseTransformList(gt));

    // A coordinate: in objectBoundingBox units a fraction ("50%" = 0.5); in
    // userSpaceOnUse a percentage is of the viewport's width, height or
    // normalized diagonal.
    const float diag = std::sqrt((ctx.vpW * ctx.vpW + ctx.vpH * ctx.vpH) / 2.0f);
    auto len = [&](const char* n, const char* def) {
        std::string v = attr(n);
        if (v.empty()) v = def;
        char* end = nullptr;
        float f = std::strtof(v.c_str(), &end);
        if (end == v.c_str()) return 0.0f;
        if (*end != '%') return f;
        f /= 100.0f;
        if (obb) return f;
        const char axis = n[std::strlen(n) - 1];
        return f * (axis == 'x' ? ctx.vpW : axis == 'y' ? ctx.vpH : diag);
    };

    if (!radial) {
        out.paint.kind = GradientPaint::Kind::Linear;
        out.paint.p0[0] = len("x1", "0%");
        out.paint.p0[1] = len("y1", "0%");
        out.paint.p1[0] = len("x2", "100%");
        out.paint.p1[1] = len("y2", "0%");
    } else {
        out.paint.kind = GradientPaint::Kind::Radial;
        const float cx = len("cx", "50%");
        const float cy = len("cy", "50%");
        out.paint.center[0] = cx;
        out.paint.center[1] = cy;
        out.paint.radius = len("r", "50%");
        if (!attr("fx").empty() || !attr("fy").empty()) {
            out.paint.hasFocal = true;
            out.paint.focal[0] = attr("fx").empty() ? cx : len("fx", "50%");
            out.paint.focal[1] = attr("fy").empty() ? cy : len("fy", "50%");
        }
    }
    out.paint.transform[0] = m.getScaleX(); out.paint.transform[1] = m.getSkewY();
    out.paint.transform[2] = m.getSkewX();  out.paint.transform[3] = m.getScaleY();
    out.paint.transform[4] = m.getTranslateX(); out.paint.transform[5] = m.getTranslateY();
    out.kind = SvgPaint::Kind::Gradient;
    return true;
}

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\n\r");
    if (a == std::string::npos) return {};
    return s.substr(a, s.find_last_not_of(" \t\n\r") - a + 1);
}

void applyMatrix(render::Renderer* r, const SkMatrix& m) {
    if (m.isIdentity()) return;
    r->concat(m.getScaleX(), m.getSkewY(), m.getSkewX(), m.getScaleY(), m.getTranslateX(), m.getTranslateY());
}

std::string toD(const SkPath& p) { return std::string(SkParsePath::ToSVGString(p).c_str()); }

} // namespace

SvgPaint resolveSvgPaint(const SvgPaintCtx& ctx, const std::string& value, float opacity,
                         const SkRect& bbox, const bromath::Color& cur) {
    SvgPaint res;
    const std::string v = trim(value);
    const float op = std::clamp(opacity, 0.0f, 1.0f);
    if (v.empty() || v == "none") return res;

    auto solid = [&](const std::string& s) {
        bromath::Color c{0, 0, 0, 1};
        if (!resolveColor(s, cur, c)) return false;
        c.a *= op;
        res.kind = SvgPaint::Kind::Solid;
        res.paint.kind = GradientPaint::Kind::Solid;
        res.paint.color = c;
        return true;
    };

    std::string rest;
    const std::string ref = urlRef(v, &rest);
    if (!ref.empty() || v.rfind("url(", 0) == 0) {
        const dom::Element* server = ref.empty() ? nullptr : findById(ctx.root, ref);
        if (server) {
            const std::string tag = lower(server->tagName());
            if (tag == "pattern") {
                res.kind = SvgPaint::Kind::Pattern;
                res.pattern = server;
                res.opacity = op;
                return res;
            }
            SvgPaint g;
            if (buildGradient(ctx, server, bbox, cur, g)) {
                if (g.kind == SvgPaint::Kind::Solid) g.paint.color.a *= op;
                for (auto& s : g.stops) s.color.a *= op;
                return g;
            }
            return res;   // a server that paints nothing (no stops, empty box)
        }
        // An unresolvable reference falls back to the color after it, or none.
        const std::string fb = trim(rest);
        if (!fb.empty()) solid(fb);
        return res;
    }
    solid(v);
    return res;
}

void paintPatternArea(SvgPaintCtx& ctx, const dom::Element* pattern, const SkPath& area,
                      const SkRect& bbox, float opacity) {
    if (!pattern || area.isEmpty() || opacity <= 0.0f || ctx.depth >= SvgPaintCtx::kMaxDepth) return;
    const dom::Element* root = ctx.root;
    auto attr = [&](const char* n) -> const std::string& { return inheritedAttr(pattern, root, n); };
    auto num = [&](const char* n, float def) {
        const std::string& s = attr(n);
        if (s.empty()) return def;
        char* end = nullptr;
        float f = std::strtof(s.c_str(), &end);
        if (end == s.c_str()) return def;
        return *end == '%' ? f / 100.0f : f;
    };

    const std::string& units = attr("patternUnits");
    const bool obb = units.empty() || units == "objectBoundingBox";
    float px = num("x", 0), py = num("y", 0), pw = num("width", 0), ph = num("height", 0);
    if (obb) {
        px = bbox.fLeft + px * bbox.width();
        py = bbox.fTop + py * bbox.height();
        pw *= bbox.width();
        ph *= bbox.height();
    }
    if (!(pw > 0) || !(ph > 0)) return;

    const dom::Element* content = contentSource(pattern, root, nullptr);
    const std::string& vb = attr("viewBox");
    SkMatrix contentM = SkMatrix::I();
    if (!vb.empty()) {
        contentM = viewportMatrix(pw, ph, vb, attr("preserveAspectRatio"));
    } else if (attr("patternContentUnits") == "objectBoundingBox") {
        contentM = SkMatrix::MakeAll(bbox.width(), 0, 0, 0, bbox.height(), 0, 0, 0, 1);
    }
    const std::string& ptStr = attr("patternTransform");
    SkMatrix pt = ptStr.empty() ? SkMatrix::I() : parseTransformList(ptStr);

    // The tiles that can reach the area: its bounds in pattern space.
    SkMatrix inv;
    if (!pt.invert(&inv)) return;
    const SkRect cover = inv.mapRect(area.getBounds());
    const int i0 = static_cast<int>(std::floor((cover.fLeft - px) / pw));
    const int i1 = static_cast<int>(std::ceil((cover.fRight - px) / pw));
    const int j0 = static_cast<int>(std::floor((cover.fTop - py) / ph));
    const int j1 = static_cast<int>(std::ceil((cover.fBottom - py) / ph));
    // A tile far smaller than the area (a 0.01-unit pattern on a page-sized
    // rect) would be millions of copies; past this many, the pattern is drawn
    // over as much of the area as the budget covers.
    constexpr long kMaxTiles = 4096;
    const long tiles = static_cast<long>(i1 - i0) * static_cast<long>(j1 - j0);
    if (tiles <= 0) return;

    render::Renderer* r = ctx.r;
    r->save();
    SkPath clip = area;
    r->clipSvgPath(toD(clip), clip.getFillType() == SkPathFillType::kEvenOdd ? render::PathFillRule::EvenOdd
                                                                               : render::PathFillRule::NonZero);
    const bool layer = opacity < 0.999f;
    if (layer) r->saveLayerAlpha(static_cast<uint8_t>(std::lround(opacity * 255.0f)));
    applyMatrix(r, pt);
    const std::string overflow = styleOr(pattern, "overflow", "hidden");
    const bool clipTile = overflow != "visible" && overflow != "auto";
    ++ctx.depth;
    long drawn = 0;
    for (int j = j0; j < j1 && drawn < kMaxTiles; ++j) {
        for (int i = i0; i < i1 && drawn < kMaxTiles; ++i, ++drawn) {
            r->save();
            r->translate(px + static_cast<float>(i) * pw, py + static_cast<float>(j) * ph);
            if (clipTile) r->setClip(0, 0, pw, ph);
            applyMatrix(r, contentM);
            for (const dom::Element* c : content->children()) svgPaintNode(ctx, c);
            r->restore();
        }
    }
    --ctx.depth;
    if (layer) r->restore();
    r->restore();
}

void paintSvgGeometry(SvgPaintCtx& ctx, const SkPath& path, render::PathFillRule rule,
                      const SvgPaint& fill, const SvgPaint& stroke,
                      const render::StrokeStyle& ss, const std::vector<float>& dash,
                      const SkRect& bbox, const std::string* d) {
    if (!d && path.isEmpty()) return;
    auto direct = [](const SvgPaint& p) {
        return p.kind == SvgPaint::Kind::Solid || p.kind == SvgPaint::Kind::Gradient;
    };
    const bool hasStroke = stroke.kind != SvgPaint::Kind::None && ss.width > 0;

    if (fill.kind == SvgPaint::Kind::Pattern) {
        SkPath area = path;
        area.setFillType(rule == render::PathFillRule::EvenOdd ? SkPathFillType::kEvenOdd
                                                                : SkPathFillType::kWinding);
        paintPatternArea(ctx, fill.pattern, area, bbox, fill.opacity);
    }
    const bool fillD = direct(fill);
    const bool strokeD = hasStroke && direct(stroke);
    if (fillD || strokeD) {
        GradientPaint none;
        none.kind = GradientPaint::Kind::None;
        static const std::vector<ColorStop> kNoStops;
        ctx.r->drawSvgPath(d ? *d : toD(path), rule,
                           fillD ? fill.paint : none, fillD ? fill.stops : kNoStops,
                           strokeD ? stroke.paint : none, strokeD ? stroke.stops : kNoStops,
                           ss, dash);
    }
    if (hasStroke && stroke.kind == SvgPaint::Kind::Pattern) {
        // The stroke's outline as an area.
        SkPaint p;
        p.setStyle(SkPaint::kStroke_Style);
        p.setStrokeWidth(ss.width);
        p.setStrokeMiter(ss.miterLimit);
        p.setStrokeCap(ss.cap == render::StrokeStyle::Cap::Round    ? SkPaint::kRound_Cap
                       : ss.cap == render::StrokeStyle::Cap::Square ? SkPaint::kSquare_Cap
                                                                    : SkPaint::kButt_Cap);
        p.setStrokeJoin(ss.join == render::StrokeStyle::Join::Round   ? SkPaint::kRound_Join
                        : ss.join == render::StrokeStyle::Join::Bevel ? SkPaint::kBevel_Join
                                                                      : SkPaint::kMiter_Join);
        if (dash.size() >= 2)
            p.setPathEffect(SkDashPathEffect::Make(SkSpan(dash.data(), dash.size()), ss.dashOffset));
        SkPath outline = skpathutils::FillPathWithPaint(path, p);
        outline.setFillType(SkPathFillType::kWinding);
        paintPatternArea(ctx, stroke.pattern, outline, bbox, stroke.opacity);
    }
}

} // namespace bro::layout
