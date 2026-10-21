#include "scene/vulkan/ground/scene_cover_plan.h"

#include "render/vulkan_util.h"
#include "scene/vulkan/frame/scene_defaults.h"
#include "scene/vulkan/frame/scene_frame.h"
#include "scene/vulkan/frame/scene_gpu_timer.h"
#include "scene/vulkan/frame/scene_vk_descriptors.h"
#include "scene/vulkan/frame/scene_vk_device.h"
#include "scene/vulkan/frame/scene_vk_pipeline.h"
#include "scene/vulkan/frame/scene_vk_shader_compiler.h"
#include "util/log.h"

#include <algorithm>
#include <cstring>

namespace bro::scene::vk {

namespace {

/// cover_plan.comp's push constants.
struct CoverPush {
    uint32_t firstRun, runCount, perCell, candidates, count, first, kind, start;
};
/// cover_plan.comp's CoverRun.
struct CoverRun {
    int32_t rect[4];
    uint32_t before[4];
};
constexpr uint32_t kGroup = 64;                 // cover_plan.comp's local_size_x
constexpr uint32_t kMaxGroups = 65535;          // a dispatch's x (maxComputeWorkGroupCount's floor)
constexpr uint32_t kCommandWords = 5;           // a VkDrawIndexedIndirectCommand
constexpr uint64_t kMaxEntries = 1ull << 24;    // a guard, far above any view's need
constexpr VkDeviceSize kEntryBytes = 8;         // scene_cover_plan.glsl's uvec2

}  // namespace

bool CoverPlan::setup(SceneGpu& gpu) {
    device_ = gpu.device.device();
    storageAlign_ = std::max<VkDeviceSize>(gpu.device.context().deviceProperties().limits.minStorageBufferOffsetAlignment, 16);
    // The camera (set 0), the list, commands and runs (set 3, the draws'
    // vertex set) and the ring's grass set (set 4).
    const std::array<VkDescriptorSetLayout, 5> sets = {gpu.defaults.cameraLayout, gpu.defaults.emptyLayout,
                                                       gpu.defaults.emptyLayout, gpu.defaults.boneLayout,
                                                       gpu.defaults.customLayout};
    const VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(CoverPush)};
    VkPipelineLayoutCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    info.setLayoutCount = static_cast<uint32_t>(sets.size());
    info.pSetLayouts = sets.data();
    info.pushConstantRangeCount = 1;
    info.pPushConstantRanges = &push;
    if (vkCreatePipelineLayout(device_, &info, nullptr, &layout_) != VK_SUCCESS) {
        LOG_ERROR("cover plan: failed creating its layout");
        return false;
    }
    pipeline_ = createComputePipeline(device_, BuiltinSceneShader::CoverPlanComp, layout_, "cover plan");
    if (!pipeline_) {
        LOG_ERROR("cover plan: failed creating its pipeline");
        return false;
    }
    return true;
}

void CoverPlan::destroy(SceneVkAllocator& allocator, NodeState& n) {
    for (uint32_t s = 0; s < NodeState::kSlots; ++s)
        for (SceneVkBuffer* b : {&n.list[s], &n.commands[s]})
            if (b->isValid()) allocator.destroyBuffer(*b);
}

void CoverPlan::cleanup(SceneGpu& gpu) {
    for (auto& [id, n] : nodes_) destroy(gpu.allocator, n);
    nodes_.clear();
    jobs_.clear();
    commands_.clear();
    if (pipeline_) vkDestroyPipeline(device_, pipeline_, nullptr);
    if (layout_) vkDestroyPipelineLayout(device_, layout_, nullptr);
    pipeline_ = VK_NULL_HANDLE;
    layout_ = VK_NULL_HANDLE;
}

