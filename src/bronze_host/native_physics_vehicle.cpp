// native_physics_vehicle.cpp — Physics vehicle natives.

#include "bronze_host/native_physics_internal.h"

extern "C" {

using namespace bro::bronze_host;

void bro_physics_PhysicsVehicle_dtor(void* self) {
    auto* pv = static_cast<HostPhysicsVehicle*>(self);
    if (!pv) return;
    delete pv;
}

void* bro_physics_PhysicsVehicle_ctor(void) {
    return nullptr;
}

void* bro_physics_createVehicle(const char* config) {
    auto* pv = new HostPhysicsVehicle();
    pv->world = &g_defaultWorld;
    return pv;
}

void bro_physics_PhysicsVehicle_setDriverInput(void* self, double forward, double steer, double brake, double handBrake) {}

const char* bro_physics_PhysicsVehicle_getTransform(void* self) {
    return natives::strResult("{\"position\":{\"x\":0,\"y\":0,\"z\":0},\"rotation\":{\"x\":0,\"y\":0,\"z\":0,\"w\":1}}");
}

void bro_physics_PhysicsVehicle_destroy(void* self) {
    auto* pv = static_cast<HostPhysicsVehicle*>(self);
    if (!pv) return;
    pv->handle = 0;
}

}  // extern "C"
