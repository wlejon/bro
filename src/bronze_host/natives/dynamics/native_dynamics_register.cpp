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
           fn("__bro_native.dynamics.addBody", BRO_NATIVE_FN(bro_dynamics_addBody), "i32", {"i32", "bool", "f32[]"}, error) &&
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
           fn("__bro_native.dynamics.counts", BRO_NATIVE_FN(bro_dynamics_counts), "f64[]", {"i32"}, error);
}

bool registerDynamicsNatives(std::string* error) {
    return registerNatives_dynamics(error);
}

}  // namespace bro::bronze_host
