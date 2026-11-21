#include "eye/eye_measure.h"
#include "eye/eye_color.h"

#include <broimage/sharpness.h>

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace bro::eye {
namespace {

constexpr double kPi = 3.14159265358979323846;

const uint8_t* rowPtr(EyeImageView img, int y) {
    return img.rgba + static_cast<size_t>(y) * img.effectiveStride();
}

void fft1D(std::vector<std::complex<double>>& a) {
    const size_t n = a.size();
    if (n <= 1) return;
    for (size_t i = 1, j = 0; i < n; ++i) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        const double ang = -2.0 * kPi / static_cast<double>(len);
        const std::complex<double> wlen(std::cos(ang), std::sin(ang));
        for (size_t i = 0; i < n; i += len) {
            std::complex<double> w(1.0, 0.0);
            for (size_t j = 0; j < len / 2; ++j) {
                const auto u = a[i + j];
                const auto v = a[i + j + len / 2] * w;
                a[i + j] = u + v;
                a[i + j + len / 2] = u - v;
                w *= wlen;
            }
        }
    }
}

double computeBanding(EyeImageView img) {
    if (img.width < 12 && img.height < 12) return 0.0;
    const int w = img.width;
    const int h = img.height;

    auto scanLine = [](const uint8_t* line, int n, int stepBytes, int64_t& banded, int64_t& counted) {
        if (n < 12) return;
        struct Run { uint8_t y; int len; };
        std::vector<Run> runs;
        runs.reserve(64);
        uint8_t curY = color::rec709Luma(line[0], line[1], line[2]);
        int curLen = 1;
        for (int i = 1; i < n; ++i) {
            const uint8_t* p = line + static_cast<size_t>(i) * stepBytes;
            const uint8_t y = color::rec709Luma(p[0], p[1], p[2]);
            if (y == curY) {
                ++curLen;
            } else {
                runs.push_back({curY, curLen});
                curY = y;
                curLen = 1;
            }
        }
        runs.push_back({curY, curLen});
        counted += n;
        if (runs.size() < 4) return;

        // A staircase is >= 3 consecutive flat runs (len >= 4) stepping by 1..5 in the same direction.
        size_t i = 0;
        while (i + 2 < runs.size()) {
            if (runs[i].len < 4) { ++i; continue; }
            const int d0 = static_cast<int>(runs[i + 1].y) - static_cast<int>(runs[i].y);
            if (d0 == 0 || std::abs(d0) > 5 || runs[i + 1].len < 4) { ++i; continue; }
            const int dir = d0 > 0 ? 1 : -1;
            size_t j = i + 1;
            int64_t span = runs[i].len + runs[i + 1].len;
            while (j + 1 < runs.size()) {
                const int dj = static_cast<int>(runs[j + 1].y) - static_cast<int>(runs[j].y);
                if (dj * dir <= 0 || std::abs(dj) > 5 || runs[j + 1].len < 4) break;
                span += runs[j + 1].len;
                ++j;
            }
            if (j - i >= 2) {
                banded += span;
                i = j + 1;
            } else {
                ++i;
            }
        }
    };

    int64_t banded = 0;
    int64_t counted = 0;
    const int rowStep = std::max(1, h / 128);
    for (int y = 0; y < h; y += rowStep) {
        scanLine(rowPtr(img, y), w, 4, banded, counted);
    }
    const int colStep = std::max(1, w / 128);
    const int stride = img.effectiveStride();
    for (int x = 0; x < w; x += colStep) {
        scanLine(img.rgba + static_cast<size_t>(x) * 4, h, stride, banded, counted);
    }
    if (counted <= 0) return 0.0;
    const double ratio = static_cast<double>(banded) / static_cast<double>(counted);
    return std::clamp(ratio * 2.5, 0.0, 1.0);
}

std::vector<EyeDominantColor> extractDominantColors(EyeImageView img, int maxColors) {
    if (maxColors <= 0 || !img.valid()) return {};
    constexpr int kBinsL = 8;
    constexpr int kBinsA = 8;
    constexpr int kBinsB = 8;
    constexpr int kTotalBins = kBinsL * kBinsA * kBinsB;

    struct BinAccum {
        int64_t count = 0;
        double sumL = 0.0, sumA = 0.0, sumB = 0.0;
    };
    std::vector<BinAccum> bins(kTotalBins);

    const int step = (img.width * img.height > 262144) ? 2 : 1;
    int64_t total = 0;
    for (int y = 0; y < img.height; y += step) {
        const uint8_t* p = rowPtr(img, y);
        for (int x = 0; x < img.width; x += step) {
            const uint8_t* px = p + x * 4;
            const auto lab = color::srgbToOklab(px[0], px[1], px[2]);
            const int bL = std::clamp(static_cast<int>(lab.L * kBinsL), 0, kBinsL - 1);
            const int bA = std::clamp(static_cast<int>((lab.a + 0.4) / 0.8 * kBinsA), 0, kBinsA - 1);
            const int bB = std::clamp(static_cast<int>((lab.b + 0.4) / 0.8 * kBinsB), 0, kBinsB - 1);
            auto& bin = bins[(bL * kBinsA + bA) * kBinsB + bB];
            ++bin.count;
            bin.sumL += lab.L;
            bin.sumA += lab.a;
            bin.sumB += lab.b;
            ++total;
        }
    }
    if (total == 0) return {};

    struct Cluster {
        int64_t count;
        color::Oklab lab;
    };
    std::vector<Cluster> clusters;
    for (const auto& b : bins) {
        if (b.count == 0) continue;
        clusters.push_back({b.count, {b.sumL / b.count, b.sumA / b.count, b.sumB / b.count}});
    }
    std::sort(clusters.begin(), clusters.end(), [](const Cluster& x, const Cluster& y) {
        return x.count > y.count;
    });

    // Merge nearby clusters (within Oklab deltaE < 9.0) greedily by descending mass.
    std::vector<Cluster> merged;
    for (const auto& c : clusters) {
        bool absorbed = false;
        for (auto& m : merged) {
            if (color::oklabDeltaE(c.lab, m.lab) < 9.0) {
                const double wTot = static_cast<double>(m.count + c.count);
                m.lab.L = (m.lab.L * m.count + c.lab.L * c.count) / wTot;
                m.lab.a = (m.lab.a * m.count + c.lab.a * c.count) / wTot;
                m.lab.b = (m.lab.b * m.count + c.lab.b * c.count) / wTot;
                m.count += c.count;
                absorbed = true;
                break;
            }
        }
        if (!absorbed) merged.push_back(c);
        if (merged.size() >= static_cast<size_t>(maxColors * 3)) break;
    }
    std::sort(merged.begin(), merged.end(), [](const Cluster& x, const Cluster& y) {
        return x.count > y.count;
    });

    std::vector<EyeDominantColor> out;
    const size_t limit = std::min(merged.size(), static_cast<size_t>(maxColors));
    for (size_t i = 0; i < limit; ++i) {
        const auto rgb = color::oklabToSrgb(merged[i].lab);
        const auto lch = color::oklabToOklch(merged[i].lab);
        EyeDominantColor dc;
        dc.r = rgb.r;
        dc.g = rgb.g;
        dc.b = rgb.b;
        dc.hex = color::rgbToHex(rgb);
        dc.L = lch.L;
        dc.C = lch.C;
        dc.h = lch.h;
        dc.share = static_cast<double>(merged[i].count) / static_cast<double>(total);
        out.push_back(std::move(dc));
    }
    return out;
}

EyeSeam computeSeam(int w, int h, const std::vector<double>& L,
                    const std::vector<double>& A, const std::vector<double>& B) {
    EyeSeam s;
    if (w < 4 || h < 4) return s;

    auto pxDeltaE = [&](size_t i0, size_t i1) {
        const double dL = L[i0] - L[i1];
        const double da = A[i0] - A[i1];
        const double db = B[i0] - B[i1];
        return 100.0 * std::sqrt(dL * dL + da * da + db * db);
    };

    double hEdgeSum = 0.0;
    for (int x = 0; x < w; ++x) {
        hEdgeSum += pxDeltaE(static_cast<size_t>(x), static_cast<size_t>(h - 1) * w + x);
    }
    const double hEdgeDE = hEdgeSum / w;

    double hIntSum = 0.0;
    int64_t hIntCnt = 0;
    const int yStep = std::max(1, h / 16);
    const int xStep = std::max(1, w / 64);
    for (int y = 0; y + 1 < h; y += yStep) {
        for (int x = 0; x < w; x += xStep) {
            hIntSum += pxDeltaE(static_cast<size_t>(y) * w + x, static_cast<size_t>(y + 1) * w + x);
            ++hIntCnt;
        }
    }
    const double hIntDE = hIntCnt > 0 ? hIntSum / hIntCnt : 0.0;

    double vEdgeSum = 0.0;
    for (int y = 0; y < h; ++y) {
        vEdgeSum += pxDeltaE(static_cast<size_t>(y) * w, static_cast<size_t>(y) * w + (w - 1));
    }
    const double vEdgeDE = vEdgeSum / h;

    double vIntSum = 0.0;
    int64_t vIntCnt = 0;
    const int yStepV = std::max(1, h / 64);
    const int xStepV = std::max(1, w / 16);
    for (int y = 0; y < h; y += yStepV) {
        for (int x = 0; x + 1 < w; x += xStepV) {
            vIntSum += pxDeltaE(static_cast<size_t>(y) * w + x, static_cast<size_t>(y) * w + x + 1);
            ++vIntCnt;
        }
    }
    const double vIntDE = vIntCnt > 0 ? vIntSum / vIntCnt : 0.0;

    s.hSeam = std::clamp((hEdgeDE - hIntDE * 1.15) / std::max(3.0, hIntDE * 2.5 + 4.0), 0.0, 1.0);
    s.vSeam = std::clamp((vEdgeDE - vIntDE * 1.15) / std::max(3.0, vIntDE * 2.5 + 4.0), 0.0, 1.0);
    s.edgeDeltaE = 0.5 * (hEdgeDE + vEdgeDE);
    s.interiorDeltaE = 0.5 * (hIntDE + vIntDE);
    s.score = 0.75 * std::max(s.hSeam, s.vSeam) + 0.25 * 0.5 * (s.hSeam + s.vSeam);
    return s;
}

void separableBoxBlur(const std::vector<double>& src, int w, int h, int radius, std::vector<double>& dst) {
    std::vector<double> tmp(static_cast<size_t>(w) * h);
    std::vector<double> pref(static_cast<size_t>(std::max(w, h)) + 1, 0.0);
    for (int y = 0; y < h; ++y) {
        const double* row = src.data() + static_cast<size_t>(y) * w;
        pref[0] = 0.0;
        for (int x = 0; x < w; ++x) pref[x + 1] = pref[x] + row[x];
        double* out = tmp.data() + static_cast<size_t>(y) * w;
        for (int x = 0; x < w; ++x) {
            const int x0 = std::max(0, x - radius);
            const int x1 = std::min(w - 1, x + radius);
            out[x] = (pref[x1 + 1] - pref[x0]) / static_cast<double>(x1 - x0 + 1);
        }
    }
    dst.resize(static_cast<size_t>(w) * h);
    for (int x = 0; x < w; ++x) {
        pref[0] = 0.0;
        for (int y = 0; y < h; ++y) pref[y + 1] = pref[y] + tmp[static_cast<size_t>(y) * w + x];
        for (int y = 0; y < h; ++y) {
            const int y0 = std::max(0, y - radius);
            const int y1 = std::min(h - 1, y + radius);
            dst[static_cast<size_t>(y) * w + x] = (pref[y1 + 1] - pref[y0]) / static_cast<double>(y1 - y0 + 1);
        }
    }
}

EyeDetailBands computeDetailBands(int w, int h, const std::vector<double>& L) {
    EyeDetailBands db;
    if (w < 8 || h < 8) return db;
    const int sw = std::min(w, 256);
    const int sh = std::min(h, 256);
    std::vector<double> base(static_cast<size_t>(sw) * sh);
    for (int y = 0; y < sh; ++y) {
        const int sy = (y * h) / sh;
        for (int x = 0; x < sw; ++x) {
            const int sx = (x * w) / sw;
            base[static_cast<size_t>(y) * sw + x] = L[static_cast<size_t>(sy) * w + sx];
        }
    }
    std::vector<double> b1, b4, b16;
    separableBoxBlur(base, sw, sh, 1, b1);
    separableBoxBlur(base, sw, sh, 4, b4);
    separableBoxBlur(base, sw, sh, 16, b16);

    double sHigh = 0.0, sMid = 0.0, sLow = 0.0;
    const size_t n = base.size();
    for (size_t i = 0; i < n; ++i) {
        const double dh = base[i] - b1[i];
        const double dm = b1[i] - b4[i];
        const double dl = b4[i] - b16[i];
        sHigh += dh * dh;
        sMid += dm * dm;
        sLow += dl * dl;
    }
    const double rHigh = std::sqrt(sHigh / n);
    const double rMid = std::sqrt(sMid / n);
    const double rLow = std::sqrt(sLow / n);
    const double tot = rHigh + rMid + rLow;
    if (tot > 1e-6) {
        db.high = rHigh / tot;
        db.mid = rMid / tot;
        db.low = rLow / tot;
    }
    return db;
}

}  // namespace

