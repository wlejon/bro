#include "scene/vulkan/shadow/scene_caster_drawer.h"
#include "scene/vulkan/frame/scene_shader_sets.h"

#include "render/vulkan_pipeline_cache.h"
#include "scene/vulkan/draw/scene_vk_custom_shader.h"
#include "scene/vulkan/draw/scene_vk_kernel_hook.h"
#include "scene/vulkan/frame/scene_defaults.h"
#include "scene/vulkan/frame/scene_frame.h"
#include "scene/vulkan/frame/scene_targets.h"
#include "scene/vulkan/frame/scene_vk_device.h"
#include "scene/vulkan/frame/scene_vk_pipeline.h"
#include "util/log.h"

#include <array>
#include <cstring>

namespace bro::scene::vk {

namespace {

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
            default: keep = a.location <= 3 || a.location == 5 || a.location == 6 || veg; break;
            }
        }
        if (keep) used.push_back(a);
    }
    attributes = std::move(used);
}

}  // namespace

bool SceneCasterDrawer::setup(SceneGpu& gpu) {
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

    static constexpr const char* kVsEntries[kMeshKindCount] = {
        "shadow_vert",
        "shadow_instanced_vert",
        "shadow_skinned_vert",
        "shadow_scatter_vert",
        "shadow_tube_vert",
        "shadow_veg_vert",
        "shadow_instanced_veg_vert",
    };
    auto buildStage = [&](const char* entry, VkShaderModule& mod, std::string& outEntry) {
        KernelHookStage stage;
        std::string err;
        if (!compileKernelShadowStage(entry, stage, err)) {
            LOG_ERROR("SceneCasterDrawer: %s failed: %s", entry, err.c_str());
            return false;
        }
        mod = SceneVkShaderModule::create(dev, stage.spirv);
        outEntry = std::move(stage.entry);
        return mod != VK_NULL_HANDLE;
    };
    for (int k = 0; k < kMeshKindCount; ++k) {
        if (!buildStage(kVsEntries[k], vs_[k], vsEntry_[k])) return false;
    }
    return buildStage("shadow_frag", fs_, fsEntry_) &&
           buildStage("shadow_cutout_vert", vsCutout_[0], vsCutoutEntry_[0]) &&
           buildStage("shadow_instanced_cutout_vert", vsCutout_[1], vsCutoutEntry_[1]) &&
           buildStage("shadow_cutout_frag", fsCutout_, fsCutoutEntry_);
}

