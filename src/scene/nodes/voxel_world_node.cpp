#include "scene/nodes/voxel_world_node.h"

#include "render/vulkan_compute.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace bro::scene {

namespace {

void normalizeAxis(float v[3], float fx, float fy, float fz) noexcept {
    const double len2 = static_cast<double>(v[0]) * v[0] +
                        static_cast<double>(v[1]) * v[1] +
                        static_cast<double>(v[2]) * v[2];
    if (len2 > 1e-12) {
        const double inv = 1.0 / std::sqrt(len2);
        v[0] = static_cast<float>(v[0] * inv);
        v[1] = static_cast<float>(v[1] * inv);
        v[2] = static_cast<float>(v[2] * inv);
    } else {
        v[0] = fx;
        v[1] = fy;
        v[2] = fz;
    }
}

template <typename T>
bool uploadArrayBuffer(render::VulkanCompute& compute,
                       std::shared_ptr<render::ComputeBuffer>& dst,
                       const std::vector<T>& src,
                       std::string* err) {
    const VkDeviceSize byteSize = src.empty()
        ? VkDeviceSize{64}
        : static_cast<VkDeviceSize>(src.size() * sizeof(T));
    if (!dst || dst->size() < byteSize) {
        dst = compute.createBuffer(byteSize, err);
        if (!dst) return false;
    }
    if (src.empty()) {
        const uint8_t zero[64] = {};
        return compute.write(*dst, 0, zero, sizeof(zero));
    }
    return compute.write(*dst, 0, src.data(), byteSize);
}

}  // namespace

void VoxelRegion::originWorld(double out[3]) const noexcept {
    const double half = 0.5 * static_cast<double>(extent());
    for (int k = 0; k < 3; ++k) {
        out[k] = center[k] - half * (static_cast<double>(right[k]) +
                                     static_cast<double>(up[k]) +
                                     static_cast<double>(forward[k]));
    }
}

bromath::AABB3 VoxelRegion::worldBounds() const noexcept {
    const float half = 0.5f * extent();
    float rx = half * (std::abs(right[0]) + std::abs(up[0]) + std::abs(forward[0]));
    float ry = half * (std::abs(right[1]) + std::abs(up[1]) + std::abs(forward[1]));
    float rz = half * (std::abs(right[2]) + std::abs(up[2]) + std::abs(forward[2]));
    const bromath::Vec3 c{
        static_cast<float>(center[0]),
        static_cast<float>(center[1]),
        static_cast<float>(center[2]),
    };
    const bromath::Vec3 ext{rx, ry, rz};
    return bromath::AABB3{c - ext, c + ext};
}

GpuVoxelRegion VoxelRegion::packGpu(const double eye[3]) const noexcept {
    double origin[3] = {};
    originWorld(origin);
    GpuVoxelRegion gpu;
    gpu.r0[0] = static_cast<float>(origin[0] - eye[0]);
    gpu.r0[1] = static_cast<float>(origin[1] - eye[1]);
    gpu.r0[2] = static_cast<float>(origin[2] - eye[2]);
    gpu.r0[3] = extent();

    gpu.r1[0] = right[0];
    gpu.r1[1] = right[1];
    gpu.r1[2] = right[2];
    gpu.r1[3] = voxelSize;

    gpu.r2[0] = up[0];
    gpu.r2[1] = up[1];
    gpu.r2[2] = up[2];
    gpu.r2[3] = maxDist;

    gpu.r3[0] = forward[0];
    gpu.r3[1] = forward[1];
    gpu.r3[2] = forward[2];
    gpu.r3[3] = fadeStart;

    const uint32_t minDistM = static_cast<uint32_t>(
        std::clamp(std::llround(static_cast<double>(minDist)), 0LL, static_cast<long long>(kVoxelMaxMinDist)));
    gpu.rootNode = rootNode;
    gpu.maxDepth = (maxDepth & 0x1Fu) | (minDistM << 5u);
    gpu.flags = flags;
    gpu.animTickOffset = animTickOffset;

    // Rounded outward, so the box always holds what was placed.
    const auto lo = [](float v) {
        return static_cast<uint32_t>(std::clamp(std::floor(v * 65535.0f), 0.0f, 65535.0f));
    };
    const auto hi = [](float v) {
        return static_cast<uint32_t>(std::clamp(std::ceil(v * 65535.0f), 0.0f, 65535.0f));
    };
    const bool empty = contentMin[0] > contentMax[0] || contentMin[1] > contentMax[1] ||
                       contentMin[2] > contentMax[2];
    if (empty) {
        gpu.content[0] = 0xFFFFFFFFu;
        gpu.content[1] = 0x0000FFFFu;
        gpu.content[2] = 0u;
    } else {
        gpu.content[0] = lo(contentMin[0]) | (lo(contentMin[1]) << 16u);
        gpu.content[1] = lo(contentMin[2]) | (hi(contentMax[0]) << 16u);
        gpu.content[2] = hi(contentMax[1]) | (hi(contentMax[2]) << 16u);
    }
    const float nearEnd = minFadeEnd > minDist ? minFadeEnd : minDist * 1.35f;
    std::memcpy(&gpu.content[3], &nearEnd, sizeof(float));
    return gpu;
}

