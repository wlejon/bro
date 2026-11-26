#include "dynamics/world.h"

#include "dynamics/structure.h"

#include "box3d/box3d.h"
#include "box3d/math_functions.h"

#include <algorithm>
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
    JointKind kind = JointKind::Weld;
    std::uint32_t a = kWorld, b = kWorld;
    bool alive = false;
};

static b3Vec3 normalizeOr(b3Vec3 v, b3Vec3 fallback) {
    const float len = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
    return len > 1e-6f ? b3Vec3{v.x / len, v.y / len, v.z / len} : fallback;
}

static b3Quat quatFromBasis(b3Vec3 x, b3Vec3 y, b3Vec3 z) {
    const float trace = x.x + y.y + z.z;
    b3Quat q;
    if (trace > 0.0f) {
        const float s = std::sqrt(trace + 1.0f) * 2.0f;
        q.s = 0.25f * s;
        q.v.x = (y.z - z.y) / s;
        q.v.y = (z.x - x.z) / s;
        q.v.z = (x.y - y.x) / s;
    } else if (x.x > y.y && x.x > z.z) {
        const float s = std::sqrt(1.0f + x.x - y.y - z.z) * 2.0f;
        q.s = (y.z - z.y) / s;
        q.v.x = 0.25f * s;
        q.v.y = (y.x + x.y) / s;
        q.v.z = (z.x + x.z) / s;
    } else if (y.y > z.z) {
        const float s = std::sqrt(1.0f + y.y - x.x - z.z) * 2.0f;
        q.s = (z.x - x.z) / s;
        q.v.x = (y.x + x.y) / s;
        q.v.y = 0.25f * s;
        q.v.z = (z.y + y.z) / s;
    } else {
        const float s = std::sqrt(1.0f + z.z - x.x - y.y) * 2.0f;
        q.s = (x.y - y.x) / s;
        q.v.x = (z.x + x.z) / s;
        q.v.y = (z.y + y.z) / s;
        q.v.z = 0.25f * s;
    }
    return b3NormalizeQuat(q);
}

struct BodyRec {
    b3BodyId id = b3_nullBodyId;
    std::vector<std::uint32_t> joints;
    b3MeshData* mesh = nullptr;  // a mesh body's triangles, which its shape reads in place
    b3HeightFieldData* heightField = nullptr;
    std::uint32_t layers = 1;
    std::uint32_t structure = kWorld;  // the structure it is a body of
    bool fixed = false;
    bool alive = false;
};

void put(float* out, b3Vec3 v) {
    out[0] = v.x;
    out[1] = v.y;
    out[2] = v.z;
}

void putPos(float* out, const b3Pos& p) {
    out[0] = static_cast<float>(p.x);
    out[1] = static_cast<float>(p.y);
    out[2] = static_cast<float>(p.z);
}

}  // namespace

struct World::Impl {
    b3WorldId world = b3_nullWorldId;
    b3BodyId ground = b3_nullBodyId;  // what a joint to the world itself is made to
    std::vector<BodyRec> bodies;
    std::vector<JointRec> joints;
    std::vector<std::uint32_t> freeBodies, freeJoints;
    std::vector<std::uint32_t> fresh;  // the moving bodies made since the last step
    std::vector<std::unique_ptr<Structure>> structures;
    std::vector<Moved> moved;
    std::vector<std::uint32_t> broken;
    std::vector<Touch> touched;
    std::vector<Hit> hits;
    std::vector<Awake> awake;
    mutable std::vector<b3ContactData> contactScratch;
    int bodyCount = 0, jointCount = 0;
    double stepMs = 0;

    // A contact of a structure's body, before it is solved: the structure says whether
    // the other is stopped by it.
    static bool preSolve(b3ShapeId shapeA, b3ShapeId shapeB, b3Pos point, b3Vec3 normal, void* context) {
        Impl& self = *static_cast<Impl*>(context);
        std::uint32_t a, b;
        if (!self.bodyOf(shapeA, a) || !self.bodyOf(shapeB, b)) return true;
        const float p[3] = {static_cast<float>(point.x), static_cast<float>(point.y), static_cast<float>(point.z)};
        const float n[3] = {normal.x, normal.y, normal.z};
        bool stops = true;
        for (int side = 0; side < 2; side++) {
            const std::uint32_t mine = side ? b : a, other = side ? a : b;
            const std::uint32_t s = self.bodies[mine].structure;
            if (s < self.structures.size() && self.structures[s]) {
                stops = self.structures[s]->meet(mine, other, p, n) && stops;
            }
        }
        return stops;
    }

    // The body a shape is on, by its number; false for one that is not ours.
    bool bodyOf(b3ShapeId shape, std::uint32_t& out) const {
        void* u = b3Body_GetUserData(b3Shape_GetBody(shape));
        if (!u) return false;
        out = slotOf(u);
        return true;
    }

