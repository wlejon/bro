#include "scene/vulkan/shadow/pass_shadow.h"

#include "scene/vulkan/frame/scene_defaults.h"
#include "scene/vulkan/frame/scene_frame.h"
#include "scene/vulkan/frame/scene_gpu_timer.h"
#include "scene/vulkan/frame/scene_targets.h"
#include "scene/vulkan/frame/scene_view.h"
#include "scene/vulkan/draw/scene_vk_custom_shader.h"
#include "scene/vulkan/frame/scene_vk_device.h"
#include "scene/vulkan/frame/scene_vk_shader_compiler.h"
#include "scene/vulkan/shadow/scene_shadow_pages.h"
#include "util/log.h"

#include <array>
#include <cmath>
#include <cstring>

namespace bro::scene::vk {

namespace {

// Slope-scaled polygon offset on the conventional [0,1] shadow depth: the big
// hammer against acne across the slopes a light sees. The receivers add a
// texel-sized normal offset and bias on top (scene_lighting.glsl).
constexpr float kDepthBiasConstant = 4.0f;
constexpr float kDepthBiasSlope = 2.0f;

/// The vertex streams a shadow vertex shader reads: position, normal, uv and
/// colour (the wind bend), plus the instance rows or the joints and weights.
/// A custom vertex chunk sees every mesh attribute but the tangent. A cutout
/// caster reads the uv and colour too, and an instance's record (its cell).
void shadowVertexInput(MeshKind kind, bool custom, bool cutout, std::vector<VkVertexInputBindingDescription>& bindings,
                       std::vector<VkVertexInputAttributeDescription>& attributes) {
    SceneMeshDrawer::vertexInput(kind, bindings, attributes);
    std::vector<VkVertexInputAttributeDescription> used;
    for (const auto& a : attributes) {
        // A custom chunk sees every attribute the colour pass has; the
        // built-in casters read position, wind colour and skin only.
        // Vegetation casters read their sway records at 12-15 as well.
        const bool veg = a.location >= 12 && a.location <= 15;
        // An instanced caster's tint (11) is read only for its atlas cell.
        bool keep = custom && !(kind == MeshKind::Instanced && a.location == 11 && !cutout);
        if (!custom) {
            switch (kind) {
            case MeshKind::Instanced:
                keep = a.location == 0 || (a.location >= 8 && a.location <= 10) ||
                       (cutout && (a.location == 2 || a.location == 3 || a.location == 11));
                break;
            case MeshKind::InstancedVegetation:
                // The normal too: a leaf folds about its card's normal.
                keep = a.location <= 1 || (a.location >= 8 && a.location <= 10) || veg;
                break;
            case MeshKind::Scatter: keep = a.location == 0; break;
            case MeshKind::Grass: keep = a.location <= 1; break;
            default: keep = a.location <= 3 || a.location == 5 || a.location == 6 || veg; break;
            }
        }
        if (keep) used.push_back(a);
    }
    attributes = std::move(used);
}

/// A view's rect of the scratch target, its pages and their border.
VkRect2D viewRect(const ShadowViewPlan& v) {
    return {{v.scratchX, v.scratchY}, {static_cast<uint32_t>(v.texelsW), static_cast<uint32_t>(v.texelsH)}};
}

}  // namespace

bool PassShadow::setup(SceneGpu& gpu) {
    device_ = &gpu.device;
    VkDevice dev = gpu.device.device();
    const SceneDefaults& d = gpu.defaults;

    // The mesh pipelines' set numbering, so a custom vertex chunk written for
    // the colour pass (camera at 0, bones at 3, its own set at 4) compiles
    // here; a cutout caster samples its material's base colour at 2.
    const std::array<VkDescriptorSetLayout, 5> sets = {d.cameraLayout, d.emptyLayout, d.materialLayout, d.boneLayout,
                                                       d.customLayout};
    VkPushConstantRange push{VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(Push)};
    VkPipelineLayoutCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    info.setLayoutCount = static_cast<uint32_t>(sets.size());
    info.pSetLayouts = sets.data();
    info.pushConstantRangeCount = 1;
    info.pPushConstantRanges = &push;
    if (vkCreatePipelineLayout(dev, &info, nullptr, &layout_) != VK_SUCCESS) return false;

    vs_[static_cast<int>(MeshKind::Static)] =
        SceneVkShaderCompiler::createBuiltinModule(dev, BuiltinSceneShader::ShadowVert);
    vs_[static_cast<int>(MeshKind::Instanced)] =
        SceneVkShaderCompiler::createBuiltinModule(dev, BuiltinSceneShader::ShadowInstancedVert);
    vs_[static_cast<int>(MeshKind::Skinned)] =
        SceneVkShaderCompiler::createBuiltinModule(dev, BuiltinSceneShader::ShadowSkinnedVert);
    vs_[static_cast<int>(MeshKind::Tube)] =
        SceneVkShaderCompiler::createBuiltinModule(dev, BuiltinSceneShader::ShadowTubeVert);
    vs_[static_cast<int>(MeshKind::Scatter)] =
        SceneVkShaderCompiler::createBuiltinModule(dev, BuiltinSceneShader::ShadowScatterVert);
    vs_[static_cast<int>(MeshKind::Vegetation)] =
        SceneVkShaderCompiler::createBuiltinModule(dev, BuiltinSceneShader::ShadowVegVert);
    vs_[static_cast<int>(MeshKind::InstancedVegetation)] =
        SceneVkShaderCompiler::createBuiltinModule(dev, BuiltinSceneShader::ShadowInstancedVegVert);
    vs_[static_cast<int>(MeshKind::Grass)] =
        SceneVkShaderCompiler::createBuiltinModule(dev, BuiltinSceneShader::ShadowGrassVert);
    vs_[static_cast<int>(MeshKind::Plants)] =
        SceneVkShaderCompiler::createBuiltinModule(dev, BuiltinSceneShader::ShadowPlantsVert);
    fs_ = SceneVkShaderCompiler::createBuiltinModule(dev, BuiltinSceneShader::ShadowFrag);
    vsCutout_[0] = SceneVkShaderCompiler::createBuiltinModule(dev, BuiltinSceneShader::ShadowCutoutVert);
    vsCutout_[1] = SceneVkShaderCompiler::createBuiltinModule(dev, BuiltinSceneShader::ShadowInstancedCutoutVert);
    fsCutout_ = SceneVkShaderCompiler::createBuiltinModule(dev, BuiltinSceneShader::ShadowCutoutFrag);
    for (VkShaderModule m : vs_) {
        if (!m) return false;
    }
    return fs_ != VK_NULL_HANDLE && vsCutout_[0] != VK_NULL_HANDLE && vsCutout_[1] != VK_NULL_HANDLE && fsCutout_ != VK_NULL_HANDLE;
}

void PassShadow::cleanup(SceneGpu& gpu) {
    VkDevice dev = gpu.device.device();
    std::vector<VkPipeline> pipelines;
    for (auto& perKind : builtin_) pipelines.insert(pipelines.end(), std::begin(perKind), std::end(perKind));
    for (auto& perKind : cutout_) pipelines.insert(pipelines.end(), std::begin(perKind), std::end(perKind));
    for (auto& [key, p] : custom_) pipelines.push_back(p);
    gpu.device.defer([dev, pipelines] {
        for (VkPipeline p : pipelines) {
            if (p != VK_NULL_HANDLE) vkDestroyPipeline(dev, p, nullptr);
        }
    });
    custom_.clear();
    for (auto& perKind : builtin_) perKind[0] = perKind[1] = VK_NULL_HANDLE;
    for (auto& perKind : cutout_) perKind[0] = perKind[1] = VK_NULL_HANDLE;
    for (VkShaderModule& m : vsCutout_) {
        SceneVkShaderModule::destroy(dev, m);
        m = VK_NULL_HANDLE;
    }
    SceneVkShaderModule::destroy(dev, fsCutout_);
    fsCutout_ = VK_NULL_HANDLE;
    for (VkShaderModule& m : vs_) {
        SceneVkShaderModule::destroy(dev, m);
        m = VK_NULL_HANDLE;
    }
    SceneVkShaderModule::destroy(dev, fs_);
    fs_ = VK_NULL_HANDLE;
    if (layout_ != VK_NULL_HANDLE) vkDestroyPipelineLayout(dev, layout_, nullptr);
    layout_ = VK_NULL_HANDLE;
}

VkPipeline PassShadow::buildPipeline(VkShaderModule vs, MeshKind kind, bool custom, bool twoSided, bool cutout) {
    std::vector<VkVertexInputBindingDescription> bindings;
    std::vector<VkVertexInputAttributeDescription> attributes;
    shadowVertexInput(kind, custom, cutout, bindings, attributes);

    TargetFormat target;
    target.depth = SceneTargets::kDepthFormat;

    // Light-facing faces are culled, so the depth stored is the casters' far
    // side and closed meshes do not shadow themselves; two-sided surfaces
    // (leaves, cards) have no far side and draw both.
    SceneVkPipelineBuilder b;
    b.setShaderStages(vs, cutout ? fsCutout_ : fs_)
     .setVertexInput(bindings, attributes)
     .setInputTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
     .setPolygonMode(VK_POLYGON_MODE_FILL)
     .setCullMode(twoSided ? VK_CULL_MODE_NONE : VK_CULL_MODE_FRONT_BIT, VK_FRONT_FACE_COUNTER_CLOCKWISE)
     .setTarget(target)
     .disableBlending(0)
     .setDepthBias(true)
     // Shadow maps are outside the camera depth policy: always conventional.
     .enableDepthTest(true, VK_COMPARE_OP_LESS_OR_EQUAL)
     .setDynamicStates({VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_DEPTH_BIAS});
    return b.build(device_->device(), layout_);
}

VkPipeline PassShadow::builtinPipeline(MeshKind kind, bool twoSided) {
    VkPipeline& p = builtin_[static_cast<int>(kind)][twoSided ? 1 : 0];
    if (p == VK_NULL_HANDLE) p = buildPipeline(vs_[static_cast<int>(kind)], kind, false, twoSided, false);
    return p;
}

VkPipeline PassShadow::cutoutPipeline(MeshKind kind, bool twoSided) {
    const int instanced = kind == MeshKind::Instanced ? 1 : 0;
    VkPipeline& p = cutout_[instanced][twoSided ? 1 : 0];
    if (p == VK_NULL_HANDLE) p = buildPipeline(vsCutout_[instanced], kind, false, twoSided, true);
    return p;
}

VkPipeline PassShadow::customPipeline(const CustomShaderState& cs, MeshKind kind, bool twoSided, bool cutout) {
    using Caster = SceneVkCustomShader::ShadowCaster;
    const Caster caster = kind == MeshKind::Skinned     ? Caster::Skinned
                          : kind == MeshKind::Instanced ? Caster::Instanced
                                                        : Caster::Static;
    std::string key = cs.key + (caster == Caster::Skinned ? "_skin" : caster == Caster::Instanced ? "_inst" : "_static");
    if (twoSided) key += "_2s";
    if (cutout) key += "_cut";
    auto it = custom_.find(key);
    if (it != custom_.end()) return it->second;

    VkPipeline pipeline = VK_NULL_HANDLE;
    VkShaderModule vs = VK_NULL_HANDLE;
    std::string err;
    CustomShaderInterface iface;
    if (iface.parse(cs.vertexChunk, cs.fragmentChunk, err) &&
        SceneVkCustomShader::compileCustomShadowShaderModule(device_->device(), caster, cutout, iface, vs, err)) {
        pipeline = buildPipeline(vs, kind, true, twoSided, cutout);
        SceneVkShaderModule::destroy(device_->device(), vs);
    }
    if (pipeline == VK_NULL_HANDLE) {
        LOG_WARN("PassShadow: a custom vertex chunk does not build as a shadow caster; the mesh casts its "
                 "undisplaced silhouette: %s", err.c_str());
    }
    custom_.emplace(std::move(key), pipeline);
    return pipeline;
}

bool PassShadow::active(const SceneFrame& frame) const {
    // Pages that failed to allocate draw nothing.
    const ShadowPlan& plan = frame.renderer.shadowPlan();
    if (!frame.gpu.shadows.ready() || plan.viewCount == 0) return false;
    if (part_ == Part::All) return true;
    // The unplanned part clears the overlays and draws their unplanned
    // casters; the planned one theirs and every redrawn page.
    bool overlays = false;
    for (int v = 0; v < plan.viewCount; ++v) overlays = overlays || plan.views[v].dynamic;
    if (part_ == Part::Unplanned) return overlays;
    if (plan.staticPages > 0) return true;
    for (const MeshDraw& draw : frame.lists.meshes)
        if (draw.castsShadow && draw.indirect != VK_NULL_HANDLE && overlays) return true;
    return false;
}

void PassShadow::declare(const SceneFrame& frame, PassIO& io) const {
    io.depthTarget(frame.gpu.shadows.scratch);
    if (part_ == Part::Planned) io.readsCompute();
}

void PassShadow::drawCaster(SceneFrame& frame, const bromath::Mat4& lightViewProj, const MeshDraw& draw,
                            const bromath::Frustum* tileFrustum) {
    VkCommandBuffer cmd = frame.cmd;
    VkPipeline pipeline = VK_NULL_HANDLE;
    const uint32_t flags = static_cast<uint32_t>(draw.push.pbrParams[3]);
    // A textured static or instanced mesh with an alpha cutoff casts its cut.
    const bool cutout = (draw.kind == MeshKind::Static || draw.kind == MeshKind::Instanced) &&
                        draw.push.pbrParams[2] > 0.0f && (flags & mesh_flags::kAlbedoMap) != 0u &&
                        draw.materialSet != VK_NULL_HANDLE;
    const bool custom = draw.custom && draw.customSet && !draw.custom->vertexChunk.empty();
    if (custom) pipeline = customPipeline(*draw.custom, draw.kind, draw.twoSided, cutout);
    const bool customBound = pipeline != VK_NULL_HANDLE;
    // (A custom chunk that does not build casts the undisplaced silhouette,
    // cut all the same.)
    if (!pipeline && cutout) pipeline = cutoutPipeline(draw.kind, draw.twoSided);
    if (!pipeline) pipeline = builtinPipeline(draw.kind, draw.twoSided);
    if (!pipeline) return;

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    if (draw.kind != MeshKind::Instanced || customBound)
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0, 1, &frame.cameraSet, 0, nullptr);
    if (cutout)
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 2, 1, &draw.materialSet, 0, nullptr);
    if (draw.vertexSet)
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 3, 1, &draw.vertexSet, 0, nullptr);
    if (customBound || (draw.kind == MeshKind::Grass && draw.customSet))
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 4, 1, &draw.customSet, 0, nullptr);

    Push push{};
    std::memcpy(push.lightViewProj, lightViewProj.data, sizeof(push.lightViewProj));
    // Column-major model -> its first three rows.
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 4; ++c) push.modelRows[r * 4 + c] = draw.push.model[c * 4 + r];
    }
    push.params[0] = draw.push.extra[2];
    if (cutout) {
        // scene_shadow_cutout.glsl's: the cutoff, the base colour's alpha
        // (negated: the vertex colour's alpha counts too), the atlas grid.
        const bool vertexAlpha = (flags & mesh_flags::vertexColor(3)) != 0u;
        push.params[1] = draw.push.pbrParams[2];
        push.params[2] = vertexAlpha ? -draw.push.baseColor[3] : draw.push.baseColor[3];
        push.params[3] = draw.push.extra[3];
    }
    vkCmdPushConstants(cmd, layout_, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(push), &push);

    // A chunked caster draws the chunks inside the view.
    const SceneMeshDrawer::ChunkCull cull{tileFrustum, frame.view.eye};
    if (draw.shadowIndexCount != 0 && draw.shadowIndexCount < draw.indexCount) {
        MeshDraw cut = draw;
        cut.indexCount = draw.shadowIndexCount;
        SceneMeshDrawer::bindGeometryAndDraw(cmd, cut, &cull);
        return;
    }
    SceneMeshDrawer::bindGeometryAndDraw(cmd, draw, &cull);
}

