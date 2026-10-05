#include "scene/vulkan/pass_gaussian_splat.h"

#include "scene/gaussian_splat_node.h"
#include "scene/scene_graph.h"
#include "scene/vulkan/scene_frame.h"
#include "scene/vulkan/scene_vk_depth.h"
#include "scene/vulkan/scene_vk_descriptors.h"
#include "scene/vulkan/scene_vk_device.h"
#include "scene/vulkan/scene_vk_pipeline.h"
#include "scene/vulkan/scene_vk_shader_compiler.h"

#include <cmath>
#include <cstring>
#include <vector>

namespace bro::scene::vk {

namespace {
constexpr uint32_t kInstanceFloats = 14;   // centre 3, scale 3, quaternion 4, colour 4
}

bool PassGaussianSplat::setup(SceneGpu& gpu) {
    device_ = &gpu.device;
    allocator_ = &gpu.allocator;
    VkDevice dev = gpu.device.device();

    static const float quad[8] = {-1.0f, -1.0f, 1.0f, -1.0f, -1.0f, 1.0f, 1.0f, 1.0f};
    if (!gpu.allocator.createVertexBuffer(sizeof(quad), quad, quad_)) return false;

    SceneVkDescriptorLayoutBuilder sets;
    sets.addBinding(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT);
    setLayout_ = sets.build(dev);
    if (!setLayout_) return false;
    VkPipelineLayoutCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    info.setLayoutCount = 1;
    info.pSetLayouts = &setLayout_;
    if (vkCreatePipelineLayout(dev, &info, nullptr, &layout_) != VK_SUCCESS) return false;

    vs_ = SceneVkShaderCompiler::createBuiltinModule(dev, BuiltinSceneShader::GaussianSplatVert);
    fs_ = SceneVkShaderCompiler::createBuiltinModule(dev, BuiltinSceneShader::GaussianSplatFrag);
    return vs_ && fs_;
}

void PassGaussianSplat::cleanup(SceneGpu& gpu) {
    VkDevice dev = gpu.device.device();
    pipelines_.destroy(gpu.device);
    for (auto& [id, inst] : instances_) gpu.allocator.destroyBuffer(inst.buffer);
    instances_.clear();
    gpu.allocator.destroyBuffer(quad_);
    SceneVkShaderCompiler::destroyModule(dev, vs_);
    SceneVkShaderCompiler::destroyModule(dev, fs_);
    vs_ = fs_ = VK_NULL_HANDLE;
    if (layout_) vkDestroyPipelineLayout(dev, layout_, nullptr);
    if (setLayout_) vkDestroyDescriptorSetLayout(dev, setLayout_, nullptr);
    layout_ = VK_NULL_HANDLE;
    setLayout_ = VK_NULL_HANDLE;
}

void PassGaussianSplat::releaseNodes(SceneGpu& gpu, std::span<const uint32_t> ids) {
    for (uint32_t id : ids) {
        auto it = instances_.find(id);
        if (it == instances_.end()) continue;
        gpu.allocator.destroyBuffer(it->second.buffer);
        instances_.erase(it);
    }
}

void PassGaussianSplat::declare(const SceneFrame&, PassIO& io) const {
    io.hdr();
}

void PassGaussianSplat::record(SceneFrame& frame) {
    VkPipeline pipeline = VK_NULL_HANDLE;
    for (SceneNode* node : frame.lists.nodes) {
        if (node->type() != SceneNode::Type::GaussianSplat) continue;
        if (frame.renderer.cameraCulled(node)) {
            frame.stats.splatCulled++;
            continue;
        }
        frame.stats.splatDrawn++;
        auto& splat = static_cast<GaussianSplatNode&>(*node);
        if (splat.splatCount() == 0) continue;
        if (!pipeline) {
            pipeline = pipelines_.get(0, frame.hdrTarget, [&] {
                const std::vector<VkVertexInputBindingDescription> bindings = {
                    {0, sizeof(float) * 2, VK_VERTEX_INPUT_RATE_VERTEX},
                    {1, sizeof(float) * kInstanceFloats, VK_VERTEX_INPUT_RATE_INSTANCE},
                };
                const std::vector<VkVertexInputAttributeDescription> attributes = {
                    {0, 0, VK_FORMAT_R32G32_SFLOAT, 0},
                    {1, 1, VK_FORMAT_R32G32B32_SFLOAT, 0},
                    {2, 1, VK_FORMAT_R32G32B32_SFLOAT, sizeof(float) * 3},
                    {3, 1, VK_FORMAT_R32G32B32A32_SFLOAT, sizeof(float) * 6},
                    {4, 1, VK_FORMAT_R32G32B32A32_SFLOAT, sizeof(float) * 10},
                };
                VkPipelineColorBlendAttachmentState blend{};
                blend.blendEnable = VK_TRUE;
                blend.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
                blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
                blend.colorBlendOp = VK_BLEND_OP_ADD;
                blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
                blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
                blend.alphaBlendOp = VK_BLEND_OP_ADD;
                blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                       VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
                SceneVkPipelineBuilder b;
                b.setShaderStages(vs_, fs_)
                 .setVertexInput(bindings, attributes)
                 .setInputTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP)
                 .setPolygonMode(VK_POLYGON_MODE_FILL)
                 .setCullMode(VK_CULL_MODE_NONE)
                 .setTarget(frame.hdrTarget)
                 .setColorBlendAttachment(0, blend)
                 .enableDepthTest(false);
                return b.build(device_->device(), layout_);
            });
            if (!pipeline) return;
        }
        drawNode(frame, pipeline, splat);
    }
}

