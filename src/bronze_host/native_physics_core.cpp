// native_physics_core.cpp — Physics core world, rigid body, and simulation natives.

#include "bronze_host/native_physics_internal.h"
#include "natives/physics/native_physics_decl.h"
#include <iomanip>
#include <sstream>

namespace bro::bronze_host {

HostPhysicsWorld g_defaultWorld;
static thread_local std::vector<HostPhysicsWorld*> tl_activeWorlds;

HostPhysicsWorld* getActiveWorld() {
    return tl_activeWorlds.empty() ? &g_defaultWorld : tl_activeWorlds.back();
}

void pushActiveWorld(HostPhysicsWorld* w) {
    if (w) tl_activeWorlds.push_back(w);
}

void popActiveWorld() {
    if (!tl_activeWorlds.empty()) tl_activeWorlds.pop_back();
}

void forgetActiveWorld(HostPhysicsWorld* w) {
    std::erase(tl_activeWorlds, w);
}


// Physics.setMotionType is hand-written in js/physics.js: the public member
// takes 'static' | 'dynamic' | 'kinematic' or a boolean, the native takes the
// boolean, and js/physics.js maps between them. So the native is declared
// and registered here, not in the natives/physics/ pair.
extern "C" void bro_physics_setMotionType(int32_t tag, bool isStatic);

// Physics.moveKinematic is hand-written in js/physics.js for the same reason: the public
// member has always taken either (tag, x, y, z, dt) or (tag, x, y, z, qx, qy,
// qz, qw, dt) — the QuickJS binding dispatched on argc — and a native has one
// arity, so each form is its own entry point and js/physics.js picks by the
// arguments it was given. Both are declared and registered here.
extern "C" void bro_physics_moveKinematic(int32_t tag, double x, double y, double z, double dt);
extern "C" void bro_physics_moveKinematicRot(int32_t tag, double x, double y, double z,
                                             double qx, double qy, double qz, double qw,
                                             double dt);


static thread_local std::vector<float> tl_allTransformsBuf;

physics::PhysicsWorld* unwrapPhysicsWorld(Value v) {
    if (ev::isObject(v)) {
        if (auto* w = static_cast<HostPhysicsWorld*>(ev::handleData(v))) {
            if (w->tag == kHostPhysicsWorldTag) return w->getWorld();
        }
    }
    auto* e = hostEngine();
    return e ? e->physicsWorld() : nullptr;
}

}  // namespace bro::bronze_host

