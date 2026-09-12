#if BRO_WITH_3D

#include "bronze_host/host_mesh_internal.h"

#include <bromesh/mesh_data.h>
#include <bromesh/analysis/bbox.h>
#include <bromesh/analysis/raycast.h>
#include <bromesh/analysis/intersect.h>
#include <bromesh/analysis/bvh.h>
#include <bromesh/analysis/sample.h>
#include <bromesh/analysis/bake.h>
#include <bromesh/analysis/bake_texture.h>
#include <bromesh/analysis/bake_transfer.h>
#include <bromesh/analysis/convex_decomposition.h>
#include <bromesh/uv/projection.h>
#include <bromesh/uv/unwrap.h>
#include <bromesh/uv/uv_metrics.h>
#include <bromesh/procedural/obstacle_field.h>

#include <cmath>
#include <vector>

namespace bro::bronze_host {

HostClass g_meshBVHClass;
HostClass g_capsuleFieldClass;

static void hostMeshBVHDtor(void* p) {
    delete static_cast<HostMeshBVH*>(p);
}

Value wrapMeshBVH(bromesh::MeshBVH&& bvh) {
    auto* cell = new HostMeshBVH();
    cell->bvh = std::make_unique<bromesh::MeshBVH>(std::move(bvh));
    return g_meshBVHClass.make(cell, hostMeshBVHDtor);
}

static void hostCapsuleFieldDtor(void* p) {
    delete static_cast<HostCapsuleField*>(p);
}

Value wrapCapsuleField(std::unique_ptr<bromesh::CapsuleField> field) {
    if (!field) return ev::undefined();
    auto* cell = new HostCapsuleField();
    cell->field = std::move(field);
    return g_capsuleFieldClass.make(cell, hostCapsuleFieldDtor);
}

void installMeshBVH() {
    g_meshBVHClass.install(
        "MeshBVH", 1,
        [](Value, std::span<const Value> a) -> Value {
            if (a.empty()) {
                return wrapMeshBVH(bromesh::MeshBVH());
            }
            auto* m = hostMeshDataOf(a[0]);
            if (!m) {
                return wrapMeshBVH(bromesh::MeshBVH());
            }
            int leafSize = (a.size() > 1 && ev::isNumber(a[1])) ? static_cast<int>(ev::toDouble(a[1])) : 8;
            auto bvh = bromesh::MeshBVH::build(*m, leafSize);
            return wrapMeshBVH(std::move(bvh));
        },
        [](ObjectBuilder& proto) {
            proto.accessor("empty", [](Value self_, std::span<const Value>) {
                auto* b = hostMeshBVHOf(self_);
                return ev::fromBool(!b || !b->bvh || b->bvh->empty());
            }, nullptr);

            proto.accessor("nodeCount", [](Value self_, std::span<const Value>) {
                auto* b = hostMeshBVHOf(self_);
                return ev::fromDouble((b && b->bvh) ? static_cast<double>(b->bvh->nodeCount()) : 0.0);
            }, nullptr);

            proto.accessor("triangleCount", [](Value self_, std::span<const Value>) {
                auto* b = hostMeshBVHOf(self_);
                return ev::fromDouble((b && b->bvh) ? static_cast<double>(b->bvh->triangleCount()) : 0.0);
            }, nullptr);

            proto.def("bounds", 0, [](Value self_, std::span<const Value>) {
                auto* b = hostMeshBVHOf(self_);
                if (!b || !b->bvh) return ev::undefined();
                return makeBBox(b->bvh->bounds());
            });

            proto.def("raycast", 3, [](Value self_, std::span<const Value> a) {
                auto* b = hostMeshBVHOf(self_);
                if (!b || !b->bvh || a.empty()) return ev::throwTypeError("raycast requires source Mesh");
                auto* m = hostMeshDataOf(a[0]);
                if (!m) return ev::throwTypeError("first argument must be the source Mesh");
                float origin[3] = {0, 0, 0};
                float dir[3] = {0, 0, -1};
                if (a.size() > 1) readVec3(a[1], origin);
                if (a.size() > 2) readVec3(a[2], dir);
                float maxDist = (a.size() > 3 && ev::isNumber(a[3])) ? static_cast<float>(ev::toDouble(a[3])) : 0.0f;
                auto hit = b->bvh->raycast(*m, origin, dir, maxDist);
                return makeRayHit(hit);
            });

            proto.def("raycastTest", 3, [](Value self_, std::span<const Value> a) {
                auto* b = hostMeshBVHOf(self_);
                if (!b || !b->bvh || a.empty()) return ev::fromBool(false);
                auto* m = hostMeshDataOf(a[0]);
                if (!m) return ev::fromBool(false);
                float origin[3] = {0, 0, 0};
                float dir[3] = {0, 0, -1};
                if (a.size() > 1) readVec3(a[1], origin);
                if (a.size() > 2) readVec3(a[2], dir);
                float maxDist = (a.size() > 3 && ev::isNumber(a[3])) ? static_cast<float>(ev::toDouble(a[3])) : 0.0f;
                return ev::fromBool(b->bvh->raycastTest(*m, origin, dir, maxDist));
            });
        });
}

void installCapsuleField() {
    g_capsuleFieldClass.install(
        "CapsuleField", 3,
        [](Value, std::span<const Value> a) -> Value {
            std::vector<bromesh::Capsule> caps;
            std::vector<bromesh::Sphere> sphs;
            if (a.size() > 0) readCapsules(a[0], caps);
            if (a.size() > 1) readSpheres(a[1], sphs);
            float cellSize = (a.size() > 2 && ev::isNumber(a[2])) ? static_cast<float>(ev::toDouble(a[2])) : 0.0f;
            auto field = std::make_unique<bromesh::CapsuleField>(std::move(caps), std::move(sphs), cellSize);
            return wrapCapsuleField(std::move(field));
        },
        [](ObjectBuilder& proto) {
            proto.accessor("empty", [](Value self_, std::span<const Value>) {
                auto* cf = hostCapsuleFieldOf(self_);
                return ev::fromBool(!cf || !cf->field || cf->field->empty());
            }, nullptr);

            proto.accessor("capsuleCount", [](Value self_, std::span<const Value>) {
                auto* cf = hostCapsuleFieldOf(self_);
                return ev::fromDouble((cf && cf->field) ? static_cast<double>(cf->field->capsuleCount()) : 0.0);
            }, nullptr);

            proto.accessor("sphereCount", [](Value self_, std::span<const Value>) {
                auto* cf = hostCapsuleFieldOf(self_);
                return ev::fromDouble((cf && cf->field) ? static_cast<double>(cf->field->sphereCount()) : 0.0);
            }, nullptr);

            proto.accessor("cellSize", [](Value self_, std::span<const Value>) {
                auto* cf = hostCapsuleFieldOf(self_);
                return ev::fromDouble((cf && cf->field) ? static_cast<double>(cf->field->cellSize()) : 0.0);
            }, nullptr);

            proto.def("distance", 1, [](Value self_, std::span<const Value> a) {
                auto* cf = hostCapsuleFieldOf(self_);
                if (!cf || !cf->field || a.empty()) return ev::fromDouble(0.0);
                bromath::Vec3 p = readBmVec3(a[0]);
                int excludeTag = (a.size() > 1 && ev::isNumber(a[1])) ? static_cast<int>(ev::toDouble(a[1])) : -1;
                return ev::fromDouble(cf->field->distance(p, excludeTag));
            });

            proto.def("nearest", 1, [](Value self_, std::span<const Value> a) {
                auto* cf = hostCapsuleFieldOf(self_);
                if (!cf || !cf->field || a.empty()) return ev::null();
                bromath::Vec3 p = readBmVec3(a[0]);
                int excludeTag = (a.size() > 1 && ev::isNumber(a[1])) ? static_cast<int>(ev::toDouble(a[1])) : -1;
                auto n = cf->field->nearest(p, excludeTag);
                ObjectBuilder o;
                float ptv[3] = {n.point.x, n.point.y, n.point.z};
                float nmv[3] = {n.normal.x, n.normal.y, n.normal.z};
                o.set("point", hostArrayOf(3, [&ptv](size_t i) { return ev::fromDouble(ptv[i]); }));
                o.set("normal", hostArrayOf(3, [&nmv](size_t i) { return ev::fromDouble(nmv[i]); }));
                o.set("distance", ev::fromDouble(n.distance));
                o.set("tag", ev::fromDouble(n.tag));
                return o.get();
            });

            proto.def("intersectsSphere", 2, [](Value self_, std::span<const Value> a) {
                auto* cf = hostCapsuleFieldOf(self_);
                if (!cf || !cf->field || a.size() < 2) return ev::fromBool(false);
                bromath::Vec3 c = readBmVec3(a[0]);
                float r = static_cast<float>(ev::toDouble(a[1]));
                int excludeTag = (a.size() > 2 && ev::isNumber(a[2])) ? static_cast<int>(ev::toDouble(a[2])) : -1;
                return ev::fromBool(cf->field->intersectsSphere(c, r, excludeTag));
            });
        });
}

void decorateMeshAnalysis(ObjectBuilder& b, HostClass& cls) {
    // ── Bounding Box & Topological / Geometric Analysis ─────────────────────────
    b.def("computeBBox", 0, [](Value self_, std::span<const Value>) {
        auto* m = hostMeshDataOf(self_);
        if (!m) return ev::undefined();
        return makeBBox(bromesh::computeBBox(*m));
    });

    b.def("isManifold", 0, [](Value self_, std::span<const Value>) {
        auto* m = hostMeshDataOf(self_);
        return ev::fromBool(m ? bromesh::isManifold(*m) : false);
    });

    b.def("computeVolume", 0, [](Value self_, std::span<const Value>) {
        auto* m = hostMeshDataOf(self_);
        return ev::fromDouble(m ? static_cast<double>(bromesh::computeVolume(*m)) : 0.0);
    });

    b.def("surfaceArea", 0, [](Value self_, std::span<const Value>) {
        auto* m = hostMeshDataOf(self_);
        return ev::fromDouble(m ? static_cast<double>(bromesh::computeSurfaceArea(*m)) : 0.0);
    });

    b.def("triangleAreas", 0, [](Value self_, std::span<const Value>) {
        auto* m = hostMeshDataOf(self_);
        if (!m) return ev::undefined();
        auto areas = bromesh::computeTriangleAreas(*m);
        return makeFloat32Array(areas.data(), areas.size());
    });

    b.def("sampleSurface", 2, [](Value self_, std::span<const Value> a) {
        auto* m = hostMeshDataOf(self_);
        if (!m) return ev::undefined();
        int count = a.size() > 0 ? static_cast<int>(ev::toDouble(a[0])) : 100;
        int seed = a.size() > 1 ? static_cast<int>(ev::toDouble(a[1])) : 42;
        return wrapMesh(bromesh::sampleSurface(*m, count, seed));
    });

    // ── Ray Queries & Proximity ─────────────────────────────────────────────────
    b.def("raycast", 3, [](Value self_, std::span<const Value> a) {
        auto* m = hostMeshDataOf(self_);
        if (!m) return ev::null();
        float origin[3] = {0, 0, 0};
        float dir[3] = {0, 0, -1};
        if (a.size() > 0) readVec3(a[0], origin);
        if (a.size() > 1) readVec3(a[1], dir);
        float maxDist = (a.size() > 2 && ev::isNumber(a[2])) ? static_cast<float>(ev::toDouble(a[2])) : 0.0f;
        return makeRayHit(bromesh::raycast(*m, origin, dir, maxDist));
    });

    b.def("raycastAll", 3, [](Value self_, std::span<const Value> a) {
        auto* m = hostMeshDataOf(self_);
        if (!m) return makeEmptyArray();
        float origin[3] = {0, 0, 0};
        float dir[3] = {0, 0, -1};
        if (a.size() > 0) readVec3(a[0], origin);
        if (a.size() > 1) readVec3(a[1], dir);
        float maxDist = (a.size() > 2 && ev::isNumber(a[2])) ? static_cast<float>(ev::toDouble(a[2])) : 0.0f;
        auto hits = bromesh::raycastAll(*m, origin, dir, maxDist);
        return hostArrayOf(hits.size(), [&](size_t i) {
            return makeRayHit(hits[i]);
        });
    });

    b.def("raycastTest", 3, [](Value self_, std::span<const Value> a) {
        auto* m = hostMeshDataOf(self_);
        if (!m) return ev::fromBool(false);
        float origin[3] = {0, 0, 0};
        float dir[3] = {0, 0, -1};
        if (a.size() > 0) readVec3(a[0], origin);
        if (a.size() > 1) readVec3(a[1], dir);
        float maxDist = (a.size() > 2 && ev::isNumber(a[2])) ? static_cast<float>(ev::toDouble(a[2])) : 0.0f;
        return ev::fromBool(bromesh::raycastTest(*m, origin, dir, maxDist));
    });

    b.def("closestPoint", 1, [](Value self_, std::span<const Value> a) {
        auto* m = hostMeshDataOf(self_);
        if (!m || a.empty()) return ev::null();
        float pt[3] = {0, 0, 0};
        readVec3(a[0], pt);
        return makeRayHit(bromesh::closestPoint(*m, pt));
    });

    b.def("hasSelfIntersections", 0, [](Value self_, std::span<const Value>) {
        auto* m = hostMeshDataOf(self_);
        return ev::fromBool(m ? bromesh::hasSelfIntersections(*m) : false);
    });

    b.def("findSelfIntersections", 0, [](Value self_, std::span<const Value>) {
        auto* m = hostMeshDataOf(self_);
        if (!m) return makeEmptyArray();
        auto pairs = bromesh::findSelfIntersections(*m);
        return hostArrayOf(pairs.size(), [&](size_t i) {
            ObjectBuilder pairObj;
            pairObj.set("triA", ev::fromDouble(pairs[i].triA));
            pairObj.set("triB", ev::fromDouble(pairs[i].triB));
            return pairObj.get();
        });
    });

    b.def("intersectsMesh", 1, [](Value self_, std::span<const Value> a) {
        auto* ma = hostMeshDataOf(self_);
        if (!ma || a.empty()) return ev::fromBool(false);
        auto* mb = hostMeshDataOf(a[0]);
        if (!mb) return ev::fromBool(false);
        return ev::fromBool(bromesh::meshesIntersect(*ma, *mb));
    });

    // ── Vertex Baking ───────────────────────────────────────────────────────────
    b.def("bakeAmbientOcclusion", 2, [](Value self_, std::span<const Value> a) {
        auto* m = hostMeshDataOf(self_);
        if (!m) return self_;
        int numRays = a.size() > 0 ? static_cast<int>(ev::toDouble(a[0])) : 64;
        float maxDist = a.size() > 1 ? static_cast<float>(ev::toDouble(a[1])) : 0.0f;
        bromesh::bakeAmbientOcclusion(*m, numRays, maxDist);
        return self_;
    });

    b.def("bakeCurvature", 1, [](Value self_, std::span<const Value> a) {
        auto* m = hostMeshDataOf(self_);
        if (!m) return self_;
        float scale = a.size() > 0 ? static_cast<float>(ev::toDouble(a[0])) : 1.0f;
        bromesh::bakeCurvature(*m, scale);
        return self_;
    });

    b.def("bakeThickness", 2, [](Value self_, std::span<const Value> a) {
        auto* m = hostMeshDataOf(self_);
        if (!m) return self_;
        int numRays = a.size() > 0 ? static_cast<int>(ev::toDouble(a[0])) : 32;
        float maxDist = a.size() > 1 ? static_cast<float>(ev::toDouble(a[1])) : 0.0f;
        bromesh::bakeThickness(*m, numRays, maxDist);
        return self_;
    });

    // ── Texture Baking ──────────────────────────────────────────────────────────
    b.def("bakeAOToTexture", 4, [](Value self_, std::span<const Value> a) {
        auto* m = hostMeshDataOf(self_);
        if (!m || a.size() < 2) return ev::undefined();
        int w = static_cast<int>(ev::toDouble(a[0]));
        int h = static_cast<int>(ev::toDouble(a[1]));
        int numRays = a.size() > 2 ? static_cast<int>(ev::toDouble(a[2])) : 64;
        float maxDist = a.size() > 3 ? static_cast<float>(ev::toDouble(a[3])) : 0.0f;
        return makeTextureBuffer(bromesh::bakeAmbientOcclusionToTexture(*m, w, h, numRays, maxDist));
    });

    b.def("bakeCurvatureToTexture", 3, [](Value self_, std::span<const Value> a) {
        auto* m = hostMeshDataOf(self_);
        if (!m || a.size() < 2) return ev::undefined();
        int w = static_cast<int>(ev::toDouble(a[0]));
        int h = static_cast<int>(ev::toDouble(a[1]));
        float scale = a.size() > 2 ? static_cast<float>(ev::toDouble(a[2])) : 1.0f;
        return makeTextureBuffer(bromesh::bakeCurvatureToTexture(*m, w, h, scale));
    });

    b.def("bakeThicknessToTexture", 4, [](Value self_, std::span<const Value> a) {
        auto* m = hostMeshDataOf(self_);
        if (!m || a.size() < 2) return ev::undefined();
        int w = static_cast<int>(ev::toDouble(a[0]));
        int h = static_cast<int>(ev::toDouble(a[1]));
        int numRays = a.size() > 2 ? static_cast<int>(ev::toDouble(a[2])) : 32;
        float maxDist = a.size() > 3 ? static_cast<float>(ev::toDouble(a[3])) : 0.0f;
        return makeTextureBuffer(bromesh::bakeThicknessToTexture(*m, w, h, numRays, maxDist));
    });

    b.def("bakeNormalsToTexture", 2, [](Value self_, std::span<const Value> a) {
        auto* m = hostMeshDataOf(self_);
        if (!m || a.size() < 2) return ev::undefined();
        int w = static_cast<int>(ev::toDouble(a[0]));
        int h = static_cast<int>(ev::toDouble(a[1]));
        return makeTextureBuffer(bromesh::bakeNormalsToTexture(*m, w, h));
    });

    b.def("bakePositionToTexture", 2, [](Value self_, std::span<const Value> a) {
        auto* m = hostMeshDataOf(self_);
        if (!m || a.size() < 2) return ev::undefined();
        int w = static_cast<int>(ev::toDouble(a[0]));
        int h = static_cast<int>(ev::toDouble(a[1]));
        return makeTextureBuffer(bromesh::bakePositionToTexture(*m, w, h));
    });

    b.def("bakeNormalsFromReference", 4, [](Value self_, std::span<const Value> a) {
        auto* low = hostMeshDataOf(self_);
        if (!low || a.empty()) return ev::undefined();
        auto* ref = hostMeshDataOf(a[0]);
        if (!ref) return ev::throwTypeError("first argument must be reference Mesh");
        int w = a.size() > 1 ? static_cast<int>(ev::toDouble(a[1])) : 256;
        int h = a.size() > 2 ? static_cast<int>(ev::toDouble(a[2])) : 256;
        float searchDist = a.size() > 3 ? static_cast<float>(ev::toDouble(a[3])) : 0.0f;
        return makeTextureBuffer(bromesh::bakeNormalsFromReference(*low, *ref, w, h, searchDist));
    });

    b.def("bakeAOFromReference", 5, [](Value self_, std::span<const Value> a) {
        auto* low = hostMeshDataOf(self_);
        if (!low || a.empty()) return ev::undefined();
        auto* ref = hostMeshDataOf(a[0]);
        if (!ref) return ev::throwTypeError("first argument must be reference Mesh");
        int w = a.size() > 1 ? static_cast<int>(ev::toDouble(a[1])) : 256;
        int h = a.size() > 2 ? static_cast<int>(ev::toDouble(a[2])) : 256;
        int numRays = a.size() > 3 ? static_cast<int>(ev::toDouble(a[3])) : 64;
        float maxDist = a.size() > 4 ? static_cast<float>(ev::toDouble(a[4])) : 0.0f;
        return makeTextureBuffer(bromesh::bakeAOFromReference(*low, *ref, w, h, numRays, maxDist));
    });

    // ── Convex Analysis ─────────────────────────────────────────────────────────
    b.def("convexHull", 0, [](Value self_, std::span<const Value>) {
        auto* m = hostMeshDataOf(self_);
        if (!m) return ev::undefined();
        return wrapMesh(bromesh::convexHull(*m));
    });

    b.def("convexDecomposition", 1, [](Value self_, std::span<const Value> a) {
        auto* m = hostMeshDataOf(self_);
        if (!m) return makeEmptyArray();
        bromesh::ConvexDecompParams p;
        if (!a.empty() && ev::isObject(a[0])) {
            Value mhV = ev::getProperty(a[0], "maxHulls");
            if (ev::isNumber(mhV)) p.maxHulls = static_cast<int>(ev::toDouble(mhV));
            Value mvV = ev::getProperty(a[0], "maxVerticesPerHull");
            if (ev::isNumber(mvV)) p.maxVerticesPerHull = static_cast<int>(ev::toDouble(mvV));
            Value resV = ev::getProperty(a[0], "resolution");
            if (ev::isNumber(resV)) p.resolution = static_cast<float>(ev::toDouble(resV));
        }
        auto hulls = bromesh::convexDecomposition(*m, p);
        return hostArrayOf(hulls.size(), [&](size_t i) {
            return wrapMesh(std::move(hulls[i]));
        });
    });

    // ── UV Projections & Metrics ────────────────────────────────────────────────
    b.def("projectUVs", 2, [](Value self_, std::span<const Value> a) {
        auto* m = hostMeshDataOf(self_);
        if (!m || a.empty()) return self_;
        std::string type = ev::isString(a[0]) ? ev::toUtf8(a[0]) : "box";
        float scale = (a.size() > 1 && ev::isNumber(a[1])) ? static_cast<float>(ev::toDouble(a[1])) : 1.0f;
        auto pt = bromesh::ProjectionType::Box;
        if (type == "planarXY") pt = bromesh::ProjectionType::PlanarXY;
        else if (type == "planarXZ") pt = bromesh::ProjectionType::PlanarXZ;
        else if (type == "planarYZ") pt = bromesh::ProjectionType::PlanarYZ;
        else if (type == "cylindrical") pt = bromesh::ProjectionType::Cylindrical;
        else if (type == "spherical") pt = bromesh::ProjectionType::Spherical;
        bromesh::projectUVs(*m, pt, scale);
        return self_;
    });

    b.def("unwrapUVs", 1, [](Value self_, std::span<const Value> a) {
        auto* m = hostMeshDataOf(self_);
        if (!m) return ev::undefined();
        bromesh::UnwrapParams up;
        bromesh::PackParams pp;
        if (!a.empty() && ev::isObject(a[0])) {
            Value mcV = ev::getProperty(a[0], "maxChartCount");
            if (ev::isNumber(mcV)) up.maxChartCount = static_cast<int>(ev::toDouble(mcV));
            Value msV = ev::getProperty(a[0], "maxStretch");
            if (ev::isNumber(msV)) up.maxStretch = static_cast<float>(ev::toDouble(msV));
            Value resV = ev::getProperty(a[0], "resolution");
            if (ev::isNumber(resV)) pp.resolution = static_cast<int>(ev::toDouble(resV));
            Value padV = ev::getProperty(a[0], "padding");
            if (ev::isNumber(padV)) pp.padding = static_cast<int>(ev::toDouble(padV));
        }
        auto res = bromesh::unwrapUVs(*m, up, pp);
        ev::Persistent o(ev::createObject());
        ev::setProperty(o.get(), "success", ev::fromBool(res.success));
        ev::setProperty(o.get(), "atlasWidth", ev::fromDouble(res.atlasWidth));
        ev::setProperty(o.get(), "atlasHeight", ev::fromDouble(res.atlasHeight));
        ev::setProperty(o.get(), "chartCount", ev::fromDouble(res.chartCount));
        return o.get();
    });

    b.def("computeUVDistortion", 0, [](Value self_, std::span<const Value>) {
        auto* m = hostMeshDataOf(self_);
        if (!m) return makeEmptyArray();
        auto dist = bromesh::computeUVDistortion(*m);
        return hostArrayOf(dist.size(), [&](size_t i) {
            ObjectBuilder o;
            o.set("stretch", ev::fromDouble(dist[i].stretch));
            o.set("areaDistortion", ev::fromDouble(dist[i].areaDistortion));
            o.set("angleDistortion", ev::fromDouble(dist[i].angleDistortion));
            return o.get();
        });
    });

    b.def("measureUVQuality", 0, [](Value self_, std::span<const Value>) {
        auto* m = hostMeshDataOf(self_);
        if (!m) return ev::undefined();
        auto q = bromesh::measureUVQuality(*m);
        ev::Persistent o(ev::createObject());
        ev::setProperty(o.get(), "avgStretch", ev::fromDouble(q.avgStretch));
        ev::setProperty(o.get(), "maxStretch", ev::fromDouble(q.maxStretch));
        ev::setProperty(o.get(), "avgAreaDistortion", ev::fromDouble(q.avgAreaDistortion));
        ev::setProperty(o.get(), "maxAreaDistortion", ev::fromDouble(q.maxAreaDistortion));
        ev::setProperty(o.get(), "avgAngleDistortion", ev::fromDouble(q.avgAngleDistortion));
        ev::setProperty(o.get(), "maxAngleDistortion", ev::fromDouble(q.maxAngleDistortion));
        ev::setProperty(o.get(), "uvSpaceUsage", ev::fromDouble(q.uvSpaceUsage));
        ev::setProperty(o.get(), "triangleCount", ev::fromDouble(static_cast<double>(q.triangleCount)));
        return o.get();
    });

    // ── Static methods on Mesh ──────────────────────────────────────────────────
    cls.setStatic("capsuleField", ev::makeFunction([](Value, std::span<const Value> a) -> Value {
        std::vector<bromesh::Capsule> caps;
        std::vector<bromesh::Sphere> sphs;
        if (a.size() > 0) readCapsules(a[0], caps);
        if (a.size() > 1) readSpheres(a[1], sphs);
        float cellSize = (a.size() > 2 && ev::isNumber(a[2])) ? static_cast<float>(ev::toDouble(a[2])) : 0.0f;
        auto field = std::make_unique<bromesh::CapsuleField>(std::move(caps), std::move(sphs), cellSize);
        return wrapCapsuleField(std::move(field));
    }, 3));

    cls.setStatic("capsuleFieldFromSegments", ev::makeFunction([](Value, std::span<const Value> a) -> Value {
        if (a.empty()) return ev::throwTypeError("capsuleFieldFromSegments requires (segments[, radiusScale[, spheres]])");
        std::vector<bromesh::BranchSegment> segs;
        if (!readBranchSegments(a[0], segs))
            return ev::throwTypeError("segments must be a branch-segment array");
        float radiusScale = (a.size() > 1 && ev::isNumber(a[1])) ? static_cast<float>(ev::toDouble(a[1])) : 1.0f;
        std::vector<bromesh::Sphere> sphs;
        if (a.size() > 2) readSpheres(a[2], sphs);
        auto caps = bromesh::CapsuleField::capsulesFromSegments(segs, radiusScale);
        auto field = std::make_unique<bromesh::CapsuleField>(std::move(caps), std::move(sphs), 0.0f);
        return wrapCapsuleField(std::move(field));
    }, 3));

    cls.setStatic("packAnchors", ev::makeFunction([](Value, std::span<const Value> a) -> Value {
        if (a.empty()) return ev::throwTypeError("packAnchors requires (candidates[, opts])");
        std::vector<bromath::Vec3> cand;
        if (!readVec3List(a[0], cand))
            return ev::throwTypeError("candidates must be a Vec3 list");

        bromesh::AnchorPackOptions opts;
        const bromesh::CapsuleField* avoid = nullptr;
        std::vector<bromesh::Sphere> keepOut;
        if (a.size() > 1 && ev::isObject(a[1])) {
            Value msV = ev::getProperty(a[1], "minSpacing");
            if (ev::isNumber(msV)) opts.minSpacing = static_cast<float>(ev::toDouble(msV));
            Value modV = ev::getProperty(a[1], "minObstacleDistance");
            if (ev::isNumber(modV)) opts.minObstacleDistance = static_cast<float>(ev::toDouble(modV));
            Value mcV = ev::getProperty(a[1], "maxCount");
            if (ev::isNumber(mcV)) opts.maxCount = static_cast<int>(ev::toDouble(mcV));
            Value seedV = ev::getProperty(a[1], "seed");
            if (ev::isNumber(seedV)) opts.seed = static_cast<uint64_t>(ev::toDouble(seedV));
            avoid = readAvoidField(a[1], "avoid");
            Value ko = ev::getProperty(a[1], "keepOut");
            if (ev::isObject(ko)) readSpheres(ko, keepOut);
        }

        auto idx = bromesh::packAnchors(cand, avoid, keepOut, opts);
        return makeInt32Array(idx.data(), idx.size());
    }, 2));
}

}  // namespace bro::bronze_host

#endif  // BRO_WITH_3D
