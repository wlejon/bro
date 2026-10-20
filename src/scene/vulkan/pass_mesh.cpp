#include "scene/vulkan/pass_mesh.h"

#include "scene/vulkan/pass_hiz.h"
#include "scene/mesh_node.h"
#include "scene/scene_graph.h"
#include "scene/vulkan/scene_defaults.h"
#include "scene/vulkan/scene_frame.h"
#include "scene/vulkan/scene_gpu_resources.h"
#include "scene/vulkan/scene_gpu_timer.h"
#include "scene/vulkan/scene_fullscreen.h"
#include "scene/vulkan/scene_mesh_drawer.h"
#include "scene/vulkan/scene_targets.h"
#include "scene/vulkan/scene_vk_depth.h"
#include "scene/vulkan/scene_vk_descriptors.h"
#include "scene/vulkan/scene_vk_device.h"
#include "scene/vulkan/scene_vk_shader_compiler.h"
#include "scene/vulkan/shaders/scene_forest_layout.h"
#include "scene/vulkan/shaders/scene_forest_plan_layout.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iterator>
#include <vector>

namespace bro::scene::vk {

namespace {

// A forest's clumps or crowns: the sprites placed by their leaves, so the
// ones drawn behind the depth drawn so far are dropped (forestFarRejected).
bool farTested(const MeshDraw& d) {
    if (d.kind != MeshKind::ForestSprite || d.shadowOnly) return false;
    if (static_cast<uint32_t>(d.push.pbrParams[3]) & mesh_flags::kForestBloom) return false;
    const int mode = static_cast<int>(d.push.extra[2]);
    return mode == FOREST_SPRITE_CLUMPS || mode == FOREST_SPRITE_CROWNS;
}

// A crown is an unindexed draw of its entries' cards; clumps an instance an entry.
bool crowns(const MeshDraw& d) {
    return static_cast<int>(d.push.extra[2]) == FOREST_SPRITE_CROWNS;
}

// Where each batch's share of a draw's entries (nearest first) ends, as the
// GPU planner splits its draws (scene_forest_plan_layout.h): the nearest few
// cover most of what the draw covers, and each boundary costs a pass over
// the depth. On the wilds routes (GPU ms, REPEAT=4) 5% / 25% beat 10% / 35%,
// 10% / 45%, 20% / 50% and 3% / 15%.
static_assert(FP_BATCHES == OpaqueBatches::kBatches);
constexpr float kBatchEnd[OpaqueBatches::kBatches] = {static_cast<float>(FP_BATCH_END_0),
                                                      static_cast<float>(FP_BATCH_END_1), 1.0f};
}  // namespace

void PassForestBatches::declare(const SceneFrame& frame, PassIO& io) const {
    OpaqueBatches& b = batches_;
    for (auto& list : b.draws) list.clear();
    b.sprites = false;
    // Every other draw goes in the first batch, the ground and the static
    // meshes included: their depth drops the far crowns behind a hill (most
    // of what a far view gains). Drawn after the crowns instead, as they are
    // seen from over the canopy, they kept their early test under the
    // crowns, but the vista, treetop and skim gained a third as much and
    // the orbit lost 0.35 ms either way.
    for (uint32_t i : frame.lists.opaque) {
        const MeshDraw& draw = frame.lists.meshes[i];
        if (!farTested(draw)) {
            b.draws[0].push_back({i});
            continue;
        }
        b.sprites = true;
        // Planned on the GPU, its entries' count is the GPU's: the planner
        // writes each batch's command, split as kBatchEnd splits it here.
        if (draw.indirect != VK_NULL_HANDLE) {
            for (int k = 0; k < OpaqueBatches::kBatches; ++k) {
                OpaqueBatches::Draw d{i};
                if (draw.indirectBatches == OpaqueBatches::kBatches) d.batch = k;
                else if (k > 0) break;
                b.draws[k].push_back(d);
            }
            continue;
        }
        const uint32_t n = crowns(draw) ? draw.indexCount / FOREST_CARD_VERTICES : draw.instanceCount;
        uint32_t at = 0;
        for (int k = 0; k < OpaqueBatches::kBatches; ++k) {
            const uint32_t end = k + 1 == OpaqueBatches::kBatches
                                     ? n : static_cast<uint32_t>(std::floor(kBatchEnd[k] * static_cast<float>(n)));
            if (end <= at) continue;
            b.draws[k].push_back({i, true, at, end - at});
            at = end;
        }
    }
    if (b.sprites) {
        io.transferDst(frame.gpu.targets.farDepth);
        if (hiZEnabled()) io.transferDst(frame.gpu.targets.hiZ);
    }
}

void PassForestBatches::record(SceneFrame& frame) {
    if (!batches_.sprites) return;
    // Nothing drawn yet: the first batch drops nothing, by pixel or by card.
    VkClearColorValue far{};
    far.float32[0] = depth::clearFar();
    const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(frame.cmd, frame.gpu.targets.farDepth.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &far, 1,
                         &range);
    SceneVkImage& hiZ = frame.gpu.targets.hiZ;
    if (hiZEnabled() && hiZ.isValid()) {
        const VkImageSubresourceRange all{VK_IMAGE_ASPECT_COLOR_BIT, 0, hiZ.mipLevels, 0, 1};
        vkCmdClearColorImage(frame.cmd, hiZ.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &far, 1, &all);
    }
}

bool PassOpaque::active(const SceneFrame&) const {
    return batch_ == 0 || !batches_.draws[batch_].empty();
}

void PassOpaque::declare(const SceneFrame& frame, PassIO& io) const {
    io.hdr({.indirect = frame.ssao});
    // (Its draws light through the shadow atlas: readable even where the
    // planned casters drew last, PassShadow::Part::Planned.)
    io.sample(frame.gpu.targets.shadowAtlas);
    if (batches_.sprites) {
        io.sample(frame.gpu.targets.farDepth);
        // (Its sprites' cards cull against the pyramid: scene_occlusion.glsl.)
        io.sample(frame.gpu.targets.hiZ);
    }
    // A forest's planned draws read the plan's entries and commands.
    if (frame.computeApart()) io.readsCompute();
}

void PassOpaque::record(SceneFrame& frame) {
    // Each draw's own time when the passes are timed (SceneGpuTimer), by kind.
    static constexpr const char* kKind[kMeshKindCount] = {
        "opaque:static", "opaque:instanced", "opaque:skinned", "opaque:scatter", "opaque:tube",
        "opaque:vegetation", "opaque:instancedVegetation", "opaque:grass", "opaque:forestLeaf",
        "opaque:forestTube", "opaque:forestSprite"};
    for (const OpaqueBatches::Draw& d : batches_.draws[batch_]) {
        MeshDraw draw = frame.lists.meshes[d.mesh];
        if (d.batch >= 0) {
            draw.indirectOffset += static_cast<VkDeviceSize>(d.batch + 1) * draw.indirectBatchStride;
            draw.indirectBatches = 0;
        } else if (d.split && crowns(draw)) {
            draw.firstVertex += d.first * FOREST_CARD_VERTICES;
            draw.indexCount = d.count * FOREST_CARD_VERTICES;
        } else if (d.split) {
            draw.firstInstance += d.first;
            draw.instanceCount = d.count;
        }
        const char* label = draw.timerLabel ? draw.timerLabel : kKind[static_cast<int>(draw.kind)];
        if (frame.timer) frame.timer->statBegin(frame.cmd, label);
        const SceneMeshDrawer::ChunkCull cull{frame.renderer.cullingFrustum(), frame.view.eye, true};
        frame.gpu.meshes.record(frame.cmd, frame.hdrTarget, frame.cameraSet, frame.lightingSet, draw, &cull);
        if (frame.timer) {
            frame.timer->statEnd(frame.cmd);
            frame.timer->mark(frame.cmd, label);
        }
    }
}

bool PassFarDepth::setup(SceneGpu& gpu) {
    VkDevice dev = gpu.device.device();
    VkSamplerCreateInfo sampler{};
    sampler.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampler.magFilter = sampler.minFilter = VK_FILTER_NEAREST;
    sampler.addressModeU = sampler.addressModeV = sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (vkCreateSampler(dev, &sampler, nullptr, &sampler_) != VK_SUCCESS) return false;
    setLayout_ = fullscreen::samplerSetLayout(dev, 1);
    if (!setLayout_) return false;
    layout_ = fullscreen::layout(dev, setLayout_, 0);
    if (!layout_) return false;
    VkShaderModule vs = SceneVkShaderCompiler::createBuiltinModule(dev, BuiltinSceneShader::PostFxVert);
    VkShaderModule fs = SceneVkShaderCompiler::createBuiltinModule(dev, BuiltinSceneShader::FarDepthFrag);
    VkShaderModule fsMs = SceneVkShaderCompiler::createBuiltinModule(dev, BuiltinSceneShader::FarDepthMsFrag);
    pipelines_[0] = fullscreen::pipeline(dev, layout_, vs, fs, SceneTargets::kFarDepthFormat);
    pipelines_[1] = fullscreen::pipeline(dev, layout_, vs, fsMs, SceneTargets::kFarDepthFormat);
    for (VkShaderModule m : {vs, fs, fsMs}) SceneVkShaderModule::destroy(dev, m);
    return pipelines_[0] && pipelines_[1];
}

void PassFarDepth::cleanup(SceneGpu& gpu) {
    VkDevice dev = gpu.device.device();
    for (VkPipeline& p : pipelines_) {
        if (p) vkDestroyPipeline(dev, p, nullptr);
        p = VK_NULL_HANDLE;
    }
    if (layout_) vkDestroyPipelineLayout(dev, layout_, nullptr);
    if (setLayout_) vkDestroyDescriptorSetLayout(dev, setLayout_, nullptr);
    if (sampler_) vkDestroySampler(dev, sampler_, nullptr);
    layout_ = VK_NULL_HANDLE;
    setLayout_ = VK_NULL_HANDLE;
    sampler_ = VK_NULL_HANDLE;
}

bool PassFarDepth::active(const SceneFrame&) const {
    return !batches_.draws[batch_].empty();
}

void PassFarDepth::declare(const SceneFrame& frame, PassIO& io) const {
    io.sample(frame.gpu.targets.depthDrawn());
    io.colorTarget(frame.gpu.targets.farDepth);
    io.unresolved();
}

void PassFarDepth::record(SceneFrame& frame) {
    SceneTargets& t = frame.gpu.targets;
    VkDescriptorSet set = frame.gpu.device.frameSet(setLayout_);
    SceneVkDescriptorWriter writer;
    writer.writeImage(0, t.depthDrawn().view, sampler_);
    writer.updateSet(frame.gpu.device.device(), set);
    fullscreen::draw(frame.gpu.device, frame.cmd, t.farDepth, pipelines_[t.msaa() ? 1 : 0], layout_, set);
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
