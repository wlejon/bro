#include "eye/eye_structure_islands.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <queue>
#include <unordered_map>

namespace bro::eye {
namespace {

inline bromath::Vec3 mulPt(const bromath::Mat4& m, const bromath::Vec3& v) {
    const float* d = m.data;
    return {
        d[0] * v.x + d[4] * v.y + d[8]  * v.z + d[12],
        d[1] * v.x + d[5] * v.y + d[9]  * v.z + d[13],
        d[2] * v.x + d[6] * v.y + d[10] * v.z + d[14],
    };
}

inline bromath::Vec3 mulDir(const bromath::Mat4& m, const bromath::Vec3& v) {
    const float* d = m.data;
    return {
        d[0] * v.x + d[4] * v.y + d[8]  * v.z,
        d[1] * v.x + d[5] * v.y + d[9]  * v.z,
        d[2] * v.x + d[6] * v.y + d[10] * v.z,
    };
}

struct QuantKey {
    int64_t x, y, z;
    bool operator==(const QuantKey& o) const { return x == o.x && y == o.y && z == o.z; }
};

struct QuantKeyHash {
    size_t operator()(const QuantKey& k) const {
        uint64_t h = static_cast<uint64_t>(k.x) * 73856093ULL ^
                     static_cast<uint64_t>(k.y) * 19349663ULL ^
                     static_cast<uint64_t>(k.z) * 83492791ULL;
        return static_cast<size_t>(h ^ (h >> 30));
    }
};

struct DSU {
    std::vector<uint32_t> p;
    explicit DSU(size_t n) : p(n) {
        for (size_t i = 0; i < n; ++i) p[i] = static_cast<uint32_t>(i);
    }
    uint32_t find(uint32_t x) {
        while (p[x] != x) {
            p[x] = p[p[x]];
            x = p[x];
        }
        return x;
    }
    void unite(uint32_t a, uint32_t b) {
        a = find(a); b = find(b);
        if (a != b) p[b] = a;
    }
};

struct HalfEdgeAcc {
    int forward = 0;
    int reverse = 0;
    int total() const { return forward + reverse; }
};

double aabbGap(const bromath::AABB3& a, const bromath::AABB3& b) {
    const double dx = std::max(0.0f, std::max(a.min.x - b.max.x, b.min.x - a.max.x));
    const double dy = std::max(0.0f, std::max(a.min.y - b.max.y, b.min.y - a.max.y));
    const double dz = std::max(0.0f, std::max(a.min.z - b.max.z, b.min.z - a.max.z));
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

bromath::Vec3 closestPtOnTri(const bromath::Vec3& p, const bromath::Vec3& a,
                             const bromath::Vec3& b, const bromath::Vec3& c) {
    const bromath::Vec3 ab = b - a, ac = c - a, ap = p - a;
    const float d1 = bromath::vdot(ab, ap), d2 = bromath::vdot(ac, ap);
    if (d1 <= 0.0f && d2 <= 0.0f) return a;
    const bromath::Vec3 bp = p - b;
    const float d3 = bromath::vdot(ab, bp), d4 = bromath::vdot(ac, bp);
    if (d3 >= 0.0f && d4 <= d3) return b;
    const float vc = d1 * d4 - d3 * d2;
    if (vc <= 0.0f && d1 >= 0.0f && d3 <= 0.0f) {
        const float v = d1 / (d1 - d3);
        return a + ab * v;
    }
    const bromath::Vec3 cp = p - c;
    const float d5 = bromath::vdot(ab, cp), d6 = bromath::vdot(ac, cp);
    if (d6 >= 0.0f && d5 <= d6) return c;
    const float vb = d5 * d2 - d1 * d6;
    if (vb <= 0.0f && d2 >= 0.0f && d6 <= 0.0f) {
        const float w = d2 / (d2 - d6);
        return a + ac * w;
    }
    const float va = d3 * d6 - d5 * d4;
    if (va <= 0.0f && (d4 - d3) >= 0.0f && (d5 - d6) >= 0.0f) {
        const float w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
        return b + (c - b) * w;
    }
    const float denom = 1.0f / (va + vb + vc);
    return a + ab * (vb * denom) + ac * (vc * denom);
}

double pointToIslandDist(const bromath::Vec3& pW, const IslandSummary& isl,
                         const bromesh::MeshData& md, const bromath::Mat4& world, const bromath::Mat4& invWorld) {
    const bromath::Vec3 pL = mulPt(invWorld, pW);
    double best = std::numeric_limits<double>::infinity();
    const size_t step = std::max<size_t>(1, isl.triIndices.size() / 160);
    for (size_t k = 0; k < isl.triIndices.size(); k += step) {
        const uint32_t t = isl.triIndices[k];
        const uint32_t i0 = md.indices[t * 3 + 0];
        const uint32_t i1 = md.indices[t * 3 + 1];
        const uint32_t i2 = md.indices[t * 3 + 2];
        const bromath::Vec3 a{md.positions[i0 * 3], md.positions[i0 * 3 + 1], md.positions[i0 * 3 + 2]};
        const bromath::Vec3 b{md.positions[i1 * 3], md.positions[i1 * 3 + 1], md.positions[i1 * 3 + 2]};
        const bromath::Vec3 c{md.positions[i2 * 3], md.positions[i2 * 3 + 1], md.positions[i2 * 3 + 2]};
        const bromath::Vec3 qL = closestPtOnTri(pL, a, b, c);
        const bromath::Vec3 qW = mulPt(world, qL);
        const double d = bromath::vlen(qW - pW);
        if (d < best) best = d;
    }
    return best;
}

bool segmentHitsTri(const bromath::Vec3& p0, const bromath::Vec3& p1,
                    const bromath::Vec3& a, const bromath::Vec3& b, const bromath::Vec3& c) {
    const bromath::Vec3 dir = p1 - p0;
    const bromath::Vec3 e1 = b - a, e2 = c - a;
    const bromath::Vec3 h = bromath::vcross(dir, e2);
    const float det = bromath::vdot(e1, h);
    if (std::fabs(det) < 1e-8f) return false;
    const float invDet = 1.0f / det;
    const bromath::Vec3 s = p0 - a;
    const float u = invDet * bromath::vdot(s, h);
    if (u < -1e-4f || u > 1.0f + 1e-4f) return false;
    const bromath::Vec3 q = bromath::vcross(s, e1);
    const float v = invDet * bromath::vdot(dir, q);
    if (v < -1e-4f || u + v > 1.0f + 1e-4f) return false;
    const float t = invDet * bromath::vdot(e2, q);
    return t >= -1e-4f && t <= 1.0f + 1e-4f;
}

bool islandsIntersectTris(const std::vector<uint32_t>& trisA,
                          const std::vector<uint32_t>& trisB,
                          const bromesh::MeshData& md) {
    const size_t stepA = std::max<size_t>(1, trisA.size() / 96);
    const size_t stepB = std::max<size_t>(1, trisB.size() / 160);
    for (size_t ka = 0; ka < trisA.size(); ka += stepA) {
        const uint32_t ta = trisA[ka];
        const bromath::Vec3 pa[3] = {
            {md.positions[md.indices[ta * 3 + 0] * 3], md.positions[md.indices[ta * 3 + 0] * 3 + 1], md.positions[md.indices[ta * 3 + 0] * 3 + 2]},
            {md.positions[md.indices[ta * 3 + 1] * 3], md.positions[md.indices[ta * 3 + 1] * 3 + 1], md.positions[md.indices[ta * 3 + 1] * 3 + 2]},
            {md.positions[md.indices[ta * 3 + 2] * 3], md.positions[md.indices[ta * 3 + 2] * 3 + 1], md.positions[md.indices[ta * 3 + 2] * 3 + 2]},
        };
        for (size_t kb = 0; kb < trisB.size(); kb += stepB) {
            const uint32_t tb = trisB[kb];
            const uint32_t j0 = md.indices[tb * 3 + 0], j1 = md.indices[tb * 3 + 1], j2 = md.indices[tb * 3 + 2];
            const bromath::Vec3 b0{md.positions[j0 * 3], md.positions[j0 * 3 + 1], md.positions[j0 * 3 + 2]};
            const bromath::Vec3 b1{md.positions[j1 * 3], md.positions[j1 * 3 + 1], md.positions[j1 * 3 + 2]};
            const bromath::Vec3 b2{md.positions[j2 * 3], md.positions[j2 * 3 + 1], md.positions[j2 * 3 + 2]};
            if (segmentHitsTri(pa[0], pa[1], b0, b1, b2) ||
                segmentHitsTri(pa[1], pa[2], b0, b1, b2) ||
                segmentHitsTri(pa[2], pa[0], b0, b1, b2)) {
                return true;
            }
        }
    }
    return false;
}

}  // namespace

std::vector<LocalIsland> extractLocalIslands(const bromesh::MeshData& md,
                                             size_t maxTris,
                                             size_t maxIslands) {
    const size_t vc = md.vertexCount();
    const size_t tc = md.triangleCount();
    if (vc == 0 || tc == 0 || tc > maxTris) return {};

    std::vector<uint32_t> weldedId(vc);
    std::unordered_map<QuantKey, uint32_t, QuantKeyHash> quantMap;
    quantMap.reserve(vc);
    uint32_t nextWeld = 0;
    for (size_t i = 0; i < vc; ++i) {
        const QuantKey key{
            static_cast<int64_t>(std::llround(md.positions[i * 3 + 0] * 10000.0)),
            static_cast<int64_t>(std::llround(md.positions[i * 3 + 1] * 10000.0)),
            static_cast<int64_t>(std::llround(md.positions[i * 3 + 2] * 10000.0)),
        };
        auto [it, ins] = quantMap.emplace(key, nextWeld);
        if (ins) ++nextWeld;
        weldedId[i] = it->second;
    }

    DSU dsu(nextWeld);
    for (size_t t = 0; t < tc; ++t) {
        const uint32_t i0 = md.indices[t * 3 + 0];
        const uint32_t i1 = md.indices[t * 3 + 1];
        const uint32_t i2 = md.indices[t * 3 + 2];
        if (i0 >= vc || i1 >= vc || i2 >= vc) continue;
        dsu.unite(weldedId[i0], weldedId[i1]);
        dsu.unite(weldedId[i1], weldedId[i2]);
    }

    std::unordered_map<uint32_t, size_t> rootToIsl;
    std::vector<LocalIsland> islands;
    for (size_t t = 0; t < tc; ++t) {
        const uint32_t i0 = md.indices[t * 3 + 0];
        const uint32_t i1 = md.indices[t * 3 + 1];
        const uint32_t i2 = md.indices[t * 3 + 2];
        if (i0 >= vc || i1 >= vc || i2 >= vc) continue;
        const uint32_t root = dsu.find(weldedId[i0]);
        auto [it, ins] = rootToIsl.emplace(root, islands.size());
        if (ins) {
            if (islands.size() >= maxIslands) return {};
            islands.emplace_back();
        }
        islands[it->second].triIndices.push_back(static_cast<uint32_t>(t));
    }

    const bool hasNormals = md.hasNormals();
    std::unordered_map<uint64_t, HalfEdgeAcc> edges;
    for (auto& isl : islands) {
        edges.clear();
        edges.reserve(isl.triIndices.size() * 2);
        double vol6 = 0.0;
        size_t checkedNrm = 0, badPos = 0, badNeg = 0;
        const uint32_t refIdx = isl.triIndices.empty() ? 0u : md.indices[isl.triIndices[0] * 3];
        const bromath::Vec3 pRef = isl.triIndices.empty()
            ? bromath::Vec3{0, 0, 0}
            : bromath::Vec3{md.positions[refIdx * 3], md.positions[refIdx * 3 + 1], md.positions[refIdx * 3 + 2]};

        for (uint32_t t : isl.triIndices) {
            const uint32_t i0 = md.indices[t * 3 + 0];
            const uint32_t i1 = md.indices[t * 3 + 1];
            const uint32_t i2 = md.indices[t * 3 + 2];
            const bromath::Vec3 p0{md.positions[i0 * 3], md.positions[i0 * 3 + 1], md.positions[i0 * 3 + 2]};
            const bromath::Vec3 p1{md.positions[i1 * 3], md.positions[i1 * 3 + 1], md.positions[i1 * 3 + 2]};
            const bromath::Vec3 p2{md.positions[i2 * 3], md.positions[i2 * 3 + 1], md.positions[i2 * 3 + 2]};
            isl.localBox = bromath::aexpand(isl.localBox, p0);
            isl.localBox = bromath::aexpand(isl.localBox, p1);
            isl.localBox = bromath::aexpand(isl.localBox, p2);

            const bromath::Vec3 fn = bromath::vcross(p1 - p0, p2 - p0);
            vol6 += static_cast<double>(bromath::vdot(p0 - pRef, fn));

            const uint32_t w[3] = {weldedId[i0], weldedId[i1], weldedId[i2]};
            if (w[0] != w[1] && w[1] != w[2] && w[2] != w[0]) {
                for (int e = 0; e < 3; ++e) {
                    const uint32_t u = w[e], v = w[(e + 1) % 3];
                    const uint64_t key = (static_cast<uint64_t>(std::min(u, v)) << 32) | std::max(u, v);
                    if (u < v) edges[key].forward += 1;
                    else edges[key].reverse += 1;
                }
            }
            if (hasNormals && bromath::vlen(fn) > 1e-8f) {
                const bromath::Vec3 n0{md.normals[i0 * 3], md.normals[i0 * 3 + 1], md.normals[i0 * 3 + 2]};
                const bromath::Vec3 n1{md.normals[i1 * 3], md.normals[i1 * 3 + 1], md.normals[i1 * 3 + 2]};
                const bromath::Vec3 n2{md.normals[i2 * 3], md.normals[i2 * 3 + 1], md.normals[i2 * 3 + 2]};
                const bromath::Vec3 vn = n0 + n1 + n2;
                if (bromath::vlen(vn) > 1e-6f) {
                    ++checkedNrm;
                    const float d = bromath::vdot(fn, vn);
                    if (d < -1e-6f) ++badPos;
                    if (-d < -1e-6f) ++badNeg;
                }
            }
        }

        isl.rawSignedVolume = vol6 / 6.0;
        if (checkedNrm > 0) {
            isl.badNormalRatioPos = static_cast<double>(badPos) / checkedNrm;
            isl.badNormalRatioNeg = static_cast<double>(badNeg) / checkedNrm;
        }

        size_t boundary = 0, shared = 0, reversed = 0;
        for (const auto& [k, acc] : edges) {
            (void)k;
            if (acc.total() < 2) ++boundary;
            else if (acc.total() == 2) {
                ++shared;
                if (acc.forward != 1 || acc.reverse != 1) ++reversed;
            }
        }
        isl.sharedEdges = static_cast<int>(shared);
        if (!edges.empty()) isl.closed = (boundary == 0);
        if (shared > 0) isl.reversedEdgeRatio = static_cast<double>(reversed) / shared;

        const size_t step = std::max<size_t>(1, isl.triIndices.size() / 24);
        for (size_t k = 0; k < isl.triIndices.size(); k += step) {
            const uint32_t t = isl.triIndices[k];
            for (int c = 0; c < 3; ++c) {
                const uint32_t vi = md.indices[t * 3 + c];
                isl.localSamples.push_back({md.positions[vi * 3], md.positions[vi * 3 + 1], md.positions[vi * 3 + 2]});
            }
        }
    }

    if (islands.size() >= 2 && islands.size() <= 128) {
        for (size_t i = 0; i < islands.size(); ++i) {
            for (size_t j = i + 1; j < islands.size(); ++j) {
                if (aabbGap(islands[i].localBox, islands[j].localBox) > 1e-3) continue;
                if (islandsIntersectTris(islands[i].triIndices, islands[j].triIndices, md) ||
                    islandsIntersectTris(islands[j].triIndices, islands[i].triIndices, md)) {
                    islands[i].intersectingNeighbors.push_back(static_cast<uint32_t>(j));
                    islands[j].intersectingNeighbors.push_back(static_cast<uint32_t>(i));
                }
            }
        }
    }
    return islands;
}

std::vector<IslandSummary> instantiateIslands(const std::vector<LocalIsland>& localIslands,
                                              const bromesh::MeshData& md,
                                              const bromath::Mat4& world,
                                              float detSign,
                                              double groundY) {
    (void)md;
    std::vector<IslandSummary> out;
    out.reserve(localIslands.size());
    const bool keepTris = (localIslands.size() >= 2);
    for (const auto& li : localIslands) {
        IslandSummary isl;
        isl.worldBox = bromath::aempty3();
        const bromath::Vec3& lo = li.localBox.min;
        const bromath::Vec3& hi = li.localBox.max;
        for (int mask = 0; mask < 8; ++mask) {
            const bromath::Vec3 corner{
                (mask & 1) ? hi.x : lo.x,
                (mask & 2) ? hi.y : lo.y,
                (mask & 4) ? hi.z : lo.z,
            };
            isl.worldBox = bromath::aexpand(isl.worldBox, mulPt(world, corner));
        }
        if (keepTris) isl.triIndices = li.triIndices;
        isl.localIntersectingNeighbors = li.intersectingNeighbors;
        isl.closed = li.closed;
        isl.signedVolume = li.rawSignedVolume * detSign;
        isl.badNormalRatio = detSign >= 0.0f ? li.badNormalRatioPos : li.badNormalRatioNeg;
        isl.reversedEdgeRatio = li.reversedEdgeRatio;
        isl.sharedEdges = li.sharedEdges;
        isl.grounded = (isl.worldBox.min.y <= groundY + 0.05);
        isl.samples.reserve(li.localSamples.size() + 2);
        for (const auto& lp : li.localSamples) {
            isl.samples.push_back(mulPt(world, lp));
        }
        const bromath::Vec3 ctr = bromath::acenter(isl.worldBox);
        isl.samples.push_back(ctr);
        isl.samples.push_back({ctr.x, isl.worldBox.min.y, ctr.z});
        out.push_back(std::move(isl));
    }
    return out;
}

std::vector<IslandSummary> extractMeshIslands(const bromesh::MeshData& md,
                                              const bromath::Mat4& world,
                                              float detSign,
                                              double groundY,
                                              size_t maxTris,
                                              size_t maxIslands) {
    const auto locals = extractLocalIslands(md, maxTris, maxIslands);
    if (locals.empty()) return {};
    return instantiateIslands(locals, md, world, detSign, groundY);
}

void auditIslandGrounding(const std::vector<IslandSummary>& islands,
                          size_t selfIdx,
                          const std::vector<MeshGeomRef>& allGeoms,
                          double contactTol,
                          int& outFloatingIslands,
                          double& outWorstGap,
                          bromath::Vec3& outWorstCenter,
                          double& outWorstRadius) {
    outFloatingIslands = 0;
    outWorstGap = 0.0;
    const size_t m = islands.size();
    if (m < 2 || m > 128 || selfIdx >= allGeoms.size() || !allGeoms[selfIdx].mesh) return;

    const auto& selfMd = *allGeoms[selfIdx].mesh;
    const auto& selfWorld = allGeoms[selfIdx].world;
    const auto& selfInv = allGeoms[selfIdx].invWorld;

    std::vector<bool> significant(m, false);
    int sigCount = 0;
    for (size_t i = 0; i < m; ++i) {
        const bromath::Vec3 ext = bromath::aextent(islands[i].worldBox);
        if (std::max({ext.x, ext.y, ext.z}) >= 0.04f) {
            significant[i] = true;
            ++sigCount;
        }
    }
    if (sigCount < 2) return;

    std::vector<std::vector<size_t>> adj(m);
    for (size_t i = 0; i < m; ++i) {
        if (!significant[i]) continue;
        for (uint32_t nb : islands[i].localIntersectingNeighbors) {
            if (nb > i && nb < m && significant[nb]) {
                adj[i].push_back(nb);
                adj[nb].push_back(i);
            }
        }
    }

    std::vector<bool> reached(m, false);
    std::queue<size_t> q;
    int reachedSig = 0;
    for (size_t i = 0; i < m; ++i) {
        if (significant[i] && islands[i].grounded) {
            reached[i] = true;
            ++reachedSig;
            q.push(i);
        }
    }
    while (!q.empty()) {
        const size_t u = q.front();
        q.pop();
        for (size_t v : adj[u]) {
            if (!reached[v]) {
                reached[v] = true;
                ++reachedSig;
                q.push(v);
            }
        }
    }
    if (reachedSig == sigCount) return;

    std::vector<size_t> nearbyGeoms;
    for (size_t b = 0; b < allGeoms.size(); ++b) {
        if (b == selfIdx || !allGeoms[b].mesh || !allGeoms[b].bvh || allGeoms[b].bvh->empty()) continue;
        if (aabbGap(allGeoms[selfIdx].worldBox, allGeoms[b].worldBox) <= contactTol) {
            nearbyGeoms.push_back(b);
        }
    }

    for (size_t i = 0; i < m; ++i) {
        if (!significant[i] || reached[i]) continue;
        const bromath::Vec3 ext = bromath::aextent(islands[i].worldBox);
        const bromath::Vec3 ctr = bromath::acenter(islands[i].worldBox);
        const bool checkTopOnly = (ext.y < 0.85f);
        const float topBand = std::min(0.032f, std::max(0.014f, 0.25f * ext.y));
        bool extSup = false;

        for (size_t b : nearbyGeoms) {
            if (aabbGap(islands[i].worldBox, allGeoms[b].worldBox) > contactTol) continue;
            const auto& bvhB = *allGeoms[b].bvh;
            const auto& mdB = *allGeoms[b].mesh;

            auto passesTopOnly = [&](const bromath::Vec3& pb) -> bool {
                if (!checkTopOnly || pb.y < islands[i].worldBox.max.y - topBand ||
                    pb.y <= islands[i].worldBox.min.y + 0.50f * ext.y) {
                    return true;
                }
                const bromath::Vec3 probes[2] = {
                    {pb.x, islands[i].worldBox.min.y + 0.02f, pb.z},
                    {ctr.x, islands[i].worldBox.min.y + 0.02f, ctr.z},
                };
                const bromath::Vec3 ldRaw = mulDir(allGeoms[b].invWorld, {0, -1, 0});
                const float ldLen = bromath::vlen(ldRaw);
                if (ldLen < 1e-8f) return false;
                const bromath::Vec3 ld = ldRaw * (1.0f / ldLen);
                const float dir[3] = {ld.x, ld.y, ld.z};
                for (const auto& pr : probes) {
                    const bromath::Vec3 lo = mulPt(allGeoms[b].invWorld, pr);
                    const float org[3] = {lo.x, lo.y, lo.z};
                    if (bvhB.raycast(mdB, org, dir, 0.14f * ldLen).hit) return true;
                }
                return false;
            };

            for (const auto& pa : islands[i].samples) {
                const bromath::Vec3 lb = mulPt(allGeoms[b].invWorld, pa);
                const float qPt[3] = {lb.x, lb.y, lb.z};
                const auto hit = bvhB.closestPoint(mdB, qPt);
                if (!hit.hit) continue;
                const bromath::Vec3 pb = mulPt(allGeoms[b].world, {hit.position[0], hit.position[1], hit.position[2]});
                if (bromath::vlen(pb - pa) > contactTol) continue;
                if (!passesTopOnly(pb)) continue;
                extSup = true;
                break;
            }
            if (!extSup) {
                const size_t vcB = mdB.vertexCount();
                const size_t vStepB = std::max<size_t>(1, vcB / 24);
                for (size_t vb = 0; vb < vcB; vb += vStepB) {
                    const bromath::Vec3 pb = mulPt(allGeoms[b].world, {mdB.positions[vb * 3], mdB.positions[vb * 3 + 1], mdB.positions[vb * 3 + 2]});
                    if (pb.x < islands[i].worldBox.min.x - contactTol || pb.x > islands[i].worldBox.max.x + contactTol ||
                        pb.y < islands[i].worldBox.min.y - contactTol || pb.y > islands[i].worldBox.max.y + contactTol ||
                        pb.z < islands[i].worldBox.min.z - contactTol || pb.z > islands[i].worldBox.max.z + contactTol) {
                        continue;
                    }
                    if (pointToIslandDist(pb, islands[i], selfMd, selfWorld, selfInv) <= contactTol && passesTopOnly(pb)) {
                        extSup = true;
                        break;
                    }
                }
            }
            if (!extSup && ext.y > 0.02f) {
                const bromath::Vec3 footOrg{ctr.x, islands[i].worldBox.max.y - 0.01f, ctr.z};
                const bromath::Vec3 lo = mulPt(allGeoms[b].invWorld, footOrg);
                const bromath::Vec3 ldRaw = mulDir(allGeoms[b].invWorld, {0, -1, 0});
                const float ldLen = bromath::vlen(ldRaw);
                if (ldLen >= 1e-8f) {
                    const bromath::Vec3 ld = ldRaw * (1.0f / ldLen);
                    const float org[3] = {lo.x, lo.y, lo.z};
                    const float dir[3] = {ld.x, ld.y, ld.z};
                    const float maxD = (ext.y + static_cast<float>(contactTol)) * ldLen;
                    const auto hitDown = bvhB.raycast(mdB, org, dir, maxD);
                    if (hitDown.hit) {
                        const bromath::Vec3 pb = mulPt(allGeoms[b].world, {hitDown.position[0], hitDown.position[1], hitDown.position[2]});
                        if (!checkTopOnly || pb.y < islands[i].worldBox.max.y - topBand ||
                            pb.y <= islands[i].worldBox.min.y + 0.50f * ext.y) {
                            extSup = true;
                        }
                    }
                }
            }
            if (extSup) break;
        }
        if (extSup) {
            reached[i] = true;
            ++reachedSig;
            q.push(i);
        }
    }

    while (!q.empty()) {
        const size_t u = q.front();
        q.pop();
        for (size_t v : adj[u]) {
            if (!reached[v]) {
                reached[v] = true;
                ++reachedSig;
                q.push(v);
            }
        }
    }
    if (reachedSig == sigCount) return;

    const double intraTol = std::min(contactTol, 0.018);
    for (size_t i = 0; i < m; ++i) {
        if (!significant[i]) continue;
        for (size_t j = i + 1; j < m; ++j) {
            if (!significant[j]) continue;
            if (reached[i] && reached[j]) continue;
            if (std::find(adj[i].begin(), adj[i].end(), j) != adj[i].end()) continue;
            const double gapBox = aabbGap(islands[i].worldBox, islands[j].worldBox);
            if (gapBox > intraTol * 1.5) continue;
            bool touch = false;
            for (const auto& pa : islands[i].samples) {
                if (pointToIslandDist(pa, islands[j], selfMd, selfWorld, selfInv) <= intraTol) {
                    touch = true;
                    break;
                }
            }
            if (!touch) {
                for (const auto& pb : islands[j].samples) {
                    if (pointToIslandDist(pb, islands[i], selfMd, selfWorld, selfInv) <= intraTol) {
                        touch = true;
                        break;
                    }
                }
            }
            if (touch) {
                adj[i].push_back(j);
                adj[j].push_back(i);
                if (reached[i] && !reached[j]) { reached[j] = true; q.push(j); }
                else if (reached[j] && !reached[i]) { reached[i] = true; q.push(i); }
            }
        }
    }

    while (!q.empty()) {
        const size_t u = q.front();
        q.pop();
        for (size_t v : adj[u]) {
            if (!reached[v]) {
                reached[v] = true;
                q.push(v);
            }
        }
    }

    for (size_t i = 0; i < m; ++i) {
        if (!significant[i] || reached[i]) continue;
        ++outFloatingIslands;
        double minG = std::numeric_limits<double>::infinity();
        for (size_t j = 0; j < m; ++j) {
            if (j == i || !reached[j]) continue;
            minG = std::min(minG, aabbGap(islands[i].worldBox, islands[j].worldBox));
        }
        if (minG == std::numeric_limits<double>::infinity()) minG = contactTol * 2.0;
        if (outFloatingIslands == 1 || minG > outWorstGap) {
            outWorstGap = minG;
            outWorstCenter = bromath::acenter(islands[i].worldBox);
            const bromath::Vec3 ext = bromath::aextent(islands[i].worldBox);
            outWorstRadius = std::max(0.22, 0.55 * bromath::vlen(ext));
        }
    }
}

}  // namespace bro::eye
