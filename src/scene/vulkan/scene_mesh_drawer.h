#pragma once

// Draws scene meshes — static, instanced (including static batches, foliage
// scatter and branch tubes) and GPU-skinned, with the built-in PBR shading or
// a node's custom shader chunks — into whatever target the
// calling pass has open: the HDR scope (with or without the indirect-light
// attachment, at the scene's sample count) or a reflection-probe face.
//
// prepare() turns a node into a MeshDraw once per frame: geometry and
// textures from SceneGpuResources, this frame's instance data, bone palette,
// material and custom-shader sets. record() binds the pipeline for the
// target it is given and draws. Pipelines are built per (variant, target) on
// first use, so a new target format or sample count never rebuilds anything.

#include "scene/mesh_node.h"
#include "scene/shade_map.h"

#include <bromath/aabb.h>
#include "scene/vulkan/scene_vk_custom_shader.h"
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
class GrassNode;
class ForestNode;
}

namespace bro::scene::vk {

struct SceneFrame;
struct SceneGpu;
class SceneDefaults;

/// Per-draw push constants of every mesh pipeline (and the terrain's) —
/// shaders/scene_mesh_push.glsl. Exactly the 128 bytes every device offers.
struct alignas(16) MeshPushConstants {
    float model[16];      // the node's world matrix less the frame's eye (scene_view.h)
    float baseColor[4];
    float emissive[4];    // rgb tint, a = intensity
    float pbrParams[4];   // metallic, roughness, alphaCutoff, flags
    float extra[4];       // near clip, subsurface, wind mask, atlas grid (cols + rows * 256)
};
static_assert(sizeof(MeshPushConstants) == 128);

/// MeshPushConstants::pbrParams[3] bits (scene_mesh_push.glsl).
namespace mesh_flags {
constexpr uint32_t kAlbedoMap = 1;
constexpr uint32_t kNormalMap = 2;
constexpr uint32_t kMetallicRoughnessMap = 4;
constexpr uint32_t kEmissiveMap = 8;
constexpr uint32_t kUnlit = 16;
constexpr uint32_t kShadeMap = 32;
constexpr uint32_t kReflectance = 64;   // alpha carries SSR reflectance
constexpr uint32_t kOcclusionMap = 128;
constexpr uint32_t kReceivesShadow = 256;
constexpr uint32_t kTwoSided = 512;
constexpr uint32_t kHasTangents = 4096;
constexpr uint32_t kForest = 8192;          // inInstColor.a is a forest tier's fade code
constexpr uint32_t kForestSprite = 16384;   // a forest sprite: leafy disc, sphere normal
constexpr uint32_t kForestMsaa = 32768;     // a forest draw into a 4x MSAA target: fades split samples
constexpr uint32_t kForestCluster = 65536;  // forest leaf clusters: a card of leaves drawn by mesh.frag
/// The vertex colour mode (0 none, 1 replaces the albedo, 2 tints it) in bits 10-11.
constexpr uint32_t vertexColor(int mode) { return static_cast<uint32_t>(mode & 3) << 10; }
}

/// Scatter: the leaf mesh, one instance per leaf placed by the vertex shader
/// from segment records. Tube: no vertex streams at all, the walls built from
/// the vertex index and segment records (scene_procedural.glsl). Vegetation
/// and InstancedVegetation are Static and Instanced carrying wind-sway
/// records (scene_vegetation.glsl): a per-vertex or per-instance stream at
/// locations 12-15. Scatter and Tube sway from records in their segment set.
/// Grass is a blade mesh drawn once per cell of a ring around the camera,
/// placed and bent by the vertex shader from the node's field and influence
/// map (set 4, scene_grass.glsl). The Forest kinds are a ForestNode's tiers
/// (scene_forest.glsl): leaves (the leaf mesh, copies an instance), bark
/// (no streams) and sprites (no streams), each instance or sprite one of
/// this frame's draw entries (set 3 binding 2). A sprite (a clump's lobe or
/// a crown) draws with its own build of mesh.frag (SCENE_FOREST_SPRITE),
/// which places its leaves and writes their depth.
enum class MeshKind : uint8_t {
    Static, Instanced, Skinned, Scatter, Tube, Vegetation, InstancedVegetation, Grass,
    ForestLeaf, ForestTube, ForestSprite,
};
constexpr int kMeshKindCount = 11;

/// The kinds the vegetation shading (mesh.frag's SCENE_VEGETATION variant) draws.
constexpr bool vegetationShading(MeshKind k) {
    return k == MeshKind::Vegetation || k == MeshKind::InstancedVegetation || k == MeshKind::Scatter ||
           k == MeshKind::Tube || k == MeshKind::Grass || k == MeshKind::ForestLeaf ||
           k == MeshKind::ForestTube || k == MeshKind::ForestSprite;
}

/// Kinds with no vertex streams: the vertex shader builds everything.
constexpr bool streamless(MeshKind k) {
    return k == MeshKind::Tube || k == MeshKind::ForestTube || k == MeshKind::ForestSprite;
}

/// Kinds cut to the camera's view and level of detail this frame (a
/// reflection probe's faces would see them wrong).
constexpr bool cameraCut(MeshKind k) {
    return k == MeshKind::Grass || k == MeshKind::ForestLeaf || k == MeshKind::ForestTube ||
           k == MeshKind::ForestSprite;
}

struct MeshDraw {
    /// Move the draw to positions relative to an eye `offset` from the one it
    /// was prepared for (push.model is camera-relative).
    void rebase(const bromath::Vec3& offset) {
        push.model[12] += offset.x;
        push.model[13] += offset.y;
        push.model[14] += offset.z;
    }

