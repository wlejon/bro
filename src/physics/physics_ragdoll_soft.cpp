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

// --- Ragdolls ---
//
// A Jolt Ragdoll bundles part bodies + inter-part constraints and owns their
// lifetime: RemoveFromPhysicsSystem detaches everything as a unit and the
// final Ref release destroys the bodies (~Ragdoll). The parts are ordinary
// dynamic bodies while alive — all body APIs work on them — but their
// constraints live inside the Ragdoll, never in constraints_, and the
// destroyAll/shutdown body sweeps must run AFTER the ragdoll registry is
// cleared so no body is destroyed twice.

static RefConst<Shape> buildRagdollPartShape(const RagdollPartOptions& p) {
    switch (p.shape) {
        case RagdollPartOptions::ShapeCapsule: {
            if (p.halfHeight <= 0.0f || p.radius <= 0.0f) return RefConst<Shape>();
            CapsuleShapeSettings s(p.halfHeight, p.radius);
            s.SetDensity(p.density);
            auto r = s.Create();
            return r.HasError() ? RefConst<Shape>() : r.Get();
        }
        case RagdollPartOptions::ShapeSphere: {
            if (p.radius <= 0.0f) return RefConst<Shape>();
            SphereShapeSettings s(p.radius);
            s.SetDensity(p.density);
            auto r = s.Create();
            return r.HasError() ? RefConst<Shape>() : r.Get();
        }
        case RagdollPartOptions::ShapeBox: {
            BoxShapeSettings s(p.halfExtents);
            s.SetDensity(p.density);
            auto r = s.Create();
            return r.HasError() ? RefConst<Shape>() : r.Get();
        }
    }
    return RefConst<Shape>();
}

