#include "dynamics/world.h"

#include "box3d/box3d.h"
#include "box3d/math_functions.h"

#include <chrono>
#include <cmath>
#include <cstdint>

namespace bro::dynamics {

namespace {

b3Vec3 v3(const float* p) { return b3Vec3{p[0], p[1], p[2]}; }
b3Quat quat(const float* q) { return b3Quat{{q[0], q[1], q[2]}, q[3]}; }

Pose poseOf(const b3WorldTransform& t) {
    Pose p;
    p.p[0] = static_cast<float>(t.p.x);
    p.p[1] = static_cast<float>(t.p.y);
    p.p[2] = static_cast<float>(t.p.z);
    p.q[0] = t.q.v.x;
    p.q[1] = t.q.v.y;
    p.q[2] = t.q.v.z;
    p.q[3] = t.q.s;
    return p;
}

void* tag(std::uint32_t slot) { return reinterpret_cast<void*>(static_cast<std::uintptr_t>(slot) + 1); }
std::uint32_t slotOf(void* userData) {
    return static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(userData) - 1);
}

struct JointRec {
    b3JointId id = b3_nullJointId;
    std::uint32_t a = kWorld, b = kWorld;
    bool alive = false;
};

struct BodyRec {
    b3BodyId id = b3_nullBodyId;
    std::vector<std::uint32_t> joints;
    bool alive = false;
};

}  // namespace

struct World::Impl {
    b3WorldId world = b3_nullWorldId;
    b3BodyId ground = b3_nullBodyId;  // what a joint to the world itself is made to
    std::vector<BodyRec> bodies;
    std::vector<JointRec> joints;
    std::vector<std::uint32_t> freeBodies, freeJoints;
    std::vector<Moved> moved;
    std::vector<std::uint32_t> broken;
    int bodyCount = 0, jointCount = 0;
    double stepMs = 0;

    std::uint32_t add(const BodyDesc& d, b3BodyId& out) {
        b3BodyDef bd = b3DefaultBodyDef();
        bd.type = d.mass > 0.0f ? b3_dynamicBody : b3_staticBody;
        bd.position = b3Pos{d.pose.p[0], d.pose.p[1], d.pose.p[2]};
        bd.rotation = quat(d.pose.q);
        bd.linearVelocity = v3(d.velocity);
        bd.angularVelocity = v3(d.angular);
        std::uint32_t slot;
        if (!freeBodies.empty()) {
            slot = freeBodies.back();
            freeBodies.pop_back();
        } else {
            slot = static_cast<std::uint32_t>(bodies.size());
            bodies.emplace_back();
        }
        bd.userData = tag(slot);
        out = b3CreateBody(world, &bd);
        bodies[slot].id = out;
        bodies[slot].alive = true;
        bodies[slot].joints.clear();
        bodyCount++;
        return slot;
    }

    b3ShapeDef shape(const BodyDesc& d, float volume) const {
        b3ShapeDef sd = b3DefaultShapeDef();
        sd.density = d.mass > 0.0f && volume > 0.0f ? d.mass / volume : 0.0f;
        sd.baseMaterial.friction = d.friction;
        sd.baseMaterial.restitution = d.restitution;
        return sd;
    }

    void dropJoint(std::uint32_t j, bool destroy) {
        JointRec& r = joints[j];
        if (!r.alive) return;
        if (destroy && b3Joint_IsValid(r.id)) b3DestroyJoint(r.id, true);
        for (std::uint32_t body : {r.a, r.b}) {
            if (body == kWorld || body >= bodies.size()) continue;
            auto& list = bodies[body].joints;
            for (std::size_t i = 0; i < list.size(); i++) {
                if (list[i] == j) { list[i] = list.back(); list.pop_back(); break; }
            }
        }
        r.alive = false;
        r.id = b3_nullJointId;
        freeJoints.push_back(j);
        jointCount--;
    }
};

World::World() : impl_(std::make_unique<Impl>()) {
    b3WorldDef wd = b3DefaultWorldDef();
    wd.gravity = b3Vec3{0.0f, -9.81f, 0.0f};
    impl_->world = b3CreateWorld(&wd);
    b3BodyDef gd = b3DefaultBodyDef();
    impl_->ground = b3CreateBody(impl_->world, &gd);
}

World::~World() {
    if (b3World_IsValid(impl_->world)) b3DestroyWorld(impl_->world);
}

void World::setGravity(float x, float y, float z) { b3World_SetGravity(impl_->world, b3Vec3{x, y, z}); }

