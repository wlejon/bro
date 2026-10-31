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

#include "scene/nodes/mesh_node.h"
#include "scene/world/shade_map.h"

#include <bromath/aabb.h>
#include <bromath/frustum.h>
#include "render/vulkan_frames.h"
#include "scene/vulkan/frame/scene_gpu_resources.h"
#include "scene/vulkan/frame/scene_vk_pipeline.h"
#include "scene/vulkan/frame/scene_vk_allocator.h"
#include "scene/vulkan/draw/scene_vk_custom_shader.h"
#include "scene/vulkan/frame/scene_vk_target_format.h"

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

/// Per-draw push constants of every mesh pipeline (and the terrain's) —
/// shaders/common/scene_mesh_push.glsl. Exactly the 128 bytes every device offers.
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
/// The vertex colour mode (0 none, 1 replaces the albedo, 2 tints it) in bits 10-11.
constexpr uint32_t vertexColor(int mode) { return static_cast<uint32_t>(mode & 3) << 10; }
}

/// Scatter: the leaf mesh, one instance per leaf placed by the vertex shader
/// from segment records. Tube: no vertex streams at all, the walls built from
/// the vertex index and segment records (scene_procedural.glsl). Vegetation
/// and InstancedVegetation are Static and Instanced carrying wind-sway
/// records (scene_vegetation.glsl): a per-vertex or per-instance stream at
/// locations 12-15. Scatter and Tube sway from records in their segment set.
enum class MeshKind : uint8_t {
    Static, Instanced, Skinned, Scatter, Tube, Vegetation, InstancedVegetation,
};
constexpr int kMeshKindCount = 7;

/// The kinds the vegetation shading (mesh.frag's SCENE_VEGETATION variant) draws.
constexpr bool vegetationShading(MeshKind k) {
    return k == MeshKind::Vegetation || k == MeshKind::InstancedVegetation || k == MeshKind::Scatter ||
           k == MeshKind::Tube;
}

/// Kinds with no vertex streams: the vertex shader builds everything.
constexpr bool streamless(MeshKind k) {
    return k == MeshKind::Tube;
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
    uint32_t indexCount = 0;   // Tube: the vertex count of the unindexed draw
    VkBuffer instances = VK_NULL_HANDLE;         // Instanced, InstancedVegetation
    VkDeviceSize instanceOffset = 0;
    VkBuffer vegetation = VK_NULL_HANDLE;        // Vegetation (per vertex), InstancedVegetation (per instance)
    VkDeviceSize vegetationOffset = 0;
    uint32_t instanceCount = 1;
    /// A big static mesh's chunks (GpuMesh::chunks) and their index buffer:
    /// a pass given a ChunkCull draws the chunks its frustum reaches, else
    /// `indices` whole. Null: not chunked.
    const std::vector<MeshChunk>* chunks = nullptr;
    VkBuffer chunkIndices = VK_NULL_HANDLE;
    VkBuffer skin = VK_NULL_HANDLE;              // Skinned
    VkDescriptorSet vertexSet = VK_NULL_HANDLE;  // set 3: Skinned bones, Scatter/Tube segments
    VkDescriptorSet materialSet = VK_NULL_HANDLE;// null: the default material
    VkDescriptorSet customSet = VK_NULL_HANDLE;  // set 4 when `custom`

    MeshPushConstants push{};
    bool translucent = false;
    bool castsShadow = false;
    /// How it casts (SceneRenderer::shadowMotion): in the cached depth, in
    /// the overlays drawn over it each frame, or swaying (in the overlays of
    /// the levels its sway shows in, the cached depth of the rest).
    ShadowMotion shadowMotion = ShadowMotion::Still;
    uint32_t shadowIndexCount = 0;   // a caster's indices, a prefix of indexCount's (0: all of them)
    bool twoSided = false;       // no back-face culling (and no front-face culling as a caster)
    bool lines = false;          // indices are line pairs; drawn unlit
    float lineWidth = 1.0f;
    bool mirrored = false;       // drawn through a y-flipped projection (probe faces): clockwise is front
    bool noDepthTest = false;    // ignores depth entirely (gizmo handles)
    /// A volume (MeshNode::isVolume): only `custom`, `customSet` and
    /// `nodeId` are set, and only recordVolume draws it.
    bool volume = false;
    float depthBiasFactor = 0.0f;   // MeshNode::setDepthBias: negative pulls toward the camera
    float depthBiasUnits = 0.0f;
    bool cameraCulled = false;   // outside the camera frustum this frame
    float viewDepth = 0.0f;      // along the view direction, for sorting
    /// Where the opaque pass draws it (stable, low first): what is near and
    /// solid first, so what is behind it fails the early depth test
    /// (SceneNode::drawOrder, kOrderMid unless set).
    static constexpr uint8_t kOrderMid = 10;
    uint8_t order = kOrderMid;
    /// What the timed passes call it (a static string; null: its kind's name).
    const char* timerLabel = nullptr;
    /// The shadow pass's, for a caster it times on its own (null: untimed,
    /// its time going to the next one timed).
    const char* shadowTimerLabel = nullptr;
    bool hasBounds = false;     // world bounds, for the shadow views' and probe faces' culling
    bromath::AABB3 bounds;
    bromath::Vec3 center{0.0f, 0.0f, 0.0f};   // bounds centre (else the origin): probe selection
    /// The tile shade map the draw samples (kShadeMap set), resolved from
    /// its node's provider this frame.
    ShadeMapBinding shade;
    /// The draw's own lighting set — its shade map and/or reflection probe
    /// (lightingSetFor) — null for the frame's.
    VkDescriptorSet lightingSet = VK_NULL_HANDLE;
};