void VoxelRegion::growContent(int ix, int iy, int iz, uint32_t targetDepth) noexcept {
    const float cell = 1.0f / static_cast<float>(1u << std::min(targetDepth, 16u));
    const int c[3] = {ix, iy, iz};
    for (int k = 0; k < 3; ++k) {
        const float a = std::clamp(static_cast<float>(c[k]) * cell, 0.0f, 1.0f);
        contentMin[k] = std::min(contentMin[k], a);
        contentMax[k] = std::max(contentMax[k], std::min(a + cell, 1.0f));
    }
}

void VoxelRegion::setContentEmpty() noexcept {
    for (int k = 0; k < 3; ++k) {
        contentMin[k] = 1.0f;
        contentMax[k] = 0.0f;
    }
}

void VoxelRegion::setContentFull() noexcept {
    for (int k = 0; k < 3; ++k) {
        contentMin[k] = 0.0f;
        contentMax[k] = 1.0f;
    }
}

VoxelWorldNode::VoxelWorldNode(const std::string& name) : SceneNode(name) {}

uint32_t VoxelWorldNode::addRegion(const VoxelRegion& spec) {
    VoxelRegion reg = spec;
    reg.id = nextRegionId_++;
    reg.voxelSize = reg.voxelSize > 1e-5f ? reg.voxelSize : 1.0f;
    reg.maxDepth = std::clamp(reg.maxDepth, 1u, 16u);
    normalizeAxis(reg.right, 1.0f, 0.0f, 0.0f);
    normalizeAxis(reg.up, 0.0f, 1.0f, 0.0f);
    normalizeAxis(reg.forward, 0.0f, 0.0f, 1.0f);
    if (reg.rootNode == bromesh::kDagNullNode) {
        reg.setContentEmpty();
    } else {
        reg.setContentFull();
    }
    refreshCarveFlag(reg);
    regions_.push_back(reg);
    ++regionsGen_;
    bumpChangeGeneration();
    return reg.id;
}

VoxelRegion* VoxelWorldNode::findRegion(uint32_t regionId) noexcept {
    for (auto& r : regions_) {
        if (r.id == regionId) return &r;
    }
    return nullptr;
}

const VoxelRegion* VoxelWorldNode::findRegion(uint32_t regionId) const noexcept {
    for (const auto& r : regions_) {
        if (r.id == regionId) return &r;
    }
    return nullptr;
}

bool VoxelWorldNode::updateRegion(uint32_t regionId, const VoxelRegion& spec) {
    VoxelRegion* reg = findRegion(regionId);
    if (!reg) return false;
    const uint32_t keepRoot = spec.rootNode != bromesh::kDagNullNode ? spec.rootNode : reg->rootNode;
    const bool newRoot = keepRoot != reg->rootNode;
    float keepMin[3], keepMax[3];
    std::memcpy(keepMin, reg->contentMin, sizeof(keepMin));
    std::memcpy(keepMax, reg->contentMax, sizeof(keepMax));
    *reg = spec;
    reg->id = regionId;
    reg->rootNode = keepRoot;
    // Placements are kept in lattice units, so the bounds hold across a move
    // or a new voxel size; a root swapped in from outside may fill the box.
    std::memcpy(reg->contentMin, keepMin, sizeof(keepMin));
    std::memcpy(reg->contentMax, keepMax, sizeof(keepMax));
    if (newRoot) reg->setContentFull();
    reg->voxelSize = reg->voxelSize > 1e-5f ? reg->voxelSize : 1.0f;
    reg->maxDepth = std::clamp(reg->maxDepth, 1u, 16u);
    normalizeAxis(reg->right, 1.0f, 0.0f, 0.0f);
    normalizeAxis(reg->up, 0.0f, 1.0f, 0.0f);
    normalizeAxis(reg->forward, 0.0f, 0.0f, 1.0f);
    refreshCarveFlag(*reg);
    ++regionsGen_;
    bumpChangeGeneration();
    return true;
}

