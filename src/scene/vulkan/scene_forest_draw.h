#pragma once
// What a forest node's draws are, shared by the drawer's two planners: the
// CPU's (scene_mesh_drawer_forest.cpp) and the GPU's
// (scene_mesh_drawer_forest_gpu.cpp, scene_forest_gpu_plan.cpp). The groups
// a forest draws, their entries' sizes and index patterns, and each group's
// MeshDraw (its kind, mesh, flags, push constants and pass order), so the
// two paths draw a group alike and differ only in where its entries and
// counts come from.

#include "scene/forest_node.h"
#include "scene/vulkan/scene_mesh_drawer.h"
#include "scene/vulkan/shaders/scene_forest_layout.h"

#include <bromesh/mesh_data.h>

#include <cstdint>

namespace bro::scene::vk {

struct SceneFrame;
struct GpuMesh;
class SceneDefaults;

namespace forest {

// Geometry slots of the node's GpuMesh copies.
constexpr uint32_t kLeafSlot = 0x7ff0;
constexpr uint32_t kCardSlot = 0x7ff1;
constexpr uint32_t kClusterSlot = 0x7ff2;
// The wood's index patterns (kTubeSlot + sides; + 0 the twigs', + 1 the
// fine twigs'; kTubeCurveSlot + sides the curved bark's).
constexpr uint32_t kTubeSlot = 0x7fe0;
constexpr uint32_t kTubeCurveSlot = 0x7fc0;

constexpr uint32_t kSegmentsPerEntry = FOREST_SEGMENTS_PER_ENTRY;   // bark: segments an instance
constexpr uint32_t kClumpsPerEntry = FOREST_CLUMPS_PER_ENTRY;       // sprites: clumps an instance
constexpr uint32_t kVertsPerClump = FOREST_CLUMP_VERTICES;          // three lobes of two triangles
constexpr uint32_t kRunsPerEntry = FOREST_RUNS_PER_ENTRY;           // run casters: a disc each
constexpr uint32_t kBloomsPerEntry = FOREST_BLOOMS_PER_ENTRY;       // blooms: a card each
constexpr uint32_t kVertsPerBloom = FOREST_CARD_VERTICES;
constexpr uint32_t kTwigPiecesPerEntry = FOREST_TWIG_PIECES_PER_ENTRY;
constexpr uint32_t kVertsPerTwig = FOREST_TWIG_VERTICES;
constexpr uint32_t kTwigFineRows = FOREST_TWIG_FINE_ROWS;
static_assert(ForestNode::kCardCopies == FOREST_CARD_COPIES);

// Bark's sides (ForestNode::TreeLod::barkSides).
constexpr int kNearestSides = FOREST_BARK_SIDES_NEAREST;
constexpr int kNearSides = FOREST_BARK_SIDES_NEAR;
constexpr int kFarSides = FOREST_BARK_SIDES_FAR;
// The rows of rings a curved segment of bark is drawn in
// (scene_mesh_drawer_forest.cpp's kBarkCurvePixels says when).
constexpr uint32_t barkRows(int sides) { return sides >= 20 ? 3u : sides >= 10 ? 2u : 1u; }
// A bark entry's param (scene_forest_tube.glsl's forestTubeVertexMorph).
constexpr uint32_t barkParam(int sides, uint32_t rows) {
    return static_cast<uint32_t>(sides) | rows << FOREST_BARK_ROWS_SHIFT;
}

/// The wood's fixed index pattern: bark of `sides` sides in `rows` rows of
/// rings a segment, or (sides 0) the twigs' ribbons in `rows` rows a piece.
const bromesh::MeshData& woodIndices(int sides, uint32_t rows = 1);

/// The shapes a forest draws, each one draw, in this order: a needle or
/// scale node's lobe CORES first, so the leaves they hide fail the depth
/// test; the blooms before the clumps and crowns, so the pixels they cover
/// cost those nothing.
enum Group {
    Cores, LeafNear, LeafCard, Bark20Curved, Bark10Curved, Bark20, Bark10, Bark5, Twigs, TwigsFine, Blooms, Clumps,
    Crowns, Runs, kGroups
};

/// A node's draws' shared state this frame: the base draw (its entries'
/// set, push constants, flags) and the meshes and material its groups pick.
struct DrawBase {
    MeshDraw base;
    uint32_t leafFlags = 0, barkFlags = 0, spriteFlags = 0;
    VkDescriptorSet leafMaterial = VK_NULL_HANDLE;
    const GpuMesh* leafMesh = nullptr;
    const GpuMesh* cardMesh = nullptr;
    const GpuMesh* clusterMesh = nullptr;
    bool clusters = false;   // a colour card is a cluster (Lod::clusters, a broad node's)
    ForestNode::Lod lod, shadowLod;   // the plan's (the bark's morph widths, the near sphere)
    bool farLast = false;    // the clumps and crowns go after the scene (the eye under the canopy)
};

/// The node's meshes (false when one is missing), and `out`'s state: `set`
/// is its set 3 (store, entries, poses), `view` the frame's.
bool drawBase(SceneFrame& frame, SceneDefaults& defaults, VkDevice device, ForestNode& node, VkDescriptorSet set,
              DrawBase& out);

/// Group `k`'s draw of `n` entries from entry `first` (the camera's, or a
/// shadow caster's in shadow plan slot `slot`). `posed`: a leaf group (a
/// colour one, or a tile's leaf casters) whose leaves the pose pass posed
/// (else it poses them inline).
MeshDraw groupDraw(SceneFrame& frame, ForestNode& node, const DrawBase& b, int k, bool shadow, int slot, bool posed,
                   uint32_t first, uint32_t n);

}  // namespace forest
}  // namespace bro::scene::vk
