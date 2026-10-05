#include "scene/vulkan/pass_reflection_probe.h"

#include "scene/depth_policy.h"
#include "scene/reflection_probe_node.h"
#include "scene/scene_graph.h"
#include "scene/vulkan/scene_defaults.h"
#include "scene/vulkan/scene_environment.h"
#include "scene/vulkan/scene_frame.h"
#include "scene/vulkan/scene_lighting.h"
#include "scene/vulkan/scene_mesh_drawer.h"
#include "scene/vulkan/scene_targets.h"
#include "scene/vulkan/scene_vk_depth.h"
#include "scene/vulkan/scene_vk_device.h"
#include "util/log.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace bro::scene::vk {

namespace {

constexpr VkFormat kCubeFormat = SceneEnvironment::kCubeFormat;
constexpr float kFaceNear = 0.05f;
constexpr float kMinFaceFar = 1000.0f;

// Cube face order +X, -X, +Y, -Y, +Z, -Z with the cube convention's ups: row 0
// of a face is its -t edge, which an unflipped projection puts at the top.
const bromath::Vec3 kFaceDirs[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
const bromath::Vec3 kFaceUps[6] = {{0, -1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}, {0, -1, 0}, {0, -1, 0}};

// Prefilter mips: down to an 8 px face (128 -> 5), the density of the
// environment's 256² x 6.
uint32_t specularMips(int res) {
    uint32_t mips = 1;
    for (int s = res; s > 8; s >>= 1) ++mips;
    return mips;
}

bool createCube(SceneVkAllocator& allocator, uint32_t size, uint32_t mips, SceneVkImage& out) {
    return allocator.createImage(size, size, kCubeFormat,
                                 VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                     VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                 VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, out, mips, VK_SAMPLE_COUNT_1_BIT,
                                 VK_IMAGE_ASPECT_COLOR_BIT, 6, VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT);
}

}  // namespace

void PassReflectionProbe::release(SceneGpu& gpu, Probe& probe) {
    VkDevice dev = gpu.device.device();
    gpu.device.defer([dev, views = probe.faces] {
        for (VkImageView v : views) {
            if (v != VK_NULL_HANDLE) vkDestroyImageView(dev, v, nullptr);
        }
    });
    probe.faces = {};
    gpu.allocator.destroyImage(probe.capture);
    gpu.allocator.destroyImage(probe.specular);
    gpu.allocator.destroyImage(probe.depth);
    probe.captured = false;
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
    if (probe.capture.isValid() && probe.resolution == res) return &probe;
    release(gpu, probe);

    probe.resolution = res;
    const auto size = static_cast<uint32_t>(res);
    const uint32_t mips = static_cast<uint32_t>(std::floor(std::log2(static_cast<float>(res)))) + 1;
    bool ok = createCube(gpu.allocator, size, mips, probe.capture) &&
              createCube(gpu.allocator, size, specularMips(res), probe.specular) &&
              gpu.allocator.createImage(size, size, SceneTargets::kDepthFormat,
                                        VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                                        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, probe.depth, 1, VK_SAMPLE_COUNT_1_BIT,
                                        VK_IMAGE_ASPECT_DEPTH_BIT);
    for (uint32_t f = 0; ok && f < 6; ++f) {
        VkImageViewCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        info.image = probe.capture.image;
        info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        info.format = kCubeFormat;
        info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, f, 1};
        ok = vkCreateImageView(gpu.device.device(), &info, nullptr, &probe.faces[f]) == VK_SUCCESS;
    }
    if (!ok) {
        LOG_ERROR("PassReflectionProbe: Failed creating the %dx%d probe cubes", res, res);
        release(gpu, probe);
        probes_.erase(node.id());
        return nullptr;
    }
    return &probe;
}

void PassReflectionProbe::declare(const SceneFrame& frame, PassIO& io) const {
    // The faces bind the frame's lighting set, whose shadow binding must be readable.
    io.sample(frame.gpu.targets.shadowAtlas);
}

