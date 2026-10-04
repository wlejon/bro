#include "scene/vulkan/pass_mesh.h"
#include "scene/vulkan/scene_vk_shader_compiler.h"
#include "util/log.h"

#include <array>
#include <cstring>

namespace bro::scene::vk {

PassMesh::~PassMesh() {
    // Rely on explicit cleanup() call or destructor safety
}

bool PassMesh::init(SceneVkDevice& device, SceneVkAllocator& allocator) {
    return init(device, allocator, Config{});
}

bool PassMesh::init(SceneVkDevice& device, SceneVkAllocator& allocator, const Config& config) {
    config_ = config;
    VkDevice dev = device.device();

    if (!createDescriptorLayouts(dev)) {
        LOG_ERROR("PassMesh: Failed to create descriptor layouts");
        return false;
    }

    if (!createDefaultTextures(device, allocator)) {
        LOG_ERROR("PassMesh: Failed to create default textures");
        return false;
    }

    if (!createPipelines(dev, config)) {
        LOG_ERROR("PassMesh: Failed to create pipelines");
        return false;
    }

    return true;
}

bool PassMesh::setSampleCount(VkDevice dev, VkSampleCountFlagBits samples) {
    if (config_.samples == samples) return true;
    destroyPipelines(dev);
    config_.samples = samples;
    return createPipelines(dev, config_);
}

void PassMesh::destroyPipelines(VkDevice dev) {
    if (pipelineStatic_ != VK_NULL_HANDLE) {
        vkDestroyPipeline(dev, pipelineStatic_, nullptr);
        pipelineStatic_ = VK_NULL_HANDLE;
    }
    if (pipelineInstanced_ != VK_NULL_HANDLE) {
        vkDestroyPipeline(dev, pipelineInstanced_, nullptr);
        pipelineInstanced_ = VK_NULL_HANDLE;
    }
    if (pipelineSkinned_ != VK_NULL_HANDLE) {
        vkDestroyPipeline(dev, pipelineSkinned_, nullptr);
        pipelineSkinned_ = VK_NULL_HANDLE;
    }
    if (pipelineStaticTranslucent_ != VK_NULL_HANDLE) {
        vkDestroyPipeline(dev, pipelineStaticTranslucent_, nullptr);
        pipelineStaticTranslucent_ = VK_NULL_HANDLE;
    }
    if (pipelineInstancedTranslucent_ != VK_NULL_HANDLE) {
        vkDestroyPipeline(dev, pipelineInstancedTranslucent_, nullptr);
        pipelineInstancedTranslucent_ = VK_NULL_HANDLE;
    }
    if (pipelineSkinnedTranslucent_ != VK_NULL_HANDLE) {
        vkDestroyPipeline(dev, pipelineSkinnedTranslucent_, nullptr);
        pipelineSkinnedTranslucent_ = VK_NULL_HANDLE;
    }
}

void PassMesh::cleanup(SceneVkDevice& device, SceneVkAllocator& allocator) {
    VkDevice dev = device.device();
    destroyPipelines(dev);
    if (pipelineLayout_ != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(dev, pipelineLayout_, nullptr);
        pipelineLayout_ = VK_NULL_HANDLE;
    }

    defaultDescPool_.destroy();

    defaultSampler_ = VK_NULL_HANDLE;

    allocator.destroyImage(dummyWhiteImage_);
    allocator.destroyImage(dummyNormalImage_);
    allocator.destroyImage(dummyBlackImage_);

    if (cameraLayout_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(dev, cameraLayout_, nullptr);
        cameraLayout_ = VK_NULL_HANDLE;
    }
    if (lightingLayout_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(dev, lightingLayout_, nullptr);
        lightingLayout_ = VK_NULL_HANDLE;
    }
    if (materialLayout_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(dev, materialLayout_, nullptr);
        materialLayout_ = VK_NULL_HANDLE;
    }
    if (bonePaletteLayout_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(dev, bonePaletteLayout_, nullptr);
        bonePaletteLayout_ = VK_NULL_HANDLE;
    }
    if (customLayout_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(dev, customLayout_, nullptr);
        customLayout_ = VK_NULL_HANDLE;
    }
}

bool PassMesh::createDescriptorLayouts(VkDevice device) {
    // Set 0: Camera Layout
    SceneVkDescriptorLayoutBuilder camBuilder;
    camBuilder.addBinding(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1,
                          VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT);
    cameraLayout_ = camBuilder.build(device);
    if (!cameraLayout_) return false;

    // Set 1: Lighting Layout (UBO + Shadow Cascade Array + Reflection Probe Cubemap + Tile Shade Map)
    SceneVkDescriptorLayoutBuilder lightBuilder;
    lightBuilder.addBinding(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT);
    lightBuilder.addBinding(1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT);
    lightBuilder.addBinding(2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT);
    lightBuilder.addBinding(3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT);
    lightingLayout_ = lightBuilder.build(device);
    if (!lightingLayout_) return false;

    // Set 2: Material Layout (4 textures)
    SceneVkDescriptorLayoutBuilder matBuilder;
    for (uint32_t i = 0; i < 4; ++i) {
        matBuilder.addBinding(i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT);
    }
    materialLayout_ = matBuilder.build(device);
    if (!materialLayout_) return false;

    // Set 3: Bone Palette Layout (UBO with 256 mat4s)
    SceneVkDescriptorLayoutBuilder boneBuilder;
    boneBuilder.addBinding(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT);
    bonePaletteLayout_ = boneBuilder.build(device);
    if (!bonePaletteLayout_) return false;

    // Set 4: Custom Shader Layout (binding 0: UBO, bindings 1..8: combined image samplers)
    SceneVkDescriptorLayoutBuilder customBuilder;
    customBuilder.addBinding(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT);
    for (uint32_t i = 1; i <= 8; ++i) {
        customBuilder.addBinding(i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT);
    }
    customLayout_ = customBuilder.build(device);
    if (!customLayout_) return false;

    // Push constant range: 112 bytes
    VkPushConstantRange pushRange{};
    pushRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    pushRange.offset = 0;
    pushRange.size = sizeof(MeshPushConstants);

    std::array<VkDescriptorSetLayout, 5> layouts = {
        cameraLayout_, lightingLayout_, materialLayout_, bonePaletteLayout_, customLayout_
    };

    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = static_cast<uint32_t>(layouts.size());
    layoutInfo.pSetLayouts = layouts.data();
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &pushRange;

    if (vkCreatePipelineLayout(device, &layoutInfo, nullptr, &pipelineLayout_) != VK_SUCCESS) {
        LOG_ERROR("PassMesh: vkCreatePipelineLayout failed");
        return false;
    }

    return true;
}

bool PassMesh::createDefaultTextures(SceneVkDevice& device, SceneVkAllocator& allocator) {
    uint8_t whitePixels[4]  = {255, 255, 255, 255};
    uint8_t normalPixels[4] = {128, 128, 255, 255}; // (0, 0, 1) in tangent space
    uint8_t blackPixels[4]  = {0, 0, 0, 255};

    TextureDesc desc{};
    desc.width = 1;
    desc.height = 1;
    desc.format = VK_FORMAT_R8G8B8A8_UNORM;
    desc.generateMipmaps = false;
    desc.enableAnisotropy = false;

    if (!allocator.createTexture2D(whitePixels, desc, dummyWhiteImage_) ||
        !allocator.createTexture2D(normalPixels, desc, dummyNormalImage_) ||
        !allocator.createTexture2D(blackPixels, desc, dummyBlackImage_)) {
        LOG_ERROR("PassMesh: Failed creating dummy fallback textures");
        return false;
    }
    defaultSampler_ = dummyWhiteImage_.sampler;

    if (!defaultDescPool_.init(device.device(), 4)) {
        return false;
    }
    defaultMaterialSet_ = defaultDescPool_.allocate(materialLayout_);
    if (!defaultMaterialSet_) return false;

    SceneVkDescriptorWriter writer;
    writer.writeImage(0, dummyWhiteImage_.view, dummyWhiteImage_.sampler);
    writer.writeImage(1, dummyNormalImage_.view, dummyNormalImage_.sampler);
    writer.writeImage(2, dummyWhiteImage_.view, dummyWhiteImage_.sampler);
    writer.writeImage(3, dummyBlackImage_.view, dummyBlackImage_.sampler);
    writer.updateSet(device.device(), defaultMaterialSet_);

    return true;
}

bool PassMesh::createPipelines(VkDevice device, const Config& config) {
    VkShaderModule vsStatic = SceneVkShaderCompiler::createBuiltinModule(device, BuiltinSceneShader::MeshVert);
    VkShaderModule vsInst   = SceneVkShaderCompiler::createBuiltinModule(device, BuiltinSceneShader::MeshInstancedVert);
    VkShaderModule vsSkin   = SceneVkShaderCompiler::createBuiltinModule(device, BuiltinSceneShader::MeshSkinnedVert);
    VkShaderModule fs       = SceneVkShaderCompiler::createBuiltinModule(device, BuiltinSceneShader::MeshFrag);

    if (!vsStatic || !vsInst || !vsSkin || !fs) {
        LOG_ERROR("PassMesh: Failed creating shader modules");
        return false;
    }

    // Common Static Vertex Input Layout: binding 0, stride = 64 bytes
    // pos (12), norm (12), uv (8), col (16), tan (16)
    VkVertexInputBindingDescription staticBinding{};
    staticBinding.binding = 0;
    staticBinding.stride = 64;
    staticBinding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    std::vector<VkVertexInputAttributeDescription> staticAttributes = {
        {0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0},    // pos
        {1, 0, VK_FORMAT_R32G32B32_SFLOAT, 12},   // normal
        {2, 0, VK_FORMAT_R32G32_SFLOAT, 24},      // uv
        {3, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 32},// color
        {4, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 48} // tangent
    };

    // 1. Static Mesh Pipeline
    SceneVkPipelineBuilder bStatic;
    bStatic.addShaderStage(VK_SHADER_STAGE_VERTEX_BIT, vsStatic)
           .addShaderStage(VK_SHADER_STAGE_FRAGMENT_BIT, fs)
           .setVertexInput({staticBinding}, staticAttributes)
           .setInputTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
           .setPolygonMode(VK_POLYGON_MODE_FILL)
           .setCullMode(config.cullMode, VK_FRONT_FACE_COUNTER_CLOCKWISE)
           .setMultisampling(config.samples)
           .disableBlending(1)
           .enableDepthTest(config.depthWrite, config.depthCompareOp)
           .setDynamicRendering({config.colorFormat}, config.depthFormat);

    pipelineStatic_ = bStatic.build(device, pipelineLayout_);

    // 1b. Static Mesh Pipeline (Translucent)
    bStatic.enableAlphaBlending(1)
           .enableDepthTest(false, config.depthCompareOp);
    pipelineStaticTranslucent_ = bStatic.build(device, pipelineLayout_);

    // 2. Instanced Mesh Pipeline (Binding 0: Mesh, Binding 1: Instance attributes)
    VkVertexInputBindingDescription instBinding{};
    instBinding.binding = 1;
    instBinding.stride = 64; // row0, row1, row2, color
    instBinding.inputRate = VK_VERTEX_INPUT_RATE_INSTANCE;

    std::vector<VkVertexInputBindingDescription> instBindings = {staticBinding, instBinding};
    std::vector<VkVertexInputAttributeDescription> instAttributes = staticAttributes;
    instAttributes.push_back({8,  1, VK_FORMAT_R32G32B32A32_SFLOAT, 0});  // row0
    instAttributes.push_back({9,  1, VK_FORMAT_R32G32B32A32_SFLOAT, 16}); // row1
    instAttributes.push_back({10, 1, VK_FORMAT_R32G32B32A32_SFLOAT, 32}); // row2
    instAttributes.push_back({11, 1, VK_FORMAT_R32G32B32A32_SFLOAT, 48}); // color

    SceneVkPipelineBuilder bInst;
    bInst.addShaderStage(VK_SHADER_STAGE_VERTEX_BIT, vsInst)
         .addShaderStage(VK_SHADER_STAGE_FRAGMENT_BIT, fs)
         .setVertexInput(instBindings, instAttributes)
         .setInputTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
         .setPolygonMode(VK_POLYGON_MODE_FILL)
         .setCullMode(config.cullMode, VK_FRONT_FACE_COUNTER_CLOCKWISE)
         .setMultisampling(config.samples)
         .disableBlending(1)
         .enableDepthTest(config.depthWrite, config.depthCompareOp)
         .setDynamicRendering({config.colorFormat}, config.depthFormat);

    pipelineInstanced_ = bInst.build(device, pipelineLayout_);

    // 2b. Instanced Mesh Pipeline (Translucent)
    bInst.enableAlphaBlending(1)
         .enableDepthTest(false, config.depthCompareOp);
    pipelineInstancedTranslucent_ = bInst.build(device, pipelineLayout_);

    // 3. Skinned Mesh Pipeline (Binding 0: Mesh, Binding 1: Skin joints + weights)
    VkVertexInputBindingDescription skinBinding{};
    skinBinding.binding = 1;
    skinBinding.stride = 24; // u16vec4 (8) + vec4 (16)
    skinBinding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    std::vector<VkVertexInputBindingDescription> skinBindings = {staticBinding, skinBinding};
    std::vector<VkVertexInputAttributeDescription> skinAttributes = staticAttributes;
    skinAttributes.push_back({5, 1, VK_FORMAT_R16G16B16A16_UINT, 0});  // joints
    skinAttributes.push_back({6, 1, VK_FORMAT_R32G32B32A32_SFLOAT, 8});// weights

    SceneVkPipelineBuilder bSkin;
    bSkin.addShaderStage(VK_SHADER_STAGE_VERTEX_BIT, vsSkin)
         .addShaderStage(VK_SHADER_STAGE_FRAGMENT_BIT, fs)
         .setVertexInput(skinBindings, skinAttributes)
         .setInputTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
         .setPolygonMode(VK_POLYGON_MODE_FILL)
         .setCullMode(config.cullMode, VK_FRONT_FACE_COUNTER_CLOCKWISE)
         .setMultisampling(config.samples)
         .disableBlending(1)
         .enableDepthTest(config.depthWrite, config.depthCompareOp)
         .setDynamicRendering({config.colorFormat}, config.depthFormat);

    pipelineSkinned_ = bSkin.build(device, pipelineLayout_);

    // 3b. Skinned Mesh Pipeline (Translucent)
    bSkin.enableAlphaBlending(1)
         .enableDepthTest(false, config.depthCompareOp);
    pipelineSkinnedTranslucent_ = bSkin.build(device, pipelineLayout_);

    SceneVkShaderModule::destroy(device, vsStatic);
    SceneVkShaderModule::destroy(device, vsInst);
    SceneVkShaderModule::destroy(device, vsSkin);
    SceneVkShaderModule::destroy(device, fs);

    return (pipelineStatic_ != VK_NULL_HANDLE &&
            pipelineInstanced_ != VK_NULL_HANDLE &&
            pipelineSkinned_ != VK_NULL_HANDLE &&
            pipelineStaticTranslucent_ != VK_NULL_HANDLE &&
            pipelineInstancedTranslucent_ != VK_NULL_HANDLE &&
            pipelineSkinnedTranslucent_ != VK_NULL_HANDLE);
}

VkPipeline PassMesh::createCustomPipeline(VkDevice device,
                                          VkShaderModule vs,
                                          VkShaderModule fs,
                                          uint32_t target,
                                          bool translucent) {
    VkVertexInputBindingDescription staticBinding{};
    staticBinding.binding = 0;
    staticBinding.stride = 64;
    staticBinding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    std::vector<VkVertexInputAttributeDescription> staticAttributes = {
        {0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0},    // pos
        {1, 0, VK_FORMAT_R32G32B32_SFLOAT, 12},   // normal
        {2, 0, VK_FORMAT_R32G32_SFLOAT, 24},      // uv
        {3, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 32},// color
        {4, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 48} // tangent
    };

    SceneVkPipelineBuilder builder;
    builder.setShaderStages(vs, fs)
           .setInputTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
           .setPolygonMode(VK_POLYGON_MODE_FILL)
           .setCullMode(config_.cullMode, VK_FRONT_FACE_COUNTER_CLOCKWISE)
           .setMultisampling(config_.samples)
           .setDynamicRendering({config_.colorFormat}, config_.depthFormat);

    if (translucent) {
        builder.enableAlphaBlending(1)
               .enableDepthTest(false, config_.depthCompareOp);
    } else {
        builder.disableBlending(1)
               .enableDepthTest(config_.depthWrite, config_.depthCompareOp);
    }

    if (target == 1 /* Instanced */) {
        VkVertexInputBindingDescription instBinding{};
        instBinding.binding = 1;
        instBinding.stride = 64;
        instBinding.inputRate = VK_VERTEX_INPUT_RATE_INSTANCE;

        std::vector<VkVertexInputBindingDescription> instBindings = {staticBinding, instBinding};
        std::vector<VkVertexInputAttributeDescription> instAttributes = staticAttributes;
        instAttributes.push_back({8,  1, VK_FORMAT_R32G32B32A32_SFLOAT, 0});  // row0
        instAttributes.push_back({9,  1, VK_FORMAT_R32G32B32A32_SFLOAT, 16}); // row1
        instAttributes.push_back({10, 1, VK_FORMAT_R32G32B32A32_SFLOAT, 32}); // row2
        instAttributes.push_back({11, 1, VK_FORMAT_R32G32B32A32_SFLOAT, 48}); // color
        builder.setVertexInput(instBindings, instAttributes);
    } else if (target == 2 /* Skinned */) {
        VkVertexInputBindingDescription skinBinding{};
        skinBinding.binding = 1;
        skinBinding.stride = 24;
        skinBinding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

        std::vector<VkVertexInputBindingDescription> skinBindings = {staticBinding, skinBinding};
        std::vector<VkVertexInputAttributeDescription> skinAttributes = staticAttributes;
        skinAttributes.push_back({5, 1, VK_FORMAT_R16G16B16A16_UINT, 0});
        skinAttributes.push_back({6, 1, VK_FORMAT_R32G32B32A32_SFLOAT, 8});
        builder.setVertexInput(skinBindings, skinAttributes);
    } else {
        builder.setVertexInput({staticBinding}, staticAttributes);
    }

    return builder.build(device, pipelineLayout_);
}

void PassMesh::begin(VkCommandBuffer cmd,
                     VkDescriptorSet cameraSet,
                     VkDescriptorSet lightingSet,
                     uint32_t viewportWidth,
                     uint32_t viewportHeight) {
    activeCameraSet_ = cameraSet;
    activeLightingSet_ = lightingSet;
    viewportWidth_ = viewportWidth;
    viewportHeight_ = viewportHeight;

    VkViewport vp{};
    vp.x = 0.0f;
    vp.y = 0.0f;
    vp.width = static_cast<float>(viewportWidth);
    vp.height = static_cast<float>(viewportHeight);
    vp.minDepth = 0.0f;
    vp.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &vp);