    std::uint32_t add(const BodyDesc& d, b3BodyId& out, bool fixed = false) {
        b3BodyDef bd = b3DefaultBodyDef();
        fixed = fixed || (!d.kinematic && !(d.mass > 0.0f));
        bd.type = fixed ? b3_staticBody : d.kinematic ? b3_kinematicBody : b3_dynamicBody;
        bd.position = b3Pos{d.pose.p[0], d.pose.p[1], d.pose.p[2]};
        bd.rotation = quat(d.pose.q);
        bd.linearVelocity = v3(d.velocity);
        bd.angularVelocity = v3(d.angular);
        bd.linearDamping = d.linearDamping;
        bd.angularDamping = d.angularDamping;
        bd.gravityScale = d.gravityScale;
        bd.isBullet = d.bullet;
        if (d.sleepThreshold > 0.0f) bd.sleepThreshold = d.sleepThreshold;
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
        bodies[slot].fixed = fixed;
        bodies[slot].layers = d.layers;
        bodies[slot].structure = d.structure;
        bodies[slot].mesh = nullptr;
        bodies[slot].heightField = nullptr;
        bodies[slot].joints.clear();
        bodyCount++;
        if (!fixed) fresh.push_back(slot);
        if (d.planar && !fixed && !d.kinematic) {
            b3MotionLockJointDef jd = b3DefaultMotionLockJointDef();
            jd.base.bodyIdA = ground;
            jd.base.bodyIdB = out;
            jd.base.localFrameA.p = v3(d.pose.p);
            jd.base.localFrameA.q = b3Quat_identity;
            jd.base.localFrameB.p = b3Vec3{0, 0, 0};
            jd.base.localFrameB.q = b3InvMulQuat(bd.rotation, b3Quat_identity);
            jd.lockLinearZ = true;
            jd.lockAngularXY = true;
            b3CreateMotionLockJoint(world, &jd);
        }
        return slot;
    }

