// native_physics_character.cpp — Physics character controller natives.

#include "bronze_host/native_physics_internal.h"
#include <sstream>

extern "C" {

using namespace bro;
using namespace bro::bronze_host;

void bro_physics_PhysicsCharacter_dtor(void* self) {
    auto* pc = static_cast<HostPhysicsCharacter*>(self);
    if (!pc) return;
    if (pc->handle) {
        HostPhysicsWorld* pw = pc->world ? pc->world : getActiveWorld();
        if (auto* w = pw ? pw->getWorld() : nullptr) {
            if (pc->innerTag >= 0) pw->unregisterBody(pc->innerTag);
            w->destroyCharacter(pc->handle);
        }
    }
    if (pc->world) pc->world->liveCharacters.erase(pc);
    delete pc;
}

void* bro_physics_PhysicsCharacter_ctor(void) {
    return nullptr;
}

void* bro_physics_createCharacter(const char* config) {
    auto* pw = getActiveWorld();
    auto* world = pw ? pw->getWorld() : nullptr;
    if (!world || !config || !*config) return nullptr;
    auto res = ev::parseJson(config);
    if (res.thrown || !ev::isObject(res.value)) return nullptr;
    const Rooted cfg(res.value);

    bro::physics::CharacterOptions opts;
    opts.position = readRVec3(ev::getProperty(cfg, "position"));
    opts.up = readVec3(ev::getProperty(cfg, "up"), JPH::Vec3(0, 1, 0));
    opts.radius = static_cast<float>(getPropNumber(cfg, "radius", opts.radius));
    opts.halfHeight = static_cast<float>(getPropNumber(cfg, "halfHeight", opts.halfHeight));
    opts.mass = static_cast<float>(getPropNumber(cfg, "mass", opts.mass));
    opts.maxSlopeAngle = static_cast<float>(getPropNumber(cfg, "maxSlopeAngle", opts.maxSlopeAngle));
    opts.maxStrength = static_cast<float>(getPropNumber(cfg, "maxStrength", opts.maxStrength));
    opts.padding = static_cast<float>(getPropNumber(cfg, "padding", opts.padding));
    opts.stepUp = static_cast<float>(getPropNumber(cfg, "stepUp", opts.stepUp));
    opts.stickToFloor = static_cast<float>(getPropNumber(cfg, "stickToFloor", opts.stickToFloor));
    opts.innerBody = getPropBool(cfg, "innerBody", false);

    Value layerVal = ev::getProperty(cfg, "layer");
    if (!ev::isUndefined(layerVal) && !ev::isNull(layerVal)) {
        if (!ev::isObject(layerVal)) {
            std::string s = ev::toUtf8(layerVal);
            int idx = 0;
            if (parseDecimalIndex(s, idx)) opts.layer = idx;
            else opts.layer = world->layerIndex(s);
        }
    }
    Value innerLayerVal = ev::getProperty(cfg, "innerBodyLayer");
    if (!ev::isUndefined(innerLayerVal) && !ev::isNull(innerLayerVal)) {
        if (!ev::isObject(innerLayerVal)) {
            std::string s = ev::toUtf8(innerLayerVal);
            int idx = 0;
            if (parseDecimalIndex(s, idx)) opts.innerBodyLayer = idx;
            else opts.innerBodyLayer = world->layerIndex(s);
        }
    }

    uint32_t handle = world->createCharacter(opts);
    if (!handle) return nullptr;

    auto* pc = new HostPhysicsCharacter();
    pc->world = pw;
    pc->handle = handle;
    if (opts.innerBody) {
        JPH::BodyID innerId = world->characterInnerBody(handle);
        if (!innerId.IsInvalid()) {
            pc->innerTag = pw->registerBody(innerId);
        }
    }
    pw->liveCharacters.insert(pc);
    return pc;
}

void bro_physics_PhysicsCharacter_setPosition(void* self, double x, double y, double z) {
    auto* pc = static_cast<HostPhysicsCharacter*>(self);
    if (!pc || !pc->handle) return;
    HostPhysicsWorld* pw = pc->world ? pc->world : getActiveWorld();
    if (auto* w = pw ? pw->getWorld() : nullptr) w->setCharacterPosition(pc->handle, JPH::RVec3(x, y, z));
}

void bro_physics_PhysicsCharacter_setVelocity(void* self, double x, double y, double z) {
    auto* pc = static_cast<HostPhysicsCharacter*>(self);
    if (!pc || !pc->handle) return;
    HostPhysicsWorld* pw = pc->world ? pc->world : getActiveWorld();
    if (auto* w = pw ? pw->getWorld() : nullptr) w->setCharacterVelocity(pc->handle, JPH::Vec3((float)x, (float)y, (float)z));
}

void bro_physics_PhysicsCharacter_setLinearVelocity(void* self, double x, double y, double z) {
    bro_physics_PhysicsCharacter_setVelocity(self, x, y, z);
}

const char* bro_physics_PhysicsCharacter_getPosition(void* self) {
    auto* pc = static_cast<HostPhysicsCharacter*>(self);
    if (!pc || !pc->handle) return natives::strResult("{\"x\":0,\"y\":0,\"z\":0}");
    HostPhysicsWorld* pw = pc->world ? pc->world : getActiveWorld();
    auto* w = pw ? pw->getWorld() : nullptr;
    if (!w) return natives::strResult("{\"x\":0,\"y\":0,\"z\":0}");

    bro::physics::CharacterState st;
    if (!w->getCharacterState(pc->handle, st)) return natives::strResult("{\"x\":0,\"y\":0,\"z\":0}");
    std::string s = "{\"x\":" + std::to_string(st.position.GetX()) +
                    ",\"y\":" + std::to_string(st.position.GetY()) +
                    ",\"z\":" + std::to_string(st.position.GetZ()) + "}";
    return natives::strResult(s);
}

const char* bro_physics_PhysicsCharacter_getVelocity(void* self) {
    auto* pc = static_cast<HostPhysicsCharacter*>(self);
    if (!pc || !pc->handle) return natives::strResult("{\"x\":0,\"y\":0,\"z\":0}");
    HostPhysicsWorld* pw = pc->world ? pc->world : getActiveWorld();
    auto* w = pw ? pw->getWorld() : nullptr;
    if (!w) return natives::strResult("{\"x\":0,\"y\":0,\"z\":0}");

    bro::physics::CharacterState st;
    if (!w->getCharacterState(pc->handle, st)) return natives::strResult("{\"x\":0,\"y\":0,\"z\":0}");
    std::string s = "{\"x\":" + std::to_string(st.velocity.GetX()) +
                    ",\"y\":" + std::to_string(st.velocity.GetY()) +
                    ",\"z\":" + std::to_string(st.velocity.GetZ()) + "}";
    return natives::strResult(s);
}

const char* bro_physics_PhysicsCharacter_getLinearVelocity(void* self) {
    return bro_physics_PhysicsCharacter_getVelocity(self);
}

const char* bro_physics_PhysicsCharacter_getState(void* self) {
    auto* pc = static_cast<HostPhysicsCharacter*>(self);
    if (!pc || !pc->handle) return natives::strResult("null");
    HostPhysicsWorld* pw = pc->world ? pc->world : getActiveWorld();
    auto* w = pw ? pw->getWorld() : nullptr;
    if (!w) return natives::strResult("null");

    bro::physics::CharacterState st;
    if (!w->getCharacterState(pc->handle, st)) return natives::strResult("null");

    std::string gState = "inAir";
    if (st.ground == bro::physics::CharacterGround::OnGround) gState = "onGround";
    else if (st.ground == bro::physics::CharacterGround::OnSteepGround) gState = "onSteepGround";
    else if (st.ground == bro::physics::CharacterGround::NotSupported) gState = "notSupported";

    int32_t groundTag = st.groundBody.IsInvalid() ? -1 : pw->tagForBodyId(st.groundBody);
    bool isGrounded = (st.ground == bro::physics::CharacterGround::OnGround);

    std::ostringstream ss;
    ss << "{\"groundState\":\"" << gState << "\""
       << ",\"isGrounded\":" << (isGrounded ? "true" : "false")
       << ",\"groundBody\":" << groundTag
       << ",\"groundBodyId\":" << groundTag
       << ",\"position\":{\"x\":" << st.position.GetX() << ",\"y\":" << st.position.GetY() << ",\"z\":" << st.position.GetZ() << "}"
       << ",\"velocity\":{\"x\":" << st.velocity.GetX() << ",\"y\":" << st.velocity.GetY() << ",\"z\":" << st.velocity.GetZ() << "}"
       << ",\"groundNormal\":{\"x\":" << st.groundNormal.GetX() << ",\"y\":" << st.groundNormal.GetY() << ",\"z\":" << st.groundNormal.GetZ() << "}"
       << ",\"groundVelocity\":{\"x\":" << st.groundVelocity.GetX() << ",\"y\":" << st.groundVelocity.GetY() << ",\"z\":" << st.groundVelocity.GetZ() << "}}";
    return natives::strResult(ss.str());
}

bool bro_physics_PhysicsCharacter_setShape(void* self, const char* shapeJson) {
    auto* pc = static_cast<HostPhysicsCharacter*>(self);
    if (!pc || !pc->handle || !shapeJson || !*shapeJson) return false;
    HostPhysicsWorld* pw = pc->world ? pc->world : getActiveWorld();
    auto* w = pw ? pw->getWorld() : nullptr;
    if (!w) return false;

    auto res = ev::parseJson(shapeJson);
    if (res.thrown || !ev::isObject(res.value)) return false;

    physics::BodyOptions shapeOpts;
    std::string err;
    if (!readBodyOptions(res.value, shapeOpts, err, w)) return false;

    return w->setCharacterShape(pc->handle, shapeOpts);
}

int32_t bro_physics_PhysicsCharacter_innerBody_get(void* self) {
    auto* pc = static_cast<HostPhysicsCharacter*>(self);
    return pc ? pc->innerTag : -1;
}

void bro_physics_PhysicsCharacter_update(void* /*self*/, double /*dt*/) {
    // Characters are updated inside world->step()
}

void bro_physics_PhysicsCharacter_destroy(void* self) {
    auto* pc = static_cast<HostPhysicsCharacter*>(self);
    if (!pc || !pc->handle) return;
    HostPhysicsWorld* pw = pc->world ? pc->world : getActiveWorld();
    if (auto* w = pw ? pw->getWorld() : nullptr) {
        if (pc->innerTag >= 0) pw->unregisterBody(pc->innerTag);
        w->destroyCharacter(pc->handle);
    }
    // Off the live set, so the world's destructor no longer clears this
    // pointer: drop it here (native_physics_softbody.cpp says why).
    if (pc->world) pc->world->liveCharacters.erase(pc);
    pc->world = nullptr;
    pc->handle = 0;
    pc->innerTag = -1;
}

}  // extern "C"

