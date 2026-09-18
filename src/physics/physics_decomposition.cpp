#include "physics/physics_decomposition.h"

#if BRO_WITH_3D

#include <bromesh/mesh_data.h>
#include <bromesh/analysis/convex_decomposition.h>
#include <Jolt/Physics/Collision/Shape/ConvexHullShape.h>
#include <Jolt/Physics/Collision/Shape/StaticCompoundShape.h>

namespace bro::physics {

RefConst<JPH::Shape> buildDecomposedMeshShape(const BodyOptions& opts) {
    if (opts.meshIndices.size() < 3 || opts.meshVertices.empty()) {
        return RefConst<JPH::Shape>();
    }

    bromesh::MeshData mesh;
    mesh.positions.reserve(opts.meshVertices.size() * 3);
    for (const auto& v : opts.meshVertices) {
        mesh.positions.push_back(v.GetX());
        mesh.positions.push_back(v.GetY());
        mesh.positions.push_back(v.GetZ());
    }
    mesh.indices = opts.meshIndices;

    bromesh::ConvexDecompParams params;
    params.maxHulls = opts.maxHulls > 0 ? opts.maxHulls : 16;
    params.maxVerticesPerHull = opts.maxVerticesPerHull > 0 ? opts.maxVerticesPerHull : 64;
    params.resolution = opts.decompResolution > 0 ? opts.decompResolution : 100000.0f;
    params.minVolumePerHull = opts.minVolumePerHull > 0 ? opts.minVolumePerHull : 0.001f;

    auto hulls = bromesh::convexDecomposition(mesh, params);
    if (hulls.empty()) {
        hulls.push_back(bromesh::convexHull(mesh));
    }

    if (hulls.size() == 1) {
        const auto& hull = hulls[0];
        if (hull.vertexCount() < 4) {
            return RefConst<JPH::Shape>();
        }
        JPH::Array<JPH::Vec3> pts;
        pts.reserve(hull.vertexCount());
        for (size_t i = 0; i + 2 < hull.positions.size(); i += 3) {
            pts.push_back(JPH::Vec3(hull.positions[i], hull.positions[i + 1], hull.positions[i + 2]));
        }
        JPH::ConvexHullShapeSettings s(pts);
        s.SetDensity(opts.density);
        auto r = s.Create();
        return r.HasError() ? RefConst<JPH::Shape>() : r.Get();
    }

    if (hulls.size() > 1) {
        JPH::StaticCompoundShapeSettings compound;
        for (const auto& hull : hulls) {
            if (hull.vertexCount() >= 4) {
                JPH::Array<JPH::Vec3> pts;
                pts.reserve(hull.vertexCount());
                for (size_t i = 0; i + 2 < hull.positions.size(); i += 3) {
                    pts.push_back(JPH::Vec3(hull.positions[i], hull.positions[i + 1], hull.positions[i + 2]));
                }
                auto hullSettings = new JPH::ConvexHullShapeSettings(pts);
                hullSettings->SetDensity(opts.density);
                compound.AddShape(JPH::Vec3::sZero(), JPH::Quat::sIdentity(), hullSettings);
            }
        }
        auto r = compound.Create();
        return r.HasError() ? RefConst<JPH::Shape>() : r.Get();
    }

    return RefConst<JPH::Shape>();
}

} // namespace bro::physics

#else // !BRO_WITH_3D

namespace bro::physics {

RefConst<JPH::Shape> buildDecomposedMeshShape(const BodyOptions& opts) {
    (void)opts;
    return RefConst<JPH::Shape>();
}

} // namespace bro::physics

#endif // BRO_WITH_3D