class SceneMeshDrawer final {
public:
    bool setup(SceneGpu& gpu);
    void cleanup(SceneGpu& gpu);

    /// Whether this frame has the atmosphere on: the pipelines its draws
    /// select are built with the atmosphere's shading, or without it.
    void setAtmosphere(bool on) { atmosphere_ = on; }

    /// Fill `out` for this frame. False when the node has nothing to draw.
    bool prepare(SceneFrame& frame, MeshNode& node, MeshDraw& out);
    bool prepare(SceneFrame& frame, InstancedMeshNode& node, MeshDraw& out);

    /// A pass's view for a chunked draw (MeshDraw::chunks): its frustum
    /// (absolute world) and the origin its draws' push.model is relative to
    /// (the eye they were made for, or a probe's after MeshDraw::rebase).
    struct ChunkCull {
        const bromath::Frustum* frustum = nullptr;
        bromath::Vec3 origin{0.0f, 0.0f, 0.0f};
        bool nearFirst = false;   // a view's chunks nearest the origin first (a camera's, a probe face's)
    };

    /// Bind and draw into the open target described by `target`, lit by
    /// the draw's probe lighting set when it has one, else `lightingSet`;
    /// a chunked draw cut to `cull` when given.
    void record(VkCommandBuffer cmd, const TargetFormat& target, VkDescriptorSet cameraSet,
                VkDescriptorSet lightingSet, const MeshDraw& draw, const ChunkCull* cull = nullptr);

    VkPipelineLayout layout() const { return layout_; }

    /// Draw a volume (MeshDraw::volume) into the open volume target
    /// (PassVolume: light over transmittance, then the medium's depth): its
    /// triangle over the view, blended as dst * transmittance + its light
    /// (volume.frag), reading the opaque depth from `depthSet` (the
    /// material layout, the depth at binding 0, the exposure meter's slots
    /// at 5); `index` its place among the frame's volumes, its meter slot.
    void recordVolume(VkCommandBuffer cmd, const TargetFormat& target, VkDescriptorSet cameraSet,
                      VkDescriptorSet lightingSet, VkDescriptorSet depthSet, const MeshDraw& draw,
                      uint32_t index);

    /// Whether a volume's chunk gives its near medium point by point
    /// (userMedium, volume.frag), so it has a near grid.
    bool volumeHasMedium(const MeshDraw& draw);
    /// Integrate a volume's userMedium over its near grid, in an open pass
    /// with no attachments whose viewport is the grid's cells: `nearSet`
    /// (volumeNearSetLayout) holds the grid's light and depth images, in
    /// GENERAL.
    void recordVolumeNear(VkCommandBuffer cmd, VkDescriptorSet cameraSet, VkDescriptorSet lightingSet,
                          VkDescriptorSet nearSet, const MeshDraw& draw, uint32_t index);
    VkDescriptorSetLayout volumeNearSetLayout() const { return nearSetLayout_; }