    VkRect2D scissor{};
    scissor.offset = {0, 0};
    scissor.extent = {viewportWidth, viewportHeight};
    vkCmdSetScissor(cmd, 0, 1, &scissor);
}

void PassMesh::drawStatic(VkCommandBuffer cmd, const MeshDrawCall& draw) {
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                      draw.customPipeline ? draw.customPipeline : pipelineStatic_);

    // Bind Camera (Set 0) and Lighting (Set 1)
    VkDescriptorSet sets[2] = {activeCameraSet_, activeLightingSet_};
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_, 0, 2, sets, 0, nullptr);

    // Bind Material (Set 2)
    VkDescriptorSet matSet = draw.materialSet ? draw.materialSet : defaultMaterialSet_;
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_, 2, 1, &matSet, 0, nullptr);

    if (draw.customSet) {
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_, 4, 1, &draw.customSet, 0, nullptr);
    }

    // Upload Push Constants
    MeshPushConstants push{};
    std::memcpy(push.model, draw.modelMatrix, sizeof(push.model));
    std::memcpy(push.baseColor, draw.baseColor, sizeof(push.baseColor));
    push.emissive[0] = draw.emissiveColor[0];
    push.emissive[1] = draw.emissiveColor[1];
    push.emissive[2] = draw.emissiveColor[2];
    push.emissive[3] = draw.emissiveIntensity;
    push.pbrParams[0] = draw.metallic;
    push.pbrParams[1] = draw.roughness;
    push.pbrParams[2] = draw.alphaCutoff;
    push.pbrParams[3] = static_cast<float>(draw.flags);

    vkCmdPushConstants(cmd, pipelineLayout_,
                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                       0, sizeof(push), &push);

    // Bind vertex and index buffers
    vkCmdBindVertexBuffers(cmd, 0, 1, &draw.vertexBuffer, &draw.vertexOffset);
    vkCmdBindIndexBuffer(cmd, draw.indexBuffer, draw.indexOffset, draw.indexType);

    vkCmdDrawIndexed(cmd, draw.indexCount, 1, 0, 0, 0);
}

