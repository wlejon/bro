#include "scene/vulkan/lighting/pass_light_cull.h"
#include "scene/vulkan/frame/scene_shader_sets.h"

#include "render/vulkan_util.h"
#include "scene/scene_renderer.h"
#include "scene/vulkan/draw/scene_vk_kernel_hook.h"
#include "scene/vulkan/frame/scene_frame.h"
#include "scene/vulkan/frame/scene_gpu_resources.h"
#include "scene/vulkan/frame/scene_targets.h"
#include "scene/vulkan/frame/scene_vk_descriptors.h"
#include "scene/vulkan/frame/scene_vk_device.h"
#include "scene/vulkan/frame/scene_vk_pipeline.h"
#include "scene/vulkan/lighting/scene_lighting.h"
#include "util/log.h"

#include "light_cull.bk.src.h"

#include <bromath/mat.h>

#include <algorithm>
#include <cstring>

namespace bro::scene::vk {

namespace {

struct alignas(16) LightCullUbo {
    alignas(16) float invRelViewProj[16];
    alignas(16) float params[4];   // width, height, localCount, tilesX
    alignas(16) float params2[4];  // tilesY, 0, 0, 0
};

}  // namespace

bool PassLightCull::setup(SceneGpu& gpu) {
    VkDevice dev = gpu.device.device();
    SceneVkDescriptorLayoutBuilder b;
    b.addBinding(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT);
    b.addBinding(1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT);
    b.addBinding(2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT);
    setLayout_ = b.build(dev);
    if (!setLayout_) return false;

    VkPipelineLayoutCreateInfo li{};
    li.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    li.setLayoutCount = 1;
    li.pSetLayouts = &setLayout_;
    if (vkCreatePipelineLayout(dev, &li, nullptr, &layout_) != VK_SUCCESS) return false;

    KernelHookStage stage;
    std::string err;
    if (!compileKernelStage("lighting/light_cull.bk", kBkLightCullSrc, "light_cull", stage, err)) {
        LOG_ERROR("PassLightCull: light_cull.bk failed: %s", err.c_str());
        return false;
    }
    VkShaderModule mod = SceneVkShaderModule::create(dev, stage.spirv);
    pipeline_ = createComputePipeline(dev, mod, layout_, "light_cull", stage.entry.c_str());
    SceneVkShaderModule::destroy(dev, mod);
    return pipeline_ != VK_NULL_HANDLE;
}

void PassLightCull::cleanup(SceneGpu& gpu) {
    VkDevice dev = gpu.device.device();
    if (pipeline_) gpu.device.defer([dev, p = pipeline_] { destroyScenePipeline(dev, p, nullptr); });
    if (layout_) vkDestroyPipelineLayout(dev, layout_, nullptr);
    if (setLayout_) vkDestroyDescriptorSetLayout(dev, setLayout_, nullptr);
    pipeline_ = VK_NULL_HANDLE;
    layout_ = VK_NULL_HANDLE;
    setLayout_ = VK_NULL_HANDLE;
    for (SceneVkBuffer& b : tilesBuf_) {
        if (b.isValid()) gpu.allocator.destroyBuffer(b);
    }
}

void PassLightCull::record(SceneFrame& frame) {
    LocalLightingBuffers local{};
    uploadLocalLightingData(frame.renderer, frame.gpu, frame.view.eye, frame.lighting, local);
    frame.localLightsBuf = local.localLights;
    frame.interiorVolumesBuf = local.interiorVolumes;
    frame.lightTilesBuf = local.lightTiles;

    if (frame.lighting.localParams[0] <= 0.5f) return;

    const uint32_t w = std::max(1u, frame.gpu.targets.width());
    const uint32_t h = std::max(1u, frame.gpu.targets.height());
    const uint32_t tilesX = (w + static_cast<uint32_t>(kSceneLightTileSize) - 1u) /
                            static_cast<uint32_t>(kSceneLightTileSize);
    const uint32_t tilesY = (h + static_cast<uint32_t>(kSceneLightTileSize) - 1u) /
                            static_cast<uint32_t>(kSceneLightTileSize);
    const VkDeviceSize tileBytes =
        static_cast<VkDeviceSize>(tilesX) * tilesY * kSceneLightTileStride * sizeof(uint32_t);

    slot_ = (slot_ + 1) % kSlots;
    SceneVkBuffer& buf = tilesBuf_[static_cast<size_t>(slot_)];
    if (buf.size < tileBytes) {
        if (buf.isValid()) frame.gpu.allocator.destroyBuffer(buf);
        if (!frame.gpu.allocator.createBuffer(tileBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                              VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, buf)) {
            LOG_ERROR("PassLightCull: failed creating %llu byte tile buffer",
                      static_cast<unsigned long long>(tileBytes));
            return;
        }
    }

    LightCullUbo u{};
    const bromath::Mat4 inv = bromath::minverse(frame.view.relViewProj);
    std::memcpy(u.invRelViewProj, inv.data, sizeof(u.invRelViewProj));
    u.params[0] = static_cast<float>(w);
    u.params[1] = static_cast<float>(h);
    u.params[2] = frame.lighting.localParams[0];
    u.params[3] = static_cast<float>(tilesX);
    u.params2[0] = static_cast<float>(tilesY);

    const VkDescriptorBufferInfo ubo = frame.gpu.device.frameUniform(&u, sizeof(u));
    VkDescriptorSet set = frame.gpu.device.frameSet(setLayout_);
    if (!ubo.buffer || !set) return;

    SceneVkDescriptorWriter writer;
    writer.writeBuffer(0, ubo.buffer, ubo.range, ubo.offset);
    writer.writeBuffer(1, frame.localLightsBuf.buffer, frame.localLightsBuf.range, frame.localLightsBuf.offset,
                       VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
    writer.writeBuffer(2, buf.buffer, tileBytes, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
    writer.updateSet(frame.gpu.device.device(), set);

    bindScenePipeline(frame.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_);
    vkCmdBindDescriptorSets(frame.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout_, 0, 1, &set, 0, nullptr);
    vkCmdDispatch(frame.cmd, (tilesX + 7u) / 8u, (tilesY + 7u) / 8u, 1u);

    render::cmdBufferBarrier(frame.cmd, buf.buffer, 0, tileBytes, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                             VK_ACCESS_SHADER_READ_BIT);

    frame.lightTilesBuf = {buf.buffer, 0, tileBytes};
    frame.lighting.localParams[2] = static_cast<float>(tilesX);
    frame.lighting.localParams[3] = static_cast<float>(tilesY);
}

}  // namespace bro::scene::vk
