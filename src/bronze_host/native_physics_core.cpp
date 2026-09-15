// native_physics_core.cpp — Physics core world, rigid body, and simulation natives.

#include "bronze_host/native_physics_internal.h"
#include <iomanip>
#include <sstream>

namespace bro::bronze_host {

HostPhysicsWorld g_defaultWorld;

bool registerNatives_physics(std::string* error);

bool registerPhysicsNatives(std::string* error) {
    return registerNatives_physics(error);
}

static thread_local std::vector<float> tl_allTransformsBuf;

}  // namespace bro::bronze_host

extern "C" {

using namespace bro;
using namespace bro::bronze_host;

// --- PhysicsWorldHandle -----------------------------------------------------

void bro_physics_PhysicsWorldHandle_dtor(void* self) {
    auto* pw = static_cast<HostPhysicsWorld*>(self);
    if (pw && pw != &g_defaultWorld) delete pw;
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
        if (dt > 0.0) pw->getWorld()->setTimeStep(static_cast<float>(dt));
        pw->getWorld()->stepInline();
    }
}

void* bro_physics_createWorldHandle(const char* /*opts*/) {
    auto* pw = new HostPhysicsWorld();
    pw->world = new physics::PhysicsWorld();
    pw->ownsWorld = true;
    return pw;
}

void bro_physics_createWorld(const char* /*opts*/) {
    g_defaultWorld.clear();
    if (auto* w = g_defaultWorld.getWorld()) w->destroyAll();
}

// --- Simulation Timing & Gravity --------------------------------------------

void bro_physics_setTimeStep(double dt) {
    if (auto* w = g_defaultWorld.getWorld()) w->setTimeStep(static_cast<float>(dt));
}

void bro_physics_step(double dt) {
    if (auto* w = g_defaultWorld.getWorld()) {
        if (dt > 0.0) w->setTimeStep(static_cast<float>(dt));
        w->stepInline();
        g_defaultWorld.lastContactEvents = w->drainContactEvents(&g_defaultWorld.lastContactOverflow);
    }
}

void bro_physics_setInterpolation(bool enabled) {
    if (auto* w = g_defaultWorld.getWorld()) w->setInterpolation(enabled);
}

bool bro_physics_getInterpolation(void) {
    auto* w = g_defaultWorld.getWorld();
    return w ? w->interpolation() : false;
}

void bro_physics_setGravity(double x, double y, double z) {
    if (auto* w = g_defaultWorld.getWorld()) w->setGravity((float)x, (float)y, (float)z);
}

const char* bro_physics_getGravity(void) {
    auto* w = g_defaultWorld.getWorld();
    JPH::Vec3 g = w ? w->gravity() : JPH::Vec3(0, -9.81f, 0);
    std::string s = "{\"x\":" + std::to_string(g.GetX()) +
                    ",\"y\":" + std::to_string(g.GetY()) +
                    ",\"z\":" + std::to_string(g.GetZ()) + "}";
    return natives::strResult(s);
}

bool bro_physics_setLayers(const char* config) {
    auto* w = g_defaultWorld.getWorld();
    if (!w || !config || !*config) return false;
    auto res = ev::parseJson(config);
    if (res.thrown || !ev::isObject(res.value)) return false;

    Value namesVal = ev::getProperty(res.value, "names");
    Value matVal = ev::getProperty(res.value, "matrix");
    std::vector<std::string> names;
    if (ev::isObject(namesVal)) {
        Value lenV = ev::getProperty(namesVal, "length");
        if (ev::isNumber(lenV)) {
            uint32_t n = static_cast<uint32_t>(ev::toDouble(lenV));
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
            uint32_t n = static_cast<uint32_t>(ev::toDouble(lenV));
            for (uint32_t i = 0; i < n; ++i) {
                matrix.push_back(ev::toBool(ev::getElement(matVal, i)));
            }
        }
    }
    return w->configureLayers(names, matrix);
}

