#include "render/filter_chain.h"

#include <algorithm>
#include <cmath>

#include <include/core/SkColor.h>
#include <include/core/SkColorFilter.h>
#include <include/effects/SkImageFilters.h>

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
                float v = f.a;
                float m[20] = {
                    0.2126f + 0.7874f * v, 0.7152f - 0.7152f * v, 0.0722f - 0.0722f * v, 0, 0,
                    0.2126f - 0.2126f * v, 0.7152f + 0.2848f * v, 0.0722f - 0.0722f * v, 0, 0,
                    0.2126f - 0.2126f * v, 0.7152f - 0.7152f * v, 0.0722f + 0.9278f * v, 0, 0,
                    0, 0, 0, 1, 0
                };
                auto cf = SkColorFilters::Matrix(m);
                result = SkImageFilters::ColorFilter(std::move(cf), std::move(result));
                break;
            }
            case CssFilterParams::HueRotate: {
                float rad = f.a * 3.14159265f / 180.0f;
                float cosA = std::cos(rad), sinA = std::sin(rad);
                float m[20] = {
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
                SkColor skc = SkColorSetARGB(f.shadowColor.a, f.shadowColor.r,
                                             f.shadowColor.g, f.shadowColor.b);
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

} // namespace bro::render
