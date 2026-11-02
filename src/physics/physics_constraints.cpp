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

// --- Constraints ---

// collideConnected = false (the default, matching the documented behavior and
// typical engine defaults — Jolt's own default is that constrained bodies DO
// collide): register the pair in pairFilter_ so the narrow phase skips it,
// and stamp both bodies into the pair group. A body already carrying a
// FOREIGN group filter (ragdoll parts use a per-ragdoll GroupFilterTable)
// can't participate — overwriting its group would break that scheme — so the
// pair is skipped with a warning and those two bodies keep colliding.
void PhysicsWorld::applyCollideConnected(const ConstraintOptions& opts,
                                         ConstraintEntry& entry) {
    if (opts.collideConnected) return;   // pair collides — nothing to set up
    if (opts.body1.IsInvalid() || opts.body2.IsInvalid()) return;  // world anchor
    if (opts.body1 == opts.body2) return;

    const BodyLockInterface& li = physicsSystem_.GetBodyLockInterface();
    for (BodyID id : {opts.body1, opts.body2}) {
        BodyLockRead lock(li, id);
        if (!lock.Succeeded()) return;
        const GroupFilter* f = lock.GetBody().GetCollisionGroup().GetGroupFilter();
        if (f && f != pairFilter_.GetPtr()) {
            LOG_WARN("createConstraint: collideConnected=false ignored — a body "
                     "already uses another collision-group filter (e.g. a ragdoll part)");
            return;
        }
    }
    for (BodyID id : {opts.body1, opts.body2}) {
        BodyLockWrite lock(li, id);
        if (!lock.Succeeded()) return;
        lock.GetBody().SetCollisionGroup(CollisionGroup(
            pairFilter_.GetPtr(), PairGroupFilter::kGroupID,
            id.GetIndexAndSequenceNumber()));
    }
    entry.pairKey = PairGroupFilter::pairKey(
        opts.body1.GetIndexAndSequenceNumber(),
        opts.body2.GetIndexAndSequenceNumber());
    entry.hasPair = true;
    pairFilter_->disablePair(entry.pairKey);
}

void PhysicsWorld::evictConstraintPair(const ConstraintEntry& entry) {
    if (entry.hasPair && pairFilter_) pairFilter_->enablePair(entry.pairKey);
}

