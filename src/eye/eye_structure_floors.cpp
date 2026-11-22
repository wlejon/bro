#include "eye/eye_structure_floors.h"

#include <bromesh/analysis/raycast.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace bro::eye {
namespace {

bool hasSlabName(const StructureMeshEntry& e) {
    return e.groundNamed ||
           e.name.find("Slab") != std::string::npos ||
           e.name.find("Deck") != std::string::npos ||
           e.name.find("Landing") != std::string::npos;
}

bool isSlabCandidate(const StructureMeshEntry& e) {
    const bromath::Vec3 ext = bromath::aextent(e.worldBox);
    const float maxXZ = std::max(ext.x, ext.z);
    if (ext.x >= 1.2f && ext.z >= 1.2f && ext.y > 0.005f && ext.y <= 0.65f && ext.y < 0.25f * maxXZ) {
        return true;
    }
    if (hasSlabName(e)) {
        return ext.y <= 0.85f && maxXZ >= 1.0f;
    }
    return false;
}

bool boxesOverlapXZ(const bromath::AABB3& a, const bromath::AABB3& b) {
    return a.max.x >= b.min.x && a.min.x <= b.max.x &&
           a.max.z >= b.min.z && a.min.z <= b.max.z;
}

}  // namespace

void prepareFloors(std::vector<StructureMeshEntry>& entries,
                   const EyeStructureOptions& opts,
                   std::vector<ResolvedFloor>& outFloors,
                   EyeStructureResult& res) {
    outFloors.clear();
    if (opts.floors.empty() && !opts.autoDetectFloors) return;

    for (size_t i = 0; i < opts.floors.size(); ++i) {
        ResolvedFloor rf;
        rf.y = opts.floors[i].y;
        rf.tolerance = std::clamp(opts.floors[i].tolerance, 0.005, 0.5);
        rf.name = opts.floors[i].name.empty() ? ("floor_" + std::to_string(i)) : opts.floors[i].name;
        outFloors.push_back(std::move(rf));
    }

    if (opts.autoDetectFloors) {
        struct Cluster {
            double ySum = 0.0;
            int count = 0;
        };
        std::vector<const StructureMeshEntry*> slabCands;
        for (const auto& e : entries) {
            if (isSlabCandidate(e)) slabCands.push_back(&e);
        }
        std::vector<double> candYs;
        for (size_t ci = 0; ci < slabCands.size(); ++ci) {
            const auto* c = slabCands[ci];
            bool isFurnitureOnLowerSlab = false;
            if (!hasSlabName(*c)) {
                const bromath::Vec3 extC = bromath::aextent(c->worldBox);
                const float minXZ = std::min(extC.x, extC.z);
                const float areaC = extC.x * extC.z;
                for (size_t bi = 0; bi < slabCands.size(); ++bi) {
                    if (bi == ci) continue;
                    const auto* b = slabCands[bi];
                    if (b->worldBox.max.y > c->worldBox.min.y + 0.08f) continue;
                    if (!boxesOverlapXZ(c->worldBox, b->worldBox)) continue;
                    const bromath::Vec3 extB = bromath::aextent(b->worldBox);
                    const float areaB = extB.x * extB.z;
                    const bool restingOnB = (c->worldBox.min.y <= b->worldBox.max.y + 0.08f);
                    const bool lowAboveB = ((c->worldBox.max.y - b->worldBox.max.y) < 1.50f);
                    if ((restingOnB && (minXZ < 2.5f || areaC < 0.80f * areaB)) ||
                        (lowAboveB && minXZ < 2.5f && areaC < 0.65f * areaB)) {
                        isFurnitureOnLowerSlab = true;
                        break;
                    }
                }
                if (!isFurnitureOnLowerSlab) {
                    for (const auto& rf : outFloors) {
                        if (rf.y > static_cast<double>(c->worldBox.min.y) + 0.08) continue;
                        const bool restingOnRf = (static_cast<double>(c->worldBox.min.y) <= rf.y + 0.08);
                        const bool lowAboveRf = ((static_cast<double>(c->worldBox.max.y) - rf.y) < 1.50);
                        if ((restingOnRf || lowAboveRf) && minXZ < 2.5f) {
                            isFurnitureOnLowerSlab = true;
                            break;
                        }
                    }
                }
            }
            if (!isFurnitureOnLowerSlab) {
                candYs.push_back(static_cast<double>(c->worldBox.max.y));
            }
        }
        std::sort(candYs.begin(), candYs.end());
        std::vector<Cluster> clusters;
        for (double y : candYs) {
            if (!clusters.empty() && std::abs(y - (clusters.back().ySum / clusters.back().count)) <= 0.15) {
                clusters.back().ySum += y;
                clusters.back().count += 1;
            } else {
                clusters.push_back({y, 1});
            }
        }
        size_t autoIdx = 0;
        for (const auto& cl : clusters) {
            const double cy = cl.ySum / cl.count;
            bool merged = false;
            for (const auto& rf : outFloors) {
                if (std::abs(rf.y - cy) <= 0.15) { merged = true; break; }
            }
            if (!merged) {
                ResolvedFloor rf;
                rf.name = "floor_auto_" + std::to_string(autoIdx++);
                rf.y = cy;
                rf.tolerance = 0.05;
                outFloors.push_back(std::move(rf));
            }
        }
    }

    if (outFloors.empty()) return;

    std::sort(outFloors.begin(), outFloors.end(), [](const ResolvedFloor& a, const ResolvedFloor& b) {
        return a.y < b.y;
    });

    const double minSep = std::max(0.1, opts.minFloorSeparation);
    for (size_t k = 0; k + 1 < outFloors.size(); ++k) {
        const double sep = outFloors[k + 1].y - outFloors[k].y;
        if (sep < minSep) {
            ++res.floorDefectCount;
            EyeStructureIssue iss;
            iss.kind = "floor-separation";
            iss.severity = "error";
            iss.nodeA = outFloors[k].name;
            iss.nodeB = outFloors[k + 1].name;
            iss.value = sep;
            iss.point[0] = 0.0;
            iss.point[1] = 0.5 * (outFloors[k].y + outFloors[k + 1].y);
            iss.point[2] = 0.0;
            iss.radius = std::max(0.5, minSep);
            char buf[192];
            std::snprintf(buf, sizeof(buf),
                          "floors %s (y=%.2f) and %s (y=%.2f) separated by %.2fm < %.2fm",
                          outFloors[k].name.c_str(), outFloors[k].y,
                          outFloors[k + 1].name.c_str(), outFloors[k + 1].y,
                          sep, minSep);
            iss.message = buf;
            res.issues.push_back(std::move(iss));
        }
    }

    for (auto& e : entries) {
        if (isSlabCandidate(e)) {
            int bestK = -1;
            double bestDist = 1e9;
            for (size_t k = 0; k < outFloors.size(); ++k) {
                const double tol = outFloors[k].tolerance + 0.05;
                const double d = std::abs(static_cast<double>(e.worldBox.max.y) - outFloors[k].y);
                const bool topAtFloor = (d <= tol) &&
                    (static_cast<double>(e.worldBox.min.y) < outFloors[k].y - 0.01 || e.groundNamed);
                if (topAtFloor && d < bestDist) {
                    bestDist = d;
                    bestK = static_cast<int>(k);
                }
            }
            if (bestK >= 0) {
                e.isFloorAnchor = true;
                e.grounded = true;
                e.floorIndex = bestK;
                auto& rf = outFloors[static_cast<size_t>(bestK)];
                rf.slabCount += 1;
                rf.entryCount += 1;
                rf.minSlabY = std::min(rf.minSlabY, static_cast<double>(e.worldBox.min.y));
                continue;
            }
        }

        int assigned = 0;
        for (size_t k = 0; k < outFloors.size(); ++k) {
            if (outFloors[k].y <= static_cast<double>(e.worldBox.min.y) + outFloors[k].tolerance) {
                assigned = static_cast<int>(k);
            }
        }
        e.floorIndex = assigned;
        outFloors[static_cast<size_t>(assigned)].entryCount += 1;
    }
}

