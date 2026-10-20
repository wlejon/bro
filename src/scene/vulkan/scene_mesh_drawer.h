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
#include <bromath/frustum.h>
#include "render/vulkan_frames.h"
#include "scene/surface_layers.h"
#include "scene/vulkan/scene_gpu_resources.h"
#include "scene/vulkan/scene_surface_layers.h"
#include "scene/vulkan/scene_vk_allocator.h"
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
class PlantsNode;
}

namespace bro::scene::vk {

struct SceneFrame;
struct SceneGpu;
class SceneDefaults;
namespace forest {
class GpuPlanner;
}
namespace plants {
class GpuPlanner;
}

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
constexpr uint32_t kGround = 131072;        // turf: a static mesh shaded as its grass node's ground (set 4)
constexpr uint32_t kForestSpray = 262144;   // a needle or scale-leaf forest's leaves: mesh.frag's SCENE_FOREST_SPRAY build
constexpr uint32_t kForestBloom = 524288;   // a forest's blooms (a ForestSprite draw): mesh_forest_bloom.vert, SCENE_FOREST_BLOOM
constexpr uint32_t kForestLeafCut = 1048576;   // a broad forest leaf's outline cut per pixel (mesh.frag)
constexpr uint32_t kHerb = 2097152;            // a grass node's herb plants (a Grass draw): mesh_herb.vert, SCENE_HERB
constexpr uint32_t kForestLeafInline = 4194304;   // forest leaves with no pose slot: mesh_forest_leaf.vert's FOREST_LEAF_INLINE
constexpr uint32_t kForestReveal = 8388608;       // a forest's leaves or clumps cut to their side of the near sphere (emissive.xy)
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
/// which places its leaves and writes their depth. A forest's blooms are a
/// ForestSprite draw flagged mesh_flags::kForestBloom, which takes its own
/// vertex stage (mesh_forest_bloom.vert) and mesh.frag build
/// (SCENE_FOREST_BLOOM).
enum class MeshKind : uint8_t {
    Static, Instanced, Skinned, Scatter, Tube, Vegetation, InstancedVegetation, Grass,
    ForestLeaf, ForestTube, ForestSprite, Plants,
};
constexpr int kMeshKindCount = 12;

/// The kinds the vegetation shading (mesh.frag's SCENE_VEGETATION variant) draws.
constexpr bool vegetationShading(MeshKind k) {
    return k == MeshKind::Vegetation || k == MeshKind::InstancedVegetation || k == MeshKind::Scatter ||
           k == MeshKind::Tube || k == MeshKind::Grass || k == MeshKind::ForestLeaf ||
           k == MeshKind::ForestTube || k == MeshKind::ForestSprite;
}

/// Kinds with no vertex streams: the vertex shader builds everything.
constexpr bool streamless(MeshKind k) {
    return k == MeshKind::Tube || k == MeshKind::ForestTube || k == MeshKind::ForestSprite || k == MeshKind::Plants;
}

/// Kinds cut to the camera's view and level of detail this frame (a
/// reflection probe's faces would see them wrong).
constexpr bool cameraCut(MeshKind k) {
    return k == MeshKind::Grass || k == MeshKind::ForestLeaf || k == MeshKind::ForestTube ||
           k == MeshKind::ForestSprite || k == MeshKind::Plants;
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
    uint32_t indexCount = 0;   // Tube, ForestSprite: the vertex count of the unindexed draw (ForestTube: indexed)
    uint32_t firstInstance = 0;  // Forest kinds: the first draw entry (instanced)
    uint32_t firstVertex = 0;    // ForestSprite crowns: the first entry times 6 (not instanced)
    /// A forest draw the GPU planned (scene_forest_gpu_plan.cpp): its counts
    /// and firsts are in this buffer's command at `indirectOffset` (a
    /// VkDrawIndexedIndirectCommand, or a VkDrawIndirectCommand for the
    /// unindexed kinds), not in the fields above, which then hold 1 instance
    /// and (crowns) one entry's vertices. Null: drawn from the fields.
    VkBuffer indirect = VK_NULL_HANDLE;
    VkDeviceSize indirectOffset = 0;
    /// Its entries (nearest first) split in `indirectBatches` sub-ranges
    /// (0: none; else 3, a camera's clumps and crowns): batch b is the
    /// command at indirectOffset + (b + 1) * indirectBatchStride, entries
    /// [floor(n * f_b), floor(n * f_(b+1))) of the group's n, f = 0, 0.05,
    /// 0.25, 1 (FP_BATCH_END_* in scene_forest_plan_layout.h). A pass that
    /// batches draws batch b by drawing a copy with indirectOffset moved
    /// there and indirectBatches 0; the whole-range command stays at
    /// indirectOffset.
    uint8_t indirectBatches = 0;
    uint32_t indirectBatchStride = 0;
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

    MeshPushConstants push{};
    bool translucent = false;
    bool castsShadow = false;
    bool shadowOnly = false;     // a caster only (a forest's shadow tiers): never in the colour lists
    uint32_t shadowTileMask = ~0u;   // the shadow plan's tiles it casts into (bit per slot)
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
    /// solid first, so the far tiers behind it fail the early depth test
    /// (kOrderMid for anything not ranked).
    static constexpr uint8_t kOrderMid = 10;
    static constexpr uint8_t kOrderFar = 20;   // a forest's clumps and crowns
    uint8_t order = kOrderMid;
    /// What the timed passes call it (a static string; null: its kind's name).
    const char* timerLabel = nullptr;
    /// The shadow pass's, for a caster it times on its own (null: untimed,
    /// its time going to the next one timed).
    const char* shadowTimerLabel = nullptr;
    bool hasBounds = false;     // world bounds, for the shadow tiles' and probe faces' culling
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
    /// A plants node's draws, its leaves and its wood for the camera and
    /// each shadow tile, planned on the GPU (scene_mesh_drawer_plants.cpp).
    void preparePlants(SceneFrame& frame, PlantsNode& node, std::vector<MeshDraw>& out);
    /// A forest node's draws (scene_mesh_drawer_forest.cpp): a draw per tier
    /// and shape for the camera, and the shadow tiers per shadow tile
    /// (shadowOnly, each with its tile's bit).
    void prepareForest(SceneFrame& frame, ForestNode& node, std::vector<MeshDraw>& out);
    /// Several forest nodes' draws under one budget: the trees of every node
    /// thin alike (one detail factor for all, fitting the largest of their
    /// budgets), as one node's trees do. `out[i]` is `nodes[i]`'s.
    void prepareForests(SceneFrame& frame, const std::vector<ForestNode*>& nodes,
                        std::vector<std::vector<MeshDraw>>& out);
    /// Pose the leaves this frame's forest draws keep (forest_leaf_pose.comp,
    /// a leaf at a time; their vertices read the poses): recorded outside any
    /// rendering, after the forests are prepared and before anything draws
    /// them. The GPU planner's plans and poses go into the frame's compute
    /// stream (SceneFrame::computeStream), the CPU's poses into frame.cmd.
    void recordForestPoses(SceneFrame& frame);
    /// The plants' plans and the leaves their draws read (plants_pose.comp,
    /// queued by preparePlants), before the passes that draw them: the GPU
    /// planner's into the frame's compute stream, the CPU's poses into
    /// frame.cmd.
    void recordPlantPoses(SceneFrame& frame);
    /// The plans recorded since the last call went to the GPU as `ticket` on
    /// `queue`: the GPU planners' read-backs of them are read once it completes.
    void plansSubmitted(uint64_t ticket, render::VulkanQueue& queue);
    /// The turf's surface layers (docs/scene-world-architecture.md §5;
    /// scene_mesh_drawer_ground.cpp): for each grass node whose turf this
    /// frame draws, the tiles the camera needs, the fills due this frame
    /// (its turf meshes drawn straight down into them) and the page table's
    /// changes. Recorded outside any rendering, after the frame's uniforms
    /// and before anything draws the turf.
    void recordGroundLayers(SceneFrame& frame);

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
    // The forest path's phases (scene_mesh_drawer_forest.cpp): which trees
    // a node shows the camera and the shadow tiles, then, at a detail
    // factor, its draws.
    struct ForestPlan;
    std::unique_ptr<ForestPlan> planForest(SceneFrame& frame, ForestNode& node);
    // Last frame's plans, whose buffers the next frame's take (ForestPlan::reuse).
    std::vector<std::shared_ptr<ForestPlan>> sparePlans_;
    // A node's trees' world bounds and the cells of ground they stand in,
    // kept while its trees, transform and sway reach hold, so a frame tests
    // a cell before its trees and never re-transforms them.
    struct ForestCull;
    ForestCull& forestCull(const ForestNode& node, const bromath::Mat4& world, float reach);
    std::unordered_map<const ForestNode*, std::shared_ptr<ForestCull>> forestCull_;
    uint64_t forestCullUse_ = 0;
    void finishForest(SceneFrame& frame, ForestPlan& plan, float detail, float shadowDetail,
                      std::vector<MeshDraw>& out);

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
    /// mesh is an item of its own, recordGroundLayers').
    static uint64_t groundGeneration(GrassNode& grass);
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
    VkShaderModule fsSpray_ = VK_NULL_HANDLE;          // needle / scale-leaf forest leaves: SCENE_FOREST_SPRAY
    VkShaderModule fsSprayIndirect_ = VK_NULL_HANDLE;
    VkShaderModule vsBloom_ = VK_NULL_HANDLE;          // forest blooms: mesh_forest_bloom.vert
    VkShaderModule fsBloom_ = VK_NULL_HANDLE;          // and mesh.frag's SCENE_FOREST_BLOOM
    VkShaderModule fsBloomIndirect_ = VK_NULL_HANDLE;
    VkShaderModule vsLeafInline_ = VK_NULL_HANDLE;     // forest leaves past the pose buffer's room
    VkShaderModule vsHerb_ = VK_NULL_HANDLE;           // a grass node's herbs: mesh_herb.vert
    VkShaderModule fsHerb_ = VK_NULL_HANDLE;           // and mesh.frag's SCENE_HERB
    VkShaderModule fsHerbIndirect_ = VK_NULL_HANDLE;
    VkShaderModule fsPlants_ = VK_NULL_HANDLE;         // plants: plants.frag (plants.vert is vs_[Plants])
    VkShaderModule fsPlantsIndirect_ = VK_NULL_HANDLE;
    VkShaderModule fsGrass_ = VK_NULL_HANDLE;          // grass blades: mesh.frag's SCENE_GRASS build
    VkShaderModule fsGrassIndirect_ = VK_NULL_HANDLE;
    VkShaderModule fsBark_ = VK_NULL_HANDLE;          // Tube, ForestTube: mesh.frag's SCENE_BARK build
    VkShaderModule fsBarkIndirect_ = VK_NULL_HANDLE;
    VkShaderModule fsGround_ = VK_NULL_HANDLE;         // turf: mesh.frag's SCENE_GROUND build
    VkShaderModule fsGroundIndirect_ = VK_NULL_HANDLE;
    VkShaderModule fsGroundFill_ = VK_NULL_HANDLE;     // a turf layer tile's fill (ground_layer_fill.frag)
    std::unordered_map<uint32_t, std::unique_ptr<GroundLayers>> groundLayers_;   // by grass node id
    VkShaderModule fsSpriteIndirect_ = VK_NULL_HANDLE;
    PipelineVariants builtin_;
    std::unordered_map<std::string, std::unique_ptr<CustomProgram>> custom_;

    // The forests' leaf poses (forest_leaf_pose.comp): this frame's jobs,
    // one per posed leaf draw (colour, or a tile's leaf casters, push.extra.w
    // 1), the slots handed out so far, and the
    // device-local buffer they go to (kept across frames, grown as needed).
    struct ForestPoseJob {
        VkDescriptorSet set = VK_NULL_HANDLE;
        MeshPushConstants push{};
        uint32_t entries = 0;
    };
    /// This frame slot's pose buffer with room for `slots` poses (a new one,
    /// the old deferred, when it has not); null on failure. A copy a slot:
    /// the GPU planner's poses for a frame are written on the async compute
    /// queue while the last frame's draws may still read theirs.
    const SceneVkBuffer* ensurePoses(SceneFrame& frame, uint32_t slots);
    std::vector<ForestPoseJob> poseJobs_;
    uint32_t poseUsed_ = 0;
    SceneVkBuffer poses_[render::VulkanFrames::kFramesInFlight];
    VkPipelineLayout poseLayout_ = VK_NULL_HANDLE;
    VkShaderModule poseCs_ = VK_NULL_HANDLE;
    VkPipeline posePipeline_ = VK_NULL_HANDLE;
    // The plants' GPU planner (scene_plants_gpu_plan.h); without it, the
    // CPU plans them (scene_mesh_drawer_plants.cpp).
    std::shared_ptr<plants::GpuPlanner> plantPlanner_;
    // The CPU-planned plants' leaf poses (plants_pose.comp, on the forest
    // poses' layout): this frame's jobs, a dispatch of a workgroup a leaf chunk,
    // and each frame slot's buffer (kept, grown as needed; a node's set
    // names the buffer its job writes, so a node growing it leaves the
    // earlier nodes' poses where they were).
    struct PlantPoseJob {
        VkDescriptorSet set = VK_NULL_HANDLE;
        uint32_t firstChunk = 0;
        uint32_t chunks = 0;
    };
    /// Room for `bytes` more poses in this frame slot's buffer, at `offset`
    /// (a new buffer when it has none, the old deferred); null on failure.
    const SceneVkBuffer* ensurePlantPoses(SceneFrame& frame, VkDeviceSize bytes, VkDeviceSize& offset);
    std::vector<PlantPoseJob> plantPoseJobs_;
    VkDeviceSize plantPoseUsed_ = 0;   // bytes of this frame's buffer handed out
    VkShaderModule plantPoseCs_ = VK_NULL_HANDLE;
    VkPipeline plantPosePipeline_ = VK_NULL_HANDLE;
    SceneVkBuffer plantPoses_[render::VulkanFrames::kFramesInFlight];

    // The forests' detail factors as last drawn (per node, or the shared
    // one of prepareForests): the budget's fit moves them only so fast and
    // past a margin, so a moving camera does not re-thin the crowns a little
    // every frame (scene_mesh_drawer_forest.cpp's settleDetail).
    struct ForestDetail {
        float detail = 0.0f, shadow = 0.0f;
        float eye[3] = {0.0f, 0.0f, 0.0f};
        float budget = 0.0f, shadowBudget = 0.0f;
        float time = 0.0f;   // the scene clock (windTime) when last settled
        // The GPU planner settled it last (its details are in the planner's
        // slot `gpuSlot`, not above).
        bool onGpu = false;
        int gpuSlot = -1;
    };
    std::unordered_map<const void*, ForestDetail> forestDetail_;
    ForestDetail forestsDetail_;
    int nextGpuSlot_ = 1;   // the planner's settled-detail slots handed out (0: forestsDetail_'s)
    // A settle's rate this frame (and the bookkeeping: the eye, budgets and
    // clock it was taken at): snap takes the fit as it is (jump: for a jump
    // of the eye or a new budget, not for want of a detail).
    struct SettleRate {
        bool snap = true;
        bool jump = true;
        float step = 1.0f;
    };
    SettleRate settleRate(ForestDetail& d, const SceneFrame& frame, float budget, float shadowBudget);
    void settleDetail(ForestDetail& d, const SceneFrame& frame, float budget, float shadowBudget, float& detail,
                      float& shadowDetail);

    // The GPU planner (scene_mesh_drawer_forest_gpu.cpp, scene_forest_gpu_plan.cpp):
    // the nodes' draws planned on the GPU, false when it does not apply this
    // frame (the CPU's path then plans them).
    bool prepareForestsGpu(SceneFrame& frame, const std::vector<ForestNode*>& nodes,
                           std::vector<std::vector<MeshDraw>>& out, ForestDetail& detail);
    std::shared_ptr<forest::GpuPlanner> gpuPlanner_;
};

}  // namespace bro::scene::vk