extern "C" {

using namespace bro;
using namespace bro::bronze_host;

// --- PhysicsWorldHandle -----------------------------------------------------

void bro_physics_PhysicsWorldHandle_dtor(void* self) {
    auto* pw = static_cast<HostPhysicsWorld*>(self);
    if (pw && pw != &g_defaultWorld) {
        forgetActiveWorld(pw);
        delete pw;
    }
}

void* bro_physics_PhysicsWorldHandle_ctor(void) {
    return nullptr;
}

void bro_physics_PhysicsWorldHandle_destroy(void* self) {
    auto* pw = static_cast<HostPhysicsWorld*>(self);
    if (pw && pw != &g_defaultWorld) {
        if (pw->world) {
            pw->world->destroyAll();
            pw->clear();
        }
    }
}

void bro_physics_PhysicsWorldHandle_step(void* self, double dt) {
    auto* pw = static_cast<HostPhysicsWorld*>(self);
    if (pw && pw->getWorld()) {
        pushActiveWorld(pw);
        if (std::isfinite(dt) && dt > 0.0) pw->getWorld()->setTimeStep(static_cast<float>(dt));
        pw->getWorld()->stepInline();
        popActiveWorld();
    }
}

void bro_physics_PhysicsWorldHandle_enter(void* self) {
    auto* pw = static_cast<HostPhysicsWorld*>(self);
    pushActiveWorld(pw);
}

void bro_physics_PhysicsWorldHandle_exit(void* /*self*/) {
    popActiveWorld();
}

void* bro_physics_createWorldHandle(const char* opts) {
    auto* pw = new HostPhysicsWorld();
    pw->world = new physics::PhysicsWorld();
    pw->ownsWorld = true;

    uint32_t maxBodies = 10240;
    uint32_t contactBufferSize = 4096;
    JPH::Vec3 gravity(0, -9.81f, 0);

    if (opts && *opts) {
        auto res = ev::parseJson(opts);
        if (!res.thrown && ev::isObject(res.value)) {
            const Rooted cfg(res.value);
            // Both size native tables up front, so they are bounded here:
            // PhysicsWorld::init clamps maxBodies to its body cap and the
            // contact buffer to [16, 65536].
            maxBodies = satCast<uint32_t>(getPropNumber(cfg, "maxBodies", 10240));
            contactBufferSize = satCast<uint32_t>(getPropNumber(cfg, "contactBufferSize", 4096));
            Value gv = ev::getProperty(cfg, "gravity");
            if (!ev::isUndefined(gv) && !ev::isNull(gv)) {
                gravity = readVec3(gv, gravity);
            }
        }
    }

    pw->world->init(static_cast<int>(std::min<uint32_t>(
                        maxBodies, physics::PhysicsWorld::kMaxBodies)),
                    static_cast<int>(std::min<uint32_t>(contactBufferSize, 65536)));
    pw->world->setGravity(gravity.GetX(), gravity.GetY(), gravity.GetZ());
    return pw;
}

void bro_physics_createWorld(const char* opts) {
    auto* pw = getActiveWorld();
    pw->clear();
    if (auto* w = pw->getWorld()) {
        w->destroyAll();
        if (opts && *opts) {
            auto res = ev::parseJson(opts);
            if (!res.thrown && ev::isObject(res.value)) {
                Value gv = ev::getProperty(res.value, "gravity");
                if (!ev::isUndefined(gv) && !ev::isNull(gv)) {
                    JPH::Vec3 g = readVec3(gv, JPH::Vec3(0, -9.81f, 0));
                    w->setGravity(g.GetX(), g.GetY(), g.GetZ());
                }
            }
        }
    }
}

// --- Simulation Timing & Gravity --------------------------------------------

void bro_physics_setTimeStep(double dt) {
    if (!std::isfinite(dt) || dt <= 0.0) return;
    auto* pw = getActiveWorld();
    if (auto* w = pw ? pw->getWorld() : nullptr) w->setTimeStep(static_cast<float>(dt));
}

double bro_physics_getTimeStep(void) {
    auto* pw = getActiveWorld();
    auto* w = pw ? pw->getWorld() : nullptr;
    return w ? w->timeStep() : (1.0 / 60.0);
}

void bro_physics_step(double dt) {
    auto* pw = getActiveWorld();
    if (auto* w = pw ? pw->getWorld() : nullptr) {
        if (std::isfinite(dt) && dt > 0.0) w->setTimeStep(static_cast<float>(dt));
        w->stepInline();
    }
}

void bro_physics_setInterpolation(bool enabled) {
    auto* pw = getActiveWorld();
    if (auto* w = pw ? pw->getWorld() : nullptr) w->setInterpolation(enabled);
}

bool bro_physics_getInterpolation(void) {
    auto* pw = getActiveWorld();
    auto* w = pw ? pw->getWorld() : nullptr;
    return w ? w->interpolation() : false;
}

void bro_physics_setGravity(double x, double y, double z) {
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) return;
    auto* pw = getActiveWorld();
    if (auto* w = pw ? pw->getWorld() : nullptr) w->setGravity((float)x, (float)y, (float)z);
}

