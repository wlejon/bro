// SceneMeshDrawer's forest draws planned on the GPU (scene_forest_gpu_plan.cpp):
// the nodes' parameters, the draws of every group the plan can fill (their
// counts in its indirect commands), and the budget's settle. For a
// perspective view with no forced tier, while the planner is ready; else a
// frame takes the CPU's path (scene_mesh_drawer_forest.cpp), whose
// choices the GPU's are, tree by tree.
//
// What differs from the CPU's frame: every group of a camera's draws goes
// nearest first by the top 16 bits of its trees' distances (the CPU sorts
// the leaves, twigs and clumps in full and its crowns so, the cores, bark
// and blooms not at all); the stats are a frame or two old; a group's draw
// is issued even when the plan leaves it empty (a zero count), and a posed
// leaf group has a second draw for when it is posed inline; and the first
// frames of a large view draw what the entries' and poses' room holds until
// it has grown to the asks read back.

#include "scene/vulkan/scene_mesh_drawer.h"

#include "scene/forest_node.h"
#include "scene/scene_graph.h"
#include "scene/vulkan/scene_defaults.h"
#include "scene/vulkan/scene_forest_cull.h"
#include "scene/vulkan/scene_forest_draw.h"
#include "scene/vulkan/scene_forest_gpu_plan.h"
#include "scene/vulkan/scene_frame.h"
#include "scene/vulkan/scene_gpu_resources.h"
#include "scene/vulkan/scene_targets.h"
#include "scene/vulkan/scene_vk_descriptors.h"
#include "scene/vulkan/scene_vk_device.h"
#include "scene/vulkan/shaders/scene_forest_layout.h"
#include "scene/vulkan/shaders/scene_forest_plan_layout.h"
#include "util/log.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <memory>

namespace bro::scene::vk {

namespace {

using namespace forest;

constexpr uint32_t kMaxPoses = 4u << 20;   // scene_mesh_drawer_forest.cpp's
constexpr VkDeviceSize kPoseBytes = FOREST_POSE_RECORDS * 16;

uint32_t bitsOf(float f) {
    uint32_t u;
    std::memcpy(&u, &f, sizeof(u));
    return u;
}

void putLod(std::vector<uint32_t>& p, uint32_t at, const ForestNode::Lod& l) {
    p[at + FP_LOD_LEAF_PIXELS] = bitsOf(l.leafPixels);
    p[at + FP_LOD_CARD_WIDENING] = bitsOf(l.cardWidening);
    p[at + FP_LOD_CLUMP_FROM] = bitsOf(l.clumpFrom);
    p[at + FP_LOD_CLUMP_TO] = bitsOf(l.clumpTo);
    p[at + FP_LOD_CROWN_FROM] = bitsOf(l.crownFrom);
    p[at + FP_LOD_CROWN_TO] = bitsOf(l.crownTo);
    p[at + FP_LOD_BARK_PIXELS] = bitsOf(l.barkPixels);
    p[at + FP_LOD_BARK_SIDES_PIXELS] = bitsOf(l.barkSidesPixels);
    p[at + FP_LOD_SHADOW_KEEP] = bitsOf(l.shadowKeep);
    p[at + FP_LOD_RANGE] = bitsOf(l.range);
    p[at + FP_LOD_BARK_KEEP_PIXELS] = bitsOf(l.barkKeepPixels);
    p[at + FP_LOD_BARK_FINE_SIDES] = bitsOf(l.barkFineSides);
    p[at + FP_LOD_LEAVES_RANGE] = bitsOf(l.leavesRange);
    p[at + FP_LOD_LEAVES_BAND] = bitsOf(l.leavesBand);
}

void putPlanes(std::vector<uint32_t>& p, uint32_t at, const bromath::Frustum& f) {
    for (int i = 0; i < 6; ++i) {
        const bromath::Plane& q = f.planes[i];
        p[at + 4 * i] = bitsOf(q.normal.x);
        p[at + 4 * i + 1] = bitsOf(q.normal.y);
        p[at + 4 * i + 2] = bitsOf(q.normal.z);
        p[at + 4 * i + 3] = bitsOf(q.d);
    }
}

void putRows(std::vector<uint32_t>& p, uint32_t at, const bromath::Mat4& m) {
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 4; ++c) p[at + 4 * r + c] = bitsOf(m.at(r, c));
}

// A group's command kind and per-instance count (FP_P_DRAWS) from its draw.
void putDraw(std::vector<uint32_t>& p, uint32_t k, bool shadow, const MeshDraw& d) {
    const uint32_t at = FP_P_DRAWS + 2 * (k + (shadow ? FP_GROUPS : 0));
    const uint32_t kind = k == Crowns ? FP_DRAW_CROWNS
                        : d.kind == MeshKind::ForestSprite ? FP_DRAW_INSTANCED : FP_DRAW_INDEXED;
    p[at] = kind | d.instanceCount << 8;
    p[at + 1] = d.indexCount;
}

// The vertices a budget of `millions` allows (fitDetail compares the cost,
// a whole number, with the budget times a million: past its floor).
uint64_t limitOf(float millions) {
    return static_cast<uint64_t>(std::floor(static_cast<double>(millions) * 1e6));
}

// A node's plan this frame.
struct NodePlan {
    ForestNode* node = nullptr;
    size_t index = 0;   // in `out`
    std::shared_ptr<GpuPlanner::Node> state;
    std::vector<uint32_t> params;
    const SceneVkBuffer* store = nullptr;
    std::vector<int> tileSlots;
    ForestNode::Lod lod, shadowLod;
    float reach = 0.0f, nodeScale = 1.0f, pxAtMetre = 0.0f, canopyTop = -1e30f;
    bool cores = false, clusters = false, twigs = false, needles = false, blooms = false;
};

}  // namespace