void PassMesh::drawInstanced(VkCommandBuffer cmd, const InstancedMeshDrawCall& draw) {
    if (draw.instanceCount == 0) return;

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                      draw.customPipeline ? draw.customPipeline : pipelineInstanced_);

    VkDescriptorSet sets[2] = {activeCameraSet_, activeLightingSet_};
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_, 0, 2, sets, 0, nullptr);

    VkDescriptorSet matSet = draw.materialSet ? draw.materialSet : defaultMaterialSet_;
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_, 2, 1, &matSet, 0, nullptr);

    if (draw.customSet) {
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_, 4, 1, &draw.customSet, 0, nullptr);
    }

    MeshPushConstants push{};
    std::memcpy(push.model, draw.modelMatrix, sizeof(push.model));
    std::memcpy(push.baseColor, draw.baseColor, sizeof(push.baseColor));
    push.emissive[0] = draw.emissiveColor[0];
    push.emissive[1] = draw.emissiveColor[1];
    push.emissive[2] = draw.emissiveColor[2];
    push.emissive[3] = draw.emissiveIntensity;
    push.pbrParams[0] = draw.metallic;
    push.pbrParams[1] = draw.roughness;
    push.pbrParams[2] = draw.alphaCutoff;
    push.pbrParams[3] = static_cast<float>(draw.flags);

    vkCmdPushConstants(cmd, pipelineLayout_,
                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                       0, sizeof(push), &push);

    VkBuffer buffers[2] = {draw.vertexBuffer, draw.instanceBuffer};
    VkDeviceSize offsets[2] = {draw.vertexOffset, draw.instanceOffset};
    vkCmdBindVertexBuffers(cmd, 0, 2, buffers, offsets);
    vkCmdBindIndexBuffer(cmd, draw.indexBuffer, draw.indexOffset, draw.indexType);

    vkCmdDrawIndexed(cmd, draw.indexCount, draw.instanceCount, 0, 0, 0);
}

