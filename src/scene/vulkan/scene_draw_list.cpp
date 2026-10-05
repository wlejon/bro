#include "scene/vulkan/scene_draw_list.h"

#include "scene/instanced_mesh_node.h"
#include "scene/mesh_node.h"
#include "scene/scene_graph.h"
#include "scene/vulkan/scene_frame.h"

#include <algorithm>

namespace bro::scene::vk {

namespace {

// The node's world bounds (the shadow pass and probe faces cull by them),
// their centre (probe selection) and its view depth (translucent sorting).
void placeDraw(const SceneFrame& frame, SceneNode* node, MeshDraw& draw) {
    if (auto bounds = frame.renderer.nodeWorldBounds(node)) {
        draw.hasBounds = true;
        draw.bounds = *bounds;
        draw.center = (bounds->min + bounds->max) * 0.5f;
    } else {
        const bromath::Mat4& w = node->worldMatrix();
        draw.center = bromath::Vec3{w.at(0, 3), w.at(1, 3), w.at(2, 3)};
    }
    draw.viewDepth = bromath::vdot(draw.center - frame.view.eye, frame.view.forward());
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
        bool overlay = false;
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
            overlay = mesh->effectiveUnlit();
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
        placeDraw(frame, node, draw);
        const auto index = static_cast<uint32_t>(lists.meshes.size());
        lists.meshes.push_back(draw);
        if (culled) continue;
        (overlay ? lists.overlay : draw.translucent ? lists.translucent : lists.opaque).push_back(index);
    }

    std::stable_sort(lists.translucent.begin(), lists.translucent.end(), [&](uint32_t a, uint32_t b) {
        return lists.meshes[a].viewDepth > lists.meshes[b].viewDepth;
    });
    // The overlay writes no depth, so its draws layer in creation order (the
    // GL renderer's tree walk), not the node map's.
    std::sort(lists.overlay.begin(), lists.overlay.end(), [&](uint32_t a, uint32_t b) {
        return lists.meshes[a].nodeId < lists.meshes[b].nodeId;
    });
}

}  // namespace bro::scene::vk
