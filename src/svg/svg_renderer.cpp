#include "svg/svg_renderer.h"
#include "util/log.h"

#include <cctype>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>

#include <SkSVGDOM.h>
#include <SkSVGSVG.h>
#include <include/core/SkCanvas.h>
#include <include/core/SkFontMgr.h>
#include <include/core/SkImageInfo.h>
#include <include/core/SkStream.h>
#include <include/core/SkSurface.h>
#include <SkShaper_factory.h>

#include <cmath>

#ifdef _WIN32
#include <include/ports/SkTypeface_win.h>
#elif defined(__APPLE__)
#include <include/ports/SkFontMgr_mac_ct.h>
#else
#include <include/ports/SkFontMgr_fontconfig.h>
#include <include/ports/SkFontScanner_FreeType.h>
#endif

namespace bro::svg {

static sk_sp<SkFontMgr> getFontMgr() {
    static sk_sp<SkFontMgr> mgr =
#ifdef _WIN32
        SkFontMgr_New_DirectWrite();
#elif defined(__APPLE__)
        SkFontMgr_New_CoreText(nullptr);
#else
        SkFontMgr_New_FontConfig(nullptr, SkFontScanner_Make_FreeType());
#endif
    return mgr;
}

namespace {

/// One attribute's raw value off the outer `<svg …>` tag; false when absent.
/// The name must stand alone — preceded by whitespace, so `stroke-width` or
/// `inkscape:viewBox` never answer for `width` / `viewBox` — and may be
/// separated from its value by whitespace on either side of the `=`, the way
/// editors that put one attribute per line write it.
bool svgTagAttrRaw(const std::string& tag, const char* name, std::string& out) {
    const size_t nlen = std::strlen(name);
    for (size_t p = tag.find(name); p != std::string::npos; p = tag.find(name, p + 1)) {
        if (p == 0 || !std::isspace(static_cast<unsigned char>(tag[p - 1]))) continue;
        size_t i = p + nlen;
        while (i < tag.size() && std::isspace(static_cast<unsigned char>(tag[i]))) ++i;
        if (i >= tag.size() || tag[i] != '=') continue;
        ++i;
        while (i < tag.size() && std::isspace(static_cast<unsigned char>(tag[i]))) ++i;
        if (i >= tag.size() || (tag[i] != '"' && tag[i] != '\'')) continue;
        const char q = tag[i++];
        const size_t e = tag.find(q, i);
        if (e == std::string::npos) return false;
        out = tag.substr(i, e - i);
        return true;
    }
    return false;
}

/// A `width` / `height` length in CSS px: a bare number or px as is, the
/// absolute units at CSS's 96 px per inch, the font-relative ones against
/// the 16px default font. -1 when absent, unparseable, or a percentage (which
/// gives the document no intrinsic size along that axis).
float svgTagLength(const std::string& tag, const char* name) {
    std::string raw;
    if (!svgTagAttrRaw(tag, name, raw)) return -1.0f;
    const char* s = raw.c_str();
    char* end = nullptr;
    const float v = std::strtof(s, &end);
    if (end == s) return -1.0f;
    std::string unit(end);
    while (!unit.empty() && std::isspace(static_cast<unsigned char>(unit.back()))) unit.pop_back();
    for (char& c : unit) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    float scale = 1.0f;
    if (unit.empty() || unit == "px") scale = 1.0f;
    else if (unit == "in") scale = 96.0f;
    else if (unit == "cm") scale = 96.0f / 2.54f;
    else if (unit == "mm") scale = 96.0f / 25.4f;
    else if (unit == "q") scale = 96.0f / 101.6f;
    else if (unit == "pt") scale = 96.0f / 72.0f;
    else if (unit == "pc") scale = 16.0f;
    else if (unit == "em") scale = 16.0f;
    else if (unit == "ex") scale = 8.0f;
    else return -1.0f;   // % or something unknown
    return v * scale;
}

/// The outer `<svg …>` tag's text, empty when there isn't one.
std::string svgOuterTag(const char* data, size_t len) {
    std::string_view sv(data, len);
    auto svgPos = sv.find("<svg");
    if (svgPos == std::string_view::npos) return {};
    auto endPos = sv.find('>', svgPos);
    if (endPos == std::string_view::npos) return {};
    return std::string(sv.substr(svgPos, endPos - svgPos));
}

/// The outer tag's viewBox, false when it has none (or a malformed one).
bool svgTagViewBox(const std::string& tag, float v[4]) {
    std::string box;
    if (!svgTagAttrRaw(tag, "viewBox", box)) return false;
    const char* s = box.c_str();
    char* end = nullptr;
    for (int k = 0; k < 4; ++k) {
        while (*s == ',' || std::isspace(static_cast<unsigned char>(*s))) ++s;
        v[k] = std::strtof(s, &end);
        if (end == s) return false;
        s = end;
    }
    return v[2] > 0 && v[3] > 0;
}

/// The document's intrinsic pixel size: its `width`/`height` attributes when it
/// has them, else the extent of its `viewBox`. Either output is left at 0 when
/// the document says nothing — an SVG with only a viewBox has an intrinsic
/// *ratio* and no intrinsic size, and it is the caller's business what concrete
/// size to give it.
void svgIntrinsicSizeImpl(const char* data, size_t len,
                          float& outW, float& outH, bool& outHasViewBox) {
    outW = outH = 0.0f;
    outHasViewBox = false;

    const std::string tag = svgOuterTag(data, len);
    if (tag.empty()) return;
    float vb[4] = {0, 0, 0, 0};
    outHasViewBox = svgTagViewBox(tag, vb);

    const float aw = svgTagLength(tag, "width");
    const float ah = svgTagLength(tag, "height");
    if (aw > 0) outW = aw;
    if (ah > 0) outH = ah;
    if (outW > 0 && outH > 0) return;
    if (!outHasViewBox) return;
    // One side given: the other follows the viewBox's ratio, as CSS's
    // default sizing does for an image with an intrinsic ratio.
    if (outW > 0) { outH = outW * vb[3] / vb[2]; return; }
    if (outH > 0) { outW = outH * vb[2] / vb[3]; return; }
    outW = vb[2];
    outH = vb[3];
}

/// Number lists the way minifiers write them — `translate(-384.57-499.8)`,
/// `matrix(1 0 0 1 839.14-40)`, `0 0 .5.5` — are valid SVG (a sign or a
/// second decimal point starts the next number) but Skia's attribute parser
/// wants a separator between numbers and drops the whole transform without
/// one, drawing the icon hundreds of pixels off its canvas. Rewrite the values
/// of the number-list attributes it parses that way with explicit spaces.
/// (Path data has its own parser, which handles the compact form.)
std::string normalizeSvgNumberLists(const char* data, size_t len) {
    static const char* kAttrs[] = {"transform", "gradientTransform", "patternTransform",
                                   "viewBox", "points"};
    std::string out(data, len);
    for (const char* name : kAttrs) {
        const std::string needle = std::string(name) + "=";
        for (size_t p = out.find(needle); p != std::string::npos; p = out.find(needle, p + 1)) {
            if (p == 0 || !(std::isspace(static_cast<unsigned char>(out[p - 1])))) continue;
            size_t i = p + needle.size();
            if (i >= out.size() || (out[i] != '"' && out[i] != '\'')) continue;
            const char q = out[i++];
            const size_t e = out.find(q, i);
            if (e == std::string::npos) break;
            std::string value;
            value.reserve(e - i + 8);
            bool inNumber = false, sawDot = false, sawExp = false;
            char prev = '\0';
            for (size_t k = i; k < e; ++k) {
                const char c = out[k];
                if (c == '-' || c == '+') {
                    const bool exponentSign = prev == 'e' || prev == 'E';
                    if (inNumber && !exponentSign) {
                        value += ' ';
                        sawDot = sawExp = false;
                    }
                    inNumber = true;
                } else if (c == '.') {
                    if (inNumber && (sawDot || sawExp)) {
                        value += ' ';
                        sawExp = false;
                    }
                    inNumber = true;
                    sawDot = true;
                } else if (c >= '0' && c <= '9') {
                    inNumber = true;
                } else if ((c == 'e' || c == 'E') && inNumber && !sawExp &&
                           ((prev >= '0' && prev <= '9') || prev == '.')) {
                    sawExp = true;
                } else {
                    inNumber = sawDot = sawExp = false;
                }
                value += c;
                prev = c;
            }
            out.replace(i, e - i, value);
            p = i + value.size();
        }
    }
    return out;
}

/// Parse markup into a DOM sized to draw exactly into a `w`×`h` box: the
/// root's own width/height (absolute lengths Skia would otherwise honour over
/// the container, at its 90 dpi) give way to the box, and a document with no
/// viewBox gets one spanning its intrinsic size, so its content scales into
/// the box instead of drawing at 1:1 from the origin.
sk_sp<SkSVGDOM> makeBoxedDom(const char* data, size_t len, float w, float h) {
    const std::string markup = normalizeSvgNumberLists(data, len);
    SkMemoryStream stream(markup.data(), markup.size());
    auto dom = SkSVGDOM::Builder()
        .setFontManager(getFontMgr())
        .setTextShapingFactory(SkShapers::Primitive::Factory())
        .make(stream);
    if (!dom || !dom->getRoot()) return nullptr;

    SkSVGSVG* root = dom->getRoot();
    if (!root->getViewBox().has_value()) {
        float iw = 0, ih = 0;
        bool hasViewBox = false;
        svgIntrinsicSizeImpl(data, len, iw, ih, hasViewBox);
        // No intrinsic size either: the content is in px against the box.
        if (iw <= 0) iw = w;
        if (ih <= 0) ih = h;
        if (iw > 0 && ih > 0) root->setViewBox(SkRect::MakeWH(iw, ih));
    }
    root->setWidth(SkSVGLength(100, SkSVGLength::Unit::kPercentage));
    root->setHeight(SkSVGLength(100, SkSVGLength::Unit::kPercentage));
    dom->setContainerSize(SkSize::Make(w, h));
    return dom;
}

} // namespace

void svgIntrinsicSize(const char* data, size_t len, float& outW, float& outH) {
    bool hasViewBox = false;
    svgIntrinsicSizeImpl(data, len, outW, outH, hasViewBox);
}

bool looksLikeSvg(const char* data, size_t len) {
    if (!data || len == 0) return false;
    // Only the head matters, and an SVG file may open with an XML declaration,
    // a doctype, a BOM or comments before the root element.
    const size_t n = len < 1024 ? len : 1024;
    return std::string_view(data, n).find("<svg") != std::string_view::npos;
}

void renderSvgMarkupToCanvas(SkCanvas* canvas,
                             const char* data, size_t len,
                             float x, float y, float w, float h) {
    if (!canvas || !data || len == 0 || w <= 0 || h <= 0) return;

    auto dom = makeBoxedDom(data, len, w, h);
    if (!dom) {
        LOG_WARN("SVG: failed to parse markup");
        return;
    }
    canvas->save();
    canvas->translate(x, y);
    canvas->clipRect(SkRect::MakeWH(w, h));
    dom->render(canvas);
    canvas->restore();
}

bool rasterizeSvgMarkup(const char* data, size_t len,
                        int reqW, int reqH,
                        int& outW, int& outH,
                        std::vector<uint8_t>& outPixels) {
    if (!data || len == 0) return false;

    float intrW = 0, intrH = 0;
    bool hasViewBox = false;
    svgIntrinsicSizeImpl(data, len, intrW, intrH, hasViewBox);

    int w = reqW > 0 ? reqW : static_cast<int>(std::lround(intrW));
    int h = reqH > 0 ? reqH : static_cast<int>(std::lround(intrH));
    // A document with neither a size nor a viewBox has no opinion at all, so
    // fall back to CSS's default object size rather than refusing to draw it.
    if (w <= 0) w = 300;
    if (h <= 0) h = 150;

    // A vector image has no natural pixel bound, so a bad or hostile viewBox
    // ("0 0 1e9 1e9") would otherwise ask for a terabyte of surface.
    constexpr int kMaxDim = 8192;
    if (w > kMaxDim || h > kMaxDim) {
        LOG_WARN("SVG: refusing to rasterize %dx%d (limit %d per side)", w, h, kMaxDim);
        return false;
    }

    // Rasterize premultiplied — the only alpha type a Skia surface draws into —
    // and let readPixels un-premultiply on the way out, so the buffer matches
    // what the bitmap decoders produce.
    auto surface = SkSurfaces::Raster(
        SkImageInfo::Make(w, h, kRGBA_8888_SkColorType, kPremul_SkAlphaType));
    if (!surface) return false;

    SkCanvas* canvas = surface->getCanvas();
    canvas->clear(SK_ColorTRANSPARENT);
    renderSvgMarkupToCanvas(canvas, data, len, 0.0f, 0.0f,
                            static_cast<float>(w), static_cast<float>(h));

    std::vector<uint8_t> pixels(static_cast<size_t>(w) * static_cast<size_t>(h) * 4, 0);
    const SkImageInfo readInfo =
        SkImageInfo::Make(w, h, kRGBA_8888_SkColorType, kUnpremul_SkAlphaType);
    if (!surface->readPixels(readInfo, pixels.data(),
                             static_cast<size_t>(w) * 4, 0, 0)) {
        return false;
    }

    outW = w;
    outH = h;
    outPixels = std::move(pixels);
    return true;
}

} // namespace bro::svg
