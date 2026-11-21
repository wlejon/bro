#include "eye/eye_silhouette.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace bro::eye {
namespace {

constexpr double kPi = 3.14159265358979323846;

struct Pt2 {
    double x = 0.0;
    double y = 0.0;
};

double cross2(const Pt2& o, const Pt2& a, const Pt2& b) {
    return (a.x - o.x) * (b.y - o.y) - (a.y - o.y) * (b.x - o.x);
}

std::vector<Pt2> convexHull2D(std::vector<Pt2> pts) {
    if (pts.size() <= 2) return pts;
    std::sort(pts.begin(), pts.end(), [](const Pt2& a, const Pt2& b) {
        return a.x < b.x || (a.x == b.x && a.y < b.y);
    });
    std::vector<Pt2> hull;
    for (const auto& p : pts) {
        while (hull.size() >= 2 && cross2(hull[hull.size() - 2], hull.back(), p) <= 0.0) hull.pop_back();
        hull.push_back(p);
    }
    const size_t lowerSz = hull.size();
    for (size_t i = pts.size(); i-- > 0;) {
        const auto& p = pts[i];
        while (hull.size() > lowerSz && cross2(hull[hull.size() - 2], hull.back(), p) <= 0.0) hull.pop_back();
        hull.push_back(p);
    }
    if (!hull.empty()) hull.pop_back();
    return hull;
}

double polygonArea(const std::vector<Pt2>& poly) {
    if (poly.size() < 3) return 0.0;
    double a = 0.0;
    for (size_t i = 0; i < poly.size(); ++i) {
        const auto& p0 = poly[i];
        const auto& p1 = poly[(i + 1) % poly.size()];
        a += p0.x * p1.y - p1.x * p0.y;
    }
    return 0.5 * std::abs(a);
}

std::vector<Pt2> traceOuterContour(const std::vector<uint8_t>& mask, int w, int h) {
    // Find top-left foreground pixel
    int sx = -1, sy = -1;
    for (int y = 0; y < h && sx < 0; ++y) {
        for (int x = 0; x < w; ++x) {
            if (mask[static_cast<size_t>(y) * w + x]) { sx = x; sy = y; break; }
        }
    }
    if (sx < 0) return {};

    // Moore-Neighbor 8-connected clockwise offsets: N, NE, E, SE, S, SW, W, NW
    static constexpr int kDx[8] = {0, 1, 1, 1, 0, -1, -1, -1};
    static constexpr int kDy[8] = {-1, -1, 0, 1, 1, 1, 0, -1};

    auto isFg = [&](int x, int y) {
        return x >= 0 && y >= 0 && x < w && y < h && mask[static_cast<size_t>(y) * w + x] != 0;
    };

    std::vector<Pt2> contour;
    int cx = sx, cy = sy;
    int backtrackDir = 6;  // came from West (outside background)
    const int maxSteps = w * h * 2;

    for (int step = 0; step < maxSteps; ++step) {
        contour.push_back({static_cast<double>(cx), static_cast<double>(cy)});
        bool found = false;
        int nx = cx, ny = cy, nextBack = backtrackDir;
        for (int k = 1; k <= 8; ++k) {
            const int d = (backtrackDir + k) & 7;
            const int tx = cx + kDx[d];
            const int ty = cy + kDy[d];
            if (isFg(tx, ty)) {
                nx = tx;
                ny = ty;
                nextBack = (d + 4) & 7;
                found = true;
                break;
            }
        }
        if (!found) break;
        cx = nx;
        cy = ny;
        backtrackDir = nextBack;
        if (cx == sx && cy == sy && step > 4) break;
    }
    return contour;
}

std::vector<Pt2> resampleClosedContour(const std::vector<Pt2>& pts, int nOut) {
    if (pts.size() < 8 || nOut < 8) return {};
    const size_t n = pts.size();
    std::vector<double> cum(n + 1, 0.0);
    for (size_t i = 0; i < n; ++i) {
        const auto& a = pts[i];
        const auto& b = pts[(i + 1) % n];
        cum[i + 1] = cum[i] + std::hypot(b.x - a.x, b.y - a.y);
    }
    const double perim = cum.back();
    if (perim < 1e-4) return {};

    std::vector<Pt2> out;
    out.reserve(nOut);
    size_t seg = 0;
    for (int i = 0; i < nOut; ++i) {
        const double target = (perim * i) / nOut;
        while (seg + 1 < n && cum[seg + 1] < target) ++seg;
        const double len = std::max(1e-8, cum[seg + 1] - cum[seg]);
        const double t = std::clamp((target - cum[seg]) / len, 0.0, 1.0);
        const auto& a = pts[seg];
        const auto& b = pts[(seg + 1) % n];
        out.push_back({a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t});
    }
    return out;
}

void analyzeContourCurvature(const std::vector<Pt2>& rawContour, int res, EyeSilhouetteView& view) {
    constexpr int kN = 128;
    const auto uniform = resampleClosedContour(rawContour, kN);
    if (uniform.size() < 16) return;

    // Smooth contour lightly (2 passes circular [0.25, 0.5, 0.25]) to remove pixel staircase aliasing
    std::vector<Pt2> sm = uniform, tmp(kN);
    for (int pass = 0; pass < 3; ++pass) {
        for (int i = 0; i < kN; ++i) {
            const auto& p = sm[(i + kN - 1) % kN];
            const auto& c = sm[i];
            const auto& n = sm[(i + 1) % kN];
            tmp[i] = {0.25 * p.x + 0.5 * c.x + 0.25 * n.x, 0.25 * p.y + 0.5 * c.y + 0.25 * n.y};
        }
        sm.swap(tmp);
    }

    double perim = 0.0;
    for (int i = 0; i < kN; ++i) {
        perim += std::hypot(sm[(i + 1) % kN].x - sm[i].x, sm[(i + 1) % kN].y - sm[i].y);
    }
    const double ds = std::max(1e-5, perim / kN);

    std::vector<double> kappa(kN, 0.0);
    int flatCnt = 0, sharpCnt = 0;
    for (int i = 0; i < kN; ++i) {
        const auto& pm = sm[(i + kN - 1) % kN];
        const auto& pc = sm[i];
        const auto& pp = sm[(i + 1) % kN];
        const double xp = (pp.x - pm.x) / (2.0 * ds);
        const double yp = (pp.y - pm.y) / (2.0 * ds);
        const double xpp = (pp.x - 2.0 * pc.x + pm.x) / (ds * ds);
        const double ypp = (pp.y - 2.0 * pc.y + pm.y) / (ds * ds);
        const double denom = std::pow(xp * xp + yp * yp, 1.5);
        // Scale by perimeter so kappa is scale-invariant (~2*pi for a circle)
        const double k = (denom > 1e-8) ? ((xp * ypp - yp * xpp) / denom) * perim : 0.0;
        kappa[i] = k;
        if (std::abs(k) < 0.35) ++flatCnt;
        if (std::abs(k) > 22.0) {
            ++sharpCnt;
            if (view.anomalies.size() < 12) {
                view.anomalies.push_back({"sharp-corner", pc.x / res, pc.y / res, std::abs(k)});
            }
        }
    }

    // Count macro inflections vs short high-frequency wiggles
    int lastSign = 0, lastFlipIdx = -100;
    int inflections = 0, wiggles = 0;
    double dkSum = 0.0;
    for (int i = 0; i < kN; ++i) {
        dkSum += std::abs(kappa[(i + 1) % kN] - kappa[i]);
        if (std::abs(kappa[i]) < 0.8) continue;
        const int sgn = kappa[i] > 0.0 ? 1 : -1;
        if (lastSign != 0 && sgn != lastSign) {
            if (i - lastFlipIdx <= 4) {
                ++wiggles;
                if (view.anomalies.size() < 12) {
                    view.anomalies.push_back({"wiggle", sm[i].x / res, sm[i].y / res, std::abs(kappa[i])});
                }
            } else {
                ++inflections;
            }
            lastFlipIdx = i;
            lastSign = sgn;
        } else if (lastSign == 0) {
            lastSign = sgn;
            lastFlipIdx = i;
        }
    }

    view.flatRatio = static_cast<double>(flatCnt) / kN;
    view.inflectionCount = inflections;
    view.wiggleCount = wiggles;
    view.sharpCornerCount = sharpCnt;
    const double roughNorm = (dkSum / kN) / 6.0;
    view.smoothness = std::clamp(1.0 - 0.15 * wiggles - 0.08 * sharpCnt - 0.25 * roughNorm, 0.0, 1.0);
}

}  // namespace

