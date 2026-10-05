#include "scene/vulkan/pass_shadow.h"

#include "scene/vulkan/scene_defaults.h"
#include "scene/vulkan/scene_frame.h"
#include "scene/vulkan/scene_targets.h"
#include "scene/vulkan/scene_view.h"
#include "scene/vulkan/scene_vk_custom_shader.h"
#include "scene/vulkan/scene_vk_device.h"
#include "scene/vulkan/scene_vk_shader_compiler.h"
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
/// A custom vertex chunk sees every mesh attribute but the tangent.
void shadowVertexInput(MeshKind kind, bool custom, std::vector<VkVertexInputBindingDescription>& bindings,
                       std::vector<VkVertexInputAttributeDescription>& attributes) {
    SceneMeshDrawer::vertexInput(kind, bindings, attributes);
    std::vector<VkVertexInputAttributeDescription> used;
    for (const auto& a : attributes) {
        // A custom chunk sees every attribute the colour pass has; the
        // built-in casters read position, wind colour and skin only.
        const bool keep = custom ? true
                        : kind == MeshKind::Instanced ? (a.location == 0 || (a.location >= 8 && a.location <= 10))
                                                      : (a.location <= 3 || a.location == 5 || a.location == 6);
        if (keep) used.push_back(a);
    }
    attributes = std::move(used);
}

/// The texel rect of an atlas tile.
VkRect2D tileRect(const float (&rect)[4], uint32_t atlasSize) {
    const auto px = [&](float uv) { return static_cast<int32_t>(std::lround(uv * static_cast<float>(atlasSize))); };
    const int32_t x0 = px(rect[0]), y0 = px(rect[1]);
    const int32_t x1 = px(rect[0] + rect[2]), y1 = px(rect[1] + rect[3]);
    return {{x0, y0}, {static_cast<uint32_t>(x1 - x0), static_cast<uint32_t>(y1 - y0)}};
}

}  // namespace

bool PassShadow::setup(SceneGpu& gpu) {
    device_ = &gpu.device;
    VkDevice dev = gpu.device.device();
    const SceneDefaults& d = gpu.defaults;

    // The mesh pipelines' set numbering, so a custom vertex chunk written for
    // the colour pass (camera at 0, bones at 3, its own set at 4) compiles here.
    const std::array<VkDescriptorSetLayout, 5> sets = {d.cameraLayout, d.emptyLayout, d.emptyLayout, d.boneLayout,
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
    fs_ = SceneVkShaderCompiler::createBuiltinModule(dev, BuiltinSceneShader::ShadowFrag);
    // Scatter leaves cast no shadow, so that kind has no caster stage.
    for (MeshKind k : {MeshKind::Static, MeshKind::Instanced, MeshKind::Skinned, MeshKind::Tube}) {
        if (!vs_[static_cast<int>(k)]) return false;
    }
    return fs_ != VK_NULL_HANDLE;
}

void PassShadow::cleanup(SceneGpu& gpu) {
    VkDevice dev = gpu.device.device();
    std::vector<VkPipeline> pipelines;
    for (auto& perKind : builtin_) pipelines.insert(pipelines.end(), std::begin(perKind), std::end(perKind));
    for (auto& [key, p] : custom_) pipelines.push_back(p);
    gpu.device.defer([dev, pipelines] {
        for (VkPipeline p : pipelines) {
            if (p != VK_NULL_HANDLE) vkDestroyPipeline(dev, p, nullptr);
        }
    });
    custom_.clear();
    for (auto& perKind : builtin_) perKind[0] = perKind[1] = VK_NULL_HANDLE;
    for (VkShaderModule& m : vs_) {
        SceneVkShaderModule::destroy(dev, m);
        m = VK_NULL_HANDLE;
    }
    SceneVkShaderModule::destroy(dev, fs_);
    fs_ = VK_NULL_HANDLE;
    if (layout_ != VK_NULL_HANDLE) vkDestroyPipelineLayout(dev, layout_, nullptr);
    layout_ = VK_NULL_HANDLE;
}

VkPipeline PassShadow::buildPipeline(VkShaderModule vs, MeshKind kind, bool custom, bool twoSided) {
    std::vector<VkVertexInputBindingDescription> bindings;
    std::vector<VkVertexInputAttributeDescription> attributes;
    shadowVertexInput(kind, custom, bindings, attributes);

    TargetFormat target;
    target.depth = SceneTargets::kDepthFormat;

    // Light-facing faces are culled, so the depth stored is the casters' far
    // side and closed meshes do not shadow themselves; two-sided surfaces
    // (leaves, cards) have no far side and draw both.
    SceneVkPipelineBuilder b;
    b.setShaderStages(vs, fs_)
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
    if (p == VK_NULL_HANDLE) p = buildPipeline(vs_[static_cast<int>(kind)], kind, false, twoSided);
    return p;
}

VkPipeline PassShadow::customPipeline(const CustomShaderState& cs, MeshKind kind, bool twoSided) {
    std::string key = cs.key + (kind == MeshKind::Skinned ? "_skin" : "_static");
    if (twoSided) key += "_2s";
    auto it = custom_.find(key);
    if (it != custom_.end()) return it->second;

    VkPipeline pipeline = VK_NULL_HANDLE;
    VkShaderModule vs = VK_NULL_HANDLE;
    std::string err;
    CustomShaderInterface iface;
    if (iface.parse(cs.vertexChunk, cs.fragmentChunk, err) &&
        SceneVkCustomShader::compileCustomShadowShaderModule(device_->device(), kind == MeshKind::Skinned, iface,
                                                             vs, err)) {
        pipeline = buildPipeline(vs, kind, true, twoSided);
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
    const ShadowPlan& plan = frame.renderer.shadowPlan();
    // An atlas that failed to allocate at the planned size draws nothing.
    if (frame.gpu.targets.shadowAtlas.width != static_cast<uint32_t>(plan.atlasSize)) return false;
    for (int i = 0; i < plan.tileCount; ++i) {
        if (plan.tiles[i].render) return true;
    }
    return false;
}

void PassShadow::declare(const SceneFrame& frame, PassIO& io) const {
    io.depthTarget(frame.gpu.targets.shadowAtlas);
}

void PassShadow::drawCaster(SceneFrame& frame, const bromath::Mat4& lightViewProj, const MeshDraw& draw) {
    VkCommandBuffer cmd = frame.cmd;
    VkPipeline pipeline = VK_NULL_HANDLE;
    const bool custom = draw.custom && draw.customSet && !draw.custom->vertexChunk.empty() &&
                        draw.kind != MeshKind::Instanced;
    if (custom) pipeline = customPipeline(*draw.custom, draw.kind, draw.twoSided);
    const bool customBound = pipeline != VK_NULL_HANDLE;
    if (!pipeline) pipeline = builtinPipeline(draw.kind, draw.twoSided);
    if (!pipeline) return;

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    if (draw.kind != MeshKind::Instanced)
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0, 1, &frame.cameraSet, 0, nullptr);
    if (draw.vertexSet)
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 3, 1, &draw.vertexSet, 0, nullptr);
    if (customBound)
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 4, 1, &draw.customSet, 0, nullptr);

    Push push{};
    std::memcpy(push.lightViewProj, lightViewProj.data, sizeof(push.lightViewProj));
    // Column-major model -> its first three rows.
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 4; ++c) push.modelRows[r * 4 + c] = draw.push.model[c * 4 + r];
    }
    push.params[0] = draw.push.extra[2];
    vkCmdPushConstants(cmd, layout_, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(push), &push);

    SceneMeshDrawer::bindGeometryAndDraw(cmd, draw);
}

