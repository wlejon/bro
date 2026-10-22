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
#include "scene/world/surface_layers.h"
#include "scene/vulkan/frame/scene_gpu_resources.h"
#include "scene/vulkan/world/scene_surface_layers.h"
#include "scene/vulkan/world/scene_world_service.h"
#include "scene/vulkan/frame/scene_vk_allocator.h"
#include "scene/vulkan/draw/scene_vk_custom_shader.h"
#include "scene/vulkan/ground/scene_cover_shaders.h"
#include "scene/vulkan/frame/scene_vk_pipeline.h"
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
class GrassNode;
class PlantsNode;
}

namespace bro::scene::vk {

struct SceneFrame;
struct SceneGpu;
class SceneDefaults;
class CoverPlan;
namespace plants {
class GpuPlanner;
}

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
constexpr uint32_t kMsaa = 32768;           // a draw into a 4x MSAA target: a cut card's cover splits samples
constexpr uint32_t kGround = 131072;        // turf: a static mesh shaded as its grass node's ground (set 4)
constexpr uint32_t kHerb = 2097152;         // a grass node's herb plants (a Grass draw): mesh_herb.vert, SCENE_HERB
constexpr uint32_t kCoverFar = 4194304;     // the cover's far tier (a Grass draw): mesh_cover_far.vert, cover_far.frag
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
/// map (set 4, scene_grass.glsl). Plants are a PlantsNode's chunks of
/// elements (no streams), planned on the GPU (scene_plants.glsl).
enum class MeshKind : uint8_t {
    Static, Instanced, Skinned, Scatter, Tube, Vegetation, InstancedVegetation, Grass, Plants,
};
constexpr int kMeshKindCount = 9;

/// The kinds the vegetation shading (mesh.frag's SCENE_VEGETATION variant) draws.
constexpr bool vegetationShading(MeshKind k) {
    return k == MeshKind::Vegetation || k == MeshKind::InstancedVegetation || k == MeshKind::Scatter ||
           k == MeshKind::Tube || k == MeshKind::Grass;
}

/// Kinds with no vertex streams: the vertex shader builds everything.
constexpr bool streamless(MeshKind k) {
    return k == MeshKind::Tube || k == MeshKind::Plants;
}

/// Kinds cut to the camera's view and level of detail this frame (a
/// reflection probe's faces would see them wrong).
constexpr bool cameraCut(MeshKind k) {
    return k == MeshKind::Grass || k == MeshKind::Plants;
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
    /// A plants draw (scene_plants_gpu_plan.cpp) or a grass ring's
    /// (scene_cover_plan.cpp): its counts and firsts are in this buffer's
    /// command at `indirectOffset` (a VkDrawIndexedIndirectCommand, or a
    /// VkDrawIndirectCommand for the plants' wood). Null for every other
    /// draw.
    VkBuffer indirect = VK_NULL_HANDLE;
    VkDeviceSize indirectOffset = 0;
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
    VkDescriptorSet customSet = VK_NULL_HANDLE;  // set 4 when `custom`; Grass: its ring and textures
    /// Grass and turf: their grass node's shaders (CoverShaders::index, 0
    /// the built-in set).
    uint32_t cover = 0;

    MeshPushConstants push{};
    bool translucent = false;
    bool castsShadow = false;
    bool shadowOnly = false;     // a caster only (a plants node's view casters): never in the colour lists
    /// The shadow plan's view it casts into alone (a plants node's planned
    /// casters), -1 for any its kind goes into: a caster that moves the
    /// overlays, one that does not the redrawn pages (ShadowViewPlan).
    int shadowView = -1;
    /// How it casts (SceneRenderer::shadowMotion): in the cached depth, in
    /// the overlays drawn over it each frame, swaying (in the overlays of
    /// the levels its sway shows in, the cached depth of the rest) or as the
    /// grass (the finest levels' overlays alone).
    ShadowMotion shadowMotion = ShadowMotion::Still;
    uint32_t shadowIndexCount = 0;   // a caster's indices, a prefix of indexCount's (0: all of them)
    bool twoSided = false;       // no back-face culling (and no front-face culling as a caster)
    bool lines = false;          // indices are line pairs; drawn unlit
    float lineWidth = 1.0f;
    bool mirrored = false;       // drawn through a y-flipped projection (probe faces): clockwise is front
    bool noDepthTest = false;    // ignores depth entirely (gizmo handles)
    float depthBiasFactor = 0.0f;   // MeshNode::setDepthBias: negative pulls toward the camera
    float depthBiasUnits = 0.0f;
    bool cameraCulled = false;   // outside the camera frustum this frame
    float viewDepth = 0.0f;      // along the view direction, for sorting
    /// Where the opaque pass draws it (stable, low first): what is near and
    /// solid first, so what is behind it fails the early depth test
    /// (kOrderMid for anything not ranked).
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

class SceneMeshDrawer final : public WorldProducer {
public:
    bool setup(SceneGpu& gpu);
    void cleanup(SceneGpu& gpu);

    /// Whether this frame has the atmosphere on: the pipelines its draws
    /// select are built with the atmosphere's shading, or without it.
    void setAtmosphere(bool on) { atmosphere_ = on; }

    /// Fill `out` for this frame. False when the node has nothing to draw.
    bool prepare(SceneFrame& frame, MeshNode& node, MeshDraw& out);
    bool prepare(SceneFrame& frame, InstancedMeshNode& node, MeshDraw& out);
    /// A grass node's draws, one per ring with visible cells, nearest first
    /// (scene_mesh_drawer_grass.cpp). Also stamps its movers for the frame.
    void prepareGrass(SceneFrame& frame, GrassNode& node, std::vector<MeshDraw>& out);
    /// A plants node's draws, its leaves and its wood for the camera and
    /// each shadow view, planned on the GPU (scene_mesh_drawer_plants.cpp).
    void preparePlants(SceneFrame& frame, PlantsNode& node, std::vector<MeshDraw>& out);
    /// The plants' plans and the leaf poses their draws read (queued by
    /// preparePlants) into the frame's compute stream (SceneFrame::
    /// computeStream), and the grass rings' (queued by prepareGrass) into
    /// its graphics stream: recorded outside any rendering, before the
    /// passes that draw them.
    void recordPlans(SceneFrame& frame);
    /// The plans recorded since the last call went to the GPU as `ticket` on
    /// `queue`: the GPU planner's read-backs of them are read once it completes.
    void plansSubmitted(uint64_t ticket, render::VulkanQueue& queue);
    /// The turf's surface layers (docs/scene-world-architecture.md §5;
    /// scene_mesh_drawer_ground.cpp), the world service's first producer:
    /// for each grass node whose turf this frame draws, the tiles the camera
    /// needs and has not got, offered as fills (offer), and the fills the
    /// service granted (its turf meshes drawn straight down into them) with
    /// the page table's changes (record). Recorded outside any rendering,
    /// after the frame's uniforms and before anything draws the turf.
    void offer(SceneFrame& frame, WorldService& service) override;
    uint32_t record(SceneFrame& frame, int kind, std::span<const WorldJob> granted, WorldStream& stream) override;

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
    /// Set 4 for a turf mesh: grass node `grass`'s field, influence map and
    /// floor, with its outermost ring (scene_mesh_drawer_grass.cpp). Null
    /// when the node has no field.
    VkDescriptorSet groundSet(SceneFrame& frame, GrassNode& grass);
    /// A grass node's turf surface layers: the tiles' CPU state and atlas.
    struct GroundLayers {
        SurfaceLayers tiles;
        SceneSurfaceLayerAtlas atlas;
        uint64_t frame = 0;
        uint64_t turfKey = 0;  // the turf meshes the tiles' items were taken from
        bromath::AABB3 bounds = bromath::aempty3();   // the items' union
        bool failed = false;   // its atlas could not be made: the turf works everything out
        /// Each turf mesh's items (by node id), shown or not, kept while it is in the graph.
        struct TurfItems {
            uint64_t key = 0;   // its geometry and placement they were taken at
            std::vector<SurfaceItem> items;
        };
        std::unordered_map<uint32_t, TurfItems> turf;
    };
    /// Grass node `grass`'s, made on first use (its atlas included).
    GroundLayers* groundLayers(SceneFrame& frame, const GrassNode& grass);
    /// Turf mesh `node`'s items (its world box `bounds`): its ground's
    /// heights in cells, retaken when its geometry or placement changes.
    static const std::vector<SurfaceItem>& turfItems(GroundLayers& gl, const MeshNode& node,
                                                     const bromath::AABB3& bounds);
    /// What every tile of a grass node's turf layer reads changes with this
    /// (its field, floor, herbs and the patterns' parameters; each turf
    /// mesh is an item of its own, offer()'s).
    static uint64_t groundGeneration(GrassNode& grass);
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
    VkPipeline buildPipeline(VkShaderModule vs, VkShaderModule fs, MeshKind kind, uint32_t raster,
                             const TargetFormat& target);

    VkDevice device_ = VK_NULL_HANDLE;
    SceneDefaults* defaults_ = nullptr;
    bool wideLines_ = false;   // the device draws lines wider than 1 px
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    bool atmosphere_ = false;   // setAtmosphere
    VkShaderModule vs_[kMeshKindCount] = {};
    VkShaderModule fs_ = VK_NULL_HANDLE;
    VkShaderModule fsIndirect_ = VK_NULL_HANDLE;
    VkShaderModule fsVeg_ = VK_NULL_HANDLE;
    VkShaderModule fsVegIndirect_ = VK_NULL_HANDLE;
    VkShaderModule fsCoverFar_ = VK_NULL_HANDLE;       // the cover's far tier: cover_far.frag
    VkShaderModule fsCoverFarIndirect_ = VK_NULL_HANDLE;
    VkShaderModule fsPlants_ = VK_NULL_HANDLE;         // plants: plants.frag (plants.vert is vs_[Plants])
    VkShaderModule fsPlantsIndirect_ = VK_NULL_HANDLE;
    VkShaderModule fsGrass_ = VK_NULL_HANDLE;          // grass blades: mesh.frag's SCENE_GRASS build
    VkShaderModule fsGrassIndirect_ = VK_NULL_HANDLE;
    VkShaderModule fsBark_ = VK_NULL_HANDLE;           // Tube: mesh.frag's SCENE_BARK build
    VkShaderModule fsBarkIndirect_ = VK_NULL_HANDLE;
    /// The shaders a grass node's cover is planned and drawn with (the
    /// blades' vertex stage, the herbs, the far tier's vertex stage, the
    /// turf and its fill): [0] the built-in set, then one a world's herb
    /// module, compiled at the first node that has it.
    std::vector<std::unique_ptr<CoverShaders>> covers_;
    std::unordered_map<uint64_t, uint32_t> coverByModule_;   // GrassNode::herbSourceHash -> index (0: failed)
    const CoverShaders& coverShaders(const GrassNode& node);
    std::unordered_map<uint32_t, std::unique_ptr<GroundLayers>> groundLayers_;   // by grass node id
    int turfKind_ = -1;   // the turf's fills, a world service job kind
    /// This frame's turf draws by grass node (offer's, for record).
    std::unordered_map<uint32_t, std::vector<const MeshDraw*>> turfDraws_;
    std::vector<uint32_t> turfOffered_;   // the grass nodes whose tiles offer() updated this frame
    PipelineVariants builtin_;
    std::unordered_map<std::string, std::unique_ptr<CustomProgram>> custom_;

    // The plants' GPU planner (scene_plants_gpu_plan.h).
    std::shared_ptr<plants::GpuPlanner> plantPlanner_;
    // The grass rings' (scene_cover_plan.h).
    std::shared_ptr<CoverPlan> coverPlan_;
};

}  // namespace bro::scene::vk
