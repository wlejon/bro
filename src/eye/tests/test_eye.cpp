#include "eye/eye_color.h"
#include "eye/eye_compose.h"
#include "eye/eye_fit.h"
#include "eye/eye_measure.h"
#include "eye/eye_motion.h"
#include "eye/eye_visualize.h"
#if BRO_EYE_TEST_WITH_FORM
#include "eye/eye_form.h"
#include <bromesh/primitives/primitives.h>
#endif

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {

int gFailures = 0;

void check(bool cond, const char* msg) {
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", msg);
        ++gFailures;
    }
}

bro::eye::EyeImage makeSolid(int w, int h, uint8_t r, uint8_t g, uint8_t b) {
    bro::eye::EyeImage img{w, h, std::vector<uint8_t>(static_cast<size_t>(w) * h * 4)};
    for (size_t i = 0; i < static_cast<size_t>(w) * h; ++i) {
        img.rgba[i * 4 + 0] = r;
        img.rgba[i * 4 + 1] = g;
        img.rgba[i * 4 + 2] = b;
        img.rgba[i * 4 + 3] = 255;
    }
    return img;
}

bro::eye::EyeImage makeSteppedGradient(int w, int h, int levels) {
    bro::eye::EyeImage img{w, h, std::vector<uint8_t>(static_cast<size_t>(w) * h * 4)};
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const int band = (x * levels) / w;
            // Steps by 3 luma units with wide flat runs (>= 12 px) so staircase detector fires.
            const uint8_t v = static_cast<uint8_t>(std::clamp(40 + band * 3, 0, 255));
            const size_t i = (static_cast<size_t>(y) * w + x) * 4;
            img.rgba[i + 0] = v;
            img.rgba[i + 1] = v;
            img.rgba[i + 2] = v;
            img.rgba[i + 3] = 255;
        }
    }
    return img;
}

bro::eye::EyeImage makeSmoothGradient(int w, int h) {
    bro::eye::EyeImage img{w, h, std::vector<uint8_t>(static_cast<size_t>(w) * h * 4)};
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const uint8_t v = static_cast<uint8_t>((x * 255) / std::max(1, w - 1));
            const size_t i = (static_cast<size_t>(y) * w + x) * 4;
            img.rgba[i + 0] = v;
            img.rgba[i + 1] = v;
            img.rgba[i + 2] = v;
            img.rgba[i + 3] = 255;
        }
    }
    return img;
}

bro::eye::EyeImage makeTiledSine(int w, int h, int period) {
    bro::eye::EyeImage img{w, h, std::vector<uint8_t>(static_cast<size_t>(w) * h * 4)};
    const double k = 2.0 * 3.14159265358979323846 / period;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const double s = 0.5 + 0.4 * std::cos(k * x) * std::cos(k * y);
            const uint8_t v = static_cast<uint8_t>(std::clamp(std::lround(s * 255.0), 0L, 255L));
            const size_t i = (static_cast<size_t>(y) * w + x) * 4;
            img.rgba[i + 0] = v;
            img.rgba[i + 1] = v;
            img.rgba[i + 2] = v;
            img.rgba[i + 3] = 255;
        }
    }
    return img;
}

}  // namespace

