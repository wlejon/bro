// native_animation_register.cpp — registers every native of native_animation_decl.h with
// bronze (embed::registerNative) under __bro_native.animation. Hand-maintained: each
// registration's signature matches its prototype there. Call registerNatives_animation from registerBroNatives
// (src/bronze_host/host_natives.h) on the thread that runs the program.
//
// Class members are Function-kind natives taking the handle first (the wrapper's
// receiver is dynamic, so a Method-kind registration would be unreachable from it);
// each class has one Constructor-kind registration, which mints the class, and its
// prototype is published as __bro_native.animation.<Class>Proto for js/animation.js to chain.

#include "native_animation_decl.h"
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

bool registerNatives_animation(std::string* error) {
    const bool ok =
        ctor("__bro_native.animation.Tween", BRO_NATIVE_FN(bro_animation_Tween_ctor), BRO_NATIVE_DTOR(bro_animation_Tween_dtor), bronze::runtime::Finalize::InSweep, {}, error) &&
        ctor("__bro_native.animation.AnimationPlayer", BRO_NATIVE_FN(bro_animation_AnimationPlayer_ctor), BRO_NATIVE_DTOR(bro_animation_AnimationPlayer_dtor), bronze::runtime::Finalize::InSweep, {}, error) &&
        fn("__bro_native.animation.Tween_call", BRO_NATIVE_FN(bro_animation_Tween_call), "__bro_native.animation.Tween", {"__bro_native.animation.Tween", "dynamic"}, error) &&
        fn("__bro_native.animation.Tween_loop", BRO_NATIVE_FN(bro_animation_Tween_loop), "__bro_native.animation.Tween", {"__bro_native.animation.Tween", "bool", "i32"}, error) &&
        fn("__bro_native.animation.Tween_start", BRO_NATIVE_FN(bro_animation_Tween_start), "__bro_native.animation.Tween", {"__bro_native.animation.Tween"}, error) &&
        fn("__bro_native.animation.Tween_stop", BRO_NATIVE_FN(bro_animation_Tween_stop), "__bro_native.animation.Tween", {"__bro_native.animation.Tween"}, error) &&
        fn("__bro_native.animation.Tween_pause", BRO_NATIVE_FN(bro_animation_Tween_pause), "__bro_native.animation.Tween", {"__bro_native.animation.Tween"}, error) &&
        fn("__bro_native.animation.Tween_resume", BRO_NATIVE_FN(bro_animation_Tween_resume), "__bro_native.animation.Tween", {"__bro_native.animation.Tween"}, error) &&
        fn("__bro_native.animation.Tween_destroy", BRO_NATIVE_FN(bro_animation_Tween_destroy), "void", {"__bro_native.animation.Tween"}, error) &&
        fn("__bro_native.animation.AnimationPlayer_pause", BRO_NATIVE_FN(bro_animation_AnimationPlayer_pause), "void", {"__bro_native.animation.AnimationPlayer"}, error) &&
        fn("__bro_native.animation.AnimationPlayer_resume", BRO_NATIVE_FN(bro_animation_AnimationPlayer_resume), "void", {"__bro_native.animation.AnimationPlayer"}, error) &&
        fn("__bro_native.animation.AnimationPlayer_stop", BRO_NATIVE_FN(bro_animation_AnimationPlayer_stop), "void", {"__bro_native.animation.AnimationPlayer"}, error) &&
        fn("__bro_native.animation.AnimationPlayer_seek", BRO_NATIVE_FN(bro_animation_AnimationPlayer_seek), "void", {"__bro_native.animation.AnimationPlayer", "f64"}, error) &&
        fn("__bro_native.animation.AnimationPlayer_destroy", BRO_NATIVE_FN(bro_animation_AnimationPlayer_destroy), "void", {"__bro_native.animation.AnimationPlayer"}, error);
    if (!ok) return false;
    publishPrototype("animation", "__bro_native.animation.Tween", "TweenProto");
    publishPrototype("animation", "__bro_native.animation.AnimationPlayer", "AnimationPlayerProto");
    return true;
}

