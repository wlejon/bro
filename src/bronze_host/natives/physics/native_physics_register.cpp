// native_physics_register.cpp — registers every native of native_physics_decl.h with
// bronze (embed::registerNative) under __bro_native.physics. Hand-maintained: each
// registration's signature matches its prototype there. Call registerNatives_physics from registerBroNatives
// (src/bronze_host/host_natives.h) on the thread that runs the program.
//
// Class members are Function-kind natives taking the handle first (the wrapper's
// receiver is dynamic, so a Method-kind registration would be unreachable from it);
// each class has one Constructor-kind registration, which mints the class, and its
// prototype is published as __bro_native.physics.<Class>Proto for js/physics.js to chain.

#include "native_physics_decl.h"
#include "bronze_host/native_table.h"
#include "bronze_host/host_natives.h"

#include "embed/embed.h"

#include <initializer_list>
#include <string>

namespace bro::bronze_host {

namespace {

namespace ev = bronze::embed;

ev::NativeSignature sig(const char* ret, std::initializer_list<const char*> params, ev::NativeKind kind) {
    ev::NativeSignature s;
    s.returnType = ret;
    for (const char* p : params) s.paramTypes.emplace_back(p);
    s.kind = kind;
    return s;
}

bool fn(const char* path, void* f, const char* ret, std::initializer_list<const char*> params, std::string* error) {
    return ev::registerNative(path, f, sig(ret, params, ev::NativeKind::Function), error);
}

bool ctor(const char* path, void* f, ev::HandleDestructor dtor, bronze::runtime::Finalize finalize,
          std::initializer_list<const char*> params, std::string* error) {
    ev::NativeSignature s = sig(path, params, ev::NativeKind::Constructor);
    s.className = path;
    s.destructor = dtor;
    s.finalize = finalize;
    return ev::registerNative(path, f, s, error);
}

// The prototype every instance of a class is born on, published where the
// wrapper reads it. Skipped when the roots are not there (bro-native-manifest
// registers without them; the manifest does not need the prototype).
void publishPrototype(const char* ns, const char* className, const char* protoName) {
    ev::GlobalValue root = ev::globalValue("__bro_native");
    if (!root.found || !ev::isObject(root.value)) return;
    ev::GlobalValue proto = ev::nativeClassPrototype(className);
    if (!proto.found) return;
    // Each value rooted before the next allocating call (bronze embed GC contract).
    ev::Persistent rootSlot(root.value);
    ev::Persistent protoSlot(proto.value);
    ev::Persistent nsSlot(ev::getProperty(rootSlot.get(), ns));
    if (!ev::isObject(nsSlot.get())) return;
    ev::setProperty(nsSlot.get(), protoName, protoSlot.get());
}

}  // namespace

bool registerNatives_physics(std::string* error) {
    const bool ok =
        ctor("__bro_native.physics.PhysicsWorldHandle", BRO_NATIVE_FN(bro_physics_PhysicsWorldHandle_ctor), BRO_NATIVE_DTOR(bro_physics_PhysicsWorldHandle_dtor), bronze::runtime::Finalize::InSweep, {}, error) &&
        ctor("__bro_native.physics.PhysicsCharacter", BRO_NATIVE_FN(bro_physics_PhysicsCharacter_ctor), BRO_NATIVE_DTOR(bro_physics_PhysicsCharacter_dtor), bronze::runtime::Finalize::InSweep, {}, error) &&
        ctor("__bro_native.physics.PhysicsVehicle", BRO_NATIVE_FN(bro_physics_PhysicsVehicle_ctor), BRO_NATIVE_DTOR(bro_physics_PhysicsVehicle_dtor), bronze::runtime::Finalize::InSweep, {}, error) &&
        ctor("__bro_native.physics.PhysicsRagdoll", BRO_NATIVE_FN(bro_physics_PhysicsRagdoll_ctor), BRO_NATIVE_DTOR(bro_physics_PhysicsRagdoll_dtor), bronze::runtime::Finalize::InSweep, {}, error) &&
        ctor("__bro_native.physics.PhysicsSoftBody", BRO_NATIVE_FN(bro_physics_PhysicsSoftBody_ctor), BRO_NATIVE_DTOR(bro_physics_PhysicsSoftBody_dtor), bronze::runtime::Finalize::InSweep, {}, error) &&
        fn("__bro_native.physics.PhysicsWorldHandle_destroy", BRO_NATIVE_FN(bro_physics_PhysicsWorldHandle_destroy), "void", {"__bro_native.physics.PhysicsWorldHandle"}, error) &&
        fn("__bro_native.physics.PhysicsWorldHandle_step", BRO_NATIVE_FN(bro_physics_PhysicsWorldHandle_step), "void", {"__bro_native.physics.PhysicsWorldHandle", "f64"}, error) &&
        fn("__bro_native.physics.PhysicsWorldHandle_enter", BRO_NATIVE_FN(bro_physics_PhysicsWorldHandle_enter), "void", {"__bro_native.physics.PhysicsWorldHandle"}, error) &&
        fn("__bro_native.physics.PhysicsWorldHandle_exit", BRO_NATIVE_FN(bro_physics_PhysicsWorldHandle_exit), "void", {"__bro_native.physics.PhysicsWorldHandle"}, error) &&
        fn("__bro_native.physics.PhysicsCharacter_setPosition", BRO_NATIVE_FN(bro_physics_PhysicsCharacter_setPosition), "void", {"__bro_native.physics.PhysicsCharacter", "f64", "f64", "f64"}, error) &&
        fn("__bro_native.physics.PhysicsCharacter_setVelocity", BRO_NATIVE_FN(bro_physics_PhysicsCharacter_setVelocity), "void", {"__bro_native.physics.PhysicsCharacter", "f64", "f64", "f64"}, error) &&
        fn("__bro_native.physics.PhysicsCharacter_setLinearVelocity", BRO_NATIVE_FN(bro_physics_PhysicsCharacter_setLinearVelocity), "void", {"__bro_native.physics.PhysicsCharacter", "f64", "f64", "f64"}, error) &&
        fn("__bro_native.physics.PhysicsCharacter_getPosition", BRO_NATIVE_FN(bro_physics_PhysicsCharacter_getPosition), "str", {"__bro_native.physics.PhysicsCharacter"}, error) &&
        fn("__bro_native.physics.PhysicsCharacter_getVelocity", BRO_NATIVE_FN(bro_physics_PhysicsCharacter_getVelocity), "str", {"__bro_native.physics.PhysicsCharacter"}, error) &&
        fn("__bro_native.physics.PhysicsCharacter_getLinearVelocity", BRO_NATIVE_FN(bro_physics_PhysicsCharacter_getLinearVelocity), "str", {"__bro_native.physics.PhysicsCharacter"}, error) &&
        fn("__bro_native.physics.PhysicsCharacter_getState", BRO_NATIVE_FN(bro_physics_PhysicsCharacter_getState), "str", {"__bro_native.physics.PhysicsCharacter"}, error) &&
        fn("__bro_native.physics.PhysicsCharacter_setShape", BRO_NATIVE_FN(bro_physics_PhysicsCharacter_setShape), "bool", {"__bro_native.physics.PhysicsCharacter", "str"}, error) &&
        fn("__bro_native.physics.PhysicsCharacter_innerBody_get", BRO_NATIVE_FN(bro_physics_PhysicsCharacter_innerBody_get), "i32", {"__bro_native.physics.PhysicsCharacter"}, error) &&
        fn("__bro_native.physics.PhysicsCharacter_update", BRO_NATIVE_FN(bro_physics_PhysicsCharacter_update), "void", {"__bro_native.physics.PhysicsCharacter", "f64"}, error) &&
        fn("__bro_native.physics.PhysicsCharacter_destroy", BRO_NATIVE_FN(bro_physics_PhysicsCharacter_destroy), "void", {"__bro_native.physics.PhysicsCharacter"}, error) &&
        fn("__bro_native.physics.PhysicsVehicle_setDriverInput", BRO_NATIVE_FN(bro_physics_PhysicsVehicle_setDriverInput), "void", {"__bro_native.physics.PhysicsVehicle", "f64", "f64", "f64", "f64"}, error) &&
        fn("__bro_native.physics.PhysicsVehicle_setInput", BRO_NATIVE_FN(bro_physics_PhysicsVehicle_setInput), "void", {"__bro_native.physics.PhysicsVehicle", "str"}, error) &&
        fn("__bro_native.physics.PhysicsVehicle_setLeanController", BRO_NATIVE_FN(bro_physics_PhysicsVehicle_setLeanController), "void", {"__bro_native.physics.PhysicsVehicle", "bool"}, error) &&
        fn("__bro_native.physics.PhysicsVehicle_setGear", BRO_NATIVE_FN(bro_physics_PhysicsVehicle_setGear), "void", {"__bro_native.physics.PhysicsVehicle", "i32", "f64"}, error) &&
        fn("__bro_native.physics.PhysicsVehicle_wheelState", BRO_NATIVE_FN(bro_physics_PhysicsVehicle_wheelState), "str", {"__bro_native.physics.PhysicsVehicle", "i32"}, error) &&
        fn("__bro_native.physics.PhysicsVehicle_getState", BRO_NATIVE_FN(bro_physics_PhysicsVehicle_getState), "str", {"__bro_native.physics.PhysicsVehicle"}, error) &&
        fn("__bro_native.physics.PhysicsVehicle_getTransform", BRO_NATIVE_FN(bro_physics_PhysicsVehicle_getTransform), "str", {"__bro_native.physics.PhysicsVehicle"}, error) &&
        fn("__bro_native.physics.PhysicsVehicle_wheelCount_get", BRO_NATIVE_FN(bro_physics_PhysicsVehicle_wheelCount_get), "i32", {"__bro_native.physics.PhysicsVehicle"}, error) &&
        fn("__bro_native.physics.PhysicsVehicle_chassisBody_get", BRO_NATIVE_FN(bro_physics_PhysicsVehicle_chassisBody_get), "i32", {"__bro_native.physics.PhysicsVehicle"}, error) &&
        fn("__bro_native.physics.PhysicsVehicle_type_get", BRO_NATIVE_FN(bro_physics_PhysicsVehicle_type_get), "str", {"__bro_native.physics.PhysicsVehicle"}, error) &&
        fn("__bro_native.physics.PhysicsVehicle_speed_get", BRO_NATIVE_FN(bro_physics_PhysicsVehicle_speed_get), "f64", {"__bro_native.physics.PhysicsVehicle"}, error) &&
        fn("__bro_native.physics.PhysicsVehicle_rpm_get", BRO_NATIVE_FN(bro_physics_PhysicsVehicle_rpm_get), "f64", {"__bro_native.physics.PhysicsVehicle"}, error) &&
        fn("__bro_native.physics.PhysicsVehicle_gear_get", BRO_NATIVE_FN(bro_physics_PhysicsVehicle_gear_get), "i32", {"__bro_native.physics.PhysicsVehicle"}, error) &&
        fn("__bro_native.physics.PhysicsVehicle_destroy", BRO_NATIVE_FN(bro_physics_PhysicsVehicle_destroy), "void", {"__bro_native.physics.PhysicsVehicle"}, error) &&
        fn("__bro_native.physics.PhysicsRagdoll_pose", BRO_NATIVE_FN(bro_physics_PhysicsRagdoll_pose), "f32[]", {"__bro_native.physics.PhysicsRagdoll"}, error) &&
        fn("__bro_native.physics.PhysicsRagdoll_localPose", BRO_NATIVE_FN(bro_physics_PhysicsRagdoll_localPose), "f32[]", {"__bro_native.physics.PhysicsRagdoll"}, error) &&
        fn("__bro_native.physics.PhysicsRagdoll_setPose", BRO_NATIVE_FN(bro_physics_PhysicsRagdoll_setPose), "bool", {"__bro_native.physics.PhysicsRagdoll", "str"}, error) &&
        fn("__bro_native.physics.PhysicsRagdoll_driveToPose", BRO_NATIVE_FN(bro_physics_PhysicsRagdoll_driveToPose), "bool", {"__bro_native.physics.PhysicsRagdoll", "str", "str"}, error) &&
        fn("__bro_native.physics.PhysicsRagdoll_driveToPoseKinematic", BRO_NATIVE_FN(bro_physics_PhysicsRagdoll_driveToPoseKinematic), "bool", {"__bro_native.physics.PhysicsRagdoll", "str", "f64"}, error) &&
        fn("__bro_native.physics.PhysicsRagdoll_stopDrive", BRO_NATIVE_FN(bro_physics_PhysicsRagdoll_stopDrive), "void", {"__bro_native.physics.PhysicsRagdoll"}, error) &&
        fn("__bro_native.physics.PhysicsRagdoll_addImpulse", BRO_NATIVE_FN(bro_physics_PhysicsRagdoll_addImpulse), "void", {"__bro_native.physics.PhysicsRagdoll", "f64", "f64", "f64"}, error) &&
        fn("__bro_native.physics.PhysicsRagdoll_activate", BRO_NATIVE_FN(bro_physics_PhysicsRagdoll_activate), "void", {"__bro_native.physics.PhysicsRagdoll"}, error) &&
        fn("__bro_native.physics.PhysicsRagdoll_deactivate", BRO_NATIVE_FN(bro_physics_PhysicsRagdoll_deactivate), "void", {"__bro_native.physics.PhysicsRagdoll"}, error) &&
        fn("__bro_native.physics.PhysicsRagdoll_isActive", BRO_NATIVE_FN(bro_physics_PhysicsRagdoll_isActive), "bool", {"__bro_native.physics.PhysicsRagdoll"}, error) &&
        fn("__bro_native.physics.PhysicsRagdoll_partCount_get", BRO_NATIVE_FN(bro_physics_PhysicsRagdoll_partCount_get), "i32", {"__bro_native.physics.PhysicsRagdoll"}, error) &&
        fn("__bro_native.physics.PhysicsRagdoll_partBody", BRO_NATIVE_FN(bro_physics_PhysicsRagdoll_partBody), "i32", {"__bro_native.physics.PhysicsRagdoll", "i32"}, error) &&
        fn("__bro_native.physics.PhysicsRagdoll_partParent", BRO_NATIVE_FN(bro_physics_PhysicsRagdoll_partParent), "i32", {"__bro_native.physics.PhysicsRagdoll", "i32"}, error) &&
        fn("__bro_native.physics.PhysicsRagdoll_partIndex", BRO_NATIVE_FN(bro_physics_PhysicsRagdoll_partIndex), "i32", {"__bro_native.physics.PhysicsRagdoll", "str"}, error) &&
        fn("__bro_native.physics.PhysicsRagdoll_destroy", BRO_NATIVE_FN(bro_physics_PhysicsRagdoll_destroy), "void", {"__bro_native.physics.PhysicsRagdoll"}, error) &&
        fn("__bro_native.physics.PhysicsSoftBody_body_get", BRO_NATIVE_FN(bro_physics_PhysicsSoftBody_body_get), "i32", {"__bro_native.physics.PhysicsSoftBody"}, error) &&
        fn("__bro_native.physics.PhysicsSoftBody_vertexCount_get", BRO_NATIVE_FN(bro_physics_PhysicsSoftBody_vertexCount_get), "i32", {"__bro_native.physics.PhysicsSoftBody"}, error) &&
        fn("__bro_native.physics.PhysicsSoftBody_topology", BRO_NATIVE_FN(bro_physics_PhysicsSoftBody_topology), "str", {"__bro_native.physics.PhysicsSoftBody"}, error) &&
        fn("__bro_native.physics.PhysicsSoftBody_vertices", BRO_NATIVE_FN(bro_physics_PhysicsSoftBody_vertices), "f32[]", {"__bro_native.physics.PhysicsSoftBody"}, error) &&
        fn("__bro_native.physics.PhysicsSoftBody_pin", BRO_NATIVE_FN(bro_physics_PhysicsSoftBody_pin), "bool", {"__bro_native.physics.PhysicsSoftBody", "i32", "bool"}, error) &&
        fn("__bro_native.physics.PhysicsSoftBody_setVertex", BRO_NATIVE_FN(bro_physics_PhysicsSoftBody_setVertex), "bool", {"__bro_native.physics.PhysicsSoftBody", "i32", "f64", "f64", "f64"}, error) &&
        fn("__bro_native.physics.PhysicsSoftBody_setVertexVelocity", BRO_NATIVE_FN(bro_physics_PhysicsSoftBody_setVertexVelocity), "bool", {"__bro_native.physics.PhysicsSoftBody", "i32", "f64", "f64", "f64"}, error) &&
        fn("__bro_native.physics.PhysicsSoftBody_getBounds", BRO_NATIVE_FN(bro_physics_PhysicsSoftBody_getBounds), "str", {"__bro_native.physics.PhysicsSoftBody"}, error) &&
        fn("__bro_native.physics.PhysicsSoftBody_destroy", BRO_NATIVE_FN(bro_physics_PhysicsSoftBody_destroy), "void", {"__bro_native.physics.PhysicsSoftBody"}, error) &&
        fn("__bro_native.physics.createWorldHandle", BRO_NATIVE_FN(bro_physics_createWorldHandle), "__bro_native.physics.PhysicsWorldHandle", {"str"}, error) &&
        fn("__bro_native.physics.createWorld", BRO_NATIVE_FN(bro_physics_createWorld), "void", {"str"}, error) &&
        fn("__bro_native.physics.setGravity", BRO_NATIVE_FN(bro_physics_setGravity), "void", {"f64", "f64", "f64"}, error) &&
        fn("__bro_native.physics.getGravity", BRO_NATIVE_FN(bro_physics_getGravity), "str", {}, error) &&
        fn("__bro_native.physics.setLayers", BRO_NATIVE_FN(bro_physics_setLayers), "bool", {"str"}, error) &&
        fn("__bro_native.physics.createBody", BRO_NATIVE_FN(bro_physics_createBody), "i32", {"str"}, error) &&
        fn("__bro_native.physics.destroyBody", BRO_NATIVE_FN(bro_physics_destroyBody), "void", {"i32"}, error) &&
        fn("__bro_native.physics.destroyAll", BRO_NATIVE_FN(bro_physics_destroyAll), "void", {}, error) &&
        fn("__bro_native.physics.getTransform", BRO_NATIVE_FN(bro_physics_getTransform), "str", {"i32", "bool"}, error) &&
        fn("__bro_native.physics.getVelocity", BRO_NATIVE_FN(bro_physics_getVelocity), "str", {"i32"}, error) &&
        fn("__bro_native.physics.setPosition", BRO_NATIVE_FN(bro_physics_setPosition), "void", {"i32", "f64", "f64", "f64"}, error) &&
        fn("__bro_native.physics.setRotation", BRO_NATIVE_FN(bro_physics_setRotation), "void", {"i32", "f64", "f64", "f64", "f64"}, error) &&
        fn("__bro_native.physics.setLinearVelocity", BRO_NATIVE_FN(bro_physics_setLinearVelocity), "void", {"i32", "f64", "f64", "f64"}, error) &&
        fn("__bro_native.physics.setAngularVelocity", BRO_NATIVE_FN(bro_physics_setAngularVelocity), "void", {"i32", "f64", "f64", "f64"}, error) &&
        fn("__bro_native.physics.addForce", BRO_NATIVE_FN(bro_physics_addForce), "void", {"i32", "f64", "f64", "f64"}, error) &&
        fn("__bro_native.physics.addImpulse", BRO_NATIVE_FN(bro_physics_addImpulse), "void", {"i32", "f64", "f64", "f64"}, error) &&
        fn("__bro_native.physics.addTorque", BRO_NATIVE_FN(bro_physics_addTorque), "void", {"i32", "f64", "f64", "f64"}, error) &&
        fn("__bro_native.physics.setUserData", BRO_NATIVE_FN(bro_physics_setUserData), "void", {"i32", "f64"}, error) &&
        fn("__bro_native.physics.getUserData", BRO_NATIVE_FN(bro_physics_getUserData), "f64", {"i32"}, error) &&
        fn("__bro_native.physics.setLayer", BRO_NATIVE_FN(bro_physics_setLayer), "bool", {"i32", "str"}, error) &&
        fn("__bro_native.physics.setKinematic", BRO_NATIVE_FN(bro_physics_setKinematic), "void", {"i32"}, error) &&
        fn("__bro_native.physics.raycastClosestRaw", BRO_NATIVE_FN(bro_physics_raycastClosestRaw), "str", {"f64", "f64", "f64", "f64", "f64", "f64", "f64", "i32"}, error) &&
        fn("__bro_native.physics.raycastRaw", BRO_NATIVE_FN(bro_physics_raycastRaw), "str", {"f64", "f64", "f64", "f64", "f64", "f64", "f64", "i32"}, error) &&
        fn("__bro_native.physics.castShapeRaw", BRO_NATIVE_FN(bro_physics_castShapeRaw), "str", {"str"}, error) &&
        fn("__bro_native.physics.castShapeClosestRaw", BRO_NATIVE_FN(bro_physics_castShapeClosestRaw), "str", {"str"}, error) &&
        fn("__bro_native.physics.overlapShapeRaw", BRO_NATIVE_FN(bro_physics_overlapShapeRaw), "i32[]", {"str"}, error) &&
        fn("__bro_native.physics.penetrations", BRO_NATIVE_FN(bro_physics_penetrations), "f64[]", {"str"}, error) &&
        fn("__bro_native.physics.setTransforms", BRO_NATIVE_FN(bro_physics_setTransforms), "void", {"f64[]", "i32"}, error) &&
        fn("__bro_native.physics.setTransform", BRO_NATIVE_FN(bro_physics_setTransform), "void", {"i32", "f64", "f64", "f64", "f64", "f64", "f64", "f64"}, error) &&
        fn("__bro_native.physics.overlapSphereRaw", BRO_NATIVE_FN(bro_physics_overlapSphereRaw), "i32[]", {"f64", "f64", "f64", "f64"}, error) &&
        fn("__bro_native.physics.overlapBoxRaw", BRO_NATIVE_FN(bro_physics_overlapBoxRaw), "i32[]", {"f64", "f64", "f64", "f64", "f64", "f64"}, error) &&
        fn("__bro_native.physics.overlapPointRaw", BRO_NATIVE_FN(bro_physics_overlapPointRaw), "i32[]", {"f64", "f64", "f64", "i32"}, error) &&
        fn("__bro_native.physics.raycastClosestJsonRaw", BRO_NATIVE_FN(bro_physics_raycastClosestJsonRaw), "str", {"f64", "f64", "f64", "f64", "f64", "f64", "f64", "str"}, error) &&
        fn("__bro_native.physics.raycastJsonRaw", BRO_NATIVE_FN(bro_physics_raycastJsonRaw), "str", {"f64", "f64", "f64", "f64", "f64", "f64", "f64", "str"}, error) &&
        fn("__bro_native.physics.overlapShapeJsonRaw", BRO_NATIVE_FN(bro_physics_overlapShapeJsonRaw), "str", {"str"}, error) &&
        fn("__bro_native.physics.overlapPointJsonRaw", BRO_NATIVE_FN(bro_physics_overlapPointJsonRaw), "str", {"f64", "f64", "f64", "str"}, error) &&
        fn("__bro_native.physics.getContacts", BRO_NATIVE_FN(bro_physics_getContacts), "str", {}, error) &&
        fn("__bro_native.physics.setFrictionCombine", BRO_NATIVE_FN(bro_physics_setFrictionCombine), "void", {"i32", "str"}, error) &&
        fn("__bro_native.physics.setRestitutionCombine", BRO_NATIVE_FN(bro_physics_setRestitutionCombine), "void", {"i32", "str"}, error) &&
        fn("__bro_native.physics.getMass", BRO_NATIVE_FN(bro_physics_getMass), "f64", {"i32"}, error) &&
        fn("__bro_native.physics.setMass", BRO_NATIVE_FN(bro_physics_setMass), "void", {"i32", "f64"}, error) &&
        fn("__bro_native.physics.setLinearDamping", BRO_NATIVE_FN(bro_physics_setLinearDamping), "void", {"i32", "f64"}, error) &&
        fn("__bro_native.physics.setAngularDamping", BRO_NATIVE_FN(bro_physics_setAngularDamping), "void", {"i32", "f64"}, error) &&
        fn("__bro_native.physics.setGravityFactor", BRO_NATIVE_FN(bro_physics_setGravityFactor), "void", {"i32", "f64"}, error) &&
        fn("__bro_native.physics.setFriction", BRO_NATIVE_FN(bro_physics_setFriction), "void", {"i32", "f64"}, error) &&
        fn("__bro_native.physics.setRestitution", BRO_NATIVE_FN(bro_physics_setRestitution), "void", {"i32", "f64"}, error) &&
        fn("__bro_native.physics.getBodyProperties", BRO_NATIVE_FN(bro_physics_getBodyProperties), "str", {"i32"}, error) &&
        fn("__bro_native.physics.setAreaOverride", BRO_NATIVE_FN(bro_physics_setAreaOverride), "bool", {"i32", "str"}, error) &&
        fn("__bro_native.physics.setTimeStep", BRO_NATIVE_FN(bro_physics_setTimeStep), "void", {"f64"}, error) &&
        fn("__bro_native.physics.getTimeStep", BRO_NATIVE_FN(bro_physics_getTimeStep), "f64", {}, error) &&
        fn("__bro_native.physics.step", BRO_NATIVE_FN(bro_physics_step), "void", {"f64"}, error) &&
        fn("__bro_native.physics.setInterpolation", BRO_NATIVE_FN(bro_physics_setInterpolation), "void", {"bool"}, error) &&
        fn("__bro_native.physics.getInterpolation", BRO_NATIVE_FN(bro_physics_getInterpolation), "bool", {}, error) &&
        fn("__bro_native.physics.isActive", BRO_NATIVE_FN(bro_physics_isActive), "bool", {"i32"}, error) &&
        fn("__bro_native.physics.activate", BRO_NATIVE_FN(bro_physics_activate), "void", {"i32"}, error) &&
        fn("__bro_native.physics.getAllTransforms", BRO_NATIVE_FN(bro_physics_getAllTransforms), "f32[]", {"bool"}, error) &&
        fn("__bro_native.physics.createCharacter", BRO_NATIVE_FN(bro_physics_createCharacter), "__bro_native.physics.PhysicsCharacter", {"str"}, error) &&
        fn("__bro_native.physics.createVehicle", BRO_NATIVE_FN(bro_physics_createVehicle), "__bro_native.physics.PhysicsVehicle", {"str"}, error) &&
        fn("__bro_native.physics.createRagdoll", BRO_NATIVE_FN(bro_physics_createRagdoll), "__bro_native.physics.PhysicsRagdoll", {"str"}, error) &&
        fn("__bro_native.physics.createSoftBody", BRO_NATIVE_FN(bro_physics_createSoftBody), "__bro_native.physics.PhysicsSoftBody", {"str"}, error) &&
        fn("__bro_native.physics.createConstraint", BRO_NATIVE_FN(bro_physics_createConstraint), "i32", {"str"}, error) &&
        fn("__bro_native.physics.destroyConstraint", BRO_NATIVE_FN(bro_physics_destroyConstraint), "void", {"i32"}, error) &&
        fn("__bro_native.physics.setConstraintEnabled", BRO_NATIVE_FN(bro_physics_setConstraintEnabled), "void", {"i32", "bool"}, error) &&
        fn("__bro_native.physics.isConstraintEnabled", BRO_NATIVE_FN(bro_physics_isConstraintEnabled), "bool", {"i32"}, error) &&
        fn("__bro_native.physics.setWheelMotor", BRO_NATIVE_FN(bro_physics_setWheelMotor), "void", {"i32", "i32", "f64", "f64"}, error) &&
        fn("__bro_native.physics.setConstraintMotor", BRO_NATIVE_FN(bro_physics_setConstraintMotor), "bool", {"i32", "str"}, error) &&
        fn("__bro_native.physics.setConstraintBreakingImpulse", BRO_NATIVE_FN(bro_physics_setConstraintBreakingImpulse), "void", {"i32", "f64"}, error) &&
        fn("__bro_native.physics.getConstraintBreakingImpulse", BRO_NATIVE_FN(bro_physics_getConstraintBreakingImpulse), "f64", {"i32"}, error) &&
        fn("__bro_native.physics.getBrokenConstraints", BRO_NATIVE_FN(bro_physics_getBrokenConstraints), "i32[]", {}, error);
    if (!ok) return false;
    publishPrototype("physics", "__bro_native.physics.PhysicsWorldHandle", "PhysicsWorldHandleProto");
    publishPrototype("physics", "__bro_native.physics.PhysicsCharacter", "PhysicsCharacterProto");
    publishPrototype("physics", "__bro_native.physics.PhysicsVehicle", "PhysicsVehicleProto");
    publishPrototype("physics", "__bro_native.physics.PhysicsRagdoll", "PhysicsRagdollProto");
    publishPrototype("physics", "__bro_native.physics.PhysicsSoftBody", "PhysicsSoftBodyProto");
    return true;
}

bool registerPhysicsNatives(std::string* error) {
    if (!registerNatives_physics(error)) return false;
    return natives::fn("__bro_native.physics.setMotionType", BRO_NATIVE_FN(bro_physics_setMotionType),
                       "void", {"i32", "bool"}, error) &&
           natives::fn("__bro_native.physics.moveKinematic", BRO_NATIVE_FN(bro_physics_moveKinematic),
                       "void", {"i32", "f64", "f64", "f64", "f64"}, error) &&
           natives::fn("__bro_native.physics.moveKinematicRot", BRO_NATIVE_FN(bro_physics_moveKinematicRot),
                       "void", {"i32", "f64", "f64", "f64", "f64", "f64", "f64", "f64", "f64"}, error);
}

}  // namespace bro::bronze_host
