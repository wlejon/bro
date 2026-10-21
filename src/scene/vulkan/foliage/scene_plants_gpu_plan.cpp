#include "scene/vulkan/foliage/scene_plants_gpu_plan.h"

#include "render/vulkan_pipeline_cache.h"
#include "render/vulkan_queue.h"
#include "scene/scene_renderer.h"
#include "scene/vulkan/frame/scene_defaults.h"
#include "scene/vulkan/frame/scene_frame.h"
#include "scene/vulkan/frame/scene_gpu_timer.h"
#include "scene/vulkan/draw/scene_mesh_drawer.h"
#include "scene/vulkan/frame/scene_vk_descriptors.h"
#include "scene/vulkan/frame/scene_vk_device.h"
#include "scene/vulkan/frame/scene_vk_shader_compiler.h"
#include "scene/vulkan/shaders/world/world_cells_layout.h"
#include "util/log.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

namespace bro::scene::vk::plants {

namespace {

uint32_t bitsOf(float f) {
    uint32_t u;
    std::memcpy(&u, &f, sizeof(u));
    return u;
}

constexpr VkDeviceSize kRecordBytes = 16;   // a chunk record, a vec4
constexpr VkDeviceSize kPoseChunkBytes = PP_LEAVES_PER_CHUNK * 4 * 16;   // a leaf chunk's poses (PLANT_POSE_UVEC4)
// The room a node starts with, before any frame has asked: enough for a
// few thousand plants in view; a larger view's first frames draw what fits.
constexpr uint32_t kFirstRecords = 1u << 15;
constexpr uint32_t kFirstLeafChunks = 1u << 13;

constexpr BuiltinSceneShader kStageShaders[] = {BuiltinSceneShader::PlantsPlanCountComp,
                                                BuiltinSceneShader::PlantsPlanLayoutComp,
                                                BuiltinSceneShader::PlantsPlanPlaceComp};
constexpr const char* kStageNames[] = {"plants plan count", "plants plan layout", "plants plan place"};

VkPipeline computePipeline(SceneVkDevice& device, VkShaderModule module, VkPipelineLayout layout, const char* name) {
    VkComputePipelineCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    ci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    ci.stage.module = module;
    ci.stage.pName = "main";
    ci.layout = layout;
    ci.flags = render::pipelineStatsFlags();
    VkPipeline p = VK_NULL_HANDLE;
    if (vkCreateComputePipelines(device.device(), device.context().pipelineCache(), 1, &ci, nullptr, &p) !=
        VK_SUCCESS)
        return VK_NULL_HANDLE;
    render::setPipelineStatsLabel(name);
    render::logPipelineStats(device.device(), p);
    return p;
}

void putPlanes(uint32_t* p, const bromath::Frustum& f) {
    for (int i = 0; i < 6; ++i) {
        p[4 * i] = bitsOf(f.planes[i].normal.x);
        p[4 * i + 1] = bitsOf(f.planes[i].normal.y);
        p[4 * i + 2] = bitsOf(f.planes[i].normal.z);
        p[4 * i + 3] = bitsOf(f.planes[i].d);
    }
}

// Every write before it seen by every compute read and write, indirect
// command and copy after it.
void barrier(VkCommandBuffer cmd) {
    VkMemoryBarrier b{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    b.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_INDIRECT_COMMAND_READ_BIT |
                      VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT |
                             VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT,
                         0, 1, &b, 0, nullptr, 0, nullptr);
}

}  // namespace

// --- A node's read-backs ---

void GpuPlanner::Node::poll() {
    if (!readback_.mappedData) return;
    // At most a render late: the render before the newest is waited for (done, or all but, by the time the next is
    // prepared), so a script's read two renders after a change sees it.
    int newest = -1, before = -1;
    for (int i = 0; i < kRing; ++i) {
        const Slot& s = ring_[i];
        if (s.state != Slot::Submitted) continue;
        if (newest < 0 || s.serial > ring_[newest].serial) {
            before = newest;
            newest = i;
        } else if (before < 0 || s.serial > ring_[before].serial) {
            before = i;
        }
    }
    if (before >= 0 && ring_[before].serial > readSerial_) ring_[before].queue->wait(ring_[before].ticket);
    int best = -1;
    for (int i = 0; i < kRing; ++i) {
        const Slot& s = ring_[i];
        if (s.state != Slot::Submitted || s.serial <= readSerial_ || !s.queue->isComplete(s.ticket)) continue;
        if (best < 0 || s.serial > ring_[best].serial) best = i;
    }
    if (best < 0) return;
    const auto* w = static_cast<const uint32_t*>(readback_.mappedData) + best * PP_S_WORDS;
    newest_.plants = w[PP_S_PLANTS];
    newest_.elements = w[PP_S_ELEMENTS];
    newest_.limbs = w[PP_S_LIMBS];
    newest_.shadowElements = w[PP_S_SHADOW_ELEMENTS];
    askedRecords_ = w[PP_S_RECORDS];
    askedLeaves_ = w[PP_S_LEAF_CHUNKS];
    have_ = true;
    readSerial_ = ring_[best].serial;
    for (Slot& slot : ring_)
        if (slot.state == Slot::Submitted && slot.serial <= readSerial_ && slot.queue->isComplete(slot.ticket))
            slot.state = Slot::Free;
}

bool GpuPlanner::Node::latest(PlantsNode::Stats& out) {
    poll();
    if (!have_) return false;
    out = newest_;
    return true;
}

// --- Setup ---

bool GpuPlanner::setup(SceneGpu& gpu) {
    device_ = gpu.device.device();
    limits_ = gpu.device.context().deviceProperties().limits;
    SceneVkDescriptorLayoutBuilder builder;
    for (uint32_t b = 0; b < 6; ++b) builder.addBinding(b, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT);
    setLayout_ = builder.build(device_);
    VkPipelineLayoutCreateInfo pi{};
    pi.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pi.setLayoutCount = 1;
    pi.pSetLayouts = &setLayout_;
    if (!setLayout_ || vkCreatePipelineLayout(device_, &pi, nullptr, &layout_) != VK_SUCCESS) {
        LOG_ERROR("plants planner: failed creating its layout");
        return false;
    }
    for (int s = 0; s < kStages; ++s) {
        modules_[s] = SceneVkShaderCompiler::createBuiltinModule(device_, kStageShaders[s]);
        pipelines_[s] = modules_[s] ? computePipeline(gpu.device, modules_[s], layout_, kStageNames[s]) : VK_NULL_HANDLE;
        if (!pipelines_[s]) {
            LOG_ERROR("plants planner: failed creating the %s stage", kStageNames[s]);
            return false;
        }
    }
    // The leaf poses over what the plan made: the camera (set 0) and the
    // draws' set 3.
    const std::array<VkDescriptorSetLayout, 4> poseSets = {gpu.defaults.cameraLayout, gpu.defaults.emptyLayout,
                                                           gpu.defaults.emptyLayout, gpu.defaults.boneLayout};
    pi.setLayoutCount = static_cast<uint32_t>(poseSets.size());
    pi.pSetLayouts = poseSets.data();
    pi.pushConstantRangeCount = 0;
    pi.pPushConstantRanges = nullptr;
    poseModule_ = SceneVkShaderCompiler::createBuiltinModule(device_, BuiltinSceneShader::PlantsPoseComp);
    if (!poseModule_ || vkCreatePipelineLayout(device_, &pi, nullptr, &poseLayout_) != VK_SUCCESS ||
        !(posePipeline_ = computePipeline(gpu.device, poseModule_, poseLayout_, "plants pose indirect"))) {
        LOG_ERROR("plants planner: failed creating its pose stage");
        return false;
    }
    if (!cellsPass_.setup(gpu)) {
        LOG_ERROR("plants planner: failed setting up the world cell pass");
        return false;
    }
    return true;
}

void GpuPlanner::destroyNode(SceneVkAllocator& allocator, Node& n) {
    for (SceneVkBuffer* b : {&n.work_, &n.readback_})
        if (b->isValid()) allocator.destroyBuffer(*b);
    for (uint32_t s = 0; s < Node::kSlots; ++s)
        for (SceneVkBuffer* b : {&n.slotChunks_[s], &n.slotArgs_[s], &n.slotPoses_[s]})
            if (b->isValid()) allocator.destroyBuffer(*b);
    n.chunks = n.args = n.poses = {};
    WorldCellsPass::destroy(allocator, n.cellTarget_);
}

void GpuPlanner::cleanup(SceneGpu& gpu) {
    for (auto& [key, n] : nodes_) destroyNode(gpu.allocator, *n);
    nodes_.clear();
    queued_.clear();
    cellsPass_.cleanup(gpu);
    for (int s = 0; s < kStages; ++s) {
        if (pipelines_[s]) vkDestroyPipeline(device_, pipelines_[s], nullptr);
        SceneVkShaderModule::destroy(device_, modules_[s]);
        pipelines_[s] = VK_NULL_HANDLE;
        modules_[s] = VK_NULL_HANDLE;
    }
    if (posePipeline_) vkDestroyPipeline(device_, posePipeline_, nullptr);
    SceneVkShaderModule::destroy(device_, poseModule_);
    if (poseLayout_) vkDestroyPipelineLayout(device_, poseLayout_, nullptr);
    if (layout_) vkDestroyPipelineLayout(device_, layout_, nullptr);
    if (setLayout_) vkDestroyDescriptorSetLayout(device_, setLayout_, nullptr);
    posePipeline_ = VK_NULL_HANDLE;
    poseModule_ = VK_NULL_HANDLE;
    poseLayout_ = layout_ = VK_NULL_HANDLE;
    setLayout_ = VK_NULL_HANDLE;
}

std::shared_ptr<GpuPlanner::Node> GpuPlanner::node(const PlantsNode& n) {
    auto& slot = nodes_[&n];
    if (!slot) slot = std::make_shared<Node>();
    slot->used_ = ++use_;
    return slot;
}

bool GpuPlanner::ensure(SceneFrame& frame, SceneVkBuffer& buf, VkDeviceSize size, VkBufferUsageFlags usage,
                        VkMemoryPropertyFlags props) {
    if (buf.isValid() && buf.size >= size) return true;
    // (The old one stays until the frames that read it are done.)
    if (buf.isValid()) frame.gpu.allocator.destroyBuffer(buf);
    // (Shared: the plan writes it on the compute queue, the draws read it.)
    if (!frame.gpu.allocator.createBuffer(size, usage, props, buf, /*shared=*/true)) {
        buf = {};
        return false;
    }
    return true;
}

// --- A node's frame ---

bool GpuPlanner::add(SceneFrame& frame, Node& n, const PlantsNode& node, const SceneVkBuffer& store,
                     const std::vector<int>& tiles) {
    if (tiles.size() > PP_MAX_TILES || node.cells().empty()) return false;
    // Nodes gone (or long unplanned) give their buffers back.
    if ((use_ & 1023) == 0)
        for (auto it = nodes_.begin(); it != nodes_.end();) {
            if (it->second->used_ + 4096 < use_) {
                destroyNode(frame.gpu.allocator, *it->second);
                it = nodes_.erase(it);
            } else {
                ++it;
            }
        }
    n.poll();
    // A frame recorded but never submitted reads back nothing.
    for (Node::Slot& s : n.ring_)
        if (s.state == Node::Slot::Recorded) s.state = Node::Slot::Free;

    // Its cells, rebuilt when its plants or species move.
    if (n.generation_ != node.generation()) {
        n.cellsGeneration_ = node.generation();
        n.cells_.setContent(n.provider_, node.cells(), n.cellsGeneration_);
        n.generation_ = node.generation();
    }
    if (!cellsPass_.update(frame, n.cellTarget_, n.cells_, n.provider_, n.cellsGeneration_)) return false;

    // The room: what the last frames asked for and half again, this frame
    // slot's (the last frame's draws may still read theirs).
    const uint32_t f = frame.gpu.device.frames().frameIndex();
    const uint64_t maxRecords = limits_.maxStorageBufferRange / kRecordBytes;
    const uint64_t maxLeaves = limits_.maxStorageBufferRange / kPoseChunkBytes;
    const auto room = [](uint64_t asked, uint64_t first, uint64_t most, VkDeviceSize have, VkDeviceSize unit) {
        const uint64_t need = std::min<uint64_t>(std::max(asked, first), most);
        if (have / unit >= need) return have / unit;
        return std::min<uint64_t>(std::max(need, asked + asked / 2), most);
    };
    const uint64_t records = room(n.askedRecords_, kFirstRecords, maxRecords, n.slotChunks_[f].size, kRecordBytes);
    const uint64_t leaves = room(n.askedLeaves_, kFirstLeafChunks, maxLeaves, n.slotPoses_[f].size, kPoseChunkBytes);
    constexpr VkMemoryPropertyFlags kLocal = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    if (!ensure(frame, n.slotChunks_[f], records * kRecordBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, kLocal) ||
        !ensure(frame, n.slotPoses_[f], leaves * kPoseChunkBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, kLocal) ||
        !ensure(frame, n.slotArgs_[f], PP_A_WORDS * 4,
                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT, kLocal) ||
        !ensure(frame, n.work_, PP_N_WORDS * 4,
                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                    VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                kLocal))
        return false;
    n.chunks = n.slotChunks_[f];
    n.poses = n.slotPoses_[f];
    n.args = n.slotArgs_[f];
    if (!n.readback_.isValid()) {
        if (!frame.gpu.allocator.createBuffer(Node::kRing * PP_S_WORDS * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                              VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                              n.readback_) ||
            !n.readback_.mappedData) {
            if (n.readback_.isValid()) frame.gpu.allocator.destroyBuffer(n.readback_);
            return false;
        }
        for (Node::Slot& s : n.ring_) s = {};
    }

    // The frame's params: the camera, the tiles, the node's detail.
    const SceneView& view = frame.view;
    const ShadowPlan& plan = frame.renderer.shadowPlan();
    const PlantsNode::Params& p = node.params();
    std::vector<uint32_t> w(PP_P_WORDS, 0u);
    w[PP_P_TILES] = static_cast<uint32_t>(tiles.size());
    w[PP_P_CHUNK_CAP] = static_cast<uint32_t>(records);
    w[PP_P_LEAF_CAP] = static_cast<uint32_t>(leaves);
    w[PP_P_EYE] = bitsOf(view.eye.x);
    w[PP_P_EYE + 1] = bitsOf(view.eye.y);
    w[PP_P_EYE + 2] = bitsOf(view.eye.z);
    // Pixels a metre at unit distance: the projection's y scale over half
    // the target's height.
    const float focal = std::fabs(view.proj.at(1, 1)) * 0.5f * static_cast<float>(view.height);
    w[PP_P_FOCAL] = bitsOf(focal);
    const float detail[] = {p.pixels, p.minElements, p.woodRange, p.range, p.shadowTexels, p.shadowRange};
    for (int i = 0; i < 6; ++i) w[PP_P_PIXELS + i] = bitsOf(detail[i]);
    putPlanes(w.data() + PP_P_FRUSTUM, view.frustum);
    // The cell pass's views, the plan's own: the camera, then each tile,
    // each with its pixel scale.
    std::vector<uint32_t> planes(24 * (1 + tiles.size()));
    std::vector<WorldCellsPass::Scale> scales(1 + tiles.size());
    putPlanes(planes.data(), view.frustum);
    scales[0].focalPixels = focal;
    for (size_t t = 0; t < tiles.size(); ++t) {
        const ShadowViewPlan& tile = plan.views[tiles[t]];
        uint32_t* at = w.data() + PP_P_TILE + t * PP_TILE_WORDS;
        putPlanes(at + PP_TILE_FRUSTUM, tile.frustum);
        at[PP_TILE_TEXEL_CONST] = bitsOf(tile.texel);
        at[PP_TILE_TEXEL_PER_METRE] = bitsOf(tile.texelPerMetre);
        at[PP_TILE_ORTHO] = tile.ortho ? 1u : 0u;
        putPlanes(planes.data() + 24 * (1 + t), tile.frustum);
        scales[1 + t].texel = tile.texel;
        scales[1 + t].texelPerMetre = tile.ortho ? 0.0f : tile.texelPerMetre;
    }
    WorldCellsPass::Views views;
    views.eye[0] = view.eye.x;
    views.eye[1] = view.eye.y;
    views.eye[2] = view.eye.z;
    views.count = static_cast<uint32_t>(1 + tiles.size());
    views.planes = planes.data();
    views.scales = scales.data();
    views.itemGroup = PP_GROUP_SIZE;   // (no bands: every cell is planned plant by plant)
    if (!cellsPass_.begin(frame, n.cellTarget_, views, n.cells_, n.provider_)) return false;

    const VkDeviceSize bytes = PP_P_WORDS * 4;
    const render::UploadSlice slice =
        frame.gpu.device.frameUpload(bytes, std::max<VkDeviceSize>(limits_.minStorageBufferOffsetAlignment, 16));
    if (!slice) return false;
    std::memcpy(slice.mapped, w.data(), bytes);
    n.planSet_ = frame.gpu.device.frameSet(setLayout_);
    n.drawSet = frame.gpu.device.frameSet(frame.gpu.defaults.boneLayout);
    if (n.planSet_ == VK_NULL_HANDLE || n.drawSet == VK_NULL_HANDLE) return false;
    constexpr VkDescriptorType kStorage = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    SceneVkDescriptorWriter plan0;
    plan0.writeBuffer(0, store.buffer, store.size, 0, kStorage);
    plan0.writeBuffer(1, slice.buffer, bytes, slice.offset, kStorage);
    plan0.writeBuffer(2, n.cellTarget_.out.buffer, n.cellTarget_.out.size, 0, kStorage);
    plan0.writeBuffer(3, n.work_.buffer, n.work_.size, 0, kStorage);
    plan0.writeBuffer(4, n.chunks.buffer, n.chunks.size, 0, kStorage);
    plan0.writeBuffer(5, n.args.buffer, n.args.size, 0, kStorage);
    plan0.updateSet(device_, n.planSet_);
    SceneVkDescriptorWriter draw3;
    draw3.writeBuffer(1, store.buffer, store.size, 0, kStorage);
    draw3.writeBuffer(2, n.chunks.buffer, n.chunks.size, 0, kStorage);
    draw3.writeBuffer(3, n.poses.buffer, n.poses.size, 0, kStorage);
    draw3.updateSet(device_, n.drawSet);
    queued_.push_back(&n);
    return true;
}

// --- Recording ---

void GpuPlanner::record(SceneFrame& frame) {
    if (queued_.empty()) return;
    VkCommandBuffer cmd = frame.computeStream();
    // On the async compute queue the draws are on another queue: the frame's
    // join orders them after the plan, and its barriers name only the stages
    // a compute queue has.
    const bool apart = frame.computeApart();
    const auto mark = [&](const char* name) {
        if (frame.timer) frame.timer->mark(cmd, name);
    };
    // Last frame's plan, poses and read-back used what this rewrites.
    VkMemoryBarrier start{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    start.srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_INDIRECT_COMMAND_READ_BIT |
                          VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    start.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &start, 0,
                         nullptr, 0, nullptr);
    for (Node* n : queued_) vkCmdFillBuffer(cmd, n->work_.buffer, 0, PP_N_WORDS * 4, 0u);
    barrier(cmd);
    // The cells in view.
    std::vector<WorldCellsPass::Target*> targets;
    for (Node* n : queued_) targets.push_back(&n->cellTarget_);
    cellsPass_.record(cmd, targets);
    barrier(cmd);
    mark("plants:cells");
    // Count, lay out, place.
    const auto bind = [&](Stage s, const Node* n) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipelines_[s]);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout_, 0, 1, &n->planSet_, 0, nullptr);
    };
    for (Node* n : queued_) {
        bind(Count, n);
        vkCmdDispatchIndirect(cmd, n->cellTarget_.out.buffer, VkDeviceSize{WC_O_DISPATCH} * 4);
    }
    barrier(cmd);
    for (Node* n : queued_) {
        bind(Layout, n);
        vkCmdDispatch(cmd, 1, 1, 1);
    }
    barrier(cmd);
    for (Node* n : queued_) {
        bind(Place, n);
        vkCmdDispatchIndirect(cmd, n->cellTarget_.out.buffer, VkDeviceSize{WC_O_DISPATCH} * 4);
    }
    barrier(cmd);
    mark("plants:plan");
    // The leaf poses, a workgroup a leaf chunk the plan made.
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, posePipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, poseLayout_, 0, 1, &frame.cameraSet, 0, nullptr);
    for (Node* n : queued_) {
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, poseLayout_, 3, 1, &n->drawSet, 0, nullptr);
        vkCmdDispatchIndirect(cmd, n->args.buffer, VkDeviceSize{PP_A_POSE} * 4);
    }
    VkMemoryBarrier done{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    done.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    done.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         (apart ? 0 : VK_PIPELINE_STAGE_VERTEX_SHADER_BIT) | VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT |
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 1, &done, 0, nullptr, 0, nullptr);
    mark("plants:pose");
    // The stats, to the read-back ring.
    for (Node* n : queued_) {
        const int slot = n->cursor_;
        n->cursor_ = (n->cursor_ + 1) % Node::kRing;
        n->ring_[slot].state = Node::Slot::Recorded;
        n->ring_[slot].serial = ++n->serial_;
        n->ring_[slot].ticket = 0;
        VkBufferCopy copy{VkDeviceSize{PP_N_STATS} * 4, VkDeviceSize(slot) * PP_S_WORDS * 4, VkDeviceSize{PP_S_WORDS} * 4};
        vkCmdCopyBuffer(cmd, n->work_.buffer, n->readback_.buffer, 1, &copy);
    }
    VkMemoryBarrier host{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &host, 0, nullptr, 0,
                         nullptr);
    queued_.clear();
}

void GpuPlanner::submitted(uint64_t ticket, render::VulkanQueue& queue) {
    for (auto& [key, n] : nodes_)
        for (Node::Slot& s : n->ring_)
            if (s.state == Node::Slot::Recorded) {
                s.state = Node::Slot::Submitted;
                s.ticket = ticket;
                s.queue = &queue;
            }
}

}  // namespace bro::scene::vk::plants
