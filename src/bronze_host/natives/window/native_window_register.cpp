// native_window_register.cpp — registers every native of native_window_decl.h with
// bronze (embed::registerNative) under __bro_native.window. Hand-maintained: each
// registration's signature matches its prototype there. Call registerNatives_window from registerBroNatives
// (src/bronze_host/host_natives.h) on the thread that runs the program.
//
// Class members are Function-kind natives taking the handle first (the wrapper's
// receiver is dynamic, so a Method-kind registration would be unreachable from it);
// each class has one Constructor-kind registration, which mints the class, and its
// prototype is published as __bro_native.window.<Class>Proto for js/bro_core.js to chain.

#include "native_window_decl.h"

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

bool getter(const char* path, void* f, const char* ret, std::string* error) {
    return ev::registerNative(path, f, sig(ret, {}, ev::NativeKind::Getter), error);
}

bool setter(const char* path, void* f, const char* type, std::string* error) {
    return ev::registerNative(path, f, sig("void", {type}, ev::NativeKind::Setter), error);
}

template <typename F>
void* p(F* f) { return reinterpret_cast<void*>(f); }

}  // namespace

bool registerNatives_window(std::string* error) {
    const bool ok =
        getter("__bro_native.window.state", p(&bro_window_state_get), "str", error) &&
        getter("__bro_native.window.borderless", p(&bro_window_borderless_get), "bool", error) &&
        setter("__bro_native.window.borderless", p(&bro_window_borderless_set), "bool", error) &&
        getter("__bro_native.window.alwaysOnTop", p(&bro_window_alwaysOnTop_get), "bool", error) &&
        setter("__bro_native.window.alwaysOnTop", p(&bro_window_alwaysOnTop_set), "bool", error) &&
        fn("__bro_native.window.minimize", p(&bro_window_minimize), "void", {}, error) &&
        fn("__bro_native.window.maximize", p(&bro_window_maximize), "void", {}, error) &&
        fn("__bro_native.window.restore", p(&bro_window_restore), "void", {}, error) &&
        fn("__bro_native.window.getPosition", p(&bro_window_getPosition), "void", {}, error) &&
        fn("__bro_native.window.getPosition_x", p(&bro_window_getPosition_x), "i32", {}, error) &&
        fn("__bro_native.window.getPosition_y", p(&bro_window_getPosition_y), "i32", {}, error) &&
        fn("__bro_native.window.setPosition", p(&bro_window_setPosition), "void", {"i32", "i32"}, error) &&
        fn("__bro_native.window.getMinSize", p(&bro_window_getMinSize), "void", {}, error) &&
        fn("__bro_native.window.getMinSize_width", p(&bro_window_getMinSize_width), "i32", {}, error) &&
        fn("__bro_native.window.getMinSize_height", p(&bro_window_getMinSize_height), "i32", {}, error) &&
        fn("__bro_native.window.setMinSize", p(&bro_window_setMinSize), "void", {"i32", "i32"}, error) &&
        fn("__bro_native.window.getMaxSize", p(&bro_window_getMaxSize), "void", {}, error) &&
        fn("__bro_native.window.getMaxSize_width", p(&bro_window_getMaxSize_width), "i32", {}, error) &&
        fn("__bro_native.window.getMaxSize_height", p(&bro_window_getMaxSize_height), "i32", {}, error) &&
        fn("__bro_native.window.setMaxSize", p(&bro_window_setMaxSize), "void", {"i32", "i32"}, error) &&
        fn("__bro_native.window.getDisplays", p(&bro_window_getDisplays), "i32", {}, error) &&
        fn("__bro_native.window.getDisplays_id", p(&bro_window_getDisplays_id), "f64", {"i32"}, error) &&
        fn("__bro_native.window.getDisplays_name", p(&bro_window_getDisplays_name), "str", {"i32"}, error) &&
        fn("__bro_native.window.getDisplays_x", p(&bro_window_getDisplays_x), "i32", {"i32"}, error) &&
        fn("__bro_native.window.getDisplays_y", p(&bro_window_getDisplays_y), "i32", {"i32"}, error) &&
        fn("__bro_native.window.getDisplays_width", p(&bro_window_getDisplays_width), "i32", {"i32"}, error) &&
        fn("__bro_native.window.getDisplays_height", p(&bro_window_getDisplays_height), "i32", {"i32"}, error) &&
        fn("__bro_native.window.getDisplays_workX", p(&bro_window_getDisplays_workX), "i32", {"i32"}, error) &&
        fn("__bro_native.window.getDisplays_workY", p(&bro_window_getDisplays_workY), "i32", {"i32"}, error) &&
        fn("__bro_native.window.getDisplays_workWidth", p(&bro_window_getDisplays_workWidth), "i32", {"i32"}, error) &&
        fn("__bro_native.window.getDisplays_workHeight", p(&bro_window_getDisplays_workHeight), "i32", {"i32"}, error) &&
        fn("__bro_native.window.getDisplays_refreshRate", p(&bro_window_getDisplays_refreshRate), "f64", {"i32"}, error) &&
        fn("__bro_native.window.getDisplays_contentScale", p(&bro_window_getDisplays_contentScale), "f64", {"i32"}, error) &&
        fn("__bro_native.window.getDisplays_isPrimary", p(&bro_window_getDisplays_isPrimary), "bool", {"i32"}, error) &&
        fn("__bro_native.window.getDisplays_isCurrent", p(&bro_window_getDisplays_isCurrent), "bool", {"i32"}, error) &&
        fn("__bro_native.window.moveToDisplay", p(&bro_window_moveToDisplay), "bool", {"f64"}, error) &&
        fn("__bro_native.window.getSize", p(&bro_window_getSize), "void", {}, error) &&
        fn("__bro_native.window.getSize_width", p(&bro_window_getSize_width), "i32", {}, error) &&
        fn("__bro_native.window.getSize_height", p(&bro_window_getSize_height), "i32", {}, error) &&
        fn("__bro_native.window.setSize", p(&bro_window_setSize), "void", {"i32", "i32"}, error) &&
        fn("__bro_native.window.quit", p(&bro_window_quit), "void", {}, error);
    if (!ok) return false;
    return true;
}

}  // namespace bro::bronze_host
