#include "scene/physics_node.h"
#include "physics/physics_world.h"

JPH_SUPPRESS_WARNINGS

namespace bro::scene {

using bromath::Quat;
using bromath::Vec3;

PhysicsNode::PhysicsNode(const std::string& name) : SceneNode(name) {}

void PhysicsNode::syncFromPhysics(physics::PhysicsWorld* world) {
    if (!hasBody_ || !world) return;

    // Render-side consumer: reads the interpolated transform when the world
    // has interpolation enabled (Physics.setInterpolation), the true stepped
    // transform otherwise. Physics queries are unaffected.
    JPH::RVec3 pos;
    JPH::Quat rot;
    world->getRenderTransform(bodyId_, pos, rot);

    float sx = pos.GetX() * pixelsPerUnit_;
    float sy = pos.GetY() * pixelsPerUnit_;
    float sz = pos.GetZ() * pixelsPerUnit_;

    setPosition(sx, sy, sz);
    setRotation(Quat(rot.GetX(), rot.GetY(), rot.GetZ(), rot.GetW()));
}

void PhysicsNode::syncToPhysics(physics::PhysicsWorld* world) {
    if (!hasBody_ || !world) return;

    const auto& pos = position();
    float px = pos.x / pixelsPerUnit_;
    float py = pos.y / pixelsPerUnit_;
    float pz = pos.z / pixelsPerUnit_;

    const auto& rot = rotation();

    world->setPosition(bodyId_, JPH::RVec3(px, py, pz));
    world->setRotation(bodyId_, JPH::Quat(rot.x, rot.y, rot.z, rot.w));
}

} // namespace bro::scene