void PassReflectionProbe::record(SceneFrame& frame) {
    for (auto& [id, owned] : frame.graph.nodes()) {
        if (!owned->renderVisible() || owned->type() != SceneNode::Type::ReflectionProbe) continue;
        auto* node = static_cast<ReflectionProbeNode*>(owned.get());
        if (!node->captureRequested()) continue;
        Probe* probe = ensure(frame.gpu, *node);
        if (probe && capture(frame, *node, *probe)) {
            probe->captured = true;
            node->markCaptured();
        } else {
            // A broken capture does not retry (and re-log) every frame.
            node->clearCaptureRequest();
            LOG_ERROR("PassReflectionProbe: capturing probe '%s' failed", node->name().c_str());
        }
    }
    assign(frame);
}

bool PassReflectionProbe::capture(SceneFrame& frame, const ReflectionProbeNode& node, Probe& probe) {
    SceneGpu& gpu = frame.gpu;
    SceneVkAllocator& alloc = gpu.allocator;
    // The probes' specular reads the split-sum LUT, environment or not.
    if (!gpu.environment.ensureBrdfLut(gpu, frame.cmd)) return false;

    SceneVkImage& cube = probe.capture;
    alloc.transitionImageLayout(frame.cmd, cube.image, kCubeFormat, cube.currentLayout,
                                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, cube.mipLevels, 0,
                                VK_IMAGE_ASPECT_COLOR_BIT, 6, 0);
    alloc.transitionImageLayout(frame.cmd, probe.depth.image, SceneTargets::kDepthFormat, probe.depth.currentLayout,
                                VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, 1, 0, VK_IMAGE_ASPECT_DEPTH_BIT);
    probe.depth.currentLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    for (int f = 0; f < 6; ++f) renderFace(frame, node, probe, f);

    // Mip the capture (the prefilter's Krivanek bias reads its levels), then
    // build the roughness chain from it.
    alloc.transitionImageLayout(frame.cmd, cube.image, kCubeFormat, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, cube.mipLevels, 0, VK_IMAGE_ASPECT_COLOR_BIT, 6,
                                0);
    alloc.generateMipmaps(frame.cmd, cube.image, kCubeFormat, probe.resolution, probe.resolution, cube.mipLevels, 0,
                          6);
    cube.currentLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    return gpu.environment.prefilter(gpu, frame.cmd, cube, probe.specular);
}

void PassReflectionProbe::renderFace(SceneFrame& frame, const ReflectionProbeNode& node, Probe& probe, int face) {
    SceneGpu& gpu = frame.gpu;
    VkCommandBuffer cmd = frame.cmd;
    const auto res = static_cast<uint32_t>(probe.resolution);

    const auto& world = node.worldMatrix();
    const bromath::Vec3 eye{world.at(0, 3), world.at(1, 3), world.at(2, 3)};
    const float farZ = std::max(frame.view.farZ, kMinFaceFar);
    // toVulkanClip twice is no flip at all: the face keeps y up in clip space.
    const SceneView view =
        SceneView::make(bromath::mlookAt(eye, eye + kFaceDirs[face], kFaceUps[face]),
                        toVulkanClip(makePerspective(3.14159265358979f * 0.5f, 1.0f, kFaceNear, farZ)), eye,
                        kFaceNear, farZ, true, res, res);
    const SceneCameraUniforms cam = view.uniforms(&frame.renderer);
    const VkDescriptorBufferInfo camInfo = gpu.device.frameUniform(&cam, sizeof(cam));
    VkDescriptorSet cameraSet = gpu.device.frameSet(gpu.defaults.cameraLayout);
    SceneVkDescriptorWriter camWriter;
    camWriter.writeBuffer(0, camInfo.buffer, camInfo.range, camInfo.offset);
    camWriter.updateSet(gpu.device.device(), cameraSet);

    VkRenderingAttachmentInfo color{};
    color.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    color.imageView = probe.faces[face];
    color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color.clearValue.color = {{0.0f, 0.0f, 0.0f, 0.0f}};
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
    if (gpu.environment.skyVisible(frame.renderer, true)) {
        gpu.environment.drawSky(gpu, cmd, target, cameraSet, frame.lightingSet, frame.renderer);
    }

    const bool cull = frame.renderer.frustumCullingEnabled();
    for (const MeshDraw& draw : frame.lists.meshes) {
        if (draw.translucent) continue;   // no sorted translucent pass in a capture
        if (cull && draw.hasBounds && !bromath::fintersects(view.frustum, draw.bounds)) continue;
        MeshDraw faceDraw = draw;
        faceDraw.mirrored = true;
        faceDraw.lightingSet = VK_NULL_HANDLE;
        // The capture is raw radiance: alpha is coverage, not the SSR mask.
        faceDraw.push.pbrParams[3] = static_cast<float>(static_cast<uint32_t>(draw.push.pbrParams[3]) &
                                                        ~mesh_flags::kReflectance);
        gpu.meshes.record(cmd, target, cameraSet, frame.lightingSet, faceDraw);
    }
    gpu.device.cmdEndRendering(cmd);
}

