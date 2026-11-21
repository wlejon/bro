#include "eye/eye_form.h"
#include "eye/eye_silhouette.h"

#include <bromath/aabb.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <unordered_map>
#include <vector>

namespace bro::eye {
namespace {

constexpr double kPi = 3.14159265358979323846;

struct Vec3d {
    double x = 0.0, y = 0.0, z = 0.0;
};

inline Vec3d operator+(const Vec3d& a, const Vec3d& b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline Vec3d operator-(const Vec3d& a, const Vec3d& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline Vec3d operator*(const Vec3d& a, double s) { return {a.x * s, a.y * s, a.z * s}; }
inline double dot(const Vec3d& a, const Vec3d& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3d cross(const Vec3d& a, const Vec3d& b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
inline double len(const Vec3d& v) { return std::sqrt(dot(v, v)); }

struct PosKey {
    int64_t x, y, z;
    bool operator==(const PosKey& o) const { return x == o.x && y == o.y && z == o.z; }
};

struct PosKeyHash {
    size_t operator()(const PosKey& k) const {
        size_t h = static_cast<size_t>(k.x) * 73856093u;
        h ^= static_cast<size_t>(k.y) * 19349663u;
        h ^= static_cast<size_t>(k.z) * 83492791u;
        return h;
    }
};

struct Pt2 { double x = 0.0, y = 0.0; };

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
        while (hull.size() > lowerSz && cross2(hull[hull.size() - 2], hull.back(), pts[i]) <= 0.0) hull.pop_back();
        hull.push_back(pts[i]);
    }
    if (!hull.empty()) hull.pop_back();
    return hull;
}

// Signed distance from p to convex polygon boundary (+ = inside, - = outside).
double signedDistanceConvexPoly(const Pt2& p, const std::vector<Pt2>& hull) {
    if (hull.size() < 3) return 0.0;
    bool inside = true;
    double minEdgeDist = 1e18;
    for (size_t i = 0; i < hull.size(); ++i) {
        const Pt2& a = hull[i];
        const Pt2& b = hull[(i + 1) % hull.size()];
        const double cr = cross2(a, b, p);
        if (cr < 0.0) inside = false;
        const double dx = b.x - a.x, dy = b.y - a.y;
        const double segLen2 = std::max(1e-12, dx * dx + dy * dy);
        const double t = std::clamp(((p.x - a.x) * dx + (p.y - a.y) * dy) / segLen2, 0.0, 1.0);
        const double projX = a.x + t * dx, projY = a.y + t * dy;
        minEdgeDist = std::min(minEdgeDist, std::hypot(p.x - projX, p.y - projY));
    }
    return inside ? minEdgeDist : -minEdgeDist;
}

}  // namespace

EyeFormResult analyzeMeshForm(const bromesh::MeshData& mesh, const EyeFormOptions& opts) {
    EyeFormResult res;
    if (mesh.empty() || mesh.indices.size() < 3) {
        res.ok = false;
        return res;
    }

    const size_t origVc = mesh.vertexCount();
    const size_t tc = mesh.triangleCount();
    res.surface.vertexCount = static_cast<int>(origVc);
    res.surface.triangleCount = static_cast<int>(tc);

    // 1. Bounds & transient spatial welding so UV seams don't create phantom open edges
    Vec3d bMin{1e18, 1e18, 1e18}, bMax{-1e18, -1e18, -1e18};
    for (size_t i = 0; i < origVc; ++i) {
        const double x = mesh.positions[i * 3 + 0];
        const double y = mesh.positions[i * 3 + 1];
        const double z = mesh.positions[i * 3 + 2];
        bMin.x = std::min(bMin.x, x); bMin.y = std::min(bMin.y, y); bMin.z = std::min(bMin.z, z);
        bMax.x = std::max(bMax.x, x); bMax.y = std::max(bMax.y, y); bMax.z = std::max(bMax.z, z);
    }
    res.mass.boundsMin = {bMin.x, bMin.y, bMin.z};
    res.mass.boundsMax = {bMax.x, bMax.y, bMax.z};

    const double sx = std::max(1e-6, bMax.x - bMin.x);
    const double sy = std::max(1e-6, bMax.y - bMin.y);
    const double sz = std::max(1e-6, bMax.z - bMin.z);
    const double diag = std::sqrt(sx * sx + sy * sy + sz * sz);
    res.mass.heightWidthRatio = sy / sx;
    res.mass.depthWidthRatio = sz / sx;

    const double weldCell = std::max(1e-6, diag * 1e-4);
    std::unordered_map<PosKey, uint32_t, PosKeyHash> posToWeld;
    posToWeld.reserve(origVc);
    std::vector<uint32_t> remap(origVc);
    std::vector<Vec3d> wPos;
    wPos.reserve(origVc);

    for (size_t i = 0; i < origVc; ++i) {
        const Vec3d p{mesh.positions[i * 3 + 0], mesh.positions[i * 3 + 1], mesh.positions[i * 3 + 2]};
        const PosKey k{
            static_cast<int64_t>(std::llround(p.x / weldCell)),
            static_cast<int64_t>(std::llround(p.y / weldCell)),
            static_cast<int64_t>(std::llround(p.z / weldCell)),
        };
        auto [it, inserted] = posToWeld.emplace(k, static_cast<uint32_t>(wPos.size()));
        if (inserted) wPos.push_back(p);
        remap[i] = it->second;
    }
    const size_t wVc = wPos.size();

    // 2. Triangle areas, face normals, divergence volume & CoM, edge adjacency
    std::vector<Vec3d> fNorm(tc, {0, 1, 0});
    std::vector<double> fArea(tc, 0.0);
    std::vector<std::array<uint32_t, 3>> wTris(tc);

    struct EdgeInfo { uint32_t tri; };
    std::unordered_map<uint64_t, std::vector<EdgeInfo>> edgeMap;
    edgeMap.reserve(tc * 2);

    auto edgeKey = [](uint32_t a, uint32_t b) -> uint64_t {
        if (a > b) std::swap(a, b);
        return (static_cast<uint64_t>(a) << 32) | b;
    };

    double totalArea = 0.0, signedVol6 = 0.0;
    Vec3d volCom{0, 0, 0};
    Vec3d areaCentroid{0, 0, 0};
    std::vector<double> vertArea(wVc, 0.0);
    std::vector<Vec3d> vertNormal(wVc, {0, 0, 0});
    std::vector<Vec3d> laplacian(wVc, {0, 0, 0});
    std::vector<double> angleSum(wVc, 0.0);
    std::vector<uint8_t> isBoundaryVert(wVc, 0);

    for (size_t t = 0; t < tc; ++t) {
        const uint32_t i0 = remap[mesh.indices[t * 3 + 0]];
        const uint32_t i1 = remap[mesh.indices[t * 3 + 1]];
        const uint32_t i2 = remap[mesh.indices[t * 3 + 2]];
        wTris[t] = {i0, i1, i2};
        if (i0 == i1 || i1 == i2 || i2 == i0) continue;

        const Vec3d& p0 = wPos[i0];
        const Vec3d& p1 = wPos[i1];
        const Vec3d& p2 = wPos[i2];
        const Vec3d e10 = p1 - p0, e20 = p2 - p0, e21 = p2 - p1;
        const Vec3d cr = cross(e10, e20);
        const double dblA = len(cr);
        if (dblA < 1e-12) continue;

        const double area = 0.5 * dblA;
        const Vec3d fn = cr * (1.0 / dblA);
        fArea[t] = area;
        fNorm[t] = fn;
        totalArea += area;

        const Vec3d triCenter = (p0 + p1 + p2) * (1.0 / 3.0);
        areaCentroid = areaCentroid + triCenter * area;

        const double tetVol6 = dot(p0, cross(p1, p2));
        signedVol6 += tetVol6;
        volCom = volCom + (p0 + p1 + p2) * (0.25 * tetVol6);

        vertArea[i0] += area / 3.0;
        vertArea[i1] += area / 3.0;
        vertArea[i2] += area / 3.0;
        vertNormal[i0] = vertNormal[i0] + fn * area;
        vertNormal[i1] = vertNormal[i1] + fn * area;
        vertNormal[i2] = vertNormal[i2] + fn * area;

        // Vertex angles & cotangent weights
        auto angleAndCot = [&](const Vec3d& a, const Vec3d& b, const Vec3d& c, double& outAng, double& outCot) {
            const Vec3d u = b - a, v = c - a;
            const double lu = std::max(1e-12, len(u)), lv = std::max(1e-12, len(v));
            const double cosA = std::clamp(dot(u, v) / (lu * lv), -0.9995, 0.9995);
            outAng = std::acos(cosA);
            const double sinA = std::max(0.05, std::sqrt(std::max(0.0, 1.0 - cosA * cosA)));
            outCot = std::clamp(cosA / sinA, -20.0, 20.0);
        };
        double a0, c0, a1, c1, a2, c2;
        angleAndCot(p0, p1, p2, a0, c0);
        angleAndCot(p1, p2, p0, a1, c1);
        angleAndCot(p2, p0, p1, a2, c2);
        angleSum[i0] += a0;
        angleSum[i1] += a1;
        angleSum[i2] += a2;

        // Cotangent Laplacian contributions
        laplacian[i1] = laplacian[i1] + (p2 - p1) * c0;
        laplacian[i2] = laplacian[i2] + (p1 - p2) * c0;
        laplacian[i2] = laplacian[i2] + (p0 - p2) * c1;
        laplacian[i0] = laplacian[i0] + (p2 - p0) * c1;
        laplacian[i0] = laplacian[i0] + (p1 - p0) * c2;
        laplacian[i1] = laplacian[i1] + (p0 - p1) * c2;

        edgeMap[edgeKey(i0, i1)].push_back({static_cast<uint32_t>(t)});
        edgeMap[edgeKey(i1, i2)].push_back({static_cast<uint32_t>(t)});
        edgeMap[edgeKey(i2, i0)].push_back({static_cast<uint32_t>(t)});
    }

    res.surface.surfaceArea = totalArea;
    const double vol = std::abs(signedVol6 / 6.0);
    res.mass.volume = vol;
    if (vol > 1e-12 && std::abs(signedVol6) > 1e-12) {
        const Vec3d com = volCom * (1.0 / signedVol6);
        res.mass.centerOfMass = {com.x, com.y, com.z};
    } else if (totalArea > 1e-12) {
        const Vec3d com = areaCentroid * (1.0 / totalArea);
        res.mass.centerOfMass = {com.x, com.y, com.z};
    }

    // 3. Edge dihedral creases & non-manifold detection
    const double cosThresh = std::cos(std::clamp(opts.creaseAngleDeg, 5.0, 175.0) * (kPi / 180.0));
    int creaseEdges = 0, interiorEdges = 0, nonManifold = 0;
    struct CreaseCand { double score; Vec3d mid; double deg; };
    std::vector<CreaseCand> creaseCands;

    for (const auto& kv : edgeMap) {
        const uint32_t a = static_cast<uint32_t>(kv.first >> 32);
        const uint32_t b = static_cast<uint32_t>(kv.first & 0xffffffffu);
        const auto& tris = kv.second;
        if (tris.size() == 1) {
            isBoundaryVert[a] = 1;
            isBoundaryVert[b] = 1;
        } else if (tris.size() == 2) {
            ++interiorEdges;
            const uint32_t t0 = tris[0].tri, t1 = tris[1].tri;
            if (fArea[t0] > 1e-12 && fArea[t1] > 1e-12) {
                const double c = std::clamp(dot(fNorm[t0], fNorm[t1]), -1.0, 1.0);
                if (c < cosThresh) {
                    ++creaseEdges;
                    const double deg = std::acos(c) * (180.0 / kPi);
                    creaseCands.push_back({deg, (wPos[a] + wPos[b]) * 0.5, deg});
                }
            }
        } else if (tris.size() > 2) {
            ++nonManifold;
            isBoundaryVert[a] = 1;
            isBoundaryVert[b] = 1;
        }
    }
    res.surface.nonManifoldEdges = nonManifold;
    res.surface.creaseEdgeCount = creaseEdges;
    res.surface.creaseRatio = (interiorEdges > 0) ? static_cast<double>(creaseEdges) / interiorEdges : 0.0;

    // Build 1-ring neighbor list for roughness evaluation
    std::vector<std::vector<uint32_t>> neighbors(wVc);
    for (const auto& kv : edgeMap) {
        const uint32_t a = static_cast<uint32_t>(kv.first >> 32);
        const uint32_t b = static_cast<uint32_t>(kv.first & 0xffffffffu);
        neighbors[a].push_back(b);
        neighbors[b].push_back(a);
    }

    // 4. Per-vertex Mean Curvature H & Gaussian Curvature K (scale-normalized by diag)
    std::vector<double> H(wVc, 0.0), K(wVc, 0.0);
    double sumAbsH = 0.0, sumAbsK = 0.0;
    double ellipticArea = 0.0, hyperbolicArea = 0.0, parabolicArea = 0.0, flatArea = 0.0;

    for (size_t i = 0; i < wVc; ++i) {
        const double area = vertArea[i];
        if (area < 1e-14) continue;
        const double vnLen = len(vertNormal[i]);
        const Vec3d vn = (vnLen > 1e-12) ? vertNormal[i] * (1.0 / vnLen) : Vec3d{0, 1, 0};

        // Scale-invariant discrete curvatures: H * diag, K * diag^2 (skip boundary half-star bias)
        const Vec3d hn = laplacian[i] * (0.5 / area);
        const double signedH = isBoundaryVert[i] ? 0.0 : ((dot(hn, vn) >= 0.0 ? 1.0 : -1.0) * len(hn) * diag);
        H[i] = signedH;
        const double gK = isBoundaryVert[i] ? 0.0 : ((2.0 * kPi - angleSum[i]) / area) * (diag * diag);
        K[i] = gK;

        sumAbsH += std::abs(signedH) * area;
        sumAbsK += std::abs(gK) * area;

        if (std::abs(signedH) < 0.25 && std::abs(gK) < 1.5) {
            flatArea += area;
        } else if (gK > 2.0) {
            ellipticArea += area;
        } else if (gK < -2.0) {
            hyperbolicArea += area;
        } else {
            parabolicArea += area;
        }
    }

    if (totalArea > 1e-12) {
        res.surface.meanCurvatureAbs = sumAbsH / totalArea;
        res.surface.gaussianCurvatureAbs = sumAbsK / totalArea;
        res.surface.ellipticRatio = ellipticArea / totalArea;
        res.surface.hyperbolicRatio = hyperbolicArea / totalArea;
        res.surface.parabolicRatio = parabolicArea / totalArea;
        res.surface.flatRatio = flatArea / totalArea;
    }

    // High-frequency curvature roughness & curvature pinch outliers
    std::vector<double> absHVals;
    absHVals.reserve(wVc);
    double roughSq = 0.0, roughWeight = 0.0;
    for (size_t i = 0; i < wVc; ++i) {
        if (vertArea[i] < 1e-14 || neighbors[i].empty()) continue;
        absHVals.push_back(std::abs(H[i]));
        double meanNb = 0.0;
        for (uint32_t nb : neighbors[i]) meanNb += H[nb];
        meanNb /= neighbors[i].size();
        const double diff = H[i] - meanNb;
        roughSq += diff * diff * vertArea[i];
        roughWeight += vertArea[i];
    }
    res.surface.highFreqRoughness = (roughWeight > 1e-12) ? std::sqrt(roughSq / roughWeight) : 0.0;

    int pinchCnt = 0;
    struct PinchCand { double val; Vec3d pos; };
    std::vector<PinchCand> pinches;
    if (absHVals.size() >= 16) {
        std::sort(absHVals.begin(), absHVals.end());
        const double medH = absHVals[absHVals.size() / 2];
        const double p95H = absHVals[std::min(absHVals.size() - 1, (absHVals.size() * 95) / 100)];
        const double pinchThresh = std::max(12.0, std::max(medH * 6.0, p95H * 2.4));
        for (size_t i = 0; i < wVc; ++i) {
            if (!isBoundaryVert[i] && std::abs(H[i]) > pinchThresh) {
                ++pinchCnt;
                pinches.push_back({std::abs(H[i]), wPos[i]});
            }
        }
    }
    res.surface.pinchVertexCount = pinchCnt;
    res.surface.smoothnessScore = std::clamp(
        1.0 - 2.2 * res.surface.creaseRatio - 0.04 * res.surface.highFreqRoughness -
        0.5 * std::min(1.0, static_cast<double>(pinchCnt) / std::max<size_t>(1, wVc / 20)),
        0.0, 1.0);

    // Emit ranked top defects
    std::sort(creaseCands.begin(), creaseCands.end(), [](const auto& a, const auto& b) { return a.score > b.score; });
    for (size_t i = 0; i < creaseCands.size() && res.defects.size() < 8; ++i) {
        char msg[128];
        std::snprintf(msg, sizeof(msg), "G1 dihedral crease %.1f deg near (%.3f, %.3f, %.3f)",
                      creaseCands[i].deg, creaseCands[i].mid.x, creaseCands[i].mid.y, creaseCands[i].mid.z);
        res.defects.push_back({"g1-crease", creaseCands[i].mid.x, creaseCands[i].mid.y, creaseCands[i].mid.z, creaseCands[i].deg, msg});
    }
    std::sort(pinches.begin(), pinches.end(), [](const auto& a, const auto& b) { return a.val > b.val; });
    for (size_t i = 0; i < pinches.size() && res.defects.size() < 16; ++i) {
        char msg[128];
        std::snprintf(msg, sizeof(msg), "Curvature pinch (|H| %.1f) at (%.3f, %.3f, %.3f)",
                      pinches[i].val, pinches[i].pos.x, pinches[i].pos.y, pinches[i].pos.z);
        res.defects.push_back({"curvature-pinch", pinches[i].pos.x, pinches[i].pos.y, pinches[i].pos.z, pinches[i].val, msg});
    }
    if (nonManifold > 0) {
        char msg[96];
        std::snprintf(msg, sizeof(msg), "Mesh contains %d non-manifold edge(s)", nonManifold);
        res.defects.push_back({"non-manifold", 0, 0, 0, static_cast<double>(nonManifold), msg});
    }

    // 5. Base support polygon & plumb-line gravity balance
    const double baseBandY = bMin.y + sy * 0.045;
    std::vector<Pt2> basePts;
    for (const auto& p : wPos) {
        if (p.y <= baseBandY) basePts.push_back({p.x, p.z});
    }
    const double midX = 0.5 * (bMin.x + bMax.x), midZ = 0.5 * (bMin.z + bMax.z);
    res.mass.plumbOffsetX = (res.mass.centerOfMass[0] - midX) / sx;
    res.mass.plumbOffsetZ = (res.mass.centerOfMass[2] - midZ) / sz;
    if (basePts.size() >= 3) {
        const auto hull = convexHull2D(std::move(basePts));
        const double sd = signedDistanceConvexPoly({res.mass.centerOfMass[0], res.mass.centerOfMass[2]}, hull);
        res.mass.stabilityMargin = sd / std::max(1e-6, std::max(sx, sz));
        res.mass.grounded = (sd >= -1e-4);
    }

    // 6. Bilateral X-symmetry estimate via coarse 3D occupancy IoU mirrored across X center
    {
        constexpr int G = 24;
        std::vector<uint8_t> occ(G * G * G, 0);
        for (const auto& p : wPos) {
            const int gx = std::clamp(static_cast<int>(std::llround(((p.x - bMin.x) / sx) * (G - 1))), 0, G - 1);
            const int gy = std::clamp(static_cast<int>(std::llround(((p.y - bMin.y) / sy) * (G - 1))), 0, G - 1);
            const int gz = std::clamp(static_cast<int>(std::llround(((p.z - bMin.z) / sz) * (G - 1))), 0, G - 1);
            occ[(gy * G + gz) * G + gx] = 1;
        }
        int inter = 0, un = 0;
        for (int gy = 0; gy < G; ++gy) {
            for (int gz = 0; gz < G; ++gz) {
                for (int gx = 0; gx < G; ++gx) {
                    const uint8_t a = occ[(gy * G + gz) * G + gx];
                    const int mx = G - 1 - gx;
                    const uint8_t b = occ[(gy * G + gz) * G + mx] ||
                                      (mx > 0 && occ[(gy * G + gz) * G + (mx - 1)]) ||
                                      (mx + 1 < G && occ[(gy * G + gz) * G + (mx + 1)]);
                    if (a) {
                        ++un;
                        if (b) ++inter;
                    }
                }
            }
        }
        res.mass.symmetryScore = (un > 0) ? static_cast<double>(inter) / un : 1.0;
    }

    // 7. Horizontal cross-sectional slice profiles
    const int nSlices = std::clamp(opts.sliceCount, 2, 24);
    res.slices.resize(static_cast<size_t>(nSlices));
    for (int s = 0; s < nSlices; ++s) {
        const double y0 = bMin.y + (sy * s) / nSlices;
        const double y1 = bMin.y + (sy * (s + 1)) / nSlices;
        double minX = 1e18, maxX = -1e18, minZ = 1e18, maxZ = -1e18;
        int cnt = 0;
        for (const auto& p : wPos) {
            if (p.y >= y0 && p.y <= y1) {
                minX = std::min(minX, p.x); maxX = std::max(maxX, p.x);
                minZ = std::min(minZ, p.z); maxZ = std::max(maxZ, p.z);
                ++cnt;
            }
        }
        auto& sp = res.slices[s];
        sp.yFrac = (s + 0.5) / nSlices;
        if (cnt > 0) {
            sp.width = std::max(0.0, maxX - minX);
            sp.depth = std::max(0.0, maxZ - minZ);
            sp.area = sp.width * sp.depth;
            sp.centerX = 0.5 * (minX + maxX) - midX;
            sp.centerZ = 0.5 * (minZ + maxZ) - midZ;
        }
    }

    // 8. Multi-angle orthographic silhouettes + negative space + kappa(s)
    const Vec3d bCenter{(bMin.x + bMax.x) * 0.5, (bMin.y + bMax.y) * 0.5, (bMin.z + bMax.z) * 0.5};
    res.silhouettes = analyzeMeshSilhouettes(mesh, {bCenter.x, bCenter.y, bCenter.z}, 0.5 * diag, opts);

    double meanSilSmooth = 1.0;
    int totalWiggles = 0;
    if (!res.silhouettes.empty()) {
        double sumSm = 0.0;
        for (const auto& sv : res.silhouettes) {
            sumSm += sv.smoothness;
            totalWiggles += sv.wiggleCount;
        }
        meanSilSmooth = sumSm / res.silhouettes.size();
    }

    double sc = 100.0;
    sc -= 35.0 * (1.0 - res.surface.smoothnessScore);
    sc -= 25.0 * (1.0 - meanSilSmooth);
    sc -= std::min(20.0, 4.0 * nonManifold);
    if (!res.mass.grounded) sc -= 12.0;
    sc -= std::min(15.0, totalWiggles * 0.8);
    res.score = std::clamp(sc, 0.0, 100.0);
    return res;
}

}  // namespace bro::eye
