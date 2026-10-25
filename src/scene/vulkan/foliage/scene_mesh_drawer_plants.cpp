// SceneMeshDrawer's plants path: a PlantsNode becomes two draws a view, its
// leaves and its wood, each an instance a chunk of one plant's elements
// (shaders/foliage/scene_plants.glsl builds them with the node's world
// module, its PlantShaders), for the camera and for each shadow view (a
// rect of shadow pages the frame draws) its casters go to.
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

#include "scene/nodes/land_node.h"
#include "scene/nodes/plants_node.h"
#include "scene/scene_graph.h"
#include "scene/scene_renderer.h"
#include "scene/vulkan/shadow/scene_shadow_pages.h"
#include "scene/vulkan/frame/scene_frame.h"
#include "scene/vulkan/frame/scene_gpu_resources.h"
#include "scene/vulkan/foliage/scene_plants_gpu_plan.h"
#include "scene/vulkan/frame/scene_targets.h"
#include "scene/vulkan/ground/scene_cover_plan.h"

#include "util/log.h"

#include <cstring>
#include <initializer_list>

namespace bro::scene::vk {

namespace {

constexpr uint32_t kLeafVerts = PP_LEAF_INDICES;
constexpr uint32_t kCasterLeafVerts = PP_CASTER_INDICES;
constexpr uint32_t kTubeVerts = PP_TUBE_VERTS;
constexpr uint32_t kLeavesPerChunk = PP_LEAVES_PER_CHUNK;
constexpr uint32_t kLimbsPerChunk = PP_LIMBS_PER_CHUNK;

// The leaves draw indexed, so the vertex stage runs once a point of an
// element's polygon (PLANT_LEAF_POINTS) a triangle uses, not once a
// triangle's corner: scene_plants.glsl's polygon of 8 triangles for the
// camera (base, a strip up its 4 pairs, tip), a chunk's elements on, and
// its coarse polygon of 4 (points 0, 1 2, 5 6, 9) for a shadow view, whose
// vertex stage never runs for the points it leaves out.
constexpr uint32_t kLeafPoints = PP_LEAF_POINTS;
constexpr uint32_t kLeafIndexSlot = 1;     // their slots among the node's GPU meshes
constexpr uint32_t kCasterIndexSlot = 2;
bromesh::MeshData elementIndices(std::initializer_list<uint32_t> polygon) {
    bromesh::MeshData m;
    for (uint32_t e = 0; e < kLeavesPerChunk; ++e)
        for (uint32_t c : polygon) m.indices.push_back(e * kLeafPoints + c);
    m.positions.assign(3 * kLeafPoints * kLeavesPerChunk, 0.0f);
    return m;
}
const bromesh::MeshData& leafIndices() {
    static const bromesh::MeshData m = elementIndices(
        {0u, 2u, 1u, 1u, 2u, 4u, 1u, 4u, 3u, 3u, 4u, 6u, 3u, 6u, 5u, 5u, 6u, 8u, 5u, 8u, 7u, 7u, 8u, 9u});
    return m;
}
const bromesh::MeshData& casterIndices() {
    static const bromesh::MeshData m = elementIndices({0u, 2u, 1u, 1u, 2u, 6u, 1u, 6u, 5u, 5u, 6u, 9u});
    return m;
}

}  // namespace

void SceneMeshDrawer::recordPlans(SceneFrame& frame) {
    plantPlanner_->record(frame);
    coverPlan_->record(frame);
}

void SceneMeshDrawer::plansSubmitted(uint64_t ticket, render::VulkanQueue& queue) {
    plantPlanner_->submitted(ticket, queue);
    horizon_.submitted(ticket);
    landProcesses_.submitted(ticket);
    landRegions_.submitted(ticket);
}

const PlantShaders* SceneMeshDrawer::plantShaders(const PlantsNode& node) {
    const uint64_t key = node.moduleSourceHash();
    if (!key) return nullptr;
    auto it = plantByModule_.find(key);
    if (it == plantByModule_.end()) {
        // Asked for at the first frame that draws it and compiled off the
        // frame thread (its shaders in parallel, each from the SPIR-V cache
        // when it has been compiled before); the plants draw nothing until
        // it is in. A module that does not compile is logged once and draws
        // nothing.
        auto set = std::make_unique<PlantShaders>();
        set->start(node.moduleSource());
        set->index = static_cast<uint32_t>(plantSets_.size());
        plantSets_.push_back(std::move(set));
        it = plantByModule_.emplace(key, static_cast<int>(plantSets_.size() - 1)).first;
    }
    PlantShaders& set = *plantSets_[static_cast<size_t>(it->second)];
    std::string err;
    const bool wasPending = set.state() == ModuleShaders::State::Pending;
    const ModuleShaders::State st = set.poll(device_, err);
    if (st == ModuleShaders::State::Failed && wasPending)
        LOG_ERROR("Plants node %u: its module did not compile, so it draws no plants:\n%s", node.id(), err.c_str());
    return st == ModuleShaders::State::Ready ? &set : nullptr;
}

void SceneMeshDrawer::preparePlants(SceneFrame& frame, PlantsNode& node, std::vector<MeshDraw>& out) {
    out.clear();
    // On the land they stand on as its processes have it (and nothing
    // placed until they have run), then the cells placed about the
    // camera's eye.
    if (node.land()) {
        SceneNode* n = frame.graph.findById(node.land());
        const LandNode* land = n && n->type() == SceneNode::Type::Mesh ? static_cast<MeshNode*>(n)->asLand() : nullptr;
        if (land) node.setGround(land->groundBase(), !land->groundReady());
    }
    node.follow(frame.world.eye);   // (placement is keyed on the body)
    const PlantShaders* shaders = plantShaders(node);
    // (Nor planned when every plant is past its range: a planet's from orbit.)
    if (!shaders || node.plantCount() == 0 || node.speciesCount() == 0 || node.effectiveUnlit() ||
        node.nearest(frame.world.eye) > node.params().range) {
        node.setStats({});
        return;
    }
    // The records, the runs changed since the last frame copied in.
    bool changed = false;
    const SceneVkBuffer* store = frame.gpu.resources.storage(node.id(), node.store(), changed);
    const GpuMesh* ix = frame.gpu.resources.mesh(node.id(), kLeafIndexSlot, 2, leafIndices());
    const GpuMesh* casterIx = frame.gpu.resources.mesh(node.id(), kCasterIndexSlot, 1, casterIndices());
    if (!store || !ix || !casterIx) return;
    // (Copied on the graphics queue, read by the plan on the compute queue.)
    if (changed) frame.gpu.device.computeReadsGraphics();
    // The shadow views its casters go to: the overlays of the levels where
    // its sway shows, the redrawn pages of the rest (the plan parts the
    // crowns by penumbra).
    std::vector<int> tiles;
    const ShadowPlan& shadows = frame.renderer.shadowPlan();
    if (node.castsShadow() && frame.gpu.shadows.ready())
        for (int v = 0; v < shadows.viewCount && v < PP_MAX_TILES; ++v)
            if (shadows.views[v].plants != ShadowPlants::None) tiles.push_back(v);

    // Planned on the GPU: each view's draws indirect, their counts the plan's.
    std::shared_ptr<plants::GpuPlanner::Node> state = plantPlanner_->node(node);
    if (!plantPlanner_->add(frame, *state, node, *store, tiles, *shaders, landHorizon(frame))) return;
    node.setStatsSource(state);

    uint32_t flags = 0;
    if (node.receivesShadow()) flags |= mesh_flags::kReceivesShadow;
    MeshDraw draw;
    draw.kind = MeshKind::Plants;
    draw.nodeId = node.id();
    draw.moduleSet = shaders->index;
    std::memset(draw.push.model, 0, sizeof(draw.push.model));
    draw.push.model[0] = draw.push.model[5] = draw.push.model[10] = draw.push.model[15] = 1.0f;
    draw.push.baseColor[0] = draw.push.baseColor[1] = draw.push.baseColor[2] = draw.push.baseColor[3] = 1.0f;
    draw.push.pbrParams[3] = static_cast<float>(flags);
    draw.twoSided = true;
    // Near and solid first: the plants are the nearest solid cover (then the
    // grass and herbs, then the ground under them).
    draw.order = MeshDraw::kOrderMid - 2;
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
    // A view's part: the camera's (view 0) or a shadow view's casters
    // (never in the colour lists).
    for (uint32_t v = 0; v <= tiles.size(); ++v)
        for (MeshDraw part : {leaves, wood}) {
            part.indirectOffset = plants::commandOffset(v, part.push.extra[0] > 0.5f);
            if (v > 0) {
                part.shadowOnly = part.castsShadow = true;
                if (part.push.extra[0] < 0.5f) {
                    part.indices = casterIx->indices.buffer;
                    part.indexCount = kLeavesPerChunk * kCasterLeafVerts;
                }
                part.shadowView = tiles[v - 1];
                part.shadowTimerLabel = "shadow:plants";
            }
            out.push_back(part);
        }
    // The camera's far bands' leaves, their outline's detail 0 already: the
    // coarse polygon's points alone.
    MeshDraw far = leaves;
    far.indirectOffset = plants::farLeavesOffset();
    far.indices = casterIx->indices.buffer;
    far.indexCount = kLeavesPerChunk * kCasterLeafVerts;
    out.push_back(far);
}

}  // namespace bro::scene::vk
