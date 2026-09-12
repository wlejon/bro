#if BRO_WITH_3D

#include "bronze_host/host_mesh_internal.h"
#include "bronze_host/host_rigging_internal.h"

#include <bromesh/mesh_data.h>
#include <bromesh/manipulation/normals.h>
#include <bromesh/manipulation/transform.h>
#include <bromesh/manipulation/merge.h>
#include <bromesh/manipulation/subdivide.h>
#include <bromesh/manipulation/weld.h>
#include <bromesh/manipulation/smooth.h>
#include <bromesh/manipulation/remesh.h>
#include <bromesh/manipulation/repair.h>
#include <bromesh/manipulation/split_components.h>
#include <bromesh/manipulation/polygon.h>
#include <bromesh/manipulation/shrinkwrap.h>
#include <bromesh/manipulation/skin.h>
#include <bromesh/csg/boolean.h>

#include <cmath>
#include <vector>

namespace bro::bronze_host {

void decorateMeshManipulate(ObjectBuilder& b, HostClass& cls) {
    // ── Transform methods on prototype ──────────────────────────────────────────
    b.def("translate", 3, [](Value self_, std::span<const Value> a) {
        auto* m = hostMeshDataOf(self_);
        if (!m) return self_;
        float dx = a.size() > 0 ? static_cast<float>(ev::toDouble(a[0])) : 0.0f;
        float dy = a.size() > 1 ? static_cast<float>(ev::toDouble(a[1])) : 0.0f;
        float dz = a.size() > 2 ? static_cast<float>(ev::toDouble(a[2])) : 0.0f;
        bromesh::translateMesh(*m, dx, dy, dz);
        return self_;
    });

    b.def("scale", 1, [](Value self_, std::span<const Value> a) {
        auto* m = hostMeshDataOf(self_);
        if (!m || a.empty()) return self_;
        float sx = static_cast<float>(ev::toDouble(a[0]));
        float sy = a.size() > 1 ? static_cast<float>(ev::toDouble(a[1])) : sx;
        float sz = a.size() > 2 ? static_cast<float>(ev::toDouble(a[2])) : sx;
        bromesh::scaleMesh(*m, sx, sy, sz);
        return self_;
    });

    b.def("rotate", 4, [](Value self_, std::span<const Value> a) {
        auto* m = hostMeshDataOf(self_);
        if (!m) return self_;
        float ax = a.size() > 0 ? static_cast<float>(ev::toDouble(a[0])) : 0.0f;
        float ay = a.size() > 1 ? static_cast<float>(ev::toDouble(a[1])) : 1.0f;
        float az = a.size() > 2 ? static_cast<float>(ev::toDouble(a[2])) : 0.0f;
        float angle = a.size() > 3 ? static_cast<float>(ev::toDouble(a[3])) : 0.0f;
        bromesh::rotateMesh(*m, ax, ay, az, angle);
        return self_;
    });

    b.def("center", 0, [](Value self_, std::span<const Value>) {
        auto* m = hostMeshDataOf(self_);
        if (m) bromesh::centerMesh(*m);
        return self_;
    });

    b.def("mirror", 1, [](Value self_, std::span<const Value> a) {
        auto* m = hostMeshDataOf(self_);
        if (!m) return self_;
        int axis = a.size() > 0 ? static_cast<int>(ev::toDouble(a[0])) : 0;
        bromesh::mirrorMesh(*m, axis);
        return self_;
    });

    b.def("transform", 1, [](Value self_, std::span<const Value> a) {
        auto* m = hostMeshDataOf(self_);
        if (!m || a.empty()) return self_;
        std::vector<float> mat;
        if (readFloatVector(a[0], mat) && mat.size() >= 16) {
            bromesh::transformMesh(*m, mat.data());
        }
        return self_;
    });

    // ── Normals & Tangents ──────────────────────────────────────────────────────
    b.def("computeNormals", 0, [](Value self_, std::span<const Value>) {
        auto* m = hostMeshDataOf(self_);
        if (m) bromesh::computeNormals(*m);
        return self_;
    });

    b.def("computeFlatNormals", 0, [](Value self_, std::span<const Value>) {
        auto* m = hostMeshDataOf(self_);
        if (!m) return ev::undefined();
        return wrapMesh(bromesh::computeFlatNormals(*m));
    });

    b.def("computeTangents", 0, [](Value self_, std::span<const Value>) {
        auto* m = hostMeshDataOf(self_);
        if (!m) return ev::undefined();
        auto tangents = bromesh::computeTangents(*m);
        return makeFloat32Array(tangents.data(), tangents.size());
    });

    b.def("computeCreaseNormals", 1, [](Value self_, std::span<const Value> a) {
        auto* m = hostMeshDataOf(self_);
        if (!m) return self_;
        float angleDeg = a.size() > 0 ? static_cast<float>(ev::toDouble(a[0])) : 30.0f;
        *m = bromesh::computeCreaseNormals(*m, angleDeg);
        return self_;
    });

    // ── Subdivision ─────────────────────────────────────────────────────────────
    b.def("subdivideLoop", 1, [](Value self_, std::span<const Value> a) {
        auto* m = hostMeshDataOf(self_);
        if (!m) return self_;
        int iter = a.size() > 0 ? static_cast<int>(ev::toDouble(a[0])) : 1;
        *m = bromesh::subdivideLoop(*m, iter);
        return self_;
    });

    b.def("subdivideCatmullClark", 1, [](Value self_, std::span<const Value> a) {
        auto* m = hostMeshDataOf(self_);
        if (!m) return self_;
        int iter = a.size() > 0 ? static_cast<int>(ev::toDouble(a[0])) : 1;
        *m = bromesh::subdivideCatmullClark(*m, iter);
        return self_;
    });

    b.def("subdivideMidpoint", 1, [](Value self_, std::span<const Value> a) {
        auto* m = hostMeshDataOf(self_);
        if (!m) return self_;
        int iter = a.size() > 0 ? static_cast<int>(ev::toDouble(a[0])) : 1;
        *m = bromesh::subdivideMidpoint(*m, iter);
        return self_;
    });

    // ── Smoothing ───────────────────────────────────────────────────────────────
    b.def("smoothLaplacian", 2, [](Value self_, std::span<const Value> a) {
        auto* m = hostMeshDataOf(self_);
        if (!m) return self_;
        float lambda = a.size() > 0 ? static_cast<float>(ev::toDouble(a[0])) : 0.5f;
        int iter = a.size() > 1 ? static_cast<int>(ev::toDouble(a[1])) : 1;
        bromesh::smoothLaplacian(*m, lambda, iter);
        return self_;
    });

    b.def("smoothTaubin", 3, [](Value self_, std::span<const Value> a) {
        auto* m = hostMeshDataOf(self_);
        if (!m) return self_;
        float lambda = a.size() > 0 ? static_cast<float>(ev::toDouble(a[0])) : 0.5f;
        float mu = a.size() > 1 ? static_cast<float>(ev::toDouble(a[1])) : -0.53f;
        int iter = a.size() > 2 ? static_cast<int>(ev::toDouble(a[2])) : 1;
        bromesh::smoothTaubin(*m, lambda, mu, iter);
        return self_;
    });

    // ── Repair & Remesh ─────────────────────────────────────────────────────────
    b.def("weld", 1, [](Value self_, std::span<const Value> a) {
        auto* m = hostMeshDataOf(self_);
        if (!m) return self_;
        float eps = a.size() > 0 ? static_cast<float>(ev::toDouble(a[0])) : 1e-5f;
        *m = bromesh::weldVertices(*m, eps);
        return self_;
    });

    b.def("removeDegenerateTriangles", 1, [](Value self_, std::span<const Value> a) {
        auto* m = hostMeshDataOf(self_);
        if (!m) return self_;
        float eps = a.size() > 0 ? static_cast<float>(ev::toDouble(a[0])) : 1e-8f;
        *m = bromesh::removeDegenerateTriangles(*m, eps);
        return self_;
    });

    b.def("removeDuplicateTriangles", 0, [](Value self_, std::span<const Value>) {
        auto* m = hostMeshDataOf(self_);
        if (m) *m = bromesh::removeDuplicateTriangles(*m);
        return self_;
    });

    b.def("fillHoles", 1, [](Value self_, std::span<const Value> a) {
        auto* m = hostMeshDataOf(self_);
        if (!m) return self_;
        int maxEdges = a.size() > 0 ? static_cast<int>(ev::toDouble(a[0])) : 64;
        *m = bromesh::fillHoles(*m, maxEdges);
        return self_;
    });

    b.def("remeshIsotropic", 2, [](Value self_, std::span<const Value> a) {
        auto* m = hostMeshDataOf(self_);
        if (!m) return self_;
        float edgeLen = a.size() > 0 ? static_cast<float>(ev::toDouble(a[0])) : 0.0f;
        int iter = a.size() > 1 ? static_cast<int>(ev::toDouble(a[1])) : 5;
        *m = bromesh::remeshIsotropic(*m, edgeLen, iter);
        return self_;
    });

    b.def("splitComponents", 0, [](Value self_, std::span<const Value>) {
        auto* m = hostMeshDataOf(self_);
        if (!m) return makeEmptyArray();
        auto comps = bromesh::splitConnectedComponents(*m);
        return hostArrayOf(comps.size(), [&](size_t i) {
            return wrapMesh(std::move(comps[i]));
        });
    });

    b.def("shrinkwrap", 1, [](Value self_, std::span<const Value> a) {
        auto* m = hostMeshDataOf(self_);
        if (!m) return self_;
        if (a.empty() || !ev::isObject(a[0]))
            return ev::throwTypeError("shrinkwrap requires a target Mesh");
        auto* target = hostMeshDataOf(a[0]);
        if (!target) return ev::throwTypeError("shrinkwrap: target must be a Mesh");

        auto mode = bromesh::ShrinkwrapMode::Nearest;
        if (a.size() > 1 && ev::isString(a[1])) {
            std::string s = ev::toUtf8(a[1]);
            if (s == "projectAlongNormal") mode = bromesh::ShrinkwrapMode::ProjectAlongNormal;
            else if (s == "projectAlongAxis") mode = bromesh::ShrinkwrapMode::ProjectAlongAxis;
        }

        float maxDist = (a.size() > 2 && ev::isNumber(a[2])) ? static_cast<float>(ev::toDouble(a[2])) : 0.0f;
        float offset = (a.size() > 3 && ev::isNumber(a[3])) ? static_cast<float>(ev::toDouble(a[3])) : 0.0f;
        float axis[3] = {0, 1, 0};
        const float* axisPtr = nullptr;
        if (a.size() > 4 && !ev::isNull(a[4]) && !ev::isUndefined(a[4])) {
            if (readVec3(a[4], axis)) axisPtr = axis;
        }

        bromesh::shrinkwrap(*m, *target, mode, maxDist, offset, axisPtr);
        return self_;
    });

    // ── Skinning & Morph Targets ────────────────────────────────────────────────
    b.def("applySkinning", 2, [](Value self_, std::span<const Value> a) {
        auto* m = hostMeshDataOf(self_);
        if (!m) return ev::throwTypeError("neutered Mesh");
        if (a.size() < 2) return ev::throwTypeError("applySkinning requires (SkinData, Float32Array)");
        auto* skin = hostSkinDataOf(a[0]);
        if (!skin) return ev::throwTypeError("first argument must be a SkinData");
        std::vector<float> mats;
        if (!readFloatVector(a[1], mats))
            return ev::throwTypeError("second argument must be a Float32Array of pose matrices");
        size_t needed = skin->boneCount * 16;
        if (mats.size() < needed)
            return ev::throwRangeError("pose matrices length too small");
        bromesh::applySkinning(*m, *skin, mats.data());
        return self_;
    });

    b.def("applyMorphTarget", 2, [](Value self_, std::span<const Value> a) {
        auto* m = hostMeshDataOf(self_);
        if (!m) return ev::throwTypeError("neutered Mesh");
        if (a.empty() || !ev::isObject(a[0]))
            return ev::throwTypeError("applyMorphTarget: morph target must be an object");
        double weight = (a.size() > 1 && ev::isNumber(a[1])) ? ev::toDouble(a[1]) : 1.0;
        if (std::abs(weight) < 1e-9) return self_;

        bromesh::MorphTarget mt;
        Value nameV = ev::getProperty(a[0], "name");
        if (ev::isString(nameV)) mt.name = ev::toUtf8(nameV);

        Value dpVal = ev::getProperty(a[0], "deltaPositions");
        if (!ev::isUndefined(dpVal) && !ev::isNull(dpVal)) {
            readFloatVector(dpVal, mt.deltaPositions);
        }
        Value dnVal = ev::getProperty(a[0], "deltaNormals");
        if (!ev::isUndefined(dnVal) && !ev::isNull(dnVal)) {
            readFloatVector(dnVal, mt.deltaNormals);
        }

        bromesh::applyMorphTarget(*m, mt, static_cast<float>(weight));
        return self_;
    });

    // ── Static methods on Mesh class ────────────────────────────────────────────
    cls.setStatic("union", ev::makeFunction([](Value, std::span<const Value> a) -> Value {
        if (a.size() < 2) return ev::throwTypeError("union requires two Mesh instances");
        auto* ma = hostMeshDataOf(a[0]);
        auto* mb = hostMeshDataOf(a[1]);
        if (!ma || !mb) return ev::throwTypeError("arguments must be Mesh instances");
        return wrapMesh(bromesh::booleanUnion(*ma, *mb));
    }, 2));

    cls.setStatic("subtract", ev::makeFunction([](Value, std::span<const Value> a) -> Value {
        if (a.size() < 2) return ev::throwTypeError("subtract requires two Mesh instances");
        auto* ma = hostMeshDataOf(a[0]);
        auto* mb = hostMeshDataOf(a[1]);
        if (!ma || !mb) return ev::throwTypeError("arguments must be Mesh instances");
        return wrapMesh(bromesh::booleanDifference(*ma, *mb));
    }, 2));

    cls.setStatic("intersect", ev::makeFunction([](Value, std::span<const Value> a) -> Value {
        if (a.size() < 2) return ev::throwTypeError("intersect requires two Mesh instances");
        auto* ma = hostMeshDataOf(a[0]);
        auto* mb = hostMeshDataOf(a[1]);
        if (!ma || !mb) return ev::throwTypeError("arguments must be Mesh instances");
        return wrapMesh(bromesh::booleanIntersection(*ma, *mb));
    }, 2));

    cls.setStatic("splitByPlane", ev::makeFunction([](Value, std::span<const Value> a) -> Value {
        if (a.empty()) return ev::throwTypeError("splitByPlane requires a Mesh argument");
        auto* m = hostMeshDataOf(a[0]);
        if (!m) return ev::throwTypeError("first argument must be a Mesh");
        float nx = a.size() > 1 ? static_cast<float>(ev::toDouble(a[1])) : 0.0f;
        float ny = a.size() > 2 ? static_cast<float>(ev::toDouble(a[2])) : 1.0f;
        float nz = a.size() > 3 ? static_cast<float>(ev::toDouble(a[3])) : 0.0f;
        float offset = a.size() > 4 ? static_cast<float>(ev::toDouble(a[4])) : 0.0f;
        auto res = bromesh::splitByPlane(*m, nx, ny, nz, offset);
        return hostArrayOf(2, [&](size_t i) {
            return (i == 0) ? wrapMesh(std::move(res.first)) : wrapMesh(std::move(res.second));
        });
    }, 5));

    cls.setStatic("polygon2D", ev::makeFunction([](Value, std::span<const Value> a) -> Value {
        if (a.empty()) return ev::throwTypeError("polygon2D requires (outer[, holes[, z]])");
        std::vector<float> outer;
        if (!readFloatVector(a[0], outer))
            return ev::throwTypeError("outer must be Float32Array or number[]");
        std::vector<std::vector<float>> holes;
        if (a.size() > 1 && ev::isObject(a[1])) {
            Value lenV = ev::getProperty(a[1], "length");
            if (ev::isNumber(lenV)) {
                size_t len = static_cast<size_t>(ev::toDouble(lenV));
                holes.reserve(len);
                for (size_t i = 0; i < len; ++i) {
                    std::vector<float> h;
                    readFloatVector(ev::getElement(a[1], static_cast<uint32_t>(i)), h);
                    holes.push_back(std::move(h));
                }
            }
        }
        float z = a.size() > 2 ? static_cast<float>(ev::toDouble(a[2])) : 0.0f;
        return wrapMesh(bromesh::triangulatePolygon2D(outer, holes, z));
    }, 3));

    cls.setStatic("polygon3D", ev::makeFunction([](Value, std::span<const Value> a) -> Value {
        if (a.size() < 3) return ev::throwTypeError("polygon3D requires (outer, holes, normal)");
        std::vector<float> outer;
        if (!readFloatVector(a[0], outer))
            return ev::throwTypeError("outer must be Float32Array or number[]");
        std::vector<std::vector<float>> holes;
        if (ev::isObject(a[1])) {
            Value lenV = ev::getProperty(a[1], "length");
            if (ev::isNumber(lenV)) {
                size_t len = static_cast<size_t>(ev::toDouble(lenV));
                holes.reserve(len);
                for (size_t i = 0; i < len; ++i) {
                    std::vector<float> h;
                    readFloatVector(ev::getElement(a[1], static_cast<uint32_t>(i)), h);
                    holes.push_back(std::move(h));
                }
            }
        }
        float normal[3] = {0, 0, 1};
        readVec3(a[2], normal);
        return wrapMesh(bromesh::triangulatePolygon3D(outer, holes, normal));
    }, 3));

    cls.setStatic("merge", ev::makeFunction([](Value, std::span<const Value> a) -> Value {
        if (a.empty() || !ev::isObject(a[0]))
            return ev::throwTypeError("merge requires an array of Mesh instances");
        Value lenV = ev::getProperty(a[0], "length");
        if (!ev::isNumber(lenV))
            return ev::throwTypeError("merge requires an array of Mesh instances");
        size_t len = static_cast<size_t>(ev::toDouble(lenV));
        std::vector<bromesh::MeshData> meshes;
        meshes.reserve(len);
        for (size_t i = 0; i < len; ++i) {
            Value elem = ev::getElement(a[0], static_cast<uint32_t>(i));
            auto* m = hostMeshDataOf(elem);
            if (m) meshes.push_back(*m);
        }
        return wrapMesh(bromesh::mergeMeshes(meshes));
    }, 1));
}

}  // namespace bro::bronze_host

#endif  // BRO_WITH_3D