const char* bro_physics_getGravity(void) {
    auto* pw = getActiveWorld();
    auto* w = pw ? pw->getWorld() : nullptr;
    JPH::Vec3 g = w ? w->gravity() : JPH::Vec3(0, -9.81f, 0);
    std::string s = "{\"x\":" + std::to_string(g.GetX()) +
                    ",\"y\":" + std::to_string(g.GetY()) +
                    ",\"z\":" + std::to_string(g.GetZ()) + "}";
    return natives::strResult(s);
}

bool bro_physics_setLayers(const char* config) {
    auto* pw = getActiveWorld();
    auto* w = pw ? pw->getWorld() : nullptr;
    if (!w || !config || !*config) return false;
    auto res = ev::parseJson(config);
    if (res.thrown || !ev::isObject(res.value)) return false;

    const Rooted cfg(res.value);
    const Rooted namesVal(ev::getProperty(cfg, "names"));
    const Rooted matVal(ev::getProperty(cfg, "matrix"));
    std::vector<std::string> names;
    if (ev::isObject(namesVal)) {
        Value lenV = ev::getProperty(namesVal, "length");
        if (ev::isNumber(lenV)) {
            uint32_t n = satCast<uint32_t>(ev::toDouble(lenV));
            for (uint32_t i = 0; i < n; ++i) {
                Value el = ev::getElement(namesVal, i);
                names.push_back(ev::isString(el) ? ev::toUtf8(el) : "");
            }
        }
    }
    std::vector<bool> matrix;
    if (ev::isObject(matVal)) {
        Value lenV = ev::getProperty(matVal, "length");
        if (ev::isNumber(lenV)) {
            uint32_t n = satCast<uint32_t>(ev::toDouble(lenV));
            for (uint32_t i = 0; i < n; ++i) {
                matrix.push_back(ev::toBool(ev::getElement(matVal, i)));
            }
        }
    }
    return w->configureLayers(names, matrix);
}

// --- Body Lifecycle ---------------------------------------------------------

int32_t bro_physics_createBody(const char* config) {
    auto* pw = getActiveWorld();
    auto* w = pw ? pw->getWorld() : nullptr;
    if (!w || !config || !*config) return -1;
    auto res = ev::parseJson(config);
    if (res.thrown || !ev::isObject(res.value)) return -1;

    physics::BodyOptions opts;
    std::string err;
    if (!readBodyOptions(res.value, opts, err, w)) {
        LOG_WARN("Physics.createBody: %s", err.c_str());
        return -1;
    }

    JPH::BodyID id = w->createBody(opts);
    if (id.IsInvalid()) return -1;
    return pw->registerBody(id);
}

void bro_physics_destroyBody(int32_t tag) {
    auto* pw = getActiveWorld();
    auto* w = pw ? pw->getWorld() : nullptr;
    if (!w) return;
    JPH::BodyID id = pw->bodyIdForTag(tag);
    if (id.IsInvalid()) return;
    bool destroyed = false;
    w->destroyBody(id, [pw, &destroyed](JPH::BodyID bid) {
        pw->unregisterBodyId(bid);
        destroyed = true;
    });
    if (destroyed) {
        pw->unregisterBody(tag);
    }
}

void bro_physics_destroyAll(void) {
    auto* pw = getActiveWorld();
    auto* w = pw ? pw->getWorld() : nullptr;
    if (w) {
        w->destroyAll();
        pw->clear();
    }
}

// --- Transforms & Properties ------------------------------------------------

const char* bro_physics_getTransform(int32_t tag, bool interpolated) {
    auto* pw = getActiveWorld();
    auto* w = pw ? pw->getWorld() : nullptr;
    if (!w) return natives::strResult("null");
    JPH::BodyID id = pw->bodyIdForTag(tag);
    if (id.IsInvalid() || !w->bodyExists(id)) return natives::strResult("null");

    JPH::RVec3 pos;
    JPH::Quat rot;
    if (interpolated) {
        w->getRenderTransform(id, pos, rot);
    } else {
        pos = w->getPosition(id);
        rot = w->getRotation(id);
    }
    uint64_t udata = w->getUserData(id);

    std::string s = "{\"position\":{\"x\":" + std::to_string(pos.GetX()) +
                    ",\"y\":" + std::to_string(pos.GetY()) +
                    ",\"z\":" + std::to_string(pos.GetZ()) + "}," +
                    "\"rotation\":{\"x\":" + std::to_string(rot.GetX()) +
                    ",\"y\":" + std::to_string(rot.GetY()) +
                    ",\"z\":" + std::to_string(rot.GetZ()) +
                    ",\"w\":" + std::to_string(rot.GetW()) + "}," +
                    "\"userData\":" + std::to_string(udata) + "}";
    return natives::strResult(s);
}