    b3ShapeDef shape(const BodyDesc& d, float volume) const {
        b3ShapeDef sd = b3DefaultShapeDef();
        sd.density = d.mass > 0.0f && volume > 0.0f ? d.mass / volume : 0.0f;
        sd.baseMaterial.friction = d.friction;
        sd.baseMaterial.restitution = d.restitution;
        sd.filter.categoryBits = d.layers;
        sd.filter.maskBits = d.mask;
        sd.enableContactEvents = d.events;
        sd.enableHitEvents = d.events;
        sd.enablePreSolveEvents = d.structure != kWorld;
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
    b3World_SetPreSolveCallback(impl_->world, &Impl::preSolve, impl_.get());
}

World::~World() {
    impl_->structures.clear();
    if (b3World_IsValid(impl_->world)) b3DestroyWorld(impl_->world);
    for (BodyRec& r : impl_->bodies) {
        if (r.mesh) b3DestroyMesh(r.mesh);
        if (r.heightField) b3DestroyHeightField(r.heightField);
    }
}

void World::setGravity(float x, float y, float z) { b3World_SetGravity(impl_->world, b3Vec3{x, y, z}); }
void World::setMaxSpeed(float speed) { b3World_SetMaximumLinearSpeed(impl_->world, speed); }
void World::setHitSpeed(float speed) { b3World_SetHitEventThreshold(impl_->world, speed); }
void World::setWorkers(int workers) { b3World_SetWorkerCount(impl_->world, std::clamp(workers, 1, 16)); }

std::uint32_t World::addCapsule(const BodyDesc& d, float radius, float halfHeight) {
    b3BodyId id;
    const std::uint32_t slot = impl_->add(d, id);
    const float volume = 3.14159265f * radius * radius * (2.0f * halfHeight + 1.33333333f * radius);
    const b3ShapeDef sd = impl_->shape(d, volume);
    const b3Capsule c{b3Vec3{0, -halfHeight, 0}, b3Vec3{0, halfHeight, 0}, radius};
    b3CreateCapsuleShape(id, &sd, &c);
    return slot;
}

std::uint32_t World::addHull(const BodyDesc& d, const float* xyz, int points) {
    if (points < 4) return kWorld;
    std::vector<b3Vec3> pts(static_cast<std::size_t>(points));
    for (int i = 0; i < points; i++) pts[static_cast<std::size_t>(i)] = v3(xyz + 3 * i);
    b3HullData* hull = b3CreateHull(pts.data(), points, points);
    if (!hull) return kWorld;
    b3BodyId id;
    const std::uint32_t slot = impl_->add(d, id);
    const b3ShapeDef sd = impl_->shape(d, hull->volume);
    b3CreateHullShape(id, &sd, hull);
    b3DestroyHull(hull);
    return slot;
}

std::uint32_t World::addMesh(const BodyDesc& d, const float* xyz, int vertices, const std::int32_t* indices,
                             int triangles) {
    if (vertices < 3 || triangles < 1) return kWorld;
    std::vector<b3Vec3> verts(static_cast<std::size_t>(vertices));
    for (int i = 0; i < vertices; i++) verts[static_cast<std::size_t>(i)] = v3(xyz + 3 * i);
    std::vector<std::int32_t> idx(indices, indices + 3 * static_cast<std::size_t>(triangles));
    for (std::int32_t i : idx) {
        if (i < 0 || i >= vertices) return kWorld;
    }
    b3MeshDef md = {};
    md.vertices = verts.data();
    md.indices = idx.data();
    md.vertexCount = vertices;
    md.triangleCount = triangles;
    md.identifyEdges = true;
    b3MeshData* mesh = b3CreateMesh(&md, nullptr, 0);
    if (!mesh) return kWorld;
    b3BodyId id;
    const std::uint32_t slot = impl_->add(d, id, true);
    const b3ShapeDef sd = impl_->shape(d, 0.0f);
    b3CreateMeshShape(id, &sd, mesh, b3Vec3{1, 1, 1});
    impl_->bodies[slot].mesh = mesh;
    return slot;
}

std::uint32_t World::addHeightField(const BodyDesc& d, const float* heights, int countX, int countZ,
                                    const float scale[3], float minH, float maxH) {
    if (!heights || countX < 2 || countZ < 2 || !(scale[0] > 0.0f) || !(scale[1] > 0.0f) || !(scale[2] > 0.0f) ||
        maxH < minH) {
        return kWorld;
    }
    const std::size_t n = static_cast<std::size_t>(countX) * static_cast<std::size_t>(countZ);
    std::vector<float> copy(heights, heights + n);
    b3HeightFieldDef hfd = {};
    hfd.heights = copy.data();
    hfd.materialIndices = nullptr;
    hfd.scale = v3(scale);
    hfd.countX = countX;
    hfd.countZ = countZ;
    hfd.globalMinimumHeight = minH;
    hfd.globalMaximumHeight = maxH;
    hfd.clockwiseWinding = false;
    b3HeightFieldData* hf = b3CreateHeightField(&hfd);
    if (!hf) return kWorld;
    b3BodyId id;
    const std::uint32_t slot = impl_->add(d, id, true);
    BodyDesc sdDesc = d;
    sdDesc.events = false;
    const b3ShapeDef sd = impl_->shape(sdDesc, 0.0f);
    b3CreateHeightFieldShape(id, &sd, hf);
    impl_->bodies[slot].heightField = hf;
    return slot;
}

std::uint32_t World::addBox(const BodyDesc& d, const float half[3]) {
    b3BodyId id;
    const std::uint32_t slot = impl_->add(d, id);
    const b3ShapeDef sd = impl_->shape(d, 8.0f * half[0] * half[1] * half[2]);
    const b3BoxHull hull = b3MakeBoxHull(half[0], half[1], half[2]);
    b3CreateHullShape(id, &sd, &hull.base);
    return slot;
}

std::uint32_t World::addBoxes(const BodyDesc& d, const Part* parts, int count) {
    if (count < 1) return kWorld;
    bool massless = true;
    for (int i = 0; i < count; i++) massless = massless && !(parts[i].mass > 0.0f);
    BodyDesc whole = d;
    whole.mass = massless ? 0.0f : 1.0f;
    b3BodyId id;
    const std::uint32_t slot = impl_->add(whole, id);
    b3ShapeDef sd = impl_->shape(d, 0.0f);
    sd.updateBodyMass = false;
    for (int i = 0; i < count; i++) {
        const Part& p = parts[i];
        const float volume = 8.0f * p.h[0] * p.h[1] * p.h[2];
        sd.density = p.mass > 0.0f && volume > 0.0f ? p.mass / volume : 0.0f;
        b3Transform at;
        at.p = v3(p.c);
        at.q = quat(p.q);
        const b3BoxHull hull = b3MakeTransformedBoxHull(p.h[0], p.h[1], p.h[2], at);
        b3CreateHullShape(id, &sd, &hull.base);
    }
    b3Body_ApplyMassFromShapes(id);
    return slot;
}

void World::bounds(std::uint32_t body, float min[3], float max[3]) const {
    min[0] = min[1] = min[2] = max[0] = max[1] = max[2] = 0.0f;
    if (!hasBody(body)) return;
    const b3AABB box = b3Body_ComputeAABB(impl_->bodies[body].id);
    put(min, box.lowerBound);
    put(max, box.upperBound);
}

void World::pressing(std::uint32_t body, std::vector<Press>& out) const {
    out.clear();
    if (!hasBody(body)) return;
    const b3BodyId id = impl_->bodies[body].id;
    const int capacity = b3Body_GetContactCapacity(id);
    if (capacity < 1) return;
    if (static_cast<int>(impl_->contactScratch.size()) < capacity) {
        impl_->contactScratch.resize(static_cast<std::size_t>(capacity));
    }
    const int count = b3Body_GetContactData(id, impl_->contactScratch.data(), capacity);
    for (int i = 0; i < count; i++) {
        const b3ContactData& c = impl_->contactScratch[static_cast<std::size_t>(i)];
        std::uint32_t a, b;
        if (!impl_->bodyOf(c.shapeIdA, a) || !impl_->bodyOf(c.shapeIdB, b)) continue;
        const bool first = a == body;
        const b3Pos origin = b3Body_GetWorldCenter(b3Shape_GetBody(c.shapeIdA));
        for (int m = 0; m < c.manifoldCount; m++) {
            const b3Manifold& mf = c.manifolds[m];
            for (int k = 0; k < mf.pointCount; k++) {
                const float imp = mf.points[k].totalNormalImpulse;
                if (!(imp > 0.0f)) continue;
                Press p;
                p.other = first ? b : a;
                putPos(p.point, b3OffsetPos(origin, mf.points[k].anchorA));
                // The normal runs from the first to the second, which is how the second is pushed.
                const float s = first ? -imp : imp;
                p.impulse[0] = mf.normal.x * s;
                p.impulse[1] = mf.normal.y * s;
                p.impulse[2] = mf.normal.z * s;
                out.push_back(p);
            }
        }
    }
}

void World::gravity(float out[3]) const { put(out, b3World_GetGravity(impl_->world)); }

void World::center(std::uint32_t body, float out[3]) const {
    out[0] = out[1] = out[2] = 0.0f;
    if (hasBody(body)) putPos(out, b3Body_GetWorldCenter(impl_->bodies[body].id));
}

void World::velocityAt(std::uint32_t body, const float at[3], float out[3]) const {
    out[0] = out[1] = out[2] = 0.0f;
    if (hasBody(body)) put(out, b3Body_GetWorldPointVelocity(impl_->bodies[body].id, b3Pos{at[0], at[1], at[2]}));
}

int World::touching(std::uint32_t body) const {
    return hasBody(body) ? b3Body_GetContactCapacity(impl_->bodies[body].id) : 0;
}

bool World::isAwake(std::uint32_t body) const {
    return hasBody(body) && b3Body_IsAwake(impl_->bodies[body].id);
}

void World::awake(std::vector<Awake>& out) const {
    out.clear();
    const auto take = [&](std::uint32_t body) {
        if (!hasBody(body) || impl_->bodies[body].fixed) return;
        const b3BodyId id = impl_->bodies[body].id;
        if (b3Body_GetType(id) == b3_staticBody || !b3Body_IsAwake(id)) return;
        Awake a;
        a.body = body;
        const b3AABB box = b3Body_ComputeAABB(id);
        put(a.min, box.lowerBound);
        put(a.max, box.upperBound);
        put(a.velocity, b3Body_GetLinearVelocity(id));
        a.mass = b3Body_GetMass(id);
        out.push_back(a);
    };
    for (const Moved& m : impl_->moved) {
        if (!m.asleep) take(m.body);
    }
    for (std::uint32_t body : impl_->fresh) take(body);
}

std::uint32_t World::addStructure(const StructureDesc& d) {
    std::uint32_t slot = 0;
    while (slot < impl_->structures.size() && impl_->structures[slot]) slot++;
    if (slot == impl_->structures.size()) impl_->structures.emplace_back();
    impl_->structures[slot] = std::make_unique<Structure>(*this, slot, d);
    return slot;
}

void World::removeStructure(std::uint32_t structure) {
    if (structure >= impl_->structures.size() || !impl_->structures[structure]) return;
    impl_->structures[structure]->clear();
    impl_->structures[structure].reset();
}

Structure* World::structure(std::uint32_t structure) const {
    return structure < impl_->structures.size() ? impl_->structures[structure].get() : nullptr;
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
    if (r.mesh) b3DestroyMesh(r.mesh);
    if (r.heightField) b3DestroyHeightField(r.heightField);
    r.mesh = nullptr;
    r.heightField = nullptr;
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
    // Joint reference orientation depends on Box3D convention:
    //   Revolute (Hinge): rotates about local Z
    //   Prismatic (Slider): slides along local X
    //   Wheel: suspends/steers along frameA local X, spins about frameB local Z
    const b3Vec3 axis = normalizeOr(v3(d.axis), b3Vec3{0, 1, 0});
    b3Quat frame = b3Quat_identity;
    if (d.kind == JointKind::Slider) {
        frame = b3ComputeQuatBetweenUnitVectors(b3Vec3{1, 0, 0}, axis);
    } else if (d.kind == JointKind::Wheel) {
        b3Vec3 spin = d.hasAxisB ? v3(d.axisB) : b3Vec3{1, 0, 0};
        // Orthogonalize spin axis against suspension axis.
        const float proj = spin.x * axis.x + spin.y * axis.y + spin.z * axis.z;
        spin = b3Vec3{spin.x - proj * axis.x, spin.y - proj * axis.y, spin.z - proj * axis.z};
        const float slen = std::sqrt(spin.x * spin.x + spin.y * spin.y + spin.z * spin.z);
        if (slen < 1e-4f) {
            const b3Vec3 alt = std::fabs(axis.x) < 0.8f ? b3Vec3{1, 0, 0} : b3Vec3{0, 0, 1};
            spin = b3Cross(axis, alt);
        }
        const b3Vec3 cz = normalizeOr(spin, b3Vec3{0, 0, 1});
        const b3Vec3 cy = normalizeOr(b3Cross(cz, axis), b3Vec3{0, 1, 0});
        frame = quatFromBasis(axis, cy, cz);
    } else {
        frame = b3ComputeQuatBetweenUnitVectors(b3Vec3{0, 0, 1}, axis);
    }
    const b3Pos atA{d.anchor[0], d.anchor[1], d.anchor[2]};
    const b3Pos atB = d.hasAnchorB ? b3Pos{d.anchorB[0], d.anchorB[1], d.anchorB[2]} : atA;
    b3Transform fa, fb;
    fa.p = b3Body_GetLocalPoint(ida, atA);
    fa.q = b3InvMulQuat(b3Body_GetRotation(ida), frame);
    fb.p = b3Body_GetLocalPoint(idb, atB);
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
            jd.enableLimit = d.enableLimit;
            jd.lowerAngle = d.lower;
            jd.upperAngle = d.upper;
            jd.enableMotor = d.enableMotor;
            jd.motorSpeed = d.motorSpeed;
            jd.maxMotorTorque = std::max(0.0f, d.maxMotorEffort);
            jd.enableSpring = d.enableSpring;
            jd.targetAngle = d.target;
            jd.hertz = std::max(0.0f, d.hertz);
            jd.dampingRatio = std::max(0.0f, d.damping);
            jd.maxSpringTorque = std::max(0.0f, d.maxSpringEffort);
            id = b3CreateRevoluteJoint(impl_->world, &jd);
            break;
        }
        case JointKind::Ball: {
            b3SphericalJointDef jd = b3DefaultSphericalJointDef();
            base(jd.base);
            if (d.enableLimit) {
                jd.enableConeLimit = true;
                jd.coneAngle = std::clamp(d.upper, 0.0f, 3.10f);
            }
            jd.enableMotor = d.enableMotor;
            jd.motorVelocity = b3Vec3{0, 0, d.motorSpeed};
            jd.maxMotorTorque = std::max(0.0f, d.maxMotorEffort);
            jd.enableSpring = d.enableSpring;
            jd.hertz = std::max(0.0f, d.hertz);
            jd.dampingRatio = std::max(0.0f, d.damping);
            id = b3CreateSphericalJoint(impl_->world, &jd);
            break;
        }
        case JointKind::Slider: {
            b3PrismaticJointDef jd = b3DefaultPrismaticJointDef();
            base(jd.base);
            jd.enableLimit = d.enableLimit;
            jd.lowerTranslation = std::min(d.lower, d.upper);
            jd.upperTranslation = std::max(d.lower, d.upper);
            jd.enableMotor = d.enableMotor;
            jd.motorSpeed = d.motorSpeed;
            jd.maxMotorForce = std::max(0.0f, d.maxMotorEffort);
            jd.enableSpring = d.enableSpring;
            jd.targetTranslation = d.target;
            jd.hertz = std::max(0.0f, d.hertz);
            jd.dampingRatio = std::max(0.0f, d.damping);
            jd.maxSpringForce = std::max(0.0f, d.maxSpringEffort);
            id = b3CreatePrismaticJoint(impl_->world, &jd);
            break;
        }
        case JointKind::Wheel: {
            b3WheelJointDef jd = b3DefaultWheelJointDef();
            base(jd.base);
            jd.enableSuspensionSpring = d.enableSpring || !(d.hertz == 0.0f && d.enableLimit);
            jd.suspensionHertz = d.hertz > 0.0f ? d.hertz : 6.0f;
            jd.suspensionDampingRatio = d.damping > 0.0f ? d.damping : 0.8f;
            jd.enableSuspensionLimit = d.enableLimit;
            jd.lowerSuspensionLimit = std::min(d.lower, d.upper);
            jd.upperSuspensionLimit = std::max(d.lower, d.upper);
            jd.enableSpinMotor = d.enableMotor;
            jd.spinSpeed = d.motorSpeed;
            jd.maxSpinTorque = std::max(0.0f, d.maxMotorEffort);
            jd.enableSteering = false;
            id = b3CreateWheelJoint(impl_->world, &jd);
            break;
        }
        case JointKind::Distance: {
            b3DistanceJointDef jd = b3DefaultDistanceJointDef();
            base(jd.base);
            const float dx = static_cast<float>(atB.x - atA.x);
            const float dy = static_cast<float>(atB.y - atA.y);
            const float dz = static_cast<float>(atB.z - atA.z);
            const float initialLen = std::sqrt(dx * dx + dy * dy + dz * dz);
            jd.length = std::max(0.01f, d.target > 0.0f ? d.target : initialLen);
            jd.enableLimit = d.enableLimit;
            jd.minLength = std::max(0.01f, std::min(d.lower, d.upper));
            jd.maxLength = std::max(jd.minLength, std::max(d.lower, d.upper));
            jd.enableSpring = d.enableSpring;
            jd.hertz = std::max(0.0f, d.hertz);
            jd.dampingRatio = std::max(0.0f, d.damping);
            if (d.maxSpringEffort > 0.0f) {
                jd.lowerSpringForce = -d.maxSpringEffort;
                jd.upperSpringForce = d.maxSpringEffort;
            }
            jd.enableMotor = d.enableMotor;
            jd.motorSpeed = d.motorSpeed;
            jd.maxMotorForce = std::max(0.0f, d.maxMotorEffort);
            id = b3CreateDistanceJoint(impl_->world, &jd);
            break;
        }
        case JointKind::Filter: {
            b3FilterJointDef jd = b3DefaultFilterJointDef();
            base(jd.base);
            id = b3CreateFilterJoint(impl_->world, &jd);
            break;
        }
    }
    JointRec& r = impl_->joints[slot];
    r.id = id;
    r.kind = d.kind;
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