// --- Body Lifecycle ---------------------------------------------------------

int32_t bro_physics_createBody(const char* config) {
    auto* w = g_defaultWorld.getWorld();
    if (!w || !config || !*config) return -1;
    auto res = ev::parseJson(config);
    if (res.thrown || !ev::isObject(res.value)) return -1;

    physics::BodyOptions opts;
    std::string err;
    if (!readBodyOptions(res.value, opts, err, w)) return -1;

    JPH::BodyID id = w->createBody(opts);
    if (id.IsInvalid()) return -1;
    return g_defaultWorld.registerBody(id);
}

void bro_physics_destroyBody(int32_t tag) {
    auto* w = g_defaultWorld.getWorld();
    if (!w) return;
    JPH::BodyID id = g_defaultWorld.bodyIdForTag(tag);
    if (id.IsInvalid()) return;
    w->destroyBody(id, [](JPH::BodyID bid) { g_defaultWorld.unregisterBodyId(bid); });
    g_defaultWorld.unregisterBody(tag);
}

void bro_physics_destroyAll(void) {
    auto* w = g_defaultWorld.getWorld();
    if (w) {
        w->destroyAll();
        g_defaultWorld.clear();
    }
}

// --- Transforms & Properties ------------------------------------------------

const char* bro_physics_getTransform(int32_t tag) {
    auto* w = g_defaultWorld.getWorld();
    if (!w) return natives::strResult("null");
    JPH::BodyID id = g_defaultWorld.bodyIdForTag(tag);
    if (id.IsInvalid() || !w->bodyExists(id)) return natives::strResult("null");

    JPH::RVec3 pos = w->getPosition(id);
    JPH::Quat rot = w->getRotation(id);
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
    auto* w = g_defaultWorld.getWorld();
    if (!w) return natives::strResult("null");
    JPH::BodyID id = g_defaultWorld.bodyIdForTag(tag);
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
    auto* w = g_defaultWorld.getWorld();
    if (!w) return;
    JPH::BodyID id = g_defaultWorld.bodyIdForTag(tag);
    if (!id.IsInvalid()) w->setPosition(id, JPH::RVec3(x, y, z));
}

void bro_physics_setRotation(int32_t tag, double x, double y, double z, double w) {
    auto* world = g_defaultWorld.getWorld();
    if (!world) return;
    JPH::BodyID id = g_defaultWorld.bodyIdForTag(tag);
    if (!id.IsInvalid()) world->setRotation(id, JPH::Quat((float)x, (float)y, (float)z, (float)w));
}

void bro_physics_setLinearVelocity(int32_t tag, double x, double y, double z) {
    auto* w = g_defaultWorld.getWorld();
    if (!w) return;
    JPH::BodyID id = g_defaultWorld.bodyIdForTag(tag);
    if (!id.IsInvalid()) w->setLinearVelocity(id, JPH::Vec3((float)x, (float)y, (float)z));
}

void bro_physics_setAngularVelocity(int32_t tag, double x, double y, double z) {
    auto* w = g_defaultWorld.getWorld();
    if (!w) return;
    JPH::BodyID id = g_defaultWorld.bodyIdForTag(tag);
    if (!id.IsInvalid()) w->setAngularVelocity(id, JPH::Vec3((float)x, (float)y, (float)z));
}

void bro_physics_addForce(int32_t tag, double x, double y, double z) {
    auto* w = g_defaultWorld.getWorld();
    if (!w) return;
    JPH::BodyID id = g_defaultWorld.bodyIdForTag(tag);
    if (!id.IsInvalid()) w->addForce(id, JPH::Vec3((float)x, (float)y, (float)z));
}

void bro_physics_addImpulse(int32_t tag, double x, double y, double z) {
    auto* w = g_defaultWorld.getWorld();
    if (!w) return;
    JPH::BodyID id = g_defaultWorld.bodyIdForTag(tag);
    if (!id.IsInvalid()) w->addImpulse(id, JPH::Vec3((float)x, (float)y, (float)z));
}

