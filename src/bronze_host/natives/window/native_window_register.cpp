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

bool getter(const char* path, void* f, const char* ret, std::string* error) {
    return ev::registerNative(path, f, sig(ret, {}, ev::NativeKind::Getter), error);
}

bool setter(const char* path, void* f, const char* type, std::string* error) {
    return ev::registerNative(path, f, sig("void", {type}, ev::NativeKind::Setter), error);
}

}  // namespace

bool registerNatives_window(std::string* error) {
    const bool ok =
        getter("__bro_native.window.state", BRO_NATIVE_FN(bro_window_state_get), "str", error) &&
        getter("__bro_native.window.borderless", BRO_NATIVE_FN(bro_window_borderless_get), "bool", error) &&
        setter("__bro_native.window.borderless", BRO_NATIVE_FN(bro_window_borderless_set), "bool", error) &&
        getter("__bro_native.window.alwaysOnTop", BRO_NATIVE_FN(bro_window_alwaysOnTop_get), "bool", error) &&
        setter("__bro_native.window.alwaysOnTop", BRO_NATIVE_FN(bro_window_alwaysOnTop_set), "bool", error) &&
        getter("__bro_native.window.transparent", BRO_NATIVE_FN(bro_window_transparent_get), "bool", error) &&
        getter("__bro_native.window.clickThrough", BRO_NATIVE_FN(bro_window_clickThrough_get), "bool", error) &&
        setter("__bro_native.window.clickThrough", BRO_NATIVE_FN(bro_window_clickThrough_set), "bool", error) &&
        fn("__bro_native.window.minimize", BRO_NATIVE_FN(bro_window_minimize), "void", {}, error) &&
        fn("__bro_native.window.maximize", BRO_NATIVE_FN(bro_window_maximize), "void", {}, error) &&
        fn("__bro_native.window.restore", BRO_NATIVE_FN(bro_window_restore), "void", {}, error) &&
        fn("__bro_native.window.getPosition", BRO_NATIVE_FN(bro_window_getPosition), "void", {}, error) &&
        fn("__bro_native.window.getPosition_x", BRO_NATIVE_FN(bro_window_getPosition_x), "i32", {}, error) &&
        fn("__bro_native.window.getPosition_y", BRO_NATIVE_FN(bro_window_getPosition_y), "i32", {}, error) &&
        fn("__bro_native.window.setPosition", BRO_NATIVE_FN(bro_window_setPosition), "void", {"i32", "i32"}, error) &&
        fn("__bro_native.window.getMinSize", BRO_NATIVE_FN(bro_window_getMinSize), "void", {}, error) &&
        fn("__bro_native.window.getMinSize_width", BRO_NATIVE_FN(bro_window_getMinSize_width), "i32", {}, error) &&
        fn("__bro_native.window.getMinSize_height", BRO_NATIVE_FN(bro_window_getMinSize_height), "i32", {}, error) &&
        fn("__bro_native.window.setMinSize", BRO_NATIVE_FN(bro_window_setMinSize), "void", {"i32", "i32"}, error) &&
        fn("__bro_native.window.getMaxSize", BRO_NATIVE_FN(bro_window_getMaxSize), "void", {}, error) &&
        fn("__bro_native.window.getMaxSize_width", BRO_NATIVE_FN(bro_window_getMaxSize_width), "i32", {}, error) &&
        fn("__bro_native.window.getMaxSize_height", BRO_NATIVE_FN(bro_window_getMaxSize_height), "i32", {}, error) &&
        fn("__bro_native.window.setMaxSize", BRO_NATIVE_FN(bro_window_setMaxSize), "void", {"i32", "i32"}, error) &&
        fn("__bro_native.window.getDisplays", BRO_NATIVE_FN(bro_window_getDisplays), "i32", {}, error) &&
        fn("__bro_native.window.getDisplays_id", BRO_NATIVE_FN(bro_window_getDisplays_id), "f64", {"i32"}, error) &&
        fn("__bro_native.window.getDisplays_name", BRO_NATIVE_FN(bro_window_getDisplays_name), "str", {"i32"}, error) &&
        fn("__bro_native.window.getDisplays_x", BRO_NATIVE_FN(bro_window_getDisplays_x), "i32", {"i32"}, error) &&
        fn("__bro_native.window.getDisplays_y", BRO_NATIVE_FN(bro_window_getDisplays_y), "i32", {"i32"}, error) &&
        fn("__bro_native.window.getDisplays_width", BRO_NATIVE_FN(bro_window_getDisplays_width), "i32", {"i32"}, error) &&
        fn("__bro_native.window.getDisplays_height", BRO_NATIVE_FN(bro_window_getDisplays_height), "i32", {"i32"}, error) &&
        fn("__bro_native.window.getDisplays_workX", BRO_NATIVE_FN(bro_window_getDisplays_workX), "i32", {"i32"}, error) &&
        fn("__bro_native.window.getDisplays_workY", BRO_NATIVE_FN(bro_window_getDisplays_workY), "i32", {"i32"}, error) &&
        fn("__bro_native.window.getDisplays_workWidth", BRO_NATIVE_FN(bro_window_getDisplays_workWidth), "i32", {"i32"}, error) &&
        fn("__bro_native.window.getDisplays_workHeight", BRO_NATIVE_FN(bro_window_getDisplays_workHeight), "i32", {"i32"}, error) &&
        fn("__bro_native.window.getDisplays_refreshRate", BRO_NATIVE_FN(bro_window_getDisplays_refreshRate), "f64", {"i32"}, error) &&
        fn("__bro_native.window.getDisplays_contentScale", BRO_NATIVE_FN(bro_window_getDisplays_contentScale), "f64", {"i32"}, error) &&
        fn("__bro_native.window.getDisplays_isPrimary", BRO_NATIVE_FN(bro_window_getDisplays_isPrimary), "bool", {"i32"}, error) &&
        fn("__bro_native.window.getDisplays_isCurrent", BRO_NATIVE_FN(bro_window_getDisplays_isCurrent), "bool", {"i32"}, error) &&
        fn("__bro_native.window.moveToDisplay", BRO_NATIVE_FN(bro_window_moveToDisplay), "bool", {"f64"}, error) &&
        fn("__bro_native.window.getSize", BRO_NATIVE_FN(bro_window_getSize), "void", {}, error) &&
        fn("__bro_native.window.getSize_width", BRO_NATIVE_FN(bro_window_getSize_width), "i32", {}, error) &&
        fn("__bro_native.window.getSize_height", BRO_NATIVE_FN(bro_window_getSize_height), "i32", {}, error) &&
        fn("__bro_native.window.setSize", BRO_NATIVE_FN(bro_window_setSize), "void", {"i32", "i32"}, error) &&
        fn("__bro_native.window.quit", BRO_NATIVE_FN(bro_window_quit), "void", {}, error) &&
        getter("__bro_native.window.title", BRO_NATIVE_FN(bro_window_title_get), "str", error) &&
        setter("__bro_native.window.title", BRO_NATIVE_FN(bro_window_title_set), "str", error) &&
        getter("__bro_native.window.opacity", BRO_NATIVE_FN(bro_window_opacity_get), "f64", error) &&
        setter("__bro_native.window.opacity", BRO_NATIVE_FN(bro_window_opacity_set), "f64", error) &&
        getter("__bro_native.window.fullscreen", BRO_NATIVE_FN(bro_window_fullscreen_get), "bool", error) &&
        setter("__bro_native.window.fullscreen", BRO_NATIVE_FN(bro_window_fullscreen_set), "bool", error) &&
        getter("__bro_native.window.focused", BRO_NATIVE_FN(bro_window_focused_get), "bool", error) &&
        fn("__bro_native.window.flash", BRO_NATIVE_FN(bro_window_flash), "bool", {"bool"}, error) &&
        fn("__bro_native.window.beep", BRO_NATIVE_FN(bro_window_beep), "bool", {}, error) &&
        fn("__bro_native.window.getBeepCount", BRO_NATIVE_FN(bro_window_getBeepCount), "i32", {}, error) &&
        fn("__bro_native.window.resetBeepCount", BRO_NATIVE_FN(bro_window_resetBeepCount), "void", {}, error) &&
        fn("__bro_native.window.setProgress", BRO_NATIVE_FN(bro_window_setProgress), "bool", {"i32", "i32"}, error) &&
        fn("__bro_native.window.getProgressState", BRO_NATIVE_FN(bro_window_getProgressState), "i32", {}, error) &&
        fn("__bro_native.window.getProgressValue", BRO_NATIVE_FN(bro_window_getProgressValue), "i32", {}, error) &&
        fn("__bro_native.window.notify", BRO_NATIVE_FN(bro_window_notify), "i32", {"str", "str", "str", "i32", "bool", "i32", "str", "str"}, error) &&
        fn("__bro_native.window.getNotificationCount", BRO_NATIVE_FN(bro_window_getNotificationCount), "i32", {}, error) &&
        fn("__bro_native.window.getLastNotificationTitle", BRO_NATIVE_FN(bro_window_getLastNotificationTitle), "str", {}, error) &&
        fn("__bro_native.window.getLastNotificationBody", BRO_NATIVE_FN(bro_window_getLastNotificationBody), "str", {}, error) &&
        fn("__bro_native.window.clearNotifications", BRO_NATIVE_FN(bro_window_clearNotifications), "void", {}, error) &&
        fn("__bro_native.window.setTray", BRO_NATIVE_FN(bro_window_setTray), "bool", {"str", "str", "str"}, error) &&
        fn("__bro_native.window.removeTray", BRO_NATIVE_FN(bro_window_removeTray), "bool", {}, error) &&
        fn("__bro_native.window.hasTray", BRO_NATIVE_FN(bro_window_hasTray), "bool", {}, error) &&
        fn("__bro_native.window.isTrayAvailable", BRO_NATIVE_FN(bro_window_isTrayAvailable), "bool", {}, error) &&
        fn("__bro_native.window.simulateTrayClick", BRO_NATIVE_FN(bro_window_simulateTrayClick), "void", {"str"}, error) &&
        fn("__bro_native.window.registerGlobalHotkey", BRO_NATIVE_FN(bro_window_registerGlobalHotkey), "i32", {"str", "bool"}, error) &&
        fn("__bro_native.window.unregisterGlobalHotkey", BRO_NATIVE_FN(bro_window_unregisterGlobalHotkey), "bool", {"i32"}, error) &&
        fn("__bro_native.window.unregisterAllGlobalHotkeys", BRO_NATIVE_FN(bro_window_unregisterAllGlobalHotkeys), "void", {}, error) &&
        fn("__bro_native.window.simulateGlobalHotkey", BRO_NATIVE_FN(bro_window_simulateGlobalHotkey), "bool", {"str"}, error) &&
        fn("__bro_native.window.simulateHotkeyKey", BRO_NATIVE_FN(bro_window_simulateHotkeyKey), "i32", {"str", "str", "i32", "bool", "bool"}, error) &&
        fn("__bro_native.window.simulateShellHookKey", BRO_NATIVE_FN(bro_window_simulateShellHookKey), "i32", {"str", "str", "i32", "bool", "bool", "bool"}, error) &&
        fn("__bro_native.window.resetHotkeyKeys", BRO_NATIVE_FN(bro_window_resetHotkeyKeys), "void", {}, error) &&
        getter("__bro_native.window.hotkeyRouting", BRO_NATIVE_FN(bro_window_hotkeyRouting_get), "str", error) &&
        getter("__bro_native.window.displayMode", BRO_NATIVE_FN(bro_window_displayMode_get), "str", error) &&
        fn("__bro_native.window.captureScreen", BRO_NATIVE_FN(bro_window_captureScreen), "str", {"str"}, error) &&
        fn("__bro_native.window.requestSingleInstance", BRO_NATIVE_FN(bro_window_requestSingleInstance), "bool", {"str", "str"}, error) &&
        fn("__bro_native.window.shutdownSingleInstance", BRO_NATIVE_FN(bro_window_shutdownSingleInstance), "void", {}, error) &&
        fn("__bro_native.window.simulateSingleInstance", BRO_NATIVE_FN(bro_window_simulateSingleInstance), "bool", {"str", "str"}, error) &&
        fn("__bro_native.window._setFocusDispatcher", BRO_NATIVE_FN(bro_window_setFocusDispatcher), "void", {"dynamic"}, error) &&
        fn("__bro_native.window._setHotkeyDispatcher", BRO_NATIVE_FN(bro_window_setHotkeyDispatcher), "void", {"dynamic"}, error) &&
        fn("__bro_native.window._setTrayDispatcher", BRO_NATIVE_FN(bro_window_setTrayDispatcher), "void", {"dynamic"}, error) &&
        fn("__bro_native.window._setSingleInstanceDispatcher", BRO_NATIVE_FN(bro_window_setSingleInstanceDispatcher), "void", {"dynamic"}, error) &&
        fn("__bro_native.window.simulateFocus", BRO_NATIVE_FN(bro_window_simulateFocus), "void", {"bool"}, error);
    if (!ok) return false;
    return true;
}

bool registerWindowNatives(std::string* error) {
    return registerNatives_window(error);
}

}  // namespace bro::bronze_host
