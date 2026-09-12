// Physics vehicle bindings for the bronze host layer.

#include "bronze_host/host_physics_internal.h"

namespace bro::bronze_host {

namespace {

void readVehicleWheel(Value oVal, physics::VehicleWheelOptions& w) {
    if (!ev::isObject(oVal)) return;
    ev::Persistent o(oVal);

    w.position = readVec3(ev::getProperty(o.get(), "position"));
    w.suspensionDirection = readVec3(ev::getProperty(o.get(), "suspensionDirection"), JPH::Vec3(0, -1, 0));
    w.radius = static_cast<float>(getPropNumber(o, "radius", w.radius));
    w.width = static_cast<float>(getPropNumber(o, "width", w.width));
    w.suspensionMinLength = static_cast<float>(getPropNumber(o, "suspensionMinLength", w.suspensionMinLength));
    w.suspensionMaxLength = static_cast<float>(getPropNumber(o, "suspensionMaxLength", w.suspensionMaxLength));
    w.suspensionFrequency = static_cast<float>(getPropNumber(o, "suspensionFrequency", w.suspensionFrequency));
    w.suspensionDamping = static_cast<float>(getPropNumber(o, "suspensionDamping", w.suspensionDamping));
    w.steerable = getPropBool(o, "steerable", w.steerable);
    w.maxSteerAngle = static_cast<float>(getPropNumber(o, "maxSteerAngle", w.maxSteerAngle));
    w.driven = getPropBool(o, "driven", w.driven);
    w.maxBrakeTorque = static_cast<float>(getPropNumber(o, "maxBrakeTorque", w.maxBrakeTorque));
    w.maxHandBrakeTorque = static_cast<float>(getPropNumber(o, "maxHandBrakeTorque", w.maxHandBrakeTorque));
    w.longitudinalFrictionScale = static_cast<float>(getPropNumber(o, "longitudinalFriction", w.longitudinalFrictionScale));
    w.lateralFrictionScale = static_cast<float>(getPropNumber(o, "lateralFriction", w.lateralFrictionScale));

    std::vector<float> longCurve;
    if (readFloatVector(ev::getProperty(o.get(), "longitudinalFrictionCurve"), longCurve) && longCurve.size() >= 2 && (longCurve.size() % 2 == 0)) {
        w.longitudinalFrictionCurve.clear();
        for (size_t i = 0; i + 1 < longCurve.size(); i += 2) {
            w.longitudinalFrictionCurve.emplace_back(longCurve[i], longCurve[i + 1]);
        }
    }
    std::vector<float> latCurve;
    if (readFloatVector(ev::getProperty(o.get(), "lateralFrictionCurve"), latCurve) && latCurve.size() >= 2 && (latCurve.size() % 2 == 0)) {
        w.lateralFrictionCurve.clear();
        for (size_t i = 0; i + 1 < latCurve.size(); i += 2) {
            w.lateralFrictionCurve.emplace_back(latCurve[i], latCurve[i + 1]);
        }
    }
}

void destroyVehicleImpl(HostPhysicsVehicle* v) {
    if (!v || !v->handle) return;
    HostPhysicsWorld* pw = v->world ? v->world : &g_defaultWorld;
    auto* world = pw->getWorld();
    if (world) {
        world->destroyVehicle(v->handle);
        if (v->ownsChassis && v->bodyTag >= 0) {
            JPH::BodyID bid = pw->bodyIdForTag(v->bodyTag);
            if (!bid.IsInvalid()) {
                world->destroyBody(bid, [pw](JPH::BodyID id) { pw->unregisterBodyId(id); });
                if (!world->bodyExists(bid)) pw->unregisterBody(v->bodyTag);
            }
        }
    }
    if (v->world) v->world->liveVehicles.erase(v);
    v->handle = 0;
    v->bodyTag = -1;
}

}  // namespace

void decorateVehicleProto(ObjectBuilder& vb) {
    vb.def("setInput", 1, [](Value self, std::span<const Value> a) -> Value {
        HostPhysicsVehicle* v = unwrapVehicle(self);
        if (!v || !v->handle) return ev::undefined();
        HostPhysicsWorld* pw = v->world ? v->world : &g_defaultWorld;
        auto* world = pw->getWorld();
        if (!world || a.empty() || !ev::isObject(a[0])) return ev::undefined();

        ev::Persistent o(a[0]);
        double fwd = getPropNumber(o, "forward", 0.0);
        double right = getPropNumber(o, "right", 0.0);
        double brake = getPropNumber(o, "brake", 0.0);
        double handBrake = getPropNumber(o, "handBrake", 0.0);

        bool explicitRatios = false;
        if (v->type == physics::VehicleOptions::ControllerTracked) {
            Value lrVal = ev::getProperty(o.get(), "leftRatio");
            Value rrVal = ev::getProperty(o.get(), "rightRatio");
            if ((!ev::isUndefined(lrVal) && !ev::isObject(lrVal)) ||
                (!ev::isUndefined(rrVal) && !ev::isObject(rrVal))) {
                explicitRatios = true;
                double lr = (!ev::isUndefined(lrVal) && !ev::isObject(lrVal)) ? ev::toDouble(lrVal) : 1.0;
                double rr = (!ev::isUndefined(rrVal) && !ev::isObject(rrVal)) ? ev::toDouble(rrVal) : 1.0;
                world->setVehicleTrackInput(v->handle, static_cast<float>(fwd), static_cast<float>(lr),
                                            static_cast<float>(rr), static_cast<float>(brake));
            }
        }
        if (!explicitRatios) {
            world->setVehicleInput(v->handle, static_cast<float>(fwd), static_cast<float>(right),
                                   static_cast<float>(brake), static_cast<float>(handBrake));
        }
        return ev::undefined();
    });

    vb.def("setLeanController", 1, [](Value self, std::span<const Value> a) -> Value {
        HostPhysicsVehicle* v = unwrapVehicle(self);
        if (!v || !v->handle) return ev::undefined();
        HostPhysicsWorld* pw = v->world ? v->world : &g_defaultWorld;
        auto* world = pw->getWorld();
        if (!world || a.empty()) return ev::undefined();
        world->setVehicleLeanController(v->handle, ev::toBool(a[0]));
        return ev::undefined();
    });

    vb.def("setGear", 2, [](Value self, std::span<const Value> a) -> Value {
        HostPhysicsVehicle* v = unwrapVehicle(self);
        if (!v || !v->handle) return ev::undefined();
        HostPhysicsWorld* pw = v->world ? v->world : &g_defaultWorld;
        auto* world = pw->getWorld();
        if (!world || a.empty()) return ev::undefined();
        int gear = i32At(a, 0);
        float clutch = a.size() >= 2 ? static_cast<float>(numAt(a, 1)) : 1.0f;
        world->setVehicleGear(v->handle, gear, clutch);
        return ev::undefined();
    });

    vb.def("wheelState", 1, [](Value self, std::span<const Value> a) -> Value {
        HostPhysicsVehicle* v = unwrapVehicle(self);
        if (!v || !v->handle) return ev::null();
        HostPhysicsWorld* pw = v->world ? v->world : &g_defaultWorld;
        auto* world = pw->getWorld();
        if (!world || a.empty()) return ev::null();

        int idx = i32At(a, 0);
        physics::VehicleWheelState ws;
        if (!world->getVehicleWheelState(v->handle, idx, ws)) return ev::null();

        ObjectBuilder obj;
        obj.set("suspensionLength", ev::fromDouble(ws.suspensionLength));
        obj.set("angularVelocity", ev::fromDouble(ws.angularVelocity));
        obj.set("steerAngle", ev::fromDouble(ws.steerAngle));
        obj.set("rotationAngle", ev::fromDouble(ws.rotationAngle));
        obj.set("contact", ev::fromBool(ws.contact));
        int32_t ctag = ws.contactBody.IsInvalid() ? -1 : pw->tagForBodyId(ws.contactBody);
        obj.set("contactBody", ev::fromDouble(ctag));
        obj.set("contactNormal", makeVec3Value(ws.contactNormal.GetX(), ws.contactNormal.GetY(), ws.contactNormal.GetZ()));
        obj.set("position", makeVec3Value(ws.position.GetX(), ws.position.GetY(), ws.position.GetZ()));
        obj.set("rotation", makeQuatValue(ws.rotation.GetX(), ws.rotation.GetY(), ws.rotation.GetZ(), ws.rotation.GetW()));
        return obj.get();
    });

    vb.def("getState", 0, [](Value self, std::span<const Value>) -> Value {
        HostPhysicsVehicle* v = unwrapVehicle(self);
        if (!v || !v->handle) return ev::null();
        HostPhysicsWorld* pw = v->world ? v->world : &g_defaultWorld;
        auto* world = pw->getWorld();
        if (!world) return ev::null();

        physics::VehicleState st;
        if (!world->getVehicleState(v->handle, st)) return ev::null();

        ObjectBuilder obj;
        obj.set("speed", ev::fromDouble(st.speed));
        obj.set("rpm", ev::fromDouble(st.rpm));
        obj.set("gear", ev::fromDouble(st.gear));
        return obj.get();
    });

    vb.def("destroy", 0, [](Value self, std::span<const Value>) -> Value {
        HostPhysicsVehicle* v = unwrapVehicle(self);
        if (!v) return ev::undefined();
        destroyVehicleImpl(v);
        return ev::undefined();
    });

    vb.accessor("wheelCount", [](Value self, std::span<const Value>) -> Value {
        HostPhysicsVehicle* v = unwrapVehicle(self);
        if (!v || !v->handle) return ev::fromDouble(0);
        HostPhysicsWorld* pw = v->world ? v->world : &g_defaultWorld;
        auto* world = pw->getWorld();
        return ev::fromDouble(world ? world->vehicleWheelCount(v->handle) : 0);
    }, nullptr);

    vb.accessor("chassisBody", [](Value self, std::span<const Value>) -> Value {
        HostPhysicsVehicle* v = unwrapVehicle(self);
        if (!v) return ev::fromDouble(-1);
        if (v->bodyTag >= 0) return ev::fromDouble(v->bodyTag);
        HostPhysicsWorld* pw = v->world ? v->world : &g_defaultWorld;
        auto* world = pw->getWorld();
        if (world && v->handle) {
            JPH::BodyID bid = world->vehicleBody(v->handle);
            if (!bid.IsInvalid()) return ev::fromDouble(pw->tagForBodyId(bid));
        }
        return ev::fromDouble(-1);
    }, nullptr);

    vb.accessor("type", [](Value self, std::span<const Value>) -> Value {
        HostPhysicsVehicle* v = unwrapVehicle(self);
        if (!v) return ev::fromUtf8("wheeled");
        const char* s = (v->type == physics::VehicleOptions::ControllerTracked ? "tracked"
                        : v->type == physics::VehicleOptions::ControllerMotorcycle ? "motorcycle" : "wheeled");
        return ev::fromUtf8(s);
    }, nullptr);

    vb.accessor("speed", [](Value self, std::span<const Value>) -> Value {
        HostPhysicsVehicle* v = unwrapVehicle(self);
        if (!v || !v->handle) return ev::fromDouble(0.0);
        HostPhysicsWorld* pw = v->world ? v->world : &g_defaultWorld;
        auto* world = pw->getWorld();
        physics::VehicleState st;
        return ev::fromDouble(world && world->getVehicleState(v->handle, st) ? st.speed : 0.0);
    }, nullptr);

    vb.accessor("rpm", [](Value self, std::span<const Value>) -> Value {
        HostPhysicsVehicle* v = unwrapVehicle(self);
        if (!v || !v->handle) return ev::fromDouble(0.0);
        HostPhysicsWorld* pw = v->world ? v->world : &g_defaultWorld;
        auto* world = pw->getWorld();
        physics::VehicleState st;
        return ev::fromDouble(world && world->getVehicleState(v->handle, st) ? st.rpm : 0.0);
    }, nullptr);

    vb.accessor("gear", [](Value self, std::span<const Value>) -> Value {
        HostPhysicsVehicle* v = unwrapVehicle(self);
        if (!v || !v->handle) return ev::fromDouble(0);
        HostPhysicsWorld* pw = v->world ? v->world : &g_defaultWorld;
        auto* world = pw->getWorld();
        physics::VehicleState st;
        return ev::fromDouble(world && world->getVehicleState(v->handle, st) ? st.gear : 0);
    }, nullptr);
}

Value physicsCreateVehicle(Value self, std::span<const Value> a) {
    HostPhysicsWorld* pw = unwrapWorld(self);
    auto* world = pw->getWorld();
    if (!world) return ev::throwError("World not available");
    if (a.empty() || !ev::isObject(a[0])) return ev::throwTypeError("createVehicle(opts) requires an object");

    ev::Persistent optsVal(a[0]);
    physics::VehicleOptions opts;

    std::string type = getPropString(optsVal, "type");
    if (type == "tracked") opts.controller = physics::VehicleOptions::ControllerTracked;
    else if (type == "motorcycle") opts.controller = physics::VehicleOptions::ControllerMotorcycle;
    else if (!type.empty() && type != "wheeled") return ev::throwTypeError("type must be 'wheeled' | 'tracked' | 'motorcycle'");
    opts.applyControllerDefaults();

    int32_t chassisTag = -1;
    bool createdChassis = false;

    Value bodyVal = ev::getProperty(optsVal.get(), "body");
    if (!ev::isUndefined(bodyVal) && !ev::isObject(bodyVal)) {
        chassisTag = static_cast<int32_t>(ev::toDouble(bodyVal));
    }

    if (chassisTag < 0) {
        Value chassisVal = ev::getProperty(optsVal.get(), "chassis");
        if (ev::isObject(chassisVal)) {
            physics::BodyOptions bodyOpts;
            std::string err;
            if (!readBodyOptions(chassisVal, bodyOpts, err)) {
                return ev::throwTypeError("chassis: " + err);
            }
            bodyOpts.isStatic = false;
            JPH::BodyID id = world->createBody(bodyOpts);
            if (id.IsInvalid()) return ev::throwError("Failed to create chassis body");
            chassisTag = pw->registerBody(id);
            createdChassis = true;
        } else {
            return ev::throwTypeError("createVehicle requires body (tag) or chassis (createBody opts)");
        }
    }

    opts.body = pw->bodyIdForTag(chassisTag);
    if (opts.body.IsInvalid()) {
        if (createdChassis) {
            world->destroyBody(opts.body);
            pw->unregisterBody(chassisTag);
        }
        return ev::throwTypeError("createVehicle: chassis body tag is invalid");
    }

    opts.up = readVec3(ev::getProperty(optsVal.get(), "up"), JPH::Vec3(0, 1, 0));
    opts.forward = readVec3(ev::getProperty(optsVal.get(), "forward"), JPH::Vec3(0, 0, 1));
    opts.maxPitchRollAngle = static_cast<float>(getPropNumber(optsVal, "maxPitchRollAngle", opts.maxPitchRollAngle));

    auto cleanupOnFail = [&]() {
        if (createdChassis) {
            world->destroyBody(pw->bodyIdForTag(chassisTag));
            pw->unregisterBody(chassisTag);
        }
    };

    Value wheelsVal = ev::getProperty(optsVal.get(), "wheels");
    if (ev::isObject(wheelsVal)) {
        ev::Persistent wp(wheelsVal);
        Value lenV = ev::getProperty(wp.get(), "length");
        uint32_t nw = (!ev::isUndefined(lenV) && !ev::isObject(lenV)) ? static_cast<uint32_t>(ev::toDouble(lenV)) : 0;
        for (uint32_t i = 0; i < nw; ++i) {
            Value wv = ev::getElement(wp.get(), i);
            physics::VehicleWheelOptions wheel;
            if (ev::isObject(wv)) readVehicleWheel(wv, wheel);
            opts.wheels.push_back(wheel);
        }
    }
    if (opts.wheels.empty()) {
        cleanupOnFail();
        return ev::throwTypeError("createVehicle requires a non-empty wheels array");
    }

    Value engVal = ev::getProperty(optsVal.get(), "engine");
    if (ev::isObject(engVal)) {
        ev::Persistent ep(engVal);
        opts.engine.maxTorque = static_cast<float>(getPropNumber(ep, "maxTorque", opts.engine.maxTorque));
        opts.engine.minRPM = static_cast<float>(getPropNumber(ep, "minRPM", opts.engine.minRPM));
        opts.engine.maxRPM = static_cast<float>(getPropNumber(ep, "maxRPM", opts.engine.maxRPM));
    }

    Value trVal = ev::getProperty(optsVal.get(), "transmission");
    if (ev::isObject(trVal)) {
        ev::Persistent tp(trVal);
        std::string mode = getPropString(tp, "mode");
        opts.transmission.manual = (mode == "manual");
        readFloatVector(ev::getProperty(tp.get(), "gearRatios"), opts.transmission.gearRatios);
        readFloatVector(ev::getProperty(tp.get(), "reverseGearRatios"), opts.transmission.reverseGearRatios);
        opts.transmission.switchTime = static_cast<float>(getPropNumber(tp, "switchTime", opts.transmission.switchTime));
        opts.transmission.clutchStrength = static_cast<float>(getPropNumber(tp, "clutchStrength", opts.transmission.clutchStrength));
        opts.transmission.shiftUpRPM = static_cast<float>(getPropNumber(tp, "shiftUpRPM", opts.transmission.shiftUpRPM));
        opts.transmission.shiftDownRPM = static_cast<float>(getPropNumber(tp, "shiftDownRPM", opts.transmission.shiftDownRPM));
    }

    if (opts.controller == physics::VehicleOptions::ControllerTracked) {
        Value tracksVal = ev::getProperty(optsVal.get(), "tracks");
        if (ev::isObject(tracksVal)) {
            ev::Persistent trp(tracksVal);
            Value lenV = ev::getProperty(trp.get(), "length");
            uint32_t n = (!ev::isUndefined(lenV) && !ev::isObject(lenV)) ? static_cast<uint32_t>(ev::toDouble(lenV)) : 0;
            for (uint32_t i = 0; i < n; ++i) {
                Value tv = ev::getElement(trp.get(), i);
                physics::VehicleTrackOptions trk;
                if (ev::isObject(tv)) {
                    ev::Persistent tvp(tv);
                    std::vector<float> idxs;
                    if (readFloatVector(ev::getProperty(tvp.get(), "wheels"), idxs)) {
                        for (float f : idxs) trk.wheels.push_back(static_cast<int>(f));
                    }
                    trk.drivenWheel = static_cast<int>(getPropNumber(tvp, "drivenWheel", trk.drivenWheel));
                    trk.inertia = static_cast<float>(getPropNumber(tvp, "inertia", trk.inertia));
                    trk.angularDamping = static_cast<float>(getPropNumber(tvp, "angularDamping", trk.angularDamping));
                    trk.maxBrakeTorque = static_cast<float>(getPropNumber(tvp, "maxBrakeTorque", trk.maxBrakeTorque));
                    trk.differentialRatio = static_cast<float>(getPropNumber(tvp, "differentialRatio", trk.differentialRatio));
                }
                opts.tracks.push_back(trk);
            }
        }
        if (opts.tracks.size() != 2) {
            cleanupOnFail();
            return ev::throwTypeError("type:'tracked' requires tracks: [leftTrack, rightTrack]");
        }
    } else if (opts.controller == physics::VehicleOptions::ControllerMotorcycle) {
        Value leanVal = ev::getProperty(optsVal.get(), "lean");
        if (ev::isObject(leanVal)) {
            ev::Persistent lp(leanVal);
            opts.lean.maxAngle = static_cast<float>(getPropNumber(lp, "maxAngle", opts.lean.maxAngle));
            opts.lean.springConstant = static_cast<float>(getPropNumber(lp, "springConstant", opts.lean.springConstant));
            opts.lean.springDamping = static_cast<float>(getPropNumber(lp, "springDamping", opts.lean.springDamping));
            opts.lean.springIntegrationCoefficient = static_cast<float>(getPropNumber(lp, "springIntegrationCoefficient", opts.lean.springIntegrationCoefficient));
            opts.lean.springIntegrationCoefficientDecay = static_cast<float>(getPropNumber(lp, "springIntegrationCoefficientDecay", opts.lean.springIntegrationCoefficientDecay));
            opts.lean.smoothingFactor = static_cast<float>(getPropNumber(lp, "smoothingFactor", opts.lean.smoothingFactor));
        }
    }

    Value diffsVal = ev::getProperty(optsVal.get(), "differentials");
    if (ev::isObject(diffsVal)) {
        ev::Persistent dp(diffsVal);
        Value lenV = ev::getProperty(dp.get(), "length");
        uint32_t n = (!ev::isUndefined(lenV) && !ev::isObject(lenV)) ? static_cast<uint32_t>(ev::toDouble(lenV)) : 0;
        for (uint32_t i = 0; i < n; ++i) {
            Value dv = ev::getElement(dp.get(), i);
            physics::VehicleDifferentialOptions d;
            if (ev::isObject(dv)) {
                ev::Persistent dvp(dv);
                d.leftWheel = static_cast<int>(getPropNumber(dvp, "leftWheel", d.leftWheel));
                d.rightWheel = static_cast<int>(getPropNumber(dvp, "rightWheel", d.rightWheel));
                d.ratio = static_cast<float>(getPropNumber(dvp, "ratio", d.ratio));
                d.leftRightSplit = static_cast<float>(getPropNumber(dvp, "leftRightSplit", d.leftRightSplit));
                d.limitedSlipRatio = static_cast<float>(getPropNumber(dvp, "limitedSlipRatio", d.limitedSlipRatio));
                d.engineTorqueRatio = static_cast<float>(getPropNumber(dvp, "engineTorqueRatio", d.engineTorqueRatio));
            }
            opts.differentials.push_back(d);
        }
    }
    opts.differentialLimitedSlipRatio = static_cast<float>(getPropNumber(optsVal, "differentialLimitedSlipRatio", opts.differentialLimitedSlipRatio));

    Value barsVal = ev::getProperty(optsVal.get(), "antiRollBars");
    if (ev::isObject(barsVal)) {
        ev::Persistent bp(barsVal);
        Value lenV = ev::getProperty(bp.get(), "length");
        uint32_t n = (!ev::isUndefined(lenV) && !ev::isObject(lenV)) ? static_cast<uint32_t>(ev::toDouble(lenV)) : 0;
        for (uint32_t i = 0; i < n; ++i) {
            Value bv = ev::getElement(bp.get(), i);
            physics::VehicleAntiRollBarOptions bar;
            if (ev::isObject(bv)) {
                ev::Persistent bvp(bv);
                bar.leftWheel = static_cast<int>(getPropNumber(bvp, "leftWheel", bar.leftWheel));
                bar.rightWheel = static_cast<int>(getPropNumber(bvp, "rightWheel", bar.rightWheel));
                bar.stiffness = static_cast<float>(getPropNumber(bvp, "stiffness", bar.stiffness));
            }
            opts.antiRollBars.push_back(bar);
        }
    }

    std::string tester = getPropString(optsVal, "collisionTester");
    if (tester == "ray") opts.tester = physics::VehicleOptions::TesterRay;
    else if (tester == "sphere") opts.tester = physics::VehicleOptions::TesterCastSphere;
    else if (tester == "cylinder" || tester.empty()) opts.tester = physics::VehicleOptions::TesterCastCylinder;
    else {
        cleanupOnFail();
        return ev::throwTypeError("collisionTester must be 'ray' | 'sphere' | 'cylinder'");
    }

    Value tlVal = ev::getProperty(optsVal.get(), "testerLayer");
    if (!ev::isUndefined(tlVal) && !ev::isNull(tlVal)) {
        if (!ev::isObject(tlVal)) {
            std::string s = ev::toUtf8(tlVal);
            int idx = -1;
            if (parseDecimalIndex(s, idx)) opts.testerLayer = idx;
            else opts.testerLayer = world->layerIndex(s);
        }
    }

    uint32_t handle = world->createVehicle(opts);
    if (!handle) {
        cleanupOnFail();
        return ev::throwError("Failed to create vehicle");
    }

    auto* jv = new HostPhysicsVehicle();
    jv->handle = handle;
    jv->bodyTag = chassisTag;
    jv->ownsChassis = createdChassis;
    jv->type = opts.controller;
    if (pw->ownsWorld) {
        jv->world = pw;
        pw->liveVehicles.insert(jv);
    }

    ObjectBuilder vb(g_vehicleClass.make(jv, [](void* p) {
        auto* v = static_cast<HostPhysicsVehicle*>(p);
        destroyVehicleImpl(v);
        delete v;
    }));
    return vb.get();
}

}  // namespace bro::bronze_host
