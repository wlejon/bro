// The per-render state of the scene bridge: camera and lighting uniforms and
// their descriptor sets (fresh from the frame each render, so an in-flight
// frame never sees the next one's values), and the on-demand CPU readback of
// the tonemapped result.

#include "scene/vulkan/scene_vk_bridge.h"
#include "scene/scene_graph.h"
#include "scene/scene_renderer.h"
#include "scene/mesh_node.h"
#include "scene/instanced_mesh_node.h"
#include "scene/light_node.h"
#include "scene/reflection_probe_node.h"
#include "render/vulkan_util.h"
#include "util/log.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace bro::scene::vk {

SceneCameraUniforms SceneVkBridge::buildCameraUniforms(SceneGraph& graph, SceneRenderer& renderer,
                                                       uint32_t width, uint32_t height) const {
    SceneCameraUniforms cam{};
    std::memcpy(cam.view, graph.viewMatrix().data, sizeof(cam.view));

    bromath::Mat4 proj = graph.projectionMatrix();
    proj.at(1, 0) *= -1.0f;
    proj.at(1, 1) *= -1.0f;
    proj.at(1, 2) *= -1.0f;
    proj.at(1, 3) *= -1.0f;
    std::memcpy(cam.proj, proj.data, sizeof(cam.proj));

    bromath::Mat4 vp = bromath::mmul(proj, graph.viewMatrix());
    std::memcpy(cam.viewProj, vp.data, sizeof(cam.viewProj));
    bromath::Mat4 invView = bromath::minverse(graph.viewMatrix());
    std::memcpy(cam.invView, invView.data, sizeof(cam.invView));
    bromath::Mat4 invProj = bromath::minverse(proj);
    std::memcpy(cam.invProj, invProj.data, sizeof(cam.invProj));

    cam.eyePos[0] = graph.cameraEye().x;
    cam.eyePos[1] = graph.cameraEye().y;
    cam.eyePos[2] = graph.cameraEye().z;
    cam.eyePos[3] = 0.0f;

    cam.viewport[0] = static_cast<float>(width);
    cam.viewport[1] = static_cast<float>(height);
    cam.viewport[2] = graph.cameraNearZ_;
    cam.viewport[3] = graph.cameraFarZ_;

    cam.fogParams[0] = renderer.fogStart();
    cam.fogParams[1] = renderer.fogEnd();
    cam.fogParams[2] = renderer.fogDensity();
    cam.fogParams[3] = renderer.fogStartDist();

    cam.fogColor[0] = renderer.fogColor()[0];
    cam.fogColor[1] = renderer.fogColor()[1];
    cam.fogColor[2] = renderer.fogColor()[2];
    cam.fogColor[3] = renderer.fogHeightFalloff();
    return cam;
}

