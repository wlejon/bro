#include "layout/svg_geometry.h"
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

namespace {

std::string lower(std::string_view s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

// gumbo lowercases attribute names at parse time, but JS setAttribute stores
// the name verbatim — try the exact spelling first, then the lowercased one.
const std::string& attrOf(const dom::Element* el, const char* name) {
    const std::string& v = el->getAttribute(name);
    if (!v.empty()) return v;
    return el->getAttribute(lower(name));
}

float attrFloat(const dom::Element* el, const char* name, float fallback = 0.0f) {
    const std::string& v = attrOf(el, name);
    if (v.empty()) return fallback;
    char* end = nullptr;
    float f = std::strtof(v.c_str(), &end);
    return end == v.c_str() ? fallback : f;
}

// Whitespace/comma separated float list (viewBox, polygon points).
std::vector<float> parseNumberList(const std::string& s) {
    std::vector<float> out;
    const char* p = s.c_str();
    while (*p) {
        while (*p && (std::isspace(static_cast<unsigned char>(*p)) || *p == ',')) ++p;
        if (!*p) break;
        char* end = nullptr;
        float f = std::strtof(p, &end);
        if (end == p) break;
        out.push_back(f);
        p = end;
    }
    return out;
}

// Elements that never render and therefore report an all-zero client rect
// (as do all their descendants). Filter primitives are matched by prefix.
bool isNonRendered(const std::string& tag) {
    static const char* kTags[] = {
        "defs", "symbol", "clippath", "mask", "marker", "pattern",
        "lineargradient", "radialgradient", "stop", "filter",
        "metadata", "title", "desc", "style", "script", "view",
    };
    for (const char* t : kTags)
        if (tag == t) return true;
    return tag.rfind("fe", 0) == 0; // feGaussianBlur, feOffset, ...
}

// SVG `transform` attribute list → matrix. Operations compose left-to-right:
// transform="A B" maps a point as A*(B*p).
SkMatrix parseTransformList(const std::string& s) {
    SkMatrix total = SkMatrix::I();
    const char* p = s.c_str();
    while (*p) {
        while (*p && !std::isalpha(static_cast<unsigned char>(*p))) ++p;
        if (!*p) break;
        const char* nameStart = p;
        while (*p && std::isalpha(static_cast<unsigned char>(*p))) ++p;
        std::string name = lower(std::string_view(nameStart, static_cast<size_t>(p - nameStart)));
        while (*p && *p != '(') ++p;
        if (!*p) break;
        ++p; // past '('
        const char* argStart = p;
        while (*p && *p != ')') ++p;
        std::vector<float> a = parseNumberList(std::string(argStart, static_cast<size_t>(p - argStart)));
        if (*p) ++p; // past ')'

        SkMatrix m = SkMatrix::I();
        if (name == "translate" && !a.empty()) {
            m = SkMatrix::Translate(a[0], a.size() > 1 ? a[1] : 0.0f);
        } else if (name == "scale" && !a.empty()) {
            m = SkMatrix::Scale(a[0], a.size() > 1 ? a[1] : a[0]);
        } else if (name == "rotate" && !a.empty()) {
            if (a.size() >= 3) m = SkMatrix::RotateDeg(a[0], {a[1], a[2]});
            else               m = SkMatrix::RotateDeg(a[0]);
        } else if (name == "skewx" && !a.empty()) {
            m.setSkewX(std::tan(a[0] * 3.14159265358979323846f / 180.0f));
        } else if (name == "skewy" && !a.empty()) {
            m.setSkewY(std::tan(a[0] * 3.14159265358979323846f / 180.0f));
        } else if (name == "matrix" && a.size() >= 6) {
            m = SkMatrix::MakeAll(a[0], a[2], a[4],
                                  a[1], a[3], a[5],
                                  0, 0, 1);
        }
        total.preConcat(m);
    }
    return total;
}

// viewBox + preserveAspectRatio → viewport transform (user units → CSS px
// within the viewport). Identity when there is no usable viewBox.
SkMatrix viewportMatrix(float vpW, float vpH,
                        const std::string& viewBox, const std::string& par) {
    if (viewBox.empty() || vpW <= 0 || vpH <= 0) return SkMatrix::I();
    std::vector<float> vb = parseNumberList(viewBox);
    if (vb.size() < 4 || vb[2] <= 0 || vb[3] <= 0) return SkMatrix::I();

    std::string align = "xmidymid";
    bool slice = false;
    {
        std::vector<std::string> toks;
        const char* p = par.c_str();
        while (*p) {
            while (*p && std::isspace(static_cast<unsigned char>(*p))) ++p;
            const char* start = p;
            while (*p && !std::isspace(static_cast<unsigned char>(*p))) ++p;
            if (p != start) toks.push_back(lower(std::string_view(start, static_cast<size_t>(p - start))));
        }
        size_t i = 0;
        if (i < toks.size() && toks[i] == "defer") ++i;
        if (i < toks.size()) { align = toks[i]; ++i; }
        if (i < toks.size() && toks[i] == "slice") slice = true;
    }

    float sx = vpW / vb[2], sy = vpH / vb[3];
    float ax = 0.5f, ay = 0.5f;
    if (align != "none") {
        float s = slice ? std::max(sx, sy) : std::min(sx, sy);
        sx = sy = s;
        if (align.size() >= 8) {
            if (align.compare(0, 4, "xmin") == 0) ax = 0.0f;
            else if (align.compare(0, 4, "xmax") == 0) ax = 1.0f;
            if (align.compare(4, 4, "ymin") == 0) ay = 0.0f;
            else if (align.compare(4, 4, "ymax") == 0) ay = 1.0f;
        }
    }
    float tx = (vpW - vb[2] * sx) * (align == "none" ? 0.0f : ax) - vb[0] * sx;
    float ty = (vpH - vb[3] * sy) * (align == "none" ? 0.0f : ay) - vb[1] * sy;
    return SkMatrix::MakeAll(sx, 0, tx,
                             0, sy, ty,
                             0, 0, 1);
}

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

// The element's own transform contribution when mapping child geometry into
// its parent's user space: the `transform` attribute, plus x/y translation
// and the nested viewport transform for inner <svg> elements.
SkMatrix ownTransform(const dom::Element* el, const std::string& tag) {
    SkMatrix m = parseTransformList(attrOf(el, "transform"));
    if (tag == "svg") {
        m.preConcat(SkMatrix::Translate(attrFloat(el, "x"), attrFloat(el, "y")));
        float w = attrFloat(el, "width", -1.0f);
        float h = attrFloat(el, "height", -1.0f);
        if (w > 0 && h > 0)
            m.preConcat(viewportMatrix(w, h, attrOf(el, "viewBox"),
                                       attrOf(el, "preserveAspectRatio")));
    }
    return m;
}

const dom::Element* findById(const dom::Element* root, const std::string& id) {
    if (attrOf(root, "id") == id) return root;
    for (const dom::Element* c : root->children())
        if (const dom::Element* hit = findById(c, id)) return hit;
    return nullptr;
}

// Fill (object) bounding box of `el` in its own user space — geometry only,
// no stroke, and NOT including el's own `transform` attribute (the caller
// applies that when mapping into the parent's space). Returns false when the
// element has no geometry (empty container, unresolved <use>, <text> without
// metrics, non-rendered element).
bool localFillBounds(const dom::Element* el, const dom::Element* idScope,
                     FillBounds& out, int depth) {
    if (depth > 24) return false;
    std::string tag = lower(el->tagName());
    if (isNonRendered(tag)) return false;

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
        FillBounds tb;
        if (!localFillBounds(target, idScope, tb, depth + 1)) return false;
        // The referenced element is rendered as if cloned under <use>: its
        // own transform applies, then the use's x/y translation.
        SkMatrix m = ownTransform(target, lower(target->tagName()));
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
            if (!localFillBounds(child, idScope, cb, depth + 1)) continue;
            SkRect mapped = ownTransform(child, ctag)
                                .mapRect(SkRect::MakeLTRB(cb.l, cb.t, cb.r, cb.b));
            out.joinRect(mapped);
            any = true;
        }
        return any;
    }
    // <text>/<tspan> would need font metrics; unknown elements have no box.
    return false;
}

} // namespace

bool svgChildBoundingClientRect(const dom::Element* el, dom::AbsoluteRect& out) {
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

    FillBounds fb;
    if (!localFillBounds(el, svgRoot, fb, 0)) return true; // no geometry → zeros

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
