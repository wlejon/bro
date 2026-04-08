#include "scene/physics_node.h"
#include "physics/physics_world.h"

JPH_SUPPRESS_WARNINGS

namespace bro::scene {

PhysicsNode::PhysicsNode(const std::string& name) : SceneNode(name) {}

void PhysicsNode::syncFromPhysics(physics::PhysicsWorld* world) {
    if (!hasBody_ || !world) return;

    auto pos = world->getPosition(bodyId_);
    auto rot = world->getRotation(bodyId_);

    // Convert 3D position to 2D scene coordinates.
    // For 2D games, typically X = right, Y = down in screen space.
    // Jolt uses Y-up, so we negate Y for screen coordinates.
    float sx = pos.GetX() * pixelsPerUnit_;
    float sy = -pos.GetY() * pixelsPerUnit_;

    // Extract 2D rotation from quaternion.
    // For 2D physics (rotation around Z axis), the angle is:
    //   angle = 2 * atan2(qz, qw)
    // Negate because screen Y is flipped.
    float angle = -2.0f * std::atan2(rot.GetZ(), rot.GetW());

    setPosition(sx, sy, pos.GetZ() * pixelsPerUnit_);
    setRotation(angle);
}

void PhysicsNode::syncToPhysics(physics::PhysicsWorld* world) {
    if (!hasBody_ || !world) return;

    const auto& pos = position();
    float px = pos.x / pixelsPerUnit_;
    float py = -pos.y / pixelsPerUnit_;
    float pz = pos.z / pixelsPerUnit_;

    // Convert 2D rotation back to quaternion (rotation around Z axis)
    float halfAngle = -rotation() * 0.5f;
    float qz = std::sin(halfAngle);
    float qw = std::cos(halfAngle);

    world->setPosition(bodyId_, JPH::RVec3(px, py, pz));
    world->setRotation(bodyId_, JPH::Quat(0, 0, qz, qw));
}

} // namespace bro::scene
