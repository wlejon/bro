#include "scene/vulkan/scene_mesh_drawer.h"

#include "scene/depth_policy.h"
#include "scene/instanced_mesh_node.h"
#include "scene/scene_graph.h"
#include "scene/skinned_mesh_node.h"
#include "scene/vulkan/scene_defaults.h"
#include "scene/vulkan/scene_frame.h"
#include "scene/vulkan/scene_gpu_resources.h"
#include "scene/vulkan/scene_vk_custom_shader.h"
#include "scene/vulkan/scene_vk_device.h"
#include "scene/vulkan/scene_vk_shader_compiler.h"
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
    case MeshKind::Instanced: return SceneRenderer::CustomShaderTarget::Instanced;
    case MeshKind::Skinned: return SceneRenderer::CustomShaderTarget::Skinned;
    case MeshKind::Static:
    case MeshKind::Scatter:
    case MeshKind::Tube: break;
    }
    return SceneRenderer::CustomShaderTarget::Static;
}

}  // namespace

void SceneMeshDrawer::vertexInput(MeshKind kind, std::vector<VkVertexInputBindingDescription>& bindings,
                                  std::vector<VkVertexInputAttributeDescription>& attributes) {
    if (kind == MeshKind::Tube) {
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
    if (kind == MeshKind::Instanced) {
        bindings.push_back({1, 64, VK_VERTEX_INPUT_RATE_INSTANCE});
        attributes.push_back({8, 1, VK_FORMAT_R32G32B32A32_SFLOAT, 0});    // row 0
        attributes.push_back({9, 1, VK_FORMAT_R32G32B32A32_SFLOAT, 16});   // row 1
        attributes.push_back({10, 1, VK_FORMAT_R32G32B32A32_SFLOAT, 32});  // row 2
        attributes.push_back({11, 1, VK_FORMAT_R32G32B32A32_SFLOAT, 48});  // colour
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
    fs_ = SceneVkShaderCompiler::createBuiltinModule(device_, BuiltinSceneShader::MeshFrag);
    fsIndirect_ = SceneVkShaderCompiler::createBuiltinModule(device_, BuiltinSceneShader::MeshIndirectFrag);
    if (std::find(std::begin(vs_), std::end(vs_), VK_NULL_HANDLE) != std::end(vs_) || !fs_ || !fsIndirect_) {
        LOG_ERROR("SceneMeshDrawer: Failed creating the mesh shader modules");
        return false;
    }
    return true;
}

void SceneMeshDrawer::cleanup(SceneGpu& gpu) {
    builtin_.destroy(gpu.device);
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
    fs_ = fsIndirect_ = VK_NULL_HANDLE;
    if (layout_ != VK_NULL_HANDLE) vkDestroyPipelineLayout(device_, layout_, nullptr);
    layout_ = VK_NULL_HANDLE;
}

namespace raster {
constexpr uint32_t kTranslucent = 1;
constexpr uint32_t kTwoSided = 2;
constexpr uint32_t kLines = 4;
constexpr uint32_t kMirrored = 8;
constexpr uint32_t kNoDepth = 16;
constexpr uint32_t kCount = 32;
}

uint32_t SceneMeshDrawer::rasterVariant(const MeshDraw& draw) {
    return (draw.translucent ? raster::kTranslucent : 0u) | (draw.twoSided ? raster::kTwoSided : 0u) |
           (draw.lines ? raster::kLines : 0u) | (draw.mirrored ? raster::kMirrored : 0u) |
           (draw.noDepthTest ? raster::kNoDepth : 0u);
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
     .setDynamicStates(dynamic);
    if (rasterBits & raster::kTranslucent) {
        b.enableAlphaBlending(1);
        if (rasterBits & raster::kNoDepth) {
            b.disableDepthTest();
        } else {
            b.enableDepthTest(false);
        }
    } else {
        // Every attachment is written: the indirect-light one, when the
        // target has it, by the indirect variant of the fragment stage.
        b.disableBlending(target.colorCount).enableDepthTest(true);
    }
    return b.build(device_, layout_);
}

VkPipeline SceneMeshDrawer::builtinPipeline(const MeshDraw& draw, const TargetFormat& target) {
    const uint32_t rasterBits = rasterVariant(draw);
    const uint32_t variant = static_cast<uint32_t>(draw.kind) * raster::kCount + rasterBits;
    return builtin_.get(variant, target, [&] {
        VkShaderModule fs = target.colorCount > 1 ? fsIndirect_ : fs_;
        return buildPipeline(vs_[static_cast<int>(draw.kind)], fs, draw.kind, rasterBits, target);
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
        if (target.colorCount > 1) {
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

VkDescriptorSet SceneMeshDrawer::segmentSet(SceneFrame& frame, const float (&header)[12],
                                            const std::vector<float>& records,
                                            const std::vector<float>* leafSegments) {
    const size_t leafFloats = leafSegments ? (leafSegments->size() + 3) / 4 * 4 : 0;
    const size_t bytes = (12 + records.size() + leafFloats) * sizeof(float);
    const render::UploadSlice slice = frame.gpu.device.frameUpload(bytes);
    if (!slice) return VK_NULL_HANDLE;
    auto* dst = static_cast<float*>(slice.mapped);
    std::memcpy(dst, header, sizeof(header));
    std::memcpy(dst + 12, records.data(), records.size() * sizeof(float));
    if (leafSegments) {
        float* leaves = dst + 12 + records.size();
        std::memcpy(leaves, leafSegments->data(), leafSegments->size() * sizeof(float));
        std::fill(leaves + leafSegments->size(), leaves + leafFloats, 0.0f);
    }
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
    out.kind = skinned ? MeshKind::Skinned : MeshKind::Static;
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
    float header[12] = {};
    if (node.isScatter()) {
        const size_t leaves = node.scatterInstanceCount();
        if (node.mesh().empty() || node.scatterSegCount() == 0 || leaves == 0) return false;
        const GpuMesh* gm = frame.gpu.resources.mesh(node.id(), 0, node.geometryGeneration(), node.mesh());
        if (!gm) return false;
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
        out.vertexSet = segmentSet(frame, header, node.scatterSegments(), &node.scatterInstanceSegments());
        out.kind = MeshKind::Scatter;
        out.vertices = gm->vertices.buffer;
        out.indices = gm->indices.buffer;
        out.indexCount = gm->indexCount;
        out.instanceCount = static_cast<uint32_t>(leaves);
    } else {
        if (node.tubeSegCount() == 0) return false;
        header[0] = static_cast<float>(node.tubeSides());
        header[1] = node.tubeRadiusScale();
        out.vertexSet = segmentSet(frame, header, node.tubeSegments(), nullptr);
        out.kind = MeshKind::Tube;
        out.indexCount = static_cast<uint32_t>(node.tubeVertexCount());
    }
    if (!out.vertexSet) return false;
    out.nodeId = node.id();
    fillMaterial(frame, node, out);
    if (out.kind == MeshKind::Scatter) out.castsShadow = false;   // leaves never cast, as on GL
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

    out.kind = MeshKind::Instanced;
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
                             VkDescriptorSet lightingSet, const MeshDraw& draw) {
    VkPipeline pipeline = VK_NULL_HANDLE;
    bool custom = false;
    if (draw.custom && draw.customSet) {
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
    if (custom)
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 4, 1, &draw.customSet, 0, nullptr);
    vkCmdPushConstants(cmd, layout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                       sizeof(MeshPushConstants), &draw.push);
    // Polygon offset, oriented so a negative bias pulls toward the camera
    // under either depth policy.
    const float toward = reversedZ() ? -1.0f : 1.0f;
    vkCmdSetDepthBias(cmd, toward * draw.depthBiasUnits, 0.0f, toward * draw.depthBiasFactor);
    if (draw.lines && wideLines_) vkCmdSetLineWidth(cmd, draw.lineWidth);

    bindGeometryAndDraw(cmd, draw);
}

void SceneMeshDrawer::bindGeometryAndDraw(VkCommandBuffer cmd, const MeshDraw& draw) {
    if (draw.kind == MeshKind::Tube) {
        vkCmdDraw(cmd, draw.indexCount, 1, 0, 0);
        return;
    }
    const bool second = draw.kind == MeshKind::Instanced || draw.kind == MeshKind::Skinned;
    const VkBuffer buffers[2] = {draw.vertices, draw.kind == MeshKind::Instanced ? draw.instances : draw.skin};
    const VkDeviceSize offsets[2] = {0, draw.kind == MeshKind::Instanced ? draw.instanceOffset : 0};
    vkCmdBindVertexBuffers(cmd, 0, second ? 2 : 1, buffers, offsets);
    vkCmdBindIndexBuffer(cmd, draw.indices, 0, VK_INDEX_TYPE_UINT32);
    vkCmdDrawIndexed(cmd, draw.indexCount, draw.instanceCount, 0, 0, 0);
}

}  // namespace bro::scene::vk