uint32_t PhysicsWorld::createRagdoll(const RagdollOptions& opts) {
    if (!initialized_ || opts.parts.empty()) return 0;
    const int n = (int)opts.parts.size();

    // Jolt requires parents before children in the joint array.
    for (int i = 0; i < n; i++) {
        int p = opts.parts[i].parentIndex;
        if (p >= i || p < -1) return 0;
    }

    int layer = opts.layer;
    if (layer < 0 || layer >= numLayers_) layer = 1;

    const Quat worldRot = opts.rotation.Normalized();

    Ref<RagdollSettings> settings = new RagdollSettings();
    settings->mSkeleton = new Skeleton();
    settings->mParts.resize(n);

    Array<Mat44> bindModel(n);  // model-space bind matrices (overlap detection)

    for (int i = 0; i < n; i++) {
        const RagdollPartOptions& p = opts.parts[i];
        std::string name = p.name.empty() ? ("part" + std::to_string(i)) : p.name;
        settings->mSkeleton->AddJoint(name, p.parentIndex);

        auto shape = buildRagdollPartShape(p);
        if (!shape) return 0;

        const Quat bindRot = p.rotation.Normalized();
        bindModel[i] = Mat44::sRotationTranslation(bindRot, p.position);

        RagdollSettings::Part& part = settings->mParts[i];
        part.SetShape(shape.GetPtr());
        part.mPosition = opts.position + worldRot * p.position;
        part.mRotation = (worldRot * bindRot).Normalized();
        part.mMotionType = EMotionType::Dynamic;
        part.mObjectLayer = static_cast<ObjectLayer>(layer);
        part.mFriction = p.friction;
        part.mRestitution = p.restitution;
        part.mGravityFactor = opts.gravityFactor;
        part.mLinearDamping = opts.linearDamping;
        part.mAngularDamping = opts.angularDamping;
        if (p.mass > 0.0f) {
            // Clean mass override: keep the shape's inertia distribution,
            // scaled to the requested mass (no density hacks).
            part.mOverrideMassProperties = EOverrideMassProperties::CalculateInertia;
            part.mMassPropertiesOverride.mMass = p.mass;
        }

        if (p.parentIndex < 0) continue;
        const RagdollPartOptions& parent = opts.parts[p.parentIndex];

        // Joint pivot: explicit, or this part's bind position.
        Vec3 pivotModel = p.hasJointPoint ? p.jointPoint : p.position;
        RVec3 pivotWorld = opts.position + worldRot * pivotModel;

        if (p.joint == RagdollPartOptions::JointFixed) {
            auto* fs = new FixedConstraintSettings();
            fs->mSpace = EConstraintSpace::WorldSpace;
            fs->mAutoDetectPoint = true;
            part.mToParent = fs;
            continue;
        }

        // Twist axis: explicit, or parent→child bind direction (model space).
        Vec3 twistModel = p.hasTwistAxis
            ? p.twistAxis
            : (p.position - parent.position);
        twistModel = twistModel.NormalizedOr(Vec3::sAxisY());
        Vec3 twistWorld = (worldRot * twistModel).Normalized();

        // Plane axis: explicit (re-orthonormalized against twist), or any
        // perpendicular — SwingTwist requires plane ⟂ twist.
        Vec3 planeWorld;
        if (p.hasPlaneAxis) {
            Vec3 pw = worldRot * p.planeAxis;
            pw -= twistWorld * pw.Dot(twistWorld);
            planeWorld = pw.NormalizedOr(twistWorld.GetNormalizedPerpendicular());
        } else {
            planeWorld = twistWorld.GetNormalizedPerpendicular();
        }

        auto* ss = new SwingTwistConstraintSettings();
        ss->mSpace = EConstraintSpace::WorldSpace;
        ss->mPosition1 = pivotWorld;
        ss->mPosition2 = pivotWorld;
        ss->mTwistAxis1 = twistWorld;
        ss->mTwistAxis2 = twistWorld;
        ss->mPlaneAxis1 = planeWorld;
        ss->mPlaneAxis2 = planeWorld;
        constexpr float kPi = 3.14159265358979f;
        ss->mNormalHalfConeAngle = std::clamp(p.normalHalfConeAngle, 0.0f, kPi);
        ss->mPlaneHalfConeAngle = std::clamp(p.planeHalfConeAngle, 0.0f, kPi);
        ss->mTwistMinAngle = std::clamp(p.twistMinAngle, -kPi, kPi);
        ss->mTwistMaxAngle = std::clamp(std::max(p.twistMaxAngle, p.twistMinAngle), -kPi, kPi);
        ss->mMaxFrictionTorque = p.maxFrictionTorque;
        // Drive-motor springs (used when driveRagdollToPose powers the joint).
        auto configureMotor = [&](MotorSettings& m) {
            m.mSpringSettings.mMode = ESpringMode::FrequencyAndDamping;
            m.mSpringSettings.mFrequency = opts.motor.frequency;
            m.mSpringSettings.mDamping = opts.motor.damping;
            if (opts.motor.maxTorque >= 0.0f) m.SetTorqueLimit(opts.motor.maxTorque);
        };
        configureMotor(ss->mSwingMotorSettings);
        configureMotor(ss->mTwistMotorSettings);
        part.mToParent = ss;
    }

    // Clamp parent/child mass ratios + grow parent inertia so long chains
    // don't oscillate (Havok-style stabilization).
    if (opts.stabilize) settings->Stabilize();

    // Parent/child pairs never self-collide; the bind pose also disables any
    // part pairs that overlap at rest (e.g. pelvis vs spine capsules).
    settings->DisableParentChildCollisions(bindModel.data());
    settings->CalculateBodyIndexToConstraintIndex();

    Ragdoll* rag = settings->CreateRagdoll(nextRagdollGroup_++, 0, &physicsSystem_);
    if (!rag) return 0;
    Ref<Ragdoll> ragRef(rag);
    rag->AddToPhysicsSystem(opts.activate ? EActivation::Activate
                                          : EActivation::DontActivate);

    // Ragdoll part bodies reuse body-manager indices; reset the per-index
    // listener tables (sensor bit, combine modes) so a part never inherits a
    // destroyed body's flags.
    if (listener_) {
        for (const BodyID& id : rag->GetBodyIDs()) {
            listener_->setSensorId(id, false);
            listener_->setCombineModes(id, CombineMode::Default, CombineMode::Default);
        }
    }

    RagdollEntry entry;
    entry.ragdoll = ragRef;
    entry.settings = settings;
    entry.parentIndex.reserve(n);
    for (int i = 0; i < n; i++) entry.parentIndex.push_back(opts.parts[i].parentIndex);

    uint32_t handle = nextRagdollHandle_++;
    ragdolls_[handle] = std::move(entry);
    return handle;
}

