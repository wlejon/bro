#include "scene/vulkan/pass_shadow.h"

#include "scene/vulkan/scene_defaults.h"
#include "scene/vulkan/scene_frame.h"
#include "scene/vulkan/scene_targets.h"
#include "scene/vulkan/scene_vk_custom_shader.h"
#include "scene/vulkan/scene_vk_device.h"
#include "scene/vulkan/scene_vk_shader_compiler.h"
#include "util/log.h"

#include <array>
#include <cstring>

namespace bro::scene::vk {

namespace {

// Slope-scaled bias against acne on the conventional [0,1] shadow depth.
constexpr float kDepthBiasConstant = 1.25f;
constexpr float kDepthBiasSlope = 1.75f;

/// The vertex streams a shadow vertex shader reads: the position (with the
/// normal and uv the built-in static shader declares), the instance rows, or
/// the joints and weights. A custom vertex chunk sees every mesh attribute.
void shadowVertexInput(MeshKind kind, bool custom, std::vector<VkVertexInputBindingDescription>& bindings,
                       std::vector<VkVertexInputAttributeDescription>& attributes) {
    SceneMeshDrawer::vertexInput(kind, bindings, attributes);
    if (custom) return;
    std::vector<VkVertexInputAttributeDescription> used;
    for (const auto& a : attributes) {
        const bool keep = kind == MeshKind::Instanced ? (a.location == 0 || a.location >= 8)
                                                      : (a.location <= 2 || a.location == 5 || a.location == 6);
        if (keep) used.push_back(a);
    }
    attributes = std::move(used);
}

}  // namespace

bool PassShadow::setup(SceneGpu& gpu) {
    device_ = &gpu.device;
    VkDevice dev = gpu.device.device();
    const SceneDefaults& d = gpu.defaults;

    const std::array<VkDescriptorSetLayout, 5> sets = {d.boneLayout, d.emptyLayout, d.emptyLayout, d.emptyLayout,
                                                       d.customLayout};
    VkPushConstantRange push{VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(Push)};
    VkPipelineLayoutCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    info.setLayoutCount = static_cast<uint32_t>(sets.size());
    info.pSetLayouts = sets.data();
    info.pushConstantRangeCount = 1;
    info.pPushConstantRanges = &push;
    if (vkCreatePipelineLayout(dev, &info, nullptr, &layout_) != VK_SUCCESS) return false;

    vs_[static_cast<int>(MeshKind::Static)] =
        SceneVkShaderCompiler::createBuiltinModule(dev, BuiltinSceneShader::ShadowVert);
    vs_[static_cast<int>(MeshKind::Instanced)] =
        SceneVkShaderCompiler::createBuiltinModule(dev, BuiltinSceneShader::ShadowInstancedVert);
    vs_[static_cast<int>(MeshKind::Skinned)] =
        SceneVkShaderCompiler::createBuiltinModule(dev, BuiltinSceneShader::ShadowSkinnedVert);
    fs_ = SceneVkShaderCompiler::createBuiltinModule(dev, BuiltinSceneShader::ShadowFrag);
    return vs_[0] && vs_[1] && vs_[2] && fs_;
}

void PassShadow::cleanup(SceneGpu& gpu) {
    VkDevice dev = gpu.device.device();
    std::vector<VkPipeline> pipelines(std::begin(builtin_), std::end(builtin_));
    for (auto& [key, p] : custom_) pipelines.push_back(p);
    gpu.device.defer([dev, pipelines] {
        for (VkPipeline p : pipelines) {
            if (p != VK_NULL_HANDLE) vkDestroyPipeline(dev, p, nullptr);
        }
    });
    custom_.clear();
    for (VkPipeline& p : builtin_) p = VK_NULL_HANDLE;
    for (VkShaderModule& m : vs_) {
        SceneVkShaderModule::destroy(dev, m);
        m = VK_NULL_HANDLE;
    }
    SceneVkShaderModule::destroy(dev, fs_);
    fs_ = VK_NULL_HANDLE;
    if (layout_ != VK_NULL_HANDLE) vkDestroyPipelineLayout(dev, layout_, nullptr);
    layout_ = VK_NULL_HANDLE;
}

VkPipeline PassShadow::buildPipeline(VkShaderModule vs, MeshKind kind, bool custom) {
    std::vector<VkVertexInputBindingDescription> bindings;
    std::vector<VkVertexInputAttributeDescription> attributes;
    shadowVertexInput(kind, custom, bindings, attributes);

    TargetFormat target;
    target.depth = SceneTargets::kDepthFormat;

    SceneVkPipelineBuilder b;
    b.setShaderStages(vs, fs_)
     .setVertexInput(bindings, attributes)
     .setInputTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
     .setPolygonMode(VK_POLYGON_MODE_FILL)
     .setCullMode(VK_CULL_MODE_NONE, VK_FRONT_FACE_COUNTER_CLOCKWISE)
     .setTarget(target)
     .disableBlending(0)
     // Shadow maps are outside the camera depth policy: always conventional.
     .enableDepthTest(true, VK_COMPARE_OP_LESS_OR_EQUAL)
     .setDynamicStates({VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_DEPTH_BIAS});
    return b.build(device_->device(), layout_);
}

VkPipeline PassShadow::builtinPipeline(MeshKind kind) {
    VkPipeline& p = builtin_[static_cast<int>(kind)];
    if (p == VK_NULL_HANDLE) p = buildPipeline(vs_[static_cast<int>(kind)], kind, false);
    return p;
}

VkPipeline PassShadow::customPipeline(const CustomShaderState& cs, MeshKind kind) {
    const std::string key = cs.key + (kind == MeshKind::Skinned ? "_skin" : "_static");
    auto it = custom_.find(key);
    if (it != custom_.end()) return it->second;

    VkPipeline pipeline = VK_NULL_HANDLE;
    VkShaderModule vs = VK_NULL_HANDLE;
    std::string err;
    if (SceneVkCustomShader::compileCustomShadowShaderModule(device_->device(), kind == MeshKind::Skinned,
                                                             cs.vertexChunk, vs, err)) {
        pipeline = buildPipeline(vs, kind, true);
        SceneVkShaderModule::destroy(device_->device(), vs);
    }
    if (pipeline == VK_NULL_HANDLE) {
        LOG_ERROR("PassShadow: Failed building a custom shadow pipeline: %s", err.c_str());
    }
    custom_.emplace(key, pipeline);
    return pipeline;
}

bool PassShadow::active(const SceneFrame& frame) const {
    return frame.shadowed;
}

void PassShadow::declare(const SceneFrame& frame, PassIO& io) const {
    io.depthTarget(frame.gpu.targets.shadow);
}

void PassShadow::record(SceneFrame& frame) {
    VkCommandBuffer cmd = frame.cmd;
    const uint32_t res = SceneTargets::kShadowResolution;

    VkRenderingAttachmentInfo depth{};
    depth.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    depth.imageView = frame.gpu.targets.shadowCascadeViews[0];
    depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    depth.clearValue.depthStencil = {1.0f, 0};
    VkRenderingInfo info{};
    info.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    info.renderArea = {{0, 0}, {res, res}};
    info.layerCount = 1;
    info.pDepthAttachment = &depth;
    frame.gpu.device.cmdBeginRendering(cmd, &info);

    const VkViewport viewport{0.0f, 0.0f, static_cast<float>(res), static_cast<float>(res), 0.0f, 1.0f};
    const VkRect2D scissor{{0, 0}, {res, res}};
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    vkCmdSetDepthBias(cmd, kDepthBiasConstant, 0.0f, kDepthBiasSlope);

    bromath::Mat4 lightVP;
    std::memcpy(lightVP.data, frame.lighting.shadowCascadeProj, sizeof(lightVP.data));

    for (const MeshDraw& draw : frame.lists.meshes) {
        if (!draw.castsShadow) continue;

        VkPipeline pipeline = VK_NULL_HANDLE;
        const bool custom = draw.custom && draw.customSet && !draw.custom->vertexChunk.empty() &&
                            draw.kind != MeshKind::Instanced;
        if (custom) pipeline = customPipeline(*draw.custom, draw.kind);
        if (!pipeline) pipeline = builtinPipeline(draw.kind);
        if (!pipeline) continue;

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        if (draw.kind == MeshKind::Skinned)
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0, 1, &draw.boneSet, 0, nullptr);
        if (custom)
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 4, 1, &draw.customSet, 0, nullptr);

        Push push{};
        bromath::Mat4 model;
        std::memcpy(model.data, draw.push.model, sizeof(model.data));
        const bromath::Mat4 mvp = bromath::mmul(lightVP, model);
        std::memcpy(push.lightMVP, mvp.data, sizeof(push.lightMVP));
        vkCmdPushConstants(cmd, layout_, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(push), &push);

        const VkBuffer buffers[2] = {draw.vertices, draw.kind == MeshKind::Instanced ? draw.instances : draw.skin};
        const VkDeviceSize offsets[2] = {0, draw.kind == MeshKind::Instanced ? draw.instanceOffset : 0};
        vkCmdBindVertexBuffers(cmd, 0, draw.kind == MeshKind::Static ? 1 : 2, buffers, offsets);
        vkCmdBindIndexBuffer(cmd, draw.indices, 0, VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(cmd, draw.indexCount, draw.instanceCount, 0, 0, 0);
        if (frame.stats.shadowTilesTotal == 0) frame.stats.shadowDrawn++;
    }

    frame.gpu.device.cmdEndRendering(cmd);
}

}  // namespace bro::scene::vk
