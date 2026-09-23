// native_physics_vehicle.cpp — Physics vehicle natives.

#include "bronze_host/native_physics_internal.h"
#include <sstream>

namespace bro::bronze_host {

namespace {

void readVehicleWheel(Value oIn, physics::VehicleWheelOptions& w) {
    if (!ev::isObject(oIn)) return;
    const Rooted oVal(oIn);

    w.position = readVec3(ev::getProperty(oVal, "position"));
    w.suspensionDirection = readVec3(ev::getProperty(oVal, "suspensionDirection"), JPH::Vec3(0, -1, 0));
    w.radius = static_cast<float>(getPropNumber(oVal, "radius", w.radius));
    w.width = static_cast<float>(getPropNumber(oVal, "width", w.width));
    w.suspensionMinLength = static_cast<float>(getPropNumber(oVal, "suspensionMinLength", w.suspensionMinLength));
    w.suspensionMaxLength = static_cast<float>(getPropNumber(oVal, "suspensionMaxLength", w.suspensionMaxLength));
    w.suspensionFrequency = static_cast<float>(getPropNumber(oVal, "suspensionFrequency", w.suspensionFrequency));
    w.suspensionDamping = static_cast<float>(getPropNumber(oVal, "suspensionDamping", w.suspensionDamping));
    w.steerable = getPropBool(oVal, "steerable", w.steerable);
    w.maxSteerAngle = static_cast<float>(getPropNumber(oVal, "maxSteerAngle", w.maxSteerAngle));
    w.driven = getPropBool(oVal, "driven", w.driven);
    w.maxBrakeTorque = static_cast<float>(getPropNumber(oVal, "maxBrakeTorque", w.maxBrakeTorque));
    w.maxHandBrakeTorque = static_cast<float>(getPropNumber(oVal, "maxHandBrakeTorque", w.maxHandBrakeTorque));
    w.longitudinalFrictionScale = static_cast<float>(getPropNumber(oVal, "longitudinalFriction", w.longitudinalFrictionScale));
    w.lateralFrictionScale = static_cast<float>(getPropNumber(oVal, "lateralFriction", w.lateralFrictionScale));

    std::vector<float> longCurve;
    if (readFloatVector(ev::getProperty(oVal, "longitudinalFrictionCurve"), longCurve) && longCurve.size() >= 2 && (longCurve.size() % 2 == 0)) {
        w.longitudinalFrictionCurve.clear();
        for (size_t i = 0; i + 1 < longCurve.size(); i += 2) {
            w.longitudinalFrictionCurve.emplace_back(longCurve[i], longCurve[i + 1]);
        }
    }
    std::vector<float> latCurve;
    if (readFloatVector(ev::getProperty(oVal, "lateralFrictionCurve"), latCurve) && latCurve.size() >= 2 && (latCurve.size() % 2 == 0)) {
        w.lateralFrictionCurve.clear();
        for (size_t i = 0; i + 1 < latCurve.size(); i += 2) {
            w.lateralFrictionCurve.emplace_back(latCurve[i], latCurve[i + 1]);
        }
    }
}

void destroyVehicleImpl(HostPhysicsVehicle* v) {
    if (!v || !v->handle) return;
    HostPhysicsWorld* pw = v->world ? v->world : getActiveWorld();
    auto* world = pw ? pw->getWorld() : nullptr;
    if (world) {
        world->destroyVehicle(v->handle);
        if (v->ownsChassis && v->bodyTag >= 0) {
            JPH::BodyID bid = pw->bodyIdForTag(v->bodyTag);
            if (!bid.IsInvalid()) {
                world->destroyBody(bid, [pw](JPH::BodyID id) { pw->unregisterBodyId(id); });
            }
            pw->unregisterBody(v->bodyTag);
        }
    }
    // Off the live set, so the world's destructor no longer clears this
    // pointer: drop it here (native_physics_softbody.cpp says why).
    if (v->world) v->world->liveVehicles.erase(v);
    v->world = nullptr;
    v->handle = 0;
    v->bodyTag = -1;
}

}  // namespace

}  // namespace bro::bronze_host