    MeshKind kind = MeshKind::Static;
    uint32_t nodeId = 0;
    const CustomShaderState* custom = nullptr;   // null: built-in shading

    VkBuffer vertices = VK_NULL_HANDLE;
    VkBuffer indices = VK_NULL_HANDLE;
    uint32_t indexCount = 0;   // Tube, ForestTube, ForestSprite: the vertex count of the unindexed draw
    uint32_t firstInstance = 0;  // Forest kinds: the first draw entry (instanced)
    uint32_t firstVertex = 0;    // ForestSprite crowns: the first entry times 6 (not instanced)
    VkBuffer instances = VK_NULL_HANDLE;         // Instanced, InstancedVegetation
    VkDeviceSize instanceOffset = 0;
    VkBuffer vegetation = VK_NULL_HANDLE;        // Vegetation (per vertex), InstancedVegetation (per instance)
    VkDeviceSize vegetationOffset = 0;
    uint32_t instanceCount = 1;
    VkBuffer skin = VK_NULL_HANDLE;              // Skinned
    VkDescriptorSet vertexSet = VK_NULL_HANDLE;  // set 3: Skinned bones, Scatter/Tube segments
    VkDescriptorSet materialSet = VK_NULL_HANDLE;// null: the default material
    VkDescriptorSet customSet = VK_NULL_HANDLE;  // set 4 when `custom`; Grass: its ring and textures

    MeshPushConstants push{};
    bool translucent = false;
    bool castsShadow = false;
    bool shadowOnly = false;     // a caster only (a forest's shadow tiers): never in the colour lists
    uint32_t shadowTileMask = ~0u;   // the shadow plan's tiles it casts into (bit per slot)
    bool twoSided = false;       // no back-face culling (and no front-face culling as a caster)
    bool lines = false;          // indices are line pairs; drawn unlit
    float lineWidth = 1.0f;
    bool mirrored = false;       // drawn through a y-flipped projection (probe faces): clockwise is front
    bool noDepthTest = false;    // ignores depth entirely (gizmo handles)
    float depthBiasFactor = 0.0f;   // MeshNode::setDepthBias: negative pulls toward the camera
    float depthBiasUnits = 0.0f;
    bool cameraCulled = false;   // outside the camera frustum this frame
    float viewDepth = 0.0f;      // along the view direction, for sorting
    bool hasBounds = false;      // world bounds, for the shadow tiles' and probe faces' culling
    bromath::AABB3 bounds;
    bromath::Vec3 center{0.0f, 0.0f, 0.0f};   // bounds centre (else the origin): probe selection
    /// The tile shade map the draw samples (kShadeMap set), resolved from
    /// its node's provider this frame.
    ShadeMapBinding shade;
    /// The draw's own lighting set — its shade map and/or reflection probe
    /// (lightingSetFor) — null for the frame's.
    VkDescriptorSet lightingSet = VK_NULL_HANDLE;
};

class SceneMeshDrawer {
public:
    bool setup(SceneGpu& gpu);
    void cleanup(SceneGpu& gpu);

    /// Fill `out` for this frame. False when the node has nothing to draw.
    bool prepare(SceneFrame& frame, MeshNode& node, MeshDraw& out);
    bool prepare(SceneFrame& frame, InstancedMeshNode& node, MeshDraw& out);
    /// A grass node's draws, one per ring with visible cells, nearest first
    /// (scene_mesh_drawer_grass.cpp). Also stamps its movers for the frame.
    void prepareGrass(SceneFrame& frame, GrassNode& node, std::vector<MeshDraw>& out);
    /// A forest node's draws (scene_mesh_drawer_forest.cpp): a draw per tier
    /// and shape for the camera, and the shadow tiers per shadow tile
    /// (shadowOnly, each with its tile's bit).
    void prepareForest(SceneFrame& frame, ForestNode& node, std::vector<MeshDraw>& out);

