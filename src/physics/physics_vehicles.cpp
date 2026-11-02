#include "physics/physics_internal.h"
#include "physics/physics_decomposition.h"

#include <Jolt/RegisterTypes.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/Physics/PhysicsSettings.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/CylinderShape.h>
#include <Jolt/Physics/Collision/Shape/ConvexHullShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/Shape/StaticCompoundShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/Collision/Shape/HeightFieldShape.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/Body.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/ShapeCast.h>
#include <Jolt/Physics/Collision/CollideShape.h>
#include <Jolt/Physics/Collision/CollidePointResult.h>
#include <Jolt/Physics/Collision/NarrowPhaseQuery.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>
#include <Jolt/Physics/Constraints/DistanceConstraint.h>
#include <Jolt/Physics/Constraints/PointConstraint.h>
#include <Jolt/Physics/Constraints/HingeConstraint.h>
#include <Jolt/Physics/Constraints/FixedConstraint.h>
#include <Jolt/Physics/Constraints/SliderConstraint.h>
#include <Jolt/Physics/Constraints/SixDOFConstraint.h>
#include <Jolt/Physics/Constraints/ConeConstraint.h>
#include <Jolt/Physics/Constraints/SwingTwistConstraint.h>
#include <Jolt/Physics/Constraints/PulleyConstraint.h>
#include <Jolt/Physics/Constraints/GearConstraint.h>
#include <Jolt/Physics/Constraints/RackAndPinionConstraint.h>
#include <Jolt/Physics/Constraints/TwoBodyConstraint.h>
#include <Jolt/Physics/Character/CharacterVirtual.h>
#include <Jolt/Physics/Vehicle/VehicleConstraint.h>
#include <Jolt/Physics/Vehicle/WheeledVehicleController.h>
#include <Jolt/Physics/Vehicle/TrackedVehicleController.h>
#include <Jolt/Physics/Vehicle/MotorcycleController.h>
#include <Jolt/Physics/Vehicle/VehicleCollisionTester.h>
#include <Jolt/Physics/Ragdoll/Ragdoll.h>
#include <Jolt/Skeleton/Skeleton.h>
#include <Jolt/Physics/SoftBody/SoftBodyCreationSettings.h>
#include <Jolt/Physics/SoftBody/SoftBodyMotionProperties.h>

#include "util/log.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <unordered_set>

JPH_SUPPRESS_WARNINGS

using namespace JPH;