void World::setJointMotor(std::uint32_t joint, float speed, float maxEffort, bool enabled) {
    if (!hasJoint(joint)) return;
    const JointRec& r = impl_->joints[joint];
    const float effort = std::max(0.0f, maxEffort);
    bool changed = false;
    switch (r.kind) {
        case JointKind::Hinge:
            changed = b3RevoluteJoint_IsMotorEnabled(r.id) != enabled ||
                      b3RevoluteJoint_GetMotorSpeed(r.id) != speed ||
                      b3RevoluteJoint_GetMaxMotorTorque(r.id) != effort;
            b3RevoluteJoint_EnableMotor(r.id, enabled);
            b3RevoluteJoint_SetMotorSpeed(r.id, speed);
            b3RevoluteJoint_SetMaxMotorTorque(r.id, effort);
            break;
        case JointKind::Slider:
            changed = b3PrismaticJoint_IsMotorEnabled(r.id) != enabled ||
                      b3PrismaticJoint_GetMotorSpeed(r.id) != speed ||
                      b3PrismaticJoint_GetMaxMotorForce(r.id) != effort;
            b3PrismaticJoint_EnableMotor(r.id, enabled);
            b3PrismaticJoint_SetMotorSpeed(r.id, speed);
            b3PrismaticJoint_SetMaxMotorForce(r.id, effort);
            break;
        case JointKind::Wheel:
            changed = b3WheelJoint_IsSpinMotorEnabled(r.id) != enabled ||
                      b3WheelJoint_GetSpinMotorSpeed(r.id) != speed ||
                      b3WheelJoint_GetMaxSpinTorque(r.id) != effort;
            b3WheelJoint_EnableSpinMotor(r.id, enabled);
            b3WheelJoint_SetSpinMotorSpeed(r.id, speed);
            b3WheelJoint_SetMaxSpinTorque(r.id, effort);
            break;
        case JointKind::Distance:
            changed = b3DistanceJoint_IsMotorEnabled(r.id) != enabled ||
                      b3DistanceJoint_GetMotorSpeed(r.id) != speed ||
                      b3DistanceJoint_GetMaxMotorForce(r.id) != effort;
            b3DistanceJoint_EnableMotor(r.id, enabled);
            b3DistanceJoint_SetMotorSpeed(r.id, speed);
            b3DistanceJoint_SetMaxMotorForce(r.id, effort);
            break;
        default: break;
    }
    if (changed) { wake(r.a); wake(r.b); }
}

