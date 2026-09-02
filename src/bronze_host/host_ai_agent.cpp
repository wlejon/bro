// AIAgent implementation for bronze_host.
// Kinematic/pathed agent, steering, route tracking, navmesh & navgrid following.

#include "bronze_host/host_ai_internal.h"

namespace bro::bronze_host {

void decorateAgentProto(ObjectBuilder& b) {
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

    // The docs' bare coordinates (docs/ai-game-api.js `bot.x`, `bot.z`),
    // beside the {x,y,z} `position` above.
    b.accessor("x", [](Value self_, std::span<const Value>) {
        HostAgent* h = unwrapAgent(self_);
        if (!h) return ev::undefined();
        return ev::fromDouble(h->agent.x());
    }, nullptr);

    b.accessor("z", [](Value self_, std::span<const Value>) {
        HostAgent* h = unwrapAgent(self_);
        if (!h) return ev::undefined();
        return ev::fromDouble(h->agent.z());
    }, nullptr);

    // Index of the waypoint the path follower is walking toward — what lets a
    // program that supplied the route via setPath consume its own mirror of
    // it as the agent passes each waypoint.
    b.accessor("currentWaypoint", [](Value self_, std::span<const Value>) {
        HostAgent* h = unwrapAgent(self_);
        if (!h) return ev::undefined();
        return ev::fromDouble(h->agent.currentWaypoint());
    }, nullptr);

    // The route as [{x, z}, ...] — a real array, as the QuickJS binding answers.
    b.accessor("path", [](Value self_, std::span<const Value>) -> Value {
        HostAgent* h = unwrapAgent(self_);
        if (!h) return ev::undefined();
        const std::vector<bromath::Vec2>& p = h->agent.path();
        return makePathArray(p);
    }, nullptr);

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

    b.accessor("aimYaw", [](Value self_, std::span<const Value>) {
        HostAgent* h = unwrapAgent(self_);
        if (!h) return ev::undefined();
        return ev::fromDouble(h->agent.aimYaw());
    }, nullptr);

    b.accessor("aimPitch", [](Value self_, std::span<const Value>) {
        HostAgent* h = unwrapAgent(self_);
        if (!h) return ev::undefined();
        return ev::fromDouble(h->agent.aimPitch());
    }, nullptr);

    b.accessor("elevation",
        [](Value self_, std::span<const Value>) -> Value {
            HostAgent* h = unwrapAgent(self_);
            if (!h) return ev::undefined();
            return ev::fromDouble(h->agent.elevation());
        },
        [](Value self_, std::span<const Value> a) -> Value {
            HostAgent* h = unwrapAgent(self_);
            if (!h) return ev::undefined();
            if (!a.empty()) {
                float y = static_cast<float>(numAt(a, 0));
                h->agent.setElevation(y);
                h->navY = y;
            }
            return ev::undefined();
        });

    b.def("getPosition", 0, [](Value self_, std::span<const Value>) -> Value {
        HostAgent* h = unwrapAgent(self_);
        if (!h) return ev::undefined();
        float y = h->navActive ? h->navY : h->agent.elevation();
        return makeVec3Value(h->agent.x(), y, h->agent.z());
    });

    b.def("getVelocity", 0, [](Value self_, std::span<const Value>) -> Value {
        HostAgent* h = unwrapAgent(self_);
        if (!h) return ev::undefined();
        auto v = h->agent.velocity();
        return makeVec3Value(v.x, 0.0f, v.y);
    });

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
        } else if (!a.empty() && ev::isObject(a[0])) {
            Value setGoalFn = ev::getProperty(self, "setGoal");
            if (ev::isFunction(setGoalFn)) ev::call(setGoalFn, self, a);
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

    b.def("setSpeed", 1, [](Value self_, std::span<const Value> a) -> Value {
        HostAgent* h = unwrapAgent(self_);
        if (!h) return ev::undefined();
        if (!a.empty()) h->agent.setSpeed(static_cast<float>(numAt(a, 0)));
        return ev::undefined();
    });

    // The docs' spelling of `stop` for the scripted path: the target goes,
    // and so does any navmesh route this layer was walking.
    b.def("clearTarget", 0, [](Value self_, std::span<const Value>) -> Value {
        HostAgent* h = unwrapAgent(self_);
        if (!h) return ev::undefined();
        h->navActive = false;
        h->navPath.clear();
        h->agent.clearTarget();
        return ev::undefined();
    });

    // setPath(waypoints): follow an externally planned route verbatim — the
    // waypoints are walked in order and never re-planned by NavGrid A*; the
    // embedder owns the route, the agent owns steering/avoidance/dynamics.
    // Each waypoint is {x, z} or [x, z]; the final one becomes the target for
    // hasTarget/atTarget; an empty array is clearTarget(). Any array-like
    // (a `length` and indices) is accepted: an array that crossed the
    // interpreter bridge is a proxy, not an Array, and is a route all the same.
    b.def("setPath", 1, [](Value self_, std::span<const Value> a) -> Value {
        HostAgent* h = unwrapAgent(self_);
        if (!h) return ev::undefined();
        Value list = argAt(a, 0);
        if (!ev::isObject(list) || ev::isFunction(list))
            return ev::throwTypeError("setPath(waypoints: [{x,z}|[x,z], ...])");
        ev::Persistent root(list);
        Value lenV = ev::getProperty(root.get(), "length");
        if (ev::isUndefined(lenV) || ev::isObject(lenV))
            return ev::throwTypeError("setPath(waypoints: [{x,z}|[x,z], ...])");
        const uint32_t n = static_cast<uint32_t>(ev::toDouble(lenV));
        std::vector<bromath::Vec2> path;
        path.reserve(n);
        for (uint32_t i = 0; i < n; ++i) {
            Value wp = ev::getElement(root.get(), i);
            if (!ev::isObject(wp))
                return ev::throwTypeError("setPath: waypoint " + std::to_string(i) +
                                          " must be {x,z} or [x,z]");
            path.push_back(parseVec2(wp));
        }
        h->navActive = false;
        h->navPath.clear();
        h->agent.setPath(std::move(path));
        return ev::undefined();
    });

    // Velocity is dynamics state (ORCA reciprocity reads neighbours'
    // velocities): a program restoring agents from its own save seeds it here.
    b.def("setVelocity", 2, [](Value self_, std::span<const Value> a) -> Value {
        HostAgent* h = unwrapAgent(self_);
        if (!h) return ev::undefined();
        h->agent.setVelocity(static_cast<float>(numAt(a, 0)), static_cast<float>(numAt(a, 1)));
        return ev::undefined();
    });

    b.def("setYaw", 1, [](Value self_, std::span<const Value> a) -> Value {
        HostAgent* h = unwrapAgent(self_);
        if (!h) return ev::undefined();
        h->agent.setYaw(static_cast<float>(numAt(a, 0)));
        return ev::undefined();
    });

    b.def("setMaxAccel", 1, [](Value self_, std::span<const Value> a) -> Value {
        HostAgent* h = unwrapAgent(self_);
        if (!h) return ev::undefined();
        h->agent.setMaxAccel(static_cast<float>(numAt(a, 0)));
        return ev::undefined();
    });

    b.def("setMaxTurnRate", 1, [](Value self_, std::span<const Value> a) -> Value {
        HostAgent* h = unwrapAgent(self_);
        if (!h) return ev::undefined();
        h->agent.setMaxTurnRate(static_cast<float>(numAt(a, 0)));
        return ev::undefined();
    });

    b.def("setAvoidance", 1, [](Value self_, std::span<const Value> a) -> Value {
        HostAgent* h = unwrapAgent(self_);
        if (!h) return ev::undefined();
        applyAgentAvoidance(argAt(a, 0), h->agent);
        return ev::undefined();
    });

    b.def("setRadius", 1, [](Value self_, std::span<const Value> a) -> Value {
        HostAgent* h = unwrapAgent(self_);
        if (!h) return ev::undefined();
        if (!a.empty()) h->agent.setRadius(static_cast<float>(numAt(a, 0)));
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

void applyAgentAvoidance(Value opts, brogameagent::Agent& agent) {
    brogameagent::AgentAvoidance av;  // library defaults
    if (ev::isBool(opts)) {
        av.enabled = ev::toBool(opts);
    } else if (ev::isObject(opts)) {
        ev::Persistent root(opts);
        av.enabled = getBoolProperty(root.get(), "enabled", av.enabled);
        av.radius = static_cast<float>(getDoubleProperty(root.get(), "radius", av.radius));
        av.maxSpeed = static_cast<float>(getDoubleProperty(root.get(), "maxSpeed", av.maxSpeed));
        av.neighborDist =
            static_cast<float>(getDoubleProperty(root.get(), "neighborDist", av.neighborDist));
        av.maxNeighbors =
            static_cast<int>(getDoubleProperty(root.get(), "maxNeighbors", av.maxNeighbors));
        av.timeHorizon =
            static_cast<float>(getDoubleProperty(root.get(), "timeHorizon", av.timeHorizon));
        av.timeHorizonObst = static_cast<float>(
            getDoubleProperty(root.get(), "timeHorizonObst", av.timeHorizonObst));
        av.height = static_cast<float>(getDoubleProperty(root.get(), "height", av.height));
        av.priority = static_cast<float>(getDoubleProperty(root.get(), "priority", av.priority));
        av.layers = static_cast<uint32_t>(getDoubleProperty(root.get(), "layers", av.layers));
        av.mask = static_cast<uint32_t>(getDoubleProperty(root.get(), "mask", av.mask));
    } else {
        return;
    }
    agent.setAvoidance(av);
}

Value makeAgentHandle(HostAgent* h) {
    ObjectBuilder b(g_agentClass.make(h, [](void* p) {
        delete static_cast<HostAgent*>(p);
    }));

    return b.get();
}

Value aiCreateAgent(Value, std::span<const Value> a) {
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

        // The unit identity the world's queries key on (docs/ai-game-api.js
        // createAgent: `id`, `teamId`), and the ORCA participation —
        // `avoidance: true | false | {...}` — when the world's pass is on.
        h->agent.unit().id = static_cast<int>(getDoubleProperty(root.get(), "id", 0.0));
        h->agent.unit().teamId = static_cast<int>(getDoubleProperty(root.get(), "teamId", 0.0));
        Value avoidV = ev::getProperty(root.get(), "avoidance");
        if (!ev::isUndefined(avoidV) && !ev::isNull(avoidV)) applyAgentAvoidance(avoidV, h->agent);

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

}  // namespace bro::bronze_host
