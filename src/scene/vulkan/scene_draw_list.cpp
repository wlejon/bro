#include "scene/vulkan/scene_draw_list.h"

#include "scene/instanced_mesh_node.h"
#include "scene/mesh_node.h"
#include "scene/scene_graph.h"
#include "scene/vulkan/scene_frame.h"

#include <algorithm>

namespace bro::scene::vk {

namespace {

float viewDepthOf(const SceneFrame& frame, SceneNode* node) {
    bromath::Vec3 center;
    if (auto bounds = frame.renderer.nodeWorldBounds(node)) {
        center = (bounds->min + bounds->max) * 0.5f;
    } else {
        const bromath::Mat4& w = node->worldMatrix();
        center = bromath::Vec3{w.at(0, 3), w.at(1, 3), w.at(2, 3)};
    }
    return bromath::vdot(center - frame.view.eye, frame.view.forward());
}

}  // namespace

void buildDrawLists(SceneFrame& frame) {
    SceneDrawLists& lists = frame.lists;
    SceneMeshDrawer& drawer = frame.gpu.meshes;
    CullStats& stats = frame.stats;

    for (auto& [id, owned] : frame.graph.nodes()) {
        SceneNode* node = owned.get();
        if (!node->renderVisible()) continue;

        const bool culled = frame.renderer.cameraCulled(node);
        MeshDraw draw;
        if (node->type() == SceneNode::Type::Mesh) {
            auto* mesh = static_cast<MeshNode*>(node);
            if (culled) {
                stats.meshCulled++;
            } else {
                stats.meshDrawn++;
                frame.drewContent = true;
            }
            if (mesh->clipmapRole() && mesh->hasCustomShader()) {
                if (!culled && !mesh->currentMesh().empty()) lists.terrains.push_back(mesh);
                continue;
            }
            if (!drawer.prepare(frame, *mesh, draw)) continue;
        } else if (node->type() == SceneNode::Type::InstancedMesh) {
            auto* inst = static_cast<InstancedMeshNode*>(node);
            if (culled) {
                stats.instancedCulled++;
            } else {
                stats.instancedDrawn++;
                frame.drewContent = true;
            }
            if (!drawer.prepare(frame, *inst, draw)) continue;
        } else {
            continue;
        }

        draw.cameraCulled = culled;
        draw.viewDepth = viewDepthOf(frame, node);
        const auto index = static_cast<uint32_t>(lists.meshes.size());
        lists.meshes.push_back(draw);
        if (culled) continue;
        (draw.translucent ? lists.translucent : lists.opaque).push_back(index);
    }

    std::stable_sort(lists.translucent.begin(), lists.translucent.end(), [&](uint32_t a, uint32_t b) {
        return lists.meshes[a].viewDepth > lists.meshes[b].viewDepth;
    });
}

}  // namespace bro::scene::vk
