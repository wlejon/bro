#include "scene/vulkan/shadow/pass_shadow_mark.h"
#include "scene/vulkan/frame/scene_shader_sets.h"

#include "render/vulkan_util.h"
#include "scene/nodes/light_node.h"
#include "scene/scene_renderer.h"
#include "scene/vulkan/frame/scene_frame.h"
#include "scene/vulkan/frame/scene_targets.h"
#include "scene/vulkan/frame/scene_vk_device.h"
#include "scene/vulkan/frame/scene_vk_pipeline.h"
#include "scene/vulkan/frame/scene_vk_shader_compiler.h"
#include "scene/vulkan/lighting/scene_lighting.h"
#include "scene/vulkan/shadow/scene_shadow_pages.h"

#include <cstring>

namespace bro::scene::vk {

bool PassShadowMark::setup(SceneGpu& gpu) {
    VkDevice dev = gpu.device.device();
    VkSamplerCreateInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    si.magFilter = si.minFilter = VK_FILTER_NEAREST;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.maxAnisotropy = 1.0f;
    if (vkCreateSampler(dev, &si, nullptr, &sampler_) != VK_SUCCESS) return false;
    SceneVkDescriptorLayoutBuilder b;
    b.addBinding(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT);
    b.addBinding(1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT);
    b.addBinding(2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT);
    setLayout_ = b.build(dev);
    if (!setLayout_) return false;
    VkPipelineLayoutCreateInfo li{};
    li.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    li.setLayoutCount = 1;
    li.pSetLayouts = &setLayout_;
    if (vkCreatePipelineLayout(dev, &li, nullptr, &layout_) != VK_SUCCESS) return false;
    pipeline_ = createComputePipeline(dev, BuiltinSceneShader::ShadowMarkComp, layout_, "shadow_mark");
    return pipeline_ != VK_NULL_HANDLE;
}

void PassShadowMark::cleanup(SceneGpu& gpu) {
    VkDevice dev = gpu.device.device();
    if (pipeline_) gpu.device.defer([dev, p = pipeline_] { destroyScenePipeline(dev, p, nullptr); });
    if (layout_) vkDestroyPipelineLayout(dev, layout_, nullptr);
    if (setLayout_) vkDestroyDescriptorSetLayout(dev, setLayout_, nullptr);
    if (sampler_) gpu.device.defer([dev, s = sampler_] { vkDestroySampler(dev, s, nullptr); });
    pipeline_ = VK_NULL_HANDLE;
    layout_ = VK_NULL_HANDLE;
    setLayout_ = VK_NULL_HANDLE;
    sampler_ = VK_NULL_HANDLE;
}

bool PassShadowMark::active(const SceneFrame& frame) const {
    return frame.gpu.shadows.ready() && frame.renderer.shadowPlan().mapCount > 0;
}

void PassShadowMark::declare(const SceneFrame& frame, PassIO& io) const { io.sample(frame.gpu.targets.depth); }

void PassShadowMark::record(SceneFrame& frame) {
    const ShadowPlan& plan = frame.renderer.shadowPlan();
    SceneShadowPages& pages = frame.gpu.shadows;
    const SceneView& view = frame.view;
    VkCommandBuffer cmd = frame.cmd;

    // The block: the camera's inverse, the footprint, the lights' maps.
    SceneShadowMarkUniforms u{};
    const bromath::Mat4 inv = bromath::minverse(view.relViewProj);
    std::memcpy(u.invViewProj, inv.data, sizeof(u.invViewProj));
    u.grid[0] = static_cast<float>(frame.gpu.targets.width());
    u.grid[1] = static_cast<float>(frame.gpu.targets.height());
    u.grid[2] = static_cast<float>(kStep);
    u.view[0] = plan.pixelAngle;
    u.view[1] = plan.orthoPixel;
    u.view[2] = plan.finestTexel;
    u.view[3] = static_cast<float>(plan.levels);
    float soft[4];
    shadowSoftUniform(frame.renderer, soft);
    u.soft[0] = soft[2];
    const std::vector<LightNode*>& lights = frame.renderer.frameLights();
    int count = 0;
    for (int i = 0; i < static_cast<int>(lights.size()) && i < ShadowPlan::kMaxLights; ++i) {
        if (plan.lightMap[i] < 0 || plan.lightMapCount[i] <= 0) continue;
        const LightNode& l = *lights[i];
        const int kind = l.kind() == LightNode::Kind::Directional ? 0 : l.kind() == LightNode::Kind::Spot ? 1 : 2;
        u.lights[count][0] = static_cast<float>(plan.lightMap[i]);
        u.lights[count][1] = static_cast<float>(plan.lightMapCount[i]);
        u.lights[count][2] = static_cast<float>(kind);
        u.lights[count][3] = plan.lightReach[i];
        const bromath::Mat4& m = l.worldMatrix();
        u.lightPos[count][0] = m.at(0, 3) - view.eye.x;
        u.lightPos[count][1] = m.at(1, 3) - view.eye.y;
        u.lightPos[count][2] = m.at(2, 3) - view.eye.z;
        u.lightPos[count][3] = l.range() > 0.0f ? l.range() : 1e30f;
        ++count;
    }
    for (int m = 0; m < plan.mapCount && m < kSceneMaxShadowMaps; ++m)
        shadowMapUniform(plan, plan.maps[m], frame.world.eye, u.maps[m]);   // (the pages are the body's)
    // The sun's next clipmap, while it is built: one more sun, its levels
    // past the sampled maps.
    if (plan.nextCount > 0 && count < kSceneMaxLights) {
        const int light = plan.next[0].light;
        u.lights[count][0] = static_cast<float>(kSceneMaxShadowMaps);
        u.lights[count][1] = static_cast<float>(plan.nextCount);
        u.lights[count][2] = 0.0f;
        u.lights[count][3] = light >= 0 && light < ShadowPlan::kMaxLights ? plan.lightReach[light] : 0.0f;
        ++count;
        for (int m = 0; m < plan.nextCount; ++m)
            shadowMapUniform(plan, plan.next[m], frame.world.eye, u.maps[kSceneMaxShadowMaps + m]);
    }
    u.grid[3] = static_cast<float>(count);

    const VkDescriptorBufferInfo ubo = frame.gpu.device.frameUniform(&u, sizeof(u));
    VkDescriptorSet set = frame.gpu.device.frameSet(setLayout_);
    if (!ubo.buffer || !set) return;
    SceneVkDescriptorWriter w;
    w.writeBuffer(0, ubo.buffer, ubo.range, ubo.offset);
    w.writeImage(1, frame.gpu.targets.depth.view, sampler_);
    w.writeBuffer(2, pages.requestBuffer(), pages.requestBytes(), 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
    w.updateSet(frame.gpu.device.device(), set);

    // Cleared (after the last frame's copy out read it), marked, copied out.
    render::cmdBufferBarrier(cmd, pages.requestBuffer(), 0, pages.requestBytes(), VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    vkCmdFillBuffer(cmd, pages.requestBuffer(), 0, pages.requestBytes(), 0u);
    render::cmdBufferBarrier(cmd, pages.requestBuffer(), 0, pages.requestBytes(), VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
    bindScenePipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout_, 0, 1, &set, 0, nullptr);
    const uint32_t cols = (frame.gpu.targets.width() + kStep - 1) / kStep;
    const uint32_t rows = (frame.gpu.targets.height() + kStep - 1) / kStep;
    vkCmdDispatch(cmd, (cols + 7) / 8, (rows + 7) / 8, 1);
    pages.recordReadback(cmd, plan.serial);
}

}  // namespace bro::scene::vk
