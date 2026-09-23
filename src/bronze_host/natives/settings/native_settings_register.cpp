// native_settings_register.cpp — registers every native of native_settings_decl.h with
// bronze (embed::registerNative) under __bro_native.settings. Hand-maintained: each
// registration's signature matches its prototype there. Call registerNatives_settings from registerBroNatives
// (src/bronze_host/host_natives.h) on the thread that runs the program.
//
// Class members are Function-kind natives taking the handle first (the wrapper's
// receiver is dynamic, so a Method-kind registration would be unreachable from it);
// each class has one Constructor-kind registration, which mints the class, and its
// prototype is published as __bro_native.settings.<Class>Proto for js/bro_core.js to chain.

#include "native_settings_decl.h"

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

template <typename F>
void* p(F* f) { return reinterpret_cast<void*>(f); }

}  // namespace

bool registerNatives_settings(std::string* error) {
    const bool ok =
        fn("__bro_native.settings.get", p(&bro_settings_get), "str", {"str"}, error) &&
        fn("__bro_native.settings.getAllJson", p(&bro_settings_getAllJson), "str", {"str"}, error) &&
        fn("__bro_native.settings.getDefaultsJson", p(&bro_settings_getDefaultsJson), "str", {"str"}, error) &&
        fn("__bro_native.settings.setString", p(&bro_settings_setString), "void", {"str", "str"}, error) &&
        fn("__bro_native.settings.setNumber", p(&bro_settings_setNumber), "void", {"str", "f64"}, error) &&
        fn("__bro_native.settings.setBool", p(&bro_settings_setBool), "void", {"str", "bool"}, error) &&
        fn("__bro_native.settings.setDefaultString", p(&bro_settings_setDefaultString), "void", {"str", "str"}, error) &&
        fn("__bro_native.settings.setDefaultNumber", p(&bro_settings_setDefaultNumber), "void", {"str", "f64"}, error) &&
        fn("__bro_native.settings.setDefaultBool", p(&bro_settings_setDefaultBool), "void", {"str", "bool"}, error) &&
        fn("__bro_native.settings.reset", p(&bro_settings_reset), "void", {"str"}, error) &&
        fn("__bro_native.settings.defineAction", p(&bro_settings_defineAction), "void", {"str", "str", "f64"}, error) &&
        fn("__bro_native.settings.rebindAction", p(&bro_settings_rebindAction), "void", {"str", "str"}, error) &&
        fn("__bro_native.settings.resetAction", p(&bro_settings_resetAction), "void", {"str"}, error) &&
        fn("__bro_native.settings.resetAllActions", p(&bro_settings_resetAllActions), "void", {}, error) &&
        fn("__bro_native.settings.actionKeysJson", p(&bro_settings_actionKeysJson), "str", {"str"}, error) &&
        fn("__bro_native.settings.keyAction", p(&bro_settings_keyAction), "str", {"str"}, error) &&
        fn("__bro_native.settings.actionStrength", p(&bro_settings_actionStrength), "f64", {"str"}, error) &&
        fn("__bro_native.settings.isActionPressed", p(&bro_settings_isActionPressed), "bool", {"str"}, error) &&
        fn("__bro_native.settings.actionsJson", p(&bro_settings_actionsJson), "str", {}, error) &&
        fn("__bro_native.settings.appActionsJson", p(&bro_settings_appActionsJson), "str", {}, error) &&
        fn("__bro_native.settings.displayModesJson", p(&bro_settings_displayModesJson), "str", {}, error) &&
        fn("__bro_native.settings.onChange", p(&bro_settings_onChange), "void", {"dynamic"}, error);
    if (!ok) return false;
    return true;
}

}  // namespace bro::bronze_host
