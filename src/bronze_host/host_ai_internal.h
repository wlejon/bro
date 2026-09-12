#pragma once

// Shared internal declarations and helpers for the bronze host AI & navigation module.

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
#include <brogameagent/hex_nav.h>
#include <brogameagent/world.h>
#include <brogameagent/perception.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace bro::bronze_host {

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
    ev::Persistent unitProxy;
};

struct HostUnit {
    uint32_t tag = kHostUnitTag;
    HostAgent* owner = nullptr;
    brogameagent::Agent* agentRef = nullptr;
    ev::Persistent agentValue;
    brogameagent::Agent* agent() const {
        if (owner) return &owner->agent;
        return agentRef;
    }
};

// bro.ai.game.createHexNav({ size }): the weighted hex-grid navigator. Owns
// its tables (copies), so the program's arrays may go away after a push.
struct HostHexNav {
    uint32_t tag = kHostHexNavTag;
    std::unique_ptr<brogameagent::HexNav> nav;
};

// bro.ai.game.createWorld(): the ORCA world. brogameagent::World holds RAW
// Agent pointers, so every agent added is rooted here until it is removed —
// a roster of Persistents the world's own handle owns. That is only legal
// because the handle is born Finalize::Deferred (embed.h): its destructor
// runs on a plain host stack at the finalizer drain, where releasing a root is
// an ordinary embed call rather than the forbidden mid-collection one
// host_internal.h's GC rule names.
struct HostWorld {
    uint32_t tag = kHostWorldTag;
    brogameagent::World world;
    struct Roster {
        const brogameagent::Agent* agent;  // identity for removeAgent
        ev::Persistent value;              // the handle, kept alive
    };
    std::vector<Roster> roster;
};

// ---------------------------------------------------------------------------
// Host Classes
// ---------------------------------------------------------------------------

extern HostClass g_navGridClass;
extern HostClass g_navMeshClass;
extern HostClass g_agentClass;
extern HostClass g_unitClass;
extern HostClass g_hexNavClass;
extern HostClass g_worldClass;

// ---------------------------------------------------------------------------
// Unwrap Helpers
// ---------------------------------------------------------------------------

inline HostNavGrid* unwrapNavGrid(Value v) {
    void* ptr = ev::handleData(v);
    if (!ptr) return nullptr;
    auto* h = static_cast<HostNavGrid*>(ptr);
    return (h->tag == kHostNavGridTag) ? h : nullptr;
}

inline HostNavMesh* unwrapNavMesh(Value v) {
    void* ptr = ev::handleData(v);
    if (!ptr) return nullptr;
    auto* h = static_cast<HostNavMesh*>(ptr);
    return (h->tag == kHostNavMeshTag) ? h : nullptr;
}

inline HostAgent* unwrapAgent(Value v) {
    void* ptr = ev::handleData(v);
    if (!ptr) return nullptr;
    auto* h = static_cast<HostAgent*>(ptr);
    return (h->tag == kHostAgentTag) ? h : nullptr;
}

inline HostUnit* unwrapUnit(Value v) {
    void* ptr = ev::handleData(v);
    if (!ptr) return nullptr;
    auto* h = static_cast<HostUnit*>(ptr);
    return (h->tag == kHostUnitTag) ? h : nullptr;
}

inline HostHexNav* unwrapHexNav(Value v) {
    void* ptr = ev::handleData(v);
    if (!ptr) return nullptr;
    auto* h = static_cast<HostHexNav*>(ptr);
    return (h->tag == kHostHexNavTag) ? h : nullptr;
}

inline HostWorld* unwrapWorld(Value v) {
    void* ptr = ev::handleData(v);
    if (!ptr) return nullptr;
    auto* h = static_cast<HostWorld*>(ptr);
    return (h->tag == kHostWorldTag) ? h : nullptr;
}

// ---------------------------------------------------------------------------
// Value Builders & Readers
// ---------------------------------------------------------------------------

inline Value makeVec2Value(float x, float z) {
    ObjectBuilder b;
    b.set("x", ev::fromDouble(x));
    b.set("z", ev::fromDouble(z));
    return b.get();
}