void PhysicsWorld::removeRagdollFromSystem(RagdollEntry& e) {
    if (!e.ragdoll) return;
    // User constraints attached to part bodies (e.g. a grab) must go first —
    // ~Ragdoll destroys the bodies and a live constraint would dangle.
    for (const BodyID& id : e.ragdoll->GetBodyIDs())
        removeConstraintsReferencing(id);
    e.ragdoll->RemoveFromPhysicsSystem();
}

void PhysicsWorld::destroyRagdoll(uint32_t handle,
                                  const std::function<void(BodyID)>& onBodyDestroyed) {
    auto it = ragdolls_.find(handle);
    if (it == ragdolls_.end()) return;
    if (it->second.ragdoll) {
        const auto& ids = it->second.ragdoll->GetBodyIDs();
        if (onBodyDestroyed) {
            for (const BodyID& pid : ids) onBodyDestroyed(pid);
        }
        for (const BodyID& pid : ids) evictAreaBookkeeping(pid);
    }
    removeRagdollFromSystem(it->second);
    ragdolls_.erase(it);  // Ref release destroys the part bodies
}

int PhysicsWorld::ragdollPartCount(uint32_t handle) const {
    auto it = ragdolls_.find(handle);
    return it == ragdolls_.end() ? -1 : (int)it->second.ragdoll->GetBodyCount();
}

BodyID PhysicsWorld::ragdollPartBody(uint32_t handle, int part) const {
    auto it = ragdolls_.find(handle);
    if (it == ragdolls_.end()) return BodyID();
    if (part < 0 || part >= (int)it->second.ragdoll->GetBodyCount()) return BodyID();
    return it->second.ragdoll->GetBodyID(part);
}

int PhysicsWorld::ragdollPartParent(uint32_t handle, int part) const {
    auto it = ragdolls_.find(handle);
    if (it == ragdolls_.end()) return -1;
    if (part < 0 || part >= (int)it->second.parentIndex.size()) return -1;
    return it->second.parentIndex[part];
}

bool PhysicsWorld::getRagdollPose(uint32_t handle,
                                  std::vector<RagdollPartState>& out) const {
    auto it = ragdolls_.find(handle);
    if (it == ragdolls_.end()) return false;
    const auto& ids = it->second.ragdoll->GetBodyIDs();
    const BodyInterface& bi = physicsSystem_.GetBodyInterface();
    out.resize(ids.size());
    for (size_t i = 0; i < ids.size(); i++) {
        bi.GetPositionAndRotation(ids[i], out[i].position, out[i].rotation);
    }
    return true;
}

bool PhysicsWorld::setRagdollPose(uint32_t handle,
                                  const std::vector<RagdollPartState>& pose) {
    auto it = ragdolls_.find(handle);
    if (it == ragdolls_.end()) return false;
    const auto& ids = it->second.ragdoll->GetBodyIDs();
    if (pose.size() != ids.size()) return false;
    BodyInterface& bi = physicsSystem_.GetBodyInterface();
    for (size_t i = 0; i < ids.size(); i++) {
        bi.SetPositionAndRotation(ids[i], pose[i].position,
                                  pose[i].rotation.Normalized(),
                                  EActivation::DontActivate);
    }
    return true;
}

