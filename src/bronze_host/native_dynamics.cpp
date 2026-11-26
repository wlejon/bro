// `__bro_native.dynamics` — the C entry points behind bro.dynamics (docs/dynamics-api.js):
// worlds of dynamics::World (src/dynamics/world.h), named by number. The public shape is
// js/dynamics.js's.
//
// A world belongs to the thread that made it: the table is per thread, and a Worker's
// worlds are its own.

#include "dynamics/world.h"
#include "bronze_host/host_natives.h"
#include "natives/dynamics/native_dynamics_decl.h"

#include "embed/embed.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace {

using bro::dynamics::World;

std::vector<std::unique_ptr<World>>& worlds() {
    thread_local std::vector<std::unique_ptr<World>> table;
    return table;
}

World* worldOf(int32_t id) {
    auto& t = worlds();
    if (id < 0 || static_cast<std::size_t>(id) >= t.size()) return nullptr;
    return t[static_cast<std::size_t>(id)].get();
}

std::uint32_t idOf(int32_t v) { return v < 0 ? bro::dynamics::kWorld : static_cast<std::uint32_t>(v); }

template <typename T>
void give(std::vector<T>&& v, bronze_native_buffer* out) {
    if (v.empty()) {
        out->data = nullptr;
        out->length = 0;
        out->release = nullptr;
        out->ctx = nullptr;
        return;
    }
    auto* owned = new std::vector<T>(std::move(v));
    out->data = owned->data();
    out->length = static_cast<uint32_t>(owned->size());
    out->release = [](void* ctx) { delete static_cast<std::vector<T>*>(ctx); };
    out->ctx = owned;
}

// position 3, rotation 4, size 3, mass, friction, restitution, velocity 3, angular 3,
// linear damping, angular damping, gravity scale: 22 floats. flags: 1 kinematic, 2 bullet,
// 4 planar.
bro::dynamics::BodyDesc bodyDesc(const float* desc, int32_t flags, int32_t layers, int32_t mask) {
    bro::dynamics::BodyDesc d;
    for (int i = 0; i < 3; i++) d.pose.p[i] = desc[i];
    for (int i = 0; i < 4; i++) d.pose.q[i] = desc[3 + i];
    d.mass = desc[10];
    d.friction = desc[11];
    d.restitution = desc[12];
    for (int i = 0; i < 3; i++) { d.velocity[i] = desc[13 + i]; d.angular[i] = desc[16 + i]; }
    d.linearDamping = desc[19];
    d.angularDamping = desc[20];
    d.gravityScale = desc[21];
    d.kinematic = (flags & 1) != 0;
    d.bullet = (flags & 2) != 0;
    d.planar = (flags & 4) != 0;
    d.layers = static_cast<std::uint32_t>(layers);
    d.mask = static_cast<std::uint32_t>(mask);
    return d;
}

int32_t named(std::uint32_t id) { return id == bro::dynamics::kWorld ? -1 : static_cast<int32_t>(id); }

}  // namespace

namespace bro::bronze_host {

// The world a script's World names on this thread (its _id), for the host's own readers of
// a world: nav bakes, an agent's ground follow.
dynamics::World* dynamicsWorld(int32_t id) { return worldOf(id); }

int32_t dynamicsWorldId(bronze::Value world) {
    if (!bronze::embed::isObject(world)) return -1;
    const bronze::Value id = bronze::embed::getProperty(world, "_id");
    if (!bronze::embed::isNumber(id)) return -1;
    const int32_t n = static_cast<int32_t>(bronze::embed::toDouble(id));
    return worldOf(n) ? n : -1;
}

}  // namespace bro::bronze_host

