#include "scene/vulkan/draw/pass_mesh.h"

#include "scene/nodes/mesh_node.h"
#include "scene/scene_graph.h"
#include "scene/vulkan/frame/scene_defaults.h"
#include "scene/vulkan/frame/scene_frame.h"
#include "scene/vulkan/frame/scene_gpu_resources.h"
#include "scene/vulkan/frame/scene_gpu_timer.h"
#include "scene/vulkan/draw/scene_mesh_drawer.h"
#include "scene/vulkan/frame/scene_targets.h"
#include "scene/vulkan/shadow/scene_shadow_pages.h"
#include "scene/vulkan/frame/scene_vk_descriptors.h"
#include "scene/vulkan/frame/scene_vk_device.h"

#include <algorithm>
#include <cstring>
#include <iterator>
#include <vector>

namespace bro::scene::vk {

void PassOpaque::declare(const SceneFrame& frame, PassIO& io) const {
    io.hdr({.mark = true, .indirect = frame.ssao});
    // (Its draws light through the shadow pages' pool and overlays.)
    io.sample(frame.gpu.shadows.pool);
    io.sample(frame.gpu.shadows.scratch);
    // The plants' planned draws read the plan's chunks and commands.
    if (frame.computeApart()) io.readsCompute();
}

void PassOpaque::record(SceneFrame& frame) {
    // Each draw's own time when the passes are timed (SceneGpuTimer), by kind.
    static constexpr const char* kKind[kMeshKindCount] = {
        "opaque:static", "opaque:instanced", "opaque:skinned", "opaque:scatter", "opaque:tube",
        "opaque:vegetation", "opaque:instancedVegetation", "opaque:grass", "opaque:plants", "opaque:land"};
    const SceneMeshDrawer::ChunkCull cull{frame.renderer.cullingFrustum(), frame.view.eye, true};
    for (uint32_t i : frame.lists.opaque) {
        const MeshDraw& draw = frame.lists.meshes[i];
        frame.gpu.meshes.record(frame.cmd, frame.hdrTarget, frame.cameraSet, frame.lightingSet, draw, &cull);
        if (frame.timer)
            frame.timer->mark(frame.cmd, draw.timerLabel ? draw.timerLabel : kKind[static_cast<int>(draw.kind)]);
    }
}

bool PassTranslucent::active(const SceneFrame& frame) const {
    return !frame.lists.translucent.empty();
}

void PassTranslucent::declare(const SceneFrame&, PassIO& io) const {
    io.hdr();
}

void PassTranslucent::record(SceneFrame& frame) {
    // (Drawn whole, not cut to chunks: a blend's order is its triangles'.)
    for (uint32_t i : frame.lists.translucent) {
        frame.gpu.meshes.record(frame.cmd, frame.hdrTarget, frame.cameraSet, frame.lightingSet,
                                frame.lists.meshes[i]);
    }
}

bool PassOverlay::active(const SceneFrame& frame) const {
    return frame.ldrResult &&
           (!frame.lists.overlay.empty() || frame.graph.hasGizmoProvider() || !gizmoIds_.empty());
}

void PassOverlay::declare(const SceneFrame& frame, PassIO& io) const {
    io.colorTarget(*frame.ldrResult);
    io.depthTarget(frame.gpu.targets.depth);
}

void PassOverlay::record(SceneFrame& frame) {
    SceneGpu& gpu = frame.gpu;
    VkCommandBuffer cmd = frame.cmd;
    SceneVkImage& color = *frame.ldrResult;
    SceneVkImage& depth = gpu.targets.depth;

    std::vector<MeshNode*> gizmos;
    if (frame.graph.hasGizmoProvider()) gizmos = frame.graph.gizmoMeshes();
    std::vector<uint32_t> ids;
    for (MeshNode* g : gizmos) {
        if (g) ids.push_back(g->id());
    }
    std::sort(ids.begin(), ids.end());
    std::vector<uint32_t> gone;
    std::set_difference(gizmoIds_.begin(), gizmoIds_.end(), ids.begin(), ids.end(), std::back_inserter(gone));
    if (!gone.empty()) gpu.resources.releaseNodes(gone);
    gizmoIds_ = std::move(ids);
    if (frame.lists.overlay.empty() && gizmoIds_.empty()) return;

    // The frame's camera without fog: the overlay keeps its authored colours.
    SceneCameraUniforms cam = frame.view.uniforms(&frame.renderer);
    std::fill(std::begin(cam.fogParams), std::end(cam.fogParams), 0.0f);
    std::fill(std::begin(cam.fogColor), std::end(cam.fogColor), 0.0f);
    const VkDescriptorBufferInfo camInfo = gpu.device.frameUniform(&cam, sizeof(cam));
    VkDescriptorSet cameraSet = gpu.device.frameSet(gpu.defaults.cameraLayout);
    SceneVkDescriptorWriter camWriter;
    camWriter.writeBuffer(0, camInfo.buffer, camInfo.range, camInfo.offset);
    camWriter.updateSet(gpu.device.device(), cameraSet);

    VkRenderingAttachmentInfo colorAtt{};
    colorAtt.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    colorAtt.imageView = color.view;
    colorAtt.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    colorAtt.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    colorAtt.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingAttachmentInfo depthAtt{};
    depthAtt.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    depthAtt.imageView = depth.view;
    depthAtt.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    depthAtt.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    depthAtt.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo info{};
    info.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    info.renderArea = {{0, 0}, {color.width, color.height}};
    info.layerCount = 1;
    info.colorAttachmentCount = 1;
    info.pColorAttachments = &colorAtt;
    info.pDepthAttachment = &depthAtt;
    gpu.device.cmdBeginRendering(cmd, &info);
    const VkViewport viewport{0.0f, 0.0f, static_cast<float>(color.width), static_cast<float>(color.height), 0.0f,
                              1.0f};
    const VkRect2D scissor{{0, 0}, {color.width, color.height}};
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    TargetFormat target = TargetFormat::colorOnly(color.format);
    target.depth = depth.format;
    for (uint32_t i : frame.lists.overlay) {
        MeshDraw draw = frame.lists.meshes[i];
        draw.translucent = true;   // blended, depth tested, no depth write
        draw.push.pbrParams[3] =
            static_cast<float>(static_cast<uint32_t>(draw.push.pbrParams[3]) & ~mesh_flags::kReflectance);
        gpu.meshes.record(cmd, target, cameraSet, frame.lightingSet, draw);
    }

    for (MeshNode* gizmo : gizmos) {
        if (!gizmo || gizmo->mesh().empty()) continue;
        const GpuMesh* gm = gpu.resources.mesh(gizmo->id(), 0, gizmo->geometryGeneration(), gizmo->mesh());
        if (!gm) continue;
        MeshDraw draw;
        draw.kind = MeshKind::Static;
        draw.nodeId = gizmo->id();
        draw.vertices = gm->vertices.buffer;
        draw.indices = gm->indices.buffer;
        draw.indexCount = gm->indexCount;
        draw.translucent = true;
        draw.noDepthTest = true;   // handles stay on top
        std::memcpy(draw.push.model, frame.view.relative(gizmo->worldMatrix()).data, sizeof(draw.push.model));
        std::memcpy(draw.push.baseColor, gizmo->color(), sizeof(draw.push.baseColor));
        std::memcpy(draw.push.emissive, gizmo->emissiveColor(), 3 * sizeof(float));
        draw.push.emissive[3] = gizmo->emissive();
        draw.push.pbrParams[0] = 0.0f;   // metallic
        draw.push.pbrParams[1] = 1.0f;   // roughness
        draw.push.pbrParams[3] = static_cast<float>(mesh_flags::kUnlit);
        gpu.meshes.record(cmd, target, cameraSet, frame.lightingSet, draw);
        frame.drewContent = true;
    }
    gpu.device.cmdEndRendering(cmd);
}

}  // namespace bro::scene::vk