bool PhysicsWorld::driveRagdollToPose(uint32_t handle,
                                      const std::vector<RagdollPartState>& pose,
                                      const RagdollMotorOptions* motor) {
    auto it = ragdolls_.find(handle);
    if (it == ragdolls_.end()) return false;
    RagdollEntry& e = it->second;
    const int n = (int)e.ragdoll->GetBodyCount();
    if ((int)pose.size() != n) return false;

    // Jolt DriveToPoseUsingMotors semantics, fed from world-space part
    // transforms: the position-motor target of each swing-twist joint is the
    // child's rotation relative to its parent in the TARGET pose. The root is
    // not driven (no joint) — its position stays free.
    for (int i = 0; i < n; i++) {
        int ci = e.settings->GetConstraintIndexForBodyIndex(i);
        int parent = e.parentIndex[i];
        if (ci < 0 || parent < 0) continue;
        TwoBodyConstraint* c = e.ragdoll->GetConstraint(ci);
        if (c->GetSubType() != EConstraintSubType::SwingTwist) continue;
        auto* st = static_cast<SwingTwistConstraint*>(c);
        if (motor) {
            auto apply = [&](MotorSettings& m) {
                m.mSpringSettings.mMode = ESpringMode::FrequencyAndDamping;
                m.mSpringSettings.mFrequency = motor->frequency;
                m.mSpringSettings.mDamping = motor->damping;
                if (motor->maxTorque >= 0.0f) m.SetTorqueLimit(motor->maxTorque);
                else m.SetTorqueLimits(-FLT_MAX, FLT_MAX);
            };
            apply(st->GetSwingMotorSettings());
            apply(st->GetTwistMotorSettings());
        }
        Quat target =
            (pose[parent].rotation.Conjugated() * pose[i].rotation).Normalized();
        st->SetSwingMotorState(EMotorState::Position);
        st->SetTwistMotorState(EMotorState::Position);
        st->SetTargetOrientationBS(target);
    }
    e.ragdoll->Activate();
    return true;
}

bool PhysicsWorld::driveRagdollToPoseKinematic(uint32_t handle,
                                               const std::vector<RagdollPartState>& pose,
                                               float dt) {
    auto it = ragdolls_.find(handle);
    if (it == ragdolls_.end() || dt <= 0.0f) return false;
    const auto& ids = it->second.ragdoll->GetBodyIDs();
    if (pose.size() != ids.size()) return false;
    it->second.ragdoll->Activate();
    BodyInterface& bi = physicsSystem_.GetBodyInterface();
    for (size_t i = 0; i < ids.size(); i++) {
        bi.MoveKinematic(ids[i], pose[i].position, pose[i].rotation.Normalized(), dt);
    }
    return true;
}

void PhysicsWorld::stopRagdollDrive(uint32_t handle) {
    auto it = ragdolls_.find(handle);
    if (it == ragdolls_.end()) return;
    Ragdoll* rag = it->second.ragdoll.GetPtr();
    for (int ci = 0; ci < (int)rag->GetConstraintCount(); ci++) {
        TwoBodyConstraint* c = rag->GetConstraint(ci);
        if (c->GetSubType() != EConstraintSubType::SwingTwist) continue;
        auto* st = static_cast<SwingTwistConstraint*>(c);
        st->SetSwingMotorState(EMotorState::Off);
        st->SetTwistMotorState(EMotorState::Off);
    }
}

void PhysicsWorld::addRagdollImpulse(uint32_t handle, Vec3 impulse) {
    auto it = ragdolls_.find(handle);
    if (it == ragdolls_.end()) return;
    it->second.ragdoll->AddImpulse(impulse);
}

void PhysicsWorld::activateRagdoll(uint32_t handle) {
    auto it = ragdolls_.find(handle);
    if (it == ragdolls_.end()) return;
    it->second.ragdoll->Activate();
}

void PhysicsWorld::deactivateRagdoll(uint32_t handle) {
    auto it = ragdolls_.find(handle);
    if (it == ragdolls_.end()) return;
    BodyInterface& bi = physicsSystem_.GetBodyInterface();
    for (const BodyID& id : it->second.ragdoll->GetBodyIDs())
        bi.DeactivateBody(id);
}

bool PhysicsWorld::isRagdollActive(uint32_t handle) const {
    auto it = ragdolls_.find(handle);
    if (it == ragdolls_.end()) return false;
    return it->second.ragdoll->IsActive();
}

// --- Soft bodies ---
//
// Jolt soft bodies are ordinary bodies whose SoftBodyMotionProperties hold
// the XPBD vertex state; they step inside PhysicsSystem::Update like any
// dynamic body, so no per-step bookkeeping is needed here. The registry
// entry pins the SoftBodySharedSettings Ref and remembers the even mass
// split for unpinning.