extern "C" {

int32_t bro_dynamics_create(void) {
    auto& t = worlds();
    for (std::size_t i = 0; i < t.size(); i++) {
        if (!t[i]) { t[i] = std::make_unique<World>(); return static_cast<int32_t>(i); }
    }
    t.push_back(std::make_unique<World>());
    return static_cast<int32_t>(t.size() - 1);
}

void bro_dynamics_destroy(int32_t world) {
    auto& t = worlds();
    if (world >= 0 && static_cast<std::size_t>(world) < t.size()) t[static_cast<std::size_t>(world)].reset();
}

void bro_dynamics_setGravity(int32_t world, double x, double y, double z) {
    if (World* w = worldOf(world)) w->setGravity(static_cast<float>(x), static_cast<float>(y), static_cast<float>(z));
}

int32_t bro_dynamics_addBody(int32_t world, int32_t shape, const float* desc, uint32_t descLen, int32_t flags,
                             int32_t layers, int32_t mask) {
    World* w = worldOf(world);
    if (!w || descLen < 22) return -1;
    const bro::dynamics::BodyDesc d = bodyDesc(desc, flags, layers, mask);
    switch (shape) {
        case 0: return named(w->addBox(d, desc + 7));
        case 1: return named(w->addSphere(d, desc[7]));
        case 2: return named(w->addCapsule(d, desc[7], desc[8]));
        default: return -1;
    }
}

int32_t bro_dynamics_addHull(int32_t world, const float* desc, uint32_t descLen, int32_t flags, int32_t layers,
                             int32_t mask, const float* xyz, uint32_t xyzLen) {
    World* w = worldOf(world);
    if (!w || descLen < 22) return -1;
    return named(w->addHull(bodyDesc(desc, flags, layers, mask), xyz, static_cast<int>(xyzLen / 3)));
}

int32_t bro_dynamics_addMesh(int32_t world, const float* desc, uint32_t descLen, int32_t layers, int32_t mask,
                             const float* xyz, uint32_t xyzLen, const int32_t* indices, uint32_t indexLen) {
    World* w = worldOf(world);
    if (!w || descLen < 22) return -1;
    return named(w->addMesh(bodyDesc(desc, 0, layers, mask), xyz, static_cast<int>(xyzLen / 3), indices,
                            static_cast<int>(indexLen / 3)));
}

void bro_dynamics_setMaxSpeed(int32_t world, double speed) {
    if (World* w = worldOf(world)) w->setMaxSpeed(static_cast<float>(speed));
}

void bro_dynamics_setHitSpeed(int32_t world, double speed) {
    if (World* w = worldOf(world)) w->setHitSpeed(static_cast<float>(speed));
}

void bro_dynamics_moveTo(int32_t world, int32_t body, const float* pose, uint32_t poseLen, double dt) {
    World* w = worldOf(world);
    if (!w || poseLen < 7) return;
    bro::dynamics::Pose p;
    for (int i = 0; i < 3; i++) p.p[i] = pose[i];
    for (int i = 0; i < 4; i++) p.q[i] = pose[3 + i];
    w->moveTo(idOf(body), p, static_cast<float>(dt));
}

void bro_dynamics_wake(int32_t world, int32_t body) {
    if (World* w = worldOf(world)) w->wake(idOf(body));
}

bool bro_dynamics_hasBody(int32_t world, int32_t body) {
    World* w = worldOf(world);
    return w && w->hasBody(idOf(body));
}

void bro_dynamics_touched(int32_t world, bronze_native_buffer* out) {
    std::vector<int32_t> v;
    if (World* w = worldOf(world)) {
        for (const bro::dynamics::Touch& t : w->touched()) {
            v.push_back(static_cast<int32_t>(t.a));
            v.push_back(static_cast<int32_t>(t.b));
        }
    }
    give(std::move(v), out);
}

void bro_dynamics_hits(int32_t world, bronze_native_buffer* out) {
    std::vector<float> v;
    if (World* w = worldOf(world)) {
        for (const bro::dynamics::Hit& h : w->hits()) {
            v.push_back(static_cast<float>(h.a));
            v.push_back(static_cast<float>(h.b));
            v.insert(v.end(), h.point, h.point + 3);
            v.insert(v.end(), h.normal, h.normal + 3);
            v.push_back(h.speed);
        }
    }
    give(std::move(v), out);
}

void bro_dynamics_raycast(int32_t world, const float* ray, uint32_t rayLen, double maxDistance, int32_t mask,
                          bronze_native_buffer* out) {
    std::vector<float> v;
    World* w = worldOf(world);
    if (w && rayLen >= 6) {
        const bro::dynamics::RayHit h =
            w->raycast(ray, ray + 3, static_cast<float>(maxDistance), static_cast<std::uint32_t>(mask));
        if (h.hit) {
            v.push_back(static_cast<float>(h.body));
            v.insert(v.end(), h.point, h.point + 3);
            v.insert(v.end(), h.normal, h.normal + 3);
            v.push_back(h.distance);
        }
    }
    give(std::move(v), out);
}

void bro_dynamics_removeBody(int32_t world, int32_t body) {
    if (World* w = worldOf(world)) w->removeBody(idOf(body));
}

int32_t bro_dynamics_addJoint(int32_t world, int32_t kind, int32_t a, int32_t b, const float* desc, uint32_t descLen,
                              bool collide) {
    World* w = worldOf(world);
    if (!w || descLen < 8 || kind < 0 || kind > 2) return -1;
    bro::dynamics::JointDesc d;
    d.kind = static_cast<bro::dynamics::JointKind>(kind);
    d.a = idOf(a);
    d.b = idOf(b);
    for (int i = 0; i < 3; i++) { d.anchor[i] = desc[i]; d.axis[i] = desc[3 + i]; }
    d.breakForce = desc[6];
    d.breakTorque = desc[7];
    d.collide = collide;
    const std::uint32_t id = w->addJoint(d);
    return id == bro::dynamics::kWorld ? -1 : static_cast<int32_t>(id);
}

void bro_dynamics_removeJoint(int32_t world, int32_t joint) {
    if (World* w = worldOf(world)) w->removeJoint(idOf(joint));
}

bool bro_dynamics_hasJoint(int32_t world, int32_t joint) {
    World* w = worldOf(world);
    return w && w->hasJoint(idOf(joint));
}

void bro_dynamics_jointLoad(int32_t world, int32_t joint, bronze_native_buffer* out) {
    std::vector<float> v(6, 0.0f);
    if (World* w = worldOf(world)) w->jointLoad(idOf(joint), v.data(), v.data() + 3);
    give(std::move(v), out);
}

void bro_dynamics_step(int32_t world, double dt, int32_t substeps) {
    if (World* w = worldOf(world)) w->step(static_cast<float>(dt), substeps);
}

void bro_dynamics_pose(int32_t world, int32_t body, bronze_native_buffer* out) {
    std::vector<float> v(7, 0.0f);
    v[6] = 1.0f;
    if (World* w = worldOf(world)) {
        const bro::dynamics::Pose p = w->pose(idOf(body));
        for (int i = 0; i < 3; i++) v[i] = p.p[i];
        for (int i = 0; i < 4; i++) v[3 + i] = p.q[i];
    }
    give(std::move(v), out);
}

void bro_dynamics_setPose(int32_t world, int32_t body, const float* pose, uint32_t poseLen) {
    World* w = worldOf(world);
    if (!w || poseLen < 7) return;
    bro::dynamics::Pose p;
    for (int i = 0; i < 3; i++) p.p[i] = pose[i];
    for (int i = 0; i < 4; i++) p.q[i] = pose[3 + i];
    w->setPose(idOf(body), p);
}

void bro_dynamics_velocity(int32_t world, int32_t body, bronze_native_buffer* out) {
    std::vector<float> v(6, 0.0f);
    if (World* w = worldOf(world)) w->velocity(idOf(body), v.data(), v.data() + 3);
    give(std::move(v), out);
}

void bro_dynamics_setVelocity(int32_t world, int32_t body, const float* v, uint32_t vLen) {
    World* w = worldOf(world);
    if (w && vLen >= 6) w->setVelocity(idOf(body), v, v + 3);
}

void bro_dynamics_applyImpulse(int32_t world, int32_t body, const float* v, uint32_t vLen) {
    World* w = worldOf(world);
    if (w && vLen >= 6) w->applyImpulse(idOf(body), v, v + 3);
}

double bro_dynamics_mass(int32_t world, int32_t body) {
    World* w = worldOf(world);
    return w ? w->mass(idOf(body)) : 0.0;
}

void bro_dynamics_moved(int32_t world, bronze_native_buffer* out) {
    std::vector<float> v;
    if (World* w = worldOf(world)) {
        const auto& m = w->moved();
        v.resize(m.size() * 9);
        for (std::size_t i = 0; i < m.size(); i++) {
            float* r = v.data() + i * 9;
            r[0] = static_cast<float>(m[i].body);
            for (int k = 0; k < 3; k++) r[1 + k] = m[i].pose.p[k];
            for (int k = 0; k < 4; k++) r[4 + k] = m[i].pose.q[k];
            r[8] = m[i].asleep ? 1.0f : 0.0f;
        }
    }
    give(std::move(v), out);
}

void bro_dynamics_broken(int32_t world, bronze_native_buffer* out) {
    std::vector<int32_t> v;
    if (World* w = worldOf(world)) {
        for (std::uint32_t j : w->broken()) v.push_back(static_cast<int32_t>(j));
    }
    give(std::move(v), out);
}

void bro_dynamics_counts(int32_t world, bronze_native_buffer* out) {
    std::vector<double> v(5, 0.0);
    if (World* w = worldOf(world)) {
        const bro::dynamics::Counts c = w->counts();
        v[0] = c.bodies;
        v[1] = c.joints;
        v[2] = c.awake;
        v[3] = c.contacts;
        v[4] = c.stepMs;
    }
    give(std::move(v), out);
}

}  // extern "C"