const char* bro_physics_getVelocity(int32_t tag) {
    auto* pw = getActiveWorld();
    auto* w = pw ? pw->getWorld() : nullptr;
    if (!w) return natives::strResult("null");
    JPH::BodyID id = pw->bodyIdForTag(tag);
    if (id.IsInvalid() || !w->bodyExists(id)) return natives::strResult("null");

    JPH::Vec3 lin = w->getLinearVelocity(id);
    JPH::Vec3 ang = w->getAngularVelocity(id);

    std::string s = "{\"linear\":{\"x\":" + std::to_string(lin.GetX()) +
                    ",\"y\":" + std::to_string(lin.GetY()) +
                    ",\"z\":" + std::to_string(lin.GetZ()) + "}," +
                    "\"angular\":{\"x\":" + std::to_string(ang.GetX()) +
                    ",\"y\":" + std::to_string(ang.GetY()) +
                    ",\"z\":" + std::to_string(ang.GetZ()) + "}}";
    return natives::strResult(s);
}

void bro_physics_setPosition(int32_t tag, double x, double y, double z) {
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) return;
    auto* pw = getActiveWorld();
    auto* w = pw ? pw->getWorld() : nullptr;
    if (!w) return;
    JPH::BodyID id = pw->bodyIdForTag(tag);
    if (!id.IsInvalid()) w->setPosition(id, JPH::RVec3(x, y, z));
}

void bro_physics_setRotation(int32_t tag, double x, double y, double z, double w) {
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z) || !std::isfinite(w)) return;
    auto* pw = getActiveWorld();
    auto* world = pw ? pw->getWorld() : nullptr;
    if (!world) return;
    JPH::BodyID id = pw->bodyIdForTag(tag);
    if (!id.IsInvalid()) {
        JPH::Quat q((float)x, (float)y, (float)z, (float)w);
        if (q.LengthSq() > 1e-12f) world->setRotation(id, q.Normalized());
    }
}

void bro_physics_setLinearVelocity(int32_t tag, double x, double y, double z) {
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) return;
    auto* pw = getActiveWorld();
    auto* w = pw ? pw->getWorld() : nullptr;
    if (!w) return;
    JPH::BodyID id = pw->bodyIdForTag(tag);
    if (!id.IsInvalid()) w->setLinearVelocity(id, JPH::Vec3((float)x, (float)y, (float)z));
}

void bro_physics_setAngularVelocity(int32_t tag, double x, double y, double z) {
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) return;
    auto* pw = getActiveWorld();
    auto* w = pw ? pw->getWorld() : nullptr;
    if (!w) return;
    JPH::BodyID id = pw->bodyIdForTag(tag);
    if (!id.IsInvalid()) w->setAngularVelocity(id, JPH::Vec3((float)x, (float)y, (float)z));
}

void bro_physics_addForce(int32_t tag, double x, double y, double z) {
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) return;
    auto* pw = getActiveWorld();
    auto* w = pw ? pw->getWorld() : nullptr;
    if (!w) return;
    JPH::BodyID id = pw->bodyIdForTag(tag);
    if (!id.IsInvalid()) w->addForce(id, JPH::Vec3((float)x, (float)y, (float)z));
}

