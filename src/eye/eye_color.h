#pragma once

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <string>

namespace bro::eye::color {

struct RGB {
    uint8_t r = 0;
    uint8_t g = 0;
    uint8_t b = 0;
};

struct Oklab {
    double L = 0.0;
    double a = 0.0;
    double b = 0.0;
};

struct Oklch {
    double L = 0.0;
    double C = 0.0;
    double h = 0.0;  // degrees [0, 360)
};

inline double srgbToLinear(uint8_t u) noexcept {
    const double c = static_cast<double>(u) / 255.0;
    return (c <= 0.04045) ? (c / 12.92) : std::pow((c + 0.055) / 1.055, 2.4);
}

inline uint8_t linearToSrgb(double c) noexcept {
    c = std::clamp(c, 0.0, 1.0);
    const double s = (c <= 0.0031308) ? (12.92 * c) : (1.055 * std::pow(c, 1.0 / 2.4) - 0.055);
    return static_cast<uint8_t>(std::clamp(std::lround(s * 255.0), 0L, 255L));
}

inline Oklab srgbToOklab(uint8_t r, uint8_t g, uint8_t b) noexcept {
    const double lr = srgbToLinear(r);
    const double lg = srgbToLinear(g);
    const double lb = srgbToLinear(b);

    const double l = std::cbrt(0.4122214708 * lr + 0.5363325363 * lg + 0.0514459929 * lb);
    const double m = std::cbrt(0.2119034982 * lr + 0.6806995451 * lg + 0.1073969566 * lb);
    const double s = std::cbrt(0.0883024619 * lr + 0.2817188376 * lg + 0.6299787005 * lb);

    return {
        0.2104542553 * l + 0.7936177850 * m - 0.0040720468 * s,
        1.9779984951 * l - 2.4285922050 * m + 0.4505937099 * s,
        0.0259040371 * l + 0.7827717662 * m - 0.8086757660 * s,
    };
}

inline RGB oklabToSrgb(const Oklab& lab) noexcept {
    const double l_ = lab.L + 0.3963377774 * lab.a + 0.2158037573 * lab.b;
    const double m_ = lab.L - 0.1055613458 * lab.a - 0.0638541728 * lab.b;
    const double s_ = lab.L - 0.0894841775 * lab.a - 1.2914855480 * lab.b;

    const double l = l_ * l_ * l_;
    const double m = m_ * m_ * m_;
    const double s = s_ * s_ * s_;

    const double lr = +4.0767416621 * l - 3.3077115913 * m + 0.2309699292 * s;
    const double lg = -1.2684380046 * l + 2.6097574011 * m - 0.3413193965 * s;
    const double lb = -0.0041960863 * l - 0.7034186147 * m + 1.7076147010 * s;

    return {linearToSrgb(lr), linearToSrgb(lg), linearToSrgb(lb)};
}

inline Oklch oklabToOklch(const Oklab& lab) noexcept {
    const double C = std::sqrt(lab.a * lab.a + lab.b * lab.b);
    double h = 0.0;
    if (C > 1e-6) {
        h = std::atan2(lab.b, lab.a) * (180.0 / 3.14159265358979323846);
        if (h < 0.0) h += 360.0;
    }
    return {lab.L, C, h};
}

// Perceptual Oklab distance scaled to 0..100+ (JND ~ 1.0..2.0).
inline double oklabDeltaE(const Oklab& a, const Oklab& b) noexcept {
    const double dL = a.L - b.L;
    const double da = a.a - b.a;
    const double db = a.b - b.b;
    return 100.0 * std::sqrt(dL * dL + da * da + db * db);
}

inline double relativeLuminance(RGB c) noexcept {
    return 0.2126 * srgbToLinear(c.r) + 0.7152 * srgbToLinear(c.g) + 0.0722 * srgbToLinear(c.b);
}

inline double wcagContrastRatio(RGB c1, RGB c2) noexcept {
    const double l1 = relativeLuminance(c1);
    const double l2 = relativeLuminance(c2);
    const double hi = std::max(l1, l2);
    const double lo = std::min(l1, l2);
    return (hi + 0.05) / (lo + 0.05);
}

// Signed APCA-W3 Lc contrast (+ = dark text on light bg, - = light text on dark bg).
inline double apcaContrast(RGB fg, RGB bg) noexcept {
    auto screenY = [](RGB c) {
        const double r = std::pow(static_cast<double>(c.r) / 255.0, 2.4);
        const double g = std::pow(static_cast<double>(c.g) / 255.0, 2.4);
        const double b = std::pow(static_cast<double>(c.b) / 255.0, 2.4);
        double y = 0.2126729 * r + 0.7151522 * g + 0.0721750 * b;
        if (y < 0.022) y += std::pow(0.022 - y, 1.414);
        return y;
    };
    const double yTxt = screenY(fg);
    const double yBg = screenY(bg);
    if (std::abs(yBg - yTxt) < 0.0005) return 0.0;
    double Lc = 0.0;
    if (yBg > yTxt) {
        const double s = (std::pow(yBg, 0.56) - std::pow(yTxt, 0.57)) * 1.14;
        Lc = (s < 0.1) ? 0.0 : (s - 0.027) * 100.0;
    } else {
        const double s = (std::pow(yBg, 0.65) - std::pow(yTxt, 0.62)) * 1.14;
        Lc = (s > -0.1) ? 0.0 : (s + 0.027) * 100.0;
    }
    return Lc;
}

inline std::string rgbToHex(RGB c) {
    char buf[8];
    std::snprintf(buf, sizeof(buf), "#%02x%02x%02x", c.r, c.g, c.b);
    return std::string(buf);
}

inline uint8_t rec709Luma(uint8_t r, uint8_t g, uint8_t b) noexcept {
    return static_cast<uint8_t>((2126u * r + 7152u * g + 722u * b + 5000u) / 10000u);
}

inline RGB magmaColor(double t) noexcept {
    static constexpr double kStops[6][3] = {
        {0.00146, 0.00047, 0.01387},
        {0.23208, 0.05989, 0.43769},
        {0.55029, 0.16116, 0.50572},
        {0.87079, 0.28759, 0.40900},
        {0.99474, 0.62435, 0.42740},
        {0.98705, 0.99144, 0.74950},
    };
    t = std::clamp(t, 0.0, 1.0) * 5.0;
    const int idx = std::min(4, static_cast<int>(t));
    const double f = t - idx;
    const double r = kStops[idx][0] + (kStops[idx + 1][0] - kStops[idx][0]) * f;
    const double g = kStops[idx][1] + (kStops[idx + 1][1] - kStops[idx][1]) * f;
    const double b = kStops[idx][2] + (kStops[idx + 1][2] - kStops[idx][2]) * f;
    return {
        static_cast<uint8_t>(std::clamp(std::lround(r * 255.0), 0L, 255L)),
        static_cast<uint8_t>(std::clamp(std::lround(g * 255.0), 0L, 255L)),
        static_cast<uint8_t>(std::clamp(std::lround(b * 255.0), 0L, 255L)),
    };
}

}  // namespace bro::eye::color
