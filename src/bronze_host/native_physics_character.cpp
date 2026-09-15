// native_physics_character.cpp — Physics character controller natives.

#include "bronze_host/native_physics_internal.h"

extern "C" {

using namespace bro::bronze_host;

void bro_physics_PhysicsCharacter_dtor(void* self) {
    auto* pc = static_cast<HostPhysicsCharacter*>(self);
    if (!pc) return;
    if (pc->handle) {
        HostPhysicsWorld* pw = pc->world ? pc->world : &g_defaultWorld;
        if (auto* w = pw->getWorld()) {
            if (pc->innerTag >= 0) pw->unregisterBody(pc->innerTag);
            w->destroyCharacter(pc->handle);
        }
    }
    delete pc;
}

void* bro_physics_PhysicsCharacter_ctor(void) {
    return nullptr;
}

void* bro_physics_createCharacter(const char* config) {
    auto* pw = &g_defaultWorld;
    auto* world = pw->getWorld();
    if (!world || !config || !*config) return nullptr;
    auto res = ev::parseJson(config);
    if (res.thrown || !ev::isObject(res.value)) return nullptr;

    bro::physics::CharacterOptions opts;
    opts.position = readRVec3(ev::getProperty(res.value, "position"));
    opts.radius = static_cast<float>(getPropNumber(res.value, "radius", opts.radius));
    opts.halfHeight = static_cast<float>(getPropNumber(res.value, "halfHeight", opts.halfHeight));
    opts.mass = static_cast<float>(getPropNumber(res.value, "mass", opts.mass));

    uint32_t handle = world->createCharacter(opts);
    if (!handle) return nullptr;

    auto* pc = new HostPhysicsCharacter();
    pc->world = pw;
    pc->handle = handle;
    pw->liveCharacters.insert(pc);
    return pc;
}

void bro_physics_PhysicsCharacter_setPosition(void* self, double x, double y, double z) {
    auto* pc = static_cast<HostPhysicsCharacter*>(self);
    if (!pc || !pc->handle) return;
    HostPhysicsWorld* pw = pc->world ? pc->world : &g_defaultWorld;
    if (auto* w = pw->getWorld()) w->setCharacterPosition(pc->handle, JPH::RVec3(x, y, z));
}

void bro_physics_PhysicsCharacter_setVelocity(void* self, double x, double y, double z) {
    auto* pc = static_cast<HostPhysicsCharacter*>(self);
    if (!pc || !pc->handle) return;
    HostPhysicsWorld* pw = pc->world ? pc->world : &g_defaultWorld;
    if (auto* w = pw->getWorld()) w->setCharacterVelocity(pc->handle, JPH::Vec3((float)x, (float)y, (float)z));
}

void bro_physics_PhysicsCharacter_setLinearVelocity(void* self, double x, double y, double z) {
    bro_physics_PhysicsCharacter_setVelocity(self, x, y, z);
}

const char* bro_physics_PhysicsCharacter_getPosition(void* self) {
    auto* pc = static_cast<HostPhysicsCharacter*>(self);
    if (!pc || !pc->handle) return natives::strResult("{\"x\":0,\"y\":0,\"z\":0}");
    HostPhysicsWorld* pw = pc->world ? pc->world : &g_defaultWorld;
    auto* w = pw->getWorld();
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
    HostPhysicsWorld* pw = pc->world ? pc->world : &g_defaultWorld;
    auto* w = pw->getWorld();
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
    if (!pc || !pc->handle) return natives::strResult("{\"groundState\":\"inAir\"}");
    HostPhysicsWorld* pw = pc->world ? pc->world : &g_defaultWorld;
    auto* w = pw->getWorld();
    if (!w) return natives::strResult("{\"groundState\":\"inAir\"}");

    bro::physics::CharacterState st;
    if (!w->getCharacterState(pc->handle, st)) return natives::strResult("{\"groundState\":\"inAir\"}");

    std::string gState = "inAir";
    if (st.ground == bro::physics::CharacterGround::OnGround) gState = "onGround";
    else if (st.ground == bro::physics::CharacterGround::OnSteepGround) gState = "steepSlope";
    else if (st.ground == bro::physics::CharacterGround::NotSupported) gState = "notSupported";

    int32_t groundTag = st.groundBody.IsInvalid() ? -1 : pw->tagForBodyId(st.groundBody);

    std::string s = "{\"groundState\":\"" + gState + "\"," +
                    "\"groundBody\":" + std::to_string(groundTag) + "," +
                    "\"position\":{\"x\":" + std::to_string(st.position.GetX()) +
                    ",\"y\":" + std::to_string(st.position.GetY()) +
                    ",\"z\":" + std::to_string(st.position.GetZ()) + "}," +
                    "\"velocity\":{\"x\":" + std::to_string(st.velocity.GetX()) +
                    ",\"y\":" + std::to_string(st.velocity.GetY()) +
                    ",\"z\":" + std::to_string(st.velocity.GetZ()) + "}}";
    return natives::strResult(s);
}

void bro_physics_PhysicsCharacter_update(void* /*self*/, double /*dt*/) {
    // Characters are updated inside world->step()
}

void bro_physics_PhysicsCharacter_destroy(void* self) {
    auto* pc = static_cast<HostPhysicsCharacter*>(self);
    if (!pc || !pc->handle) return;
    HostPhysicsWorld* pw = pc->world ? pc->world : &g_defaultWorld;
    if (auto* w = pw->getWorld()) {
        if (pc->innerTag >= 0) pw->unregisterBody(pc->innerTag);
        w->destroyCharacter(pc->handle);
    }
    if (pc->world) pc->world->liveCharacters.erase(pc);
    pc->handle = 0;
    pc->innerTag = -1;
}

}  // extern "C"