bool VoxelWorldNode::setRegionRoot(uint32_t regionId, uint32_t rootNode) {
    VoxelRegion* reg = findRegion(regionId);
    if (!reg) return false;
    reg->rootNode = rootNode;
    if (rootNode == bromesh::kDagNullNode) {
        reg->setContentEmpty();
    } else {
        reg->setContentFull();
    }
    refreshCarveFlag(*reg);
    ++regionsGen_;
    bumpChangeGeneration();
    return true;
}

bool VoxelWorldNode::removeRegion(uint32_t regionId) {
    const auto it = std::find_if(regions_.begin(), regions_.end(),
                                 [regionId](const VoxelRegion& r) { return r.id == regionId; });
    if (it == regions_.end()) return false;
    regions_.erase(it);
    ++regionsGen_;
    bumpChangeGeneration();
    return true;
}

bool VoxelWorldNode::hasRenderableRegions() const noexcept {
    for (const auto& r : regions_) {
        if ((r.flags & kVoxelRegionEnabled) != 0u && r.rootNode != bromesh::kDagNullNode) {
            return true;
        }
    }
    return false;
}

void VoxelWorldNode::refreshCarveFlag(VoxelRegion& region) noexcept {
    bool hasCarve = false;
    if (region.rootNode != bromesh::kDagNullNode && region.rootNode < pool_.nodes().size()) {
        hasCarve = bromesh::dag_pack::carveMaskOf(pool_.nodes()[region.rootNode]) != 0u;
    }
    if (hasCarve) {
        region.flags |= kVoxelRegionHasCarve;
    } else {
        region.flags &= ~kVoxelRegionHasCarve;
    }
}

bool VoxelWorldNode::placeSubtree(uint32_t regionId, int ix, int iy, int iz,
                                  uint32_t childRef, bool isLeaf, uint32_t targetDepth) {
    VoxelRegion* reg = findRegion(regionId);
    if (!reg) return false;
    reg->rootNode = pool_.setSubtreeAt(
        reg->rootNode,
        static_cast<int>(reg->maxDepth),
        static_cast<int>(targetDepth),
        ix, iy, iz,
        childRef,
        isLeaf);
    if (childRef != bromesh::kDagNullNode) reg->growContent(ix, iy, iz, targetDepth);
    refreshCarveFlag(*reg);
    ++regionsGen_;
    bumpChangeGeneration();
    return true;
}

bool VoxelWorldNode::placeSubtreesBatch(uint32_t regionId,
                                        std::span<const bromesh::DagPlacement> placements,
                                        uint32_t targetDepth) {
    VoxelRegion* reg = findRegion(regionId);
    if (!reg) return false;
    std::vector<bromesh::DagPlacement> copy(placements.begin(), placements.end());
    reg->rootNode = pool_.setSubtreesBatch(
        reg->rootNode,
        static_cast<int>(targetDepth),
        copy);
    for (const bromesh::DagPlacement& p : placements) {
        if (p.childRef != bromesh::kDagNullNode) reg->growContent(p.ix, p.iy, p.iz, targetDepth);
    }
    refreshCarveFlag(*reg);
    ++regionsGen_;
    bumpChangeGeneration();
    return true;
}

bool VoxelWorldNode::placeAnimatedSubtree(uint32_t regionId, int ix, int iy, int iz,
                                          uint32_t clipIndex, uint32_t phaseOffset,
                                          bool isLeaf, uint32_t targetDepth) {
    const uint32_t animRef = pool_.makeAnimChildRef(clipIndex, phaseOffset);
    return placeSubtree(regionId, ix, iy, iz, animRef, isLeaf, targetDepth);
}