void SceneCasterDrawer::cleanup(SceneGpu& gpu) {
    VkDevice dev = gpu.device.device();
    std::vector<VkPipeline> pipelines;
    for (auto& [key, p] : custom_) pipelines.push_back(p);
    gpu.device.defer([dev, pipelines] {
        for (VkPipeline p : pipelines) {
            if (p != VK_NULL_HANDLE) destroyScenePipeline(dev, p, nullptr);
        }
    });
    custom_.clear();
    pipelines_.destroy(gpu.device);
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



VkPipeline SceneCasterDrawer::buildPipeline(VkShaderModule vs, MeshKind kind, bool custom, bool twoSided,
                                            bool cutout, const char* vsEntry) {
    std::vector<VkVertexInputBindingDescription> bindings;
    std::vector<VkVertexInputAttributeDescription> attributes;
    shadowVertexInput(kind, custom, cutout, bindings, attributes);

    TargetFormat target;
    target.depth = SceneTargets::kDepthFormat;

    SceneVkPipelineBuilder b;
    if (render::pipelineStatsEnabled()) {
        b.setLabel(std::string(custom ? "custom shadow" : "shadow") + " kind " +
                   std::to_string(static_cast<int>(kind)) + (cutout ? " cutout" : ""));
    }
    b.setShaderStages(vs, cutout ? fsCutout_ : fs_, vsEntry, cutout ? fsCutoutEntry_.c_str() : fsEntry_.c_str())
     .setVertexInput(bindings, attributes)
     .setInputTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
     .setPolygonMode(VK_POLYGON_MODE_FILL)
     .setCullMode(twoSided ? VK_CULL_MODE_NONE : VK_CULL_MODE_FRONT_BIT, VK_FRONT_FACE_COUNTER_CLOCKWISE)
     .setTarget(target)
     .disableBlending(0)
     .setDepthBias(true)
     // Outside the camera depth policy: always conventional.
     .enableDepthTest(true, VK_COMPARE_OP_LESS_OR_EQUAL)
     .setDynamicStates({VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_DEPTH_BIAS});
    return b.build(device_->device(), layout_);
}

namespace {
TargetFormat casterTarget() {
    TargetFormat target;
    target.depth = SceneTargets::kDepthFormat;
    return target;
}
}  // namespace

VkPipeline SceneCasterDrawer::builtinPipeline(MeshKind kind, bool twoSided) {
    const uint32_t variant = kBuiltin | static_cast<uint32_t>(kind) << 1 | (twoSided ? 1u : 0u);
    const int k = static_cast<int>(kind);
    return pipelines_.get(variant, casterTarget(), [&] {
        return buildPipeline(vs_[k], kind, false, twoSided, false, vsEntry_[k].c_str());
    }, /*async=*/true);
}

VkPipeline SceneCasterDrawer::cutoutPipeline(MeshKind kind, bool twoSided) {
    const int instanced = kind == MeshKind::Instanced ? 1 : 0;
    const uint32_t variant = kCutout | static_cast<uint32_t>(instanced) << 1 | (twoSided ? 1u : 0u);
    return pipelines_.get(variant, casterTarget(), [&] {
        return buildPipeline(vsCutout_[instanced], kind, false, twoSided, true, vsCutoutEntry_[instanced].c_str());
    }, /*async=*/true);
}

VkPipeline SceneCasterDrawer::customPipeline(const CustomShaderState& cs, MeshKind kind, bool twoSided, bool cutout) {
    const ShadowCaster caster = kind == MeshKind::Skinned     ? ShadowCaster::Skinned
                              : kind == MeshKind::Instanced ? ShadowCaster::Instanced
                                                            : ShadowCaster::Static;
    std::string key = cs.key + (caster == ShadowCaster::Skinned ? "_skin" : caster == ShadowCaster::Instanced ? "_inst" : "_static");
    if (twoSided) key += "_2s";
    if (cutout) key += "_cut";
    auto it = custom_.find(key);
    if (it != custom_.end()) return it->second;

    VkPipeline pipeline = VK_NULL_HANDLE;
    std::string err;
    KernelHookStage stage;
    if (compileKernelShadowHook(caster, cutout, cs.module, cs.moduleFile, cs.modules, stage, err)) {
        VkShaderModule vs = SceneVkShaderModule::create(device_->device(), stage.spirv);
        if (vs) {
            pipeline = buildPipeline(vs, kind, true, twoSided, cutout, stage.entry.c_str());
            SceneVkShaderModule::destroy(device_->device(), vs);
        }
    }
    if (pipeline == VK_NULL_HANDLE) {
        LOG_WARN("SceneCasterDrawer: a custom vertex hook does not build as a shadow caster; the mesh casts its "
                 "undisplaced silhouette: %s", err.c_str());
    }
    custom_.emplace(std::move(key), pipeline);
    return pipeline;
}

void SceneCasterDrawer::draw(SceneFrame& frame, VkCommandBuffer cmd, const bromath::Mat4& viewProj,
                             const MeshDraw& draw, const bromath::Frustum* frustum, Mode mode) {
    VkPipeline pipeline = VK_NULL_HANDLE;
    const bool twoSided = draw.twoSided || mode == Mode::Capture;
    const uint32_t flags = static_cast<uint32_t>(draw.push.pbrParams[3]);
    // A textured static or instanced mesh with an alpha cutoff casts its cut.
    const bool cutout = (draw.kind == MeshKind::Static || draw.kind == MeshKind::Instanced) &&
                        draw.push.pbrParams[2] > 0.0f && (flags & mesh_flags::kAlbedoMap) != 0u &&
                        draw.materialSet != VK_NULL_HANDLE;
    const bool custom = draw.custom && draw.customSet && draw.custom->hasVertex;
    if (custom) pipeline = customPipeline(*draw.custom, draw.kind, twoSided, cutout);
    const bool customBound = pipeline != VK_NULL_HANDLE;
    // (A custom chunk that does not build casts the undisplaced silhouette,
    // cut all the same. A pipeline still being built draws nothing.)
    if (!pipeline)
        pipeline = cutout ? cutoutPipeline(draw.kind, twoSided) : builtinPipeline(draw.kind, twoSided);
    if (!pipeline) return;

    bindScenePipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    if (draw.kind != MeshKind::Instanced || customBound)
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0, 1, &frame.cameraSet, 0, nullptr);
    if (cutout)
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 2, 1, &draw.materialSet, 0, nullptr);
    if (draw.vertexSet)
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 3, 1, &draw.vertexSet, 0, nullptr);
    if (customBound)
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 4, 1, &draw.customSet, 0, nullptr);

    Push push{};
    std::memcpy(push.lightViewProj, viewProj.data, sizeof(push.lightViewProj));
    // Column-major model -> its first three rows.
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 4; ++c) push.modelRows[r * 4 + c] = draw.push.model[c * 4 + r];
    }
    // (A capture's wind mask is 0: it stands still.)
    push.params[0] = mode == Mode::Capture ? 0.0f : draw.push.extra[2];
    if (cutout) {
        // shadow/shadow.bk's cutout stages: the cutoff, the base colour's alpha
        // (negated: the vertex colour's alpha counts too), the atlas grid.
        const bool vertexAlpha = (flags & mesh_flags::vertexColor(3)) != 0u;
        push.params[1] = draw.push.pbrParams[2];
        push.params[2] = vertexAlpha ? -draw.push.baseColor[3] : draw.push.baseColor[3];
        push.params[3] = draw.push.extra[3];
    }
    vkCmdPushConstants(cmd, layout_, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(push), &push);

    // A chunked caster draws the chunks inside the view.
    const SceneMeshDrawer::ChunkCull cull{frustum, frame.view.eye};
    if (draw.shadowIndexCount != 0 && draw.shadowIndexCount < draw.indexCount) {
        MeshDraw cut = draw;
        cut.indexCount = draw.shadowIndexCount;
        SceneMeshDrawer::bindGeometryAndDraw(cmd, cut, &cull);
        return;
    }
    SceneMeshDrawer::bindGeometryAndDraw(cmd, draw, &cull);
}

}  // namespace bro::scene::vk