// Sun, punctual lights, ambient and the shadow projection. The probe and
// shade-map fields are filled by finishLightingUniforms once the probes have
// been captured.
SceneLightingUniforms SceneVkBridge::buildLightingUniforms(SceneGraph& graph, SceneRenderer& renderer) const {
    SceneLightingUniforms light{};
    LightNode* sunLight = nullptr;
    std::vector<LightNode*> otherLights;
    for (auto& [id, node] : graph.nodes_) {
        if (!node->renderVisible() || node->type() != SceneNode::Type::Light) continue;
        auto* l = static_cast<LightNode*>(node.get());
        if (l->kind() == LightNode::Kind::Directional) {
            if (!sunLight) {
                sunLight = l;
            } else if (l->castsShadow() && !sunLight->castsShadow()) {
                otherLights.push_back(sunLight);
                sunLight = l;
            } else {
                otherLights.push_back(l);
            }
        } else if (l->kind() == LightNode::Kind::Point || l->kind() == LightNode::Kind::Spot) {
            otherLights.push_back(l);
        }
    }

    if (sunLight) {
        bromath::Vec3 dir = bromath::vnorm(sunLight->direction());
        light.sunDirection[0] = dir.x;
        light.sunDirection[1] = dir.y;
        light.sunDirection[2] = dir.z;
        light.sunDirection[3] = 1.0f;
        const auto& col = sunLight->color();
        light.sunColor[0] = col.x;
        light.sunColor[1] = col.y;
        light.sunColor[2] = col.z;
        light.sunColor[3] = sunLight->intensity();
        light.numLights[0] = 1.0f;
        if (sunLight->castsShadow()) light.numLights[2] = 1.0f;
    } else if (otherLights.empty()) {
        const float dLen = std::sqrt(0.09f + 1.0f + 0.25f);
        light.sunDirection[0] = -0.3f / dLen;
        light.sunDirection[1] = -1.0f / dLen;
        light.sunDirection[2] = -0.5f / dLen;
        light.sunDirection[3] = 1.0f;
        light.sunColor[0] = 1.0f;
        light.sunColor[1] = 0.98f;
        light.sunColor[2] = 0.95f;
        light.sunColor[3] = 3.0f;
        light.numLights[0] = 1.0f;
    } else {
        light.numLights[0] = 0.0f;
    }

    const size_t count = std::min(otherLights.size(), size_t(16));
    light.numLights[1] = static_cast<float>(count);
    for (size_t i = 0; i < count; ++i) {
        LightNode* l = otherLights[i];
        if (l->kind() == LightNode::Kind::Directional) {
            bromath::Vec3 dir = bromath::vnorm(l->direction());
            light.pointLights[i].position[0] = dir.x;
            light.pointLights[i].position[1] = dir.y;
            light.pointLights[i].position[2] = dir.z;
            light.pointLights[i].position[3] = -1.0f;
        } else {
            const auto& M = l->worldMatrix();
            light.pointLights[i].position[0] = M.at(0, 3);
            light.pointLights[i].position[1] = M.at(1, 3);
            light.pointLights[i].position[2] = M.at(2, 3);
            light.pointLights[i].position[3] = l->range();
        }
        const auto& c = l->color();
        light.pointLights[i].color[0] = c.x;
        light.pointLights[i].color[1] = c.y;
        light.pointLights[i].color[2] = c.z;
        light.pointLights[i].color[3] = l->intensity();
    }

    const float* amb = renderer.effectiveAmbient();
    light.ambientColor[0] = amb[0];
    light.ambientColor[1] = amb[1];
    light.ambientColor[2] = amb[2];
    light.ambientColor[3] = 1.0f;

    const bromath::Vec3 lightDir = {light.sunDirection[0], light.sunDirection[1], light.sunDirection[2]};
    const bromath::Vec3 fwd = {-graph.viewMatrix().at(2, 0), -graph.viewMatrix().at(2, 1),
                               -graph.viewMatrix().at(2, 2)};
    const bromath::Vec3 sceneCenter = graph.cameraEye() + fwd * 8.0f;
    const bromath::Vec3 lightEye = sceneCenter - lightDir * 25.0f;
    const bromath::Vec3 lightUp = (std::abs(lightDir.y) > 0.99f) ? bromath::Vec3{0, 0, 1} : bromath::Vec3{0, 1, 0};
    const bromath::Mat4 lightView = bromath::mlookAt(lightEye, sceneCenter, lightUp);

    const float orthoSize = 12.0f;
    const float znear = 1.0f;
    const float zfar = 50.0f;
    bromath::Mat4 lightProj = bromath::midentity();
    for (int i = 0; i < 16; ++i) lightProj.data[i] = 0.0f;
    lightProj.at(0, 0) = 1.0f / orthoSize;
    lightProj.at(1, 1) = -1.0f / orthoSize;  // Vulkan Y-flip
    lightProj.at(2, 2) = -1.0f / (zfar - znear);
    lightProj.at(2, 3) = -znear / (zfar - znear);
    lightProj.at(3, 3) = 1.0f;
    const bromath::Mat4 lightVP = bromath::mmul(lightProj, lightView);
    std::memcpy(light.shadowCascadeProj, lightVP.data, sizeof(light.shadowCascadeProj));
    return light;
}

