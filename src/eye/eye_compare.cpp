#include "eye/eye_measure.h"
#include "eye/eye_color.h"
#include "eye/eye_compose.h"

#include <broimage/compare.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace bro::eye {
namespace {

const uint8_t* rowPtr(EyeImageView img, int y) {
    return img.rgba + static_cast<size_t>(y) * img.effectiveStride();
}

double earthMovers1D(const std::array<double, 10>& a, const std::array<double, 10>& b) {
    double cdfA = 0.0, cdfB = 0.0, emd = 0.0;
    for (size_t i = 0; i < 10; ++i) {
        cdfA += a[i];
        cdfB += b[i];
        emd += std::abs(cdfA - cdfB);
    }
    return std::clamp(emd / 9.0, 0.0, 1.0);
}

}  // namespace

EyeComparison compareImages(EyeImageView aIn, EyeImageView bIn, const EyeCompareOptions& opts) {
    EyeComparison cmp;
    if (!aIn.valid() || !bIn.valid()) return cmp;

    EyeMeasureOptions mo;
    mo.gridRows = opts.gridRows;
    mo.gridCols = opts.gridCols;
    mo.detectTiling = true;

    const EyeMeasurement ma = measureImage(aIn, mo);
    const EyeMeasurement mb = measureImage(bIn, mo);

    cmp.sameSize = (aIn.width == bIn.width && aIn.height == bIn.height);

    EyeImage resampledA, resampledB;
    EyeImageView va = aIn;
    EyeImageView vb = bIn;
    if (!cmp.sameSize) {
        const int cw = std::clamp(bIn.width, 1, 512);
        const int ch = std::clamp(bIn.height, 1, 512);
        resampledA = resampleRgba(aIn, cw, ch);
        resampledB = resampleRgba(bIn, cw, ch);
        va = resampledA.view();
        vb = resampledB.view();
    }

    const int w = va.width;
    const int h = va.height;
    const int rows = std::clamp(opts.gridRows, 1, std::min(16, h));
    const int cols = std::clamp(opts.gridCols, 1, std::min(16, w));

    const auto bc = broimage::compare_u8(va.rgba, vb.rgba, w, h, 4,
                                        va.effectiveStride(), vb.effectiveStride(),
                                        rows, cols, opts.changeThreshold);
    cmp.ssim = bc.all.ssim;
    cmp.ssimDist = 1.0 - std::clamp(bc.all.ssim, 0.0, 1.0);
    cmp.changedRatio = bc.all.changed;
    cmp.psnr = bc.all.psnr;
    cmp.rms = bc.all.rms;

    // Exact per-pixel & per-cell Oklab deltaE.
    const int64_t nPx = static_cast<int64_t>(w) * h;
    std::vector<double> deltaEs;
    deltaEs.reserve(nPx);
    std::vector<double> cellDe(static_cast<size_t>(rows) * cols, 0.0);
    std::vector<double> cellLd(static_cast<size_t>(rows) * cols, 0.0);
    std::vector<int64_t> cellCnt(static_cast<size_t>(rows) * cols, 0);

    double deSum = 0.0, deMax = 0.0;
    for (int y = 0; y < h; ++y) {
        const uint8_t* pa = rowPtr(va, y);
        const uint8_t* pb = rowPtr(vb, y);
        const int gy = std::min(rows - 1, (y * rows) / h);
        for (int x = 0; x < w; ++x) {
            const uint8_t* qa = pa + x * 4;
            const uint8_t* qb = pb + x * 4;
            const auto la = color::srgbToOklab(qa[0], qa[1], qa[2]);
            const auto lb = color::srgbToOklab(qb[0], qb[1], qb[2]);
            const double de = color::oklabDeltaE(la, lb);
            deltaEs.push_back(de);
            deSum += de;
            if (de > deMax) deMax = de;

            const size_t ci = static_cast<size_t>(gy) * cols + std::min(cols - 1, (x * cols) / w);
            cellDe[ci] += de;
            cellLd[ci] += (la.L - lb.L);
            ++cellCnt[ci];
        }
    }

    cmp.deltaEMean = nPx > 0 ? deSum / static_cast<double>(nPx) : 0.0;
    cmp.deltaEMax = deMax;
    if (!deltaEs.empty()) {
        const size_t idx95 = static_cast<size_t>(std::min<size_t>(deltaEs.size() - 1, deltaEs.size() * 95 / 100));
        std::nth_element(deltaEs.begin(), deltaEs.begin() + idx95, deltaEs.end());
        cmp.deltaEP95 = deltaEs[idx95];
    }

    cmp.exposureDist = earthMovers1D(ma.zones, mb.zones);
    const double chromaDiff = std::abs(ma.chromaMean - mb.chromaMean) / 0.15;
    const double warmDiff = std::abs(ma.warmCoolBalance - mb.warmCoolBalance) * 0.5;
    const double hueDiff = std::abs(ma.hueEntropy - mb.hueEntropy);
    cmp.colorDist = std::clamp(0.5 * chromaDiff + 0.3 * warmDiff + 0.2 * hueDiff, 0.0, 2.0);
    cmp.detailDist = std::clamp(0.5 * std::abs(ma.sharpness - mb.sharpness) +
                                0.5 * std::abs(ma.edgeDensity - mb.edgeDensity) * 3.0, 0.0, 2.0);

    {
        const EyeComposeResult ca = analyzeComposition(va, {});
        const EyeComposeResult cb = analyzeComposition(vb, {});
        cmp.perceptual.focalShiftX = ca.saliency.centerOfMassX - cb.saliency.centerOfMassX;
        cmp.perceptual.focalShiftY = ca.saliency.centerOfMassY - cb.saliency.centerOfMassY;
        cmp.perceptual.focalShiftDist = std::sqrt(cmp.perceptual.focalShiftX * cmp.perceptual.focalShiftX +
                                                  cmp.perceptual.focalShiftY * cmp.perceptual.focalShiftY);
        cmp.perceptual.focalDominanceDiff = ca.saliency.focalDominance - cb.saliency.focalDominance;
        cmp.perceptual.notanMassDiff = static_cast<double>(ca.notan.massCount - cb.notan.massCount);
        cmp.perceptual.restDetailDiff = ca.rhythm.restDetailContrast - cb.rhythm.restDetailContrast;
        cmp.perceptual.balanceDiff =
            std::sqrt(ca.saliency.balanceX * ca.saliency.balanceX + ca.saliency.balanceY * ca.saliency.balanceY) -
            std::sqrt(cb.saliency.balanceX * cb.saliency.balanceX + cb.saliency.balanceY * cb.saliency.balanceY);
    }

    // Directional deltas: candidate a relative to reference b (positive lumaDiff = candidate brighter).
    cmp.lumaDiff = ma.lumaMean - mb.lumaMean;
    cmp.contrastRatio = (mb.lumaStd > 1e-4) ? (ma.lumaStd / mb.lumaStd) : (ma.lumaStd <= 1e-4 ? 1.0 : 10.0);
    cmp.chromaRatio = (mb.chromaMean > 1e-4) ? (ma.chromaMean / mb.chromaMean) : (ma.chromaMean <= 1e-4 ? 1.0 : 10.0);
    cmp.sharpnessRatio = (mb.sharpness > 1e-4) ? (ma.sharpness / mb.sharpness) : (ma.sharpness <= 1e-4 ? 1.0 : 10.0);
    cmp.tilingDiff = ma.tiling.score - mb.tiling.score;

    const double deNorm = std::min(1.0, cmp.deltaEMean / 35.0);
    cmp.score = opts.ssimWeight * cmp.ssimDist +
                opts.deltaEWeight * deNorm +
                opts.exposureWeight * cmp.exposureDist +
                opts.colorWeight * std::min(1.0, cmp.colorDist);

    cmp.cells.reserve(bc.cells.size());
    for (size_t i = 0; i < bc.cells.size(); ++i) {
        EyeCompareCell cc;
        cc.x = bc.cells[i].x;
        cc.y = bc.cells[i].y;
        cc.w = bc.cells[i].w;
        cc.h = bc.cells[i].h;
        cc.ssim = bc.cells[i].stats.ssim;
        cc.changedRatio = bc.cells[i].stats.changed;
        if (i < cellCnt.size() && cellCnt[i] > 0) {
            cc.deltaEMean = cellDe[i] / static_cast<double>(cellCnt[i]);
            cc.lumaDiff = cellLd[i] / static_cast<double>(cellCnt[i]);
        }
        cmp.cells.push_back(cc);
    }

    return cmp;
}

}  // namespace bro::eye
