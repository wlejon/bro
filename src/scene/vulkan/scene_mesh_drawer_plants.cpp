// SceneMeshDrawer's plants path: a PlantsNode becomes two draws a view, its
// leaves and its wood, each an instance a chunk of one plant's elements
// (shaders/scene_plants.glsl builds them), for the camera and for each
// shadow tile its casters go to.
//
// Which plants each view draws and with how many elements is planned on the
// GPU (scene_plants_gpu_plan.cpp, plants_plan.comp) over the cells in view:
// the draws here are indirect, their counts the plan's, and nothing on the
// CPU walks the plants. Without the planner (a device that cannot start an
// indirect draw at an instance, or no world cell pass) the CPU plans every
// plant as the GPU would, below, and draws what it chose; the forest keeps
// its CPU path for the same reason.
//
// A plant's element count: its species' full count while an element (a
// leaf of its length) is at least the node's `pixels` across on screen,
// falling with the square of the distance past that (so an element stays
// about `pixels` across), never under `minElements`. The count is
// continuous: its fraction grows the last element in.

#include "scene/vulkan/scene_mesh_drawer.h"

#include "scene/plants_node.h"
#include "scene/scene_renderer.h"
#include "scene/vulkan/scene_defaults.h"
#include "scene/vulkan/scene_frame.h"
#include "scene/vulkan/scene_gpu_resources.h"
#include "scene/vulkan/scene_plants_gpu_plan.h"
#include "scene/vulkan/scene_targets.h"
#include "scene/vulkan/scene_vk_descriptors.h"
#include "scene/vulkan/scene_vk_device.h"
#include "util/log.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace bro::scene::vk {