void PassGaussianSplat::drawNode(SceneFrame& frame, VkPipeline pipeline, GaussianSplatNode& node) {
    const SceneView& view = frame.view;
    if (view.width == 0 || view.height == 0) return;
    const float eye[3] = {view.eye.x, view.eye.y, view.eye.z};
    const bromath::Mat4& model = node.worldMatrix();
    const bool resorted = node.needsResort(view.view.data, eye, model);
    if (resorted) node.resort(view.view.data, eye, model);
    const auto& data = node.instanceData();
    if (data.empty()) return;

    Instances& inst = instances_[node.id()];
    const size_t bytes = data.size() * sizeof(float);
    bool upload = resorted;
    if (!inst.buffer.buffer || inst.capacity < bytes) {
        allocator_->destroyBuffer(inst.buffer);
        inst.capacity = bytes * 2;
        if (!allocator_->createBuffer(inst.capacity, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, inst.buffer)) {
            inst.capacity = 0;
            return;
        }
        upload = true;
    }
    if (upload) allocator_->stageAndUploadBuffer(inst.buffer.buffer, data.data(), bytes);

    Uniforms u{};
    std::memcpy(u.model, model.data, sizeof(u.model));
    std::memcpy(u.view, view.view.data, sizeof(u.view));
    std::memcpy(u.proj, view.proj.data, sizeof(u.proj));
    u.focal[0] = 0.5f * static_cast<float>(view.width) * std::fabs(view.proj.data[0]);
    // Signed: Vulkan's clip y points down (proj[5] < 0), so the footprint's
    // pixel offsets, projected through the same focal lengths, must too, or
    // every tilted splat draws mirrored about its row.
    u.focal[1] = 0.5f * static_cast<float>(view.height) * view.proj.data[5];
    u.viewport[0] = static_cast<float>(view.width);
    u.viewport[1] = static_cast<float>(view.height);
    const VkDescriptorBufferInfo ubo = device_->frameUniform(&u, sizeof(u));
    VkDescriptorSet set = device_->frameSet(setLayout_);
    SceneVkDescriptorWriter writer;
    writer.writeBuffer(0, ubo.buffer, ubo.range, ubo.offset);
    writer.updateSet(device_->device(), set);

    VkCommandBuffer cmd = frame.cmd;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0, 1, &set, 0, nullptr);
    const VkBuffer buffers[2] = {quad_.buffer, inst.buffer.buffer};
    const VkDeviceSize offsets[2] = {0, 0};
    vkCmdBindVertexBuffers(cmd, 0, 2, buffers, offsets);
    vkCmdDraw(cmd, 4, static_cast<uint32_t>(node.splatCount()), 0, 0);
    frame.drewContent = true;
}

}  // namespace bro::scene::vk
