#include "eye/eye_motion.h"
#include "eye/eye_color.h"
#include "eye/eye_draw_internal.h"
#include "eye/eye_measure.h"

#include <broimage/encode.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <numeric>
#include <vector>

namespace bro::eye {
namespace {

std::vector<EyeImage> normalizeFrames(const std::vector<EyeImageView>& frames, int& outW, int& outH) {
    outW = frames[0].width;
    outH = frames[0].height;
    const int longSide = std::max(outW, outH);
    if (longSide > 320) {
        const double s = 320.0 / longSide;
        outW = std::max(16, static_cast<int>(std::lround(outW * s)));
        outH = std::max(16, static_cast<int>(std::lround(outH * s)));
    }
    std::vector<EyeImage> scaled;
    scaled.reserve(frames.size());
    for (const auto& f : frames) {
        if (!f.valid()) continue;
        scaled.push_back(resampleRgba(f, outW, outH));
    }
    return scaled;
}

double frameMeanLuma(const EyeImage& img) {
    const size_t n = static_cast<size_t>(img.width) * img.height;
    if (n == 0) return 0.0;
    double sum = 0.0;
    for (size_t i = 0; i < n; ++i) {
        const uint8_t* p = &img.rgba[i * 4];
        sum += color::srgbToOklab(p[0], p[1], p[2]).L;
    }
    return sum / n;
}

}  // namespace

EyeMotionResult analyzeMotion(const std::vector<EyeImageView>& frames, const EyeMotionOptions& opts) {
    EyeMotionResult res;
    if (frames.size() < 2 || !frames[0].valid()) return res;

    int w = 0, h = 0;
    std::vector<EyeImage> work = normalizeFrames(frames, w, h);
    if (work.size() < 2) return res;

    res.width = frames[0].width;
    res.height = frames[0].height;
    res.frameCount = static_cast<int>(work.size());

    const size_t pxCount = static_cast<size_t>(w) * h;
    std::vector<double> maxDeltaMap(pxCount, 0.0);
    std::vector<uint8_t> touchedMask(pxCount, 0);

    std::vector<double> lumaMeans(work.size(), 0.0);
    res.lumaMin = 1.0;
    res.lumaMax = 0.0;
    for (size_t k = 0; k < work.size(); ++k) {
        lumaMeans[k] = frameMeanLuma(work[k]);
        res.lumaMin = std::min(res.lumaMin, lumaMeans[k]);
        res.lumaMax = std::max(res.lumaMax, lumaMeans[k]);
    }
    res.lumaRange = std::max(0.0, res.lumaMax - res.lumaMin);

    std::vector<double> stepDes;
    stepDes.reserve(work.size() - 1);

    int minBx = w, minBy = h, maxBx = -1, maxBy = -1;
    double sumDeMean = 0.0;

    for (size_t k = 0; k + 1 < work.size(); ++k) {
        const EyeImage& a = work[k];
        const EyeImage& b = work[k + 1];

        EyeMotionStep step;
        step.fromIndex = static_cast<int>(k);
        step.toIndex = static_cast<int>(k + 1);
        step.lumaDiff = lumaMeans[k + 1] - lumaMeans[k];

        std::vector<double> des(pxCount);
        double deSum = 0.0;
        double wSum = 0.0, wxSum = 0.0, wySum = 0.0;
        size_t changed = 0;

        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                const size_t i = static_cast<size_t>(y) * w + x;
                const uint8_t* pa = &a.rgba[i * 4];
                const uint8_t* pb = &b.rgba[i * 4];
                const double de = color::oklabDeltaE(
                    color::srgbToOklab(pa[0], pa[1], pa[2]),
                    color::srgbToOklab(pb[0], pb[1], pb[2]));
                des[i] = de;
                deSum += de;
                maxDeltaMap[i] = std::max(maxDeltaMap[i], de);
                if (de >= opts.threshold) {
                    ++changed;
                    touchedMask[i] = 1;
                    minBx = std::min(minBx, x);
                    minBy = std::min(minBy, y);
                    maxBx = std::max(maxBx, x);
                    maxBy = std::max(maxBy, y);
                    const double wt = de - opts.threshold + 1.0;
                    wSum += wt;
                    wxSum += wt * (x + 0.5) / w;
                    wySum += wt * (y + 0.5) / h;
                }
            }
        }

        step.deltaEMean = deSum / std::max<size_t>(1, pxCount);
        step.changedRatio = static_cast<double>(changed) / std::max<size_t>(1, pxCount);
        if (wSum > 0.0) {
            step.centroidX = wxSum / wSum;
            step.centroidY = wySum / wSum;
        }
        std::nth_element(des.begin(), des.begin() + static_cast<size_t>(0.95 * pxCount), des.end());
        step.deltaEP95 = des[static_cast<size_t>(0.95 * (pxCount - 1))];

        const EyeComparison cmp = compareImages(a.view(), b.view(), {4, 4, static_cast<int>(std::lround(opts.threshold))});
        step.ssim = cmp.ssim;
        step.staticStep = (step.changedRatio < 0.001 && step.deltaEMean < 0.15);
        if (step.staticStep) ++res.staticPairCount;

