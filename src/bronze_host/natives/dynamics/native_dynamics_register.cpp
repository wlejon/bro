// native_dynamics_register.cpp — registers every native of native_dynamics_decl.h with
// bronze (embed::registerNative) under __bro_native.dynamics. Hand-maintained: each
// registration's signature matches its prototype there. Call registerNatives_dynamics from
// registerBroNatives (src/bronze_host/host_natives.h) on the thread that runs the program.

#include "native_dynamics_decl.h"
#include "bronze_host/native_table.h"
#include "bronze_host/host_natives.h"

#include "embed/embed.h"

#include <string>

namespace bro::bronze_host {

bool registerNatives_dynamics(std::string* error) {
    using natives::fn;
    return fn("__bro_native.dynamics.create", BRO_NATIVE_FN(bro_dynamics_create), "i32", {}, error) &&
           fn("__bro_native.dynamics.destroy", BRO_NATIVE_FN(bro_dynamics_destroy), "void", {"i32"}, error) &&
           fn("__bro_native.dynamics.setGravity", BRO_NATIVE_FN(bro_dynamics_setGravity), "void", {"i32", "f64", "f64", "f64"}, error) &&
           fn("__bro_native.dynamics.addBody", BRO_NATIVE_FN(bro_dynamics_addBody), "i32", {"i32", "i32", "f32[]", "i32", "i32", "i32"}, error) &&
           fn("__bro_native.dynamics.addHull", BRO_NATIVE_FN(bro_dynamics_addHull), "i32", {"i32", "f32[]", "i32", "i32", "i32", "f32[]"}, error) &&
           fn("__bro_native.dynamics.addMesh", BRO_NATIVE_FN(bro_dynamics_addMesh), "i32", {"i32", "f32[]", "i32", "i32", "f32[]", "i32[]"}, error) &&
           fn("__bro_native.dynamics.setMaxSpeed", BRO_NATIVE_FN(bro_dynamics_setMaxSpeed), "void", {"i32", "f64"}, error) &&
           fn("__bro_native.dynamics.setHitSpeed", BRO_NATIVE_FN(bro_dynamics_setHitSpeed), "void", {"i32", "f64"}, error) &&
           fn("__bro_native.dynamics.moveTo", BRO_NATIVE_FN(bro_dynamics_moveTo), "void", {"i32", "i32", "f32[]", "f64"}, error) &&
           fn("__bro_native.dynamics.wake", BRO_NATIVE_FN(bro_dynamics_wake), "void", {"i32", "i32"}, error) &&
           fn("__bro_native.dynamics.hasBody", BRO_NATIVE_FN(bro_dynamics_hasBody), "bool", {"i32", "i32"}, error) &&
           fn("__bro_native.dynamics.touched", BRO_NATIVE_FN(bro_dynamics_touched), "i32[]", {"i32"}, error) &&
           fn("__bro_native.dynamics.hits", BRO_NATIVE_FN(bro_dynamics_hits), "f32[]", {"i32"}, error) &&
           fn("__bro_native.dynamics.raycast", BRO_NATIVE_FN(bro_dynamics_raycast), "f32[]", {"i32", "f32[]", "f64", "i32"}, error) &&
           fn("__bro_native.dynamics.removeBody", BRO_NATIVE_FN(bro_dynamics_removeBody), "void", {"i32", "i32"}, error) &&
           fn("__bro_native.dynamics.addJoint", BRO_NATIVE_FN(bro_dynamics_addJoint), "i32", {"i32", "i32", "i32", "i32", "f32[]", "bool"}, error) &&
           fn("__bro_native.dynamics.removeJoint", BRO_NATIVE_FN(bro_dynamics_removeJoint), "void", {"i32", "i32"}, error) &&
           fn("__bro_native.dynamics.hasJoint", BRO_NATIVE_FN(bro_dynamics_hasJoint), "bool", {"i32", "i32"}, error) &&
           fn("__bro_native.dynamics.jointLoad", BRO_NATIVE_FN(bro_dynamics_jointLoad), "f32[]", {"i32", "i32"}, error) &&
           fn("__bro_native.dynamics.step", BRO_NATIVE_FN(bro_dynamics_step), "void", {"i32", "f64", "i32"}, error) &&
           fn("__bro_native.dynamics.pose", BRO_NATIVE_FN(bro_dynamics_pose), "f32[]", {"i32", "i32"}, error) &&
           fn("__bro_native.dynamics.setPose", BRO_NATIVE_FN(bro_dynamics_setPose), "void", {"i32", "i32", "f32[]"}, error) &&
           fn("__bro_native.dynamics.velocity", BRO_NATIVE_FN(bro_dynamics_velocity), "f32[]", {"i32", "i32"}, error) &&
           fn("__bro_native.dynamics.setVelocity", BRO_NATIVE_FN(bro_dynamics_setVelocity), "void", {"i32", "i32", "f32[]"}, error) &&
           fn("__bro_native.dynamics.applyImpulse", BRO_NATIVE_FN(bro_dynamics_applyImpulse), "void", {"i32", "i32", "f32[]"}, error) &&
           fn("__bro_native.dynamics.mass", BRO_NATIVE_FN(bro_dynamics_mass), "f64", {"i32", "i32"}, error) &&
           fn("__bro_native.dynamics.moved", BRO_NATIVE_FN(bro_dynamics_moved), "f32[]", {"i32"}, error) &&
           fn("__bro_native.dynamics.broken", BRO_NATIVE_FN(bro_dynamics_broken), "i32[]", {"i32"}, error) &&
           fn("__bro_native.dynamics.counts", BRO_NATIVE_FN(bro_dynamics_counts), "f64[]", {"i32"}, error) &&
           fn("__bro_native.dynamics.addStructure", BRO_NATIVE_FN(bro_dynamics_addStructure), "i32", {"i32", "f32[]", "f32[]", "i32[]", "f32[]", "i32", "i32"}, error) &&
           fn("__bro_native.dynamics.removeStructure", BRO_NATIVE_FN(bro_dynamics_removeStructure), "void", {"i32", "i32"}, error) &&
           fn("__bro_native.dynamics.structureCells", BRO_NATIVE_FN(bro_dynamics_structureCells), "f32[]", {"i32", "i32"}, error) &&
           fn("__bro_native.dynamics.structureState", BRO_NATIVE_FN(bro_dynamics_structureState), "i32[]", {"i32", "i32"}, error) &&
           fn("__bro_native.dynamics.structureMoved", BRO_NATIVE_FN(bro_dynamics_structureMoved), "f32[]", {"i32", "i32"}, error) &&
           fn("__bro_native.dynamics.structureRaycast", BRO_NATIVE_FN(bro_dynamics_structureRaycast), "f32[]", {"i32", "i32", "f32[]", "f64"}, error) &&
           fn("__bro_native.dynamics.structureCrushed", BRO_NATIVE_FN(bro_dynamics_structureCrushed), "i32[]", {"i32", "i32"}, error) &&
           fn("__bro_native.dynamics.structureBared", BRO_NATIVE_FN(bro_dynamics_structureBared), "i32[]", {"i32", "i32"}, error) &&
           fn("__bro_native.dynamics.structurePieces", BRO_NATIVE_FN(bro_dynamics_structurePieces), "f32[]", {"i32", "i32"}, error) &&
           fn("__bro_native.dynamics.structurePieceCells", BRO_NATIVE_FN(bro_dynamics_structurePieceCells), "i32[]", {"i32", "i32", "i32"}, error) &&
           fn("__bro_native.dynamics.structureDrop", BRO_NATIVE_FN(bro_dynamics_structureDrop), "void", {"i32", "i32", "i32"}, error) &&
           fn("__bro_native.dynamics.structureCounts", BRO_NATIVE_FN(bro_dynamics_structureCounts), "f64[]", {"i32", "i32"}, error);
}

bool registerDynamicsNatives(std::string* error) {
    return registerNatives_dynamics(error);
}

}  // namespace bro::bronze_host
