#include "scene/vulkan/world/scene_world_cells_pass.h"

#include "render/vulkan_pipeline_cache.h"
#include "scene/vulkan/frame/scene_frame.h"
#include "scene/vulkan/frame/scene_vk_descriptors.h"
#include "scene/vulkan/frame/scene_vk_device.h"
#include "scene/vulkan/frame/scene_vk_shader_compiler.h"
#include "scene/vulkan/frame/scene_vk_pipeline.h"
#include "scene/vulkan/frame/scene_shader_sets.h"
#include "scene/vulkan/shaders/world/world_cells_layout.h"
#include "scene/vulkan/shaders/world/world_horizon_layout.h"
#include "util/log.h"

#include <algorithm>
#include <cstring>

namespace bro::scene::vk {

namespace {

uint32_t bitsOf(float f) {
    uint32_t u;
    std::memcpy(&u, &f, sizeof(u));
    return u;
}

constexpr int kSteps = 3;
constexpr const char* kStepNames[kSteps] = {"world cells top", "world cells leaf", "world cells gather"};

void computeBarrier(VkCommandBuffer cmd) {
    VkMemoryBarrier b{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    b.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_INDIRECT_COMMAND_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT, 0, 1, &b, 0,
                         nullptr, 0, nullptr);
}

}  // namespace

bool WorldCellsPass::setup(SceneGpu& gpu) {
    device_ = gpu.device.device();
    uploadAlign_ = std::max<VkDeviceSize>(gpu.device.context().deviceProperties().limits.minStorageBufferOffsetAlignment, 16);
    SceneVkDescriptorLayoutBuilder builder;
    for (uint32_t b = 0; b < 5; ++b) builder.addBinding(b, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT);
    setLayout_ = builder.build(device_);
    VkPipelineLayoutCreateInfo pi{};
    pi.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pi.setLayoutCount = 1;
    pi.pSetLayouts = &setLayout_;
    if (!setLayout_ || vkCreatePipelineLayout(device_, &pi, nullptr, &layout_) != VK_SUCCESS) {
        LOG_ERROR("world cells: failed creating the pass's layout");
        return false;
    }
    const BuiltinSceneShader shaders[kSteps] = {BuiltinSceneShader::WorldCellsTopComp,
                                                BuiltinSceneShader::WorldCellsLeafComp,
                                                BuiltinSceneShader::WorldCellsGatherComp};
    for (int s = 0; s < kSteps; ++s) {
        modules_[s] = SceneVkShaderCompiler::createBuiltinModule(device_, shaders[s]);
        if (!modules_[s]) return false;
        pipelines_[s] = createComputePipeline(device_, modules_[s], layout_, kStepNames[s]);
        if (!pipelines_[s]) {
            LOG_ERROR("world cells: failed creating the %s step", kStepNames[s]);
            return false;
        }
    }
    ready_ = true;
    return true;
}

void WorldCellsPass::cleanup(SceneGpu& gpu) {
    if (noHorizon_.isValid()) gpu.allocator.destroyBuffer(noHorizon_);
    for (int s = 0; s < kSteps; ++s) {
        if (pipelines_[s]) destroyScenePipeline(device_, pipelines_[s], nullptr);
        SceneVkShaderModule::destroy(device_, modules_[s]);
        pipelines_[s] = VK_NULL_HANDLE;
        modules_[s] = VK_NULL_HANDLE;
    }
    if (layout_) vkDestroyPipelineLayout(device_, layout_, nullptr);
    if (setLayout_) vkDestroyDescriptorSetLayout(device_, setLayout_, nullptr);
    layout_ = VK_NULL_HANDLE;
    setLayout_ = VK_NULL_HANDLE;
    ready_ = false;
}

void WorldCellsPass::destroy(SceneVkAllocator& allocator, Target& t) {
    for (SceneVkBuffer* b : {&t.cells, &t.out})
        if (b->isValid()) allocator.destroyBuffer(*b);
    t = Target{};
}

bool WorldCellsPass::update(SceneFrame& frame, Target& t, const WorldCells& cells, WorldCells::Provider p,
                            uint64_t generation) {
    if (!ready_) return false;
    if (t.cells.isValid() && t.generation == generation) return true;
    std::vector<uint32_t> words;
    cells.flatten(p, words);
    // (A top's leaves are a workgroup of the leaf step.)
    for (uint32_t i = 0; i < words[WC_H_TOPS]; ++i)
        if (words[words[WC_H_TOP_AT] + i * WC_CELL_WORDS + WC_CELL_COUNT] > WC_TOP_LEAVES) {
            LOG_ERROR("world cells: a top holds more than %d leaves", WC_TOP_LEAVES);
            return false;
        }
    const VkDeviceSize bytes = VkDeviceSize{words.size()} * 4;
    SceneVkAllocator& allocator = frame.gpu.allocator;
    // A new buffer each time, as the planner's static data: the last frame's
    // pass may still read the old one on the compute queue.
    if (t.cells.isValid()) allocator.destroyBuffer(t.cells);
    if (!allocator.createBuffer(bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, t.cells, /*shared=*/true) ||
        !allocator.stageAndUploadBuffer(t.cells.buffer, words.data(), bytes)) {
        if (t.cells.isValid()) allocator.destroyBuffer(t.cells);
        t.generation = ~uint64_t{0};
        return false;
    }
    frame.gpu.device.computeReadsGraphics();
    t.tops = words[WC_H_TOPS];
    t.leaves = words[WC_H_LEAVES];
    t.generation = generation;
    return true;
}

bool WorldCellsPass::begin(SceneFrame& frame, Target& t, const Views& views, const WorldCells& cells,
                           WorldCells::Provider provider) {
    if (!ready_ || !t.cells.isValid() || t.leaves == 0 || views.count == 0 || views.count > WC_MAX_VIEWS ||
        cells.content(provider).size() != t.leaves)
        return false;
    // The output's room: its runs (world_cells_layout.h), the sizes a leaf a
    // view last.
    SceneVkAllocator& allocator = frame.gpu.allocator;
    const VkDeviceSize outWords = WC_O_HEADER + VkDeviceSize{t.leaves} * (2 * WC_V_WORDS + WC_BANDS + views.count) +
                                  VkDeviceSize{t.tops} * WC_T_WORDS;
    if (!t.out.isValid() || t.out.size < outWords * 4) {
        // (The old one stays until the frames that read it are done.)
        if (t.out.isValid()) allocator.destroyBuffer(t.out);
        if (!allocator.createBuffer(outWords * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT,
                                    VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, t.out)) {
            t.out = {};
            return false;
        }
    }
    const VkDeviceSize bytes = WC_P_WORDS * 4;
    const VkDeviceSize originBytes = VkDeviceSize{t.leaves} * WC_ORIGIN_WORDS * 4;
    const render::UploadSlice slice = frame.gpu.device.frameUpload(bytes, uploadAlign_);
    const render::UploadSlice originSlice = frame.gpu.device.frameUpload(originBytes, uploadAlign_);
    if (!slice || !originSlice) return false;
    uint32_t p[WC_P_WORDS] = {};
    p[WC_P_VIEWS] = views.count;
    p[WC_P_ITEM_GROUP] = views.itemGroup;
    for (int j = 0; j < 3; ++j) p[WC_P_EYE + j] = bitsOf(static_cast<float>(views.eye[j]));
    p[WC_P_BAND_NEAR_PX] = bitsOf(views.nearPixels);
    p[WC_P_BAND_FAR_PX] = bitsOf(views.farPixels);
    for (uint32_t v = 0; v < views.count; ++v) {
        uint32_t* s = p + WC_P_SCALES + v * WC_SCALE_WORDS;
        s[WC_SCALE_FOCAL] = bitsOf(views.scales[v].focalPixels);
        s[WC_SCALE_TEXEL] = bitsOf(views.scales[v].texel);
        s[WC_SCALE_TEXEL_PER_METRE] = bitsOf(views.scales[v].texelPerMetre);
    }
    std::memcpy(p + WC_P_PLANES, views.planes, size_t{views.count} * 24 * 4);
    std::memcpy(slice.mapped, p, bytes);
    // Every leaf's origin relative to the eye, taken in double.
    cells.eyeRelativeOrigins(provider, views.eye, static_cast<float*>(originSlice.mapped));
    // The land's horizon, which the graphics queue made (the compute waits
    // for the frame that did, long finished), else one never made.
    VkBuffer horizon = VK_NULL_HANDLE;
    VkDeviceSize horizonBytes = 0;
    if (views.horizon) {
        horizon = views.horizon->buffer;
        horizonBytes = views.horizon->size;
        frame.gpu.device.computeWaitsFor(views.horizon->ticket);
    } else {
        if (!noHorizon_.isValid()) {
            const uint32_t none[WH_HEADER] = {};
            if (!allocator.createBuffer(sizeof(none), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, noHorizon_, /*shared=*/true) ||
                !allocator.stageAndUploadBuffer(noHorizon_.buffer, none, sizeof(none))) {
                if (noHorizon_.isValid()) allocator.destroyBuffer(noHorizon_);
                return false;
            }
            frame.gpu.device.computeReadsGraphics();
        }
        horizon = noHorizon_.buffer;
        horizonBytes = noHorizon_.size;
    }
    t.set = frame.gpu.device.frameSet(setLayout_);
    if (t.set == VK_NULL_HANDLE) return false;
    SceneVkDescriptorWriter writer;
    constexpr VkDescriptorType kStorage = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writer.writeBuffer(0, t.cells.buffer, t.cells.size, 0, kStorage);
    writer.writeBuffer(1, slice.buffer, bytes, slice.offset, kStorage);
    writer.writeBuffer(2, t.out.buffer, t.out.size, 0, kStorage);
    writer.writeBuffer(3, originSlice.buffer, originBytes, originSlice.offset, kStorage);
    writer.writeBuffer(4, horizon, horizonBytes, 0, kStorage);
    writer.updateSet(device_, t.set);
    return true;
}

void WorldCellsPass::record(VkCommandBuffer cmd, const std::vector<Target*>& targets) const {
    if (targets.empty()) return;
    for (int s = 0; s < kSteps; ++s) {
        if (s > 0) computeBarrier(cmd);
        bindScenePipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipelines_[s]);
        for (const Target* t : targets) {
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout_, 0, 1, &t->set, 0, nullptr);
            // (The leaf step: a workgroup a top in view, as the top step counted.)
            if (s == 1)
                vkCmdDispatchIndirect(cmd, t->out.buffer, VkDeviceSize{WC_O_TOP_DISPATCH} * 4);
            else
                vkCmdDispatch(cmd, 1, 1, 1);
        }
    }
}

}  // namespace bro::scene::vk
