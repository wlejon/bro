#include "scene/vulkan/pass_gaussian_splat.h"
#include "scene/gaussian_splat_node.h"
#include "scene/vulkan/scene_vk_shader_compiler.h"
#include "util/log.h"
#include <cstring>

namespace bro::scene::vk {

PassGaussianSplat::~PassGaussianSplat() = default;

bool PassGaussianSplat::init(SceneVkDevice& device, SceneVkAllocator& allocator,
                             VkFormat colorFormat, VkFormat depthFormat, VkSampleCountFlagBits samples) {
    if (!createQuadBuffer(allocator)) {
        LOG_ERROR("PassGaussianSplat: Failed creating quad vertex buffer");
        return false;
    }
    colorFormat_ = colorFormat;
    depthFormat_ = depthFormat;
    samples_ = samples;
    return createPipelines(device.device(), colorFormat, depthFormat, samples);
}

bool PassGaussianSplat::setSampleCount(VkDevice dev, VkSampleCountFlagBits samples) {
    if (samples_ == samples) return true;
    destroyPipelines(dev);
    samples_ = samples;
    return createPipelines(dev, colorFormat_, depthFormat_, samples_);
}

void PassGaussianSplat::cleanup(SceneVkDevice& device, SceneVkAllocator& allocator) {
    allocator.destroyBuffer(quadBuffer_);

    for (auto& [key, data] : nodeCache_) {
        allocator.destroyBuffer(data.instanceBuffer);
    }
    nodeCache_.clear();
    destroyPipelines(device.device());
}

void PassGaussianSplat::destroyPipelines(VkDevice dev) {
    if (pipeline_ != VK_NULL_HANDLE) {
        vkDestroyPipeline(dev, pipeline_, nullptr);
        pipeline_ = VK_NULL_HANDLE;
    }
    if (pipelineLayout_ != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(dev, pipelineLayout_, nullptr);
        pipelineLayout_ = VK_NULL_HANDLE;
    }
    if (descLayout_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(dev, descLayout_, nullptr);
        descLayout_ = VK_NULL_HANDLE;
    }
}

bool PassGaussianSplat::createQuadBuffer(SceneVkAllocator& allocator) {
    static const float quad[8] = {
        -1.0f, -1.0f,
         1.0f, -1.0f,
        -1.0f,  1.0f,
         1.0f,  1.0f,
    };
    return allocator.createVertexBuffer(sizeof(quad), quad, quadBuffer_);
}

bool PassGaussianSplat::createPipelines(VkDevice device, VkFormat colorFormat, VkFormat depthFormat, VkSampleCountFlagBits samples) {
    SceneVkDescriptorLayoutBuilder descBuilder;
    descBuilder.addBinding(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT);
    descLayout_ = descBuilder.build(device);
    if (!descLayout_) return false;

    VkPipelineLayoutCreateInfo plInfo{};
    plInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plInfo.setLayoutCount = 1;
    plInfo.pSetLayouts = &descLayout_;
    if (vkCreatePipelineLayout(device, &plInfo, nullptr, &pipelineLayout_) != VK_SUCCESS) return false;

    VkShaderModule vsMod = SceneVkShaderCompiler::createBuiltinModule(device, BuiltinSceneShader::GaussianSplatVert);
    VkShaderModule fsMod = SceneVkShaderCompiler::createBuiltinModule(device, BuiltinSceneShader::GaussianSplatFrag);
    if (!vsMod || !fsMod) {
        if (vsMod) SceneVkShaderCompiler::destroyModule(device, vsMod);
        if (fsMod) SceneVkShaderCompiler::destroyModule(device, fsMod);
        return false;
    }

    // Vertex bindings
    VkVertexInputBindingDescription bindings[2]{};
    // Binding 0: Quad corner (vec2)
    bindings[0].binding = 0;
    bindings[0].stride = sizeof(float) * 2;
    bindings[0].inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    // Binding 1: Instance data (14 floats: pos 3, scale 3, quat 4, color 4)
    bindings[1].binding = 1;
    bindings[1].stride = sizeof(float) * 14;
    bindings[1].inputRate = VK_VERTEX_INPUT_RATE_INSTANCE;

    VkVertexInputAttributeDescription attribs[5]{};
    // location 0: aCorner
    attribs[0].location = 0;
    attribs[0].binding = 0;
    attribs[0].format = VK_FORMAT_R32G32_SFLOAT;
    attribs[0].offset = 0;

    // location 1: aCenter
    attribs[1].location = 1;
    attribs[1].binding = 1;
    attribs[1].format = VK_FORMAT_R32G32B32_SFLOAT;
    attribs[1].offset = 0;

    // location 2: aScale
    attribs[2].location = 2;
    attribs[2].binding = 1;
    attribs[2].format = VK_FORMAT_R32G32B32_SFLOAT;
    attribs[2].offset = sizeof(float) * 3;

    // location 3: aQuat
    attribs[3].location = 3;
    attribs[3].binding = 1;
    attribs[3].format = VK_FORMAT_R32G32B32A32_SFLOAT;
    attribs[3].offset = sizeof(float) * 6;

    // location 4: aColor
    attribs[4].location = 4;
    attribs[4].binding = 1;
    attribs[4].format = VK_FORMAT_R32G32B32A32_SFLOAT;
    attribs[4].offset = sizeof(float) * 10;

    VkPipelineColorBlendAttachmentState splatBlend{};
    splatBlend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                               VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    splatBlend.blendEnable = VK_TRUE;
    splatBlend.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
    splatBlend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    splatBlend.colorBlendOp = VK_BLEND_OP_ADD;
    splatBlend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    splatBlend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    splatBlend.alphaBlendOp = VK_BLEND_OP_ADD;

    SceneVkPipelineBuilder builder;
    builder.setShaderStages(vsMod, fsMod)
           .setInputTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP)
           .setCullMode(VK_CULL_MODE_NONE)
           .setMultisampling(samples)
           .setColorBlendAttachment(0, splatBlend)
           .enableDepthTest(false, VK_COMPARE_OP_GREATER_OR_EQUAL)
           .setDynamicRendering({colorFormat}, depthFormat);

