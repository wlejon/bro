// native_models_register.cpp — registers every native of native_models_decl.h with
// bronze (embed::registerNative) under __bro_native.models. Hand-maintained: each
// registration's signature matches its prototype there. Called from registerModelsNatives
// (native_models.cpp), part of registerBroNatives (src/bronze_host/host_natives.h).

#include "native_models_decl.h"

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

bool registerNatives_models(std::string* error) {
    return fn("__bro_native.models.dirs", p(&bro_models_dirs), "str", {}, error) &&
           fn("__bro_native.models.find", p(&bro_models_find), "str", {"str"}, error);
}

}  // namespace bro::bronze_host
