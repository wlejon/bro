// native_physics_softbody.cpp — Physics soft body and cloth natives.

#include "bronze_host/native_physics_internal.h"

namespace bro::bronze_host {

namespace {

static thread_local std::vector<float> tl_softVerts;

}  // namespace

}  // namespace bro::bronze_host

extern "C" {

using namespace bro;
using namespace bro::bronze_host;

void bro_physics_PhysicsSoftBody_dtor(void* self) {
    auto* sb = static_cast<HostPhysicsSoftBody*>(self);
    if (!sb) return;
    if (sb->handle) {
        HostPhysicsWorld* pw = sb->world ? sb->world : &g_defaultWorld;
        if (auto* w = pw->getWorld()) {
            w->destroySoftBody(sb->handle);
            pw->unregisterBody(sb->bodyTag);
        }
    }
    delete sb;
}

void* bro_physics_PhysicsSoftBody_ctor(void) {
    return nullptr;
}

void* bro_physics_createSoftBody(const char* config) {
    auto* pw = &g_defaultWorld;
    auto* world = pw->getWorld();
    if (!world || !config || !*config) return nullptr;
    auto res = ev::parseJson(config);
    if (res.thrown || !ev::isObject(res.value)) return nullptr;

    physics::SoftBodyOptions sopts;
    Value clothV = ev::getProperty(res.value, "cloth");
    Value posV = ev::getProperty(res.value, "position");
    sopts.position = readRVec3(posV);

    int gx = 0, gz = 0;
    if (ev::isObject(clothV)) {
        sopts.kind = physics::SoftBodyOptions::Cloth;
        gx = static_cast<int>(getPropNumber(clothV, "gridX", 4));
        gz = static_cast<int>(getPropNumber(clothV, "gridZ", 4));
        sopts.gridX = gx;
        sopts.gridZ = gz;
        sopts.spacing = static_cast<float>(getPropNumber(clothV, "spacing", 0.5));
        sopts.mass = static_cast<float>(getPropNumber(clothV, "mass", 1.0));
        std::string pinned = getPropString(clothV, "pinned");
        if (pinned == "corners" && gx >= 2 && gz >= 2) {
            sopts.pinned.push_back(0);
            sopts.pinned.push_back(gx - 1);
            sopts.pinned.push_back(gx * (gz - 1));
            sopts.pinned.push_back(gx * gz - 1);
        }
    }

    uint32_t handle = world->createSoftBody(sopts);
    if (!handle) return nullptr;

    auto* sb = new HostPhysicsSoftBody();
    sb->world = pw;
    sb->handle = handle;
    sb->gridX = gx;
    sb->gridZ = gz;
    JPH::BodyID bid = world->softBodyBody(handle);
    if (!bid.IsInvalid()) sb->bodyTag = pw->registerBody(bid);
    pw->liveSoftBodies.insert(sb);
    return sb;
}

int32_t bro_physics_PhysicsSoftBody_vertexCount_get(void* self) {
    auto* sb = static_cast<HostPhysicsSoftBody*>(self);
    if (!sb || !sb->handle) return 0;
    HostPhysicsWorld* pw = sb->world ? sb->world : &g_defaultWorld;
    auto* w = pw->getWorld();
    int count = w ? w->softBodyVertexCount(sb->handle) : 0;
    return count < 0 ? 0 : count;
}

const char* bro_physics_PhysicsSoftBody_topology(void* self) {
    auto* sb = static_cast<HostPhysicsSoftBody*>(self);
    if (!sb || !sb->handle) return natives::strResult("null");
    HostPhysicsWorld* pw = sb->world ? sb->world : &g_defaultWorld;
    auto* w = pw->getWorld();
    if (!w) return natives::strResult("null");

    std::string s = "{\"gridX\":" + std::to_string(sb->gridX) +
                    ",\"gridZ\":" + std::to_string(sb->gridZ) + "}";
    return natives::strResult(s);
}

void bro_physics_PhysicsSoftBody_vertices(void* self, bronze_native_buffer* out) {
    auto* sb = static_cast<HostPhysicsSoftBody*>(self);
    if (!sb || !sb->handle) {
        out->data = nullptr;
        out->length = 0;
        out->release = nullptr;
        return;
    }
    HostPhysicsWorld* pw = sb->world ? sb->world : &g_defaultWorld;
    auto* w = pw->getWorld();
    if (!w || !w->getSoftBodyVertices(sb->handle, tl_softVerts)) {
        out->data = nullptr;
        out->length = 0;
        out->release = nullptr;
        return;
    }
    out->data = tl_softVerts.data();
    out->length = static_cast<uint32_t>(tl_softVerts.size());
    out->release = nullptr;
}

bool bro_physics_PhysicsSoftBody_pin(void* self, int32_t index, bool pinned) {
    auto* sb = static_cast<HostPhysicsSoftBody*>(self);
    if (!sb || !sb->handle) return false;
    HostPhysicsWorld* pw = sb->world ? sb->world : &g_defaultWorld;
    auto* w = pw->getWorld();
    return w ? w->pinSoftBodyVertex(sb->handle, index, pinned) : false;
}

bool bro_physics_PhysicsSoftBody_setVertex(void* self, int32_t index, double x, double y, double z) {
    auto* sb = static_cast<HostPhysicsSoftBody*>(self);
    if (!sb || !sb->handle) return false;
    HostPhysicsWorld* pw = sb->world ? sb->world : &g_defaultWorld;
    auto* w = pw->getWorld();
    return w ? w->setSoftBodyVertexPosition(sb->handle, index, JPH::RVec3(x, y, z)) : false;
}

bool bro_physics_PhysicsSoftBody_setVertexVelocity(void* self, int32_t index, double x, double y, double z) {
    auto* sb = static_cast<HostPhysicsSoftBody*>(self);
    if (!sb || !sb->handle) return false;
    HostPhysicsWorld* pw = sb->world ? sb->world : &g_defaultWorld;
    auto* w = pw->getWorld();
    return w ? w->setSoftBodyVertexVelocity(sb->handle, index, JPH::Vec3((float)x, (float)y, (float)z)) : false;
}

const char* bro_physics_PhysicsSoftBody_getBounds(void* self) {
    return natives::strResult("{\"min\":{\"x\":0,\"y\":0,\"z\":0},\"max\":{\"x\":0,\"y\":0,\"z\":0}}");
}

void bro_physics_PhysicsSoftBody_destroy(void* self) {
    auto* sb = static_cast<HostPhysicsSoftBody*>(self);
    if (!sb || !sb->handle) return;
    HostPhysicsWorld* pw = sb->world ? sb->world : &g_defaultWorld;
    if (auto* w = pw->getWorld()) {
        w->destroySoftBody(sb->handle);
        pw->unregisterBody(sb->bodyTag);
    }
    if (sb->world) sb->world->liveSoftBodies.erase(sb);
    sb->handle = 0;
    sb->bodyTag = -1;
}

}  // extern "C"
