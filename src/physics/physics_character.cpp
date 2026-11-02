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

// --- Character controllers ---

uint32_t PhysicsWorld::createCharacter(const CharacterOptions& opts) {
    if (!initialized_) return 0;
    Vec3 up = opts.up.NormalizedOr(Vec3::sAxisY());

    CapsuleShapeSettings capsule(opts.halfHeight, opts.radius);
    auto shape = capsule.Create();
    if (shape.HasError()) return 0;

    int layer = opts.layer;
    if (layer < 0 || layer >= numLayers_) layer = numLayers_ > 1 ? 1 : 0;

    Ref<CharacterVirtualSettings> settings = new CharacterVirtualSettings();
    settings->mShape = shape.Get();
    settings->mUp = up;
    settings->mMass = opts.mass;
    settings->mMaxSlopeAngle = DegreesToRadians(opts.maxSlopeAngle);
    settings->mMaxStrength = opts.maxStrength;
    settings->mCharacterPadding = opts.padding;
    // Only contacts on the bottom sphere cap can support the character —
    // without this, side contacts on the cylinder count as "ground".
    settings->mSupportingVolume = Plane(up, -opts.radius);
    if (opts.innerBody) {
        // Kinematic body that shadows the character so sensors, raycasts and
        // ordinary bodies can see it (a bare CharacterVirtual never enters
        // the broadphase). Created/destroyed by the CharacterVirtual itself.
        int innerLayer = opts.innerBodyLayer;
        if (innerLayer < 0 || innerLayer >= numLayers_) innerLayer = layer;
        settings->mInnerBodyShape = shape.Get();
        settings->mInnerBodyLayer = static_cast<ObjectLayer>(innerLayer);
    }

    CharacterEntry entry;
    entry.character = new CharacterVirtual(settings, opts.position,
                                           Quat::sIdentity(), 0, &physicsSystem_);
    entry.stepUp = opts.stepUp;
    entry.stickToFloor = opts.stickToFloor;
    entry.layer = layer;

    // Characters collide with each other (instead of ghosting through).
    entry.character->SetCharacterVsCharacterCollision(charVsChar_.get());
    charVsChar_->Add(entry.character.GetPtr());

    // The inner body reuses a body-manager index — reset the per-index
    // listener tables so it can't inherit a destroyed body's flags.
    BodyID innerId = entry.character->GetInnerBodyID();
    if (listener_ && !innerId.IsInvalid()) {
        listener_->setSensorId(innerId, false);
        listener_->setCombineModes(innerId, CombineMode::Default, CombineMode::Default);
    }

    uint32_t handle = nextCharacterHandle_++;
    characters_[handle] = std::move(entry);
    return handle;
}

void PhysicsWorld::destroyCharacter(uint32_t handle) {
    auto it = characters_.find(handle);
    if (it == characters_.end()) return;
    if (charVsChar_ && it->second.character)
        charVsChar_->Remove(it->second.character.GetPtr());
    characters_.erase(it);   // Ref release destroys the inner body (if any)
}

void PhysicsWorld::setCharacterVelocity(uint32_t handle, Vec3 v) {
    auto it = characters_.find(handle);
    if (it != characters_.end()) it->second.desiredVelocity = v;
}

void PhysicsWorld::setCharacterPosition(uint32_t handle, RVec3 pos) {
    auto it = characters_.find(handle);
    if (it != characters_.end()) it->second.character->SetPosition(pos);
}

