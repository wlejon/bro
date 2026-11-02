// SVG <filter> → render::SvgFilterPrimitive graph, resolved to the filtered
// element's user space. The backend builds the Skia image filters from it
// (render/filter_chain.h, BuildSvgFilterGraph).

#include "layout/svg_paint_internal.h"
#include "layout/svg_common.h"
#include "layout/svg_image.h"
#include "dom/element.h"
#include "render/filter_chain.h"
#include "render/image_store.h"
#include "render/raster_renderer.h"

#include <include/core/SkImageInfo.h>
#include <include/core/SkSurface.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <unordered_map>
#include <vector>

namespace bro::layout {

using namespace svgcommon;
using render::SvgFilterPrimitive;
using Kind = render::SvgFilterPrimitive::Kind;

namespace {

// An attribute, then the computed style (filter properties such as
// flood-color are presentation attributes the cascade may carry).
std::string propOf(const dom::Element* el, const char* name, const char* fallback) {
    const std::string& a = attrOf(el, name);
    if (!a.empty()) return a;
    return styleOr(el, name, fallback);
}

float numAttr(const dom::Element* el, const char* name, float def) {
    return attrFloat(el, name, def);
}

// "a" or "a b" → (a, b).
void numPair(const dom::Element* el, const char* name, float def, float& a, float& b) {
    std::vector<float> v = parseNumberList(attrOf(el, name));
    a = v.empty() ? def : v[0];
    b = v.size() > 1 ? v[1] : a;
}

render::BlendMode blendModeOf(const std::string& m) {
    using B = render::BlendMode;
    if (m == "multiply") return B::Multiply;
    if (m == "screen") return B::Screen;
    if (m == "overlay") return B::Overlay;
    if (m == "darken") return B::Darken;
    if (m == "lighten") return B::Lighten;
    if (m == "color-dodge") return B::ColorDodge;
    if (m == "color-burn") return B::ColorBurn;
    if (m == "hard-light") return B::HardLight;
    if (m == "soft-light") return B::SoftLight;
    if (m == "difference") return B::Difference;
    if (m == "exclusion") return B::Exclusion;
    if (m == "hue") return B::Hue;
    if (m == "saturation") return B::Saturation;
    if (m == "color") return B::Color;
    if (m == "luminosity") return B::Luminosity;
    return B::Normal;
}

uint8_t channelOf(const std::string& c) {
    if (c == "R") return 0;
    if (c == "G") return 1;
    if (c == "B") return 2;
    return 3;
}

bromath::Color colorProp(const dom::Element* el, const char* colorName, const char* opacityName,
                         const char* fallback) {
    bromath::Color c{0, 0, 0, 1};
    resolveColor(propOf(el, colorName, fallback), currentColorOf(el), c);
    if (opacityName) {
        const std::string o = propOf(el, opacityName, "1");
        char* end = nullptr;
        float f = std::strtof(o.c_str(), &end);
        if (end != o.c_str()) {
            if (*end == '%') f /= 100.0f;
            c.a *= std::clamp(f, 0.0f, 1.0f);
        }
    }
    return c;
}

bool packRgbaToFloats(const uint8_t* rgba, int w, int h,
                      SvgFilterPrimitive& p, std::vector<float>& data,
                      float dstX, float dstY, float dstW, float dstH) {
    if (!rgba || w <= 0 || h <= 0 || dstW <= 0 || dstH <= 0) return false;
    constexpr int kMaxDim = 512;
    int outW = w, outH = h;
    std::vector<uint8_t> resampled;
    const uint8_t* src = rgba;
    if (w > kMaxDim || h > kMaxDim) {
        const float s = std::min(static_cast<float>(kMaxDim) / static_cast<float>(w),
                                 static_cast<float>(kMaxDim) / static_cast<float>(h));
        outW = std::max(1, static_cast<int>(std::lround(w * s)));
        outH = std::max(1, static_cast<int>(std::lround(h * s)));
        resampled.resize(static_cast<size_t>(outW) * static_cast<size_t>(outH) * 4);
        for (int y = 0; y < outH; ++y) {
            const int sy = std::clamp(static_cast<int>(y * h / outH), 0, h - 1);
            for (int x = 0; x < outW; ++x) {
                const int sx = std::clamp(static_cast<int>(x * w / outW), 0, w - 1);
                const uint8_t* sp = rgba + (static_cast<size_t>(sy) * w + sx) * 4;
                uint8_t* dp = resampled.data() + (static_cast<size_t>(y) * outW + x) * 4;
                dp[0] = sp[0]; dp[1] = sp[1]; dp[2] = sp[2]; dp[3] = sp[3];
            }
        }
        src = resampled.data();
    }
    p.kind = Kind::Image;
    p.p[0] = dstX;
    p.p[1] = dstY;
    p.p[2] = dstW;
    p.p[3] = dstH;
    p.p[4] = static_cast<float>(outW);
    p.p[5] = static_cast<float>(outH);
    p.dataOffset = static_cast<uint32_t>(data.size());
    const size_t n = static_cast<size_t>(outW) * static_cast<size_t>(outH);
    data.reserve(data.size() + n * 2);
    for (size_t i = 0; i < n; ++i) {
        const uint16_t rg = static_cast<uint16_t>(src[i * 4 + 0]) |
                            (static_cast<uint16_t>(src[i * 4 + 1]) << 8);
        const uint16_t ba = static_cast<uint16_t>(src[i * 4 + 2]) |
                            (static_cast<uint16_t>(src[i * 4 + 3]) << 8);
        data.push_back(static_cast<float>(rg));
        data.push_back(static_cast<float>(ba));
    }
    p.dataLen = static_cast<uint32_t>(data.size() - p.dataOffset);
    return true;
}

} // namespace

bool buildSvgFilter(const SvgPaintCtx& ctx, const dom::Element* el, const SkRect& bbox,
                    std::vector<SvgFilterPrimitive>& prims, std::vector<float>& data,
                    SkRect& region) {
    const std::string ref = urlRef(styleOr(el, "filter", "none"));
    if (ref.empty()) return false;
    const dom::Element* filter = findById(ctx.root, ref);
    if (!filter || lower(filter->tagName()) != "filter") return false;

    // The filter region: objectBoundingBox fractions (default -10% / 120%)
    // or user-space lengths.
    const bool obb = attrOf(filter, "filterUnits") != "userSpaceOnUse";
    auto regionLen = [&](const char* n, const char* def, float bbStart, float bbSize, float vp) {
        std::string v = attrOf(filter, n);
        if (v.empty()) v = def;
        char* end = nullptr;
        float f = std::strtof(v.c_str(), &end);
        const bool pct = end && *end == '%';
        if (obb) return bbStart + (pct ? f / 100.0f : f) * bbSize;
        return pct ? f / 100.0f * vp : f;
    };
    const float rx = regionLen("x", "-10%", bbox.fLeft, bbox.width(), ctx.vpW);
    const float ry = regionLen("y", "-10%", bbox.fTop, bbox.height(), ctx.vpH);
    const float rw = regionLen("width", "120%", 0, bbox.width(), ctx.vpW);
    const float rh = regionLen("height", "120%", 0, bbox.height(), ctx.vpH);
    region = SkRect::MakeXYWH(rx, ry, rw, rh);
    prims.clear();
    data.clear();
    // An objectBoundingBox filter on geometry with no area has no region:
    // the element is not rendered.
    if (obb && (bbox.width() <= 0 || bbox.height() <= 0)) { region = SkRect::MakeEmpty(); return true; }
    if (!(rw > 0) || !(rh > 0)) { region = SkRect::MakeEmpty(); return true; }

    const bool primObb = attrOf(filter, "primitiveUnits") == "objectBoundingBox";
    // A length in primitive units, along x / y.
    auto plx = [&](float v) { return primObb ? v * bbox.width() : v; };
    auto ply = [&](float v) { return primObb ? v * bbox.height() : v; };

    const std::string filterSpace = propOf(filter, "color-interpolation-filters", "linearRGB");
    std::unordered_map<std::string, int16_t> results;

    auto inputOf = [&](const dom::Element* fe, const char* name) -> int16_t {
        const std::string& v = attrOf(fe, name);
        if (v == "SourceGraphic") return SvgFilterPrimitive::kSourceGraphic;
        if (v == "SourceAlpha") return SvgFilterPrimitive::kSourceAlpha;
        if (!v.empty()) {
            auto it = results.find(v);
            if (it != results.end()) return it->second;
            // BackgroundImage / FillPaint / StrokePaint and unknown names:
            // browsers give the source graphic or nothing; the source.
            if (v != "BackgroundImage" && v != "BackgroundAlpha" && v != "FillPaint" && v != "StrokePaint")
                return prims.empty() ? SvgFilterPrimitive::kSourceGraphic
                                     : static_cast<int16_t>(prims.size() - 1);
            return SvgFilterPrimitive::kSourceGraphic;
        }
        return prims.empty() ? SvgFilterPrimitive::kSourceGraphic : static_cast<int16_t>(prims.size() - 1);
    };

    for (const dom::Element* fe : filter->children()) {
        const std::string tag = lower(fe->tagName());
        if (tag.rfind("fe", 0) != 0) continue;
        SvgFilterPrimitive p;
        p.in = inputOf(fe, "in");
        p.in2 = inputOf(fe, "in2");
        std::string space = attrOf(fe, "color-interpolation-filters");
        if (space.empty() || space == "auto" || space == "inherit") space = filterSpace;
        p.linearRGB = space != "sRGB";

        // Subregion: the filter region, narrowed by any x/y/width/height.
        float sx = region.fLeft, sy = region.fTop, sw = region.width(), sh = region.height();
        auto sub = [&](const char* n, float& out, float bbStart, float bbSize, float vp, bool isSize) {
            const std::string& v = attrOf(fe, n);
            if (v.empty()) return;
            char* end = nullptr;
            float f = std::strtof(v.c_str(), &end);
            if (end == v.c_str()) return;
            const bool pct = *end == '%';
            if (primObb) out = (isSize ? 0.0f : bbStart) + (pct ? f / 100.0f : f) * bbSize;
            else out = pct ? f / 100.0f * vp : f;
        };
        sub("x", sx, bbox.fLeft, bbox.width(), ctx.vpW, false);
        sub("y", sy, bbox.fTop, bbox.height(), ctx.vpH, false);
        sub("width", sw, 0, bbox.width(), ctx.vpW, true);
        sub("height", sh, 0, bbox.height(), ctx.vpH, true);
        p.x = sx; p.y = sy; p.w = sw; p.h = sh;

        if (tag == "fegaussianblur") {
            p.kind = Kind::GaussianBlur;
            float a, b;
            numPair(fe, "stdDeviation", 0, a, b);
            p.p[0] = plx(a); p.p[1] = ply(b);
        } else if (tag == "feoffset") {
            p.kind = Kind::Offset;
            p.p[0] = plx(numAttr(fe, "dx", 0)); p.p[1] = ply(numAttr(fe, "dy", 0));
        } else if (tag == "fecolormatrix") {
            p.kind = Kind::ColorMatrix;
            const std::string type = attrOf(fe, "type");
            std::vector<float> v = parseNumberList(attrOf(fe, "values"));
            float m[20] = {1, 0, 0, 0, 0,  0, 1, 0, 0, 0,  0, 0, 1, 0, 0,  0, 0, 0, 1, 0};
            if (type == "saturate") render::SaturateMatrix(v.empty() ? 1.0f : v[0], m);
            else if (type == "hueRotate") render::HueRotateMatrix(v.empty() ? 0.0f : v[0], m);
            else if (type == "luminanceToAlpha") render::LuminanceToAlphaMatrix(m);
            else if (v.size() == 20) std::copy(v.begin(), v.end(), m);
            p.dataOffset = static_cast<uint32_t>(data.size());
            p.dataLen = 20;
            data.insert(data.end(), m, m + 20);
        } else if (tag == "feblend") {
            p.kind = Kind::Blend;
            p.mode = static_cast<uint8_t>(blendModeOf(attrOf(fe, "mode")));
        } else if (tag == "fecomposite") {
            p.kind = Kind::Composite;
            const std::string op = attrOf(fe, "operator");
            p.mode = op == "in" ? SvgFilterPrimitive::In
                   : op == "out" ? SvgFilterPrimitive::Out
                   : op == "atop" ? SvgFilterPrimitive::Atop
                   : op == "xor" ? SvgFilterPrimitive::Xor
                   : op == "arithmetic" ? SvgFilterPrimitive::Arithmetic
                                        : SvgFilterPrimitive::Over;
            p.p[0] = numAttr(fe, "k1", 0); p.p[1] = numAttr(fe, "k2", 0);
            p.p[2] = numAttr(fe, "k3", 0); p.p[3] = numAttr(fe, "k4", 0);
        } else if (tag == "feflood") {
            p.kind = Kind::Flood;
            p.color = colorProp(fe, "flood-color", "flood-opacity", "black");
        } else if (tag == "femerge") {
            p.kind = Kind::Merge;
            p.dataOffset = static_cast<uint32_t>(data.size());
            for (const dom::Element* node : fe->children()) {
                if (lower(node->tagName()) != "femergenode") continue;
                data.push_back(static_cast<float>(inputOf(node, "in")));
            }
            p.dataLen = static_cast<uint32_t>(data.size() - p.dataOffset);
        } else if (tag == "fedropshadow") {
            p.kind = Kind::DropShadow;
            p.p[0] = plx(numAttr(fe, "dx", 2)); p.p[1] = ply(numAttr(fe, "dy", 2));
            float a, b;
            numPair(fe, "stdDeviation", 2, a, b);
            p.p[2] = plx(a); p.p[3] = ply(b);
            p.color = colorProp(fe, "flood-color", "flood-opacity", "black");
        } else if (tag == "fecomponenttransfer") {
            p.kind = Kind::ComponentTransfer;
            p.dataOffset = static_cast<uint32_t>(data.size());
            const char* funcs[4] = {"fefuncr", "fefuncg", "fefuncb", "fefunca"};
            for (const char* fn : funcs) {
                const dom::Element* f = nullptr;
                for (const dom::Element* c : fe->children())
                    if (lower(c->tagName()) == fn) f = c;
                std::string type = f ? attrOf(f, "type") : std::string();
                if (type == "table" || type == "discrete") {
                    std::vector<float> tv = parseNumberList(attrOf(f, "tableValues"));
                    data.push_back(type == "table" ? 1.0f : 2.0f);
                    data.push_back(static_cast<float>(tv.size()));
                    data.insert(data.end(), tv.begin(), tv.end());
                } else if (type == "linear") {
                    data.insert(data.end(), {3.0f, 2.0f, numAttr(f, "slope", 1), numAttr(f, "intercept", 0)});
                } else if (type == "gamma") {
                    data.insert(data.end(), {4.0f, 3.0f, numAttr(f, "amplitude", 1), numAttr(f, "exponent", 1),
                                             numAttr(f, "offset", 0)});
                } else {
                    data.insert(data.end(), {0.0f, 0.0f});
                }
            }
            p.dataLen = static_cast<uint32_t>(data.size() - p.dataOffset);
        } else if (tag == "femorphology") {
            p.kind = Kind::Morphology;
            p.mode = attrOf(fe, "operator") == "dilate" ? 1 : 0;
            float a, b;
            numPair(fe, "radius", 0, a, b);
            p.p[0] = plx(a); p.p[1] = ply(b);
        } else if (tag == "feturbulence") {
            p.kind = Kind::Turbulence;
            p.mode = attrOf(fe, "type") == "fractalNoise" ? 1 : 0;
            float a, b;
            numPair(fe, "baseFrequency", 0, a, b);
            p.p[0] = a; p.p[1] = b;
            p.p[2] = numAttr(fe, "numOctaves", 1);
            p.p[3] = numAttr(fe, "seed", 0);
            p.p[4] = attrOf(fe, "stitchTiles") == "stitch" ? 1.0f : 0.0f;
        } else if (tag == "fetile") {
            p.kind = Kind::Tile;
        } else if (tag == "fedisplacementmap") {
            p.kind = Kind::DisplacementMap;
            p.p[0] = plx(numAttr(fe, "scale", 0));
            p.mode = static_cast<uint8_t>(channelOf(attrOf(fe, "xChannelSelector")) |
                                          (channelOf(attrOf(fe, "yChannelSelector")) << 4));
        } else if (tag == "fediffuselighting" || tag == "fespecularlighting") {
            const bool spec = tag == "fespecularlighting";
            p.kind = spec ? Kind::SpecularLighting : Kind::DiffuseLighting;
            p.p[0] = numAttr(fe, "surfaceScale", 1);
            p.p[1] = numAttr(fe, spec ? "specularConstant" : "diffuseConstant", 1);
            p.p[2] = numAttr(fe, "specularExponent", 1);
            p.color = colorProp(fe, "lighting-color", nullptr, "white");
            for (const dom::Element* l : fe->children()) {
                const std::string lt = lower(l->tagName());
                if (lt == "fedistantlight") {
                    p.lightKind = 0;
                    p.light[0] = numAttr(l, "azimuth", 0);
                    p.light[1] = numAttr(l, "elevation", 0);
                } else if (lt == "fepointlight" || lt == "fespotlight") {
                    p.lightKind = lt == "fepointlight" ? 1 : 2;
                    p.light[0] = numAttr(l, "x", 0); p.light[1] = numAttr(l, "y", 0);
                    p.light[2] = numAttr(l, "z", 0);
                    p.light[3] = numAttr(l, "pointsAtX", 0); p.light[4] = numAttr(l, "pointsAtY", 0);
                    p.light[5] = numAttr(l, "pointsAtZ", 0);
                    p.light[6] = numAttr(l, "specularExponent", 1);
                    p.light[7] = numAttr(l, "limitingConeAngle", 0);
                } else {
                    continue;
                }
                break;
            }
        } else if (tag == "feimage") {
            p.kind = Kind::Flood;
            p.color = bromath::Color{0, 0, 0, 0};
            const dom::Element* target = hrefTarget(fe, ctx.root);
            if (target && ctx.depth < SvgPaintCtx::kMaxDepth && p.w > 0 && p.h > 0) {
                const int rw = std::clamp(static_cast<int>(std::ceil(p.w)), 1, 512);
                const int rh = std::clamp(static_cast<int>(std::ceil(p.h)), 1, 512);
                render::RasterRenderer rr;
                rr.beginFrame(rw, rh);
                rr.clear(bromath::Color{0, 0, 0, 0});
                rr.scale(static_cast<float>(rw) / p.w, static_cast<float>(rh) / p.h);
                if (attrOf(fe, "x").empty() && attrOf(fe, "y").empty())
                    rr.translate(-p.x, -p.y);
                if (primObb)
                    rr.scale(bbox.width(), bbox.height());
                SvgPaintCtx subCtx = ctx;
                subCtx.r = &rr;
                subCtx.depth = ctx.depth + 1;
                const std::string ttag = lower(target->tagName());
                if (ttag == "symbol") {
                    const std::string& vb = attrOf(target, "viewBox");
                    if (!vb.empty()) {
                        SkMatrix vm = viewportMatrix(p.w, p.h, vb, attrOf(target, "preserveAspectRatio"));
                        rr.concat(vm.getScaleX(), vm.getSkewY(), vm.getSkewX(),
                                  vm.getScaleY(), vm.getTranslateX(), vm.getTranslateY());
                    }
                    for (const dom::Element* c : target->children()) svgPaintNode(subCtx, c);
                } else {
                    svgPaintNode(subCtx, target);
                }
                rr.endFrame();
                if (rr.surface()) {
                    std::vector<uint8_t> px(static_cast<size_t>(rw) * static_cast<size_t>(rh) * 4, 0);
                    const SkImageInfo info = SkImageInfo::Make(rw, rh, kRGBA_8888_SkColorType, kUnpremul_SkAlphaType);
                    if (rr.surface()->readPixels(info, px.data(), static_cast<size_t>(rw) * 4, 0, 0))
                        packRgbaToFloats(px.data(), rw, rh, p, data, p.x, p.y, p.w, p.h);
                }
            } else if (!target && ctx.images && *ctx.images && p.w > 0 && p.h > 0) {
                const std::string& href = hrefOf(fe);
                std::shared_ptr<const render::DecodedImage> img = href.empty() ? nullptr : (*ctx.images)(href);
                if (img) {
                    if (img->isSvg && img->rgba.empty()) {
                        const int rw = std::clamp(static_cast<int>(std::ceil(p.w)), 1, 512);
                        const int rh = std::clamp(static_cast<int>(std::ceil(p.h)), 1, 512);
                        render::RasterRenderer rr;
                        rr.beginFrame(rw, rh);
                        rr.clear(bromath::Color{0, 0, 0, 0});
                        paintSvgImage(&rr, *img, 0, 0, static_cast<float>(rw), static_cast<float>(rh));
                        rr.endFrame();
                        if (rr.surface()) {
                            std::vector<uint8_t> px(static_cast<size_t>(rw) * static_cast<size_t>(rh) * 4, 0);
                            const SkImageInfo info = SkImageInfo::Make(rw, rh, kRGBA_8888_SkColorType, kUnpremul_SkAlphaType);
                            if (rr.surface()->readPixels(info, px.data(), static_cast<size_t>(rw) * 4, 0, 0))
                                packRgbaToFloats(px.data(), rw, rh, p, data, p.x, p.y, p.w, p.h);
                        }
                    } else if (!img->rgba.empty() && img->width > 0 && img->height > 0) {
                        char vb[96];
                        std::snprintf(vb, sizeof vb, "0 0 %d %d", img->width, img->height);
                        SkMatrix m = viewportMatrix(p.w, p.h, vb, attrOf(fe, "preserveAspectRatio"));
                        const SkRect dst = m.mapRect(SkRect::MakeWH(static_cast<float>(img->width),
                                                                    static_cast<float>(img->height)))
                                            .makeOffset(p.x, p.y);
                        packRgbaToFloats(img->rgba.data(), img->width, img->height, p, data,
                                         dst.fLeft, dst.fTop, dst.width(), dst.height());
                    }
                }
            }
        } else {
            p.kind = Kind::Passthrough;
        }

        const std::string& result = attrOf(fe, "result");
        prims.push_back(p);
        if (!result.empty()) results[result] = static_cast<int16_t>(prims.size() - 1);
    }
    return true;
}

} // namespace bro::layout
