#include "render/filter_chain.h"

#include <algorithm>
#include <cmath>

#include <include/core/SkCanvas.h>
#include <include/core/SkColor.h>
#include <include/core/SkRRect.h>
#include <include/core/SkColorFilter.h>
#include <include/core/SkPaint.h>
#include <include/effects/SkImageFilters.h>
#include <include/effects/SkLumaColorFilter.h>
#include <include/effects/SkPerlinNoiseShader.h>
#include <include/core/SkPoint3.h>
#include <include/core/SkShader.h>
#include <include/core/SkRect.h>

#include <vector>

namespace bro::render {

sk_sp<SkImageFilter> BuildSkImageFilterChain(std::span<const CssFilterParams> filters) {
    sk_sp<SkImageFilter> result;
    for (const auto& f : filters) {
        switch (f.kind) {
            case CssFilterParams::Blur: {
                // CSS filter blur(radius): the radius IS the Gaussian
                // stdDeviation (Filter Effects §blur), so sigma = radius.
                // (This differs from box-shadow / drop-shadow, whose blur
                // radius maps to stdDeviation = radius / 2 — see DropShadow.)
                float sigma = f.a;
                result = SkImageFilters::Blur(sigma, sigma, std::move(result));
                break;
            }
            case CssFilterParams::Brightness: {
                float v = f.a;
                float m[20] = {
                    v, 0, 0, 0, 0,
                    0, v, 0, 0, 0,
                    0, 0, v, 0, 0,
                    0, 0, 0, 1, 0
                };
                auto cf = SkColorFilters::Matrix(m);
                result = SkImageFilters::ColorFilter(std::move(cf), std::move(result));
                break;
            }
            case CssFilterParams::Contrast: {
                float v = f.a;
                float t = 0.5f * (1.0f - v);
                float m[20] = {
                    v, 0, 0, 0, t,
                    0, v, 0, 0, t,
                    0, 0, v, 0, t,
                    0, 0, 0, 1, 0
                };
                auto cf = SkColorFilters::Matrix(m);
                result = SkImageFilters::ColorFilter(std::move(cf), std::move(result));
                break;
            }
            case CssFilterParams::Grayscale: {
                float v = std::clamp(f.a, 0.0f, 1.0f);
                float inv = 1.0f - v;
                float m[20] = {
                    0.2126f + 0.7874f * inv, 0.7152f - 0.7152f * inv, 0.0722f - 0.0722f * inv, 0, 0,
                    0.2126f - 0.2126f * inv, 0.7152f + 0.2848f * inv, 0.0722f - 0.0722f * inv, 0, 0,
                    0.2126f - 0.2126f * inv, 0.7152f - 0.7152f * inv, 0.0722f + 0.9278f * inv, 0, 0,
                    0, 0, 0, 1, 0
                };
                auto cf = SkColorFilters::Matrix(m);
                result = SkImageFilters::ColorFilter(std::move(cf), std::move(result));
                break;
            }
            case CssFilterParams::Sepia: {
                float v = std::clamp(f.a, 0.0f, 1.0f);
                float inv = 1.0f - v;
                float m[20] = {
                    0.393f + 0.607f * inv, 0.769f - 0.769f * inv, 0.189f - 0.189f * inv, 0, 0,
                    0.349f - 0.349f * inv, 0.686f + 0.314f * inv, 0.168f - 0.168f * inv, 0, 0,
                    0.272f - 0.272f * inv, 0.534f - 0.534f * inv, 0.131f + 0.869f * inv, 0, 0,
                    0, 0, 0, 1, 0
                };
                auto cf = SkColorFilters::Matrix(m);
                result = SkImageFilters::ColorFilter(std::move(cf), std::move(result));
                break;
            }
            case CssFilterParams::Saturate: {
                float m[20];
                SaturateMatrix(f.a, m);
                auto cf = SkColorFilters::Matrix(m);
                result = SkImageFilters::ColorFilter(std::move(cf), std::move(result));
                break;
            }
            case CssFilterParams::HueRotate: {
                float m[20];
                HueRotateMatrix(f.a, m);
                auto cf = SkColorFilters::Matrix(m);
                result = SkImageFilters::ColorFilter(std::move(cf), std::move(result));
                break;
            }
            case CssFilterParams::Invert: {
                float v = std::clamp(f.a, 0.0f, 1.0f);
                float s = 1.0f - 2.0f * v;
                float t = v;
                float m[20] = {
                    s, 0, 0, 0, t,
                    0, s, 0, 0, t,
                    0, 0, s, 0, t,
                    0, 0, 0, 1, 0
                };
                auto cf = SkColorFilters::Matrix(m);
                result = SkImageFilters::ColorFilter(std::move(cf), std::move(result));
                break;
            }
            case CssFilterParams::Opacity: {
                float v = f.a;
                float m[20] = {
                    1, 0, 0, 0, 0,
                    0, 1, 0, 0, 0,
                    0, 0, 1, 0, 0,
                    0, 0, 0, v, 0
                };
                auto cf = SkColorFilters::Matrix(m);
                result = SkImageFilters::ColorFilter(std::move(cf), std::move(result));
                break;
            }
            case CssFilterParams::DropShadow: {
                // shadowColor is a linear-RGB float Color; encode to 8-bit sRGB
                // (gamma on RGB, linear alpha) before packing into an SkColor.
                // Passing the raw floats made alpha 0.9 truncate to 0 — an
                // invisible shadow.
                bromath::Color8 c8 = bromath::ctoColor8(f.shadowColor);
                SkColor skc = SkColorSetARGB(c8.a, c8.r, c8.g, c8.b);
                result = SkImageFilters::DropShadow(f.dx, f.dy,
                                                    f.blur / 2.0f, f.blur / 2.0f,
                                                    skc, std::move(result));
                break;
            }
        }
    }
    return result;
}

SkBlendMode toSkBlendMode(BlendMode mode) {
    switch (mode) {
        case BlendMode::Multiply:   return SkBlendMode::kMultiply;
        case BlendMode::Screen:     return SkBlendMode::kScreen;
        case BlendMode::Overlay:    return SkBlendMode::kOverlay;
        case BlendMode::Darken:     return SkBlendMode::kDarken;
        case BlendMode::Lighten:    return SkBlendMode::kLighten;
        case BlendMode::ColorDodge: return SkBlendMode::kColorDodge;
        case BlendMode::ColorBurn:  return SkBlendMode::kColorBurn;
        case BlendMode::HardLight:  return SkBlendMode::kHardLight;
        case BlendMode::SoftLight:  return SkBlendMode::kSoftLight;
        case BlendMode::Difference: return SkBlendMode::kDifference;
        case BlendMode::Exclusion:  return SkBlendMode::kExclusion;
        case BlendMode::Hue:        return SkBlendMode::kHue;
        case BlendMode::Saturation: return SkBlendMode::kSaturation;
        case BlendMode::Color:      return SkBlendMode::kColor;
        case BlendMode::Luminosity: return SkBlendMode::kLuminosity;
        case BlendMode::Normal:
        default:                    return SkBlendMode::kSrcOver;
    }
}

void DrawSkBackdropFilter(SkCanvas* canvas, std::span<const CssFilterParams> filters, const SkRRect& clip,
                          float opacity) {
    if (!canvas || clip.isEmpty() || !(opacity > 0.0f)) return;
    sk_sp<SkImageFilter> filter = BuildSkImageFilterChain(filters);
    if (!filter) return;
    canvas->save();
    if (clip.isRect()) canvas->clipRect(clip.rect(), true);
    else canvas->clipRRect(clip, true);
    // The layer starts as the backdrop run through `filter`; a blur samples
    // past the box's edge (clamped at the surface's), as CSS asks, and the
    // clip keeps its output inside the box.
    const SkRect bounds = clip.rect();
    SkPaint alpha;
    alpha.setAlphaf(std::min(opacity, 1.0f));
    canvas->saveLayer(SkCanvas::SaveLayerRec(&bounds, opacity < 1.0f ? &alpha : nullptr, filter.get(), 0));
    canvas->restore();
    canvas->restore();
}

void SaturateMatrix(float v, float m[20]) {
    const float r[20] = {
        0.2126f + 0.7874f * v, 0.7152f - 0.7152f * v, 0.0722f - 0.0722f * v, 0, 0,
        0.2126f - 0.2126f * v, 0.7152f + 0.2848f * v, 0.0722f - 0.0722f * v, 0, 0,
        0.2126f - 0.2126f * v, 0.7152f - 0.7152f * v, 0.0722f + 0.9278f * v, 0, 0,
        0, 0, 0, 1, 0
    };
    std::copy(r, r + 20, m);
}

void HueRotateMatrix(float degrees, float m[20]) {
    const float rad = degrees * 3.14159265f / 180.0f;
    const float cosA = std::cos(rad), sinA = std::sin(rad);
    const float r[20] = {
        0.213f + cosA * 0.787f - sinA * 0.213f,
        0.715f - cosA * 0.715f - sinA * 0.715f,
        0.072f - cosA * 0.072f + sinA * 0.928f, 0, 0,
        0.213f - cosA * 0.213f + sinA * 0.143f,
        0.715f + cosA * 0.285f + sinA * 0.140f,
        0.072f - cosA * 0.072f - sinA * 0.283f, 0, 0,
        0.213f - cosA * 0.213f - sinA * 0.787f,
        0.715f - cosA * 0.715f + sinA * 0.715f,
        0.072f + cosA * 0.928f + sinA * 0.072f, 0, 0,
        0, 0, 0, 1, 0
    };
    std::copy(r, r + 20, m);
}

void LuminanceToAlphaMatrix(float m[20]) {
    const float r[20] = {
        0, 0, 0, 0, 0,
        0, 0, 0, 0, 0,
        0, 0, 0, 0, 0,
        0.2125f, 0.7154f, 0.0721f, 0, 0
    };
    std::copy(r, r + 20, m);
}

void SetMaskLayerPaint(MaskMode mode, SkPaint& paint) {
    paint.setBlendMode(SkBlendMode::kDstIn);
    if (mode == MaskMode::Luminance) paint.setColorFilter(SkLumaColorFilter::Make());
}

namespace {

using SvgKind = SvgFilterPrimitive::Kind;

SkColor svgColor(const bromath::Color& c) {
    bromath::Color8 c8 = bromath::ctoColor8(c);
    return SkColorSetARGB(c8.a, c8.r, c8.g, c8.b);
}

// One evaluated node: its filter (null = the source graphic, as Skia reads a
// null input) and whether its pixels are linearRGB.
struct SvgNode {
    sk_sp<SkImageFilter> f;
    bool linear = false;
    SkRect sub = SkRect::MakeEmpty();
};

// A 256-entry transfer table for one channel from its encoded function:
// type (0 identity, 1 table, 2 discrete, 3 linear, 4 gamma), count, values.
// Returns the number of floats consumed.
size_t transferTable(std::span<const float> d, size_t at, uint8_t out[256], bool& identity) {
    identity = true;
    if (at + 2 > d.size()) { for (int i = 0; i < 256; ++i) out[i] = static_cast<uint8_t>(i); return 0; }
    const int type = static_cast<int>(d[at]);
    const size_t n = static_cast<size_t>(d[at + 1]);
    const size_t used = 2 + n;
    std::span<const float> v = d.subspan(at + 2, std::min(n, d.size() - at - 2));
    for (int i = 0; i < 256; ++i) {
        const float c = i / 255.0f;
        float r = c;
        if (type == 1 && v.size() >= 1) {
            if (v.size() == 1) r = v[0];
            else {
                const float pos = c * static_cast<float>(v.size() - 1);
                const size_t k = std::min(static_cast<size_t>(pos), v.size() - 2);
                r = v[k] + (pos - static_cast<float>(k)) * (v[k + 1] - v[k]);
            }
        } else if (type == 2 && !v.empty()) {
            const size_t k = std::min(static_cast<size_t>(c * static_cast<float>(v.size())), v.size() - 1);
            r = v[k];
        } else if (type == 3 && v.size() >= 2) {
            r = v[0] * c + v[1];
        } else if (type == 4 && v.size() >= 3) {
            r = v[0] * std::pow(c, v[1]) + v[2];
        }
        out[i] = static_cast<uint8_t>(std::lround(std::clamp(r, 0.0f, 1.0f) * 255.0f));
    }
    identity = type == 0 || (type == 1 && v.empty()) || (type == 2 && v.empty());
    return used;
}

sk_sp<SkImageFilter> sourceAlpha() {
    const float m[20] = {0, 0, 0, 0, 0,  0, 0, 0, 0, 0,  0, 0, 0, 0, 0,  0, 0, 0, 1, 0};
    return SkImageFilters::ColorFilter(SkColorFilters::Matrix(m), nullptr);
}

// `n` re-expressed in the wanted space.
sk_sp<SkImageFilter> inSpace(const SvgNode& n, bool wantLinear) {
    if (n.linear == wantLinear) return n.f;
    return SkImageFilters::ColorFilter(wantLinear ? SkColorFilters::SRGBToLinearGamma()
                                                  : SkColorFilters::LinearToSRGBGamma(),
                                       n.f);
}

SkColorChannel channelOf(int c) {
    switch (c & 3) {
        case 0: return SkColorChannel::kR;
        case 1: return SkColorChannel::kG;
        case 2: return SkColorChannel::kB;
        default: return SkColorChannel::kA;
    }
}

}  // namespace

sk_sp<SkImageFilter> BuildSvgFilterGraph(std::span<const SvgFilterPrimitive> prims,
                                         std::span<const float> data, const SkRect& region) {
    // SourceGraphic is sRGB; SourceAlpha has no color, so it is in every space.
    const SvgNode source{nullptr, false, region};
    const SvgNode alpha{sourceAlpha(), false, region};
    std::vector<SvgNode> nodes;
    nodes.reserve(prims.size());
    auto input = [&](int16_t idx, bool wantLinear) -> sk_sp<SkImageFilter> {
        if (idx == SvgFilterPrimitive::kSourceAlpha) return alpha.f;
        if (idx >= 0 && static_cast<size_t>(idx) < nodes.size()) return inSpace(nodes[idx], wantLinear);
        return inSpace(source, wantLinear);
    };
    auto subOf = [&](int16_t idx) -> SkRect {
        if (idx >= 0 && static_cast<size_t>(idx) < nodes.size()) return nodes[idx].sub;
        return region;
    };
    auto dataAt = [&](const SvgFilterPrimitive& p) -> std::span<const float> {
        if (p.dataOffset >= data.size()) return {};
        return data.subspan(p.dataOffset, std::min<size_t>(p.dataLen, data.size() - p.dataOffset));
    };

    for (const SvgFilterPrimitive& p : prims) {
        const bool lin = p.linearRGB;
        const SkRect sub = SkRect::MakeXYWH(p.x, p.y, p.w, p.h);
        const SkImageFilters::CropRect crop(sub);
        SvgNode out;
        out.linear = lin;
        out.sub = sub;
        switch (p.kind) {
            case SvgKind::GaussianBlur: {
                const float sx = std::max(0.0f, p.p[0]), sy = std::max(0.0f, p.p[1]);
                if (sx <= 0 && sy <= 0) out.f = SkImageFilters::Offset(0, 0, input(p.in, lin), crop);
                else out.f = SkImageFilters::Blur(sx, sy, SkTileMode::kDecal, input(p.in, lin), crop);
                break;
            }
            case SvgKind::Offset:
                out.f = SkImageFilters::Offset(p.p[0], p.p[1], input(p.in, lin), crop);
                break;
            case SvgKind::ColorMatrix: {
                auto v = dataAt(p);
                if (v.size() >= 20) {
                    out.f = SkImageFilters::ColorFilter(SkColorFilters::Matrix(v.data()), input(p.in, lin), crop);
                } else {
                    out.f = SkImageFilters::Offset(0, 0, input(p.in, lin), crop);
                }
                break;
            }
            case SvgKind::Blend:
                out.f = SkImageFilters::Blend(toSkBlendMode(static_cast<BlendMode>(p.mode)),
                                              input(p.in2, lin), input(p.in, lin), crop);
                break;
            case SvgKind::Composite: {
                if (p.mode == SvgFilterPrimitive::Arithmetic) {
                    out.f = SkImageFilters::Arithmetic(p.p[0], p.p[1], p.p[2], p.p[3], true,
                                                       input(p.in2, lin), input(p.in, lin), crop);
                } else {
                    SkBlendMode m = SkBlendMode::kSrcOver;
                    switch (p.mode) {
                        case SvgFilterPrimitive::In:   m = SkBlendMode::kSrcIn; break;
                        case SvgFilterPrimitive::Out:  m = SkBlendMode::kSrcOut; break;
                        case SvgFilterPrimitive::Atop: m = SkBlendMode::kSrcATop; break;
                        case SvgFilterPrimitive::Xor:  m = SkBlendMode::kXor; break;
                        default: break;
                    }
                    out.f = SkImageFilters::Blend(m, input(p.in2, lin), input(p.in, lin), crop);
                }
                break;
            }
            case SvgKind::Flood:
                // The color is given in sRGB; it is the node's pixels as is.
                out.f = SkImageFilters::Shader(SkShaders::Color(svgColor(p.color)), crop);
                out.linear = false;
                break;
            case SvgKind::Merge: {
                std::vector<sk_sp<SkImageFilter>> ins;
                for (float idx : dataAt(p)) ins.push_back(input(static_cast<int16_t>(idx), lin));
                if (ins.empty()) out.f = SkImageFilters::Shader(SkShaders::Color(SK_ColorTRANSPARENT), crop);
                else out.f = SkImageFilters::Merge(ins.data(), static_cast<int>(ins.size()), crop);
                break;
            }
            case SvgKind::DropShadow:
                out.f = SkImageFilters::DropShadow(p.p[0], p.p[1], std::max(0.0f, p.p[2]),
                                                   std::max(0.0f, p.p[3]), svgColor(p.color),
                                                   input(p.in, lin), crop);
                break;
            case SvgKind::ComponentTransfer: {
                auto v = dataAt(p);
                uint8_t t[4][256];
                bool ident[4];
                size_t at = 0;
                for (int ch = 0; ch < 4; ++ch) at += transferTable(v, at, t[ch], ident[ch]);
                if (ident[0] && ident[1] && ident[2] && ident[3]) {
                    out.f = SkImageFilters::Offset(0, 0, input(p.in, lin), crop);
                } else {
                    // Data order is R, G, B, A; TableARGB takes A first.
                    out.f = SkImageFilters::ColorFilter(SkColorFilters::TableARGB(t[3], t[0], t[1], t[2]),
                                                        input(p.in, lin), crop);
                }
                break;
            }
            case SvgKind::Morphology:
                if (p.mode == 1) out.f = SkImageFilters::Dilate(p.p[0], p.p[1], input(p.in, lin), crop);
                else out.f = SkImageFilters::Erode(p.p[0], p.p[1], input(p.in, lin), crop);
                break;
            case SvgKind::Turbulence: {
                const int octaves = std::clamp(static_cast<int>(p.p[2]), 1, 8);
                SkISize tile = SkISize::Make(static_cast<int>(sub.width()), static_cast<int>(sub.height()));
                const SkISize* tp = p.p[4] != 0 ? &tile : nullptr;
                sk_sp<SkShader> sh = p.mode == 1
                    ? SkShaders::MakeFractalNoise(p.p[0], p.p[1], octaves, p.p[3], tp)
                    : SkShaders::MakeTurbulence(p.p[0], p.p[1], octaves, p.p[3], tp);
                // Noise is generated in the primitive's space as is.
                out.f = SkImageFilters::Shader(sh, crop);
                break;
            }
            case SvgKind::Tile:
                out.f = SkImageFilters::Tile(subOf(p.in), sub, input(p.in, lin));
                break;
            case SvgKind::DisplacementMap:
                out.f = SkImageFilters::DisplacementMap(channelOf(p.mode & 0xF), channelOf(p.mode >> 4), p.p[0],
                                                        input(p.in2, lin), input(p.in, lin), crop);
                break;
            case SvgKind::DiffuseLighting:
            case SvgKind::SpecularLighting: {
                const bool spec = p.kind == SvgKind::SpecularLighting;
                const SkColor lc = svgColor(p.color);
                sk_sp<SkImageFilter> in = input(p.in, lin);
                const float* L = p.light;
                if (p.lightKind == 0) {
                    const float az = L[0] * 3.14159265f / 180.0f, el = L[1] * 3.14159265f / 180.0f;
                    const SkPoint3 dir = SkPoint3::Make(std::cos(az) * std::cos(el), std::sin(az) * std::cos(el),
                                                        std::sin(el));
                    out.f = spec ? SkImageFilters::DistantLitSpecular(dir, lc, p.p[0], p.p[1], p.p[2], in, crop)
                                 : SkImageFilters::DistantLitDiffuse(dir, lc, p.p[0], p.p[1], in, crop);
                } else if (p.lightKind == 1) {
                    const SkPoint3 loc = SkPoint3::Make(L[0], L[1], L[2]);
                    out.f = spec ? SkImageFilters::PointLitSpecular(loc, lc, p.p[0], p.p[1], p.p[2], in, crop)
                                 : SkImageFilters::PointLitDiffuse(loc, lc, p.p[0], p.p[1], in, crop);
                } else {
                    const SkPoint3 loc = SkPoint3::Make(L[0], L[1], L[2]);
                    const SkPoint3 at = SkPoint3::Make(L[3], L[4], L[5]);
                    const float cone = L[7] > 0 ? L[7] : 90.0f;
                    out.f = spec ? SkImageFilters::SpotLitSpecular(loc, at, L[6], cone, lc, p.p[0], p.p[1], p.p[2],
                                                                   in, crop)
                                 : SkImageFilters::SpotLitDiffuse(loc, at, L[6], cone, lc, p.p[0], p.p[1], in, crop);
                }
                break;
            }
            case SvgKind::Passthrough:
            default:
                out.f = SkImageFilters::Offset(0, 0, input(p.in, lin), crop);
                break;
        }
        nodes.push_back(std::move(out));
    }
    // A filter with no primitives draws nothing (Filter Effects §filter).
    if (nodes.empty()) return SkImageFilters::Shader(SkShaders::Color(SK_ColorTRANSPARENT));
    return SkImageFilters::Crop(region, inSpace(nodes.back(), false));
}

} // namespace bro::render
