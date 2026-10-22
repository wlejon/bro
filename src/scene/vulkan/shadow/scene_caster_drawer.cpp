#include "scene/vulkan/shadow/scene_caster_drawer.h"

#include "scene/vulkan/draw/scene_vk_custom_shader.h"
#include "scene/vulkan/frame/scene_defaults.h"
#include "scene/vulkan/frame/scene_frame.h"
#include "scene/vulkan/frame/scene_targets.h"
#include "scene/vulkan/frame/scene_vk_device.h"
#include "scene/vulkan/frame/scene_vk_pipeline.h"
#include "scene/vulkan/frame/scene_vk_shader_compiler.h"
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
            case MeshKind::Grass: keep = a.location <= 1; break;
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
    vs_[static_cast<int>(MeshKind::Land)] =
        SceneVkShaderCompiler::createBuiltinModule(dev, BuiltinSceneShader::ShadowLandVert);
    fs_ = SceneVkShaderCompiler::createBuiltinModule(dev, BuiltinSceneShader::ShadowFrag);
    vsCutout_[0] = SceneVkShaderCompiler::createBuiltinModule(dev, BuiltinSceneShader::ShadowCutoutVert);
    vsCutout_[1] = SceneVkShaderCompiler::createBuiltinModule(dev, BuiltinSceneShader::ShadowInstancedCutoutVert);
    fsCutout_ = SceneVkShaderCompiler::createBuiltinModule(dev, BuiltinSceneShader::ShadowCutoutFrag);
    // (The blades and plants cast with their node's module set.)
    for (int k = 0; k < kMeshKindCount; ++k) {
        if (!vs_[k] && k != static_cast<int>(MeshKind::Grass) && k != static_cast<int>(MeshKind::Plants))
            return false;
    }
    return fs_ != VK_NULL_HANDLE && vsCutout_[0] != VK_NULL_HANDLE && vsCutout_[1] != VK_NULL_HANDLE && fsCutout_ != VK_NULL_HANDLE;
}