void World::setJointTarget(std::uint32_t joint, float target, float hertz, float damping, float maxEffort, bool enabled) {
    if (!hasJoint(joint)) return;
    const JointRec& r = impl_->joints[joint];
    const float hz = std::max(0.0f, hertz);
    const float dr = std::max(0.0f, damping);
    const float eff = std::max(0.0f, maxEffort);
    bool changed = false;
    switch (r.kind) {
        case JointKind::Hinge: {
            const float tgt = std::clamp(target, -3.14159f, 3.14159f);
            changed = b3RevoluteJoint_IsSpringEnabled(r.id) != enabled ||
                      b3RevoluteJoint_GetTargetAngle(r.id) != tgt ||
                      b3RevoluteJoint_GetSpringHertz(r.id) != hz ||
                      b3RevoluteJoint_GetSpringDampingRatio(r.id) != dr ||
                      b3RevoluteJoint_GetMaxSpringTorque(r.id) != eff;
            b3RevoluteJoint_EnableSpring(r.id, enabled);
            b3RevoluteJoint_SetTargetAngle(r.id, tgt);
            b3RevoluteJoint_SetSpringHertz(r.id, hz);
            b3RevoluteJoint_SetSpringDampingRatio(r.id, dr);
            b3RevoluteJoint_SetMaxSpringTorque(r.id, eff);
            break;
        }
        case JointKind::Slider:
            changed = b3PrismaticJoint_IsSpringEnabled(r.id) != enabled ||
                      b3PrismaticJoint_GetTargetTranslation(r.id) != target ||
                      b3PrismaticJoint_GetSpringHertz(r.id) != hz ||
                      b3PrismaticJoint_GetSpringDampingRatio(r.id) != dr ||
                      b3PrismaticJoint_GetMaxSpringForce(r.id) != eff;
            b3PrismaticJoint_EnableSpring(r.id, enabled);
            b3PrismaticJoint_SetTargetTranslation(r.id, target);
            b3PrismaticJoint_SetSpringHertz(r.id, hz);
            b3PrismaticJoint_SetSpringDampingRatio(r.id, dr);
            b3PrismaticJoint_SetMaxSpringForce(r.id, eff);
            break;
        case JointKind::Wheel: {
            const float shz = hz > 0.0f ? hz : 8.0f;
            changed = b3WheelJoint_IsSteeringEnabled(r.id) != enabled ||
                      b3WheelJoint_GetTargetSteeringAngle(r.id) != target ||
                      b3WheelJoint_GetSteeringHertz(r.id) != shz ||
                      b3WheelJoint_GetSteeringDampingRatio(r.id) != dr;
            b3WheelJoint_EnableSteering(r.id, enabled);
            b3WheelJoint_SetTargetSteeringAngle(r.id, target);
            b3WheelJoint_SetSteeringHertz(r.id, shz);
            b3WheelJoint_SetSteeringDampingRatio(r.id, dr);
            if (eff > 0.0f) b3WheelJoint_SetMaxSteeringTorque(r.id, eff);
            break;
        }
        case JointKind::Distance: {
            const float len = std::max(0.01f, target);
            changed = b3DistanceJoint_IsSpringEnabled(r.id) != enabled ||
                      b3DistanceJoint_GetLength(r.id) != len ||
                      b3DistanceJoint_GetSpringHertz(r.id) != hz ||
                      b3DistanceJoint_GetSpringDampingRatio(r.id) != dr;
            b3DistanceJoint_EnableSpring(r.id, enabled);
            if (b3DistanceJoint_GetLength(r.id) != len) b3DistanceJoint_SetLength(r.id, len);
            b3DistanceJoint_SetSpringHertz(r.id, hz);
            b3DistanceJoint_SetSpringDampingRatio(r.id, dr);
            if (eff > 0.0f) b3DistanceJoint_SetSpringForceRange(r.id, -eff, eff);
            break;
        }
        default: break;
    }
    if (changed) { wake(r.a); wake(r.b); }
}