        stepDes.push_back(step.deltaEMean);
        sumDeMean += step.deltaEMean;
        res.steps.push_back(step);
    }

    const size_t nSteps = res.steps.size();
    res.meanDeltaE = sumDeMean / std::max<size_t>(1, nSteps);

    double var = 0.0;
    for (double d : stepDes) {
        const double diff = d - res.meanDeltaE;
        var += diff * diff;
    }
    res.deltaEStd = std::sqrt(var / std::max<size_t>(1, nSteps));

    if (res.meanDeltaE > 0.05) {
        const double cv = res.deltaEStd / res.meanDeltaE;
        res.smoothness = 1.0 / (1.0 + cv);
    }

    std::vector<double> sortedDe = stepDes;
    std::sort(sortedDe.begin(), sortedDe.end());
    const double medianDe = sortedDe[sortedDe.size() / 2];

    for (auto& st : res.steps) {
        if (medianDe > 0.35) {
            if (st.deltaEMean < medianDe * 0.35) {
                st.stutter = true;
                ++res.stutterCount;
            } else if (st.deltaEMean > medianDe * 2.5) {
                st.pop = true;
                ++res.popCount;
            }
        }
    }

    // Reversal energy across frame luma deltas captures exposure/SSR flicker.
    if (nSteps >= 2) {
        double revSum = 0.0;
        for (size_t k = 1; k < nSteps; ++k) {
            const double d0 = res.steps[k - 1].lumaDiff;
            const double d1 = res.steps[k].lumaDiff;
            if (d0 * d1 < 0.0) revSum += std::sqrt(std::abs(d0 * d1));
        }
        res.flickerScore = std::clamp((revSum / (nSteps - 1)) * 18.0, 0.0, 1.0);
    }

    size_t touchedCount = 0;
    for (uint8_t t : touchedMask) if (t) ++touchedCount;
    res.motionCoverage = static_cast<double>(touchedCount) / std::max<size_t>(1, pxCount);
    if (maxBx >= minBx && maxBy >= minBy) {
        const double sx = static_cast<double>(res.width) / w;
        const double sy = static_cast<double>(res.height) / h;
        res.motionBounds = {
            minBx * sx,
            minBy * sy,
            (maxBx - minBx + 1) * sx,
            (maxBy - minBy + 1) * sy,
        };
    }

    if (opts.buildVisual || !opts.path.empty()) {
        std::vector<EyeImage> panels;
        std::vector<std::string> lbls;
        const size_t maxSampled = std::min<size_t>(frames.size(), 5);
        for (size_t i = 0; i < maxSampled; ++i) {
            const size_t idx = (maxSampled == 1) ? 0 : (i * (frames.size() - 1)) / (maxSampled - 1);
            panels.push_back(draw::fitPanel(frames[idx], opts.maxPanelDim));
            if (idx < opts.labels.size()) {
                lbls.push_back(opts.labels[idx]);
            } else {
                char buf[32];
                std::snprintf(buf, sizeof(buf), "f%zu", idx);
                lbls.emplace_back(buf);
            }
        }

        // Accumulate motion energy trail panel with centroid polyline overlay.
        EyeImage trail = resampleRgba(panels[0].view(), w, h);
        for (size_t i = 0; i < pxCount; ++i) {
            const double v = std::clamp(maxDeltaMap[i] / 28.0, 0.0, 1.0);
            const auto c = color::magmaColor(v);
            uint8_t* p = &trail.rgba[i * 4];
            p[0] = static_cast<uint8_t>(p[0] * 0.30 + c.r * 0.70);
            p[1] = static_cast<uint8_t>(p[1] * 0.30 + c.g * 0.70);
            p[2] = static_cast<uint8_t>(p[2] * 0.30 + c.b * 0.70);
            p[3] = 255;
        }
        trail = draw::fitPanel(trail.view(), opts.maxPanelDim);

        int prevCx = -1, prevCy = -1;
        for (const auto& st : res.steps) {
            if (st.changedRatio <= 0.0005) continue;
            const int cx = std::clamp(static_cast<int>(std::lround(st.centroidX * trail.width)), 2, trail.width - 3);
            const int cy = std::clamp(static_cast<int>(std::lround(st.centroidY * trail.height)), 2, trail.height - 3);
            if (prevCx >= 0) draw::drawLine(trail, prevCx, prevCy, cx, cy, 2, 80, 235, 255, 230);
            draw::drawCircle(trail, cx, cy, 4, 2, 255, 220, 60, 240);
            prevCx = cx; prevCy = cy;
        }
        if (maxBx >= minBx && maxBy >= minBy) {
            const int rx = static_cast<int>(std::lround((minBx * 1.0 / w) * trail.width));
            const int ry = static_cast<int>(std::lround((minBy * 1.0 / h) * trail.height));
            const int rw = std::max(4, static_cast<int>(std::lround(((maxBx - minBx + 1.0) / w) * trail.width)));
            const int rh = std::max(4, static_cast<int>(std::lround(((maxBy - minBy + 1.0) / h) * trail.height)));
            draw::drawRectBox(trail, rx, ry, rw, rh, 1, 110, 245, 165);
        }

        char trailLbl[96];
        std::snprintf(trailLbl, sizeof(trailLbl), "trail | smooth %.2f | cov %.1f%% | stut %d",
                      res.smoothness, res.motionCoverage * 100.0, res.stutterCount);
        panels.push_back(std::move(trail));
        lbls.emplace_back(trailLbl);

        EyeVisualResult sheet = draw::assembleSheet(std::move(panels), lbls, opts.layout, 1);
        res.visual = {sheet.width, sheet.height, std::move(sheet.rgba)};
        if (!opts.path.empty() && !res.visual.empty()) {
            std::error_code ec;
            const std::filesystem::path outPath(opts.path);
            if (outPath.has_parent_path()) std::filesystem::create_directories(outPath.parent_path(), ec);
            if (broimage::encode_png_file(opts.path, res.visual.rgba.data(), res.visual.width, res.visual.height, 4)) {
                res.visualPath = opts.path;
            }
        }
    }

    return res;
}

}  // namespace bro::eye
