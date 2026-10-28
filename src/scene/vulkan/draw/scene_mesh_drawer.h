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
#include "scene/world/land_sun.h"
#include "scene/world/surface_layers.h"
#include "scene/vulkan/frame/scene_gpu_resources.h"
#include "scene/vulkan/frame/scene_vk_pipeline.h"
#include "scene/vulkan/world/scene_surface_layers.h"
#include "scene/vulkan/world/scene_world_horizon.h"
#include "scene/vulkan/world/scene_world_service.h"
#include "scene/vulkan/frame/scene_vk_allocator.h"
#include "scene/vulkan/draw/scene_vk_custom_shader.h"
#include "scene/vulkan/ground/scene_cover_shaders.h"
#include "scene/vulkan/foliage/scene_plant_shaders.h"
#include "scene/vulkan/ground/scene_land_process.h"
#include "scene/vulkan/ground/scene_land_regions.h"
#include "scene/vulkan/ground/scene_land_shaders.h"
#include "scene/vulkan/shaders/ground/scene_land_layout.h"
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
class LandNode;
}

namespace bro::scene::vk {

struct SceneFrame;
struct SceneGpu;
struct LandSunGpu;
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
constexpr uint32_t kGroundFar = 262144;     // with kGround, a land's blocks past the painted band: the SCENE_GROUND_FAR
                                            // build (picks the pipeline; mesh.frag never reads it)
constexpr uint32_t kHerb = 2097152;         // a grass node's herb plants (a Grass draw): mesh_herb.vert, SCENE_HERB
constexpr uint32_t kCoverFar = 4194304;     // the cover's far tier (a Grass draw): mesh_cover_far.vert, cover_far.frag
constexpr uint32_t kCanopy = 8388608;       // a land carrying its plants' canopy (mesh.frag's landCanopyAt)
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
/// elements (no streams), planned on the GPU (scene_plants.glsl). Land is a
/// LandNode's rings, placed on the land by the vertex stage from its tiles
/// (set 3, scene_land.glsl).
enum class MeshKind : uint8_t {
    Static, Instanced, Skinned, Scatter, Tube, Vegetation, InstancedVegetation, Grass, Plants, Land,
};
constexpr int kMeshKindCount = 10;

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
    /// The shaders of its node's world modules: grass and turf their grass
    /// node's (CoverShaders::index, 0 the built-in set), plants their
    /// plants node's (PlantShaders::index).
    uint32_t moduleSet = 0;

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
    /// A land node's draw: its rings, placed from its tiles
    /// (scene_mesh_drawer_land.cpp). False while it has no module that
    /// compiles or no tile yet.
    /// A land's draws: its blocks, and as turf those wholly past the painted
    /// band apart (kGroundFar), each a draw of its own.
    bool prepareLand(SceneFrame& frame, LandNode& node, std::vector<MeshDraw>& out);
    /// The plants' plans and the leaf poses their draws read (queued by
    /// preparePlants) into the frame's compute stream (SceneFrame::
    /// computeStream), and the grass rings' (queued by prepareGrass) into
    /// its graphics stream: recorded outside any rendering, before the
    /// passes that draw them.
    void recordPlans(SceneFrame& frame);
    /// The plans recorded since the last call went to the GPU as `ticket` on
    /// `queue`: the GPU planner's read-backs of them are read once it
    /// completes, and the land's horizon made since then.
    void plansSubmitted(uint64_t ticket, render::VulkanQueue& queue);
    /// The world service's jobs of three kinds, the land's tiles first (so
    /// the horizon and the turf's fills read the land as it now is):
    ///  - the land's tiles (scene_mesh_drawer_land.cpp): for each land node
    ///    drawn, the tiles the camera needs and has not got, offered as
    ///    fills, and those granted made by its module in a compute pass;
    ///  - the land's horizon about the eye (scene_world_horizon.h), from the
    ///    first land drawn's tiles, when the eye has moved or they changed;
    ///  - the land's sun (scene/world/land_sun.h), its levels about the eye
    ///    from the same tiles, as the eye, the sun or the land move them;
    ///  - the turf's surface layers (docs/scene-world-architecture.md §5;
    ///    scene_mesh_drawer_ground.cpp): for each grass node whose turf this
    ///    frame draws, likewise, its turf meshes drawn straight down into
    ///    the fills granted.
    /// With the page tables' changes. Recorded outside any rendering, after
    /// the frame's uniforms and before anything draws the land or the turf.
    void offer(SceneFrame& frame, WorldService& service) override;
    double record(SceneFrame& frame, int kind, std::span<const WorldJob> granted, WorldStream& stream) override;
    /// The land's sun as every lit shader reads it (the lighting block's
    /// LandSunGpu, about `eye`, and the lighting set's binding 23).
    void fillLandSun(LandSunGpu& out, const bromath::Vec3& eye) const;
    VkImageView landSunView() const;
    /// Land `id`'s processes' grid as its fills last read it (rgba32f, in
    /// GENERAL; ground/land_grid.glsl), null until they have run.
    VkImageView landGrid(uint32_t id) const {
        const auto it = lands_.find(id);
        return it == lands_.end() ? VK_NULL_HANDLE : it->second->grid;
    }

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
    /// The cover's two passes in the opaque pass (docs/scene-world-architecture.md
    /// §3): its blades' and herbs' depth alone (their cut decides it, nothing
    /// lit), then each lit where its depth is the nearest, so a fragment
    /// another blade hides is never lit. Elsewhere the cover draws in one
    /// pass, cutting as it shades.
    enum class CoverPass { None, Depth, Shade };
    /// Whether `draw` is drawn in the cover's two passes in the opaque pass.
    bool coverPrepassed(const MeshDraw& draw) const;
    void record(VkCommandBuffer cmd, const TargetFormat& target, VkDescriptorSet cameraSet,
                VkDescriptorSet lightingSet, const MeshDraw& draw, const ChunkCull* cull = nullptr,
                CoverPass pass = CoverPass::None);

