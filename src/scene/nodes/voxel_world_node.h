#pragma once

#include "scene/scene_node.h"
#include <bromath/aabb.h>
#include <bromesh/voxel/voxel_dag.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace bro::render {
class ComputeBuffer;
class VulkanCompute;
}  // namespace bro::render

namespace bro::scene {

inline constexpr uint32_t kVoxelRegionEnabled = 1u << 0;
inline constexpr uint32_t kVoxelRegionHasCarve = 1u << 1;
inline constexpr uint32_t kVoxelRegionCastShadow = 1u << 2;

/// GPU layout for one active SVDAG region (`6 * vec4<f32>` = 96 bytes; voxeldag.bk REGION_STRIDE).
struct alignas(16) GpuVoxelRegion {
    float r0[4] = {0.0f, 0.0f, 0.0f, 0.0f};  // xyz: camera-relative/world min corner, w: extent
    float r1[4] = {1.0f, 0.0f, 0.0f, 1.0f};  // xyz: tangent right, w: voxelSize
    float r2[4] = {0.0f, 1.0f, 0.0f, 0.0f};  // xyz: tangent up, w: maxDist (0 = unlimited)
    float r3[4] = {0.0f, 0.0f, 1.0f, 0.0f};  // xyz: tangent forward, w: fadeStart
    uint32_t rootNode = bromesh::kDagNullNode;
    uint32_t maxDepth = 4;
    uint32_t flags = 0;
    uint32_t animTickOffset = 0;
    // The content bounds in the region's [0, 1]^3 as unorm16 pairs:
    // min.x | min.y << 16, min.z | max.x << 16, max.y | max.z << 16.
    uint32_t content[4] = {0u, 0xFFFF0000u, 0xFFFFFFFFu, 0u};
};
static_assert(sizeof(GpuVoxelRegion) == 96, "GpuVoxelRegion must be 96 bytes (6 x vec4)");

/// Sparse Voxel DAG region mounted in world space (double-precision center + orthonormal basis).
struct VoxelRegion {
    uint32_t id = 0;
    double center[3] = {0.0, 0.0, 0.0};
    float right[3] = {1.0f, 0.0f, 0.0f};
    float up[3] = {0.0f, 1.0f, 0.0f};
    float forward[3] = {0.0f, 0.0f, 1.0f};
    float voxelSize = 1.0f;
    uint32_t maxDepth = 4;  // extent = voxelSize * (4 << maxDepth)
    uint32_t rootNode = bromesh::kDagNullNode;
    float minDist = 0.0f;
    float maxDist = 0.0f;
    float fadeStart = 0.0f;
    uint32_t flags = kVoxelRegionEnabled | kVoxelRegionCastShadow;
    uint32_t animTickOffset = 0;
    // What the placements cover, in the region's [0, 1]^3 (min > max: nothing).
    // Rays are clipped to it, so a region mostly empty sky costs what its content does.
    float contentMin[3] = {1.0f, 1.0f, 1.0f};
    float contentMax[3] = {0.0f, 0.0f, 0.0f};

    float extent() const noexcept {
        return voxelSize * static_cast<float>(4u << maxDepth);
    }

    /// Grow the content bounds by cell (ix, iy, iz) of the lattice at `targetDepth`.
    void growContent(int ix, int iy, int iz, uint32_t targetDepth) noexcept;
    void setContentEmpty() noexcept;
    void setContentFull() noexcept;

    void originWorld(double out[3]) const noexcept;
    bromath::AABB3 worldBounds() const noexcept;
    GpuVoxelRegion packGpu(const double eye[3]) const noexcept;
};

/// Shared compute storage buffers holding the synced SVDAG pool + world-space regions,
/// directly bindable to custom mesh / tile shaders (`import voxeldag;`).
struct SharedVoxelGpuBuffers {
    std::shared_ptr<render::ComputeBuffer> regions;
    std::shared_ptr<render::ComputeBuffer> nodes;
    std::shared_ptr<render::ComputeBuffer> children;
    std::shared_ptr<render::ComputeBuffer> bricks;
    std::shared_ptr<render::ComputeBuffer> attrs;
    std::shared_ptr<render::ComputeBuffer> animClips;
    std::shared_ptr<render::ComputeBuffer> animFrames;
    uint64_t syncedPoolGen = 0;
    uint64_t syncedRegionsGen = 0;
    uint32_t regionCount = 0;
};

/// Scene node owning a content-addressed Sparse Voxel DAG pool and world-space regions
/// for ultra-dense mid/far geometry, true 3D caves/overhangs, and cyclic subtree animation.
class VoxelWorldNode final : public SceneNode {
public:
    explicit VoxelWorldNode(const std::string& name = "");
    ~VoxelWorldNode() override = default;