bool CoverPlan::add(SceneFrame& frame, uint32_t nodeId, std::span<const Ring> rings, std::vector<Draw>& out) {
    out.clear();
    if (rings.empty() || !pipeline_) return false;
    // Nodes gone (or long undrawn) give their buffers back.
    if ((++use_ & 1023) == 0)
        for (auto it = nodes_.begin(); it != nodes_.end();) {
            if (it->second.used + 4096 < use_) {
                destroy(frame.gpu.allocator, it->second);
                it = nodes_.erase(it);
            } else {
                ++it;
            }
        }
    NodeState& n = nodes_[nodeId];
    n.used = use_;

    // Each ring's candidates, its entries' room the same.
    uint64_t entries = 0;
    size_t runCount = 0;
    for (const Ring& r : rings) {
        uint64_t cells = 0;
        for (const auto& run : *r.runs) cells += static_cast<uint64_t>(run[2]) * static_cast<uint64_t>(run[3]);
        entries += cells * r.perCell;
        runCount += r.runs->size();
    }
    if (entries == 0 || entries > kMaxEntries) return false;

    // This frame slot's list and commands (the old ones stay until the
    // frames that read them are done).
    const uint32_t f = frame.gpu.device.frames().frameIndex();
    // (A list grows to a quarter more than it needs, so a turning view does
    // not regrow it every frame.)
    const auto ensure = [&](SceneVkBuffer& buf, VkDeviceSize need, VkDeviceSize room, VkBufferUsageFlags usage) {
        if (buf.isValid() && buf.size >= need) return true;
        if (buf.isValid()) frame.gpu.allocator.destroyBuffer(buf);
        if (!frame.gpu.allocator.createBuffer(room, usage, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, buf)) {
            buf = {};
            return false;
        }
        return true;
    };
    const VkDeviceSize listBytes = entries * kEntryBytes;
    const VkDeviceSize commandBytes = VkDeviceSize{4} * kCommandWords * rings.size();
    if (!ensure(n.list[f], listBytes, listBytes + listBytes / 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT) ||
        !ensure(n.commands[f], commandBytes, commandBytes,
                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
                    VK_BUFFER_USAGE_TRANSFER_DST_BIT))
        return false;

    // The runs, each with the ring's cells before it.
    const VkDeviceSize runBytes = runCount * sizeof(CoverRun);
    const render::UploadSlice slice = frame.gpu.device.frameUpload(runBytes, storageAlign_);
    if (!slice) return false;
    auto* dst = static_cast<CoverRun*>(slice.mapped);
    VkDescriptorSet listSet = frame.gpu.device.frameSet(frame.gpu.defaults.boneLayout);
    if (listSet == VK_NULL_HANDLE) return false;
    SceneVkDescriptorWriter writer;
    writer.writeBuffer(1, n.list[f].buffer, n.list[f].size, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
    writer.writeBuffer(2, n.commands[f].buffer, n.commands[f].size, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
    writer.writeBuffer(3, slice.buffer, runBytes, slice.offset, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
    writer.updateSet(device_, listSet);

    Commands cmds;
    cmds.buffer = n.commands[f].buffer;
    uint32_t run = 0, first = 0;
    for (size_t i = 0; i < rings.size(); ++i) {
        const Ring& r = rings[i];
        Job job;
        job.listSet = listSet;
        job.set = r.set;
        job.firstRun = run;
        job.runCount = static_cast<uint32_t>(r.runs->size());
        job.perCell = r.perCell;
        job.count = static_cast<uint32_t>(i) * kCommandWords + 1;
        job.first = first;
        job.kind = static_cast<uint32_t>(r.kind);
        uint32_t cells = 0;
        for (const auto& rr : *r.runs) {
            CoverRun& c = dst[run++];
            std::memcpy(c.rect, rr.data(), sizeof(c.rect));
            c.before[0] = cells;
            c.before[1] = c.before[2] = c.before[3] = 0;
            cells += static_cast<uint32_t>(rr[2]) * static_cast<uint32_t>(rr[3]);
        }
        job.candidates = cells * r.perCell;
        jobs_.push_back(job);
        // Its command: the ring's index count, no instances yet, its entries
        // from `first` on.
        cmds.words.insert(cmds.words.end(), {r.indexCount, 0u, 0u, 0u, first});
        out.push_back({n.commands[f].buffer, VkDeviceSize{4} * kCommandWords * i, listSet});
        first += job.candidates;
    }
    commands_.push_back(std::move(cmds));
    return true;
}

void CoverPlan::record(SceneFrame& frame) {
    if (jobs_.empty()) {
        commands_.clear();
        return;
    }
    VkCommandBuffer cmd = frame.cmd;
    // The commands as each frame starts them (the frames before this slot's
    // last use read them, and are done).
    for (const Commands& c : commands_)
        vkCmdUpdateBuffer(cmd, c.buffer, 0, c.words.size() * 4, c.words.data());
    render::cmdMemoryBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout_, 0, 1, &frame.cameraSet, 0, nullptr);
    VkDescriptorSet bound = VK_NULL_HANDLE;
    for (const Job& j : jobs_) {
        if (j.listSet != bound) {
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout_, 3, 1, &j.listSet, 0, nullptr);
            bound = j.listSet;
        }
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout_, 4, 1, &j.set, 0, nullptr);
        CoverPush p{j.firstRun, j.runCount, j.perCell, j.candidates, j.count, j.first, j.kind, 0};
        // (A ring past a dispatch's most workgroups goes in several.)
        for (p.start = 0; p.start < j.candidates; p.start += kMaxGroups * kGroup) {
            vkCmdPushConstants(cmd, layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(p), &p);
            const uint32_t left = j.candidates - p.start;
            vkCmdDispatch(cmd, std::min((left + kGroup - 1) / kGroup, kMaxGroups), 1, 1);
        }
    }
    render::cmdMemoryBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                             VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT,
                             VK_ACCESS_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_SHADER_READ_BIT);
    if (frame.timer) frame.timer->mark(cmd, "cover:plan");
    jobs_.clear();
    commands_.clear();
}

}  // namespace bro::scene::vk