bool PhysicsWorld::setCharacterShape(uint32_t handle, const BodyOptions& shapeOpts) {
    auto it = characters_.find(handle);
    if (it == characters_.end() || !it->second.character) return false;
    // Mesh-family shapes can't be a character volume.
    if (shapeOpts.shape == BodyOptions::ShapeMesh ||
        shapeOpts.shape == BodyOptions::ShapeChain ||
        shapeOpts.shape == BodyOptions::ShapeHeightField) return false;
    auto shape = buildShape(shapeOpts);
    if (!shape) return false;

    CharacterVirtual* c = it->second.character.GetPtr();
    const ObjectLayer layer(static_cast<ObjectLayer>(it->second.layer));

    // Feet-planted stance change: the character's position is the shape
    // CENTER, so swapping shapes in place would sink a taller shape into the
    // floor (making "stand up" always fail) and float a shorter one. Shift
    // the position along `up` so the new shape's bottom lands where the old
    // one's was, try the switch there, and restore on refusal.
    const Vec3 up = c->GetUp();
    auto bottomAlongUp = [&](const Shape* s) {
        const AABox b = s->GetLocalBounds();
        // Support point along -up: min over box corners of dot(corner, up).
        const Vec3 corner(up.GetX() >= 0.0f ? b.mMin.GetX() : b.mMax.GetX(),
                          up.GetY() >= 0.0f ? b.mMin.GetY() : b.mMax.GetY(),
                          up.GetZ() >= 0.0f ? b.mMin.GetZ() : b.mMax.GetZ());
        return corner.Dot(up);
    };
    const float shift = bottomAlongUp(c->GetShape()) - bottomAlongUp(shape.GetPtr());
    const RVec3 oldPos = c->GetPosition();
    if (shift != 0.0f) c->SetPosition(oldPos + up * shift);

    // Finite max penetration ⇒ Jolt checks the new shape for room first and
    // refuses the switch when it would start deeply penetrating (e.g.
    // standing up under a low ceiling). 1.5*slop is Jolt's sample value.
    const bool ok = c->SetShape(
        shape.GetPtr(),
        1.5f * physicsSystem_.GetPhysicsSettings().mPenetrationSlop,
        physicsSystem_.GetDefaultBroadPhaseLayerFilter(layer),
        physicsSystem_.GetDefaultLayerFilter(layer),
        {}, {}, *tempAllocator_);
    if (ok) {
        if (!c->GetInnerBodyID().IsInvalid()) c->SetInnerBodyShape(shape.GetPtr());
    } else if (shift != 0.0f) {
        c->SetPosition(oldPos);   // refused — stay in the old stance, in place
    }
    return ok;
}

JPH::BodyID PhysicsWorld::characterInnerBody(uint32_t handle) const {
    auto it = characters_.find(handle);
    if (it == characters_.end() || !it->second.character) return BodyID();
    return it->second.character->GetInnerBodyID();
}

bool PhysicsWorld::getCharacterState(uint32_t handle, CharacterState& out) const {
    auto it = characters_.find(handle);
    if (it == characters_.end()) return false;
    const CharacterVirtual* c = it->second.character.GetPtr();
    out.position = c->GetPosition();
    out.velocity = c->GetLinearVelocity();
    switch (c->GetGroundState()) {
        case CharacterBase::EGroundState::OnGround:      out.ground = CharacterGround::OnGround; break;
        case CharacterBase::EGroundState::OnSteepGround: out.ground = CharacterGround::OnSteepGround; break;
        case CharacterBase::EGroundState::NotSupported:  out.ground = CharacterGround::NotSupported; break;
        case CharacterBase::EGroundState::InAir:         out.ground = CharacterGround::InAir; break;
    }
    out.groundNormal = c->GetGroundNormal();
    out.groundVelocity = c->GetGroundVelocity();
    out.groundBody = c->GetGroundBodyID();
    return true;
}

void PhysicsWorld::updateCharacters(float dt) {
    if (characters_.empty() || dt <= 0.0f) return;
    const Vec3 gravity = physicsSystem_.GetGravity();

    for (auto& [handle, ch] : characters_) {
        CharacterVirtual* c = ch.character.GetPtr();
        // The ground body may have moved/changed velocity last world step.
        c->UpdateGroundVelocity();

        const Vec3 up = c->GetUp();
        const Vec3 desired = ch.desiredVelocity;
        const Vec3 desiredHorizontal = desired - desired.Dot(up) * up;
        const Vec3 groundVelocity = c->GetGroundVelocity();
        const float currentUp = c->GetLinearVelocity().Dot(up);

        Vec3 newVelocity;
        const bool movingTowardsGround = (currentUp - groundVelocity.Dot(up)) < 0.1f;
        if (c->GetGroundState() == CharacterBase::EGroundState::OnGround && movingTowardsGround) {
            // Supported: ride the ground (moving-platform carry) plus the
            // app's desired velocity; a positive up component launches a jump.
            newVelocity = groundVelocity + desired;
        } else {
            // Unsupported (in air / too-steep slope) or already moving away
            // from the ground: keep vertical momentum, integrate gravity, and
            // steer horizontally only — the desired up component is ignored so
            // holding "jump" can't fly.
            newVelocity = currentUp * up + gravity * dt + desiredHorizontal;
        }
        c->SetLinearVelocity(newVelocity);

        CharacterVirtual::ExtendedUpdateSettings settings;
        settings.mWalkStairsStepUp = up * ch.stepUp;
        settings.mStickToFloorStepDown = -up * ch.stickToFloor;
        c->ExtendedUpdate(dt, gravity, settings,
                          physicsSystem_.GetDefaultBroadPhaseLayerFilter(ObjectLayer(ch.layer)),
                          physicsSystem_.GetDefaultLayerFilter(ObjectLayer(ch.layer)),
                          {}, {}, *tempAllocator_);
    }
}


} // namespace bro::physics
