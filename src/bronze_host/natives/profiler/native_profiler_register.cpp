// native_profiler_register.cpp — registers every native of native_profiler_decl.h with
// bronze (embed::registerNative) under __bro_native.profiler. Hand-maintained: each
// registration's signature matches its prototype there. Call registerNatives_profiler
// from registerBroNatives (src/bronze_host/host_natives.h) on the thread that runs the
// program.

#include "native_profiler_decl.h"

#include "embed/embed.h"

#include <initializer_list>
#include <string>

namespace bro::bronze_host {

namespace {

namespace ev = bronze::embed;

bool fn(const char* path, void* f, const char* ret, std::initializer_list<const char*> params, std::string* error) {
    ev::NativeSignature s;
    s.returnType = ret;
    for (const char* p : params) s.paramTypes.emplace_back(p);
    s.kind = ev::NativeKind::Function;
    return ev::registerNative(path, f, s, error);
}

template <typename F>
void* p(F* f) { return reinterpret_cast<void*>(f); }

}  // namespace

bool registerNatives_profiler(std::string* error) {
    return fn("__bro_native.profiler.start", p(&bro_profiler_start), "str", {"f64", "str"}, error) &&
           fn("__bro_native.profiler.running", p(&bro_profiler_running), "bool", {}, error) &&
           fn("__bro_native.profiler.stop", p(&bro_profiler_stop), "str", {"bool", "bool", "i32"}, error);
}

}  // namespace bro::bronze_host