inline Value makePathArray(const std::vector<bromath::Vec3>& pts) {
    return hostArrayOf(pts.size(), [&](size_t i) {
        return makeVec3Value(pts[i].x, pts[i].y, pts[i].z);
    });
}

inline Value makePathArray(const std::vector<bromath::Vec2>& pts) {
    return hostArrayOf(pts.size(), [&](size_t i) {
        return makeVec2Value(pts[i].x, pts[i].y);
    });
}

inline double getDoubleProperty(Value obj, const char* key, double def = 0.0) {
    if (!ev::isObject(obj)) return def;
    ev::Persistent root(obj);
    Value v = ev::getProperty(root.get(), key);
    if (ev::isUndefined(v) || ev::isNull(v) || ev::isObject(v)) return def;
    double d = ev::toDouble(v);
    return std::isnan(d) ? def : d;
}

inline uint64_t getU64Property(Value obj, const char* key, uint64_t def = 0) {
    if (!ev::isObject(obj)) return def;
    ev::Persistent root(obj);
    Value v = ev::getProperty(root.get(), key);
    if (ev::isUndefined(v) || ev::isNull(v)) return def;
    return ev::toUint64(v);
}

inline bool getBoolProperty(Value obj, const char* key, bool def = false) {
    if (!ev::isObject(obj)) return def;
    ev::Persistent root(obj);
    Value v = ev::getProperty(root.get(), key);
    if (ev::isUndefined(v) || ev::isNull(v)) return def;
    return ev::toBool(v);
}