// The active reflection probe and the tile shade map, after updateProbes.
void SceneVkBridge::finishLightingUniforms(SceneGraph& graph, SceneLightingUniforms& light) {
    if (passReflectionProbe_.hasActiveProbe()) {
        const auto* probe = passReflectionProbe_.activeProbe();
        const auto& pw = probe->worldMatrix();
        bromath::Mat4 invPw = bromath::minverse(pw);
        std::memcpy(light.probeWorldToLocal, invPw.data, sizeof(light.probeWorldToLocal));
        std::memcpy(light.probeLocalToWorld, pw.data, sizeof(light.probeLocalToWorld));
        light.probePos[0] = pw.at(0, 3);
        light.probePos[1] = pw.at(1, 3);
        light.probePos[2] = pw.at(2, 3);
        light.probePos[3] = 1.0f;

        auto axisLen = [&](int c) {
            return std::sqrt(pw.at(0, c) * pw.at(0, c) + pw.at(1, c) * pw.at(1, c) + pw.at(2, c) * pw.at(2, c));
        };
        light.probeBoxSize[0] = axisLen(0);
        light.probeBoxSize[1] = axisLen(1);
        light.probeBoxSize[2] = axisLen(2);
        light.probeBoxSize[3] = probe->boxProjection() ? 1.0f : 0.0f;

        const uint32_t mips = static_cast<uint32_t>(std::floor(std::log2(probe->resolution()))) + 1;
        light.probeParams[0] = probe->intensity();
        light.probeParams[1] = probe->interior();
        light.probeParams[2] = static_cast<float>(mips > 0 ? mips - 1 : 0);
        light.probeParams[3] = 0.0f;
    } else {
        light.probePos[3] = 0.0f;
    }

    ShadeMapBinding shadeB{};
    bool hasShade = false;
    for (auto& [id, node] : graph.nodes_) {
        if (!node->renderVisible()) continue;
        if (node->type() == SceneNode::Type::Mesh) {
            auto* mn = static_cast<MeshNode*>(node.get());
            if (mn->shadeMap() && (*mn->shadeMap())(shadeB) && shadeB.pixels && shadeB.width > 0 && shadeB.height > 0) {
                hasShade = true;
                break;
            }
        } else if (node->type() == SceneNode::Type::InstancedMesh) {
            auto* im = static_cast<InstancedMeshNode*>(node.get());
            if (im->shadeMap() && (*im->shadeMap())(shadeB) && shadeB.pixels && shadeB.width > 0 && shadeB.height > 0) {
                hasShade = true;
                break;
            }
        }
    }

    shadeView_ = dummyShadeMap_.view;
    shadeSampler_ = dummyShadeMap_.sampler;
    if (!hasShade) {
        light.shadeOrigin[3] = 0.0f;
        return;
    }

    uint64_t hash = 14695981039346656037ULL;
    const size_t pixelCount = static_cast<size_t>(shadeB.width) * shadeB.height;
    for (size_t i = 0; i < pixelCount; ++i) {
        hash ^= shadeB.pixels[i];
        hash *= 1099511628211ULL;
    }
    if (!shadeMapImage_.isValid() || shadeMapW_ != shadeB.width || shadeMapH_ != shadeB.height || shadeMapHash_ != hash) {
        allocator_.destroyImage(shadeMapImage_);
        TextureDesc sDesc{};
        sDesc.width = shadeB.width;
        sDesc.height = shadeB.height;
        sDesc.format = VK_FORMAT_R8_UNORM;
        sDesc.magFilter = VK_FILTER_NEAREST;
        sDesc.minFilter = VK_FILTER_NEAREST;
        sDesc.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sDesc.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sDesc.generateMipmaps = false;
        allocator_.createTexture2D(shadeB.pixels, sDesc, shadeMapImage_);
        shadeMapW_ = shadeB.width;
        shadeMapH_ = shadeB.height;
        shadeMapHash_ = hash;
    }
    if (shadeMapImage_.isValid()) {
        shadeView_ = shadeMapImage_.view;
        shadeSampler_ = shadeMapImage_.sampler;
    }
    light.shadeOrigin[0] = shadeB.origin.x;
    light.shadeOrigin[1] = shadeB.origin.y;
    light.shadeOrigin[2] = shadeB.origin.z;
    light.shadeOrigin[3] = 1.0f;
    light.shadeParams[0] = shadeB.cellSize;
    light.shadeParams[1] = shadeB.hex ? 1.0f : 0.0f;
    light.shadeParams[2] = static_cast<float>(shadeB.width);
    light.shadeParams[3] = static_cast<float>(shadeB.height);
}