uint32_t PhysicsWorld::createConstraint(const ConstraintOptions& optsIn) {
    auto& bi = physicsSystem_.GetBodyInterface();

    // Jolt measures a constraint's body2 relative to its body1: slider
    // position, hinge angle, limits, motor targets. The API's world-anchored
    // form names the moving body as body1 and the world as body2 (-1), which
    // handed to Jolt as-is measures the world relative to the body, so every
    // limit and motor ran mirrored (and gear / rack-and-pinion, which read
    // those hinge angles and slider positions, corrected the wrong way).
    // Put the world first, as Jolt's own samples do. Gear, rack-and-pinion
    // and pulley name two real bodies whose order is part of their meaning.
    ConstraintOptions swapped;
    const bool worldAnchored = optsIn.body2.IsInvalid() && !optsIn.body1.IsInvalid() &&
        optsIn.type != ConstraintOptions::Pulley &&
        optsIn.type != ConstraintOptions::Gear &&
        optsIn.type != ConstraintOptions::RackAndPinion;
    if (worldAnchored) {
        swapped = optsIn;
        std::swap(swapped.body1, swapped.body2);
        std::swap(swapped.point1, swapped.point2);
        // The wheel's hub is point1 whichever body holds it.
        if (swapped.type == ConstraintOptions::Wheel) swapped.point1 = optsIn.point1;
    }
    const ConstraintOptions& opts = worldAnchored ? swapped : optsIn;

    // Wheel = SixDOFConstraint configured Box2D-style: free translation along
    // the suspension axis (with optional soft limits via spring), free rotation
    // around the hinge axis (with optional motor); all other DOFs locked.
    // Slider+Hinge composition was tried and over-constrains (each constraint
    // fixes the DOF the other frees).
    if (opts.type == ConstraintOptions::Wheel) {
        Vec3 suspAxis  = opts.wheelSuspensionAxis.NormalizedOr(Vec3(0, 1, 0));
        Vec3 hingeAxis = opts.wheelHingeAxis.NormalizedOr(Vec3(0, 0, 1));
        // Build an orthonormal frame where local-Y = suspAxis, local-Z = hingeAxis.
        Vec3 axisX = suspAxis.Cross(hingeAxis).NormalizedOr(Vec3::sAxisX());
        Vec3 axisY = suspAxis;
        // If the user-supplied axes weren't perfectly perpendicular, re-derive Y.
        Vec3 axisZ = axisX.Cross(axisY).NormalizedOr(hingeAxis);
        axisY = axisZ.Cross(axisX).NormalizedOr(suspAxis);

        RVec3 hub = opts.point1;
        if (hub == RVec3::sZero())
            hub = bi.GetCenterOfMassPosition(opts.body2);

        auto* s = new SixDOFConstraintSettings();
        s->mSpace = EConstraintSpace::WorldSpace;
        s->mPosition1 = hub;
        s->mPosition2 = hub;
        s->mAxisX1 = axisX; s->mAxisY1 = axisY;
        s->mAxisX2 = axisX; s->mAxisY2 = axisY;

        using EAxis = SixDOFConstraintSettings::EAxis;
        // Lock everything by default, then free the two wheel DOFs.
        s->MakeFixedAxis(EAxis::TranslationX);
        s->MakeFixedAxis(EAxis::TranslationZ);
        s->MakeFixedAxis(EAxis::RotationX);
        s->MakeFixedAxis(EAxis::RotationY);
        // Suspension along local Y. Jolt's spring lives on the axis limits
        // (mLimitsSpringSettings), so a free axis has nothing for it to act
        // on. With a spring and no travel limits, pin the axis at the rest
        // position (0 / 0) and let the soft limit be the spring, the Box2D
        // wheel joint's behaviour; with explicit limits the spring softens
        // them; hertz 0 and no limits leaves the suspension free.
        if (opts.wheelHasTranslationLimits) {
            s->SetLimitedAxis(EAxis::TranslationY,
                              opts.wheelLowerTranslation,
                              opts.wheelUpperTranslation);
        } else if (opts.wheelHertz > 0.0f) {
            s->SetLimitedAxis(EAxis::TranslationY, 0.0f, 0.0f);
        } else {
            s->MakeFreeAxis(EAxis::TranslationY);
        }
        if (opts.wheelHertz > 0.0f) {
            s->mLimitsSpringSettings[EAxis::TranslationY].mMode = ESpringMode::FrequencyAndDamping;
            s->mLimitsSpringSettings[EAxis::TranslationY].mFrequency = opts.wheelHertz;
            s->mLimitsSpringSettings[EAxis::TranslationY].mDamping = opts.wheelDampingRatio;
        }
        // Wheel pin = rotation around local Z, always free.
        s->MakeFreeAxis(EAxis::RotationZ);
        if (opts.wheelEnableMotor) {
            s->mMotorSettings[EAxis::RotationZ].SetTorqueLimit(opts.wheelMaxMotorTorque);
        }
        Ref<TwoBodyConstraintSettings> sRef = s;

        TwoBodyConstraint* raw = bi.CreateConstraint(s, opts.body1, opts.body2);
        if (!raw) return 0;
        Ref<Constraint> c(raw);
        physicsSystem_.AddConstraint(c.GetPtr());

        if (opts.wheelEnableMotor) {
            auto* sd = static_cast<SixDOFConstraint*>(c.GetPtr());
            sd->SetMotorState(EAxis::RotationZ, EMotorState::Velocity);
            // Target angular velocity is in constraint space; only Z component matters.
            sd->SetTargetAngularVelocityCS(Vec3(0, 0, opts.wheelMotorSpeed));
        }

        uint32_t handle = nextConstraintHandle_++;
        ConstraintEntry entry{c, nullptr, opts.breakingImpulse};
        applyCollideConnected(opts, entry);
        constraints_[handle] = std::move(entry);
        return handle;
    }

    TwoBodyConstraintSettings* settings = nullptr;
    Ref<TwoBodyConstraintSettings> settingsRef;

    switch (opts.type) {
        case ConstraintOptions::Distance: {
            auto* s = new DistanceConstraintSettings();
            s->mPoint1 = opts.point1;
            s->mPoint2 = opts.point2;
            s->mSpace = EConstraintSpace::WorldSpace;
            if (opts.minDistance >= 0) s->mMinDistance = opts.minDistance;
            if (opts.maxDistance >= 0) s->mMaxDistance = opts.maxDistance;
            settings = s;
            break;
        }
        case ConstraintOptions::Point: {
            auto* s = new PointConstraintSettings();
            s->mPoint1 = opts.point1;
            s->mPoint2 = opts.point2;
            s->mSpace = EConstraintSpace::WorldSpace;
            settings = s;
            break;
        }
        case ConstraintOptions::Hinge: {
            auto* s = new HingeConstraintSettings();
            s->mPoint1 = opts.point1;
            s->mPoint2 = opts.point2;
            s->mHingeAxis1 = opts.axis;
            s->mHingeAxis2 = opts.axis;
            Vec3 normalAxis = std::abs(opts.axis.GetY()) < 0.9f
                ? opts.axis.Cross(Vec3(0,1,0)).Normalized()
                : opts.axis.Cross(Vec3(1,0,0)).Normalized();
            s->mNormalAxis1 = normalAxis;
            s->mNormalAxis2 = normalAxis;
            s->mSpace = EConstraintSpace::WorldSpace;
            if (opts.hasLimits) {
                s->mLimitsMin = opts.limitMin;
                s->mLimitsMax = opts.limitMax;
            }
            settings = s;
            break;
        }
        case ConstraintOptions::Fixed: {
            auto* s = new FixedConstraintSettings();
            s->mAutoDetectPoint = true;
            s->mSpace = EConstraintSpace::WorldSpace;
            settings = s;
            break;
        }
        case ConstraintOptions::Slider: {
            auto* s = new SliderConstraintSettings();
            s->mAutoDetectPoint = true;
            s->SetSliderAxis(opts.axis.NormalizedOr(Vec3(1,0,0)));
            s->mSpace = EConstraintSpace::WorldSpace;
            if (opts.hasLimits) {
                s->mLimitsMin = opts.limitMin;
                s->mLimitsMax = opts.limitMax;
            }
            settings = s;
            break;
        }
        case ConstraintOptions::Cone: {
            auto* s = new ConeConstraintSettings();
            s->mPoint1 = opts.point1;
            s->mPoint2 = opts.point2;
            s->mTwistAxis1 = opts.axis.NormalizedOr(Vec3::sAxisX());
            s->mTwistAxis2 = opts.axis.NormalizedOr(Vec3::sAxisX());
            s->mHalfConeAngle = opts.coneHalfAngle;
            s->mSpace = EConstraintSpace::WorldSpace;
            settings = s;
            break;
        }
        case ConstraintOptions::SwingTwist: {
            auto* s = new SwingTwistConstraintSettings();
            s->mPosition1 = opts.point1;
            s->mPosition2 = opts.point2;
            s->mTwistAxis1 = opts.axis.NormalizedOr(Vec3::sAxisX());
            s->mTwistAxis2 = opts.axis.NormalizedOr(Vec3::sAxisX());
            s->mPlaneAxis1 = opts.planeAxis.NormalizedOr(Vec3::sAxisY());
            s->mPlaneAxis2 = opts.planeAxis.NormalizedOr(Vec3::sAxisY());
            s->mNormalHalfConeAngle = opts.normalHalfConeAngle;
            s->mPlaneHalfConeAngle = opts.planeHalfConeAngle;
            s->mTwistMinAngle = opts.twistMinAngle;
            s->mTwistMaxAngle = opts.twistMaxAngle;
            s->mMaxFrictionTorque = opts.maxFrictionTorque;
            s->mSpace = EConstraintSpace::WorldSpace;
            settings = s;
            break;
        }
        case ConstraintOptions::Pulley: {
            auto* s = new PulleyConstraintSettings();
            s->mBodyPoint1 = opts.bodyPoint1;
            s->mFixedPoint1 = opts.fixedPoint1;
            s->mBodyPoint2 = opts.bodyPoint2;
            s->mFixedPoint2 = opts.fixedPoint2;
            s->mRatio = opts.ratio;
            s->mMinLength = opts.minLength;
            s->mMaxLength = opts.maxLength;
            s->mSpace = EConstraintSpace::WorldSpace;
            settings = s;
            break;
        }
        case ConstraintOptions::Gear: {
            auto* s = new GearConstraintSettings();
            s->mHingeAxis1 = opts.hingeAxis1.NormalizedOr(Vec3::sAxisX());
            s->mHingeAxis2 = opts.hingeAxis2.NormalizedOr(Vec3::sAxisX());
            s->mRatio = opts.ratio;
            s->mSpace = EConstraintSpace::WorldSpace;
            settings = s;
            break;
        }
        case ConstraintOptions::RackAndPinion: {
            auto* s = new RackAndPinionConstraintSettings();
            s->mHingeAxis = opts.hingeAxis1.NormalizedOr(Vec3::sAxisX());
            s->mSliderAxis = opts.hingeAxis2.NormalizedOr(Vec3::sAxisX());
            s->mRatio = opts.ratio;
            s->mSpace = EConstraintSpace::WorldSpace;
            settings = s;
            break;
        }
        case ConstraintOptions::SixDOF: {
            auto* s = new SixDOFConstraintSettings();
            s->mSpace = EConstraintSpace::WorldSpace;
            s->mPosition1 = opts.point1;
            s->mPosition2 = opts.point2;
            // Build an orthonormal constraint frame from the requested axes
            // (re-derive Y so imperfectly perpendicular input still works —
            // same treatment as the wheel composite above).
            Vec3 axisX = opts.sixDofAxisX.NormalizedOr(Vec3::sAxisX());
            Vec3 axisY = opts.sixDofAxisY.NormalizedOr(Vec3::sAxisY());
            Vec3 axisZ = axisX.Cross(axisY).NormalizedOr(Vec3::sAxisZ());
            axisY = axisZ.Cross(axisX).NormalizedOr(Vec3::sAxisY());
            s->mAxisX1 = axisX; s->mAxisY1 = axisY;
            s->mAxisX2 = axisX; s->mAxisY2 = axisY;
            s->mSwingType = opts.sixDofSwingPyramid ? ESwingType::Pyramid
                                                    : ESwingType::Cone;
            using EAxis = SixDOFConstraintSettings::EAxis;
            for (int i = 0; i < 6; i++) {
                const SixDofAxis& a = opts.sixDofAxes[i];
                auto axis = static_cast<EAxis>(i);
                switch (a.mode) {
                    case SixDofAxis::Locked:  s->MakeFixedAxis(axis); break;
                    case SixDofAxis::Free:    s->MakeFreeAxis(axis);  break;
                    case SixDofAxis::Limited: s->SetLimitedAxis(axis, a.min, a.max); break;
                }
                s->mMaxFriction[i] = a.maxFriction;
                // Soft (spring) limits exist only for the translation axes.
                if (i < EAxis::NumTranslation && a.springFrequency > 0.0f) {
                    s->mLimitsSpringSettings[i].mMode = ESpringMode::FrequencyAndDamping;
                    s->mLimitsSpringSettings[i].mFrequency = a.springFrequency;
                    s->mLimitsSpringSettings[i].mDamping = a.springDamping;
                }
            }
            settings = s;
            break;
        }
        case ConstraintOptions::Wheel:
            // handled above; unreachable here.
            return 0;
    }

    if (!settings) return 0;
    settingsRef = settings;  // take ownership via Ref

    TwoBodyConstraint* raw = bi.CreateConstraint(settings, opts.body1, opts.body2);
    if (!raw) return 0;
    Ref<Constraint> c(raw);

    // Gear / rack-and-pinion couple two *existing* constraints; wire them up from
    // the registry handles before adding to the system.
    if (opts.type == ConstraintOptions::Gear) {
        auto it1 = constraints_.find(opts.dependentConstraint1);
        auto it2 = constraints_.find(opts.dependentConstraint2);
        if (it1 == constraints_.end() || it2 == constraints_.end() ||
            !it1->second.ref || !it2->second.ref) {
            return 0;  // gear requires two valid existing hinge constraints
        }
        static_cast<GearConstraint*>(raw)->SetConstraints(
            it1->second.ref.GetPtr(), it2->second.ref.GetPtr());
    } else if (opts.type == ConstraintOptions::RackAndPinion) {
        auto it1 = constraints_.find(opts.dependentConstraint1);
        auto it2 = constraints_.find(opts.dependentConstraint2);
        if (it1 == constraints_.end() || it2 == constraints_.end() ||
            !it1->second.ref || !it2->second.ref) {
            return 0;  // requires a valid pinion hinge + rack slider
        }
        static_cast<RackAndPinionConstraint*>(raw)->SetConstraints(
            it1->second.ref.GetPtr(), it2->second.ref.GetPtr());
    }

    physicsSystem_.AddConstraint(c.GetPtr());

    uint32_t handle = nextConstraintHandle_++;
    ConstraintEntry entry{c, nullptr, opts.breakingImpulse};
    applyCollideConnected(opts, entry);
    constraints_[handle] = std::move(entry);

    // Create-time motors (hinge/slider: single entry; sixdof: one per axis).
    for (const MotorOptions& m : opts.motors)
        setConstraintMotor(handle, m);

    return handle;
}

