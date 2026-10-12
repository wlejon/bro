// native_vision_register.cpp — registers every native of native_vision_decl.h with
// bronze (embed::registerNative) under __bro_native.vision. Hand-maintained: each
// registration's signature matches its prototype there. Call registerNatives_vision
// from registerBroNatives (src/bronze_host/host_natives.h), and from
// installWorkerBroRoot, on the thread that runs the program.

#include "native_vision_decl.h"

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

bool registerNatives_vision(std::string* error) {
    return fn("__bro_native.vision.dotRows", p(&bro_vision_dotRows), "f32[]", {"f32[]", "f32[]", "i32"}, error);
}

}  // namespace bro::bronze_host
