#include "engine/hit_testing.h"
#include "dom/element.h"
#include "dom/node.h"

#include <cmath>
#include <cstdlib>
#include <string>

namespace bro::engine {

// Minimal 2D affine matrix for hit test point mapping.
struct HitMatrix {
    float a = 1, b = 0, c = 0, d = 1, e = 0, f = 0;
    HitMatrix operator*(const HitMatrix& r) const {
        return { a*r.a + c*r.b, b*r.a + d*r.b,
                 a*r.c + c*r.d, b*r.c + d*r.d,
                 a*r.e + c*r.f + e, b*r.e + d*r.f + f };
    }
    bool isIdentity() const { return a==1 && b==0 && c==0 && d==1 && e==0 && f==0; }
    bool invert(HitMatrix& out) const {
        float det = a*d - b*c;
        if (std::abs(det) < 1e-9f) return false;
        float inv = 1.0f / det;
        out = { d*inv, -b*inv, -c*inv, a*inv, (c*f-d*e)*inv, (b*e-a*f)*inv };
        return true;
    }
};

static float htParseFloat(const std::string& s, size_t& pos) {
    while (pos < s.size() && (s[pos]==' '||s[pos]==','||s[pos]=='\t')) ++pos;
    char* end = nullptr;
    float v = std::strtof(s.c_str() + pos, &end);
    pos = static_cast<size_t>(end - s.c_str());
    while (pos < s.size() && std::isalpha(static_cast<unsigned char>(s[pos]))) ++pos;
    if (pos < s.size() && s[pos] == '%') ++pos;
    return v;
}

static float htParseAngle(const std::string& s, size_t& pos) {
    while (pos < s.size() && (s[pos]==' '||s[pos]==',')) ++pos;
    char* end = nullptr;
    float v = std::strtof(s.c_str() + pos, &end);
    size_t u = static_cast<size_t>(end - s.c_str());
    std::string unit;
    while (u < s.size() && std::isalpha(static_cast<unsigned char>(s[u]))) unit += s[u++];
    pos = u;
    if (unit == "rad") return v;
    if (unit == "turn") return v * 2.0f * 3.14159265f;
    if (unit == "grad") return v * 3.14159265f / 200.0f;
    return v * 3.14159265f / 180.0f;
}

static HitMatrix htParseTransform(const std::string& val) {
    HitMatrix result;
    size_t pos = 0;
    while (pos < val.size()) {
        while (pos < val.size() && (val[pos]==' '||val[pos]=='\t')) ++pos;
        if (pos >= val.size()) break;
        size_t ns = pos;
        while (pos < val.size() && val[pos]!='(' && val[pos]!=' ') ++pos;
        std::string func = val.substr(ns, pos - ns);
        if (pos >= val.size() || val[pos] != '(') break;
        ++pos;
        HitMatrix m;
        if (func == "translate") {
            m.e = htParseFloat(val, pos);
            size_t s = pos; while (s<val.size()&&(val[s]==' '||val[s]==',')) ++s;
            if (s<val.size()&&val[s]!=')') m.f = htParseFloat(val, pos);
        } else if (func == "translateX") { m.e = htParseFloat(val, pos);
        } else if (func == "translateY") { m.f = htParseFloat(val, pos);
        } else if (func == "scale") {
            m.a = htParseFloat(val, pos); m.d = m.a;
            size_t s = pos; while (s<val.size()&&(val[s]==' '||val[s]==',')) ++s;
            if (s<val.size()&&val[s]!=')') m.d = htParseFloat(val, pos);
        } else if (func == "scaleX") { m.a = htParseFloat(val, pos);
        } else if (func == "scaleY") { m.d = htParseFloat(val, pos);
        } else if (func == "rotate") {
            float r = htParseAngle(val, pos);
            m.a = std::cos(r); m.c = -std::sin(r);
            m.b = std::sin(r); m.d =  std::cos(r);
        } else if (func == "skewX") { m.c = std::tan(htParseAngle(val, pos));
        } else if (func == "skewY") { m.b = std::tan(htParseAngle(val, pos));
        } else if (func == "skew") {
            m.c = std::tan(htParseAngle(val, pos));
            size_t s = pos; while (s<val.size()&&(val[s]==' '||val[s]==',')) ++s;
            if (s<val.size()&&val[s]!=')') m.b = std::tan(htParseAngle(val, pos));
        } else if (func == "matrix") {
            m.a = htParseFloat(val, pos); m.b = htParseFloat(val, pos);
            m.c = htParseFloat(val, pos); m.d = htParseFloat(val, pos);
            m.e = htParseFloat(val, pos); m.f = htParseFloat(val, pos);
        }
        while (pos < val.size() && val[pos] != ')') ++pos;
        if (pos < val.size()) ++pos;
        result = result * m;
    }
    return result;
}

dom::Element* hitTestElement(dom::Element* elem, float x, float y,
                             float offsetX, float offsetY) {
    if (!elem) return nullptr;

    auto& style = elem->computedStyle();

    // Skip invisible / non-interactive elements
    {
        auto it = style.find("display");
        if (it != style.end() && it->second == "none") return nullptr;
    }
    {
        auto it = style.find("pointer-events");
        if (it != style.end() && it->second == "none") return nullptr;
    }
    {
        auto it = style.find("visibility");
        if (it != style.end() && it->second == "hidden") return nullptr;
    }

    auto& box = elem->layoutBox();

    // Absolute content position
    float absX = box.contentRect.x + offsetX;
    float absY = box.contentRect.y + offsetY;

    // Border box for transform origin
    float bx = absX - box.padding.left - box.border.left;
    float by = absY - box.padding.top  - box.border.top;
    float bw = box.fullWidth();
    float bh = box.fullHeight();

    // Apply CSS transform: map the test point through the inverse transform
    float testX = x, testY = y;
    {
        auto it = style.find("transform");
        if (it != style.end() && !it->second.empty() && it->second != "none") {
            HitMatrix mat = htParseTransform(it->second);
            if (!mat.isIdentity()) {
                // Compute transform-origin (default 50% 50%)
                float ox = bw * 0.5f, oy = bh * 0.5f;
                auto oIt = style.find("transform-origin");
                if (oIt != style.end() && !oIt->second.empty()) {
                    size_t p = 0;
                    ox = htParseFloat(oIt->second, p);
                    if (p < oIt->second.size()) oy = htParseFloat(oIt->second, p);
                    // Handle percentage
                    // (simplified — works for px values and the default)
                }
                // Build full transform: T(origin) * M * T(-origin)
                HitMatrix toOrigin = {1,0,0,1, bx+ox, by+oy};
                HitMatrix fromOrigin = {1,0,0,1, -(bx+ox), -(by+oy)};
                HitMatrix full = toOrigin * mat * fromOrigin;
                HitMatrix inv;
                if (full.invert(inv)) {
                    float px = inv.a * x + inv.c * y + inv.e;
                    float py = inv.b * x + inv.d * y + inv.f;
                    testX = px;
                    testY = py;
                }
            }
        }
    }

    // Skip bounds clipping for the document element (<html>) — it should accept
    // hits across the entire viewport, matching browser behavior.
    bool isDocumentElement = (elem->parentNode() == nullptr ||
                              elem->parentNode()->nodeType() == dom::NodeType::Document);

    // Determine if this element clips child hit testing to its border box.
    // Elements with overflow:visible (the default) allow absolutely positioned
    // children to overflow, so we must test children even when the click is
    // outside this element's bounds.
    bool clipsChildren = false;
    if (!isDocumentElement) {
        auto ovIt = style.find("overflow");
        if (ovIt != style.end() && ovIt->second != "visible")
            clipsChildren = true;
        if (!clipsChildren) {
            auto ovxIt = style.find("overflow-x");
            auto ovyIt = style.find("overflow-y");
            if ((ovxIt != style.end() && ovxIt->second != "visible") ||
                (ovyIt != style.end() && ovyIt->second != "visible"))
                clipsChildren = true;
        }
    }

    // Border box bounds (bx, by, bw, bh computed above for transform-origin)
    bool insideBounds = isDocumentElement ||
        (testX >= bx && testX < bx + bw && testY >= by && testY < by + bh);

    // If this element clips and point is outside, reject entirely
    if (clipsChildren && !insideBounds)
        return nullptr;

    // Adjust child offset for element-level scroll
    float childOffsetX = absX;
    float childOffsetY = absY - elem->scrollTopValue();

    // Composed children (shadow DOM + slot replacement), checked in reverse
    // order for correct z-ordering (last-painted = topmost)
    auto composedChildren = elem->composedChildNodes();
    for (int i = static_cast<int>(composedChildren.size()) - 1; i >= 0; --i) {
        auto* hit = hitTestNode(composedChildren[i], testX, testY,
                                childOffsetX, childOffsetY);
        if (hit) return hit;
    }

    // No child hit — return this element only if point is inside its bounds
    if (insideBounds)
        return elem;
    return nullptr;
}

dom::Element* hitTestNode(dom::Node* node, float x, float y,
                          float offsetX, float offsetY) {
    if (!node || node->nodeType() != dom::NodeType::Element) return nullptr;
    return hitTestElement(static_cast<dom::Element*>(node), x, y, offsetX, offsetY);
}

} // namespace bro::engine
