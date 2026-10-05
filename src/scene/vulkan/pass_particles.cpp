#include "scene/vulkan/pass_particles.h"

#include "scene/particles3d_node.h"
#include "scene/scene_graph.h"
#include "scene/vulkan/scene_defaults.h"
#include "scene/vulkan/scene_frame.h"
#include "scene/vulkan/scene_gpu_resources.h"
#include "scene/vulkan/scene_targets.h"
#include "scene/vulkan/scene_vk_device.h"
#include "scene/vulkan/scene_vk_pipeline.h"
#include "scene/vulkan/scene_vk_shader_compiler.h"

#include <cstring>
#include <vector>

namespace bro::scene::vk {

namespace {

constexpr uint32_t kInstanceFloats = 10;   // position, size, colour, rotation, frame

}  // namespace

bool PassParticles::setup(SceneGpu& gpu) {
    device_ = &gpu.device;
    VkDevice dev = gpu.device.device();

    SceneVkDescriptorLayoutBuilder builder;
    builder.addBinding(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT);
    builder.addBinding(1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT);
    materialLayout_ = builder.build(dev);
    if (!materialLayout_) return false;

    const VkDescriptorSetLayout sets[2] = {gpu.defaults.cameraLayout, materialLayout_};
    VkPushConstantRange push{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(Push)};
    VkPipelineLayoutCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    info.setLayoutCount = 2;
    info.pSetLayouts = sets;
    info.pushConstantRangeCount = 1;
    info.pPushConstantRanges = &push;
    if (vkCreatePipelineLayout(dev, &info, nullptr, &layout_) != VK_SUCCESS) return false;

    vs_ = SceneVkShaderCompiler::createBuiltinModule(dev, BuiltinSceneShader::ParticlesVert);
    fs_ = SceneVkShaderCompiler::createBuiltinModule(dev, BuiltinSceneShader::ParticlesFrag);
    return vs_ && fs_;
}

void PassParticles::cleanup(SceneGpu& gpu) {
    VkDevice dev = gpu.device.device();
    pipelines_.destroy(gpu.device);
    SceneVkShaderModule::destroy(dev, vs_);
    SceneVkShaderModule::destroy(dev, fs_);
    vs_ = fs_ = VK_NULL_HANDLE;
    if (layout_) vkDestroyPipelineLayout(dev, layout_, nullptr);
    if (materialLayout_) vkDestroyDescriptorSetLayout(dev, materialLayout_, nullptr);
    layout_ = VK_NULL_HANDLE;
    materialLayout_ = VK_NULL_HANDLE;
}

VkPipeline PassParticles::pipeline(const TargetFormat& target, bool additive) {
    return pipelines_.get(additive ? 1 : 0, target, [&] {
        const VkVertexInputBindingDescription binding{0, kInstanceFloats * sizeof(float),
                                                      VK_VERTEX_INPUT_RATE_INSTANCE};
        const std::vector<VkVertexInputAttributeDescription> attributes = {
            {0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0},                     // position
            {1, 0, VK_FORMAT_R32_SFLOAT, 3 * sizeof(float)},           // size
            {2, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 4 * sizeof(float)},  // colour
            {3, 0, VK_FORMAT_R32_SFLOAT, 8 * sizeof(float)},           // rotation
            {4, 0, VK_FORMAT_R32_SFLOAT, 9 * sizeof(float)},           // sheet frame
        };
        VkPipelineColorBlendAttachmentState blend{};
        blend.blendEnable = VK_TRUE;
        blend.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
        blend.dstColorBlendFactor = additive ? VK_BLEND_FACTOR_ONE : VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        blend.colorBlendOp = VK_BLEND_OP_ADD;
        blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        blend.alphaBlendOp = VK_BLEND_OP_ADD;
        blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT |
                               VK_COLOR_COMPONENT_A_BIT;
        SceneVkPipelineBuilder b;
        b.setShaderStages(vs_, fs_)
         .setVertexInput({binding}, attributes)
         .setInputTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
         .setPolygonMode(VK_POLYGON_MODE_FILL)
         .setCullMode(VK_CULL_MODE_NONE)
         .setTarget(target)
         .setColorBlendAttachment(0, blend)
         .enableDepthTest(false);
        return b.build(device_->device(), layout_);
    });
}

void PassParticles::declare(const SceneFrame& frame, PassIO& io) const {
    io.sample(frame.gpu.targets.depthSnapshot);
    io.hdr();
}

void PassParticles::record(SceneFrame& frame) {
    static const bromath::Mat4 kIdentity = bromath::midentity();
    const bromath::Vec3 camFwd = frame.view.forward();
    const bromath::Vec3 camRight = frame.view.right();
    const bromath::Vec3 camUp = frame.view.up();
    const SceneDefaults& d = frame.gpu.defaults;
    const SceneVkImage& depth = frame.gpu.targets.depthSnapshot;
    VkCommandBuffer cmd = frame.cmd;

    for (auto& [id, owned] : frame.graph.nodes()) {
        if (!owned->renderVisible() || owned->type() != SceneNode::Type::Particles3D) continue;
        auto* p = static_cast<Particles3DNode*>(owned.get());
        if (p->liveCount() <= 0) continue;
        if (frame.renderer.cameraCulled(p)) {
            frame.stats.particlesCulled++;
            continue;
        }
        frame.stats.particlesDrawn++;

        const std::vector<float>& data = p->buildInstanceData(camFwd);
        const auto count = static_cast<uint32_t>(p->activeParticleCount());
        if (count == 0 || data.empty()) continue;
        const size_t bytes = static_cast<size_t>(count) * kInstanceFloats * sizeof(float);
        const render::UploadSlice instances = device_->frameUpload(bytes);
        if (!instances) continue;
        std::memcpy(instances.mapped, data.data(), bytes);

        VkPipeline pipe = pipeline(frame.hdrTarget, p->blend() == Particles3DNode::Blend::Additive);
        if (!pipe) continue;

        Push push{};
        const bromath::Mat4& model = p->space() == Particles3DNode::SimSpace::Local ? p->worldMatrix() : kIdentity;
        std::memcpy(push.model, model.data, sizeof(push.model));
        push.camRight[0] = camRight.x;
        push.camRight[1] = camRight.y;
        push.camRight[2] = camRight.z;
        push.camUp[0] = camUp.x;
        push.camUp[1] = camUp.y;
        push.camUp[2] = camUp.z;
        push.params[0] = static_cast<float>(p->sheetCols());
        push.params[1] = static_cast<float>(p->sheetRows());
        push.params[3] = p->softness();

        const SceneVkImage* tex = nullptr;
        if (p->ensureTextureLoaded()) tex = frame.gpu.resources.texture(p->id(), TextureSlot::Image, p->texture());
        push.params[2] = tex ? 1.0f : 0.0f;

        VkDescriptorSet set = device_->frameSet(materialLayout_);
        SceneVkDescriptorWriter writer;
        writer.writeImage(0, tex ? tex->view : d.white.view, tex ? tex->sampler : d.sampler);
        writer.writeImage(1, depth.view, depth.sampler);
        writer.updateSet(device_->device(), set);

        const VkDescriptorSet sets[2] = {frame.cameraSet, set};
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0, 2, sets, 0, nullptr);
        vkCmdPushConstants(cmd, layout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push),
                           &push);
        vkCmdBindVertexBuffers(cmd, 0, 1, &instances.buffer, &instances.offset);
        vkCmdDraw(cmd, 6, count, 0, 0);
        frame.drewContent = true;
    }
}

}  // namespace bro::scene::vk