std::uint32_t World::addBox(const BodyDesc& d, const float half[3]) {
    b3BodyId id;
    const std::uint32_t slot = impl_->add(d, id);
    const b3ShapeDef sd = impl_->shape(d, 8.0f * half[0] * half[1] * half[2]);
    const b3BoxHull hull = b3MakeBoxHull(half[0], half[1], half[2]);
    b3CreateHullShape(id, &sd, &hull.base);
    return slot;
}

std::uint32_t World::addSphere(const BodyDesc& d, float radius) {
    b3BodyId id;
    const std::uint32_t slot = impl_->add(d, id);
    const b3ShapeDef sd = impl_->shape(d, 4.18879020f * radius * radius * radius);
    const b3Sphere s{b3Vec3{0, 0, 0}, radius};
    b3CreateSphereShape(id, &sd, &s);
    return slot;
}

bool World::hasBody(std::uint32_t body) const { return body < impl_->bodies.size() && impl_->bodies[body].alive; }

void World::removeBody(std::uint32_t body) {
    if (!hasBody(body)) return;
    BodyRec& r = impl_->bodies[body];
    // Box3D takes a body's joints with it; the records go first.
    const std::vector<std::uint32_t> on = r.joints;
    for (std::uint32_t j : on) impl_->dropJoint(j, false);
    b3DestroyBody(r.id);
    r.alive = false;
    r.id = b3_nullBodyId;
    r.joints.clear();
    impl_->freeBodies.push_back(body);
    impl_->bodyCount--;
}

std::uint32_t World::addJoint(const JointDesc& d) {
    const bool wa = d.a == kWorld, wb = d.b == kWorld;
    if ((!wa && !hasBody(d.a)) || (!wb && !hasBody(d.b)) || (wa && wb)) return kWorld;
    const b3BodyId ida = wa ? impl_->ground : impl_->bodies[d.a].id;
    const b3BodyId idb = wb ? impl_->ground : impl_->bodies[d.b].id;
    // One frame in the world, at the anchor, its z along the axis; each body holds it as it
    // sees it now.
    b3Vec3 axis = v3(d.axis);
    const float len = std::sqrt(axis.x * axis.x + axis.y * axis.y + axis.z * axis.z);
    axis = len > 1e-6f ? b3Vec3{axis.x / len, axis.y / len, axis.z / len} : b3Vec3{0, 0, 1};
    const b3Quat frame = b3ComputeQuatBetweenUnitVectors(b3Vec3{0, 0, 1}, axis);
    const b3Pos at{d.anchor[0], d.anchor[1], d.anchor[2]};
    b3Transform fa, fb;
    fa.p = b3Body_GetLocalPoint(ida, at);
    fa.q = b3InvMulQuat(b3Body_GetRotation(ida), frame);
    fb.p = b3Body_GetLocalPoint(idb, at);
    fb.q = b3InvMulQuat(b3Body_GetRotation(idb), frame);

    std::uint32_t slot;
    if (!impl_->freeJoints.empty()) {
        slot = impl_->freeJoints.back();
        impl_->freeJoints.pop_back();
    } else {
        slot = static_cast<std::uint32_t>(impl_->joints.size());
        impl_->joints.emplace_back();
    }
    const auto base = [&](b3JointDef& j) {
        j.bodyIdA = ida;
        j.bodyIdB = idb;
        j.localFrameA = fa;
        j.localFrameB = fb;
        j.collideConnected = d.collide;
        j.userData = tag(slot);
        if (d.breakForce > 0.0f) j.forceThreshold = d.breakForce;
        if (d.breakTorque > 0.0f) j.torqueThreshold = d.breakTorque;
    };
    b3JointId id = b3_nullJointId;
    switch (d.kind) {
        case JointKind::Weld: {
            b3WeldJointDef jd = b3DefaultWeldJointDef();
            base(jd.base);
            id = b3CreateWeldJoint(impl_->world, &jd);
            break;
        }
        case JointKind::Hinge: {
            b3RevoluteJointDef jd = b3DefaultRevoluteJointDef();
            base(jd.base);
            id = b3CreateRevoluteJoint(impl_->world, &jd);
            break;
        }
        case JointKind::Ball: {
            b3SphericalJointDef jd = b3DefaultSphericalJointDef();
            base(jd.base);
            id = b3CreateSphericalJoint(impl_->world, &jd);
            break;
        }
    }
    JointRec& r = impl_->joints[slot];
    r.id = id;
    r.a = d.a;
    r.b = d.b;
    r.alive = true;
    if (!wa) impl_->bodies[d.a].joints.push_back(slot);
    if (!wb) impl_->bodies[d.b].joints.push_back(slot);
    impl_->jointCount++;
    return slot;
}

