#include "scene/vulkan/ground/scene_land_process.h"
#include "scene/vulkan/frame/scene_shader_sets.h"

#include "render/vulkan_queue.h"
#include "render/vulkan_util.h"
#include "scene/nodes/land_node.h"
#include "scene/nodes/world_module.h"
#include "scene/vulkan/frame/scene_frame.h"
#include "scene/vulkan/frame/scene_vk_descriptors.h"
#include "scene/vulkan/frame/scene_vk_device.h"
#include "scene/vulkan/frame/scene_vk_pipeline.h"
#include "scene/vulkan/shaders/ground/land_process_layout.h"

#include "util/log.h"

#include <algorithm>
#include <cstring>

namespace bro::scene::vk {

namespace {

static_assert(sizeof(LandGrid::HaloCopy) == 16, "a halo copy is the shader's uvec4");

constexpr VkFormat kGridFormat = VK_FORMAT_R32G32B32A32_SFLOAT;
constexpr VkDeviceSize kTexelBytes = 16;
// The halo's copies run in rows of kHaloGroupsX workgroups (land_process.comp).
constexpr uint32_t kHaloGroupsX = 64;

void computeBarrier(VkCommandBuffer cmd) {
    VkMemoryBarrier b{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    b.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &b,
                         0, nullptr, 0, nullptr);
}

}  // namespace

bool SceneLandProcesses::setup(SceneGpu& gpu) {
    device_ = gpu.device.device();
    SceneVkDescriptorLayoutBuilder set;
    set.addBinding(0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT);
    set.addBinding(1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT);
    set.addBinding(2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT);
    set.addBinding(3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT);
    setLayout_ = set.build(device_);
    const VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, LP_P_WORDS * 4};
    VkPipelineLayoutCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    info.setLayoutCount = 1;
    info.pSetLayouts = &setLayout_;
    info.pushConstantRangeCount = 1;
    info.pPushConstantRanges = &push;
    if (!setLayout_ || vkCreatePipelineLayout(device_, &info, nullptr, &layout_) != VK_SUCCESS ||
        !gpu.allocator.createStorageImage(1, 1, 1, kGridFormat, none_, false, 1)) {
        LOG_ERROR("SceneLandProcesses: failed creating the processes' layout");
        return false;
    }
    return true;
}

void SceneLandProcesses::cleanup(SceneGpu& gpu) {
    for (auto& [id, run] : runs_) release(gpu, *run);
    runs_.clear();
    for (VkPipeline p : pipelines_)
        if (p) destroyScenePipeline(device_, p, nullptr);
    pipelines_.clear();
    for (auto& s : sets_) s->destroy(device_);
    sets_.clear();
    byHash_.clear();
    if (none_.isValid()) gpu.allocator.destroyImage(none_);
    if (layout_) vkDestroyPipelineLayout(device_, layout_, nullptr);
    if (setLayout_) vkDestroyDescriptorSetLayout(device_, setLayout_, nullptr);
    layout_ = VK_NULL_HANDLE;
    setLayout_ = VK_NULL_HANDLE;
}

void SceneLandProcesses::release(SceneGpu& gpu, Run& run) {
    for (SceneVkImage& g : run.grids)
        if (g.isValid()) gpu.allocator.destroyImage(g);
    for (SceneVkBuffer* b : {&run.halo, &run.data, &run.readback})
        if (b->isValid()) gpu.allocator.destroyBuffer(*b);
}

void SceneLandProcesses::forget(SceneGpu& gpu, uint32_t id) {
    const auto it = runs_.find(id);
    if (it == runs_.end()) return;
    release(gpu, *it->second);
    runs_.erase(it);
}

VkPipeline SceneLandProcesses::pipeline(const LandNode& node) {
    const std::string dispatch = node.processDispatch();
    const std::string grid = node.gridConfig();
    const LandProcess& p = node.process();
    const uint64_t key = scene::moduleSourceHash(p.source) ^ (scene::moduleSourceHash(dispatch) * 0x9e3779b97f4a7c15ull) ^
                         (scene::moduleSourceHash(grid) * 0xc2b2ae3d27d4eb4full);
    auto it = byHash_.find(key);
    if (it == byHash_.end()) {
        // Compiled when the land's processes first run (through the SPIR-V
        // cache); processes that do not compile are logged once, and the
        // land is never ready.
        auto set = std::make_unique<LandProcessShaders>();
        std::string err;
        int index = -1;
        if (set->create(device_, p.source, dispatch, grid, err)) {
            index = static_cast<int>(sets_.size());
            set->index = static_cast<uint32_t>(index);
            sets_.push_back(std::move(set));
            pipelines_.push_back(
                createComputePipeline(device_, (*sets_.back())[LandProcessShaders::Run], layout_, "land process"));
        } else {
            LOG_ERROR("Land node %u: its processes did not compile, so it draws no land:\n%s", node.id(), err.c_str());
        }
        it = byHash_.emplace(key, index).first;
    }
    return it->second < 0 ? VK_NULL_HANDLE : pipelines_[static_cast<size_t>(it->second)];
}