    VoxelWorldNode(const VoxelWorldNode&) = delete;
    VoxelWorldNode& operator=(const VoxelWorldNode&) = delete;

    Type type() const override { return Type::VoxelWorld; }

    bromesh::VoxelDagPool& pool() noexcept { return pool_; }
    const bromesh::VoxelDagPool& pool() const noexcept { return pool_; }

    uint32_t addRegion(const VoxelRegion& spec);
    VoxelRegion* findRegion(uint32_t regionId) noexcept;
    const VoxelRegion* findRegion(uint32_t regionId) const noexcept;
    bool updateRegion(uint32_t regionId, const VoxelRegion& spec);
    bool setRegionRoot(uint32_t regionId, uint32_t rootNode);
    bool removeRegion(uint32_t regionId);
    const std::vector<VoxelRegion>& regions() const noexcept { return regions_; }

    bool hasRenderableRegions() const noexcept;
    uint64_t regionsGeneration() const noexcept { return regionsGen_; }
    void bumpRegionsGeneration() noexcept { ++regionsGen_; }

    bool placeSubtree(uint32_t regionId, int ix, int iy, int iz,
                      uint32_t childRef, bool isLeaf, uint32_t targetDepth);
    bool placeSubtreesBatch(uint32_t regionId,
                          std::span<const bromesh::DagPlacement> placements,
                          uint32_t targetDepth);
    bool placeAnimatedSubtree(uint32_t regionId, int ix, int iy, int iz,
                              uint32_t clipIndex, uint32_t phaseOffset,
                              bool isLeaf, uint32_t targetDepth);
    bool stampCarveAndSolid(uint32_t regionId, int ix, int iy, int iz,
                            uint32_t carveRef, bool carveIsLeaf,
                            uint32_t solidRef, bool solidIsLeaf,
                            uint32_t targetDepth);
    bool clearCell(uint32_t regionId, int ix, int iy, int iz, uint32_t targetDepth);

    void onTick(float dtSec) override;

    void setAnimTick(uint32_t tick) noexcept { animTick_ = tick; }
    uint32_t animTick() const noexcept { return animTick_; }

    void setAnimFps(float fps) noexcept { animFps_ = fps > 0.0f ? fps : 0.0f; }
    float animFps() const noexcept { return animFps_; }

    void setAutoAnimate(bool enabled) noexcept { autoAnimate_ = enabled; }
    bool autoAnimate() const noexcept { return autoAnimate_; }

    void setLodScale(float s) noexcept { lodScale_ = s > 0.05f ? s : 0.05f; }
    float lodScale() const noexcept { return lodScale_; }

    void setShadowSteps(uint32_t steps) noexcept { shadowSteps_ = steps; }
    uint32_t shadowSteps() const noexcept { return shadowSteps_; }

    void setMaxRaySteps(uint32_t steps) noexcept { maxRaySteps_ = steps > 8u ? steps : 8u; }
    uint32_t maxRaySteps() const noexcept { return maxRaySteps_; }

    bool syncBuffers(render::VulkanCompute& compute, std::string* err = nullptr);
    const SharedVoxelGpuBuffers& sharedBuffers() const noexcept { return shared_; }

    std::optional<bromath::AABB3> worldBounds() const;

private:
    void refreshCarveFlag(VoxelRegion& region) noexcept;

    bromesh::VoxelDagPool pool_;
    std::vector<VoxelRegion> regions_;
    uint32_t nextRegionId_ = 1;
    uint64_t regionsGen_ = 1;

    uint32_t animTick_ = 0;
    float animFps_ = 12.0f;
    double animAccum_ = 0.0;
    bool autoAnimate_ = true;

    float lodScale_ = 1.0f;
    uint32_t shadowSteps_ = 24u;
    uint32_t maxRaySteps_ = 128u;

    SharedVoxelGpuBuffers shared_;
};

}  // namespace bro::scene
