#include "scene/vulkan/pass_decal.h"
#include "scene/vulkan/scene_vk_shader_compiler.h"
#include "util/log.h"

namespace bro::scene::vk {

bool PassDecal::init(SceneVkDevice& device, SceneVkAllocator& allocator,
                     VkDescriptorSetLayout cameraLayout, VkDescriptorSetLayout lightingLayout) {
    VkDevice dev = device.device();
    cameraLayout_ = cameraLayout;
    lightingLayout_ = lightingLayout;

    // Set 2: Material layout (binding 0: scene depth, binding 1: albedo, binding 2: emission)
    SceneVkDescriptorLayoutBuilder matBuilder;
    matBuilder.addBinding(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT);
    matBuilder.addBinding(1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT);
    matBuilder.addBinding(2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT);
    materialLayout_ = matBuilder.build(dev);
    if (!materialLayout_) {
        LOG_ERROR("PassDecal: Failed creating material layout");
        return false;
    }

    // Default dummy textures
    uint8_t whitePixels[4] = {255, 255, 255, 255};
    uint8_t blackPixels[4] = {0, 0, 0, 0};
    TextureDesc texDesc{};
    texDesc.width = 1;
    texDesc.height = 1;
    texDesc.format = VK_FORMAT_R8G8B8A8_UNORM;
    texDesc.generateMipmaps = false;
    if (!allocator.createTexture2D(whitePixels, texDesc, dummyWhiteImage_) ||
        !allocator.createTexture2D(blackPixels, texDesc, dummyBlackImage_)) {
        LOG_ERROR("PassDecal: Failed creating dummy textures");
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
        LOG_ERROR("PassDecal: Failed creating default sampler");
        return false;
    }

    // Unit cube vertex buffer: [-0.5, 0.5]^3, 36 vertices (CCW winding)
    static const float c = 0.5f;
    static const float cubeVerts[36 * 3] = {
        // -X face
        -c,-c,-c,  -c,-c, c,  -c, c, c,   -c,-c,-c,  -c, c, c,  -c, c,-c,
        // +X face
         c,-c,-c,   c, c,-c,   c, c, c,    c,-c,-c,   c, c, c,   c,-c, c,
        // -Y face
        -c,-c,-c,   c,-c,-c,   c,-c, c,   -c,-c,-c,   c,-c, c,  -c,-c, c,
        // +Y face
        -c, c,-c,  -c, c, c,   c, c, c,   -c, c,-c,   c, c, c,   c, c,-c,
        // -Z face
        -c,-c,-c,  -c, c,-c,   c, c,-c,   -c,-c,-c,   c, c,-c,   c,-c,-c,
        // +Z face
        -c,-c, c,   c,-c, c,   c, c, c,   -c,-c, c,   c, c, c,  -c, c, c,
    };
    if (!allocator.createVertexBuffer(sizeof(cubeVerts), cubeVerts, cubeVertexBuffer_)) {
        LOG_ERROR("PassDecal: Failed creating cube vertex buffer");
        return false;
    }

    // Pipeline Layout
    VkDescriptorSetLayout setLayouts[3] = {cameraLayout_, lightingLayout_, materialLayout_};
    VkPushConstantRange pushRange{};
    pushRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    pushRange.offset = 0;
    pushRange.size = sizeof(DecalPushConstants);

    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = 3;
    layoutInfo.pSetLayouts = setLayouts;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &pushRange;

    if (vkCreatePipelineLayout(dev, &layoutInfo, nullptr, &pipelineLayout_) != VK_SUCCESS) {
        LOG_ERROR("PassDecal: Failed creating pipeline layout");
        return false;
    }

    // Vertex input for cube pos
    VkVertexInputBindingDescription bindingDesc{};
    bindingDesc.binding = 0;
    bindingDesc.stride = 3 * sizeof(float);
    bindingDesc.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    VkVertexInputAttributeDescription attribDesc{};
    attribDesc.location = 0;
    attribDesc.binding = 0;
    attribDesc.format = VK_FORMAT_R32G32B32_SFLOAT;
    attribDesc.offset = 0;

    VkShaderModule vertMod = SceneVkShaderCompiler::createBuiltinModule(dev, BuiltinSceneShader::DecalVert);
    VkShaderModule fragMod = SceneVkShaderCompiler::createBuiltinModule(dev, BuiltinSceneShader::DecalFrag);
    if (!vertMod || !fragMod) {
        LOG_ERROR("PassDecal: Failed creating shader modules");
        return false;
    }

    SceneVkPipelineBuilder b;
    b.addShaderStage(VK_SHADER_STAGE_VERTEX_BIT, vertMod)
     .addShaderStage(VK_SHADER_STAGE_FRAGMENT_BIT, fragMod)
     .setVertexInput({bindingDesc}, {attribDesc})
     .setInputTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
     .setPolygonMode(VK_POLYGON_MODE_FILL)
     .setCullMode(VK_CULL_MODE_FRONT_BIT) // Cull front faces so camera inside still renders
     .setMultisamplingNone();

    VkPipelineColorBlendAttachmentState blendAttachment{};
    blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                     VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    blendAttachment.blendEnable = VK_TRUE;
    blendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
    blendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
    blendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    blendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;
    b.setColorBlendAttachment(0, blendAttachment)
     .enableDepthTest(false, VK_COMPARE_OP_LESS_OR_EQUAL) // Reversed-Z
     .setDynamicRendering({VK_FORMAT_R16G16B16A16_SFLOAT}, VK_FORMAT_D32_SFLOAT);

    pipeline_ = b.build(dev, pipelineLayout_);

    SceneVkShaderCompiler::destroyModule(dev, vertMod);
    SceneVkShaderCompiler::destroyModule(dev, fragMod);

    return pipeline_ != VK_NULL_HANDLE;
}

void PassDecal::cleanup(SceneVkDevice& device, SceneVkAllocator& allocator) {
    VkDevice dev = device.device();
    if (pipeline_ != VK_NULL_HANDLE) {
        vkDestroyPipeline(dev, pipeline_, nullptr);
        pipeline_ = VK_NULL_HANDLE;
    }
    if (pipelineLayout_ != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(dev, pipelineLayout_, nullptr);
        pipelineLayout_ = VK_NULL_HANDLE;
    }
    allocator.destroyBuffer(cubeVertexBuffer_);
    if (defaultSampler_ != VK_NULL_HANDLE) {
        vkDestroySampler(dev, defaultSampler_, nullptr);
        defaultSampler_ = VK_NULL_HANDLE;
    }
    allocator.destroyImage(dummyWhiteImage_);
    allocator.destroyImage(dummyBlackImage_);
    if (materialLayout_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(dev, materialLayout_, nullptr);
        materialLayout_ = VK_NULL_HANDLE;
    }
}

void PassDecal::begin(VkCommandBuffer cmd, VkDescriptorSet cameraSet, VkDescriptorSet lightingSet) {
    activeCameraSet_ = cameraSet;
    activeLightingSet_ = lightingSet;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
    VkDescriptorSet sets[2] = {activeCameraSet_, activeLightingSet_};
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_,
                            0, 2, sets, 0, nullptr);
}

