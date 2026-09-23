// native_paths_register.cpp — registers every native of native_paths_decl.h with
// bronze (embed::registerNative) under __bro_native.paths. Hand-maintained: each
// registration's signature matches its prototype there. Call registerNatives_paths from registerBroNatives
// (src/bronze_host/host_natives.h) on the thread that runs the program.
//
// Class members are Function-kind natives taking the handle first (the wrapper's
// receiver is dynamic, so a Method-kind registration would be unreachable from it);
// each class has one Constructor-kind registration, which mints the class, and its
// prototype is published as __bro_native.paths.<Class>Proto for js/bro_core.js to chain.

#include "native_paths_decl.h"

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

template <typename F>
void* p(F* f) { return reinterpret_cast<void*>(f); }

}  // namespace

bool registerNatives_paths(std::string* error) {
    const bool ok =
        getter("__bro_native.paths.appDir", p(&bro_paths_appDir_get), "str", error) &&
        getter("__bro_native.paths.userDataDir", p(&bro_paths_userDataDir_get), "str", error) &&
        fn("__bro_native.paths.resolvePath", p(&bro_paths_resolvePath), "str", {"str"}, error) &&
        fn("__bro_native.paths.resolveWritePath", p(&bro_paths_resolveWritePath), "str", {"str"}, error);
    if (!ok) return false;
    return true;
}

}  // namespace bro::bronze_host
