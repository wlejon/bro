#include "svg/svg_renderer.h"
#include "util/log.h"

#include <cstdlib>
#include <string>
#include <string_view>

#include <SkSVGDOM.h>
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

/// One attribute off the outer `<svg …>` tag, as a float. -1 when absent.
float svgTagAttr(const std::string& tag, const char* name) {
    std::string needle = std::string(" ") + name + "=";
    auto p = tag.find(needle);
    if (p == std::string::npos) return -1.0f;
    p += needle.size();
    if (p >= tag.size()) return -1.0f;
    const char q = tag[p];
    if (q != '"' && q != '\'') return -1.0f;
    ++p;
    auto eq = tag.find(q, p);
    if (eq == std::string::npos) return -1.0f;
    return std::strtof(tag.substr(p, eq - p).c_str(), nullptr);
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
    outHasViewBox = tag.find("viewBox") != std::string::npos;

    const float aw = svgTagAttr(tag, "width");
    const float ah = svgTagAttr(tag, "height");
    if (aw > 0) outW = aw;
    if (ah > 0) outH = ah;
    if (outW > 0 && outH > 0) return;

    if (!outHasViewBox) return;
    // viewBox="min-x min-y width height" — the last two are the extent.
    auto p = tag.find("viewBox");
    p = tag.find_first_of("\"'", p);
    if (p == std::string::npos) return;
    const char q = tag[p];
    auto eq = tag.find(q, p + 1);
    if (eq == std::string::npos) return;
    const std::string box = tag.substr(p + 1, eq - p - 1);
    float v[4] = {0, 0, 0, 0};
    const char* s = box.c_str();
    char* end = nullptr;
    for (float& f : v) {
        while (*s == ',' || *s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') ++s;
        f = std::strtof(s, &end);
        if (end == s) return;
        s = end;
    }
    if (outW <= 0 && v[2] > 0) outW = v[2];
    if (outH <= 0 && v[3] > 0) outH = v[3];
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
    if (!canvas || !data || len == 0) return;

    SkMemoryStream stream(data, len);
    auto dom = SkSVGDOM::Builder()
        .setFontManager(getFontMgr())
        .setTextShapingFactory(SkShapers::Primitive::Factory())
        .make(stream);
    if (!dom) {
        LOG_WARN("SVG: failed to parse markup");
        return;
    }

    // Determine SVG intrinsic size so we can scale to fit the requested rect
    // when the SVG declares an explicit width/height with no viewBox (Skia
    // would otherwise draw at intrinsic dimensions). With a viewBox present the
    // container size does the mapping, so the requested rect is used as-is.
    float intrW = w, intrH = h;
    {
        float aw = 0, ah = 0;
        bool hasViewBox = false;
        svgIntrinsicSizeImpl(data, len, aw, ah, hasViewBox);
        if (!hasViewBox) {
            if (aw > 0) intrW = aw;
            if (ah > 0) intrH = ah;
        }
    }

    dom->setContainerSize(SkSize::Make(intrW, intrH));
    canvas->save();
    canvas->translate(x, y);
    if (intrW > 0 && intrH > 0 && (intrW != w || intrH != h)) {
        canvas->scale(w / intrW, h / intrH);
    }
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