void PhysicsWorld::destroyConstraint(uint32_t handle) {
    auto it = constraints_.find(handle);
    if (it == constraints_.end()) return;
    if (it->second.ref) physicsSystem_.RemoveConstraint(it->second.ref.GetPtr());
    if (it->second.ref2) physicsSystem_.RemoveConstraint(it->second.ref2.GetPtr());
    evictConstraintPair(it->second);
    constraints_.erase(it);
}

void PhysicsWorld::setConstraintEnabled(uint32_t handle, bool enabled) {
    auto it = constraints_.find(handle);
    if (it == constraints_.end() || !it->second.ref) return;
    it->second.ref->SetEnabled(enabled);
    if (it->second.ref2) it->second.ref2->SetEnabled(enabled);
}

bool PhysicsWorld::isConstraintEnabled(uint32_t handle) const {
    auto it = constraints_.find(handle);
    if (it == constraints_.end() || !it->second.ref) return false;
    return it->second.ref->GetEnabled();
}

JPH::Constraint* PhysicsWorld::getConstraint(uint32_t handle) const {
    auto it = constraints_.find(handle);
    if (it == constraints_.end()) return nullptr;
    return it->second.ref.GetPtr();
}

void PhysicsWorld::setWheelMotor(uint32_t handle, bool enabled, float speed, float maxTorque) {
    auto it = constraints_.find(handle);
    if (it == constraints_.end() || !it->second.ref) return;
    if (it->second.ref->GetSubType() != EConstraintSubType::SixDOF) return;
    auto* sd = static_cast<SixDOFConstraint*>(it->second.ref.GetPtr());
    using EAxis = SixDOFConstraintSettings::EAxis;
    sd->SetMotorState(EAxis::RotationZ, enabled ? EMotorState::Velocity : EMotorState::Off);
    sd->SetTargetAngularVelocityCS(Vec3(0, 0, speed));
    sd->GetMotorSettings(EAxis::RotationZ).SetTorqueLimit(maxTorque);
}