bool registerClipPlayerNatives(std::string* error);

bool registerClipPlayerNatives(std::string* error) {
    return natives::fn("__bro_native.animation.AnimationPlayer_addClip", BRO_NATIVE_FN(bro_animation_AnimationPlayer_addClip), "str", {"__bro_native.animation.AnimationPlayer", "str", "dynamic"}, error) &&
           natives::fn("__bro_native.animation.AnimationPlayer_clipDef", BRO_NATIVE_FN(bro_animation_AnimationPlayer_clipDef), "str", {"__bro_native.animation.AnimationPlayer", "str"}, error) &&
           natives::fn("__bro_native.animation.AnimationPlayer_play", BRO_NATIVE_FN(bro_animation_AnimationPlayer_play), "str", {"__bro_native.animation.AnimationPlayer", "str", "dynamic"}, error) &&
           natives::fn("__bro_native.animation.AnimationPlayer_playing", BRO_NATIVE_FN(bro_animation_AnimationPlayer_playing), "bool", {"__bro_native.animation.AnimationPlayer"}, error) &&
           natives::fn("__bro_native.animation.AnimationPlayer_currentClip", BRO_NATIVE_FN(bro_animation_AnimationPlayer_currentClip), "str", {"__bro_native.animation.AnimationPlayer"}, error) &&
           natives::fn("__bro_native.animation.AnimationPlayer_currentTime", BRO_NATIVE_FN(bro_animation_AnimationPlayer_currentTime), "f64", {"__bro_native.animation.AnimationPlayer"}, error) &&
           natives::fn("__bro_native.animation.AnimationPlayer_speed_get", BRO_NATIVE_FN(bro_animation_AnimationPlayer_speed_get), "f64", {"__bro_native.animation.AnimationPlayer"}, error) &&
           natives::fn("__bro_native.animation.AnimationPlayer_speed_set", BRO_NATIVE_FN(bro_animation_AnimationPlayer_speed_set), "void", {"__bro_native.animation.AnimationPlayer", "f64"}, error) &&
           natives::fn("__bro_native.animation.AnimationPlayer_setOnFinished", BRO_NATIVE_FN(bro_animation_AnimationPlayer_setOnFinished), "void", {"__bro_native.animation.AnimationPlayer", "dynamic"}, error) &&
           natives::fn("__bro_native.animation.AnimationPlayer_setOnEvent", BRO_NATIVE_FN(bro_animation_AnimationPlayer_setOnEvent), "void", {"__bro_native.animation.AnimationPlayer", "dynamic"}, error);
}

bool registerAnimationNatives(std::string* error) {
    if (!registerNatives_animation(error)) return false;
    return natives::fn("__bro_native.animation.Tween_to", BRO_NATIVE_FN(bro_animation_Tween_to), "void", {"__bro_native.animation.Tween", "i32", "dynamic", "f64", "dynamic", "dynamic"}, error) &&
           natives::fn("__bro_native.animation.Tween_parallel", BRO_NATIVE_FN(bro_animation_Tween_parallel), "void", {"__bro_native.animation.Tween"}, error) &&
           natives::fn("__bro_native.animation.Tween_isRunning", BRO_NATIVE_FN(bro_animation_Tween_isRunning), "bool", {"__bro_native.animation.Tween"}, error) &&
           natives::fn("__bro_native.animation.Tween_isPaused", BRO_NATIVE_FN(bro_animation_Tween_isPaused), "bool", {"__bro_native.animation.Tween"}, error) &&
           natives::fn("__bro_native.animation.Tween_isFinished", BRO_NATIVE_FN(bro_animation_Tween_isFinished), "bool", {"__bro_native.animation.Tween"}, error) &&
           natives::fn("__bro_native.animation.Tween_setOnFinished", BRO_NATIVE_FN(bro_animation_Tween_setOnFinished), "void", {"__bro_native.animation.Tween", "dynamic"}, error) &&
           registerClipPlayerNatives(error);
}

}  // namespace bro::bronze_host
