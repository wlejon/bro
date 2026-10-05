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

// A node type that puts something in the frame (the layer composites while
// one is shown, even when the camera culls all of it, so the sky stays).
bool drawsContent(const SceneFrame& frame, const SceneNode& node) {
    switch (node.type()) {
    case SceneNode::Type::Mesh:
    case SceneNode::Type::InstancedMesh:
    case SceneNode::Type::GaussianSplat:
    case SceneNode::Type::Particles3D:
    case SceneNode::Type::Decal:
        return true;
    case SceneNode::Type::Light:
        return frame.renderer.showLightIcons() || node.hasWorldAnchor();
    default:
        return node.hasWorldAnchor();
    }
}

}  // namespace

void buildDrawLists(SceneFrame& frame) {
    SceneDrawLists& lists = frame.lists;
    SceneMeshDrawer& drawer = frame.gpu.meshes;
    CullStats& stats = frame.stats;

    // A hidden node hides its subtree; a node off the root is not drawn.
    auto walk = [&](auto& self, SceneNode* n) -> void {
        if (!n || !n->renderVisible()) return;
        lists.nodes.push_back(n);
        for (SceneNode* child : n->children()) self(self, child);
    };
    walk(walk, frame.graph.root());

    for (SceneNode* node : lists.nodes) {
        if (drawsContent(frame, *node)) frame.drewContent = true;

        const bool culled = frame.renderer.cameraCulled(node);
        bool overlay = false;
        MeshDraw draw;
        if (node->type() == SceneNode::Type::Mesh) {
            auto* mesh = static_cast<MeshNode*>(node);
            if (culled) {
                stats.meshCulled++;
            } else {
                stats.meshDrawn++;
            }
            if (!drawer.prepare(frame, *mesh, draw)) continue;
            overlay = mesh->effectiveUnlit();
        } else if (node->type() == SceneNode::Type::InstancedMesh) {
            auto* inst = static_cast<InstancedMeshNode*>(node);
            if (culled) {
                stats.instancedCulled++;
            } else {
                stats.instancedDrawn++;
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
}

}  // namespace bro::scene::vk
