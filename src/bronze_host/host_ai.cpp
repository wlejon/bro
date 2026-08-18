// AI & Navigation System (NavMesh, NavGrid, Pathfinding, Agents) for bronze_host.
//
// Connects brogameagent library to bronze host:
// - Global `AI` namespace
// - `AINavGrid` handle (2D grid navigation, A* pathfinding, line-of-sight)
// - `AINavMesh` handle (3D Recast/Detour polygon navmesh, raycasting, closest point)
// - `AIAgent` handle (Kinematic/pathed agent, crowd steering)
//
// Follows bronze GC rules strictly:
// - Payload structs are plain host memory, freed by handle finalizers.
// - Finalizers never touch the embed API / never own Persistents.
// - Persistents and child objects live on JS properties.
// - Heap pointers from typedArrayInfo/arrayBufferInfo are consumed before any allocation.

#include "bronze_host/bronze_host.h"
#include "bronze_host/gl_internal.h"
#include "bronze_host/host_internal.h"
#include "engine/engine.h"
#include "engine/navmesh_subsystem.h"
#include "physics/physics_world.h"
#include "util/log.h"

#include <brogameagent/brogameagent.h>
#include <brogameagent/nav_grid.h>
#include <brogameagent/nav_mesh.h>
#include <brogameagent/agent.h>
#include <brogameagent/avoidance.h>
#include <brogameagent/world.h>
#include <brogameagent/perception.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace bro::bronze_host {

namespace {

// ---------------------------------------------------------------------------
// Payload Structures
// ---------------------------------------------------------------------------

struct HostNavGrid {
    uint32_t tag = kHostNavGridTag;
    std::unique_ptr<brogameagent::NavGrid> grid;
};

struct HostNavMesh {
    uint32_t tag = kHostNavMeshTag;
    std::shared_ptr<brogameagent::NavMesh> mesh;
};

struct HostAgent {
    uint32_t tag = kHostAgentTag;
    brogameagent::Agent agent;
    std::shared_ptr<brogameagent::NavMesh> navMesh;