bool SceneMeshDrawer::prepareForestsGpu(SceneFrame& frame, const std::vector<ForestNode*>& nodes,
                                        std::vector<std::vector<MeshDraw>>& out, ForestDetail& detail) {
    if (!gpuPlanner_|| !gpuPlanner_->ready()) return false;
    const SceneView& view = frame.view;
    if (!view.perspective) return false;
    for (const ForestNode* n : nodes)
        if (n->forceTier() >= 0) return false;
    GpuPlanner& planner = *gpuPlanner_;
    SceneGpuResources& res = frame.gpu.resources;
    const ShadowPlan& shadows = frame.renderer.shadowPlan();
    const float pxAtMetre = std::fabs(view.proj.at(1, 1)) * static_cast<float>(view.height) * 0.5f;

    // Each node's trees, view and meshes; one with nothing to draw has no
    // plan and no stats (as planForest's).
    std::vector<NodePlan> plans;
    std::vector<bool> empty(nodes.size(), false);
    for (size_t i = 0; i < nodes.size(); ++i) {
        ForestNode& node = *nodes[i];
        if (node.instanceCount() == 0 || node.specimenCount() == 0 || node.mesh().empty() || node.effectiveUnlit()) {
            empty[i] = true;
            continue;
        }
        NodePlan P;
        P.node = &node;
        P.index = i;
        const bromath::Mat4& world = node.worldMatrix();
        P.nodeScale = std::sqrt(world.data[4] * world.data[4] + world.data[5] * world.data[5] +
                                world.data[6] * world.data[6]);
        P.pxAtMetre = pxAtMetre;
        P.lod = node.lod();
        P.shadowLod = P.lod;
        P.shadowLod.crownFrom *= 2.0f;
        P.shadowLod.crownTo *= 2.0f;
        P.cores = node.look().leafForm != 0;
        P.clusters = P.lod.clusters > 0.5f && node.look().leafForm == 0;
        P.twigs = node.look().leafForm == 0 && !node.clumpTwigs().empty();
        P.needles = node.look().leafForm == 1;
        float maxHeight = 0.0f;
        for (const auto& s : node.specimens()) {
            maxHeight = std::max(maxHeight, s.height);
            P.blooms = P.blooms || s.bloomCount > 0;
        }
        float maxScale = 0.0f;
        for (const auto& in : node.instances()) maxScale = std::max(maxScale, in.scale);
        P.reach = frame.renderer.vegetationReach(maxHeight * maxScale, world);
        if (node.castsShadow() && frame.gpu.targets.shadowAtlas.width == static_cast<uint32_t>(shadows.atlasSize))
            for (int slot = 0; slot < shadows.tileCount && slot < 32; ++slot)
                if (shadows.tiles[slot].render) P.tileSlots.push_back(slot);
        const std::vector<float>& data = node.gpuData();
        P.store = res.storage(node.id(), node.dataGeneration(), data.data(), data.size() * sizeof(float));
        const bool meshes = res.mesh(node.id(), kLeafSlot, node.geometryGeneration(), node.leafBatch()) &&
                            res.mesh(node.id(), kCardSlot, node.cardGeneration(), node.cardBatch()) &&
                            res.mesh(node.id(), kClusterSlot, node.geometryGeneration(), node.clusterBatch());
        if (!P.store || !meshes) {
            empty[i] = true;
            continue;
        }
        ForestCull& cull = forestCull(node, world, P.reach);
        P.canopyTop = cull.canopyTop(view.frustum, view.eye);
        P.state = planner.node(node);
        if (!planner.begin(frame, *P.state, node, P.store->buffer, cull.bounds, P.reach,
                           static_cast<uint32_t>(P.tileSlots.size()), P.params))
            return false;
        plans.push_back(std::move(P));
    }

    // One budget for them all, as prepareForests' (a lone node's its own).
    float budget = 0.0f, shadowBudget = 0.0f;
    if (nodes.size() == 1) {
        if (!plans.empty()) {
            budget = plans[0].lod.budget;
            shadowBudget = plans[0].lod.shadowBudget;
        }
    } else {
        for (const NodePlan& P : plans) {
            budget = P.lod.budget <= 0.0f || budget < 0.0f ? -1.0f : std::max(budget, P.lod.budget);
            shadowBudget = P.lod.shadowBudget <= 0.0f || shadowBudget < 0.0f
                               ? -1.0f : std::max(shadowBudget, P.lod.shadowBudget);
        }
    }

    // The pose buffer: room for the leaves the last frames asked to pose.
    uint32_t asked = 0;
    for (const NodePlan& P : plans) asked += P.state->askedPoses();
    const SceneVkBuffer* poses = ensurePoses(frame, std::min(asked, kMaxPoses));
    if (!poses) return false;
    const uint32_t poseCap = static_cast<uint32_t>(std::min<VkDeviceSize>(poses->size / kPoseBytes, kMaxPoses));

    // Each node's draws (every group its plan can fill) and parameters.
    std::vector<std::vector<MeshDraw>> draws(plans.size());
    std::vector<VkDescriptorSet> sets(plans.size(), VK_NULL_HANDLE);
    // Each posed group's pose job push constants (FP_POSED_GROUPS: the
    // camera's two leaf groups, then each tile's leaf casters).
    std::vector<std::vector<MeshPushConstants>> posePush(plans.size());
    for (size_t pi = 0; pi < plans.size(); ++pi) {
        NodePlan& P = plans[pi];
        ForestNode& node = *P.node;
        GpuPlanner::Node& state = *P.state;
        VkDescriptorSet set = frame.gpu.device.frameSet(defaults_->boneLayout);
        if (set == VK_NULL_HANDLE) return false;
        SceneVkDescriptorWriter writer;
        writer.writeBuffer(1, P.store->buffer, P.store->size, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
        writer.writeBuffer(2, state.entries.buffer, state.entries.size, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
        writer.writeBuffer(3, poses->buffer, poses->size, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
        writer.updateSet(device_, set);
        sets[pi] = set;
        DrawBase base;
        if (!drawBase(frame, *defaults_, device_, node, set, base)) return false;
        base.clusters = P.clusters;
        base.lod = P.lod;
        base.shadowLod = P.shadowLod;
        base.farLast = P.canopyTop > -1e29f && view.eye.y < P.canopyTop;

        std::vector<uint32_t>& p = P.params;
        auto& list = draws[pi];
        const auto indirect = [&](MeshDraw d, uint32_t v, uint32_t slot) {
            d.indirect = state.args.buffer;
            d.indirectOffset = argsCommandOffset(v, slot);
            return d;
        };
        posePush[pi].assign(2 + P.tileSlots.size(), MeshPushConstants{});
        // The camera's groups, in the CPU's order (a posed leaf group, then
        // its inline twin; the clumps and crowns in batches).
        for (int k = 0; k < kGroups; ++k) {
            if (k == Runs || (k == Cores && !P.cores) || (k == TwigsFine && !P.twigs) || (k == Blooms && !P.blooms))
                continue;
            if (k == LeafNear || k == LeafCard) {
                const uint32_t j = static_cast<uint32_t>(k - LeafNear);
                const MeshDraw posed = groupDraw(frame, node, base, k, false, 0, true, 0, 1);
                putDraw(p, static_cast<uint32_t>(k), false, posed);
                list.push_back(indirect(posed, 0, static_cast<uint32_t>(k)));
                list.push_back(indirect(groupDraw(frame, node, base, k, false, 0, false, 0, 1), 0, FP_GROUPS + j));
                posePush[pi][j] = posed.push;
                posePush[pi][j].extra[3] = 0.0f;
                continue;
            }
            MeshDraw d = indirect(groupDraw(frame, node, base, k, false, 0, false, 0, 1), 0, static_cast<uint32_t>(k));
            putDraw(p, static_cast<uint32_t>(k), false, d);
            if (k == Clumps || k == Crowns) {
                d.indirectBatches = FP_BATCHES;
                d.indirectBatchStride = kArgsBatchStride;
            }
            list.push_back(d);
        }
        // Each shadow tile's casters: cards, the far bark, runs, clumps and
        // crowns (addTree draws a caster nothing else). The cards are a posed
        // group as the camera's leaves are (its inline twin at FP_GROUPS + 1),
        // posed with the caster's push constants, extra.w 1.
        for (size_t t = 0; t < P.tileSlots.size(); ++t)
            for (int k : {LeafCard, Bark5, Clumps, Crowns, Runs}) {
                const uint32_t v = static_cast<uint32_t>(1 + t);
                const MeshDraw d = groupDraw(frame, node, base, k, true, P.tileSlots[t], k == LeafCard, 0, 1);
                putDraw(p, static_cast<uint32_t>(k), true, d);
                list.push_back(indirect(d, v, static_cast<uint32_t>(k)));
                if (k == LeafCard) {
                    list.push_back(indirect(groupDraw(frame, node, base, k, true, P.tileSlots[t], false, 0, 1), v,
                                            FP_GROUPS + 1));
                    posePush[pi][2 + t] = d.push;
                    posePush[pi][2 + t].extra[3] = 1.0f;
                }
            }

        // The view, the node and the tiles.
        const bromath::Mat4& world = node.worldMatrix();
        p[FP_P_EYE] = bitsOf(view.eye.x);
        p[FP_P_EYE + 1] = bitsOf(view.eye.y);
        p[FP_P_EYE + 2] = bitsOf(view.eye.z);
        p[FP_P_PX_AT_METRE] = bitsOf(P.pxAtMetre);
        p[FP_P_NODE_SCALE] = bitsOf(P.nodeScale);
        p[FP_P_REACH] = bitsOf(P.reach);
        p[FP_P_PERSPECTIVE] = view.perspective ? 1u : 0u;
        p[FP_P_LEAF_LENGTH] = bitsOf(node.leafLength());
        p[FP_P_LEAF_VERTS] = static_cast<uint32_t>(node.mesh().vertexCount());
        p[FP_P_LEAF_COPIES] = static_cast<uint32_t>(node.leafCopies());
        p[FP_P_CARD_VERTS] = static_cast<uint32_t>(node.cardVertices());
        p[FP_P_FLAGS] = (P.cores ? FP_F_CORES : 0u) | (P.clusters ? FP_F_CLUSTERS : 0u) |
                        (P.twigs ? FP_F_TWIGS : 0u) | (P.needles ? FP_F_NEEDLES : 0u);
        p[FP_P_BLOOM_BASE] = node.bloomRecordBase();
        p[FP_P_POSE_CAP] = poseCap;
        putRows(p, FP_P_WORLD, world);
        putRows(p, FP_P_INV_WORLD, bromath::minverse(world));
        putPlanes(p, FP_P_FRUSTUM, view.frustum);
        putLod(p, FP_P_LOD, P.lod);
        putLod(p, FP_P_SHADOW_LOD, P.shadowLod);
        for (size_t t = 0; t < P.tileSlots.size(); ++t) {
            const ShadowTilePlan& tile = shadows.tiles[P.tileSlots[t]];
            const uint32_t at = FP_P_TILE + static_cast<uint32_t>(t) * FP_TILE_WORDS;
            putPlanes(p, at + FP_TILE_FRUSTUM, tile.frustum);
            p[at + FP_TILE_TEXEL_CONST] = bitsOf(tile.texelConst);
            p[at + FP_TILE_TEXEL_PER_METRE] = bitsOf(tile.texelPerMetre);
            p[at + FP_TILE_ORTHO] = tile.ortho ? 1u : 0u;
        }
    }

    // The budget's search and settle (settleDetail's rate; its snap for a
    // jump, a new budget, or details last settled on the CPU).
    if (detail.gpuSlot < 0) {
        if (&detail == &forestsDetail_) {
            detail.gpuSlot = 0;
        } else {
            if (nextGpuSlot_ >= static_cast<int>(FP_G_SLOTS)) nextGpuSlot_ = 1;
            detail.gpuSlot = nextGpuSlot_++;
        }
        detail.onGpu = false;
    }
    const SettleRate rate = settleRate(detail, frame, budget, shadowBudget);
    const bool snap = rate.jump || !detail.onGpu;
    detail.onGpu = true;
    for (NodePlan& P : plans) {
        std::vector<uint32_t>& p = P.params;
        const float budgets[2] = {budget, shadowBudget};
        for (uint32_t q = 0; q < 2; ++q) {
            const uint32_t at = FP_P_SEARCH + 4 * q;
            const uint64_t limit = budgets[q] > 0.0f ? limitOf(budgets[q]) : 0u;
            p[at + FP_Q_ON] = budgets[q] > 0.0f ? 1u : 0u;
            p[at + FP_Q_HOLD] = q == 1 && shadows.heldTiles > 0 ? 1u : 0u;
            p[at + FP_Q_LIMIT] = static_cast<uint32_t>(limit);
            p[at + FP_Q_LIMIT + 1] = static_cast<uint32_t>(limit >> 32);
        }
        p[FP_P_SEARCH + FP_Q_SNAP] = snap ? 1u : 0u;
        p[FP_P_SEARCH + FP_Q_STEP] = bitsOf(rate.step);
        p[FP_P_SEARCH + FP_Q_SLOT] = static_cast<uint32_t>(detail.gpuSlot);
    }

    for (NodePlan& P : plans)
        if (!planner.add(frame, *P.state, P.params)) {
            planner.cancel();
            return false;
        }
    out.assign(nodes.size(), {});
    for (size_t i = 0; i < nodes.size(); ++i)
        if (empty[i]) nodes[i]->setStats({});
    for (size_t pi = 0; pi < plans.size(); ++pi) {
        NodePlan& P = plans[pi];
        for (uint32_t j = 0; j < posePush[pi].size(); ++j) planner.addPoseJob(*P.state, posePush[pi][j], j, sets[pi]);
        out[P.index].swap(draws[pi]);
        ForestNode& node = *P.node;
        node.setStatsSource(P.state);
        ForestNode::SeenView seen;
        seen.frustum = view.frustum;
        seen.eye = view.eye;
        seen.world = node.worldMatrix();
        seen.reach = P.reach;
        seen.pxAtMetre = P.pxAtMetre;
        seen.nodeScale = P.nodeScale;
        seen.perspective = view.perspective;
        node.setSeenView(seen);
    }
    return true;
}

void SceneMeshDrawer::forestSubmitted(uint64_t ticket, render::VulkanQueue& queue) {
    if (gpuPlanner_) gpuPlanner_->submitted(ticket, queue);
}

}  // namespace bro::scene::vk