bool SceneLandProcesses::start(SceneGpu& gpu, const LandNode& node, Run& run) {
    const LandGrid& grid = node.grid();
    const LandProcess& p = node.process();
    run.pipeline = pipeline(node);
    if (!run.pipeline) return false;
    const uint32_t side = static_cast<uint32_t>(grid.side());
    const uint32_t layers = static_cast<uint32_t>(grid.roots() * p.channels);
    for (SceneVkImage& g : run.grids)
        if (!gpu.allocator.createStorageImage(side, side, 1, kGridFormat, g, false, layers)) return false;
    // The halo's copies and the data, uploaded once.
    const std::vector<LandGrid::HaloCopy>& halo = grid.halo();
    run.haloCopies = static_cast<uint32_t>(halo.size());
    const VkDeviceSize haloBytes = std::max<VkDeviceSize>(halo.size() * sizeof(LandGrid::HaloCopy), 16);
    std::vector<float> data(LandProcess::kMaxData * 4, 0.0f);
    std::copy_n(p.data.begin(), std::min(p.data.size(), data.size()), data.begin());
    constexpr VkBufferUsageFlags kStorage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if (!gpu.allocator.createBuffer(haloBytes, kStorage, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, run.halo) ||
        !gpu.allocator.createBuffer(data.size() * sizeof(float), kStorage, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                                    run.data) ||
        (!halo.empty() && !gpu.allocator.stageAndUploadBuffer(run.halo.buffer, halo.data(),
                                                              halo.size() * sizeof(LandGrid::HaloCopy))) ||
        !gpu.allocator.stageAndUploadBuffer(run.data.buffer, data.data(), data.size() * sizeof(float)))
        return false;
    // Channel 0 of every root, for the CPU.
    if (!gpu.allocator.createBuffer(grid.texels() * kTexelBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                    gpu.allocator.readbackMemory(), run.readback) ||
        !run.readback.mappedData)
        return false;
    return true;
}