bool World::hasJoint(std::uint32_t joint) const { return joint < impl_->joints.size() && impl_->joints[joint].alive; }

void World::removeJoint(std::uint32_t joint) {
    if (hasJoint(joint)) impl_->dropJoint(joint, true);
}

void World::jointLoad(std::uint32_t joint, float force[3], float torque[3]) const {
    b3Vec3 f{0, 0, 0}, t{0, 0, 0};
    if (hasJoint(joint)) {
        f = b3Joint_GetConstraintForce(impl_->joints[joint].id);
        t = b3Joint_GetConstraintTorque(impl_->joints[joint].id);
    }
    force[0] = f.x; force[1] = f.y; force[2] = f.z;
    torque[0] = t.x; torque[1] = t.y; torque[2] = t.z;
}

void World::step(float dt, int substeps) {
    const auto t0 = std::chrono::steady_clock::now();
    b3World_Step(impl_->world, dt, substeps < 1 ? 1 : substeps);
    impl_->moved.clear();
    const b3BodyEvents be = b3World_GetBodyEvents(impl_->world);
    impl_->moved.reserve(static_cast<std::size_t>(be.moveCount));
    for (int i = 0; i < be.moveCount; i++) {
        const b3BodyMoveEvent& e = be.moveEvents[i];
        if (!e.userData) continue;
        Moved m;
        m.body = slotOf(e.userData);
        m.pose = poseOf(e.transform);
        m.asleep = e.fellAsleep;
        impl_->moved.push_back(m);
    }
    // More went through these than they were made to take.
    impl_->broken.clear();
    const b3JointEvents je = b3World_GetJointEvents(impl_->world);
    for (int i = 0; i < je.count; i++) {
        if (!je.jointEvents[i].userData) continue;
        impl_->broken.push_back(slotOf(je.jointEvents[i].userData));
    }
    for (std::uint32_t j : impl_->broken) impl_->dropJoint(j, true);
    impl_->stepMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

Pose World::pose(std::uint32_t body) const {
    if (!hasBody(body)) return {};
    return poseOf(b3Body_GetTransform(impl_->bodies[body].id));
}

void World::setPose(std::uint32_t body, const Pose& pose) {
    if (!hasBody(body)) return;
    b3Body_SetTransform(impl_->bodies[body].id, b3Pos{pose.p[0], pose.p[1], pose.p[2]}, quat(pose.q));
}

void World::velocity(std::uint32_t body, float linear[3], float angular[3]) const {
    b3Vec3 v{0, 0, 0}, w{0, 0, 0};
    if (hasBody(body)) {
        v = b3Body_GetLinearVelocity(impl_->bodies[body].id);
        w = b3Body_GetAngularVelocity(impl_->bodies[body].id);
    }
    linear[0] = v.x; linear[1] = v.y; linear[2] = v.z;
    angular[0] = w.x; angular[1] = w.y; angular[2] = w.z;
}

void World::setVelocity(std::uint32_t body, const float linear[3], const float angular[3]) {
    if (!hasBody(body)) return;
    b3Body_SetLinearVelocity(impl_->bodies[body].id, v3(linear));
    b3Body_SetAngularVelocity(impl_->bodies[body].id, v3(angular));
}

void World::applyImpulse(std::uint32_t body, const float impulse[3], const float at[3]) {
    if (!hasBody(body)) return;
    b3Body_ApplyLinearImpulse(impl_->bodies[body].id, v3(impulse), b3Pos{at[0], at[1], at[2]}, true);
}

float World::mass(std::uint32_t body) const { return hasBody(body) ? b3Body_GetMass(impl_->bodies[body].id) : 0.0f; }

const std::vector<Moved>& World::moved() const { return impl_->moved; }
const std::vector<std::uint32_t>& World::broken() const { return impl_->broken; }

Counts World::counts() const {
    Counts c;
    c.bodies = impl_->bodyCount;
    c.joints = impl_->jointCount;
    c.awake = b3World_GetAwakeBodyCount(impl_->world);
    c.contacts = b3World_GetCounters(impl_->world).contactCount;
    c.stepMs = impl_->stepMs;
    return c;
}

}  // namespace bro::dynamics