void bro_physics_addTorque(int32_t tag, double x, double y, double z) {
    auto* w = g_defaultWorld.getWorld();
    if (!w) return;
    JPH::BodyID id = g_defaultWorld.bodyIdForTag(tag);
    if (!id.IsInvalid()) w->addTorque(id, JPH::Vec3((float)x, (float)y, (float)z));
}

void bro_physics_setUserData(int32_t tag, double data) {
    auto* w = g_defaultWorld.getWorld();
    if (!w) return;
    JPH::BodyID id = g_defaultWorld.bodyIdForTag(tag);
    if (!id.IsInvalid()) w->setUserData(id, static_cast<uint64_t>(data));
}

double bro_physics_getUserData(int32_t tag) {
    auto* w = g_defaultWorld.getWorld();
    if (!w) return 0.0;
    JPH::BodyID id = g_defaultWorld.bodyIdForTag(tag);
    return id.IsInvalid() ? 0.0 : static_cast<double>(w->getUserData(id));
}

void bro_physics_setLayer(int32_t tag, int32_t layer) {
    auto* w = g_defaultWorld.getWorld();
    if (!w) return;
    JPH::BodyID id = g_defaultWorld.bodyIdForTag(tag);
    if (!id.IsInvalid()) w->setLayer(id, layer);
}

void bro_physics_setKinematic(int32_t tag) {
    auto* w = g_defaultWorld.getWorld();
    if (!w) return;
    JPH::BodyID id = g_defaultWorld.bodyIdForTag(tag);
    if (!id.IsInvalid()) w->setKinematic(id);
}

void bro_physics_moveKinematic(int32_t tag, double x, double y, double z, double dt) {
    auto* w = g_defaultWorld.getWorld();
    if (!w) return;
    JPH::BodyID id = g_defaultWorld.bodyIdForTag(tag);
    if (!id.IsInvalid()) {
        JPH::Quat rot = w->getRotation(id);
        w->moveKinematic(id, JPH::RVec3(x, y, z), rot, (float)dt);
    }
}

void bro_physics_setFrictionCombine(int32_t tag, const char* mode) {
    auto* w = g_defaultWorld.getWorld();
    if (!w || !mode) return;
    JPH::BodyID id = g_defaultWorld.bodyIdForTag(tag);
    physics::CombineMode cm;
    if (!id.IsInvalid() && parseCombineMode(mode, cm)) w->setFrictionCombine(id, cm);
}

void bro_physics_setRestitutionCombine(int32_t tag, const char* mode) {
    auto* w = g_defaultWorld.getWorld();
    if (!w || !mode) return;
    JPH::BodyID id = g_defaultWorld.bodyIdForTag(tag);
    physics::CombineMode cm;
    if (!id.IsInvalid() && parseCombineMode(mode, cm)) w->setRestitutionCombine(id, cm);
}

double bro_physics_getMass(int32_t tag) {
    auto* w = g_defaultWorld.getWorld();
    if (!w) return 0.0;
    JPH::BodyID id = g_defaultWorld.bodyIdForTag(tag);
    return id.IsInvalid() ? 0.0 : (double)w->getMass(id);
}

void bro_physics_setMass(int32_t tag, double mass) {
    auto* w = g_defaultWorld.getWorld();
    if (!w) return;
    JPH::BodyID id = g_defaultWorld.bodyIdForTag(tag);
    if (!id.IsInvalid()) w->setMass(id, (float)mass);
}

void bro_physics_setLinearDamping(int32_t tag, double damping) {
    auto* w = g_defaultWorld.getWorld();
    if (!w) return;
    JPH::BodyID id = g_defaultWorld.bodyIdForTag(tag);
    if (!id.IsInvalid()) w->setLinearDamping(id, (float)damping);
}

