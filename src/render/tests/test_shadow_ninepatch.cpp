// The nine-patch box-shadow (render/shadow_ninepatch.h) against Skia's own
// mask blur of the same shape: the two must agree pixel for pixel, within
// rounding, for shapes Ganesh would blur on the CPU, at the sizes and blurs a
// window shadow has, under a translate and a scale.

#include "render/shadow_ninepatch.h"

#include <include/core/SkBlurTypes.h>
#include <include/core/SkCanvas.h>
#include <include/core/SkColor.h>
#include <include/core/SkImageInfo.h>
#include <include/core/SkMaskFilter.h>
#include <include/core/SkPaint.h>
#include <include/core/SkPath.h>
#include <include/core/SkPixmap.h>
#include <include/core/SkRRect.h>
#include <include/core/SkSurface.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>

namespace {

int gFailures = 0;

#define CHECK(cond)                                                                  \
    do {                                                                             \
        if (!(cond)) {                                                               \
            std::cerr << "  FAIL: " #cond " (line " << __LINE__ << ")" << std::endl; \
            ++gFailures;                                                             \
        }                                                                            \
    } while (0)

SkRRect shape(float x, float y, float w, float h, SkVector ul, SkVector ur, SkVector lr, SkVector ll) {
    SkVector radii[4] = {ul, ur, lr, ll};
    SkRRect r;
    r.setRectRadii(SkRect::MakeXYWH(x, y, w, h), radii);
    return r;
}

struct Diff {
    int max = 0;
    double mean = 0;
};

// Draws `rr` blurred both ways on `size` surfaces under scale `s` and
// translate (tx, ty); compares the alpha.
Diff compare(const SkRRect& rr, float sigma, int size, float s, float tx, float ty, bool* drew) {
    auto ref = SkSurfaces::Raster(SkImageInfo::MakeN32Premul(size, size));
    auto got = SkSurfaces::Raster(SkImageInfo::MakeN32Premul(size, size));
    const SkColor color = SkColorSetARGB(255, 0, 0, 0);
    for (auto* surf : {ref.get(), got.get()}) {
        surf->getCanvas()->clear(SK_ColorTRANSPARENT);
        surf->getCanvas()->translate(tx, ty);
        surf->getCanvas()->scale(s, s);
    }
    SkPaint p;
    p.setAntiAlias(true);
    p.setColor(color);
    p.setMaskFilter(SkMaskFilter::MakeBlur(kNormal_SkBlurStyle, sigma));
    // As a path: Skia's raster drawRRect has a nine-patch shortcut of its
    // own (whole-pixel placed); a path is blurred whole, where it lies.
    ref->getCanvas()->drawPath(SkPath::RRect(rr), p);
    *drew = bro::render::drawBlurredRRectNinePatch(got->getCanvas(), rr, sigma, color);

    SkPixmap a, b;
    ref->peekPixels(&a);
    got->peekPixels(&b);
    Diff d;
    double sum = 0;
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            const int da = std::abs(static_cast<int>(SkColorGetA(a.getColor(x, y))) -
                                    static_cast<int>(SkColorGetA(b.getColor(x, y))));
            d.max = std::max(d.max, da);
            sum += da;
        }
    }
    d.mean = sum / (static_cast<double>(size) * size);
    return d;
}

}  // namespace

int main() {
    std::cout << "=== Running test_shadow_ninepatch ===" << std::endl;

    // Which shapes take the nine-patch: those Ganesh blurs on the CPU.
    CHECK(!bro::render::shadowNeedsNinePatch(SkRRect::MakeRect(SkRect::MakeWH(100, 50))));
    CHECK(!bro::render::shadowNeedsNinePatch(SkRRect::MakeRectXY(SkRect::MakeWH(100, 50), 12, 12)));
    CHECK(bro::render::shadowNeedsNinePatch(SkRRect::MakeRectXY(SkRect::MakeWH(100, 50), 12, 6)));
    CHECK(bro::render::shadowNeedsNinePatch(shape(0, 0, 100, 50, {12, 12}, {12, 12}, {0, 0}, {0, 0})));

    struct Case {
        const char* what;
        SkRRect rr;
        float sigma;
        int size;
        float scale, tx, ty;
        int maxDiff = 1;
    };
    const Case cases[] = {
        // helm's window: rounded at the top, a deep shadow
        {"top-rounded, sigma 55", shape(200, 200, 500, 360, {14, 14}, {14, 14}, {0, 0}, {0, 0}), 55, 900, 1, 0, 0},
        {"top-rounded, sigma 12", shape(60, 60, 300, 200, {14, 14}, {14, 14}, {0, 0}, {0, 0}), 12, 420, 1, 0, 0},
        {"elliptical corners", shape(50, 50, 260, 180, {30, 12}, {30, 12}, {30, 12}, {30, 12}), 8, 360, 1, 0, 0},
        {"mixed corners", shape(80, 80, 240, 200, {4, 4}, {24, 24}, {0, 0}, {40, 20}), 16, 400, 1, 0, 0},
        // Off the pixel grid the mask is resampled (bilinear) where Skia blurs
        // the shape in place: close, not exact.
        {"fractional offset", shape(60, 60, 300, 200, {14, 14}, {14, 14}, {0, 0}, {0, 0}), 12, 420, 1, 0.37f, 0.61f, 12},
        {"scale 2", shape(30, 30, 150, 100, {7, 7}, {7, 7}, {0, 0}, {0, 0}), 6, 420, 2, 0, 0},
    };
    for (const Case& c : cases) {
        bool drew = false;
        const Diff d = compare(c.rr, c.sigma, c.size, c.scale, c.tx, c.ty, &drew);
        std::cout << "  " << c.what << ": drew=" << drew << " max diff " << d.max << " mean " << d.mean << std::endl;
        CHECK(drew);
        CHECK(d.max <= c.maxDiff);
        CHECK(d.mean < 0.5);
    }

    // What it declines (drawing nothing): a rotation, and a shape too small
    // for the middle of its edges to be clear of the corners' blur.
    {
        auto surf = SkSurfaces::Raster(SkImageInfo::MakeN32Premul(200, 200));
        surf->getCanvas()->rotate(10);
        CHECK(!bro::render::drawBlurredRRectNinePatch(surf->getCanvas(),
                                                      shape(50, 50, 100, 80, {8, 8}, {8, 8}, {0, 0}, {0, 0}), 6,
                                                      SK_ColorBLACK));
    }
    {
        auto surf = SkSurfaces::Raster(SkImageInfo::MakeN32Premul(200, 200));
        CHECK(!bro::render::drawBlurredRRectNinePatch(surf->getCanvas(),
                                                      shape(50, 50, 40, 30, {8, 8}, {8, 8}, {0, 0}, {0, 0}), 20,
                                                      SK_ColorBLACK));
    }

    if (gFailures == 0) {
        std::cout << "=== test_shadow_ninepatch: ALL TESTS PASSED ===" << std::endl;
        return 0;
    }
    std::cerr << "=== test_shadow_ninepatch: " << gFailures << " FAILURES ===" << std::endl;
    return 1;
}