void auditFloorFootings(const std::vector<StructureMeshEntry>& entries,
                        const EyeStructureOptions& opts,
                        const std::vector<bool>& reached,
                        double overhangTol,
                        std::vector<ResolvedFloor>& floors,
                        EyeStructureResult& res,
                        std::vector<bool>& outHandled) {
    const size_t n = entries.size();
    outHandled.assign(n, false);
    if (floors.empty()) return;

    for (size_t i = 0; i < n; ++i) {
        const auto& e = entries[i];
        if (e.ignored || e.isFloorAnchor || e.groundNamed || e.twoSided || e.alpha < 0.99f) continue;
        if (e.floorIndex < 0 || static_cast<size_t>(e.floorIndex) >= floors.size()) continue;
        const auto& rf = floors[static_cast<size_t>(e.floorIndex)];
        const double footY = static_cast<double>(e.worldBox.min.y);
        if (std::abs(footY - rf.y) > rf.tolerance + 0.04) continue;

        const bromath::Vec3 ext = bromath::aextent(e.worldBox);
        if (ext.y < 0.04f || ext.x < 0.10f || ext.z < 0.10f) continue;
        if (!e.bvh || e.bvh->empty() || !e.mesh) continue;

        outHandled[i] = true;
        int footSamples = 0;
        int unsupported = 0;
        bromath::Vec3 missSum{0, 0, 0};

        for (int ix = 0; ix < 3; ++ix) {
            const float wx = e.worldBox.min.x + ext.x * (0.14f + 0.36f * ix);
            for (int iz = 0; iz < 3; ++iz) {
                const float wz = e.worldBox.min.z + ext.z * (0.14f + 0.36f * iz);
                const bromath::Vec3 probeW{wx, static_cast<float>(footY) - 0.02f, wz};
                const bromath::Vec3 loS = mulPt(e.invWorld, probeW);
                const bromath::Vec3 ldSRaw = mulDir(e.invWorld, {0, 1, 0});
                const float ldSLen = bromath::vlen(ldSRaw);
                if (ldSLen < 1e-8f) continue;
                const bromath::Vec3 ldS = ldSRaw * (1.0f / ldSLen);
                const float opS[3] = {loS.x, loS.y, loS.z}, dpS[3] = {ldS.x, ldS.y, ldS.z};
                const auto selfHit = e.bvh->raycast(*e.mesh, opS, dpS, 0.06f * ldSLen);
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
                bool hasSupportBelow = (rf.slabCount == 0 && footY <= opts.groundY + 0.04);
                if (!hasSupportBelow) {
                    const bromath::Vec3 rayOrg{wx, static_cast<float>(footY) + 0.03f, wz};
                    const float maxDist = static_cast<float>(0.11 + rf.tolerance);
                    for (size_t j = 0; j < n; ++j) {
                        if (j == i || !entries[j].bvh || entries[j].bvh->empty() || !entries[j].mesh) continue;
                        const auto& bBox = entries[j].worldBox;
                        if (wx < bBox.min.x - 0.01f || wx > bBox.max.x + 0.01f ||
                            wz < bBox.min.z - 0.01f || wz > bBox.max.z + 0.01f ||
                            bBox.max.y < footY - maxDist || bBox.min.y > footY + 0.05f) {
                            continue;
                        }
                        const bromath::Vec3 loO = mulPt(entries[j].invWorld, rayOrg);
                        const bromath::Vec3 ldO = mulDir(entries[j].invWorld, {0, -1, 0});
                        const float ldLen = bromath::vlen(ldO);
                        if (ldLen < 1e-8f) continue;
                        const bromath::Vec3 ldN = ldO * (1.0f / ldLen);
                        const float opO[3] = {loO.x, loO.y, loO.z}, dpO[3] = {ldN.x, ldN.y, ldN.z};
                        if (entries[j].bvh->raycast(*entries[j].mesh, opO, dpO, maxDist * ldLen).hit) {
                            hasSupportBelow = true;
                            break;
                        }
                    }
                }
                if (!hasSupportBelow) {
                    ++unsupported;
                    missSum = missSum + bromath::Vec3{wx, static_cast<float>(footY), wz};
                }
            }
        }

        if (footSamples >= 3 && unsupported > 0 && unsupported < footSamples) {
            const double missRatio = static_cast<double>(unsupported) / footSamples;
            if (missRatio >= overhangTol) {
                ++res.overhangCount;
                floors[static_cast<size_t>(e.floorIndex)].overhangCount += 1;
                const bromath::Vec3 mc = missSum * (1.0f / static_cast<float>(unsupported));
                EyeStructureIssue iss;
                iss.kind = "overhang";
                iss.severity = "error";
                iss.nodeA = e.name;
                iss.value = missRatio;
                iss.point[0] = mc.x; iss.point[1] = mc.y; iss.point[2] = mc.z;
                iss.radius = std::max(0.28, 0.65 * std::max(ext.x, ext.z));
                char buf[192];
                std::snprintf(buf, sizeof(buf),
                              "%s footing overhangs %s (%d/%d = %.0f%% unsupported)",
                              e.name.c_str(), rf.name.c_str(), unsupported, footSamples, missRatio * 100.0);
                iss.message = buf;
                res.issues.push_back(std::move(iss));
            }
        } else if (footSamples >= 3 && unsupported == footSamples && reached[i]) {
            // Reached laterally via neighbor, or unsupported beneath -> ignore if supported laterally.
        }
    }
}

void finalizeFloorSummaries(const std::vector<ResolvedFloor>& floors,
                            EyeStructureResult& res) {
    res.floors.clear();
    res.floors.reserve(floors.size());
    for (const auto& rf : floors) {
        EyeStructureFloorSummary fs;
        fs.name = rf.name;
        fs.y = rf.y;
        fs.tolerance = rf.tolerance;
        fs.slabCount = rf.slabCount;
        fs.entryCount = rf.entryCount;
        fs.floatingCount = rf.floatingCount;
        fs.overhangCount = rf.overhangCount;
        res.floors.push_back(std::move(fs));
    }
}

}  // namespace bro::eye
