// native_tables.cpp — registerBroNativeTables, every native table in the
// order the runtime registers them, and the registration helpers
// (host_natives.h) the tables share. A table, like this file, is compiled
// into bro-native-manifest as well as into bro_bronze_host, so nothing here
// may reach the engine (native_table.h).

#include "bronze_host/host_natives.h"

#include "embed/embed.h"

#include <initializer_list>
#include <string>

namespace bro::bronze_host {

namespace ev = bronze::embed;

namespace natives {

namespace {

ev::NativeSignature sig(const char* ret, std::initializer_list<const char*> params,
                        ev::NativeKind kind) {
    ev::NativeSignature s;
    s.returnType = ret;
    for (const char* p : params) s.paramTypes.emplace_back(p);
    s.kind = kind;
    return s;
}

}  // namespace

bool fn(const char* path, void* f, const char* ret,
        std::initializer_list<const char*> params, std::string* error) {
    return ev::registerNative(path, f, sig(ret, params, ev::NativeKind::Function), error);
}

bool getter(const char* path, void* f, const char* ret, std::string* error) {
    return ev::registerNative(path, f, sig(ret, {}, ev::NativeKind::Getter), error);
}

bool setter(const char* path, void* f, const char* type, std::string* error) {
    return ev::registerNative(path, f, sig("void", {type}, ev::NativeKind::Setter), error);
}

bool ctor(const char* path, void* f, void (*dtor)(void*),
          std::initializer_list<const char*> params, std::string* error) {
    ev::NativeSignature s = sig(path, params, ev::NativeKind::Constructor);
    s.className = path;
    s.destructor = dtor;
    return ev::registerNative(path, f, s, error);
}

}  // namespace natives

bool registerBroNativeTables(std::string* error) {
    bool ok = registerTimeNatives(error) &&
              registerServerNatives(error) &&
              registerPathsNatives(error) &&
              registerModelsNatives(error) &&
              registerWindowNatives(error) &&
              registerSettingsNatives(error) &&
              registerDunderBroNatives(error) &&
              registerMeshNatives(error) &&
              registerNetNatives(error) &&
              registerRiggingNatives(error) &&
              registerPhysicsNatives(error) &&
              registerLmNatives(error) &&
              registerRaveNatives(error) &&
              registerMotionNatives(error) &&
              registerProfilerNatives(error) &&
              registerMicNatives(error) &&
              registerSenseNatives(error) &&
              registerGestureNatives(error) &&
              registerWakeNatives(error) &&
              registerKwsNatives(error) &&
              registerListenNatives(error) &&
              registerTriposplatNatives(error) &&
              registerDiffusionNatives(error) &&
              registerVisionNatives(error) &&
              registerDiarNatives(error) &&
              registerSttNatives(error) &&
              registerTtsNatives(error) &&
              registerFloraNatives(error) &&
              registerTensorNatives(error);
#if BRO_WITH_3D
    ok = ok &&
         registerAnimationNatives(error) &&
         registerTerrainNatives(error) &&
         registerSceneNatives(error) &&
         registerTileWorldNatives(error) &&
         registerGizmoNatives(error) &&
         registerLightingNatives(error);
#endif
    return ok;
}

}  // namespace bro::bronze_host
