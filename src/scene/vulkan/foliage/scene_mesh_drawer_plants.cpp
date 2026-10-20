// SceneMeshDrawer's plants path: a PlantsNode becomes two draws a view, its
// leaves and its wood, each an instance a chunk of one plant's elements
// (shaders/foliage/scene_plants.glsl builds them), for the camera and for each
// shadow tile its casters go to.
//
// Which plants each view draws and with how many elements is planned on the
// GPU (scene_plants_gpu_plan.cpp, plants_plan.comp) over the cells in view:
// the draws here are indirect, their counts the plan's, and nothing on the
// CPU walks the plants.
//
// A plant's element count: its species' full count while an element (a
// leaf of its length) is at least the node's `pixels` across on screen,
// falling with the square of the distance past that (so an element stays
// about `pixels` across), never under `minElements`. The count is
// continuous: its fraction grows the last element in.

#include "scene/vulkan/draw/scene_mesh_drawer.h"

#include "scene/nodes/plants_node.h"
#include "scene/scene_renderer.h"
#include "scene/vulkan/frame/scene_frame.h"
#include "scene/vulkan/frame/scene_gpu_resources.h"
#include "scene/vulkan/foliage/scene_plants_gpu_plan.h"
#include "scene/vulkan/frame/scene_targets.h"

#include <cstring>

namespace bro::scene::vk {

namespace {

constexpr uint32_t kLeafVerts = 12;
constexpr uint32_t kTubeVerts = PP_TUBE_VERTS;
constexpr uint32_t kLeavesPerChunk = PP_LEAVES_PER_CHUNK;
constexpr uint32_t kLimbsPerChunk = PP_LIMBS_PER_CHUNK;

// The leaves draw indexed, so the vertex stage runs once a point of an
// element's polygon (PLANT_LEAF_POINTS), not once a triangle's corner:
// scene_plants.glsl's plantLeafCorner's 4 triangles, a chunk's elements on.
constexpr uint32_t kLeafPoints = 6;
constexpr uint32_t kLeafIndexSlot = 1;   // its slot among the node's GPU meshes
const bromesh::MeshData& leafIndices() {
    static bromesh::MeshData m;
    if (!m.indices.empty()) return m;
    for (uint32_t e = 0; e < kLeavesPerChunk; ++e)
        for (uint32_t c : {0u, 2u, 1u, 1u, 2u, 4u, 1u, 4u, 3u, 3u, 4u, 5u}) m.indices.push_back(e * kLeafPoints + c);
    m.positions.assign(3 * kLeafPoints * kLeavesPerChunk, 0.0f);
    return m;
}

}  // namespace

void SceneMeshDrawer::recordPlans(SceneFrame& frame) {
    plantPlanner_->record(frame);
}

void SceneMeshDrawer::plansSubmitted(uint64_t ticket, render::VulkanQueue& queue) {
    plantPlanner_->submitted(ticket, queue);
}

void SceneMeshDrawer::preparePlants(SceneFrame& frame, PlantsNode& node, std::vector<MeshDraw>& out) {
    out.clear();
    if (node.plantCount() == 0 || node.speciesCount() == 0 || node.effectiveUnlit()) {
        node.setStats({});
        return;
    }
    // The records, uploaded when they change.
    const std::vector<float>& data = node.gpuData();
    const SceneVkBuffer* store = frame.gpu.resources.storage(node.id(), node.generation(), data.data(),
                                                             data.size() * sizeof(float));
    const GpuMesh* ix = frame.gpu.resources.mesh(node.id(), kLeafIndexSlot, 1, leafIndices());
    if (!store || !ix) return;
    // The shadow tiles its casters go to.
    std::vector<int> tiles;
    const ShadowPlan& shadows = frame.renderer.shadowPlan();
    if (node.castsShadow() && frame.gpu.targets.shadowAtlas.width == static_cast<uint32_t>(shadows.atlasSize))
        for (int slot = 0; slot < shadows.tileCount && slot < PP_MAX_TILES; ++slot)
            if (shadows.tiles[slot].render) tiles.push_back(slot);

    // Planned on the GPU: each view's draws indirect, their counts the plan's.
    std::shared_ptr<plants::GpuPlanner::Node> state = plantPlanner_->node(node);
    if (!plantPlanner_->add(frame, *state, node, *store, tiles)) return;
    node.setStatsSource(state);

    uint32_t flags = 0;
    if (node.receivesShadow()) flags |= mesh_flags::kReceivesShadow;
    MeshDraw draw;
    draw.kind = MeshKind::Plants;
    draw.nodeId = node.id();
    std::memset(draw.push.model, 0, sizeof(draw.push.model));
    draw.push.model[0] = draw.push.model[5] = draw.push.model[10] = draw.push.model[15] = 1.0f;
    draw.push.baseColor[0] = draw.push.baseColor[1] = draw.push.baseColor[2] = draw.push.baseColor[3] = 1.0f;
    draw.push.pbrParams[3] = static_cast<float>(flags);
    draw.twoSided = true;
    // Near and solid first: the plants are the nearest solid cover.
    draw.order = MeshDraw::kOrderMid - 1;
    draw.vertexSet = state->drawSet;
    draw.indirect = state->args.buffer;
    // (extra.z, the wind mask elsewhere, is the mode again for the casters:
    // the shadow pass hands it on as params.x.)
    MeshDraw leaves = draw;
    leaves.push.extra[0] = 0.0f;
    leaves.push.extra[2] = 0.0f;
    leaves.indexCount = kLeavesPerChunk * kLeafVerts;
    leaves.indices = ix->indices.buffer;
    leaves.timerLabel = "plants:leaves";
    MeshDraw wood = draw;
    wood.push.extra[0] = 1.0f;
    wood.push.extra[2] = 1.0f;
    wood.indexCount = kLimbsPerChunk * kTubeVerts;
    wood.timerLabel = "plants:wood";
    // A view's part: the camera's (view 0) or a tile's casters (never in
    // the colour lists).
    for (uint32_t v = 0; v <= tiles.size(); ++v)
        for (MeshDraw part : {leaves, wood}) {
            part.indirectOffset = plants::commandOffset(v, part.push.extra[0] > 0.5f);
            if (v > 0) {
                part.shadowOnly = part.castsShadow = true;
                part.shadowTileMask = 1u << tiles[v - 1];
                part.shadowTimerLabel = "shadow:plants";
            }
            out.push_back(part);
        }
}

}  // namespace bro::scene::vk
