#include "scene/vulkan/scene_draw_list.h"

#include "scene/forest_node.h"
#include "scene/grass_node.h"
#include "scene/instanced_mesh_node.h"
#include "scene/mesh_node.h"
#include "scene/scene_graph.h"
#include "scene/vulkan/scene_frame.h"

#include <algorithm>

namespace bro::scene::vk {

namespace {

// The node's world bounds (the shadow pass and probe faces cull by them),
// their centre (probe selection) and its view depth (translucent sorting).
void placeDraw(const SceneFrame& frame, SceneNode* node,
               const std::optional<bromath::AABB3>& bounds, MeshDraw& draw) {
    if (bounds) {
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

    const size_t approxNodes = frame.graph.nodes().size() + 1;
    lists.nodes.reserve(approxNodes);

    // A hidden node hides its subtree; a node off the root is not drawn.
    auto walk = [&](auto& self, SceneNode* n) -> void {
        if (!n || !n->renderVisible()) return;
        lists.nodes.push_back(n);
        for (SceneNode* child : n->children()) self(self, child);
    };
    walk(walk, frame.graph.root());

    lists.meshes.reserve(lists.nodes.size());
    lists.opaque.reserve(lists.nodes.size());
    const bromath::Frustum* frustum = frame.renderer.cullingFrustum();

    for (SceneNode* node : lists.nodes) {
        if (drawsContent(frame, *node)) frame.drewContent = true;

        const SceneNode::Type type = node->type();
        if (type != SceneNode::Type::Mesh && type != SceneNode::Type::InstancedMesh) {
            continue;
        }

        if (type == SceneNode::Type::Mesh) {
            if (GrassNode* grass = static_cast<MeshNode*>(node)->asGrass()) {
                // Grass: a draw per ring, each cut to the cells in view.
                std::vector<MeshDraw> rings;
                drawer.prepareGrass(frame, *grass, rings);
                for (MeshDraw& draw : rings) {
                    placeDraw(frame, node, std::nullopt, draw);
                    stats.meshDrawn++;
                    lists.opaque.push_back(static_cast<uint32_t>(lists.meshes.size()));
                    lists.meshes.push_back(draw);
                }
                continue;
            }
            if (ForestNode* forest = static_cast<MeshNode*>(node)->asForest()) {
                // A forest: a draw per tier and shape for the camera, and
                // the shadow tiles' casters (never in the colour lists).
                std::vector<MeshDraw> draws;
                drawer.prepareForest(frame, *forest, draws);
                for (MeshDraw& draw : draws) {
                    placeDraw(frame, node, std::nullopt, draw);
                    const auto index = static_cast<uint32_t>(lists.meshes.size());
                    lists.meshes.push_back(draw);
                    if (draw.shadowOnly) continue;
                    stats.meshDrawn++;
                    lists.opaque.push_back(index);
                }
                continue;
            }
        }

        const auto bounds = frame.renderer.nodeWorldBounds(node);
        const bool culled = frustum && bounds && !bromath::fintersects(*frustum, *bounds);
        bool overlay = false;
        MeshDraw draw;
        if (type == SceneNode::Type::Mesh) {
            auto* mesh = static_cast<MeshNode*>(node);
            if (culled) {
                stats.meshCulled++;
            } else {
                stats.meshDrawn++;
            }
            if (!drawer.prepare(frame, *mesh, draw)) continue;
            overlay = mesh->effectiveUnlit();
        } else {
            auto* inst = static_cast<InstancedMeshNode*>(node);
            if (culled) {
                stats.instancedCulled++;
            } else {
                stats.instancedDrawn++;
            }
            if (!drawer.prepare(frame, *inst, draw)) continue;
        }

        draw.cameraCulled = culled;
        placeDraw(frame, node, bounds, draw);
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
