#include "scene/vulkan/pass_reflection_probe.h"

#include "scene/depth_policy.h"
#include "scene/mesh_node.h"
#include "scene/reflection_probe_node.h"
#include "scene/scene_graph.h"
#include "scene/vulkan/scene_defaults.h"
#include "scene/vulkan/scene_frame.h"
#include "scene/vulkan/scene_gpu_resources.h"
#include "scene/vulkan/scene_lighting.h"
#include "scene/vulkan/scene_mesh_drawer.h"
#include "scene/vulkan/scene_targets.h"
#include "scene/vulkan/scene_vk_depth.h"
#include "scene/vulkan/scene_vk_device.h"
#include "util/log.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstring>

namespace bro::scene::vk {

namespace {

constexpr VkFormat kCubeFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
constexpr float kFaceNear = 0.05f;
constexpr float kFaceFar = 1000.0f;

// Cube face order +X, -X, +Y, -Y, +Z, -Z with the conventional face ups.
const bromath::Vec3 kFaceDirs[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
const bromath::Vec3 kFaceUps[6] = {{0, -1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}, {0, -1, 0}, {0, -1, 0}};

}  // namespace

bool PassReflectionProbe::setup(SceneGpu&) {
    return true;
}

void PassReflectionProbe::release(SceneGpu& gpu, Probe& probe) {
    VkDevice dev = gpu.device.device();
    gpu.device.defer([dev, views = probe.faces] {
        for (VkImageView v : views) {
            if (v != VK_NULL_HANDLE) vkDestroyImageView(dev, v, nullptr);
        }
    });
    probe.faces = {};
    gpu.allocator.destroyImage(probe.cube);
    gpu.allocator.destroyImage(probe.depth);
}

void PassReflectionProbe::releaseNodes(SceneGpu& gpu, std::span<const uint32_t> ids) {
    for (uint32_t id : ids) {
        auto it = probes_.find(id);
        if (it == probes_.end()) continue;
        release(gpu, it->second);
        probes_.erase(it);
    }
}

void PassReflectionProbe::cleanup(SceneGpu& gpu) {
    for (auto& [id, probe] : probes_) release(gpu, probe);
    probes_.clear();
}

PassReflectionProbe::Probe* PassReflectionProbe::ensure(SceneGpu& gpu, const ReflectionProbeNode& node) {
    Probe& probe = probes_[node.id()];
    const int res = std::clamp(node.resolution(), 16, 1024);
    if (probe.cube.isValid() && probe.resolution == res) return &probe;
    release(gpu, probe);

    probe.resolution = res;
    probe.mipLevels = static_cast<uint32_t>(std::floor(std::log2(res))) + 1;
    const auto size = static_cast<uint32_t>(res);
    if (!gpu.allocator.createImage(size, size, kCubeFormat,
                                   VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                       VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                   VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, probe.cube, probe.mipLevels,
                                   VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_ASPECT_COLOR_BIT, 6,
                                   VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT)) {
        LOG_ERROR("PassReflectionProbe: Failed creating a %dx%d cube", res, res);
        probes_.erase(node.id());
        return nullptr;
    }
    for (uint32_t f = 0; f < 6; ++f) {
        VkImageViewCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        info.image = probe.cube.image;
        info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        info.format = kCubeFormat;
        info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, f, 1};
        if (vkCreateImageView(gpu.device.device(), &info, nullptr, &probe.faces[f]) != VK_SUCCESS) {
            LOG_ERROR("PassReflectionProbe: Failed creating face view %u", f);
            release(gpu, probe);
            probes_.erase(node.id());
            return nullptr;
        }
    }
    if (!gpu.allocator.createImage(size, size, SceneTargets::kDepthFormat, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                                   VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, probe.depth, 1, VK_SAMPLE_COUNT_1_BIT,
                                   VK_IMAGE_ASPECT_DEPTH_BIT)) {
        LOG_ERROR("PassReflectionProbe: Failed creating the face depth buffer");
        release(gpu, probe);
        probes_.erase(node.id());
        return nullptr;
    }
    return &probe;
}

void PassReflectionProbe::declare(const SceneFrame& frame, PassIO& io) const {
    // The faces bind a lighting set, whose shadow binding must be readable.
    io.sample(frame.gpu.targets.shadow);
}

void PassReflectionProbe::record(SceneFrame& frame) {
    const ReflectionProbeNode* best = nullptr;
    int bestPriority = INT_MIN;
    for (auto& [id, owned] : frame.graph.nodes()) {
        if (!owned->renderVisible() || owned->type() != SceneNode::Type::ReflectionProbe) continue;
        auto* node = static_cast<ReflectionProbeNode*>(owned.get());
        if (node->updateMode() == ReflectionProbeNode::UpdateMode::Once && !node->hasData()) node->requestCapture();
        if (node->captureRequested()) {
            if (Probe* probe = ensure(frame.gpu, *node)) {
                capture(frame, *node, *probe);
                node->markCaptured();
            }
        }
        if (node->hasData() && node->priority() >= bestPriority && probes_.count(node->id())) {
            bestPriority = node->priority();
            best = node;
        }
    }
    if (best) {
        const Probe& probe = probes_.at(best->id());
        frame.probe.node = best;
        frame.probe.view = probe.cube.view;
        frame.probe.mipLevels = probe.mipLevels;
    }
}

void PassReflectionProbe::capture(SceneFrame& frame, const ReflectionProbeNode& node, Probe& probe) {
    SceneVkAllocator& alloc = frame.gpu.allocator;
    alloc.transitionImageLayout(frame.cmd, probe.cube.image, kCubeFormat, probe.cube.currentLayout,
                                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 1, 0, VK_IMAGE_ASPECT_COLOR_BIT, 6, 0);
    alloc.transitionImageLayout(frame.cmd, probe.depth.image, SceneTargets::kDepthFormat, probe.depth.currentLayout,
                                VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, 1, 0, VK_IMAGE_ASPECT_DEPTH_BIT);
    probe.depth.currentLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    for (int f = 0; f < 6; ++f) renderFace(frame, node, probe, f);
    buildMips(frame, probe);
}

void PassReflectionProbe::renderFace(SceneFrame& frame, const ReflectionProbeNode& node, Probe& probe, int face) {
    SceneGpu& gpu = frame.gpu;
    VkCommandBuffer cmd = frame.cmd;
    const auto res = static_cast<uint32_t>(probe.resolution);

    const auto& world = node.worldMatrix();
    const bromath::Vec3 eye{world.at(0, 3), world.at(1, 3), world.at(2, 3)};
    const SceneView view = SceneView::make(bromath::mlookAt(eye, eye + kFaceDirs[face], kFaceUps[face]),
                                           makePerspective(3.14159265358979f * 0.5f, 1.0f, kFaceNear, kFaceFar),
                                           eye, kFaceNear, kFaceFar, true, res, res);

    // The face's own camera, and a lighting block with no lights (unfogged, unlit).
    const SceneCameraUniforms cam = view.uniforms(nullptr);
    const VkDescriptorBufferInfo camInfo = gpu.device.frameUniform(&cam, sizeof(cam));
    VkDescriptorSet cameraSet = gpu.device.frameSet(gpu.defaults.cameraLayout);
    SceneVkDescriptorWriter camWriter;
    camWriter.writeBuffer(0, camInfo.buffer, camInfo.range, camInfo.offset);
    camWriter.updateSet(gpu.device.device(), cameraSet);
    const SceneLightingUniforms noLights{};
    VkDescriptorSet lightingSet = writeLightingSet(gpu, noLights, VK_NULL_HANDLE, nullptr);

    VkRenderingAttachmentInfo color{};
    color.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    color.imageView = probe.faces[face];
    color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color.clearValue.color = {{0.0f, 0.0f, 0.0f, 1.0f}};
    VkRenderingAttachmentInfo depthAtt{};
    depthAtt.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    depthAtt.imageView = probe.depth.view;
    depthAtt.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    depthAtt.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depthAtt.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depthAtt.clearValue.depthStencil = {depth::clearFar(), 0};
    VkRenderingInfo info{};
    info.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    info.renderArea = {{0, 0}, {res, res}};
    info.layerCount = 1;
    info.colorAttachmentCount = 1;
    info.pColorAttachments = &color;
    info.pDepthAttachment = &depthAtt;
    gpu.device.cmdBeginRendering(cmd, &info);
    const VkViewport viewport{0.0f, 0.0f, static_cast<float>(res), static_cast<float>(res), 0.0f, 1.0f};
    const VkRect2D scissor{{0, 0}, {res, res}};
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    TargetFormat target = TargetFormat::colorOnly(kCubeFormat);
    target.depth = SceneTargets::kDepthFormat;

    for (auto& [id, owned] : frame.graph.nodes()) {
        if (!owned->renderVisible() || owned->type() != SceneNode::Type::Mesh) continue;
        auto* mesh = static_cast<MeshNode*>(owned.get());
        const bromesh::MeshData& data = mesh->currentMesh();
        if (data.empty()) continue;
        // Mirrors would only reflect themselves.
        if (mesh->metallic() > 0.8f && mesh->roughness() < 0.2f) continue;

        const uint32_t slot = &data == &mesh->mesh() ? 0u : static_cast<uint32_t>(mesh->selectedLod()) + 1u;
        const GpuMesh* gm = gpu.resources.mesh(mesh->id(), slot, mesh->geometryGeneration(), data);
        if (!gm) continue;

        MeshDraw draw;
        draw.kind = MeshKind::Static;
        draw.nodeId = mesh->id();
        draw.vertices = gm->vertices.buffer;
        draw.indices = gm->indices.buffer;
        draw.indexCount = gm->indexCount;
        std::memcpy(draw.push.model, mesh->worldMatrix().data, sizeof(draw.push.model));
        std::memcpy(draw.push.baseColor, mesh->color(), sizeof(draw.push.baseColor));
        std::memcpy(draw.push.emissive, mesh->emissiveColor(), 3 * sizeof(float));
        draw.push.emissive[3] = mesh->emissive();
        draw.push.pbrParams[0] = mesh->metallic();
        draw.push.pbrParams[1] = mesh->roughness();
        draw.push.pbrParams[2] = mesh->alphaCutoff();

        uint32_t flags = mesh->effectiveUnlit() ? mesh_flags::kUnlit : 0u;
        if (const SceneVkImage* albedo = gpu.resources.texture(mesh->id(), TextureSlot::BaseColor,
                                                               mesh->baseColorTexture())) {
            const SceneDefaults& d = gpu.defaults;
            draw.materialSet = gpu.device.frameSet(d.materialLayout);
            SceneVkDescriptorWriter writer;
            writer.writeImage(0, albedo->view, albedo->sampler);
            writer.writeImage(1, d.flatNormal.view, d.sampler);
            writer.writeImage(2, d.white.view, d.sampler);
            writer.writeImage(3, d.black.view, d.sampler);
            writer.updateSet(gpu.device.device(), draw.materialSet);
            flags |= mesh_flags::kAlbedoMap;
        }
        draw.push.pbrParams[3] = static_cast<float>(flags);
        gpu.meshes.record(cmd, target, cameraSet, lightingSet, draw);
    }

    gpu.device.cmdEndRendering(cmd);
}

void PassReflectionProbe::buildMips(SceneFrame& frame, Probe& probe) {
    SceneVkAllocator& alloc = frame.gpu.allocator;
    VkCommandBuffer cmd = frame.cmd;
    alloc.transitionImageLayout(cmd, probe.cube.image, kCubeFormat, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 1, 0, VK_IMAGE_ASPECT_COLOR_BIT, 6, 0);
    for (uint32_t i = 1; i < probe.mipLevels; ++i) {
        alloc.transitionImageLayout(cmd, probe.cube.image, kCubeFormat, VK_IMAGE_LAYOUT_UNDEFINED,
                                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, i, VK_IMAGE_ASPECT_COLOR_BIT, 6, 0);
        const int32_t src = std::max(1, probe.resolution >> (i - 1));
        const int32_t dst = std::max(1, probe.resolution >> i);
        VkImageBlit blit{};
        blit.srcOffsets[1] = {src, src, 1};
        blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, i - 1, 0, 6};
        blit.dstOffsets[1] = {dst, dst, 1};
        blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, i, 0, 6};
        vkCmdBlitImage(cmd, probe.cube.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, probe.cube.image,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
        alloc.transitionImageLayout(cmd, probe.cube.image, kCubeFormat, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 1, i, VK_IMAGE_ASPECT_COLOR_BIT, 6, 0);
    }
    alloc.transitionImageLayout(cmd, probe.cube.image, kCubeFormat, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, probe.mipLevels, 0,
                                VK_IMAGE_ASPECT_COLOR_BIT, 6, 0);
    probe.cube.currentLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
}

}  // namespace bro::scene::vk