EyeImage resampleRgba(EyeImageView src, int dstW, int dstH) {
    EyeImage out;
    if (!src.valid() || dstW <= 0 || dstH <= 0) return out;
    out.width = dstW;
    out.height = dstH;
    out.rgba.resize(static_cast<size_t>(dstW) * dstH * 4);
    for (int y = 0; y < dstH; ++y) {
        const double sy = (dstH > 1) ? (y + 0.5) * src.height / dstH - 0.5 : 0.0;
        const int y0 = std::clamp(static_cast<int>(std::floor(sy)), 0, src.height - 1);
        const int y1 = std::min(y0 + 1, src.height - 1);
        const double fy = std::clamp(sy - y0, 0.0, 1.0);
        const uint8_t* row0 = rowPtr(src, y0);
        const uint8_t* row1 = rowPtr(src, y1);
        uint8_t* dstRow = out.rgba.data() + static_cast<size_t>(y) * dstW * 4;
        for (int x = 0; x < dstW; ++x) {
            const double sx = (dstW > 1) ? (x + 0.5) * src.width / dstW - 0.5 : 0.0;
            const int x0 = std::clamp(static_cast<int>(std::floor(sx)), 0, src.width - 1);
            const int x1 = std::min(x0 + 1, src.width - 1);
            const double fx = std::clamp(sx - x0, 0.0, 1.0);
            for (int c = 0; c < 4; ++c) {
                const double v00 = row0[x0 * 4 + c];
                const double v10 = row0[x1 * 4 + c];
                const double v01 = row1[x0 * 4 + c];
                const double v11 = row1[x1 * 4 + c];
                const double v0 = v00 + (v10 - v00) * fx;
                const double v1 = v01 + (v11 - v01) * fx;
                dstRow[x * 4 + c] = static_cast<uint8_t>(std::clamp(std::lround(v0 + (v1 - v0) * fy), 0L, 255L));
            }
        }
    }
    return out;
}