    VkPipelineLayout layout() const { return layout_; }

    /// A grass draw's cover set and a plants draw's (MeshDraw::moduleSet):
    /// the shadow casters draw them with its shadow stage.
    const CoverShaders& coverSet(uint32_t index) const { return *covers_[index]; }
    const PlantShaders& plantSet(uint32_t index) const { return *plantSets_[index]; }

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

    /// The land a grass node stands on, as the grass's set 4 binds it
    /// (scene_grass.glsl; scene_land_layout.h's LG_*).
    struct GrassGround {
        LandNode* land = nullptr;
        VkImageView layers[4] = {};   // heights, surface, cover, canopy
        VkSampler sampler = VK_NULL_HANDLE;
        VkImageView page = VK_NULL_HANDLE;
        VkSampler pageSampler = VK_NULL_HANDLE;
        float words[LD_WORDS] = {};   // its LandDraw (LD_WORDS) this frame
    };

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
    /// Set 4 for a turf mesh: grass node `grass`'s land and influence map,
    /// with its outermost ring (scene_mesh_drawer_grass.cpp). Null while the
    /// node has no land with tiles to stand on.
    VkDescriptorSet groundSet(SceneFrame& frame, GrassNode& grass);
    /// A grass node's turf surface layers: the tiles' CPU state and atlas.
    struct GroundLayers {
        SurfaceLayers tiles;
        SceneSurfaceLayerAtlas atlas;
        uint64_t frame = 0;
        int roots = 1;         // a round body's land's chart: its six faces
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
    /// Turf mesh `node`'s items (its world box `bounds`, in the place frame):
    /// its ground's heights in cells, on the body (`origin` the place
    /// frame's), retaken when its geometry or placement changes.
    static const std::vector<SurfaceItem>& turfItems(GroundLayers& gl, const MeshNode& node,
                                                     const bromath::AABB3& bounds, const bromath::Vec3& origin);
    /// What every tile of a grass node's turf layer reads changes with this
    /// (its land, herbs and the patterns' parameters; each turf mesh is an
    /// item of its own, offer()'s).
    static uint64_t groundGeneration(SceneGraph& graph, GrassNode& grass);
    /// Set 3 holding a procedural draw's header and records, then its
    /// vegetation records (4 vec4s a segment; header[10] says where); null
    /// when the frame's upload memory runs out.
    VkDescriptorSet segmentSet(SceneFrame& frame, float (&header)[16], const std::vector<float>& records,
                               const std::vector<float>* leafSegments, const std::vector<float>* vegetation);
    bool prepareProcedural(SceneFrame& frame, InstancedMeshNode& node, MeshDraw& out);
    /// The pipeline state a draw selects beyond its shaders.
    uint32_t rasterVariant(const MeshDraw& draw) const;
    VkPipeline builtinPipeline(const MeshDraw& draw, const TargetFormat& target, CoverPass pass = CoverPass::None);
    VkPipeline customPipeline(CustomProgram& prog, const MeshDraw& draw, const TargetFormat& target);
    VkPipeline buildPipeline(VkShaderModule vs, VkShaderModule fs, MeshKind kind, uint32_t raster,
                             const TargetFormat& target);
    /// The fill of a round body's turf tile (ground_layer_fill.frag's
    /// GROUND_LAYER_CHART build) with `cover`'s modules.
    VkPipeline chartFillPipeline(CoverShaders& cover, const TargetFormat& target);

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
    VkShaderModule fsBark_ = VK_NULL_HANDLE;           // Tube: mesh.frag's SCENE_BARK build
    VkShaderModule fsBarkIndirect_ = VK_NULL_HANDLE;
    VkShaderModule fsLand_ = VK_NULL_HANDLE;           // Land, not turf: mesh.frag's SCENE_LAND build
    VkShaderModule fsLandIndirect_ = VK_NULL_HANDLE;
    VkShaderModule vsChartFill_ = VK_NULL_HANDLE;      // a round body's turf tile, whole: ground_chart_fill.vert
    /// The shaders a grass node's cover is planned and drawn with (the
    /// blades, their shadows, the herbs, the far tier's vertex stage, the
    /// turf and its fill): [0] the built-in set, then one per pair of a
    /// world's sward and herb modules, compiled at the first node that
    /// has it, off the frame thread (the built-in set while it compiles,
    /// `*pending` true then: its turf is not drawn).
    std::vector<std::unique_ptr<CoverShaders>> covers_;
    std::unordered_map<uint64_t, uint32_t> coverByModule_;   // the modules' source hashes -> index
    const CoverShaders& coverShaders(const GrassNode& node, bool* pending = nullptr);
    /// The shaders a plants node is drawn with: one a world's plant module,
    /// compiled at the first node that has it.
    std::vector<std::unique_ptr<PlantShaders>> plantSets_;
    std::unordered_map<uint64_t, int> plantByModule_;   // the module's source hash -> index (-1: failed)
    /// A plants node's set; null for a node with no module or one that did
    /// not compile.
    const PlantShaders* plantShaders(const PlantsNode& node);
    std::unordered_map<uint32_t, std::unique_ptr<GroundLayers>> groundLayers_;   // by grass node id
    int turfKind_ = -1;   // the turf's fills, a world service job kind
    void offerTurf(SceneFrame& frame, WorldService& service);
    uint32_t recordTurf(SceneFrame& frame, std::span<const WorldJob> granted, WorldStream& stream);