void SceneVkBridge::writeFrameSets(const SceneCameraUniforms& cam, const SceneLightingUniforms& light) {
    const VkDescriptorBufferInfo camInfo = device_.frameUniform(&cam, sizeof(cam));
    cameraSet_ = device_.frameSet(passMesh_.cameraLayout());
    SceneVkDescriptorWriter camWriter;
    camWriter.writeBuffer(0, camInfo.buffer, camInfo.range, camInfo.offset);
    camWriter.updateSet(device_.device(), cameraSet_);

    const VkDescriptorBufferInfo lightInfo = device_.frameUniform(&light, sizeof(light));
    lightingSet_ = device_.frameSet(passMesh_.lightingLayout());
    SceneVkDescriptorWriter lightWriter;
    lightWriter.writeBuffer(0, lightInfo.buffer, lightInfo.range, lightInfo.offset);
    lightWriter.writeImage(1, shadowTarget_.arrayView(), VK_NULL_HANDLE);  // immutable compare sampler
    lightWriter.writeImage(2, passReflectionProbe_.hasActiveProbe() ? passReflectionProbe_.activeCubemapView()
                                                                    : passReflectionProbe_.dummyCubemapView(),
                           passReflectionProbe_.activeCubemapSampler());
    lightWriter.writeImage(3, shadeView_, shadeSampler_);
    lightWriter.updateSet(device_.device(), lightingSet_);
}

bool SceneVkBridge::ensureReadbackBuffer() {
    const VkDeviceSize size = static_cast<VkDeviceSize>(currentWidth_) * currentHeight_ * 4;
    if (readbackBuffer_.isValid() && readbackBuffer_.size >= size) return true;
    allocator_.destroyBuffer(readbackBuffer_);
    const auto& memProps = device_.context().memoryProperties();
    constexpr VkMemoryPropertyFlags kCached = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                              VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
                                              VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
    const VkMemoryPropertyFlags flags = render::findMemoryType(memProps, ~0u, kCached)
        ? kCached
        : VkMemoryPropertyFlags(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (!allocator_.createBuffer(size, VK_BUFFER_USAGE_TRANSFER_DST_BIT, flags, readbackBuffer_) ||
        !readbackBuffer_.mappedData) {
        LOG_ERROR("SceneVkBridge: Failed creating the readback buffer");
        allocator_.destroyBuffer(readbackBuffer_);
        return false;
    }
    return true;
}

// Copy the LDR result into the readback buffer. The image is sampleable (or
// still an attachment, inside render3D) before and sampleable after.
void SceneVkBridge::recordReadback(VkCommandBuffer cmd) {
    allocator_.transitionImageLayout(cmd, ldrPresentationImage_.image, VK_FORMAT_R8G8B8A8_UNORM,
                                     ldrPresentationImage_.currentLayout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    // The previous copy into the buffer (a frame the CPU never read) is done.
    render::cmdBufferBarrier(cmd, readbackBuffer_.buffer, 0, VK_WHOLE_SIZE,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    VkBufferImageCopy region{};
    region.bufferRowLength = currentWidth_;
    region.bufferImageHeight = currentHeight_;
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {currentWidth_, currentHeight_, 1};
    vkCmdCopyImageToBuffer(cmd, ldrPresentationImage_.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           readbackBuffer_.buffer, 1, &region);
    render::cmdBufferBarrier(cmd, readbackBuffer_.buffer, 0, VK_WHOLE_SIZE,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                             VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_READ_BIT);
    allocator_.transitionImageLayout(cmd, ldrPresentationImage_.image, VK_FORMAT_R8G8B8A8_UNORM,
                                     VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    ldrPresentationImage_.currentLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
}

std::vector<uint8_t> SceneVkBridge::readTonemapPixelsRGBA(int& outW, int& outH) {
    outW = outH = 0;
    if (renderSerial_ == 0 || !ldrPresentationImage_.isValid() || currentWidth_ == 0 || currentHeight_ == 0)
        return {};
    readSinceRender_ = true;

    if (pixelsSerial_ != renderSerial_) {
        if (readbackRecordedSerial_ != renderSerial_) {
            // Not predicted: copy now, in a submission of its own.
            if (!ensureReadbackBuffer()) return {};
            VkCommandBuffer cmd = device_.frames().beginCommands();
            recordReadback(cmd);
            readbackTicket_ = device_.frames().submit(cmd);
            readbackRecordedSerial_ = renderSerial_;
        }
        if (readbackTicket_ == 0 || !device_.context().queue().wait(readbackTicket_)) {
            LOG_ERROR("SceneVkBridge: waiting for the tonemap readback failed");
            return {};
        }
        const size_t size = static_cast<size_t>(currentWidth_) * currentHeight_ * 4;
        const auto* src = static_cast<const uint8_t*>(readbackBuffer_.mappedData);
        pixels_.assign(src, src + size);
        pixelsSerial_ = renderSerial_;
    }
    outW = static_cast<int>(currentWidth_);
    outH = static_cast<int>(currentHeight_);
    return pixels_;
}

} // namespace bro::scene::vk