// Apply the shared MotorSettings fields (spring for position motors, symmetric
// force/torque limits). Negative fields mean "leave unchanged".
static void applyMotorSettings(MotorSettings& ms, const MotorOptions& m) {
    if (m.frequency >= 0.0f) {
        ms.mSpringSettings.mMode = ESpringMode::FrequencyAndDamping;
        ms.mSpringSettings.mFrequency = m.frequency;
    }
    if (m.damping >= 0.0f) {
        ms.mSpringSettings.mMode = ESpringMode::FrequencyAndDamping;
        ms.mSpringSettings.mDamping = m.damping;
    }
    if (m.maxForce >= 0.0f)  ms.SetForceLimit(m.maxForce);
    if (m.maxTorque >= 0.0f) ms.SetTorqueLimit(m.maxTorque);
}

bool PhysicsWorld::setConstraintMotor(uint32_t handle, const MotorOptions& motor) {
    auto it = constraints_.find(handle);
    if (it == constraints_.end() || !it->second.ref) return false;
    Constraint* c = it->second.ref.GetPtr();

    EMotorState state = EMotorState::Off;
    if (motor.state == MotorOptions::Velocity) state = EMotorState::Velocity;
    else if (motor.state == MotorOptions::Position) state = EMotorState::Position;

    bool ok = false;
    switch (c->GetSubType()) {
        case EConstraintSubType::Hinge: {
            auto* h = static_cast<HingeConstraint*>(c);
            applyMotorSettings(h->GetMotorSettings(), motor);
            if (state == EMotorState::Velocity)      h->SetTargetAngularVelocity(motor.target);
            else if (state == EMotorState::Position) h->SetTargetAngle(motor.target);
            h->SetMotorState(state);
            ok = true;
            break;
        }
        case EConstraintSubType::Slider: {
            auto* s = static_cast<SliderConstraint*>(c);
            applyMotorSettings(s->GetMotorSettings(), motor);
            if (state == EMotorState::Velocity)      s->SetTargetVelocity(motor.target);
            else if (state == EMotorState::Position) s->SetTargetPosition(motor.target);
            s->SetMotorState(state);
            ok = true;
            break;
        }
        case EConstraintSubType::SixDOF: {
            // Covers both explicit sixdof constraints and wheel composites
            // (whose driven axis is RotationZ = 5).
            if (motor.axis < 0 || motor.axis > 5) return false;
            auto* sd = static_cast<SixDOFConstraint*>(c);
            using EAxis = SixDOFConstraintSettings::EAxis;
            auto axis = static_cast<EAxis>(motor.axis);
            applyMotorSettings(sd->GetMotorSettings(axis), motor);
            if (motor.axis < 3) {
                if (state == EMotorState::Velocity) {
                    Vec3 v = sd->GetTargetVelocityCS();
                    v.SetComponent(motor.axis, motor.target);
                    sd->SetTargetVelocityCS(v);
                } else if (state == EMotorState::Position) {
                    Vec3 p = sd->GetTargetPositionCS();
                    p.SetComponent(motor.axis, motor.target);
                    sd->SetTargetPositionCS(p);
                }
            } else {
                if (state == EMotorState::Velocity) {
                    Vec3 w = sd->GetTargetAngularVelocityCS();
                    w.SetComponent(motor.axis - 3, motor.target);
                    sd->SetTargetAngularVelocityCS(w);
                } else if (state == EMotorState::Position) {
                    float* t = it->second.sixDofRotTarget;
                    t[motor.axis - 3] = motor.target;
                    sd->SetTargetOrientationCS(Quat::sEulerAngles(Vec3(t[0], t[1], t[2])));
                }
            }
            sd->SetMotorState(axis, state);
            ok = true;
            break;
        }
        default:
            return false;  // no motor on this constraint type
    }

    // Wake both bodies so a motor change acts on sleeping islands.
    if (ok) {
        auto* tb = static_cast<TwoBodyConstraint*>(c);
        auto& bi = physicsSystem_.GetBodyInterface();
        Body* b1 = tb->GetBody1();
        Body* b2 = tb->GetBody2();
        if (b1 && !b1->IsStatic()) bi.ActivateBody(b1->GetID());
        if (b2 && !b2->IsStatic()) bi.ActivateBody(b2->GetID());
    }
    return ok;
}

