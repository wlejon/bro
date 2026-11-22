#pragma once

#include <bromath/aabb.h>
#include <bromath/mat.h>
#include <bromath/vec.h>

#include <cstdint>
#include <vector>

namespace bro::scene { class MeshNode; }

namespace bro::eye {

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

/// Decompose a mesh into welded connected triangle islands (returning empty if mesh exceeds caps).
std::vector<IslandSummary> extractMeshIslands(const scene::MeshNode& node,
                                              const bromath::Mat4& world,
                                              float detSign,
                                              double groundY,
                                              size_t maxTris = 24000,
                                              size_t maxIslands = 256);

/// Audit intra-node connectivity and external BVH support across all significant islands of one node.
/// Guards against top-only resting attachments (e.g. a handrail resting atop an unsupported stair tread).
void auditIslandGrounding(const std::vector<IslandSummary>& islands,
                          size_t selfIdx,
                          const std::vector<scene::MeshNode*>& allNodes,
                          const std::vector<bromath::Mat4>& allWorlds,
                          const std::vector<bromath::Mat4>& allInvWorlds,
                          const std::vector<bromath::AABB3>& allBoxes,
                          double contactTol,
                          int& outFloatingIslands,
                          double& outWorstGap,
                          bromath::Vec3& outWorstCenter,
                          double& outWorstRadius);

}  // namespace bro::eye