namespace {

constexpr uint32_t kLeafVerts = 12;
constexpr uint32_t kTubeVerts = PP_TUBE_VERTS;
constexpr uint32_t kLeavesPerChunk = PP_LEAVES_PER_CHUNK;
constexpr uint32_t kLimbsPerChunk = PP_LIMBS_PER_CHUNK;
constexpr int kMaxLobes = PP_MAX_LOBES;
constexpr VkDeviceSize kPoseBytes = 4 * 16;   // PLANT_POSE_UVEC4 uvec4s

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

// --- The CPU's plan (without the GPU planner) ---

struct Visible {
    float dist;
    int plant;
    float elements;   // 0: a caster drawn as its crown's disc
    bool wood;
};

// The plants one view draws: the camera's, or a shadow tile's casters.
struct Group {
    int slot = -1;                 // the shadow tile (-1: the camera)
    std::vector<Visible> plants;
    // Its chunks: leaves, then crown discs (drawn with them), then wood.
    uint32_t firstLeaf = 0, leaves = 0, discs = 0, firstWood = 0, wood = 0;
};

// Every plant's pick in every view, as plants_plan.comp's pick(), the
// camera's nearest first. Fills the groups' chunk records and the discs'.
void planOnCpu(const SceneFrame& frame, const PlantsNode& node, const std::vector<int>& tiles,
               std::vector<Group>& groups, std::vector<float>& chunks, std::vector<float>& discs,
               PlantsNode::Stats& stats) {
    const SceneView& view = frame.view;
    const PlantsNode::Params& p = node.params();
    const float focal = std::fabs(view.proj.at(1, 1)) * 0.5f * static_cast<float>(view.height);
    const std::vector<float>& sph = node.spheres();
    const int speciesCount = node.speciesCount();
    const auto plantSpecies = [&](int i) {
        int si = static_cast<int>(node.plant(i)[5]);
        if (si < 0 || si >= speciesCount) si = 0;
        return node.species(si);
    };
    const auto elementCount = [&](int i, float across, float kept, float least) {
        const float* sp = plantSpecies(i);
        const float full = std::max(sp[8], 1.0f);
        const float leaf = sp[6] * node.plant(i)[4] * across;
        return std::clamp(full * std::min(1.0f, (leaf * leaf) / (kept * kept)), std::min(least, full), full);
    };
    const auto boxOf = [&](int i) {
        const float* s = sph.data() + static_cast<size_t>(i) * 4;
        return bromath::AABB3{{s[0] - s[3], s[1] - s[3], s[2] - s[3]}, {s[0] + s[3], s[1] + s[3], s[2] + s[3]}};
    };
    groups.assign(1, Group{});
    std::vector<int> casters;
    for (int i = 0; i < node.plantCount(); ++i) {
        const float* s = sph.data() + static_cast<size_t>(i) * 4;
        const float dx = s[0] - view.eye.x, dy = s[1] - view.eye.y, dz = s[2] - view.eye.z;
        const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (!tiles.empty() && d - s[3] <= p.shadowRange) casters.push_back(i);
        if (d - s[3] > p.range || !bromath::fintersects(view.frustum, boxOf(i))) continue;
        const float across = focal / std::max(d - 0.5f * s[3], 0.5f);
        const float crown = plantSpecies(i)[2] * node.plant(i)[4] * across / p.pixels;
        const float least = std::clamp(3.14159f * crown * crown, 1.0f, p.minElements);
        groups[0].plants.push_back({d, i, elementCount(i, across, p.pixels, least), d - s[3] < p.woodRange});
    }
    std::sort(groups[0].plants.begin(), groups[0].plants.end(),
              [](const Visible& a, const Visible& b) { return a.dist < b.dist; });
    const ShadowPlan& plan = frame.renderer.shadowPlan();
    for (int slot : tiles) {
        const ShadowTilePlan& tile = plan.tiles[slot];
        Group g;
        g.slot = slot;
        for (int i : casters) {
            if (!bromath::fintersects(tile.frustum, boxOf(i))) continue;
            const float* s = sph.data() + static_cast<size_t>(i) * 4;
            const float dx = s[0] - view.eye.x, dy = s[1] - view.eye.y, dz = s[2] - view.eye.z;
            const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
            const float texel = std::max(tile.ortho ? tile.texelConst : tile.texelPerMetre * std::max(d, 1.0f), 1e-4f);
            const float crown = s[3] / (texel * p.shadowTexels);
            if (crown < 1.5f) {
                g.plants.push_back({d, i, 0.0f, false});
                continue;
            }
            const float least = std::clamp(3.14159f * crown * crown, 2.0f, p.minElements);
            g.plants.push_back({d, i, elementCount(i, 1.0f / texel, p.shadowTexels, least), d - s[3] < p.woodRange});
        }
        if (!g.plants.empty()) groups.push_back(std::move(g));
    }

    // The chunks: each group's leaves, its discs' chunks, its wood. A disc
    // chunk's first record is in the discs placed after every chunk
    // (renumbered once their count is known).
    uint32_t chunkCount = 0;
    std::vector<size_t> discChunks;   // where each disc chunk's first-record float is
    for (Group& g : groups) {
        g.firstLeaf = chunkCount;
        for (const Visible& v : g.plants) {
            const uint32_t n = static_cast<uint32_t>(std::ceil(v.elements));
            for (uint32_t k = 0; k < n; k += kLeavesPerChunk) {
                chunks.insert(chunks.end(), {static_cast<float>(v.plant), static_cast<float>(k), v.elements,
                                             g.slot >= 0 ? 1.0f : 0.0f});
                ++chunkCount;
            }
        }
        g.leaves = chunkCount - g.firstLeaf;
        const uint32_t firstDisc = static_cast<uint32_t>(discs.size() / 4);
        for (const Visible& v : g.plants)
            if (v.elements <= 0.0f) discs.insert(discs.end(), {static_cast<float>(v.plant), 0.0f, 0.0f, 0.0f});
        const uint32_t discCount = static_cast<uint32_t>(discs.size() / 4) - firstDisc;
        for (uint32_t k = 0; k < discCount; k += kLeavesPerChunk) {
            discChunks.push_back(chunks.size());
            chunks.insert(chunks.end(), {static_cast<float>(firstDisc + k),
                                         static_cast<float>(std::min(kLeavesPerChunk, discCount - k)), 0.0f, 2.0f});
            ++chunkCount;
            ++g.discs;
        }
    }
    for (Group& g : groups) {
        g.firstWood = chunkCount;
        const float limbRange = (g.slot < 0 ? 0.4f : 0.15f) * p.woodRange;
        for (const Visible& v : g.plants) {
            if (!v.wood) continue;
            const int lobes = std::clamp(static_cast<int>(plantSpecies(v.plant)[4]), 1, kMaxLobes);
            const int limbs = v.dist < limbRange ? lobes + 1 : 1;
            for (int k = 0; k < limbs; k += static_cast<int>(kLimbsPerChunk)) {
                chunks.insert(chunks.end(), {static_cast<float>(v.plant), static_cast<float>(k),
                                             static_cast<float>(limbs), 0.0f});
                ++chunkCount;
            }
            if (g.slot < 0) stats.limbs += static_cast<uint32_t>(limbs);
        }
        g.wood = chunkCount - g.firstWood;
    }
    for (size_t at : discChunks) chunks[at] += static_cast<float>(chunkCount);
    for (const Visible& v : groups[0].plants) stats.elements += static_cast<uint32_t>(std::ceil(v.elements));
    stats.plants = static_cast<uint32_t>(groups[0].plants.size());
    for (size_t g = 1; g < groups.size(); ++g)
        for (const Visible& v : groups[g].plants)
            stats.shadowElements += std::max(1u, static_cast<uint32_t>(std::ceil(v.elements)));
}

}  // namespace