void PassShadow::drawView(SceneFrame& frame, int index, bool planned, bool unplanned) {
    VkCommandBuffer cmd = frame.cmd;
    const ShadowPlan& plan = frame.renderer.shadowPlan();
    const ShadowViewPlan& view = plan.views[index];
    const VkRect2D rect = viewRect(view);
    const VkViewport viewport{static_cast<float>(rect.offset.x), static_cast<float>(rect.offset.y),
                              static_cast<float>(rect.extent.width), static_cast<float>(rect.extent.height), 0.0f, 1.0f};
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &rect);
    // The draws are relative to the frame's eye, the view to the plan's
    // (the same eye unless a caller re-planned in between).
    const bromath::Mat4 lightViewProj = toVulkanClip(rebased(view.relViewProj, frame.view.eye - plan.origin));
    const bool culling = frame.renderer.cullingFrustum() != nullptr;
    // The timed passes' names: the overlays' casters by kind and the pages'
    // by kind, a run of draws under one name marked once as it ends.
    static constexpr const char* kOverlay[kMeshKindCount] = {
        "shadow:moving:static", "shadow:moving:instanced", "shadow:moving:skinned", "shadow:moving:scatter",
        "shadow:moving:tube",   "shadow:moving:veg",       "shadow:moving:instancedVeg", "shadow:moving:grass",
        "shadow:moving:plants"};
    static constexpr const char* kPages[kMeshKindCount] = {
        "shadow:pages:static", "shadow:pages:instanced", "shadow:pages:skinned", "shadow:pages:scatter",
        "shadow:pages:tube",   "shadow:pages:veg",       "shadow:pages:instancedVeg", "shadow:pages:grass",
        "shadow:pages:plants"};
    const char* run = nullptr;
    for (const MeshDraw& draw : frame.lists.meshes) {
        if (!draw.castsShadow) continue;
        const bool isPlanned = draw.indirect != VK_NULL_HANDLE;
        if (isPlanned ? !planned : !unplanned) continue;
        // A plants node's planned casters are its view's alone; any other
        // goes where its motion does.
        if (isPlanned ? draw.shadowView != index : !shadowCastsInto(view, draw.shadowMotion)) continue;
        if (culling && draw.hasBounds && !isPlanned && !bromath::fintersects(view.frustum, draw.bounds)) {
            frame.stats.shadowCulled++;
            continue;
        }
        frame.stats.shadowDrawn++;
        const char* label = (view.dynamic ? kOverlay : kPages)[static_cast<int>(draw.kind)];
        if (frame.timer && run && run != label) frame.timer->mark(cmd, run);
        run = label;
        drawCaster(frame, lightViewProj, draw, culling ? &view.frustum : nullptr);
    }
    if (frame.timer && run) frame.timer->mark(cmd, run);
}