    // --- the land (scene_mesh_drawer_land.cpp) ---
    /// A land node's tiles: their CPU state and atlas (heights, surface).
    struct LandTiles {
        SurfaceLayers tiles;
        SceneSurfaceLayerAtlas atlas;
        uint64_t frame = 0;
        uint64_t generation = 0;   // the node's its tiles were configured at
        uint64_t itemsSerial = 0;  // the node's its items were last taken at (LandNode::itemsSerial)
        int roots = 1;             // its chart's roots (its page table's layers are maxLevels a root)
        int firstRing = 0;         // this frame's (LandNode::firstRing)
        bool cleared = false;      // its page table cleared (once made or reconfigured)
        bool failed = false;       // its atlas could not be made
        const LandShaders* shaders = nullptr;   // this frame's
        VkImageView grid = VK_NULL_HANDLE;      // its processes' grid as its fills read it, this frame's
        std::vector<MeshChunk> chunks;          // its ring blocks where this frame's eye puts them
        std::vector<MeshChunk> farChunks;       // those of them wholly past the painted band (kGroundFar)
    };
    /// The lands' processes (scene_land_process.h), run in offerLand.
    SceneLandProcesses landProcesses_;
    /// Their regional grids (scene_land_regions.h), made in offerLand after them.
    SceneLandRegions landRegions_;
    LandTiles* landTiles(SceneFrame& frame, LandNode& node);
    /// Its LandDraw words (scene_land_layout.h, LD_WORDS of them) for this
    /// frame: the rings and tiles about the frame's eye.
    static void landWords(const SceneFrame& frame, const LandNode& node, const LandTiles& lt, float* words);
    /// The land grass node `grass` stands on, as the grass's set binds it;
    /// false while it has no tiles resident (nothing to stand on yet).
    bool grassGround(SceneFrame& frame, const GrassNode& grass, GrassGround& out);
    /// A land node's set; null for a node with no module or one that did
    /// not compile.
    const LandShaders* landShaders(const LandNode& node);
    VkPipeline landFillPipeline(const LandShaders& shaders);
    bool setupLand(SceneGpu& gpu);
    void cleanupLand(SceneGpu& gpu);
    void offerLand(SceneFrame& frame, WorldService& service);
    uint32_t recordLand(SceneFrame& frame, std::span<const WorldJob> granted, WorldStream& stream);
    std::unordered_map<uint32_t, std::unique_ptr<LandTiles>> lands_;   // by land node id
    std::vector<std::unique_ptr<LandShaders>> landSets_;
    std::unordered_map<uint64_t, int> landByModule_;   // the module's source hash -> index (-1: failed)
    std::vector<AsyncComputePipeline> landFillPipelines_;   // by set index
    VkDescriptorSetLayout landFillSetLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout landFillLayout_ = VK_NULL_HANDLE;
    int landKind_ = -1;   // the land's fills, a world service job kind
    /// The lands' regions' steps of kind `step` (a job each, granted as the
    /// budget holds), recorded into the frame's graphics stream.
    uint32_t recordLandRegions(SceneFrame& frame, SceneLandRegions::Step step, std::span<const WorldJob> granted,
                               WorldStream& stream);
    int regionKindOf(SceneLandRegions::Step step) const;
    int regionsKind_ = -1;          // a batch's passes
    int regionInitsKind_ = -1;      // its init's bands
    int regionFinishesKind_ = -1;   // its finish (copies and read-backs)
    std::vector<uint32_t> landDrawn_;     // the land nodes prepared this frame
    std::vector<uint32_t> landOffered_;   // those whose tiles offer() updated this frame
    /// The land's horizon (scene_world_horizon.h): made from the first land
    /// offered's tiles by land_horizon.comp, a world service job kind of its
    /// own; landHorizon is the newest the frame's compute may read.
    bool setupLandHorizon(SceneGpu& gpu);
    void cleanupLandHorizon(SceneGpu& gpu);
    const WorldHorizon::Read* landHorizon(SceneFrame& frame);
    uint32_t recordLandHorizon(SceneFrame& frame, std::span<const WorldJob> granted, WorldStream& stream);
    WorldHorizon horizon_;
    int horizonKind_ = -1;
    bool landChanged_ = false;   // tiles filled since the horizon was last made
    VkShaderModule horizonModule_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout horizonSetLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout horizonLayout_ = VK_NULL_HANDLE;
    VkPipeline horizonPipeline_ = VK_NULL_HANDLE;
    /// The land's sun (scene/world/land_sun.h; scene_mesh_drawer_land_sun.cpp):
    /// its levels made from the first land offered's tiles by land_sun.comp,
    /// a world service job kind of its own, into landSunImage_ (made with
    /// the first land; until then the 1x1 landSunNone_ is bound).
    bool setupLandSun(SceneGpu& gpu);
    void cleanupLandSun(SceneGpu& gpu);
    void offerLandSun(SceneFrame& frame, WorldService& service);
    uint32_t recordLandSun(SceneFrame& frame, std::span<const WorldJob> granted, WorldStream& stream);
    LandSun landSun_;
    SceneVkImage landSunImage_;
    SceneVkImage landSunNone_;
    double landSunRadius_ = 0.0;   // its land's body's (0 flat): its heights are over the datum
    float landSunCell_ = 0.0f;     // its land's finest drawn cell (m): a level finer than half of it is not shown
    bool landSunShown(int l) const;
    int landSunKind_ = -1;
    bool landSunTiles_ = false;   // tiles filled since the land's sun was last offered
    VkShaderModule landSunModule_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout landSunSetLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout landSunLayout_ = VK_NULL_HANDLE;
    VkPipeline landSunPipeline_ = VK_NULL_HANDLE;
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