std::vector<EyeSilhouetteView> analyzeMeshSilhouettes(const bromesh::MeshData& mesh,
                                                      const std::array<double, 3>& center,
                                                      double radius,
                                                      const EyeFormOptions& opts) {
    std::vector<EyeSilhouetteView> out;
    if (mesh.empty() || mesh.indices.size() < 3 || radius <= 1e-6) return out;

    const int nAz = std::clamp(opts.azimuthCount, 1, 24);
    const int res = std::clamp(opts.silhouetteRes, 64, 320);
    const double elevRad = opts.elevationDeg * (kPi / 180.0);
    const double ce = std::cos(elevRad), se = std::sin(elevRad);

    const size_t vc = mesh.vertexCount();
    const size_t tc = mesh.triangleCount();
    const size_t triStep = std::max<size_t>(1, tc / 32000);

    std::vector<double> uArr(vc), vArr(vc);
    std::vector<uint8_t> mask(static_cast<size_t>(res) * res);
    std::vector<uint8_t> ext(static_cast<size_t>(res) * res);
    std::vector<int> q;
    q.reserve(4096);

    for (int ai = 0; ai < nAz; ++ai) {
        const double azDeg = (360.0 * ai) / nAz;
        const double azRad = azDeg * (kPi / 180.0);
        const double ca = std::cos(azRad), sa = std::sin(azRad);

        // Camera basis: right=(ca, 0, -sa), forward=(sa*ce, -se, ca*ce), up=(sa*se, ce, ca*se)
        const double scale = (res * 0.42) / radius;
        for (size_t i = 0; i < vc; ++i) {
            const double px = mesh.positions[i * 3 + 0] - center[0];
            const double py = mesh.positions[i * 3 + 1] - center[1];
            const double pz = mesh.positions[i * 3 + 2] - center[2];
            const double u = px * ca - pz * sa;
            const double v = px * sa * se + py * ce + pz * ca * se;
            uArr[i] = res * 0.5 + u * scale;
            vArr[i] = res * 0.5 - v * scale;
        }

        std::fill(mask.begin(), mask.end(), 0);
        for (size_t t = 0; t < tc; t += triStep) {
            const uint32_t i0 = mesh.indices[t * 3 + 0];
            const uint32_t i1 = mesh.indices[t * 3 + 1];
            const uint32_t i2 = mesh.indices[t * 3 + 2];
            if (i0 >= vc || i1 >= vc || i2 >= vc) continue;

            const double x0 = uArr[i0], y0 = vArr[i0];
            const double x1 = uArr[i1], y1 = vArr[i1];
            const double x2 = uArr[i2], y2 = vArr[i2];
            const double area = (x1 - x0) * (y2 - y0) - (y1 - y0) * (x2 - x0);
            if (std::abs(area) < 0.08) continue;

            const int bx0 = std::clamp(static_cast<int>(std::floor(std::min({x0, x1, x2}))), 0, res - 1);
            const int bx1 = std::clamp(static_cast<int>(std::ceil(std::max({x0, x1, x2}))), 0, res - 1);
            const int by0 = std::clamp(static_cast<int>(std::floor(std::min({y0, y1, y2}))), 0, res - 1);
            const int by1 = std::clamp(static_cast<int>(std::ceil(std::max({y0, y1, y2}))), 0, res - 1);

            for (int yy = by0; yy <= by1; ++yy) {
                for (int xx = bx0; xx <= bx1; ++xx) {
                    const double fx = xx + 0.5, fy = yy + 0.5;
                    const double w0 = ((x1 - fx) * (y2 - fy) - (y1 - fy) * (x2 - fx)) / area;
                    const double w1 = ((x2 - fx) * (y0 - fy) - (y2 - fy) * (x0 - fx)) / area;
                    const double w2 = 1.0 - w0 - w1;
                    if (w0 >= -0.02 && w1 >= -0.02 && w2 >= -0.02) {
                        mask[static_cast<size_t>(yy) * res + xx] = 1;
                    }
                }
            }
        }

        EyeSilhouetteView sv;
        sv.azimuthDeg = azDeg;
        sv.elevationDeg = opts.elevationDeg;

        int fgCount = 0;
        std::vector<Pt2> boundaryPts;
        for (int y = 0; y < res; ++y) {
            for (int x = 0; x < res; ++x) {
                const size_t idx = static_cast<size_t>(y) * res + x;
                if (!mask[idx]) continue;
                ++fgCount;
                const bool edge = (x == 0 || y == 0 || x == res - 1 || y == res - 1 ||
                                   !mask[idx - 1] || !mask[idx + 1] || !mask[idx - res] || !mask[idx + res]);
                if (edge) boundaryPts.push_back({static_cast<double>(x), static_cast<double>(y)});
            }
        }
        sv.coverage = static_cast<double>(fgCount) / (res * res);

        if (fgCount > 8) {
            const auto hull = convexHull2D(boundaryPts);
            const double hullArea = polygonArea(hull);
            if (hullArea > 1.0) sv.solidity = std::clamp(static_cast<double>(fgCount) / hullArea, 0.0, 1.0);
            sv.perimeterRatio = static_cast<double>(boundaryPts.size()) / std::max(1.0, std::sqrt(static_cast<double>(fgCount)));
        }

        // Flood exterior air from border to discover enclosed negative-space holes & pinch gaps
        std::fill(ext.begin(), ext.end(), 0);
        q.clear();
        for (int x = 0; x < res; ++x) {
            if (!mask[x]) { ext[x] = 1; q.push_back(x); }
            const size_t bot = static_cast<size_t>(res - 1) * res + x;
            if (!mask[bot] && !ext[bot]) { ext[bot] = 1; q.push_back(static_cast<int>(bot)); }
        }
        for (int y = 1; y + 1 < res; ++y) {
            const size_t l = static_cast<size_t>(y) * res;
            const size_t r = l + res - 1;
            if (!mask[l] && !ext[l]) { ext[l] = 1; q.push_back(static_cast<int>(l)); }
            if (!mask[r] && !ext[r]) { ext[r] = 1; q.push_back(static_cast<int>(r)); }
        }
        for (size_t head = 0; head < q.size(); ++head) {
            const int cur = q[head];
            const int cx = cur % res, cy = cur / res;
            const int d4[4][2] = {{-1,0},{1,0},{0,-1},{0,1}};
            for (const auto& d : d4) {
                const int nx = cx + d[0], ny = cy + d[1];
                if (nx < 0 || ny < 0 || nx >= res || ny >= res) continue;
                const size_t ni = static_cast<size_t>(ny) * res + nx;
                if (!mask[ni] && !ext[ni]) { ext[ni] = 1; q.push_back(static_cast<int>(ni)); }
            }
        }

        // Collect enclosed holes (background pixels with !mask && !ext)
        for (int y = 1; y < res - 1; ++y) {
            for (int x = 1; x < res - 1; ++x) {
                const size_t idx = static_cast<size_t>(y) * res + x;
                if (mask[idx] || ext[idx]) continue;
                ext[idx] = 2;
                q.clear();
                q.push_back(static_cast<int>(idx));
                int hMinX = x, hMaxX = x, hMinY = y, hMaxY = y;
                double sumX = 0.0, sumY = 0.0;
                for (size_t head = 0; head < q.size(); ++head) {
                    const int cur = q[head];
                    const int cx = cur % res, cy = cur / res;
                    sumX += cx; sumY += cy;
                    hMinX = std::min(hMinX, cx); hMaxX = std::max(hMaxX, cx);
                    hMinY = std::min(hMinY, cy); hMaxY = std::max(hMaxY, cy);
                    const int d4[4][2] = {{-1,0},{1,0},{0,-1},{0,1}};
                    for (const auto& d : d4) {
                        const int nx = cx + d[0], ny = cy + d[1];
                        if (nx < 0 || ny < 0 || nx >= res || ny >= res) continue;
                        const size_t ni = static_cast<size_t>(ny) * res + nx;
                        if (!mask[ni] && !ext[ni]) { ext[ni] = 2; q.push_back(static_cast<int>(ni)); }
                    }
                }
                const double ratio = static_cast<double>(q.size()) / (res * res);
                if (ratio >= 0.0015) {
                    const double hw = std::max(1, hMaxX - hMinX + 1);
                    const double hh = std::max(1, hMaxY - hMinY + 1);
                    sv.holes.push_back({ratio, (sumX / q.size()) / res, (sumY / q.size()) / res,
                                        std::max(hw, hh) / std::min(hw, hh)});
                }
            }
        }
        sv.holeCount = static_cast<int>(sv.holes.size());

        // Narrow exterior air-gap pinches (foreground trapped 2..4 px apart)
        for (int y = 4; y < res - 4; y += 2) {
            for (int x = 4; x < res - 4; x += 2) {
                const size_t idx = static_cast<size_t>(y) * res + x;
                if (mask[idx]) continue;
                const bool tightH = (mask[idx - 2] || mask[idx - 3]) && (mask[idx + 2] || mask[idx + 3]);
                const bool tightV = (mask[idx - 2 * res] || mask[idx - 3 * res]) && (mask[idx + 2 * res] || mask[idx + 3 * res]);
                if (tightH || tightV) ++sv.pinchCount;
            }
        }

        const auto contour = traceOuterContour(mask, res, res);
        analyzeContourCurvature(contour, res, sv);
        out.push_back(std::move(sv));
    }

    return out;
}

}  // namespace bro::eye
