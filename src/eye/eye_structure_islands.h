#pragma once

#include <bromath/aabb.h>
#include <bromath/mat.h>
#include <bromath/vec.h>
#include <bromesh/analysis/bvh.h>
#include <bromesh/mesh_data.h>

#include <cstdint>
#include <vector>

namespace bro::eye {

struct MeshGeomRef {
    const bromesh::MeshData* mesh = nullptr;
    const bromesh::MeshBVH* bvh = nullptr;
    bromath::Mat4 world{};
    bromath::Mat4 invWorld{};
    bromath::AABB3 worldBox{};
};

struct LocalIsland {
    std::vector<bromath::Vec3> localSamples;
    std::vector<uint32_t> triIndices;
    bool closed = false;
    double rawSignedVolume = 0.0;
    double badNormalRatioPos = 0.0;
    double badNormalRatioNeg = 0.0;
    double reversedEdgeRatio = 0.0;
    int sharedEdges = 0;
};

struct IslandSummary {
    bromath::AABB3 worldBox{};
    std::vector<bromath::Vec3> samples;
    std::vector<uint32_t> triIndices;
    bool grounded = false;
    bool closed = false;
    double signedVolume = 0.0;
    double badNormalRatio = 0.0;
    double reversedEdgeRatio = 0.0;
    int sharedEdges = 0;
};

/// Extract welded connected triangle islands once in mesh-local space (reusable across instances).
std::vector<LocalIsland> extractLocalIslands(const bromesh::MeshData& md,
                                             size_t maxTris = 24000,
                                             size_t maxIslands = 256);

/// Instantiate local islands into world space for a specific transform.
std::vector<IslandSummary> instantiateIslands(const std::vector<LocalIsland>& localIslands,
                                              const bromesh::MeshData& md,
                                              const bromath::Mat4& world,
                                              float detSign,
                                              double groundY);

/// Convenience single-mesh island extraction.
std::vector<IslandSummary> extractMeshIslands(const bromesh::MeshData& md,
                                              const bromath::Mat4& world,
                                              float detSign,
                                              double groundY,
                                              size_t maxTris = 24000,
                                              size_t maxIslands = 256);

/// Audit intra-node connectivity and external BVH support across all significant islands of one entry.
void auditIslandGrounding(const std::vector<IslandSummary>& islands,
                          size_t selfIdx,
                          const std::vector<MeshGeomRef>& allGeoms,
                          double contactTol,
                          int& outFloatingIslands,
                          double& outWorstGap,
                          bromath::Vec3& outWorstCenter,
                          double& outWorstRadius);

}  // namespace bro::eye