void computeFftPowerMap(EyeImageView img, int fftSize, std::vector<float>& outLogNorm, EyeTiling* outTiling) {
    outLogNorm.clear();
    if (outTiling) *outTiling = EyeTiling{};
    if (!img.valid() || fftSize < 16 || (fftSize & (fftSize - 1)) != 0) return;

    const int N = fftSize;
    std::vector<double> gray(static_cast<size_t>(N) * N, 0.0);
    double mean = 0.0;
    for (int y = 0; y < N; ++y) {
        const int sy = std::clamp((y * img.height) / N, 0, img.height - 1);
        const uint8_t* src = rowPtr(img, sy);
        for (int x = 0; x < N; ++x) {
            const int sx = std::clamp((x * img.width) / N, 0, img.width - 1);
            const uint8_t* p = src + sx * 4;
            const double l = color::rec709Luma(p[0], p[1], p[2]) / 255.0;
            gray[static_cast<size_t>(y) * N + x] = l;
            mean += l;
        }
    }
    mean /= static_cast<double>(N * N);

    // Detrend linear gradient plane before Hann windowing so smooth ramps
    // and directional lighting gradients do not create axis DFT leakage.
    double sxSum = 0.0, sySum = 0.0, xxSum = 0.0;
    const double mid = 0.5 * (N - 1);
    for (int i = 0; i < N; ++i) xxSum += (i - mid) * (i - mid);
    const double planeDen = std::max(1.0, xxSum * N);
    for (int y = 0; y < N; ++y) {
        for (int x = 0; x < N; ++x) {
            const double d = gray[static_cast<size_t>(y) * N + x] - mean;
            sxSum += d * (x - mid);
            sySum += d * (y - mid);
        }
    }
    const double gx = sxSum / planeDen;
    const double gy = sySum / planeDen;

    double signalVar = 0.0;
    std::vector<double> hann(N);
    for (int i = 0; i < N; ++i) {
        hann[i] = 0.5 * (1.0 - std::cos(2.0 * kPi * i / N));
    }

    std::vector<std::complex<double>> grid(static_cast<size_t>(N) * N);
    for (int y = 0; y < N; ++y) {
        for (int x = 0; x < N; ++x) {
            const double d = (gray[static_cast<size_t>(y) * N + x] - mean) -
                             (gx * (x - mid) + gy * (y - mid));
            signalVar += d * d;
            grid[static_cast<size_t>(y) * N + x] = std::complex<double>(d * hann[x] * hann[y], 0.0);
        }
    }
    signalVar /= static_cast<double>(N * N);

    std::vector<std::complex<double>> line(N);
    for (int y = 0; y < N; ++y) {
        for (int x = 0; x < N; ++x) line[x] = grid[static_cast<size_t>(y) * N + x];
        fft1D(line);
        for (int x = 0; x < N; ++x) grid[static_cast<size_t>(y) * N + x] = line[x];
    }
    for (int x = 0; x < N; ++x) {
        for (int y = 0; y < N; ++y) line[y] = grid[static_cast<size_t>(y) * N + x];
        fft1D(line);
        for (int y = 0; y < N; ++y) grid[static_cast<size_t>(y) * N + x] = line[y];
    }

    const int half = N / 2;
    std::vector<double> power(static_cast<size_t>(N) * N, 0.0);
    double maxLog = 1e-12;
    outLogNorm.resize(static_cast<size_t>(N) * N, 0.0f);
    for (int y = 0; y < N; ++y) {
        const int sy = (y + half) % N;
        for (int x = 0; x < N; ++x) {
            const int sx = (x + half) % N;
            const double p = std::norm(grid[static_cast<size_t>(sy) * N + sx]);
            power[static_cast<size_t>(y) * N + x] = p;
            const double lp = std::log1p(p);
            outLogNorm[static_cast<size_t>(y) * N + x] = static_cast<float>(lp);
            if (lp > maxLog) maxLog = lp;
        }
    }
    for (float& v : outLogNorm) v = static_cast<float>(v / maxLog);

    if (!outTiling || signalVar < 1e-5) return;

    // Radial ring background power for normalization (r in [3, half - 2]).
    const int maxR = half;
    std::vector<std::vector<double>> rings(maxR + 1);
    for (int y = 0; y < N; ++y) {
        for (int x = 0; x < N; ++x) {
            const int du = x - half;
            const int dv = y - half;
            const int r = static_cast<int>(std::lround(std::hypot(du, dv)));
            if (r >= 3 && r <= maxR - 2) {
                rings[r].push_back(power[static_cast<size_t>(y) * N + x]);
            }
        }
    }
    std::vector<double> ringMed(maxR + 1, 1.0);
    for (int r = 3; r <= maxR - 2; ++r) {
        auto& v = rings[r];
        if (v.empty()) continue;
        std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
        ringMed[r] = std::max(1e-6, v[v.size() / 2]);
    }

    double bestRatio = 0.0;
    int bestU = 0, bestV = 0;
    double horizSum = 0.0, vertSum = 0.0;
    double ringBuf[25];
    const double totalPower = std::max(1e-6, signalVar * N * N * 0.14);

    for (int y = 3; y < N - 3; ++y) {
        for (int x = 3; x < N - 3; ++x) {
            const int du = x - half;
            const int dv = y - half;
            const int r = static_cast<int>(std::lround(std::hypot(du, dv)));
            if (r < 4 || r > maxR - 3) continue;

            const double p = power[static_cast<size_t>(y) * N + x];
            // Must hold a nontrivial share of signal energy and dominate its
            // immediate 2-3px neighborhood along every axis.
            if (p < totalPower * 0.005) continue;

            int rc = 0;
            double axisNeighbor = 0.0;
            for (int oy = -2; oy <= 2; ++oy) {
                for (int ox = -2; ox <= 2; ++ox) {
                    if (std::abs(ox) <= 1 && std::abs(oy) <= 1) continue;
                    const double np = power[static_cast<size_t>(y + oy) * N + (x + ox)];
                    ringBuf[rc++] = np;
                    if (ox == 0 || oy == 0) axisNeighbor = std::max(axisNeighbor, np);
                }
            }
            std::nth_element(ringBuf, ringBuf + rc / 2, ringBuf + rc);
            const double localMed = std::max(1e-6, ringBuf[rc / 2]);
            const double axisGate = p / std::max(1e-6, axisNeighbor);

            const double ratioRad = p / ringMed[r];
            const double ratioLoc = p / localMed;
            const double scoreHere = std::min({ratioRad, ratioLoc * 1.5, axisGate * 2.0});

            if (scoreHere > 12.0) {
                if (std::abs(du) > std::abs(dv)) horizSum += scoreHere;
                else if (std::abs(dv) > std::abs(du)) vertSum += scoreHere;
            }
            if (scoreHere > bestRatio) {
                bestRatio = scoreHere;
                bestU = std::abs(du);
                bestV = std::abs(dv);
            }
        }
    }

    outTiling->peakRatio = bestRatio;
    outTiling->score = (bestRatio <= 8.0) ? 0.0 : std::clamp(std::log2(bestRatio / 8.0) / 5.0, 0.0, 1.0);
    if (bestRatio > 12.0) {
        outTiling->periodX = bestU > 0 ? static_cast<double>(img.width) / static_cast<double>(bestU) : 0.0;
        outTiling->periodY = bestV > 0 ? static_cast<double>(img.height) / static_cast<double>(bestV) : 0.0;
    }
    const double hvTot = horizSum + vertSum;
    if (hvTot > 0.0) {
        outTiling->horizontalBias = horizSum / hvTot;
        outTiling->verticalBias = vertSum / hvTot;
    }
}

