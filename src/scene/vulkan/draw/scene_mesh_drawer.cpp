#include "scene/vulkan/draw/scene_mesh_drawer.h"

#include "scene/depth_policy.h"
#include "scene/nodes/grass_node.h"
#include "scene/nodes/instanced_mesh_node.h"
#include "scene/scene_graph.h"
#include "scene/nodes/skinned_mesh_node.h"
#include "scene/vulkan/frame/scene_defaults.h"
#include "scene/vulkan/frame/scene_frame.h"
#include "scene/vulkan/frame/scene_gpu_resources.h"
#include "scene/vulkan/frame/scene_targets.h"
#include "scene/vulkan/foliage/scene_plants_gpu_plan.h"
#include "scene/vulkan/ground/scene_cover_plan.h"
#include "scene/vulkan/draw/scene_vk_custom_shader.h"
#include "scene/vulkan/frame/scene_vk_device.h"
#include "scene/vulkan/frame/scene_vk_shader_compiler.h"
#include "scene/vulkan/world/scene_world_service.h"
#include "render/vulkan_pipeline_cache.h"
#include "util/log.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

namespace bro::scene::vk {

namespace {

constexpr uint32_t kBonePaletteSize = 256;

SceneRenderer::CustomShaderTarget shaderTarget(MeshKind kind) {
    switch (kind) {
    case MeshKind::Instanced:
    case MeshKind::InstancedVegetation: return SceneRenderer::CustomShaderTarget::Instanced;
    case MeshKind::Skinned: return SceneRenderer::CustomShaderTarget::Skinned;
    case MeshKind::Static:
    case MeshKind::Vegetation:
    case MeshKind::Scatter:
    case MeshKind::Tube:
    case MeshKind::Grass:
    case MeshKind::Plants:
    case MeshKind::Land: break;
    }
    return SceneRenderer::CustomShaderTarget::Static;
}

}  // namespace

void SceneMeshDrawer::vertexInput(MeshKind kind, std::vector<VkVertexInputBindingDescription>& bindings,
                                  std::vector<VkVertexInputAttributeDescription>& attributes) {
    if (streamless(kind)) {
        bindings.clear();
        attributes.clear();
        return;
    }
    bindings = {{0, SceneGpuResources::kVertexStride, VK_VERTEX_INPUT_RATE_VERTEX}};
    attributes = {
        {0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0},       // position
        {1, 0, VK_FORMAT_R32G32B32_SFLOAT, 12},      // normal
        {2, 0, VK_FORMAT_R32G32_SFLOAT, 24},         // uv
        {3, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 32},   // colour
        {4, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 48},   // tangent
    };
    // The vegetation record: four vec4s at locations 12-15 (scene_vegetation.glsl).
    const auto vegetation = [&](uint32_t binding, VkVertexInputRate rate) {
        bindings.push_back({binding, 64, rate});
        for (uint32_t i = 0; i < 4; ++i)
            attributes.push_back({12 + i, binding, VK_FORMAT_R32G32B32A32_SFLOAT, 16 * i});
    };
    if (kind == MeshKind::Instanced || kind == MeshKind::InstancedVegetation) {
        bindings.push_back({1, 64, VK_VERTEX_INPUT_RATE_INSTANCE});
        attributes.push_back({8, 1, VK_FORMAT_R32G32B32A32_SFLOAT, 0});    // row 0
        attributes.push_back({9, 1, VK_FORMAT_R32G32B32A32_SFLOAT, 16});   // row 1
        attributes.push_back({10, 1, VK_FORMAT_R32G32B32A32_SFLOAT, 32});  // row 2
        attributes.push_back({11, 1, VK_FORMAT_R32G32B32A32_SFLOAT, 48});  // colour
        if (kind == MeshKind::InstancedVegetation) vegetation(2, VK_VERTEX_INPUT_RATE_INSTANCE);
    } else if (kind == MeshKind::Vegetation) {
        vegetation(1, VK_VERTEX_INPUT_RATE_VERTEX);
    } else if (kind == MeshKind::Skinned) {
        bindings.push_back({1, SceneGpuResources::kSkinStride, VK_VERTEX_INPUT_RATE_VERTEX});
        attributes.push_back({5, 1, VK_FORMAT_R16G16B16A16_UINT, 0});      // joints
        attributes.push_back({6, 1, VK_FORMAT_R32G32B32A32_SFLOAT, 8});    // weights
    }
}

bool SceneMeshDrawer::setup(SceneGpu& gpu) {
    device_ = gpu.device.device();
    defaults_ = &gpu.defaults;
    wideLines_ = gpu.device.context().wideLines();

    const std::array<VkDescriptorSetLayout, 5> sets = {gpu.defaults.cameraLayout, gpu.defaults.lightingLayout,
                                                       gpu.defaults.materialLayout, gpu.defaults.boneLayout,
                                                       gpu.defaults.customLayout};
    VkPushConstantRange push{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(MeshPushConstants)};
    VkPipelineLayoutCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    info.setLayoutCount = static_cast<uint32_t>(sets.size());
    info.pSetLayouts = sets.data();
    info.pushConstantRangeCount = 1;
    info.pPushConstantRanges = &push;
    if (vkCreatePipelineLayout(device_, &info, nullptr, &layout_) != VK_SUCCESS) {
        LOG_ERROR("SceneMeshDrawer: Failed creating the pipeline layout");
        return false;
    }

    vs_[static_cast<int>(MeshKind::Static)] =
        SceneVkShaderCompiler::createBuiltinModule(device_, BuiltinSceneShader::MeshVert);
    vs_[static_cast<int>(MeshKind::Instanced)] =
        SceneVkShaderCompiler::createBuiltinModule(device_, BuiltinSceneShader::MeshInstancedVert);
    vs_[static_cast<int>(MeshKind::Skinned)] =
        SceneVkShaderCompiler::createBuiltinModule(device_, BuiltinSceneShader::MeshSkinnedVert);
    vs_[static_cast<int>(MeshKind::Scatter)] =
        SceneVkShaderCompiler::createBuiltinModule(device_, BuiltinSceneShader::MeshScatterVert);
    vs_[static_cast<int>(MeshKind::Tube)] =
        SceneVkShaderCompiler::createBuiltinModule(device_, BuiltinSceneShader::MeshTubeVert);
    vs_[static_cast<int>(MeshKind::Vegetation)] =
        SceneVkShaderCompiler::createBuiltinModule(device_, BuiltinSceneShader::MeshVegVert);
    vs_[static_cast<int>(MeshKind::InstancedVegetation)] =
        SceneVkShaderCompiler::createBuiltinModule(device_, BuiltinSceneShader::MeshInstancedVegVert);
    vs_[static_cast<int>(MeshKind::Land)] =
        SceneVkShaderCompiler::createBuiltinModule(device_, BuiltinSceneShader::MeshLandVert);
    // (The blades' and the plants' stages are their node's module set's.)
    fs_ = SceneVkShaderCompiler::createBuiltinModule(device_, BuiltinSceneShader::MeshFrag);
    fsIndirect_ = SceneVkShaderCompiler::createBuiltinModule(device_, BuiltinSceneShader::MeshIndirectFrag);
    fsVeg_ = SceneVkShaderCompiler::createBuiltinModule(device_, BuiltinSceneShader::MeshVegFrag);
    fsVegIndirect_ = SceneVkShaderCompiler::createBuiltinModule(device_, BuiltinSceneShader::MeshVegIndirectFrag);
    fsBark_ = SceneVkShaderCompiler::createBuiltinModule(device_, BuiltinSceneShader::MeshBarkFrag);
    fsBarkIndirect_ = SceneVkShaderCompiler::createBuiltinModule(device_, BuiltinSceneShader::MeshBarkIndirectFrag);
    fsCoverFar_ = SceneVkShaderCompiler::createBuiltinModule(device_, BuiltinSceneShader::MeshCoverFarFrag);
    fsCoverFarIndirect_ =
        SceneVkShaderCompiler::createBuiltinModule(device_, BuiltinSceneShader::MeshCoverFarIndirectFrag);
    covers_.push_back(std::make_unique<CoverShaders>());
    bool vsAll = true;
    for (int k = 0; k < kMeshKindCount; ++k)
        vsAll = vsAll && (vs_[k] || k == static_cast<int>(MeshKind::Grass) || k == static_cast<int>(MeshKind::Plants));
    if (!fsCoverFar_ || !fsCoverFarIndirect_ || !vsAll || !fs_ || !fsIndirect_ || !fsVeg_ || !fsVegIndirect_ || !fsBark_ || !fsBarkIndirect_ ||
        !covers_[0]->createBuiltin(device_)) {
        LOG_ERROR("SceneMeshDrawer: Failed creating the mesh shader modules");
        return false;
    }

    // The plants' GPU planner.
    plantPlanner_ = std::make_shared<plants::GpuPlanner>();
    if (!plantPlanner_->setup(gpu)) {
        LOG_ERROR("SceneMeshDrawer: Failed setting up the plants' GPU planner");
        return false;
    }
    coverPlan_ = std::make_shared<CoverPlan>();
    if (!coverPlan_->setup(gpu)) {
        LOG_ERROR("SceneMeshDrawer: Failed setting up the cover's plan");
        return false;
    }
    // The land's tiles and then the turf's surface layer fills are the world
    // service's jobs (recorded in that order), a first guess at a fill's
    // cost until the service measures one.
    if (!setupLand(gpu)) return false;
    if (turfKind_ < 0) turfKind_ = gpu.world.addKind(this, {"turf", WorldSchedule::Raster, 0.03f});
    if (turfKind_ < 0) {
        LOG_ERROR("SceneMeshDrawer: the world service has no room for the turf's fills");
        return false;
    }
    return true;
}

void SceneMeshDrawer::offer(SceneFrame& frame, WorldService& service) {
    offerLand(frame, service);
    offerLandSun(frame, service);
    offerTurf(frame, service);
}

double SceneMeshDrawer::record(SceneFrame& frame, int kind, std::span<const WorldJob> granted, WorldStream& stream) {
    if (kind == landKind_) return recordLand(frame, granted, stream);
    if (kind == horizonKind_) return recordLandHorizon(frame, granted, stream);
    if (kind == landSunKind_) return recordLandSun(frame, granted, stream);
    if (kind == turfKind_) return recordTurf(frame, granted, stream);
    return 0;
}

void SceneMeshDrawer::cleanup(SceneGpu& gpu) {
    cleanupLand(gpu);
    builtin_.destroy(gpu.device);
    for (auto& [id, layers] : groundLayers_) layers->atlas.destroy(gpu.device, gpu.allocator);
    groundLayers_.clear();
    for (auto& c : covers_) {
        c->pipelines.destroy(gpu.device);
        c->destroy(device_);
    }
    covers_.clear();
    coverByModule_.clear();
    for (auto& p : plantSets_) {
        p->pipelines.destroy(gpu.device);
        p->destroy(device_);
    }
    plantSets_.clear();
    plantByModule_.clear();
    for (auto& [key, prog] : custom_) {
        prog->pipelines.destroy(gpu.device);
        for (VkShaderModule m : {prog->vs, prog->fs, prog->fsIndirect}) SceneVkShaderModule::destroy(device_, m);
    }
    custom_.clear();
    for (VkShaderModule& m : vs_) {
        SceneVkShaderModule::destroy(device_, m);
        m = VK_NULL_HANDLE;
    }
    SceneVkShaderModule::destroy(device_, fs_);
    SceneVkShaderModule::destroy(device_, fsIndirect_);
    SceneVkShaderModule::destroy(device_, fsVeg_);
    SceneVkShaderModule::destroy(device_, fsVegIndirect_);
    SceneVkShaderModule::destroy(device_, fsBark_);
    SceneVkShaderModule::destroy(device_, fsBarkIndirect_);
    SceneVkShaderModule::destroy(device_, fsCoverFar_);
    SceneVkShaderModule::destroy(device_, fsCoverFarIndirect_);
    fsCoverFar_ = fsCoverFarIndirect_ = VK_NULL_HANDLE;
    fs_ = fsIndirect_ = fsVeg_ = fsVegIndirect_ = fsBark_ = fsBarkIndirect_ = VK_NULL_HANDLE;
    if (layout_ != VK_NULL_HANDLE) vkDestroyPipelineLayout(device_, layout_, nullptr);
    layout_ = VK_NULL_HANDLE;
    if (plantPlanner_) plantPlanner_->cleanup(gpu);
    plantPlanner_.reset();
    if (coverPlan_) coverPlan_->cleanup(gpu);
    coverPlan_.reset();
}

namespace raster {
constexpr uint32_t kTranslucent = 1;
constexpr uint32_t kTwoSided = 2;
constexpr uint32_t kLines = 4;
constexpr uint32_t kMirrored = 8;
constexpr uint32_t kNoDepth = 16;
// It cuts pixels (a near clip or an alpha cutoff): mesh.frag's discards are
// built in only then (MESH_CUTS). A fragment stage that can discard is
// depth-tested after it shades, so every layer behind the nearest was shaded
// too: the wilds' ground, three meshes over one another, its turf shaded
// under the near tiles twice over.
constexpr uint32_t kCuts = 32;
// The frame has the atmosphere on: the lit stages are built with its code
// (scene_lighting.glsl's SCENE_ATMOSPHERE), and without it when it is off.
constexpr uint32_t kAtmosphere = 64;
// The cover's two passes (CoverPass): its depth alone, then lit where its
// depth is the nearest.
constexpr uint32_t kCoverDepth = 128;
constexpr uint32_t kCoverShade = 256;
constexpr uint32_t kCount = 512;
// mesh.frag's MESH_CUTS.
constexpr uint32_t kCutsConstantId = 1;
// scene_lighting.glsl's SCENE_ATMOSPHERE.
constexpr uint32_t kAtmosphereConstantId = 2;
// mesh.frag's MESH_COVER_DEPTH and MESH_COVER_SHADE.
constexpr uint32_t kCoverDepthConstantId = 3;
constexpr uint32_t kCoverShadeConstantId = 4;
}

bool SceneMeshDrawer::coverPrepassed(const MeshDraw& draw) const {
    // The blades and herb plants (not the far tier's tufts) of an opaque
    // draw that cuts nothing else.
    return draw.kind == MeshKind::Grass &&
           (static_cast<uint32_t>(draw.push.pbrParams[3]) & mesh_flags::kCoverFar) == 0u &&
           (rasterVariant(draw) & (raster::kTranslucent | raster::kLines | raster::kNoDepth | raster::kCuts)) == 0u;
}

uint32_t SceneMeshDrawer::rasterVariant(const MeshDraw& draw) const {
    return (atmosphere_ ? raster::kAtmosphere : 0u) | (draw.translucent ? raster::kTranslucent : 0u) |
           (draw.twoSided ? raster::kTwoSided : 0u) |
           (draw.lines ? raster::kLines : 0u) | (draw.mirrored ? raster::kMirrored : 0u) |
           (draw.noDepthTest ? raster::kNoDepth : 0u) |
           (draw.push.extra[0] > 0.0f || draw.push.pbrParams[2] > 0.0f ? raster::kCuts : 0u);
}

VkPipeline SceneMeshDrawer::buildPipeline(VkShaderModule vs, VkShaderModule fs, MeshKind kind, uint32_t rasterBits,
                                          const TargetFormat& target) {
    std::vector<VkVertexInputBindingDescription> bindings;
    std::vector<VkVertexInputAttributeDescription> attributes;
    vertexInput(kind, bindings, attributes);

    const bool lines = (rasterBits & raster::kLines) != 0;
    const bool cullBack = (rasterBits & (raster::kTwoSided | raster::kLines)) == 0;
    std::vector<VkDynamicState> dynamic = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR,
                                           VK_DYNAMIC_STATE_DEPTH_BIAS};
    if (lines && wideLines_) dynamic.push_back(VK_DYNAMIC_STATE_LINE_WIDTH);