const SceneVkBuffer* SceneMeshDrawer::ensurePlantPoses(SceneFrame& frame, VkDeviceSize bytes, VkDeviceSize& offset) {
    const VkDeviceSize align = std::max<VkDeviceSize>(
        frame.gpu.device.context().deviceProperties().limits.minStorageBufferOffsetAlignment, 16);
    SceneVkBuffer& poses = plantPoses_[frame.gpu.device.frames().frameIndex()];
    offset = (plantPoseUsed_ + align - 1) / align * align;
    if (!poses.isValid() || poses.size < offset + bytes) {
        // Grown with room to spare; the old buffer stays until the frames
        // that read it are done (this one's earlier nodes among them).
        if (poses.isValid()) frame.gpu.allocator.destroyBuffer(poses);
        const VkDeviceSize size = std::max<VkDeviceSize>(bytes + bytes / 2, VkDeviceSize{1} << 20);
        if (!frame.gpu.allocator.createBuffer(size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                              VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, poses)) {
            LOG_ERROR("SceneMeshDrawer: Failed creating the plants pose buffer (%llu bytes)",
                      static_cast<unsigned long long>(size));
            poses = {};
            return nullptr;
        }
        offset = 0;
    }
    plantPoseUsed_ = offset + bytes;
    return &poses;
}