void bro_physics_setAngularDamping(int32_t tag, double damping) {
    auto* w = g_defaultWorld.getWorld();
    if (!w) return;
    JPH::BodyID id = g_defaultWorld.bodyIdForTag(tag);
    if (!id.IsInvalid()) w->setAngularDamping(id, (float)damping);
}

void bro_physics_setGravityFactor(int32_t tag, double factor) {
    auto* w = g_defaultWorld.getWorld();
    if (!w) return;
    JPH::BodyID id = g_defaultWorld.bodyIdForTag(tag);
    if (!id.IsInvalid()) w->setGravityFactor(id, (float)factor);
}

void bro_physics_setFriction(int32_t tag, double friction) {
    auto* w = g_defaultWorld.getWorld();
    if (!w) return;
    JPH::BodyID id = g_defaultWorld.bodyIdForTag(tag);
    if (!id.IsInvalid()) w->setFriction(id, (float)friction);
}

void bro_physics_setRestitution(int32_t tag, double restitution) {
    auto* w = g_defaultWorld.getWorld();
    if (!w) return;
    JPH::BodyID id = g_defaultWorld.bodyIdForTag(tag);
    if (!id.IsInvalid()) w->setRestitution(id, (float)restitution);
}

const char* bro_physics_getBodyProperties(int32_t tag) {
    auto* w = g_defaultWorld.getWorld();
    if (!w) return natives::strResult("null");
    JPH::BodyID id = g_defaultWorld.bodyIdForTag(tag);
    if (id.IsInvalid() || !w->bodyExists(id)) return natives::strResult("null");

    float mass = w->getMass(id);
    float friction = w->getFriction(id);
    float restitution = w->getRestitution(id);

    std::string s = "{\"mass\":" + std::to_string(mass) +
                    ",\"friction\":" + std::to_string(friction) +
                    ",\"restitution\":" + std::to_string(restitution) + "}";
    return natives::strResult(s);
}

void bro_physics_setAreaOverride(int32_t tag, const char* config) {
    auto* w = g_defaultWorld.getWorld();
    if (!w || !config || !*config) return;
    JPH::BodyID id = g_defaultWorld.bodyIdForTag(tag);
    if (id.IsInvalid()) return;
    auto res = ev::parseJson(config);
    if (res.thrown || !ev::isObject(res.value)) return;

    physics::AreaOverride a;
    std::string err;
    if (readAreaOverride(res.value, a, err)) w->setAreaOverride(id, a);
}

bool bro_physics_isActive(int32_t tag) {
    auto* w = g_defaultWorld.getWorld();
    if (!w) return false;
    JPH::BodyID id = g_defaultWorld.bodyIdForTag(tag);
    return id.IsInvalid() ? false : w->isActive(id);
}

void bro_physics_activate(int32_t tag) {
    auto* w = g_defaultWorld.getWorld();
    if (!w) return;
    JPH::BodyID id = g_defaultWorld.bodyIdForTag(tag);
    if (!id.IsInvalid()) w->activate(id);
}

void bro_physics_getAllTransforms(bool worldHandle_given, int32_t worldHandle, bronze_native_buffer* out) {
    auto* w = g_defaultWorld.getWorld();
    if (!w || g_defaultWorld.bodyTags.empty()) {
        out->data = nullptr;
        out->length = 0;
        out->release = nullptr;
        return;
    }

    constexpr size_t stride = 8;
    size_t count = g_defaultWorld.bodyTags.size();
    tl_allTransformsBuf.resize(count * stride);

    size_t idx = 0;
    auto& bi = w->getBodyInterface();
    for (auto& [key, tag] : g_defaultWorld.bodyTags) {
        JPH::BodyID id(key);
        JPH::RVec3 pos = bi.GetPosition(id);
        JPH::Quat rot = bi.GetRotation(id);
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
