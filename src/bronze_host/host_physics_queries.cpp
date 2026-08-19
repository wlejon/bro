// Spatial queries and contact event listeners for the bronze host physics module.

#include "bronze_host/host_physics_internal.h"

namespace bro::bronze_host {

void drainPhysicsContactEvents() {
    auto* world = getPhysicsWorld();
    if (!world) return;

    bool overflowed = false;
    auto events = world->drainContactEvents(&overflowed);
    g_phys.lastContactOverflow = overflowed;
    g_phys.lastContactEvents = events;

    if (events.empty() || ev::isUndefined(g_phys.physicsObj.get())) return;

    std::vector<ev::Persistent> handlers;
    {
        Value on = ev::getProperty(g_phys.physicsObj.get(), "oncontact");
        if (ev::isFunction(on)) handlers.emplace_back(on);
    }
    {
        Value on = ev::getProperty(g_phys.physicsObj.get(), "onContact");
        if (ev::isFunction(on)) handlers.emplace_back(on);
    }
    for (ev::Persistent& entry : hostListSnapshot(g_phys.physicsObj, "__bronzeHostListeners_contact")) {
        if (ev::isFunction(entry.get())) handlers.push_back(std::move(entry));
    }
    if (handlers.empty()) return;

    for (const auto& e : events) {
        int32_t t1 = g_phys.tagForBodyId(e.body1);
        int32_t t2 = g_phys.tagForBodyId(e.body2);

        for (auto& handler : handlers) {
            ev::Persistent evt(ev::createObject());
            const char* typeStr = (e.type == physics::ContactEvent::Added) ? "added" : "removed";
            evt.set(ev::setProperty(evt.get(), "type", ev::fromUtf8(typeStr)));
            evt.set(ev::setProperty(evt.get(), "body1", ev::fromDouble(t1)));
            evt.set(ev::setProperty(evt.get(), "body2", ev::fromDouble(t2)));
            evt.set(ev::setProperty(evt.get(), "sensor", ev::fromBool(e.isSensor)));
            if (e.type == physics::ContactEvent::Added) {
                evt.set(ev::setProperty(evt.get(), "normal", makeVec3Value(e.normal.x, e.normal.y, e.normal.z)));
                evt.set(ev::setProperty(evt.get(), "penetration", ev::fromDouble(e.penetration)));
                evt.set(ev::setProperty(evt.get(), "impulse", ev::fromDouble(e.impulse)));
                Value pts = hostArrayOf(e.numPoints, [&](size_t k) {
                    return makeVec3Value(e.points[k].x, e.points[k].y, e.points[k].z);
                });
                evt.set(ev::setProperty(evt.get(), "points", pts));
            }

            Value arg = evt.get();
            ev::CallResult r = ev::call(handler.get(), g_phys.physicsObj.get(), std::span<const Value>(&arg, 1));
            if (r.thrown) {
                reportBronzeError("Physics.onContact", r.value);
            }
        }
    }
}

void registerQueryMethods(ObjectBuilder& b) {
    // Spatial Queries
    b.def("raycast", 8, [](Value, std::span<const Value> a) -> Value {
        auto* world = getPhysicsWorld();
        if (!world || a.size() < 2) return hostArrayOf(0, [](size_t) { return ev::undefined(); });

        JPH::RVec3 origin;
        JPH::Vec3 dir;
        float maxDist = 1000.0f;
        physics::QueryFilter filter;

        if (ev::isObject(a[0])) {
            origin = readRVec3(a[0]);
            JPH::RVec3 target = readRVec3(a[1]);
            JPH::Vec3 diff = JPH::Vec3(target - origin);
            float len = diff.Length();
            maxDist = len > 0.0f ? len : 1000.0f;
            dir = len > 1e-6f ? diff / len : JPH::Vec3(0, 0, 0);
            if (a.size() >= 3) {
                if (ev::isObject(a[2])) readQueryFilter(a[2], filter);
                else filter.layerMask = static_cast<uint32_t>(numAt(a, 2));
            }
        } else if (a.size() >= 6) {
            origin = JPH::RVec3(static_cast<float>(numAt(a, 0)), static_cast<float>(numAt(a, 1)), static_cast<float>(numAt(a, 2)));
            dir = JPH::Vec3(static_cast<float>(numAt(a, 3)), static_cast<float>(numAt(a, 4)), static_cast<float>(numAt(a, 5)));
            if (a.size() >= 7) {
                if (ev::isObject(a[6])) readQueryFilter(a[6], filter);
                else maxDist = static_cast<float>(numAt(a, 6));
            }
            if (a.size() >= 8 && ev::isObject(a[7])) readQueryFilter(a[7], filter);
        }

        auto hits = world->raycast(origin, dir, maxDist, filter);
        return hostArrayOf(hits.size(), [&](size_t i) {
            const auto& hit = hits[i];
            int32_t tag = g_phys.tagForBodyId(hit.bodyID);
            uint64_t udata = hit.bodyID.IsInvalid() ? 0 : world->getUserData(hit.bodyID);
            return makeRayHitValue(tag, hit, udata);
        });
    });

    b.def("raycastClosest", 8, [](Value, std::span<const Value> a) -> Value {
        auto* world = getPhysicsWorld();
        if (!world || a.size() < 2) return ev::null();

        JPH::RVec3 origin;
        JPH::Vec3 dir;
        float maxDist = 1000.0f;
        physics::QueryFilter filter;

        if (ev::isObject(a[0])) {
            origin = readRVec3(a[0]);
            JPH::RVec3 target = readRVec3(a[1]);
            JPH::Vec3 diff = JPH::Vec3(target - origin);
            float len = diff.Length();
            maxDist = len > 0.0f ? len : 1000.0f;
            dir = len > 1e-6f ? diff / len : JPH::Vec3(0, 0, 0);
            if (a.size() >= 3) {
                if (ev::isObject(a[2])) readQueryFilter(a[2], filter);
                else filter.layerMask = static_cast<uint32_t>(numAt(a, 2));
            }
        } else if (a.size() >= 6) {
            origin = JPH::RVec3(static_cast<float>(numAt(a, 0)), static_cast<float>(numAt(a, 1)), static_cast<float>(numAt(a, 2)));
            dir = JPH::Vec3(static_cast<float>(numAt(a, 3)), static_cast<float>(numAt(a, 4)), static_cast<float>(numAt(a, 5)));
            if (a.size() >= 7) {
                if (ev::isObject(a[6])) readQueryFilter(a[6], filter);
                else maxDist = static_cast<float>(numAt(a, 6));
            }
            if (a.size() >= 8 && ev::isObject(a[7])) readQueryFilter(a[7], filter);
        }

        physics::RayHit hit;
        bool ok = world->raycastClosest(origin, dir, hit, maxDist, filter);
        if (!ok) return ev::null();
        int32_t tag = g_phys.tagForBodyId(hit.bodyID);
        uint64_t udata = hit.bodyID.IsInvalid() ? 0 : world->getUserData(hit.bodyID);
        return makeRayHitValue(tag, hit, udata);
    });

    b.def("overlapSphere", 3, [](Value, std::span<const Value> a) -> Value {
        auto* world = getPhysicsWorld();
        if (!world || a.empty()) return hostArrayOf(0, [](size_t) { return ev::undefined(); });

        physics::BodyOptions shape;
        shape.shape = physics::BodyOptions::ShapeSphere;
        shape.position = readRVec3(a[0]);
        shape.radius = a.size() >= 2 ? static_cast<float>(numAt(a, 1)) : 0.5f;

        physics::QueryFilter filter;
        if (a.size() >= 3) {
            if (ev::isObject(a[2])) readQueryFilter(a[2], filter);
            else filter.layerMask = static_cast<uint32_t>(numAt(a, 2));
        }

        auto hits = world->overlapShape(shape, filter);
        return hostArrayOf(hits.size(), [&](size_t i) {
            int32_t tag = g_phys.tagForBodyId(hits[i].bodyID);
            return ev::fromDouble(tag);
        });
    });

    b.def("overlapBox", 4, [](Value, std::span<const Value> a) -> Value {
        auto* world = getPhysicsWorld();
        if (!world || a.empty()) return hostArrayOf(0, [](size_t) { return ev::undefined(); });

        physics::BodyOptions shape;
        shape.shape = physics::BodyOptions::ShapeBox;
        shape.position = readRVec3(a[0]);
        shape.halfExtents = a.size() >= 2 ? readVec3(a[1], JPH::Vec3(0.5f, 0.5f, 0.5f)) : JPH::Vec3(0.5f, 0.5f, 0.5f);
        if (a.size() >= 3 && ev::isObject(a[2])) shape.rotation = readQuat(a[2]);

        physics::QueryFilter filter;
        if (a.size() >= 4) {
            if (ev::isObject(a[3])) readQueryFilter(a[3], filter);
            else filter.layerMask = static_cast<uint32_t>(numAt(a, 3));
        }

        auto hits = world->overlapShape(shape, filter);
        return hostArrayOf(hits.size(), [&](size_t i) {
            int32_t tag = g_phys.tagForBodyId(hits[i].bodyID);
            return ev::fromDouble(tag);
        });
    });

    b.def("overlapPoint", 4, [](Value, std::span<const Value> a) -> Value {
        auto* world = getPhysicsWorld();
        if (!world || a.size() < 3) return hostArrayOf(0, [](size_t) { return ev::undefined(); });
        JPH::RVec3 pt(static_cast<float>(numAt(a, 0)), static_cast<float>(numAt(a, 1)), static_cast<float>(numAt(a, 2)));
        physics::QueryFilter filter;
        if (a.size() >= 4 && ev::isObject(a[3])) readQueryFilter(a[3], filter);

        auto bodies = world->overlapPoint(pt, filter);
        return hostArrayOf(bodies.size(), [&](size_t i) {
            int32_t tag = g_phys.tagForBodyId(bodies[i]);
            return ev::fromDouble(tag);
        });
    });

    // Contact Events
    b.def("onContact", 1, [](Value, std::span<const Value> a) {
        if (!a.empty() && ev::isFunction(a[0])) {
            addHostListener(g_phys.physicsObj, "contact", a[0]);
        }
        return ev::undefined();
    });

    b.def("addEventListener", 2, [](Value, std::span<const Value> a) {
        if (a.size() >= 2 && ev::isFunction(a[1])) {
            std::string type = !ev::isObject(a[0]) ? ev::toUtf8(a[0]) : "";
            if (!type.empty()) {
                addHostListener(g_phys.physicsObj, type, a[1]);
            }
        }
        return ev::undefined();
    });

    b.def("removeEventListener", 2, [](Value, std::span<const Value> a) {
        if (a.size() >= 2 && ev::isFunction(a[1])) {
            std::string type = !ev::isObject(a[0]) ? ev::toUtf8(a[0]) : "";
            if (!type.empty()) {
                removeHostListener(g_phys.physicsObj, type, a[1]);
            }
        }
        return ev::undefined();
    });

    b.def("getContacts", 0, [](Value, std::span<const Value>) {
        auto* world = getPhysicsWorld();
        if (!world) return hostArrayOf(0, [](size_t) { return ev::undefined(); });

        bool overflowed = false;
        auto events = world->drainContactEvents(&overflowed);
        if (!events.empty()) {
            g_phys.lastContactEvents = events;
            g_phys.lastContactOverflow = overflowed;
        }

        const auto& evts = g_phys.lastContactEvents;
        return hostArrayOf(evts.size(), [&](size_t i) {
            const auto& e = evts[i];
            ObjectBuilder obj;
            const char* typeStr = (e.type == physics::ContactEvent::Added) ? "added" : "removed";
            obj.set("type", ev::fromUtf8(typeStr));
            int32_t t1 = g_phys.tagForBodyId(e.body1);
            int32_t t2 = g_phys.tagForBodyId(e.body2);
            obj.set("body1", ev::fromDouble(t1));
            obj.set("body2", ev::fromDouble(t2));
            obj.set("sensor", ev::fromBool(e.isSensor));
            if (e.type == physics::ContactEvent::Added) {
                obj.set("normal", makeVec3Value(e.normal.x, e.normal.y, e.normal.z));
                obj.set("penetration", ev::fromDouble(e.penetration));
                obj.set("impulse", ev::fromDouble(e.impulse));
                Value pts = hostArrayOf(e.numPoints, [&](size_t k) {
                    return makeVec3Value(e.points[k].x, e.points[k].y, e.points[k].z);
                });
                obj.set("points", pts);
            }
            return obj.get();
        });
    });
}

}  // namespace bro::bronze_host
