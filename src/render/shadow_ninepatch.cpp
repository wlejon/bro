#include "render/shadow_ninepatch.h"

#include <include/core/SkBlurTypes.h>
#include <include/core/SkCanvas.h>
#include <include/core/SkImage.h>
#include <include/core/SkImageInfo.h>
#include <include/core/SkMaskFilter.h>
#include <include/core/SkMatrix.h>
#include <include/core/SkPaint.h>
#include <include/core/SkRRect.h>
#include <include/core/SkSamplingOptions.h>
#include <include/core/SkSurface.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <mutex>

namespace bro::render {

namespace {

constexpr SkRRect::Corner kCorners[4] = {SkRRect::kUpperLeft_Corner, SkRRect::kUpperRight_Corner,
                                         SkRRect::kLowerRight_Corner, SkRRect::kLowerLeft_Corner};

// Masks by corner radii and sigma, in quarter device px. A handful of shadow
// shapes are on screen at once; the cap only bounds a pathological page.
using MaskKey = std::array<int, 9>;
constexpr size_t kMaxMasks = 48;

std::mutex& cacheMutex() {
    static std::mutex m;
    return m;
}
std::map<MaskKey, sk_sp<SkImage>>& cache() {
    static std::map<MaskKey, sk_sp<SkImage>> c;
    return c;
}

int quarter(float v) { return static_cast<int>(std::lround(v * 4.0f)); }

}  // namespace

bool shadowNeedsNinePatch(const SkRRect& rrect) {
    if (rrect.isRect() || rrect.isEmpty()) return false;
    const SkVector r0 = rrect.radii(SkRRect::kUpperLeft_Corner);
    if (r0.fX != r0.fY) return true;
    for (SkRRect::Corner c : kCorners)
        if (rrect.radii(c) != r0) return true;
    return false;
}

bool drawBlurredRRectNinePatch(SkCanvas* canvas, const SkRRect& rrect, float sigma, SkColor color) {
    if (!canvas || sigma <= 0.0f) return false;
    const SkMatrix m = canvas->getTotalMatrix();
    if (!m.isScaleTranslate() || m.getScaleX() <= 0.0f || std::fabs(m.getScaleX() - m.getScaleY()) > 1e-3f)
        return false;
    SkRRect dev;
    if (!rrect.transform(m, &dev)) return false;
    const float devSigma = sigma * m.getScaleX();

    // The corners' extents, and the blur's reach.
    const SkVector ul = dev.radii(SkRRect::kUpperLeft_Corner), ur = dev.radii(SkRRect::kUpperRight_Corner);
    const SkVector lr = dev.radii(SkRRect::kLowerRight_Corner), ll = dev.radii(SkRRect::kLowerLeft_Corner);
    const int L = static_cast<int>(std::ceil(std::max(ul.fX, ll.fX)));
    const int R = static_cast<int>(std::ceil(std::max(ur.fX, lr.fX)));
    const int T = static_cast<int>(std::ceil(std::max(ul.fY, ur.fY)));
    const int B = static_cast<int>(std::ceil(std::max(ll.fY, lr.fY)));
    const int reach = static_cast<int>(std::ceil(3.0f * devSigma));

    // The smallest shape with these corners whose middle column and row are
    // more than the blur's reach from any corner: there the profile is that
    // of a straight edge, the same as anywhere along the large shape's edge.
    const int ws = L + R + 2 * reach + 1;
    const int hs = T + B + 2 * reach + 1;
    const SkRect& rect = dev.rect();
    if (rect.width() < ws || rect.height() < hs) return false;
    const int W = ws + 2 * reach, H = hs + 2 * reach;
    const int cx = reach + L + reach, cy = reach + T + reach;

    MaskKey key{quarter(ul.fX), quarter(ul.fY), quarter(ur.fX), quarter(ur.fY), quarter(lr.fX),
                quarter(lr.fY), quarter(ll.fX), quarter(ll.fY), quarter(devSigma)};
    sk_sp<SkImage> mask;
    {
        std::lock_guard<std::mutex> lk(cacheMutex());
        auto it = cache().find(key);
        if (it != cache().end()) mask = it->second;
    }
    if (!mask) {
        sk_sp<SkSurface> surface = SkSurfaces::Raster(SkImageInfo::MakeA8(W, H));
        if (!surface) return false;
        SkVector radii[4] = {ul, ur, lr, ll};
        SkRRect small;
        small.setRectRadii(SkRect::MakeXYWH(static_cast<float>(reach), static_cast<float>(reach),
                                            static_cast<float>(ws), static_cast<float>(hs)),
                           radii);
        SkPaint p;
        p.setAntiAlias(true);
        p.setMaskFilter(SkMaskFilter::MakeBlur(kNormal_SkBlurStyle, devSigma));
        surface->getCanvas()->clear(SK_ColorTRANSPARENT);
        surface->getCanvas()->drawRRect(small, p);
        mask = surface->makeImageSnapshot();
        if (!mask) return false;
        std::lock_guard<std::mutex> lk(cacheMutex());
        if (cache().size() >= kMaxMasks) cache().clear();
        cache()[key] = mask;
    }

    // Nine pieces, in device space (the clip is already there). The mask's
    // shape edge sits `reach` in from its border, so the destination is the
    // shape outset by the same.
    const float xs[4] = {rect.fLeft - reach, rect.fLeft - reach + cx, rect.fRight + reach - (W - cx - 1),
                         rect.fRight + reach};
    const float ys[4] = {rect.fTop - reach, rect.fTop - reach + cy, rect.fBottom + reach - (H - cy - 1),
                         rect.fBottom + reach};
    const float sx[4] = {0.0f, static_cast<float>(cx), static_cast<float>(cx + 1), static_cast<float>(W)};
    const float sy[4] = {0.0f, static_cast<float>(cy), static_cast<float>(cy + 1), static_cast<float>(H)};

    SkPaint paint;
    paint.setColor(color);  // an alpha-only image draws in the paint's colour
    const SkSamplingOptions sampling(SkFilterMode::kLinear);
    canvas->save();
    canvas->resetMatrix();
    for (int j = 0; j < 3; ++j) {
        for (int i = 0; i < 3; ++i) {
            const SkRect src = SkRect::MakeLTRB(sx[i], sy[j], sx[i + 1], sy[j + 1]);
            const SkRect dst = SkRect::MakeLTRB(xs[i], ys[j], xs[i + 1], ys[j + 1]);
            if (dst.isEmpty()) continue;
            canvas->drawImageRect(mask, src, dst, sampling, &paint, SkCanvas::kStrict_SrcRectConstraint);
        }
    }
    canvas->restore();
    return true;
}

}  // namespace bro::render