void PassMesh::drawSkinned(VkCommandBuffer cmd, const SkinnedMeshDrawCall& draw) {
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                      draw.customPipeline ? draw.customPipeline : pipelineSkinned_);

    VkDescriptorSet sets[2] = {activeCameraSet_, activeLightingSet_};
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_, 0, 2, sets, 0, nullptr);

    VkDescriptorSet matSet = draw.materialSet ? draw.materialSet : defaultMaterialSet_;
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_, 2, 1, &matSet, 0, nullptr);

    if (draw.bonePaletteSet) {
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_, 3, 1,
                               &draw.bonePaletteSet, 0, nullptr);
    }

    if (draw.customSet) {
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_, 4, 1, &draw.customSet, 0, nullptr);
    }

    MeshPushConstants push{};
    std::memcpy(push.model, draw.modelMatrix, sizeof(push.model));
    std::memcpy(push.baseColor, draw.baseColor, sizeof(push.baseColor));
    push.emissive[0] = draw.emissiveColor[0];
    push.emissive[1] = draw.emissiveColor[1];
    push.emissive[2] = draw.emissiveColor[2];
    push.emissive[3] = draw.emissiveIntensity;
    push.pbrParams[0] = draw.metallic;
    push.pbrParams[1] = draw.roughness;
    push.pbrParams[2] = draw.alphaCutoff;
    push.pbrParams[3] = static_cast<float>(draw.flags);

    vkCmdPushConstants(cmd, pipelineLayout_,
                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                       0, sizeof(push), &push);

    VkBuffer buffers[2] = {draw.vertexBuffer, draw.skinAttribBuffer};
    VkDeviceSize offsets[2] = {draw.vertexOffset, draw.skinAttribOffset};
    vkCmdBindVertexBuffers(cmd, 0, 2, buffers, offsets);
    vkCmdBindIndexBuffer(cmd, draw.indexBuffer, draw.indexOffset, draw.indexType);

    vkCmdDrawIndexed(cmd, draw.indexCount, 1, 0, 0, 0);
}

