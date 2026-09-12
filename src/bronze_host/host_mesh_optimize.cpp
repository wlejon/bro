#if BRO_WITH_3D

#include "bronze_host/host_mesh_internal.h"
#include "bronze_host/host_rigging_internal.h"
#include "util/asset_path.h"

#include <bromesh/mesh_data.h>
#include <bromesh/manipulation/simplify.h>
#include <bromesh/optimization/optimize.h>
#include <bromesh/optimization/analyze.h>
#include <bromesh/optimization/meshlets.h>
#include <bromesh/optimization/strips.h>
#include <bromesh/optimization/encode.h>
#include <bromesh/optimization/progressive.h>
#include <bromesh/optimization/spatial.h>
#include <bromesh/isosurface/marching_cubes.h>
#include <bromesh/isosurface/dual_contouring.h>
#include <bromesh/isosurface/surface_nets.h>
#include <bromesh/isosurface/transvoxel.h>
#include <bromesh/voxel/greedy_mesh.h>
#include <bromesh/io/gltf.h>
#include <bromesh/io/obj.h>
#include <bromesh/io/fbx.h>
#include <bromesh/io/ply.h>
#include <bromesh/io/stl.h>
#include <bromesh/io/vox.h>
#include <bromesh/io/splat_ply.h>
#include <bromesh/gaussian_splat.h>
#include <bromesh/reconstruction/reconstruct.h>
#include <bromesh/manipulation/poly_mesh.h>
#include <bromesh/procedural/lsystem.h>
#include <bromesh/procedural/branches.h>
#include <bromesh/procedural/lsystem_turtle.h>

#include <cmath>
#include <vector>

