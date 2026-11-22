#include "eye/eye_structure_collect.h"

#include "scene/nodes/instanced_mesh_node.h"
#include "scene/nodes/mesh_node.h"
#include "scene/scene_graph.h"

#include <algorithm>
#include <cmath>

namespace bro::eye {
namespace {

bool isGroundName(const std::string& name) {
    return name.find("Floor") != std::string::npos ||
           name.find("Ground") != std::string::npos ||
           name.find("Terrain") != std::string::npos ||
           name.find("Bedplate") != std::string::npos;
}

bool passesSpatialFilter(const bromath::AABB3& box, const EyeStructureOptions& opts) {
    if (opts.hasBounds) {
        const bromath::AABB3 bounds{
            {static_cast<float>(std::min(opts.boundsMin[0], opts.boundsMax[0])),
             static_cast<float>(std::min(opts.boundsMin[1], opts.boundsMax[1])),
             static_cast<float>(std::min(opts.boundsMin[2], opts.boundsMax[2]))},
            {static_cast<float>(std::max(opts.boundsMin[0], opts.boundsMax[0])),
             static_cast<float>(std::max(opts.boundsMin[1], opts.boundsMax[1])),
             static_cast<float>(std::max(opts.boundsMin[2], opts.boundsMax[2]))},
        };
        if (!bromath::aintersects(box, bounds)) return false;
    }
    if (opts.hasNear && opts.nearRadius > 0.0) {
        const double cx = opts.nearCenter[0];
        const double cy = opts.nearCenter[1];
        const double cz = opts.nearCenter[2];
        const double dx = std::max({0.0, static_cast<double>(box.min.x) - cx, cx - static_cast<double>(box.max.x)});
        const double dy = std::max({0.0, static_cast<double>(box.min.y) - cy, cy - static_cast<double>(box.max.y)});
        const double dz = std::max({0.0, static_cast<double>(box.min.z) - cz, cz - static_cast<double>(box.max.z)});
        if (dx * dx + dy * dy + dz * dz > opts.nearRadius * opts.nearRadius) return false;
    }
    return true;
}

void walkVisible(scene::SceneNode* node,
                 const EyeStructureOptions& opts,
                 int maxInst,
                 int& instCount,
                 std::vector<StructureMeshEntry>& out) {
    if (!node || !node->visible()) return;

    if (node->type() == scene::SceneNode::Type::Mesh) {
        auto* mn = static_cast<scene::MeshNode*>(node);
        const auto& md = mn->currentMesh();
        if (!md.positions.empty() && !md.indices.empty()) {
            const bromath::Mat4 world = node->worldMatrix();
            const bromath::AABB3 box = worldAabb(mn->localBounds(), world);
            if (passesSpatialFilter(box, opts)) {
                const float* d = world.data;
                const bromath::Vec3 c0{d[0], d[1], d[2]}, c1{d[4], d[5], d[6]}, c2{d[8], d[9], d[10]};
                const float det = bromath::vdot(c0, bromath::vcross(c1, c2));
                if (std::isfinite(det) && std::fabs(det) >= 1e-12f) {
                    StructureMeshEntry me;
                    me.sceneNode = mn;
                    me.mesh = &md;
                    me.bvh = &mn->bvh();
                    me.instanced = false;
                    me.twoSided = mn->twoSided();
                    me.alpha = mn->color()[3];
                    me.baseName = node->name().empty() ? ("mesh#" + std::to_string(node->id())) : node->name();
                    me.name = me.baseName;
                    me.world = world;
                    me.invWorld = bromath::minverse(world);
                    me.worldBox = box;
                    me.detSign = det < 0.0f ? -1.0f : 1.0f;
                    me.ignored = containsAny(me.name, opts.ignoreFloating);
                    me.walkIgnored = containsAny(me.name, opts.interiorWalk.ignoreNodes);
                    me.groundNamed = isGroundName(me.name);
                    me.grounded = me.groundNamed || (me.worldBox.min.y <= opts.groundY + 0.05);
                    out.push_back(std::move(me));
                }
            }
        }
    } else if (node->type() == scene::SceneNode::Type::InstancedMesh) {
        auto* im = static_cast<scene::InstancedMeshNode*>(node);
        const auto& md = im->mesh();
        if (!im->isScatter() && !im->isTube() && !md.positions.empty() && !md.indices.empty() && im->instanceCount() > 0) {
            const bromath::Mat4 nodeWorld = node->worldMatrix();
            const bromath::AABB3 localBox = im->localBounds();
            const bromesh::MeshBVH* sharedBvh = &im->bvh();
            const std::string baseName = node->name().empty() ? ("inst#" + std::to_string(node->id())) : node->name();
            const bool baseIgnored = containsAny(baseName, opts.ignoreFloating);
            const bool baseWalkIgnored = containsAny(baseName, opts.interiorWalk.ignoreNodes);
            const bool groundNamed = isGroundName(baseName);
            const float alpha = im->color()[3];
            const bool twoSided = im->doubleSided();

            for (size_t i = 0; i < im->instanceCount(); ++i) {
                if (instCount >= maxInst) break;
                float rows[12];
                if (!im->instanceRows(i, rows)) continue;
                bool finiteOk = true;
                for (int k = 0; k < 12; ++k) {
                    if (!std::isfinite(rows[k])) { finiteOk = false; break; }
                }
                if (!finiteOk) continue;

                const bromath::Vec3 rc0{rows[0], rows[4], rows[8]};
                const bromath::Vec3 rc1{rows[1], rows[5], rows[9]};
                const bromath::Vec3 rc2{rows[2], rows[6], rows[10]};
                if (bromath::vlen2(rc0) < 1e-12f || bromath::vlen2(rc1) < 1e-12f || bromath::vlen2(rc2) < 1e-12f) {
                    continue;
                }
                const float localDet = bromath::vdot(rc0, bromath::vcross(rc1, rc2));
                if (!std::isfinite(localDet) || std::fabs(localDet) < 1e-12f) continue;

                bromath::Mat4 instLocal = bromath::midentity();
                instLocal.at(0, 0) = rows[0]; instLocal.at(0, 1) = rows[1]; instLocal.at(0, 2) = rows[2]; instLocal.at(0, 3) = rows[3];
                instLocal.at(1, 0) = rows[4]; instLocal.at(1, 1) = rows[5]; instLocal.at(1, 2) = rows[6]; instLocal.at(1, 3) = rows[7];
                instLocal.at(2, 0) = rows[8]; instLocal.at(2, 1) = rows[9]; instLocal.at(2, 2) = rows[10]; instLocal.at(2, 3) = rows[11];

                const bromath::Mat4 world = bromath::mmul(nodeWorld, instLocal);
                const float* d = world.data;
                const bromath::Vec3 c0{d[0], d[1], d[2]}, c1{d[4], d[5], d[6]}, c2{d[8], d[9], d[10]};
                const float det = bromath::vdot(c0, bromath::vcross(c1, c2));
                if (!std::isfinite(det) || std::fabs(det) < 1e-12f) continue;

                const bromath::AABB3 box = worldAabb(localBox, world);
                if (!passesSpatialFilter(box, opts)) continue;

                StructureMeshEntry me;
                me.sceneNode = im;
                me.instSource = im;
                me.mesh = &md;
                me.bvh = sharedBvh;
                me.instanced = true;
                me.instanceIndex = i;
                me.twoSided = twoSided;
                me.alpha = alpha;
                me.baseName = baseName;
                me.name = baseName + "#" + std::to_string(i);
                me.world = world;
                me.invWorld = bromath::minverse(world);
                me.worldBox = box;
                me.detSign = det < 0.0f ? -1.0f : 1.0f;
                me.ignored = baseIgnored || containsAny(me.name, opts.ignoreFloating);
                me.walkIgnored = baseWalkIgnored || containsAny(me.name, opts.interiorWalk.ignoreNodes);
                me.groundNamed = groundNamed;
                me.grounded = me.groundNamed || (me.worldBox.min.y <= opts.groundY + 0.05);
                out.push_back(std::move(me));
                ++instCount;
            }
        }
    }

    for (scene::SceneNode* ch : node->children()) {
        walkVisible(ch, opts, maxInst, instCount, out);
    }
}

}  // namespace

std::vector<StructureMeshEntry> collectStructureEntries(scene::SceneGraph& graph,
                                                        const EyeStructureOptions& opts,
                                                        int& outInstancedCount) {
    outInstancedCount = 0;
    std::vector<StructureMeshEntry> entries;
    const int maxInst = std::clamp(opts.maxInstances, 1, 65536);
    walkVisible(graph.root(), opts, maxInst, outInstancedCount, entries);
    return entries;
}

}  // namespace bro::eye
