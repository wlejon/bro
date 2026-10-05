#include "scene/vulkan/pass_decal.h"

#include "scene/decal_node.h"
#include "scene/scene_graph.h"
#include "scene/vulkan/scene_defaults.h"
#include "scene/vulkan/scene_frame.h"
#include "scene/vulkan/scene_gpu_resources.h"
#include "scene/vulkan/scene_targets.h"
#include "scene/vulkan/scene_vk_device.h"
#include "scene/vulkan/scene_vk_pipeline.h"
#include "scene/vulkan/scene_vk_shader_compiler.h"

#include <algorithm>
#include <cstring>
#include <vector>

namespace bro::scene::vk {

namespace {

constexpr float c = 0.5f;
// [-0.5, 0.5]^3, counter-clockwise from outside.
constexpr float kCube[36 * 3] = {
    -c, -c, -c, -c, -c, c,  -c, c,  c,  -c, -c, -c, -c, c,  c,  -c, c,  -c,   // -X
    c,  -c, -c, c,  c,  -c, c,  c,  c,  c,  -c, -c, c,  c,  c,  c,  -c, c,    // +X
    -c, -c, -c, c,  -c, -c, c,  -c, c,  -c, -c, -c, c,  -c, c,  -c, -c, c,    // -Y
    -c, c,  -c, -c, c,  c,  c,  c,  c,  -c, c,  -c, c,  c,  c,  c,  c,  -c,   // +Y
    -c, -c, -c, -c, c,  -c, c,  c,  -c, -c, -c, -c, c,  c,  -c, c,  -c, -c,   // -Z
    -c, -c, c,  c,  -c, c,  c,  c,  c,  -c, -c, c,  c,  c,  c,  -c, c,  c,    // +Z
};

}  // namespace

bool PassDecal::setup(SceneGpu& gpu) {
    device_ = &gpu.device;
    VkDevice dev = gpu.device.device();

    SceneVkDescriptorLayoutBuilder builder;
    for (uint32_t b = 0; b < 3; ++b)
        builder.addBinding(b, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT);
    materialLayout_ = builder.build(dev);
    if (!materialLayout_) return false;

    const VkDescriptorSetLayout sets[3] = {gpu.defaults.cameraLayout, gpu.defaults.lightingLayout, materialLayout_};
    VkPushConstantRange push{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(Push)};
    VkPipelineLayoutCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    info.setLayoutCount = 3;
    info.pSetLayouts = sets;
    info.pushConstantRangeCount = 1;
    info.pPushConstantRanges = &push;
    if (vkCreatePipelineLayout(dev, &info, nullptr, &layout_) != VK_SUCCESS) return false;

    if (!gpu.allocator.createVertexBuffer(sizeof(kCube), kCube, cube_)) return false;
    vs_ = SceneVkShaderCompiler::createBuiltinModule(dev, BuiltinSceneShader::DecalVert);
    fs_ = SceneVkShaderCompiler::createBuiltinModule(dev, BuiltinSceneShader::DecalFrag);
    return vs_ && fs_;
}

void PassDecal::cleanup(SceneGpu& gpu) {
    VkDevice dev = gpu.device.device();
    pipelines_.destroy(gpu.device);
    gpu.allocator.destroyBuffer(cube_);
    SceneVkShaderModule::destroy(dev, vs_);
    SceneVkShaderModule::destroy(dev, fs_);
    vs_ = fs_ = VK_NULL_HANDLE;
    if (layout_) vkDestroyPipelineLayout(dev, layout_, nullptr);
    if (materialLayout_) vkDestroyDescriptorSetLayout(dev, materialLayout_, nullptr);
    layout_ = VK_NULL_HANDLE;
    materialLayout_ = VK_NULL_HANDLE;
}

void PassDecal::declare(const SceneFrame& frame, PassIO& io) const {
    io.sample(frame.gpu.targets.depthSnapshot);
    io.hdr();
}

void PassDecal::record(SceneFrame& frame) {
    std::vector<DecalNode*> decals;
    for (auto& [id, owned] : frame.graph.nodes()) {
        if (!owned->renderVisible() || owned->type() != SceneNode::Type::Decal) continue;
        auto* decal = static_cast<DecalNode*>(owned.get());
        if (frame.renderer.cameraCulled(decal)) {
            frame.stats.decalsCulled++;
        } else {
            frame.stats.decalsDrawn++;
            decals.push_back(decal);
        }
    }
    if (decals.empty()) return;
    std::stable_sort(decals.begin(), decals.end(),
                     [](DecalNode* a, DecalNode* b) { return a->renderPriority() < b->renderPriority(); });

    VkPipeline pipeline = pipelines_.get(0, frame.hdrTarget, [&] {
        const VkVertexInputBindingDescription binding{0, 3 * sizeof(float), VK_VERTEX_INPUT_RATE_VERTEX};
        const VkVertexInputAttributeDescription position{0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0};
        // Premultiplied over the scene colour; the scene's alpha is kept.
        VkPipelineColorBlendAttachmentState blend{};
        blend.blendEnable = VK_TRUE;
        blend.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
        blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        blend.colorBlendOp = VK_BLEND_OP_ADD;
        blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        blend.alphaBlendOp = VK_BLEND_OP_ADD;
        blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT |
                               VK_COLOR_COMPONENT_A_BIT;
        SceneVkPipelineBuilder b;
        b.setShaderStages(vs_, fs_)
         .setVertexInput({binding}, {position})
         .setInputTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
         .setPolygonMode(VK_POLYGON_MODE_FILL)
         .setCullMode(VK_CULL_MODE_FRONT_BIT)   // back faces: a camera inside the box still sees it
         .setTarget(frame.hdrTarget)
         .setColorBlendAttachment(0, blend)
         // The box's back faces lie at or behind the surfaces it covers.
         .enableDepthTest(false, depth::compareFartherEqual());
        return b.build(device_->device(), layout_);
    });
    if (!pipeline) return;

    VkCommandBuffer cmd = frame.cmd;
    const SceneDefaults& d = frame.gpu.defaults;
    const SceneVkImage& depth = frame.gpu.targets.depthSnapshot;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    const VkDescriptorSet frameSets[2] = {frame.cameraSet, frame.lightingSet};
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0, 2, frameSets, 0, nullptr);
    const VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &cube_.buffer, &offset);

    for (DecalNode* decal : decals) {
        Push push{};
        const bromath::Mat4& world = decal->worldMatrix();
        std::memcpy(push.model, world.data, sizeof(push.model));
        const bromath::Mat4 inv = bromath::minverse(world);
        std::memcpy(push.invModel, inv.data, sizeof(push.invModel));
        std::memcpy(push.modulate, decal->modulate(), sizeof(push.modulate));
        const bromath::Vec3 up = bromath::vnorm(bromath::Vec3{world.at(0, 1), world.at(1, 1), world.at(2, 1)});
        push.decalUp[0] = up.x;
        push.decalUp[1] = up.y;
        push.decalUp[2] = up.z;
        push.decalUp[3] = decal->emissionStrength();
        push.fades[0] = decal->upperFade();
        push.fades[1] = decal->lowerFade();
        push.fades[2] = decal->normalFade();

        const SceneVkImage* albedo =
            frame.gpu.resources.texture(decal->id(), TextureSlot::DecalAlbedo, decal->albedoTexture());
        const SceneVkImage* emission =
            frame.gpu.resources.texture(decal->id(), TextureSlot::DecalEmission, decal->emissionTexture());
        push.flags[0] = albedo ? 1 : 0;
        push.flags[1] = emission ? 1 : 0;

        VkDescriptorSet set = device_->frameSet(materialLayout_);
        SceneVkDescriptorWriter writer;
        writer.writeImage(0, depth.view, depth.sampler);
        writer.writeImage(1, albedo ? albedo->view : d.white.view, albedo ? albedo->sampler : d.sampler);
        writer.writeImage(2, emission ? emission->view : d.black.view, emission ? emission->sampler : d.sampler);
        writer.updateSet(device_->device(), set);

        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 2, 1, &set, 0, nullptr);
        vkCmdPushConstants(cmd, layout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push),
                           &push);
        vkCmdDraw(cmd, 36, 1, 0, 0);
        frame.drewContent = true;
    }
}

}  // namespace bro::scene::vk