inline bromath::Vec2 parseVec2(Value v, bromath::Vec2 def = {0.0f, 0.0f}) {
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

inline bromath::Vec3 parseVec3(Value v, bromath::Vec3 def = {0.0f, 0.0f, 0.0f}) {
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

inline brogameagent::AABB parseAABB(Value v) {
    brogameagent::AABB box{0.0f, 0.0f, 0.5f, 0.5f};
    if (!ev::isObject(v)) return box;
    ev::Persistent root(v);

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

    Value hwV = ev::getProperty(root.get(), "hw");
    Value hdV = ev::getProperty(root.get(), "hd");
    Value cxV = ev::getProperty(root.get(), "cx");
    Value czV = ev::getProperty(root.get(), "cz");
    Value xV = ev::getProperty(root.get(), "x");
    Value zV = ev::getProperty(root.get(), "z");
    Value wV = ev::getProperty(root.get(), "width");
    Value dV = ev::getProperty(root.get(), "depth");

    if (!ev::isUndefined(hwV) || !ev::isUndefined(hdV)) {
        box.hw = !ev::isUndefined(hwV) ? static_cast<float>(ev::toDouble(hwV)) : 0.5f;
        box.hd = !ev::isUndefined(hdV) ? static_cast<float>(ev::toDouble(hdV)) : 0.5f;
        box.cx = !ev::isUndefined(cxV) ? static_cast<float>(ev::toDouble(cxV)) : (!ev::isUndefined(xV) ? static_cast<float>(ev::toDouble(xV)) : 0.0f);
        box.cz = !ev::isUndefined(czV) ? static_cast<float>(ev::toDouble(czV)) : (!ev::isUndefined(zV) ? static_cast<float>(ev::toDouble(zV)) : 0.0f);
        return box;
    }

    if (!ev::isUndefined(wV) && !ev::isUndefined(dV)) {
        float w = static_cast<float>(ev::toDouble(wV));
        float d = static_cast<float>(ev::toDouble(dV));
        float x = !ev::isUndefined(xV) ? static_cast<float>(ev::toDouble(xV)) : 0.0f;
        float z = !ev::isUndefined(zV) ? static_cast<float>(ev::toDouble(zV)) : 0.0f;
        box.cx = !ev::isUndefined(cxV) ? static_cast<float>(ev::toDouble(cxV)) : (x + 0.5f * w);
        box.cz = !ev::isUndefined(czV) ? static_cast<float>(ev::toDouble(czV)) : (z + 0.5f * d);
        box.hw = 0.5f * w;
        box.hd = 0.5f * d;
        return box;
    }

    return box;
}

inline std::vector<brogameagent::AABB> parseAABBArray(Value v) {
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

inline brogameagent::DamageKind parseDamageKind(const char* str) {
    if (str && strcmp(str, "magical") == 0) return brogameagent::DamageKind::Magical;
    if (str && strcmp(str, "true") == 0) return brogameagent::DamageKind::True;
    return brogameagent::DamageKind::Physical;
}

inline const char* damageKindStr(brogameagent::DamageKind k) {
    switch (k) {
        case brogameagent::DamageKind::Magical: return "magical";
        case brogameagent::DamageKind::True: return "true";
        default: return "physical";
    }
}


inline brogameagent::AgentAction parseAgentAction(Value obj) {
    brogameagent::AgentAction a;
    if (!ev::isObject(obj)) return a;
    ev::Persistent root(obj);
    a.moveX = static_cast<float>(getDoubleProperty(root.get(), "moveX", 0.0));
    a.moveZ = static_cast<float>(getDoubleProperty(root.get(), "moveZ", 0.0));
    a.aimYaw = static_cast<float>(getDoubleProperty(root.get(), "aimYaw", 0.0));
    a.aimPitch = static_cast<float>(getDoubleProperty(root.get(), "aimPitch", 0.0));
    a.attackTargetId = static_cast<int>(getDoubleProperty(root.get(), "attackTargetId", -1.0));
    a.useAbilityId = static_cast<int>(getDoubleProperty(root.get(), "useAbilityId",
                     getDoubleProperty(root.get(), "abilitySlot", -1.0)));
    return a;
}

// ---------------------------------------------------------------------------
// Prototypes & Factory Declarations
// ---------------------------------------------------------------------------

// NavGrid (host_ai_navgrid.cpp)
void decorateNavGridProto(ObjectBuilder& b);
Value makeNavGridHandle(std::unique_ptr<brogameagent::NavGrid> grid);
inline Value makeNavGridValue(std::unique_ptr<brogameagent::NavGrid> grid) { return makeNavGridHandle(std::move(grid)); }
Value aiCreateNavGrid(Value, std::span<const Value> a);

// NavMesh (host_ai_navmesh.cpp)
void decorateNavMeshProto(ObjectBuilder& b);
Value makeNavMeshHandle(std::shared_ptr<brogameagent::NavMesh> mesh);
inline Value makeNavMeshValue(std::shared_ptr<brogameagent::NavMesh> mesh) { return makeNavMeshHandle(std::move(mesh)); }
Value aiBakeNavMesh(Value, std::span<const Value> a);
Value aiLoadNavMesh(Value, std::span<const Value> a);

// Unit (host_ai_unit.cpp)
void decorateUnitProto(ObjectBuilder& b);
Value makeUnitHandle(HostAgent* h, Value agentVal = ev::undefined());
void ensureAIClassesInstalled();

// Agent (host_ai_agent.cpp)
void decorateAgentProto(ObjectBuilder& b);
Value makeAgentHandle(HostAgent* h);
inline Value makeAgentValue(HostAgent* h) { return makeAgentHandle(h); }
Value aiCreateAgent(Value, std::span<const Value> a);
void applyAgentAvoidance(Value opts, brogameagent::Agent& agent);

// HexNav + World + the bro.ai.game object (host_ai_game.cpp)
void decorateHexNavProto(ObjectBuilder& b);
void decorateWorldProto(ObjectBuilder& b);
Value aiCreateHexNav(Value, std::span<const Value> a);
Value aiCreateWorld(Value, std::span<const Value> a);
Value aiCanSee(Value, std::span<const Value> a);
Value aiComputeLeadAim(Value, std::span<const Value> a);
Value makeAiGameValue();

// Extras & MCTS (host_ai_extras.cpp, host_ai_mcts.cpp)
void installAIExtras(ObjectBuilder& b);
void installAIMcts(ObjectBuilder& b);
void installRegisterCapability(ObjectBuilder& b);

// Core (host_ai_core.cpp)
Value aiHasLineOfSight(Value, std::span<const Value> a);
Value aiComputeAim(Value, std::span<const Value> a);
Value makeAIObject();
inline Value makeAiObject() { return makeAIObject(); }

}  // namespace bro::bronze_host