    /// Bind a draw's vertex streams (none for a tube) and issue it: shared
    /// with the shadow casters, whose pipelines read the same streams. A
    /// chunked draw given `cull` draws its chunks inside cull's frustum,
    /// neighbouring ones as one range.
    static void bindGeometryAndDraw(VkCommandBuffer cmd, const MeshDraw& draw, const ChunkCull* cull = nullptr);

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
        // A volume whose chunk gives a near medium: its near pass's stage.
        bool medium = false;
        VkShaderModule fsNear = VK_NULL_HANDLE;
        PipelineVariants nearPipelines;
        // A brokernel hook's stages' entry points ("main" for GLSL).
        std::string fsEntry = "main";
        std::string fsNearEntry = "main";
    };

    template <typename Node>
    static void fillMaterial(const SceneFrame& frame, const Node& node, MeshDraw& out);
    /// kShadeMap when the node's shade map resolves this frame (into out.shade).
    template <typename Node>
    static uint32_t resolveShade(const Node& node, MeshDraw& out);
    CustomProgram* program(const CustomShaderState& cs, MeshKind kind);
    /// A volume's: `fs` its volume.frag build, `vs` unset (vsVolume_).
    CustomProgram* volumeProgram(const CustomShaderState& cs);
    VkDescriptorSet customSet(SceneFrame& frame, uint32_t nodeId, const CustomShaderState& cs,
                              const CustomProgram& prog, std::vector<MeshNode::UserTexture>* textures,
                              const std::vector<CustomShaderBuffer>& buffers);
    /// What an unbound `buffer` declaration reads: zeroes.
    uint64_t zeroBufferAddress(SceneFrame& frame);
    VkDescriptorSet boneSet(SceneFrame& frame, const std::vector<float>& palette);
    /// Set 3 holding a procedural draw's header and records, then its
    /// vegetation records (4 vec4s a segment; header[10] says where); null
    /// when the frame's upload memory runs out.
    VkDescriptorSet segmentSet(SceneFrame& frame, float (&header)[16], const std::vector<float>& records,
                               const std::vector<float>* leafSegments, const std::vector<float>* vegetation);
    bool prepareProcedural(SceneFrame& frame, InstancedMeshNode& node, MeshDraw& out);
    /// The pipeline state a draw selects beyond its shaders.
    uint32_t rasterVariant(const MeshDraw& draw) const;
    VkPipeline builtinPipeline(const MeshDraw& draw, const TargetFormat& target);
    VkPipeline customPipeline(CustomProgram& prog, const MeshDraw& draw, const TargetFormat& target);
    /// `streams` false: no vertex input, whatever `kind` (a vertex stage that
    /// reads only gl_VertexIndex). `what` names it in the pipeline statistics.
    VkPipeline buildPipeline(VkShaderModule vs, VkShaderModule fs, MeshKind kind, uint32_t raster,
                             const TargetFormat& target, const char* what, bool streams = true);

    VkDevice device_ = VK_NULL_HANDLE;
    SceneDefaults* defaults_ = nullptr;
    bool wideLines_ = false;   // the device draws lines wider than 1 px
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    // A volume's near pass: layout_ with the near grid's set in the
    // material set's place.
    VkDescriptorSetLayout nearSetLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout nearLayout_ = VK_NULL_HANDLE;
    bool atmosphere_ = false;   // setAtmosphere
    VkShaderModule vs_[kMeshKindCount] = {};
    VkShaderModule fs_ = VK_NULL_HANDLE;
    VkShaderModule fsIndirect_ = VK_NULL_HANDLE;
    VkShaderModule fsVeg_ = VK_NULL_HANDLE;
    VkShaderModule fsVegIndirect_ = VK_NULL_HANDLE;
    VkShaderModule fsBark_ = VK_NULL_HANDLE;           // Tube: mesh.frag's SCENE_BARK build
    VkShaderModule fsBarkIndirect_ = VK_NULL_HANDLE;
    VkShaderModule vsVolume_ = VK_NULL_HANDLE;
    PipelineVariants builtin_;
    std::unordered_map<std::string, std::unique_ptr<CustomProgram>> custom_;
    std::shared_ptr<render::ComputeBuffer> zeroBuffer_;
};

}  // namespace bro::scene::vk