void SceneMeshDrawer::recordPlantPoses(SceneFrame& frame) {
    // The GPU's plans (and their poses) into the frame's compute stream.
    if (plantPlanner_) plantPlanner_->record(frame);
    if (plantPoseJobs_.empty()) {
        plantPoseUsed_ = 0;
        return;
    }
    // The CPU's plans' poses.
    VkCommandBuffer cmd = frame.cmd;
    // The last frame's draws read the buffer this one overwrites.
    VkMemoryBarrier before{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    before.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    before.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_VERTEX_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &before,
                         0, nullptr, 0, nullptr);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, plantPosePipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, poseLayout_, 0, 1, &frame.cameraSet, 0, nullptr);
    for (const PlantPoseJob& job : plantPoseJobs_) {
        MeshPushConstants push{};
        push.extra[0] = static_cast<float>(job.firstChunk);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, poseLayout_, 3, 1, &job.set, 0, nullptr);
        vkCmdPushConstants(cmd, poseLayout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(MeshPushConstants), &push);
        vkCmdDispatch(cmd, job.chunks, 1, 1);
    }
    VkMemoryBarrier after{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    after.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    after.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_VERTEX_SHADER_BIT, 0, 1, &after,
                         0, nullptr, 0, nullptr);
    plantPoseJobs_.clear();
    plantPoseUsed_ = 0;
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
    // Near and solid first: the plants are the near forest.
    draw.order = MeshDraw::kOrderMid - 1;
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
    // A view's part: the camera's (slot -1) or a tile's casters (never in
    // the colour lists).
    const auto place = [&](MeshDraw part, int slot) {
        if (slot >= 0) {
            part.shadowOnly = part.castsShadow = true;
            part.shadowTileMask = 1u << slot;
            part.shadowTimerLabel = "shadow:plants";
        }
        out.push_back(part);
    };

    // Planned on the GPU: each view's draws indirect, their counts the plan's.
    if (plantPlanner_ && plantPlanner_->ready()) {
        std::shared_ptr<plants::GpuPlanner::Node> state = plantPlanner_->node(node);
        if (plantPlanner_->add(frame, *state, node, *store, tiles)) {
            node.setStatsSource(state);
            for (uint32_t v = 0; v <= tiles.size(); ++v)
                for (MeshDraw part : {leaves, wood}) {
                    part.vertexSet = state->drawSet;
                    part.indirect = state->args.buffer;
                    part.indirectOffset = plants::commandOffset(v, part.push.extra[0] > 0.5f);
                    place(part, v == 0 ? -1 : tiles[v - 1]);
                }
            return;
        }
    }

    // On the CPU: every plant, every view.
    PlantsNode::Stats stats;
    std::vector<Group> groups;
    std::vector<float> chunks, discs;
    planOnCpu(frame, node, tiles, groups, chunks, discs, stats);
    node.setStats(stats);
    const uint32_t chunkCount = static_cast<uint32_t>(chunks.size() / 4);
    if (chunkCount == 0) return;
    chunks.insert(chunks.end(), discs.begin(), discs.end());
    const VkDeviceSize bytes = chunks.size() * sizeof(float);
    const render::UploadSlice slice = frame.gpu.device.frameUpload(bytes);
    if (!slice) return;
    std::memcpy(slice.mapped, chunks.data(), bytes);
    // The leaves' poses: a slot an element of every chunk (a wood chunk's
    // left unused).
    const VkDeviceSize poseBytes = VkDeviceSize{chunkCount} * kLeavesPerChunk * kPoseBytes;
    VkDeviceSize poseOffset = 0;
    const SceneVkBuffer* poses = ensurePlantPoses(frame, poseBytes, poseOffset);
    if (!poses) return;
    VkDescriptorSet set = frame.gpu.device.frameSet(defaults_->boneLayout);
    SceneVkDescriptorWriter writer;
    writer.writeBuffer(1, store->buffer, store->size, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
    writer.writeBuffer(2, slice.buffer, bytes, slice.offset, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
    writer.writeBuffer(3, poses->buffer, poseBytes, poseOffset, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
    writer.updateSet(device_, set);
    for (const Group& g : groups)
        if (g.leaves > 0) plantPoseJobs_.push_back({set, g.firstLeaf, g.leaves});
    for (const Group& g : groups)
        for (MeshDraw part : {leaves, wood}) {
            const bool isWood = part.push.extra[0] > 0.5f;
            part.vertexSet = set;
            part.instanceCount = isWood ? g.wood : g.leaves + g.discs;
            part.firstInstance = isWood ? g.firstWood : g.firstLeaf;
            if (part.instanceCount > 0) place(part, g.slot);
        }
}

}  // namespace bro::scene::vk
