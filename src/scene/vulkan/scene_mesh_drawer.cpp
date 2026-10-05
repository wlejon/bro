#include "scene/vulkan/scene_mesh_drawer.h"

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
#include <cstring>

namespace bro::scene::vk {

namespace {

constexpr uint32_t kBonePaletteSize = 256;

SceneRenderer::CustomShaderTarget shaderTarget(MeshKind kind) {
    switch (kind) {
    case MeshKind::Instanced: return SceneRenderer::CustomShaderTarget::Instanced;
    case MeshKind::Skinned: return SceneRenderer::CustomShaderTarget::Skinned;
    case MeshKind::Static: break;
    }
    return SceneRenderer::CustomShaderTarget::Static;
}

}  // namespace

void SceneMeshDrawer::vertexInput(MeshKind kind, std::vector<VkVertexInputBindingDescription>& bindings,
                                  std::vector<VkVertexInputAttributeDescription>& attributes) {
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
    fs_ = SceneVkShaderCompiler::createBuiltinModule(device_, BuiltinSceneShader::MeshFrag);
    fsIndirect_ = SceneVkShaderCompiler::createBuiltinModule(device_, BuiltinSceneShader::MeshIndirectFrag);
    if (!vs_[0] || !vs_[1] || !vs_[2] || !fs_ || !fsIndirect_) {
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

VkPipeline SceneMeshDrawer::buildPipeline(VkShaderModule vs, VkShaderModule fs, MeshKind kind, bool translucent,
                                          const TargetFormat& target) {
    std::vector<VkVertexInputBindingDescription> bindings;
    std::vector<VkVertexInputAttributeDescription> attributes;
    vertexInput(kind, bindings, attributes);

    SceneVkPipelineBuilder b;
    b.setShaderStages(vs, fs)
     .setVertexInput(bindings, attributes)
     .setInputTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
     .setPolygonMode(VK_POLYGON_MODE_FILL)
     .setCullMode(VK_CULL_MODE_BACK_BIT, VK_FRONT_FACE_COUNTER_CLOCKWISE)
     .setTarget(target);
    if (translucent) {
        b.enableAlphaBlending(1).enableDepthTest(false);
    } else {
        // Every attachment is written: the indirect-light one, when the
        // target has it, by the indirect variant of the fragment stage.
        b.disableBlending(target.colorCount).enableDepthTest(true);
    }
    return b.build(device_, layout_);
}

VkPipeline SceneMeshDrawer::builtinPipeline(MeshKind kind, bool translucent, const TargetFormat& target) {
    const uint32_t variant = static_cast<uint32_t>(kind) * 2 + (translucent ? 1 : 0);
    return builtin_.get(variant, target, [&] {
        VkShaderModule fs = target.colorCount > 1 ? fsIndirect_ : fs_;
        return buildPipeline(vs_[static_cast<int>(kind)], fs, kind, translucent, target);
    });
}

SceneMeshDrawer::CustomProgram* SceneMeshDrawer::program(const CustomShaderState& cs, MeshKind kind) {
    std::string key = cs.key;
    key += '\x1e';
    key += static_cast<char>('0' + static_cast<int>(kind));
    auto it = custom_.find(key);
    if (it != custom_.end()) return it->second->failed ? nullptr : it->second.get();

    auto prog = std::make_unique<CustomProgram>();
    prog->vertexChunk = cs.vertexChunk;
    prog->fragmentChunk = cs.fragmentChunk;
    std::string err;
    if (!SceneVkCustomShader::compileCustomShaderModules(device_, shaderTarget(kind), cs.vertexChunk,
                                                         cs.fragmentChunk, prog->vs, prog->fs, false,
                                                         prog->samplerNames, err)) {
        // Logged once; the node draws with the built-in shading from now on.
        LOG_ERROR("SceneMeshDrawer: Failed compiling a custom shader: %s", err.c_str());
        prog->failed = true;
        custom_.emplace(std::move(key), std::move(prog));
        return nullptr;
    }
    uint32_t size = 0;
    prog->uniformOffsets = SceneVkCustomShader::parseUniformOffsets(cs.vertexChunk + "\n" + cs.fragmentChunk, size);
    prog->uboSize = (std::max<uint32_t>(size, 16) + 15) & ~15u;
    return custom_.emplace(std::move(key), std::move(prog)).first->second.get();
}

VkPipeline SceneMeshDrawer::customPipeline(CustomProgram& prog, MeshKind kind, bool translucent,
                                           const TargetFormat& target) {
    return prog.pipelines.get(translucent ? 1 : 0, target, [&]() -> VkPipeline {
        VkShaderModule fs = prog.fs;
        if (target.colorCount > 1) {
            if (prog.indirectFailed) return VK_NULL_HANDLE;
            if (prog.fsIndirect == VK_NULL_HANDLE) {
                VkShaderModule vs = VK_NULL_HANDLE;
                std::vector<std::string> names;
                std::string err;
                // The chunks compiled once already, so this is the indirect
                // splice failing: the node keeps the built-in shading there.
                if (!SceneVkCustomShader::compileCustomShaderModules(device_, shaderTarget(kind), prog.vertexChunk,
                                                                     prog.fragmentChunk, vs, prog.fsIndirect, true,
                                                                     names, err)) {
                    LOG_ERROR("SceneMeshDrawer: Failed compiling the indirect custom variant: %s", err.c_str());
                    prog.indirectFailed = true;
                    return VK_NULL_HANDLE;
                }
                SceneVkShaderModule::destroy(device_, vs);
            }
            fs = prog.fsIndirect;
        }
        return buildPipeline(prog.vs, fs, kind, translucent, target);
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

VkDescriptorSet SceneMeshDrawer::customSet(SceneFrame& frame, uint32_t nodeId, const CustomShaderState& cs,
                                           const CustomProgram& prog,
                                           std::vector<MeshNode::UserTexture>* textures) {
    std::vector<uint8_t> ubo(prog.uboSize, 0);
    for (const auto& [name, offset] : prog.uniformOffsets) {
        for (const auto& u : cs.uniforms) {
            if (u.name != name) continue;
            const uint32_t bytes = static_cast<uint32_t>(std::clamp(u.comps, 1, 4) * sizeof(float));
            if (offset + bytes <= prog.uboSize) std::memcpy(ubo.data() + offset, u.v, bytes);
            break;
        }
    }
    const VkDescriptorBufferInfo uboInfo = frame.gpu.device.frameUniform(ubo.data(), ubo.size());

    SceneGpuResources& res = frame.gpu.resources;
    VkDescriptorSet set = frame.gpu.device.frameSet(defaults_->customLayout);
    SceneVkDescriptorWriter writer;
    writer.writeBuffer(0, uboInfo.buffer, uboInfo.range, uboInfo.offset);
    for (uint32_t i = 1; i <= 8; ++i) {
        VkImageView view = defaults_->white.view;
        VkSampler sampler = defaults_->sampler;
        if (textures && i - 1 < prog.samplerNames.size()) {
            for (auto& t : *textures) {
                if (t.name != prog.samplerNames[i - 1]) continue;
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

void SceneMeshDrawer::fill(const float* color, const float* emissiveColor, float emissive, float metallic,
                           float roughness, float alphaCutoff, MeshDraw& out) {
    std::memcpy(out.push.baseColor, color, sizeof(out.push.baseColor));
    out.push.emissive[0] = emissiveColor[0];
    out.push.emissive[1] = emissiveColor[1];
    out.push.emissive[2] = emissiveColor[2];
    out.push.emissive[3] = emissive;
    out.push.pbrParams[0] = metallic;
    out.push.pbrParams[1] = roughness;
    out.push.pbrParams[2] = alphaCutoff;
    out.translucent = color[3] < 1.0f;
}

namespace {

struct MaterialImages {
    const SceneVkImage* albedo = nullptr;
    VkImageView albedoView = VK_NULL_HANDLE;   // a linked scene's output
    VkSampler albedoSampler = VK_NULL_HANDLE;
    const SceneVkImage* normal = nullptr;
    const SceneVkImage* metallicRoughness = nullptr;
    const SceneVkImage* emissive = nullptr;
};

template <typename Node>
MaterialImages materialImages(SceneGpuResources& res, const Node& node) {
    MaterialImages m;
    m.albedo = res.texture(node.id(), TextureSlot::BaseColor, node.baseColorTexture());
    m.normal = res.texture(node.id(), TextureSlot::Normal, node.normalTexture());
    m.metallicRoughness = res.texture(node.id(), TextureSlot::MetallicRoughness, node.metallicRoughnessTexture());
    m.emissive = res.texture(node.id(), TextureSlot::Emissive, node.emissiveTexture());
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
    if (!albedoView && !m.normal && !m.metallicRoughness && !m.emissive) return VK_NULL_HANDLE;

    VkDescriptorSet set = frame.gpu.device.frameSet(d.materialLayout);
    SceneVkDescriptorWriter writer;
    writer.writeImage(0, albedoView ? albedoView : d.white.view, albedoView ? albedoSampler : d.sampler);
    writer.writeImage(1, m.normal ? m.normal->view : d.flatNormal.view, m.normal ? m.normal->sampler : d.sampler);
    writer.writeImage(2, m.metallicRoughness ? m.metallicRoughness->view : d.white.view,
                      m.metallicRoughness ? m.metallicRoughness->sampler : d.sampler);
    writer.writeImage(3, m.emissive ? m.emissive->view : d.black.view, m.emissive ? m.emissive->sampler : d.sampler);
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

    SkinnedMeshNode* skinned = node.asSkinnedMesh();
    out.kind = skinned ? MeshKind::Skinned : MeshKind::Static;
    out.nodeId = node.id();
    out.vertices = gm->vertices.buffer;
    out.indices = gm->indices.buffer;
    out.indexCount = gm->indexCount;
    out.castsShadow = node.castsShadow();
    std::memcpy(out.push.model, node.worldMatrix().data, sizeof(out.push.model));
    fill(node.color(), node.emissiveColor(), node.emissive(), node.metallic(), node.roughness(),
         node.alphaCutoff(), out);

    if (skinned) {
        out.skin = frame.gpu.resources.skinAttributes(*skinned, data.vertexCount());
        out.boneSet = boneSet(frame, skinned->skinPalette());
        if (!out.skin) return false;
    }

    uint32_t flags = 0;
    if (frame.renderer.ssrEnabled()) flags |= mesh_flags::kReflectance;
    if (node.shadeMap()) flags |= mesh_flags::kShadeMap;
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

bool SceneMeshDrawer::prepare(SceneFrame& frame, InstancedMeshNode& node, MeshDraw& out) {
    const size_t count = node.instanceCount();
    if (node.mesh().empty() || count == 0) return false;
    const GpuMesh* gm = frame.gpu.resources.mesh(node.id(), 0, node.geometryGeneration(), node.mesh());
    if (!gm) return false;

    // This render's instance rows, read straight from the frame's upload memory.
    const size_t bytes = count * 16 * sizeof(float);
    const render::UploadSlice rows = frame.gpu.device.frameUpload(bytes);
    if (!rows) return false;
    std::memcpy(rows.mapped, node.instanceData().data(), bytes);

    out.kind = MeshKind::Instanced;
    out.nodeId = node.id();
    out.vertices = gm->vertices.buffer;
    out.indices = gm->indices.buffer;
    out.indexCount = gm->indexCount;
    out.instances = rows.buffer;
    out.instanceOffset = rows.offset;
    out.instanceCount = static_cast<uint32_t>(count);
    out.castsShadow = node.castsShadow();
    std::memcpy(out.push.model, node.worldMatrix().data, sizeof(out.push.model));
    fill(node.color(), node.emissiveColor(), node.emissive(), node.metallic(), node.roughness(),
         node.alphaCutoff(), out);

    uint32_t flags = 0;
    if (frame.renderer.ssrEnabled()) flags |= mesh_flags::kReflectance;
    if (node.shadeMap()) flags |= mesh_flags::kShadeMap;
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
            pipeline = customPipeline(*prog, draw.kind, draw.translucent, target);
            custom = pipeline != VK_NULL_HANDLE;
        }
    }
    if (!pipeline) pipeline = builtinPipeline(draw.kind, draw.translucent, target);
    if (!pipeline) return;

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    const VkDescriptorSet sets[3] = {cameraSet, lightingSet,
                                     draw.materialSet ? draw.materialSet : defaults_->defaultMaterialSet};
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0, 3, sets, 0, nullptr);
    if (draw.kind == MeshKind::Skinned)
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 3, 1, &draw.boneSet, 0, nullptr);
    if (custom)
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 4, 1, &draw.customSet, 0, nullptr);
    vkCmdPushConstants(cmd, layout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                       sizeof(MeshPushConstants), &draw.push);

    const VkBuffer buffers[2] = {draw.vertices, draw.kind == MeshKind::Instanced ? draw.instances : draw.skin};
    const VkDeviceSize offsets[2] = {0, draw.kind == MeshKind::Instanced ? draw.instanceOffset : 0};
    vkCmdBindVertexBuffers(cmd, 0, draw.kind == MeshKind::Static ? 1 : 2, buffers, offsets);
    vkCmdBindIndexBuffer(cmd, draw.indices, 0, VK_INDEX_TYPE_UINT32);
    vkCmdDrawIndexed(cmd, draw.indexCount, draw.instanceCount, 0, 0, 0);
}

}  // namespace bro::scene::vk