// Build a cloth grid: gridX*gridZ vertices in the local XZ plane (Y up),
// centered on the origin. Vertex (x,z) lives at index z*gridX + x; faces
// wind CCW seen from +Y so the rendered front faces (and the rest normals a
// render mesh derives from them) point up.
static Ref<SoftBodySharedSettings> buildClothSettings(const SoftBodyOptions& o,
                                                      float invMass) {
    const int gx = std::max(2, o.gridX);
    const int gz = std::max(2, o.gridZ);
    const float sp = o.spacing > 0.0f ? o.spacing : 0.1f;
    const float halfX = 0.5f * (gx - 1) * sp;
    const float halfZ = 0.5f * (gz - 1) * sp;

    Ref<SoftBodySharedSettings> s = new SoftBodySharedSettings();
    s->mVertices.reserve((size_t)gx * gz);
    for (int z = 0; z < gz; z++)
        for (int x = 0; x < gx; x++)
            s->mVertices.push_back(SoftBodySharedSettings::Vertex(
                Float3(x * sp - halfX, 0.0f, z * sp - halfZ),
                Float3(0, 0, 0), invMass));

    for (int z = 0; z + 1 < gz; z++) {
        for (int x = 0; x + 1 < gx; x++) {
            uint32_t v00 = (uint32_t)(z * gx + x);
            uint32_t v10 = v00 + 1;
            uint32_t v01 = v00 + gx;
            uint32_t v11 = v01 + 1;
            s->AddFace(SoftBodySharedSettings::Face(v00, v01, v11));
            s->AddFace(SoftBodySharedSettings::Face(v00, v11, v10));
        }
    }
    return s;
}

// Build settings for an arbitrary triangle mesh. Degenerate and out-of-range
// triangles are skipped. With pressure the enclosed volume must be positive
// (CCW-outward winding); a negative signed rest volume flips all faces.
static Ref<SoftBodySharedSettings> buildMeshSettings(const SoftBodyOptions& o,
                                                     float invMass) {
    if (o.vertices.size() < 3 || o.indices.size() < 3) return nullptr;
    const uint32_t n = (uint32_t)o.vertices.size();

    Ref<SoftBodySharedSettings> s = new SoftBodySharedSettings();
    s->mVertices.reserve(n);
    for (const Vec3& v : o.vertices)
        s->mVertices.push_back(SoftBodySharedSettings::Vertex(
            Float3(v.GetX(), v.GetY(), v.GetZ()), Float3(0, 0, 0), invMass));

    for (size_t i = 0; i + 2 < o.indices.size(); i += 3) {
        uint32_t a = o.indices[i], b = o.indices[i + 1], c = o.indices[i + 2];
        if (a >= n || b >= n || c >= n) continue;
        SoftBodySharedSettings::Face f(a, b, c);
        if (f.IsDegenerate()) continue;
        s->AddFace(f);
    }
    if (s->mFaces.empty()) return nullptr;

    if (o.pressure > 0.0f) {
        // Signed rest volume (same sum ApplyPressure uses). Inside-out
        // meshes would deflate instead of inflate — flip them.
        float sixVolume = 0.0f;
        for (const auto& f : s->mFaces) {
            Vec3 x1(s->mVertices[f.mVertex[0]].mPosition);
            Vec3 x2(s->mVertices[f.mVertex[1]].mPosition);
            Vec3 x3(s->mVertices[f.mVertex[2]].mPosition);
            sixVolume += x1.Cross(x2).Dot(x3);
        }
        if (sixVolume < 0.0f)
            for (auto& f : s->mFaces) std::swap(f.mVertex[1], f.mVertex[2]);
    }
    return s;
}

