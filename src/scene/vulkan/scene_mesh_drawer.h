#pragma once

// Draws scene meshes — static, instanced and GPU-skinned, with the built-in
// PBR shading or a node's custom shader chunks — into whatever target the
// calling pass has open: the HDR scope (with or without the indirect-light
// attachment, at the scene's sample count) or a reflection-probe face.
//
// prepare() turns a node into a MeshDraw once per frame: geometry and
// textures from SceneGpuResources, this frame's instance data, bone palette,
// material and custom-shader sets. record() binds the pipeline for the
// target it is given and draws. Pipelines are built per (variant, target) on
// first use, so a new target format or sample count never rebuilds anything.

#include "scene/mesh_node.h"
#include "scene/vulkan/scene_vk_pipeline.h"
#include "scene/vulkan/scene_vk_target_format.h"

#include <vulkan/vulkan.h>

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace bro::scene {
class InstancedMeshNode;
}

namespace bro::scene::vk {

struct SceneFrame;
struct SceneGpu;
class SceneDefaults;

/// Per-draw push constants of every mesh pipeline (and the terrain's).
struct alignas(16) MeshPushConstants {
    float model[16];
    float baseColor[4];
    float emissive[4];    // rgb tint, a = intensity
    float pbrParams[4];   // metallic, roughness, alphaCutoff, flags
};

/// MeshPushConstants::pbrParams[3] bits (mesh.frag).
namespace mesh_flags {
constexpr uint32_t kAlbedoMap = 1;
constexpr uint32_t kNormalMap = 2;
constexpr uint32_t kMetallicRoughnessMap = 4;
constexpr uint32_t kEmissiveMap = 8;
constexpr uint32_t kUnlit = 16;
constexpr uint32_t kShadeMap = 32;
constexpr uint32_t kReflectance = 64;   // alpha carries SSR reflectance
}

enum class MeshKind : uint8_t { Static, Instanced, Skinned };

struct MeshDraw {
    MeshKind kind = MeshKind::Static;
    uint32_t nodeId = 0;
    const CustomShaderState* custom = nullptr;   // null: built-in shading

    VkBuffer vertices = VK_NULL_HANDLE;
    VkBuffer indices = VK_NULL_HANDLE;
    uint32_t indexCount = 0;
    VkBuffer instances = VK_NULL_HANDLE;         // Instanced
    VkDeviceSize instanceOffset = 0;
    uint32_t instanceCount = 1;
    VkBuffer skin = VK_NULL_HANDLE;              // Skinned
    VkDescriptorSet boneSet = VK_NULL_HANDLE;    // Skinned (set 3)
    VkDescriptorSet materialSet = VK_NULL_HANDLE;// null: the default material
    VkDescriptorSet customSet = VK_NULL_HANDLE;  // set 4 when `custom`

    MeshPushConstants push{};
    bool translucent = false;
    bool castsShadow = false;
    bool cameraCulled = false;   // outside the camera frustum this frame
    float viewDepth = 0.0f;      // along the view direction, for sorting
};

class SceneMeshDrawer {
public:
    bool setup(SceneGpu& gpu);
    void cleanup(SceneGpu& gpu);

    /// Fill `out` for this frame. False when the node has nothing to draw.
    bool prepare(SceneFrame& frame, MeshNode& node, MeshDraw& out);
    bool prepare(SceneFrame& frame, InstancedMeshNode& node, MeshDraw& out);

    /// Bind and draw into the open target described by `target`.
    void record(VkCommandBuffer cmd, const TargetFormat& target, VkDescriptorSet cameraSet,
                VkDescriptorSet lightingSet, const MeshDraw& draw);

    VkPipelineLayout layout() const { return layout_; }

    /// The vertex input of a mesh kind (binding 0 = packed vertices, binding 1
    /// = instance rows or skin attributes).
    static void vertexInput(MeshKind kind, std::vector<VkVertexInputBindingDescription>& bindings,
                            std::vector<VkVertexInputAttributeDescription>& attributes);

private:
    struct CustomProgram {
        bool failed = false;
        bool indirectFailed = false;
        std::string vertexChunk;
        std::string fragmentChunk;
        VkShaderModule vs = VK_NULL_HANDLE;
        VkShaderModule fs = VK_NULL_HANDLE;
        VkShaderModule fsIndirect = VK_NULL_HANDLE;   // compiled on first use
        std::vector<std::string> samplerNames;
        std::vector<std::pair<std::string, uint32_t>> uniformOffsets;
        uint32_t uboSize = 16;
        PipelineVariants pipelines;
    };

    static void fill(const float* color, const float* emissiveColor, float emissive, float metallic,
                     float roughness, float alphaCutoff, MeshDraw& out);
    CustomProgram* program(const CustomShaderState& cs, MeshKind kind);
    VkDescriptorSet customSet(SceneFrame& frame, uint32_t nodeId, const CustomShaderState& cs,
                              const CustomProgram& prog, std::vector<MeshNode::UserTexture>* textures);
    VkDescriptorSet boneSet(SceneFrame& frame, const std::vector<float>& palette);
    VkPipeline builtinPipeline(MeshKind kind, bool translucent, const TargetFormat& target);
    VkPipeline customPipeline(CustomProgram& prog, MeshKind kind, bool translucent, const TargetFormat& target);
    VkPipeline buildPipeline(VkShaderModule vs, VkShaderModule fs, MeshKind kind, bool translucent,
                             const TargetFormat& target);

    VkDevice device_ = VK_NULL_HANDLE;
    SceneDefaults* defaults_ = nullptr;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkShaderModule vs_[3] = {};
    VkShaderModule fs_ = VK_NULL_HANDLE;
    VkShaderModule fsIndirect_ = VK_NULL_HANDLE;
    PipelineVariants builtin_;
    std::unordered_map<std::string, std::unique_ptr<CustomProgram>> custom_;
};

}  // namespace bro::scene::vk
