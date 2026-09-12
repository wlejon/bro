// Spatial queries and contact event listeners for the bronze host physics module.

#include "bronze_host/host_physics_internal.h"

namespace bro::bronze_host {

namespace {

bool readQueryShape(Value optsVal, physics::BodyOptions& shape, std::string& err) {
    if (!readBodyOptions(optsVal, shape, err)) return false;
    switch (shape.shape) {
        case physics::BodyOptions::ShapeBox:
        case physics::BodyOptions::ShapeSphere:
        case physics::BodyOptions::ShapeCapsule:
        case physics::BodyOptions::ShapeCylinder:
        case physics::BodyOptions::ShapeConvexHull:
            return true;
        default:
            err = "query shape must be convex (box|sphere|capsule|cylinder|convexHull)";
            return false;
    }
}

Value makeCastHitValue(HostPhysicsWorld* pw, physics::PhysicsWorld* world, const physics::ShapeCastHit& hit) {
    ObjectBuilder obj;
    obj.set("bodyId", ev::fromDouble(pw->tagForBodyId(hit.bodyID)));
    obj.set("fraction", ev::fromDouble(hit.fraction));
    uint64_t udata = hit.bodyID.IsInvalid() ? 0 : world->getUserData(hit.bodyID);
    obj.set("userData", makeBigIntValue(udata));
    obj.set("position", makeVec3Value(hit.position.GetX(), hit.position.GetY(), hit.position.GetZ()));
    obj.set("normal", makeVec3Value(hit.normal.GetX(), hit.normal.GetY(), hit.normal.GetZ()));
    return obj.get();
}

}  // namespace

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
    b.def("raycast", 8, [](Value self, std::span<const Value> a) -> Value {
        HostPhysicsWorld* pw = unwrapWorld(self);
        auto* world = pw->getWorld();
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
            if (hasArg(a, 2)) {
                if (ev::isObject(a[2])) readQueryFilter(a[2], filter, pw);
                else filter.layerMask = static_cast<uint32_t>(numAt(a, 2));
            }
        } else if (hasArg(a, 5)) {
            origin = JPH::RVec3(static_cast<float>(numAt(a, 0)), static_cast<float>(numAt(a, 1)), static_cast<float>(numAt(a, 2)));
            dir = JPH::Vec3(static_cast<float>(numAt(a, 3)), static_cast<float>(numAt(a, 4)), static_cast<float>(numAt(a, 5)));
            if (hasArg(a, 6)) {
                if (ev::isObject(a[6])) readQueryFilter(a[6], filter, pw);
                else maxDist = static_cast<float>(numAt(a, 6));
            }
            if (hasArg(a, 7) && ev::isObject(a[7])) readQueryFilter(a[7], filter, pw);
        }

        auto hits = world->raycast(origin, dir, maxDist, filter);
        return hostArrayOf(hits.size(), [&](size_t i) {
            const auto& hit = hits[i];
            int32_t tag = pw->tagForBodyId(hit.bodyID);
            uint64_t udata = hit.bodyID.IsInvalid() ? 0 : world->getUserData(hit.bodyID);
            return makeRayHitValue(tag, hit, udata);
        });
    });

    b.def("raycastClosest", 8, [](Value self, std::span<const Value> a) -> Value {
        HostPhysicsWorld* pw = unwrapWorld(self);
        auto* world = pw->getWorld();
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
            if (hasArg(a, 2)) {
                if (ev::isObject(a[2])) readQueryFilter(a[2], filter, pw);
                else filter.layerMask = static_cast<uint32_t>(numAt(a, 2));
            }
        } else if (hasArg(a, 5)) {
            origin = JPH::RVec3(static_cast<float>(numAt(a, 0)), static_cast<float>(numAt(a, 1)), static_cast<float>(numAt(a, 2)));
            dir = JPH::Vec3(static_cast<float>(numAt(a, 3)), static_cast<float>(numAt(a, 4)), static_cast<float>(numAt(a, 5)));
            if (hasArg(a, 6)) {
                if (ev::isObject(a[6])) readQueryFilter(a[6], filter, pw);
                else maxDist = static_cast<float>(numAt(a, 6));
            }
            if (hasArg(a, 7) && ev::isObject(a[7])) readQueryFilter(a[7], filter, pw);
        }

        physics::RayHit hit;
        bool ok = world->raycastClosest(origin, dir, hit, maxDist, filter);
        if (!ok) return ev::null();
        int32_t tag = pw->tagForBodyId(hit.bodyID);
        uint64_t udata = hit.bodyID.IsInvalid() ? 0 : world->getUserData(hit.bodyID);
        return makeRayHitValue(tag, hit, udata);
    });

    b.def("castShape", 1, [](Value self, std::span<const Value> a) -> Value {
        HostPhysicsWorld* pw = unwrapWorld(self);
        auto* world = pw->getWorld();
        if (!world) return hostArrayOf(0, [](size_t) { return ev::undefined(); });
        if (a.empty() || !ev::isObject(a[0])) return ev::throwTypeError("castShape(opts) requires an object");

        physics::BodyOptions shape;
        std::string err;
        if (!readQueryShape(a[0], shape, err)) return ev::throwTypeError(err);

        physics::QueryFilter filter;
        readQueryFilter(a[0], filter, pw);

        ev::Persistent opts(a[0]);
        Value dv = ev::getProperty(opts.get(), "direction");
        JPH::Vec3 dir = readVec3(dv);
        if (dir == JPH::Vec3::sZero()) return ev::throwTypeError("castShape requires a non-zero direction");
        double maxDist = getPropNumber(opts, "maxDistance", 1000.0);

        auto hits = world->castShape(shape, dir, static_cast<float>(maxDist), filter);
        return hostArrayOf(hits.size(), [&](size_t i) {
            return makeCastHitValue(pw, world, hits[i]);
        });
    });

    b.def("castShapeClosest", 1, [](Value self, std::span<const Value> a) -> Value {
        HostPhysicsWorld* pw = unwrapWorld(self);
        auto* world = pw->getWorld();
        if (!world) return ev::null();
        if (a.empty() || !ev::isObject(a[0])) return ev::throwTypeError("castShapeClosest(opts) requires an object");

        physics::BodyOptions shape;
        std::string err;
        if (!readQueryShape(a[0], shape, err)) return ev::throwTypeError(err);

        physics::QueryFilter filter;
        readQueryFilter(a[0], filter, pw);

        ev::Persistent opts(a[0]);
        Value dv = ev::getProperty(opts.get(), "direction");
        JPH::Vec3 dir = readVec3(dv);
        if (dir == JPH::Vec3::sZero()) return ev::throwTypeError("castShape requires a non-zero direction");
        double maxDist = getPropNumber(opts, "maxDistance", 1000.0);

        physics::ShapeCastHit hit;
        if (!world->castShapeClosest(shape, dir, static_cast<float>(maxDist), hit, filter)) return ev::null();
        return makeCastHitValue(pw, world, hit);
    });

    b.def("overlapShape", 1, [](Value self, std::span<const Value> a) -> Value {
        HostPhysicsWorld* pw = unwrapWorld(self);
        auto* world = pw->getWorld();
        if (!world) return hostArrayOf(0, [](size_t) { return ev::undefined(); });
        if (a.empty() || !ev::isObject(a[0])) return ev::throwTypeError("overlapShape(opts) requires an object");

        physics::BodyOptions shape;
        std::string err;
        if (!readQueryShape(a[0], shape, err)) return ev::throwTypeError(err);

        physics::QueryFilter filter;
        readQueryFilter(a[0], filter, pw);

        auto hits = world->overlapShape(shape, filter);
        return hostArrayOf(hits.size(), [&](size_t i) {
            const auto& hit = hits[i];
            ObjectBuilder obj;
            obj.set("bodyId", ev::fromDouble(pw->tagForBodyId(hit.bodyID)));
            obj.set("depth", ev::fromDouble(hit.depth));
            uint64_t udata = hit.bodyID.IsInvalid() ? 0 : world->getUserData(hit.bodyID);
            obj.set("userData", makeBigIntValue(udata));
            obj.set("position", makeVec3Value(hit.position.GetX(), hit.position.GetY(), hit.position.GetZ()));
            obj.set("normal", makeVec3Value(hit.normal.GetX(), hit.normal.GetY(), hit.normal.GetZ()));
            return obj.get();
        });
    });

    b.def("overlapPoint", 4, [](Value self, std::span<const Value> a) -> Value {
        HostPhysicsWorld* pw = unwrapWorld(self);
        auto* world = pw->getWorld();
        if (!world || a.size() < 3) return hostArrayOf(0, [](size_t) { return ev::undefined(); });
        JPH::RVec3 pt(static_cast<float>(numAt(a, 0)), static_cast<float>(numAt(a, 1)), static_cast<float>(numAt(a, 2)));
        physics::QueryFilter filter;
        if (hasArg(a, 3) && ev::isObject(a[3])) readQueryFilter(a[3], filter, pw);

        auto bodies = world->overlapPoint(pt, filter);
        Value arr = hostArrayOf(bodies.size(), [&](size_t i) {
            int32_t tag = pw->tagForBodyId(bodies[i]);
            uint64_t udata = bodies[i].IsInvalid() ? 0 : world->getUserData(bodies[i]);
            ObjectBuilder obj;
            obj.set("bodyId", ev::fromDouble(tag));
            obj.set("userData", makeBigIntValue(udata));
            return obj.get();
        });

        ObjectBuilder arrBuilder(arr);
        arrBuilder.def("includes", 1, [](Value thisArr, std::span<const Value> args) -> Value {
            if (args.empty()) return ev::fromBool(false);
            Value search = args[0];
            double searchNum = ev::isNumber(search) ? ev::toDouble(search) : -999999.0;
            ev::Persistent rooted(thisArr);
            Value lenVal = ev::getProperty(rooted.get(), "length");
            uint32_t len = ev::isNumber(lenVal) ? static_cast<uint32_t>(ev::toDouble(lenVal)) : 0;
            for (uint32_t i = 0; i < len; ++i) {
                Value item = ev::getElement(rooted.get(), i);
                if (ev::isNumber(search) && ev::isNumber(item) && ev::toDouble(item) == searchNum) {
                    return ev::fromBool(true);
                }
                if (ev::isObject(item)) {
                    ev::Persistent rootedItem(item);
                    Value bid = ev::getProperty(rootedItem.get(), "bodyId");
                    if (ev::isNumber(bid) && ev::toDouble(bid) == searchNum) {
                        return ev::fromBool(true);
                    }
                }
            }
            return ev::fromBool(false);
        });
        arrBuilder.def("indexOf", 1, [](Value thisArr, std::span<const Value> args) -> Value {
            if (args.empty()) return ev::fromDouble(-1);
            Value search = args[0];
            double searchNum = ev::isNumber(search) ? ev::toDouble(search) : -999999.0;
            ev::Persistent rooted(thisArr);
            Value lenVal = ev::getProperty(rooted.get(), "length");
            uint32_t len = ev::isNumber(lenVal) ? static_cast<uint32_t>(ev::toDouble(lenVal)) : 0;
            for (uint32_t i = 0; i < len; ++i) {
                Value item = ev::getElement(rooted.get(), i);
                if (ev::isNumber(search) && ev::isNumber(item) && ev::toDouble(item) == searchNum) {
                    return ev::fromDouble(static_cast<double>(i));
                }
                if (ev::isObject(item)) {
                    ev::Persistent rootedItem(item);
                    Value bid = ev::getProperty(rootedItem.get(), "bodyId");
                    if (ev::isNumber(bid) && ev::toDouble(bid) == searchNum) {
                        return ev::fromDouble(static_cast<double>(i));
                    }
                }
            }
            return ev::fromDouble(-1);
        });
        return arrBuilder.get();
    });

    b.def("overlapSphere", 3, [](Value self, std::span<const Value> a) -> Value {
        HostPhysicsWorld* pw = unwrapWorld(self);
        auto* world = pw->getWorld();
        if (!world || a.empty()) return hostArrayOf(0, [](size_t) { return ev::undefined(); });

        physics::BodyOptions shape;
        shape.shape = physics::BodyOptions::ShapeSphere;
        shape.position = readRVec3(a[0]);
        shape.radius = hasArg(a, 1) ? static_cast<float>(numAt(a, 1)) : 0.5f;

        physics::QueryFilter filter;
        if (hasArg(a, 2)) {
            if (ev::isObject(a[2])) readQueryFilter(a[2], filter, pw);
            else filter.layerMask = static_cast<uint32_t>(numAt(a, 2));
        }

        auto hits = world->overlapShape(shape, filter);
        return hostArrayOf(hits.size(), [&](size_t i) {
            int32_t tag = pw->tagForBodyId(hits[i].bodyID);
            return ev::fromDouble(tag);
        });
    });

    b.def("overlapBox", 4, [](Value self, std::span<const Value> a) -> Value {
        HostPhysicsWorld* pw = unwrapWorld(self);
        auto* world = pw->getWorld();
        if (!world || a.empty()) return hostArrayOf(0, [](size_t) { return ev::undefined(); });

        physics::BodyOptions shape;
        shape.shape = physics::BodyOptions::ShapeBox;
        shape.position = readRVec3(a[0]);
        shape.halfExtents = hasArg(a, 1) ? readVec3(a[1], JPH::Vec3(0.5f, 0.5f, 0.5f)) : JPH::Vec3(0.5f, 0.5f, 0.5f);
        if (hasArg(a, 2) && ev::isObject(a[2])) shape.rotation = readQuat(a[2]);

        physics::QueryFilter filter;
        if (hasArg(a, 3)) {
            if (ev::isObject(a[3])) readQueryFilter(a[3], filter, pw);
            else filter.layerMask = static_cast<uint32_t>(numAt(a, 3));
        }

        auto hits = world->overlapShape(shape, filter);
        return hostArrayOf(hits.size(), [&](size_t i) {
            int32_t tag = pw->tagForBodyId(hits[i].bodyID);
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

    b.def("getContacts", 0, [](Value self, std::span<const Value>) {
        HostPhysicsWorld* pw = unwrapWorld(self);
        auto* world = pw->getWorld();
        if (!world) return hostArrayOf(0, [](size_t) { return ev::undefined(); });

        bool overflowed = false;
        auto events = world->drainContactEvents(&overflowed);
        pw->lastContactEvents = events;
        pw->lastContactOverflow = overflowed;

        const auto& evts = pw->lastContactEvents;
        Value arr = hostArrayOf(evts.size(), [&](size_t i) {
            const auto& e = evts[i];
            ObjectBuilder obj;
            const char* typeStr = (e.type == physics::ContactEvent::Added) ? "added" : "removed";
            obj.set("type", ev::fromUtf8(typeStr));
            int32_t t1 = pw->tagForBodyId(e.body1);
            int32_t t2 = pw->tagForBodyId(e.body2);
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
        ev::setProperty(arr, "overflow", ev::fromBool(pw->lastContactOverflow));
        return arr;
    });
}

}  // namespace bro::bronze_host