    SceneVkPipelineBuilder b;
    b.setShaderStages(vs, fs)
     .setVertexInput(bindings, attributes)
     .setInputTopology(lines ? VK_PRIMITIVE_TOPOLOGY_LINE_LIST : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
     .setPolygonMode(VK_POLYGON_MODE_FILL)
     .setCullMode(cullBack ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE,
                  (rasterBits & raster::kMirrored) ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE)
     .setTarget(target)
     .setDepthBias(true)
     .setDynamicStates(dynamic)
     .setSpecialization(raster::kCutsConstantId, (rasterBits & raster::kCuts) != 0)
     .setSpecialization(raster::kAtmosphereConstantId, (rasterBits & raster::kAtmosphere) != 0)
     .setSpecialization(raster::kCoverDepthConstantId, (rasterBits & raster::kCoverDepth) != 0)
     .setSpecialization(raster::kCoverShadeConstantId, (rasterBits & raster::kCoverShade) != 0);
    if (rasterBits & raster::kCoverDepth) {
        // The depth alone.
        b.disableBlending(target.colorCount).enableDepthTest(true);
        for (uint32_t i = 0; i < target.colorCount; ++i) {
            VkPipelineColorBlendAttachmentState none{};
            b.setColorBlendAttachment(i, none);
        }
    } else if (rasterBits & raster::kCoverShade) {
        // Lit where the prepass left its depth: depth-tested before shading.
        b.disableBlending(target.colorCount).enableDepthTest(false, VK_COMPARE_OP_EQUAL);
    } else if (rasterBits & raster::kTranslucent) {
        b.enableAlphaBlending(1);
        if (rasterBits & raster::kNoDepth) {
            b.disableDepthTest();
        } else {
            b.enableDepthTest(false);
        }
    } else {
        // Every attachment is written: the sub-pixel mark by every build of
        // the fragment stage, the indirect-light one, when the target has
        // it, by the indirect variant.
        b.disableBlending(target.colorCount).enableDepthTest(true);
    }
    return b.build(device_, layout_);
}

VkPipeline SceneMeshDrawer::builtinPipeline(const MeshDraw& draw, const TargetFormat& target, CoverPass pass) {
    const uint32_t rasterBits = rasterVariant(draw) | (pass == CoverPass::Depth   ? raster::kCoverDepth
                                                       : pass == CoverPass::Shade ? raster::kCoverShade
                                                                                  : 0u);
    // Turf has its own build of mesh.frag (a static mesh's or a land's:
    // SCENE_GROUND), so the other meshes' carries none of its work.
    const bool ground = (draw.kind == MeshKind::Static || draw.kind == MeshKind::Land) &&
                        (static_cast<uint32_t>(draw.push.pbrParams[3]) & mesh_flags::kGround) != 0u;
    // And a grass node's herb plants, a grass draw with its own stages.
    const bool herb = draw.kind == MeshKind::Grass &&
                      (static_cast<uint32_t>(draw.push.pbrParams[3]) & mesh_flags::kHerb) != 0u;
    // And the cover's far tier, the same.
    const bool coverFar = draw.kind == MeshKind::Grass &&
                          (static_cast<uint32_t>(draw.push.pbrParams[3]) & mesh_flags::kCoverFar) != 0u;
    // The grass and turf take their node's shaders, and the set keeps their
    // pipelines.
    const bool covered = draw.kind == MeshKind::Grass || ground;
    CoverShaders& cover = *covers_[covered && draw.moduleSet < covers_.size() ? draw.moduleSet : 0];
    // The plants take their node's, likewise (prepared only with one).
    if (draw.kind == MeshKind::Plants) {
        PlantShaders& set = *plantSets_[draw.moduleSet];
        return set.pipelines.get(rasterBits, target, [&] {
            const bool indirect = SceneTargets::carriesIndirect(target);
            return buildPipeline(set[PlantShaders::Vert], set[indirect ? PlantShaders::IndirectFrag : PlantShaders::Frag],
                                 draw.kind, rasterBits, target);
        });
    }
    const uint32_t block = static_cast<uint32_t>(kMeshKindCount) * raster::kCount;
    const uint32_t variant = static_cast<uint32_t>(draw.kind) * raster::kCount + rasterBits +
                             (ground ? 1u * block : 0u) + (herb ? 2u * block : 0u) + (coverFar ? 3u * block : 0u);
    return (covered ? cover.pipelines : builtin_).get(variant, target, [&] {
        const bool indirect = SceneTargets::carriesIndirect(target);
        if (render::pipelineStatsEnabled()) {
            render::setPipelineStatsLabel(std::string("mesh kind ") + std::to_string(static_cast<int>(draw.kind)) +
                                          (ground ? " ground" : "") + (herb ? " herb" : "") +
                                          (coverFar ? " cover far" : "") + (indirect ? " indirect" : "") +
                                          " raster " + std::to_string(rasterBits));
        }
        if (herb)
            return buildPipeline(cover[CoverShaders::HerbVert],
                                 cover[indirect ? CoverShaders::HerbIndirectFrag : CoverShaders::HerbFrag], draw.kind,
                                 rasterBits, target);
        if (coverFar)
            return buildPipeline(cover[CoverShaders::FarVert], indirect ? fsCoverFarIndirect_ : fsCoverFar_, draw.kind,
                                 rasterBits, target);
        const bool veg = vegetationShading(draw.kind);
        VkShaderModule vs = vs_[static_cast<int>(draw.kind)];
        VkShaderModule fs = indirect ? (veg ? fsVegIndirect_ : fsIndirect_) : (veg ? fsVeg_ : fs_);
        if (draw.kind == MeshKind::Tube) fs = indirect ? fsBarkIndirect_ : fsBark_;
        if (ground) fs = cover[indirect ? CoverShaders::GroundIndirectFrag : CoverShaders::GroundFrag];
        // (A surface layer tile's fill draws the turf into its four layers.)
        if (ground && target.colorCount == 4) fs = cover[CoverShaders::GroundFill];
        if (draw.kind == MeshKind::Grass) {
            vs = cover[CoverShaders::GrassVert];
            fs = cover[indirect ? CoverShaders::GrassIndirectFrag : CoverShaders::GrassFrag];
        }
        return buildPipeline(vs, fs, draw.kind, rasterBits, target);
    });
}

SceneMeshDrawer::CustomProgram* SceneMeshDrawer::program(const CustomShaderState& cs, MeshKind kind) {
    std::string key = cs.key;
    key += '\x1e';
    key += static_cast<char>('0' + static_cast<int>(kind));
    auto it = custom_.find(key);
    if (it != custom_.end()) return it->second->failed ? nullptr : it->second.get();

    auto prog = std::make_unique<CustomProgram>();
    std::string err;
    if (!prog->iface.parse(cs.vertexChunk, cs.fragmentChunk, err) ||
        !SceneVkCustomShader::compileCustomShaderModules(device_, shaderTarget(kind), prog->iface, prog->vs,
                                                         prog->fs, false, err)) {
        // Logged once; the node draws with the built-in shading from now on.
        LOG_ERROR("SceneMeshDrawer: Failed compiling a custom shader: %s", err.c_str());
        prog->failed = true;
        custom_.emplace(std::move(key), std::move(prog));
        return nullptr;
    }
    return custom_.emplace(std::move(key), std::move(prog)).first->second.get();
}

VkPipeline SceneMeshDrawer::customPipeline(CustomProgram& prog, const MeshDraw& draw, const TargetFormat& target) {
    const MeshKind kind = draw.kind;
    const uint32_t rasterBits = rasterVariant(draw);
    return prog.pipelines.get(rasterBits, target, [&]() -> VkPipeline {
        VkShaderModule fs = prog.fs;
        if (SceneTargets::carriesIndirect(target)) {
            if (prog.indirectFailed) return VK_NULL_HANDLE;
            if (prog.fsIndirect == VK_NULL_HANDLE) {
                VkShaderModule vs = VK_NULL_HANDLE;
                std::string err;
                // The chunks compiled once already, so this is the indirect
                // splice failing: the node keeps the built-in shading there.
                if (!SceneVkCustomShader::compileCustomShaderModules(device_, shaderTarget(kind), prog.iface, vs,
                                                                     prog.fsIndirect, true, err)) {
                    LOG_ERROR("SceneMeshDrawer: Failed compiling the indirect custom variant: %s", err.c_str());
                    prog.indirectFailed = true;
                    return VK_NULL_HANDLE;
                }
                SceneVkShaderModule::destroy(device_, vs);
            }
            fs = prog.fsIndirect;
        }
        return buildPipeline(prog.vs, fs, kind, rasterBits, target);
    });
}

VkDescriptorSet SceneMeshDrawer::boneSet(SceneFrame& frame, const std::vector<float>& palette) {
    std::vector<float> bones(kBonePaletteSize * 16, 0.0f);
    for (uint32_t b = 0; b < kBonePaletteSize; ++b) {
        bones[b * 16 + 0] = bones[b * 16 + 5] = bones[b * 16 + 10] = bones[b * 16 + 15] = 1.0f;
    }
    std::memcpy(bones.data(), palette.data(), std::min(palette.size(), bones.size()) * sizeof(float));
    const VkDescriptorBufferInfo ubo = frame.gpu.device.frameUniform(bones.data(), bones.size() * sizeof(float));
    VkDescriptorSet set = frame.gpu.device.frameSet(defaults_->boneLayout);
    SceneVkDescriptorWriter writer;
    writer.writeBuffer(0, ubo.buffer, ubo.range, ubo.offset);
    writer.updateSet(device_, set);
    return set;
}

VkDescriptorSet SceneMeshDrawer::segmentSet(SceneFrame& frame, float (&header)[16],
                                            const std::vector<float>& records,
                                            const std::vector<float>* leafSegments,
                                            const std::vector<float>* vegetation) {
    constexpr size_t kHeader = 16;   // scene_procedural.glsl's header[4]
    const size_t leafFloats = leafSegments ? (leafSegments->size() + 3) / 4 * 4 : 0;
    // Records are whole vec4s, so the vegetation starts on one.
    const size_t recordFloats = (records.size() + 3) / 4 * 4;
    const size_t vegStart = recordFloats + leafFloats;
    const size_t vegFloats = vegetation ? vegetation->size() : 0;
    header[10] = vegetation ? static_cast<float>(vegStart / 4) : 0.0f;
    const size_t bytes = (kHeader + vegStart + vegFloats) * sizeof(float);
    const render::UploadSlice slice = frame.gpu.device.frameUpload(bytes);
    if (!slice) return VK_NULL_HANDLE;
    auto* dst = static_cast<float*>(slice.mapped);
    std::memcpy(dst, header, sizeof(header));
    std::memcpy(dst + kHeader, records.data(), records.size() * sizeof(float));
    std::fill(dst + kHeader + records.size(), dst + kHeader + recordFloats, 0.0f);
    if (leafSegments) {
        float* leaves = dst + kHeader + recordFloats;
        std::memcpy(leaves, leafSegments->data(), leafSegments->size() * sizeof(float));
        std::fill(leaves + leafSegments->size(), leaves + leafFloats, 0.0f);
    }
    if (vegetation) std::memcpy(dst + kHeader + vegStart, vegetation->data(), vegFloats * sizeof(float));
    VkDescriptorSet set = frame.gpu.device.frameSet(defaults_->boneLayout);
    SceneVkDescriptorWriter writer;
    writer.writeBuffer(1, slice.buffer, bytes, slice.offset, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
    writer.updateSet(device_, set);
    return set;
}

VkDescriptorSet SceneMeshDrawer::customSet(SceneFrame& frame, uint32_t nodeId, const CustomShaderState& cs,
                                           const CustomProgram& prog,
                                           std::vector<MeshNode::UserTexture>* textures) {
    std::vector<uint8_t> ubo(prog.iface.uboSize(), 0);
    for (const CustomShaderInterface::Uniform& slot : prog.iface.uniforms()) {
        for (const auto& u : cs.uniforms) {
            if (u.name != slot.name) continue;
            const int comps = std::clamp(u.comps, 1, 4);
            if (slot.offset + comps * 4 > ubo.size()) break;
            for (int c = 0; c < comps; ++c) {
                if (slot.integer) {
                    const int32_t v = static_cast<int32_t>(std::lround(u.v[c]));
                    std::memcpy(ubo.data() + slot.offset + c * 4, &v, 4);
                } else {
                    std::memcpy(ubo.data() + slot.offset + c * 4, &u.v[c], 4);
                }
            }
            break;
        }
    }
    const VkDescriptorBufferInfo uboInfo = frame.gpu.device.frameUniform(ubo.data(), ubo.size());

    SceneGpuResources& res = frame.gpu.resources;
    VkDescriptorSet set = frame.gpu.device.frameSet(defaults_->customLayout);
    SceneVkDescriptorWriter writer;
    writer.writeBuffer(0, uboInfo.buffer, uboInfo.range, uboInfo.offset);
    const auto& samplers = prog.iface.samplers();
    for (uint32_t i = 1; i <= CustomShaderInterface::kMaxSamplers; ++i) {
        const bool array = i - 1 < samplers.size() && samplers[i - 1].array;
        VkImageView view = array ? defaults_->zeroArray.view : defaults_->white.view;
        VkSampler sampler = defaults_->sampler;
        if (textures && i - 1 < samplers.size()) {
            for (auto& t : *textures) {
                if (t.name != samplers[i - 1].name || (t.layers > 0) != array) continue;
                if (const SceneVkImage* img = res.userTexture(nodeId, t)) {
                    view = img->view;
                    sampler = img->sampler;
                }
                break;
            }
        }
        writer.writeImage(i, view, sampler);
    }
    writer.writeImage(CustomShaderInterface::kMaxSamplers + 1, defaults_->zeroArray.view, defaults_->sampler);
    writer.writeImage(CustomShaderInterface::kMaxSamplers + 2, defaults_->zeroUintArray.view,
                      defaults_->nearestSampler);
    static_assert(CustomShaderInterface::kMaxSamplers + 2 == SceneDefaults::kCustomBindings);
    writer.updateSet(device_, set);
    if (textures) res.pruneUserTextures(nodeId, *textures);
    return set;
}

template <typename Node>
void SceneMeshDrawer::fillMaterial(const SceneFrame& frame, const Node& node, MeshDraw& out) {
    const float* color = node.color();
    const float* emissiveColor = node.emissiveColor();
    std::memcpy(out.push.model, frame.view.relative(node.worldMatrix()).data, sizeof(out.push.model));
    std::memcpy(out.push.baseColor, color, sizeof(out.push.baseColor));
    out.push.emissive[0] = emissiveColor[0];
    out.push.emissive[1] = emissiveColor[1];
    out.push.emissive[2] = emissiveColor[2];
    out.push.emissive[3] = node.emissive();
    out.push.pbrParams[0] = node.metallic();
    out.push.pbrParams[1] = node.roughness();
    out.push.pbrParams[2] = node.alphaCutoff();
    out.push.extra[0] = node.nearClipDist();
    out.translucent = color[3] < 1.0f;
    // Unlit meshes never cast: they draw outside the lighting a shadow
    // belongs to (a custom shader suppresses unlit, so it casts).
    out.castsShadow = node.castsShadow() && !node.effectiveUnlit();
    out.shadowMotion = frame.renderer.shadowMotion(node.id());
    out.depthBiasFactor = node.depthBiasFactor();
    out.depthBiasUnits = node.depthBiasUnits();
}

template <typename Node>
uint32_t SceneMeshDrawer::resolveShade(const Node& node, MeshDraw& out) {
    const ShadeMapProvider* provider = node.shadeMap();
    ShadeMapBinding binding{};
    if (!provider || !*provider || !(*provider)(binding) || !binding.pixels || binding.width <= 0 ||
        binding.height <= 0) {
        return 0;
    }
    out.shade = binding;
    return mesh_flags::kShadeMap;
}

namespace {

struct MaterialImages {
    const SceneVkImage* albedo = nullptr;
    VkImageView albedoView = VK_NULL_HANDLE;   // a linked scene's output
    VkSampler albedoSampler = VK_NULL_HANDLE;
    const SceneVkImage* normal = nullptr;
    const SceneVkImage* metallicRoughness = nullptr;
    const SceneVkImage* emissive = nullptr;
    const SceneVkImage* occlusion = nullptr;
};

template <typename Node>
MaterialImages materialImages(SceneGpuResources& res, const Node& node) {
    MaterialImages m;
    m.albedo = res.texture(node.id(), TextureSlot::BaseColor, node.baseColorTexture());
    m.normal = res.texture(node.id(), TextureSlot::Normal, node.normalTexture());
    m.metallicRoughness = res.texture(node.id(), TextureSlot::MetallicRoughness, node.metallicRoughnessTexture());
    m.emissive = res.texture(node.id(), TextureSlot::Emissive, node.emissiveTexture());
    m.occlusion = res.texture(node.id(), TextureSlot::Occlusion, node.occlusionTexture());
    return m;
}

/// The material set for `m` (null when the node has no texture at all), and
/// the flags saying which maps it carries.
VkDescriptorSet materialSet(SceneFrame& frame, const SceneDefaults& d, const MaterialImages& m, uint32_t& flags) {
    VkImageView albedoView = m.albedo ? m.albedo->view : m.albedoView;
    VkSampler albedoSampler = m.albedo ? m.albedo->sampler : m.albedoSampler;
    if (albedoView) flags |= mesh_flags::kAlbedoMap;
    if (m.normal) flags |= mesh_flags::kNormalMap;
    if (m.metallicRoughness) flags |= mesh_flags::kMetallicRoughnessMap;
    if (m.emissive) flags |= mesh_flags::kEmissiveMap;
    if (m.occlusion) flags |= mesh_flags::kOcclusionMap;
    if (!albedoView && !m.normal && !m.metallicRoughness && !m.emissive && !m.occlusion) return VK_NULL_HANDLE;

    VkDescriptorSet set = frame.gpu.device.frameSet(d.materialLayout);
    SceneVkDescriptorWriter writer;
    writer.writeImage(0, albedoView ? albedoView : d.white.view, albedoView ? albedoSampler : d.sampler);
    writer.writeImage(1, m.normal ? m.normal->view : d.flatNormal.view, m.normal ? m.normal->sampler : d.sampler);
    writer.writeImage(2, m.metallicRoughness ? m.metallicRoughness->view : d.white.view,
                      m.metallicRoughness ? m.metallicRoughness->sampler : d.sampler);
    writer.writeImage(3, m.emissive ? m.emissive->view : d.black.view, m.emissive ? m.emissive->sampler : d.sampler);
    writer.writeImage(4, m.occlusion ? m.occlusion->view : d.white.view,
                      m.occlusion ? m.occlusion->sampler : d.sampler);
    writer.updateSet(frame.gpu.device.device(), set);
    return set;
}

}  // namespace

bool SceneMeshDrawer::prepare(SceneFrame& frame, MeshNode& node, MeshDraw& out) {
    const bromesh::MeshData& data = node.currentMesh();
    if (data.empty()) return false;
    const uint32_t slot = &data == &node.mesh() ? 0u : static_cast<uint32_t>(node.selectedLod()) + 1u;
    const GpuMesh* gm = frame.gpu.resources.mesh(node.id(), slot, node.geometryGeneration(), data);
    if (!gm) return false;

    // A skinned node whose skin is not set up yet (no bones, or weights that
    // do not cover the mesh) draws its bind pose through the static path.
    SkinnedMeshNode* skinned = node.asSkinnedMesh();
    if (skinned && !skinned->skinReady()) skinned = nullptr;
    // Vegetation sways in the wind field; a custom shader or line drawing
    // keeps the plain path (the custom splice owns locations 7 and up).
    const bool vegetation = !skinned && !node.customShader() && node.drawMode() != MeshNode::DrawMode::Lines &&
                            gm->vegetation.buffer != VK_NULL_HANDLE;
    out.kind = skinned ? MeshKind::Skinned : vegetation ? MeshKind::Vegetation : MeshKind::Static;
    if (vegetation) out.vegetation = gm->vegetation.buffer;
    out.nodeId = node.id();
    out.vertices = gm->vertices.buffer;
    out.indices = gm->indices.buffer;
    out.indexCount = gm->indexCount;
    fillMaterial(frame, node, out);
    out.push.extra[1] = node.subsurface();
    out.push.extra[2] = node.windMask();
    out.twoSided = node.twoSided();
    if (node.drawMode() == MeshNode::DrawMode::Lines) {
        out.lines = true;
        out.lineWidth = node.lineWidth();
        out.castsShadow = false;
    }
    // A big plain mesh (a ground, a mountain ring) in chunks, each pass
    // drawing those its view reaches. Not a custom shader's: its vertex
    // chunk may move the points out of the chunks' bounds.
    if (out.kind == MeshKind::Static && !out.lines && !node.customShader()) {
        const GpuMesh* c = frame.gpu.resources.chunked(node.id(), slot, node.geometryGeneration(), data);
        if (c && !c->chunks.empty()) {
            out.chunks = &c->chunks;
            out.chunkIndices = c->chunkIndices.buffer;
        }
    }

    if (skinned) {
        out.skin = frame.gpu.resources.skinAttributes(*skinned, data.vertexCount());
        out.vertexSet = boneSet(frame, skinned->skinPalette());
        if (!out.skin) return false;
    }

    uint32_t flags = mesh_flags::vertexColor(node.vertexColorMode());
    // Only opaque draws write the SSR mask: translucents blend over it after SSR.
    if (frame.ssr && !out.translucent) flags |= mesh_flags::kReflectance;
    flags |= resolveShade(node, out);
    if (node.receivesShadow()) flags |= mesh_flags::kReceivesShadow;
    if (out.twoSided) flags |= mesh_flags::kTwoSided;
    if (data.hasTangents()) flags |= mesh_flags::kHasTangents;
    if (const CustomShaderState* cs = node.customShader()) {
        out.custom = cs;
        if (CustomProgram* prog = program(*cs, out.kind))
            out.customSet = customSet(frame, node.id(), *cs, *prog, &node.customShaderTextures());
    } else if (node.effectiveUnlit()) {
        flags |= mesh_flags::kUnlit;
    } else if (node.turfGrass() != 0u && (out.kind == MeshKind::Static || out.kind == MeshKind::Land) && !out.lines) {
        // Turf: shaded as its grass node's ground, from that node's field.
        SceneNode* g = frame.graph.findById(node.turfGrass());
        MeshNode* gm = g && g->type() == SceneNode::Type::Mesh ? static_cast<MeshNode*>(g) : nullptr;
        if (GrassNode* grass = gm ? gm->asGrass() : nullptr) {
            out.customSet = groundSet(frame, *grass);
            out.moduleSet = coverShaders(*grass).index;
            if (out.customSet) flags |= mesh_flags::kGround;
        }
    }

    MaterialImages images = materialImages(frame.gpu.resources, node);
    if (!images.albedo && node.hasExternalBaseColorTexture()) {
        // Another scene's output as the base colour, sampled in place: that
        // scene rendered earlier this frame, in queue order.
        SceneGraph* other = node.externalSceneGraph();
        if (other && other != &frame.graph) {
            const render::LayerImage img = other->renderer().outputImage();
            if (img) {
                images.albedoView = img.view;
                images.albedoSampler = img.sampler;
            }
        }
    }
    out.materialSet = materialSet(frame, *defaults_, images, flags);
    out.push.pbrParams[3] = static_cast<float>(flags);
    return true;
}

bool SceneMeshDrawer::prepareProcedural(SceneFrame& frame, InstancedMeshNode& node, MeshDraw& out) {
    float header[16] = {};
    if (node.isScatter()) {
        const size_t leaves = node.scatterInstanceCount();
        if (node.mesh().empty() || node.scatterSegCount() == 0 || leaves == 0) return false;
        // Several leaves an instance (scatterBatchMesh), in a slot of its own.
        constexpr uint32_t kScatterBatchSlot = 0x7fff;
        const GpuMesh* gm = frame.gpu.resources.mesh(node.id(), kScatterBatchSlot, node.geometryGeneration(),
                                                     node.scatterBatchMesh());
        if (!gm) return false;
        const int copies = node.scatterCopies();
        header[12] = static_cast<float>(copies);
        header[13] = static_cast<float>(node.mesh().vertexCount());
        header[14] = static_cast<float>(leaves);
        const InstancedMeshNode::ScatterParams& p = node.scatterParams();
        std::memcpy(&header[0], &p.seed, sizeof(float));
        header[1] = p.upBias;
        header[2] = p.tiltJitter;
        header[3] = p.rollJitter;
        header[4] = p.baseScale;
        header[5] = p.scaleJitter;
        header[6] = p.scaleByRadius;
        header[7] = p.refRadius;
        header[8] = p.densityFalloff;
        header[9] = static_cast<float>(node.scatterSegments().size() / 4);   // first leaf-index record
        if (p.clumps) {
            // Clump mode: the roll slot carries the shell depth.
            header[3] = p.shell;
            header[11] = 1.0f;
        }
        out.vertexSet = segmentSet(frame, header, node.scatterSegments(), &node.scatterInstanceSegments(),
                                   node.hasVegetation() ? &node.vegetation() : nullptr);
        out.kind = MeshKind::Scatter;
        out.vertices = gm->vertices.buffer;
        out.indices = gm->indices.buffer;
        out.indexCount = gm->indexCount;
        out.instanceCount = static_cast<uint32_t>((leaves + copies - 1) / copies);
    } else {
        if (node.tubeSegCount() == 0) return false;
        header[0] = static_cast<float>(node.tubeSides());
        header[1] = node.tubeRadiusScale();
        header[2] = static_cast<float>(node.tubeRecordStride());
        out.vertexSet = segmentSet(frame, header, node.tubeSegments(), nullptr,
                                   node.hasVegetation() ? &node.vegetation() : nullptr);
        out.kind = MeshKind::Tube;
        out.indexCount = static_cast<uint32_t>(node.tubeVertexCount());
    }
    if (!out.vertexSet) return false;
    out.nodeId = node.id();
    fillMaterial(frame, node, out);
    // Plain scatter leaves never cast, as on GL; swaying ones do when asked,
    // so the dappled light moves with the crown.
    if (out.kind == MeshKind::Scatter && !node.hasVegetation()) out.castsShadow = false;
    out.twoSided = node.doubleSided();
    out.push.extra[3] = static_cast<float>(std::clamp(node.atlasCols(), 1, 255)) +
                        256.0f * static_cast<float>(std::clamp(node.atlasRows(), 1, 255));

    uint32_t flags = mesh_flags::vertexColor(out.kind == MeshKind::Scatter && node.vertexColorTintEnabled() ? 1 : 0);
    // Only opaque draws write the SSR mask: translucents blend over it after SSR.
    if (frame.ssr && !out.translucent) flags |= mesh_flags::kReflectance;
    flags |= resolveShade(node, out);
    if (node.receivesShadow()) flags |= mesh_flags::kReceivesShadow;
    if (out.twoSided) flags |= mesh_flags::kTwoSided;
    if (out.kind == MeshKind::Scatter && node.mesh().hasTangents()) flags |= mesh_flags::kHasTangents;
    if (node.effectiveUnlit()) flags |= mesh_flags::kUnlit;
    out.materialSet = materialSet(frame, *defaults_, materialImages(frame.gpu.resources, node), flags);
    out.push.pbrParams[3] = static_cast<float>(flags);
    return true;
}

bool SceneMeshDrawer::prepare(SceneFrame& frame, InstancedMeshNode& node, MeshDraw& out) {
    // Scatter and tube draws build their own geometry (and ignore a custom
    // shader, as the GL renderer did).
    if (node.isScatter() || node.isTube()) return prepareProcedural(frame, node, out);

    const size_t count = node.instanceCount();
    if (node.mesh().empty() || count == 0) return false;
    // A static batch draws its merged mesh as one identity instance, white:
    // each instance's tint and atlas cell are baked into it.
    const bool batched = node.renderingBatched();
    const bromesh::MeshData& geometry = batched ? node.staticBatchMesh() : node.mesh();
    const uint64_t generation = batched ? node.staticBatchGeneration() : node.geometryGeneration();
    const GpuMesh* gm = frame.gpu.resources.mesh(node.id(), 0, generation, geometry);
    if (!gm) return false;

    // This render's instance rows, read straight from the frame's upload memory.
    static constexpr float kIdentityRow[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 1, 1, 1, 1};
    const size_t rowCount = batched ? 1 : count;
    const size_t bytes = rowCount * 16 * sizeof(float);
    const render::UploadSlice rows = frame.gpu.device.frameUpload(bytes);
    if (!rows) return false;
    std::memcpy(rows.mapped, batched ? kIdentityRow : node.instanceData().data(), bytes);

    // Per-instance vegetation records sway each instance (a static batch has
    // merged them away; a custom shader keeps the plain path).
    out.kind = MeshKind::Instanced;
    if (!batched && !node.customShader() && node.hasVegetation()) {
        const size_t vegBytes = count * 16 * sizeof(float);
        const render::UploadSlice veg = frame.gpu.device.frameUpload(vegBytes);
        if (veg) {
            std::memcpy(veg.mapped, node.vegetation().data(), vegBytes);
            out.kind = MeshKind::InstancedVegetation;
            out.vegetation = veg.buffer;
            out.vegetationOffset = veg.offset;
        }
    }
    out.nodeId = node.id();
    out.vertices = gm->vertices.buffer;
    out.indices = gm->indices.buffer;
    out.indexCount = gm->indexCount;
    out.instances = rows.buffer;
    out.instanceOffset = rows.offset;
    out.instanceCount = static_cast<uint32_t>(rowCount);
    fillMaterial(frame, node, out);
    out.twoSided = node.doubleSided();
    out.push.extra[3] = static_cast<float>(std::clamp(node.effectiveAtlasCols(), 1, 255)) +
                        256.0f * static_cast<float>(std::clamp(node.effectiveAtlasRows(), 1, 255));

    uint32_t flags = mesh_flags::vertexColor(node.useVertexColorForDraw() ? 1 : 0);
    // Only opaque draws write the SSR mask: translucents blend over it after SSR.
    if (frame.ssr && !out.translucent) flags |= mesh_flags::kReflectance;
    flags |= resolveShade(node, out);
    if (node.receivesShadow()) flags |= mesh_flags::kReceivesShadow;
    if (out.twoSided) flags |= mesh_flags::kTwoSided;
    if (geometry.hasTangents()) flags |= mesh_flags::kHasTangents;
    if (const CustomShaderState* cs = node.customShader()) {
        out.custom = cs;
        if (CustomProgram* prog = program(*cs, out.kind)) out.customSet = customSet(frame, node.id(), *cs, *prog, nullptr);
    } else if (node.effectiveUnlit()) {
        flags |= mesh_flags::kUnlit;
    }
    out.materialSet = materialSet(frame, *defaults_, materialImages(frame.gpu.resources, node), flags);
    out.push.pbrParams[3] = static_cast<float>(flags);
    return true;
}

void SceneMeshDrawer::record(VkCommandBuffer cmd, const TargetFormat& target, VkDescriptorSet cameraSet,
                             VkDescriptorSet lightingSet, const MeshDraw& draw, const ChunkCull* cull,
                             CoverPass pass) {
    VkPipeline pipeline = VK_NULL_HANDLE;
    bool custom = false;
    if (pass != CoverPass::None) {
        pipeline = builtinPipeline(draw, target, pass);
    } else if (draw.custom && draw.customSet) {
        if (CustomProgram* prog = program(*draw.custom, draw.kind)) {
            pipeline = customPipeline(*prog, draw, target);
            custom = pipeline != VK_NULL_HANDLE;
        }
    }
    if (!pipeline) pipeline = builtinPipeline(draw, target);
    if (!pipeline) return;

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    const VkDescriptorSet sets[3] = {cameraSet, draw.lightingSet ? draw.lightingSet : lightingSet,
                                     draw.materialSet ? draw.materialSet : defaults_->defaultMaterialSet};
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0, 3, sets, 0, nullptr);
    if (draw.vertexSet)
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 3, 1, &draw.vertexSet, 0, nullptr);
    const bool ground = (static_cast<uint32_t>(draw.push.pbrParams[3]) & mesh_flags::kGround) != 0u;
    if (custom || ((draw.kind == MeshKind::Grass || ground) && draw.customSet))
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 4, 1, &draw.customSet, 0, nullptr);
    vkCmdPushConstants(cmd, layout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                       sizeof(MeshPushConstants), &draw.push);
    // Polygon offset, negated so a negative bias pulls toward the camera
    // (reversed-Z: nearer is greater).
    vkCmdSetDepthBias(cmd, -draw.depthBiasUnits, 0.0f, -draw.depthBiasFactor);
    if (draw.lines && wideLines_) vkCmdSetLineWidth(cmd, draw.lineWidth);

    bindGeometryAndDraw(cmd, draw, cull);
}

