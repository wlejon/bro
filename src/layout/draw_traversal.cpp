#include "layout/draw_traversal.h"
#include "layout/el_input.h"
#include "layout/el_textarea.h"
#include "layout/el_select.h"
#include "layout/el_svg.h"
#include "layout/el_video.h"
#include "canvas/canvas_scene.h"
#include "webgl/webgl2_context.h"
#include "css/transform.h"
#include "dom/element.h"
#include "dom/element_geometry.h"
#include "dom/text_node.h"
#include "dom/node.h"
#include "util/log.h"
#include "util/string_utils.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <span>
#include <sstream>
#include "broimage/decode.h"


namespace bro::layout {

using bromath::cfromColor8;

// Forward declarations of file-local border-collapse helpers (defined later in
// the file alongside drawBorders).
static bool isCollapsedTable(dom::Element* elem);
static bool isCellInCollapsedTable(dom::Element* elem);

// CSS transform and transform-origin parsing live in htmlayout
// (htmlayout::css::parseTransform, parseTransformOrigin, Matrix2D / Matrix3D).

namespace {

// Hand out a process-unique id for each newly cached image. The renderer's
// DecodedImageCache keys on this so an image is decoded once, not every frame.
// Ids are process-global (not per-DrawTraversal) so a rebuilt traversal after a
// document reload can never collide with a stale renderer-side cache entry.
uint64_t nextImageId() {
    static std::atomic<uint64_t> counter{1};
    return counter.fetch_add(1, std::memory_order_relaxed);
}

// Return true if a CSS `transform` value uses any 3D function (rotateX/Y/3d,
// translateZ/3d, scaleZ/3d, perspective(), matrix3d). Cheap substring scan —
// false positives only mean we take the 4x4 path unnecessarily.
bool transformHas3D(const std::string& v) {
    if (v.empty() || v == "none") return false;
    // Quickly look for any of the 3D function tokens.
    static constexpr const char* kKeys[] = {
        "rotateX", "rotateY", "rotate3d", "rotateZ",
        "translateZ", "translate3d", "scaleZ", "scale3d",
        "matrix3d", "perspective("
    };
    for (auto* k : kKeys) {
        if (v.find(k) != std::string::npos) return true;
    }
    return false;
}

// Walk to the element's layout parent and read its `perspective` value (length
// or 0 for "none"). Returns 0 if no ancestor sets perspective or the element
// has no parent.
float parentPerspective(bro::dom::Element* elem) {
    if (!elem) return 0.0f;
    auto* p = elem->layoutParent();
    if (!p) return 0.0f;
    auto& cs = p->computedStyle();
    auto it = cs.find("perspective");
    if (it == cs.end()) return 0.0f;
    return htmlayout::css::parsePerspective(it->second);
}

void parsePerspectiveOrigin(const htmlayout::css::ComputedStyle& cs,
                             float refW, float refH, float& ox, float& oy) {
    ox = refW * 0.5f;
    oy = refH * 0.5f;
    auto it = cs.find("perspective-origin");
    if (it == cs.end() || it->second.empty()) return;
    htmlayout::css::parseTransformOrigin(it->second, refW, refH, ox, oy);
}

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

} // namespace

// ---------------------------------------------------------------------------
// CSS `filter:` parsing → list of CssFilterParams descriptors. Backends
// translate descriptors into native filter objects (see render::filter_chain).
// Supports: blur, brightness, contrast, grayscale, sepia, saturate,
//           hue-rotate, invert, opacity, drop-shadow
// ---------------------------------------------------------------------------
static std::vector<render::CssFilterParams> parseCSSFilter(const std::string& val) {
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
            f.kind = render::CssFilterParams::Blur;
            f.a = readFloat();
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
            f.dx = readFloat();
            f.dy = readFloat();
            f.blur = readFloat();
            bromath::Color sc = cfromColor8({0, 0, 0, 255});
            size_t colorStart = pos;
            // Scan to the drop-shadow's closing paren, tracking depth so a
            // functional color — rgba()/hsl()/color() — isn't truncated at its
            // own inner ')'.
            int pdepth = 0;
            while (pos < val.size()) {
                char c = val[pos];
                if (c == '(') ++pdepth;
                else if (c == ')') { if (pdepth == 0) break; --pdepth; }
                ++pos;
            }
            std::string colorStr = val.substr(colorStart, pos - colorStart);
            size_t ca = colorStr.find_first_not_of(" \t");
            if (ca != std::string::npos) {
                colorStr = colorStr.substr(ca);
                size_t cb = colorStr.find_last_not_of(" \t");
                if (cb != std::string::npos) colorStr = colorStr.substr(0, cb + 1);
                DrawTraversal::tryParseColor(colorStr, sc);
            }
            f.shadowColor = sc;
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
static render::BlendMode parseBlendMode(const std::string& v) {
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
/// Supports `polygon(<x1> <y1>, <x2> <y2>, ...)` with px or % units. Skips a
/// leading `<fill-rule>,` (nonzero|evenodd) since we treat the path as a
/// simple polygon outline.
static std::vector<render::PointF> parseClipPathPolygon(
    const std::string& val, float refW, float refH) {
    std::vector<render::PointF> out;
    if (val.empty() || val == "none") return out;
    auto skip = [](const char*& p) {
        while (*p && std::isspace(static_cast<unsigned char>(*p))) ++p;
    };
    const char* p = val.c_str();
    skip(p);
    if (std::strncmp(p, "polygon(", 8) != 0) return out;
    p += 8;
    skip(p);
    // Optional fill-rule prefix.
    if (std::strncmp(p, "nonzero", 7) == 0 || std::strncmp(p, "evenodd", 7) == 0) {
        p += 7;
        skip(p);
        if (*p == ',') { ++p; skip(p); }
    }
    auto parseLen = [&](float ref) -> float {
        skip(p);
        char* end = nullptr;
        float v = std::strtof(p, &end);
        if (end == p) return 0.0f;
        p = end;
        if (*p == '%') { v = v * ref / 100.0f; ++p; }
        else if (std::strncmp(p, "px", 2) == 0) { p += 2; }
        return v;
    };
    while (*p && *p != ')') {
        float xv = parseLen(refW);
        float yv = parseLen(refH);
        out.push_back({xv, yv});
        skip(p);
        if (*p == ',') { ++p; skip(p); }
    }
    return out;
}

/// Get the effective vertical overflow value, checking overflow-y then overflow.
static std::string getOverflowY(const htmlayout::css::ComputedStyle& style) {
    auto oyIt = style.find("overflow-y");
    if (oyIt != style.end()) return oyIt->second;
    auto oIt = style.find("overflow");
    if (oIt != style.end()) return oIt->second;
    return "visible";
}

/// Get the effective horizontal overflow value, checking overflow-x then overflow.
static std::string getOverflowX(const htmlayout::css::ComputedStyle& style) {
    auto oxIt = style.find("overflow-x");
    if (oxIt != style.end()) return oxIt->second;
    auto oIt = style.find("overflow");
    if (oIt != style.end()) return oIt->second;
    return "visible";
}

DrawTraversal::DrawTraversal(render::Renderer* renderer)
    : renderer_(renderer) {}

// ---------------------------------------------------------------------------
// Stacking-context helpers (CSS 2.1 Appendix E)
// ---------------------------------------------------------------------------
namespace {

const std::string& styleProp(const htmlayout::css::ComputedStyle& s,
                             const char* name, const std::string& fallback) {
    auto it = s.find(name);
    return (it != s.end()) ? it->second : fallback;
}

bool isPositioned(const htmlayout::css::ComputedStyle& s) {
    auto it = s.find("position");
    if (it == s.end()) return false;
    const std::string& p = it->second;
    return p == "relative" || p == "absolute" || p == "fixed" || p == "sticky";
}

// Returns true if z-index is the literal keyword 'auto' (or absent).
// Out-parameter outZ holds the parsed integer (0 when auto/absent/invalid).
bool getZIndex(const htmlayout::css::ComputedStyle& s, int& outZ) {
    auto it = s.find("z-index");
    if (it == s.end() || it->second.empty() || it->second == "auto") {
        outZ = 0;
        return true; // is auto
    }
    char* end = nullptr;
    long v = std::strtol(it->second.c_str(), &end, 10);
    if (end == it->second.c_str()) {
        outZ = 0;
        return true;
    }
    outZ = static_cast<int>(v);
    return false;
}

// Returns true if this element creates a new stacking context.
// CSS 2.1 + commonly-implemented CSS3 triggers, restricted to what parity
// tests exercise. Does not yet trigger on: will-change, contain:layout/paint,
// container-type.
bool createsStackingContext(dom::Element* elem, bool isRoot) {
    if (isRoot) return true; // root element always creates SC
    auto& s = elem->computedStyle();

    // position: fixed / sticky → always SC
    auto posIt = s.find("position");
    std::string pos = (posIt != s.end()) ? posIt->second : "static";
    if (pos == "fixed" || pos == "sticky") return true;

    // position: relative/absolute with z-index != auto → SC
    if (pos == "relative" || pos == "absolute") {
        int z; bool isAuto = getZIndex(s, z);
        if (!isAuto) return true;
    }

    // opacity < 1
    auto opIt = s.find("opacity");
    if (opIt != s.end() && !opIt->second.empty()) {
        float op = std::strtof(opIt->second.c_str(), nullptr);
        if (op < 1.0f) return true;
    }

    // transform != none
    auto trIt = s.find("transform");
    if (trIt != s.end() && !trIt->second.empty() && trIt->second != "none")
        return true;

    // filter != none
    auto fIt = s.find("filter");
    if (fIt != s.end() && !fIt->second.empty() && fIt->second != "none")
        return true;

    // isolation: isolate
    auto isoIt = s.find("isolation");
    if (isoIt != s.end() && isoIt->second == "isolate") return true;

    // mix-blend-mode != normal
    auto mbIt = s.find("mix-blend-mode");
    if (mbIt != s.end() && !mbIt->second.empty() && mbIt->second != "normal")
        return true;

    return false;
}

} // namespace

void DrawTraversal::pushClipRect(float x, float y, float w, float h) {
    // Intersect with the current effective clip so the new top stays the
    // running intersection (empty intersections collapse to zero area).
    if (!clipRectStack_.empty()) {
        const ClipBox& t = clipRectStack_.back();
        float nx = std::max(x, t.x);
        float ny = std::max(y, t.y);
        float nr = std::min(x + w, t.x + t.w);
        float nb = std::min(y + h, t.y + t.h);
        w = std::max(0.0f, nr - nx);
        h = std::max(0.0f, nb - ny);
        x = nx; y = ny;
    }
    clipRectStack_.push_back({x, y, w, h});
}

bool DrawTraversal::currentClipRect(float& x, float& y, float& w, float& h) const {
    if (clipRectStack_.empty()) return false;
    const ClipBox& t = clipRectStack_.back();
    x = t.x; y = t.y; w = t.w; h = t.h;
    return true;
}

void DrawTraversal::draw(dom::Element* root, float scrollX, float scrollY,
                         int viewportW, int viewportH, int viewportTop) {
    if (!root || !renderer_) return;
    viewportW_ = viewportW;
    viewportH_ = viewportH;
    viewportTop_ = viewportTop;
    // The root offset args double as the document→surface translation for
    // this pass. The app document draws into content-sized layer surfaces in
    // content space, so the engine passes (0, −scrollY); system panels pass
    // (0, 0) in window space. The engine-reserved inset never enters here —
    // the compositor applies it once when placing app layers.
    rootOffsetX_ = scrollX;
    rootOffsetY_ = scrollY;

    // skipSet_ is rebuilt per draw() — buildStackingContextTree inserts every
    // element that's an SC root or positioned-non-SC for the *current* frame.
    // Without clearing, entries from prior frames stick: an element that was
    // an SC last frame (e.g. .tile-inner with an active scale animation) but
    // isn't this frame (animation completed, transform fell back to none)
    // would remain in skipSet_ and be skipped by the in-flow walker even
    // though it's no longer in the SC tree — its content would vanish.
    skipSet_.clear();
    clipRectStack_.clear();

    // Build the stacking-context tree (CSS 2.1 Appendix E), then paint in the
    // seven-step order. Each SC root paints its own box first, then recurses
    // into negative-z children, then in-flow descendants (via the normal
    // walker, with positioned/SC descendants skipped via skipSet_), then
    // positioned-non-SC descendants and z:auto SC children in tree order,
    // then positive-z child SCs.
    auto rootSC = buildStackingContextTree(root, scrollX, scrollY);
    if (rootSC) paintStackingContext(rootSC.get());
}

void DrawTraversal::drawElement(dom::Element* elem, float offsetX, float offsetY) {
    if (!elem) return;
    drawElementContent(elem, offsetX, offsetY);
}

void DrawTraversal::drawNode(dom::Node* node, float offsetX, float offsetY) {
    if (!node) return;

    if (node->nodeType() == dom::NodeType::Element) {
        // CSS 2.1 Appendix E: subtrees rooted at a child stacking context or at
        // a positioned non-SC descendant are painted out-of-order by the SC
        // walker — skip them when reached via the normal in-flow walk so they
        // don't paint twice (or at the wrong z order).
        auto* el = static_cast<dom::Element*>(node);
        if (skipSet_.count(el)) return;
        drawElementContent(el, offsetX, offsetY);
    } else if (node->nodeType() == dom::NodeType::Text) {
        auto* parent = node->parentNode();
        if (parent && parent->nodeType() == dom::NodeType::Element) {
            drawText(node, static_cast<dom::Element*>(parent), offsetX, offsetY);
        }
    }
}

// Parse a CSS length value (px, em, %) into pixels. Percentage is relative to ref.
static float parseLengthPx(const std::string& val, float ref = 0) {
    if (val.empty()) return 0;
    char* end = nullptr;
    float v = std::strtof(val.c_str(), &end);
    if (end == val.c_str()) return 0;
    if (end && *end == '%') return v * ref / 100.0f;
    return v; // px or unitless
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
static render::Radii getRadii(const htmlayout::css::ComputedStyle& style,
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

// Legacy single-value radius helper (kept for places that still want a scalar
// fallback — only the "is there any rounding?" question, not actual drawing).
static float getBorderRadius(const htmlayout::css::ComputedStyle& style) {
    auto r = getRadii(style, 0, 0);
    float sum = 0; int count = 0;
    for (int i = 0; i < 4; ++i) {
        if (r.x[i] > 0) { sum += r.x[i]; ++count; }
        if (r.y[i] > 0) { sum += r.y[i]; ++count; }
    }
    return count > 0 ? sum / count : 0;
}

void DrawTraversal::drawElementContent(dom::Element* elem, float offsetX, float offsetY) {
    if (!elem) return;

    auto& style = elem->computedStyle();

    // Check display:none
    auto dispIt = style.find("display");
    if (dispIt != style.end() && dispIt->second == "none") return;

    // Flow-collapsed content (closed <details> body — UA -x-flow-collapse):
    // laid out with real geometry but never painted, whole subtree.
    auto fcIt = style.find("-x-flow-collapse");
    if (fcIt != style.end() && fcIt->second == "collapse") return;

    // Check visibility:hidden (still occupies space but not drawn)
    bool visible = true;
    auto visIt = style.find("visibility");
    if (visIt != style.end() && visIt->second == "hidden") visible = false;

    auto& box = elem->layoutBox();
    float x = box.contentRect.x + offsetX;
    float y = box.contentRect.y + offsetY;
    float w = box.contentRect.width;
    float h = box.contentRect.height;

    // Border box for background/border drawing
    float bx = x - box.padding.left - box.border.left;
    float by = y - box.padding.top - box.border.top;
    float bw = box.fullWidth();
    float bh = box.fullHeight();

    // CSS Transform / opacity / filter: wrap entire element drawing.
    // For stacking-context roots, paintStackingContext has already wrapped
    // these around the full SC subtree (so positioned descendants inherit the
    // transform); skip re-applying them here in that case.
    bool skipWrap = scRootSkipWrap_.count(elem) > 0;

    // Off-screen culling. When an ancestor clips its overflow (a scroll
    // container), any in-flow descendant whose border box lies entirely outside
    // that clip paints nothing, so skip recording it and its whole subtree. This
    // is what keeps the base re-record O(visible) instead of O(total DOM): a long
    // list (e.g. a 300-row model picker) re-records on every hover/scroll frame,
    // and without this every row is recorded even when only ~15 are on screen —
    // the source of hover/scroll lag on big lists.
    //
    // Kept conservative to never drop visible paint:
    //   * only inside an active clip (clipRectStack_ non-empty) — the clip
    //     guarantees in-flow descendants can't paint beyond it;
    //   * only in-flow, untransformed boxes. absolute/fixed can escape the clip,
    //     sticky is repositioned at paint time, and a transform can map the box
    //     back on screen — layoutBox already bakes in relative/sticky offsets;
    //   * overscan the clip by a slack margin so a just-off-screen element's
    //     box-shadow/outline spilling toward the viewport still records.
    if (!clipRectStack_.empty() && !skipWrap) {
        const ClipBox& clip = clipRectStack_.back();
        constexpr float kCullSlack = 256.0f;  // covers typical shadow/outline spill
        bool outside = by + bh < clip.y - kCullSlack ||
                       by      > clip.y + clip.h + kCullSlack ||
                       bx + bw < clip.x - kCullSlack ||
                       bx      > clip.x + clip.w + kCullSlack;
        if (outside) {
            auto posIt = style.find("position");
            bool inFlow = posIt == style.end() ||
                          posIt->second == "static" ||
                          posIt->second == "relative";
            auto trIt = style.find("transform");
            bool noTransform = trIt == style.end() || trIt->second.empty() ||
                               trIt->second == "none";
            if (inFlow && noTransform) return;
        }
    }

    bool hasTransform = false;
    if (!skipWrap) {
        auto trIt = style.find("transform");
        bool hasT = (trIt != style.end() && !trIt->second.empty()
                     && trIt->second != "none");
        float persp = parentPerspective(elem);
        bool wants3D = (persp > 0) || (hasT && transformHas3D(trIt->second));

        if (wants3D) {
            // 4x4 path (3D transforms or ancestor perspective).
            // Parent border box in absolute coords:
            float pbx = 0, pby = 0, pbw = 0, pbh = 0;
            const htmlayout::css::ComputedStyle* perspStyle = nullptr;
            if (auto* parent = elem->layoutParent()) {
                auto& pb = parent->layoutBox();
                pbx = offsetX - pb.padding.left - pb.border.left;
                pby = offsetY - pb.padding.top - pb.border.top;
                pbw = pb.fullWidth();
                pbh = pb.fullHeight();
                perspStyle = &parent->computedStyle();
            }
            bool is3D = false;
            auto m4 = buildElementTransform4x4(style, bx, by, bw, bh,
                                               persp, pbx, pby, pbw, pbh,
                                               perspStyle, is3D);
            if (!m4.isIdentity()) {
                hasTransform = true;
                renderer_->save();
                if (is3D) {
                    renderer_->concat4x4(m4.m);
                } else {
                    auto m2 = m4.to2D();
                    renderer_->concat(m2.a, m2.b, m2.c, m2.d, m2.e, m2.f);
                }
            }
        } else if (hasT) {
            auto mat = htmlayout::css::parseTransform(trIt->second, bw, bh);
            if (!mat.isIdentity()) {
                hasTransform = true;
                float ox, oy;
                auto toIt = style.find("transform-origin");
                std::string_view originVal =
                    (toIt != style.end()) ? std::string_view(toIt->second)
                                          : std::string_view();
                htmlayout::css::parseTransformOrigin(originVal, bw, bh, ox, oy);
                // Apply: translate to origin, concat matrix, translate back
                renderer_->save();
                renderer_->translate(bx + ox, by + oy);
                renderer_->concat(mat.a, mat.b, mat.c, mat.d, mat.e, mat.f);
                renderer_->translate(-(bx + ox), -(by + oy));
            }
        }
    }

    // Opacity: wrap entire element in a layer
    bool hasOpacity = false;
    if (!skipWrap) {
        auto opIt = style.find("opacity");
        if (opIt != style.end()) {
            float opacity = std::clamp(std::strtof(opIt->second.c_str(), nullptr), 0.0f, 1.0f);
            if (opacity < 1.0f) {
                hasOpacity = true;
                renderer_->saveLayerAlpha(static_cast<uint8_t>(opacity * 255));
            }
        }
    }

    // CSS filter: wrap element drawing in a filter layer
    bool hasFilter = false;
    if (!skipWrap) {
        auto fIt = style.find("filter");
        if (fIt != style.end() && !fIt->second.empty() && fIt->second != "none") {
            auto filters = parseCSSFilter(fIt->second);
            if (!filters.empty()) {
                hasFilter = true;
                // Use a generous bounds that includes blur/shadow overflow
                renderer_->saveLayerWithFilter(filters,
                    bx - 50, by - 50, bw + 100, bh + 100);
            }
        }
    }

    // CSS clip-path: clip the entire element (background, border, content,
    // descendants) to the specified shape. Restored after children paint.
    bool hasClipPath = false;
    auto cpIt = style.find("clip-path");
    if (cpIt != style.end() && !cpIt->second.empty() && cpIt->second != "none") {
        auto pts = parseClipPathPolygon(cpIt->second, bw, bh);
        if (!pts.empty()) {
            for (auto& pt : pts) { pt.x += bx; pt.y += by; }
            hasClipPath = true;
            renderer_->save();
            renderer_->setClipPolygon(pts);
        }
    }

    if (visible) {
        // Box shadows. CSS paint order:
        //   1. outset shadows (drawn before background, behind the element)
        //   2. background
        //   3. inset shadows (drawn over background, under content/border)
        // Within outset/inset groups, the first shadow in the list paints on
        // top of later ones, so we draw in reverse list order.
        auto bsIt = style.find("box-shadow");
        std::vector<std::string> shadows;
        render::Radii shadowRadii = {{0, 0, 0, 0}, {0, 0, 0, 0}};
        bool hasShadows = (bsIt != style.end() && !bsIt->second.empty() && bsIt->second != "none");
        if (hasShadows) {
            shadowRadii = getRadii(style, bw, bh);

            // Split on commas, respecting parentheses (for rgb()/rgba())
            const auto& full = bsIt->second;
            int depth = 0;
            size_t start = 0;
            for (size_t i = 0; i <= full.size(); ++i) {
                if (i < full.size() && full[i] == '(') ++depth;
                else if (i < full.size() && full[i] == ')') --depth;
                else if ((i == full.size() || full[i] == ',') && depth <= 0) {
                    std::string s = full.substr(start, i - start);
                    size_t a = s.find_first_not_of(" \t");
                    size_t b = s.find_last_not_of(" \t");
                    if (a != std::string::npos)
                        shadows.push_back(s.substr(a, b - a + 1));
                    start = i + 1;
                }
            }
        }

        auto drawShadows = [&](bool wantInset) {
            for (int si = static_cast<int>(shadows.size()) - 1; si >= 0; --si) {
                std::string val = shadows[si];
                bool inset = false;
                auto ipos = val.find("inset");
                if (ipos != std::string::npos) {
                    inset = true;
                    val.erase(ipos, 5);
                }
                if (inset != wantInset) continue;
                std::istringstream iss(val);
                std::vector<float> nums;
                std::string colorStr;
                std::string token;
                while (iss >> token) {
                    char* end = nullptr;
                    float v = std::strtof(token.c_str(), &end);
                    if (end != token.c_str() && (*end == '\0' || *end == 'p'))
                        nums.push_back(v);
                    else {
                        if (!colorStr.empty()) colorStr += ' ';
                        colorStr += token;
                    }
                }
                if (nums.size() >= 2) {
                    float sdx = nums[0], sdy = nums[1];
                    float sblur = nums.size() >= 3 ? nums[2] : 0;
                    float sspread = nums.size() >= 4 ? nums[3] : 0;
                    bromath::Color sc = cfromColor8({0, 0, 0, 80});
                    if (!colorStr.empty()) tryParseColor(colorStr, sc);
                    renderer_->drawBoxShadowRadii(bx, by, bw, bh, shadowRadii,
                                            sdx, sdy, sblur, sspread, sc, inset);
                }
            }
        };

        // Outset shadows first (behind background).
        if (hasShadows) drawShadows(false);

        // For html/body elements, background covers the entire viewport (CSS2.1 spec).
        // viewportTop_ offsets for engine-reserved insets (e.g. menu bar).
        std::string tag = elem->tagName();
        if ((tag == "html" || tag == "HTML" || tag == "body" || tag == "BODY") &&
            viewportW_ > 0 && viewportH_ > 0) {
            drawBackground(elem, 0, static_cast<float>(viewportTop_),
                           static_cast<float>(viewportW_), static_cast<float>(viewportH_));
        } else {
            // A fieldset's background starts at its painted border-box top
            // (the legend's vertical center), not the layout box top.
            float fsShift = fieldsetTopShift(elem, bx, by);
            drawBackground(elem, bx, by + fsShift, bw, bh - fsShift);
        }

        // Inset shadows after background (so they're visible on top of it).
        if (hasShadows) drawShadows(true);

        // Draw borders (skipped here for border-collapse tables — they
        // repaint after children so the table border wins on the gridline)
        if (!isCollapsedTable(elem)) {
            drawBorders(elem, bx, by, bw, bh);
        }

        // Draw outline (outside the border box)
        auto olwIt = style.find("outline-width");
        auto olsIt = style.find("outline-style");
        if (olwIt != style.end() && olsIt != style.end() && olsIt->second != "none") {
            float olw = parseLengthPx(olwIt->second);
            if (olw > 0) {
                bromath::Color olc = cfromColor8({0, 0, 0, 255});
                auto olcIt = style.find("outline-color");
                if (olcIt != style.end()) tryParseColor(olcIt->second, olc);
                float olOff = 0;
                auto oloIt = style.find("outline-offset");
                if (oloIt != style.end()) olOff = parseLengthPx(oloIt->second);
                float ox = bx - olw - olOff;
                float oy = by - olw - olOff;
                float ow = bw + 2 * (olw + olOff);
                float oh = bh + 2 * (olw + olOff);
                // Top, Bottom, Left, Right as filled rects
                renderer_->fillRect(ox, oy, ow, olw, olc);
                renderer_->fillRect(ox, oy + oh - olw, ow, olw, olc);
                renderer_->fillRect(ox, oy + olw, olw, oh - 2*olw, olc);
                renderer_->fillRect(ox + ow - olw, oy + olw, olw, oh - 2*olw, olc);
            }
        }

        // Column rules for multicol containers: one rule centered in each
        // column gap, spanning the content height. Geometry mirrors the
        // layout's column computation (block.cpp multicol path) so rules land
        // exactly between the laid-out columns. All rule styles render solid.
        {
            auto ccIt = style.find("column-count");
            auto cwIt = style.find("column-width");
            bool hasCount = ccIt != style.end() && !ccIt->second.empty() &&
                            ccIt->second != "auto";
            bool hasWidth = cwIt != style.end() && !cwIt->second.empty() &&
                            cwIt->second != "auto";
            auto crsIt = style.find("column-rule-style");
            if ((hasCount || hasWidth) && crsIt != style.end() &&
                crsIt->second != "none" && crsIt->second != "hidden" &&
                w > 0 && h > 0) {
                float rw = 3.0f;  // medium
                auto crwIt = style.find("column-rule-width");
                if (crwIt != style.end()) {
                    const std::string& v = crwIt->second;
                    if (v == "thin") rw = 1.0f;
                    else if (v == "medium") rw = 3.0f;
                    else if (v == "thick") rw = 5.0f;
                    else rw = parseLengthPx(v);
                }
                if (rw > 0) {
                    // column-rule-color defaults to currentColor.
                    bromath::Color rc = cfromColor8({0, 0, 0, 255});
                    auto crcIt = style.find("column-rule-color");
                    if (crcIt == style.end()) crcIt = style.find("color");
                    if (crcIt != style.end()) tryParseColor(crcIt->second, rc);

                    float gap = 0.0f;
                    auto cgIt = style.find("column-gap");
                    if (cgIt != style.end() && !cgIt->second.empty() &&
                        cgIt->second != "normal") {
                        gap = parseLengthPx(cgIt->second, w);
                    }
                    int count = 1;
                    if (hasCount) {
                        count = std::max(1, std::atoi(ccIt->second.c_str()));
                    } else {
                        float colW = parseLengthPx(cwIt->second, w);
                        if (colW > 0) {
                            count = std::max(1, static_cast<int>(
                                (w + gap) / (colW + gap)));
                        }
                    }
                    float colW = (w - gap * (count - 1)) / count;
                    if (colW < 0) colW = 0;
                    for (int i = 1; i < count; ++i) {
                        float gapLeft = x + colW * i + gap * (i - 1);
                        float rx = gapLeft + gap * 0.5f - rw * 0.5f;
                        renderer_->fillRect(rx, y, rw, h, rc);
                    }
                }
            }
        }

        // Draw list marker for display:list-item boxes (<li>, <summary>…).
        // Outside markers hang left of the content box, aligned to the first
        // line's baseline. Inside markers are inline content at the start of
        // the first line: layout reserves their inline size (htmlayout's
        // insideMarkerInlineSize — Blink geometry: symbol box + 1em margin
        // for symbolic bullets, text + one space for ordinals) and the
        // painter fills the reserved box here.
        {
            auto dispIt = style.find("display");
            auto lstIt = style.find("list-style-type");
            std::string listType = (lstIt != style.end()) ? lstIt->second : "disc";
            auto lspIt = style.find("list-style-position");
            bool outside = (lspIt == style.end() || lspIt->second != "inside");
            if (dispIt != style.end() && dispIt->second == "list-item" &&
                listType != "none") {
                bromath::Color mc = cfromColor8({0, 0, 0, 255});
                auto mcIt = style.find("color");
                if (mcIt != style.end()) tryParseColor(mcIt->second, mc);

                render::FontRef font = getFontRef(elem);
                // First-line baseline: content top + font ascent (half-leading
                // at UA line heights is sub-pixel; close enough for markers).
                auto am = renderer_->measureText("0", font);
                float baselineY = by + am.ascent;
                float gap = 7.0f;   // Blink's marker padding

                if (listType == "disclosure-open" ||
                    listType == "disclosure-closed") {
                    // <summary> disclosure triangle. Blink: symbol box is
                    // 0.66em (DisclosureSymbolSize) with a 0.4em end margin;
                    // closed points right, open points down.
                    float fs = 16.0f;
                    auto fsIt = style.find("font-size");
                    if (fsIt != style.end()) {
                        float v = parseLengthPx(fsIt->second);
                        if (v > 0) fs = v;
                    }
                    float s = 0.66f * fs;
                    float x0 = outside ? (bx - 0.4f * fs - s) : bx;
                    float top = baselineY - am.ascent * 0.35f - s * 0.5f;
                    bromath::Color none = cfromColor8({0, 0, 0, 0});
                    if (listType == "disclosure-closed") {
                        render::PointF pts[3] = {
                            {x0, top}, {x0 + s, top + s * 0.5f}, {x0, top + s}};
                        renderer_->drawPolygon(pts, mc, none, 0);
                    } else {
                        render::PointF pts[3] = {
                            {x0, top}, {x0 + s, top}, {x0 + s * 0.5f, top + s}};
                        renderer_->drawPolygon(pts, mc, none, 0);
                    }
                } else if (listType == "disc" || listType == "circle" ||
                    listType == "square") {
                    // Bullet centered on roughly half the x-height above the
                    // baseline. Outside: right edge gap px left of the content
                    // box. Inside: at the content-box left, within the space
                    // layout reserved.
                    float r = 3.0f;
                    float cy = baselineY - am.ascent * 0.30f;
                    float cx = outside ? (bx - gap - r) : (bx + r);
                    if (listType == "disc") {
                        renderer_->drawCircle(cx, cy, r, mc, mc, 0);
                    } else if (listType == "circle") {
                        bromath::Color none = cfromColor8({0, 0, 0, 0});
                        renderer_->drawCircle(cx, cy, r, none, mc, 1.0f);
                    } else {
                        renderer_->fillRect(cx - r, cy - r, 2 * r, 2 * r, mc);
                    }
                } else {
                    // Ordinal marker: position among list-item siblings,
                    // honoring <ol start> and <li value>.
                    auto isListItem = [](dom::Node* n) {
                        if (n->nodeType() != dom::NodeType::Element) return false;
                        auto* e = static_cast<dom::Element*>(n);
                        auto& st = e->computedStyle();
                        auto dIt = st.find("display");
                        return dIt != st.end() && dIt->second == "list-item";
                    };
                    int idx = 1;
                    auto* parent = elem->parentNode();
                    if (parent && parent->nodeType() == dom::NodeType::Element) {
                        auto* pe = static_cast<dom::Element*>(parent);
                        const std::string& startAttr = pe->getAttribute("start");
                        if (!startAttr.empty()) idx = std::atoi(startAttr.c_str());
                        for (auto* sib : parent->childNodes()) {
                            if (!isListItem(sib)) continue;
                            auto* se = static_cast<dom::Element*>(sib);
                            const std::string& valAttr = se->getAttribute("value");
                            if (!valAttr.empty()) idx = std::atoi(valAttr.c_str());
                            if (sib == elem) break;
                            ++idx;
                        }
                    }

                    auto toAlpha = [](int n) {
                        std::string s;
                        while (n > 0) {
                            int rem = (n - 1) % 26;
                            s.insert(s.begin(), static_cast<char>('a' + rem));
                            n = (n - 1) / 26;
                        }
                        return s.empty() ? std::string("a") : s;
                    };
                    auto toRoman = [](int n) {
                        if (n <= 0 || n >= 4000) return std::to_string(n);
                        static const int vals[] = {1000, 900, 500, 400, 100, 90,
                                                   50, 40, 10, 9, 5, 4, 1};
                        static const char* syms[] = {"m", "cm", "d", "cd", "c",
                                                     "xc", "l", "xl", "x", "ix",
                                                     "v", "iv", "i"};
                        std::string s;
                        for (int i = 0; i < 13; ++i)
                            while (n >= vals[i]) { s += syms[i]; n -= vals[i]; }
                        return s;
                    };
                    auto toUpper = [](std::string s) {
                        for (auto& ch : s)
                            ch = static_cast<char>(std::toupper(
                                static_cast<unsigned char>(ch)));
                        return s;
                    };

                    std::string text;
                    if (listType == "decimal") {
                        text = std::to_string(idx);
                    } else if (listType == "decimal-leading-zero") {
                        text = (idx >= 0 && idx < 10 ? "0" : "") + std::to_string(idx);
                    } else if (listType == "lower-alpha" || listType == "lower-latin") {
                        text = toAlpha(idx);
                    } else if (listType == "upper-alpha" || listType == "upper-latin") {
                        text = toUpper(toAlpha(idx));
                    } else if (listType == "lower-roman") {
                        text = toRoman(idx);
                    } else if (listType == "upper-roman") {
                        text = toUpper(toRoman(idx));
                    } else {
                        text = std::to_string(idx);
                    }
                    text += ".";
                    auto tm = renderer_->measureText(text, font);
                    float tx = outside ? (bx - gap - tm.width) : bx;
                    renderer_->drawText(text, tx, baselineY, font, mc);
                }
            }
        }
    }

    // Check for overflow clipping. Per CSS spec, when one axis is non-visible
    // and the other is visible, the visible axis is treated as auto — i.e. both
    // axes effectively clip. So clip if EITHER axis is non-visible.
    bool needsClip = false;
    std::string overflowY = getOverflowY(style);
    std::string overflowX = getOverflowX(style);
    auto axisClips = [](const std::string& v) {
        return v == "hidden" || v == "scroll" || v == "auto" || v == "clip";
    };
    if (axisClips(overflowX) || axisClips(overflowY)) {
        needsClip = true;
        renderer_->save();
        render::Radii clipRadii = getRadii(style, bw, bh);
        if (!clipRadii.isZero())
            renderer_->setClipRRect(bx, by, bw, bh, clipRadii);
        else
            renderer_->setClip(bx, by, bw, bh);
        // Track the rect so a canvas/WebGL layer break in this subtree can
        // report the clip the compositor must scissor to (rounded corners are
        // approximated by their bounding box).
        pushClipRect(bx, by, bw, bh);
    }

    // SVG elements render their own children via the SVG pipeline — skip DOM traversal
    if (elem->svgControl()) {
        // Draw the SVG control, then return (no child traversal)
        if (visible) {
            elem->svgControl()->draw(renderer_, elem, box, offsetX, offsetY);
        }
        if (needsClip) {
            renderer_->restore();
            popClipRect();
        }
        if (hasClipPath) renderer_->restore();
        if (hasFilter) renderer_->restore();
        if (hasOpacity) renderer_->restore();
        if (hasTransform) renderer_->restore();
        return;
    }

    // Canvas/WebGL/SceneGraph elements: trigger a layer break so the compositor
    // can interleave canvas textures with HTML layers in document order.
    // SceneGraph check must come first since scene elements also have a canvasScene.
    // Active overflow/scroll clip for any layer-break quad in this subtree.
    // The compositor draws canvas/WebGL layers as standalone quads outside the
    // Skia clip stack, so we hand it the clip explicitly. clipW < 0 ⇒ none.
    float lbCX = 0, lbCY = 0, lbCW = -1, lbCH = -1;
    bool haveLBClip = currentClipRect(lbCX, lbCY, lbCW, lbCH);
    if (!haveLBClip) { lbCW = -1; lbCH = -1; }
    // Canvas/WebGL/scene content isn't drawn through renderer_ (it's a
    // separately-composited GL texture quad), so it doesn't pick up the CTM
    // concat above — project through the element's own ancestor-transform
    // chain explicitly, the same math getBoundingClientRect() uses, so a
    // zoomed/panned ancestor (CSS transform: translate/scale) positions the
    // quad correctly instead of leaving it at its untransformed layout rect.
    // absoluteContentBox() is document-space; add the pass's root offset so
    // the quad lands in the same surface space the HTML painted at (content
    // space for the app document — the compositor shifts HTML surface and
    // quad together by the engine inset, so they can never drift apart).
    // Without the root offset, the quad would ignore document scroll.
    if ((elem->sceneGraph() || elem->canvasScene() || elem->webglContext() || elem->iframeDoc()) && visible) {
        auto lbRect = dom::absoluteContentBox(elem);
        x = lbRect.x + rootOffsetX_;
        y = lbRect.y + rootOffsetY_;
        w = lbRect.width;
        h = lbRect.height;
    }
    if (elem->sceneGraph() && visible) {
        // 3D mesh FBO layer (texture ID stored on element by scene graph render)
        unsigned int fboTex = elem->sceneGraphFBOTexture();
        if (fboTex && layerBreakCb_) {
            layerBreakCb_(nullptr, fboTex, x, y, w, h, lbCX, lbCY, lbCW, lbCH);
        }
        // 2D canvas layer (for ShapeNode/SpriteNode content)
        if (elem->canvasScene() && layerBreakCb_) {
            auto* scene = static_cast<canvas::CanvasScene*>(elem->canvasScene());
            layerBreakCb_(scene, 0, x, y, w, h, lbCX, lbCY, lbCW, lbCH);
        }
        if (needsClip) { renderer_->restore(); popClipRect(); }
        if (hasClipPath) renderer_->restore();
        if (hasFilter) renderer_->restore();
        if (hasOpacity) renderer_->restore();
        if (hasTransform) renderer_->restore();
        return;
    }
    if (elem->canvasScene() && visible) {
        auto* scene = static_cast<canvas::CanvasScene*>(elem->canvasScene());
        if (layerBreakCb_) {
            layerBreakCb_(scene, 0, x, y, w, h, lbCX, lbCY, lbCW, lbCH);
        }
        if (needsClip) { renderer_->restore(); popClipRect(); }
        if (hasClipPath) renderer_->restore();
        if (hasFilter) renderer_->restore();
        if (hasOpacity) renderer_->restore();
        if (hasTransform) renderer_->restore();
        return;
    }
    if (elem->webglContext() && visible) {
        auto* webglCtx = static_cast<webgl::WebGL2RenderingContext*>(elem->webglContext());
        if (layerBreakCb_) {
            layerBreakCb_(nullptr, webglCtx->colorTexture(), x, y, w, h,
                          lbCX, lbCY, lbCW, lbCH);
        }
        if (needsClip) { renderer_->restore(); popClipRect(); }
        if (hasClipPath) renderer_->restore();
        if (hasFilter) renderer_->restore();
        if (hasOpacity) renderer_->restore();
        if (hasTransform) renderer_->restore();
        return;
    }
    if (elem->iframeDoc() && visible) {
        // The sub-document renders to its own texture (engine iframe pass); the
        // engine records a break carrying the IframeDoc id and composites that
        // texture at this element's content box.
        if (iframeLayerBreakCb_) {
            iframeLayerBreakCb_(elem->iframeDoc(), x, y, w, h, lbCX, lbCY, lbCW, lbCH);
        }
        if (needsClip) { renderer_->restore(); popClipRect(); }
        if (hasClipPath) renderer_->restore();
        if (hasFilter) renderer_->restore();
        if (hasOpacity) renderer_->restore();
        if (hasTransform) renderer_->restore();
        return;
    }

    // Children's offset is the parent's absolute content position
    // (so child positions, which are relative to parent content area, become absolute)
    float childOffsetX = x;
    // Clamp scrollTop to valid range — JS may have set it before layout updated
    float maxST = std::max(0.0f, box.naturalHeight - box.contentRect.height);
    float scrollTop = std::clamp(elem->scrollTopValue(), 0.0f, maxST);
    float childOffsetY = y - scrollTop;

    // ::before pseudo content (drawn before children)
    if (visible) drawPseudo(elem, "before", childOffsetX, childOffsetY);

    // Draw composed children (shadow DOM + slot replacement).
    // A <textarea> renders its value through ElTextarea (below); its DOM text
    // children are the *source* of that value, not separate flow content.
    // Painting them here too double-draws the text — once in the element's
    // computed color via this walk, once again by the control — which reads
    // as the placeholder/value "ghosting" behind the real text.
    if (!elem->textareaControl()) {
        // CSS2.1 Appendix E: non-positioned floats paint above the
        // backgrounds/borders of in-flow block-level siblings. Defer floated
        // children to a second pass so a later sibling's background can't
        // cover a float that precedes it in the DOM.
        std::vector<dom::Node*> floatedChildren;
        for (auto* child : elem->composedChildNodes()) {
            if (child && child->nodeType() == dom::NodeType::Element) {
                auto& cs = static_cast<dom::Element*>(child)->computedStyle();
                auto fIt = cs.find("float");
                if (fIt != cs.end() && fIt->second != "none" &&
                    !fIt->second.empty()) {
                    floatedChildren.push_back(child);
                    continue;
                }
            }
            drawNode(child, childOffsetX, childOffsetY);
        }
        for (auto* child : floatedChildren)
            drawNode(child, childOffsetX, childOffsetY);
    }

    // ::after pseudo content (drawn after children)
    if (visible) drawPseudo(elem, "after", childOffsetX, childOffsetY);

    // Draw replaced element content (input, textarea, select, svg)
    if (visible) {
        auto* inputCtrl = elem->inputControl();
        if (inputCtrl) {
            inputCtrl->draw(renderer_, box, style, offsetX, offsetY,
                            rootOffsetX_, rootOffsetY_);
        }
        auto* textareaCtrl = elem->textareaControl();
        if (textareaCtrl) {
            textareaCtrl->draw(renderer_, box, style, offsetX, offsetY,
                               rootOffsetX_, rootOffsetY_);
        }
        auto* selectCtrl = elem->selectControl();
        if (selectCtrl) {
            selectCtrl->draw(renderer_, box, style, offsetX, offsetY,
                             rootOffsetX_, rootOffsetY_);
        }
        auto* videoCtrl = elem->videoControl();
        if (videoCtrl) {
            videoCtrl->draw(renderer_, elem, box, offsetX, offsetY);
        }
        // <img> replaced content. Layout already sized the box via
        // intrinsicSize() in layout_node_adapter; here we paint the raster
        // bytes (or SVG markup) into the content rect.
        const std::string& tag = elem->tagName();
        if (tag == "img" || tag == "IMG") {
            std::string src = elem->getAttribute("src");
            if (!src.empty()) {
                loadImage(src, basePath_);
                auto it = imageCache_.find(src);
                if (it != imageCache_.end() && !it->second.data.empty()) {
                    float ix = box.contentRect.x + offsetX;
                    float iy = box.contentRect.y + offsetY;
                    float iw = box.contentRect.width;
                    float ih = box.contentRect.height;
                    if (iw > 0 && ih > 0) {
                        // CSS object-fit / object-position. The default (fill)
                        // stretches the image to the content box. cover/contain/
                        // none/scale-down preserve the intrinsic aspect ratio and
                        // position the result via object-position (default
                        // center). cover/none can overflow the content box, so we
                        // clip to it.
                        float dx = ix, dy = iy, dw = iw, dh = ih;
                        float imgW = static_cast<float>(it->second.width);
                        float imgH = static_cast<float>(it->second.height);
                        std::string fit = "fill";
                        if (auto ofIt = style.find("object-fit"); ofIt != style.end() && !ofIt->second.empty())
                            fit = ofIt->second;
                        bool needClip = false;
                        if (fit != "fill" && imgW > 0 && imgH > 0) {
                            float fitScale = 1.0f;
                            if (fit == "contain") {
                                fitScale = std::min(iw / imgW, ih / imgH);
                            } else if (fit == "cover") {
                                fitScale = std::max(iw / imgW, ih / imgH);
                            } else if (fit == "none") {
                                fitScale = 1.0f;
                            } else if (fit == "scale-down") {
                                fitScale = std::min(1.0f, std::min(iw / imgW, ih / imgH));
                            }
                            dw = imgW * fitScale;
                            dh = imgH * fitScale;

                            // object-position: place the scaled box within the
                            // content box. Default "50% 50%". Each axis fraction
                            // f maps the f-point of the image to the f-point of
                            // the box: offset = (boxSize - drawSize) * f.
                            float fx = 0.5f, fy = 0.5f;
                            if (auto opIt = style.find("object-position");
                                opIt != style.end() && !opIt->second.empty()) {
                                std::istringstream iss(opIt->second);
                                std::string t1, t2;
                                iss >> t1; iss >> t2;
                                auto axisFrac = [](const std::string& tok, bool isX, float def) -> float {
                                    if (tok.empty()) return def;
                                    if (tok == "left")   return isX ? 0.0f : def;
                                    if (tok == "right")  return isX ? 1.0f : def;
                                    if (tok == "top")    return isX ? def : 0.0f;
                                    if (tok == "bottom") return isX ? def : 1.0f;
                                    if (tok == "center") return 0.5f;
                                    if (tok.back() == '%')
                                        return std::strtof(tok.c_str(), nullptr) / 100.0f;
                                    return def;  // px offsets unsupported → default
                                };
                                fx = axisFrac(t1, true, 0.5f);
                                fy = axisFrac(t2.empty() ? t1 : t2, false, 0.5f);
                            }
                            dx = ix + (iw - dw) * fx;
                            dy = iy + (ih - dh) * fy;
                            needClip = (dw > iw + 0.5f) || (dh > ih + 0.5f) ||
                                       dx < ix - 0.5f || dy < iy - 0.5f;
                        }

                        if (needClip) { renderer_->save(); renderer_->setClip(ix, iy, iw, ih); }
                        if (it->second.isSvg) {
                            // Recorded; replayer re-parses and draws the SVG markup at the fitted rect.
                            renderer_->drawSvgMarkup(
                                reinterpret_cast<const char*>(it->second.data.data()),
                                it->second.data.size(),
                                dx, dy, dw, dh);
                        } else {
                            renderer_->drawImage(it->second.data.data(),
                                                 it->second.data.size(),
                                                 dx, dy, dw, dh,
                                                 it->second.id);
                        }
                        if (needClip) renderer_->restore();
                    }
                }
            }
        }
    }

    // For tables in border-collapse mode, repaint the table's outer border
    // AFTER cells so it wins on the gridline (per CSS 17.6.2 paint order:
    // cells, then rows, then row-groups, then cols, then col-groups, then
    // table — last in the list paints on top). drawBorders() handles the
    // collapsed-mode centering when isCollapsedTable() is true.
    if (visible && isCollapsedTable(elem)) {
        drawBorders(elem, bx, by, bw, bh);
    }

    if (needsClip) {
        renderer_->restore();
        popClipRect();
    }
    if (hasClipPath) renderer_->restore();
    if (hasFilter) renderer_->restore();
    if (hasOpacity) renderer_->restore();
    if (hasTransform) renderer_->restore();
}

void DrawTraversal::drawBackground(dom::Element* elem, float x, float y, float w, float h) {
    auto& style = elem->computedStyle();
    render::Radii radii = getRadii(style, w, h);
    bool rounded = !radii.isZero();

    // Background color
    auto bgIt = style.find("background-color");
    if (bgIt != style.end() && !bgIt->second.empty()) {
        bromath::Color c;
        if (tryParseColor(bgIt->second, c) && c.a > 0) {
            if (rounded)
                renderer_->fillRoundRectRadii(x, y, w, h, radii, c);
            else
                renderer_->fillRect(x, y, w, h, c);
        }
    }

    // For background image / gradient, clip to rounded bounds. Save the
    // canvas state and pop after drawing the image/gradient.
    bool clipped = false;
    auto imgItCheck = style.find("background-image");
    if (rounded && imgItCheck != style.end() && !imgItCheck->second.empty() &&
        imgItCheck->second != "none") {
        renderer_->save();
        renderer_->setClipRRect(x, y, w, h, radii);
        clipped = true;
    }

    // Helper: split CSS multi-value at top-level commas (paren/quote-aware).
    auto splitLayers = [](const std::string& v) {
        std::vector<std::string> out;
        std::string cur;
        int depth = 0;
        bool inQ = false;
        char qc = 0;
        for (char c : v) {
            if (inQ) { cur += c; if (c == qc) inQ = false; continue; }
            if (c == '"' || c == '\'') { inQ = true; qc = c; cur += c; continue; }
            if (c == '(') { ++depth; cur += c; continue; }
            if (c == ')') { --depth; cur += c; continue; }
            if (c == ',' && depth == 0) {
                while (!cur.empty() && std::isspace(static_cast<unsigned char>(cur.front()))) cur.erase(cur.begin());
                while (!cur.empty() && std::isspace(static_cast<unsigned char>(cur.back()))) cur.pop_back();
                out.push_back(cur);
                cur.clear();
                continue;
            }
            cur += c;
        }
        while (!cur.empty() && std::isspace(static_cast<unsigned char>(cur.front()))) cur.erase(cur.begin());
        while (!cur.empty() && std::isspace(static_cast<unsigned char>(cur.back()))) cur.pop_back();
        if (!cur.empty() || !out.empty()) out.push_back(cur);
        return out;
    };

    // Build per-layer values for each background sub-property.
    auto getLayerProp = [&](const char* name, const std::string& fallback) {
        auto it = style.find(name);
        if (it == style.end() || it->second.empty()) return std::vector<std::string>{fallback};
        auto v = splitLayers(it->second);
        if (v.empty()) v.push_back(fallback);
        return v;
    };

    auto imgIt = style.find("background-image");
    if (imgIt != style.end() && !imgIt->second.empty() && imgIt->second != "none") {
        auto images   = splitLayers(imgIt->second);
        auto positions= getLayerProp("background-position", "0% 0%");
        auto sizes    = getLayerProp("background-size", "auto");
        auto repeats  = getLayerProp("background-repeat", "repeat");
        auto blends   = getLayerProp("background-blend-mode", "normal");

        // Paint layers from last to first (CSS: first listed is on top).
        for (size_t li = images.size(); li-- > 0; ) {
            const std::string& val = images[li];
            if (val.empty() || val == "none") continue;
            const std::string layerPosition = li < positions.size() ? positions[li] : positions.back();
            const std::string layerSize     = li < sizes.size()     ? sizes[li]     : sizes.back();
            const std::string layerRepeat   = li < repeats.size()   ? repeats[li]   : repeats.back();

            // background-blend-mode: composite this layer against the layers
            // (and background color) already painted beneath it. A non-normal
            // mode wraps the layer's paint in a blended group.
            render::BlendMode layerBlend = parseBlendMode(
                li < blends.size() ? blends[li] : blends.back());
            bool layerBlended = (layerBlend != render::BlendMode::Normal);
            if (layerBlended) renderer_->saveLayerWithBlend(layerBlend);

        if (val.substr(0, 4) == "url(") {
            // Extract URL
            size_t start = val.find('(') + 1;
            size_t end = val.rfind(')');
            if (end > start) {
                std::string url = val.substr(start, end - start);
                if (!url.empty() && (url.front() == '"' || url.front() == '\'')) {
                    url = url.substr(1, url.size() - 2);
                }
                loadImage(url, basePath_);
                auto imgCacheIt = imageCache_.find(url);
                if (imgCacheIt != imageCache_.end() && !imgCacheIt->second.data.empty()) {
                    float imgW = static_cast<float>(imgCacheIt->second.width);
                    float imgH = static_cast<float>(imgCacheIt->second.height);
                    float drawW = imgW > 0 ? imgW : w;
                    float drawH = imgH > 0 ? imgH : h;

                    // background-size
                    {
                        const auto& bs = layerSize;
                        if (bs == "cover" && imgW > 0 && imgH > 0) {
                            float scale = std::max(w / imgW, h / imgH);
                            drawW = imgW * scale; drawH = imgH * scale;
                        } else if (bs == "contain" && imgW > 0 && imgH > 0) {
                            float scale = std::min(w / imgW, h / imgH);
                            drawW = imgW * scale; drawH = imgH * scale;
                        } else if (bs != "auto") {
                            // Parse "Wpx Hpx" or "W% H%"
                            std::istringstream bsIss(bs);
                            std::string wStr, hStr;
                            bsIss >> wStr;
                            bsIss >> hStr;
                            if (!wStr.empty() && wStr != "auto")
                                drawW = parseLengthPx(wStr, w);
                            if (!hStr.empty() && hStr != "auto")
                                drawH = parseLengthPx(hStr, h);
                            else if (imgW > 0 && imgH > 0 && !wStr.empty() && wStr != "auto")
                                drawH = drawW * imgH / imgW; // maintain aspect ratio
                        }
                    }

                    // background-position
                    float posX = x, posY = y;
                    if (!layerPosition.empty()) {
                        const auto& bp = layerPosition;
                        if (bp == "center") {
                            posX = x + (w - drawW) / 2;
                            posY = y + (h - drawH) / 2;
                        } else if (bp == "right") {
                            posX = x + w - drawW;
                        } else if (bp == "bottom") {
                            posY = y + h - drawH;
                        } else {
                            std::istringstream bpIss(bp);
                            std::string pxStr, pyStr;
                            bpIss >> pxStr;
                            bpIss >> pyStr;
                            if (!pxStr.empty()) posX = x + parseLengthPx(pxStr, w);
                            if (!pyStr.empty()) posY = y + parseLengthPx(pyStr, h);
                        }
                    }

                    // background-repeat
                    std::string repeat = layerRepeat.empty() ? "repeat" : layerRepeat;

                    if (repeat == "no-repeat") {
                        // Clip to the box so an oversized image (e.g.
                        // background-size: cover, or a natural-size image larger
                        // than the element) doesn't bleed past its bounds.
                        renderer_->save();
                        renderer_->setClip(x, y, w, h);
                        renderer_->drawImage(imgCacheIt->second.data.data(),
                                            imgCacheIt->second.data.size(),
                                            posX, posY, drawW, drawH,
                                            imgCacheIt->second.id);
                        renderer_->restore();
                    } else {
                        // Tile the image
                        renderer_->save();
                        renderer_->setClip(x, y, w, h);
                        bool repeatX = (repeat == "repeat" || repeat == "repeat-x");
                        bool repeatY = (repeat == "repeat" || repeat == "repeat-y");
                        float startX = repeatX ? x - std::fmod(posX - x, drawW) - drawW : posX;
                        float startY = repeatY ? y - std::fmod(posY - y, drawH) - drawH : posY;
                        float endX = repeatX ? x + w : startX + drawW;
                        float endY = repeatY ? y + h : startY + drawH;
                        for (float iy = startY; iy < endY; iy += drawH) {
                            for (float ix = startX; ix < endX; ix += drawW) {
                                renderer_->drawImage(imgCacheIt->second.data.data(),
                                                    imgCacheIt->second.data.size(),
                                                    ix, iy, drawW, drawH,
                                                    imgCacheIt->second.id);
                            }
                        }
                        renderer_->restore();
                    }
                }
            }
        }
        else if (val.find("linear-gradient") != std::string::npos ||
                 val.find("radial-gradient") != std::string::npos ||
                 val.find("conic-gradient") != std::string::npos) {
            // Parse gradient color stops from the CSS value
            auto parenStart = val.find('(');
            auto parenEnd = val.rfind(')');
            if (parenStart != std::string::npos && parenEnd != std::string::npos) {
                std::string inner = val.substr(parenStart + 1, parenEnd - parenStart - 1);
                // Split on commas (respecting nested parens)
                std::vector<std::string> parts;
                int depth = 0;
                std::string cur;
                for (char c : inner) {
                    if (c == '(') ++depth;
                    else if (c == ')') --depth;
                    else if (c == ',' && depth == 0) {
                        parts.push_back(cur);
                        cur.clear();
                        continue;
                    }
                    cur += c;
                }
                if (!cur.empty()) parts.push_back(cur);

                // Gradient kind. `repeating-*` variants contain the base name
                // as a substring, so the base flags stay valid for them.
                bool isRadial = (val.find("radial-gradient") != std::string::npos);
                bool isConic  = (val.find("conic-gradient")  != std::string::npos);
                bool isRepeating = (val.find("repeating-") != std::string::npos);

                // Parse direction/angle for linear-gradient, shape/extent/
                // position prefix for radial-gradient, or "from <angle>" for
                // conic-gradient.
                float angleDeg = 180;    // linear default: to bottom
                float conicFromDeg = 0;  // conic default: 0deg (12 o'clock)
                size_t colorStart = 0;

                // Radial-gradient defaults: ellipse, farthest-corner, center.
                bool radialIsCircle = false;
                enum RadExtent { RAD_FARTHEST_CORNER, RAD_FARTHEST_SIDE,
                                 RAD_CLOSEST_CORNER,  RAD_CLOSEST_SIDE };
                RadExtent radExtent = RAD_FARTHEST_CORNER;
                float radCxFrac = 0.5f, radCyFrac = 0.5f; // fraction of (w, h)

                if (isRadial && !parts.empty()) {
                    std::string first = parts[0];
                    while (!first.empty() && first.front() == ' ') first.erase(first.begin());
                    while (!first.empty() && first.back() == ' ') first.pop_back();
                    // The prefix (if present) ends before the first color stop.
                    // Heuristic: if the first part contains shape/extent keywords
                    // or starts with "at ", treat it as the prefix.
                    bool looksPrefix =
                        first.find("circle") != std::string::npos ||
                        first.find("ellipse") != std::string::npos ||
                        first.find("at ") != std::string::npos ||
                        first.find("closest") != std::string::npos ||
                        first.find("farthest") != std::string::npos;
                    if (looksPrefix) {
                        if (first.find("circle") != std::string::npos) radialIsCircle = true;
                        if (first.find("closest-side") != std::string::npos) radExtent = RAD_CLOSEST_SIDE;
                        else if (first.find("closest-corner") != std::string::npos) radExtent = RAD_CLOSEST_CORNER;
                        else if (first.find("farthest-side") != std::string::npos) radExtent = RAD_FARTHEST_SIDE;
                        else if (first.find("farthest-corner") != std::string::npos) radExtent = RAD_FARTHEST_CORNER;
                        // Position: "at <x> <y>"
                        auto atPos = first.find("at ");
                        if (atPos != std::string::npos) {
                            std::string posStr = first.substr(atPos + 3);
                            while (!posStr.empty() && posStr.front() == ' ') posStr.erase(posStr.begin());
                            // Tokenize on spaces
                            std::vector<std::string> toks;
                            std::string t;
                            for (char c : posStr) {
                                if (c == ' ') { if (!t.empty()) { toks.push_back(t); t.clear(); } }
                                else t += c;
                            }
                            if (!t.empty()) toks.push_back(t);
                            auto resolveAxis = [](const std::string& tok, bool isX, float& outFrac) {
                                if (tok == "left") { if (isX) outFrac = 0.0f; }
                                else if (tok == "right") { if (isX) outFrac = 1.0f; }
                                else if (tok == "top") { if (!isX) outFrac = 0.0f; }
                                else if (tok == "bottom") { if (!isX) outFrac = 1.0f; }
                                else if (tok == "center") { outFrac = 0.5f; }
                                else if (!tok.empty() && tok.back() == '%') {
                                    outFrac = std::strtof(tok.c_str(), nullptr) / 100.0f;
                                }
                            };
                            if (toks.size() == 1) {
                                resolveAxis(toks[0], true, radCxFrac);
                                resolveAxis(toks[0], false, radCyFrac);
                            } else if (toks.size() >= 2) {
                                resolveAxis(toks[0], true, radCxFrac);
                                resolveAxis(toks[1], false, radCyFrac);
                            }
                        }
                        colorStart = 1;
                    }
                }

                if (!isRadial && !isConic && !parts.empty()) {
                    std::string first = parts[0];
                    // Trim
                    while (!first.empty() && first.front() == ' ') first.erase(first.begin());
                    while (!first.empty() && first.back() == ' ') first.pop_back();
                    if (first.find("to ") == 0) {
                        if (first == "to right") angleDeg = 90;
                        else if (first == "to left") angleDeg = 270;
                        else if (first == "to top") angleDeg = 0;
                        else if (first == "to bottom") angleDeg = 180;
                        else if (first == "to top right" || first == "to right top") angleDeg = 45;
                        else if (first == "to bottom right" || first == "to right bottom") angleDeg = 135;
                        else if (first == "to bottom left" || first == "to left bottom") angleDeg = 225;
                        else if (first == "to top left" || first == "to left top") angleDeg = 315;
                        colorStart = 1;
                    } else {
                        char* end = nullptr;
                        float a = std::strtof(first.c_str(), &end);
                        if (end != first.c_str()) {
                            angleDeg = a;
                            colorStart = 1;
                        }
                    }
                }

                // conic-gradient "from <angle> [at <pos>]" prefix. Only the
                // starting angle is honored; the conic center stays the box
                // center.
                if (isConic && !parts.empty()) {
                    std::string first = parts[0];
                    while (!first.empty() && first.front() == ' ') first.erase(first.begin());
                    while (!first.empty() && first.back() == ' ') first.pop_back();
                    auto fromPos = first.find("from ");
                    if (fromPos == 0 || first.find("at ") == 0) {
                        if (fromPos != std::string::npos) {
                            std::string a = first.substr(fromPos + 5);
                            while (!a.empty() && a.front() == ' ') a.erase(a.begin());
                            char* end = nullptr;
                            float av = std::strtof(a.c_str(), &end);
                            if (end != a.c_str()) {
                                std::string unit(end);
                                if (unit == "rad")  av *= 180.0f / 3.14159265f;
                                else if (unit == "grad") av *= 0.9f;
                                else if (unit == "turn") av *= 360.0f;
                                conicFromDeg = av;
                            }
                        }
                        colorStart = 1;
                    }
                }

                // Reference length for resolving <length> stop positions to a
                // fraction of the gradient line. Linear: |W·sin|+|H·cos| (same
                // metric the draw below uses). Radial: the end radius (extent
                // rule applied to the box), matching the ray the stops live
                // on — using anything else skews px-positioned stops (visible
                // as wrong ring spacing in repeating-radial-gradient). Conic
                // uses angle units instead, so length positions there fall
                // back to auto.
                float refLen = 1.0f;
                if (isRadial) {
                    float rcx = radCxFrac * w, rcy = radCyFrac * h;
                    float csX = std::min(rcx, w - rcx), csY = std::min(rcy, h - rcy);
                    float fsX = std::max(rcx, w - rcx), fsY = std::max(rcy, h - rcy);
                    switch (radExtent) {
                        case RAD_CLOSEST_SIDE:
                            refLen = radialIsCircle ? std::min(csX, csY) : csX; break;
                        case RAD_CLOSEST_CORNER:
                            refLen = radialIsCircle ? std::sqrt(csX*csX + csY*csY)
                                                    : csX * std::sqrt(2.0f); break;
                        case RAD_FARTHEST_SIDE:
                            refLen = radialIsCircle ? std::max(fsX, fsY) : fsX; break;
                        case RAD_FARTHEST_CORNER:
                        default:
                            refLen = radialIsCircle ? std::sqrt(fsX*fsX + fsY*fsY)
                                                    : fsX * std::sqrt(2.0f); break;
                    }
                    if (refLen < 1.0f) refLen = 1.0f;
                } else if (!isConic) {
                    float rad0 = angleDeg * 3.14159265f / 180.0f;
                    refLen = std::abs(w * std::sin(rad0)) + std::abs(h * std::cos(rad0));
                    if (refLen < 1.0f) refLen = 1.0f;
                }

                // Parse color stops. Each part is "<color> [<pos1>] [<pos2>]".
                // The CSS double-position form (e.g. "#hex 0 40%") is shorthand
                // for two stops of the same color, producing a hard band. We
                // strip up to two trailing position tokens off the end (the
                // color may itself contain spaces inside rgb()/hsl()), then emit
                // one stop per position found.
                //
                // A position token resolves to a fraction of the gradient:
                //   %                → v/100
                //   <length> (px)    → v/refLen (linear/radial)
                //   <angle> (deg/…)  → v/360    (conic)
                //   bare 0           → 0
                // Anything else (em, unresolvable) is stripped anyway and the
                // stop falls back to an evenly-spaced auto offset — the key is
                // that the position token never leaks into the color string.
                auto parsePosToken = [&](const std::string& t, float& outFrac) -> bool {
                    if (t.empty()) return false;
                    char* end = nullptr;
                    float v = std::strtof(t.c_str(), &end);
                    if (end == t.c_str()) return false;
                    std::string unit(end);
                    if (unit == "%") { outFrac = v / 100.0f; return true; }
                    if (isConic) {
                        if (unit == "deg")  { outFrac = v / 360.0f; return true; }
                        if (unit == "grad") { outFrac = v / 400.0f; return true; }
                        if (unit == "rad")  { outFrac = v / 6.28318530718f; return true; }
                        if (unit == "turn") { outFrac = v; return true; }
                    } else {
                        if (unit == "px") { outFrac = v / refLen; return true; }
                    }
                    if (unit.empty() && v == 0.0f) { outFrac = 0.0f; return true; }
                    return false;  // unresolvable length/angle → auto
                };
                // A trailing token that begins like a number is a position
                // token and must be peeled off the color regardless of whether
                // we can resolve it to a fraction.
                auto looksLikePos = [](const std::string& t) -> bool {
                    if (t.empty()) return false;
                    char c = t[0];
                    return (c >= '0' && c <= '9') || c == '+' || c == '-' || c == '.';
                };

                std::vector<render::ColorStop> stops;
                size_t numColors = parts.size() - colorStart;
                for (size_t i = colorStart; i < parts.size(); ++i) {
                    std::string part = parts[i];
                    while (!part.empty() && part.front() == ' ') part.erase(part.begin());
                    while (!part.empty() && part.back() == ' ') part.pop_back();

                    // Pull up to two trailing position tokens (kept in order).
                    std::vector<float> positions;
                    for (int n = 0; n < 2; ++n) {
                        size_t sp = part.find_last_of(' ');
                        if (sp == std::string::npos) break;
                        std::string tail = part.substr(sp + 1);
                        if (!looksLikePos(tail)) break;
                        float frac;
                        bool ok = parsePosToken(tail, frac);
                        part.erase(sp);
                        while (!part.empty() && part.back() == ' ') part.pop_back();
                        if (ok) positions.insert(positions.begin(), frac);
                    }

                    bromath::Color sc = cfromColor8({0, 0, 0, 255});
                    tryParseColor(part, sc);

                    if (positions.empty()) {
                        float offset = numColors > 1
                            ? static_cast<float>(i - colorStart) / static_cast<float>(numColors - 1)
                            : 0.0f;
                        stops.push_back({offset, sc});
                    } else {
                        for (float p : positions) stops.push_back({p, sc});
                    }
                }

                // CSS: stop offsets are non-decreasing; clamp each up to the max
                // seen so far (Skia also requires sorted positions).
                {
                    float maxSoFar = 0.0f;
                    bool firstStop = true;
                    for (auto& s : stops) {
                        if (firstStop) { maxSoFar = s.offset; firstStop = false; }
                        else if (s.offset < maxSoFar) s.offset = maxSoFar;
                        else maxSoFar = s.offset;
                    }
                }

                // repeating-*-gradient: tile the resolved stop pattern across
                // [0,1]. The renderer uses a clamp tile mode, so we materialize
                // the repetition as explicit stops here.
                if (isRepeating && stops.size() >= 2) {
                    float firstOff = stops.front().offset;
                    float period = stops.back().offset - firstOff;
                    if (period > 1e-4f && (firstOff > 1e-4f || period < 0.999f)) {
                        std::vector<render::ColorStop> tiled;
                        for (float base = firstOff;
                             base < 1.0f + period && tiled.size() < 8192;
                             base += period) {
                            bool done = false;
                            for (const auto& s : stops) {
                                float off = base + (s.offset - firstOff);
                                if (off < -1e-4f) continue;
                                if (off >= 1.0f) { tiled.push_back({1.0f, s.color}); done = true; break; }
                                tiled.push_back({off, s.color});
                            }
                            if (done) break;
                        }
                        if (tiled.size() >= 2) stops.swap(tiled);
                    }
                }

                if (stops.size() >= 2) {
                    // Compute per-cell box from background-size + position +
                    // repeat. Gradient is drawn into this cell, tiled per
                    // background-repeat.
                    float cellW = w, cellH = h;
                    {
                        const auto& bs = layerSize;
                        if (!bs.empty() && bs != "auto" && bs != "cover" && bs != "contain") {
                            std::istringstream iss(bs);
                            std::string ws, hs;
                            iss >> ws; iss >> hs;
                            if (!ws.empty() && ws != "auto") cellW = parseLengthPx(ws, w);
                            if (!hs.empty() && hs != "auto") cellH = parseLengthPx(hs, h);
                            else if (!ws.empty() && ws != "auto") cellH = cellW;
                        }
                    }
                    if (cellW < 0.5f) cellW = 0.5f;
                    if (cellH < 0.5f) cellH = 0.5f;

                    float originX = x, originY = y;
                    if (!layerPosition.empty() && layerPosition != "0% 0%") {
                        const auto& bp = layerPosition;
                        if (bp == "center") {
                            originX = x + (w - cellW) / 2;
                            originY = y + (h - cellH) / 2;
                        } else {
                            std::istringstream iss(bp);
                            std::string ps1, ps2;
                            iss >> ps1; iss >> ps2;
                            auto resolvePos = [&](const std::string& tok, bool isX, float box, float cell) -> float {
                                if (tok == "left") return 0;
                                if (tok == "right") return box - cell;
                                if (tok == "top") return 0;
                                if (tok == "bottom") return box - cell;
                                if (tok == "center") return (box - cell) / 2;
                                if (!tok.empty() && tok.back() == '%') {
                                    float p = std::strtof(tok.c_str(), nullptr) / 100.0f;
                                    return p * (box - cell);
                                }
                                return parseLengthPx(tok, box);
                            };
                            if (!ps1.empty()) originX = x + resolvePos(ps1, true,  w, cellW);
                            if (!ps2.empty()) originY = y + resolvePos(ps2, false, h, cellH);
                            else originY = y + resolvePos(ps1, false, h, cellH);
                        }
                    }

                    bool tileX = (layerRepeat == "repeat" || layerRepeat == "repeat-x");
                    bool tileY = (layerRepeat == "repeat" || layerRepeat == "repeat-y");
                    bool needTile = tileX || tileY;
                    bool needClip = needTile || cellW < w || cellH < h ||
                                    originX > x || originY > y ||
                                    originX + cellW < x + w || originY + cellH < y + h;

                    // Build the tile list (for repeat, populate spans across the box).
                    struct Cell { float gx, gy, gw, gh; };
                    std::vector<Cell> cells;
                    if (needTile) {
                        float startX = tileX ? x - std::fmod(originX - x, cellW) - cellW : originX;
                        float startY = tileY ? y - std::fmod(originY - y, cellH) - cellH : originY;
                        float endX = tileX ? x + w : originX + cellW;
                        float endY = tileY ? y + h : originY + cellH;
                        for (float iy = startY; iy < endY; iy += cellH)
                            for (float ix = startX; ix < endX; ix += cellW)
                                cells.push_back({ix, iy, cellW, cellH});
                    } else {
                        cells.push_back({originX, originY, cellW, cellH});
                    }

                    if (needClip) {
                        renderer_->save();
                        renderer_->setClip(x, y, w, h);
                    }

                    for (const auto& cl : cells) {
                    float gx = cl.gx, gy = cl.gy, gw = cl.gw, gh = cl.gh;
                    if (val.find("linear-gradient") != std::string::npos) {
                        // CSS linear-gradient: gradient line passes through the
                        // center, with length = |W·sin(angle)| + |H·cos(angle)|.
                        // Direction is (sin(angle), -cos(angle)) so that
                        // 0deg = up, 90deg = right (CSS convention).
                        // Endpoints are in canvas space, so add the box origin.
                        float rad = angleDeg * 3.14159265f / 180.0f;
                        float sa = std::sin(rad), ca = -std::cos(rad);
                        float cx2 = gx + gw / 2, cy2 = gy + gh / 2;
                        float lineLen = std::abs(gw * std::sin(rad)) +
                                        std::abs(gh * std::cos(rad));
                        float dx = sa * lineLen / 2.0f;
                        float dy = ca * lineLen / 2.0f;
                        renderer_->fillLinearGradient(gx, gy, gw, gh,
                            cx2 - dx, cy2 - dy, cx2 + dx, cy2 + dy, stops);
                    } else if (isRadial) {
                        float rcx = radCxFrac * gw;
                        float rcy = radCyFrac * gh;
                        // Distances to each side from center.
                        float dL = rcx, dR = gw - rcx;
                        float dT = rcy, dB = gh - rcy;
                        float closestSideX = std::min(dL, dR);
                        float closestSideY = std::min(dT, dB);
                        float farthestSideX = std::max(dL, dR);
                        float farthestSideY = std::max(dT, dB);
                        float rx = 0, ry = 0;
                        if (radialIsCircle) {
                            // Circle: pick a single radius based on distances.
                            switch (radExtent) {
                                case RAD_CLOSEST_SIDE:
                                    rx = ry = std::min(closestSideX, closestSideY); break;
                                case RAD_CLOSEST_CORNER:
                                    rx = ry = std::sqrt(closestSideX*closestSideX +
                                                        closestSideY*closestSideY); break;
                                case RAD_FARTHEST_SIDE:
                                    rx = ry = std::max(farthestSideX, farthestSideY); break;
                                case RAD_FARTHEST_CORNER:
                                default:
                                    rx = ry = std::sqrt(farthestSideX*farthestSideX +
                                                        farthestSideY*farthestSideY); break;
                            }
                        } else {
                            // Ellipse: rx, ry computed independently per CSS spec.
                            switch (radExtent) {
                                case RAD_CLOSEST_SIDE:
                                    rx = closestSideX; ry = closestSideY; break;
                                case RAD_FARTHEST_SIDE:
                                    rx = farthestSideX; ry = farthestSideY; break;
                                case RAD_CLOSEST_CORNER: {
                                    // Ellipse with same aspect as closest-side, passing
                                    // through closest corner.
                                    float k = std::sqrt(2.0f);
                                    rx = closestSideX * k; ry = closestSideY * k; break;
                                }
                                case RAD_FARTHEST_CORNER:
                                default: {
                                    float k = std::sqrt(2.0f);
                                    rx = farthestSideX * k; ry = farthestSideY * k; break;
                                }
                            }
                        }
                        if (rx < 0.001f) rx = 0.001f;
                        if (ry < 0.001f) ry = 0.001f;
                        renderer_->fillRadialGradient(gx, gy, gw, gh,
                            gx + rcx, gy + rcy, rx, ry, stops);
                    } else if (val.find("conic-gradient") != std::string::npos) {
                        renderer_->fillConicGradient(gx, gy, gw, gh,
                            gx + gw/2, gy + gh/2, conicFromDeg, stops);
                    }
                    } // per-cell loop
                    if (needClip) renderer_->restore();
                }
            }
        }
        if (layerBlended) renderer_->restore();
        } // for layer
    }

    if (clipped) renderer_->restore();
}

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

// Return true if `elem` is a table with `border-collapse: collapse`. The
// table's own borders are painted centered on its border-box outer edge to
// "win" against adjacent cell borders when the table border is thicker.
static bool isCollapsedTable(dom::Element* elem) {
    if (!elem) return false;
    auto& cs = elem->computedStyle();
    auto dIt = cs.find("display");
    if (dIt == cs.end()) return false;
    const std::string& d = dIt->second;
    if (d != "table" && d != "inline-table") return false;
    auto bcIt = cs.find("border-collapse");
    return (bcIt != cs.end() && bcIt->second == "collapse");
}

// Read a length from the style map, treating empty/none as 0.
static float styleLengthPx(const htmlayout::css::ComputedStyle& cs, const char* prop) {
    auto it = cs.find(prop);
    if (it == cs.end() || it->second.empty() || it->second == "none") return 0.0f;
    // Simple px parse — collapse-mode borders are always concrete lengths.
    char* end = nullptr;
    float v = std::strtof(it->second.c_str(), &end);
    return v;
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

bool DrawTraversal::drawBorderImage(dom::Element* elem, float x, float y, float w, float h) {
    auto& style = elem->computedStyle();
    auto srcIt = style.find("border-image-source");
    if (srcIt == style.end() || srcIt->second.empty() || srcIt->second == "none")
        return false;
    const std::string& src = srcIt->second;
    // Only url() sources are supported; gradients fall back to normal borders.
    if (src.compare(0, 4, "url(") != 0) return false;
    size_t uStart = src.find('(') + 1;
    size_t uEnd = src.rfind(')');
    if (uEnd == std::string::npos || uEnd <= uStart) return false;
    std::string url = src.substr(uStart, uEnd - uStart);
    if (!url.empty() && (url.front() == '"' || url.front() == '\''))
        url = url.substr(1, url.size() - 2);
    if (url.empty()) return false;

    loadImage(url, basePath_);
    auto imgIt = imageCache_.find(url);
    // Missing/broken source (or an SVG, whose sub-rect sampling the encoded-
    // image path can't express): normal border painting takes over.
    if (imgIt == imageCache_.end() || imgIt->second.data.empty() || imgIt->second.isSvg)
        return false;
    const float imgW = static_cast<float>(imgIt->second.width);
    const float imgH = static_cast<float>(imgIt->second.height);
    if (imgW <= 0 || imgH <= 0) return false;
    const void* bytes = imgIt->second.data.data();
    const size_t byteLen = imgIt->second.data.size();
    const uint64_t imageId = imgIt->second.id;

    auto& box = elem->layoutBox();
    // Computed border widths — the reference for number-valued
    // border-image-width and border-image-outset.
    const float cbw[4] = {box.border.top, box.border.right,
                          box.border.bottom, box.border.left};

    auto tokenize = [](const std::string& s) {
        std::vector<std::string> out;
        std::string cur;
        for (char c : s) {
            if (std::isspace(static_cast<unsigned char>(c))) {
                if (!cur.empty()) { out.push_back(cur); cur.clear(); }
            } else {
                cur += c;
            }
        }
        if (!cur.empty()) out.push_back(cur);
        return out;
    };
    // 1-4 value box expansion into [top, right, bottom, left].
    auto boxOrder = [](const std::vector<std::string>& t) {
        std::array<std::string, 4> o;
        size_t n = t.size();
        if (n == 0) return o;
        o[0] = t[0];
        o[1] = n > 1 ? t[1] : t[0];
        o[2] = n > 2 ? t[2] : t[0];
        o[3] = n > 3 ? t[3] : o[1];
        return o;
    };

    // ---- border-image-slice: number = px in the SOURCE image, % of the
    // image dimension; optional `fill`. Clamped to the image bounds. --------
    bool fill = false;
    float slice[4];  // top, right, bottom, left (image px)
    {
        std::vector<std::string> toks;
        if (auto it = style.find("border-image-slice"); it != style.end())
            toks = tokenize(it->second);
        for (auto tIt = toks.begin(); tIt != toks.end();) {
            if (*tIt == "fill") { fill = true; tIt = toks.erase(tIt); }
            else ++tIt;
        }
        auto o = boxOrder(toks);
        for (int i = 0; i < 4; ++i) {
            float dim = (i == 0 || i == 2) ? imgH : imgW;  // top/bottom vs left/right
            if (o[i].empty()) { slice[i] = dim; continue; }  // initial 100%
            char* end = nullptr;
            float v = std::strtof(o[i].c_str(), &end);
            if (end == o[i].c_str()) { slice[i] = dim; continue; }
            if (end && *end == '%') v = v * dim / 100.0f;
            slice[i] = std::clamp(v, 0.0f, dim);
        }
    }

    // ---- border-image-outset: number = multiples of the computed
    // border-width, length = px. Expands the paint area outside the border
    // box (paint-only: layout and hit-testing are unaffected). -------------
    float outset[4] = {0, 0, 0, 0};
    {
        if (auto it = style.find("border-image-outset"); it != style.end()) {
            auto o = boxOrder(tokenize(it->second));
            for (int i = 0; i < 4; ++i) {
                if (o[i].empty()) continue;
                char* end = nullptr;
                float v = std::strtof(o[i].c_str(), &end);
                if (end == o[i].c_str()) continue;
                bool bare = (end == nullptr || *end == '\0');
                outset[i] = std::max(0.0f, bare ? v * cbw[i] : v);
            }
        }
    }

    // Border image area: the border box expanded by the outsets.
    const float ax = x - outset[3];
    const float ay = y - outset[0];
    const float aw = w + outset[3] + outset[1];
    const float ah = h + outset[0] + outset[2];
    if (aw <= 0 || ah <= 0) return true;  // nothing to paint, but bi is active

    // ---- border-image-width: number = multiples of the computed
    // border-width, length = px, % of the border image area (horizontal
    // sides against its width, vertical against its height), auto = the
    // intrinsic slice size. ------------------------------------------------
    float bw4[4];  // top, right, bottom, left
    {
        std::vector<std::string> toks;
        if (auto it = style.find("border-image-width"); it != style.end())
            toks = tokenize(it->second);
        auto o = boxOrder(toks);
        for (int i = 0; i < 4; ++i) {
            float areaRef = (i == 0 || i == 2) ? ah : aw;
            if (o[i].empty()) { bw4[i] = cbw[i]; continue; }  // initial 1
            if (o[i] == "auto") { bw4[i] = slice[i]; continue; }
            char* end = nullptr;
            float v = std::strtof(o[i].c_str(), &end);
            if (end == o[i].c_str()) { bw4[i] = cbw[i]; continue; }
            if (end && *end == '%') bw4[i] = v * areaRef / 100.0f;
            else if (end == nullptr || *end == '\0') bw4[i] = v * cbw[i];  // number
            else bw4[i] = v;  // length (px)
            bw4[i] = std::max(0.0f, bw4[i]);
        }
        // Proportional reduction when opposite widths together exceed the
        // border image area (CSS Backgrounds-3 §6.3).
        float f = 1.0f;
        if (bw4[3] + bw4[1] > aw && bw4[3] + bw4[1] > 0)
            f = std::min(f, aw / (bw4[3] + bw4[1]));
        if (bw4[0] + bw4[2] > ah && bw4[0] + bw4[2] > 0)
            f = std::min(f, ah / (bw4[0] + bw4[2]));
        for (float& v : bw4) v *= f;
    }
    const float wT = bw4[0], wR = bw4[1], wB = bw4[2], wL = bw4[3];

    // ---- border-image-repeat: 1-2 of stretch | repeat | round | space
    // (first horizontal — top/bottom edges + middle x — second vertical). --
    enum { kStretch = 0, kRepeat, kRound, kSpace };
    int repH = kStretch, repV = kStretch;
    {
        auto parseMode = [](const std::string& s) {
            if (s == "repeat") return (int)kRepeat;
            if (s == "round") return (int)kRound;
            if (s == "space") return (int)kSpace;
            return (int)kStretch;
        };
        if (auto it = style.find("border-image-repeat"); it != style.end()) {
            auto toks = tokenize(it->second);
            if (!toks.empty()) repH = parseMode(toks[0]);
            repV = toks.size() > 1 ? parseMode(toks[1]) : repH;
        }
    }

    // Source slice geometry (image px).
    const float sT = slice[0], sR = slice[1], sB = slice[2], sL = slice[3];
    const float midW = imgW - sL - sR;  // <= 0 → top/bottom edges + middle empty
    const float midH = imgH - sT - sB;  // <= 0 → left/right edges + middle empty

    // Draw the source sub-rect (sx, sy, sw, sh) into the dest rect
    // (dx, dy, dw, dh) using the existing clip + scaled whole-image drawImage
    // primitives: the full image is scaled so the sub-rect lands exactly on
    // the dest rect, and the clip cuts everything else away. drawImage
    // samples nearest-neighbor, so no neighboring-slice texels bleed in.
    auto drawRegion = [&](float sx, float sy, float sw, float sh,
                          float dx, float dy, float dw, float dh) {
        if (sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0) return;
        float scX = dw / sw, scY = dh / sh;
        renderer_->save();
        renderer_->setClip(dx, dy, dw, dh);
        renderer_->drawImage(bytes, byteLen,
                             dx - sx * scX, dy - sy * scY,
                             imgW * scX, imgH * scY, imageId);
        renderer_->restore();
    };

    // Tile positions along one axis for a repeat mode. `ideal` is the tile
    // length the source region maps to at the edge's cross-axis scale.
    struct Tile { float pos, len; };
    auto axisTiles = [](float start, float len, float ideal, int mode) {
        std::vector<Tile> out;
        if (len <= 0) return out;
        if (mode == kStretch || ideal <= 0.01f) {
            out.push_back({start, len});
            return out;
        }
        if (mode == kRound) {
            // Scale tiles so a whole number fits exactly.
            int n = std::max(1, (int)std::lround(len / ideal));
            n = std::min(n, 4096);
            float t = len / n;
            for (int i = 0; i < n; ++i) out.push_back({start + i * t, t});
            return out;
        }
        if (mode == kSpace) {
            // Whole tiles at their ideal size, leftover distributed as equal
            // gaps around them. Less than one tile fits → nothing painted.
            int n = (int)std::floor(len / ideal + 1e-4f);
            if (n < 1) return out;
            n = std::min(n, 4096);
            float gap = (len - n * ideal) / (n + 1);
            for (int i = 0; i < n; ++i)
                out.push_back({start + gap + i * (ideal + gap), ideal});
            return out;
        }
        // kRepeat: ideal-size tiles with the pattern centered in the area
        // (a tile centered on the midpoint), extended to cover it; the
        // enclosing clip trims the partial tiles at both ends.
        float mid = start + len * 0.5f - ideal * 0.5f;
        float first = mid - std::ceil((mid - start) / ideal) * ideal;
        int n = (int)std::ceil((start + len - first) / ideal);
        n = std::clamp(n, 1, 4096);
        for (int i = 0; i < n; ++i) out.push_back({first + i * ideal, ideal});
        return out;
    };

    auto tileRegion = [&](float sx, float sy, float sw, float sh,
                          float dx, float dy, float dw, float dh,
                          int modeH, int modeV, float idealW, float idealH) {
        if (sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0) return;
        auto hs = axisTiles(dx, dw, idealW, modeH);
        auto vs = axisTiles(dy, dh, idealH, modeV);
        if (hs.empty() || vs.empty()) return;
        renderer_->save();
        renderer_->setClip(dx, dy, dw, dh);
        for (auto& tv : vs)
            for (auto& th : hs)
                drawRegion(sx, sy, sw, sh, th.pos, tv.pos, th.len, tv.len);
        renderer_->restore();
    };

    // Corners: drawn 1:1 into their border-image-width boxes (stretched).
    drawRegion(0, 0, sL, sT,                    ax, ay, wL, wT);                    // TL
    drawRegion(imgW - sR, 0, sR, sT,            ax + aw - wR, ay, wR, wT);          // TR
    drawRegion(imgW - sR, imgH - sB, sR, sB,    ax + aw - wR, ay + ah - wB, wR, wB);// BR
    drawRegion(0, imgH - sB, sL, sB,            ax, ay + ah - wB, wL, wB);          // BL

    // Edges: cross axis stretched to the border-image width, main axis tiled
    // per the repeat mode. The ideal tile length preserves the slice's aspect
    // ratio at the cross-axis scale.
    const float edgeW = aw - wL - wR;  // horizontal edges' dest length
    const float edgeH = ah - wT - wB;  // vertical edges' dest length
    if (midW > 0) {
        if (sT > 0 && wT > 0)
            tileRegion(sL, 0, midW, sT, ax + wL, ay, edgeW, wT,
                       repH, kStretch, midW * wT / sT, wT);
        if (sB > 0 && wB > 0)
            tileRegion(sL, imgH - sB, midW, sB, ax + wL, ay + ah - wB, edgeW, wB,
                       repH, kStretch, midW * wB / sB, wB);
    }
    if (midH > 0) {
        if (sL > 0 && wL > 0)
            tileRegion(0, sT, sL, midH, ax, ay + wT, wL, edgeH,
                       kStretch, repV, wL, midH * wL / sL);
        if (sR > 0 && wR > 0)
            tileRegion(imgW - sR, sT, sR, midH, ax + aw - wR, ay + wT, wR, edgeH,
                       kStretch, repV, wR, midH * wR / sR);
    }

    // Middle: only with `fill`. Tile size follows the top edge's horizontal
    // scale and the left edge's vertical scale (falling back to the opposite
    // side, then 1:1) so the middle pattern lines up with the edges.
    if (fill && midW > 0 && midH > 0) {
        float hScale = sT > 0 && wT > 0 ? wT / sT : (sB > 0 && wB > 0 ? wB / sB : 1.0f);
        float vScale = sL > 0 && wL > 0 ? wL / sL : (sR > 0 && wR > 0 ? wR / sR : 1.0f);
        tileRegion(sL, sT, midW, midH, ax + wL, ay + wT, edgeW, edgeH,
                   repH, repV, midW * hScale, midH * vScale);
    }

    return true;
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
        float ix0 = x + L,     iy0 = y + T;
        float ix1 = x + w - R, iy1 = y + h - B;
        struct Wedge { const char* colorProp; render::PointF p[4]; };
        Wedge wedges[4] = {
            {"border-top-color",    {{ox0, oy0}, {ox1, oy0}, {ix1, iy0}, {ix0, iy0}}},
            {"border-right-color",  {{ox1, oy0}, {ox1, oy1}, {ix1, iy1}, {ix1, iy0}}},
            {"border-bottom-color", {{ox1, oy1}, {ox0, oy1}, {ix0, iy1}, {ix1, iy1}}},
            {"border-left-color",   {{ox0, oy1}, {ox0, oy0}, {ix0, iy0}, {ix0, iy1}}},
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

// Apply text-transform to a string
static std::string applyTextTransform(const std::string& text, const std::string& transform) {
    if (transform == "uppercase") {
        std::string r = text;
        for (auto& c : r) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        return r;
    }
    if (transform == "lowercase") {
        std::string r = text;
        for (auto& c : r) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return r;
    }
    if (transform == "capitalize") {
        std::string r = text;
        bool nextCap = true;
        for (auto& c : r) {
            if (std::isspace(static_cast<unsigned char>(c))) { nextCap = true; }
            else if (nextCap) { c = static_cast<char>(std::toupper(static_cast<unsigned char>(c))); nextCap = false; }
        }
        return r;
    }
    return text;
}

// Parse text-shadow: offsetX offsetY [blur] color (simplified — single shadow only)
struct TextShadow { float dx = 0, dy = 0, blur = 0; bromath::Color color = cfromColor8({0, 0, 0, 128}); };
static bool parseTextShadow(const std::string& val, TextShadow& out) {
    if (val.empty() || val == "none") return false;
    // Try to parse numbers and a color from the value
    std::istringstream iss(val);
    std::vector<float> nums;
    std::string colorStr;
    std::string token;
    while (iss >> token) {
        char* end = nullptr;
        float v = std::strtof(token.c_str(), &end);
        // Check if the token is a number (possibly with px suffix)
        if (end != token.c_str() && (*end == '\0' || *end == 'p')) {
            nums.push_back(v);
        } else {
            // Accumulate rest as color
            if (!colorStr.empty()) colorStr += ' ';
            colorStr += token;
        }
    }
    if (nums.size() >= 2) {
        out.dx = nums[0];
        out.dy = nums[1];
        if (nums.size() >= 3) out.blur = nums[2];
        if (!colorStr.empty()) DrawTraversal::tryParseColor(colorStr, out.color);
        return true;
    }
    return false;
}

void DrawTraversal::drawText(dom::Node* textNode, dom::Element* parent,
                             float offsetX, float offsetY) {
    if (!textNode || !parent) return;

    auto* tn = static_cast<dom::TextNode*>(textNode);
    std::string text = tn->data();
    if (text.empty()) return;

    // Skip whitespace-only text
    bool allWhitespace = true;
    for (char c : text) {
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r') {
            allWhitespace = false;
            break;
        }
    }
    if (allWhitespace) return;

    auto& style = parent->computedStyle();

    // Collapse whitespace to match the CSS white-space property that the layout
    // measured. Without this, raw newlines/tabs render as tofu glyphs and the
    // drawn width drifts from the measured width.
    {
        auto wsIt = style.find("white-space");
        std::string ws = (wsIt != style.end()) ? wsIt->second : std::string("normal");
        if (ws != "pre" && ws != "pre-wrap" && ws != "pre-line") {
            std::string collapsed;
            collapsed.reserve(text.size());
            bool lastSpace = false;
            for (char c : text) {
                if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                    if (!lastSpace) collapsed += ' ';
                    lastSpace = true;
                } else {
                    collapsed += c;
                    lastSpace = false;
                }
            }
            text = std::move(collapsed);
            if (text.empty()) return;
        }
    }
    render::FontRef fontRef = getFontRef(parent);

    // Pull vertical metrics straight from the renderer (empty-text measureText
    // returns just font metrics, no glyph shaping).
    auto baseMetrics = renderer_->measureText("", fontRef);
    float ascent  = baseMetrics.ascent;
    float descent = baseMetrics.descent;
    float lineH   = std::round(baseMetrics.ascent + baseMetrics.descent + baseMetrics.leading);
    if (lineH <= 0) lineH = fontRef.size * 1.2f;

    // Apply text-transform
    auto ttIt = style.find("text-transform");
    if (ttIt != style.end()) text = applyTextTransform(text, ttIt->second);

    // Get text color
    bromath::Color color = cfromColor8({0, 0, 0, 255});
    auto cIt = style.find("color");
    if (cIt != style.end()) tryParseColor(cIt->second, color);

    // Parse text-shadow
    TextShadow shadow;
    bool hasShadow = false;
    auto tsIt = style.find("text-shadow");
    if (tsIt != style.end()) hasShadow = parseTextShadow(tsIt->second, shadow);

    // Resolve letter-spacing for the renderer. Layout already accounts for
    // it in the text-run widths (text.cpp), so the painter must add the same
    // per-glyph advance or the visible glyphs end up flush-left in their box.
    float letterSpacing = 0.0f;
    float wordSpacing = 0.0f;
    {
        float fs = 16.0f;
        auto fsIt = style.find("font-size");
        if (fsIt != style.end()) {
            char* end = nullptr;
            float v = std::strtof(fsIt->second.c_str(), &end);
            if (end != fsIt->second.c_str() && v > 0) fs = v;
        }
        auto resolveSpacing = [&](const char* prop) -> float {
            auto it = style.find(prop);
            if (it == style.end() || it->second.empty() ||
                it->second == "normal")
                return 0.0f;
            const std::string& v = it->second;
            char* end = nullptr;
            float n = std::strtof(v.c_str(), &end);
            if (end == v.c_str()) return 0.0f;
            std::string unit(end);
            if (unit == "em") return n * fs;
            if (unit == "rem") return n * 16.0f;
            return n; // px / unitless
        };
        letterSpacing = resolveSpacing("letter-spacing");
        wordSpacing = resolveSpacing("word-spacing");
    }

    // Parse text-decoration
    std::string decoration;
    auto tdIt = style.find("text-decoration");
    if (tdIt != style.end()) decoration = tdIt->second;
    if (decoration.empty()) {
        auto tdlIt = style.find("text-decoration-line");
        if (tdlIt != style.end()) decoration = tdlIt->second;
    }

    // Text-align: compute offset when layout provides a content width
    float textAlignOffset = 0;
    auto taIt = style.find("text-align");
    if (taIt != style.end() && (taIt->second == "center" || taIt->second == "right")) {
        auto& pbox = parent->layoutBox();
        float availW = pbox.contentRect.width;
        if (availW > 0) {
            auto tm = renderer_->measureText(text, fontRef);
            if (taIt->second == "center")
                textAlignOffset = (availW - tm.width) / 2.0f;
            else
                textAlignOffset = availW - tm.width;
            if (textAlignOffset < 0) textAlignOffset = 0;
        }
    }

    // Use layout-computed position if available (from IFC text positioning),
    // otherwise fall back to parent's content origin.
    // When the IFC provides positions, text-align is already applied in contentRect.x,
    // so only add textAlignOffset in the fallback path to avoid double-centering.
    float x, y;

    auto& tbox = tn->layoutBox();
    if (tbox.contentRect.width > 0) {
        x = offsetX + tbox.contentRect.x;
        y = offsetY + tbox.contentRect.y + ascent;
    } else {
        x = offsetX + textAlignOffset;
        y = offsetY + ascent;
    }

    // Helper to draw a single line of text with shadow and decoration
    auto drawLine = [&](std::string_view line, float lx, float ly) {
        auto tm = renderer_->measureText(line, fontRef);

        // Draw text shadow first (behind text). drawTextEx applies the blur
        // mask filter to the shadow paint so a non-zero blur radius produces
        // a real Gaussian halo instead of a sharp colored copy.
        if (hasShadow) {
            renderer_->drawTextEx(line, lx + shadow.dx, ly + shadow.dy,
                                  fontRef, shadow.color,
                                  letterSpacing, shadow.blur, wordSpacing);
        }

        // Draw the text. Letter/word-spacing are applied here so visible
        // glyph advances match the widths the layout assumed.
        if (letterSpacing != 0.0f || wordSpacing != 0.0f) {
            renderer_->drawTextEx(line, lx, ly, fontRef, color, letterSpacing,
                                  0.0f, wordSpacing);
        } else {
            renderer_->drawText(line, lx, ly, fontRef, color);
        }

        // Draw text-decoration
        if (!decoration.empty() && decoration != "none") {
            float decoThickness = std::max(1.0f, ascent / 12.0f);
            if (decoration.find("underline") != std::string::npos) {
                float uy = ly + descent * 0.4f;
                renderer_->drawLine(lx, uy, lx + tm.width, uy, color, decoThickness);
            }
            if (decoration.find("overline") != std::string::npos) {
                float oy = ly - ascent;
                renderer_->drawLine(lx, oy, lx + tm.width, oy, color, decoThickness);
            }
            if (decoration.find("line-through") != std::string::npos) {
                float sy = ly - ascent * 0.35f;
                renderer_->drawLine(lx, sy, lx + tm.width, sy, color, decoThickness);
            }
        }
    };

    // Prefer the runs produced by the inline formatting context: one per
    // wrapped line segment, already positioned. This keeps drawn glyphs aligned
    // with the layout — and therefore with selection highlights and hit tests.
    const auto& runs = tbox.textRuns;
    if (!runs.empty()) {
        std::string transform;
        if (ttIt != style.end()) transform = ttIt->second;
        for (const auto& run : runs) {
            if (run.text.empty()) continue;
            std::string line = transform.empty()
                ? run.text
                : applyTextTransform(run.text, transform);
            float lx = offsetX + run.x;
            float ly = offsetY + run.y + ascent;
            drawLine(line, lx, ly);
        }
        return;
    }

    // Handle multi-line text (newlines in pre/pre-wrap)
    auto wsIt = style.find("white-space");
    bool preserveNewlines = false;
    if (wsIt != style.end()) {
        const auto& ws = wsIt->second;
        preserveNewlines = (ws == "pre" || ws == "pre-wrap" || ws == "pre-line");
    }

    if (preserveNewlines && text.find('\n') != std::string::npos) {
        float curY = y;
        size_t start = 0;
        while (start < text.size()) {
            size_t nl = text.find('\n', start);
            if (nl == std::string::npos) nl = text.size();
            if (nl > start) {
                std::string_view line(text.data() + start, nl - start);
                drawLine(line, x, curY);
            }
            curY += lineH;
            start = nl + 1;
        }
    } else {
        drawLine(text, x, y);
    }
}

void DrawTraversal::drawPseudo(dom::Element* host, const std::string& which,
                                float offsetX, float offsetY) {
    if (!host) return;
    if (!host->hasPseudo(which)) return;
    auto& style = host->pseudoStyle(which);
    auto& pbox = host->pseudoBox(which);

    // Paint the pseudo box's background and border first — a generated-content
    // box renders like any element, and a `content: ""` box can still carry a
    // visible background/border (e.g. a coloured block spacer or a badge dot).
    {
        float bx = offsetX + pbox.contentRect.x - pbox.padding.left - pbox.border.left;
        float by = offsetY + pbox.contentRect.y - pbox.padding.top - pbox.border.top;
        float bw = pbox.fullWidth();
        float bh = pbox.fullHeight();
        if (bw > 0.0f && bh > 0.0f) {
            render::Radii radii = getRadii(style, bw, bh);
            bool rounded = !radii.isZero();
            auto bgIt = style.find("background-color");
            if (bgIt != style.end() && !bgIt->second.empty()) {
                bromath::Color bc;
                if (tryParseColor(bgIt->second, bc) && bc.a > 0) {
                    if (rounded) renderer_->fillRoundRectRadii(bx, by, bw, bh, radii, bc);
                    else         renderer_->fillRect(bx, by, bw, bh, bc);
                }
            }
            // Uniform solid borders, painted per side as rectangles (dashed /
            // dotted / rounded-corner borders on pseudos are uncommon and fall
            // back to solid — sufficient for generated badges and rules).
            const char* wp[4] = {"border-top-width","border-right-width","border-bottom-width","border-left-width"};
            const char* sp[4] = {"border-top-style","border-right-style","border-bottom-style","border-left-style"};
            const char* cp[4] = {"border-top-color","border-right-color","border-bottom-color","border-left-color"};
            float bt[4];
            for (int i = 0; i < 4; ++i) {
                auto stIt = style.find(sp[i]);
                std::string st = (stIt == style.end()) ? "none" : stIt->second;
                bt[i] = (st == "none" || st == "hidden") ? 0.0f : styleLengthPx(style, wp[i]);
            }
            auto sideColor = [&](int i) {
                bromath::Color c = cfromColor8({0, 0, 0, 255});
                auto it = style.find(cp[i]);
                if (it != style.end() && !it->second.empty()) tryParseColor(it->second, c);
                return c;
            };
            if (bt[0] > 0) renderer_->fillRect(bx, by, bw, bt[0], sideColor(0));                    // top
            if (bt[2] > 0) renderer_->fillRect(bx, by + bh - bt[2], bw, bt[2], sideColor(2));       // bottom
            if (bt[3] > 0) renderer_->fillRect(bx, by, bt[3], bh, sideColor(3));                    // left
            if (bt[1] > 0) renderer_->fillRect(bx + bw - bt[1], by, bt[1], bh, sideColor(1));       // right
        }
    }

    // Font from pseudo style — pseudo styles inherit from the host but may
    // override font-* / color, so we cannot reuse the host's font handle.
    std::string family = "Arial";
    auto famIt = style.find("font-family");
    if (famIt != style.end() && !famIt->second.empty()) {
        family = famIt->second;
        if (family.front() == '"' || family.front() == '\'') {
            family = family.substr(1, family.size() - 2);
        }
    }
    float size = 16.0f;
    auto sizeIt = style.find("font-size");
    if (sizeIt != style.end()) {
        char* end = nullptr;
        float v = std::strtof(sizeIt->second.c_str(), &end);
        if (end != sizeIt->second.c_str() && v > 0) size = v;
    }
    int weight = 400;
    auto weightIt = style.find("font-weight");
    if (weightIt != style.end()) {
        const auto& w = weightIt->second;
        if (w == "bold") weight = 700;
        else if (w == "lighter") weight = 100;
        else if (w == "normal" || w.empty()) weight = 400;
        else {
            char* end = nullptr;
            long v = std::strtol(w.c_str(), &end, 10);
            if (end != w.c_str() && v > 0) weight = static_cast<int>(v);
        }
    }
    bool italic = false;
    auto styleIt = style.find("font-style");
    if (styleIt != style.end()) {
        italic = (styleIt->second == "italic" || styleIt->second == "oblique");
    }
    render::FontRef fontRef{family, size, weight, italic};
    auto fm = renderer_->measureText("", fontRef);
    float ascent = fm.ascent;

    bromath::Color color = cfromColor8({0, 0, 0, 255});
    auto cIt = style.find("color");
    if (cIt != style.end()) tryParseColor(cIt->second, color);

    TextShadow shadow;
    bool hasShadow = false;
    auto tsIt = style.find("text-shadow");
    if (tsIt != style.end()) hasShadow = parseTextShadow(tsIt->second, shadow);

    // Resolve letter-spacing for the pseudo's own style (pseudos inherit it
    // by default but the rule may override).
    float letterSpacing = 0.0f;
    float wordSpacing = 0.0f;
    {
        auto resolveSpacing = [&](const char* prop) -> float {
            auto it = style.find(prop);
            if (it == style.end() || it->second.empty() ||
                it->second == "normal")
                return 0.0f;
            const std::string& v = it->second;
            char* end = nullptr;
            float n = std::strtof(v.c_str(), &end);
            if (end == v.c_str()) return 0.0f;
            std::string unit(end);
            if (unit == "em") return n * size;
            if (unit == "rem") return n * 16.0f;
            return n;
        };
        letterSpacing = resolveSpacing("letter-spacing");
        wordSpacing = resolveSpacing("word-spacing");
    }

    // Use placed runs lifted onto pseudoBox by the layout adapter.
    // Run positions are relative to the pseudo wrapper's content origin
    // (recursive layoutInline produces a local IFC), so add the wrapper's
    // contentRect to translate them into the host's IFC coord space.
    float baseX = offsetX + pbox.contentRect.x;
    float baseY = offsetY + pbox.contentRect.y;
    for (const auto& run : pbox.textRuns) {
        if (run.text.empty()) continue;
        float lx = baseX + run.x;
        float ly = baseY + run.y + ascent;
        if (hasShadow) {
            renderer_->drawTextEx(run.text, lx + shadow.dx, ly + shadow.dy,
                                  fontRef, shadow.color,
                                  letterSpacing, shadow.blur, wordSpacing);
        }
        if (letterSpacing != 0.0f || wordSpacing != 0.0f) {
            renderer_->drawTextEx(run.text, lx, ly, fontRef, color,
                                  letterSpacing, 0.0f, wordSpacing);
        } else {
            renderer_->drawText(run.text, lx, ly, fontRef, color);
        }
    }
}

render::FontRef DrawTraversal::getFontRef(dom::Element* elem) {
    auto& style = elem->computedStyle();

    // Family: borrow the string from computedStyle. Quoted forms get unquoted
    // into a side string stashed on the element so the string_view stays
    // valid until the next style resolution.
    std::string_view family = "Arial";
    auto famIt = style.find("font-family");
    if (famIt != style.end() && !famIt->second.empty()) {
        const std::string& v = famIt->second;
        if (!v.empty() && (v.front() == '"' || v.front() == '\'')) {
            // Strip surrounding quotes via a substring view — the underlying
            // string in computedStyle is stable for the draw pass.
            if (v.size() >= 2 && v.back() == v.front())
                family = std::string_view(v).substr(1, v.size() - 2);
            else
                family = v;
        } else {
            family = v;
        }
    }

    float size = 16.0f;
    auto sizeIt = style.find("font-size");
    if (sizeIt != style.end()) {
        char* end = nullptr;
        float v = std::strtof(sizeIt->second.c_str(), &end);
        if (end != sizeIt->second.c_str() && v > 0) size = v;
    }

    int weight = 400;
    auto weightIt = style.find("font-weight");
    if (weightIt != style.end()) {
        const auto& w = weightIt->second;
        if (w == "bold") weight = 700;
        else if (w == "lighter") weight = 100;
        else if (w == "normal" || w.empty()) weight = 400;
        else {
            char* end = nullptr;
            long v = std::strtol(w.c_str(), &end, 10);
            if (end != w.c_str() && v > 0) weight = static_cast<int>(v);
        }
    }

    bool italic = false;
    auto styleIt = style.find("font-style");
    if (styleIt != style.end()) {
        italic = (styleIt->second == "italic" || styleIt->second == "oblique");
    }

    return render::FontRef{family, size, weight, italic};
}

// Decode a single percent-encoded (URL-encoded) string in place semantics.
// SVG/utf8 data URLs may percent-encode the markup (e.g. %3C for '<').
static std::string urlDecode(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            auto hex = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return 10 + c - 'a';
                if (c >= 'A' && c <= 'F') return 10 + c - 'A';
                return -1;
            };
            int hi = hex(s[i+1]);
            int lo = hex(s[i+2]);
            if (hi >= 0 && lo >= 0) {
                out.push_back(static_cast<char>((hi << 4) | lo));
                i += 2;
                continue;
            }
        }
        out.push_back(s[i]);
    }
    return out;
}

// Decode standard base64 (RFC 4648). Tolerates whitespace and missing padding.
// Base64 lives in util so the <img> intrinsic-size probe in
// engine/replaced_elements.cpp decodes data: URLs the same way this does.
static std::vector<uint8_t> base64Decode(const std::string& s) {
    return bro::util::base64Decode(s);
}

void DrawTraversal::loadImage(const std::string& url, const std::string& basePath) {
    if (imageCache_.count(url)) return;

    // data: URL — inline image content. Three forms we care about:
    //   data:image/svg+xml,<svg...>            (utf8 percent-encoded, with or without explicit ;utf8)
    //   data:image/svg+xml;utf8,<svg...>
    //   data:image/<type>;base64,<bytes>       (PNG/JPG/etc, base64-encoded)
    if (url.compare(0, 5, "data:") == 0) {
        auto comma = url.find(',');
        if (comma == std::string::npos) {
            imageCache_[url] = CachedImage{};
            return;
        }
        std::string meta = url.substr(5, comma - 5);   // e.g. "image/svg+xml;utf8"
        std::string body = url.substr(comma + 1);
        bool isBase64 = meta.find(";base64") != std::string::npos;
        bool isSvgXml = meta.find("image/svg+xml") != std::string::npos;

        CachedImage img;
        img.id = nextImageId();
        if (isSvgXml) {
            std::string markup = isBase64
                ? std::string(reinterpret_cast<const char*>(base64Decode(body).data()),
                              base64Decode(body).size())
                : urlDecode(body);
            img.isSvg = true;
            img.data.assign(markup.begin(), markup.end());
            // Parse intrinsic size from <svg width=... height=...>
            auto svgPos = markup.find("<svg");
            if (svgPos != std::string::npos) {
                auto endPos = markup.find('>', svgPos);
                if (endPos != std::string::npos) {
                    std::string tag = markup.substr(svgPos, endPos - svgPos);
                    auto attr = [&](const char* name) -> int {
                        std::string needle = std::string(" ") + name + "=";
                        auto p = tag.find(needle);
                        if (p == std::string::npos) return 0;
                        p += needle.size();
                        if (p >= tag.size()) return 0;
                        char q = tag[p];
                        if (q != '"' && q != '\'') return 0;
                        ++p;
                        auto eq = tag.find(q, p);
                        if (eq == std::string::npos) return 0;
                        return (int)std::strtof(tag.substr(p, eq - p).c_str(), nullptr);
                    };
                    img.width = attr("width");
                    img.height = attr("height");
                }
            }
            imageCache_[url] = std::move(img);
            return;
        }
        // Raster data URL — base64 or percent-encoded body
        std::vector<uint8_t> bytes = isBase64
            ? base64Decode(body)
            : [&]() {
                std::string d = urlDecode(body);
                return std::vector<uint8_t>(d.begin(), d.end());
            }();
        int w = 0, h = 0, comp = 0;
        if (!bytes.empty() &&
            broimage::probe_dimensions_memory(bytes.data(), bytes.size(), &w, &h, &comp)) {
            img.width = w;
            img.height = h;
        }
        img.data = std::move(bytes);
        imageCache_[url] = std::move(img);
        return;
    }

    // Strip URL query/fragment so `thumbnails/foo.png?v=12345` (standard
    // cache-bust) resolves to the file on disk.
    std::string cleanUrl = url;
    auto qPos = cleanUrl.find_first_of("?#");
    if (qPos != std::string::npos) cleanUrl.resize(qPos);

    std::string path;
    if (cleanUrl.size() >= 2 && cleanUrl[1] == ':') {
        path = cleanUrl;
    } else if (!cleanUrl.empty() && (cleanUrl[0] == '/' || cleanUrl[0] == '\\')) {
        path = cleanUrl;
    } else if (!basePath.empty()) {
        path = basePath;
        if (path.back() != '/' && path.back() != '\\') path += '/';
        path += cleanUrl;
    } else {
        path = cleanUrl;
    }

    std::ifstream ifs(path, std::ios::binary | std::ios::ate);
    if (!ifs.is_open()) {
        LOG_WARN("loadImage: failed to open '%s'", path.c_str());
        imageCache_[url] = CachedImage{};  // negative-cache so we don't re-warn each frame
        return;
    }
    auto fileSize = ifs.tellg();
    ifs.seekg(0);
    CachedImage img;
    img.id = nextImageId();
    img.data.resize(static_cast<size_t>(fileSize));
    ifs.read(reinterpret_cast<char*>(img.data.data()), fileSize);

    int w = 0, h = 0, comp = 0;
    if (broimage::probe_dimensions_memory(img.data.data(), img.data.size(), &w, &h, &comp)) {
        img.width = w;
        img.height = h;
    }
    imageCache_[url] = std::move(img);
}

bool DrawTraversal::tryParseColor(const std::string& colorStr, bromath::Color& out) {
    if (colorStr.empty()) return false;
    if (colorStr == "transparent") {
        // CSS transparent = rgba(0,0,0,0). Returning false would leave the
        // caller's `out` unchanged (often opaque black), so gradient stops
        // and similar uses of "transparent" would paint as solid black.
        out = cfromColor8({0, 0, 0, 0});
        return true;
    }

    // Hex color
    if (colorStr[0] == '#') {
        std::string hex = colorStr.substr(1);
        if (hex.size() == 3) {
            hex = {hex[0], hex[0], hex[1], hex[1], hex[2], hex[2]};
        }
        if (hex.size() == 6 || hex.size() == 8) {
            unsigned long val = std::strtoul(hex.c_str(), nullptr, 16);
            bromath::Color8 p;
            if (hex.size() == 6) {
                p = {static_cast<uint8_t>((val >> 16) & 0xFF),
                     static_cast<uint8_t>((val >> 8) & 0xFF),
                     static_cast<uint8_t>(val & 0xFF),
                     255};
            } else {
                p = {static_cast<uint8_t>((val >> 24) & 0xFF),
                     static_cast<uint8_t>((val >> 16) & 0xFF),
                     static_cast<uint8_t>((val >> 8) & 0xFF),
                     static_cast<uint8_t>(val & 0xFF)};
            }
            out = cfromColor8(p);
            return true;
        }
    }

    // rgb/rgba
    if (colorStr.substr(0, 4) == "rgb(" || colorStr.substr(0, 5) == "rgba(") {
        auto start = colorStr.find('(');
        auto end = colorStr.rfind(')');
        if (start != std::string::npos && end != std::string::npos) {
            std::string inner = colorStr.substr(start + 1, end - start - 1);
            for (char& c : inner) { if (c == ',' || c == '/') c = ' '; }
            std::istringstream iss(inner);
            float r, g, b, a = 1.0f;
            if (iss >> r >> g >> b) {
                iss >> a;
                // If alpha is <= 1.0, treat as 0-1 range; otherwise as 0-255
                if (a <= 1.0f) a *= 255.0f;
                bromath::Color8 p{
                    static_cast<uint8_t>(std::clamp(r, 0.0f, 255.0f)),
                    static_cast<uint8_t>(std::clamp(g, 0.0f, 255.0f)),
                    static_cast<uint8_t>(std::clamp(b, 0.0f, 255.0f)),
                    static_cast<uint8_t>(std::clamp(a, 0.0f, 255.0f))
                };
                out = cfromColor8(p);
                return true;
            }
        }
    }

    // hsl/hsla
    if (colorStr.substr(0, 4) == "hsl(" || colorStr.substr(0, 5) == "hsla(") {
        auto start = colorStr.find('(');
        auto end = colorStr.rfind(')');
        if (start != std::string::npos && end != std::string::npos) {
            std::string inner = colorStr.substr(start + 1, end - start - 1);
            // Remove % signs, replace commas/slashes with spaces
            for (char& c : inner) {
                if (c == ',' || c == '/' || c == '%') c = ' ';
            }
            std::istringstream iss(inner);
            float h, s, l, a = 1.0f;
            if (iss >> h >> s >> l) {
                iss >> a;
                // Normalize: h in [0,360), s and l in [0,1]
                h = std::fmod(h, 360.0f);
                if (h < 0) h += 360.0f;
                s = std::clamp(s / 100.0f, 0.0f, 1.0f);
                l = std::clamp(l / 100.0f, 0.0f, 1.0f);
                if (a <= 1.0f) a *= 255.0f;

                auto hue2rgb = [](float p, float q, float t) -> float {
                    if (t < 0) t += 1; if (t > 1) t -= 1;
                    if (t < 1.0f/6) return p + (q-p)*6*t;
                    if (t < 1.0f/2) return q;
                    if (t < 2.0f/3) return p + (q-p)*(2.0f/3-t)*6;
                    return p;
                };
                bromath::Color8 p8;
                if (s == 0) {
                    uint8_t v = static_cast<uint8_t>(l * 255);
                    p8.r = p8.g = p8.b = v;
                } else {
                    float q = l < 0.5f ? l*(1+s) : l+s-l*s;
                    float p = 2*l-q;
                    float hn = h/360.0f;
                    p8.r = static_cast<uint8_t>(hue2rgb(p, q, hn+1.0f/3)*255);
                    p8.g = static_cast<uint8_t>(hue2rgb(p, q, hn)*255);
                    p8.b = static_cast<uint8_t>(hue2rgb(p, q, hn-1.0f/3)*255);
                }
                p8.a = static_cast<uint8_t>(std::clamp(a, 0.0f, 255.0f));
                out = cfromColor8(p8);
                return true;
            }
        }
    }

    // Named colors — full CSS Color Level 4 set
    static const std::unordered_map<std::string, bromath::Color> named = {
        {"aliceblue",cfromColor8({240, 248, 255, 255})},{"antiquewhite",cfromColor8({250, 235, 215, 255})},
        {"aqua",cfromColor8({0, 255, 255, 255})},{"aquamarine",cfromColor8({127, 255, 212, 255})},
        {"azure",cfromColor8({240, 255, 255, 255})},{"beige",cfromColor8({245, 245, 220, 255})},
        {"bisque",cfromColor8({255, 228, 196, 255})},{"black",cfromColor8({0, 0, 0, 255})},
        {"blanchedalmond",cfromColor8({255, 235, 205, 255})},{"blue",cfromColor8({0, 0, 255, 255})},
        {"blueviolet",cfromColor8({138, 43, 226, 255})},{"brown",cfromColor8({165, 42, 42, 255})},
        {"burlywood",cfromColor8({222, 184, 135, 255})},{"cadetblue",cfromColor8({95, 158, 160, 255})},
        {"chartreuse",cfromColor8({127, 255, 0, 255})},{"chocolate",cfromColor8({210, 105, 30, 255})},
        {"coral",cfromColor8({255, 127, 80, 255})},{"cornflowerblue",cfromColor8({100, 149, 237, 255})},
        {"cornsilk",cfromColor8({255, 248, 220, 255})},{"crimson",cfromColor8({220, 20, 60, 255})},
        {"cyan",cfromColor8({0, 255, 255, 255})},{"darkblue",cfromColor8({0, 0, 139, 255})},
        {"darkcyan",cfromColor8({0, 139, 139, 255})},{"darkgoldenrod",cfromColor8({184, 134, 11, 255})},
        {"darkgray",cfromColor8({169, 169, 169, 255})},{"darkgreen",cfromColor8({0, 100, 0, 255})},
        {"darkgrey",cfromColor8({169, 169, 169, 255})},{"darkkhaki",cfromColor8({189, 183, 107, 255})},
        {"darkmagenta",cfromColor8({139, 0, 139, 255})},{"darkolivegreen",cfromColor8({85, 107, 47, 255})},
        {"darkorange",cfromColor8({255, 140, 0, 255})},{"darkorchid",cfromColor8({153, 50, 204, 255})},
        {"darkred",cfromColor8({139, 0, 0, 255})},{"darksalmon",cfromColor8({233, 150, 122, 255})},
        {"darkseagreen",cfromColor8({143, 188, 143, 255})},{"darkslateblue",cfromColor8({72, 61, 139, 255})},
        {"darkslategray",cfromColor8({47, 79, 79, 255})},{"darkslategrey",cfromColor8({47, 79, 79, 255})},
        {"darkturquoise",cfromColor8({0, 206, 209, 255})},{"darkviolet",cfromColor8({148, 0, 211, 255})},
        {"deeppink",cfromColor8({255, 20, 147, 255})},{"deepskyblue",cfromColor8({0, 191, 255, 255})},
        {"dimgray",cfromColor8({105, 105, 105, 255})},{"dimgrey",cfromColor8({105, 105, 105, 255})},
        {"dodgerblue",cfromColor8({30, 144, 255, 255})},{"firebrick",cfromColor8({178, 34, 34, 255})},
        {"floralwhite",cfromColor8({255, 250, 240, 255})},{"forestgreen",cfromColor8({34, 139, 34, 255})},
        {"fuchsia",cfromColor8({255, 0, 255, 255})},{"gainsboro",cfromColor8({220, 220, 220, 255})},
        {"ghostwhite",cfromColor8({248, 248, 255, 255})},{"gold",cfromColor8({255, 215, 0, 255})},
        {"goldenrod",cfromColor8({218, 165, 32, 255})},{"gray",cfromColor8({128, 128, 128, 255})},
        {"green",cfromColor8({0, 128, 0, 255})},{"greenyellow",cfromColor8({173, 255, 47, 255})},
        {"grey",cfromColor8({128, 128, 128, 255})},{"honeydew",cfromColor8({240, 255, 240, 255})},
        {"hotpink",cfromColor8({255, 105, 180, 255})},{"indianred",cfromColor8({205, 92, 92, 255})},
        {"indigo",cfromColor8({75, 0, 130, 255})},{"ivory",cfromColor8({255, 255, 240, 255})},
        {"khaki",cfromColor8({240, 230, 140, 255})},{"lavender",cfromColor8({230, 230, 250, 255})},
        {"lavenderblush",cfromColor8({255, 240, 245, 255})},{"lawngreen",cfromColor8({124, 252, 0, 255})},
        {"lemonchiffon",cfromColor8({255, 250, 205, 255})},{"lightblue",cfromColor8({173, 216, 230, 255})},
        {"lightcoral",cfromColor8({240, 128, 128, 255})},{"lightcyan",cfromColor8({224, 255, 255, 255})},
        {"lightgoldenrodyellow",cfromColor8({250, 250, 210, 255})},{"lightgray",cfromColor8({211, 211, 211, 255})},
        {"lightgreen",cfromColor8({144, 238, 144, 255})},{"lightgrey",cfromColor8({211, 211, 211, 255})},
        {"lightpink",cfromColor8({255, 182, 193, 255})},{"lightsalmon",cfromColor8({255, 160, 122, 255})},
        {"lightseagreen",cfromColor8({32, 178, 170, 255})},{"lightskyblue",cfromColor8({135, 206, 250, 255})},
        {"lightslategray",cfromColor8({119, 136, 153, 255})},{"lightslategrey",cfromColor8({119, 136, 153, 255})},
        {"lightsteelblue",cfromColor8({176, 196, 222, 255})},{"lightyellow",cfromColor8({255, 255, 224, 255})},
        {"lime",cfromColor8({0, 255, 0, 255})},{"limegreen",cfromColor8({50, 205, 50, 255})},
        {"linen",cfromColor8({250, 240, 230, 255})},{"magenta",cfromColor8({255, 0, 255, 255})},
        {"maroon",cfromColor8({128, 0, 0, 255})},{"mediumaquamarine",cfromColor8({102, 205, 170, 255})},
        {"mediumblue",cfromColor8({0, 0, 205, 255})},{"mediumorchid",cfromColor8({186, 85, 211, 255})},
        {"mediumpurple",cfromColor8({147, 111, 219, 255})},{"mediumseagreen",cfromColor8({60, 179, 113, 255})},
        {"mediumslateblue",cfromColor8({123, 104, 238, 255})},{"mediumspringgreen",cfromColor8({0, 250, 154, 255})},
        {"mediumturquoise",cfromColor8({72, 209, 204, 255})},{"mediumvioletred",cfromColor8({199, 21, 133, 255})},
        {"midnightblue",cfromColor8({25, 25, 112, 255})},{"mintcream",cfromColor8({245, 255, 250, 255})},
        {"mistyrose",cfromColor8({255, 228, 225, 255})},{"moccasin",cfromColor8({255, 228, 181, 255})},
        {"navajowhite",cfromColor8({255, 222, 173, 255})},{"navy",cfromColor8({0, 0, 128, 255})},
        {"oldlace",cfromColor8({253, 245, 230, 255})},{"olive",cfromColor8({128, 128, 0, 255})},
        {"olivedrab",cfromColor8({107, 142, 35, 255})},{"orange",cfromColor8({255, 165, 0, 255})},
        {"orangered",cfromColor8({255, 69, 0, 255})},{"orchid",cfromColor8({218, 112, 214, 255})},
        {"palegoldenrod",cfromColor8({238, 232, 170, 255})},{"palegreen",cfromColor8({152, 251, 152, 255})},
        {"paleturquoise",cfromColor8({175, 238, 238, 255})},{"palevioletred",cfromColor8({219, 112, 147, 255})},
        {"papayawhip",cfromColor8({255, 239, 213, 255})},{"peachpuff",cfromColor8({255, 218, 185, 255})},
        {"peru",cfromColor8({205, 133, 63, 255})},{"pink",cfromColor8({255, 192, 203, 255})},
        {"plum",cfromColor8({221, 160, 221, 255})},{"powderblue",cfromColor8({176, 224, 230, 255})},
        {"purple",cfromColor8({128, 0, 128, 255})},{"rebeccapurple",cfromColor8({102, 51, 153, 255})},
        {"red",cfromColor8({255, 0, 0, 255})},{"rosybrown",cfromColor8({188, 143, 143, 255})},
        {"royalblue",cfromColor8({65, 105, 225, 255})},{"saddlebrown",cfromColor8({139, 69, 19, 255})},
        {"salmon",cfromColor8({250, 128, 114, 255})},{"sandybrown",cfromColor8({244, 164, 96, 255})},
        {"seagreen",cfromColor8({46, 139, 87, 255})},{"seashell",cfromColor8({255, 245, 238, 255})},
        {"sienna",cfromColor8({160, 82, 45, 255})},{"silver",cfromColor8({192, 192, 192, 255})},
        {"skyblue",cfromColor8({135, 206, 235, 255})},{"slateblue",cfromColor8({106, 90, 205, 255})},
        {"slategray",cfromColor8({112, 128, 144, 255})},{"slategrey",cfromColor8({112, 128, 144, 255})},
        {"snow",cfromColor8({255, 250, 250, 255})},{"springgreen",cfromColor8({0, 255, 127, 255})},
        {"steelblue",cfromColor8({70, 130, 180, 255})},{"tan",cfromColor8({210, 180, 140, 255})},
        {"teal",cfromColor8({0, 128, 128, 255})},{"thistle",cfromColor8({216, 191, 216, 255})},
        {"tomato",cfromColor8({255, 99, 71, 255})},{"turquoise",cfromColor8({64, 224, 208, 255})},
        {"violet",cfromColor8({238, 130, 238, 255})},{"wheat",cfromColor8({245, 222, 179, 255})},
        {"white",cfromColor8({255, 255, 255, 255})},{"whitesmoke",cfromColor8({245, 245, 245, 255})},
        {"yellow",cfromColor8({255, 255, 0, 255})},{"yellowgreen",cfromColor8({154, 205, 50, 255})},
    };
    std::string lower = colorStr;
    for (auto& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    auto it = named.find(lower);
    if (it != named.end()) {
        out = it->second;
        return true;
    }

    return false;
}

bromath::Color DrawTraversal::parseColor(const std::string& color) {
    bromath::Color c = cfromColor8({0, 0, 0, 255});
    tryParseColor(color, c);
    return c;
}

// ---------------------------------------------------------------------------
// CSS 2.1 Appendix E painting: stacking-context tree construction + paint walk
// ---------------------------------------------------------------------------

std::unique_ptr<StackingContext> DrawTraversal::buildStackingContextTree(
    dom::Element* root, float scrollX, float scrollY) {
    auto rootSC = std::make_unique<StackingContext>();
    rootSC->root = root;
    rootSC->offsetX = scrollX;
    rootSC->offsetY = scrollY;
    rootSC->zIndex = 0;
    rootSC->zIsAuto = true;
    rootSC->treeOrder = 0;

    int dfsCounter = 1;

    // Compute the border-box clip rect contributed by `elem` if it has overflow
    // clipping on either axis. Mirrors the in-flow walker at lines ~966-983.
    auto elementClipRect = [](dom::Element* elem, float offX, float offY,
                              ClipRect& out) -> bool {
        auto& style = elem->computedStyle();
        auto axisClips = [](const std::string& v) {
            return v == "hidden" || v == "scroll" || v == "auto" || v == "clip";
        };
        std::string ox = getOverflowX(style);
        std::string oy = getOverflowY(style);
        if (!axisClips(ox) && !axisClips(oy)) return false;
        auto& box = elem->layoutBox();
        out.bx = box.contentRect.x + offX - box.padding.left - box.border.left;
        out.by = box.contentRect.y + offY - box.padding.top - box.border.top;
        out.bw = box.fullWidth();
        out.bh = box.fullHeight();
        out.radii = getRadii(style, out.bw, out.bh);
        return true;
    };

    // Recursive collector. `currentSC` is the nearest SC ancestor whose buckets
    // descendants populate. (offsetX, offsetY) is the absolute draw offset for
    // `elem`. `ancestorClips` is the clip chain accumulated from the current SC
    // root (exclusive) down to (but not including) `elem` — outermost-first.
    std::function<void(dom::Element*, StackingContext*, float, float,
                       const std::vector<ClipRect>&)> visit;
    visit = [&](dom::Element* elem, StackingContext* currentSC,
                float offX, float offY,
                const std::vector<ClipRect>& ancestorClips) {
        if (!elem) return;
        auto& style = elem->computedStyle();

        // display:none → don't paint, don't descend
        auto dispIt = style.find("display");
        if (dispIt != style.end() && dispIt->second == "none") return;

        // Flow-collapsed content (closed <details> body) never paints —
        // don't descend either, so positioned/SC descendants can't leak
        // into the stacking-context buckets.
        auto fcIt = style.find("-x-flow-collapse");
        if (fcIt != style.end() && fcIt->second == "collapse") return;

        // Compute child offset using the same logic as drawElementContent
        auto& box = elem->layoutBox();
        float x = box.contentRect.x + offX;
        float y = box.contentRect.y + offY;
        float maxST = std::max(0.0f, box.naturalHeight - box.contentRect.height);
        float scrollTop = std::clamp(elem->scrollTopValue(), 0.0f, maxST);
        float childOffX = x;
        float childOffY = y - scrollTop;

        bool isThisRoot = (elem == root);
        bool isSC = createsStackingContext(elem, isThisRoot);
        bool positioned = isPositioned(style);

        StackingContext* descendantSC = currentSC;

        if (isSC && !isThisRoot) {
            // Create a child SC entry on currentSC.
            auto sc = std::make_unique<StackingContext>();
            sc->root = elem;
            sc->offsetX = offX;
            sc->offsetY = offY;
            sc->zIsAuto = getZIndex(style, sc->zIndex);
            sc->treeOrder = dfsCounter++;
            sc->ancestorClips = ancestorClips;
            descendantSC = sc.get();
            currentSC->children.push_back(std::move(sc));
            // The SC root itself is painted by paintStackingContext (its own
            // drawElementContent call), and the legacy walker will skip it.
            skipSet_.insert(elem);
        } else if (positioned && !isThisRoot) {
            // Positioned but does not create an SC (z-index:auto on
            // relative/absolute). Goes into step 6 of nearest SC.
            StackingContext::PositionedEntry pe;
            pe.elem = elem;
            pe.offsetX = offX;
            pe.offsetY = offY;
            pe.tieBreaker = dfsCounter++;
            pe.ancestorClips = ancestorClips;
            currentSC->positionedNonSC.push_back(std::move(pe));
            skipSet_.insert(elem);
        }

        // Build the clip chain to pass to descendants. At an SC boundary the
        // chain resets — descendants of a child SC paint inside that SC's own
        // paintStackingContext call, which separately re-applies the SC's
        // captured ancestorClips. Within an SC's subtree the chain propagates
        // and accumulates each ancestor's overflow clip.
        std::vector<ClipRect> childClips;
        ClipRect selfClip;
        bool selfClips = elementClipRect(elem, offX, offY, selfClip);
        if (isSC && !isThisRoot) {
            // SC root's own clip still applies to its step-6 descendants
            // (drawElementContent's step-1 save/restore is balanced and gone
            // by the time step 6 runs).
            if (selfClips) childClips.push_back(selfClip);
        } else {
            childClips = ancestorClips;
            if (selfClips) childClips.push_back(selfClip);
        }

        // Recurse into composed children. If this element became an SC root,
        // descendants accumulate into IT; otherwise they accumulate into the
        // same currentSC. For positioned non-SC, descendants ALSO go to the
        // same currentSC (positioning doesn't open a new SC), but we still
        // skip the element in the normal walker — the positioned-entry paint
        // path will descend into it through drawElementContent.
        for (auto* child : elem->composedChildNodes()) {
            if (child && child->nodeType() == dom::NodeType::Element) {
                visit(static_cast<dom::Element*>(child),
                      descendantSC, childOffX, childOffY, childClips);
            }
        }
    };

    // Root: descendants belong to rootSC.
    auto& rootStyle = root->computedStyle();
    auto rDisp = rootStyle.find("display");
    if (rDisp != rootStyle.end() && rDisp->second == "none") return nullptr;

    // The root itself is the SC root (painted by paintStackingContext); we
    // recurse into its children directly so the root isn't double-skipped.
    auto& rbox = root->layoutBox();
    float rx = rbox.contentRect.x + scrollX;
    float ry = rbox.contentRect.y + scrollY;
    float rMaxST = std::max(0.0f, rbox.naturalHeight - rbox.contentRect.height);
    float rScrollTop = std::clamp(root->scrollTopValue(), 0.0f, rMaxST);
    float rChildOffX = rx;
    float rChildOffY = ry - rScrollTop;
    std::vector<ClipRect> rootClips;
    {
        ClipRect cr;
        if (elementClipRect(root, scrollX, scrollY, cr)) rootClips.push_back(cr);
    }
    for (auto* child : root->composedChildNodes()) {
        if (child && child->nodeType() == dom::NodeType::Element) {
            visit(static_cast<dom::Element*>(child),
                  rootSC.get(), rChildOffX, rChildOffY, rootClips);
        }
    }

    return rootSC;
}

void DrawTraversal::paintStackingContext(StackingContext* sc, bool withinPromoted) {
    if (!sc || !sc->root) return;

    // Paint-mode filter. In the default PaintMode::All (or with no promoted set
    // registered) none of this runs and the remainder is the original single-pass
    // walk, byte-for-byte. Promoted elements are always SC roots, so the only
    // subtree that can be a promoted target is one reached through here.
    if (paintMode_ != PaintMode::All && promotedElements_) {
        bool isPromoted = promotedElements_->count(sc->root) != 0;
        if (paintMode_ == PaintMode::BaseSkipPromoted && isPromoted) {
            // Skip this SC and its ENTIRE subtree (like the canvas early-return),
            // leaving a transparent hole; keep painting siblings / other SCs.
            return;
        }
        if (paintMode_ == PaintMode::PromotedOnly && isPromoted) {
            // Entered a promoted SC root: paint everything from here down (its
            // own transform/opacity wrappers included, so the promoted subtree
            // renders with its current animated transform baked in).
            withinPromoted = true;
        }
    }
    // PromotedOnly, not yet inside a promoted subtree: suppress THIS SC's own
    // painting (wrappers, box/background/borders/text, positioned-non-SC
    // content) but still recurse into child/positioned SCs to reach deeper
    // promoted roots. In All / BaseSkipPromoted this is always false.
    const bool suppressSelf =
        (paintMode_ == PaintMode::PromotedOnly && promotedElements_ && !withinPromoted);

    // The SC root's transform/opacity/filter must wrap ALL of its descendants'
    // painting — not just step 1 (the in-flow walk). Positioned descendants
    // and nested stacking contexts are part of the same SC subtree and must
    // inherit the SC root's transform. drawElementContent applies these on its
    // own around step 1 only; we need them active for steps 2–7 as well.
    //
    // Strategy: pre-apply the SC root's transform/opacity/filter here, tell
    // drawElementContent to skip its own application, and tear them down after
    // all steps complete.
    auto& rootStyle = sc->root->computedStyle();
    auto& rootBox = sc->root->layoutBox();
    float rbx = rootBox.contentRect.x + sc->offsetX -
                rootBox.padding.left - rootBox.border.left;
    float rby = rootBox.contentRect.y + sc->offsetY -
                rootBox.padding.top - rootBox.border.top;
    float rbw = rootBox.fullWidth();
    float rbh = rootBox.fullHeight();

    // mix-blend-mode composites the ENTIRE stacking context against the
    // backdrop. Push it as the outermost layer (before transform/opacity/
    // filter) so the blended group includes all of the SC's painting, and tear
    // it down last.
    bool wrappedBlend = false;
    bool wrappedTransform = false;
    bool wrappedOpacity = false;
    bool wrappedFilter = false;
    // suppressSelf (PromotedOnly, above a promoted root) skips every wrapper +
    // the SC's own content paint; recursion into children still happens below.
    if (!suppressSelf) {
    {
        auto mbIt = rootStyle.find("mix-blend-mode");
        if (mbIt != rootStyle.end() && !mbIt->second.empty() && mbIt->second != "normal") {
            render::BlendMode bm = parseBlendMode(mbIt->second);
            if (bm != render::BlendMode::Normal) {
                wrappedBlend = true;
                renderer_->saveLayerWithBlend(bm);
            }
        }
    }

    {
        auto trIt = rootStyle.find("transform");
        bool hasT = (trIt != rootStyle.end() && !trIt->second.empty()
                     && trIt->second != "none");
        float persp = parentPerspective(sc->root);
        bool wants3D = (persp > 0) || (hasT && transformHas3D(trIt->second));

        if (wants3D) {
            float pbx = 0, pby = 0, pbw = 0, pbh = 0;
            const htmlayout::css::ComputedStyle* perspStyle = nullptr;
            if (auto* parent = sc->root->layoutParent()) {
                auto& pb = parent->layoutBox();
                // sc->offsetX/Y is the parent's content-area absolute origin.
                pbx = sc->offsetX - pb.padding.left - pb.border.left;
                pby = sc->offsetY - pb.padding.top - pb.border.top;
                pbw = pb.fullWidth();
                pbh = pb.fullHeight();
                perspStyle = &parent->computedStyle();
            }
            bool is3D = false;
            auto m4 = buildElementTransform4x4(rootStyle, rbx, rby, rbw, rbh,
                                               persp, pbx, pby, pbw, pbh,
                                               perspStyle, is3D);
            if (!m4.isIdentity()) {
                wrappedTransform = true;
                renderer_->save();
                if (is3D)
                    renderer_->concat4x4(m4.m);
                else {
                    auto m2 = m4.to2D();
                    renderer_->concat(m2.a, m2.b, m2.c, m2.d, m2.e, m2.f);
                }
            }
        } else if (hasT) {
            auto mat = htmlayout::css::parseTransform(trIt->second, rbw, rbh);
            if (!mat.isIdentity()) {
                wrappedTransform = true;
                float ox, oy;
                auto toIt = rootStyle.find("transform-origin");
                std::string_view originVal = (toIt != rootStyle.end())
                    ? std::string_view(toIt->second) : std::string_view();
                htmlayout::css::parseTransformOrigin(originVal, rbw, rbh, ox, oy);
                renderer_->save();
                renderer_->translate(rbx + ox, rby + oy);
                renderer_->concat(mat.a, mat.b, mat.c, mat.d, mat.e, mat.f);
                renderer_->translate(-(rbx + ox), -(rby + oy));
            }
        }
    }
    {
        auto opIt = rootStyle.find("opacity");
        if (opIt != rootStyle.end()) {
            float opacity = std::clamp(std::strtof(opIt->second.c_str(), nullptr),
                                       0.0f, 1.0f);
            if (opacity < 1.0f) {
                wrappedOpacity = true;
                renderer_->saveLayerAlpha(static_cast<uint8_t>(opacity * 255));
            }
        }
    }
    {
        auto fIt = rootStyle.find("filter");
        if (fIt != rootStyle.end() && !fIt->second.empty() && fIt->second != "none") {
            auto filters = parseCSSFilter(fIt->second);
            if (!filters.empty()) {
                wrappedFilter = true;
                renderer_->saveLayerWithFilter(filters,
                    rbx - 50, rby - 50, rbw + 100, rbh + 100);
            }
        }
    }
    } // if (!suppressSelf)
    bool didWrap = wrappedBlend || wrappedTransform || wrappedOpacity || wrappedFilter;
    if (didWrap) scRootSkipWrap_.insert(sc->root);

    // Step 1: paint the SC root itself — its background, borders, and in-flow
    // non-positioned non-SC descendants — via the normal walker. The walker
    // consults skipSet_ to avoid descending into SC roots and positioned
    // non-SC descendants (they paint separately below).
    // In PromotedOnly above a promoted root this is suppressed (recursion into
    // children below still runs to reach deeper promoted subtrees).
    if (!suppressSelf)
        drawElementContent(sc->root, sc->offsetX, sc->offsetY);

    // Step 2: child SCs with z-index < 0, sorted by zIndex then tree order.
    std::vector<StackingContext*> negSCs, autoSCs, posSCs;
    for (auto& c : sc->children) {
        if (!c->zIsAuto && c->zIndex < 0) negSCs.push_back(c.get());
        else if (c->zIsAuto || c->zIndex == 0) autoSCs.push_back(c.get());
        else posSCs.push_back(c.get());
    }
    std::sort(negSCs.begin(), negSCs.end(), [](auto* a, auto* b) {
        if (a->zIndex != b->zIndex) return a->zIndex < b->zIndex;
        return a->treeOrder < b->treeOrder;
    });
    auto pushClips = [this](const std::vector<ClipRect>& clips) {
        for (auto& c : clips) {
            renderer_->save();
            if (!c.radii.isZero())
                renderer_->setClipRRect(c.bx, c.by, c.bw, c.bh, c.radii);
            else
                renderer_->setClip(c.bx, c.by, c.bw, c.bh);
            // Mirror onto the layer-break clip stack so a canvas/WebGL break in
            // an out-of-line (positioned / nested-SC) subtree still scissors to
            // the ancestor overflow clip the compositor would otherwise ignore.
            pushClipRect(c.bx, c.by, c.bw, c.bh);
        }
    };
    auto popClips = [this](const std::vector<ClipRect>& clips) {
        for (size_t i = 0; i < clips.size(); ++i) { renderer_->restore(); popClipRect(); }
    };
    for (auto* c : negSCs) {
        pushClips(c->ancestorClips);
        paintStackingContext(c, withinPromoted);
        popClips(c->ancestorClips);
    }

    // Steps 3-5 are folded into step 1 above (in-flow descendants paint via
    // drawElementContent in tree order — this gives correct ordering for the
    // common case; perfect block-then-float-then-inline separation would need
    // a deeper layout-tree split that bro doesn't currently materialize).

    // Step 6: positioned non-SC descendants AND child SCs with z-index:auto/0,
    // interleaved in tree order.
    struct Step6Item {
        int tieBreaker;
        std::function<void()> paint;
    };
    std::vector<Step6Item> step6;
    // positioned-non-SC entries are never promoted (promoted elements are SC
    // roots), so PromotedOnly above a promoted root suppresses them; any nested
    // promoted SC inside such an entry's subtree is a child SC of THIS sc and is
    // reached via the autoSCs/posSCs recursion below regardless.
    if (!suppressSelf) {
        for (auto& pe : sc->positionedNonSC) {
            step6.push_back({pe.tieBreaker, [this, &pe, &pushClips, &popClips]() {
                pushClips(pe.ancestorClips);
                drawElementContent(pe.elem, pe.offsetX, pe.offsetY);
                popClips(pe.ancestorClips);
            }});
        }
    }
    for (auto* c : autoSCs) {
        step6.push_back({c->treeOrder, [this, c, withinPromoted, &pushClips, &popClips]() {
            pushClips(c->ancestorClips);
            paintStackingContext(c, withinPromoted);
            popClips(c->ancestorClips);
        }});
    }
    std::sort(step6.begin(), step6.end(), [](const Step6Item& a, const Step6Item& b) {
        return a.tieBreaker < b.tieBreaker;
    });
    for (auto& item : step6) item.paint();

    // Step 7: child SCs with positive z-index.
    std::sort(posSCs.begin(), posSCs.end(), [](auto* a, auto* b) {
        if (a->zIndex != b->zIndex) return a->zIndex < b->zIndex;
        return a->treeOrder < b->treeOrder;
    });
    for (auto* c : posSCs) {
        pushClips(c->ancestorClips);
        paintStackingContext(c, withinPromoted);
        popClips(c->ancestorClips);
    }

    if (didWrap) {
        scRootSkipWrap_.erase(sc->root);
        if (wrappedFilter) renderer_->restore();
        if (wrappedOpacity) renderer_->restore();
        if (wrappedTransform) renderer_->restore();
        if (wrappedBlend) renderer_->restore();
    }
}

} // namespace bro::layout