extern "C" {

using namespace bro;
using namespace bro::bronze_host;

void bro_physics_PhysicsVehicle_dtor(void* self) {
    auto* pv = static_cast<HostPhysicsVehicle*>(self);
    if (!pv) return;
    destroyVehicleImpl(pv);
    delete pv;
}

void* bro_physics_PhysicsVehicle_ctor(void) {
    return nullptr;
}

void* bro_physics_createVehicle(const char* config) {
    auto* pw = getActiveWorld();
    auto* world = pw ? pw->getWorld() : nullptr;
    if (!world || !config || !*config) return nullptr;
    auto res = ev::parseJson(config);
    if (res.thrown || !ev::isObject(res.value)) return nullptr;

    const Rooted optsVal(res.value);
    physics::VehicleOptions opts;

    std::string type = getPropString(optsVal, "type");
    if (type == "tracked") opts.controller = physics::VehicleOptions::ControllerTracked;
    else if (type == "motorcycle") opts.controller = physics::VehicleOptions::ControllerMotorcycle;
    else if (!type.empty() && type != "wheeled") return nullptr;
    opts.applyControllerDefaults();

    int32_t chassisTag = -1;
    bool createdChassis = false;

    Value bodyVal = ev::getProperty(optsVal, "body");
    if (!ev::isUndefined(bodyVal) && !ev::isObject(bodyVal)) {
        chassisTag = static_cast<int32_t>(ev::toDouble(bodyVal));
    }

    if (chassisTag < 0) {
        Value chassisVal = ev::getProperty(optsVal, "chassis");
        if (ev::isObject(chassisVal)) {
            physics::BodyOptions bodyOpts;
            std::string err;
            if (!readBodyOptions(chassisVal, bodyOpts, err, world)) return nullptr;
            bodyOpts.isStatic = false;
            JPH::BodyID id = world->createBody(bodyOpts);
            if (id.IsInvalid()) return nullptr;
            chassisTag = pw->registerBody(id);
            createdChassis = true;
        } else {
            return nullptr;
        }
    }

    opts.body = pw->bodyIdForTag(chassisTag);
    if (opts.body.IsInvalid()) {
        if (createdChassis) {
            world->destroyBody(opts.body);
            pw->unregisterBody(chassisTag);
        }
        return nullptr;
    }

    opts.up = readVec3(ev::getProperty(optsVal, "up"), JPH::Vec3(0, 1, 0));
    opts.forward = readVec3(ev::getProperty(optsVal, "forward"), JPH::Vec3(0, 0, 1));
    opts.maxPitchRollAngle = static_cast<float>(getPropNumber(optsVal, "maxPitchRollAngle", opts.maxPitchRollAngle));

    auto cleanupOnFail = [&]() {
        if (createdChassis) {
            world->destroyBody(pw->bodyIdForTag(chassisTag));
            pw->unregisterBody(chassisTag);
        }
    };

    const Rooted wheelsVal(ev::getProperty(optsVal, "wheels"));
    if (ev::isObject(wheelsVal)) {
        Value lenV = ev::getProperty(wheelsVal, "length");
        uint32_t nw = (!ev::isUndefined(lenV) && !ev::isObject(lenV)) ? static_cast<uint32_t>(ev::toDouble(lenV)) : 0;
        for (uint32_t i = 0; i < nw; ++i) {
            Value wv = ev::getElement(wheelsVal, i);
            physics::VehicleWheelOptions wheel;
            if (ev::isObject(wv)) readVehicleWheel(wv, wheel);
            opts.wheels.push_back(wheel);
        }
    }
    if (opts.wheels.empty()) {
        cleanupOnFail();
        return nullptr;
    }

    const Rooted engVal(ev::getProperty(optsVal, "engine"));
    if (ev::isObject(engVal)) {
        opts.engine.maxTorque = static_cast<float>(getPropNumber(engVal, "maxTorque", opts.engine.maxTorque));
        opts.engine.minRPM = static_cast<float>(getPropNumber(engVal, "minRPM", opts.engine.minRPM));
        opts.engine.maxRPM = static_cast<float>(getPropNumber(engVal, "maxRPM", opts.engine.maxRPM));
    }

    const Rooted trVal(ev::getProperty(optsVal, "transmission"));
    if (ev::isObject(trVal)) {
        std::string mode = getPropString(trVal, "mode");
        opts.transmission.manual = (mode == "manual");
        readFloatVector(ev::getProperty(trVal, "gearRatios"), opts.transmission.gearRatios);
        readFloatVector(ev::getProperty(trVal, "reverseGearRatios"), opts.transmission.reverseGearRatios);
        opts.transmission.switchTime = static_cast<float>(getPropNumber(trVal, "switchTime", opts.transmission.switchTime));
        opts.transmission.clutchStrength = static_cast<float>(getPropNumber(trVal, "clutchStrength", opts.transmission.clutchStrength));
        opts.transmission.shiftUpRPM = static_cast<float>(getPropNumber(trVal, "shiftUpRPM", opts.transmission.shiftUpRPM));
        opts.transmission.shiftDownRPM = static_cast<float>(getPropNumber(trVal, "shiftDownRPM", opts.transmission.shiftDownRPM));
    }

    if (opts.controller == physics::VehicleOptions::ControllerTracked) {
        const Rooted tracksVal(ev::getProperty(optsVal, "tracks"));
        if (ev::isObject(tracksVal)) {
            Value lenV = ev::getProperty(tracksVal, "length");
            uint32_t n = (!ev::isUndefined(lenV) && !ev::isObject(lenV)) ? static_cast<uint32_t>(ev::toDouble(lenV)) : 0;
            for (uint32_t i = 0; i < n; ++i) {
                const Rooted tv(ev::getElement(tracksVal, i));
                physics::VehicleTrackOptions trk;
                if (ev::isObject(tv)) {
                    std::vector<float> idxs;
                    if (readFloatVector(ev::getProperty(tv, "wheels"), idxs)) {
                        for (float f : idxs) trk.wheels.push_back(static_cast<int>(f));
                    }
                    trk.drivenWheel = static_cast<int>(getPropNumber(tv, "drivenWheel", trk.drivenWheel));
                    trk.inertia = static_cast<float>(getPropNumber(tv, "inertia", trk.inertia));
                    trk.angularDamping = static_cast<float>(getPropNumber(tv, "angularDamping", trk.angularDamping));
                    trk.maxBrakeTorque = static_cast<float>(getPropNumber(tv, "maxBrakeTorque", trk.maxBrakeTorque));
                    trk.differentialRatio = static_cast<float>(getPropNumber(tv, "differentialRatio", trk.differentialRatio));
                }
                opts.tracks.push_back(trk);
            }
        }
        if (opts.tracks.size() != 2) {
            cleanupOnFail();
            return nullptr;
        }
    } else if (opts.controller == physics::VehicleOptions::ControllerMotorcycle) {
        const Rooted leanVal(ev::getProperty(optsVal, "lean"));
        if (ev::isObject(leanVal)) {
            opts.lean.maxAngle = static_cast<float>(getPropNumber(leanVal, "maxAngle", opts.lean.maxAngle));
            opts.lean.springConstant = static_cast<float>(getPropNumber(leanVal, "springConstant", opts.lean.springConstant));
            const double dampingConstant = getPropNumber(leanVal, "dampingConstant", opts.lean.springDamping);
            opts.lean.springDamping = static_cast<float>(getPropNumber(leanVal, "springDamping", dampingConstant));
            opts.lean.springIntegrationCoefficient = static_cast<float>(
                getPropNumber(leanVal, "springIntegrationCoefficient", opts.lean.springIntegrationCoefficient));
            opts.lean.springIntegrationCoefficientDecay = static_cast<float>(
                getPropNumber(leanVal, "springIntegrationCoefficientDecay", opts.lean.springIntegrationCoefficientDecay));
            opts.lean.smoothingFactor = static_cast<float>(
                getPropNumber(leanVal, "smoothingFactor", opts.lean.smoothingFactor));
        }
    }

    // Explicit differentials (wheel indices into `wheels`); empty = derived
    // from the wheels' `driven` flags.
    const Rooted diffsVal(ev::getProperty(optsVal, "differentials"));
    if (ev::isObject(diffsVal)) {
        Value lenV = ev::getProperty(diffsVal, "length");
        uint32_t n = (!ev::isUndefined(lenV) && !ev::isObject(lenV)) ? static_cast<uint32_t>(ev::toDouble(lenV)) : 0;
        for (uint32_t i = 0; i < n; ++i) {
            const Rooted dv(ev::getElement(diffsVal, i));
            physics::VehicleDifferentialOptions d;
            if (ev::isObject(dv)) {
                d.leftWheel = static_cast<int>(getPropNumber(dv, "leftWheel", d.leftWheel));
                d.rightWheel = static_cast<int>(getPropNumber(dv, "rightWheel", d.rightWheel));
                d.ratio = static_cast<float>(getPropNumber(dv, "ratio", d.ratio));
                d.leftRightSplit = static_cast<float>(getPropNumber(dv, "leftRightSplit", d.leftRightSplit));
                d.limitedSlipRatio = static_cast<float>(getPropNumber(dv, "limitedSlipRatio", d.limitedSlipRatio));
                d.engineTorqueRatio = static_cast<float>(getPropNumber(dv, "engineTorqueRatio", d.engineTorqueRatio));
            }
            opts.differentials.push_back(d);
        }
    }
    opts.differentialLimitedSlipRatio = static_cast<float>(
        getPropNumber(optsVal, "differentialLimitedSlipRatio", opts.differentialLimitedSlipRatio));

    // Wheel-vs-ground test shape, and the object layer the wheels test as
    // (a layer name or index; default = the chassis's layer).
    const std::string tester = getPropString(optsVal, "collisionTester");
    if (tester == "ray") opts.tester = physics::VehicleOptions::TesterRay;
    else if (tester == "sphere") opts.tester = physics::VehicleOptions::TesterCastSphere;
    else if (tester == "cylinder" || tester.empty()) opts.tester = physics::VehicleOptions::TesterCastCylinder;
    else {
        cleanupOnFail();
        ev::throwTypeError("Physics.createVehicle: collisionTester must be 'ray' | 'sphere' | 'cylinder'");
        return nullptr;
    }
    Value tlVal = ev::getProperty(optsVal, "testerLayer");
    if (ev::isString(tlVal)) opts.testerLayer = world->layerIndex(ev::toUtf8(tlVal));
    else if (ev::isNumber(tlVal)) opts.testerLayer = static_cast<int>(ev::toDouble(tlVal));

    const Rooted arbVal(ev::getProperty(optsVal, "antiRollBars"));
    if (ev::isObject(arbVal)) {
        Value lenV = ev::getProperty(arbVal, "length");
        uint32_t n = (!ev::isUndefined(lenV) && !ev::isObject(lenV)) ? static_cast<uint32_t>(ev::toDouble(lenV)) : 0;
        for (uint32_t i = 0; i < n; ++i) {
            const Rooted el(ev::getElement(arbVal, i));
            if (ev::isObject(el)) {
                physics::VehicleAntiRollBarOptions bar;
                bar.leftWheel = static_cast<int>(getPropNumber(el, "leftWheel", 0));
                bar.rightWheel = static_cast<int>(getPropNumber(el, "rightWheel", 1));
                bar.stiffness = static_cast<float>(getPropNumber(el, "stiffness", bar.stiffness));
                opts.antiRollBars.push_back(bar);
            }
        }
    }

    uint32_t handle = world->createVehicle(opts);
    if (!handle) {
        cleanupOnFail();
        return nullptr;
    }

    auto* pv = new HostPhysicsVehicle();
    pv->world = pw;
    pv->handle = handle;
    pv->bodyTag = chassisTag;
    pv->ownsChassis = createdChassis;
    pv->type = opts.controller;
    pw->liveVehicles.insert(pv);
    return pv;
}

void bro_physics_PhysicsVehicle_setDriverInput(void* self, double forward, double steer, double brake, double handBrake) {
    auto* pv = static_cast<HostPhysicsVehicle*>(self);
    if (!pv || !pv->handle) return;
    HostPhysicsWorld* pw = pv->world ? pv->world : getActiveWorld();
    if (auto* w = pw ? pw->getWorld() : nullptr) {
        w->setVehicleInput(pv->handle, (float)forward, (float)steer, (float)brake, (float)handBrake);
    }
}

void bro_physics_PhysicsVehicle_setInput(void* self, const char* config) {
    auto* pv = static_cast<HostPhysicsVehicle*>(self);
    if (!pv || !pv->handle || !config || !*config) return;
    HostPhysicsWorld* pw = pv->world ? pv->world : getActiveWorld();
    auto* w = pw ? pw->getWorld() : nullptr;
    if (!w) return;

    auto res = ev::parseJson(config);
    if (res.thrown || !ev::isObject(res.value)) return;

    const Rooted o(res.value);
    double fwd = getPropNumber(o, "forward", 0.0);
    double right = getPropNumber(o, "right", 0.0);
    double brake = getPropNumber(o, "brake", 0.0);
    double handBrake = getPropNumber(o, "handBrake", 0.0);

    bool explicitRatios = false;
    if (pv->type == physics::VehicleOptions::ControllerTracked) {
        // Each ratio converts before the next read can move it.
        Value ratio = ev::getProperty(o, "leftRatio");
        const bool hasLeft = !ev::isUndefined(ratio) && !ev::isObject(ratio);
        const double lr = hasLeft ? ev::toDouble(ratio) : 1.0;
        ratio = ev::getProperty(o, "rightRatio");
        const bool hasRight = !ev::isUndefined(ratio) && !ev::isObject(ratio);
        const double rr = hasRight ? ev::toDouble(ratio) : 1.0;
        if (hasLeft || hasRight) {
            explicitRatios = true;
            w->setVehicleTrackInput(pv->handle, static_cast<float>(fwd), static_cast<float>(lr),
                                    static_cast<float>(rr), static_cast<float>(brake));
        }
    }
    if (!explicitRatios) {
        w->setVehicleInput(pv->handle, static_cast<float>(fwd), static_cast<float>(right),
                           static_cast<float>(brake), static_cast<float>(handBrake));
    }
}

void bro_physics_PhysicsVehicle_setLeanController(void* self, bool enabled) {
    auto* pv = static_cast<HostPhysicsVehicle*>(self);
    if (!pv || !pv->handle) return;
    HostPhysicsWorld* pw = pv->world ? pv->world : getActiveWorld();
    if (auto* w = pw ? pw->getWorld() : nullptr) {
        w->setVehicleLeanController(pv->handle, enabled);
    }
}

void bro_physics_PhysicsVehicle_setGear(void* self, int32_t gear, double clutch) {
    auto* pv = static_cast<HostPhysicsVehicle*>(self);
    if (!pv || !pv->handle) return;
    HostPhysicsWorld* pw = pv->world ? pv->world : getActiveWorld();
    if (auto* w = pw ? pw->getWorld() : nullptr) {
        w->setVehicleGear(pv->handle, gear, static_cast<float>(clutch));
    }
}

const char* bro_physics_PhysicsVehicle_wheelState(void* self, int32_t index) {
    auto* pv = static_cast<HostPhysicsVehicle*>(self);
    if (!pv || !pv->handle) return natives::strResult("null");
    HostPhysicsWorld* pw = pv->world ? pv->world : getActiveWorld();
    auto* w = pw ? pw->getWorld() : nullptr;
    if (!w) return natives::strResult("null");

    physics::VehicleWheelState ws;
    if (!w->getVehicleWheelState(pv->handle, index, ws)) return natives::strResult("null");

    int32_t ctag = ws.contactBody.IsInvalid() ? -1 : pw->tagForBodyId(ws.contactBody);
    std::ostringstream ss;
    ss << "{\"suspensionLength\":" << ws.suspensionLength
       << ",\"angularVelocity\":" << ws.angularVelocity
       << ",\"steerAngle\":" << ws.steerAngle
       << ",\"rotationAngle\":" << ws.rotationAngle
       << ",\"contact\":" << (ws.contact ? "true" : "false")
       << ",\"contactBody\":" << ctag
       << ",\"contactNormal\":{\"x\":" << ws.contactNormal.GetX() << ",\"y\":" << ws.contactNormal.GetY() << ",\"z\":" << ws.contactNormal.GetZ() << "}"
       << ",\"position\":{\"x\":" << ws.position.GetX() << ",\"y\":" << ws.position.GetY() << ",\"z\":" << ws.position.GetZ() << "}"
       << ",\"rotation\":{\"x\":" << ws.rotation.GetX() << ",\"y\":" << ws.rotation.GetY() << ",\"z\":" << ws.rotation.GetZ() << ",\"w\":" << ws.rotation.GetW() << "}}";
    return natives::strResult(ss.str());
}

const char* bro_physics_PhysicsVehicle_getState(void* self) {
    auto* pv = static_cast<HostPhysicsVehicle*>(self);
    if (!pv || !pv->handle) return natives::strResult("null");
    HostPhysicsWorld* pw = pv->world ? pv->world : getActiveWorld();
    auto* w = pw ? pw->getWorld() : nullptr;
    if (!w) return natives::strResult("null");

    physics::VehicleState st;
    if (!w->getVehicleState(pv->handle, st)) return natives::strResult("null");

    std::ostringstream ss;
    ss << "{\"speed\":" << st.speed << ",\"rpm\":" << st.rpm << ",\"gear\":" << st.gear << "}";
    return natives::strResult(ss.str());
}

const char* bro_physics_PhysicsVehicle_getTransform(void* self) {
    auto* pv = static_cast<HostPhysicsVehicle*>(self);
    if (!pv || !pv->handle) return natives::strResult("null");
    HostPhysicsWorld* pw = pv->world ? pv->world : getActiveWorld();
    auto* w = pw ? pw->getWorld() : nullptr;
    if (!w) return natives::strResult("null");

    JPH::BodyID bid = (pv->bodyTag >= 0) ? pw->bodyIdForTag(pv->bodyTag) : w->vehicleBody(pv->handle);
    if (bid.IsInvalid() || !w->bodyExists(bid)) return natives::strResult("null");

    JPH::RVec3 pos = w->getPosition(bid);
    JPH::Quat rot = w->getRotation(bid);
    uint64_t udata = w->getUserData(bid);

    std::ostringstream ss;
    ss << "{\"position\":{\"x\":" << pos.GetX() << ",\"y\":" << pos.GetY() << ",\"z\":" << pos.GetZ() << "}"
       << ",\"rotation\":{\"x\":" << rot.GetX() << ",\"y\":" << rot.GetY() << ",\"z\":" << rot.GetZ() << ",\"w\":" << rot.GetW() << "}"
       << ",\"userData\":" << udata << "}";
    return natives::strResult(ss.str());
}

void bro_physics_PhysicsVehicle_destroy(void* self) {
    auto* pv = static_cast<HostPhysicsVehicle*>(self);
    if (!pv) return;
    destroyVehicleImpl(pv);
}

int32_t bro_physics_PhysicsVehicle_wheelCount_get(void* self) {
    auto* pv = static_cast<HostPhysicsVehicle*>(self);
    if (!pv || !pv->handle) return 0;
    HostPhysicsWorld* pw = pv->world ? pv->world : getActiveWorld();
    auto* w = pw ? pw->getWorld() : nullptr;
    return w ? static_cast<int32_t>(w->vehicleWheelCount(pv->handle)) : 0;
}

int32_t bro_physics_PhysicsVehicle_chassisBody_get(void* self) {
    auto* pv = static_cast<HostPhysicsVehicle*>(self);
    if (!pv) return -1;
    if (pv->bodyTag >= 0) return pv->bodyTag;
    HostPhysicsWorld* pw = pv->world ? pv->world : getActiveWorld();
    auto* w = pw ? pw->getWorld() : nullptr;
    if (w && pv->handle) {
        JPH::BodyID bid = w->vehicleBody(pv->handle);
        if (!bid.IsInvalid()) return pw->tagForBodyId(bid);
    }
    return -1;
}

const char* bro_physics_PhysicsVehicle_type_get(void* self) {
    auto* pv = static_cast<HostPhysicsVehicle*>(self);
    if (!pv) return natives::strResult("wheeled");
    const char* s = (pv->type == physics::VehicleOptions::ControllerTracked ? "tracked"
                    : pv->type == physics::VehicleOptions::ControllerMotorcycle ? "motorcycle" : "wheeled");
    return natives::strResult(s);
}

double bro_physics_PhysicsVehicle_speed_get(void* self) {
    auto* pv = static_cast<HostPhysicsVehicle*>(self);
    if (!pv || !pv->handle) return 0.0;
    HostPhysicsWorld* pw = pv->world ? pv->world : getActiveWorld();
    auto* w = pw ? pw->getWorld() : nullptr;
    physics::VehicleState st;
    return (w && w->getVehicleState(pv->handle, st)) ? static_cast<double>(st.speed) : 0.0;
}

double bro_physics_PhysicsVehicle_rpm_get(void* self) {
    auto* pv = static_cast<HostPhysicsVehicle*>(self);
    if (!pv || !pv->handle) return 0.0;
    HostPhysicsWorld* pw = pv->world ? pv->world : getActiveWorld();
    auto* w = pw ? pw->getWorld() : nullptr;
    physics::VehicleState st;
    return (w && w->getVehicleState(pv->handle, st)) ? static_cast<double>(st.rpm) : 0.0;
}

int32_t bro_physics_PhysicsVehicle_gear_get(void* self) {
    auto* pv = static_cast<HostPhysicsVehicle*>(self);
    if (!pv || !pv->handle) return 0;
    HostPhysicsWorld* pw = pv->world ? pv->world : getActiveWorld();
    auto* w = pw ? pw->getWorld() : nullptr;
    physics::VehicleState st;
    return (w && w->getVehicleState(pv->handle, st)) ? st.gear : 0;
}

}  // extern "C"
