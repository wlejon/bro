#include "eye/eye_structure_walk.h"

#include <bromesh/analysis/raycast.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>

namespace bro::eye {
namespace {

bool axisTest(float a, float b, float fa, float fb,
              float v0a, float v0b, float v1a, float v1b, float halfA, float halfB) {
    const float p0 = a * v0a + b * v0b;
    const float p1 = a * v1a + b * v1b;
    const float mn = std::min(p0, p1);
    const float mx = std::max(p0, p1);
    const float rad = fa * halfA + fb * halfB;
    return !(mn > rad || mx < -rad);
}

bool planeBoxOverlap(const bromath::Vec3& normal, const bromath::Vec3& vert, const bromath::Vec3& maxbox) {
    bromath::Vec3 vmin{}, vmax{};
    const float n[3] = {normal.x, normal.y, normal.z};
    const float v[3] = {vert.x, vert.y, vert.z};
    const float h[3] = {maxbox.x, maxbox.y, maxbox.z};
    float mn[3]{}, mx[3]{};
    for (int q = 0; q < 3; ++q) {
        if (n[q] > 0.0f) {
            mn[q] = -h[q] - v[q];
            mx[q] =  h[q] - v[q];
        } else {
            mn[q] =  h[q] - v[q];
            mx[q] = -h[q] - v[q];
        }
    }
    vmin = {mn[0], mn[1], mn[2]};
    vmax = {mx[0], mx[1], mx[2]};
    if (bromath::vdot(normal, vmin) > 0.0f) return false;
    if (bromath::vdot(normal, vmax) >= 0.0f) return true;
    return false;
}

bool triBoxOverlap(const bromath::Vec3& boxCenter, const bromath::Vec3& boxHalf,
                   const bromath::Vec3& tv0, const bromath::Vec3& tv1, const bromath::Vec3& tv2) {
    const bromath::Vec3 v0 = tv0 - boxCenter;
    const bromath::Vec3 v1 = tv1 - boxCenter;
    const bromath::Vec3 v2 = tv2 - boxCenter;
    const bromath::Vec3 e0 = v1 - v0;
    const bromath::Vec3 e1 = v2 - v1;
    const bromath::Vec3 e2 = v0 - v2;

    const float fex0 = std::fabs(e0.x), fey0 = std::fabs(e0.y), fez0 = std::fabs(e0.z);
    if (!axisTest(e0.z, -e0.y, fez0, fey0, v0.y, v0.z, v2.y, v2.z, boxHalf.y, boxHalf.z)) return false;
    if (!axisTest(-e0.z, e0.x, fez0, fex0, v0.x, v0.z, v2.x, v2.z, boxHalf.x, boxHalf.z)) return false;
    if (!axisTest(e0.y, -e0.x, fey0, fex0, v1.x, v1.y, v2.x, v2.y, boxHalf.x, boxHalf.y)) return false;

    const float fex1 = std::fabs(e1.x), fey1 = std::fabs(e1.y), fez1 = std::fabs(e1.z);
    if (!axisTest(e1.z, -e1.y, fez1, fey1, v0.y, v0.z, v2.y, v2.z, boxHalf.y, boxHalf.z)) return false;
    if (!axisTest(-e1.z, e1.x, fez1, fex1, v0.x, v0.z, v2.x, v2.z, boxHalf.x, boxHalf.z)) return false;
    if (!axisTest(e1.y, -e1.x, fey1, fex1, v0.x, v0.y, v1.x, v1.y, boxHalf.x, boxHalf.y)) return false;

    const float fex2 = std::fabs(e2.x), fey2 = std::fabs(e2.y), fez2 = std::fabs(e2.z);
    if (!axisTest(e2.z, -e2.y, fez2, fey2, v0.y, v0.z, v1.y, v1.z, boxHalf.y, boxHalf.z)) return false;
    if (!axisTest(-e2.z, e2.x, fez2, fex2, v0.x, v0.z, v1.x, v1.z, boxHalf.x, boxHalf.z)) return false;
    if (!axisTest(e2.y, -e2.x, fey2, fex2, v1.x, v1.y, v2.x, v2.y, boxHalf.x, boxHalf.y)) return false;

    if (std::min({v0.x, v1.x, v2.x}) > boxHalf.x || std::max({v0.x, v1.x, v2.x}) < -boxHalf.x) return false;
    if (std::min({v0.y, v1.y, v2.y}) > boxHalf.y || std::max({v0.y, v1.y, v2.y}) < -boxHalf.y) return false;
    if (std::min({v0.z, v1.z, v2.z}) > boxHalf.z || std::max({v0.z, v1.z, v2.z}) < -boxHalf.z) return false;

    const bromath::Vec3 normal = bromath::vcross(e0, e1);
    return planeBoxOverlap(normal, v0, boxHalf);
}

struct WalkRayHit {
    bool hit = false;
    float distance = 0.0f;
    float normalY = 0.0f;
    bool twoSided = false;
    bromath::Vec3 point{};
    std::string nodeName;
};

WalkRayHit raycastWalkEntries(const std::vector<StructureMeshEntry>& entries,
                            const bromath::Vec3& origin,
                            const bromath::Vec3& dir,
                            float maxDist) {
    WalkRayHit best;
    best.distance = maxDist;
    const bromath::Vec3 dN = bromath::vnorm(dir);
    if (bromath::vlen2(dN) < 1e-12f) return best;

    const bromath::Vec3 endPt = origin + dN * maxDist;
    bromath::AABB3 rayBox = bromath::aempty3();
    rayBox = bromath::aexpand(rayBox, origin);
    rayBox = bromath::aexpand(rayBox, endPt);

    for (const auto& e : entries) {
        if (e.walkIgnored || !e.mesh || !e.bvh || e.bvh->empty()) continue;
        if (!bromath::aintersects(e.worldBox, rayBox)) continue;
        const StructurePickFrame f = makeStructurePickFrame(e.world);
        if (!f.ok) continue;
        const bromath::Vec3 lo = f.pointToLocal(origin);
        const bromath::Vec3 ld = f.dirToLocal(dN);
        const float ldLen = bromath::vlen(ld);
        if (ldLen < 1e-12f) continue;
        const bromath::Vec3 ldN = ld * (1.0f / ldLen);
        const float localMax = best.distance * ldLen;

        float tNear = 0.0f;
        if (!structureSlabHit(e.bvh->bounds(), lo, ldN, tNear) || tNear > localMax) continue;

        const float op[3] = {lo.x, lo.y, lo.z};
        const float dp[3] = {ldN.x, ldN.y, ldN.z};
        const bromesh::RayHit hit = e.bvh->raycast(*e.mesh, op, dp, localMax);
        if (!hit.hit) continue;

        const bromath::Vec3 worldHit = f.pointToWorld({hit.position[0], hit.position[1], hit.position[2]});
        const bromath::Vec3 worldNrm = bromath::vnorm(f.normalToWorld({hit.normal[0], hit.normal[1], hit.normal[2]}));
        const float worldDist = bromath::vlen(worldHit - origin);
        if (worldDist <= best.distance) {
            best.hit = true;
            best.distance = worldDist;
            best.point = worldHit;
            best.normalY = worldNrm.y * e.detSign;
            best.twoSided = e.twoSided;
            best.nodeName = e.name;
        }
    }
    return best;
}

bool boxOverlapsObstacle(const std::vector<StructureMeshEntry>& entries,
                         const bromath::AABB3& worldBox,
                         std::string& outHitNode) {
    const bromath::Vec3 center = bromath::acenter(worldBox);
    const bromath::Vec3 half = bromath::aextent(worldBox) * 0.5f;
    for (const auto& e : entries) {
        if (e.walkIgnored || !e.mesh || !e.bvh || e.bvh->empty()) continue;
        if (!bromath::aintersects(e.worldBox, worldBox)) continue;

        const bromath::AABB3 localBox = worldAabb(worldBox, e.invWorld);
        const float lMin[3] = {localBox.min.x, localBox.min.y, localBox.min.z};
        const float lMax[3] = {localBox.max.x, localBox.max.y, localBox.max.z};
        const auto& md = *e.mesh;
        const size_t vc = md.vertexCount();
        const size_t tc = md.triangleCount();
        const bool clear = e.bvh->queryBox(lMin, lMax, [&](uint32_t triIdx) -> bool {
            if (triIdx >= tc) return true;
            const uint32_t i0 = md.indices[triIdx * 3 + 0];
            const uint32_t i1 = md.indices[triIdx * 3 + 1];
            const uint32_t i2 = md.indices[triIdx * 3 + 2];
            if (i0 >= vc || i1 >= vc || i2 >= vc) return true;
            const bromath::Vec3 w0 = mulPt(e.world, {md.positions[i0 * 3], md.positions[i0 * 3 + 1], md.positions[i0 * 3 + 2]});
            const bromath::Vec3 w1 = mulPt(e.world, {md.positions[i1 * 3], md.positions[i1 * 3 + 1], md.positions[i1 * 3 + 2]});
            const bromath::Vec3 w2 = mulPt(e.world, {md.positions[i2 * 3], md.positions[i2 * 3 + 1], md.positions[i2 * 3 + 2]});
            if (triBoxOverlap(center, half, w0, w1, w2)) {
                outHitNode = e.name;
                return false;
            }
            return true;
        });
        if (!clear) return true;
    }
    return false;
}

void pushWalkIssue(EyeStructureResult& res,
                   const std::string& kind,
                   const std::string& nodeA,
                   const std::string& nodeB,
                   double value,
                   const bromath::Vec3& pt,
                   double radius,
                   const std::string& msg) {
    EyeStructureIssue iss;
    iss.kind = kind;
    iss.severity = "error";
    iss.nodeA = nodeA;
    iss.nodeB = nodeB;
    iss.value = value;
    iss.point[0] = pt.x;
    iss.point[1] = pt.y;
    iss.point[2] = pt.z;
    iss.radius = std::max(0.25, radius);
    iss.message = msg;
    res.issues.push_back(std::move(iss));
    ++res.interiorWalk.walkIssueCount;
}

}  // namespace

void analyzeInteriorWalk(const std::vector<StructureMeshEntry>& entries,
                         const std::vector<ResolvedFloor>& resolvedFloors,
                         const EyeStructureOptions& opts,
                         EyeStructureResult& res) {
    const auto& wk = opts.interiorWalk;
    const bool active = wk.enabled || !wk.doorways.empty() || !wk.corridors.empty() || !wk.stairs.empty();
    if (!active) return;

    res.interiorWalk.checked = true;
    const double agentR = std::clamp(wk.agentRadius, 0.10, 2.0);
    const double agentH = std::clamp(wk.agentHeight, 0.50, 4.0);
    const double stepUp = std::clamp(wk.stepUp, 0.05, 1.5);
    const double minHead = std::clamp(wk.minHeadroom, agentH, 6.0);
    const double maxGap = std::clamp(wk.maxFloorGap, 0.02, 2.0);
    const double stepDist = std::clamp(wk.sampleStep, 0.05, 1.0);
    const float torsoHalfXZ = static_cast<float>(std::max(0.08, agentR - 0.02));

    // 1. Doorways
    for (size_t di = 0; di < wk.doorways.size(); ++di) {
        const auto& d = wk.doorways[di];
        ++res.interiorWalk.doorwaysChecked;
        const std::string dName = d.name.empty() ? ("doorway#" + std::to_string(di)) : d.name;
        const bromath::Vec3 pos{static_cast<float>(d.position[0]), static_cast<float>(d.position[1]), static_cast<float>(d.position[2])};
        if (d.width > 0.0 && d.width < 2.0 * agentR) {
            ++res.interiorWalk.doorwaysBlocked;
            char buf[160];
            std::snprintf(buf, sizeof(buf), "%s width %.2fm < required %.2fm (2*agentRadius)", dName.c_str(), d.width, 2.0 * agentR);
            pushWalkIssue(res, "doorway-narrow", dName, "", d.width, pos, agentR, buf);
            continue;
        }
        const double reqH = std::min(d.height > 0.0 ? d.height : minHead, minHead);
        if (d.height > 0.0 && d.height < agentH) {
            ++res.interiorWalk.doorwaysBlocked;
            ++res.interiorWalk.headroomIssues;
            char buf[160];
            std::snprintf(buf, sizeof(buf), "%s height %.2fm < agentHeight %.2fm", dName.c_str(), d.height, agentH);
            pushWalkIssue(res, "low-headroom", dName, "", d.height, pos, agentR, buf);
            continue;
        }

        bromath::Vec3 nrm{static_cast<float>(d.normal[0]), 0.0f, static_cast<float>(d.normal[2])};
        if (bromath::vlen2(nrm) < 1e-8f) nrm = {0.0f, 0.0f, 1.0f};
        else nrm = bromath::vnorm(nrm);

        const double depth = std::clamp(d.depth > 0.0 ? d.depth : 0.8, 0.2, 4.0);
        const int samples = std::clamp(static_cast<int>(std::ceil(depth / stepDist)) + 1, 3, 33);
        bool blocked = false;
        bool headBad = false;
        bool prevFloorValid = false;
        double prevFloorY = d.position[1];

        for (int s = 0; s < samples && !blocked; ++s) {
            const double t = (samples == 1) ? 0.0 : (-0.5 + static_cast<double>(s) / (samples - 1)) * depth;
            const bromath::Vec3 pt = pos + nrm * static_cast<float>(t);
            const bromath::Vec3 downOrg{pt.x, static_cast<float>(prevFloorY + stepUp + 0.25), pt.z};
            const auto floorHit = raycastWalkEntries(entries, downOrg, {0.0f, -1.0f, 0.0f},
                                                     static_cast<float>(stepUp + 0.25 + maxGap + 0.6));
            if (!floorHit.hit) {
                blocked = true;
                ++res.interiorWalk.doorwaysBlocked;
                pushWalkIssue(res, "doorway-blocked", dName, "", 0.0, pt, agentR,
                              dName + " missing walkable floor at threshold");
                break;
            }
            const double fy = floorHit.point.y;
            if ((prevFloorValid && std::abs(fy - prevFloorY) > stepUp) ||
                (!prevFloorValid && std::abs(fy - d.position[1]) > stepUp + maxGap)) {
                blocked = true;
                ++res.interiorWalk.doorwaysBlocked;
                char buf[160];
                std::snprintf(buf, sizeof(buf), "%s floor step %.2fm > stepUp %.2fm", dName.c_str(), std::abs(fy - prevFloorY), stepUp);
                pushWalkIssue(res, "doorway-blocked", dName, floorHit.nodeName, std::abs(fy - prevFloorY), pt, agentR, buf);
                break;
            }
            prevFloorValid = true;
            prevFloorY = fy;

            const bromath::AABB3 bodyBox{
                {pt.x - torsoHalfXZ, static_cast<float>(fy + stepUp + 0.04), pt.z - torsoHalfXZ},
                {pt.x + torsoHalfXZ, static_cast<float>(fy + agentH),        pt.z + torsoHalfXZ},
            };
            std::string hitNode;
            if (boxOverlapsObstacle(entries, bodyBox, hitNode)) {
                blocked = true;
                ++res.interiorWalk.doorwaysBlocked;
                pushWalkIssue(res, "doorway-blocked", dName, hitNode, agentR, pt, agentR,
                              dName + " obstructed by " + hitNode);
                break;
            }

            if (!headBad) {
                const bromath::Vec3 upOrg{pt.x, static_cast<float>(fy + 0.15), pt.z};
                const auto ceilHit = raycastWalkEntries(entries, upOrg, {0.0f, 1.0f, 0.0f}, static_cast<float>(reqH - 0.15));
                if (ceilHit.hit) {
                    headBad = true;
                    ++res.interiorWalk.headroomIssues;
                    ++res.interiorWalk.doorwaysBlocked;
                    const double clearH = ceilHit.distance + 0.15;
                    char buf[160];
                    std::snprintf(buf, sizeof(buf), "%s headroom %.2fm < %.2fm (hit %s)", dName.c_str(), clearH, reqH, ceilHit.nodeName.c_str());
                    pushWalkIssue(res, "low-headroom", dName, ceilHit.nodeName, clearH, ceilHit.point, agentR, buf);
                    blocked = true;
                }
            }
        }
    }

    // 2. Corridors
    for (size_t ci = 0; ci < wk.corridors.size(); ++ci) {
        const auto& c = wk.corridors[ci];
        ++res.interiorWalk.corridorsChecked;
        const std::string cName = c.name.empty() ? ("corridor#" + std::to_string(ci)) : c.name;
        const bromath::Vec3 p0{static_cast<float>(c.start[0]), static_cast<float>(c.start[1]), static_cast<float>(c.start[2])};
        const bromath::Vec3 p1{static_cast<float>(c.end[0]),   static_cast<float>(c.end[1]),   static_cast<float>(c.end[2])};
        if (c.width > 0.0 && c.width < 2.0 * agentR) {
            ++res.interiorWalk.corridorsBlocked;
            char buf[160];
            std::snprintf(buf, sizeof(buf), "%s width %.2fm < %.2fm", cName.c_str(), c.width, 2.0 * agentR);
            pushWalkIssue(res, "corridor-narrow", cName, "", c.width, (p0 + p1) * 0.5f, agentR, buf);
            continue;
        }
        const double reqH = c.height > 0.0 ? std::max(agentH, c.height) : minHead;
        const double len = bromath::vlen(p1 - p0);
        const int samples = std::clamp(static_cast<int>(std::ceil(len / stepDist)) + 1, 2, 128);
        bool blocked = false;
        double prevFloorY = c.start[1];
        for (int s = 0; s < samples && !blocked; ++s) {
            const float f = (samples == 1) ? 0.0f : static_cast<float>(s) / (samples - 1);
            const bromath::Vec3 pt = p0 + (p1 - p0) * f;
            const auto floorHit = raycastWalkEntries(entries, {pt.x, static_cast<float>(prevFloorY + stepUp + 0.25), pt.z},
                                                     {0.0f, -1.0f, 0.0f}, static_cast<float>(stepUp + 0.25 + maxGap + 0.6));
            if (!floorHit.hit || std::abs(static_cast<double>(floorHit.point.y) - prevFloorY) > stepUp) {
                blocked = true;
                ++res.interiorWalk.corridorsBlocked;
                pushWalkIssue(res, "corridor-blocked", cName, floorHit.nodeName, 0.0, pt, agentR,
                              cName + " lacks continuous walkable floor");
                break;
            }
            prevFloorY = floorHit.point.y;
            const bromath::AABB3 bodyBox{
                {pt.x - torsoHalfXZ, static_cast<float>(prevFloorY + stepUp + 0.04), pt.z - torsoHalfXZ},
                {pt.x + torsoHalfXZ, static_cast<float>(prevFloorY + agentH),        pt.z + torsoHalfXZ},
            };
            std::string hitNode;
            if (boxOverlapsObstacle(entries, bodyBox, hitNode)) {
                blocked = true;
                ++res.interiorWalk.corridorsBlocked;
                pushWalkIssue(res, "corridor-blocked", cName, hitNode, agentR, pt, agentR,
                              cName + " obstructed by " + hitNode);
                break;
            }
            const auto ceilHit = raycastWalkEntries(entries, {pt.x, static_cast<float>(prevFloorY + 0.15), pt.z},
                                                    {0.0f, 1.0f, 0.0f}, static_cast<float>(reqH - 0.15));
            if (ceilHit.hit) {
                blocked = true;
                ++res.interiorWalk.headroomIssues;
                ++res.interiorWalk.corridorsBlocked;
                const double clearH = ceilHit.distance + 0.15;
                char buf[160];
                std::snprintf(buf, sizeof(buf), "%s headroom %.2fm < %.2fm (hit %s)", cName.c_str(), clearH, reqH, ceilHit.nodeName.c_str());
                pushWalkIssue(res, "low-headroom", cName, ceilHit.nodeName, clearH, ceilHit.point, agentR, buf);
            }
        }
    }

    // 3. Stairs
    for (size_t si = 0; si < wk.stairs.size(); ++si) {
        const auto& st = wk.stairs[si];
        ++res.interiorWalk.stairsChecked;
        const std::string sName = st.name.empty() ? ("stair#" + std::to_string(si)) : st.name;
        const bromath::Vec3 p0{static_cast<float>(st.start[0]), static_cast<float>(st.start[1]), static_cast<float>(st.start[2])};
        const bromath::Vec3 p1{static_cast<float>(st.end[0]),   static_cast<float>(st.end[1]),   static_cast<float>(st.end[2])};
        if (st.width > 0.0 && st.width < 2.0 * agentR) {
            ++res.interiorWalk.stairsBroken;
            char buf[160];
            std::snprintf(buf, sizeof(buf), "%s width %.2fm < %.2fm", sName.c_str(), st.width, 2.0 * agentR);
            pushWalkIssue(res, "stair-narrow", sName, "", st.width, (p0 + p1) * 0.5f, agentR, buf);
            continue;
        }
        const double reqH = st.headroom > 0.0 ? std::max(agentH, st.headroom) : minHead;
        const double len = bromath::vlen(p1 - p0);
        const double horizLen = std::hypot(static_cast<double>(p1.x - p0.x), static_cast<double>(p1.z - p0.z));
        const double fineStep = std::min(stepDist, 0.12);
        const int samples = std::clamp(static_cast<int>(std::ceil(len / fineStep)) + 1, 4, 256);
        bool broken = false;
        bool prevValid = false;
        double prevY = st.start[1];
        const float stairHalfXZ = static_cast<float>(std::max(0.08, std::min(agentR - 0.03, 0.25)));
        const double slopeY = std::abs(st.end[1] - st.start[1]) / std::max(0.25, horizLen);
        const double slopeGain = slopeY * stairHalfXZ;
        const double downTol = std::max(stepUp, 0.35);

        struct StairSample {
            bromath::Vec3 pt;
            double hy = 0.0;
        };
        std::vector<StairSample> stairSamples;
        stairSamples.reserve(static_cast<size_t>(samples));

        // Phase A: verify continuous treads, riser stepUp bounds, and overhead headroom before torso obstruction checks
        for (int s = 0; s < samples && !broken; ++s) {
            const float f = (samples == 1) ? 0.0f : static_cast<float>(s) / (samples - 1);
            const bromath::Vec3 pt = p0 + (p1 - p0) * f;
            const double expY = st.start[1] + (st.end[1] - st.start[1]) * f;
            const float maxDown = static_cast<float>(stepUp + 0.30 + downTol);
            const auto treadHit = raycastWalkEntries(entries, {pt.x, static_cast<float>(expY + stepUp + 0.30), pt.z},
                                                     {0.0f, -1.0f, 0.0f}, maxDown);
            if (!treadHit.hit || (expY - static_cast<double>(treadHit.point.y)) > downTol) {
                broken = true;
                ++res.interiorWalk.stairsBroken;
                pushWalkIssue(res, "stair-gap", sName, "", expY, pt, agentR, sName + " has missing tread/landing");
                break;
            }
            const double hy = treadHit.point.y;
            if (prevValid && std::abs(hy - prevY) > stepUp + 0.02) {
                broken = true;
                ++res.interiorWalk.stairsBroken;
                char buf[160];
                std::snprintf(buf, sizeof(buf), "%s riser jump %.2fm > stepUp %.2fm", sName.c_str(), std::abs(hy - prevY), stepUp);
                pushWalkIssue(res, "stair-step-too-high", sName, treadHit.nodeName, std::abs(hy - prevY), treadHit.point, agentR, buf);
                break;
            }
            prevValid = true;
            prevY = hy;

            const auto ceilHit = raycastWalkEntries(entries, {pt.x, static_cast<float>(hy + 0.15), pt.z},
                                                    {0.0f, 1.0f, 0.0f}, static_cast<float>(reqH - 0.15));
            if (ceilHit.hit && (ceilHit.twoSided || ceilHit.normalY < 0.1f)) {
                broken = true;
                ++res.interiorWalk.headroomIssues;
                ++res.interiorWalk.stairsBroken;
                const double clearH = ceilHit.distance + 0.15;
                char buf[160];
                std::snprintf(buf, sizeof(buf), "%s headroom %.2fm < %.2fm (hit %s)", sName.c_str(), clearH, reqH, ceilHit.nodeName.c_str());
                pushWalkIssue(res, "stair-low-headroom", sName, ceilHit.nodeName, clearH, ceilHit.point, agentR, buf);
                break;
            }

            stairSamples.push_back({pt, hy});
        }

        // Phase B: verify torso clearance along the stair slope
        for (const auto& ss : stairSamples) {
            if (broken) break;
            const float bodyMinY = static_cast<float>(ss.hy + stepUp + 0.04 + slopeGain);
            const float bodyMaxY = static_cast<float>(std::min(ss.hy + agentH, ss.hy + reqH - 0.02));
            if (bodyMaxY <= bodyMinY + 0.02f) continue;
            const bromath::AABB3 bodyBox{
                {ss.pt.x - stairHalfXZ, bodyMinY, ss.pt.z - stairHalfXZ},
                {ss.pt.x + stairHalfXZ, bodyMaxY, ss.pt.z + stairHalfXZ},
            };
            std::string hitNode;
            if (boxOverlapsObstacle(entries, bodyBox, hitNode)) {
                broken = true;
                ++res.interiorWalk.stairsBroken;
                pushWalkIssue(res, "stair-blocked", sName, hitNode, agentR, ss.pt, agentR,
                              sName + " obstructed by " + hitNode);
                break;
            }
        }
    }

    // 4. Floor-to-floor vertical slab clearance
    for (size_t k = 0; k + 1 < resolvedFloors.size(); ++k) {
        if (!std::isfinite(resolvedFloors[k + 1].minSlabY)) continue;
        const double clearH = resolvedFloors[k + 1].minSlabY - resolvedFloors[k].y;
        if (clearH > 0.05 && clearH < minHead) {
            ++res.interiorWalk.headroomIssues;
            char buf[192];
            std::snprintf(buf, sizeof(buf),
                          "floor-to-floor clearance between %s and %s is %.2fm < minHeadroom %.2fm",
                          resolvedFloors[k].name.c_str(), resolvedFloors[k + 1].name.c_str(), clearH, minHead);
            pushWalkIssue(res, "low-headroom", resolvedFloors[k].name, resolvedFloors[k + 1].name,
                          clearH, {0.0f, static_cast<float>(resolvedFloors[k].y + 0.5 * clearH), 0.0f}, minHead, buf);
        }
    }
}

}  // namespace bro::eye
