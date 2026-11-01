#include "render/svg_image.h"
#include "render/raster_renderer.h"
#include "util/log.h"

#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>

#include <include/core/SkImageInfo.h>
#include <include/core/SkSurface.h>

namespace bro::render {

namespace {

std::atomic<SvgPainter> g_painter{nullptr};

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

}  // namespace

void setSvgPainter(SvgPainter painter) { g_painter.store(painter); }

bool looksLikeSvg(const char* data, size_t len) {
    if (!data || len == 0) return false;
    // Only the head matters, and an SVG file may open with an XML declaration,
    // a doctype, a BOM or comments before the root element.
    const size_t n = len < 1024 ? len : 1024;
    return std::string_view(data, n).find("<svg") != std::string_view::npos;
}

bool svgViewBox(const char* data, size_t len, float out[4]) {
    const std::string tag = svgOuterTag(data, len);
    return !tag.empty() && svgTagViewBox(tag, out);
}

void svgIntrinsicSize(const char* data, size_t len, float& outW, float& outH) {
    outW = outH = 0.0f;
    const std::string tag = svgOuterTag(data, len);
    if (tag.empty()) return;
    float vb[4] = {0, 0, 0, 0};
    const bool hasViewBox = svgTagViewBox(tag, vb);

    const float aw = svgTagLength(tag, "width");
    const float ah = svgTagLength(tag, "height");
    if (aw > 0) outW = aw;
    if (ah > 0) outH = ah;
    if (outW > 0 && outH > 0) return;
    if (!hasViewBox) return;
    // One side given: the other follows the viewBox's ratio, as CSS's
    // default sizing does for an image with an intrinsic ratio.
    if (outW > 0) { outH = outW * vb[3] / vb[2]; return; }
    if (outH > 0) { outW = outH * vb[2] / vb[3]; return; }
    outW = vb[2];
    outH = vb[3];
}

bool rasterizeSvgMarkup(const char* data, size_t len, int reqW, int reqH,
                        int& outW, int& outH, std::vector<uint8_t>& outPixels) {
    if (!data || len == 0) return false;
    SvgPainter painter = g_painter.load();
    if (!painter) return false;

    float intrW = 0, intrH = 0;
    svgIntrinsicSize(data, len, intrW, intrH);
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

    // One CPU renderer per thread: decoders run on the image store's threads,
    // and a renderer (its fonts and shaping cache) is never shared.
    thread_local std::unique_ptr<RasterRenderer> raster;
    if (!raster) raster = std::make_unique<RasterRenderer>();
    raster->beginFrame(w, h);
    raster->clear(bromath::Color{0, 0, 0, 0});
    const bool ok = painter(raster.get(), data, len, static_cast<float>(w), static_cast<float>(h));
    raster->endFrame();
    if (!ok || !raster->surface()) return false;

    // Read back un-premultiplied, so the buffer matches what the bitmap
    // decoders produce.
    std::vector<uint8_t> pixels(static_cast<size_t>(w) * static_cast<size_t>(h) * 4, 0);
    const SkImageInfo readInfo = SkImageInfo::Make(w, h, kRGBA_8888_SkColorType, kUnpremul_SkAlphaType);
    if (!raster->surface()->readPixels(readInfo, pixels.data(), static_cast<size_t>(w) * 4, 0, 0))
        return false;
    outW = w;
    outH = h;
    outPixels = std::move(pixels);
    return true;
}

} // namespace bro::render
