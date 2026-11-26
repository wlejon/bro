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
    float linearDamping = 0.0f;
    float angularDamping = 0.0f;
    float gravityScale = 1.0f;
    bool kinematic = false;    // moved by its velocity or moveTo, pushed by nothing
    bool bullet = false;       // swept against what moves, for the small and fast
    bool planar = false;       // held to the plane z = its z, turning about z only
    std::uint32_t layers = 1;            // the layers it is on
    std::uint32_t mask = 0xffffffffu;    // the layers it touches
    // The structure whose body this is (set by the structure): it is shown what meets the
    // body before the step solves it.
    std::uint32_t structure = kWorld;
};

// Two bodies that began to touch in a step.
struct Touch {
    std::uint32_t a = 0, b = 0;
};

// Two bodies that met faster than the world's hit speed.
struct Hit {
    std::uint32_t a = 0, b = 0;
    float point[3] = {0, 0, 0};
    float normal[3] = {0, 0, 0};  // from a to b
    float speed = 0;
};

struct RayHit {
    bool hit = false;
    std::uint32_t body = 0;
    float point[3] = {0, 0, 0};
    float normal[3] = {0, 0, 0};
    float distance = 0;
};

struct Bounds {
    std::uint32_t body = 0;
    float min[3] = {0, 0, 0};
    float max[3] = {0, 0, 0};
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

// One box of a body made of several: its centre, turn and half extents in the body's space,
// and its share of the mass.
struct Part {
    float c[3] = {0, 0, 0};
    float q[4] = {0, 0, 0, 1};
    float h[3] = {0.5f, 0.5f, 0.5f};
    float mass = 0;
};

// A body that is moving: where it is, how fast, how heavy.
struct Awake {
    std::uint32_t body = 0;
    float min[3] = {0, 0, 0};
    float max[3] = {0, 0, 0};
    float velocity[3] = {0, 0, 0};
    float mass = 0;
};

// What another body pressed on a body with over the last step.
struct Press {
    std::uint32_t other = 0;
    float point[3] = {0, 0, 0};
    float impulse[3] = {0, 0, 0};  // N s, on the body
};

class Structure;
struct StructureDesc;

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

    // The fastest a body moves, and the closing speed past which a meeting is a hit.
    void setMaxSpeed(float speed);
    void setHitSpeed(float speed);

    std::uint32_t addBox(const BodyDesc& d, const float half[3]);
    std::uint32_t addSphere(const BodyDesc& d, float radius);
    // Along the body's y.
    std::uint32_t addCapsule(const BodyDesc& d, float radius, float halfHeight);
    // The convex hull of the points (body space); kWorld when they make none.
    std::uint32_t addHull(const BodyDesc& d, const float* xyz, int points);
    // Triangles (body space), fixed in place whatever the mass; kWorld when they make none.
    std::uint32_t addMesh(const BodyDesc& d, const float* xyz, int vertices, const std::int32_t* indices,
                          int triangles);
    // One body of several boxes, each with its own mass (d.mass is not read); fixed in
    // place when no part has any.
    std::uint32_t addBoxes(const BodyDesc& d, const Part* parts, int count);
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
    // A kinematic body, given the velocity that has it at the pose a step of dt from now.
    void moveTo(std::uint32_t body, const Pose& pose, float dt);
    void wake(std::uint32_t body);
    // A body's centre of mass, and the velocity of the point of it at `at` (world).
    void center(std::uint32_t body, float out[3]) const;
    void velocityAt(std::uint32_t body, const float at[3], float out[3]) const;
    // How many contacts a body has, and the box about it.
    int touching(std::uint32_t body) const;
    void bounds(std::uint32_t body, float min[3], float max[3]) const;
    // What the bodies touching a body pressed on it with in the last step, a row a contact
    // point.
    void pressing(std::uint32_t body, std::vector<Press>& out) const;
    void gravity(float out[3]) const;
    // The bodies that are not at rest: those the last step moved and left awake, and those
    // made since.
    void awake(std::vector<Awake>& out) const;

    // A structure standing in this world (dynamics/structure.h), stepped with it. Named by
    // a small index, reused once removed.
    std::uint32_t addStructure(const StructureDesc& d);
    void removeStructure(std::uint32_t structure);
    Structure* structure(std::uint32_t structure) const;

    // The bodies the last step moved, the joints it broke, the pairs that began to touch in
    // it and the pairs that hit.
    const std::vector<Moved>& moved() const;
    const std::vector<std::uint32_t>& broken() const;
    const std::vector<Touch>& touched() const;
    const std::vector<Hit>& hits() const;

    // The nearest body on the layers of mask along a ray; dir need not be unit.
    RayHit raycast(const float origin[3], const float dir[3], float maxDistance, std::uint32_t mask) const;

    // The fixed bodies on the layers of mask: their boxes, hulls and meshes as triangles
    // in the world (appended; indices into xyz / 3), and each one's bounds.
    void fixedTriangles(std::uint32_t mask, std::vector<float>& xyz, std::vector<std::uint32_t>& indices) const;
    std::vector<Bounds> fixedBounds(std::uint32_t mask) const;

    Counts counts() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace bro::dynamics