void PassMesh::drawStaticTranslucent(VkCommandBuffer cmd, const MeshDrawCall& draw) {
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                      draw.customPipeline ? draw.customPipeline : pipelineStaticTranslucent_);

    VkDescriptorSet sets[2] = {activeCameraSet_, activeLightingSet_};
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_, 0, 2, sets, 0, nullptr);

    VkDescriptorSet matSet = draw.materialSet ? draw.materialSet : defaultMaterialSet_;
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_, 2, 1, &matSet, 0, nullptr);

    if (draw.customSet) {
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_, 4, 1, &draw.customSet, 0, nullptr);
    }

    MeshPushConstants push{};
    std::memcpy(push.model, draw.modelMatrix, sizeof(push.model));
    std::memcpy(push.baseColor, draw.baseColor, sizeof(push.baseColor));
    push.emissive[0] = draw.emissiveColor[0];
    push.emissive[1] = draw.emissiveColor[1];
    push.emissive[2] = draw.emissiveColor[2];
    push.emissive[3] = draw.emissiveIntensity;
    push.pbrParams[0] = draw.metallic;
    push.pbrParams[1] = draw.roughness;
    push.pbrParams[2] = draw.alphaCutoff;
    push.pbrParams[3] = static_cast<float>(draw.flags);

    vkCmdPushConstants(cmd, pipelineLayout_,
                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                       0, sizeof(push), &push);

    vkCmdBindVertexBuffers(cmd, 0, 1, &draw.vertexBuffer, &draw.vertexOffset);
    vkCmdBindIndexBuffer(cmd, draw.indexBuffer, draw.indexOffset, draw.indexType);

    vkCmdDrawIndexed(cmd, draw.indexCount, 1, 0, 0, 0);
}