EyeMeasurement measureImage(EyeImageView img, const EyeMeasureOptions& opts) {
    EyeMeasurement m;
    if (!img.valid()) return m;

    const int w = img.width;
    const int h = img.height;
    const int64_t totalPx = static_cast<int64_t>(w) * h;
    m.width = w;
    m.height = h;
    m.pixels = totalPx;
    m.aspect = static_cast<double>(w) / static_cast<double>(h);

    const int rows = std::clamp(opts.gridRows, 1, std::min(16, h));
    const int cols = std::clamp(opts.gridCols, 1, std::min(16, w));

    std::vector<double> Lbuf(totalPx);
    std::vector<double> Abuf(totalPx);
    std::vector<double> Bbuf(totalPx);

    uint64_t lHist[256]{};
    uint64_t cHist[256]{};
    double hueHist[12]{};

    double sumL = 0.0, sumL2 = 0.0, minL = 1.0, maxL = 0.0;
    double sumC = 0.0, sumC2 = 0.0;
    double warmSum = 0.0, chromaWeightSum = 0.0;
    int64_t crushedCount = 0, blownCount = 0, colorfulCount = 0;

    for (int y = 0; y < h; ++y) {
        const uint8_t* p = rowPtr(img, y);
        for (int x = 0; x < w; ++x) {
            const uint8_t* px = p + x * 4;
            const uint8_t r = px[0], g = px[1], b = px[2];
            const auto lab = color::srgbToOklab(r, g, b);
            const auto lch = color::oklabToOklch(lab);
            const size_t idx = static_cast<size_t>(y) * w + x;
            Lbuf[idx] = lab.L;
            Abuf[idx] = lab.a;
            Bbuf[idx] = lab.b;

            sumL += lab.L;
            sumL2 += lab.L * lab.L;
            minL = std::min(minL, lab.L);
            maxL = std::max(maxL, lab.L);
            sumC += lch.C;
            sumC2 += lch.C * lch.C;

            const int lBin = std::clamp(static_cast<int>(lab.L * 255.0 + 0.5), 0, 255);
            const int cBin = std::clamp(static_cast<int>((lch.C / 0.4) * 255.0 + 0.5), 0, 255);
            ++lHist[lBin];
            ++cHist[cBin];

            const int zone = std::clamp(static_cast<int>(lab.L * 10.0), 0, 9);
            m.zones[zone] += 1.0;

            if ((r <= 4 && g <= 4 && b <= 4) || lab.L < 0.04) ++crushedCount;
            if ((r >= 251 && g >= 251 && b >= 251) || lab.L > 0.97) ++blownCount;

            if (lch.C > 0.04) {
                ++colorfulCount;
                const int hBin = std::clamp(static_cast<int>(lch.h / 30.0), 0, 11);
                hueHist[hBin] += lch.C;
                const double rad = (lch.h - 45.0) * (kPi / 180.0);
                warmSum += lch.C * std::cos(rad);
                chromaWeightSum += lch.C;
            }
        }
    }

    const double invN = 1.0 / static_cast<double>(totalPx);
    m.lumaMean = sumL * invN;
    m.lumaStd = std::sqrt(std::max(0.0, sumL2 * invN - m.lumaMean * m.lumaMean));
    m.lumaMin = minL;
    m.lumaMax = maxL;
    m.exposureBias = m.lumaMean - 0.5;
    m.crushedBlackRatio = crushedCount * invN;
    m.blownWhiteRatio = blownCount * invN;
    for (double& z : m.zones) z *= invN;

    auto histQuantile = [&](const uint64_t hist[256], double q, double scale) {
        const uint64_t target = static_cast<uint64_t>(std::ceil(q * totalPx));
        uint64_t acc = 0;
        for (int i = 0; i < 256; ++i) {
            acc += hist[i];
            if (acc >= target) return (static_cast<double>(i) / 255.0) * scale;
        }
        return scale;
    };
    m.lumaP05 = histQuantile(lHist, 0.05, 1.0);
    m.lumaP50 = histQuantile(lHist, 0.50, 1.0);
    m.lumaP95 = histQuantile(lHist, 0.95, 1.0);
    m.dynamicRange = std::max(0.0, m.lumaP95 - m.lumaP05);

    m.chromaMean = sumC * invN;
    m.chromaStd = std::sqrt(std::max(0.0, sumC2 * invN - m.chromaMean * m.chromaMean));
    m.chromaP95 = histQuantile(cHist, 0.95, 0.4);
    m.colorfulRatio = colorfulCount * invN;
    if (chromaWeightSum > 1e-6) {
        m.warmCoolBalance = std::clamp(warmSum / chromaWeightSum, -1.0, 1.0);
        double ent = 0.0;
        for (double hw : hueHist) {
            const double p = hw / chromaWeightSum;
            if (p > 1e-12) ent -= p * std::log2(p);
        }
        m.hueEntropy = std::clamp(ent / std::log2(12.0), 0.0, 1.0);
    }

    m.bandingScore = computeBanding(img);
    m.dominantColors = extractDominantColors(img, opts.maxColors);

    // Sharpness via broimage::sharpness_u8
    const auto sharp = broimage::sharpness_u8(img.rgba, w, h, 4, img.effectiveStride());
    m.laplacianVariance = sharp.laplacian_variance;
    m.tenengrad = sharp.tenengrad;
    m.sharpness = std::clamp(std::log1p(sharp.laplacian_variance) / 8.0, 0.0, 1.0);

    // Gradient / edge density, saliency balance, and grid breakdown.
    struct GridAccum {
        int64_t count = 0;
        double sumL = 0.0, sumC = 0.0;
        int64_t edges = 0, crushed = 0, blown = 0;
    };
    std::vector<GridAccum> gridAcc(static_cast<size_t>(rows) * cols);

    int64_t edgeCount = 0;
    int64_t calmCount = 0;
    double salTotal = 0.0, salX = 0.0, salY = 0.0;
    double salLeft = 0.0, salRight = 0.0, salTop = 0.0, salBottom = 0.0;
    double salThirds = 0.0;

    for (int y = 0; y < h; ++y) {
        const int gy = std::min(rows - 1, (y * rows) / h);
        const int ym = std::max(0, y - 1);
        const int yp = std::min(h - 1, y + 1);
        for (int x = 0; x < w; ++x) {
            const int gx = std::min(cols - 1, (x * cols) / w);
            const int xm = std::max(0, x - 1);
            const int xp = std::min(w - 1, x + 1);
            const size_t idx = static_cast<size_t>(y) * w + x;

            const double dxL = Lbuf[static_cast<size_t>(y) * w + xp] - Lbuf[static_cast<size_t>(y) * w + xm];
            const double dyL = Lbuf[static_cast<size_t>(yp) * w + x] - Lbuf[static_cast<size_t>(ym) * w + x];
            const double dxA = Abuf[static_cast<size_t>(y) * w + xp] - Abuf[static_cast<size_t>(y) * w + xm];
            const double dyA = Abuf[static_cast<size_t>(yp) * w + x] - Abuf[static_cast<size_t>(ym) * w + x];
            const double dxB = Bbuf[static_cast<size_t>(y) * w + xp] - Bbuf[static_cast<size_t>(y) * w + xm];
            const double dyB = Bbuf[static_cast<size_t>(yp) * w + x] - Bbuf[static_cast<size_t>(ym) * w + x];

            const double grad = std::sqrt(dxL * dxL + dyL * dyL + 0.5 * (dxA * dxA + dyA * dyA + dxB * dxB + dyB * dyB));
            const bool isEdge = grad > 0.06;
            const bool isCalm = grad < 0.015;
            if (isEdge) ++edgeCount;
            if (isCalm) ++calmCount;

            const double dMean = std::abs(Lbuf[idx] - m.lumaMean);
            const double C = std::hypot(Abuf[idx], Bbuf[idx]);
            const double sal = grad + 0.35 * dMean + 0.5 * C;
            const double nx = (x + 0.5) / w;
            const double ny = (y + 0.5) / h;
            salTotal += sal;
            salX += sal * nx;
            salY += sal * ny;
            if (nx < 0.5) salLeft += sal; else salRight += sal;
            if (ny < 0.5) salTop += sal; else salBottom += sal;

            const double dThirdX = std::min(std::abs(nx - 1.0 / 3.0), std::abs(nx - 2.0 / 3.0));
            const double dThirdY = std::min(std::abs(ny - 1.0 / 3.0), std::abs(ny - 2.0 / 3.0));
            if (dThirdX < 0.12 && dThirdY < 0.12) salThirds += sal;

            auto& cell = gridAcc[static_cast<size_t>(gy) * cols + gx];
            ++cell.count;
            cell.sumL += Lbuf[idx];
            cell.sumC += C;
            if (isEdge) ++cell.edges;
            if (Lbuf[idx] < 0.04) ++cell.crushed;
            if (Lbuf[idx] > 0.97) ++cell.blown;
        }
    }

    m.edgeDensity = edgeCount * invN;
    m.whitespaceRatio = calmCount * invN;
    m.clutterScore = std::clamp(0.65 * std::min(1.0, m.edgeDensity * 2.5) + 0.35 * (1.0 - m.whitespaceRatio), 0.0, 1.0);

    if (salTotal > 1e-9) {
        m.balance.centerOfMassX = salX / salTotal;
        m.balance.centerOfMassY = salY / salTotal;
        m.balance.leftRightDiff = (salRight - salLeft) / salTotal;
        m.balance.topBottomDiff = (salBottom - salTop) / salTotal;
        m.balance.thirdsEnergy = salThirds / salTotal;
    }

    m.grid.reserve(static_cast<size_t>(rows) * cols);
    for (int gy = 0; gy < rows; ++gy) {
        const int y0 = (gy * h) / rows;
        const int y1 = ((gy + 1) * h) / rows;
        for (int gx = 0; gx < cols; ++gx) {
            const int x0 = (gx * w) / cols;
            const int x1 = ((gx + 1) * w) / cols;
            const auto& acc = gridAcc[static_cast<size_t>(gy) * cols + gx];
            EyeGridCell cell;
            cell.x = x0;
            cell.y = y0;
            cell.w = x1 - x0;
            cell.h = y1 - y0;
            if (acc.count > 0) {
                const double invC = 1.0 / static_cast<double>(acc.count);
                cell.lumaMean = acc.sumL * invC;
                cell.chromaMean = acc.sumC * invC;
                cell.edgeDensity = acc.edges * invC;
                cell.crushedBlackRatio = acc.crushed * invC;
                cell.blownWhiteRatio = acc.blown * invC;
            }
            m.grid.push_back(cell);
        }
    }

    m.seam = computeSeam(w, h, Lbuf, Abuf, Bbuf);
    m.detailBands = computeDetailBands(w, h, Lbuf);

    if (opts.detectTiling && w >= 16 && h >= 16) {
        const int fftN = (std::min(w, h) >= 96) ? 128 : ((std::min(w, h) >= 48) ? 64 : 32);
        std::vector<float> fftMap;
        computeFftPowerMap(img, fftN, fftMap, &m.tiling);
    }

    return m;
}

}  // namespace bro::eye
