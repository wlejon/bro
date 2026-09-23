// native_clipmap_register.cpp — registers every native of native_clipmap_decl.h with
// bronze (embed::registerNative) under __bro_native.clipmap. Hand-maintained: each
// registration's signature matches its prototype there. Call registerNatives_clipmap from registerBroNatives
// (src/bronze_host/host_natives.h) on the thread that runs the program, after registerNatives_scene.
//
// Class members are Function-kind natives taking the handle first (the wrapper's
// receiver is dynamic, so a Method-kind registration would be unreachable from it);
// each class has one Constructor-kind registration, which mints the class, and its
// prototype is published as __bro_native.clipmap.<Class>Proto for js/clipmap.js to chain.

#include "native_clipmap_decl.h"

#if BRO_WITH_3D

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

template <typename F>
void* p(F* f) { return reinterpret_cast<void*>(f); }

}  // namespace

bool registerNatives_clipmap(std::string* error) {
    const bool ok =
        ctor("__bro_native.clipmap.ClipmapTerrain", p(&bro_clipmap_ClipmapTerrain_ctor), &bro_clipmap_ClipmapTerrain_dtor, bronze::runtime::Finalize::InSweep, {}, error) &&
        fn("__bro_native.clipmap.ClipmapTerrain_node_get", p(&bro_clipmap_ClipmapTerrain_node_get), "__bro_native.scene.SceneNode", {"__bro_native.clipmap.ClipmapTerrain"}, error) &&
        fn("__bro_native.clipmap.ClipmapTerrain_levels_get", p(&bro_clipmap_ClipmapTerrain_levels_get), "i32", {"__bro_native.clipmap.ClipmapTerrain"}, error) &&
        fn("__bro_native.clipmap.ClipmapTerrain_resolution_get", p(&bro_clipmap_ClipmapTerrain_resolution_get), "i32", {"__bro_native.clipmap.ClipmapTerrain"}, error) &&
        fn("__bro_native.clipmap.ClipmapTerrain_cellSize_get", p(&bro_clipmap_ClipmapTerrain_cellSize_get), "f64", {"__bro_native.clipmap.ClipmapTerrain"}, error) &&
        fn("__bro_native.clipmap.ClipmapTerrain_layerCount_get", p(&bro_clipmap_ClipmapTerrain_layerCount_get), "i32", {"__bro_native.clipmap.ClipmapTerrain"}, error) &&
        fn("__bro_native.clipmap.ClipmapTerrain_triangleCount_get", p(&bro_clipmap_ClipmapTerrain_triangleCount_get), "i32", {"__bro_native.clipmap.ClipmapTerrain"}, error) &&
        fn("__bro_native.clipmap.ClipmapTerrain_vertexCount_get", p(&bro_clipmap_ClipmapTerrain_vertexCount_get), "i32", {"__bro_native.clipmap.ClipmapTerrain"}, error) &&
        fn("__bro_native.clipmap.ClipmapTerrain_farDistance_get", p(&bro_clipmap_ClipmapTerrain_farDistance_get), "f64", {"__bro_native.clipmap.ClipmapTerrain"}, error) &&
        fn("__bro_native.clipmap.ClipmapTerrain_cellScale_get", p(&bro_clipmap_ClipmapTerrain_cellScale_get), "f64", {"__bro_native.clipmap.ClipmapTerrain"}, error) &&
        fn("__bro_native.clipmap.ClipmapTerrain_planetRadius_get", p(&bro_clipmap_ClipmapTerrain_planetRadius_get), "f64", {"__bro_native.clipmap.ClipmapTerrain"}, error) &&
        fn("__bro_native.clipmap.ClipmapTerrain_setSnowLine", p(&bro_clipmap_ClipmapTerrain_setSnowLine), "__bro_native.clipmap.ClipmapTerrain", {"__bro_native.clipmap.ClipmapTerrain", "f64"}, error) &&
        fn("__bro_native.clipmap.ClipmapTerrain_setDetail", p(&bro_clipmap_ClipmapTerrain_setDetail), "__bro_native.clipmap.ClipmapTerrain", {"__bro_native.clipmap.ClipmapTerrain", "bool", "f64", "bool", "f64", "bool", "f64", "bool", "i32"}, error) &&
        fn("__bro_native.clipmap.ClipmapTerrain_setMaterials", p(&bro_clipmap_ClipmapTerrain_setMaterials), "__bro_native.clipmap.ClipmapTerrain", {"__bro_native.clipmap.ClipmapTerrain", "f64[]", "bool", "f64", "f64[]", "bool", "f64", "f64[]", "bool", "f64", "f64[]", "bool", "f64"}, error) &&
        fn("__bro_native.clipmap.ClipmapTerrain_setForest", p(&bro_clipmap_ClipmapTerrain_setForest), "__bro_native.clipmap.ClipmapTerrain", {"__bro_native.clipmap.ClipmapTerrain", "f64[]", "bool", "f64"}, error) &&
        fn("__bro_native.clipmap.ClipmapTerrain_update", p(&bro_clipmap_ClipmapTerrain_update), "__bro_native.clipmap.ClipmapTerrain", {"__bro_native.clipmap.ClipmapTerrain", "f64", "f64", "f64"}, error) &&
        fn("__bro_native.clipmap.ClipmapTerrain_shaderSource", p(&bro_clipmap_ClipmapTerrain_shaderSource), "str", {"__bro_native.clipmap.ClipmapTerrain", "str"}, error) &&
        fn("__bro_native.clipmap.ClipmapTerrain_elevationAt", p(&bro_clipmap_ClipmapTerrain_elevationAt), "f64", {"__bro_native.clipmap.ClipmapTerrain", "f64", "f64"}, error) &&
        fn("__bro_native.clipmap.ClipmapTerrain_renderedElevationAt", p(&bro_clipmap_ClipmapTerrain_renderedElevationAt), "f64", {"__bro_native.clipmap.ClipmapTerrain", "f64", "f64"}, error) &&
        fn("__bro_native.clipmap.ClipmapTerrain_coverageDistance", p(&bro_clipmap_ClipmapTerrain_coverageDistance), "f64", {"__bro_native.clipmap.ClipmapTerrain", "f64"}, error) &&
        fn("__bro_native.clipmap.ClipmapTerrain_horizonDistance", p(&bro_clipmap_ClipmapTerrain_horizonDistance), "f64", {"__bro_native.clipmap.ClipmapTerrain", "f64"}, error) &&
        fn("__bro_native.clipmap.ClipmapTerrain_destroy", p(&bro_clipmap_ClipmapTerrain_destroy), "void", {"__bro_native.clipmap.ClipmapTerrain"}, error);
    if (!ok) return false;
    publishPrototype("clipmap", "__bro_native.clipmap.ClipmapTerrain", "ClipmapTerrainProto");
    return true;
}

}  // namespace bro::bronze_host

#else  // !(BRO_WITH_3D)

#include <string>

namespace bro::bronze_host {

// Compiled out: nothing is registered and clipmap.js is not to be installed.
bool registerNatives_clipmap(std::string*) { return true; }

}  // namespace bro::bronze_host

#endif  // BRO_WITH_3D