void World::setJointLimits(std::uint32_t joint, float lower, float upper, bool enabled) {
    if (!hasJoint(joint)) return;
    const JointRec& r = impl_->joints[joint];
    const float lo = std::min(lower, upper);
    const float hi = std::max(lower, upper);
    bool changed = false;
    switch (r.kind) {
        case JointKind::Hinge:
            changed = b3RevoluteJoint_IsLimitEnabled(r.id) != enabled ||
                      b3RevoluteJoint_GetLowerLimit(r.id) != lo ||
                      b3RevoluteJoint_GetUpperLimit(r.id) != hi;
            b3RevoluteJoint_EnableLimit(r.id, enabled);
            b3RevoluteJoint_SetLimits(r.id, lo, hi);
            break;
        case JointKind::Slider:
            changed = b3PrismaticJoint_IsLimitEnabled(r.id) != enabled ||
                      b3PrismaticJoint_GetLowerLimit(r.id) != lo ||
                      b3PrismaticJoint_GetUpperLimit(r.id) != hi;
            b3PrismaticJoint_EnableLimit(r.id, enabled);
            b3PrismaticJoint_SetLimits(r.id, lo, hi);
            break;
        case JointKind::Wheel:
            changed = b3WheelJoint_IsSuspensionLimitEnabled(r.id) != enabled ||
                      b3WheelJoint_GetLowerSuspensionLimit(r.id) != lo ||
                      b3WheelJoint_GetUpperSuspensionLimit(r.id) != hi;
            b3WheelJoint_EnableSuspensionLimit(r.id, enabled);
            b3WheelJoint_SetSuspensionLimits(r.id, lo, hi);
            break;
        case JointKind::Distance: {
            const float dlo = std::max(0.01f, lo), dhi = std::max(0.01f, hi);
            changed = b3DistanceJoint_IsLimitEnabled(r.id) != enabled ||
                      b3DistanceJoint_GetMinLength(r.id) != dlo ||
                      b3DistanceJoint_GetMaxLength(r.id) != dhi;
            b3DistanceJoint_EnableLimit(r.id, enabled);
            b3DistanceJoint_SetLengthRange(r.id, dlo, dhi);
            break;
        }
        default: break;
    }
    if (changed) { wake(r.a); wake(r.b); }
}