void PhysicsWorld::setConstraintBreakingImpulse(uint32_t handle, float threshold) {
    auto it = constraints_.find(handle);
    if (it == constraints_.end()) return;
    it->second.breakingImpulse = threshold < 0.0f ? 0.0f : threshold;
}

float PhysicsWorld::getConstraintBreakingImpulse(uint32_t handle) const {
    auto it = constraints_.find(handle);
    return it == constraints_.end() ? 0.0f : it->second.breakingImpulse;
}

// Magnitude of the position-constraint impulse (N·s) applied last step. Jolt has
// no generic accessor (see Constraint.h ~150), so dispatch on the sub-type. This
// is the "force holding the bodies together" — the natural breaking metric.
static float constraintPositionImpulse(JPH::Constraint* c) {
    switch (c->GetSubType()) {
        case EConstraintSubType::Point:
            return static_cast<PointConstraint*>(c)->GetTotalLambdaPosition().Length();
        case EConstraintSubType::Distance:
            return std::abs(static_cast<DistanceConstraint*>(c)->GetTotalLambdaPosition());
        case EConstraintSubType::Fixed:
            return static_cast<FixedConstraint*>(c)->GetTotalLambdaPosition().Length();
        case EConstraintSubType::Hinge:
            return static_cast<HingeConstraint*>(c)->GetTotalLambdaPosition().Length();
        case EConstraintSubType::Slider: {
            auto v = static_cast<SliderConstraint*>(c)->GetTotalLambdaPosition();
            return std::sqrt(v[0] * v[0] + v[1] * v[1]);
        }
        case EConstraintSubType::Cone:
            return static_cast<ConeConstraint*>(c)->GetTotalLambdaPosition().Length();
        case EConstraintSubType::SwingTwist:
            return static_cast<SwingTwistConstraint*>(c)->GetTotalLambdaPosition().Length();
        case EConstraintSubType::Pulley:
            return std::abs(static_cast<PulleyConstraint*>(c)->GetTotalLambdaPosition());
        case EConstraintSubType::SixDOF:
            return static_cast<SixDOFConstraint*>(c)->GetTotalLambdaPosition().Length();
        default:
            return 0.0f;  // gear/rack/path/vehicle — no meaningful position impulse
    }
}

void PhysicsWorld::checkBrokenConstraints() {
    for (auto& [handle, entry] : constraints_) {
        if (entry.breakingImpulse <= 0.0f || !entry.ref) continue;
        if (!entry.ref->GetEnabled()) continue;
        if (constraintPositionImpulse(entry.ref.GetPtr()) > entry.breakingImpulse) {
            entry.ref->SetEnabled(false);
            if (entry.ref2) entry.ref2->SetEnabled(false);
            brokenConstraints_.push_back(handle);
        }
    }
}

std::vector<uint32_t> PhysicsWorld::drainBrokenConstraints() {
    std::vector<uint32_t> out;
    out.swap(brokenConstraints_);
    return out;
}


} // namespace bro::physics
