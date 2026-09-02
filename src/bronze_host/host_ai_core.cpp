// Global AI namespace, core math/aim utilities, and host class registration for bronze_host.

#include "bronze_host/host_ai_internal.h"

namespace bro::bronze_host {

// ---------------------------------------------------------------------------
// Host Classes
// ---------------------------------------------------------------------------

HostClass g_navGridClass;
HostClass g_navMeshClass;
HostClass g_agentClass;
HostClass g_hexNavClass;
HostClass g_worldClass;

// ---------------------------------------------------------------------------
// Global AI Utilities
// ---------------------------------------------------------------------------

Value aiHasLineOfSight(Value, std::span<const Value> a) {
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

Value aiComputeAim(Value, std::span<const Value> a) {
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

Value makeAIObject() {
    ObjectBuilder b;
    b.def("createNavGrid", 1, aiCreateNavGrid);
    b.def("bakeNavMesh", 1, aiBakeNavMesh);
    b.def("loadNavMesh", 1, aiLoadNavMesh);
    b.def("createAgent", 1, aiCreateAgent);
    b.def("hasLineOfSight", 5, aiHasLineOfSight);
    b.def("computeAim", 3, aiComputeAim);
    return b.get();
}

// ---------------------------------------------------------------------------
// Installation
// ---------------------------------------------------------------------------

void installAIGlobals() {
    Value aiVal = makeAIObject();
    ev::registerGlobal("AI", aiVal);
    g_navGridClass.install("AINavGrid", 0, nullptr, decorateNavGridProto);
    g_navMeshClass.install("AINavMesh", 0, nullptr, decorateNavMeshProto);
    g_agentClass.install("AIAgent", 0, nullptr, decorateAgentProto);
    // The two classes only bro.ai.game hands out (host_ai_game.cpp). Named
    // as the QuickJS binding names them, and registered because every class
    // this layer births instances on is — an instance answers `instanceof`
    // for a name the program can read.
    g_hexNavClass.install("AIHexNav", 0, nullptr, decorateHexNavProto);
    g_worldClass.install("AIWorld", 0, nullptr, decorateWorldProto);
}

}  // namespace bro::bronze_host
