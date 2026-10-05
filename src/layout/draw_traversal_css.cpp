// CSS value helpers shared by the DrawTraversal painters: transforms with
// ancestor perspective, shadow lengths, `filter`, blend modes, `clip-path`
// polygons, overflow propagation and border radii.

#include "layout/draw_traversal_internal.h"
#include "layout/formatting_context.h"
#include "dom/document.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <string_view>

namespace bro::layout {

// CSS transform and transform-origin parsing live in htmlayout
// (htmlayout::css::parseTransform, parseTransformOrigin, Matrix2D / Matrix3D).

namespace {

void parsePerspectiveOrigin(const htmlayout::css::ComputedStyle& cs,
                             float refW, float refH, float& ox, float& oy) {
    ox = refW * 0.5f;
    oy = refH * 0.5f;
    auto it = cs.find("perspective-origin");
    if (it == cs.end() || it->second.empty()) return;
    htmlayout::css::parseTransformOrigin(it->second, refW, refH, ox, oy);
}

} // namespace

// Build the full 4x4 transform to wrap the element with, including ancestor
// perspective if any. bx/by/bw/bh = element border box in absolute coords.
// pbx/pby/pbw/pbh = perspective container's border box in the same absolute
// coords (only meaningful if persp > 0). Returns identity if no transform.
// Sets `is3D` when the resulting matrix has any 3D component.
htmlayout::css::Matrix3D buildElementTransform4x4(
        const htmlayout::css::ComputedStyle& style,
        float bx, float by, float bw, float bh,
        float persp,
        float pbx, float pby, float pbw, float pbh,
        const htmlayout::css::ComputedStyle* perspStyle,
        bool& has3D) {
    using htmlayout::css::Matrix3D;
    has3D = false;
    Matrix3D result; // identity

    auto trIt = style.find("transform");
    bool hasT = (trIt != style.end() && !trIt->second.empty()
                 && trIt->second != "none");
    bool wants3D = (persp > 0) || (hasT && transformHas3D(trIt->second));
    if (!wants3D) return result;

    // Element transform about transform-origin (3D form).
    if (hasT) {
        Matrix3D mat = htmlayout::css::parseTransform3D(trIt->second, bw, bh);
        float ox = bw * 0.5f, oy = bh * 0.5f, oz = 0.0f;
        auto toIt = style.find("transform-origin");
        std::string_view originVal = (toIt != style.end())
            ? std::string_view(toIt->second) : std::string_view();
        htmlayout::css::parseTransformOrigin3D(originVal, bw, bh, ox, oy, oz);
        Matrix3D toOrigin;     toOrigin.m[12] = bx + ox; toOrigin.m[13] = by + oy; toOrigin.m[14] = oz;
        Matrix3D fromOrigin;   fromOrigin.m[12] = -(bx + ox); fromOrigin.m[13] = -(by + oy); fromOrigin.m[14] = -oz;
        result = toOrigin * mat * fromOrigin;
    }

    // Apply ancestor perspective P = T(po_abs) * persp(d) * T(-po_abs).
    if (persp > 0 && perspStyle) {
        float pox = pbw * 0.5f, poy = pbh * 0.5f;
        parsePerspectiveOrigin(*perspStyle, pbw, pbh, pox, poy);
        float ax = pbx + pox, ay = pby + poy;
        Matrix3D persp_m = htmlayout::css::makePerspectiveMatrix(persp);
        Matrix3D toPO;     toPO.m[12] = ax;  toPO.m[13] = ay;
        Matrix3D fromPO;   fromPO.m[12] = -ax; fromPO.m[13] = -ay;
        Matrix3D P = toPO * persp_m * fromPO;
        result = P * result;
    }

    has3D = !result.is2D();
    return result;
}

static float styleFontSizePx(const htmlayout::css::ComputedStyle& style) {
    auto it = style.find("font-size");
    if (it == style.end()) return 16.0f;
    char* end = nullptr;
    const float v = std::strtof(it->second.c_str(), &end);
    return (end != it->second.c_str() && v > 0) ? v : 16.0f;
}

// What a shadow's em/rem/vw lengths resolve against for `elem`.
CssLengthContext shadowLengthContext(dom::Element* elem,
                                            const htmlayout::css::ComputedStyle& style,
                                            int viewportW, int viewportH) {
    CssLengthContext cx;
    cx.fontSize = styleFontSizePx(style);
    if (dom::Document* doc = elem ? elem->document() : nullptr) {
        if (dom::Element* root = doc->documentElement())
            cx.rootFontSize = styleFontSizePx(root->computedStyle());
    }
    cx.viewportW = static_cast<float>(viewportW);
    cx.viewportH = static_cast<float>(viewportH);
    return cx;
}

// ---------------------------------------------------------------------------
// CSS `filter:` parsing → list of CssFilterParams descriptors. Backends
// translate descriptors into native filter objects (see render::filter_chain).
// Supports: blur, brightness, contrast, grayscale, sepia, saturate,
//           hue-rotate, invert, opacity, drop-shadow
// ---------------------------------------------------------------------------
std::vector<render::CssFilterParams> parseCSSFilter(const std::string& val,
                                                           const bromath::Color& currentColor,
                                                           const CssLengthContext& lengths) {
    std::vector<render::CssFilterParams> result;
    size_t pos = 0;
    while (pos < val.size()) {
        while (pos < val.size() && (val[pos] == ' ' || val[pos] == '\t'))
            ++pos;
        if (pos >= val.size()) break;

        size_t nameStart = pos;
        while (pos < val.size() && val[pos] != '(') ++pos;
        std::string func = val.substr(nameStart, pos - nameStart);
        while (!func.empty() && func.back() == ' ') func.pop_back();
        if (pos >= val.size()) break;
        ++pos; // skip '('

        auto readFloat = [&]() -> float {
            while (pos < val.size() && (val[pos] == ' ' || val[pos] == ','))
                ++pos;
            char* end = nullptr;
            float v = std::strtof(val.c_str() + pos, &end);
            pos = static_cast<size_t>(end - val.c_str());
            if (pos < val.size() && val[pos] == '%') {
                v /= 100.0f;
                ++pos;
            }
            while (pos < val.size() && std::isalpha(static_cast<unsigned char>(val[pos])))
                ++pos;
            return v;
        };

        render::CssFilterParams f{};
        bool keep = true;

        if (func == "blur") {
            // blur(<length>?): the radius resolves its unit (1em is the
            // font-size, not 1px); an empty blur() is 0.
            f.kind = render::CssFilterParams::Blur;
            size_t argStart = pos;
            int pdepth = 0;
            while (pos < val.size()) {
                char c = val[pos];
                if (c == '(') ++pdepth;
                else if (c == ')') { if (pdepth == 0) break; --pdepth; }
                ++pos;
            }
            std::string_view arg = std::string_view(val).substr(argStart, pos - argStart);
            f.a = 0.0f;
            if (arg.find_first_not_of(" \t") != std::string_view::npos &&
                (!resolveCssLength(arg, lengths, f.a) || f.a < 0.0f))
                keep = false;
        } else if (func == "brightness") {
            f.kind = render::CssFilterParams::Brightness;
            f.a = readFloat();
        } else if (func == "contrast") {
            f.kind = render::CssFilterParams::Contrast;
            f.a = readFloat();
        } else if (func == "grayscale") {
            f.kind = render::CssFilterParams::Grayscale;
            f.a = readFloat();
        } else if (func == "sepia") {
            f.kind = render::CssFilterParams::Sepia;
            f.a = readFloat();
        } else if (func == "saturate") {
            f.kind = render::CssFilterParams::Saturate;
            f.a = readFloat();
        } else if (func == "hue-rotate") {
            f.kind = render::CssFilterParams::HueRotate;
            while (pos < val.size() && (val[pos] == ' ')) ++pos;
            char* end = nullptr;
            f.a = std::strtof(val.c_str() + pos, &end);
            pos = static_cast<size_t>(end - val.c_str());
            while (pos < val.size() && std::isalpha(static_cast<unsigned char>(val[pos])))
                ++pos;
        } else if (func == "invert") {
            f.kind = render::CssFilterParams::Invert;
            f.a = readFloat();
        } else if (func == "opacity") {
            f.kind = render::CssFilterParams::Opacity;
            f.a = readFloat();
        } else if (func == "drop-shadow") {
            f.kind = render::CssFilterParams::DropShadow;
            // Scan to the drop-shadow's closing paren, tracking depth so a
            // functional color — rgba()/hsl()/color() — isn't truncated at its
            // own inner ')'. The argument is `<color>? && <length>{2,3}` in
            // either order; with no colour it is currentcolor.
            size_t argStart = pos;
            int pdepth = 0;
            while (pos < val.size()) {
                char c = val[pos];
                if (c == '(') ++pdepth;
                else if (c == ')') { if (pdepth == 0) break; --pdepth; }
                ++pos;
            }
            CssShadow s;
            if (parseCssShadow(std::string_view(val).substr(argStart, pos - argStart),
                               currentColor, 3, lengths, s) && !s.inset) {
                f.dx = s.dx;
                f.dy = s.dy;
                f.blur = s.blur;
                f.shadowColor = s.color;
            } else {
                keep = false;
            }
        } else {
            keep = false;
        }

        if (keep) result.push_back(f);

        while (pos < val.size() && val[pos] != ')') ++pos;
        if (pos < val.size()) ++pos;
    }
    return result;
}

/// Map a CSS `mix-blend-mode` keyword to a render::BlendMode. Returns Normal
/// for `normal`/unknown values.
render::BlendMode parseBlendMode(const std::string& v) {
    if (v == "multiply")    return render::BlendMode::Multiply;
    if (v == "screen")      return render::BlendMode::Screen;
    if (v == "overlay")     return render::BlendMode::Overlay;
    if (v == "darken")      return render::BlendMode::Darken;
    if (v == "lighten")     return render::BlendMode::Lighten;
    if (v == "color-dodge") return render::BlendMode::ColorDodge;
    if (v == "color-burn")  return render::BlendMode::ColorBurn;
    if (v == "hard-light")  return render::BlendMode::HardLight;
    if (v == "soft-light")  return render::BlendMode::SoftLight;
    if (v == "difference")  return render::BlendMode::Difference;
    if (v == "exclusion")   return render::BlendMode::Exclusion;
    if (v == "hue")         return render::BlendMode::Hue;
    if (v == "saturation")  return render::BlendMode::Saturation;
    if (v == "color")       return render::BlendMode::Color;
    if (v == "luminosity")  return render::BlendMode::Luminosity;
    return render::BlendMode::Normal;
}

/// Parse a CSS `clip-path: polygon(...)` value into vertex points (border-box
/// relative). Returns empty when the value is none/auto/empty/unrecognized.
/// Each coordinate is a <length-percentage> resolved through the shared
/// htmlayout length resolver, so px, %, font-relative units, and the math
/// functions (calc()/min()/max()/clamp()) all work. Skips a leading
/// `<fill-rule>,` (nonzero|evenodd) since we treat the path as a simple
/// polygon outline. A vertex that isn't exactly two components invalidates
/// the whole value — no clip — matching CSS's declaration-level error
/// handling. Tokenization is paren-depth aware and always consumes input, so
/// no value can stall it (a bare `calc(...)` coordinate used to livelock the
/// old strtof cursor loop here and hang the frame).
std::vector<render::PointF> parseClipPathPolygon(
    const std::string& val, float refW, float refH, float fontSize) {
    std::vector<render::PointF> out;
    size_t start = val.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return out;
    if (val.compare(start, 8, "polygon(") != 0) return out;
    size_t open = start + 8;
    size_t close = val.rfind(')');
    if (close == std::string::npos || close < open) return out;
    const std::string body = val.substr(open, close - open);

    auto isWs = [](char c) {
        return std::isspace(static_cast<unsigned char>(c)) != 0;
    };
    auto trim = [&](const std::string& s) -> std::string {
        size_t b = 0, e = s.size();
        while (b < e && isWs(s[b])) ++b;
        while (e > b && isWs(s[e - 1])) --e;
        return s.substr(b, e - b);
    };

    // Split the vertex list on top-level commas — min()/max()/clamp() carry
    // commas of their own, so track paren depth.
    std::vector<std::string> verts;
    int depth = 0;
    size_t vstart = 0;
    for (size_t i = 0; i <= body.size(); ++i) {
        char c = i < body.size() ? body[i] : ',';
        if (c == '(') ++depth;
        else if (c == ')') --depth;
        else if (c == ',' && depth == 0) {
            verts.push_back(trim(body.substr(vstart, i - vstart)));
            vstart = i + 1;
        }
    }
    if (verts.size() == 1 && verts[0].empty()) return out;  // polygon()

    size_t first = 0;
    if (!verts.empty() && (verts[0] == "nonzero" || verts[0] == "evenodd"))
        first = 1;

    out.reserve(verts.size() - first);
    for (size_t vi = first; vi < verts.size(); ++vi) {
        // Split the vertex into its x and y components on top-level
        // whitespace — spaces inside calc(100% - 8px) don't separate.
        std::vector<std::string> comps;
        const std::string& v = verts[vi];
        depth = 0;
        size_t cstart = std::string::npos;
        for (size_t i = 0; i <= v.size(); ++i) {
            char c = i < v.size() ? v[i] : ' ';
            if (c == '(') ++depth;
            else if (c == ')') --depth;
            if (depth == 0 && isWs(c)) {
                if (cstart != std::string::npos) {
                    comps.push_back(v.substr(cstart, i - cstart));
                    cstart = std::string::npos;
                }
            } else if (cstart == std::string::npos) {
                cstart = i;
            }
        }
        if (comps.size() != 2) return {};
        out.push_back({
            htmlayout::layout::resolveLength(comps[0], refW, fontSize),
            htmlayout::layout::resolveLength(comps[1], refH, fontSize),
        });
    }
    return out;
}

/// Does this element's `overflow` belong to the *viewport* rather than to its
/// own box?
///
/// CSS 2.1 §11.1.1: the viewport takes its overflow from the root element, or
/// from `<body>` when the root's own value is `visible`. Whichever element
/// donates it is then treated as `overflow: visible`, because the clip has
/// moved to the viewport — and bro's viewport is the window, which the
/// compositor already bounds every frame.
///
/// This is not a corner case. `body { overflow: hidden }` is what nearly every
/// application-shaped page writes to stop the document scrolling, and such a
/// page usually positions its whole UI absolutely — which leaves the body box
/// zero-height. Clipping to that box instead of to the viewport clips the
/// entire interface away, and the window paints empty with a DOM, a layout and
/// computed styles that are all perfectly correct.
bool overflowBelongsToViewport(dom::Element* elem) {
    if (!elem) return false;
    dom::Element* parent = elem->parentElement();
    if (!parent) return true;                   // the root element itself
    if (parent->parentElement()) return false;  // deeper than <body>

    const std::string& tag = elem->tagName();
    if (tag != "BODY" && tag != "body") return false;

    // The root donates first; <body> only gets to when the root is visible.
    const auto& rootStyle = parent->computedStyle();
    return !overflowAxisClips(getOverflowX(rootStyle)) &&
           !overflowAxisClips(getOverflowY(rootStyle));
}

// Parse one corner radius value: "12px" -> (12, 12) or "30% 50%" -> (30%w, 50%h).
// Also handles a full slashed shorthand like "30% / 50%" which the htmlayout
// expansion stores verbatim on each corner property (see properties.cpp).
static void parseCornerRadius(const std::string& val, float boxW, float boxH,
                              float& outX, float& outY) {
    outX = outY = 0;
    if (val.empty()) return;
    // Split on '/' (slash form: horizontal / vertical lists)
    auto slash = val.find('/');
    if (slash != std::string::npos) {
        // Take first token from each side. The shorthand can have up to 4
        // values per side; for a single corner we want the first value of
        // each side. (Per CSS, htmlayout writes the whole shorthand string
        // onto each corner — picking the first token approximates the
        // top-left, which is good enough for the common 1/1 case. The
        // full per-corner mapping is handled by the per-side fallback in
        // parseRadii below, which calls this function with already-split
        // single values.)
        std::string h = val.substr(0, slash);
        std::string v = val.substr(slash + 1);
        // Trim leading whitespace + take first whitespace-separated token
        auto firstToken = [](const std::string& s) {
            size_t a = 0;
            while (a < s.size() && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
            size_t b = a;
            while (b < s.size() && !std::isspace(static_cast<unsigned char>(s[b]))) ++b;
            return s.substr(a, b - a);
        };
        outX = parseLengthPx(firstToken(h), boxW);
        outY = parseLengthPx(firstToken(v), boxH);
        return;
    }
    // Either "12px" or "h v" (two values, h then v)
    std::string s = val;
    size_t a = 0;
    while (a < s.size() && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    size_t b = a;
    while (b < s.size() && !std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    std::string t1 = s.substr(a, b - a);
    while (b < s.size() && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    size_t c = b;
    while (c < s.size() && !std::isspace(static_cast<unsigned char>(s[c]))) ++c;
    std::string t2 = (b < c) ? s.substr(b, c - b) : std::string();
    outX = parseLengthPx(t1, boxW);
    outY = t2.empty() ? outX : parseLengthPx(t2, boxH);
}

// Resolve full per-corner border radii for an element of size (boxW, boxH).
// Per CSS spec, applies the corner-overlap scaling so adjacent corners on a
// side don't sum to more than the side length.
render::Radii getRadii(const htmlayout::css::ComputedStyle& style,
                              float boxW, float boxH) {
    render::Radii r;
    // Order: TL, TR, BR, BL — matches SkRRect::Corner enum
    const char* props[4] = {
        "border-top-left-radius",
        "border-top-right-radius",
        "border-bottom-right-radius",
        "border-bottom-left-radius",
    };
    // First, check for the slashed border-radius shorthand stored verbatim on
    // each corner — htmlayout writes the full "h-list / v-list" string onto
    // every corner property when the slash form is used. Detect that and
    // expand into per-corner h and v values.
    bool slashShorthand = false;
    std::string slashVal;
    auto tlIt = style.find("border-top-left-radius");
    if (tlIt != style.end() && tlIt->second.find('/') != std::string::npos) {
        // Confirm all four corners share the same value (i.e. slash form, not
        // user-set individual longhand).
        slashShorthand = true;
        slashVal = tlIt->second;
        for (int i = 1; i < 4; ++i) {
            auto it = style.find(props[i]);
            if (it == style.end() || it->second != slashVal) {
                slashShorthand = false;
                break;
            }
        }
    }

    auto parseList = [](const std::string& s, std::vector<std::string>& out) {
        out.clear();
        size_t i = 0;
        while (i < s.size()) {
            while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
            size_t j = i;
            while (j < s.size() && !std::isspace(static_cast<unsigned char>(s[j]))) ++j;
            if (j > i) out.push_back(s.substr(i, j - i));
            i = j;
        }
    };
    auto fillBoxValues = [](std::vector<std::string>& v) {
        // CSS box shorthand: 1->all, 2->v/h, 3->t,h,b, 4->t,r,b,l. For
        // border-radius corners (TL,TR,BR,BL): 1->all, 2->TL/BR, TR/BL,
        // 3->TL, TR/BL, BR, 4->TL,TR,BR,BL.
        if (v.empty()) v.push_back("0");
        std::string a = v.size() >= 1 ? v[0] : v[0];
        std::string b = v.size() >= 2 ? v[1] : a;
        std::string c = v.size() >= 3 ? v[2] : a;
        std::string d = v.size() >= 4 ? v[3] : b;
        v = {a, b, c, d};
    };

    if (slashShorthand) {
        auto slashPos = slashVal.find('/');
        std::vector<std::string> hList, vList;
        parseList(slashVal.substr(0, slashPos), hList);
        parseList(slashVal.substr(slashPos + 1), vList);
        fillBoxValues(hList);
        fillBoxValues(vList);
        for (int i = 0; i < 4; ++i) {
            r.x[i] = parseLengthPx(hList[i], boxW);
            r.y[i] = parseLengthPx(vList[i], boxH);
        }
    } else {
        for (int i = 0; i < 4; ++i) {
            auto it = style.find(props[i]);
            if (it == style.end() || it->second.empty()) continue;
            parseCornerRadius(it->second, boxW, boxH, r.x[i], r.y[i]);
        }
    }

    // CSS spec: if sum of two adjacent corners exceeds the side, scale all
    // radii by the same factor so they fit. Apply per side, take min factor.
    auto sideFactor = [](float a, float b, float side) {
        if (a + b <= side || side <= 0) return 1.0f;
        return side / (a + b);
    };
    float fTop    = sideFactor(r.x[0], r.x[1], boxW);
    float fRight  = sideFactor(r.y[1], r.y[2], boxH);
    float fBottom = sideFactor(r.x[3], r.x[2], boxW);
    float fLeft   = sideFactor(r.y[0], r.y[3], boxH);
    float f = std::min({fTop, fRight, fBottom, fLeft});
    if (f < 1.0f) {
        for (int i = 0; i < 4; ++i) {
            r.x[i] *= f;
            r.y[i] *= f;
        }
    }
    // Clamp tiny negatives from float math to 0
    for (int i = 0; i < 4; ++i) {
        if (r.x[i] < 0) r.x[i] = 0;
        if (r.y[i] < 0) r.y[i] = 0;
    }
    return r;
}

} // namespace bro::layout