    builder.setVertexInput(
        std::vector<VkVertexInputBindingDescription>(bindings, bindings + 2),
        std::vector<VkVertexInputAttributeDescription>(attribs, attribs + 5)
    );

    pipeline_ = builder.build(device, pipelineLayout_);

    SceneVkShaderCompiler::destroyModule(device, vsMod);
    SceneVkShaderCompiler::destroyModule(device, fsMod);

    return pipeline_ != VK_NULL_HANDLE;
}

void PassGaussianSplat::renderNode(VkCommandBuffer cmd, SceneVkDevice& device, SceneVkAllocator& allocator,
                                  GaussianSplatNode* node,
                                  const float* viewMatrix,
                                  const float* projMatrix,
                                  const float eye[3],
                                  uint32_t width, uint32_t height) {
    if (!node || node->splatCount() == 0 || width == 0 || height == 0) return;

    const bromath::Mat4& model = node->worldMatrix();
    const bool resorted = node->needsResort(viewMatrix, eye, model);
    if (resorted) node->resort(viewMatrix, eye, model);
    const auto& instData = node->instanceData();
    if (instData.empty()) return;

    // The instance data only changes when the splats are re-sorted; it lives
    // in a device-local buffer that the upload stream rewrites then.
    NodeGpuData& gpuData = nodeCache_[node];
    const size_t reqBytes = instData.size() * sizeof(float);
    bool upload = resorted;
    if (!gpuData.instanceBuffer.buffer || gpuData.capacityBytes < reqBytes) {
        allocator.destroyBuffer(gpuData.instanceBuffer);
        gpuData.capacityBytes = reqBytes * 2;
        if (!allocator.createBuffer(gpuData.capacityBytes,
                                    VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                    VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, gpuData.instanceBuffer)) {
            gpuData.capacityBytes = 0;
            return;
        }
        upload = true;
    }
    if (upload) allocator.stageAndUploadBuffer(gpuData.instanceBuffer.buffer, instData.data(), reqBytes);

    SplatUBOData ubo{};
    std::memcpy(ubo.model, model.data, sizeof(ubo.model));
    std::memcpy(ubo.view, viewMatrix, sizeof(ubo.view));
    std::memcpy(ubo.proj, projMatrix, sizeof(ubo.proj));
    ubo.focal[0] = 0.5f * static_cast<float>(width) * std::fabs(projMatrix[0]);
    ubo.focal[1] = 0.5f * static_cast<float>(height) * std::fabs(projMatrix[5]);
    ubo.viewport[0] = static_cast<float>(width);
    ubo.viewport[1] = static_cast<float>(height);
    const VkDescriptorBufferInfo uboInfo = device.frameUniform(&ubo, sizeof(ubo));

    VkDescriptorSet dSet = device.frameSet(descLayout_);
    SceneVkDescriptorWriter writer;
    writer.writeBuffer(0, uboInfo.buffer, uboInfo.range, uboInfo.offset);
    writer.updateSet(device.device(), dSet);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_, 0, 1, &dSet, 0, nullptr);

    VkBuffer vtxBuffers[2] = { quadBuffer_.buffer, gpuData.instanceBuffer.buffer };
    VkDeviceSize offsets[2] = { 0, 0 };
    vkCmdBindVertexBuffers(cmd, 0, 2, vtxBuffers, offsets);

    vkCmdDraw(cmd, 4, static_cast<uint32_t>(node->splatCount()), 0, 0);
}

} // namespace bro::scene::vk
