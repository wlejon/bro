// AINavMesh implementation for bronze_host.
// 3D Recast/Detour polygon navmesh, baking, queries, raycasting, dynamic obstacles.

#include "bronze_host/host_ai_internal.h"

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

        Value arr = hostArrayOf(res.points.size(), [&](size_t i) {
            return makeVec3Value(res.points[i].x, res.points[i].y, res.points[i].z);
        });

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
        if (!h || !h->mesh || !h->mesh->supportsObstacles() || a.empty()) return ev::fromDouble(0);
        if (a.size() >= 3 && !ev::isObject(a[0])) {
            float x = static_cast<float>(numAt(a, 0));
            float y = static_cast<float>(numAt(a, 1));
            float z = static_cast<float>(numAt(a, 2));
            float radius = (a.size() >= 4) ? static_cast<float>(numAt(a, 3)) : 0.5f;
            float height = (a.size() >= 5) ? static_cast<float>(numAt(a, 4)) : 2.0f;
            auto id = h->mesh->addObstacle({x, y, z}, radius, height);
            return ev::fromDouble(id);
        } else if (!a.empty() && ev::isObject(a[0])) {
            bromath::Vec3 pos = parseVec3(a[0]);
            float radius = (a.size() >= 2) ? static_cast<float>(numAt(a, 1)) : 0.5f;
            float height = (a.size() >= 3) ? static_cast<float>(numAt(a, 2)) : 2.0f;
            auto id = h->mesh->addObstacle(pos, radius, height);
            return ev::fromDouble(id);
        }
        return ev::fromDouble(0);
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

    // Read vertices / positions
    Value posV = ev::getProperty(root.get(), "vertices");
    if (ev::isUndefined(posV) || ev::isNull(posV)) {
        posV = ev::getProperty(root.get(), "positions");
    }

    Value idxV = ev::getProperty(root.get(), "indices");

    if (!ev::isUndefined(posV) && !ev::isNull(posV)) {
        if (auto info = ev::typedArrayInfo(posV)) {
            const float* p = reinterpret_cast<const float*>(info.data);
            size_t count = info.byteLength / sizeof(float);
            xyz.assign(p, p + count);
        } else {
            std::vector<float> storage;
            const float* data = nullptr;
            size_t count = 0;
            if (floatData(posV, storage, &data, &count)) {
                xyz.assign(data, data + count);
            }
        }
    }

    if (!ev::isUndefined(idxV) && !ev::isNull(idxV)) {
        if (auto info = ev::typedArrayInfo(idxV)) {
            if (info.bytesPerElement == 2) {
                const uint16_t* p = reinterpret_cast<const uint16_t*>(info.data);
                size_t count = info.byteLength / sizeof(uint16_t);
                indices.assign(p, p + count);
            } else {
                const uint32_t* p = reinterpret_cast<const uint32_t*>(info.data);
                size_t count = info.byteLength / sizeof(uint32_t);
                indices.assign(p, p + count);
            }
        } else {
            std::vector<uint32_t> storage;
            const uint32_t* data = nullptr;
            size_t count = 0;
            if (uint32Data(idxV, storage, &data, &count)) {
                indices.assign(data, data + count);
            }
        }
    }

    Value fromPhys = ev::getProperty(root.get(), "fromPhysics");
    if (!ev::isUndefined(fromPhys) && !ev::isNull(fromPhys) && ev::toBool(fromPhys)) {
        auto* e = hostEngine();
        auto* world = e ? e->physicsWorld() : nullptr;
        if (world) {
            world->collectStaticTriangles(xyz, indices, 0xffffffffu);
        }
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
                if (ev::isObject(el)) {
                    brogameagent::NavMeshOffMeshLink link;
                    ev::Persistent elRoot(el);
                    Value sv = ev::getProperty(elRoot.get(), "start");
                    Value evVal = ev::getProperty(elRoot.get(), "end");
                    link.start = parseVec3(sv);
                    link.end   = parseVec3(evVal);
                    link.radius = static_cast<float>(getDoubleProperty(elRoot.get(), "radius", link.radius));
                    link.bidirectional = getBoolProperty(elRoot.get(), "bidirectional", true);
                    link.userId = static_cast<uint32_t>(getDoubleProperty(elRoot.get(), "userId", 0));
                    cfg.offMeshLinks.push_back(link);
                }
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