VkDescriptorSet PassDecal::createMaterialSet(VkImageView depthView, VkSampler depthSampler,
                                             VkImageView albedoView, VkSampler albedoSampler,
                                             VkImageView emissionView, VkSampler emissionSampler,
                                             SceneVkDescriptorPool& pool) {
    VkDescriptorSet set = pool.allocate(materialLayout_);
    if (!set) return VK_NULL_HANDLE;

    VkDevice dev = pool.device();
    SceneVkDescriptorWriter writer;
    writer.writeImage(0, depthView ? depthView : dummyWhiteImage_.view,
                         depthSampler ? depthSampler : defaultSampler_);
    writer.writeImage(1, albedoView ? albedoView : dummyWhiteImage_.view,
                         albedoSampler ? albedoSampler : defaultSampler_);
    writer.writeImage(2, emissionView ? emissionView : dummyBlackImage_.view,
                         emissionSampler ? emissionSampler : defaultSampler_);
    writer.updateSet(dev, set);
    return set;
}

void PassDecal::draw(VkCommandBuffer cmd, const DecalPushConstants& push, VkDescriptorSet materialSet) {
    if (materialSet) {
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_,
                                2, 1, &materialSet, 0, nullptr);
    }
    vkCmdPushConstants(cmd, pipelineLayout_,
                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                       0, sizeof(push), &push);

    VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &cubeVertexBuffer_.buffer, &offset);
    vkCmdDraw(cmd, 36, 1, 0, 0);
}

} // namespace bro::scene::vk
