// dynamics::World: rigid bodies, the joints between them and what goes through those joints,
// over Box3D (wlejon/box3d, the fork of Erin Catto's). Box3D's headers stay behind this one.
//
// Bodies and joints are named by small indices that are reused once removed. A joint made with
// a force or torque it can take is broken (removed) by the step in which more went through it,
// and named in broken() until the next step.
#pragma once

#include <cstdint>
#include <memory>
#include <vector>

namespace bro::dynamics {

inline constexpr std::uint32_t kWorld = 0xffffffffu;

enum class JointKind : std::uint8_t {
    Weld,   // nothing moves
    Hinge,  // turns about the axis
    Ball,   // turns any way about the anchor
};

struct Pose {
    float p[3] = {0, 0, 0};
    float q[4] = {0, 0, 0, 1};  // x y z w
};

struct BodyDesc {
    Pose pose;
    float mass = 1.0f;         // 0: fixed in place
    float friction = 0.6f;
    float restitution = 0.0f;
    float velocity[3] = {0, 0, 0};
    float angular[3] = {0, 0, 0};
};

struct JointDesc {
    JointKind kind = JointKind::Weld;
    std::uint32_t a = kWorld;
    std::uint32_t b = kWorld;         // kWorld: the world itself
    float anchor[3] = {0, 0, 0};      // world, at the bodies' poses now
    float axis[3] = {0, 1, 0};        // world, a hinge's
    float breakForce = 0;             // N; 0: any
    float breakTorque = 0;            // N m; 0: any
    bool collide = false;             // whether the two bodies touch each other
};

struct Moved {
    std::uint32_t body = 0;
    Pose pose;
    bool asleep = false;  // it came to rest in this step
};

struct Counts {
    int bodies = 0;
    int joints = 0;
    int awake = 0;
    int contacts = 0;
    double stepMs = 0;  // the last step's wall time
};

class World {
public:
    World();
    ~World();
    World(const World&) = delete;
    World& operator=(const World&) = delete;

    void setGravity(float x, float y, float z);

    std::uint32_t addBox(const BodyDesc& d, const float half[3]);
    std::uint32_t addSphere(const BodyDesc& d, float radius);
    void removeBody(std::uint32_t body);  // and the joints on it
    bool hasBody(std::uint32_t body) const;

    std::uint32_t addJoint(const JointDesc& d);
    void removeJoint(std::uint32_t joint);
    bool hasJoint(std::uint32_t joint) const;
    // What goes through a joint: on body b, world axes.
    void jointLoad(std::uint32_t joint, float force[3], float torque[3]) const;

    void step(float dt, int substeps);

    Pose pose(std::uint32_t body) const;
    void setPose(std::uint32_t body, const Pose& pose);
    void velocity(std::uint32_t body, float linear[3], float angular[3]) const;
    void setVelocity(std::uint32_t body, const float linear[3], const float angular[3]);
    void applyImpulse(std::uint32_t body, const float impulse[3], const float at[3]);
    float mass(std::uint32_t body) const;

    // The bodies the last step moved, and the joints it broke.
    const std::vector<Moved>& moved() const;
    const std::vector<std::uint32_t>& broken() const;

    Counts counts() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace bro::dynamics