void SceneLandProcesses::record(SceneFrame& frame, const LandNode& node, Run& run, VkCommandBuffer cmd) {
    const LandGrid& grid = node.grid();
    const LandProcess& p = node.process();
    // A set writing each grid (reading the other).
    VkDescriptorSet sets[2];
    for (int k = 0; k < 2; ++k) {
        sets[k] = frame.gpu.device.frameSet(setLayout_);
        if (sets[k] == VK_NULL_HANDLE) return;
        SceneVkDescriptorWriter writer;
        writer.writeImage(0, run.grids[k ^ 1].view, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_GENERAL,
                          VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
        writer.writeImage(1, run.grids[k].view, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_GENERAL,
                          VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
        writer.writeBuffer(2, run.halo.buffer, run.halo.size, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
        writer.writeBuffer(3, run.data.buffer, run.data.size, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
        writer.updateSet(device_, sets[k]);
    }
    bindScenePipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, run.pipeline);
    const uint32_t groups = (static_cast<uint32_t>(grid.cells()) + LP_GROUP - 1) / LP_GROUP;
    const uint32_t haloRows = (run.haloCopies + kHaloGroupsX * LP_GROUP - 1) / (kHaloGroupsX * LP_GROUP);
    const uint32_t haloGroupsY = std::max<uint32_t>((haloRows + LP_GROUP - 1) / LP_GROUP, 1);
    // One mode into grid `k`, then its halo.
    const auto dispatch = [&](uint32_t mode, int k, uint32_t pass, uint32_t iteration) {
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout_, 0, 1, &sets[k], 0, nullptr);
        uint32_t words[LP_P_WORDS] = {mode, pass, iteration, run.haloCopies};
        vkCmdPushConstants(cmd, layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(words), words);
        vkCmdDispatch(cmd, groups, groups, static_cast<uint32_t>(grid.roots()));
        computeBarrier(cmd);
        words[LP_P_MODE] = LP_MODE_HALO;
        vkCmdPushConstants(cmd, layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(words), words);
        if (run.haloCopies) vkCmdDispatch(cmd, kHaloGroupsX, haloGroupsY, 1);
        computeBarrier(cmd);
        run.current = k;
    };
    // (The grids' first barrier: the upload stream cleared them.)
    computeBarrier(cmd);
    int budget = kDispatchesPerFrame / 2;
    if (!run.started) {
        dispatch(LP_MODE_INIT, 0, 0, 0);
        run.started = true;
        --budget;
    }
    while (budget > 0 && run.pass < p.passes.size()) {
        if (run.iteration >= p.passes[run.pass].iterations) {
            ++run.pass;
            run.iteration = 0;
            continue;
        }
        dispatch(LP_MODE_PASS, run.current ^ 1, static_cast<uint32_t>(run.pass), static_cast<uint32_t>(run.iteration));
        ++run.iteration;
        --budget;
    }
    if (run.pass < p.passes.size()) return;

    // Done: channel 0 of each root to the CPU.
    VkMemoryBarrier b{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    b.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &b, 0,
                         nullptr, 0, nullptr);
    std::vector<VkBufferImageCopy> regions(static_cast<size_t>(grid.roots()));
    for (int r = 0; r < grid.roots(); ++r) {
        VkBufferImageCopy& c = regions[static_cast<size_t>(r)];
        c = {};
        c.bufferOffset = static_cast<VkDeviceSize>(r) * grid.rootTexels() * kTexelBytes;
        c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, static_cast<uint32_t>(r * p.channels), 1};
        c.imageExtent = {static_cast<uint32_t>(grid.side()), static_cast<uint32_t>(grid.side()), 1};
    }
    vkCmdCopyImageToBuffer(cmd, run.grids[run.current].image, VK_IMAGE_LAYOUT_GENERAL, run.readback.buffer,
                           static_cast<uint32_t>(regions.size()), regions.data());
    render::cmdBufferBarrier(cmd, run.readback.buffer, 0, VK_WHOLE_SIZE, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_READ_BIT);
    // (The fills that read the grid come after this in the same stream.)
    run.state = Run::State::Copied;
}

VkImageView SceneLandProcesses::update(SceneFrame& frame, LandNode& node, VkCommandBuffer cmd) {
    if (!node.process().active()) {
        forget(frame.gpu, node.id());
        return none_.view;
    }
    auto& slot = runs_[node.id()];
    if (slot && slot->serial != node.gridSerial()) {
        release(frame.gpu, *slot);
        slot.reset();
    }
    if (!slot) {
        slot = std::make_unique<Run>();
        slot->serial = node.gridSerial();
        if (!start(frame.gpu, node, *slot)) {
            LOG_ERROR("Land node %u: its processes could not be run, so it draws no land", node.id());
            slot->state = Run::State::Failed;
        }
    }
    Run& run = *slot;
    switch (run.state) {
    case Run::State::Running:
        record(frame, node, run, cmd);
        return run.state == Run::State::Running ? VK_NULL_HANDLE : run.grids[run.current].view;
    case Run::State::Reading:
        if (frame.gpu.device.context().queue().isComplete(run.ticket)) {
            // Channel 0, texel by texel.
            const auto* texels = static_cast<const float*>(run.readback.mappedData);
            std::vector<float> channel0(texels, texels + node.grid().texels() * 4);
            frame.gpu.allocator.destroyBuffer(run.readback);
            // The grid it did not end in is no longer read.
            frame.gpu.allocator.destroyImage(run.grids[run.current ^ 1]);
            run.state = Run::State::Done;
            node.setGridBase(run.serial, std::move(channel0));
        }
        return run.grids[run.current].view;
    case Run::State::Copied:
    case Run::State::Done:
        return run.grids[run.current].view;
    case Run::State::Failed:
        return VK_NULL_HANDLE;
    }
    return VK_NULL_HANDLE;
}

void SceneLandProcesses::submitted(uint64_t ticket) {
    for (auto& [id, run] : runs_)
        if (run->state == Run::State::Copied) {
            run->ticket = ticket;
            run->state = Run::State::Reading;
        }
}

}  // namespace bro::scene::vk
