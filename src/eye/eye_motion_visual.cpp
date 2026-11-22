#include "eye/eye_motion_visual.h"
#include "eye/eye_color.h"
#include "eye/eye_draw_internal.h"
#include "eye/eye_measure.h"

#include <broimage/encode.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <vector>

namespace bro::eye {
namespace {

struct Rgb8 { uint8_t r, g, b; };

Rgb8 chronoRamp(double t) {
    t = std::clamp(t, 0.0, 1.0);
    static constexpr std::array<Rgb8, 5> kStops = {{
        {56, 217, 255},   // t=0.0 cyan
        {72, 229, 154},   // t=0.25 emerald
        {255, 210, 74},   // t=0.55 gold
        {255, 106, 69},   // t=0.80 coral
        {255, 240, 248},  // t=1.0 hot white-magenta
    }};
    static constexpr std::array<double, 5> kPos = {0.0, 0.25, 0.55, 0.80, 1.0};
    for (size_t i = 0; i + 1 < kStops.size(); ++i) {
        if (t <= kPos[i + 1]) {
            const double u = (t - kPos[i]) / std::max(1e-6, kPos[i + 1] - kPos[i]);
            return {
                static_cast<uint8_t>(std::lround(kStops[i].r + (kStops[i + 1].r - kStops[i].r) * u)),
                static_cast<uint8_t>(std::lround(kStops[i].g + (kStops[i + 1].g - kStops[i].g) * u)),
                static_cast<uint8_t>(std::lround(kStops[i].b + (kStops[i + 1].b - kStops[i].b) * u)),
            };
        }
    }
    return kStops.back();
}

EyeImage buildChronoImage(const std::vector<EyeImage>& workFrames, int w, int h, double threshold, int maxDim) {
    const size_t nFrames = workFrames.size();
    const size_t pxCount = static_cast<size_t>(w) * h;

    // Sample up to 11 frames for fast temporal median stationary plate
    std::vector<size_t> medIdx;
    const size_t medCount = std::min<size_t>(nFrames, 11);
    for (size_t i = 0; i < medCount; ++i) {
        medIdx.push_back((medCount == 1) ? 0 : (i * (nFrames - 1)) / (medCount - 1));
    }
    EyeImage medianImg{w, h, std::vector<uint8_t>(pxCount * 4, 255)};
    std::vector<uint8_t> rs(medCount), gs(medCount), bs(medCount);
    const size_t mid = medCount / 2;
    for (size_t p = 0; p < pxCount; ++p) {
        for (size_t k = 0; k < medCount; ++k) {
            const uint8_t* sp = &workFrames[medIdx[k]].rgba[p * 4];
            rs[k] = sp[0]; gs[k] = sp[1]; bs[k] = sp[2];
        }
        std::nth_element(rs.begin(), rs.begin() + mid, rs.end());
        std::nth_element(gs.begin(), gs.begin() + mid, gs.end());
        std::nth_element(bs.begin(), bs.begin() + mid, bs.end());
        uint8_t* dp = &medianImg.rgba[p * 4];
        dp[0] = static_cast<uint8_t>(rs[mid] * 0.72);
        dp[1] = static_cast<uint8_t>(gs[mid] * 0.72);
        dp[2] = static_cast<uint8_t>(bs[mid] * 0.75);
        dp[3] = 255;
    }

    // Paint up to 12 evenly spaced moving slices in chromatic temporal ramp
    const size_t sliceCount = std::min<size_t>(nFrames, 12);
    const double effThresh = std::max(2.0, threshold * 1.4);
    for (size_t s = 0; s < sliceCount; ++s) {
        const size_t idx = (sliceCount == 1) ? 0 : (s * (nFrames - 1)) / (sliceCount - 1);
        const double t = (sliceCount <= 1) ? 1.0 : static_cast<double>(s) / (sliceCount - 1);
        const Rgb8 tint = chronoRamp(t);
        const double alpha = (s + 1 == sliceCount) ? 0.85 : (0.34 + 0.36 * t);
        const auto& fr = workFrames[idx];
        for (size_t p = 0; p < pxCount; ++p) {
            const uint8_t* fp = &fr.rgba[p * 4];
            const uint8_t* bp = &workFrames[medIdx[mid]].rgba[p * 4];
            const int dr = std::abs(int(fp[0]) - int(bp[0]));
            const int dg = std::abs(int(fp[1]) - int(bp[1]));
            const int db = std::abs(int(fp[2]) - int(bp[2]));
            if (dr + dg + db < static_cast<int>(effThresh * 4.5)) continue;
            const double de = color::oklabDeltaE(
                color::srgbToOklab(fp[0], fp[1], fp[2]),
                color::srgbToOklab(bp[0], bp[1], bp[2]));
            if (de <= effThresh) continue;
            uint8_t* dp = &medianImg.rgba[p * 4];
            const double cr = fp[0] * 0.35 + tint.r * 0.65;
            const double cg = fp[1] * 0.35 + tint.g * 0.65;
            const double cb = fp[2] * 0.35 + tint.b * 0.65;
            dp[0] = static_cast<uint8_t>(std::clamp(dp[0] * (1.0 - alpha) + cr * alpha, 0.0, 255.0));
            dp[1] = static_cast<uint8_t>(std::clamp(dp[1] * (1.0 - alpha) + cg * alpha, 0.0, 255.0));
            dp[2] = static_cast<uint8_t>(std::clamp(dp[2] * (1.0 - alpha) + cb * alpha, 0.0, 255.0));
        }
    }

    EyeImage out = draw::fitPanel(medianImg.view(), maxDim);
    // Color-time legend bar at bottom
    const int barW = std::min(160, std::max(48, out.width - 24));
    const int barX = 8;
    const int barY = std::max(4, out.height - 14);
    draw::fillRect(out, barX - 2, barY - 2, barW + 4, 10, 14, 16, 20, 210);
    for (int i = 0; i < barW; ++i) {
        const Rgb8 c = chronoRamp(static_cast<double>(i) / std::max(1, barW - 1));
        draw::fillRect(out, barX + i, barY, 1, 6, c.r, c.g, c.b, 240);
    }
    draw::drawText(out, barX + barW + 6, barY - 1, "t0->t1", 1, 220, 230, 240, true);
    return out;
}

EyeImage buildKymographImage(const std::vector<EyeImage>& workFrames,
                             const std::vector<double>& maxDeltaMap,
                             int w, int h,
                             const EyeMotionOptions& opts,
                             EyeMotionResult& res,
                             int targetW, int targetH) {
    // Pick peak-motion row (slitY) and column (slitX) if not explicitly given
    int bestY = h / 2, bestX = w / 2;
    if (opts.slitY >= 0.0 && opts.slitY <= 1.0) {
        bestY = std::clamp(static_cast<int>(std::lround(opts.slitY * (h - 1))), 0, h - 1);
    } else {
        double bestScore = -1.0;
        for (int y = 0; y < h; ++y) {
            double s = 0.0;
            for (int x = 0; x < w; ++x) s += maxDeltaMap[static_cast<size_t>(y) * w + x];
            if (s > bestScore) { bestScore = s; bestY = y; }
        }
    }
    if (opts.slitX >= 0.0 && opts.slitX <= 1.0) {
        bestX = std::clamp(static_cast<int>(std::lround(opts.slitX * (w - 1))), 0, w - 1);
    } else {
        double bestScore = -1.0;
        for (int x = 0; x < w; ++x) {
            double s = 0.0;
            for (int y = 0; y < h; ++y) s += maxDeltaMap[static_cast<size_t>(y) * w + x];
            if (s > bestScore) { bestScore = s; bestX = x; }
        }
    }
    res.autoSlitX = (bestX + 0.5) / std::max(1, w);
    res.autoSlitY = (bestY + 0.5) / std::max(1, h);

    const int kW = std::max(160, targetW);
    const int kH = std::max(120, targetH);
    EyeImage img{kW, kH, std::vector<uint8_t>(static_cast<size_t>(kW) * kH * 4, 18)};
    for (size_t i = 3; i < img.rgba.size(); i += 4) img.rgba[i] = 255;

    const int nF = static_cast<int>(workFrames.size());
    const int padTop = 14;
    const int midGap = 16;
    const int bandH = std::max(24, (kH - padTop - midGap - 6) / 2);
    const int ytTop = padTop + bandH + midGap;
    const int slitW = std::max(20, kW - 14);

    // Top band: Horizontal Slit X-T (5-row average)
    draw::drawText(img, 10, 2, "X-T slit y=" + std::to_string(int(std::lround(res.autoSlitY * 100))) + "%", 1, 140, 225, 255, false);
    for (int py = 0; py < bandH; ++py) {
        const int fi = std::clamp((py * nF) / std::max(1, bandH), 0, nF - 1);
        const auto& fr = workFrames[static_cast<size_t>(fi)];
        for (int px = 0; px < slitW; ++px) {
            const int sx = std::clamp((px * w) / slitW, 0, w - 1);
            int r = 0, g = 0, b = 0, cnt = 0;
            for (int dy = -2; dy <= 2; ++dy) {
                const int sy = std::clamp(bestY + dy, 0, h - 1);
                const uint8_t* sp = &fr.rgba[(static_cast<size_t>(sy) * w + sx) * 4];
                r += sp[0]; g += sp[1]; b += sp[2]; ++cnt;
            }
            draw::putPx(img, 10 + px, padTop + py,
                        static_cast<uint8_t>(r / cnt), static_cast<uint8_t>(g / cnt), static_cast<uint8_t>(b / cnt));
        }
    }

    // Bottom band: Vertical Slit Y-T (5-col average)
    draw::drawText(img, 10, padTop + bandH + 4, "Y-T slit x=" + std::to_string(int(std::lround(res.autoSlitX * 100))) + "%", 1, 255, 205, 110, false);
    for (int py = 0; py < bandH; ++py) {
        const int fi = std::clamp((py * nF) / std::max(1, bandH), 0, nF - 1);
        const auto& fr = workFrames[static_cast<size_t>(fi)];
        for (int px = 0; px < slitW; ++px) {
            const int sy = std::clamp((px * h) / slitW, 0, h - 1);
            int r = 0, g = 0, b = 0, cnt = 0;
            for (int dx = -2; dx <= 2; ++dx) {
                const int sx = std::clamp(bestX + dx, 0, w - 1);
                const uint8_t* sp = &fr.rgba[(static_cast<size_t>(sy) * w + sx) * 4];
                r += sp[0]; g += sp[1]; b += sp[2]; ++cnt;
            }
            draw::putPx(img, 10 + px, ytTop + py,
                        static_cast<uint8_t>(r / cnt), static_cast<uint8_t>(g / cnt), static_cast<uint8_t>(b / cnt));
        }
    }

    // Highlight stutter/pop/hiccup steps with bright red margin ticks on both bands
    for (size_t k = 0; k < res.steps.size(); ++k) {
        const auto& st = res.steps[k];
        if (!st.stutter && !st.pop && !st.hiccup) continue;
        const int ry = static_cast<int>(std::lround(((k + 0.5) / std::max<size_t>(1, res.steps.size())) * (bandH - 1)));
        draw::fillRect(img, 2, padTop + ry - 1, 6, 3, 255, 70, 70);
        draw::fillRect(img, 2, ytTop + ry - 1, 6, 3, 255, 70, 70);
    }
    return img;
}

EyeImage buildCadenceEcgImage(const EyeMotionResult& res, int width, int height) {
    const int W = std::max(180, width);
    const int H = std::max(120, height);
    EyeImage img{W, H, std::vector<uint8_t>(static_cast<size_t>(W) * H * 4, 16)};
    for (size_t i = 3; i < img.rgba.size(); i += 4) img.rgba[i] = 255;

    const int gx = 12, gy = 18, gw = W - 24, gh = H - 30;
    draw::fillRect(img, gx, gy, gw, gh, 22, 26, 34);
    for (int i = 1; i < 4; ++i) {
        const int yLine = gy + (gh * i) / 4;
        draw::fillRect(img, gx, yLine, gw, 1, 38, 44, 56);
    }

    const size_t nS = res.steps.size();
    double maxDe = 1.0, maxDt = 1.0;
    bool hasDt = res.dtMaxMs > 0.0;
    for (const auto& s : res.steps) {
        maxDe = std::max(maxDe, s.deltaEMean * 1.15);
        if (hasDt) maxDt = std::max(maxDt, s.dtMs * 1.15);
    }

    // Event bars for stutters, pops, and hiccups
    if (nS > 0) {
        for (size_t i = 0; i < nS; ++i) {
            const auto& s = res.steps[i];
            if (!s.stutter && !s.pop && !s.hiccup) continue;
            const int px = gx + static_cast<int>(std::lround((i + 0.5) * gw / std::max<size_t>(1, nS)));
            draw::fillRect(img, px - 1, gy, 3, gh, 220, 58, 68, 120);
        }
    }

    // Dashed median deltaE baseline
    if (res.meanDeltaE > 0.0) {
        const int my = gy + gh - 1 - static_cast<int>(std::lround((res.meanDeltaE / maxDe) * (gh - 4)));
        for (int x = gx; x < gx + gw; x += 8) draw::fillRect(img, x, my, 4, 1, 70, 150, 170);
    }

    // Cyan deltaE polyline + amber dtMs polyline
    auto stepX = [&](size_t i) -> int {
        if (nS <= 1) return gx + gw / 2;
        return gx + 3 + static_cast<int>(std::lround((static_cast<double>(i) / (nS - 1)) * (gw - 6)));
    };
    int prevX = -1, prevDeY = -1, prevDtY = -1;
    for (size_t i = 0; i < nS; ++i) {
        const int x = stepX(i);
        const int deY = gy + gh - 2 - static_cast<int>(std::lround((res.steps[i].deltaEMean / maxDe) * (gh - 6)));
        if (prevX >= 0) draw::drawLine(img, prevX, prevDeY, x, deY, 2, 72, 225, 255, 240);
        draw::fillRect(img, x - 2, deY - 2, 4, 4, 150, 245, 255);
        if (hasDt) {
            const int dtY = gy + gh - 2 - static_cast<int>(std::lround((res.steps[i].dtMs / maxDt) * (gh - 6)));
            if (prevX >= 0) draw::drawLine(img, prevX, prevDtY, x, dtY, 2, 255, 190, 68, 220);
            prevDtY = dtY;
        }
        prevX = x; prevDeY = deY;
    }

    char hdr[112];
    if (hasDt) {
        std::snprintf(hdr, sizeof(hdr), "ECG smooth %.2f | stut %d pop %d hic %d | p95 %.1fms",
                      res.smoothness, res.stutterCount, res.popCount, res.hiccupCount, res.dtP95Ms);
    } else {
        std::snprintf(hdr, sizeof(hdr), "ECG smooth %.2f | stut %d pop %d | dE %.2f",
                      res.smoothness, res.stutterCount, res.popCount, res.meanDeltaE);
    }
    draw::drawText(img, 12, 4, hdr, 1, 225, 232, 242, false);
    draw::drawText(img, gx + 4, gy + 4, hasDt ? "cyan:dE  amber:dtMs" : "cyan:step deltaE", 1, 165, 180, 200, true);
    return img;
}

EyeImage buildTrailImage(const EyeImage& seed,
                         const std::vector<double>& maxDeltaMap,
                         int w, int h,
                         int minBx, int minBy, int maxBx, int maxBy,
                         const EyeMotionResult& res,
                         int maxDim) {
    EyeImage trail = resampleRgba(seed.view(), w, h);
    const size_t pxCount = static_cast<size_t>(w) * h;
    for (size_t i = 0; i < pxCount; ++i) {
        const double v = std::clamp(maxDeltaMap[i] / 28.0, 0.0, 1.0);
        const auto c = color::magmaColor(v);
        uint8_t* p = &trail.rgba[i * 4];
        p[0] = static_cast<uint8_t>(p[0] * 0.30 + c.r * 0.70);
        p[1] = static_cast<uint8_t>(p[1] * 0.30 + c.g * 0.70);
        p[2] = static_cast<uint8_t>(p[2] * 0.30 + c.b * 0.70);
        p[3] = 255;
    }
    trail = draw::fitPanel(trail.view(), maxDim);

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
    return trail;
}

}  // namespace

EyeVisualResult buildMotionVisual(const std::vector<EyeImageView>& rawFrames,
                                  const std::vector<EyeImage>& workFrames,
                                  const std::vector<double>& maxDeltaMap,
                                  int workW,
                                  int workH,
                                  int minBx,
                                  int minBy,
                                  int maxBx,
                                  int maxBy,
                                  const EyeMotionOptions& opts,
                                  EyeMotionResult& ioResult) {
    EyeVisualResult sheet;
    const std::string style = opts.visualStyle.empty() ? "board" : opts.visualStyle;
    const int panelCap = std::clamp(opts.maxPanelDim, 160, 1400);

    EyeImage chrono = buildChronoImage(workFrames, workW, workH, opts.threshold, panelCap);
    EyeImage kymo = buildKymographImage(workFrames, maxDeltaMap, workW, workH, opts, ioResult,
                                        chrono.width, chrono.height);

    if (style == "chrono") {
        sheet = draw::assembleSheet({std::move(chrono)}, {"chronophotograph"}, "side", 1);
    } else if (style == "kymograph") {
        sheet = draw::assembleSheet({std::move(kymo)}, {"slit-scan kymograph (X-T / Y-T)"}, "side", 1);
    } else if (style == "sheet") {
        std::vector<EyeImage> panels;
        std::vector<std::string> lbls;
        const size_t maxSampled = std::min<size_t>(rawFrames.size(), 5);
        for (size_t i = 0; i < maxSampled; ++i) {
            const size_t idx = (maxSampled == 1) ? 0 : (i * (rawFrames.size() - 1)) / (maxSampled - 1);
            panels.push_back(draw::fitPanel(rawFrames[idx], panelCap));
            lbls.push_back(idx < opts.labels.size() ? opts.labels[idx] : ("f" + std::to_string(idx)));
        }
        panels.push_back(buildTrailImage(panels[0], maxDeltaMap, workW, workH, minBx, minBy, maxBx, maxBy, ioResult, panelCap));
        char trailLbl[96];
        std::snprintf(trailLbl, sizeof(trailLbl), "trail | smooth %.2f | cov %.1f%% | stut %d",
                      ioResult.smoothness, ioResult.motionCoverage * 100.0, ioResult.stutterCount);
        lbls.emplace_back(trailLbl);
        sheet = draw::assembleSheet(std::move(panels), lbls, opts.layout, 1);
    } else {
        // Default 4-panel visual motion board: Chronophotograph | Slit Kymograph / Cadence ECG | Trail
        EyeImage ecg = buildCadenceEcgImage(ioResult, chrono.width, chrono.height);
        EyeImage trail = buildTrailImage(draw::fitPanel(rawFrames[0], panelCap), maxDeltaMap,
                                         workW, workH, minBx, minBy, maxBx, maxBy, ioResult, panelCap);
        char trLbl[96];
        std::snprintf(trLbl, sizeof(trLbl), "trail | cov %.1f%% | rng %dx%d",
                      ioResult.motionCoverage * 100.0,
                      static_cast<int>(std::lround(ioResult.motionBounds.w)),
                      static_cast<int>(std::lround(ioResult.motionBounds.h)));
        std::vector<EyeImage> panels = {std::move(chrono), std::move(kymo), std::move(ecg), std::move(trail)};
        std::vector<std::string> lbls = {
            "chronophotograph (" + std::to_string(rawFrames.size()) + "f)",
            "slit-scan kymograph",
            "cadence ECG",
            trLbl,
        };
        sheet = draw::assembleSheet(std::move(panels), lbls, "grid", 1);
    }

    ioResult.visual = {sheet.width, sheet.height, sheet.rgba};
    if (!opts.path.empty() && !ioResult.visual.empty()) {
        std::error_code ec;
        const std::filesystem::path outPath(opts.path);
        if (outPath.has_parent_path()) std::filesystem::create_directories(outPath.parent_path(), ec);
        if (broimage::encode_png_file(opts.path, ioResult.visual.rgba.data(), ioResult.visual.width, ioResult.visual.height, 4)) {
            ioResult.visualPath = opts.path;
        }
    }
    return sheet;
}

}  // namespace bro::eye