void SceneMeshDrawer::bindGeometryAndDraw(VkCommandBuffer cmd, const MeshDraw& draw, const ChunkCull* cull) {
    if (draw.kind == MeshKind::Tube) {
        vkCmdDraw(cmd, draw.indexCount, 1, 0, 0);
        return;
    }
    if (draw.kind == MeshKind::Plants) {
        // An instance a chunk of a plant's elements (scene_mesh_drawer_plants.cpp):
        // the leaves indexed (each polygon point posed once), the wood not;
        // the counts the GPU plan's, in its commands.
        if (draw.indices != VK_NULL_HANDLE) {
            vkCmdBindIndexBuffer(cmd, draw.indices, 0, VK_INDEX_TYPE_UINT32);
            vkCmdDrawIndexedIndirect(cmd, draw.indirect, draw.indirectOffset, 1, 0);
        } else {
            vkCmdDrawIndirect(cmd, draw.indirect, draw.indirectOffset, 1, 0);
        }
        return;
    }
    VkBuffer buffers[3] = {draw.vertices, VK_NULL_HANDLE, VK_NULL_HANDLE};
    VkDeviceSize offsets[3] = {0, 0, 0};
    uint32_t count = 1;
    switch (draw.kind) {
    case MeshKind::Instanced:
    case MeshKind::InstancedVegetation:
        buffers[1] = draw.instances;
        offsets[1] = draw.instanceOffset;
        count = 2;
        if (draw.kind == MeshKind::InstancedVegetation) {
            buffers[2] = draw.vegetation;
            offsets[2] = draw.vegetationOffset;
            count = 3;
        }
        break;
    case MeshKind::Skinned:
        buffers[1] = draw.skin;
        count = 2;
        break;
    case MeshKind::Vegetation:
        buffers[1] = draw.vegetation;
        offsets[1] = draw.vegetationOffset;
        count = 2;
        break;
    case MeshKind::Static:
    case MeshKind::Scatter:
    case MeshKind::Tube:
    case MeshKind::Grass:
    case MeshKind::Plants:
    case MeshKind::Land: break;
    }
    vkCmdBindVertexBuffers(cmd, 0, count, buffers, offsets);
    if (draw.chunks && draw.chunkIndices != VK_NULL_HANDLE && cull && cull->frustum) {
        // The chunks the frustum reaches (nearest the origin first when
        // cull.nearFirst: what is near hides what is behind it before it
        // shades), a run of neighbours as one range.
        vkCmdBindIndexBuffer(cmd, draw.chunkIndices, 0, VK_INDEX_TYPE_UINT32);
        bromath::Mat4 model;
        std::memcpy(model.data, draw.push.model, sizeof(model.data));
        thread_local std::vector<std::pair<float, uint32_t>> seen;
        seen.clear();
        for (uint32_t i = 0; i < draw.chunks->size(); ++i) {
            // (Relative to the origin: the draw's own space.)
            const bromath::AABB3 rel = bromath::atransform((*draw.chunks)[i].bounds, model);
            const bromath::AABB3 b{rel.min + cull->origin, rel.max + cull->origin};
            if (!bromath::fintersects(*cull->frustum, b)) continue;
            const bromath::Vec3 d = bromath::vmax(bromath::vmax(rel.min, -rel.max), bromath::Vec3{0.0f, 0.0f, 0.0f});
            seen.emplace_back(cull->nearFirst ? bromath::vdot(d, d) : 0.0f, i);
        }
        if (cull->nearFirst) std::stable_sort(seen.begin(), seen.end());
        uint32_t first = 0, n = 0;
        for (const auto& [dist, i] : seen) {
            const MeshChunk& c = (*draw.chunks)[i];
            if (n && first + n == c.firstIndex) {
                n += c.indexCount;
                continue;
            }
            if (n) vkCmdDrawIndexed(cmd, n, draw.instanceCount, first, 0, 0);
            first = c.firstIndex;
            n = c.indexCount;
        }
        if (n) vkCmdDrawIndexed(cmd, n, draw.instanceCount, first, 0, 0);
        return;
    }
    vkCmdBindIndexBuffer(cmd, draw.indices, 0, VK_INDEX_TYPE_UINT32);
    if (draw.indirect != VK_NULL_HANDLE) {
        // A grass ring's draw: its instances the cover's plan's.
        vkCmdDrawIndexedIndirect(cmd, draw.indirect, draw.indirectOffset, 1, 0);
        return;
    }
    vkCmdDrawIndexed(cmd, draw.indexCount, draw.instanceCount, 0, 0, 0);
}

}  // namespace bro::scene::vk
