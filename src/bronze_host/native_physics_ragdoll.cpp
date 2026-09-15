// native_physics_ragdoll.cpp — Physics ragdoll natives.

#include "bronze_host/native_physics_internal.h"

extern "C" {

using namespace bro::bronze_host;

void bro_physics_PhysicsRagdoll_dtor(void* self) {
    auto* pr = static_cast<HostPhysicsRagdoll*>(self);
    if (!pr) return;
    delete pr;
}

void* bro_physics_PhysicsRagdoll_ctor(void) {
    return nullptr;
}

void* bro_physics_createRagdoll(const char* config) {
    auto* pr = new HostPhysicsRagdoll();
    pr->world = &g_defaultWorld;
    return pr;
}

void bro_physics_PhysicsRagdoll_driveToPose(void* self, const char* pose, double dt) {}

const char* bro_physics_PhysicsRagdoll_getPose(void* self) {
    return natives::strResult("{\"parts\":[]}");
}

void bro_physics_PhysicsRagdoll_destroy(void* self) {
    auto* pr = static_cast<HostPhysicsRagdoll*>(self);
    if (!pr) return;
    pr->handle = 0;
}

}  // extern "C"