bool VoxelWorldNode::stampCarveAndSolid(uint32_t regionId, int ix, int iy, int iz,
                                        uint32_t carveRef, bool carveIsLeaf,
                                        uint32_t solidRef, bool solidIsLeaf,
                                        uint32_t targetDepth) {
    VoxelRegion* reg = findRegion(regionId);
    if (!reg) return false;
    const uint32_t carveNode = (carveIsLeaf && carveRef != bromesh::kDagNullNode)
        ? pool_.wrapBrickAsNode(carveRef)
        : carveRef;
    const uint32_t solidNode = (solidIsLeaf && solidRef != bromesh::kDagNullNode)
        ? pool_.wrapBrickAsNode(solidRef)
        : solidRef;
    reg->rootNode = pool_.stampPairAt(
        reg->rootNode,
        static_cast<int>(reg->maxDepth),
        static_cast<int>(targetDepth),
        ix, iy, iz,
        carveNode,
        solidNode);
    reg->growContent(ix, iy, iz, targetDepth);
    refreshCarveFlag(*reg);
    ++regionsGen_;
    bumpChangeGeneration();
    return true;
}

bool VoxelWorldNode::clearCell(uint32_t regionId, int ix, int iy, int iz, uint32_t targetDepth) {
    return placeSubtree(regionId, ix, iy, iz, bromesh::kDagNullNode, false, targetDepth);
}

void VoxelWorldNode::onTick(float dtSec) {
    if (!autoAnimate_ || animFps_ <= 0.0f || dtSec <= 0.0f) return;
    animAccum_ += static_cast<double>(dtSec) * static_cast<double>(animFps_);
    if (animAccum_ >= 1.0) {
        const uint32_t adv = static_cast<uint32_t>(animAccum_);
        animAccum_ -= static_cast<double>(adv);
        animTick_ += adv;
    }
}

bool VoxelWorldNode::syncBuffers(render::VulkanCompute& compute, std::string* err) {
    if (!compute.available()) {
        if (err) *err = "VulkanCompute not available";
        return false;
    }

    const uint64_t poolGen = pool_.generation();
    if (shared_.syncedPoolGen != poolGen || !shared_.nodes) {
        if (!uploadArrayBuffer(compute, shared_.nodes, pool_.nodes(), err) ||
            !uploadArrayBuffer(compute, shared_.children, pool_.children(), err) ||
            !uploadArrayBuffer(compute, shared_.bricks, pool_.bricks(), err) ||
            !uploadArrayBuffer(compute, shared_.attrs, pool_.attrs(), err) ||
            !uploadArrayBuffer(compute, shared_.animClips, pool_.animClips(), err) ||
            !uploadArrayBuffer(compute, shared_.animFrames, pool_.animFrames(), err)) {
            return false;
        }
        shared_.syncedPoolGen = poolGen;
    }

    if (shared_.syncedRegionsGen != regionsGen_ || !shared_.regions) {
        const double zeroEye[3] = {0.0, 0.0, 0.0};
        std::vector<GpuVoxelRegion> gpuRegs;
        gpuRegs.reserve(regions_.size());
        for (const VoxelRegion& r : regions_) {
            if ((r.flags & kVoxelRegionEnabled) == 0u || r.rootNode == bromesh::kDagNullNode) continue;
            gpuRegs.push_back(r.packGpu(zeroEye));
        }
        if (!uploadArrayBuffer(compute, shared_.regions, gpuRegs, err)) return false;
        shared_.regionCount = static_cast<uint32_t>(gpuRegs.size());
        shared_.syncedRegionsGen = regionsGen_;
    }

    compute.flush();
    return true;
}

std::optional<bromath::AABB3> VoxelWorldNode::worldBounds() const {
    bromath::AABB3 bounds;
    bool any = false;
    for (const VoxelRegion& r : regions_) {
        if ((r.flags & kVoxelRegionEnabled) == 0u || r.rootNode == bromesh::kDagNullNode) continue;
        const bromath::AABB3 rb = r.worldBounds();
        if (!any) {
            bounds = rb;
            any = true;
        } else {
            bounds.min.x = std::min(bounds.min.x, rb.min.x);
            bounds.min.y = std::min(bounds.min.y, rb.min.y);
            bounds.min.z = std::min(bounds.min.z, rb.min.z);
            bounds.max.x = std::max(bounds.max.x, rb.max.x);
            bounds.max.y = std::max(bounds.max.y, rb.max.y);
            bounds.max.z = std::max(bounds.max.z, rb.max.z);
        }
    }
    if (!any) return std::nullopt;
    return bounds;
}

}  // namespace bro::scene
