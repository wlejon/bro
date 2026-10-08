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
        fn("__bro_native.window.quit", p(&bro_window_quit), "void", {}, error) &&
        getter("__bro_native.window.title", p(&bro_window_title_get), "str", error) &&
        setter("__bro_native.window.title", p(&bro_window_title_set), "str", error) &&
        getter("__bro_native.window.opacity", p(&bro_window_opacity_get), "f64", error) &&
        setter("__bro_native.window.opacity", p(&bro_window_opacity_set), "f64", error) &&
        getter("__bro_native.window.fullscreen", p(&bro_window_fullscreen_get), "bool", error) &&
        setter("__bro_native.window.fullscreen", p(&bro_window_fullscreen_set), "bool", error) &&
        getter("__bro_native.window.focused", p(&bro_window_focused_get), "bool", error) &&
        fn("__bro_native.window.flash", p(&bro_window_flash), "bool", {"bool"}, error) &&
        fn("__bro_native.window.beep", p(&bro_window_beep), "bool", {}, error) &&
        fn("__bro_native.window.getBeepCount", p(&bro_window_getBeepCount), "i32", {}, error) &&
        fn("__bro_native.window.resetBeepCount", p(&bro_window_resetBeepCount), "void", {}, error) &&
        fn("__bro_native.window.setProgress", p(&bro_window_setProgress), "bool", {"i32", "i32"}, error) &&
        fn("__bro_native.window.getProgressState", p(&bro_window_getProgressState), "i32", {}, error) &&
        fn("__bro_native.window.getProgressValue", p(&bro_window_getProgressValue), "i32", {}, error) &&
        fn("__bro_native.window.notify", p(&bro_window_notify), "i32", {"str", "str", "str", "i32", "bool", "i32"}, error) &&
        fn("__bro_native.window.getNotificationCount", p(&bro_window_getNotificationCount), "i32", {}, error) &&
        fn("__bro_native.window.getLastNotificationTitle", p(&bro_window_getLastNotificationTitle), "str", {}, error) &&
        fn("__bro_native.window.getLastNotificationBody", p(&bro_window_getLastNotificationBody), "str", {}, error) &&
        fn("__bro_native.window.clearNotifications", p(&bro_window_clearNotifications), "void", {}, error) &&
        fn("__bro_native.window.setTray", p(&bro_window_setTray), "bool", {"str", "str", "str"}, error) &&
        fn("__bro_native.window.removeTray", p(&bro_window_removeTray), "bool", {}, error) &&
        fn("__bro_native.window.hasTray", p(&bro_window_hasTray), "bool", {}, error) &&
        fn("__bro_native.window.isTrayAvailable", p(&bro_window_isTrayAvailable), "bool", {}, error) &&
        fn("__bro_native.window.simulateTrayClick", p(&bro_window_simulateTrayClick), "void", {"str"}, error) &&
        fn("__bro_native.window.registerGlobalHotkey", p(&bro_window_registerGlobalHotkey), "i32", {"str", "bool"}, error) &&
        fn("__bro_native.window.unregisterGlobalHotkey", p(&bro_window_unregisterGlobalHotkey), "bool", {"i32"}, error) &&
        fn("__bro_native.window.unregisterAllGlobalHotkeys", p(&bro_window_unregisterAllGlobalHotkeys), "void", {}, error) &&
        fn("__bro_native.window.simulateGlobalHotkey", p(&bro_window_simulateGlobalHotkey), "bool", {"str"}, error) &&
        fn("__bro_native.window.simulateHotkeyKey", p(&bro_window_simulateHotkeyKey), "i32", {"str", "str", "i32", "bool", "bool"}, error) &&
        fn("__bro_native.window.resetHotkeyKeys", p(&bro_window_resetHotkeyKeys), "void", {}, error) &&
        getter("__bro_native.window.displayMode", p(&bro_window_displayMode_get), "str", error) &&
        fn("__bro_native.window.requestSingleInstance", p(&bro_window_requestSingleInstance), "bool", {"str", "str"}, error) &&
        fn("__bro_native.window.shutdownSingleInstance", p(&bro_window_shutdownSingleInstance), "void", {}, error) &&
        fn("__bro_native.window.simulateSingleInstance", p(&bro_window_simulateSingleInstance), "bool", {"str", "str"}, error) &&
        fn("__bro_native.window._setFocusDispatcher", p(&bro_window_setFocusDispatcher), "void", {"dynamic"}, error) &&
        fn("__bro_native.window._setHotkeyDispatcher", p(&bro_window_setHotkeyDispatcher), "void", {"dynamic"}, error) &&
        fn("__bro_native.window._setTrayDispatcher", p(&bro_window_setTrayDispatcher), "void", {"dynamic"}, error) &&
        fn("__bro_native.window._setSingleInstanceDispatcher", p(&bro_window_setSingleInstanceDispatcher), "void", {"dynamic"}, error) &&
        fn("__bro_native.window.simulateFocus", p(&bro_window_simulateFocus), "void", {"bool"}, error);
    if (!ok) return false;
    return true;
}

}  // namespace bro::bronze_host