void bro_physics_addImpulse(int32_t tag, double x, double y, double z) {
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) return;
    auto* pw = getActiveWorld();
    auto* w = pw ? pw->getWorld() : nullptr;
    if (!w) return;
    JPH::BodyID id = pw->bodyIdForTag(tag);
    if (!id.IsInvalid()) w->addImpulse(id, JPH::Vec3((float)x, (float)y, (float)z));
}

void bro_physics_addTorque(int32_t tag, double x, double y, double z) {
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) return;
    auto* pw = getActiveWorld();
    auto* w = pw ? pw->getWorld() : nullptr;
    if (!w) return;
    JPH::BodyID id = pw->bodyIdForTag(tag);
    if (!id.IsInvalid()) w->addTorque(id, JPH::Vec3((float)x, (float)y, (float)z));
}

void bro_physics_setUserData(int32_t tag, double data) {
    auto* pw = getActiveWorld();
    auto* w = pw ? pw->getWorld() : nullptr;
    if (!w) return;
    JPH::BodyID id = pw->bodyIdForTag(tag);
    if (!id.IsInvalid()) w->setUserData(id, static_cast<uint64_t>(data));
}

double bro_physics_getUserData(int32_t tag) {
    auto* pw = getActiveWorld();
    auto* w = pw ? pw->getWorld() : nullptr;
    if (!w) return 0.0;
    JPH::BodyID id = pw->bodyIdForTag(tag);
    return id.IsInvalid() ? 0.0 : static_cast<double>(w->getUserData(id));
}

bool bro_physics_setLayer(int32_t tag, const char* layer) {
    auto* pw = getActiveWorld();
    auto* w = pw ? pw->getWorld() : nullptr;
    if (!w || !layer) return false;
    JPH::BodyID id = pw->bodyIdForTag(tag);
    if (id.IsInvalid()) return false;
    int idx = 0;
    if (parseDecimalIndex(layer, idx)) {
        if (idx < 0 || idx >= w->numLayers()) return false;
        w->setLayer(id, idx);
        return true;
    }
    idx = w->layerIndex(layer);
    if (idx < 0) return false;
    w->setLayer(id, idx);
    return true;
}

void bro_physics_setKinematic(int32_t tag) {
    auto* pw = getActiveWorld();
    auto* w = pw ? pw->getWorld() : nullptr;
    if (!w) return;
    JPH::BodyID id = pw->bodyIdForTag(tag);
    if (!id.IsInvalid()) w->setKinematic(id);
}

void bro_physics_setMotionType(int32_t tag, bool isStatic) {
    auto* pw = getActiveWorld();
    auto* w = pw ? pw->getWorld() : nullptr;
    if (!w) return;
    JPH::BodyID id = pw->bodyIdForTag(tag);
    if (!id.IsInvalid()) w->setMotionType(id, isStatic);
}

// A dt of zero (or less) is not refused: PhysicsWorld::moveKinematic places
// the body at the target at once, where a positive dt sets the velocity that
// reaches it over that step.
void bro_physics_moveKinematic(int32_t tag, double x, double y, double z, double dt) {
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z) || !std::isfinite(dt)) return;
    auto* pw = getActiveWorld();
    auto* w = pw ? pw->getWorld() : nullptr;
    if (!w) return;
    JPH::BodyID id = pw->bodyIdForTag(tag);
    if (!id.IsInvalid()) {
        JPH::Quat rot = w->getRotation(id);
        w->moveKinematic(id, JPH::RVec3(x, y, z), rot, (float)dt);
    }
}

void bro_physics_moveKinematicRot(int32_t tag, double x, double y, double z,
                                  double qx, double qy, double qz, double qw, double dt) {
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z) ||
        !std::isfinite(qx) || !std::isfinite(qy) || !std::isfinite(qz) || !std::isfinite(qw) ||
        !std::isfinite(dt)) return;
    auto* pw = getActiveWorld();
    auto* w = pw ? pw->getWorld() : nullptr;
    if (!w) return;
    JPH::BodyID id = pw->bodyIdForTag(tag);
    if (id.IsInvalid()) return;
    JPH::Quat rot((float)qx, (float)qy, (float)qz, (float)qw);
    if (rot.LengthSq() > 0.0f) rot = rot.Normalized();
    else rot = w->getRotation(id);
    w->moveKinematic(id, JPH::RVec3(x, y, z), rot, (float)dt);
}

