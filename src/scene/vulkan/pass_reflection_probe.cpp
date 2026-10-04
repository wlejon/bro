#include "scene/vulkan/pass_reflection_probe.h"
#include "scene/vulkan/scene_vk_bridge.h"
#include "scene/scene_graph.h"
#include "scene/scene_renderer.h"
#include "scene/mesh_node.h"
#include "scene/light_node.h"
#include "util/log.h"

#include <cmath>
#include <cstring>
#include <algorithm>

namespace bro::scene::vk {

PassReflectionProbe::~PassReflectionProbe() {
}

bool PassReflectionProbe::init(SceneVkDevice& device, SceneVkAllocator& allocator) {
    cleanup(device, allocator);
    VkDevice dev = device.device();

    // 1. Cubemap linear mipmapped sampler
    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.maxAnisotropy = 1.0f;
    samplerInfo.minLod = 0.0f;
    samplerInfo.maxLod = 16.0f;

    if (vkCreateSampler(dev, &samplerInfo, nullptr, &cubemapSampler_) != VK_SUCCESS) {
        LOG_ERROR("PassReflectionProbe: Failed creating cubemap sampler");
        return false;
    }

    // 2. Dummy 1x1 black cubemap
    bool ok = allocator.createImage(1, 1, VK_FORMAT_R8G8B8A8_UNORM,
                                   VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                   VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                                   dummyCubemap_, 1, VK_SAMPLE_COUNT_1_BIT,
                                   VK_IMAGE_ASPECT_COLOR_BIT, 6,
                                   VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT);
    if (!ok) {
        LOG_ERROR("PassReflectionProbe: Failed creating dummy cubemap");
        return false;
    }


    // 3. Face Camera & Lighting descriptor layouts & buffers
    SceneVkDescriptorLayoutBuilder camBuilder;
    camBuilder.addBinding(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1,
                          VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT);
    faceCameraLayout_ = camBuilder.build(dev);

    SceneVkDescriptorLayoutBuilder lightBuilder;
    lightBuilder.addBinding(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT);
    lightBuilder.addBinding(1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT);
    lightBuilder.addBinding(2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT);
    lightBuilder.addBinding(3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT);
    faceLightingLayout_ = lightBuilder.build(dev);

    return true;
}

void PassReflectionProbe::cleanup(SceneVkDevice& device, SceneVkAllocator& allocator) {
    VkDevice dev = device.device();
    for (auto& [probe, data] : probeCache_) {
        releaseFaceViews(device, data);
        allocator.destroyImage(data.cubemap);
        allocator.destroyImage(data.depthImage);
    }
    probeCache_.clear();
    activeProbe_ = nullptr;

    if (dummyCubemap_.isValid()) {
        dummyCubemap_.sampler = VK_NULL_HANDLE;
        allocator.destroyImage(dummyCubemap_);
    }
    if (cubemapSampler_ != VK_NULL_HANDLE) {
        vkDestroySampler(dev, cubemapSampler_, nullptr);
        cubemapSampler_ = VK_NULL_HANDLE;
    }
    if (faceCameraLayout_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(dev, faceCameraLayout_, nullptr);
        faceCameraLayout_ = VK_NULL_HANDLE;
    }
    if (faceLightingLayout_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(dev, faceLightingLayout_, nullptr);
        faceLightingLayout_ = VK_NULL_HANDLE;
    }
}

bool PassReflectionProbe::ensureProbeGpu(ReflectionProbeNode* probe, SceneVkAllocator& allocator, SceneVkDevice& device) {
    auto& data = probeCache_[probe];
    int res = probe->resolution();
    if (res < 16) res = 16;
    if (res > 1024) res = 1024;

    uint32_t mips = static_cast<uint32_t>(std::floor(std::log2(res))) + 1;
    if (data.cubemap.isValid() && data.resolution == res) {
        return true;
    }

    VkDevice dev = device.device();
    releaseFaceViews(device, data);
    allocator.destroyImage(data.cubemap);
    allocator.destroyImage(data.depthImage);

    data.resolution = res;
    data.mipLevels = mips;

    // 1. Create cubemap image (RGBA16F, 6 layers, mips)
    VkImageUsageFlags cubeUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                 VK_IMAGE_USAGE_SAMPLED_BIT |
                                 VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                 VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    bool ok = allocator.createImage(res, res, VK_FORMAT_R16G16B16A16_SFLOAT,
                                   cubeUsage, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                                   data.cubemap, mips, VK_SAMPLE_COUNT_1_BIT,
                                   VK_IMAGE_ASPECT_COLOR_BIT,
                                   6, VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT);
    if (!ok) {
        LOG_ERROR("PassReflectionProbe: Failed creating cubemap image (%dx%d)", res, res);
        return false;
    }

    // 2. Create 6 face views (2D layer views)
    data.faceViews.resize(6, VK_NULL_HANDLE);
    for (uint32_t f = 0; f < 6; ++f) {
        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = data.cubemap.image;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = VK_FORMAT_R16G16B16A16_SFLOAT;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.baseMipLevel = 0;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.baseArrayLayer = f;
        viewInfo.subresourceRange.layerCount = 1;

        if (vkCreateImageView(dev, &viewInfo, nullptr, &data.faceViews[f]) != VK_SUCCESS) {
            LOG_ERROR("PassReflectionProbe: Failed creating face view %u", f);
            return false;
        }
    }

    // 3. Create depth buffer (D32_SFLOAT, res x res)
    ok = allocator.createImage(res, res, VK_FORMAT_D32_SFLOAT,
                              VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                              VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                              data.depthImage, 1, VK_SAMPLE_COUNT_1_BIT,
                              VK_IMAGE_ASPECT_DEPTH_BIT);
    if (!ok) {
        LOG_ERROR("PassReflectionProbe: Failed creating depth buffer");
        return false;
    }

    return true;
}

void PassReflectionProbe::renderFace(VkCommandBuffer cmd, ReflectionProbeNode* probe, int face,
                                    SceneGraph& graph, SceneRenderer& renderer, PassMesh& passMesh,
                                    SceneVkAllocator& allocator, SceneVkDevice& device,
                                    SceneVkBridge& bridge) {
    auto& data = probeCache_[probe];
    uint32_t res = static_cast<uint32_t>(data.resolution);

    const auto& probeWorld = probe->worldMatrix();
    bromath::Vec3 eye{probeWorld.at(0, 3), probeWorld.at(1, 3), probeWorld.at(2, 3)};

    static const bromath::Vec3 kDirs[6] = {
        {1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}
    };
    static const bromath::Vec3 kUps[6] = {
        {0, -1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}, {0, -1, 0}, {0, -1, 0}
    };

    bromath::Mat4 view = bromath::mlookAt(eye, eye + kDirs[face], kUps[face]);

    float nearZ = 0.05f;
    float farZ = 1000.0f;
    bromath::Mat4 proj = bromath::midentity();
    for (int i = 0; i < 16; ++i) proj.data[i] = 0.0f;
    proj.at(0, 0) = 1.0f;
    proj.at(1, 1) = -1.0f; // Vulkan Y-flip
    // Reversed-Z: near maps to 1, far maps to 0
    proj.at(2, 2) = nearZ / (farZ - nearZ);
    proj.at(2, 3) = (farZ * nearZ) / (farZ - nearZ);
    proj.at(3, 2) = -1.0f;

    bromath::Mat4 vp = bromath::mmul(proj, view);

    SceneCameraUniforms camUniforms{};
    std::memcpy(camUniforms.view, view.data, sizeof(camUniforms.view));
    std::memcpy(camUniforms.proj, proj.data, sizeof(camUniforms.proj));
    std::memcpy(camUniforms.viewProj, vp.data, sizeof(camUniforms.viewProj));
    camUniforms.eyePos[0] = eye.x;
    camUniforms.eyePos[1] = eye.y;
    camUniforms.eyePos[2] = eye.z;
    camUniforms.eyePos[3] = 0.0f;
    camUniforms.viewport[0] = static_cast<float>(res);
    camUniforms.viewport[1] = static_cast<float>(res);
    camUniforms.viewport[2] = nearZ;
    camUniforms.viewport[3] = farZ;

    // The face renders with its own camera and an unlit, shadowless lighting
    // block; both sets are this frame's.
    const VkDescriptorBufferInfo camInfo = device.frameUniform(&camUniforms, sizeof(camUniforms));
    VkDescriptorSet cameraSet = device.frameSet(faceCameraLayout_);
    SceneVkDescriptorWriter camWriter;
    camWriter.writeBuffer(0, camInfo.buffer, camInfo.range, camInfo.offset);
    camWriter.updateSet(device.device(), cameraSet);

    const SceneLightingUniforms noLights{};
    const VkDescriptorBufferInfo lightInfo = device.frameUniform(&noLights, sizeof(noLights));
    VkDescriptorSet lightingSet = device.frameSet(faceLightingLayout_);
    SceneVkDescriptorWriter lightWriter;
    lightWriter.writeBuffer(0, lightInfo.buffer, lightInfo.range, lightInfo.offset);
    lightWriter.writeImage(1, bridge.shadowTarget_.arrayView(), bridge.shadowTarget_.shadowSampler());
    lightWriter.writeImage(2, dummyCubemap_.view, cubemapSampler_);
    lightWriter.writeImage(3, bridge.dummyShadeMap_.view, bridge.dummyShadeMap_.sampler);
    lightWriter.updateSet(device.device(), lightingSet);

    // Dynamic rendering into face
    allocator.transitionImageLayout(cmd, data.cubemap.image, VK_FORMAT_R16G16B16A16_SFLOAT,
                                     data.cubemap.currentLayout,
                                     VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                     1, 0, VK_IMAGE_ASPECT_COLOR_BIT,
                                     1, static_cast<uint32_t>(face));

    allocator.transitionImageLayout(cmd, data.depthImage.image, VK_FORMAT_D32_SFLOAT,
                                     data.depthImage.currentLayout,
                                     VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                                     1, 0, VK_IMAGE_ASPECT_DEPTH_BIT);
    data.depthImage.currentLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;

    VkRenderingAttachmentInfoKHR colorAttachment{};
    colorAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO_KHR;
    colorAttachment.imageView = data.faceViews[face];
    colorAttachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    colorAttachment.clearValue.color = {{0.0f, 0.0f, 0.0f, 1.0f}};

    VkRenderingAttachmentInfoKHR depthAttachment{};
    depthAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO_KHR;
    depthAttachment.imageView = data.depthImage.view;
    depthAttachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depthAttachment.clearValue.depthStencil = {0.0f, 0}; // Reversed-Z clear depth is 0.0

    VkRenderingInfoKHR renderingInfo{};
    renderingInfo.sType = VK_STRUCTURE_TYPE_RENDERING_INFO_KHR;
    renderingInfo.renderArea = {{0, 0}, {res, res}};
    renderingInfo.layerCount = 1;
    renderingInfo.colorAttachmentCount = 1;
    renderingInfo.pColorAttachments = &colorAttachment;
    renderingInfo.pDepthAttachment = &depthAttachment;

    vkCmdBeginRendering(cmd, &renderingInfo);

    passMesh.begin(cmd, cameraSet, lightingSet, res, res);

    for (auto& [id, node] : graph.nodes_) {
        if (!node->renderVisible()) continue;
        if (node.get() == probe) continue;
        if (node->type() == SceneNode::Type::Mesh) {
            auto* mn = static_cast<MeshNode*>(node.get());
            if (mn->currentMesh().empty()) continue;
            // Skip mirror objects (like metallic reflection test spheres) to avoid recursion
            if (mn->metallic() > 0.8f && mn->roughness() < 0.2f) continue;

            auto& meshBuf = bridge.uploadMesh(mn->currentMesh(), mn);
            MeshDrawCall draw{};
            draw.vertexBuffer = meshBuf.vertexBuffer.buffer;
            draw.indexBuffer = meshBuf.indexBuffer.buffer;
            draw.indexCount = meshBuf.indexCount;
            std::memcpy(draw.modelMatrix, mn->worldMatrix().data, sizeof(draw.modelMatrix));
            std::memcpy(draw.baseColor, mn->color(), sizeof(draw.baseColor));
            std::memcpy(draw.emissiveColor, mn->emissiveColor(), sizeof(draw.emissiveColor));
            draw.emissiveIntensity = mn->emissive();
            draw.metallic = mn->metallic();
            draw.roughness = mn->roughness();
            draw.alphaCutoff = mn->alphaCutoff();
            draw.flags = 0;
            if (mn->effectiveUnlit()) draw.flags |= 16u;

            const auto& pTex = mn->pendingBaseTexture();
            if (pTex.w > 0 && pTex.h > 0 && !pTex.data.empty()) {
                draw.materialSet = bridge.uploadTexture(mn, pTex.w, pTex.h, pTex.data.data());
                if (draw.materialSet != VK_NULL_HANDLE) draw.flags |= 1u;
            }

            passMesh.drawStatic(cmd, draw);
        }
    }

    vkCmdEndRendering(cmd);
}

void PassReflectionProbe::updateProbes(VkCommandBuffer cmd, SceneGraph& graph, SceneRenderer& renderer,
                                      PassMesh& passMesh, SceneVkAllocator& allocator, SceneVkDevice& device,
                                      SceneVkBridge& bridge) {
    activeProbe_ = nullptr;
    std::vector<ReflectionProbeNode*> visibleProbes;

    for (auto& [id, node] : graph.nodes_) {
        if (!node->renderVisible() || node->type() != SceneNode::Type::ReflectionProbe) continue;
        auto* p = static_cast<ReflectionProbeNode*>(node.get());
        visibleProbes.push_back(p);

        if (p->updateMode() == ReflectionProbeNode::UpdateMode::Once && !p->hasData()) {
            p->requestCapture();
        }

        if (p->captureRequested()) {
            if (!ensureProbeGpu(p, allocator, device)) continue;
            auto& data = probeCache_[p];

            // Render all 6 cube faces
            for (int f = 0; f < 6; ++f) {
                renderFace(cmd, p, f, graph, renderer, passMesh, allocator, device, bridge);
            }

            // Generate mipmaps
            allocator.transitionImageLayout(cmd, data.cubemap.image, VK_FORMAT_R16G16B16A16_SFLOAT,
                                             VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                             VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                             1, 0, VK_IMAGE_ASPECT_COLOR_BIT,
                                             6, 0);

            for (uint32_t i = 1; i < data.mipLevels; ++i) {
                allocator.transitionImageLayout(cmd, data.cubemap.image, VK_FORMAT_R16G16B16A16_SFLOAT,
                                                 VK_IMAGE_LAYOUT_UNDEFINED,
                                                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                                 1, i, VK_IMAGE_ASPECT_COLOR_BIT,
                                                 6, 0);

                int32_t srcW = std::max(1, data.resolution >> (i - 1));
                int32_t srcH = std::max(1, data.resolution >> (i - 1));
                int32_t dstW = std::max(1, data.resolution >> i);
                int32_t dstH = std::max(1, data.resolution >> i);

                VkImageBlit blit{};
                blit.srcOffsets[0] = {0, 0, 0};
                blit.srcOffsets[1] = {srcW, srcH, 1};
                blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                blit.srcSubresource.mipLevel = i - 1;
                blit.srcSubresource.baseArrayLayer = 0;
                blit.srcSubresource.layerCount = 6;

                blit.dstOffsets[0] = {0, 0, 0};
                blit.dstOffsets[1] = {dstW, dstH, 1};
                blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                blit.dstSubresource.mipLevel = i;
                blit.dstSubresource.baseArrayLayer = 0;
                blit.dstSubresource.layerCount = 6;

                vkCmdBlitImage(cmd,
                               data.cubemap.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               data.cubemap.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               1, &blit, VK_FILTER_LINEAR);

                allocator.transitionImageLayout(cmd, data.cubemap.image, VK_FORMAT_R16G16B16A16_SFLOAT,
                                                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                                 VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                                 1, i, VK_IMAGE_ASPECT_COLOR_BIT,
                                                 6, 0);
            }

            allocator.transitionImageLayout(cmd, data.cubemap.image, VK_FORMAT_R16G16B16A16_SFLOAT,
                                             VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                             VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                             data.mipLevels, 0, VK_IMAGE_ASPECT_COLOR_BIT,
                                             6, 0);
            data.cubemap.currentLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

            p->markCaptured();
        }
    }

    // Select active probe: highest priority probe with data
    int bestPriority = -999999;
    for (auto* p : visibleProbes) {
        if (!p->hasData()) continue;
        if (p->priority() >= bestPriority) {
            bestPriority = p->priority();
            activeProbe_ = p;
        }
    }
}

void PassReflectionProbe::releaseFaceViews(SceneVkDevice& device, ProbeGpuData& data) {
    if (!data.faceViews.empty()) {
        VkDevice dev = device.device();
        device.defer([dev, views = data.faceViews] {
            for (VkImageView view : views) {
                if (view != VK_NULL_HANDLE) vkDestroyImageView(dev, view, nullptr);
            }
        });
    }
    data.faceViews.clear();
}

VkImageView PassReflectionProbe::activeCubemapView() const {
    if (activeProbe_) {
        auto it = probeCache_.find(activeProbe_);
        if (it != probeCache_.end() && it->second.cubemap.isValid()) {
            return it->second.cubemap.view;
        }
    }
    return dummyCubemap_.view;
}

} // namespace bro::scene::vk