    /// Bind and draw into the open target described by `target`, lit by
    /// the draw's probe lighting set when it has one, else `lightingSet`.
    void record(VkCommandBuffer cmd, const TargetFormat& target, VkDescriptorSet cameraSet,
                VkDescriptorSet lightingSet, const MeshDraw& draw);

    VkPipelineLayout layout() const { return layout_; }

    /// Bind a draw's vertex streams (none for a tube) and issue it: shared
    /// with the shadow casters, whose pipelines read the same streams.
    static void bindGeometryAndDraw(VkCommandBuffer cmd, const MeshDraw& draw);

    /// The vertex input of a mesh kind (binding 0 = packed vertices, binding 1
    /// = instance rows, skin attributes or per-vertex vegetation, binding 2 =
    /// per-instance vegetation).
    static void vertexInput(MeshKind kind, std::vector<VkVertexInputBindingDescription>& bindings,
                            std::vector<VkVertexInputAttributeDescription>& attributes);

private:
    struct CustomProgram {
        bool failed = false;
        bool indirectFailed = false;
        CustomShaderInterface iface;
        VkShaderModule vs = VK_NULL_HANDLE;
        VkShaderModule fs = VK_NULL_HANDLE;
        VkShaderModule fsIndirect = VK_NULL_HANDLE;   // compiled on first use
        PipelineVariants pipelines;
    };

    template <typename Node>
    static void fillMaterial(const SceneFrame& frame, const Node& node, MeshDraw& out);
    /// kShadeMap when the node's shade map resolves this frame (into out.shade).
    template <typename Node>
    static uint32_t resolveShade(const Node& node, MeshDraw& out);
    CustomProgram* program(const CustomShaderState& cs, MeshKind kind);
    VkDescriptorSet customSet(SceneFrame& frame, uint32_t nodeId, const CustomShaderState& cs,
                              const CustomProgram& prog, std::vector<MeshNode::UserTexture>* textures);
    VkDescriptorSet boneSet(SceneFrame& frame, const std::vector<float>& palette);
    /// Set 3 holding a procedural draw's header and records, then its
    /// vegetation records (4 vec4s a segment; header[10] says where); null
    /// when the frame's upload memory runs out.
    VkDescriptorSet segmentSet(SceneFrame& frame, float (&header)[16], const std::vector<float>& records,
                               const std::vector<float>* leafSegments, const std::vector<float>* vegetation);
    bool prepareProcedural(SceneFrame& frame, InstancedMeshNode& node, MeshDraw& out);
    /// The pipeline state a draw selects beyond its shaders.
    static uint32_t rasterVariant(const MeshDraw& draw);
    VkPipeline builtinPipeline(const MeshDraw& draw, const TargetFormat& target);
    VkPipeline customPipeline(CustomProgram& prog, const MeshDraw& draw, const TargetFormat& target);
    VkPipeline buildPipeline(VkShaderModule vs, VkShaderModule fs, MeshKind kind, uint32_t raster,
                             const TargetFormat& target);

    VkDevice device_ = VK_NULL_HANDLE;
    SceneDefaults* defaults_ = nullptr;
    bool wideLines_ = false;   // the device draws lines wider than 1 px
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkShaderModule vs_[kMeshKindCount] = {};
    VkShaderModule fs_ = VK_NULL_HANDLE;
    VkShaderModule fsIndirect_ = VK_NULL_HANDLE;
    VkShaderModule fsVeg_ = VK_NULL_HANDLE;
    VkShaderModule fsVegIndirect_ = VK_NULL_HANDLE;
    VkShaderModule fsSprite_ = VK_NULL_HANDLE;
    VkShaderModule fsCluster_ = VK_NULL_HANDLE;
    VkShaderModule fsClusterIndirect_ = VK_NULL_HANDLE;
    VkShaderModule fsBark_ = VK_NULL_HANDLE;           // Tube, ForestTube: mesh.frag's SCENE_BARK build
    VkShaderModule fsBarkIndirect_ = VK_NULL_HANDLE;
    VkShaderModule fsSpriteIndirect_ = VK_NULL_HANDLE;
    PipelineVariants builtin_;
    std::unordered_map<std::string, std::unique_ptr<CustomProgram>> custom_;
};

}  // namespace bro::scene::vk