namespace bro::bronze_host {

HostClass g_progressiveMeshClass;
HostClass g_polyMeshClass;
HostClass g_lSystemClass;

static void hostProgressiveMeshDtor(void* p) {
    delete static_cast<HostProgressiveMesh*>(p);
}

Value wrapProgressiveMesh(bromesh::ProgressiveMesh&& pm) {
    auto* cell = new HostProgressiveMesh();
    cell->pm = std::make_unique<bromesh::ProgressiveMesh>(std::move(pm));
    return g_progressiveMeshClass.make(cell, hostProgressiveMeshDtor);
}

void installProgressiveMesh() {
    g_progressiveMeshClass.install(
        "ProgressiveMesh", 1,
        [](Value, std::span<const Value> a) -> Value {
            if (a.empty()) return ev::throwTypeError("ProgressiveMesh requires a Mesh");
            auto* m = hostMeshDataOf(a[0]);
            if (!m) return ev::throwTypeError("argument must be a Mesh");
            auto pm = bromesh::buildProgressiveMesh(*m);
            return wrapProgressiveMesh(std::move(pm));
        },
        [](ObjectBuilder& proto) {
            proto.accessor("maxTriangles", [](Value self_, std::span<const Value>) {
                auto* w = hostProgressiveMeshOf(self_);
                return ev::fromDouble((w && w->pm) ? static_cast<double>(w->pm->maxTriangles()) : 0.0);
            }, nullptr);

            proto.accessor("minTriangles", [](Value self_, std::span<const Value>) {
                auto* w = hostProgressiveMeshOf(self_);
                return ev::fromDouble((w && w->pm) ? static_cast<double>(w->pm->minTriangles()) : 0.0);
            }, nullptr);

            proto.def("atRatio", 1, [](Value self_, std::span<const Value> a) {
                auto* w = hostProgressiveMeshOf(self_);
                if (!w || !w->pm || a.empty()) return ev::undefined();
                float r = static_cast<float>(ev::toDouble(a[0]));
                return wrapMesh(bromesh::progressiveMeshAtRatio(*w->pm, r));
            });

            proto.def("atTriangleCount", 1, [](Value self_, std::span<const Value> a) {
                auto* w = hostProgressiveMeshOf(self_);
                if (!w || !w->pm || a.empty()) return ev::undefined();
                size_t count = static_cast<size_t>(ev::toDouble(a[0]));
                return wrapMesh(bromesh::progressiveMeshAtTriangleCount(*w->pm, count));
            });

            proto.def("serialize", 0, [](Value self_, std::span<const Value>) {
                auto* w = hostProgressiveMeshOf(self_);
                if (!w || !w->pm) return ev::createTypedArray(ev::elements::Uint8, 0);
                auto bytes = bromesh::serializeProgressiveMesh(*w->pm);
                return makeUint8Array(bytes);
            });
        });

    g_progressiveMeshClass.setStatic("deserialize", ev::makeFunction([](Value, std::span<const Value> a) -> Value {
        if (a.empty()) return ev::throwTypeError("deserialize requires Uint8Array");
        std::vector<uint8_t> bytes;
        if (!readU8Vector(a[0], bytes)) return ev::throwTypeError("deserialize requires Uint8Array");
        auto pm = bromesh::deserializeProgressiveMesh(bytes.data(), bytes.size());
        return wrapProgressiveMesh(std::move(pm));
    }, 1));
}

static void hostPolyMeshDtor(void* p) {
    delete static_cast<HostPolyMesh*>(p);
}

Value wrapPolyMesh(std::unique_ptr<bromesh::PolyMesh> pm) {
    if (!pm) return ev::undefined();
    auto* cell = new HostPolyMesh();
    cell->pm = std::move(pm);
    return g_polyMeshClass.make(cell, hostPolyMeshDtor);
}

void installPolyMesh() {
    g_polyMeshClass.install(
        "PolyMesh", 0,
        [](Value, std::span<const Value>) -> Value {
            return wrapPolyMesh(std::make_unique<bromesh::PolyMesh>());
        },
        [](ObjectBuilder& proto) {
            proto.accessor("vertexCount", [](Value self_, std::span<const Value>) {
                auto* w = hostPolyMeshOf(self_);
                return ev::fromDouble((w && w->pm) ? static_cast<double>(w->pm->vertexCount()) : 0.0);
            }, nullptr);

            proto.accessor("faceCount", [](Value self_, std::span<const Value>) {
                auto* w = hostPolyMeshOf(self_);
                return ev::fromDouble((w && w->pm) ? static_cast<double>(w->pm->faceCount()) : 0.0);
            }, nullptr);

            proto.accessor("halfEdgeCount", [](Value self_, std::span<const Value>) {
                auto* w = hostPolyMeshOf(self_);
                return ev::fromDouble((w && w->pm) ? static_cast<double>(w->pm->halfEdgeCount()) : 0.0);
            }, nullptr);

            proto.def("toMesh", 0, [](Value self_, std::span<const Value>) -> Value {
                auto* w = hostPolyMeshOf(self_);
                if (!w || !w->pm) return ev::undefined();
                auto t = w->pm->tessellate();
                auto md = std::make_unique<bromesh::MeshData>();
                md->positions = std::move(t.positions);
                md->normals = std::move(t.normals);
                md->indices = std::move(t.indices);
                return wrapMesh(std::move(md));
            });

    proto.def("faceVertexCount", 1, [](Value self_, std::span<const Value> a) {
        auto* w = hostPolyMeshOf(self_);
        if (!w || !w->pm || a.empty()) return ev::fromDouble(0.0);
        int faceIdx = static_cast<int>(ev::toDouble(a[0]));
        return ev::fromDouble(w->pm->faceVertexCount(faceIdx));
    });

    proto.def("faceVertices", 1, [](Value self_, std::span<const Value> a) {
        auto* w = hostPolyMeshOf(self_);
        if (!w || !w->pm || a.empty()) return makeEmptyArray();
        int faceIdx = static_cast<int>(ev::toDouble(a[0]));
        auto vs = w->pm->faceVertices(faceIdx);
        return hostArrayOf(vs.size(), [&](size_t i) {
            return ev::fromDouble(vs[i]);
        });
    });

    proto.def("getVertex", 1, [](Value self_, std::span<const Value> a) {
        auto* w = hostPolyMeshOf(self_);
        if (!w || !w->pm || a.empty()) return ev::null();
        int vi = static_cast<int>(ev::toDouble(a[0]));
        float p[3] = {0, 0, 0};
        w->pm->getVertex(vi, p);
        float pv[3] = {p[0], p[1], p[2]};
        return hostArrayOf(3, [&pv](size_t i) { return ev::fromDouble(pv[i]); });
    });

    proto.def("computeFaceNormal", 1, [](Value self_, std::span<const Value> a) {
        auto* w = hostPolyMeshOf(self_);
        if (!w || !w->pm || a.empty()) return ev::null();
        int faceIdx = static_cast<int>(ev::toDouble(a[0]));
        float n[3] = {0, 0, 1};
        w->pm->computeFaceNormal(faceIdx, n);
        float nv[3] = {n[0], n[1], n[2]};
        return hostArrayOf(3, [&nv](size_t i) { return ev::fromDouble(nv[i]); });
    });

    proto.def("translateVertex", 2, [](Value self_, std::span<const Value> a) {
        auto* w = hostPolyMeshOf(self_);
        if (w && w->pm && a.size() >= 2) {
            int vi = static_cast<int>(ev::toDouble(a[0]));
            float o[3] = {0, 0, 0};
            readVec3(a[1], o);
            w->pm->translateVertex(vi, o);
        }
        return self_;
    });

    proto.def("translateFace", 2, [](Value self_, std::span<const Value> a) {
        auto* w = hostPolyMeshOf(self_);
        if (w && w->pm && a.size() >= 2) {
            int faceIdx = static_cast<int>(ev::toDouble(a[0]));
            float o[3] = {0, 0, 0};
            readVec3(a[1], o);
            w->pm->translateFace(faceIdx, o);
        }
        return self_;
    });

    proto.def("translateFaceWithRing", 2, [](Value self_, std::span<const Value> a) {
        auto* w = hostPolyMeshOf(self_);
        if (w && w->pm && a.size() >= 2) {
            int faceIdx = static_cast<int>(ev::toDouble(a[0]));
            float o[3] = {0, 0, 0};
            readVec3(a[1], o);
            w->pm->translateFaceWithRing(faceIdx, o);
        }
        return self_;
    });

    proto.def("extrudeFace", 5, [](Value self_, std::span<const Value> a) {
        auto* w = hostPolyMeshOf(self_);
        if (!w || !w->pm || a.size() < 2) return ev::null();
        int faceIdx = static_cast<int>(ev::toDouble(a[0]));
        float o[3] = {0, 0, 0};
        readVec3(a[1], o);
        bool withBack = (a.size() > 2 && !ev::isUndefined(a[2])) ? ev::toBool(a[2]) : true;
        int bridgeGroup = (a.size() > 3 && ev::isNumber(a[3])) ? static_cast<int>(ev::toDouble(a[3])) : -1;
        int backGroup = (a.size() > 4 && ev::isNumber(a[4])) ? static_cast<int>(ev::toDouble(a[4])) : -1;
        auto res = w->pm->extrudeFace(faceIdx, o, withBack, bridgeGroup, backGroup);
        ObjectBuilder obj;
        obj.set("backFace", ev::fromDouble(res.backFace));
        obj.set("bridgeFaces", hostArrayOf(res.bridgeFaces.size(), [&res](size_t i) {
            return ev::fromDouble(res.bridgeFaces[i]);
        }));
        obj.set("dupVerts", hostArrayOf(res.dupVerts.size(), [&res](size_t i) {
            return ev::fromDouble(res.dupVerts[i]);
        }));
        return obj.get();
    });

    proto.def("mergeFacesByGroup", 0, [](Value self_, std::span<const Value>) {
        auto* w = hostPolyMeshOf(self_);
        if (w && w->pm) w->pm->mergeFacesByGroup();
        return self_;
    });

    proto.def("compact", 0, [](Value self_, std::span<const Value>) {
        auto* w = hostPolyMeshOf(self_);
        if (w && w->pm) w->pm->compact();
        return self_;
    });

    proto.def("findGroupBoundary", 1, [](Value self_, std::span<const Value> a) {
        auto* w = hostPolyMeshOf(self_);
        if (!w || !w->pm || a.empty()) return makeEmptyArray();
        int groupId = static_cast<int>(ev::toDouble(a[0]));
        auto loops = w->pm->findGroupBoundary(groupId);
        return hostArrayOf(loops.size(), [&](size_t i) -> Value {
            const auto& loop = loops[i];
            return hostArrayOf(loop.size(), [&loop](size_t j) -> Value {
                return ev::fromDouble(loop[j]);
            });
        });
    });
    });

    g_polyMeshClass.setStatic("fromMeshData", ev::makeFunction([](Value, std::span<const Value> a) -> Value {
        if (a.size() < 2) return ev::throwTypeError("fromMeshData requires (positions, indices[, triToGroup])");
        std::vector<float> positions;
        std::vector<uint32_t> indices;
        std::vector<int32_t> triToGroup;
        if (!readFloatVector(a[0], positions)) return ev::throwTypeError("positions must be Float32Array");
        if (auto info = ev::typedArrayInfo(a[1])) {
            const uint32_t* p = reinterpret_cast<const uint32_t*>(info.data);
            indices.assign(p, p + info.elementCount);
        }
        if (a.size() > 2 && ev::isObject(a[2])) {
            if (auto info = ev::typedArrayInfo(a[2])) {
                const int32_t* p = reinterpret_cast<const int32_t*>(info.data);
                triToGroup.assign(p, p + info.elementCount);
            }
        }
        auto pm = std::make_unique<bromesh::PolyMesh>(bromesh::PolyMesh::fromMeshData(positions, indices, triToGroup));
        return wrapPolyMesh(std::move(pm));
    }, 3));

    g_polyMeshClass.setStatic("fromPolygon", ev::makeFunction([](Value, std::span<const Value> a) -> Value {
        if (a.size() < 2) return ev::throwTypeError("fromPolygon requires (positionsXYZ, normal[, group])");
        std::vector<float> positions;
        if (!readFloatVector(a[0], positions)) return ev::throwTypeError("positions must be Float32Array");
        float normal[3] = {0, 0, 1};
        readVec3(a[1], normal);
        int group = (a.size() > 2 && ev::isNumber(a[2])) ? static_cast<int>(ev::toDouble(a[2])) : 0;
        auto pm = std::make_unique<bromesh::PolyMesh>(bromesh::PolyMesh::fromPolygon(positions, normal, group));
        return wrapPolyMesh(std::move(pm));
    }, 3));
}

static void hostLSystemDtor(void* p) {
    delete static_cast<HostLSystem*>(p);
}

void installLSystem() {
    g_lSystemClass.install(
        "LSystem", 0,
        [](Value, std::span<const Value> a) -> Value {
            auto* cell = new HostLSystem();
            if (!a.empty() && ev::isString(a[0])) {
                std::string s = ev::toUtf8(a[0]);
                cell->axiom = bromesh::parseModules(s);
                cell->ls->setAxiom(cell->axiom);
            }
            return g_lSystemClass.make(cell, hostLSystemDtor);
        },
        [](ObjectBuilder& proto) {
            proto.def("setAxiom", 1, [](Value self_, std::span<const Value> a) {
                auto* w = hostLSystemOf(self_);
                if (w && !a.empty() && ev::isString(a[0])) {
                    std::string text = ev::toUtf8(a[0]);
                    w->axiom = bromesh::parseModules(text);
                    w->ls->setAxiom(w->axiom);
                }
                return self_;
            });

            proto.def("addRule", 3, [](Value self_, std::span<const Value> a) {
                auto* w = hostLSystemOf(self_);
                if (w && a.size() >= 2 && ev::isString(a[0]) && ev::isString(a[1])) {
                    std::string pred = ev::toUtf8(a[0]);
                    std::string succ = ev::toUtf8(a[1]);
                    if (!pred.empty()) {
                        bromesh::ProductionRule rule;
                        rule.predecessor = pred[0];
                        rule.weight = (a.size() > 2 && ev::isNumber(a[2])) ? static_cast<float>(ev::toDouble(a[2])) : 1.0f;
                        auto mods = bromesh::parseModules(succ);
                        rule.successor = [mods](const std::vector<float>&) -> std::vector<bromesh::Module> { return mods; };
                        w->ls->addRule(std::move(rule));
                    }
                }
                return self_;
            });

            proto.def("derive", 2, [](Value self_, std::span<const Value> a) {
                auto* w = hostLSystemOf(self_);
                if (!w) return ev::fromUtf8("");
                int iterations = a.size() > 0 ? static_cast<int>(ev::toDouble(a[0])) : 1;
                uint64_t seed = a.size() > 1 ? static_cast<uint64_t>(ev::toDouble(a[1])) : 0;
                auto mods = w->ls->derive(iterations, seed);
                return ev::fromUtf8(bromesh::serializeModules(mods));
            });
        });
}

static Value makeSplatCloud(const bromesh::GaussianSplatCloud& c) {
    ev::Persistent o(ev::createObject());
    ev::setProperty(o.get(), "positions", makeFloat32Array(c.positions.data(), c.positions.size()));
    ev::setProperty(o.get(), "scales",    makeFloat32Array(c.scales.data(), c.scales.size()));
    ev::setProperty(o.get(), "rotations", makeFloat32Array(c.rotations.data(), c.rotations.size()));
    ev::setProperty(o.get(), "opacities", makeFloat32Array(c.opacities.data(), c.opacities.size()));
    ev::setProperty(o.get(), "sh",        makeFloat32Array(c.sh.data(), c.sh.size()));
    ev::setProperty(o.get(), "shDegree",  ev::fromDouble(c.shDegree));
    ev::setProperty(o.get(), "count",     ev::fromDouble(static_cast<double>(c.count())));
    return o.get();
}

static bool readSplatCloud(Value obj, bromesh::GaussianSplatCloud& cloud, std::string& err) {
    if (!ev::isObject(obj)) { err = "cloud must be an object"; return false; }
    readFloatVector(ev::getProperty(obj, "positions"), cloud.positions);
    readFloatVector(ev::getProperty(obj, "scales"),    cloud.scales);
    readFloatVector(ev::getProperty(obj, "rotations"), cloud.rotations);
    readFloatVector(ev::getProperty(obj, "opacities"), cloud.opacities);
    readFloatVector(ev::getProperty(obj, "sh"),        cloud.sh);
    Value shd = ev::getProperty(obj, "shDegree");
    cloud.shDegree = ev::isNumber(shd) ? static_cast<int>(ev::toDouble(shd)) : 0;
    if (cloud.positions.empty()) { err = "cloud has no positions"; return false; }
    if (!cloud.validate()) { err = "invalid GaussianSplatCloud attribute array lengths"; return false; }
    return true;
}

void decorateMeshOptimize(ObjectBuilder& b, HostClass& cls) {
    // ── Simplification ──────────────────────────────────────────────────────────
    b.def("simplify", 2, [](Value self_, std::span<const Value> a) {
        auto* m = hostMeshDataOf(self_);
        if (!m || a.empty()) return self_;
        float ratio = static_cast<float>(ev::toDouble(a[0]));
        float err = (a.size() > 1 && ev::isNumber(a[1])) ? static_cast<float>(ev::toDouble(a[1])) : 0.01f;
        *m = bromesh::simplify(*m, ratio, err);
        return self_;
    });

    b.def("simplifyWithAttributes", 4, [](Value self_, std::span<const Value> a) {
        auto* m = hostMeshDataOf(self_);
        if (!m || a.empty()) return self_;
        float ratio = static_cast<float>(ev::toDouble(a[0]));
        float err = (a.size() > 1 && ev::isNumber(a[1])) ? static_cast<float>(ev::toDouble(a[1])) : 0.01f;
        float uvW = (a.size() > 2 && ev::isNumber(a[2])) ? static_cast<float>(ev::toDouble(a[2])) : 1.0f;
        float nW = (a.size() > 3 && ev::isNumber(a[3])) ? static_cast<float>(ev::toDouble(a[3])) : 0.5f;
        *m = bromesh::simplifyWithAttributes(*m, ratio, err, uvW, nW);
        return self_;
    });

    b.def("simplifyToTriangleCount", 2, [](Value self_, std::span<const Value> a) {
        auto* m = hostMeshDataOf(self_);
        if (!m || a.empty()) return self_;
        size_t target = static_cast<size_t>(ev::toDouble(a[0]));
        float err = (a.size() > 1 && ev::isNumber(a[1])) ? static_cast<float>(ev::toDouble(a[1])) : 0.01f;
        *m = bromesh::simplifyToTriangleCount(*m, target, err);
        return self_;
    });

    b.def("generateLODChain", 1, [](Value self_, std::span<const Value> a) {
        auto* m = hostMeshDataOf(self_);
        if (!m || a.empty()) return makeEmptyArray();
        std::vector<float> ratios;
        if (!readFloatVector(a[0], ratios) || ratios.empty()) return makeEmptyArray();
        auto lods = bromesh::generateLODChain(*m, ratios.data(), static_cast<int>(ratios.size()));
        return hostArrayOf(lods.size(), [&](size_t i) {
            return wrapMesh(std::move(lods[i]));
        });
    });

    // ── GPU Optimization ────────────────────────────────────────────────────────
    b.def("optimizeVertexCache", 0, [](Value self_, std::span<const Value>) {
        auto* m = hostMeshDataOf(self_);
        if (m) bromesh::optimizeVertexCache(*m);
        return self_;
    });

    b.def("optimizeVertexFetch", 0, [](Value self_, std::span<const Value>) {
        auto* m = hostMeshDataOf(self_);
        if (m) bromesh::optimizeVertexFetch(*m);
        return self_;
    });

    b.def("optimizeOverdraw", 1, [](Value self_, std::span<const Value> a) {
        auto* m = hostMeshDataOf(self_);
        if (!m) return self_;
        float thresh = (a.size() > 0 && ev::isNumber(a[0])) ? static_cast<float>(ev::toDouble(a[0])) : 1.05f;
        bromesh::optimizeOverdraw(*m, thresh);
        return self_;
    });

    b.def("analyzeVertexCache", 1, [](Value self_, std::span<const Value> a) {
        auto* m = hostMeshDataOf(self_);
        if (!m) return ev::undefined();
        unsigned cs = (a.size() > 0 && ev::isNumber(a[0])) ? static_cast<unsigned>(ev::toDouble(a[0])) : 16u;
        auto s = bromesh::analyzeVertexCache(*m, cs);
        ObjectBuilder o;
        o.set("verticesTransformed", ev::fromDouble(s.verticesTransformed));
        o.set("warpsExecuted", ev::fromDouble(s.warpsExecuted));
        o.set("acmr", ev::fromDouble(s.acmr));
        o.set("atvr", ev::fromDouble(s.atvr));
        return o.get();
    });

    b.def("analyzeVertexFetch", 1, [](Value self_, std::span<const Value> a) {
        auto* m = hostMeshDataOf(self_);
        if (!m) return ev::undefined();
        size_t vs = (a.size() > 0 && ev::isNumber(a[0])) ? static_cast<size_t>(ev::toDouble(a[0])) : 32u;
        auto s = bromesh::analyzeVertexFetch(*m, vs);
        ObjectBuilder o;
        o.set("bytesFetched", ev::fromDouble(static_cast<double>(s.bytesFetched)));
        o.set("overfetch", ev::fromDouble(s.overfetch));
        return o.get();
    });

    b.def("analyzeOverdraw", 0, [](Value self_, std::span<const Value>) {
        auto* m = hostMeshDataOf(self_);
        if (!m) return ev::undefined();
        auto s = bromesh::analyzeOverdraw(*m);
        ObjectBuilder o;
        o.set("pixelsCovered", ev::fromDouble(s.pixelsCovered));
        o.set("pixelsShaded", ev::fromDouble(s.pixelsShaded));
        o.set("overdraw", ev::fromDouble(s.overdraw));
        return o.get();
    });

    b.def("spatialSortTriangles", 0, [](Value self_, std::span<const Value>) {
        auto* m = hostMeshDataOf(self_);
        if (m) bromesh::spatialSortTriangles(*m);
        return self_;
    });

    b.def("spatialSortVertices", 0, [](Value self_, std::span<const Value>) {
        auto* m = hostMeshDataOf(self_);
        if (m) bromesh::spatialSortVertices(*m);
        return self_;
    });

    b.def("simplify", 1, [](Value self_, std::span<const Value> a) {
        auto* m = hostMeshDataOf(self_);
        if (!m || a.empty()) return self_;
        float targetRatio = static_cast<float>(ev::toDouble(a[0]));
        float maxError = (a.size() > 1 && ev::isNumber(a[1])) ? static_cast<float>(ev::toDouble(a[1])) : 1e-2f;
        *m = bromesh::simplify(*m, targetRatio, maxError);
        return self_;
    });

    b.def("simplifyToTriangleCount", 1, [](Value self_, std::span<const Value> a) {
        auto* m = hostMeshDataOf(self_);
        if (!m || a.empty()) return self_;
        size_t targetCount = static_cast<size_t>(ev::toDouble(a[0]));
        float maxError = (a.size() > 1 && ev::isNumber(a[1])) ? static_cast<float>(ev::toDouble(a[1])) : 1e-2f;
        *m = bromesh::simplifyToTriangleCount(*m, targetCount, maxError);
        return self_;
    });

    b.def("generateShadowIndexBuffer", 0, [](Value self_, std::span<const Value>) {
        auto* m = hostMeshDataOf(self_);
        if (!m) return ev::undefined();
        return makeUint32Array(bromesh::generateShadowIndexBuffer(*m));
    });

    b.def("buildMeshlets", 1, [](Value self_, std::span<const Value> a) {
        auto* m = hostMeshDataOf(self_);
        if (!m) return makeEmptyArray();
        bromesh::MeshletParams p;
        if (!a.empty() && ev::isObject(a[0])) {
            Value mv = ev::getProperty(a[0], "maxVertices");
            if (ev::isNumber(mv)) p.maxVertices = static_cast<size_t>(ev::toDouble(mv));
            Value mt = ev::getProperty(a[0], "maxTriangles");
            if (ev::isNumber(mt)) p.maxTriangles = static_cast<size_t>(ev::toDouble(mt));
            Value cw = ev::getProperty(a[0], "coneWeight");
            if (ev::isNumber(cw)) p.coneWeight = static_cast<float>(ev::toDouble(cw));
        }
        auto ml = bromesh::buildMeshlets(*m, p);
        return hostArrayOf(ml.size(), [&](size_t i) -> Value {
            const auto& item = ml[i];
            ObjectBuilder o;
            o.set("vertices", makeUint32Array(item.vertices));
            o.set("triangles", makeUint8Array(item.triangles.data(), item.triangles.size()));
            ObjectBuilder bnd;
            float cenv[3] = {item.bounds.center[0], item.bounds.center[1], item.bounds.center[2]};
            float apexv[3] = {item.bounds.coneApex[0], item.bounds.coneApex[1], item.bounds.coneApex[2]};
            float axisv[3] = {item.bounds.coneAxis[0], item.bounds.coneAxis[1], item.bounds.coneAxis[2]};
            bnd.set("center", hostArrayOf(3, [&cenv](size_t k) -> Value { return ev::fromDouble(cenv[k]); }));
            bnd.set("radius", ev::fromDouble(item.bounds.radius));
            bnd.set("coneApex", hostArrayOf(3, [&apexv](size_t k) -> Value { return ev::fromDouble(apexv[k]); }));
            bnd.set("coneAxis", hostArrayOf(3, [&axisv](size_t k) -> Value { return ev::fromDouble(axisv[k]); }));
            bnd.set("coneCutoff", ev::fromDouble(item.bounds.coneCutoff));
            o.set("bounds", bnd.get());
            return o.get();
        });
    });

    b.def("encode", 0, [](Value self_, std::span<const Value>) {
        auto* m = hostMeshDataOf(self_);
        if (!m) return ev::undefined();
        auto enc = bromesh::encodeMesh(*m);
        ev::Persistent o(ev::createObject());
        ev::setProperty(o.get(), "vertexData", makeUint8Array(enc.vertexData));
        ev::setProperty(o.get(), "indexData", makeUint8Array(enc.indexData));
        ev::setProperty(o.get(), "vertexCount", ev::fromDouble(static_cast<double>(enc.vertexCount)));
        ev::setProperty(o.get(), "vertexSize", ev::fromDouble(static_cast<double>(enc.vertexSize)));
        ev::setProperty(o.get(), "indexCount", ev::fromDouble(static_cast<double>(enc.indexCount)));
        ev::setProperty(o.get(), "hasNormals", ev::fromBool(m->hasNormals()));
        ev::setProperty(o.get(), "hasUVs", ev::fromBool(m->hasUVs()));
        ev::setProperty(o.get(), "hasColors", ev::fromBool(m->hasColors()));
        return o.get();
    });

    // ── Save formats ────────────────────────────────────────────────────────────
    b.def("saveGLTF", 2, [](Value self_, std::span<const Value> a) {
        auto* m = hostMeshDataOf(self_);
        if (!m || a.empty()) return ev::fromBool(false);
        std::string path = resolveMeshWritePath(ev::toUtf8(a[0]));
        if (a.size() < 2 || !ev::isObject(a[1])) {
            return ev::fromBool(bromesh::saveGLTF(*m, path));
        }
        bromesh::SkinData* skinPtr = nullptr;
        bromesh::Skeleton* skelPtr = nullptr;
        std::vector<bromesh::Animation> anims;

        Value skinV = ev::getProperty(a[1], "skin");
        if (auto* s = hostSkinDataOf(skinV)) skinPtr = s;

        Value skelV = ev::getProperty(a[1], "skeleton");
        if (auto* sk = hostSkeletonOf(skelV)) skelPtr = sk;

        Value animV = ev::getProperty(a[1], "animations");
        if (ev::isObject(animV)) {
            Value lenV = ev::getProperty(animV, "length");
            if (ev::isNumber(lenV)) {
                size_t n = static_cast<size_t>(ev::toDouble(lenV));
                anims.reserve(n);
                for (size_t i = 0; i < n; ++i) {
                    auto* an = hostAnimationOf(ev::getElement(animV, static_cast<uint32_t>(i)));
                    if (an) anims.push_back(*an);
                }
            }
        }
        return ev::fromBool(bromesh::saveGLTF(*m, skinPtr, skelPtr, anims, path));
    });

    b.def("saveOBJ", 1, [](Value self_, std::span<const Value> a) {
        auto* m = hostMeshDataOf(self_);
        if (!m || a.empty()) return ev::fromBool(false);
        return ev::fromBool(bromesh::saveOBJ(*m, resolveMeshWritePath(ev::toUtf8(a[0]))));
    });

    b.def("savePLY", 1, [](Value self_, std::span<const Value> a) {
        auto* m = hostMeshDataOf(self_);
        if (!m || a.empty()) return ev::fromBool(false);
        return ev::fromBool(bromesh::savePLY(*m, resolveMeshWritePath(ev::toUtf8(a[0]))));
    });

    b.def("saveSTL", 1, [](Value self_, std::span<const Value> a) {
        auto* m = hostMeshDataOf(self_);
        if (!m || a.empty()) return ev::fromBool(false);
        return ev::fromBool(bromesh::saveSTL(*m, resolveMeshWritePath(ev::toUtf8(a[0]))));
    });

    // ── Static decoders & loaders on Mesh ───────────────────────────────────────
    cls.setStatic("decode", ev::makeFunction([](Value, std::span<const Value> a) -> Value {
        if (a.empty() || !ev::isObject(a[0])) return ev::throwTypeError("decode requires EncodedMesh object");
        bromesh::EncodedMesh enc;
        readU8Vector(ev::getProperty(a[0], "vertexData"), enc.vertexData);
        readU8Vector(ev::getProperty(a[0], "indexData"), enc.indexData);
        enc.vertexCount = static_cast<size_t>(ev::toDouble(ev::getProperty(a[0], "vertexCount")));
        enc.vertexSize = static_cast<size_t>(ev::toDouble(ev::getProperty(a[0], "vertexSize")));
        enc.indexCount = static_cast<size_t>(ev::toDouble(ev::getProperty(a[0], "indexCount")));
        bool hasN = ev::toBool(ev::getProperty(a[0], "hasNormals"));
        bool hasU = ev::toBool(ev::getProperty(a[0], "hasUVs"));
        bool hasC = ev::toBool(ev::getProperty(a[0], "hasColors"));
        return wrapMesh(bromesh::decodeMesh(enc, hasN, hasU, hasC));
    }, 1));

    cls.setStatic("stripify", ev::makeFunction([](Value, std::span<const Value> a) -> Value {
        if (a.size() < 2) return ev::throwTypeError("stripify requires (indices, vertexCount)");
        std::vector<uint32_t> idx;
        if (auto info = ev::typedArrayInfo(a[0])) {
            const uint32_t* p = reinterpret_cast<const uint32_t*>(info.data);
            idx.assign(p, p + info.elementCount);
        }
        size_t vc = static_cast<size_t>(ev::toDouble(a[1]));
        uint32_t restart = (a.size() > 2 && ev::isNumber(a[2])) ? static_cast<uint32_t>(ev::toDouble(a[2])) : 0xFFFFFFFFu;
        return makeUint32Array(bromesh::stripify(idx, vc, restart));
    }, 3));

    cls.setStatic("unstripify", ev::makeFunction([](Value, std::span<const Value> a) -> Value {
        if (a.empty()) return ev::throwTypeError("unstripify requires strip");
        std::vector<uint32_t> strip;
        if (auto info = ev::typedArrayInfo(a[0])) {
            const uint32_t* p = reinterpret_cast<const uint32_t*>(info.data);
            strip.assign(p, p + info.elementCount);
        }
        uint32_t restart = (a.size() > 1 && ev::isNumber(a[1])) ? static_cast<uint32_t>(ev::toDouble(a[1])) : 0xFFFFFFFFu;
        return makeUint32Array(bromesh::unstripify(strip, restart));
    }, 2));

    cls.setStatic("loadGLTF", ev::makeFunction([](Value, std::span<const Value> a) -> Value {
        if (a.empty()) return ev::undefined();
        std::string path = bro::util::resolveAssetPath(ev::toUtf8(a[0]));
        auto scene = bromesh::loadGLTF(path);
        ObjectBuilder obj;

        obj.set("meshes", hostArrayOf(scene.meshes.size(), [&](size_t i) -> Value {
            return wrapMesh(std::move(scene.meshes[i]));
        }));
        obj.set("skins", hostArrayOf(scene.skins.size(), [&](size_t i) -> Value {
            return wrapSkinData(std::move(scene.skins[i]));
        }));
        obj.set("skeletons", hostArrayOf(scene.skeletons.size(), [&](size_t i) -> Value {
            return wrapSkeleton(std::move(scene.skeletons[i]));
        }));
        obj.set("animations", hostArrayOf(scene.animations.size(), [&](size_t i) -> Value {
            return wrapAnimation(std::move(scene.animations[i]));
        }));
        obj.set("meshSkeleton", hostArrayOf(scene.meshSkeleton.size(), [&](size_t i) -> Value {
            return ev::fromDouble(scene.meshSkeleton[i]);
        }));
        obj.set("animationSkeleton", hostArrayOf(scene.animationSkeleton.size(), [&](size_t i) -> Value {
            return ev::fromDouble(scene.animationSkeleton[i]);
        }));

        return obj.get();
    }, 1));

    cls.setStatic("loadOBJ", ev::makeFunction([](Value, std::span<const Value> a) -> Value {
        if (a.empty()) return ev::undefined();
        return wrapMesh(bromesh::loadOBJ(bro::util::resolveAssetPath(ev::toUtf8(a[0]))));
    }, 1));

    cls.setStatic("loadFBX", ev::makeFunction([](Value, std::span<const Value> a) -> Value {
        if (a.empty()) return makeEmptyArray();
        auto meshes = bromesh::loadFBX(bro::util::resolveAssetPath(ev::toUtf8(a[0])));
        return hostArrayOf(meshes.size(), [&](size_t i) -> Value {
            return wrapMesh(std::move(meshes[i]));
        });
    }, 1));

    cls.setStatic("loadPLY", ev::makeFunction([](Value, std::span<const Value> a) -> Value {
        if (a.empty()) return ev::undefined();
        return wrapMesh(bromesh::loadPLY(bro::util::resolveAssetPath(ev::toUtf8(a[0]))));
    }, 1));

    cls.setStatic("loadSTL", ev::makeFunction([](Value, std::span<const Value> a) -> Value {
        if (a.empty()) return ev::undefined();
        return wrapMesh(bromesh::loadSTL(bro::util::resolveAssetPath(ev::toUtf8(a[0]))));
    }, 1));

    cls.setStatic("loadVOX", ev::makeFunction([](Value, std::span<const Value> a) -> Value {
        if (a.empty()) return ev::undefined();
        auto vox = bromesh::loadVOX(bro::util::resolveAssetPath(ev::toUtf8(a[0])));
        ObjectBuilder obj;
        obj.set("sizeX", ev::fromDouble(vox.sizeX));
        obj.set("sizeY", ev::fromDouble(vox.sizeY));
        obj.set("sizeZ", ev::fromDouble(vox.sizeZ));
        obj.set("voxels", makeUint8Array(vox.voxels));
        obj.set("palette", makeFloat32Array(vox.palette, 256 * 4));
        return obj.get();
    }, 1));

    cls.setStatic("loadSplatPLY", ev::makeFunction([](Value, std::span<const Value> a) -> Value {
        if (a.empty()) return ev::undefined();
        return makeSplatCloud(bromesh::loadSplatPLY(bro::util::resolveAssetPath(ev::toUtf8(a[0]))));
    }, 1));

    cls.setStatic("saveSplatPLY", ev::makeFunction([](Value, std::span<const Value> a) -> Value {
        if (a.size() < 2) return ev::throwTypeError("saveSplatPLY requires (path, cloud)");
        std::string path = resolveMeshWritePath(ev::toUtf8(a[0]));
        bromesh::GaussianSplatCloud cloud;
        std::string err;
        if (!readSplatCloud(a[1], cloud, err)) return ev::throwTypeError(err.c_str());
        return ev::fromBool(bromesh::saveSplatPLY(cloud, path));
    }, 2));

    cls.setStatic("reconstruct", ev::makeFunction([](Value, std::span<const Value> a) -> Value {
        if (a.empty()) return ev::throwTypeError("reconstruct requires a Mesh (point cloud)");
        auto* m = hostMeshDataOf(a[0]);
        if (!m) return ev::throwTypeError("first argument must be a Mesh");
        bromesh::ReconstructParams params;
        if (a.size() > 1 && ev::isObject(a[1])) {
            Value grV = ev::getProperty(a[1], "gridResolution");
            if (ev::isNumber(grV)) params.gridResolution = static_cast<int>(ev::toDouble(grV));
            Value srV = ev::getProperty(a[1], "supportRadius");
            if (ev::isNumber(srV)) params.supportRadius = static_cast<float>(ev::toDouble(srV));
            Value ilV = ev::getProperty(a[1], "isoLevel");
            if (ev::isNumber(ilV)) params.isoLevel = static_cast<float>(ev::toDouble(ilV));
        }
        return wrapMesh(bromesh::reconstructFromPointCloud(*m, params));
    }, 2));

    // ── Isosurface statics on Mesh ──────────────────────────────────────────────
    cls.setStatic("marchingCubes", ev::makeFunction([](Value, std::span<const Value> a) -> Value {
        if (a.size() < 4) return ev::throwTypeError("marchingCubes requires (field, gridX, gridY, gridZ)");
        std::vector<float> field;
        if (!readFloatVector(a[0], field)) return ev::throwTypeError("field must be Float32Array");
        int gx = static_cast<int>(ev::toDouble(a[1]));
        int gy = static_cast<int>(ev::toDouble(a[2]));
        int gz = static_cast<int>(ev::toDouble(a[3]));
        float iso = (a.size() > 4 && ev::isNumber(a[4])) ? static_cast<float>(ev::toDouble(a[4])) : 0.0f;
        float cs = (a.size() > 5 && ev::isNumber(a[5])) ? static_cast<float>(ev::toDouble(a[5])) : 1.0f;
        return wrapMesh(bromesh::marchingCubes(field.data(), gx, gy, gz, iso, cs));
    }, 6));

    cls.setStatic("dualContour", ev::makeFunction([](Value, std::span<const Value> a) -> Value {
        if (a.size() < 4) return ev::throwTypeError("dualContour requires (field, gridX, gridY, gridZ)");
        std::vector<float> field;
        if (!readFloatVector(a[0], field)) return ev::throwTypeError("field must be Float32Array");
        int gx = static_cast<int>(ev::toDouble(a[1]));
        int gy = static_cast<int>(ev::toDouble(a[2]));
        int gz = static_cast<int>(ev::toDouble(a[3]));
        float iso = (a.size() > 4 && ev::isNumber(a[4])) ? static_cast<float>(ev::toDouble(a[4])) : 0.0f;
        float cs = (a.size() > 5 && ev::isNumber(a[5])) ? static_cast<float>(ev::toDouble(a[5])) : 1.0f;
        return wrapMesh(bromesh::dualContour(field.data(), gx, gy, gz, iso, cs));
    }, 6));

    cls.setStatic("surfaceNets", ev::makeFunction([](Value, std::span<const Value> a) -> Value {
        if (a.size() < 4) return ev::throwTypeError("surfaceNets requires (field, gridX, gridY, gridZ)");
        std::vector<float> field;
        if (!readFloatVector(a[0], field)) return ev::throwTypeError("field must be Float32Array");
        int gx = static_cast<int>(ev::toDouble(a[1]));
        int gy = static_cast<int>(ev::toDouble(a[2]));
        int gz = static_cast<int>(ev::toDouble(a[3]));
        float iso = (a.size() > 4 && ev::isNumber(a[4])) ? static_cast<float>(ev::toDouble(a[4])) : 0.0f;
        float cs = (a.size() > 5 && ev::isNumber(a[5])) ? static_cast<float>(ev::toDouble(a[5])) : 1.0f;
        return wrapMesh(bromesh::surfaceNets(field.data(), gx, gy, gz, iso, cs));
    }, 6));

    cls.setStatic("transvoxel", ev::makeFunction([](Value, std::span<const Value> a) -> Value {
        if (a.size() < 4) return ev::throwTypeError("transvoxel requires (field, gridSize, lod, transitionMasks)");
        std::vector<float> field;
        if (!readFloatVector(a[0], field)) return ev::throwTypeError("field must be Float32Array");
        int gridSize = static_cast<int>(ev::toDouble(a[1]));
        int lod = static_cast<int>(ev::toDouble(a[2]));
        int neighborLods[6] = {-1, -1, -1, -1, -1, -1};
        if (ev::isObject(a[3])) {
            for (int i = 0; i < 6; ++i) {
                Value e = ev::getElement(a[3], i);
                if (ev::isNumber(e)) neighborLods[i] = static_cast<int>(ev::toDouble(e));
            }
        }
        float iso = (a.size() > 4 && ev::isNumber(a[4])) ? static_cast<float>(ev::toDouble(a[4])) : 0.0f;
        float cs = (a.size() > 5 && ev::isNumber(a[5])) ? static_cast<float>(ev::toDouble(a[5])) : 1.0f;
        return wrapMesh(bromesh::transvoxel(field.data(), gridSize, lod, neighborLods, iso, cs));
    }, 6));

    cls.setStatic("greedyMesh", ev::makeFunction([](Value, std::span<const Value> a) -> Value {
        if (a.size() < 4) return ev::throwTypeError("greedyMesh requires (voxels, gridX, gridY, gridZ)");
        std::vector<uint8_t> voxels;
        if (!readU8Vector(a[0], voxels)) return ev::throwTypeError("voxels must be Uint8Array");
        int gx = static_cast<int>(ev::toDouble(a[1]));
        int gy = static_cast<int>(ev::toDouble(a[2]));
        int gz = static_cast<int>(ev::toDouble(a[3]));
        float cs = (a.size() > 4 && ev::isNumber(a[4])) ? static_cast<float>(ev::toDouble(a[4])) : 1.0f;
        return wrapMesh(bromesh::greedyMesh(voxels.data(), gx, gy, gz, cs));
    }, 5));

    // ── L-System static helpers on Mesh ─────────────────────────────────────────
    cls.setStatic("parseLSystem", ev::makeFunction([](Value, std::span<const Value> a) -> Value {
        if (a.empty() || !ev::isString(a[0])) return makeEmptyArray();
        auto mods = bromesh::parseModules(ev::toUtf8(a[0]));
        return hostArrayOf(mods.size(), [&](size_t i) -> Value {
            const auto& mod = mods[i];
            ObjectBuilder o;
            char s[2] = {mod.symbol, 0};
            o.set("symbol", ev::fromUtf8(s));
            o.set("params", hostArrayOf(mod.params.size(), [&mod](size_t j) -> Value {
                return ev::fromDouble(mod.params[j]);
            }));
            return o.get();
        });
    }, 1));

    cls.setStatic("lsystemToBranches", ev::makeFunction([](Value, std::span<const Value> a) -> Value {
        if (a.empty() || !ev::isObject(a[0])) return ev::throwTypeError("lsystemToBranches requires (modules[, opts])");
        std::vector<bromesh::Module> mods;
        Value lenV = ev::getProperty(a[0], "length");
        if (ev::isNumber(lenV)) {
            size_t len = static_cast<size_t>(ev::toDouble(lenV));
            mods.reserve(len);
            for (size_t i = 0; i < len; ++i) {
                Value o = ev::getElement(a[0], static_cast<uint32_t>(i));
                bromesh::Module m{};
                Value sv = ev::getProperty(o, "symbol");
                if (ev::isString(sv)) {
                    std::string str = ev::toUtf8(sv);
                    if (!str.empty()) m.symbol = str[0];
                }
                Value pv = ev::getProperty(o, "params");
                if (ev::isObject(pv)) {
                    Value plv = ev::getProperty(pv, "length");
                    if (ev::isNumber(plv)) {
                        size_t pn = static_cast<size_t>(ev::toDouble(plv));
                        m.params.resize(pn);
                        for (size_t j = 0; j < pn; ++j) {
                            m.params[j] = static_cast<float>(ev::toDouble(ev::getElement(pv, static_cast<uint32_t>(j))));
                        }
                    }
                }
                mods.push_back(std::move(m));
            }
        }
        bromesh::TurtleOptions to;
        if (a.size() > 1 && ev::isObject(a[1])) {
            Value slV = ev::getProperty(a[1], "stepLength");
            if (ev::isNumber(slV)) to.stepLength = static_cast<float>(ev::toDouble(slV));
            Value angV = ev::getProperty(a[1], "angle");
            if (ev::isNumber(angV)) to.angle = static_cast<float>(ev::toDouble(angV));
            Value radV = ev::getProperty(a[1], "radius");
            if (ev::isNumber(radV)) to.radius = static_cast<float>(ev::toDouble(radV));
            Value posV = ev::getProperty(a[1], "position");
            if (!ev::isUndefined(posV)) to.position = readBmVec3(posV);
            Value headV = ev::getProperty(a[1], "heading");
            if (!ev::isUndefined(headV)) to.heading = readBmVec3(headV);
            Value upV = ev::getProperty(a[1], "up");
            if (!ev::isUndefined(upV)) to.up = readBmVec3(upV);
        }
        auto segs = bromesh::lsystemToBranches(mods, to);
        return makeBranchSegments(segs);
    }, 2));
}

}  // namespace bro::bronze_host

#endif  // BRO_WITH_3D
