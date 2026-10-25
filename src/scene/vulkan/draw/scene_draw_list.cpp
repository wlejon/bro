#include "scene/vulkan/draw/scene_draw_list.h"

#include "scene/nodes/grass_node.h"
#include "scene/nodes/land_node.h"
#include "scene/nodes/plants_node.h"
#include "scene/nodes/instanced_mesh_node.h"
#include "scene/nodes/mesh_node.h"
#include "scene/scene_graph.h"
#include "scene/vulkan/frame/scene_cpu_stalls.h"
#include "scene/vulkan/frame/scene_frame.h"

#include <algorithm>

namespace bro::scene::vk {

namespace {

// The node's world bounds (the shadow pass and probe faces cull by them),
// their centre (probe selection) and its view depth (translucent sorting),
// all in the place frame (a node keyed on the world converts its own).
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
                {
                    stalls::Part part("grass draws");
                    drawer.prepareGrass(frame, *grass, rings);
                }
                for (MeshDraw& draw : rings) {
                    placeDraw(frame, node, std::nullopt, draw);
                    const auto index = static_cast<uint32_t>(lists.meshes.size());
                    lists.meshes.push_back(draw);
                    if (draw.shadowOnly) continue;   // its near ring's casters
                    stats.meshDrawn++;
                    lists.opaque.push_back(index);
                }
                continue;
            }
            if (LandNode* land = static_cast<MeshNode*>(node)->asLand()) {
                // Land: its rings about the eye, over the whole land (never culled).
                MeshDraw draw;
                bool prepared;
                {
                    stalls::Part part("land draws");
                    prepared = drawer.prepareLand(frame, *land, draw);
                }
                if (!prepared) continue;
                placeDraw(frame, node, frame.view.toPlace(land->bounds()), draw);
                const auto index = static_cast<uint32_t>(lists.meshes.size());
                lists.meshes.push_back(draw);
                stats.meshDrawn++;
                lists.opaque.push_back(index);
                continue;
            }
            if (PlantsNode* plants = static_cast<MeshNode*>(node)->asPlants()) {
                // Plants: their leaves and their wood, cut to the view, and
                // their casters (never in the colour lists).
                std::vector<MeshDraw> parts;
                {
                    stalls::Part part("plants draws");
                    drawer.preparePlants(frame, *plants, parts);
                }
                for (MeshDraw& draw : parts) {
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

    // Near and solid first (MeshDraw::order), so what is behind it fails the
    // early depth test rather than shading and being overdrawn.
    std::stable_sort(lists.opaque.begin(), lists.opaque.end(), [&](uint32_t a, uint32_t b) {
        return lists.meshes[a].order < lists.meshes[b].order;
    });
    std::stable_sort(lists.translucent.begin(), lists.translucent.end(), [&](uint32_t a, uint32_t b) {
        return lists.meshes[a].viewDepth > lists.meshes[b].viewDepth;
    });
}

}  // namespace bro::scene::vk