void SceneCasterDrawer::cleanup(SceneGpu& gpu) {
    VkDevice dev = gpu.device.device();
    std::vector<VkPipeline> pipelines;
    for (auto& perKind : builtin_) pipelines.insert(pipelines.end(), std::begin(perKind), std::end(perKind));
    for (auto& perKind : cutout_) pipelines.insert(pipelines.end(), std::begin(perKind), std::end(perKind));
    for (auto& perSet : grass_) pipelines.insert(pipelines.end(), std::begin(perSet), std::end(perSet));
    for (auto& perSet : plants_) pipelines.insert(pipelines.end(), std::begin(perSet), std::end(perSet));
    for (auto& [key, p] : custom_) pipelines.push_back(p);
    gpu.device.defer([dev, pipelines] {
        for (VkPipeline p : pipelines) {
            if (p != VK_NULL_HANDLE) vkDestroyPipeline(dev, p, nullptr);
        }
    });
    custom_.clear();
    grass_.clear();
    plants_.clear();
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

VkPipeline SceneCasterDrawer::buildPipeline(VkShaderModule vs, MeshKind kind, bool custom, bool twoSided,
                                            bool cutout) {
    std::vector<VkVertexInputBindingDescription> bindings;
    std::vector<VkVertexInputAttributeDescription> attributes;
    shadowVertexInput(kind, custom, cutout, bindings, attributes);

    TargetFormat target;
    target.depth = SceneTargets::kDepthFormat;

    SceneVkPipelineBuilder b;
    b.setShaderStages(vs, cutout ? fsCutout_ : fs_)
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

VkPipeline SceneCasterDrawer::builtinPipeline(MeshKind kind, bool twoSided) {
    VkPipeline& p = builtin_[static_cast<int>(kind)][twoSided ? 1 : 0];
    if (p == VK_NULL_HANDLE) p = buildPipeline(vs_[static_cast<int>(kind)], kind, false, twoSided, false);
    return p;
}

VkPipeline SceneCasterDrawer::grassPipeline(const SceneGpu& gpu, uint32_t cover, bool twoSided) {
    if (cover >= grass_.size()) grass_.resize(cover + 1, {VK_NULL_HANDLE, VK_NULL_HANDLE});
    VkPipeline& p = grass_[cover][twoSided ? 1 : 0];
    if (p == VK_NULL_HANDLE)
        p = buildPipeline(gpu.meshes.coverSet(cover)[CoverShaders::GrassShadowVert], MeshKind::Grass, false, twoSided,
                          false);
    return p;
}

VkPipeline SceneCasterDrawer::plantsPipeline(const SceneGpu& gpu, uint32_t set, bool twoSided) {
    if (set >= plants_.size()) plants_.resize(set + 1, {VK_NULL_HANDLE, VK_NULL_HANDLE});
    VkPipeline& p = plants_[set][twoSided ? 1 : 0];
    if (p == VK_NULL_HANDLE)
        p = buildPipeline(gpu.meshes.plantSet(set)[PlantShaders::ShadowVert], MeshKind::Plants, false, twoSided, false);
    return p;
}

VkPipeline SceneCasterDrawer::cutoutPipeline(MeshKind kind, bool twoSided) {
    const int instanced = kind == MeshKind::Instanced ? 1 : 0;
    VkPipeline& p = cutout_[instanced][twoSided ? 1 : 0];
    if (p == VK_NULL_HANDLE) p = buildPipeline(vsCutout_[instanced], kind, false, twoSided, true);
    return p;
}

VkPipeline SceneCasterDrawer::customPipeline(const CustomShaderState& cs, MeshKind kind, bool twoSided, bool cutout) {
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
        LOG_WARN("SceneCasterDrawer: a custom vertex chunk does not build as a shadow caster; the mesh casts its "
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
    const bool custom = draw.custom && draw.customSet && !draw.custom->vertexChunk.empty();
    if (custom) pipeline = customPipeline(*draw.custom, draw.kind, twoSided, cutout);
    const bool customBound = pipeline != VK_NULL_HANDLE;
    // (A custom chunk that does not build casts the undisplaced silhouette,
    // cut all the same.)
    if (!pipeline && cutout) pipeline = cutoutPipeline(draw.kind, twoSided);
    if (!pipeline && draw.kind == MeshKind::Grass) pipeline = grassPipeline(frame.gpu, draw.moduleSet, twoSided);
    if (!pipeline && draw.kind == MeshKind::Plants) pipeline = plantsPipeline(frame.gpu, draw.moduleSet, twoSided);
    if (!pipeline) pipeline = builtinPipeline(draw.kind, twoSided);
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
    std::memcpy(push.lightViewProj, viewProj.data, sizeof(push.lightViewProj));
    // Column-major model -> its first three rows.
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 4; ++c) push.modelRows[r * 4 + c] = draw.push.model[c * 4 + r];
    }
    // (A capture's wind mask is 0: it stands still.)
    push.params[0] = mode == Mode::Capture ? 0.0f : draw.push.extra[2];
    if (cutout) {
        // scene_shadow_cutout.glsl's: the cutoff, the base colour's alpha
        // (negated: the vertex colour's alpha counts too), the atlas grid.
        const bool vertexAlpha = (flags & mesh_flags::vertexColor(3)) != 0u;
        push.params[1] = draw.push.pbrParams[2];
        push.params[2] = vertexAlpha ? -draw.push.baseColor[3] : draw.push.baseColor[3];
        push.params[3] = draw.push.extra[3];
    }
    vkCmdPushConstants(cmd, layout_, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(push), &push);

    // A chunked caster draws the chunks inside the view (a caster's view,
    // a shadow page's or a probe capture's, is the body's).
    const SceneMeshDrawer::ChunkCull cull{frustum, frame.world.eye};
    if (draw.shadowIndexCount != 0 && draw.shadowIndexCount < draw.indexCount) {
        MeshDraw cut = draw;
        cut.indexCount = draw.shadowIndexCount;
        SceneMeshDrawer::bindGeometryAndDraw(cmd, cut, &cull);
        return;
    }
    SceneMeshDrawer::bindGeometryAndDraw(cmd, draw, &cull);
}

}  // namespace bro::scene::vk
