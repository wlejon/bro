#include "eye/eye_structure.h"
#include "eye/eye_structure_collect.h"
#include "eye/eye_structure_floors.h"
#include "eye/eye_structure_islands.h"
#include "eye/eye_structure_walk.h"
#include "eye/eye_draw_internal.h"

#include "scene/scene_graph.h"
#include <broimage/encode.h>
#include <bromath/aabb.h>
#include <bromath/mat.h>
#include <bromath/vec.h>
#include <bromesh/analysis/bvh.h>
#include <bromesh/analysis/raycast.h>
#include <bromesh/mesh_data.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <limits>
#include <numeric>
#include <queue>
#include <unordered_map>
#include <vector>

namespace bro::eye {
namespace {

struct WindingStats {
    bool closed = false;
    double signedVolume = 0.0;
    double badNormalRatio = 0.0;
};

WindingStats inspectWinding(const StructureMeshEntry& e) {
    WindingStats ws;
    if (!e.mesh) return ws;
    const auto& md = *e.mesh;
    const size_t tc = md.triangleCount();
    const size_t vc = md.vertexCount();
    if (tc == 0 || vc == 0) return ws;

    const bool checkClosure = (tc >= 12 && tc <= 12000);
    std::unordered_map<uint64_t, int> edgeCounts;
    if (checkClosure) edgeCounts.reserve(tc * 3 / 2);

    const bool hasNormals = md.hasNormals();
    size_t badNrm = 0, checkedNrm = 0;
    double vol6 = 0.0;

    for (size_t t = 0; t < tc; ++t) {
        const uint32_t i0 = md.indices[t * 3 + 0];
        const uint32_t i1 = md.indices[t * 3 + 1];
        const uint32_t i2 = md.indices[t * 3 + 2];
        if (i0 >= vc || i1 >= vc || i2 >= vc) continue;
        const bromath::Vec3 p0{md.positions[i0 * 3], md.positions[i0 * 3 + 1], md.positions[i0 * 3 + 2]};
        const bromath::Vec3 p1{md.positions[i1 * 3], md.positions[i1 * 3 + 1], md.positions[i1 * 3 + 2]};
        const bromath::Vec3 p2{md.positions[i2 * 3], md.positions[i2 * 3 + 1], md.positions[i2 * 3 + 2]};

        const bromath::Vec3 fn = bromath::vcross(p1 - p0, p2 - p0);
        if (checkClosure) {
            vol6 += static_cast<double>(bromath::vdot(p0, fn));
            const uint32_t es[3][2] = {{i0, i1}, {i1, i2}, {i2, i0}};
            for (const auto& ed : es) {
                const uint32_t a = std::min(ed[0], ed[1]);
                const uint32_t b = std::max(ed[0], ed[1]);
                edgeCounts[(static_cast<uint64_t>(a) << 32) | b] += 1;
            }
        }
        if (hasNormals && bromath::vlen(fn) > 1e-8f) {
            const bromath::Vec3 n0{md.normals[i0 * 3], md.normals[i0 * 3 + 1], md.normals[i0 * 3 + 2]};
            const bromath::Vec3 n1{md.normals[i1 * 3], md.normals[i1 * 3 + 1], md.normals[i1 * 3 + 2]};
            const bromath::Vec3 n2{md.normals[i2 * 3], md.normals[i2 * 3 + 1], md.normals[i2 * 3 + 2]};
            const bromath::Vec3 vn = n0 + n1 + n2;
            if (bromath::vlen(vn) > 1e-6f) {
                ++checkedNrm;
                if (bromath::vdot(fn, vn) * e.detSign < -1e-6f) ++badNrm;
            }
        }
    }

    if (checkedNrm > 0) ws.badNormalRatio = static_cast<double>(badNrm) / checkedNrm;
    if (checkClosure && !edgeCounts.empty()) {
        size_t boundary = 0;
        for (const auto& [k, c] : edgeCounts) {
            (void)k;
            if (c < 2) ++boundary;
        }
        ws.closed = (static_cast<double>(boundary) / edgeCounts.size()) < 0.05;
        ws.signedVolume = (vol6 / 6.0) * e.detSign;
    }
    return ws;
}

std::vector<bromath::Vec3> sampleWorldSurface(const StructureMeshEntry& e) {
    std::vector<bromath::Vec3> pts;
    if (!e.mesh) return pts;
    const auto& md = *e.mesh;
    const size_t vc = md.vertexCount();
    const size_t tc = md.triangleCount();
    const size_t vStep = std::max<size_t>(1, vc / 80);
    for (size_t i = 0; i < vc; i += vStep) {
        pts.push_back(mulPt(e.world, {md.positions[i * 3], md.positions[i * 3 + 1], md.positions[i * 3 + 2]}));
    }
    const size_t tStep = std::max<size_t>(1, tc / 48);
    for (size_t t = 0; t < tc; t += tStep) {
        const uint32_t i0 = md.indices[t * 3], i1 = md.indices[t * 3 + 1], i2 = md.indices[t * 3 + 2];
        if (i0 >= vc || i1 >= vc || i2 >= vc) continue;
        const bromath::Vec3 c{
            (md.positions[i0 * 3] + md.positions[i1 * 3] + md.positions[i2 * 3]) / 3.0f,
            (md.positions[i0 * 3 + 1] + md.positions[i1 * 3 + 1] + md.positions[i2 * 3 + 1]) / 3.0f,
            (md.positions[i0 * 3 + 2] + md.positions[i1 * 3 + 2] + md.positions[i2 * 3 + 2]) / 3.0f,
        };
        pts.push_back(mulPt(e.world, c));
    }
    return pts;
}

double meshSurfaceDist(const StructureMeshEntry& a, const StructureMeshEntry& b,
                       const std::vector<bromath::Vec3>& ptsA,
                       const std::vector<bromath::Vec3>& ptsB,
                       bromath::Vec3& outPtA, bromath::Vec3& outPtB) {
    double best = std::numeric_limits<double>::infinity();
    if (b.bvh && !b.bvh->empty() && b.mesh) {
        for (const auto& pa : ptsA) {
            const bromath::Vec3 lb = mulPt(b.invWorld, pa);
            const float q[3] = {lb.x, lb.y, lb.z};
            const auto hit = b.bvh->closestPoint(*b.mesh, q);
            if (!hit.hit) continue;
            const bromath::Vec3 pb = mulPt(b.world, {hit.position[0], hit.position[1], hit.position[2]});
            const double d = bromath::vlen(pb - pa);
            if (d < best) { best = d; outPtA = pa; outPtB = pb; }
        }
    }
    if (a.bvh && !a.bvh->empty() && a.mesh) {
        for (const auto& pb : ptsB) {
            const bromath::Vec3 la = mulPt(a.invWorld, pb);
            const float q[3] = {la.x, la.y, la.z};
            const auto hit = a.bvh->closestPoint(*a.mesh, q);
            if (!hit.hit) continue;
            const bromath::Vec3 pa = mulPt(a.world, {hit.position[0], hit.position[1], hit.position[2]});
            const double d = bromath::vlen(pb - pa);
            if (d < best) { best = d; outPtA = pa; outPtB = pb; }
        }
    }
    return best;
}

int findEntryByName(const std::vector<StructureMeshEntry>& entries, const std::string& key) {
    for (size_t i = 0; i < entries.size(); ++i) {
        if (entries[i].name == key) return static_cast<int>(i);
    }
    for (size_t i = 0; i < entries.size(); ++i) {
        if (entries[i].baseName == key) return static_cast<int>(i);
    }
    return -1;
}

}  // namespace

EyeStructureResult analyzeStructure(scene::SceneGraph& graph, const EyeStructureOptions& opts) {
    EyeStructureResult res;
    const double contactTol = std::clamp(opts.contactTol, 0.001, 0.5);
    const double overhangTol = std::clamp(opts.overhangTol, 0.05, 0.95);

    std::vector<StructureMeshEntry> entries = collectStructureEntries(graph, opts, res.instancedMeshCount);
    const size_t n = entries.size();
    res.meshCount = static_cast<int>(n);
    if (n == 0) {
        std::vector<ResolvedFloor> emptyFloors;
        prepareFloors(entries, opts, emptyFloors, res);
        analyzeInteriorWalk(entries, emptyFloors, opts, res);
        finalizeFloorSummaries(emptyFloors, res);
        res.ok = res.issues.empty();
        return res;
    }

    std::vector<ResolvedFloor> resolvedFloors;
    prepareFloors(entries, opts, resolvedFloors, res);

    std::vector<std::vector<bromath::Vec3>> samples(n);
    std::vector<std::vector<IslandSummary>> islandsPerNode(n);
    std::unordered_map<const bromesh::MeshData*, std::vector<LocalIsland>> localIslandCache;
    for (size_t i = 0; i < n; ++i) {
        samples[i] = sampleWorldSurface(entries[i]);
        const auto& e = entries[i];
        if (opts.checkIslands && !e.twoSided && e.alpha >= 0.99f && e.mesh) {
            auto it = localIslandCache.find(e.mesh);
            if (it == localIslandCache.end()) {
                it = localIslandCache.emplace(e.mesh, extractLocalIslands(*e.mesh)).first;
            }
            if (!it->second.empty()) {
                islandsPerNode[i] = instantiateIslands(it->second, *e.mesh, e.world, e.detSign, opts.groundY);
            }
        }
    }

    // A. Inverted face / winding checks (per connected island when decomposed, else whole-node fallback)
    for (size_t i = 0; i < n; ++i) {
        const auto& e = entries[i];
        if (e.twoSided || e.alpha < 0.99f) continue;

        if (!islandsPerNode[i].empty()) {
            int badIslands = 0;
            double worstVal = 0.0;
            bromath::Vec3 worstCtr = bromath::acenter(e.worldBox);
            bromath::Vec3 worstExt = bromath::aextent(e.worldBox);
            double revMax = 0.0, badNMax = 0.0, minVol = 0.0;

            for (const auto& isl : islandsPerNode[i]) {
                const bromath::Vec3 iExt = bromath::aextent(isl.worldBox);
                const float maxE = std::max({iExt.x, iExt.y, iExt.z});
                const float minE = std::min({iExt.x, iExt.y, iExt.z});
                if (maxE < 0.03f || minE < 0.008f) continue;

                const bool mixedEdges = (isl.sharedEdges >= 6 && isl.reversedEdgeRatio > 0.12 &&
                                         ((isl.closed && std::abs(isl.signedVolume) > 1e-4) || isl.badNormalRatio > 0.15));
                const bool negVol = (isl.closed && iExt.y >= 0.015f && isl.signedVolume < -1e-6);
                const bool badN = (isl.badNormalRatio > 0.35);
                if (mixedEdges || negVol || badN) {
                    const double score = std::max({isl.reversedEdgeRatio, isl.badNormalRatio, negVol ? 1.0 : 0.0});
                    if (badIslands == 0 || score > worstVal) {
                        worstVal = score;
                        worstCtr = bromath::acenter(isl.worldBox);
                        worstExt = iExt;
                    }
                    revMax = std::max(revMax, isl.reversedEdgeRatio);
                    badNMax = std::max(badNMax, isl.badNormalRatio);
                    minVol = std::min(minVol, isl.signedVolume);
                    ++badIslands;
                }
            }
            if (badIslands > 0) {
                ++res.invertedCount;
                res.invertedIslandCount += badIslands;
                EyeStructureIssue iss;
                iss.kind = "inverted-winding";
                iss.severity = "error";
                iss.nodeA = e.name;
                iss.value = worstVal;
                iss.point[0] = worstCtr.x; iss.point[1] = worstCtr.y; iss.point[2] = worstCtr.z;
                iss.radius = std::max(0.20, 0.55 * bromath::vlen(worstExt));
                char buf[192];
                std::snprintf(buf, sizeof(buf),
                              "%s has %d inverted/inside-out island(s) (revEdge=%.0f%%, badNrm=%.0f%%, minVol=%.4f)",
                              e.name.c_str(), badIslands, revMax * 100.0, badNMax * 100.0, minVol);
                iss.message = buf;
                res.issues.push_back(std::move(iss));
            }
            continue;
        }

        const WindingStats ws = inspectWinding(e);
        const bromath::Vec3 ext = bromath::aextent(e.worldBox);
        const bool flatDisc = (ext.y < 0.015f);
        const bool inverted = (!flatDisc && ws.closed && ws.signedVolume < -1e-6) || (ws.badNormalRatio > 0.35);
        if (inverted) {
            ++res.invertedCount;
            const bromath::Vec3 ctr = bromath::acenter(e.worldBox);
            EyeStructureIssue iss;
            iss.kind = "inverted-winding";
            iss.severity = "error";
            iss.nodeA = e.name;
            iss.value = ws.badNormalRatio > 0.35 ? ws.badNormalRatio : 1.0;
            iss.point[0] = ctr.x; iss.point[1] = ctr.y; iss.point[2] = ctr.z;
            iss.radius = std::max(0.20, 0.5 * bromath::vlen(ext));
            char buf[160];
            std::snprintf(buf, sizeof(buf), "%s has inside-out/inverted winding (badNrm=%.0f%%, vol=%.4f)",
                          e.name.c_str(), ws.badNormalRatio * 100.0, ws.signedVolume);
            iss.message = buf;
            res.issues.push_back(std::move(iss));
        }
    }

    // B. Watch pairs gap measurements & sweep-and-prune surface-contact connectivity BFS
    for (const auto& [ka, kb] : opts.watchPairs) {
        const int ia = findEntryByName(entries, ka);
        const int ib = findEntryByName(entries, kb);
        if (ia < 0 || ib < 0 || ia == ib) continue;
        bromath::Vec3 pA{}, pB{};
        const double gap = meshSurfaceDist(entries[ia], entries[ib], samples[ia], samples[ib], pA, pB);
        EyeStructurePairGap pg;
        pg.nodeA = ka; pg.nodeB = kb; pg.gap = gap;
        pg.pointA[0] = pA.x; pg.pointA[1] = pA.y; pg.pointA[2] = pA.z;
        pg.pointB[0] = pB.x; pg.pointB[1] = pB.y; pg.pointB[2] = pB.z;
        res.watchedGaps.push_back(pg);
        if (gap > contactTol) {
            ++res.gapCount;
            EyeStructureIssue iss;
            iss.kind = "unmeshed-gap";
            iss.severity = "error";
            iss.nodeA = ka; iss.nodeB = kb;
            iss.value = gap;
            iss.point[0] = (pA.x + pB.x) * 0.5;
            iss.point[1] = (pA.y + pB.y) * 0.5;
            iss.point[2] = (pA.z + pB.z) * 0.5;
            iss.radius = std::max(0.24, gap * 3.0);
            char buf[160];
            std::snprintf(buf, sizeof(buf), "%s and %s do not mesh (gap=%.3fu > %.3fu)", ka.c_str(), kb.c_str(), gap, contactTol);
            iss.message = buf;
            res.issues.push_back(std::move(iss));
        }
    }

    std::vector<size_t> sweepOrder(n);
    std::iota(sweepOrder.begin(), sweepOrder.end(), 0);
    std::sort(sweepOrder.begin(), sweepOrder.end(), [&](size_t a, size_t b) {
        return entries[a].worldBox.min.x < entries[b].worldBox.min.x;
    });

    std::vector<std::vector<size_t>> adj(n);
    for (size_t sa = 0; sa < n; ++sa) {
        const size_t i = sweepOrder[sa];
        const float maxXi = entries[i].worldBox.max.x + static_cast<float>(contactTol);
        for (size_t sb = sa + 1; sb < n; ++sb) {
            const size_t j = sweepOrder[sb];
            if (entries[j].worldBox.min.x > maxXi) break;
            if (aabbGap(entries[i].worldBox, entries[j].worldBox) > contactTol) continue;
            bromath::Vec3 pA{}, pB{};
            const double d = meshSurfaceDist(entries[i], entries[j], samples[i], samples[j], pA, pB);
            if (d <= contactTol) {
                adj[i].push_back(j);
                adj[j].push_back(i);
            }
        }
    }

    std::vector<bool> reached(n, false);
    std::queue<size_t> q;
    for (size_t i = 0; i < n; ++i) {
        if (entries[i].grounded) { reached[i] = true; q.push(i); }
    }
    while (!q.empty()) {
        const size_t u = q.front();
        q.pop();
        for (size_t v : adj[u]) {
            if (!reached[v]) { reached[v] = true; q.push(v); }
        }
    }

    for (size_t i = 0; i < n; ++i) {
        if (reached[i] || entries[i].ignored) continue;
        double nearestDist = std::numeric_limits<double>::infinity();
        size_t nearestIdx = 0;
        for (size_t j = 0; j < n; ++j) {
            if (j == i) continue;
            const double bg = aabbGap(entries[i].worldBox, entries[j].worldBox);
            if (bg > nearestDist) continue;
            bromath::Vec3 pA{}, pB{};
            const double d = meshSurfaceDist(entries[i], entries[j], samples[i], samples[j], pA, pB);
            if (d < nearestDist) { nearestDist = d; nearestIdx = j; }
        }

        ++res.floatingCount;
        if (!resolvedFloors.empty() && entries[i].floorIndex >= 0 &&
            static_cast<size_t>(entries[i].floorIndex) < resolvedFloors.size()) {
            resolvedFloors[static_cast<size_t>(entries[i].floorIndex)].floatingCount += 1;
        }
        const bromath::Vec3 ctr = bromath::acenter(entries[i].worldBox);
        const bromath::Vec3 ext = bromath::aextent(entries[i].worldBox);
        EyeStructureIssue iss;
        iss.kind = "floating";
        iss.severity = "error";
        iss.nodeA = entries[i].name;
        iss.nodeB = entries[nearestIdx].name;
        iss.value = nearestDist;
        iss.point[0] = ctr.x; iss.point[1] = ctr.y; iss.point[2] = ctr.z;
        iss.radius = std::max(0.24, 0.55 * bromath::vlen(ext));
        char buf[160];
        std::snprintf(buf, sizeof(buf), "%s is floating/disconnected (nearest: %s at %.3fu)",
                      entries[i].name.c_str(), entries[nearestIdx].name.c_str(), nearestDist);
        iss.message = buf;
        res.issues.push_back(std::move(iss));
    }

    // Intra-node per-island connectivity & gravity-aware external support audit
    if (opts.checkIslands) {
        std::vector<MeshGeomRef> allGeoms(n);
        for (size_t i = 0; i < n; ++i) {
            allGeoms[i] = {entries[i].mesh, entries[i].bvh, entries[i].world, entries[i].invWorld, entries[i].worldBox};
        }
        for (size_t i = 0; i < n; ++i) {
            const auto& e = entries[i];
            if (!reached[i] || e.ignored || e.groundNamed || e.isFloorAnchor) continue;
            if (islandsPerNode[i].size() < 2 || islandsPerNode[i].size() > 128) continue;

            int floatIsl = 0;
            double worstGap = 0.0, worstRad = 0.24;
            bromath::Vec3 worstCtr{};
            auditIslandGrounding(islandsPerNode[i], i, allGeoms,
                                 contactTol, floatIsl, worstGap, worstCtr, worstRad);
            if (floatIsl > 0) {
                ++res.floatingCount;
                res.floatingIslandCount += floatIsl;
                if (!resolvedFloors.empty() && e.floorIndex >= 0 &&
                    static_cast<size_t>(e.floorIndex) < resolvedFloors.size()) {
                    resolvedFloors[static_cast<size_t>(e.floorIndex)].floatingCount += 1;
                }
                EyeStructureIssue iss;
                iss.kind = "floating";
                iss.severity = "error";
                iss.nodeA = e.name;
                iss.value = worstGap;
                iss.point[0] = worstCtr.x; iss.point[1] = worstCtr.y; iss.point[2] = worstCtr.z;
                iss.radius = worstRad;
                char buf[192];
                std::snprintf(buf, sizeof(buf),
                              "%s has %d floating/disconnected sub-part(s) (worst gap %.3fu)",
                              e.name.c_str(), floatIsl, worstGap);
                iss.message = buf;
                res.issues.push_back(std::move(iss));
            }
        }
    }

    // Multi-floor footing audit
    std::vector<bool> handledByFloorAudit;
    auditFloorFootings(entries, opts, reached, overhangTol, resolvedFloors, res, handledByFloorAudit);

    // C. Overhanging base footing check for resting flat-footed pillars/pedestals/supports
    for (size_t i = 0; i < n; ++i) {
        if (!handledByFloorAudit.empty() && handledByFloorAudit[i]) continue;
        const auto& e = entries[i];
        if (e.ignored || e.grounded || e.isFloorAnchor || e.twoSided || e.alpha < 0.99f) continue;
        const bromath::Vec3 ext = bromath::aextent(e.worldBox);
        if (ext.y < 0.14f || ext.x < 0.10f || ext.z < 0.10f || ext.x > 1.8f || ext.z > 1.8f) continue;
        if (ext.y < 0.45f * std::max(ext.x, ext.z)) continue;

        if (!e.bvh || e.bvh->empty() || !e.mesh) continue;
        const auto& bvhSelf = *e.bvh;
        const auto& mdSelf = *e.mesh;

        int footSamples = 0, unsupported = 0;
        bromath::Vec3 missSum{0, 0, 0};
        const float footY = e.worldBox.min.y;

        for (int ix = 0; ix < 3; ++ix) {
            const float wx = e.worldBox.min.x + ext.x * (0.14f + 0.36f * ix);
            for (int iz = 0; iz < 3; ++iz) {
                const float wz = e.worldBox.min.z + ext.z * (0.14f + 0.36f * iz);
                const bromath::Vec3 probeW{wx, footY - 0.02f, wz};
                const bromath::Vec3 loS = mulPt(e.invWorld, probeW);
                const bromath::Vec3 ldSRaw = mulDir(e.invWorld, {0, 1, 0});
                const float ldSLen = bromath::vlen(ldSRaw);
                if (ldSLen < 1e-8f) continue;
                const bromath::Vec3 ldS = ldSRaw * (1.0f / ldSLen);
                const float opS[3] = {loS.x, loS.y, loS.z}, dpS[3] = {ldS.x, ldS.y, ldS.z};
                const auto selfHit = bvhSelf.raycast(mdSelf, opS, dpS, 0.06f * ldSLen);
                if (!selfHit.hit) continue;
                const float* id = e.invWorld.data;
                const float* ln = selfHit.normal;
                const bromath::Vec3 wNrm = bromath::vnorm({
                    id[0] * ln[0] + id[1] * ln[1] + id[2] * ln[2],
                    id[4] * ln[0] + id[5] * ln[1] + id[6] * ln[2],
                    id[8] * ln[0] + id[9] * ln[1] + id[10] * ln[2],
                });
                if (wNrm.y > -0.65f) continue;

                ++footSamples;
                bool hasSupportBelow = (footY <= opts.groundY + 0.04f);
                if (!hasSupportBelow) {
                    const bromath::Vec3 rayOrg{wx, footY + 0.03f, wz};
                    for (size_t j = 0; j < n; ++j) {
                        if (j == i || !entries[j].bvh || entries[j].bvh->empty() || !entries[j].mesh) continue;
                        const auto& bBox = entries[j].worldBox;
                        if (wx < bBox.min.x - 0.01f || wx > bBox.max.x + 0.01f ||
                            wz < bBox.min.z - 0.01f || wz > bBox.max.z + 0.01f ||
                            bBox.max.y < footY - 0.10f || bBox.min.y > footY + 0.05f) continue;
                        const bromath::Vec3 loO = mulPt(entries[j].invWorld, rayOrg);
                        const bromath::Vec3 ldORaw = mulDir(entries[j].invWorld, {0, -1, 0});
                        const float ldOLen = bromath::vlen(ldORaw);
                        if (ldOLen < 1e-8f) continue;
                        const bromath::Vec3 ldO = ldORaw * (1.0f / ldOLen);
                        const float opO[3] = {loO.x, loO.y, loO.z}, dpO[3] = {ldO.x, ldO.y, ldO.z};
                        if (entries[j].bvh->raycast(*entries[j].mesh, opO, dpO, 0.11f * ldOLen).hit) {
                            hasSupportBelow = true;
                            break;
                        }
                    }
                }
                if (!hasSupportBelow) {
                    ++unsupported;
                    missSum = missSum + bromath::Vec3{wx, footY, wz};
                }
            }
        }

        if (footSamples >= 3 && unsupported > 0 && unsupported < footSamples) {
            const double missRatio = static_cast<double>(unsupported) / footSamples;
            if (missRatio >= overhangTol) {
                ++res.overhangCount;
                if (!resolvedFloors.empty() && e.floorIndex >= 0 &&
                    static_cast<size_t>(e.floorIndex) < resolvedFloors.size()) {
                    resolvedFloors[static_cast<size_t>(e.floorIndex)].overhangCount += 1;
                }
                const bromath::Vec3 mc = missSum * (1.0f / static_cast<float>(unsupported));
                EyeStructureIssue iss;
                iss.kind = "overhang";
                iss.severity = "error";
                iss.nodeA = e.name;
                iss.value = missRatio;
                iss.point[0] = mc.x; iss.point[1] = mc.y; iss.point[2] = mc.z;
                iss.radius = std::max(0.28, 0.65 * std::max(ext.x, ext.z));
                char buf[160];
                std::snprintf(buf, sizeof(buf), "%s footing overhangs support (%d/%d = %.0f%% unsupported)",
                              e.name.c_str(), unsupported, footSamples, missRatio * 100.0);
                iss.message = buf;
                res.issues.push_back(std::move(iss));
            }
        }
    }

    // Interior walkability (doorways, corridors, stairs, headroom)
    analyzeInteriorWalk(entries, resolvedFloors, opts, res);
    finalizeFloorSummaries(resolvedFloors, res);

    res.ok = res.issues.empty();

    // D. Auto-zoomed macro defect sheet (restoring active camera cleanly afterwards)
    if (!opts.path.empty()) {
        scene::CameraNode* savedCamNode = graph.activeCamera();
        const bromath::Mat4 origV = graph.viewMatrix();
        const bromath::Mat4 origP = graph.projectionMatrix();
        const bromath::Vec3 origEye = graph.cameraEye();
        const float origFov = graph.cameraFovY();
        const float origNear = graph.cameraNearZ();
        const float origFar = graph.cameraFarZ();
        const int cw = graph.canvasWidth(), ch = graph.canvasHeight();
        const float p00 = origP.at(0, 0), p11 = origP.at(1, 1);
        const float aspect = (cw > 0 && ch > 0) ? (static_cast<float>(cw) / ch)
                         : ((std::abs(p00) > 1e-6f && std::abs(p11) > 1e-6f) ? std::abs(p11 / p00) : 16.0f / 9.0f);
        const bromath::Vec3 origUp{origV.at(1, 0), origV.at(1, 1), origV.at(1, 2)};
        const bromath::Vec3 origFwd{-origV.at(2, 0), -origV.at(2, 1), -origV.at(2, 2)};

        std::vector<EyeImage> panels;
        std::vector<std::string> labels;
        const size_t maxPanels = static_cast<size_t>(std::clamp(opts.maxZoomPanels, 1, 12));

        if (res.issues.empty()) {
            graph.renderForRead();
            int w = 0, h = 0;
            const auto rgba = graph.readTonemapPixelsRGBA(w, h);
            if (!rgba.empty()) {
                EyeImage im = draw::fitPanel(EyeImageView{rgba.data(), w, h, w * 4}, opts.maxPanelDim);
                char bnr[80];
                std::snprintf(bnr, sizeof(bnr), "STRUCTURE OK | %d meshes | 0 defects", res.meshCount);
                draw::drawText(im, 8, 8, bnr, 1, 110, 245, 165, true);
                panels.push_back(std::move(im));
                labels.emplace_back("overview");
            }
        } else {
            for (size_t k = 0; k < std::min(res.issues.size(), maxPanels); ++k) {
                const auto& iss = res.issues[k];
                const bromath::Vec3 tgt{static_cast<float>(iss.point[0]), static_cast<float>(iss.point[1]), static_cast<float>(iss.point[2])};
                const float dist = static_cast<float>(std::clamp(iss.radius * 2.8, 0.55, 2.4));
                const bromath::Vec3 camPos = tgt + bromath::vnorm({0.65f, 0.45f, 0.72f}) * dist;
                graph.setCamera(0.68f, aspect, std::max(0.02f, origNear), std::max(40.0f, origFar), camPos, tgt, {0, 1, 0});
                graph.renderForRead();
                int w = 0, h = 0;
                const auto rgba = graph.readTonemapPixelsRGBA(w, h);
                if (rgba.empty() || w <= 0 || h <= 0) continue;
                EyeImage im = draw::fitPanel(EyeImageView{rgba.data(), w, h, w * 4}, opts.maxPanelDim);

                float lx = 0, ly = 0, depth = 0;
                if (graph.projectLocal(tgt, lx, ly, depth) && depth > 0.0f) {
                    const float cwf = static_cast<float>(cw > 0 ? cw : w);
                    const float chf = static_cast<float>(ch > 0 ? ch : h);
                    const int px = std::clamp(static_cast<int>(std::lround((lx / cwf) * im.width)), 8, im.width - 9);
                    const int py = std::clamp(static_cast<int>(std::lround((ly / chf) * im.height)), 8, im.height - 9);
                    draw::drawCircle(im, px, py, 10, 2, 255, 78, 86, 245);
                    draw::drawLine(im, px - 14, py, px + 14, py, 1, 255, 78, 86, 220);
                    draw::drawLine(im, px, py - 14, px, py + 14, 1, 255, 78, 86, 220);
                }
                panels.push_back(std::move(im));
                labels.push_back(iss.kind + ": " + iss.nodeA);
            }
            if (savedCamNode) graph.setActiveCamera(savedCamNode);
            else graph.setCamera(origFov, aspect, origNear, origFar, origEye, origEye + origFwd * 5.0f, origUp);
        }

        if (!panels.empty()) {
            const auto sheet = draw::assembleSheet(std::move(panels), labels, "grid", 1);
            res.visual = {sheet.width, sheet.height, std::move(sheet.rgba)};
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