void World::jointState(std::uint32_t joint, float out[4]) const {
    out[0] = out[1] = out[2] = out[3] = 0.0f;
    if (!hasJoint(joint)) return;
    const JointRec& r = impl_->joints[joint];
    switch (r.kind) {
        case JointKind::Hinge: {
            out[0] = b3RevoluteJoint_GetAngle(r.id);
            const b3BodyId ida = r.a == kWorld ? impl_->ground : impl_->bodies[r.a].id;
            const b3BodyId idb = r.b == kWorld ? impl_->ground : impl_->bodies[r.b].id;
            const b3Quat qa = b3MulQuat(b3Body_GetRotation(ida), b3Joint_GetLocalFrameA(r.id).q);
            const b3Vec3 worldAxis = b3RotateVector(qa, b3Vec3{0, 0, 1});
            const b3Vec3 wRel = b3Sub(b3Body_GetAngularVelocity(idb), b3Body_GetAngularVelocity(ida));
            out[1] = b3Dot(wRel, worldAxis);
            out[2] = b3RevoluteJoint_GetMotorTorque(r.id);
            break;
        }
        case JointKind::Slider:
            out[0] = b3PrismaticJoint_GetTranslation(r.id);
            out[1] = b3PrismaticJoint_GetSpeed(r.id);
            out[2] = b3PrismaticJoint_GetMotorForce(r.id);
            break;
        case JointKind::Wheel:
            out[0] = b3WheelJoint_GetSteeringAngle(r.id);
            out[1] = b3WheelJoint_GetSpinSpeed(r.id);
            out[2] = b3WheelJoint_GetSpinTorque(r.id);
            out[3] = b3WheelJoint_GetSteeringTorque(r.id);
            break;
        case JointKind::Distance:
            out[0] = b3DistanceJoint_GetCurrentLength(r.id);
            out[2] = b3DistanceJoint_GetMotorForce(r.id);
            break;
        default: break;
    }
}