int main() {
    using namespace bro::eye;

    // 1. Color space & contrast checks
    const auto blackLab = color::srgbToOklab(0, 0, 0);
    const auto whiteLab = color::srgbToOklab(255, 255, 255);
    check(std::abs(blackLab.L) < 1e-4, "black Oklab L ~ 0");
    check(std::abs(whiteLab.L - 1.0) < 1e-3, "white Oklab L ~ 1");
    const auto rt = color::oklabToSrgb(color::srgbToOklab(64, 150, 220));
    check(std::abs(int(rt.r) - 64) <= 1 && std::abs(int(rt.g) - 150) <= 1 && std::abs(int(rt.b) - 220) <= 1,
          "sRGB->Oklab->sRGB roundtrip");
    const double wcagBW = color::wcagContrastRatio({0, 0, 0}, {255, 255, 255});
    check(std::abs(wcagBW - 21.0) < 0.05, "WCAG black/white is 21:1");
    check(color::apcaContrast({0, 0, 0}, {255, 255, 255}) > 100.0, "APCA dark-on-light > 100");
    check(color::apcaContrast({255, 255, 255}, {0, 0, 0}) < -100.0, "APCA light-on-dark < -100");

    // 2. Exposure / crushed / blown checks
    const auto blackImg = makeSolid(64, 64, 0, 0, 0);
    const auto mBlack = measureImage(blackImg.view());
    check(mBlack.crushedBlackRatio > 0.99, "solid black crushed ratio ~ 1");
    check(mBlack.blownWhiteRatio < 0.01, "solid black blown ratio ~ 0");
    check(mBlack.tiling.score < 0.05, "solid black has no tiling");

    const auto whiteImg = makeSolid(64, 64, 255, 255, 255);
    const auto mWhite = measureImage(whiteImg.view());
    check(mWhite.blownWhiteRatio > 0.99, "solid white blown ratio ~ 1");

    // 3. Banding detection
    const auto banded = makeSteppedGradient(256, 128, 12);
    const auto smooth = makeSmoothGradient(256, 128);
    const auto mBand = measureImage(banded.view());
    const auto mSmooth = measureImage(smooth.view());
    check(mBand.bandingScore > 0.5, "stepped gradient flags banding");
    check(mSmooth.bandingScore < 0.25, "smooth gradient has low banding");

    // 4. 2D FFT Tiling, Wrap-around Seam & Multi-scale Detail Bands
    const auto tiled = makeTiledSine(128, 128, 16);
    const auto mTiled = measureImage(tiled.view());
    check(mTiled.tiling.score > 0.4, "periodic 16px sine grid detected as tiling");
    check(mSmooth.tiling.score < 0.15, "smooth gradient does not trigger tiling");
    check(mTiled.seam.score < 0.08, "periodic integer-period sine wraps seamlessly");
    check(mSmooth.seam.score > 0.5, "non-periodic horizontal ramp flags wrap seam");

    const auto highFreq = makeTiledSine(128, 128, 4);
    const auto lowFreq = makeTiledSine(128, 128, 64);
    const auto mHigh = measureImage(highFreq.view());
    const auto mLow = measureImage(lowFreq.view());
    check(mHigh.detailBands.high > mLow.detailBands.high, "4px pattern has higher high-band detail than 64px wave");
    check(mLow.detailBands.low > mHigh.detailBands.low, "64px wave has higher low-band detail than 4px pattern");

    // 5. Compare & determinism
    const auto cSame = compareImages(tiled.view(), tiled.view());
    check(cSame.score == 0.0 && cSame.ssim == 1.0 && cSame.deltaEMean == 0.0, "self compare is exact zero");
    const auto dark = makeSolid(64, 64, 40, 40, 40);
    const auto bright = makeSolid(64, 64, 200, 200, 200);
    const auto cDir = compareImages(bright.view(), dark.view());
    check(cDir.lumaDiff > 0.4, "brighter candidate gives positive lumaDiff");
    check(cDir.deltaEMean > 40.0, "large luminance shift gives high deltaE");
    const auto cSmall = compareImages(makeSolid(32, 32, 40, 40, 40).view(), bright.view());
    check(!cSmall.sameSize && cSmall.deltaEMean > 40.0, "mismatched size compare works cleanly");

    // 6. Visualization
    const std::vector<EyeImageView> pair{bright.view(), dark.view()};
    const auto vDiff = visualizeImages(pair, {"diff", "side", {"cand", "ref"}});
    check(vDiff.width > 128 && vDiff.height > 64 && vDiff.panels.size() == 3, "diff sheet has 3 panels");
    const auto vTile = visualizeImages({tiled.view()}, {"tiling", "side", {}});
    check(vTile.panels.size() == 2 && !vTile.rgba.empty(), "tiling spectrum renders cleanly");
    const auto vTile2 = visualizeImages({tiled.view()}, {"tile2x2", "side", {}});
    check(vTile2.panels.size() == 2 && !vTile2.rgba.empty(), "tile2x2 seam + FFT sheet renders cleanly");

    // 7. CMA-ES fitting & determinism
    const auto targetImg = makeSolid(32, 32, 70, 180, 110);
    EyeFitOptions fitOpts;
    fitOpts.params = {
        {"r", 0.0, 255.0, 200.0, true, "linear"},
        {"g", 0.0, 255.0, 30.0, true, "linear"},
        {"b", 0.0, 255.0, 220.0, true, "linear"},
    };
    fitOpts.reference = targetImg;
    fitOpts.hasReference = true;
    fitOpts.maxEvaluations = 160;
    fitOpts.targetDistance = 0.02;
    fitOpts.seed = 42;

    auto evalRgb = [](const std::vector<double>& vals) {
        EyeFitCandidateResult cr;
        cr.image = makeSolid(32, 32,
                             static_cast<uint8_t>(std::lround(vals[0])),
                             static_cast<uint8_t>(std::lround(vals[1])),
                             static_cast<uint8_t>(std::lround(vals[2])));
        return cr;
    };
    const auto fit1 = fitVisualParams(fitOpts, evalRgb);
    const auto fit2 = fitVisualParams(fitOpts, evalRgb);
    check(fit1.ok && fit1.distance < 0.06, "CMA-ES converges on RGB target");
    check(fit1.distance == fit2.distance && fit1.bestValues == fit2.bestValues,
          "CMA-ES is deterministic for fixed seed");

    // 8. Composition, Notan, Saliency, Rest-vs-Detail & Perceptual Compare
    {
        auto hero = makeSolid(160, 120, 16, 18, 22);
        // Strong focal island at (48, 42) warm-bright vs cool dark ground
        for (int y = 24; y < 62; ++y) {
            for (int x = 28; x < 72; ++x) {
                const size_t i = (static_cast<size_t>(y) * hero.width + x) * 4;
                hero.rgba[i + 0] = 245; hero.rgba[i + 1] = 215; hero.rgba[i + 2] = 175;
            }
        }
        // Quieter secondary island on right
        for (int y = 76; y < 96; ++y) {
            for (int x = 112; x < 132; ++x) {
                const size_t i = (static_cast<size_t>(y) * hero.width + x) * 4;
                hero.rgba[i + 0] = 120; hero.rgba[i + 1] = 130; hero.rgba[i + 2] = 145;
            }
        }
        const auto comp = analyzeComposition(hero.view());
        check(!comp.saliency.anchors.empty(), "saliency detects focal anchors");
        check(comp.saliency.anchors[0].x < 0.52 && comp.saliency.anchors[0].y < 0.52,
              "top focal anchor lands on bright hero mass");
        check(comp.notan.massCount >= 2 && comp.notan.darkShare > 0.4, "notan captures dark ground + bright mass");
        check(comp.rhythm.restRatio > 0.3, "calm regions count as rest space");
        check(comp.palette.warmShadowShift > 0.01, "warm highlights over cool ground gives positive warmShadowShift");

        const auto shifted = makeSolid(160, 120, 220, 220, 220);
        const auto pcmp = compareImages(hero.view(), shifted.view());
        check(pcmp.perceptual.notanMassDiff >= 0.0, "perceptual compare diff populated");

        EyeVisualizeOptions sOpts;
        sOpts.mode = "saliency";
        const auto vSal = visualizeImages({hero.view()}, sOpts);
        check(vSal.panels.size() == 2 && !vSal.rgba.empty(), "saliency visualize produces 2-panel sheet");

        EyeVisualizeOptions nOpts;
        nOpts.mode = "notan";
        nOpts.notanTones = 3;
        const auto vNot = visualizeImages({hero.view()}, nOpts);
        check(vNot.panels.size() == 2 && !vNot.rgba.empty(), "notan visualize produces 2-panel sheet");
    }

#if BRO_EYE_TEST_WITH_FORM
    // 9. 3D Mesh Differential Geometry, Plumb-Line Gravity & Silhouettes
    {
        const auto sphere = bromesh::sphere(1.0f, 32, 24);
        const auto cube = bromesh::box(1.0f, 1.0f, 1.0f);
        const auto flatPlate = bromesh::plane(2.0f, 2.0f, 8, 8);
        const auto fs = analyzeMeshForm(sphere);
        const auto fc = analyzeMeshForm(cube);
        const auto fp = analyzeMeshForm(flatPlate);
        check(fs.ok && fs.surface.smoothnessScore > 0.85, "smooth sphere scores high surface smoothness");
        check(fs.surface.creaseEdgeCount == 0, "smooth sphere has zero dihedral creases");
        check(fc.ok && fc.surface.creaseEdgeCount >= 12, "cube detects 12 sharp 90-deg creases");
        check(fp.ok && fp.surface.flatRatio > 0.80, "subdivided plane classifies as high flatRatio");
        check(fs.surface.smoothnessScore > fc.surface.smoothnessScore,
              "smooth sphere ranks smoother than hard-edged cube");
        check(fs.mass.grounded && fs.mass.symmetryScore > 0.90, "centered sphere is grounded & X-symmetric");
        check(fs.silhouettes.size() == 8 && fs.silhouettes[0].wiggleCount == 0,
              "sphere has 8 turntable silhouettes with clean contour");
    }
#endif

    // 10. Temporal motion continuity
    {
        auto drawBox = [](int w, int h, int bx, int by, int bw, int bh,
                          uint8_t bg, uint8_t fg) {
            bro::eye::EyeImage img = makeSolid(w, h, bg, bg, bg);
            for (int y = std::max(0, by); y < std::min(h, by + bh); ++y) {
                for (int x = std::max(0, bx); x < std::min(w, bx + bw); ++x) {
                    const size_t i = (static_cast<size_t>(y) * w + x) * 4;
                    img.rgba[i + 0] = fg;
                    img.rgba[i + 1] = static_cast<uint8_t>(fg / 2);
                    img.rgba[i + 2] = 40;
                }
            }
            return img;
        };

        // Static sequence -> all static pairs, smoothness == 0
        auto s0 = drawBox(96, 64, 16, 16, 20, 20, 30, 220);
        std::vector<bro::eye::EyeImageView> staticViews = {s0.view(), s0.view(), s0.view()};
        auto staticRes = bro::eye::analyzeMotion(staticViews);
        check(staticRes.staticPairCount == 2, "identical frames produce 2 static pairs");
        check(staticRes.smoothness == 0.0, "static sequence has smoothness 0");

        // Smooth linear horizontal translation across 5 frames
        std::vector<bro::eye::EyeImage> moveImgs;
        std::vector<bro::eye::EyeImageView> moveViews;
        for (int i = 0; i < 5; ++i) {
            moveImgs.push_back(drawBox(128, 64, 12 + i * 16, 20, 20, 20, 28, 220));
        }
        for (const auto& im : moveImgs) moveViews.push_back(im.view());
        bro::eye::EyeMotionOptions mopts;
        mopts.labels = {"t0", "t1", "t2", "t3", "t4"};
        mopts.timestampsMs = {0.0, 16.6, 33.2, 85.0, 101.6};  // intentional 51.8ms frame hiccup at step 2
        mopts.visualStyle = "board";
        mopts.buildVisual = true;
        auto moveRes = bro::eye::analyzeMotion(moveViews, mopts);
        check(moveRes.frameCount == 5 && moveRes.steps.size() == 4,
              "5 frames produce 4 motion steps");
        check(moveRes.staticPairCount == 0, "linear slide has no static pairs");
        check(moveRes.popCount == 0, "linear slide has no pops");
        check(moveRes.hiccupCount == 1 && moveRes.steps[2].hiccup,
              "timestampsMs detects exact single timing hiccup");
        check(moveRes.dtMaxMs > moveRes.dtMedianMs * 2.0, "dtMaxMs > 2 * dtMedianMs");
        check(moveRes.smoothness > 0.7, "linear slide has high motion smoothness");
        check(moveRes.motionCoverage > 0.05, "linear slide reports spatial coverage");
        check(moveRes.visual.width > 0 && moveRes.visual.height > 0, "motion board builds image");

        mopts.visualStyle = "chrono";
        auto chronoRes = bro::eye::analyzeMotion(moveViews, mopts);
        check(chronoRes.visual.width > 0 && chronoRes.visual.height > 0, "chrono builds image");

        mopts.visualStyle = "kymograph";
        auto kymoRes = bro::eye::analyzeMotion(moveViews, mopts);
        check(kymoRes.visual.width > 0 && kymoRes.visual.height > 0, "kymograph builds image");

        // Alternating dark/bright sequence triggers flickerScore
        auto dark = makeSolid(64, 64, 24, 24, 24);
        auto bright = makeSolid(64, 64, 230, 230, 230);
        std::vector<bro::eye::EyeImageView> flk = {dark.view(), bright.view(), dark.view(), bright.view()};
        auto flkRes = bro::eye::analyzeMotion(flk);
        check(flkRes.flickerScore > 0.1, "alternating luminance sequence triggers flickerScore");
    }

    if (gFailures == 0) {
        std::printf("PASS: bro_eye_test (all assertions passed)\n");
        return 0;
    }
    std::fprintf(stderr, "bro_eye_test: %d failure(s)\n", gFailures);
    return 1;
}