void PassReflectionProbe::assign(SceneFrame& frame) {
    struct Volume {
        const ReflectionProbeNode* node;
        bromath::Mat4 worldToLocal;
        float volume;
        VkDescriptorSet set;
    };
    std::vector<Volume> volumes;
    for (auto& [id, owned] : frame.graph.nodes()) {
        if (!owned->renderVisible() || owned->type() != SceneNode::Type::ReflectionProbe) continue;
        auto* node = static_cast<ReflectionProbeNode*>(owned.get());
        auto it = probes_.find(node->id());
        if (!node->hasData() || it == probes_.end() || !it->second.captured) continue;
        const bromath::Mat4& w = node->worldMatrix();
        float volume = 1.0f;
        for (int c = 0; c < 3; ++c) {
            volume *= std::sqrt(w.at(0, c) * w.at(0, c) + w.at(1, c) * w.at(1, c) + w.at(2, c) * w.at(2, c));
        }
        volumes.push_back({node, bromath::minverse(w), volume, VK_NULL_HANDLE});
    }
    if (volumes.empty()) return;
    // Highest priority first; ties to the smallest (most local) box.
    std::stable_sort(volumes.begin(), volumes.end(), [](const Volume& a, const Volume& b) {
        if (a.node->priority() != b.node->priority()) return a.node->priority() > b.node->priority();
        return a.volume < b.volume;
    });

    for (MeshDraw& draw : frame.lists.meshes) {
        for (Volume& v : volumes) {
            const bromath::Mat4& m = v.worldToLocal;
            const bromath::Vec3& c = draw.center;
            const float lx = m.at(0, 0) * c.x + m.at(0, 1) * c.y + m.at(0, 2) * c.z + m.at(0, 3);
            const float ly = m.at(1, 0) * c.x + m.at(1, 1) * c.y + m.at(1, 2) * c.z + m.at(1, 3);
            const float lz = m.at(2, 0) * c.x + m.at(2, 1) * c.y + m.at(2, 2) * c.z + m.at(2, 3);
            if (std::fabs(lx) > 0.5f || std::fabs(ly) > 0.5f || std::fabs(lz) > 0.5f) continue;
            if (!v.set) {
                const Probe& probe = probes_.at(v.node->id());
                SceneLightingUniforms light = frame.lighting;
                setProbe(light, *v.node, probe.specular.mipLevels);
                v.set = writeLightingSet(frame.gpu, light, probe.specular.view, frame.shadeMap);
            }
            draw.lightingSet = v.set;
            break;
        }
    }
}

}  // namespace bro::scene::vk
