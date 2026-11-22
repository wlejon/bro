#pragma once

// Geometry many mesh nodes draw (SceneGraph::createGeometry, MeshNode::
// setGeometry): one immutable mesh whose tangents, bounds, vegetation height,
// raycast BVH and GPU buffers are made once for every node that holds it,
// rather than once a node. A node made from it costs its transform and
// material alone, so a level of detail swapped for the next, or a stand of
// one plant's copies, uploads nothing. Its GPU buffers live while a node
// holds it (vk::SceneGpuResources::sharedMesh).

#include <bromath/aabb.h>
#include <bromesh/analysis/bvh.h>
#include <bromesh/mesh_data.h>

#include <cstdint>
#include <memory>
#include <mutex>

namespace bro::scene {

struct SharedGeometry {
    bromesh::MeshData mesh;        // tangents generated where it has UVs and normals
    bromath::AABB3 bounds;         // empty for an empty mesh
    float vegetationHeight = 0.0f; // the tallest vegetation record's height (0: none)
    uint64_t generation = 0;       // a resource generation of its own (never shared with a node's)

    /// The raycast BVH over it, built on first use.
    const bromesh::MeshBVH& bvh() const {
        std::call_once(bvhOnce_, [this] { bvh_ = bromesh::MeshBVH::build(mesh); });
        return bvh_;
    }

    /// `mesh` taken as the geometry, its derived data made now.
    static std::shared_ptr<const SharedGeometry> make(bromesh::MeshData&& mesh);

private:
    mutable std::once_flag bvhOnce_;
    mutable bromesh::MeshBVH bvh_;
};

}  // namespace bro::scene
