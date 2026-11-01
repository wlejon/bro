#include "layout/svg_common.h"
#include "dom/element.h"
#include "layout/draw_traversal.h"   // DrawTraversal::tryParseColor

#include <include/core/SkPath.h>
#include <include/utils/SkParsePath.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace bro::layout::svgcommon {

std::string lower(std::string_view s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

const std::string& attrOf(const dom::Element* el, const char* name) {
    const std::string& v = el->getAttribute(name);
    if (!v.empty()) return v;
    return el->getAttribute(lower(name));
}

float attrFloat(const dom::Element* el, const char* name, float fallback) {
    const std::string& v = attrOf(el, name);
    if (v.empty()) return fallback;
    char* end = nullptr;
    float f = std::strtof(v.c_str(), &end);
    return end == v.c_str() ? fallback : f;
}

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

std::string shapeToPathData(const dom::Element* el) {
    std::string tag = lower(el->tagName());
    char buf[512];

    if (tag == "path") {
        return attrOf(el, "d");
    }
    if (tag == "rect") {
        float w = attrFloat(el, "width", -1.0f), h = attrFloat(el, "height", -1.0f);
        if (w <= 0 || h <= 0) return "";
        float x = attrFloat(el, "x"), y = attrFloat(el, "y");
        float rx = attrFloat(el, "rx", -1.0f), ry = attrFloat(el, "ry", -1.0f);
        if (rx < 0 && ry < 0) { rx = ry = 0; }
        else if (rx < 0) rx = ry;
        else if (ry < 0) ry = rx;
        rx = std::min(rx, w / 2.0f);
        ry = std::min(ry, h / 2.0f);
        if (rx <= 0 || ry <= 0) {
            std::snprintf(buf, sizeof(buf), "M%g %gh%gv%gh%gZ", x, y, w, h, -w);
            return buf;
        }
        std::snprintf(buf, sizeof(buf),
            "M%g %gh%ga%g %g 0 0 1 %g %gv%ga%g %g 0 0 1 %g %gh%ga%g %g 0 0 1 %g %gv%ga%g %g 0 0 1 %g %gZ",
            x + rx, y, w - 2 * rx,           // top edge
            rx, ry, rx, ry,                  // top-right arc
            h - 2 * ry,                      // right edge
            rx, ry, -rx, ry,                 // bottom-right arc
            -(w - 2 * rx),                   // bottom edge
            rx, ry, -rx, -ry,                // bottom-left arc
            -(h - 2 * ry),                   // left edge
            rx, ry, rx, -ry);                // top-left arc
        return buf;
    }
    if (tag == "circle") {
        float r = attrFloat(el, "r", -1.0f);
        if (r <= 0) return "";
        float cx = attrFloat(el, "cx"), cy = attrFloat(el, "cy");
        // SVG-2 canonical circle path: start at (cx+r, cy) and sweep clockwise
        // through four quarter arcs, matching Blink/Skia addOval so that
        // stroke-dasharray phases align with Chromium.
        std::snprintf(buf, sizeof(buf),
            "M%g %gA%g %g 0 0 1 %g %gA%g %g 0 0 1 %g %gA%g %g 0 0 1 %g %gA%g %g 0 0 1 %g %gZ",
            cx + r, cy,
            r, r, cx, cy + r,
            r, r, cx - r, cy,
            r, r, cx, cy - r,
            r, r, cx + r, cy);
        return buf;
    }
    if (tag == "ellipse") {
        float rx = attrFloat(el, "rx", -1.0f), ry = attrFloat(el, "ry", -1.0f);
        if (rx < 0) rx = ry;
        if (ry < 0) ry = rx;
        if (rx <= 0 || ry <= 0) return "";
        float cx = attrFloat(el, "cx"), cy = attrFloat(el, "cy");
        std::snprintf(buf, sizeof(buf),
            "M%g %gA%g %g 0 0 1 %g %gA%g %g 0 0 1 %g %gA%g %g 0 0 1 %g %gA%g %g 0 0 1 %g %gZ",
            cx + rx, cy,
            rx, ry, cx, cy + ry,
            rx, ry, cx - rx, cy,
            rx, ry, cx, cy - ry,
            rx, ry, cx + rx, cy);
        return buf;
    }
    if (tag == "line") {
        float x1 = attrFloat(el, "x1"), y1 = attrFloat(el, "y1");
        float x2 = attrFloat(el, "x2"), y2 = attrFloat(el, "y2");
        std::snprintf(buf, sizeof(buf), "M%g %gL%g %g", x1, y1, x2, y2);
        return buf;
    }
    if (tag == "polygon" || tag == "polyline") {
        std::vector<float> pts = parseNumberList(attrOf(el, "points"));
        if (pts.size() < 4) return "";
        std::string d;
        char pb[64];
        for (size_t i = 0; i + 1 < pts.size(); i += 2) {
            std::snprintf(pb, sizeof(pb), "%c%g %g", i == 0 ? 'M' : 'L', pts[i], pts[i + 1]);
            d += pb;
        }
        if (tag == "polygon") d += 'Z';
        return d;
    }
    return "";
}

bool pathBounds(const std::string& d, SkRect& out) {
    if (d.empty()) return false;
    auto p = SkParsePath::FromSVGString(d.c_str());
    if (!p || p->countPoints() == 0) return false;
    out = p->computeTightBounds();
    return true;
}

namespace {

struct UseInstance {
    const dom::Element* target;
    const dom::Element* use;
};
thread_local std::vector<UseInstance> tUseInstances;

const std::string* rawStyle(const dom::Element* el, const char* prop) {
    const auto& cs = el->computedStyle();
    auto it = cs.find(prop);
    return it != cs.end() && !it->second.empty() ? &it->second : nullptr;
}

bool sameValue(const std::string* a, const std::string* b) {
    return a == b || (a && b && *a == *b);
}

bool isInheritedSvgProperty(std::string_view p) {
    static constexpr std::string_view kInherited[] = {
        "fill", "fill-opacity", "fill-rule", "stroke", "stroke-opacity", "stroke-width",
        "stroke-linecap", "stroke-linejoin", "stroke-miterlimit", "stroke-dasharray",
        "stroke-dashoffset", "clip-rule", "color", "visibility", "font-family", "font-size",
        "font-weight", "font-style", "text-anchor", "dominant-baseline", "direction",
        "letter-spacing", "word-spacing", "marker-start", "marker-mid", "marker-end",
        "paint-order", "color-interpolation-filters",
    };
    for (std::string_view k : kInherited)
        if (k == p) return true;
    return false;
}

// `el`'s value of `prop` with the innermost `level` use instances applied.
const std::string* instanceStyle(const dom::Element* el, const char* prop, size_t level) {
    if (level == 0 || !isInheritedSvgProperty(prop)) return rawStyle(el, prop);
    const UseInstance& inst = tUseInstances[level - 1];
    const std::string* v = rawStyle(el, prop);
    for (const dom::Element* cur = el; cur;) {
        const dom::Element* p = cur->parentElement();
        const bool differs = !p || !sameValue(rawStyle(cur, prop), rawStyle(p, prop));
        if (cur == inst.target)
            return differs ? v : instanceStyle(inst.use, prop, level - 1);
        if (differs) return v;   // set on the way up: the element's own value
        cur = p;
    }
    return instanceStyle(el, prop, level - 1);   // not inside this instance
}

const std::string* lookupStyle(const dom::Element* el, const char* prop) {
    return tUseInstances.empty() ? rawStyle(el, prop) : instanceStyle(el, prop, tUseInstances.size());
}

} // namespace

UseInstanceScope::UseInstanceScope(const dom::Element* target, const dom::Element* use) {
    tUseInstances.push_back({target, use});
}

UseInstanceScope::~UseInstanceScope() { tUseInstances.pop_back(); }

std::string styleOr(const dom::Element* el, const char* prop, const char* fallback) {
    const std::string* v = lookupStyle(el, prop);
    return v ? *v : std::string(fallback);
}

float styleFloat(const dom::Element* el, const char* prop, float fallback) {
    const std::string* v = lookupStyle(el, prop);
    if (!v) return fallback;
    char* end = nullptr;
    float f = std::strtof(v->c_str(), &end);
    if (end == v->c_str()) return fallback;
    if (*end == '%') f /= 100.0f;   // opacity-like properties: 50% == 0.5
    return f;
}

namespace {
bool ieq(const std::string& a, const char* b) {
    size_t n = 0;
    for (; b[n]; ++n) {
        if (n >= a.size()) return false;
        if (std::tolower(static_cast<unsigned char>(a[n])) !=
            std::tolower(static_cast<unsigned char>(b[n]))) return false;
    }
    return n == a.size();
}
} // namespace

bool resolveColor(const std::string& v, const bromath::Color& current, bromath::Color& out) {
    if (v.empty() || ieq(v, "none") || ieq(v, "transparent")) {
        if (ieq(v, "transparent")) { out = bromath::Color{0, 0, 0, 0}; return true; }
        return false;
    }
    if (ieq(v, "currentcolor")) { out = current; return true; }
    return DrawTraversal::tryParseColor(v, out);
}

bromath::Color currentColorOf(const dom::Element* el) {
    bromath::Color c{0, 0, 0, 1};
    resolveColor(styleOr(el, "color", "black"), bromath::Color{0, 0, 0, 1}, c);
    return c;
}

std::string urlRef(const std::string& v, std::string* rest) {
    size_t a = v.find_first_not_of(" \t\n\r");
    if (a == std::string::npos || v.compare(a, 4, "url(") != 0) return {};
    size_t close = v.find(')', a);
    std::string ref = v.substr(a + 4, close == std::string::npos ? std::string::npos : close - a - 4);
    if (rest) *rest = close == std::string::npos ? std::string() : v.substr(close + 1);
    ref.erase(std::remove(ref.begin(), ref.end(), '"'), ref.end());
    ref.erase(std::remove(ref.begin(), ref.end(), '\''), ref.end());
    if (size_t h = ref.find('#'); h != std::string::npos) ref = ref.substr(h + 1);
    size_t s = ref.find_first_not_of(" \t");
    if (s == std::string::npos) return {};
    return ref.substr(s, ref.find_last_not_of(" \t") - s + 1);
}

const std::string& hrefOf(const dom::Element* el) {
    const std::string& h = attrOf(el, "href");
    if (!h.empty()) return h;
    return attrOf(el, "xlink:href");
}

const dom::Element* hrefTarget(const dom::Element* el, const dom::Element* root) {
    const std::string& h = hrefOf(el);
    if (h.size() < 2 || h[0] != '#') return nullptr;
    const dom::Element* t = findById(root, h.substr(1));
    return t == el ? nullptr : t;
}

} // namespace bro::layout::svgcommon