void World::step(float dt, int substeps) {
    const auto t0 = std::chrono::steady_clock::now();
    if (!impl_->structures.empty()) {
        awake(impl_->awake);
        for (auto& s : impl_->structures) {
            if (s) s->before(dt, impl_->awake);
        }
    }
    impl_->fresh.clear();
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
    impl_->touched.clear();
    impl_->hits.clear();
    const b3ContactEvents ce = b3World_GetContactEvents(impl_->world);
    for (int i = 0; i < ce.beginCount; i++) {
        Touch t;
        if (impl_->bodyOf(ce.beginEvents[i].shapeIdA, t.a) && impl_->bodyOf(ce.beginEvents[i].shapeIdB, t.b)) {
            impl_->touched.push_back(t);
        }
    }
    for (int i = 0; i < ce.hitCount; i++) {
        const b3ContactHitEvent& e = ce.hitEvents[i];
        Hit h;
        if (!impl_->bodyOf(e.shapeIdA, h.a) || !impl_->bodyOf(e.shapeIdB, h.b)) continue;
        putPos(h.point, e.point);
        put(h.normal, e.normal);
        h.speed = e.approachSpeed;
        impl_->hits.push_back(h);
    }
    for (auto& s : impl_->structures) {
        if (s) s->after(dt);
    }
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

void World::applyForce(std::uint32_t body, const float force[3], const float at[3]) {
    if (!hasBody(body)) return;
    b3Body_ApplyForce(impl_->bodies[body].id, v3(force), b3Pos{at[0], at[1], at[2]}, true);
}

void World::applyTorque(std::uint32_t body, const float torque[3]) {
    if (!hasBody(body)) return;
    b3Body_ApplyTorque(impl_->bodies[body].id, v3(torque), true);
}

float World::mass(std::uint32_t body) const { return hasBody(body) ? b3Body_GetMass(impl_->bodies[body].id) : 0.0f; }

void World::moveTo(std::uint32_t body, const Pose& pose, float dt) {
    if (!hasBody(body) || !(dt > 0.0f)) return;
    b3WorldTransform t;
    t.p = b3Pos{pose.p[0], pose.p[1], pose.p[2]};
    t.q = quat(pose.q);
    b3Body_SetTargetTransform(impl_->bodies[body].id, t, dt, true);
}

void World::wake(std::uint32_t body) {
    if (hasBody(body)) b3Body_SetAwake(impl_->bodies[body].id, true);
}

const std::vector<Moved>& World::moved() const { return impl_->moved; }
const std::vector<std::uint32_t>& World::broken() const { return impl_->broken; }
const std::vector<Touch>& World::touched() const { return impl_->touched; }
const std::vector<Hit>& World::hits() const { return impl_->hits; }

RayHit World::raycast(const float origin[3], const float dir[3], float maxDistance, std::uint32_t mask) const {
    RayHit out;
    const float len = std::sqrt(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
    if (!(len > 1e-9f) || !(maxDistance > 0.0f)) return out;
    const float k = maxDistance / len;
    b3QueryFilter f = b3DefaultQueryFilter();
    f.maskBits = mask;
    const b3RayResult r = b3World_CastRayClosest(impl_->world, b3Pos{origin[0], origin[1], origin[2]},
                                                 b3Vec3{dir[0] * k, dir[1] * k, dir[2] * k}, f);
    if (!r.hit || !impl_->bodyOf(r.shapeId, out.body)) return out;
    out.hit = true;
    putPos(out.point, r.point);
    put(out.normal, r.normal);
    out.distance = r.fraction * maxDistance;
    return out;
}

void World::fixedTriangles(std::uint32_t mask, std::vector<float>& xyz, std::vector<std::uint32_t>& indices) const {
    std::vector<b3ShapeId> shapes;
    for (const BodyRec& r : impl_->bodies) {
        if (!r.alive || !r.fixed || !(r.layers & mask)) continue;
        const b3WorldTransform t = b3Body_GetTransform(r.id);
        shapes.resize(static_cast<std::size_t>(b3Body_GetShapeCount(r.id)));
        const int n = b3Body_GetShapes(r.id, shapes.data(), static_cast<int>(shapes.size()));
        for (int s = 0; s < n; s++) {
            const std::uint32_t base = static_cast<std::uint32_t>(xyz.size() / 3);
            const auto vertex = [&](b3Vec3 local) {
                float p[3];
                putPos(p, b3TransformWorldPoint(t, local));
                xyz.insert(xyz.end(), p, p + 3);
            };
            const b3ShapeType type = b3Shape_GetType(shapes[static_cast<std::size_t>(s)]);
            if (type == b3_hullShape) {
                const b3HullData* h = b3Shape_GetHull(shapes[static_cast<std::size_t>(s)]);
                const b3Vec3* pts = b3GetHullPoints(h);
                const b3HullHalfEdge* edges = b3GetHullEdges(h);
                const b3HullFace* faces = b3GetHullFaces(h);
                for (int i = 0; i < h->vertexCount; i++) vertex(pts[i]);
                for (int fi = 0; fi < h->faceCount; fi++) {
                    const std::uint8_t first = faces[fi].edge;
                    const std::uint32_t v0 = edges[first].origin;
                    for (std::uint8_t e = edges[first].next; edges[e].next != first; e = edges[e].next) {
                        indices.push_back(base + v0);
                        indices.push_back(base + edges[e].origin);
                        indices.push_back(base + edges[edges[e].next].origin);
                    }
                }
            } else if (type == b3_meshShape) {
                const b3Mesh m = b3Shape_GetMesh(shapes[static_cast<std::size_t>(s)]);
                const b3Vec3* verts = b3GetMeshVertices(m.data);
                const b3MeshTriangle* tris = b3GetMeshTriangles(m.data);
                for (int i = 0; i < m.data->vertexCount; i++) {
                    vertex(b3Vec3{verts[i].x * m.scale.x, verts[i].y * m.scale.y, verts[i].z * m.scale.z});
                }
                for (int i = 0; i < m.data->triangleCount; i++) {
                    indices.push_back(base + static_cast<std::uint32_t>(tris[i].index1));
                    indices.push_back(base + static_cast<std::uint32_t>(tris[i].index2));
                    indices.push_back(base + static_cast<std::uint32_t>(tris[i].index3));
                }
            }
        }
    }
}

std::vector<Bounds> World::fixedBounds(std::uint32_t mask) const {
    std::vector<Bounds> out;
    for (std::size_t i = 0; i < impl_->bodies.size(); i++) {
        const BodyRec& r = impl_->bodies[i];
        if (!r.alive || !r.fixed || !(r.layers & mask)) continue;
        const b3AABB box = b3Body_ComputeAABB(r.id);
        Bounds b;
        b.body = static_cast<std::uint32_t>(i);
        put(b.min, box.lowerBound);
        put(b.max, box.upperBound);
        out.push_back(b);
    }
    return out;
}

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
