#pragma once

#include "scene/scene_node.h"
#include <cstdint>

#include <Jolt/Jolt.h>
#include <Jolt/Physics/Body/BodyID.h>

namespace bro::physics { class PhysicsWorld; }

namespace bro::scene {

/// A scene node bound to a Jolt physics body.
/// During syncPhysics(), the body's position and rotation are copied
/// into this node's local transform (position + rotation).
///
/// PhysicsNode can also be a parent of renderable nodes (ShapeNode, SpriteNode)
/// so that visuals automatically follow the physics body.
class PhysicsNode : public SceneNode {
public:
    explicit PhysicsNode(const std::string& name = "");

    Type type() const override { return Type::Physics; }

    /// Bind to a physics body. The node does NOT own the body — the PhysicsWorld does.
    void setBody(JPH::BodyID id) { bodyId_ = id; hasBody_ = true; }
    JPH::BodyID bodyId() const { return bodyId_; }
    bool hasBody() const { return hasBody_; }
    void clearBody() { hasBody_ = false; bodyId_ = JPH::BodyID(); }

    /// Sync this node's transform from the physics body.
    /// Called by SceneGraph::syncPhysics() when the physics thread is idle.
    void syncFromPhysics(physics::PhysicsWorld* world);

    /// Push this node's transform back to the physics body (kinematic).
    void syncToPhysics(physics::PhysicsWorld* world);

    /// Convenience: whether physics-to-node sync is enabled (default true).
    bool autoSync() const { return autoSync_; }
    void setAutoSync(bool v) { autoSync_ = v; }

    /// Scale factor to convert between physics units and scene/pixel units.
    /// E.g., if physics uses meters and scene uses pixels at 50px/m,
    /// set pixelsPerMeter = 50.
    void setPixelsPerUnit(float ppu) { pixelsPerUnit_ = ppu; }
    float pixelsPerUnit() const { return pixelsPerUnit_; }

private:
    JPH::BodyID bodyId_;
    bool hasBody_ = false;
    bool autoSync_ = true;
    float pixelsPerUnit_ = 1.0f;
};

} // namespace bro::scene
