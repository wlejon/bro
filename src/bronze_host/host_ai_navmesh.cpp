// AINavMesh implementation for bronze_host.
// 3D Recast/Detour polygon navmesh, baking, queries, raycasting, dynamic obstacles.

#include "bronze_host/host_ai_internal.h"
#if BRO_WITH_PHYSICS
#include "physics/physics_world.h"
#endif
#if BRO_WITH_3D
#include "bronze_host/host_scene_internal.h"
#endif

namespace bro::bronze_host {

void decorateNavMeshProto(ObjectBuilder& b) {
    b.accessor("valid", [](Value self_, std::span<const Value>) {
        HostNavMesh* h = unwrapNavMesh(self_);
        if (!h) return ev::undefined();
        return ev::fromBool(h->mesh && h->mesh->valid());
    }, nullptr);

    b.def("findPath", 3, [](Value self, std::span<const Value> a) -> Value {
        auto* h = unwrapNavMesh(self);
        if (!h || !h->mesh || a.size() < 2) return ev::null();

        bromath::Vec3 start = parseVec3(a[0]);
        bromath::Vec3 end   = parseVec3(a[1]);
        bromath::Vec3 extents = brogameagent::NavMesh::kDefaultExtents;
        bool requireFull = false;

        if (a.size() >= 3 && ev::isObject(a[2])) {
            ev::Persistent root(a[2]);
            Value reqV = ev::getProperty(root.get(), "requireFullPath");
            Value extV = ev::getProperty(root.get(), "extents");
            if (!ev::isUndefined(reqV) || !ev::isUndefined(extV)) {
                if (!ev::isUndefined(reqV)) requireFull = ev::toBool(reqV);
                if (ev::isObject(extV)) extents = parseVec3(extV, extents);
            } else {
                extents = parseVec3(root.get(), extents);
            }
        }

        auto res = h->mesh->findPathEx(start, end, extents, requireFull);
        if (res.points.empty()) return ev::null();

        std::vector<uint32_t> linkIndices;
        for (size_t i = 0; i < res.points.size(); ++i) {
            if (res.isLinkStart(i)) linkIndices.push_back(static_cast<uint32_t>(i));
        }

        Value arr = makeFloat32Array(&res.points[0].x, res.points.size() * 3);
        ev::Persistent p(arr);
        p.set(ev::setProperty(p.get(), "partial", ev::fromBool(res.partial)));

        Value linksVal = hostArrayOf(linkIndices.size(), [&](size_t i) {
            return ev::fromDouble(linkIndices[i]);
        });
        p.set(ev::setProperty(p.get(), "links", linksVal));

        return p.get();
    });

    b.def("findRandomPoint", 1, [](Value self, std::span<const Value> a) -> Value {
        auto* h = unwrapNavMesh(self);
        if (!h || !h->mesh) return ev::null();
        uint32_t seed = a.empty() ? 0 : u32At(a, 0);
        bromath::Vec3 out;
        if (!h->mesh->randomPoint(seed, out)) return ev::null();
        return makeVec3Value(out.x, out.y, out.z);
    });

    b.def("randomPoint", 1, [](Value self, std::span<const Value> a) -> Value {
        auto* h = unwrapNavMesh(self);
        if (!h || !h->mesh) return ev::null();
        uint32_t seed = a.empty() ? 0 : u32At(a, 0);
        bromath::Vec3 out;
        if (!h->mesh->randomPoint(seed, out)) return ev::null();
        return makeVec3Value(out.x, out.y, out.z);
    });

    b.def("closestPoint", 2, [](Value self, std::span<const Value> a) -> Value {
        auto* h = unwrapNavMesh(self);
        if (!h || !h->mesh || a.empty()) return ev::null();
        bromath::Vec3 pos = parseVec3(a[0]);
        bromath::Vec3 extents = (a.size() >= 2) ? parseVec3(a[1], brogameagent::NavMesh::kDefaultExtents)
                                                : brogameagent::NavMesh::kDefaultExtents;
        bromath::Vec3 out;
        if (!h->mesh->nearestPoint(pos, out, extents)) return ev::null();
        return makeVec3Value(out.x, out.y, out.z);
    });

    b.def("nearestPoint", 2, [](Value self, std::span<const Value> a) -> Value {
        auto* h = unwrapNavMesh(self);
        if (!h || !h->mesh || a.empty()) return ev::null();
        bromath::Vec3 pos = parseVec3(a[0]);
        bromath::Vec3 extents = (a.size() >= 2) ? parseVec3(a[1], brogameagent::NavMesh::kDefaultExtents)
                                                : brogameagent::NavMesh::kDefaultExtents;
        bromath::Vec3 out;
        if (!h->mesh->nearestPoint(pos, out, extents)) return ev::null();
        return makeVec3Value(out.x, out.y, out.z);
    });

    b.def("samplePosition", 2, [](Value self, std::span<const Value> a) -> Value {
        auto* h = unwrapNavMesh(self);
        if (!h || !h->mesh || a.empty()) return ev::null();
        bromath::Vec3 pos = parseVec3(a[0]);
        bromath::Vec3 extents = (a.size() >= 2) ? parseVec3(a[1], brogameagent::NavMesh::kDefaultExtents)
                                                : brogameagent::NavMesh::kDefaultExtents;
        bromath::Vec3 out;
        if (!h->mesh->nearestPoint(pos, out, extents)) return ev::null();
        return makeVec3Value(out.x, out.y, out.z);
    });

    b.def("raycast", 3, [](Value self, std::span<const Value> a) -> Value {
        auto* h = unwrapNavMesh(self);
        if (!h || !h->mesh || a.size() < 2) return ev::null();
        bromath::Vec3 start = parseVec3(a[0]);
        bromath::Vec3 end   = parseVec3(a[1]);
        bromath::Vec3 extents = (a.size() >= 3) ? parseVec3(a[2], brogameagent::NavMesh::kDefaultExtents)
                                                : brogameagent::NavMesh::kDefaultExtents;
        auto hit = h->mesh->raycast(start, end, extents);
        ObjectBuilder res;
        res.set("hit", ev::fromBool(hit.hit));
        res.set("t", ev::fromDouble(hit.t));
        res.set("point", makeVec3Value(hit.point.x, hit.point.y, hit.point.z));
        res.set("position", makeVec3Value(hit.point.x, hit.point.y, hit.point.z));
        res.set("normal", makeVec3Value(hit.normal.x, hit.normal.y, hit.normal.z));
        return res.get();
    });

    b.def("save", 0, [](Value self, std::span<const Value>) -> Value {
        auto* h = unwrapNavMesh(self);
        if (!h || !h->mesh) return ev::throwTypeError("save: invalid NavMesh");
        std::vector<uint8_t> blob;
        if (!h->mesh->saveTo(blob)) {
            if (h->mesh->supportsObstacles())
                return ev::throwTypeError("save: dynamicObstacles meshes do not serialize");
            return ev::throwTypeError("save: NavMesh is not baked");
        }
        return ev::createArrayBuffer(std::span<const uint8_t>(blob.data(), blob.size()));
    });

    b.accessor("supportsObstacles", [](Value self_, std::span<const Value>) {
        HostNavMesh* h = unwrapNavMesh(self_);
        if (!h) return ev::undefined();
        return ev::fromBool(h->mesh && h->mesh->supportsObstacles());
    }, nullptr);

    b.accessor("generation", [](Value self_, std::span<const Value>) {
        HostNavMesh* h = unwrapNavMesh(self_);
        if (!h) return ev::undefined();
        return ev::fromDouble(h->mesh ? static_cast<double>(h->mesh->generation()) : 0.0);
    }, nullptr);

    b.accessor("obstacleCount", [](Value self_, std::span<const Value>) {
        HostNavMesh* h = unwrapNavMesh(self_);
        if (!h) return ev::undefined();
        return ev::fromDouble(h->mesh ? h->mesh->obstacleCount() : 0);
    }, nullptr);

    b.accessor("obstaclesPending", [](Value self_, std::span<const Value>) {
        HostNavMesh* h = unwrapNavMesh(self_);
        if (!h) return ev::undefined();
        return ev::fromBool(h->mesh && h->mesh->obstaclesPending());
    }, nullptr);

    b.def("update", 1, [](Value self_, std::span<const Value> a) {
        HostNavMesh* h = unwrapNavMesh(self_);
        if (!h) return ev::undefined();
        if (!h->mesh) return ev::fromBool(true);
        float dt = (a.empty()) ? (1.0f / 60.0f) : static_cast<float>(numAt(a, 0));
        return ev::fromBool(h->mesh->update(dt));
    });

    b.def("addObstacle", 3, [](Value self, std::span<const Value> a) -> Value {
        auto* h = unwrapNavMesh(self);
        if (!h || !h->mesh) return ev::null();
        if (!h->mesh->supportsObstacles()) {
            return ev::throwTypeError("addObstacle: bake the mesh with dynamicObstacles: true");
        }
        if (a.empty()) return ev::throwTypeError("addObstacle(desc)");

        uint32_t id = 0;
        if (ev::isObject(a[0])) {
            ev::Persistent desc(a[0]);
            std::string type = "cylinder";
            Value tv = ev::getProperty(desc.get(), "type");
            if (ev::isString(tv)) type = ev::toUtf8(tv);

            if (type == "cylinder" || type.empty()) {
                Value pv = ev::getProperty(desc.get(), "pos");
                bromath::Vec3 pos = parseVec3(pv);
                float radius = static_cast<float>(getDoubleProperty(desc.get(), "radius", 0.0));
                float height = static_cast<float>(getDoubleProperty(desc.get(), "height", 0.0));
                if (radius <= 0 || height <= 0) {
                    return ev::throwTypeError("addObstacle: cylinder needs {pos, radius > 0, height > 0}");
                }
                id = h->mesh->addObstacle(pos, radius, height);
            } else if (type == "box") {
                Value minV = ev::getProperty(desc.get(), "min");
                Value maxV = ev::getProperty(desc.get(), "max");
                Value ctrV = ev::getProperty(desc.get(), "center");
                Value extV = ev::getProperty(desc.get(), "halfExtents");
                if (ev::isObject(minV) && ev::isObject(maxV)) {
                    bromath::Vec3 minPt = parseVec3(minV);
                    bromath::Vec3 maxPt = parseVec3(maxV);
                    id = h->mesh->addBoxObstacle(minPt, maxPt);
                } else if (ev::isObject(ctrV) && ev::isObject(extV)) {
                    bromath::Vec3 center = parseVec3(ctrV);
                    bromath::Vec3 halfExtents = parseVec3(extV);
                    float yaw = static_cast<float>(getDoubleProperty(desc.get(), "yaw", 0.0));
                    id = h->mesh->addBoxObstacle(center, halfExtents, yaw);
                } else {
                    return ev::throwTypeError("addObstacle: box needs {min, max} or {center, halfExtents, yaw?}");
                }
            } else {
                return ev::throwTypeError("addObstacle: type must be 'cylinder' or 'box'");
            }
        } else if (a.size() >= 3) {
            float x = static_cast<float>(numAt(a, 0));
            float y = static_cast<float>(numAt(a, 1));
            float z = static_cast<float>(numAt(a, 2));
            float radius = (a.size() >= 4) ? static_cast<float>(numAt(a, 3)) : 0.5f;
            float height = (a.size() >= 5) ? static_cast<float>(numAt(a, 4)) : 2.0f;
            id = h->mesh->addObstacle({x, y, z}, radius, height);
        }
        if (id == 0) {
            return ev::throwError("addObstacle failed: " + h->mesh->lastError());
        }
        bro::engine::registerNavMeshForPump(h->mesh);
        return ev::fromDouble(id);
    });

    b.def("removeObstacle", 1, [](Value self, std::span<const Value> a) -> Value {
        auto* h = unwrapNavMesh(self);
        if (!h || !h->mesh || a.empty()) return ev::fromBool(false);
        uint32_t id = u32At(a, 0);
        return ev::fromBool(h->mesh->removeObstacle(id));
    });
}

Value makeNavMeshHandle(std::shared_ptr<brogameagent::NavMesh> mesh) {
    auto* h = new HostNavMesh();
    h->mesh = std::move(mesh);

    ObjectBuilder b(g_navMeshClass.make(h, [](void* p) {
        delete static_cast<HostNavMesh*>(p);
    }));

    return b.get();
}

Value aiBakeNavMesh(Value, std::span<const Value> a) {
    Value opts = a.empty() ? ev::undefined() : a[0];
    if (!ev::isObject(opts)) {
        return ev::throwTypeError("bakeNavMesh(options) requires an options object");
    }

    ev::Persistent root(opts);
    std::vector<float> xyz;
    std::vector<uint32_t> indices;

    Value posV = ev::getProperty(root.get(), "positions");
    if (ev::isUndefined(posV) || ev::isNull(posV)) {
        posV = ev::getProperty(root.get(), "vertices");
    }
    Value idxV = ev::getProperty(root.get(), "indices");

    const bool hasPos = !ev::isUndefined(posV) && !ev::isNull(posV);
    const bool hasIdx = !ev::isUndefined(idxV) && !ev::isNull(idxV);
    if (hasPos != hasIdx) {
        return ev::throwTypeError("bakeNavMesh: positions and indices must be passed together");
    }
    if (hasPos) {
        std::vector<float> verts;
        std::vector<uint32_t> idx;
        bool okP = readFloatVector(posV, verts);
        bool okI = readU32Vector(idxV, idx);
        if (!okP || !okI || verts.size() % 3 != 0 || idx.size() % 3 != 0) {
            return ev::throwTypeError("bakeNavMesh: positions must be flat xyz triples and indices a triangle list");
        }
        uint32_t nVerts = static_cast<uint32_t>(verts.size() / 3);
        for (uint32_t i : idx) {
            if (i >= nVerts) {
                return ev::throwRangeError("bakeNavMesh: index out of range");
            }
        }
        uint32_t base = static_cast<uint32_t>(xyz.size() / 3);
        xyz.insert(xyz.end(), verts.begin(), verts.end());
        indices.reserve(indices.size() + idx.size());
        for (uint32_t i : idx) indices.push_back(base + i);
    }

    Value fromPhys = ev::getProperty(root.get(), "fromPhysics");
    if (!ev::isUndefined(fromPhys) && !ev::isNull(fromPhys) &&
        !(ev::isBool(fromPhys) && !ev::toBool(fromPhys))) {
#if BRO_WITH_PHYSICS
        physics::PhysicsWorld* world = unwrapPhysicsWorld(fromPhys);
        if (world) {
            uint32_t layerMask = 0xffffffffu;
            Value lv = ev::getProperty(root.get(), "physicsLayers");
            if (ev::isObject(lv)) {
                ev::Persistent lvRoot(lv);
                Value lenV = ev::getProperty(lvRoot.get(), "length");
                if (ev::isNumber(lenV)) {
                    uint32_t mask = 0;
                    uint32_t n = static_cast<uint32_t>(ev::toDouble(lenV));
                    for (uint32_t i = 0; i < n; i++) {
                        Value el = ev::getElement(lvRoot.get(), i);
                        int32_t idx = -1;
                        if (ev::isString(el)) {
                            std::string s = ev::toUtf8(el);
                            idx = world->layerIndex(s);
                        } else if (ev::isNumber(el)) {
                            idx = static_cast<int32_t>(ev::toDouble(el));
                        }
                        if (idx >= 0 && idx < 32) mask |= (1u << idx);
                    }
                    layerMask = mask;
                }
            }
            world->collectStaticTriangles(xyz, indices, layerMask);
        }
#endif
    }

    Value fromTerrainV = ev::getProperty(root.get(), "fromTerrain");
    if (!ev::isUndefined(fromTerrainV) && !ev::isNull(fromTerrainV)) {
#if BRO_WITH_3D
        auto* tc = terrainCellOf(fromTerrainV);
        if (!tc) {
            return ev::throwTypeError("bakeNavMesh: fromTerrain must be a scene.createTerrain() object");
        }
        Value boundsV = ev::getProperty(root.get(), "terrainBounds");
        if (!ev::isObject(boundsV)) {
            return ev::throwTypeError("bakeNavMesh: fromTerrain requires terrainBounds: {minX, minZ, maxX, maxZ}");
        }
        ev::Persistent boundsRoot(boundsV);
        Value minXV = ev::getProperty(boundsRoot.get(), "minX");
        Value minZV = ev::getProperty(boundsRoot.get(), "minZ");
        Value maxXV = ev::getProperty(boundsRoot.get(), "maxX");
        Value maxZV = ev::getProperty(boundsRoot.get(), "maxZ");
        if (ev::isUndefined(minXV) || ev::isUndefined(minZV) || ev::isUndefined(maxXV) || ev::isUndefined(maxZV)) {
            return ev::throwTypeError("bakeNavMesh: terrainBounds requires minX, minZ, maxX, maxZ");
        }
        float minX = static_cast<float>(ev::toDouble(minXV));
        float minZ = static_cast<float>(ev::toDouble(minZV));
        float maxX = static_cast<float>(ev::toDouble(maxXV));
        float maxZ = static_cast<float>(ev::toDouble(maxZV));
        float step = static_cast<float>(getDoubleProperty(root.get(), "terrainStep", 1.0));
        if (step <= 0.0f) step = 1.0f;
        float rayStart = static_cast<float>(getDoubleProperty(root.get(), "terrainRayStart", 100.0));
        float rayLength = static_cast<float>(getDoubleProperty(root.get(), "terrainRayLength", 200.0));

        int nx = static_cast<int>(std::floor((maxX - minX) / step)) + 1;
        int nz = static_cast<int>(std::floor((maxZ - minZ) / step)) + 1;
        if (nx >= 2 && nz >= 2) {
            uint32_t baseIdx = static_cast<uint32_t>(xyz.size() / 3);
            for (int ix = 0; ix < nx; ++ix) {
                float x = minX + ix * step;
                for (int iz = 0; iz < nz; ++iz) {
                    float z = minZ + iz * step;
                    float y = 0.0f;
                    terrainSampleHeight(tc, x, z, rayStart, rayLength, y);
                    xyz.push_back(x);
                    xyz.push_back(y);
                    xyz.push_back(z);
                }
            }
            for (int ix = 0; ix < nx - 1; ++ix) {
                for (int iz = 0; iz < nz - 1; ++iz) {
                    uint32_t v00 = baseIdx + ix * nz + iz;
                    uint32_t v01 = baseIdx + ix * nz + (iz + 1);
                    uint32_t v10 = baseIdx + (ix + 1) * nz + iz;
                    uint32_t v11 = baseIdx + (ix + 1) * nz + (iz + 1);
                    indices.push_back(v00);
                    indices.push_back(v01);
                    indices.push_back(v10);
                    indices.push_back(v10);
                    indices.push_back(v01);
                    indices.push_back(v11);
                }
            }
        }
#else
        return ev::throwTypeError("bakeNavMesh: fromTerrain requires a 3D-enabled build");
#endif
    }

    if (xyz.empty() || indices.empty()) {
        return ev::throwTypeError("bakeNavMesh: no geometry (pass vertices/indices or fromPhysics)");
    }

    brogameagent::NavMeshBakeConfig cfg;
    cfg.cellSize             = static_cast<float>(getDoubleProperty(root.get(), "cellSize", cfg.cellSize));
    cfg.cellHeight           = static_cast<float>(getDoubleProperty(root.get(), "cellHeight", cfg.cellHeight));
    cfg.agentRadius          = static_cast<float>(getDoubleProperty(root.get(), "agentRadius", cfg.agentRadius));
    cfg.agentHeight          = static_cast<float>(getDoubleProperty(root.get(), "agentHeight", cfg.agentHeight));
    cfg.agentMaxClimb        = static_cast<float>(getDoubleProperty(root.get(), "agentMaxClimb", cfg.agentMaxClimb));
    cfg.agentMaxSlopeDeg     = static_cast<float>(getDoubleProperty(root.get(), "agentMaxSlope",
                                                    getDoubleProperty(root.get(), "agentMaxSlopeDeg", cfg.agentMaxSlopeDeg)));
    cfg.regionMinSize        = static_cast<float>(getDoubleProperty(root.get(), "regionMinSize", cfg.regionMinSize));
    cfg.regionMergeSize      = static_cast<float>(getDoubleProperty(root.get(), "regionMergeSize", cfg.regionMergeSize));
    cfg.edgeMaxLen           = static_cast<float>(getDoubleProperty(root.get(), "edgeMaxLen", cfg.edgeMaxLen));
    cfg.edgeMaxError         = static_cast<float>(getDoubleProperty(root.get(), "edgeMaxError", cfg.edgeMaxError));
    cfg.detailSampleDist     = static_cast<float>(getDoubleProperty(root.get(), "detailSampleDist", cfg.detailSampleDist));
    cfg.detailSampleMaxError = static_cast<float>(getDoubleProperty(root.get(), "detailSampleMaxError", cfg.detailSampleMaxError));

    cfg.dynamicObstacles     = getBoolProperty(root.get(), "dynamicObstacles", false);
    cfg.tileSize             = static_cast<float>(getDoubleProperty(root.get(), "tileSize", cfg.tileSize));
    cfg.maxObstacles         = static_cast<int>(getDoubleProperty(root.get(), "maxObstacles", cfg.maxObstacles));

    Value linksV = ev::getProperty(root.get(), "offMeshLinks");
    if (ev::isObject(linksV)) {
        ev::Persistent lroot(linksV);
        Value lenV = ev::getProperty(lroot.get(), "length");
        if (!ev::isUndefined(lenV) && !ev::isObject(lenV)) {
            uint32_t n = static_cast<uint32_t>(ev::toDouble(lenV));
            for (uint32_t i = 0; i < n; ++i) {
                Value el = ev::getElement(lroot.get(), i);
                if (!ev::isObject(el)) {
                    return ev::throwTypeError("bakeNavMesh: offMeshLinks entry must be an object");
                }
                ev::Persistent elRoot(el);
                Value sv = ev::getProperty(elRoot.get(), "start");
                Value evVal = ev::getProperty(elRoot.get(), "end");
                if (!ev::isObject(sv) || !ev::isObject(evVal)) {
                    return ev::throwTypeError("bakeNavMesh: offMeshLink must have 'start' and 'end' objects");
                }
                brogameagent::NavMeshOffMeshLink link;
                link.start = parseVec3(sv);
                link.end   = parseVec3(evVal);
                link.radius = static_cast<float>(getDoubleProperty(elRoot.get(), "radius", link.radius));
                link.bidirectional = getBoolProperty(elRoot.get(), "bidirectional", true);
                link.userId = static_cast<uint32_t>(getDoubleProperty(elRoot.get(), "userId", 0));
                cfg.offMeshLinks.push_back(link);
            }
        }
    }

    auto mesh = std::make_shared<brogameagent::NavMesh>();
    if (!mesh->bake(xyz.data(), xyz.size() / 3, indices.data(), indices.size(), cfg)) {
        return ev::throwError(std::string("bakeNavMesh failed: ") + mesh->lastError());
    }

    if (mesh->supportsObstacles()) {
        bro::engine::registerNavMeshForPump(mesh);
    }

    return makeNavMeshHandle(std::move(mesh));
}

Value aiLoadNavMesh(Value, std::span<const Value> a) {
    if (a.empty()) return ev::throwTypeError("loadNavMesh(buffer) requires a buffer");
    Value bufV = a[0];

    const uint8_t* data = nullptr;
    size_t size = 0;
    size_t elemSize = 1;

    if (!bufferBytes(bufV, &data, &size, &elemSize)) {
        return ev::throwTypeError("loadNavMesh: expected an ArrayBuffer or TypedArray");
    }

    auto mesh = std::make_shared<brogameagent::NavMesh>();
    if (!mesh->loadFrom(data, size)) {
        return ev::throwError(std::string("loadNavMesh failed: ") + mesh->lastError());
    }

    if (mesh->supportsObstacles()) {
        bro::engine::registerNavMeshForPump(mesh);
    }

    return makeNavMeshHandle(std::move(mesh));
}

}  // namespace bro::bronze_host