    // Navigation route tracking
    bool navActive = false;
    std::vector<bromath::Vec3> navPath;
    std::vector<uint8_t> navPathFlags;
    int navWaypoint = 0;
    float navY = 0.0f;
    bool destroyed = false;
};

// ---------------------------------------------------------------------------
// Unwrap Helpers
// ---------------------------------------------------------------------------

// The three AI classes. None is constructible — a nav grid comes from
// AI.createNavGrid and an agent from AI.createAgent — so what the conversion
// buys is `instanceof` plus one copy of each method instead of one per handle.
HostClass g_navGridClass;
HostClass g_navMeshClass;
HostClass g_agentClass;

HostNavGrid* unwrapNavGrid(Value v) {
    void* ptr = ev::handleData(v);
    if (!ptr) return nullptr;
    auto* h = static_cast<HostNavGrid*>(ptr);
    return (h->tag == kHostNavGridTag) ? h : nullptr;
}

HostNavMesh* unwrapNavMesh(Value v) {
    void* ptr = ev::handleData(v);
    if (!ptr) return nullptr;
    auto* h = static_cast<HostNavMesh*>(ptr);
    return (h->tag == kHostNavMeshTag) ? h : nullptr;
}

HostAgent* unwrapAgent(Value v) {
    void* ptr = ev::handleData(v);
    if (!ptr) return nullptr;
    auto* h = static_cast<HostAgent*>(ptr);
    return (h->tag == kHostAgentTag) ? h : nullptr;
}

// ---------------------------------------------------------------------------
// Value Builders & Readers
// ---------------------------------------------------------------------------

Value makeVec2Value(float x, float z) {
    ObjectBuilder b;
    b.set("x", ev::fromDouble(x));
    b.set("z", ev::fromDouble(z));
    return b.get();
}

Value makeVec3Value(float x, float y, float z) {
    ObjectBuilder b;
    b.set("x", ev::fromDouble(x));
    b.set("y", ev::fromDouble(y));
    b.set("z", ev::fromDouble(z));
    return b.get();
}

double getDoubleProperty(Value obj, const char* key, double def = 0.0) {
    if (!ev::isObject(obj)) return def;
    ev::Persistent root(obj);
    Value v = ev::getProperty(root.get(), key);
    if (ev::isUndefined(v) || ev::isNull(v) || ev::isObject(v)) return def;
    double d = ev::toDouble(v);
    return std::isnan(d) ? def : d;
}

bool getBoolProperty(Value obj, const char* key, bool def = false) {
    if (!ev::isObject(obj)) return def;
    ev::Persistent root(obj);
    Value v = ev::getProperty(root.get(), key);
    if (ev::isUndefined(v) || ev::isNull(v)) return def;
    return ev::toBool(v);
}

bromath::Vec2 parseVec2(Value v, bromath::Vec2 def = {0.0f, 0.0f}) {
    if (!ev::isObject(v)) return def;
    ev::Persistent root(v);
    Value xV = ev::getProperty(root.get(), "x");
    Value zV = ev::getProperty(root.get(), "z");
    if (ev::isUndefined(zV)) zV = ev::getProperty(root.get(), "y");
    if (!ev::isUndefined(xV) || !ev::isUndefined(zV)) {
        float x = (!ev::isUndefined(xV) && !ev::isObject(xV)) ? static_cast<float>(ev::toDouble(xV)) : def.x;
        float z = (!ev::isUndefined(zV) && !ev::isObject(zV)) ? static_cast<float>(ev::toDouble(zV)) : def.y;
        return {x, z};
    }
    Value e0 = ev::getElement(root.get(), 0);
    Value e1 = ev::getElement(root.get(), 1);
    if (!ev::isUndefined(e0) && !ev::isUndefined(e1)) {
        float x = !ev::isObject(e0) ? static_cast<float>(ev::toDouble(e0)) : def.x;
        float z = !ev::isObject(e1) ? static_cast<float>(ev::toDouble(e1)) : def.y;
        return {x, z};
    }
    return def;
}

bromath::Vec3 parseVec3(Value v, bromath::Vec3 def = {0.0f, 0.0f, 0.0f}) {
    if (!ev::isObject(v)) return def;
    ev::Persistent root(v);
    Value xV = ev::getProperty(root.get(), "x");
    Value yV = ev::getProperty(root.get(), "y");
    Value zV = ev::getProperty(root.get(), "z");
    if (!ev::isUndefined(xV) || !ev::isUndefined(yV) || !ev::isUndefined(zV)) {
        float x = (!ev::isUndefined(xV) && !ev::isObject(xV)) ? static_cast<float>(ev::toDouble(xV)) : def.x;
        float y = (!ev::isUndefined(yV) && !ev::isObject(yV)) ? static_cast<float>(ev::toDouble(yV)) : def.y;
        float z = (!ev::isUndefined(zV) && !ev::isObject(zV)) ? static_cast<float>(ev::toDouble(zV)) : def.z;
        return {x, y, z};
    }
    Value e0 = ev::getElement(root.get(), 0);
    Value e1 = ev::getElement(root.get(), 1);
    Value e2 = ev::getElement(root.get(), 2);
    if (!ev::isUndefined(e0) && !ev::isUndefined(e1) && !ev::isUndefined(e2)) {
        float x = !ev::isObject(e0) ? static_cast<float>(ev::toDouble(e0)) : def.x;
        float y = !ev::isObject(e1) ? static_cast<float>(ev::toDouble(e1)) : def.y;
        float z = !ev::isObject(e2) ? static_cast<float>(ev::toDouble(e2)) : def.z;
        return {x, y, z};
    }
    return def;
}

brogameagent::AABB parseAABB(Value v) {
    brogameagent::AABB box{0.0f, 0.0f, 0.5f, 0.5f};
    if (!ev::isObject(v)) return box;
    ev::Persistent root(v);

    Value cxV = ev::getProperty(root.get(), "cx");
    Value czV = ev::getProperty(root.get(), "cz");
    Value hwV = ev::getProperty(root.get(), "hw");
    Value hdV = ev::getProperty(root.get(), "hd");
    if (!ev::isUndefined(cxV) && !ev::isUndefined(czV) && !ev::isUndefined(hwV) && !ev::isUndefined(hdV)) {
        box.cx = static_cast<float>(ev::toDouble(cxV));
        box.cz = static_cast<float>(ev::toDouble(czV));
        box.hw = static_cast<float>(ev::toDouble(hwV));
        box.hd = static_cast<float>(ev::toDouble(hdV));
        return box;
    }

    Value minXV = ev::getProperty(root.get(), "minX");
    Value minZV = ev::getProperty(root.get(), "minZ");
    Value maxXV = ev::getProperty(root.get(), "maxX");
    Value maxZV = ev::getProperty(root.get(), "maxZ");
    if (!ev::isUndefined(minXV) && !ev::isUndefined(minZV) && !ev::isUndefined(maxXV) && !ev::isUndefined(maxZV)) {
        float x0 = static_cast<float>(ev::toDouble(minXV));
        float z0 = static_cast<float>(ev::toDouble(minZV));
        float x1 = static_cast<float>(ev::toDouble(maxXV));
        float z1 = static_cast<float>(ev::toDouble(maxZV));
        box.cx = 0.5f * (x0 + x1);
        box.cz = 0.5f * (z0 + z1);
        box.hw = 0.5f * std::abs(x1 - x0);
        box.hd = 0.5f * std::abs(z1 - z0);
        return box;
    }

    Value xV = ev::getProperty(root.get(), "x");
    Value zV = ev::getProperty(root.get(), "z");
    Value wV = ev::getProperty(root.get(), "width");
    Value dV = ev::getProperty(root.get(), "depth");
    if (!ev::isUndefined(wV) && !ev::isUndefined(dV)) {
        float x = !ev::isUndefined(xV) ? static_cast<float>(ev::toDouble(xV)) : 0.0f;
        float z = !ev::isUndefined(zV) ? static_cast<float>(ev::toDouble(zV)) : 0.0f;
        float w = static_cast<float>(ev::toDouble(wV));
        float d = static_cast<float>(ev::toDouble(dV));
        box.cx = x + 0.5f * w;
        box.cz = z + 0.5f * d;
        box.hw = 0.5f * w;
        box.hd = 0.5f * d;
        return box;
    }

    return box;
}

std::vector<brogameagent::AABB> parseAABBArray(Value v) {
    std::vector<brogameagent::AABB> result;
    if (!ev::isObject(v)) return result;
    ev::Persistent root(v);
    Value lenV = ev::getProperty(root.get(), "length");
    if (ev::isUndefined(lenV) || ev::isObject(lenV)) return result;
    uint32_t n = static_cast<uint32_t>(ev::toDouble(lenV));
    result.reserve(n);
    for (uint32_t i = 0; i < n; ++i) {
        Value el = ev::getElement(root.get(), i);
        if (ev::isObject(el)) {
            result.push_back(parseAABB(el));
        }
    }
    return result;
}

// ---------------------------------------------------------------------------
// AINavGrid Wrapper
// ---------------------------------------------------------------------------

static void decorateNavGridProto(ObjectBuilder& b) {
    b.def("isWalkable", 2, [](Value self, std::span<const Value> a) -> Value {
        auto* h = unwrapNavGrid(self);
        if (!h || !h->grid) return ev::fromBool(false);
        float x = 0.0f, z = 0.0f;
        if (a.size() >= 2) {
            x = static_cast<float>(numAt(a, 0));
            z = static_cast<float>(numAt(a, 1));
        } else if (!a.empty() && ev::isObject(a[0])) {
            auto p = parseVec2(a[0]);
            x = p.x; z = p.y;
        }
        return ev::fromBool(h->grid->isWalkable(x, z));
    });
    b.def("setWalkable", 3, [](Value self, std::span<const Value> a) -> Value {
        auto* h = unwrapNavGrid(self);
        if (!h || !h->grid) return ev::undefined();
        if (a.size() >= 3) {
            float x = static_cast<float>(numAt(a, 0));
            float z = static_cast<float>(numAt(a, 1));
            bool walkable = boolAt(a, 2);
            h->grid->setWalkable(x, z, walkable);
        } else if (a.size() >= 2 && ev::isObject(a[0])) {
            auto p = parseVec2(a[0]);
            bool walkable = boolAt(a, 1);
            h->grid->setWalkable(p.x, p.y, walkable);
        }
        return ev::undefined();
    });
    b.def("setCellCost", 3, [](Value self, std::span<const Value> a) -> Value {
        auto* h = unwrapNavGrid(self);
        if (!h || !h->grid) return ev::undefined();
        if (a.size() >= 3) {
            float x = static_cast<float>(numAt(a, 0));
            float z = static_cast<float>(numAt(a, 1));
            float cost = static_cast<float>(numAt(a, 2));
            h->grid->setCellCost(x, z, cost);
        } else if (a.size() >= 2 && ev::isObject(a[0])) {
            auto p = parseVec2(a[0]);
            float cost = static_cast<float>(numAt(a, 1));
            h->grid->setCellCost(p.x, p.y, cost);
        }
        return ev::undefined();
    });
    b.def("addObstacle", 2, [](Value self, std::span<const Value> a) -> Value {
        auto* h = unwrapNavGrid(self);
        if (!h || !h->grid || a.empty()) return ev::undefined();
        float padding = (a.size() >= 2) ? static_cast<float>(numAt(a, 1)) : 0.0f;
        h->grid->addObstacle(parseAABB(a[0]), padding);
        return ev::undefined();
    });
    b.def("findPath", 5, [](Value self, std::span<const Value> a) -> Value {
        auto* h = unwrapNavGrid(self);
        if (!h || !h->grid) return hostArrayOf(0, [](size_t) { return ev::undefined(); });

        float fx = 0.0f, fz = 0.0f, tx = 0.0f, tz = 0.0f;
        bool requireFull = false;

        if (a.size() >= 4 && !ev::isObject(a[0]) && !ev::isObject(a[1])) {
            fx = static_cast<float>(numAt(a, 0));
            fz = static_cast<float>(numAt(a, 1));
            tx = static_cast<float>(numAt(a, 2));
            tz = static_cast<float>(numAt(a, 3));
            if (a.size() >= 5 && ev::isObject(a[4])) {
                requireFull = getBoolProperty(a[4], "requireFullPath", false);
            }
        } else if (a.size() >= 2) {
            auto pFrom = parseVec2(a[0]);
            auto pTo   = parseVec2(a[1]);
            fx = pFrom.x; fz = pFrom.y;
            tx = pTo.x;   tz = pTo.y;
            if (a.size() >= 3 && ev::isObject(a[2])) {
                requireFull = getBoolProperty(a[2], "requireFullPath", false);
            }
        }

        auto res = h->grid->findPathEx({fx, fz}, {tx, tz}, requireFull);

        Value arr = hostArrayOf(res.points.size(), [&](size_t i) {
            return makeVec2Value(res.points[i].x, res.points[i].y);
        });
        ev::Persistent p(arr);
        p.set(ev::setProperty(p.get(), "partial", ev::fromBool(res.partial)));
        return p.get();
    });
    b.accessor("width", [](Value self_, std::span<const Value>) {
        HostNavGrid* h = unwrapNavGrid(self_);
        if (!h) return ev::undefined();
        return h->grid ? ev::fromDouble(h->grid->width()) : ev::fromDouble(0);
    }, nullptr);
    b.accessor("height", [](Value self_, std::span<const Value>) {
        HostNavGrid* h = unwrapNavGrid(self_);
        if (!h) return ev::undefined();
        return h->grid ? ev::fromDouble(h->grid->height()) : ev::fromDouble(0);
    }, nullptr);
    b.accessor("cellSize", [](Value self_, std::span<const Value>) {
        HostNavGrid* h = unwrapNavGrid(self_);
        if (!h) return ev::undefined();
        return h->grid ? ev::fromDouble(h->grid->cellSize()) : ev::fromDouble(0);
    }, nullptr);
}

static Value makeNavGridHandle(std::unique_ptr<brogameagent::NavGrid> grid) {
    auto* h = new HostNavGrid();
    h->grid = std::move(grid);

    ObjectBuilder b(g_navGridClass.make(h, [](void* p) {
        delete static_cast<HostNavGrid*>(p);
    }));









    return b.get();
}

// ---------------------------------------------------------------------------
// AINavMesh Wrapper
// ---------------------------------------------------------------------------

static void decorateNavMeshProto(ObjectBuilder& b) {
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
}

static Value makeNavMeshHandle(std::shared_ptr<brogameagent::NavMesh> mesh) {
    auto* h = new HostNavMesh();
    h->mesh = std::move(mesh);

    ObjectBuilder b(g_navMeshClass.make(h, [](void* p) {
        delete static_cast<HostNavMesh*>(p);
    }));













    return b.get();
}

// ---------------------------------------------------------------------------
// AIAgent Wrapper
// ---------------------------------------------------------------------------

static void decorateAgentProto(ObjectBuilder& b) {
    b.accessor("position",
        [](Value self_, std::span<const Value>) -> Value {
        HostAgent* h = unwrapAgent(self_);
        if (!h) return ev::undefined();
            float y = h->navActive ? h->navY : h->agent.elevation();
            return makeVec3Value(h->agent.x(), y, h->agent.z());
        },
        [](Value self_, std::span<const Value> a) -> Value {
        HostAgent* h = unwrapAgent(self_);
        if (!h) return ev::undefined();
            if (!a.empty() && ev::isObject(a[0])) {
                auto p = parseVec3(a[0]);
                h->agent.setPosition(p.x, p.z);
                h->agent.setElevation(p.y);
                h->navY = p.y;
            }
            return ev::undefined();
        });
    b.accessor("velocity",
        [](Value self_, std::span<const Value>) -> Value {
        HostAgent* h = unwrapAgent(self_);
        if (!h) return ev::undefined();
            auto v = h->agent.velocity();
            return makeVec3Value(v.x, 0.0f, v.y);
        },
        nullptr);
    b.accessor("maxSpeed",
        [](Value self_, std::span<const Value>) -> Value {
        HostAgent* h = unwrapAgent(self_);
        if (!h) return ev::undefined();
            return ev::fromDouble(h->agent.speed());
        },
        [](Value self_, std::span<const Value> a) -> Value {
        HostAgent* h = unwrapAgent(self_);
        if (!h) return ev::undefined();
            if (!a.empty()) h->agent.setSpeed(static_cast<float>(numAt(a, 0)));
            return ev::undefined();
        });
    b.accessor("speed",
        [](Value self_, std::span<const Value>) -> Value {
        HostAgent* h = unwrapAgent(self_);
        if (!h) return ev::undefined();
            return ev::fromDouble(h->agent.speed());
        },
        [](Value self_, std::span<const Value> a) -> Value {
        HostAgent* h = unwrapAgent(self_);
        if (!h) return ev::undefined();
            if (!a.empty()) h->agent.setSpeed(static_cast<float>(numAt(a, 0)));
            return ev::undefined();
        });
    b.accessor("radius",
        [](Value self_, std::span<const Value>) -> Value {
        HostAgent* h = unwrapAgent(self_);
        if (!h) return ev::undefined();
            return ev::fromDouble(h->agent.radius());
        },
        [](Value self_, std::span<const Value> a) -> Value {
        HostAgent* h = unwrapAgent(self_);
        if (!h) return ev::undefined();
            if (!a.empty()) h->agent.setRadius(static_cast<float>(numAt(a, 0)));
            return ev::undefined();
        });
    b.accessor("maxAcceleration",
        [](Value self_, std::span<const Value>) -> Value {
        HostAgent* h = unwrapAgent(self_);
        if (!h) return ev::undefined();
            return ev::fromDouble(h->agent.unit().moveSpeed);
        },
        [](Value self_, std::span<const Value> a) -> Value {
        HostAgent* h = unwrapAgent(self_);
        if (!h) return ev::undefined();
            if (!a.empty()) h->agent.setMaxAccel(static_cast<float>(numAt(a, 0)));
            return ev::undefined();
        });
    b.accessor("maxAccel",
        [](Value self_, std::span<const Value>) -> Value {
        HostAgent* h = unwrapAgent(self_);
        if (!h) return ev::undefined();
            return ev::fromDouble(h->agent.unit().moveSpeed);
        },
        [](Value self_, std::span<const Value> a) -> Value {
        HostAgent* h = unwrapAgent(self_);
        if (!h) return ev::undefined();
            if (!a.empty()) h->agent.setMaxAccel(static_cast<float>(numAt(a, 0)));
            return ev::undefined();
        });
    b.accessor("atTarget", [](Value self_, std::span<const Value>) {
        HostAgent* h = unwrapAgent(self_);
        if (!h) return ev::undefined();
        return ev::fromBool(h->agent.atTarget());
    }, nullptr);
    b.accessor("hasTarget", [](Value self_, std::span<const Value>) {
        HostAgent* h = unwrapAgent(self_);
        if (!h) return ev::undefined();
        return ev::fromBool(h->agent.hasTarget() || h->navActive);
    }, nullptr);
    b.accessor("yaw", [](Value self_, std::span<const Value>) {
        HostAgent* h = unwrapAgent(self_);
        if (!h) return ev::undefined();
        return ev::fromDouble(h->agent.yaw());
    }, nullptr);
    b.def("setGoal", 3, [](Value self_, std::span<const Value> a) -> Value {
        HostAgent* h = unwrapAgent(self_);
        if (!h) return ev::undefined();
        if (h->destroyed) return ev::undefined();
        bromath::Vec3 target{0, 0, 0};
        if (a.size() >= 3) {
            target = { static_cast<float>(numAt(a, 0)), static_cast<float>(numAt(a, 1)), static_cast<float>(numAt(a, 2)) };
        } else if (a.size() >= 2 && !ev::isObject(a[0])) {
            target = { static_cast<float>(numAt(a, 0)), 0.0f, static_cast<float>(numAt(a, 1)) };
        } else if (!a.empty() && ev::isObject(a[0])) {
            target = parseVec3(a[0]);
        }

        if (h->navMesh) {
            float startY = h->navActive ? h->navY : h->agent.elevation();
            bromath::Vec3 start{h->agent.x(), startY, h->agent.z()};
            auto path = h->navMesh->findPathEx(start, target);
            if (!path.points.empty()) {
                h->navPath = std::move(path.points);
                h->navPathFlags = std::move(path.flags);
                h->navWaypoint = 0;
                h->navActive = true;
                h->navY = h->navPath.front().y;
                h->agent.setTarget(h->navPath[0].x, h->navPath[0].z);
            } else {
                h->navActive = false;
                h->navPath.clear();
                h->agent.clearTarget();
            }
        } else {
            h->navActive = false;
            h->navPath.clear();
            h->agent.setTarget(target.x, target.z);
        }
        return ev::undefined();
    });
    b.def("setTarget", 2, [](Value self, std::span<const Value> a) -> Value {
        HostAgent* h = unwrapAgent(self);
        if (!h) return ev::undefined();
        if (a.size() >= 2) {
            float x = static_cast<float>(numAt(a, 0));
            float z = static_cast<float>(numAt(a, 1));
            Value gv = makeVec3Value(x, 0, z);
            std::span<const Value> args(&gv, 1);
            Value setGoalFn = ev::getProperty(self, "setGoal");
            if (ev::isFunction(setGoalFn)) ev::call(setGoalFn, self, args);
        }
        return ev::undefined();
    });
    b.def("update", 1, [](Value self_, std::span<const Value> a) -> Value {
        HostAgent* h = unwrapAgent(self_);
        if (!h) return ev::undefined();
        if (h->destroyed) return ev::undefined();
        float dt = a.empty() ? (1.0f / 60.0f) : static_cast<float>(numAt(a, 0));

        if (h->navActive && h->navMesh) {
            constexpr float kAdvanceRadius = 0.75f;
            constexpr float kArriveRadius  = 0.5f;

            while (h->navWaypoint < static_cast<int>(h->navPath.size())) {
                const auto& wp = h->navPath[static_cast<size_t>(h->navWaypoint)];
                float dx = wp.x - h->agent.x();
                float dz = wp.z - h->agent.z();
                bool isLast = (h->navWaypoint == static_cast<int>(h->navPath.size()) - 1);
                float r = isLast ? kArriveRadius : kAdvanceRadius;
                if (dx * dx + dz * dz > r * r) break;
                h->navY = wp.y;
                h->navWaypoint++;
            }

            if (h->navWaypoint >= static_cast<int>(h->navPath.size())) {
                h->navActive = false;
                h->agent.clearTarget();
            } else {
                const auto& wp = h->navPath[static_cast<size_t>(h->navWaypoint)];
                h->agent.setTarget(wp.x, wp.z);
                const bromath::Vec3 from = (h->navWaypoint > 0) ? h->navPath[static_cast<size_t>(h->navWaypoint - 1)]
                                                               : h->navPath.front();
                float sx = wp.x - from.x, sz = wp.z - from.z;
                float segLenSq = sx * sx + sz * sz;
                if (segLenSq > 1e-6f) {
                    float t = ((h->agent.x() - from.x) * sx + (h->agent.z() - from.z) * sz) / segLenSq;
                    t = std::clamp(t, 0.0f, 1.0f);
                    h->navY = from.y + (wp.y - from.y) * t;
                } else {
                    h->navY = wp.y;
                }
            }
            h->agent.update(dt);
        } else {
            h->agent.update(dt);
        }
        return ev::undefined();
    });
    b.def("stop", 0, [](Value self_, std::span<const Value>) -> Value {
        HostAgent* h = unwrapAgent(self_);
        if (!h) return ev::undefined();
        if (h->destroyed) return ev::undefined();
        h->navActive = false;
        h->navPath.clear();
        h->agent.clearTarget();
        return ev::undefined();
    });
    b.def("destroy", 0, [](Value self_, std::span<const Value>) -> Value {
        HostAgent* h = unwrapAgent(self_);
        if (!h) return ev::undefined();
        h->destroyed = true;
        h->navActive = false;
        h->navPath.clear();
        h->agent.clearTarget();
        h->agent.setNavGrid(nullptr);
        h->navMesh.reset();
        return ev::undefined();
    });
    b.def("setPosition", 3, [](Value self_, std::span<const Value> a) -> Value {
        HostAgent* h = unwrapAgent(self_);
        if (!h) return ev::undefined();
        if (a.size() >= 3) {
            float x = static_cast<float>(numAt(a, 0));
            float y = static_cast<float>(numAt(a, 1));
            float z = static_cast<float>(numAt(a, 2));
            h->agent.setPosition(x, z);
            h->agent.setElevation(y);
            h->navY = y;
        } else if (a.size() >= 2) {
            float x = static_cast<float>(numAt(a, 0));
            float z = static_cast<float>(numAt(a, 1));
            h->agent.setPosition(x, z);
        } else if (!a.empty() && ev::isObject(a[0])) {
            auto p = parseVec3(a[0]);
            h->agent.setPosition(p.x, p.z);
            h->agent.setElevation(p.y);
            h->navY = p.y;
        }
        return ev::undefined();
    });
    b.def("setNavMesh", 1, [](Value self, std::span<const Value> a) -> Value {
        HostAgent* h = unwrapAgent(self);
        if (!h) return ev::undefined();
        if (a.empty()) return ev::undefined();
        if (auto* nm = unwrapNavMesh(a[0])) {
            h->navMesh = nm->mesh;
            ev::Persistent root(self);
            root.set(ev::setProperty(root.get(), "__navMesh", a[0]));
        } else {
            h->navMesh.reset();
        }
        return ev::undefined();
    });
    b.def("setNavGrid", 1, [](Value self, std::span<const Value> a) -> Value {
        HostAgent* h = unwrapAgent(self);
        if (!h) return ev::undefined();
        if (a.empty()) return ev::undefined();
        if (auto* ng = unwrapNavGrid(a[0])) {
            h->agent.setNavGrid(ng->grid.get());
            ev::Persistent root(self);
            root.set(ev::setProperty(root.get(), "__navGrid", a[0]));
        } else {
            h->agent.setNavGrid(nullptr);
        }
        return ev::undefined();
    });
    b.def("aimAt", 4, [](Value self_, std::span<const Value> a) -> Value {
        HostAgent* h = unwrapAgent(self_);
        if (!h) return ev::undefined();
        float tx = 0.0f, ty = 0.0f, tz = 0.0f, eyeH = 1.6f;
        if (a.size() >= 3) {
            tx = static_cast<float>(numAt(a, 0));
            ty = static_cast<float>(numAt(a, 1));
            tz = static_cast<float>(numAt(a, 2));
            if (a.size() >= 4) eyeH = static_cast<float>(numAt(a, 3));
        } else if (!a.empty() && ev::isObject(a[0])) {
            auto p = parseVec3(a[0]);
            tx = p.x; ty = p.y; tz = p.z;
            if (a.size() >= 2) eyeH = static_cast<float>(numAt(a, 1));
        }
        auto aim = h->agent.aimAt(tx, ty, tz, eyeH);
        ObjectBuilder res;
        res.set("yaw", ev::fromDouble(aim.yaw));
        res.set("pitch", ev::fromDouble(aim.pitch));
        return res.get();
    });
}

static Value makeAgentHandle(HostAgent* h) {
    ObjectBuilder b(g_agentClass.make(h, [](void* p) {
        delete static_cast<HostAgent*>(p);
    }));




















    return b.get();
}

// ---------------------------------------------------------------------------
// Global AI Factory Methods
// ---------------------------------------------------------------------------

static Value aiCreateNavGrid(Value, std::span<const Value> a) {
    Value opts = a.empty() ? ev::undefined() : a[0];
    if (!ev::isObject(opts)) {
        return ev::throwTypeError("createNavGrid(options) requires an options object");
    }

    ev::Persistent root(opts);
    double cellSize = getDoubleProperty(root.get(), "cellSize", 0.5);
    double minX = -20.0, minZ = -20.0, maxX = 20.0, maxZ = 20.0;

    Value widthV = ev::getProperty(root.get(), "width");
    Value heightV = ev::getProperty(root.get(), "height");
    if (!ev::isUndefined(widthV) && !ev::isUndefined(heightV) && !ev::isObject(widthV) && !ev::isObject(heightV)) {
        double w = ev::toDouble(widthV);
        double h = ev::toDouble(heightV);
        double ox = getDoubleProperty(root.get(), "originX", 0.0);
        double oz = getDoubleProperty(root.get(), "originZ", 0.0);
        minX = ox;
        minZ = oz;
        maxX = ox + w * cellSize;
        maxZ = oz + h * cellSize;
    } else {
        minX = getDoubleProperty(root.get(), "minX", -20.0);
        minZ = getDoubleProperty(root.get(), "minZ", -20.0);
        maxX = getDoubleProperty(root.get(), "maxX", 20.0);
        maxZ = getDoubleProperty(root.get(), "maxZ", 20.0);
    }

    auto grid = std::make_unique<brogameagent::NavGrid>(
        static_cast<float>(minX), static_cast<float>(minZ),
        static_cast<float>(maxX), static_cast<float>(maxZ),
        static_cast<float>(cellSize));

    double padding = getDoubleProperty(root.get(), "padding", 0.0);

    Value obsArr = ev::getProperty(root.get(), "obstacles");
    if (ev::isObject(obsArr)) {
        auto boxes = parseAABBArray(obsArr);
        for (const auto& box : boxes) {
            grid->addObstacle(box, static_cast<float>(padding));
        }
    }

    Value fromPhys = ev::getProperty(root.get(), "fromPhysics");
    if (!ev::isUndefined(fromPhys) && !ev::isNull(fromPhys) && ev::toBool(fromPhys)) {
        auto* e = hostEngine();
        auto* world = e ? e->physicsWorld() : nullptr;
        if (world) {
            for (const auto& b : world->collectStaticBodies()) {
                if (b.isSensor) continue;
                if (b.min.GetX() <= minX && b.max.GetX() >= maxX &&
                    b.min.GetZ() <= minZ && b.max.GetZ() >= maxZ) continue;
                brogameagent::AABB box{
                    0.5f * (b.min.GetX() + b.max.GetX()),
                    0.5f * (b.min.GetZ() + b.max.GetZ()),
                    0.5f * (b.max.GetX() - b.min.GetX()),
                    0.5f * (b.max.GetZ() - b.min.GetZ()),
                };
                grid->addObstacle(box, static_cast<float>(padding));
            }
        }
    }

    return makeNavGridHandle(std::move(grid));
}

static Value aiBakeNavMesh(Value, std::span<const Value> a) {
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

static Value aiLoadNavMesh(Value, std::span<const Value> a) {
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

static Value aiCreateAgent(Value, std::span<const Value> a) {
    auto* h = new HostAgent();

    if (!a.empty() && ev::isObject(a[0])) {
        Value opts = a[0];
        ev::Persistent root(opts);

        Value posV = ev::getProperty(root.get(), "position");
        bromath::Vec3 pos = parseVec3(posV);
        if (ev::isUndefined(posV)) {
            pos.x = static_cast<float>(getDoubleProperty(root.get(), "x", 0.0));
            pos.y = static_cast<float>(getDoubleProperty(root.get(), "y", getDoubleProperty(root.get(), "elevation", 0.0)));
            pos.z = static_cast<float>(getDoubleProperty(root.get(), "z", 0.0));
        }

        h->agent.setPosition(pos.x, pos.z);
        h->agent.setElevation(pos.y);
        h->navY = pos.y;

        double radius = getDoubleProperty(root.get(), "radius", 0.4);
        h->agent.setRadius(static_cast<float>(radius));

        double speed = getDoubleProperty(root.get(), "maxSpeed", getDoubleProperty(root.get(), "speed", 6.0));
        h->agent.setSpeed(static_cast<float>(speed));

        double maxAccel = getDoubleProperty(root.get(), "maxAcceleration", getDoubleProperty(root.get(), "maxAccel", -1.0));
        if (maxAccel > 0) h->agent.setMaxAccel(static_cast<float>(maxAccel));

        double maxTurnRate = getDoubleProperty(root.get(), "maxTurnRate", -1.0);
        if (maxTurnRate > 0) h->agent.setMaxTurnRate(static_cast<float>(maxTurnRate));

        Value nmV = ev::getProperty(root.get(), "navMesh");
        if (auto* nm = unwrapNavMesh(nmV)) {
            h->navMesh = nm->mesh;
        }

        Value ngV = ev::getProperty(root.get(), "navGrid");
        if (auto* ng = unwrapNavGrid(ngV)) {
            h->agent.setNavGrid(ng->grid.get());
        }
    }

    return makeAgentHandle(h);
}

static Value aiHasLineOfSight(Value, std::span<const Value> a) {
    if (a.size() < 5) return ev::fromBool(false);
    float fx = static_cast<float>(numAt(a, 0));
    float fz = static_cast<float>(numAt(a, 1));
    float tx = static_cast<float>(numAt(a, 2));
    float tz = static_cast<float>(numAt(a, 3));

    Value obsV = a[4];
    if (auto* ng = unwrapNavGrid(obsV)) {
        return ev::fromBool(ng->grid && ng->grid->hasGridLOS({fx, fz}, {tx, tz}));
    }

    auto boxes = parseAABBArray(obsV);
    bool los = brogameagent::hasLineOfSight({fx, fz}, {tx, tz}, boxes.data(), static_cast<int>(boxes.size()));
    return ev::fromBool(los);
}

static Value aiComputeAim(Value, std::span<const Value> a) {
    if (a.empty()) return ev::null();

    float ox = 0.0f, oy = 0.0f, oz = 0.0f;
    float tx = 0.0f, ty = 0.0f, tz = 0.0f;
    float speed = 0.0f;
    bool hasTargetVelocity = false;
    float tvx = 0.0f, tvy = 0.0f, tvz = 0.0f;

    if (a.size() >= 6 && !ev::isObject(a[0]) && !ev::isObject(a[1])) {
        ox = static_cast<float>(numAt(a, 0));
        oy = static_cast<float>(numAt(a, 1));
        oz = static_cast<float>(numAt(a, 2));
        tx = static_cast<float>(numAt(a, 3));
        ty = static_cast<float>(numAt(a, 4));
        tz = static_cast<float>(numAt(a, 5));
        if (a.size() >= 7) speed = static_cast<float>(numAt(a, 6));
    } else if (a.size() >= 2) {
        bromath::Vec3 p1 = parseVec3(a[0]);
        bromath::Vec3 p2 = parseVec3(a[1]);
        ox = p1.x; oy = p1.y; oz = p1.z;
        tx = p2.x; ty = p2.y; tz = p2.z;
        if (a.size() >= 3) speed = static_cast<float>(numAt(a, 2));

        if (ev::isObject(a[1])) {
            ev::Persistent tRoot(a[1]);
            Value velV = ev::getProperty(tRoot.get(), "velocity");
            if (ev::isObject(velV)) {
                auto v = parseVec3(velV);
                tvx = v.x; tvy = v.y; tvz = v.z;
                hasTargetVelocity = true;
            } else {
                Value vxV = ev::getProperty(tRoot.get(), "vx");
                Value vyV = ev::getProperty(tRoot.get(), "vy");
                Value vzV = ev::getProperty(tRoot.get(), "vz");
                if (!ev::isUndefined(vxV) || !ev::isUndefined(vzV)) {
                    tvx = static_cast<float>(getDoubleProperty(tRoot.get(), "vx", 0.0));
                    tvy = static_cast<float>(getDoubleProperty(tRoot.get(), "vy", 0.0));
                    tvz = static_cast<float>(getDoubleProperty(tRoot.get(), "vz", 0.0));
                    hasTargetVelocity = true;
                }
            }
        }
    }

    if (speed > 0.0f && hasTargetVelocity) {
        auto lead = brogameagent::computeLeadAim(ox, oy, oz, tx, ty, tz, tvx, tvy, tvz, speed);
        ObjectBuilder res;
        res.set("yaw", ev::fromDouble(lead.aim.yaw));
        res.set("pitch", ev::fromDouble(lead.aim.pitch));
        res.set("valid", ev::fromBool(lead.valid));
        res.set("timeToHit", ev::fromDouble(lead.timeToHit));
        return res.get();
    }

    auto aim = brogameagent::computeAim(ox, oy, oz, tx, ty, tz);
    ObjectBuilder res;
    res.set("yaw", ev::fromDouble(aim.yaw));
    res.set("pitch", ev::fromDouble(aim.pitch));
    res.set("valid", ev::fromBool(true));
    return res.get();
}

static Value makeAIObject() {
    ObjectBuilder b;
    b.def("createNavGrid", 1, aiCreateNavGrid);
    b.def("bakeNavMesh", 1, aiBakeNavMesh);
    b.def("loadNavMesh", 1, aiLoadNavMesh);
    b.def("createAgent", 1, aiCreateAgent);
    b.def("hasLineOfSight", 5, aiHasLineOfSight);
    b.def("computeAim", 3, aiComputeAim);
    return b.get();
}

}  // namespace

// ---------------------------------------------------------------------------
// Installation
// ---------------------------------------------------------------------------

void installAIGlobals() {
    Value aiVal = makeAIObject();
    ev::registerGlobal("AI", aiVal);
    g_navGridClass.install("AINavGrid", 0, nullptr, decorateNavGridProto);
    g_navMeshClass.install("AINavMesh", 0, nullptr, decorateNavMeshProto);
    g_agentClass.install("AIAgent", 0, nullptr, decorateAgentProto);
}

}  // namespace bro::bronze_host
