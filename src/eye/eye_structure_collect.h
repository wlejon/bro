#pragma once

#include "eye/eye_structure.h"

#include <bromath/aabb.h>
#include <bromath/mat.h>
#include <bromath/vec.h>
#include <bromesh/analysis/bvh.h>
#include <bromesh/mesh_data.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace bro::scene {
class SceneGraph;
class SceneNode;
class InstancedMeshNode;
}

namespace bro::eye {

struct StructureMeshEntry {
    const scene::SceneNode* sceneNode = nullptr;
    const scene::InstancedMeshNode* instSource = nullptr;
    const bromesh::MeshData* mesh = nullptr;
    const bromesh::MeshBVH* bvh = nullptr;
    bool instanced = false;
    size_t instanceIndex = 0;
    bool twoSided = false;
    float alpha = 1.0f;
    std::string baseName;
    std::string name;
    bromath::Mat4 world{};
    bromath::Mat4 invWorld{};
    bromath::AABB3 worldBox{};
    float detSign = 1.0f;
    bool ignored = false;
    bool walkIgnored = false;
    bool groundNamed = false;
    bool grounded = false;
    bool isFloorAnchor = false;
    int floorIndex = -1;
};

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

inline bromath::AABB3 worldAabb(const bromath::AABB3& b, const bromath::Mat4& m) {
    bromath::AABB3 out = bromath::aempty3();
    for (int i = 0; i < 8; ++i) {
        out = bromath::aexpand(out, mulPt(m, {
            (i & 1) ? b.max.x : b.min.x,
            (i & 2) ? b.max.y : b.min.y,
            (i & 4) ? b.max.z : b.min.z,
        }));
    }
    return out;
}

inline double aabbGap(const bromath::AABB3& a, const bromath::AABB3& b) {
    const double dx = std::max(0.0f, std::max(a.min.x - b.max.x, b.min.x - a.max.x));
    const double dy = std::max(0.0f, std::max(a.min.y - b.max.y, b.min.y - a.max.y));
    const double dz = std::max(0.0f, std::max(a.min.z - b.max.z, b.min.z - a.max.z));
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

inline bool containsAny(const std::string& hay, const std::vector<std::string>& needles) {
    for (const auto& s : needles) {
        if (!s.empty() && hay.find(s) != std::string::npos) return true;
    }
    return false;
}

struct StructurePickFrame {
    bromath::Vec3 bx{}, by{}, bz{}, bt{};
    bromath::Vec3 r0{}, r1{}, r2{};
    bool ok = false;

    bromath::Vec3 pointToWorld(const bromath::Vec3& v) const {
        return bx * v.x + by * v.y + bz * v.z + bt;
    }
    bromath::Vec3 dirToLocal(const bromath::Vec3& v) const {
        return {bromath::vdot(v, r0), bromath::vdot(v, r1), bromath::vdot(v, r2)};
    }
    bromath::Vec3 pointToLocal(const bromath::Vec3& v) const {
        const bromath::Vec3 d = v - bt;
        return {bromath::vdot(d, r0), bromath::vdot(d, r1), bromath::vdot(d, r2)};
    }
    bromath::Vec3 normalToWorld(const bromath::Vec3& n) const {
        return r0 * n.x + r1 * n.y + r2 * n.z;
    }
};

inline StructurePickFrame makeStructurePickFrame(const bromath::Mat4& m) {
    StructurePickFrame f;
    const float* a = m.data;
    f.bx = {a[0], a[1], a[2]};
    f.by = {a[4], a[5], a[6]};
    f.bz = {a[8], a[9], a[10]};
    f.bt = {a[12], a[13], a[14]};
    const float det = bromath::vdot(f.bx, bromath::vcross(f.by, f.bz));
    if (std::fabs(det) < 1e-12f) return f;
    const float inv = 1.0f / det;
    f.r0 = bromath::vcross(f.by, f.bz) * inv;
    f.r1 = bromath::vcross(f.bz, f.bx) * inv;
    f.r2 = bromath::vcross(f.bx, f.by) * inv;
    f.ok = true;
    return f;
}

inline bool structureSlabHit(const bromath::AABB3& b, const bromath::Vec3& o,
                             const bromath::Vec3& d, float& tNear) {
    float tmin = 0.0f, tmax = std::numeric_limits<float>::infinity();
    const float org[3] = {o.x, o.y, o.z};
    const float dir[3] = {d.x, d.y, d.z};
    const float bmin[3] = {b.min.x, b.min.y, b.min.z};
    const float bmax[3] = {b.max.x, b.max.y, b.max.z};
    for (int i = 0; i < 3; ++i) {
        if (std::fabs(dir[i]) < 1e-8f) {
            if (org[i] < bmin[i] || org[i] > bmax[i]) return false;
        } else {
            const float inv = 1.0f / dir[i];
            float t1 = (bmin[i] - org[i]) * inv;
            float t2 = (bmax[i] - org[i]) * inv;
            if (t1 > t2) std::swap(t1, t2);
            tmin = std::max(tmin, t1);
            tmax = std::min(tmax, t2);
            if (tmin > tmax) return false;
        }
    }
    tNear = tmin;
    return true;
}

/// Collect active visible MeshNode and InstancedMeshNode entries with spatial filtering.
std::vector<StructureMeshEntry> collectStructureEntries(scene::SceneGraph& graph,
                                                        const EyeStructureOptions& opts,
                                                        int& outInstancedCount);

}  // namespace bro::eye