void PassMesh::drawInstancedTranslucent(VkCommandBuffer cmd, const InstancedMeshDrawCall& draw) {
    if (draw.instanceCount == 0) return;

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                      draw.customPipeline ? draw.customPipeline : pipelineInstancedTranslucent_);

    VkDescriptorSet sets[2] = {activeCameraSet_, activeLightingSet_};
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_, 0, 2, sets, 0, nullptr);

    VkDescriptorSet matSet = draw.materialSet ? draw.materialSet : defaultMaterialSet_;
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_, 2, 1, &matSet, 0, nullptr);

    if (draw.customSet) {
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_, 4, 1, &draw.customSet, 0, nullptr);
    }

    MeshPushConstants push{};
    std::memcpy(push.model, draw.modelMatrix, sizeof(push.model));
    std::memcpy(push.baseColor, draw.baseColor, sizeof(push.baseColor));
    push.emissive[0] = draw.emissiveColor[0];
    push.emissive[1] = draw.emissiveColor[1];
    push.emissive[2] = draw.emissiveColor[2];
    push.emissive[3] = draw.emissiveIntensity;
    push.pbrParams[0] = draw.metallic;
    push.pbrParams[1] = draw.roughness;
    push.pbrParams[2] = draw.alphaCutoff;
    push.pbrParams[3] = static_cast<float>(draw.flags);

    vkCmdPushConstants(cmd, pipelineLayout_,
                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                       0, sizeof(push), &push);

    VkBuffer buffers[2] = {draw.vertexBuffer, draw.instanceBuffer};
    VkDeviceSize offsets[2] = {draw.vertexOffset, draw.instanceOffset};
    vkCmdBindVertexBuffers(cmd, 0, 2, buffers, offsets);
    vkCmdBindIndexBuffer(cmd, draw.indexBuffer, draw.indexOffset, draw.indexType);

    vkCmdDrawIndexed(cmd, draw.indexCount, draw.instanceCount, 0, 0, 0);
}