void PassShadow::record(SceneFrame& frame) {
    VkCommandBuffer cmd = frame.cmd;
    const ShadowPlan& plan = frame.renderer.shadowPlan();
    const SceneVkImage& scratch = frame.gpu.shadows.scratch;

    VkRenderingAttachmentInfo depth{};
    depth.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    depth.imageView = scratch.view;
    depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    // Each view's rect is cleared as it is first drawn (an overlay's by the
    // unplanned part, the planned part drawing over it).
    depth.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo info{};
    info.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    info.renderArea = {{0, 0}, {scratch.width, scratch.height}};
    info.layerCount = 1;
    info.pDepthAttachment = &depth;
    frame.gpu.device.cmdBeginRendering(cmd, &info);
    vkCmdSetDepthBias(cmd, kDepthBiasConstant, 0.0f, kDepthBiasSlope);

    const auto clearView = [&](int v) {
        VkClearAttachment clear{};
        clear.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        clear.clearValue.depthStencil = {1.0f, 0};
        const VkClearRect clearRect{viewRect(plan.views[v]), 0, 1};
        vkCmdClearAttachments(cmd, 1, &clear, 1, &clearRect);
    };
    // The overlays: what moves, alone. Their unplanned casters beside the
    // compute, the planned ones after the join.
    const bool unplanned = part_ != Part::Planned, planned = part_ != Part::Unplanned;
    for (int v = 0; v < plan.viewCount; ++v) {
        if (!plan.views[v].dynamic) continue;
        if (unplanned) clearView(v);
        drawView(frame, v, planned, unplanned);
    }
    if (frame.timer) frame.timer->mark(cmd, "shadow:moving");

    // The pages redrawn into the cache, all their casters at once (after
    // the join when there is one): the world service's jobs, each kind's
    // views timed as its span with the pages they hold.
    if (planned && plan.staticPages > 0) {
        const auto drawKind = [&](int kind, bool plants, uint32_t pages) {
            if (pages == 0) return;
            const bool stamped =
                frame.timer && kind >= 0 && frame.timer->beginSpan(cmd, static_cast<uint32_t>(kind), false);
            for (int v = 0; v < plan.viewCount; ++v) {
                if (plan.views[v].dynamic || plan.views[v].plants != plants) continue;
                clearView(v);
                drawView(frame, v, true, true);
            }
            if (stamped) frame.timer->endSpan(cmd, static_cast<uint32_t>(kind), false, pages);
        };
        drawKind(frame.gpu.shadows.stillKind(), false, plan.staticPages - plan.plantPages);
        drawKind(frame.gpu.shadows.kind(), true, plan.plantPages);
        if (frame.timer) frame.timer->mark(cmd, "shadow:pages");
    }

    frame.gpu.device.cmdEndRendering(cmd);
}

}  // namespace bro::scene::vk