void bro_physics_setFrictionCombine(int32_t tag, const char* mode) {
    auto* pw = getActiveWorld();
    auto* w = pw ? pw->getWorld() : nullptr;
    if (!w || !mode) return;
    JPH::BodyID id = pw->bodyIdForTag(tag);
    physics::CombineMode cm;
    if (!id.IsInvalid() && parseCombineMode(mode, cm)) w->setFrictionCombine(id, cm);
}

void bro_physics_setRestitutionCombine(int32_t tag, const char* mode) {
    auto* pw = getActiveWorld();
    auto* w = pw ? pw->getWorld() : nullptr;
    if (!w || !mode) return;
    JPH::BodyID id = pw->bodyIdForTag(tag);
    physics::CombineMode cm;
    if (!id.IsInvalid() && parseCombineMode(mode, cm)) w->setRestitutionCombine(id, cm);
}

double bro_physics_getMass(int32_t tag) {
    auto* pw = getActiveWorld();
    auto* w = pw ? pw->getWorld() : nullptr;
    if (!w) return 0.0;
    JPH::BodyID id = pw->bodyIdForTag(tag);
    return id.IsInvalid() ? 0.0 : (double)w->getMass(id);
}

void bro_physics_setMass(int32_t tag, double mass) {
    if (!std::isfinite(mass) || mass < 0.0) return;
    auto* pw = getActiveWorld();
    auto* w = pw ? pw->getWorld() : nullptr;
    if (!w) return;
    JPH::BodyID id = pw->bodyIdForTag(tag);
    if (!id.IsInvalid()) w->setMass(id, (float)mass);
}

void bro_physics_setLinearDamping(int32_t tag, double damping) {
    if (!std::isfinite(damping)) return;
    auto* pw = getActiveWorld();
    auto* w = pw ? pw->getWorld() : nullptr;
    if (!w) return;
    JPH::BodyID id = pw->bodyIdForTag(tag);
    if (!id.IsInvalid()) w->setLinearDamping(id, (float)damping);
}

void bro_physics_setAngularDamping(int32_t tag, double damping) {
    if (!std::isfinite(damping)) return;
    auto* pw = getActiveWorld();
    auto* w = pw ? pw->getWorld() : nullptr;
    if (!w) return;
    JPH::BodyID id = pw->bodyIdForTag(tag);
    if (!id.IsInvalid()) w->setAngularDamping(id, (float)damping);
}

void bro_physics_setGravityFactor(int32_t tag, double factor) {
    if (!std::isfinite(factor)) return;
    auto* pw = getActiveWorld();
    auto* w = pw ? pw->getWorld() : nullptr;
    if (!w) return;
    JPH::BodyID id = pw->bodyIdForTag(tag);
    if (!id.IsInvalid()) w->setGravityFactor(id, (float)factor);
}

void bro_physics_setFriction(int32_t tag, double friction) {
    if (!std::isfinite(friction)) return;
    auto* pw = getActiveWorld();
    auto* w = pw ? pw->getWorld() : nullptr;
    if (!w) return;
    JPH::BodyID id = pw->bodyIdForTag(tag);
    if (!id.IsInvalid()) w->setFriction(id, (float)friction);
}

void bro_physics_setRestitution(int32_t tag, double restitution) {
    if (!std::isfinite(restitution)) return;
    auto* pw = getActiveWorld();
    auto* w = pw ? pw->getWorld() : nullptr;
    if (!w) return;
    JPH::BodyID id = pw->bodyIdForTag(tag);
    if (!id.IsInvalid()) w->setRestitution(id, (float)restitution);
}

