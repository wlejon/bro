#include "scene/vulkan/pass_particles.h"
#include "scene/vulkan/scene_vk_shader_compiler.h"
#include "util/log.h"

namespace bro::scene::vk {

bool PassParticles::init(SceneVkDevice& device, SceneVkAllocator& allocator,
                         VkDescriptorSetLayout cameraLayout) {
    VkDevice dev = device.device();
    cameraLayout_ = cameraLayout;

    // Set 1: Material layout (binding 0: texture, binding 1: scene depth)
    SceneVkDescriptorLayoutBuilder matBuilder;
    matBuilder.addBinding(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT);
    matBuilder.addBinding(1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT);
    materialLayout_ = matBuilder.build(dev);
    if (!materialLayout_) {
        LOG_ERROR("PassParticles: Failed creating material layout");
        return false;
    }

    // Default dummy 1x1 white texture
    uint8_t whitePixels[4] = {255, 255, 255, 255};
    TextureDesc texDesc{};
    texDesc.width = 1;
    texDesc.height = 1;
    texDesc.format = VK_FORMAT_R8G8B8A8_UNORM;
    texDesc.generateMipmaps = false;
    if (!allocator.createTexture2D(whitePixels, texDesc, dummyWhiteImage_)) {
        LOG_ERROR("PassParticles: Failed creating dummy white texture");
        return false;
    }

    VkSamplerCreateInfo sampInfo{};
    sampInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampInfo.magFilter = VK_FILTER_LINEAR;
    sampInfo.minFilter = VK_FILTER_LINEAR;
    sampInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    sampInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampInfo.maxAnisotropy = 1.0f;
    if (vkCreateSampler(dev, &sampInfo, nullptr, &defaultSampler_) != VK_SUCCESS) {
        LOG_ERROR("PassParticles: Failed creating default sampler");
        return false;
    }

    if (!defaultDescPool_.init(dev, 4)) {
        LOG_ERROR("PassParticles: Failed creating default desc pool");
        return false;
    }
    defaultMaterialSet_ = defaultDescPool_.allocate(materialLayout_);
    SceneVkDescriptorWriter writer;
    writer.writeImage(0, dummyWhiteImage_.view, defaultSampler_);
    writer.writeImage(1, dummyWhiteImage_.view, defaultSampler_);
    writer.updateSet(dev, defaultMaterialSet_);

    // Pipeline Layout
    VkDescriptorSetLayout setLayouts[2] = {cameraLayout_, materialLayout_};
    VkPushConstantRange pushRange{};
    pushRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    pushRange.offset = 0;
    pushRange.size = sizeof(ParticlePushConstants);

    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = 2;
    layoutInfo.pSetLayouts = setLayouts;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &pushRange;

    if (vkCreatePipelineLayout(dev, &layoutInfo, nullptr, &pipelineLayout_) != VK_SUCCESS) {
        LOG_ERROR("PassParticles: Failed creating pipeline layout");
        return false;
    }

    return createPipelines(dev);
}

bool PassParticles::setSampleCount(VkDevice dev, VkSampleCountFlagBits samples) {
    if (samples_ == samples) return true;
    if (pipelineNormal_ != VK_NULL_HANDLE) vkDestroyPipeline(dev, pipelineNormal_, nullptr);
    if (pipelineAdditive_ != VK_NULL_HANDLE) vkDestroyPipeline(dev, pipelineAdditive_, nullptr);
    pipelineNormal_ = VK_NULL_HANDLE;
    pipelineAdditive_ = VK_NULL_HANDLE;
    samples_ = samples;
    return createPipelines(dev);
}

bool PassParticles::createPipelines(VkDevice dev) {
    // Vertex input description for instanced particles
    VkVertexInputBindingDescription bindingDesc{};
    bindingDesc.binding = 0;
    bindingDesc.stride = 10 * sizeof(float);
    bindingDesc.inputRate = VK_VERTEX_INPUT_RATE_INSTANCE;

    std::vector<VkVertexInputAttributeDescription> attribs(5);
    // location 0: pos (vec3)
    attribs[0].location = 0;
    attribs[0].binding = 0;
    attribs[0].format = VK_FORMAT_R32G32B32_SFLOAT;
    attribs[0].offset = 0;
    // location 1: size (float)
    attribs[1].location = 1;
    attribs[1].binding = 0;
    attribs[1].format = VK_FORMAT_R32_SFLOAT;
    attribs[1].offset = 3 * sizeof(float);
    // location 2: color (vec4)
    attribs[2].location = 2;
    attribs[2].binding = 0;
    attribs[2].format = VK_FORMAT_R32G32B32A32_SFLOAT;
    attribs[2].offset = 4 * sizeof(float);
    // location 3: rot (float)
    attribs[3].location = 3;
    attribs[3].binding = 0;
    attribs[3].format = VK_FORMAT_R32_SFLOAT;
    attribs[3].offset = 8 * sizeof(float);
    // location 4: frame (float)
    attribs[4].location = 4;
    attribs[4].binding = 0;
    attribs[4].format = VK_FORMAT_R32_SFLOAT;
    attribs[4].offset = 9 * sizeof(float);

    VkShaderModule vertMod = SceneVkShaderCompiler::createBuiltinModule(dev, BuiltinSceneShader::ParticlesVert);
    VkShaderModule fragMod = SceneVkShaderCompiler::createBuiltinModule(dev, BuiltinSceneShader::ParticlesFrag);
    if (!vertMod || !fragMod) {
        LOG_ERROR("PassParticles: Failed creating shader modules");
        return false;
    }

    // 1. Normal blend pipeline (premultiplied over)
    {
        VkPipelineColorBlendAttachmentState blendAttachment{};
        blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                         VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        blendAttachment.blendEnable = VK_TRUE;
        blendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
        blendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        blendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
        blendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        blendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        blendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;

        SceneVkPipelineBuilder b;
        b.addShaderStage(VK_SHADER_STAGE_VERTEX_BIT, vertMod)
         .addShaderStage(VK_SHADER_STAGE_FRAGMENT_BIT, fragMod)
         .setVertexInput({bindingDesc}, attribs)
         .setInputTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
         .setPolygonMode(VK_POLYGON_MODE_FILL)
         .setCullMode(VK_CULL_MODE_NONE)
         .setMultisampling(samples_)
         .setColorBlendAttachment(0, blendAttachment)
         .enableDepthTest(false, VK_COMPARE_OP_GREATER_OR_EQUAL) // Reversed-Z
         .setDynamicRendering({VK_FORMAT_R16G16B16A16_SFLOAT}, VK_FORMAT_D32_SFLOAT);
        pipelineNormal_ = b.build(dev, pipelineLayout_);
    }

    // 2. Additive blend pipeline
    {
        VkPipelineColorBlendAttachmentState addBlend{};
        addBlend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                  VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        addBlend.blendEnable = VK_TRUE;
        addBlend.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
        addBlend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
        addBlend.colorBlendOp = VK_BLEND_OP_ADD;
        addBlend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        addBlend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        addBlend.alphaBlendOp = VK_BLEND_OP_ADD;

        SceneVkPipelineBuilder b;
        b.addShaderStage(VK_SHADER_STAGE_VERTEX_BIT, vertMod)
         .addShaderStage(VK_SHADER_STAGE_FRAGMENT_BIT, fragMod)
         .setVertexInput({bindingDesc}, attribs)
         .setInputTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
         .setPolygonMode(VK_POLYGON_MODE_FILL)
         .setCullMode(VK_CULL_MODE_NONE)
         .setMultisampling(samples_)
         .setColorBlendAttachment(0, addBlend)
         .enableDepthTest(false, VK_COMPARE_OP_GREATER_OR_EQUAL)
         .setDynamicRendering({VK_FORMAT_R16G16B16A16_SFLOAT}, VK_FORMAT_D32_SFLOAT);
        pipelineAdditive_ = b.build(dev, pipelineLayout_);
    }

    SceneVkShaderCompiler::destroyModule(dev, vertMod);
    SceneVkShaderCompiler::destroyModule(dev, fragMod);

    return pipelineNormal_ != VK_NULL_HANDLE && pipelineAdditive_ != VK_NULL_HANDLE;
}

void PassParticles::cleanup(SceneVkDevice& device, SceneVkAllocator& allocator) {
    VkDevice dev = device.device();
    if (pipelineNormal_ != VK_NULL_HANDLE) {
        vkDestroyPipeline(dev, pipelineNormal_, nullptr);
        pipelineNormal_ = VK_NULL_HANDLE;
    }
    if (pipelineAdditive_ != VK_NULL_HANDLE) {
        vkDestroyPipeline(dev, pipelineAdditive_, nullptr);
        pipelineAdditive_ = VK_NULL_HANDLE;
    }
    if (pipelineLayout_ != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(dev, pipelineLayout_, nullptr);
        pipelineLayout_ = VK_NULL_HANDLE;
    }
    defaultDescPool_.destroy();
    if (defaultSampler_ != VK_NULL_HANDLE) {
        vkDestroySampler(dev, defaultSampler_, nullptr);
        defaultSampler_ = VK_NULL_HANDLE;
    }
    allocator.destroyImage(dummyWhiteImage_);
    if (materialLayout_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(dev, materialLayout_, nullptr);
        materialLayout_ = VK_NULL_HANDLE;
    }
}

void PassParticles::begin(VkCommandBuffer cmd, VkDescriptorSet cameraSet) {
    activeCameraSet_ = cameraSet;
}

VkDescriptorSet PassParticles::createParticleMaterialSet(SceneVkDevice& device, VkImageView imageView, VkSampler sampler,
                                                         VkImageView depthView, VkSampler depthSampler) {
    VkDescriptorSet set = device.frameSet(materialLayout_);
    if (!set) return defaultMaterialSet_;

    VkDevice dev = device.device();
    SceneVkDescriptorWriter writer;
    writer.writeImage(0, imageView ? imageView : dummyWhiteImage_.view,
                         sampler ? sampler : defaultSampler_);
    writer.writeImage(1, depthView ? depthView : dummyWhiteImage_.view,
                         depthSampler ? depthSampler : defaultSampler_);
    writer.updateSet(dev, set);
    return set;
}

void PassParticles::draw(VkCommandBuffer cmd, bool additive,
                         VkBuffer instanceBuffer, VkDeviceSize instanceOffset, uint32_t instanceCount,
                         const ParticlePushConstants& push,
                         VkDescriptorSet materialSet) {
    if (instanceCount == 0 || instanceBuffer == VK_NULL_HANDLE) return;

    VkPipeline pipe = additive ? pipelineAdditive_ : pipelineNormal_;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);

    VkDescriptorSet sets[2] = {activeCameraSet_, materialSet ? materialSet : defaultMaterialSet_};
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_,
                            0, 2, sets, 0, nullptr);

    vkCmdPushConstants(cmd, pipelineLayout_,
                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                       0, sizeof(push), &push);

    vkCmdBindVertexBuffers(cmd, 0, 1, &instanceBuffer, &instanceOffset);
    vkCmdDraw(cmd, 6, instanceCount, 0, 0);
}

} // namespace bro::scene::vk