void PassMesh::drawSkinnedTranslucent(VkCommandBuffer cmd, const SkinnedMeshDrawCall& draw) {
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                      draw.customPipeline ? draw.customPipeline : pipelineSkinnedTranslucent_);

    VkDescriptorSet sets[2] = {activeCameraSet_, activeLightingSet_};
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_, 0, 2, sets, 0, nullptr);

    VkDescriptorSet matSet = draw.materialSet ? draw.materialSet : defaultMaterialSet_;
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_, 2, 1, &matSet, 0, nullptr);

    if (draw.bonePaletteSet) {
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_, 3, 1,
                               &draw.bonePaletteSet, 0, nullptr);
    }

    if (draw.customSet) {
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_, 4, 1, &draw.customSet, 0, nullptr);
    }

    MeshPushConstants push{};
    std::memcpy(push.model, draw.modelMatrix, sizeof(push.model));
    std::memcpy(push.baseColor, draw.baseColor, sizeof(push.baseColor));
    push.emissive[0] = draw.emissiveColor[0];
    push.emissive[1] = draw.emissiveColor[1];
    push.emissive[2] = draw.emissiveColor[2];
    push.emissive[3] = draw.emissiveIntensity;
    push.pbrParams[0] = draw.metallic;
    push.pbrParams[1] = draw.roughness;
    push.pbrParams[2] = draw.alphaCutoff;
    push.pbrParams[3] = static_cast<float>(draw.flags);

    vkCmdPushConstants(cmd, pipelineLayout_,
                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                       0, sizeof(push), &push);

    VkBuffer buffers[2] = {draw.vertexBuffer, draw.skinAttribBuffer};
    VkDeviceSize offsets[2] = {draw.vertexOffset, draw.skinAttribOffset};
    vkCmdBindVertexBuffers(cmd, 0, 2, buffers, offsets);
    vkCmdBindIndexBuffer(cmd, draw.indexBuffer, draw.indexOffset, draw.indexType);

    vkCmdDrawIndexed(cmd, draw.indexCount, 1, 0, 0, 0);
}

} // namespace bro::scene::vk
