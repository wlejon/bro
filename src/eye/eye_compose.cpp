#include "eye/eye_compose.h"
#include "eye/eye_color.h"
#include "eye/eye_measure.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <queue>
#include <utility>
#include <vector>

namespace bro::eye {
namespace {

void boxBlurSeparable(const std::vector<float>& src, int w, int h, int rad, std::vector<float>& dst) {
    dst.resize(static_cast<size_t>(w) * h);
    if (rad <= 0 || w <= 0 || h <= 0) {
        dst = src;
        return;
    }
    std::vector<float> tmp(static_cast<size_t>(w) * h, 0.0f);
    // Horizontal pass with prefix sums
    std::vector<double> ps(static_cast<size_t>(w) + 1, 0.0);
    for (int y = 0; y < h; ++y) {
        const size_t row = static_cast<size_t>(y) * w;
        ps[0] = 0.0;
        for (int x = 0; x < w; ++x) ps[x + 1] = ps[x] + src[row + x];
        for (int x = 0; x < w; ++x) {
            const int x0 = std::max(0, x - rad);
            const int x1 = std::min(w - 1, x + rad);
            tmp[row + x] = static_cast<float>((ps[x1 + 1] - ps[x0]) / (x1 - x0 + 1));
        }
    }
    // Vertical pass
    std::vector<double> vs(static_cast<size_t>(h) + 1, 0.0);
    for (int x = 0; x < w; ++x) {
        vs[0] = 0.0;
        for (int y = 0; y < h; ++y) vs[y + 1] = vs[y] + tmp[static_cast<size_t>(y) * w + x];
        for (int y = 0; y < h; ++y) {
            const int y0 = std::max(0, y - rad);
            const int y1 = std::min(h - 1, y + rad);
            dst[static_cast<size_t>(y) * w + x] = static_cast<float>((vs[y1 + 1] - vs[y0]) / (y1 - y0 + 1));
        }
    }
}

void gaussianApprox(const std::vector<float>& src, int w, int h, int rad, std::vector<float>& dst) {
    std::vector<float> a, b;
    boxBlurSeparable(src, w, h, rad, a);
    boxBlurSeparable(a, w, h, rad, b);
    boxBlurSeparable(b, w, h, rad, dst);
}

EyeImage downsampleMaxSide(EyeImageView img, int maxSide) {
    if (!img.valid()) return {};
    const int curMax = std::max(img.width, img.height);
    if (curMax <= maxSide) {
        return resampleRgba(img, img.width, img.height);
    }
    const double s = static_cast<double>(maxSide) / curMax;
    const int nw = std::max(8, static_cast<int>(std::lround(img.width * s)));
    const int nh = std::max(8, static_cast<int>(std::lround(img.height * s)));
    return resampleRgba(img, nw, nh);
}

}  // namespace

void computeSaliencyField(EyeImageView img, int& outW, int& outH, std::vector<float>& outSal) {
    outW = 0;
    outH = 0;
    outSal.clear();
    if (!img.valid()) return;

    const EyeImage small = downsampleMaxSide(img, 160);
    outW = small.width;
    outH = small.height;
    const size_t n = static_cast<size_t>(outW) * outH;

    std::vector<float> L(n), A(n), B(n);
    for (size_t i = 0; i < n; ++i) {
        const auto lab = color::srgbToOklab(small.rgba[i * 4], small.rgba[i * 4 + 1], small.rgba[i * 4 + 2]);
        L[i] = static_cast<float>(lab.L);
        A[i] = static_cast<float>(lab.a);
        B[i] = static_cast<float>(lab.b);
    }

    // Center-surround pyramid across scales
    const int r1 = 1;
    const int r2 = std::max(3, std::min(outW, outH) / 28);
    const int r3 = std::max(6, std::min(outW, outH) / 10);

    std::vector<float> L1, L2, L3, A1, A2, A3, B1, B2, B3;
    gaussianApprox(L, outW, outH, r1, L1);
    gaussianApprox(L, outW, outH, r2, L2);
    gaussianApprox(L, outW, outH, r3, L3);
    gaussianApprox(A, outW, outH, r1, A1);
    gaussianApprox(A, outW, outH, r2, A2);
    gaussianApprox(A, outW, outH, r3, A3);
    gaussianApprox(B, outW, outH, r1, B1);
    gaussianApprox(B, outW, outH, r2, B2);
    gaussianApprox(B, outW, outH, r3, B3);

    // Local Sobel edge energy on Oklab L
    std::vector<float> edge(n, 0.0f);
    for (int y = 1; y < outH - 1; ++y) {
        for (int x = 1; x < outW - 1; ++x) {
            auto at = [&](int xx, int yy) { return L[static_cast<size_t>(yy) * outW + xx]; };
            const float gx = -at(x - 1, y - 1) - 2.0f * at(x - 1, y) - at(x - 1, y + 1) +
                             at(x + 1, y - 1) + 2.0f * at(x + 1, y) + at(x + 1, y + 1);
            const float gy = -at(x - 1, y - 1) - 2.0f * at(x, y - 1) - at(x + 1, y - 1) +
                             at(x - 1, y + 1) + 2.0f * at(x, y + 1) + at(x + 1, y + 1);
            edge[static_cast<size_t>(y) * outW + x] = std::sqrt(gx * gx + gy * gy) * 0.25f;
        }
    }
    std::vector<float> edgeSmooth;
    gaussianApprox(edge, outW, outH, std::max(2, r2 / 2), edgeSmooth);

    outSal.resize(n, 0.0f);
    float maxVal = 1e-6f;
    for (size_t i = 0; i < n; ++i) {
        const float csL = std::abs(L1[i] - L2[i]) + 0.8f * std::abs(L1[i] - L3[i]);
        const float csC = std::sqrt((A1[i] - A2[i]) * (A1[i] - A2[i]) + (B1[i] - B2[i]) * (B1[i] - B2[i])) +
                          0.8f * std::sqrt((A1[i] - A3[i]) * (A1[i] - A3[i]) + (B1[i] - B3[i]) * (B1[i] - B3[i]));
        const float v = csL * 1.1f + csC * 2.2f + edgeSmooth[i] * 0.85f;
        outSal[i] = v;
        maxVal = std::max(maxVal, v);
    }
    for (size_t i = 0; i < n; ++i) {
        outSal[i] = std::clamp(outSal[i] / maxVal, 0.0f, 1.0f);
    }
}

void computeNotanField(EyeImageView img, int tones, int& outW, int& outH, std::vector<uint8_t>& outBands) {
    outW = 0;
    outH = 0;
    outBands.clear();
    if (!img.valid()) return;

    const int nTones = (tones >= 5) ? 5 : 3;
    const EyeImage small = downsampleMaxSide(img, 200);
    outW = small.width;
    outH = small.height;
    const size_t n = static_cast<size_t>(outW) * outH;

    std::vector<float> L(n);
    for (size_t i = 0; i < n; ++i) {
        L[i] = static_cast<float>(color::srgbToOklab(small.rgba[i * 4], small.rgba[i * 4 + 1], small.rgba[i * 4 + 2]).L);
    }

    const int blurRad = std::max(2, std::min(outW, outH) / 48);
    std::vector<float> smoothL;
    gaussianApprox(L, outW, outH, blurRad, smoothL);

    std::vector<float> sorted = smoothL;
    std::sort(sorted.begin(), sorted.end());
    const float lMin = sorted.front();
    const float lMax = sorted.back();
    const float range = std::max(0.05f, lMax - lMin);

    // Hybrid percentile + global Oklab thresholds so dark UI vs bright marble both mass cleanly.
    std::vector<float> cuts;
    if (nTones == 3) {
        const float p35 = sorted[static_cast<size_t>(n * 35 / 100)];
        const float p72 = sorted[static_cast<size_t>(n * 72 / 100)];
        const float t1 = 0.5f * (lMin + range * 0.30f) + 0.5f * p35;
        const float t2 = 0.5f * (lMin + range * 0.68f) + 0.5f * p72;
        cuts = {t1, std::max(t1 + 0.03f, t2)};
    } else {
        cuts.resize(4);
        for (int k = 1; k <= 4; ++k) {
            const float fk = static_cast<float>(k) / 5.0f;
            const float pk = sorted[static_cast<size_t>(n * (k * 20) / 100)];
            const float uk = lMin + range * fk;
            cuts[k - 1] = 0.5f * uk + 0.5f * pk;
        }
        for (int k = 1; k < 4; ++k) cuts[k] = std::max(cuts[k - 1] + 0.02f, cuts[k]);
    }

    outBands.resize(n, 0);
    for (size_t i = 0; i < n; ++i) {
        uint8_t b = 0;
        for (float c : cuts) {
            if (smoothL[i] >= c) ++b;
        }
        outBands[i] = b;
    }
}

EyeComposeResult analyzeComposition(EyeImageView img, const EyeComposeOptions& opts) {
    EyeComposeResult res;
    if (!img.valid()) return res;
    res.width = img.width;
    res.height = img.height;

    const EyeMeasurement meas = measureImage(img, {opts.rhythmRows, opts.rhythmCols, 6, false});

    // 1. Saliency field & focal anchors
    int sw = 0, sh = 0;
    std::vector<float> sal;
    computeSaliencyField(img, sw, sh, sal);
    if (sw > 0 && sh > 0) {
        double sumW = 0.0, sumX = 0.0, sumY = 0.0, edgeW = 0.0;
        const int marginX = std::max(1, sw / 20);
        const int marginY = std::max(1, sh / 20);
        for (int y = 0; y < sh; ++y) {
            for (int x = 0; x < sw; ++x) {
                const double w = sal[static_cast<size_t>(y) * sw + x];
                const double w2 = w * w;
                sumW += w2;
                sumX += w2 * ((x + 0.5) / sw);
                sumY += w2 * ((y + 0.5) / sh);
                if (x < marginX || x >= sw - marginX || y < marginY || y >= sh - marginY) {
                    edgeW += w2;
                }
            }
        }
        if (sumW > 1e-9) {
            res.saliency.centerOfMassX = std::clamp(sumX / sumW, 0.0, 1.0);
            res.saliency.centerOfMassY = std::clamp(sumY / sumW, 0.0, 1.0);
            res.saliency.balanceX = res.saliency.centerOfMassX - 0.5;
            res.saliency.balanceY = res.saliency.centerOfMassY - 0.5;
            res.saliency.edgeCrowding = std::clamp(edgeW / sumW, 0.0, 1.0);
        }

        // Greedy inhibition-of-return peak extraction over blurred saliency field
        std::vector<float> peakMap;
        gaussianApprox(sal, sw, sh, std::max(2, std::min(sw, sh) / 36), peakMap);
        std::vector<float> work = peakMap;
        const int inhRad = std::max(4, static_cast<int>(std::lround(0.12 * std::min(sw, sh))));
        const int maxAnchors = std::clamp(opts.maxAnchors, 1, 10);
        for (int k = 0; k < maxAnchors; ++k) {
            int bx = -1, by = -1;
            float best = 0.02f;
            for (int y = 0; y < sh; ++y) {
                for (int x = 0; x < sw; ++x) {
                    const float v = work[static_cast<size_t>(y) * sw + x];
                    if (v > best) { best = v; bx = x; by = y; }
                }
            }
            if (bx < 0) break;
            EyeFocalPoint fp;
            fp.rank = k + 1;
            fp.x = (bx + 0.5) / sw;
            fp.y = (by + 0.5) / sh;
            fp.radius = static_cast<double>(inhRad) / std::min(sw, sh);
            fp.strength = best;
            res.saliency.anchors.push_back(fp);

            const int r2 = inhRad * inhRad;
            for (int y = std::max(0, by - inhRad); y <= std::min(sh - 1, by + inhRad); ++y) {
                for (int x = std::max(0, bx - inhRad); x <= std::min(sw - 1, bx + inhRad); ++x) {
                    const int dx = x - bx, dy = y - by;
                    if (dx * dx + dy * dy <= r2) work[static_cast<size_t>(y) * sw + x] = 0.0f;
                }
            }
        }
        if (res.saliency.anchors.size() >= 2) {
            double restSum = 0.0;
            for (size_t i = 1; i < res.saliency.anchors.size(); ++i) restSum += res.saliency.anchors[i].strength;
            const double meanRest = restSum / static_cast<double>(res.saliency.anchors.size() - 1);
            res.saliency.focalDominance = (meanRest > 1e-5) ? (res.saliency.anchors[0].strength / meanRest) : 2.0;
        }
    }

    // 2. Notan tonal massing
    {
        int nw = 0, nh = 0;
        std::vector<uint8_t> bands3, bands5;
        computeNotanField(img, 3, nw, nh, bands3);
        int nw5 = 0, nh5 = 0;
        computeNotanField(img, 5, nw5, nh5, bands5);
        const size_t nn = bands3.size();
        if (nn > 0) {
            size_t c0 = 0, c1 = 0, c2 = 0;
            for (uint8_t b : bands3) {
                if (b == 0) ++c0;
                else if (b == 1) ++c1;
                else ++c2;
            }
            res.notan.darkShare = static_cast<double>(c0) / nn;
            res.notan.midShare = static_cast<double>(c1) / nn;
            res.notan.lightShare = static_cast<double>(c2) / nn;

            if (!bands5.empty()) {
                std::array<size_t, 5> cnt5{};
                for (uint8_t b : bands5) cnt5[std::min<size_t>(4, b)]++;
                for (size_t k = 0; k < 5; ++k) res.notan.tone5Shares[k] = static_cast<double>(cnt5[k]) / bands5.size();
            }

            // Connected components over primary Notan map
            const std::vector<uint8_t>& useBands = (opts.notanTones >= 5 && !bands5.empty()) ? bands5 : bands3;
            const int mw = (opts.notanTones >= 5 && !bands5.empty()) ? nw5 : nw;
            const int mh = (opts.notanTones >= 5 && !bands5.empty()) ? nh5 : nh;
            const size_t mn = static_cast<size_t>(mw) * mh;
            const size_t minCompPx = std::max<size_t>(12, mn / 250);
            std::vector<uint8_t> vis(mn, 0);
            std::vector<size_t> compSizes;
            std::vector<int> q;
            q.reserve(1024);

            for (size_t idx = 0; idx < mn; ++idx) {
                if (vis[idx]) continue;
                const uint8_t target = useBands[idx];
                vis[idx] = 1;
                q.clear();
                q.push_back(static_cast<int>(idx));
                for (size_t head = 0; head < q.size(); ++head) {
                    const int cur = q[head];
                    const int cx = cur % mw, cy = cur / mw;
                    const int nb[4][2] = {{-1,0},{1,0},{0,-1},{0,1}};
                    for (const auto& d : nb) {
                        const int nx = cx + d[0], ny = cy + d[1];
                        if (nx < 0 || ny < 0 || nx >= mw || ny >= mh) continue;
                        const size_t ni = static_cast<size_t>(ny) * mw + nx;
                        if (!vis[ni] && useBands[ni] == target) {
                            vis[ni] = 1;
                            q.push_back(static_cast<int>(ni));
                        }
                    }
                }
                if (q.size() >= minCompPx) compSizes.push_back(q.size());
            }
            res.notan.massCount = static_cast<int>(compSizes.size());
            if (!compSizes.empty()) {
                const size_t maxComp = *std::max_element(compSizes.begin(), compSizes.end());
                res.notan.largestMassShare = static_cast<double>(maxComp) / mn;
            }
            // Fragmentation near 0 when ~3..8 bold masses, rising toward 1 past ~35 fragments.
            res.notan.fragmentation = std::clamp((static_cast<double>(res.notan.massCount) - 4.0) / 36.0, 0.0, 1.0);
        }
    }

    // 3. Rest vs Detail rhythm
    res.rhythm.gridRows = std::clamp(opts.rhythmRows, 2, 16);
    res.rhythm.gridCols = std::clamp(opts.rhythmCols, 2, 16);
    if (!meas.grid.empty()) {
        std::vector<double> ed;
        ed.reserve(meas.grid.size());
        int restCnt = 0, detCnt = 0;
        for (const auto& g : meas.grid) {
            ed.push_back(g.edgeDensity);
            if (g.edgeDensity < 0.04) ++restCnt;
            else if (g.edgeDensity > 0.14) ++detCnt;
        }
        std::sort(ed.begin(), ed.end());
        const size_t i10 = std::min(ed.size() - 1, ed.size() / 10);
        const size_t i90 = std::min(ed.size() - 1, (ed.size() * 9) / 10);
        res.rhythm.restRatio = static_cast<double>(restCnt) / ed.size();
        res.rhythm.detailRatio = static_cast<double>(detCnt) / ed.size();
        res.rhythm.restDetailContrast = std::max(0.0, ed[i90] - ed[i10]);
    }

    // 4. Palette Harmony & Warm/Cool Light-Shadow Modeling
    {
        const auto& cols = meas.dominantColors;
        res.palette.tierCount = static_cast<int>(cols.size());
        res.palette.chromaRestraint = std::clamp(1.0 - meas.chromaP95 / 0.25, 0.0, 1.0);
        if (cols.size() >= 2) {
            std::vector<double> dists;
            for (size_t i = 0; i < cols.size(); ++i) {
                for (size_t j = i + 1; j < cols.size(); ++j) {
                    dists.push_back(color::oklabDeltaE(
                        color::srgbToOklab(cols[i].r, cols[i].g, cols[i].b),
                        color::srgbToOklab(cols[j].r, cols[j].g, cols[j].b)));
                }
            }
            std::sort(dists.begin(), dists.end());
            res.palette.minTierDeltaE = dists.front();
            res.palette.medianTierDeltaE = dists[dists.size() / 2];
        }

        // Compare mean warmth (Oklab a + b) in top vs bottom luminance quartiles
        const EyeImage small = downsampleMaxSide(img, 128);
        const size_t n = static_cast<size_t>(small.width) * small.height;
        struct PxLum { float L; float warm; };
        std::vector<PxLum> pxs(n);
        for (size_t i = 0; i < n; ++i) {
            const auto lab = color::srgbToOklab(small.rgba[i * 4], small.rgba[i * 4 + 1], small.rgba[i * 4 + 2]);
            pxs[i] = {static_cast<float>(lab.L), static_cast<float>(lab.a + lab.b)};
        }
        if (n >= 16) {
            const size_t q1 = n / 4;
            const size_t q3 = (n * 3) / 4;
            std::nth_element(pxs.begin(), pxs.begin() + q1, pxs.end(), [](const PxLum& x, const PxLum& y) { return x.L < y.L; });
            double coldWarm = 0.0;
            for (size_t i = 0; i < q1; ++i) coldWarm += pxs[i].warm;
            coldWarm /= std::max<size_t>(1, q1);

            std::nth_element(pxs.begin(), pxs.begin() + q3, pxs.end(), [](const PxLum& x, const PxLum& y) { return x.L < y.L; });
            double hiWarm = 0.0;
            for (size_t i = q3; i < n; ++i) hiWarm += pxs[i].warm;
            hiWarm /= std::max<size_t>(1, n - q3);
            res.palette.warmShadowShift = hiWarm - coldWarm;
        }
    }

    // Balanced, interpretable 0..100 composition score
    double sc = 100.0;
    sc -= 22.0 * res.notan.fragmentation;
    sc -= 25.0 * res.saliency.edgeCrowding;
    if (res.saliency.focalDominance < 1.08) sc -= 8.0 * (1.08 - res.saliency.focalDominance) / 0.08;
    if (res.rhythm.restRatio < 0.15) sc -= 12.0 * (0.15 - res.rhythm.restRatio) / 0.15;
    if (meas.dynamicRange < 0.18) sc -= 12.0 * (0.18 - meas.dynamicRange) / 0.18;
    res.score = std::clamp(sc, 0.0, 100.0);
    return res;
}

}  // namespace bro::eye