uint32_t PhysicsWorld::createSoftBody(const SoftBodyOptions& opts) {
    if (!initialized_) return 0;

    // Even mass split. Pinned vertices are zeroed after the split so a few
    // pins don't change the felt weight of the rest of the body.
    size_t nVerts = opts.kind == SoftBodyOptions::Cloth
        ? (size_t)std::max(2, opts.gridX) * std::max(2, opts.gridZ)
        : opts.vertices.size();
    if (nVerts < 3 && opts.kind == SoftBodyOptions::Mesh) return 0;
    const float mass = opts.mass > 0.0f ? opts.mass : 1.0f;
    const float invMass = (float)nVerts / mass;

    Ref<SoftBodySharedSettings> settings = opts.kind == SoftBodyOptions::Cloth
        ? buildClothSettings(opts, invMass)
        : buildMeshSettings(opts, invMass);
    if (!settings) return 0;

    for (uint32_t idx : opts.pinned)
        if (idx < settings->mVertices.size())
            settings->mVertices[idx].mInvMass = 0.0f;

    // Derive edge (+ shear for quads) and optional bend constraints from the
    // faces. Compliance is XPBD's inverse stiffness: 0 = rigid spring.
    SoftBodySharedSettings::VertexAttributes attr;
    attr.mCompliance = std::max(0.0f, opts.compliance);
    attr.mShearCompliance = opts.shearCompliance >= 0.0f ? opts.shearCompliance
                                                         : attr.mCompliance;
    const bool bend = opts.bendCompliance >= 0.0f;
    attr.mBendCompliance = bend ? opts.bendCompliance : FLT_MAX;
    settings->CreateConstraints(&attr, 1,
        bend ? SoftBodySharedSettings::EBendType::Distance
             : SoftBodySharedSettings::EBendType::None);
    settings->Optimize();  // required: builds the parallel update groups

    int layer = opts.layer;
    if (layer < 0 || layer >= numLayers_) layer = 1;

    SoftBodyCreationSettings scs(settings, opts.position,
                                 opts.rotation.Normalized(),
                                 static_cast<ObjectLayer>(layer));
    scs.mNumIterations = (uint32)std::max(1, opts.numIterations);
    scs.mFriction = opts.friction;
    scs.mRestitution = opts.restitution;
    scs.mPressure = opts.kind == SoftBodyOptions::Mesh ? std::max(0.0f, opts.pressure) : 0.0f;
    scs.mLinearDamping = opts.linearDamping;
    scs.mGravityFactor = opts.gravityFactor;
    scs.mMaxLinearVelocity = opts.maxLinearVelocity;
    scs.mVertexRadius = std::max(0.0f, opts.vertexRadius);
    scs.mUpdatePosition = opts.updatePosition;
    scs.mFacesDoubleSided = opts.doubleSided;
    scs.mAllowSleeping = opts.allowSleeping;

    BodyInterface& bi = physicsSystem_.GetBodyInterface();
    BodyID id = bi.CreateAndAddSoftBody(scs, EActivation::Activate);
    if (id.IsInvalid()) return 0;
    if (listener_) {
        listener_->setSensorId(id, false);
        listener_->setCombineModes(id, CombineMode::Default, CombineMode::Default);
    }

    SoftBodyEntry entry;
    entry.settings = settings;
    entry.body = id;
    entry.defaultInvMass = invMass;

    uint32_t handle = nextSoftBodyHandle_++;
    softBodies_[handle] = std::move(entry);
    return handle;
}

void PhysicsWorld::destroySoftBody(uint32_t handle) {
    auto it = softBodies_.find(handle);
    if (it == softBodies_.end()) return;
    BodyID id = it->second.body;
    softBodies_.erase(it);
    destroyBody(id);  // entry already gone, so this is the generic path
}

BodyID PhysicsWorld::softBodyBody(uint32_t handle) const {
    auto it = softBodies_.find(handle);
    return it == softBodies_.end() ? BodyID() : it->second.body;
}

int PhysicsWorld::softBodyVertexCount(uint32_t handle) const {
    auto it = softBodies_.find(handle);
    if (it == softBodies_.end()) return -1;
    return (int)it->second.settings->mVertices.size();
}

bool PhysicsWorld::getSoftBodyVertices(uint32_t handle,
                                       std::vector<float>& outXyz) const {
    auto it = softBodies_.find(handle);
    if (it == softBodies_.end()) return false;
    BodyLockRead lock(physicsSystem_.GetBodyLockInterface(), it->second.body);
    if (!lock.Succeeded() || !lock.GetBody().IsSoftBody()) return false;
    const Body& body = lock.GetBody();
    const auto* mp =
        static_cast<const SoftBodyMotionProperties*>(body.GetMotionProperties());
    RMat44 com = body.GetCenterOfMassTransform();
    const auto& verts = mp->GetVertices();
    outXyz.resize(verts.size() * 3);
    for (size_t i = 0; i < verts.size(); i++) {
        RVec3 wp = com * verts[i].mPosition;  // vertices are COM-relative
        outXyz[i * 3 + 0] = (float)wp.GetX();
        outXyz[i * 3 + 1] = (float)wp.GetY();
        outXyz[i * 3 + 2] = (float)wp.GetZ();
    }
    return true;
}