const char* bro_physics_getBodyProperties(int32_t tag) {
    auto* pw = getActiveWorld();
    auto* w = pw ? pw->getWorld() : nullptr;
    if (!w) return natives::strResult("null");
    JPH::BodyID id = pw->bodyIdForTag(tag);
    if (id.IsInvalid() || !w->bodyExists(id)) return natives::strResult("null");

    float mass = w->getMass(id);
    float friction = w->getFriction(id);
    float restitution = w->getRestitution(id);
    float linearDamping = w->getLinearDamping(id);
    float angularDamping = w->getAngularDamping(id);
    float gravityFactor = w->getGravityFactor(id);

    std::ostringstream ss;
    ss << "{\"mass\":" << mass
       << ",\"friction\":" << friction
       << ",\"restitution\":" << restitution
       << ",\"linearDamping\":" << linearDamping
       << ",\"angularDamping\":" << angularDamping
       << ",\"gravityFactor\":" << gravityFactor << "}";
    return natives::strResult(ss.str());
}

bool bro_physics_setAreaOverride(int32_t tag, const char* config) {
    auto* pw = getActiveWorld();
    auto* w = pw ? pw->getWorld() : nullptr;
    if (!w) return false;
    JPH::BodyID id = pw->bodyIdForTag(tag);
    if (id.IsInvalid() || !w->bodyExists(id)) return false;
    if (!config || !*config || std::string(config) == "null") {
        w->clearAreaOverride(id);
        return true;
    }
    if (!w->isSensor(id)) return false;
    auto res = ev::parseJson(config);
    if (res.thrown || !ev::isObject(res.value)) return false;

    physics::AreaOverride a;
    std::string err;
    if (!readAreaOverride(res.value, a, err)) return false;
    w->setAreaOverride(id, a);
    return true;
}

bool bro_physics_isActive(int32_t tag) {
    auto* pw = getActiveWorld();
    auto* w = pw ? pw->getWorld() : nullptr;
    if (!w) return false;
    JPH::BodyID id = pw->bodyIdForTag(tag);
    return id.IsInvalid() ? false : w->isActive(id);
}

void bro_physics_activate(int32_t tag) {
    auto* pw = getActiveWorld();
    auto* w = pw ? pw->getWorld() : nullptr;
    if (!w) return;
    JPH::BodyID id = pw->bodyIdForTag(tag);
    if (!id.IsInvalid()) w->activate(id);
}

void bro_physics_getAllTransforms(bool interpolated, bronze_native_buffer* out) {
    auto* pw = getActiveWorld();
    auto* w = pw ? pw->getWorld() : nullptr;
    if (!w || pw->bodyTags.empty()) {
        out->data = nullptr;
        out->length = 0;
        out->release = nullptr;
        return;
    }

    constexpr size_t stride = 8;
    size_t count = pw->bodyTags.size();
    tl_allTransformsBuf.resize(count * stride);

    size_t idx = 0;
    for (auto& [key, tag] : pw->bodyTags) {
        JPH::BodyID id(key);
        JPH::RVec3 pos;
        JPH::Quat rot;
        if (interpolated) {
            w->getRenderTransform(id, pos, rot);
        } else {
            pos = w->getPosition(id);
            rot = w->getRotation(id);
        }
        float* p = tl_allTransformsBuf.data() + idx * stride;
        p[0] = static_cast<float>(tag);
        p[1] = static_cast<float>(pos.GetX());
        p[2] = static_cast<float>(pos.GetY());
        p[3] = static_cast<float>(pos.GetZ());
        p[4] = rot.GetX();
        p[5] = rot.GetY();
        p[6] = rot.GetZ();
        p[7] = rot.GetW();
        idx++;
    }

    out->data = tl_allTransformsBuf.data();
    out->length = static_cast<uint32_t>(tl_allTransformsBuf.size());
    out->release = nullptr;
}

}  // extern "C"
