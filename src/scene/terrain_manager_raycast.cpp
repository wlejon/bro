#include "scene/terrain_manager.h"
#include "scene/mesh_node.h"

#include <cmath>

namespace bro::scene {

using bromath::Vec3;
using bromath::Quat;

// -------------------------------------------------------------------------
// Raycast (LOD 0 only)
// -------------------------------------------------------------------------

TerrainHit TerrainManager::raycast(const Vec3& origin, const Vec3& dir, float maxDist) const {
    TerrainHit result;

    Vec3 ndir = bromath::vnorm(dir);
    if (bromath::vlen2(ndir) < 1e-12f) return result;

    float closestDist = (maxDist > 0.0f) ? maxDist : 1e30f;
    const MeshNode* closestNode = nullptr;
    Vec3 closestWorldPos;
    Vec3 closestWorldNormal;

    for (auto& [coord, entry] : chunks_) {
        // Only raycast against LOD 0 (detail level)
        if (coord.lod != 0) continue;
        if (!entry.meshNode || !entry.meshNode->visible()) continue;

        const auto& md = entry.meshNode->mesh();
        if (md.positions.empty() || md.indices.empty()) continue;

        const Vec3& nodePos = entry.meshNode->position();
        const Quat& nodeRot = entry.meshNode->rotation();
        const Vec3& nodeScl = entry.meshNode->scale();

        Vec3 localOrigin = origin - nodePos;
        localOrigin = bromath::qrotate(bromath::qconjugate(nodeRot), localOrigin);
        if (nodeScl.x != 0.0f) localOrigin.x /= nodeScl.x;
        if (nodeScl.y != 0.0f) localOrigin.y /= nodeScl.y;
        if (nodeScl.z != 0.0f) localOrigin.z /= nodeScl.z;

        Vec3 localDir = bromath::qrotate(bromath::qconjugate(nodeRot), ndir);
        if (nodeScl.x != 0.0f) localDir.x /= nodeScl.x;
        if (nodeScl.y != 0.0f) localDir.y /= nodeScl.y;
        if (nodeScl.z != 0.0f) localDir.z /= nodeScl.z;

        float localDirLen = bromath::vlen(localDir);
        if (localDirLen < 1e-12f) continue;
        Vec3 localDirN = localDir * (1.0f / localDirLen);

        float scale = nodeScl.x != 0.0f ? nodeScl.x : 1.0f;
        float localMaxDist = closestDist / scale;

        const bromath::AABB3& lb = entry.meshNode->localBounds();
        float bmin[3] = { lb.min.x, lb.min.y, lb.min.z };
        float bmax[3] = { lb.max.x, lb.max.y, lb.max.z };
        float invD[3];
        for (int a = 0; a < 3; ++a) {
            float dv = (&localDirN.x)[a];
            invD[a] = (std::fabs(dv) > 1e-30f) ? 1.0f / dv
                                                : (dv >= 0.0f ? 1e30f : -1e30f);
        }
        float o[3] = { localOrigin.x, localOrigin.y, localOrigin.z };
        float tmin = -1e30f, tmax = 1e30f;
        for (int a = 0; a < 3; ++a) {
            float t1 = (bmin[a] - o[a]) * invD[a];
            float t2 = (bmax[a] - o[a]) * invD[a];
            float lo = t1 < t2 ? t1 : t2;
            float hi = t1 < t2 ? t2 : t1;
            if (lo > tmin) tmin = lo;
            if (hi < tmax) tmax = hi;
        }
        if (tmax < 0.0f || tmin > tmax || tmin > localMaxDist) continue;

        float rayO[3] = { localOrigin.x, localOrigin.y, localOrigin.z };
        float rayD[3] = { localDirN.x, localDirN.y, localDirN.z };
        bromesh::RayHit hit = entry.meshNode->bvh().raycast(md, rayO, rayD, localMaxDist);
        if (!hit.hit) continue;

        Vec3 localHit{hit.position[0], hit.position[1], hit.position[2]};
        localHit.x *= nodeScl.x;
        localHit.y *= nodeScl.y;
        localHit.z *= nodeScl.z;
        Vec3 worldHit = bromath::qrotate(nodeRot, localHit) + nodePos;

        float worldDist = bromath::vlen(worldHit - origin);
        if (worldDist >= closestDist) continue;

        closestDist = worldDist;
        closestNode = entry.meshNode;
        closestWorldPos = worldHit;

        Vec3 localNormal{hit.normal[0], hit.normal[1], hit.normal[2]};
        closestWorldNormal = bromath::vnorm(bromath::qrotate(nodeRot, localNormal));
    }

    if (!closestNode) return result;

    result.hit = true;
    result.worldPos[0] = closestWorldPos.x;
    result.worldPos[1] = closestWorldPos.y;
    result.worldPos[2] = closestWorldPos.z;
    result.normal[0] = closestWorldNormal.x;
    result.normal[1] = closestWorldNormal.y;
    result.normal[2] = closestWorldNormal.z;
    result.distance = closestDist;

    auto it = nodeToChunk_.find(closestNode);
    if (it != nodeToChunk_.end()) {
        result.chunk = it->second;
    }

    float nudgeX = closestWorldPos.x - closestWorldNormal.x * 0.5f;
    float nudgeY = closestWorldPos.y - closestWorldNormal.y * 0.5f;
    float nudgeZ = closestWorldPos.z - closestWorldNormal.z * 0.5f;

    ChunkCoord hitChunk;
    int lx, ly, lz;
    worldToLocal(nudgeX, nudgeY, nudgeZ, hitChunk, lx, ly, lz);
    result.chunk = hitChunk;
    result.localX = lx;
    result.localY = ly;
    result.localZ = lz;

    auto chunkIt = chunks_.find(hitChunk);
    if (chunkIt != chunks_.end()) {
        int gridW = config_.chunkSizeX + 1;
        int gridH = config_.chunkSizeZ + 1;
        if (lx >= 0 && lx < gridW && lz >= 0 && lz < gridH) {
            float h = chunkIt->second.heightmap[lz * gridW + lx];
            float seaF = static_cast<float>(config_.seaLevel);
            if (h <= seaF) result.material = 5;
            else if (h <= seaF + config_.heightAmplitude * 0.6f) result.material = 1;
            else if (h <= seaF + config_.heightAmplitude * 0.85f) result.material = 2;
            else result.material = 3;
        }
    }

    return result;
}

} // namespace bro::scene