bool PhysicsWorld::softBodyTopology(uint32_t handle, std::vector<float>& outXyz,
                                    std::vector<uint32_t>& outIndices) const {
    auto it = softBodies_.find(handle);
    if (it == softBodies_.end()) return false;
    const SoftBodySharedSettings* s = it->second.settings.GetPtr();
    outXyz.resize(s->mVertices.size() * 3);
    for (size_t i = 0; i < s->mVertices.size(); i++) {
        const Float3& p = s->mVertices[i].mPosition;
        outXyz[i * 3 + 0] = p.x;
        outXyz[i * 3 + 1] = p.y;
        outXyz[i * 3 + 2] = p.z;
    }
    outIndices.resize(s->mFaces.size() * 3);
    for (size_t i = 0; i < s->mFaces.size(); i++) {
        outIndices[i * 3 + 0] = s->mFaces[i].mVertex[0];
        outIndices[i * 3 + 1] = s->mFaces[i].mVertex[1];
        outIndices[i * 3 + 2] = s->mFaces[i].mVertex[2];
    }
    return true;
}

bool PhysicsWorld::setSoftBodyVertexPosition(uint32_t handle, int index,
                                             RVec3 pos) {
    auto it = softBodies_.find(handle);
    if (it == softBodies_.end()) return false;
    {
        BodyLockWrite lock(physicsSystem_.GetBodyLockInterface(), it->second.body);
        if (!lock.Succeeded() || !lock.GetBody().IsSoftBody()) return false;
        Body& body = lock.GetBody();
        auto* mp = static_cast<SoftBodyMotionProperties*>(body.GetMotionProperties());
        if (index < 0 || index >= (int)mp->GetVertices().size()) return false;
        auto& v = mp->GetVertex((uint)index);
        v.mPosition = Vec3(body.GetCenterOfMassTransform().Inversed() * pos);
        v.mVelocity = Vec3::sZero();
    }
    physicsSystem_.GetBodyInterface().ActivateBody(it->second.body);
    return true;
}

bool PhysicsWorld::setSoftBodyVertexVelocity(uint32_t handle, int index,
                                             Vec3 vel) {
    auto it = softBodies_.find(handle);
    if (it == softBodies_.end()) return false;
    {
        BodyLockWrite lock(physicsSystem_.GetBodyLockInterface(), it->second.body);
        if (!lock.Succeeded() || !lock.GetBody().IsSoftBody()) return false;
        auto* mp = static_cast<SoftBodyMotionProperties*>(
            lock.GetBody().GetMotionProperties());
        if (index < 0 || index >= (int)mp->GetVertices().size()) return false;
        mp->GetVertex((uint)index).mVelocity = vel;  // COM rotation is identity
    }
    physicsSystem_.GetBodyInterface().ActivateBody(it->second.body);
    return true;
}

bool PhysicsWorld::pinSoftBodyVertex(uint32_t handle, int index, bool pinned) {
    auto it = softBodies_.find(handle);
    if (it == softBodies_.end()) return false;
    {
        BodyLockWrite lock(physicsSystem_.GetBodyLockInterface(), it->second.body);
        if (!lock.Succeeded() || !lock.GetBody().IsSoftBody()) return false;
        auto* mp = static_cast<SoftBodyMotionProperties*>(
            lock.GetBody().GetMotionProperties());
        if (index < 0 || index >= (int)mp->GetVertices().size()) return false;
        auto& v = mp->GetVertex((uint)index);
        v.mInvMass = pinned ? 0.0f : it->second.defaultInvMass;
        if (pinned) v.mVelocity = Vec3::sZero();
    }
    physicsSystem_.GetBodyInterface().ActivateBody(it->second.body);
    return true;
}


} // namespace bro::physics