void PassShadow::record(SceneFrame& frame) {
    VkCommandBuffer cmd = frame.cmd;
    const ShadowPlan& plan = frame.renderer.shadowPlan();
    const SceneVkImage& atlas = frame.gpu.targets.shadowAtlas;
    const uint32_t size = atlas.width;

    // Every tile re-renders: clear the atlas as it opens. Otherwise each
    // re-rendered tile is cleared on its own and the cached ones keep theirs.
    bool all = true;
    for (int i = 0; i < plan.tileCount; ++i) all = all && plan.tiles[i].render;

    VkRenderingAttachmentInfo depth{};
    depth.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    depth.imageView = atlas.view;
    depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    depth.loadOp = all ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
    depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    depth.clearValue.depthStencil = {1.0f, 0};
    VkRenderingInfo info{};
    info.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    info.renderArea = {{0, 0}, {size, size}};
    info.layerCount = 1;
    info.pDepthAttachment = &depth;
    frame.gpu.device.cmdBeginRendering(cmd, &info);
    vkCmdSetDepthBias(cmd, kDepthBiasConstant, 0.0f, kDepthBiasSlope);

    const bool culling = frame.renderer.cullingFrustum() != nullptr;
    for (int slot = 0; slot < plan.tileCount; ++slot) {
        const ShadowTilePlan& tile = plan.tiles[slot];
        if (!tile.render) continue;
        const VkRect2D rect = tileRect(tile.rect, size);
        const VkViewport viewport{static_cast<float>(rect.offset.x), static_cast<float>(rect.offset.y),
                                  static_cast<float>(rect.extent.width), static_cast<float>(rect.extent.height),
                                  0.0f, 1.0f};
        vkCmdSetViewport(cmd, 0, 1, &viewport);
        vkCmdSetScissor(cmd, 0, 1, &rect);
        if (!all) {
            VkClearAttachment clear{};
            clear.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
            clear.clearValue.depthStencil = {1.0f, 0};
            const VkClearRect clearRect{rect, 0, 1};
            vkCmdClearAttachments(cmd, 1, &clear, 1, &clearRect);
        }

        // The draws are relative to the frame's eye, the tile to the plan's
        // (the same eye unless a caller re-planned in between).
        const bromath::Mat4 lightViewProj = toVulkanClip(rebased(tile.relViewProj, frame.view.eye - plan.origin));
        for (const MeshDraw& draw : frame.lists.meshes) {
            if (!draw.castsShadow) continue;
            if (culling && draw.hasBounds && !bromath::fintersects(tile.frustum, draw.bounds)) {
                frame.stats.shadowCulled++;
                continue;
            }
            frame.stats.shadowDrawn++;
            drawCaster(frame, lightViewProj, draw);
        }
    }

    frame.gpu.device.cmdEndRendering(cmd);
}

}  // namespace bro::scene::vk