namespace bro::physics {

// --- Vehicles (wheeled / tracked / motorcycle) ---
//
// A Jolt VehicleConstraint is both a Constraint and a PhysicsStepListener:
// the constraint solves suspension/traction impulses, the step listener runs
// the engine/transmission/wheel-collision update at the start of every
// PhysicsSystem::Update. Both registrations are added and removed together
// (removeVehicleFromSystem) so the listener can never dangle. Because the
// stepping happens inside Update, vehicles need no per-step code on our side
// and the phase-ownership contract holds by construction.
//
// Three controller families share the constraint: WheeledVehicleController
// (cars), TrackedVehicleController (tanks — two tracks, skid steering),
// MotorcycleController (a WheeledVehicleController plus a lean spring).

uint32_t PhysicsWorld::createVehicle(const VehicleOptions& opts) {
    if (!initialized_ || opts.wheels.empty()) return 0;
    const bool isTracked = opts.controller == VehicleOptions::ControllerTracked;

    // Tracked structural validation up front. Jolt's TrackedVehicleController
    // has exactly two tracks and indexes them by each wheel's track index: a
    // wheel outside any track leaves WheelTV::mTrackIndex at -1 and reads out
    // of bounds, and an empty track leaves mDrivenWheel uninitialized garbage.
    // Reject these configs cleanly (mirror of the wheeled zero-driven-wheel
    // rejection).
    if (isTracked) {
        if (opts.tracks.size() != 2) {
            LOG_WARN("createVehicle: tracked vehicles need exactly 2 tracks "
                     "[left, right], got %zu", opts.tracks.size());
            return 0;
        }
        std::vector<int> owner(opts.wheels.size(), -1);
        for (int t = 0; t < 2; ++t) {
            const VehicleTrackOptions& trk = opts.tracks[t];
            if (trk.wheels.empty()) {
                LOG_WARN("createVehicle: track %d has no wheels — every track "
                         "needs at least one", t);
                return 0;
            }
            for (int wi : trk.wheels) {
                if (wi < 0 || wi >= (int)opts.wheels.size()) {
                    LOG_WARN("createVehicle: track %d wheel index %d out of range", t, wi);
                    return 0;
                }
                if (owner[wi] != -1) {
                    LOG_WARN("createVehicle: wheel %d appears in both tracks", wi);
                    return 0;
                }
                owner[wi] = t;
            }
            if (trk.drivenWheel >= 0 &&
                std::find(trk.wheels.begin(), trk.wheels.end(), trk.drivenWheel) ==
                    trk.wheels.end()) {
                LOG_WARN("createVehicle: track %d drivenWheel %d is not a wheel "
                         "of that track", t, trk.drivenWheel);
                return 0;
            }
            if (trk.inertia < 0.0f || trk.angularDamping < 0.0f ||
                trk.maxBrakeTorque < 0.0f || trk.differentialRatio <= 0.0f) {
                LOG_WARN("createVehicle: track %d has invalid inertia/"
                         "angularDamping/maxBrakeTorque/differentialRatio", t);
                return 0;
            }
        }
        for (size_t i = 0; i < owner.size(); ++i) {
            if (owner[i] < 0) {
                LOG_WARN("createVehicle: wheel %zu belongs to no track — a "
                         "tracked vehicle must assign every wheel to a track", i);
                return 0;
            }
        }
    }

    VehicleConstraintSettings settings;
    settings.mUp = opts.up.NormalizedOr(Vec3::sAxisY());
    settings.mForward = opts.forward.NormalizedOr(Vec3::sAxisZ());
    settings.mMaxPitchRollAngle =
        DegreesToRadians(std::clamp(opts.maxPitchRollAngle, 0.0f, 180.0f));

    // Motorcycle lean-spring auto-tuning needs the chassis's roll inertia
    // about the forward axis (see VehicleLeanOptions).
    float leanInertia = 0.0f;
    if (opts.controller == VehicleOptions::ControllerMotorcycle) {
        BodyLockRead lock(physicsSystem_.GetBodyLockInterface(), opts.body);
        if (!lock.Succeeded() || !lock.GetBody().IsDynamic()) return 0;
        Mat44 invI =
            lock.GetBody().GetMotionProperties()->GetLocalSpaceInverseInertia();
        float inv = invI.Multiply3x3(settings.mForward).Dot(settings.mForward);
        if (inv > 1.0e-12f) leanInertia = 1.0f / inv;
    }

    float minWidth = FLT_MAX;
    for (const VehicleWheelOptions& w : opts.wheels) {
        WheelSettings* ws;
        if (isTracked) {
            // Tracked wheels carry scalar terrain friction (Jolt models the
            // track's grip per road wheel), not slip curves: the per-wheel
            // friction scalars multiply Jolt's track defaults (4.0
            // longitudinal, 2.0 lateral) and curve overrides don't apply.
            // Steering/brake/driven fields are ignored — tracked vehicles
            // steer, drive, and brake per TRACK.
            auto* tv = new WheelSettingsTV();
            tv->mLongitudinalFriction *= w.longitudinalFrictionScale;
            tv->mLateralFriction *= w.lateralFrictionScale;
            ws = tv;
        } else {
            auto* wv = new WheelSettingsWV();
            wv->mMaxSteerAngle = w.steerable ? DegreesToRadians(w.maxSteerAngle) : 0.0f;
            wv->mMaxBrakeTorque = w.maxBrakeTorque;
            wv->mMaxHandBrakeTorque = w.maxHandBrakeTorque;
            // Per-wheel tire friction: an explicit curve replaces Jolt's
            // default LinearCurve; otherwise the scale multiplies the default
            // curve's friction (Y) values.
            auto applyFriction = [](LinearCurve& curve, float scale,
                                    const std::vector<Float2>& points) {
                if (!points.empty()) {
                    curve.Clear();
                    curve.Reserve((uint)points.size());
                    for (const Float2& p : points) curve.AddPoint(p.x, p.y);
                    curve.Sort();
                } else if (scale != 1.0f) {
                    for (auto& p : curve.mPoints) p.mY *= scale;
                }
            };
            applyFriction(wv->mLongitudinalFriction, w.longitudinalFrictionScale,
                          w.longitudinalFrictionCurve);
            applyFriction(wv->mLateralFriction, w.lateralFrictionScale,
                          w.lateralFrictionCurve);
            ws = wv;
        }
        ws->mPosition = w.position;
        ws->mSuspensionDirection = w.suspensionDirection.NormalizedOr(Vec3(0, -1, 0));
        ws->mSteeringAxis = -ws->mSuspensionDirection;
        ws->mRadius = w.radius;
        ws->mWidth = w.width;
        ws->mSuspensionMinLength = w.suspensionMinLength;
        ws->mSuspensionMaxLength = std::max(w.suspensionMaxLength, w.suspensionMinLength);
        ws->mSuspensionSpring.mMode = ESpringMode::FrequencyAndDamping;
        ws->mSuspensionSpring.mFrequency = w.suspensionFrequency;
        ws->mSuspensionSpring.mDamping = w.suspensionDamping;
        settings.mWheels.push_back(ws);
        minWidth = std::min(minWidth, w.width);
    }

    for (const VehicleAntiRollBarOptions& ar : opts.antiRollBars) {
        const int n = (int)opts.wheels.size();
        if (ar.leftWheel < 0 || ar.leftWheel >= n ||
            ar.rightWheel < 0 || ar.rightWheel >= n) return 0;
        VehicleAntiRollBar bar;
        bar.mLeftWheel = ar.leftWheel;
        bar.mRightWheel = ar.rightWheel;
        bar.mStiffness = ar.stiffness;
        settings.mAntiRollBars.push_back(bar);
    }

    // Engine + transmission settings have identical shapes across controller
    // families (empty gear-ratio lists keep each family's own Jolt defaults).
    auto fillEngineTransmission = [&opts](VehicleEngineSettings& eng,
                                          VehicleTransmissionSettings& tr) {
        eng.mMaxTorque = opts.engine.maxTorque;
        eng.mMinRPM = opts.engine.minRPM;
        eng.mMaxRPM = std::max(opts.engine.maxRPM, opts.engine.minRPM);
        tr.mMode = opts.transmission.manual ? ETransmissionMode::Manual
                                            : ETransmissionMode::Auto;
        if (!opts.transmission.gearRatios.empty())
            tr.mGearRatios.assign(opts.transmission.gearRatios.begin(),
                                  opts.transmission.gearRatios.end());
        if (!opts.transmission.reverseGearRatios.empty())
            tr.mReverseGearRatios.assign(opts.transmission.reverseGearRatios.begin(),
                                         opts.transmission.reverseGearRatios.end());
        tr.mSwitchTime = opts.transmission.switchTime;
        tr.mClutchStrength = opts.transmission.clutchStrength;
        tr.mShiftUpRPM = opts.transmission.shiftUpRPM;
        tr.mShiftDownRPM = opts.transmission.shiftDownRPM;
    };

    if (isTracked) {
        auto* controller = new TrackedVehicleControllerSettings();
        settings.mController = controller;  // takes ownership via Ref
        fillEngineTransmission(controller->mEngine, controller->mTransmission);
        // Tracks were validated above. Torque flows engine → gearbox → each
        // track's driven wheel (differentialRatio); no wheel differentials.
        for (int t = 0; t < 2; ++t) {
            const VehicleTrackOptions& trk = opts.tracks[t];
            VehicleTrackSettings& dst = controller->mTracks[t];
            for (int wi : trk.wheels) dst.mWheels.push_back((uint)wi);
            dst.mDrivenWheel = (uint)(trk.drivenWheel >= 0 ? trk.drivenWheel
                                                           : trk.wheels.back());
            dst.mInertia = trk.inertia;
            dst.mAngularDamping = trk.angularDamping;
            dst.mMaxBrakeTorque = trk.maxBrakeTorque;
            dst.mDifferentialRatio = trk.differentialRatio;
        }
    } else {
        WheeledVehicleControllerSettings* controller;
        if (opts.controller == VehicleOptions::ControllerMotorcycle) {
            // A motorcycle is a wheeled controller plus a lean spring that
            // torques the chassis toward the balance/turn lean angle.
            auto* mc = new MotorcycleControllerSettings();
            mc->mMaxLeanAngle =
                DegreesToRadians(std::clamp(opts.lean.maxAngle, 0.0f, 90.0f));
            // Negative = auto: scale spring strength to the chassis's roll
            // inertia (stable across chassis sizes; raw Jolt defaults are
            // tuned to the sample's offset-COM bike and blow up otherwise).
            mc->mLeanSpringConstant = opts.lean.springConstant >= 0.0f
                ? opts.lean.springConstant : 150.0f * leanInertia;
            mc->mLeanSpringDamping = opts.lean.springDamping >= 0.0f
                ? opts.lean.springDamping : 30.0f * leanInertia;
            mc->mLeanSpringIntegrationCoefficient =
                opts.lean.springIntegrationCoefficient;
            mc->mLeanSpringIntegrationCoefficientDecay =
                opts.lean.springIntegrationCoefficientDecay;
            mc->mLeanSmoothingFactor =
                std::clamp(opts.lean.smoothingFactor, 0.0f, 1.0f);
            controller = mc;
        } else {
            controller = new WheeledVehicleControllerSettings();
        }
        settings.mController = controller;  // takes ownership via Ref
        fillEngineTransmission(controller->mEngine, controller->mTransmission);
        controller->mDifferentialLimitedSlipRatio = opts.differentialLimitedSlipRatio;

        // Differentials: explicit list, or auto-derived by pairing driven
        // wheels in array order with equal torque split.
        std::vector<VehicleDifferentialOptions> diffs = opts.differentials;
        if (diffs.empty()) {
            std::vector<int> driven;
            for (int i = 0; i < (int)opts.wheels.size(); ++i)
                if (opts.wheels[i].driven) driven.push_back(i);
            for (size_t i = 0; i < driven.size(); i += 2) {
                VehicleDifferentialOptions d;
                d.leftWheel = driven[i];
                d.rightWheel = i + 1 < driven.size() ? driven[i + 1] : -1;
                diffs.push_back(d);
            }
            for (auto& d : diffs) d.engineTorqueRatio = 1.0f / (float)diffs.size();
        }
        // Jolt's WheeledVehicleController requires at least one driven wheel:
        // with zero differentials its clutch torque factors sum to 0, which
        // trips a Debug assert (WheeledVehicleController.cpp
        // sum_torque_factors == 1) and NaN-poisons the drivetrain math in
        // Release. Reject the config cleanly.
        if (diffs.empty()) {
            LOG_WARN("createVehicle: no driven wheels — mark at least one wheel "
                     "driven:true or pass explicit differentials");
            return 0;
        }
        for (const VehicleDifferentialOptions& d : diffs) {
            const int n = (int)opts.wheels.size();
            if (d.leftWheel >= n || d.rightWheel >= n) return 0;
            VehicleDifferentialSettings ds;
            ds.mLeftWheel = d.leftWheel;
            ds.mRightWheel = d.rightWheel;
            ds.mDifferentialRatio = d.ratio;
            ds.mLeftRightSplit = d.leftRightSplit;
            ds.mLimitedSlipRatio = d.limitedSlipRatio;
            ds.mEngineTorqueRatio = d.engineTorqueRatio;
            controller->mDifferentials.push_back(ds);
        }
    }

    // The constraint constructor needs the Body itself; the phase-idle
    // contract makes the write lock uncontended here.
    Ref<VehicleConstraint> constraint;
    int chassisLayer = 1;
    {
        BodyLockWrite lock(physicsSystem_.GetBodyLockInterface(), opts.body);
        if (!lock.Succeeded()) return 0;
        Body& body = lock.GetBody();
        if (!body.IsDynamic()) return 0;
        chassisLayer = (int)body.GetObjectLayer();
        constraint = new VehicleConstraint(body, settings);
    }

    // Wheel-vs-ground collision tester. The object layer decides what the
    // wheels can hit (via the normal layer-pair matrix); default is the
    // chassis layer so wheels see exactly what the body would collide with.
    int layer = opts.testerLayer;
    if (layer < 0 || layer >= numLayers_) layer = chassisLayer;
    Ref<VehicleCollisionTester> tester;
    switch (opts.tester) {
        case VehicleOptions::TesterRay:
            tester = new VehicleCollisionTesterRay(ObjectLayer(layer), settings.mUp);
            break;
        case VehicleOptions::TesterCastSphere:
            tester = new VehicleCollisionTesterCastSphere(
                ObjectLayer(layer), 0.5f * minWidth, settings.mUp);
            break;
        case VehicleOptions::TesterCastCylinder:
            tester = new VehicleCollisionTesterCastCylinder(ObjectLayer(layer));
            break;
    }
    constraint->SetVehicleCollisionTester(tester);

    physicsSystem_.AddConstraint(constraint.GetPtr());
    physicsSystem_.AddStepListener(constraint.GetPtr());
    physicsSystem_.GetBodyInterface().ActivateBody(opts.body);

    uint32_t handle = nextVehicleHandle_++;
    vehicles_[handle] = VehicleEntry{constraint, opts.controller, opts.body};
    return handle;
}

void PhysicsWorld::removeVehicleFromSystem(VehicleEntry& e) {
    if (!e.constraint) return;
    physicsSystem_.RemoveStepListener(e.constraint.GetPtr());
    physicsSystem_.RemoveConstraint(e.constraint.GetPtr());
}

void PhysicsWorld::destroyVehicle(uint32_t handle) {
    auto it = vehicles_.find(handle);
    if (it == vehicles_.end()) return;
    removeVehicleFromSystem(it->second);
    vehicles_.erase(it);
}

// Track drive ratios must never be exactly 0 — Jolt's
// TrackedVehicleController::SetDriverInput asserts on it (and 0 has no
// meaning: a stopped track is brake, not ratio).
static float nonZeroRatio(float v) {
    return v == 0.0f ? 1.0e-3f : v;
}

void PhysicsWorld::setVehicleInput(uint32_t handle, float forward, float right,
                                   float brake, float handBrake) {
    auto it = vehicles_.find(handle);
    if (it == vehicles_.end()) return;
    if (it->second.controller == VehicleOptions::ControllerTracked) {
        // Map wheeled-style steering onto differential track ratios the way
        // Jolt's tank sample does: the inside track slows linearly with steer
        // input, through zero, to a full pivot turn (ratio -1) at full lock —
        // right: 0.2 gives the sample's gentle-turn 0.6, right: 1 pivots.
        // Tracked vehicles have no separate handbrake; brake is the stronger
        // of the two pedals.
        float steer = std::clamp(right, -1.0f, 1.0f);
        float leftRatio = 1.0f, rightRatio = 1.0f;
        if (steer > 0.0f) rightRatio = 1.0f - 2.0f * steer;
        else if (steer < 0.0f) leftRatio = 1.0f + 2.0f * steer;
        auto* ctrl = static_cast<TrackedVehicleController*>(
            it->second.constraint->GetController());
        ctrl->SetDriverInput(std::clamp(forward, -1.0f, 1.0f),
                             nonZeroRatio(leftRatio), nonZeroRatio(rightRatio),
                             std::clamp(std::max(brake, handBrake), 0.0f, 1.0f));
    } else {
        auto* ctrl = static_cast<WheeledVehicleController*>(
            it->second.constraint->GetController());
        ctrl->SetDriverInput(std::clamp(forward, -1.0f, 1.0f),
                             std::clamp(right, -1.0f, 1.0f),
                             std::clamp(brake, 0.0f, 1.0f),
                             std::clamp(handBrake, 0.0f, 1.0f));
    }
    // Wake the chassis so input acts on a sleeping vehicle (constraint-motor
    // rule). Zero input doesn't wake — a parked car may sleep.
    if (forward != 0.0f || right != 0.0f || brake != 0.0f || handBrake != 0.0f)
        physicsSystem_.GetBodyInterface().ActivateBody(it->second.body);
}

void PhysicsWorld::setVehicleTrackInput(uint32_t handle, float forward,
                                        float leftRatio, float rightRatio,
                                        float brake) {
    auto it = vehicles_.find(handle);
    if (it == vehicles_.end() ||
        it->second.controller != VehicleOptions::ControllerTracked)
        return;
    auto* ctrl = static_cast<TrackedVehicleController*>(
        it->second.constraint->GetController());
    ctrl->SetDriverInput(std::clamp(forward, -1.0f, 1.0f),
                         nonZeroRatio(std::clamp(leftRatio, -1.0f, 1.0f)),
                         nonZeroRatio(std::clamp(rightRatio, -1.0f, 1.0f)),
                         std::clamp(brake, 0.0f, 1.0f));
    if (forward != 0.0f || leftRatio != 1.0f || rightRatio != 1.0f || brake != 0.0f)
        physicsSystem_.GetBodyInterface().ActivateBody(it->second.body);
}

bool PhysicsWorld::setVehicleLeanController(uint32_t handle, bool enabled) {
    auto it = vehicles_.find(handle);
    if (it == vehicles_.end() ||
        it->second.controller != VehicleOptions::ControllerMotorcycle)
        return false;
    auto* ctrl = static_cast<MotorcycleController*>(
        it->second.constraint->GetController());
    ctrl->EnableLeanController(enabled);
    // The toggle changes the balance forces immediately — wake the bike so a
    // parked one starts falling when the spring is switched off.
    physicsSystem_.GetBodyInterface().ActivateBody(it->second.body);
    return true;
}

void PhysicsWorld::setVehicleGear(uint32_t handle, int gear, float clutchFriction) {
    auto it = vehicles_.find(handle);
    if (it == vehicles_.end()) return;
    VehicleController* c = it->second.constraint->GetController();
    VehicleTransmission& tr =
        it->second.controller == VehicleOptions::ControllerTracked
            ? static_cast<TrackedVehicleController*>(c)->GetTransmission()
            : static_cast<WheeledVehicleController*>(c)->GetTransmission();
    tr.Set(gear, std::clamp(clutchFriction, 0.0f, 1.0f));
    physicsSystem_.GetBodyInterface().ActivateBody(it->second.body);
}

int PhysicsWorld::vehicleWheelCount(uint32_t handle) const {
    auto it = vehicles_.find(handle);
    if (it == vehicles_.end()) return -1;
    return (int)it->second.constraint->GetWheels().size();
}

bool PhysicsWorld::getVehicleWheelState(uint32_t handle, int wheel,
                                        VehicleWheelState& out) const {
    auto it = vehicles_.find(handle);
    if (it == vehicles_.end()) return false;
    const VehicleConstraint* c = it->second.constraint.GetPtr();
    if (wheel < 0 || wheel >= (int)c->GetWheels().size()) return false;
    const Wheel* w = c->GetWheel((uint)wheel);

    // Local transform mapping a Y-axis-aligned unit cylinder onto the wheel
    // (Jolt sample convention: wheelRight = Y, wheelUp = X), in body space.
    Mat44 local = c->GetWheelLocalTransform((uint)wheel, Vec3::sAxisY(), Vec3::sAxisX());
    out.position = local.GetTranslation();
    out.rotation = local.GetQuaternion().Normalized();
    out.suspensionLength = w->GetSuspensionLength();
    out.steerAngle = w->GetSteerAngle();
    out.rotationAngle = w->GetRotationAngle();
    out.angularVelocity = w->GetAngularVelocity();
    out.contact = w->HasContact();
    if (out.contact) {
        out.contactBody = w->GetContactBodyID();
        out.contactNormal = w->GetContactNormal();
    } else {
        out.contactBody = BodyID();
        out.contactNormal = Vec3(0, 1, 0);
    }
    return true;
}

bool PhysicsWorld::getVehicleState(uint32_t handle, VehicleState& out) const {
    auto it = vehicles_.find(handle);
    if (it == vehicles_.end()) return false;
    const VehicleConstraint* c = it->second.constraint.GetPtr();
    const Body* body = c->GetVehicleBody();
    Vec3 worldForward = body->GetRotation() * c->GetLocalForward();
    out.speed = body->GetLinearVelocity().Dot(worldForward);
    // Engine/transmission accessors live on the concrete controller family
    // (tracked and wheeled don't share them; motorcycle is-a wheeled).
    const VehicleController* vc = c->GetController();
    const VehicleEngine* eng;
    const VehicleTransmission* tr;
    if (it->second.controller == VehicleOptions::ControllerTracked) {
        const auto* t = static_cast<const TrackedVehicleController*>(vc);
        eng = &t->GetEngine();
        tr = &t->GetTransmission();
    } else {
        const auto* wv = static_cast<const WheeledVehicleController*>(vc);
        eng = &wv->GetEngine();
        tr = &wv->GetTransmission();
    }
    out.rpm = eng->GetCurrentRPM();
    out.gear = tr->GetCurrentGear();
    out.isSwitchingGear = tr->IsSwitchingGear();
    return true;
}

BodyID PhysicsWorld::vehicleBody(uint32_t handle) const {
    auto it = vehicles_.find(handle);
    return it == vehicles_.end() ? BodyID() : it->second.body;
}


} // namespace bro::physics
