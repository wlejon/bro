#include "bronze_host/native_physics_internal.h"
#include "bronze_host/host_js_gpu.h"
#include "physics/force_field_jit.h"
#include "physics/trajectory_solver.h"

#include <atomic>
#include <cmath>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace bro::bronze_host {

namespace {

std::mutex& forceFieldRegistryMutex() {
    static auto* m = new std::mutex();
    return *m;
}

std::atomic<uint32_t> s_nextForceFieldId{1};

std::unordered_map<uint32_t, std::shared_ptr<physics::CompiledForceField>>& forceFieldRegistry() {
    static auto* r = new std::unordered_map<uint32_t, std::shared_ptr<physics::CompiledForceField>>();
    return *r;
}

Value hostNumberArray(const double* vals, size_t count) {
    return hostArrayOf(count, [vals](size_t i) -> Value {
        return ev::fromDouble(vals[i]);
    });
}

std::shared_ptr<physics::CompiledForceField> lookupForceField(Value self) {
    if (!ev::isObject(self)) return nullptr;
    Value idVal = ev::getProperty(self, "__broForceFieldId");
    if (!idVal.isNumber()) return nullptr;
    const uint32_t id = static_cast<uint32_t>(idVal.asNumber());
    std::lock_guard<std::mutex> lock(forceFieldRegistryMutex());
    auto& reg = forceFieldRegistry();
    auto it = reg.find(id);
    return (it != reg.end()) ? it->second : nullptr;
}

void parseCoordAndTime(
    std::span<const Value> args,
    float* outX,
    float* outY,
    float* outZ,
    float* outT) {
    *outX = 0.0f;
    *outY = 0.0f;
    *outZ = 0.0f;
    *outT = 0.0f;
    if (args.empty()) return;

    if (hostIsArray(args[0]) || ev::isTypedArray(args[0])) {
        ev::Persistent arr(args[0]);
        Value lenV = ev::getProperty(arr.get(), "length");
        const uint32_t len = lenV.isNumber() ? static_cast<uint32_t>(lenV.asNumber()) : 0;
        if (len > 0) { Value v = ev::getElement(arr.get(), 0); if (v.isNumber()) *outX = static_cast<float>(v.asNumber()); }
        if (len > 1) { Value v = ev::getElement(arr.get(), 1); if (v.isNumber()) *outY = static_cast<float>(v.asNumber()); }
        if (len > 2) { Value v = ev::getElement(arr.get(), 2); if (v.isNumber()) *outZ = static_cast<float>(v.asNumber()); }
        if (len > 3) { Value v = ev::getElement(arr.get(), 3); if (v.isNumber()) *outT = static_cast<float>(v.asNumber()); }
        if (args.size() >= 2 && args[1].isNumber()) *outT = static_cast<float>(args[1].asNumber());
        return;
    }

    if (args.size() >= 1 && args[0].isNumber()) *outX = static_cast<float>(args[0].asNumber());
    if (args.size() >= 2 && args[1].isNumber()) *outY = static_cast<float>(args[1].asNumber());
    if (args.size() >= 3 && args[2].isNumber()) *outZ = static_cast<float>(args[2].asNumber());
    if (args.size() >= 4 && args[3].isNumber()) *outT = static_cast<float>(args[3].asNumber());
}

Value solveTrajectoryJsImpl(
    std::span<const Value> args,
    std::shared_ptr<physics::CompiledForceField> boundField = nullptr);

Value buildForceFieldJsObject(std::shared_ptr<physics::CompiledForceField> field) {
    const uint32_t fieldId = s_nextForceFieldId.fetch_add(1);
    {
        std::lock_guard<std::mutex> lock(forceFieldRegistryMutex());
        forceFieldRegistry()[fieldId] = field;
    }

    const auto& ad = field->autodiffResult();
    ObjectBuilder out;
    out.set("__broForceFieldId", ev::fromDouble(static_cast<double>(fieldId)));
    out.set("ok", ev::fromBool(field->ok()));
    out.set("isPotential", ev::fromBool(field->isPotential()));
    out.set("scale", ev::fromDouble(static_cast<double>(field->options().scale)));
    out.set("damping", ev::fromDouble(static_cast<double>(field->options().damping)));
    out.set("maxForce", ev::fromDouble(static_cast<double>(field->options().maxForce)));
    out.set("provenSafe", ev::fromBool(ad.provenSafe || field->ok()));
    out.set("cpuGpuProvenEquivalent", ev::fromBool(ad.cpuGpuProvenEquivalent || field->ok()));
    out.set("errorMessage", ev::fromUtf8(field->errorMessage()));

    const auto& diags = field->diagnostics();
    out.set("diagnostics", hostArrayOf(diags.size(), [&](size_t i) -> Value {
        return ev::fromUtf8(diags[i]);
    }));

    out.def("evalPotential", 4, [](Value self, std::span<const Value> a) -> Value {
        auto f = lookupForceField(self);
        if (!f) return ev::fromDouble(0.0);
        float x = 0.0f, y = 0.0f, z = 0.0f, t = 0.0f;
        parseCoordAndTime(a, &x, &y, &z, &t);
        return ev::fromDouble(static_cast<double>(f->evalPotential(x, y, z, t)));
    });

    out.def("evalForce", 4, [](Value self, std::span<const Value> a) -> Value {
        auto f = lookupForceField(self);
        if (!f) {
            double zeros[3] = {0.0, 0.0, 0.0};
            return hostNumberArray(zeros, 3);
        }
        float x = 0.0f, y = 0.0f, z = 0.0f, t = 0.0f;
        parseCoordAndTime(a, &x, &y, &z, &t);
        const physics::ForceFieldSample s = f->evalAt(x, y, z, t);
        double vec[3] = {
            static_cast<double>(s.fx),
            static_cast<double>(s.fy),
            static_cast<double>(s.fz)
        };
        return hostNumberArray(vec, 3);
    });

    out.def("evalAt", 4, [](Value self, std::span<const Value> a) -> Value {
        auto f = lookupForceField(self);
        physics::ForceFieldSample s;
        if (f) {
            float x = 0.0f, y = 0.0f, z = 0.0f, t = 0.0f;
            parseCoordAndTime(a, &x, &y, &z, &t);
            s = f->evalAt(x, y, z, t);
        }
        double fvec[3] = {
            static_cast<double>(s.fx),
            static_cast<double>(s.fy),
            static_cast<double>(s.fz)
        };
        ObjectBuilder res;
        res.set("energy", ev::fromDouble(static_cast<double>(s.energy)));
        res.set("fx", ev::fromDouble(static_cast<double>(s.fx)));
        res.set("fy", ev::fromDouble(static_cast<double>(s.fy)));
        res.set("fz", ev::fromDouble(static_cast<double>(s.fz)));
        res.set("force", hostNumberArray(fvec, 3));
        return res.get();
    });

    out.def("evalForceBatch", 3, [](Value self, std::span<const Value> a) -> Value {
        auto f = lookupForceField(self);
        if (!f || a.empty() || !ev::isTypedArray(a[0])) return makeFloat32Array({});
        auto posInfo = ev::typedArrayInfo(a[0]);
        if (!posInfo.data || posInfo.bytesPerElement != sizeof(float) || posInfo.elementCount < 3) {
            return makeFloat32Array({});
        }
        const size_t count = posInfo.elementCount / 3;
        const float* posPtr = reinterpret_cast<const float*>(posInfo.data);

        if (a.size() >= 2 && ev::isTypedArray(a[1])) {
            auto outInfo = ev::typedArrayInfo(a[1]);
            const float t = (a.size() >= 3 && a[2].isNumber()) ? static_cast<float>(a[2].asNumber()) : 0.0f;
            if (outInfo.data && outInfo.bytesPerElement == sizeof(float) &&
                outInfo.elementCount >= count * 3) {
                f->evalForceBatch(posPtr, count, t, reinterpret_cast<float*>(outInfo.data));
                return a[1];
            }
        }

        const float t = (a.size() >= 2 && a[1].isNumber()) ? static_cast<float>(a[1].asNumber()) : 0.0f;
        std::vector<float> forces(count * 3, 0.0f);
        f->evalForceBatch(posPtr, count, t, forces.data());
        return makeFloat32Array(forces);
    });

    auto applyParticlesFn = [](Value self, std::span<const Value> a) -> Value {
        auto f = lookupForceField(self);
        if (!f || a.size() < 2 || !ev::isTypedArray(a[0]) || !ev::isTypedArray(a[1])) {
            return ev::fromDouble(0.0);
        }
        auto posInfo = ev::typedArrayInfo(a[0]);
        auto velInfo = ev::typedArrayInfo(a[1]);
        if (!posInfo.data || !velInfo.data ||
            posInfo.bytesPerElement != sizeof(float) ||
            velInfo.bytesPerElement != sizeof(float)) {
            return ev::fromDouble(0.0);
        }
        const size_t count = std::min(posInfo.elementCount, velInfo.elementCount) / 3;
        const float dt = (a.size() >= 3 && a[2].isNumber()) ? static_cast<float>(a[2].asNumber()) : (1.0f / 60.0f);
        float defaultMass = 1.0f;
        const float* massesPtr = nullptr;
        if (a.size() >= 4) {
            if (a[3].isNumber()) {
                defaultMass = static_cast<float>(a[3].asNumber());
            } else if (ev::isTypedArray(a[3])) {
                auto mInfo = ev::typedArrayInfo(a[3]);
                if (mInfo.data && mInfo.bytesPerElement == sizeof(float) && mInfo.elementCount >= count) {
                    massesPtr = reinterpret_cast<const float*>(mInfo.data);
                }
            }
        }
        const float t = (a.size() >= 5 && a[4].isNumber()) ? static_cast<float>(a[4].asNumber()) : 0.0f;
        const size_t updated = f->applyToParticles(
            reinterpret_cast<float*>(posInfo.data),
            reinterpret_cast<float*>(velInfo.data),
            count, dt, defaultMass, t, massesPtr);
        return ev::fromDouble(static_cast<double>(updated));
    };

    out.def("applyToParticles", 5, applyParticlesFn);
    out.def("applyToPositions", 5, applyParticlesFn);

    out.def("applyToPhysicsWorld", 2, [](Value self, std::span<const Value> a) -> Value {
        auto f = lookupForceField(self);
        if (!f) return ev::fromDouble(0.0);
        physics::PhysicsWorld* world = nullptr;
        float t = 0.0f;
        if (!a.empty()) {
            if (a[0].isNumber()) {
                t = static_cast<float>(a[0].asNumber());
            } else if (ev::isObject(a[0])) {
                world = unwrapPhysicsWorld(a[0]);
                if (a.size() >= 2 && a[1].isNumber()) t = static_cast<float>(a[1].asNumber());
            }
        }
        if (!world) {
            if (auto* hpw = getActiveWorld()) world = hpw->getWorld();
        }
        if (!world) return ev::fromDouble(0.0);
        return ev::fromDouble(static_cast<double>(f->applyToPhysicsWorld(*world, t)));
    });

    out.def("applyToBodies", 2, [](Value self, std::span<const Value> a) -> Value {
        auto f = lookupForceField(self);
        auto* hpw = getActiveWorld();
        if (!f || !hpw || !hpw->getWorld()) return ev::fromDouble(0.0);
        const float t = (a.size() >= 2 && a[1].isNumber()) ? static_cast<float>(a[1].asNumber()) : 0.0f;
        if (!a.empty() && hostIsArray(a[0])) {
            ev::Persistent arr(a[0]);
            Value lenV = ev::getProperty(arr.get(), "length");
            const uint32_t len = lenV.isNumber() ? static_cast<uint32_t>(lenV.asNumber()) : 0;
            std::vector<JPH::BodyID> ids;
            ids.reserve(len);
            for (uint32_t i = 0; i < len; ++i) {
                Value tv = ev::getElement(arr.get(), i);
                if (tv.isNumber()) {
                    JPH::BodyID bid = hpw->bodyIdForTag(static_cast<int32_t>(tv.asNumber()));
                    if (!bid.IsInvalid()) ids.push_back(bid);
                }
            }
            return ev::fromDouble(static_cast<double>(f->applyToPhysicsWorld(*hpw->getWorld(), t, &ids)));
        }
        return ev::fromDouble(static_cast<double>(f->applyToPhysicsWorld(*hpw->getWorld(), t)));
    });

    out.def("verifyConservation", 10, [](Value self, std::span<const Value> a) -> Value {
        auto f = lookupForceField(self);
        float x0 = 1.0f, y0 = 0.0f, z0 = 0.0f;
        float vx0 = 0.0f, vy0 = 1.0f, vz0 = 0.0f;
        float dt = 0.001f;
        uint32_t steps = 1000;
        float mass = 1.0f;
        float tolerance = 1e-3f;

        if (!a.empty() && ev::isObject(a[0]) && !hostIsArray(a[0])) {
            ev::Persistent opts(a[0]);
            x0 = static_cast<float>(getPropNumber(opts.get(), "x", x0));
            y0 = static_cast<float>(getPropNumber(opts.get(), "y", y0));
            z0 = static_cast<float>(getPropNumber(opts.get(), "z", z0));
            vx0 = static_cast<float>(getPropNumber(opts.get(), "vx", vx0));
            vy0 = static_cast<float>(getPropNumber(opts.get(), "vy", vy0));
            vz0 = static_cast<float>(getPropNumber(opts.get(), "vz", vz0));
            dt = static_cast<float>(getPropNumber(opts.get(), "dt", dt));
            steps = static_cast<uint32_t>(std::max(1.0, getPropNumber(opts.get(), "steps", steps)));
            mass = static_cast<float>(getPropNumber(opts.get(), "mass", mass));
            tolerance = static_cast<float>(getPropNumber(opts.get(), "tolerance", tolerance));
        } else {
            if (a.size() >= 1 && a[0].isNumber()) x0 = static_cast<float>(a[0].asNumber());
            if (a.size() >= 2 && a[1].isNumber()) y0 = static_cast<float>(a[1].asNumber());
            if (a.size() >= 3 && a[2].isNumber()) z0 = static_cast<float>(a[2].asNumber());
            if (a.size() >= 4 && a[3].isNumber()) vx0 = static_cast<float>(a[3].asNumber());
            if (a.size() >= 5 && a[4].isNumber()) vy0 = static_cast<float>(a[4].asNumber());
            if (a.size() >= 6 && a[5].isNumber()) vz0 = static_cast<float>(a[5].asNumber());
            if (a.size() >= 7 && a[6].isNumber()) dt = static_cast<float>(a[6].asNumber());
            if (a.size() >= 8 && a[7].isNumber()) steps = static_cast<uint32_t>(std::max(1.0, a[7].asNumber()));
            if (a.size() >= 9 && a[8].isNumber()) mass = static_cast<float>(a[8].asNumber());
            if (a.size() >= 10 && a[9].isNumber()) tolerance = static_cast<float>(a[9].asNumber());
        }

        physics::EnergyConservationReport rep;
        if (f) {
            rep = f->verifyConservation(x0, y0, z0, vx0, vy0, vz0, dt, steps, mass, tolerance);
        }
        ObjectBuilder res;
        res.set("initialEnergy", ev::fromDouble(rep.initialEnergy));
        res.set("finalEnergy", ev::fromDouble(rep.finalEnergy));
        res.set("minEnergy", ev::fromDouble(rep.minEnergy));
        res.set("maxEnergy", ev::fromDouble(rep.maxEnergy));
        res.set("maxRelativeDrift", ev::fromDouble(rep.maxRelativeDrift));
        res.set("steps", ev::fromDouble(static_cast<double>(rep.steps)));
        res.set("conserved", ev::fromBool(rep.conserved));
        return res.get();
    });

    out.def("solveTrajectory", 1, [](Value self, std::span<const Value> a) -> Value {
        auto f = lookupForceField(self);
        return solveTrajectoryJsImpl(a, f);
    });

    return out.get();
}

bool parseVec3Prop(Value obj, const char* key, float outVec[3]) {
    Value v = ev::getProperty(obj, key);
    if (ev::isUndefined(v) || ev::isNull(v)) return false;
    if (hostIsArray(v) || ev::isTypedArray(v)) {
        ev::Persistent arr(v);
        Value lenV = ev::getProperty(arr.get(), "length");
        const uint32_t len = lenV.isNumber() ? static_cast<uint32_t>(lenV.asNumber()) : 0;
        for (uint32_t i = 0; i < 3; ++i) {
            outVec[i] = 0.0f;
            if (i < len) {
                Value e = ev::getElement(arr.get(), i);
                if (e.isNumber()) outVec[i] = static_cast<float>(e.asNumber());
            }
        }
        return true;
    }
    if (ev::isObject(v)) {
        ev::Persistent o(v);
        outVec[0] = static_cast<float>(getPropNumber(o.get(), "x", 0.0));
        outVec[1] = static_cast<float>(getPropNumber(o.get(), "y", 0.0));
        outVec[2] = static_cast<float>(getPropNumber(o.get(), "z", 0.0));
        return true;
    }
    return false;
}

Value solveTrajectoryJsImpl(std::span<const Value> args, std::shared_ptr<physics::CompiledForceField> boundField) {
    if (args.empty() || !ev::isObject(args[0])) return ev::null();
    ev::Persistent cfgObj(args[0]);

    physics::TrajectorySolveConfig cfg;
    std::shared_ptr<physics::CompiledForceField> ownedField = boundField;

    auto tryResolveField = [&](const char* propName, bool potentialDefault) {
        if (ownedField) return;
        Value fv = ev::getProperty(cfgObj.get(), propName);
        if (ev::isUndefined(fv) || ev::isNull(fv)) return;
        if (ev::isObject(fv)) {
            if (auto f = lookupForceField(fv)) {
                ownedField = f;
                return;
            }
        }
        std::string rawSrc, infName;
        if (extractJsFunctionOrSource(fv, &rawSrc, &infName) && !rawSrc.empty()) {
            physics::ForceFieldCompileOptions fOpts;
            fOpts.isPotential = potentialDefault;
            fOpts.kernelName = infName;
            ownedField = std::make_shared<physics::CompiledForceField>(
                physics::CompiledForceField::compileForceField(rawSrc, fOpts));
        }
    };

    tryResolveField("field", true);
    tryResolveField("potentialField", true);
    tryResolveField("forceField", false);
    tryResolveField("potentialFn", true);
    tryResolveField("potential", true);
    tryResolveField("forceFn", false);

    if (ownedField && ownedField->ok()) {
        cfg.field = ownedField.get();
    }

    if (!parseVec3Prop(cfgObj.get(), "startPos", cfg.startPos)) {
        if (!parseVec3Prop(cfgObj.get(), "pos0", cfg.startPos)) {
            parseVec3Prop(cfgObj.get(), "start", cfg.startPos);
        }
    }
    if (!parseVec3Prop(cfgObj.get(), "targetPos", cfg.targetPos)) {
        if (!parseVec3Prop(cfgObj.get(), "target", cfg.targetPos)) {
            parseVec3Prop(cfgObj.get(), "goal", cfg.targetPos);
        }
    }
    if (parseVec3Prop(cfgObj.get(), "targetVelocity", cfg.targetVelocity) ||
        parseVec3Prop(cfgObj.get(), "targetVel", cfg.targetVelocity)) {
        cfg.hasTargetVelocity = true;
    }
    if (parseVec3Prop(cfgObj.get(), "initialVelocityGuess", cfg.initialVelocityGuess) ||
        parseVec3Prop(cfgObj.get(), "initialVelocity", cfg.initialVelocityGuess) ||
        parseVec3Prop(cfgObj.get(), "v0", cfg.initialVelocityGuess) ||
        parseVec3Prop(cfgObj.get(), "vel0", cfg.initialVelocityGuess)) {
        cfg.hasInitialVelocityGuess = true;
    }
    if (parseVec3Prop(cfgObj.get(), "gravity", cfg.gravity)) {
        cfg.hasGravity = true;
    }
    if (!parseVec3Prop(cfgObj.get(), "attractorPos", cfg.attractorPos)) {
        parseVec3Prop(cfgObj.get(), "harmonicCenter", cfg.attractorPos);
    }

    cfg.springStiffness = static_cast<float>(getPropNumber(cfgObj.get(), "springStiffness",
                          getPropNumber(cfgObj.get(), "harmonicK",
                          getPropNumber(cfgObj.get(), "stiffness", 0.0))));
    cfg.steps = static_cast<uint32_t>(std::max(1.0, getPropNumber(cfgObj.get(), "steps", 100.0)));
    const double flightTime = getPropNumber(cfgObj.get(), "flightTime",
                              getPropNumber(cfgObj.get(), "duration", 0.0));
    Value dtProp = ev::getProperty(cfgObj.get(), "dt");
    if (dtProp.isNumber() && dtProp.asNumber() > 0.0) {
        cfg.dt = static_cast<float>(dtProp.asNumber());
    } else if (flightTime > 0.0) {
        cfg.dt = static_cast<float>(flightTime / static_cast<double>(cfg.steps));
    } else {
        cfg.dt = 0.01f;
    }
    cfg.mass = static_cast<float>(std::max(1e-4, getPropNumber(cfgObj.get(), "mass", 1.0)));
    cfg.damping = static_cast<float>(std::max(0.0, getPropNumber(cfgObj.get(), "damping",
                  getPropNumber(cfgObj.get(), "drag", 0.0))));
    cfg.tolerance = static_cast<float>(std::max(1e-7, getPropNumber(cfgObj.get(), "tolerance",
                    getPropNumber(cfgObj.get(), "tol", 1e-4))));
    cfg.maxIterations = static_cast<uint32_t>(std::max(1.0, getPropNumber(cfgObj.get(), "maxIterations", 40.0)));

    Value obsVal = ev::getProperty(cfgObj.get(), "obstacles");
    if (hostIsArray(obsVal)) {
        ev::Persistent obsArr(obsVal);
        Value lenV = ev::getProperty(obsArr.get(), "length");
        const uint32_t len = lenV.isNumber() ? static_cast<uint32_t>(lenV.asNumber()) : 0;
        for (uint32_t i = 0; i < len; ++i) {
            Value item = ev::getElement(obsArr.get(), i);
            if (!ev::isObject(item)) continue;
            ev::Persistent o(item);
            physics::TrajectoryObstacle obs;
            float pos[3] = {0.0f, 0.0f, 0.0f};
            if (parseVec3Prop(o.get(), "pos", pos) || parseVec3Prop(o.get(), "position", pos) ||
                parseVec3Prop(o.get(), "center", pos)) {
                obs.x = pos[0]; obs.y = pos[1]; obs.z = pos[2];
            } else {
                obs.x = static_cast<float>(getPropNumber(o.get(), "x", 0.0));
                obs.y = static_cast<float>(getPropNumber(o.get(), "y", 0.0));
                obs.z = static_cast<float>(getPropNumber(o.get(), "z", 0.0));
            }
            obs.radius = static_cast<float>(std::max(1e-4, getPropNumber(o.get(), "radius", 0.5)));
            obs.repulsionWeight = static_cast<float>(getPropNumber(o.get(), "repulsionWeight",
                                  getPropNumber(o.get(), "weight", 25.0)));
            obs.margin = static_cast<float>(getPropNumber(o.get(), "margin", std::max(0.6 * obs.radius, 0.5)));
            cfg.obstacles.push_back(obs);
        }
    }

    const physics::TrajectorySolveResult solved = physics::solveTrajectory(cfg);

    double initVel[3] = {
        static_cast<double>(solved.initialVelocity[0]),
        static_cast<double>(solved.initialVelocity[1]),
        static_cast<double>(solved.initialVelocity[2])
    };
    double finalPos[3] = {
        static_cast<double>(solved.finalPosition[0]),
        static_cast<double>(solved.finalPosition[1]),
        static_cast<double>(solved.finalPosition[2])
    };
    double finalVel[3] = {
        static_cast<double>(solved.finalVelocity[0]),
        static_cast<double>(solved.finalVelocity[1]),
        static_cast<double>(solved.finalVelocity[2])
    };
    double jac[9];
    for (int i = 0; i < 9; ++i) jac[i] = static_cast<double>(solved.jacobian[i]);

    const size_t sampleCount = solved.trajectory.size() / 3;
    const double dtVal = static_cast<double>(cfg.dt);
    ev::Persistent trajArr(hostArrayOf(sampleCount, [&](size_t s) -> Value {
        double p[3] = {
            static_cast<double>(solved.trajectory[s * 3 + 0]),
            static_cast<double>(solved.trajectory[s * 3 + 1]),
            static_cast<double>(solved.trajectory[s * 3 + 2])
        };
        ObjectBuilder sampleObj;
        sampleObj.set("t", ev::fromDouble(static_cast<double>(s) * dtVal));
        sampleObj.set("pos", hostNumberArray(p, 3));
        sampleObj.set("x", ev::fromDouble(p[0]));
        sampleObj.set("y", ev::fromDouble(p[1]));
        sampleObj.set("z", ev::fromDouble(p[2]));
        return sampleObj.get();
    }));

    ObjectBuilder out;
    out.set("ok", ev::fromBool(true));
    out.set("converged", ev::fromBool(solved.converged));
    out.set("initialVelocity", hostNumberArray(initVel, 3));
    out.set("finalPosition", hostNumberArray(finalPos, 3));
    out.set("finalVelocity", hostNumberArray(finalVel, 3));
    out.set("positionError", ev::fromDouble(static_cast<double>(solved.positionError)));
    out.set("missDistance", ev::fromDouble(static_cast<double>(solved.positionError)));
    out.set("iterations", ev::fromDouble(static_cast<double>(solved.iterations)));
    out.set("minObstacleClearance", ev::fromDouble(static_cast<double>(solved.minObstacleClearance)));
    out.set("obstacleFree", ev::fromBool(cfg.obstacles.empty() || solved.minObstacleClearance > 0.0f));
    out.set("initialEnergy", ev::fromDouble(solved.initialEnergy));
    out.set("finalEnergy", ev::fromDouble(solved.finalEnergy));
    out.set("energyDrift", ev::fromDouble(solved.energyDrift));
    out.set("maxEnergyDrift", ev::fromDouble(solved.energyDrift));
    out.set("energyConserved", ev::fromBool(solved.energyConserved));
    out.set("energyInvariantVerified", ev::fromBool(solved.energyConserved));
    out.set("jacobian", hostNumberArray(jac, 9));
    out.set("trajectory", trajArr.get());
    out.set("trajectoryFlat", makeFloat32Array(solved.trajectory));
    return out.get();
}

Value compileForceFieldJsImpl(std::span<const Value> args, bool forcePotentialMode) {
    if (args.empty()) return ev::null();

    physics::ForceFieldCompileOptions opts;
    opts.isPotential = true;
    if (args.size() >= 2 && ev::isObject(args[1])) {
        ev::Persistent o(args[1]);
        Value potV = ev::getProperty(o.get(), "isPotential");
        if (ev::isBool(potV)) opts.isPotential = ev::toBool(potV);
        Value modeV = ev::getProperty(o.get(), "mode");
        if (modeV.isString()) {
            const std::string m = ev::toUtf8(modeV);
            if (m == "gradient" || m == "force") opts.isPotential = false;
            else if (m == "potential") opts.isPotential = true;
        }
        Value scaleV = ev::getProperty(o.get(), "scale");
        if (scaleV.isNumber()) opts.scale = static_cast<float>(scaleV.asNumber());
        Value dampV = ev::getProperty(o.get(), "damping");
        if (dampV.isNumber()) opts.damping = static_cast<float>(dampV.asNumber());
        Value maxFV = ev::getProperty(o.get(), "maxForce");
        if (maxFV.isNumber()) opts.maxForce = static_cast<float>(maxFV.asNumber());
        Value parV = ev::getProperty(o.get(), "parallel");
        if (ev::isBool(parV)) opts.enableParallel = ev::toBool(parV);
        Value nameV = ev::getProperty(o.get(), "kernelName");
        if (nameV.isString()) opts.kernelName = ev::toUtf8(nameV);
    }
    if (forcePotentialMode) opts.isPotential = true;

    // Support { fx, fy, fz } 3-component vector force object as first argument
    if (ev::isObject(args[0]) && !ev::isFunction(args[0])) {
        ev::Persistent vecObj(args[0]);
        std::string fxSrc, fySrc, fzSrc;
        if (extractJsFunctionOrSource(ev::getProperty(vecObj.get(), "fx"), &fxSrc, nullptr) &&
            extractJsFunctionOrSource(ev::getProperty(vecObj.get(), "fy"), &fySrc, nullptr) &&
            extractJsFunctionOrSource(ev::getProperty(vecObj.get(), "fz"), &fzSrc, nullptr)) {
            auto compiled = std::make_shared<physics::CompiledForceField>(
                physics::CompiledForceField::compileVectorForce(fxSrc, fySrc, fzSrc, opts));
            return buildForceFieldJsObject(std::move(compiled));
        }
    }

    std::string rawSource;
    std::string inferredName;
    if (!extractJsFunctionOrSource(args[0], &rawSource, &inferredName)) {
        return ev::null();
    }
    if (opts.kernelName.empty()) opts.kernelName = inferredName;

    auto compiled = std::make_shared<physics::CompiledForceField>(
        physics::CompiledForceField::compileForceField(rawSource, opts));
    return buildForceFieldJsObject(std::move(compiled));
}

void attachForceFieldFunctions(ObjectBuilder& b) {
    b.def("compileForceField", 2, [](Value, std::span<const Value> a) -> Value {
        return compileForceFieldJsImpl(a, false);
    });
    b.def("compilePotentialField", 2, [](Value, std::span<const Value> a) -> Value {
        return compileForceFieldJsImpl(a, true);
    });
    b.def("solveTrajectory", 1, [](Value, std::span<const Value> a) -> Value {
        return solveTrajectoryJsImpl(a, nullptr);
    });
}

}  // namespace

void installPhysicsForceFieldExtensions() {
    ev::GlobalValue gt = ev::globalValue("globalThis");
    if (!gt.found || !ev::isObject(gt.value)) return;
    ev::Persistent globalObj(gt.value);

    ev::Persistent physObj;
    Value physGlobal = ev::getProperty(globalObj.get(), "Physics");
    if (ev::isObject(physGlobal)) {
        physObj.set(physGlobal);
        ObjectBuilder pb(physObj.get());
        attachForceFieldFunctions(pb);
    }

    Value broVal = ev::getProperty(globalObj.get(), "bro");
    if (ev::isObject(broVal)) {
        ev::Persistent broRoot(broVal);
        Value broPhys = ev::getProperty(broRoot.get(), "physics");
        if (ev::isObject(broPhys)) {
            ObjectBuilder bpb(broPhys);
            attachForceFieldFunctions(bpb);
        } else if (ev::isObject(physObj.get())) {
            ev::setProperty(broRoot.get(), "physics", physObj.get());
        } else {
            ObjectBuilder bpb;
            attachForceFieldFunctions(bpb);
            ev::setProperty(broRoot.get(), "physics", bpb.get());
        }
    }
}

}  // namespace bro::bronze_host
