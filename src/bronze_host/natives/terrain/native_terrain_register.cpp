// native_terrain_register.cpp — registers every native of native_terrain_decl.h with
// bronze (embed::registerNative) under __bro_native.terrain. Hand-maintained: each
// registration's signature matches its prototype there. Call registerNatives_terrain from registerBroNatives
// (src/bronze_host/host_natives.h) on the thread that runs the program.
//
// Class members are Function-kind natives taking the handle first (the wrapper's
// receiver is dynamic, so a Method-kind registration would be unreachable from it);
// each class has one Constructor-kind registration, which mints the class, and its
// prototype is published as __bro_native.terrain.<Class>Proto for js/terrain.js to chain.

#include "native_terrain_decl.h"

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

bool registerNatives_terrain(std::string* error) {
    const bool ok =
        ctor("__bro_native.terrain.Terrain", p(&bro_terrain_Terrain_ctor), &bro_terrain_Terrain_dtor, bronze::runtime::Finalize::InSweep, {}, error) &&
        fn("__bro_native.terrain.Terrain_chunkCount_get", p(&bro_terrain_Terrain_chunkCount_get), "i32", {"__bro_native.terrain.Terrain"}, error) &&
        fn("__bro_native.terrain.Terrain_triangleCount_get", p(&bro_terrain_Terrain_triangleCount_get), "i32", {"__bro_native.terrain.Terrain"}, error) &&
        fn("__bro_native.terrain.Terrain_vertexCount_get", p(&bro_terrain_Terrain_vertexCount_get), "i32", {"__bro_native.terrain.Terrain"}, error) &&
        fn("__bro_native.terrain.Terrain_farDistance_get", p(&bro_terrain_Terrain_farDistance_get), "f64", {"__bro_native.terrain.Terrain"}, error) &&
        fn("__bro_native.terrain.Terrain_planetRadius_get", p(&bro_terrain_Terrain_planetRadius_get), "f64", {"__bro_native.terrain.Terrain"}, error) &&
        fn("__bro_native.terrain.Terrain_origin_get", p(&bro_terrain_Terrain_origin_get), "f64[]", {"__bro_native.terrain.Terrain"}, error) &&
        fn("__bro_native.terrain.Terrain_update", p(&bro_terrain_Terrain_update), "i32", {"__bro_native.terrain.Terrain", "f64", "f64", "f64"}, error) &&
        fn("__bro_native.terrain.Terrain_raycast", p(&bro_terrain_Terrain_raycast), "bool", {"__bro_native.terrain.Terrain", "f64[]", "f64[]", "bool", "f64"}, error) &&
        fn("__bro_native.terrain.Terrain_raycast_hit", p(&bro_terrain_Terrain_raycast_hit), "bool", {}, error) &&
        fn("__bro_native.terrain.Terrain_raycast_distance", p(&bro_terrain_Terrain_raycast_distance), "f64", {}, error) &&
        fn("__bro_native.terrain.Terrain_raycast_position", p(&bro_terrain_Terrain_raycast_position), "f64[]", {}, error) &&
        fn("__bro_native.terrain.Terrain_raycast_normal", p(&bro_terrain_Terrain_raycast_normal), "f64[]", {}, error) &&
        fn("__bro_native.terrain.Terrain_raycast_chunk", p(&bro_terrain_Terrain_raycast_chunk), "i32[]", {}, error) &&
        fn("__bro_native.terrain.Terrain_raycast_voxel", p(&bro_terrain_Terrain_raycast_voxel), "i32[]", {}, error) &&
        fn("__bro_native.terrain.Terrain_raycast_material", p(&bro_terrain_Terrain_raycast_material), "i32", {}, error) &&
        fn("__bro_native.terrain.Terrain_setVoxel", p(&bro_terrain_Terrain_setVoxel), "bool", {"__bro_native.terrain.Terrain", "f64", "f64", "f64", "i32"}, error) &&
        fn("__bro_native.terrain.Terrain_getVoxel", p(&bro_terrain_Terrain_getVoxel), "i32", {"__bro_native.terrain.Terrain", "f64", "f64", "f64"}, error) &&
        fn("__bro_native.terrain.Terrain_rebuild", p(&bro_terrain_Terrain_rebuild), "void", {"__bro_native.terrain.Terrain"}, error) &&
        fn("__bro_native.terrain.Terrain_configure", p(&bro_terrain_Terrain_configure), "void", {"__bro_native.terrain.Terrain", "i32[]", "bool", "f64", "bool", "i32", "bool", "i32", "bool", "i32", "bool", "i32", "bool", "f64", "bool", "i32", "bool", "f64", "bool", "f64", "bool", "i32", "bool", "i32", "bool", "i32", "bool", "i32", "bool", "f64", "bool", "f64", "bool", "f64", "bool", "f64", "bool", "f64", "bool", "f64", "bool", "i32", "bool", "i32", "bool", "i32", "bool", "f64", "f64[]", "f32[]"}, error) &&
        fn("__bro_native.terrain.Terrain_invalidateRegion", p(&bro_terrain_Terrain_invalidateRegion), "void", {"__bro_native.terrain.Terrain", "f64", "f64", "f64", "f64"}, error) &&
        fn("__bro_native.terrain.Terrain_setHeightSource", p(&bro_terrain_Terrain_setHeightSource), "void", {"__bro_native.terrain.Terrain", "dynamic"}, error) &&
        fn("__bro_native.terrain.Terrain_heightAt", p(&bro_terrain_Terrain_heightAt), "f64", {"__bro_native.terrain.Terrain", "f64", "f64"}, error) &&
        fn("__bro_native.terrain.Terrain_normalAt", p(&bro_terrain_Terrain_normalAt), "f64[]", {"__bro_native.terrain.Terrain", "f64", "f64"}, error) &&
        fn("__bro_native.terrain.Terrain_elevation", p(&bro_terrain_Terrain_elevation), "f64", {"__bro_native.terrain.Terrain", "f64", "f64"}, error) &&
        fn("__bro_native.terrain.Terrain_splat", p(&bro_terrain_Terrain_splat), "void", {"__bro_native.terrain.Terrain", "f64", "f64", "f64", "i32"}, error) &&
        fn("__bro_native.terrain.Terrain_layers_get", p(&bro_terrain_Terrain_layers_get), "i32", {"__bro_native.terrain.Terrain"}, error) &&
        fn("__bro_native.terrain.Terrain_destroy", p(&bro_terrain_Terrain_destroy), "void", {"__bro_native.terrain.Terrain"}, error);
    if (!ok) return false;
    publishPrototype("terrain", "__bro_native.terrain.Terrain", "TerrainProto");
    return true;
}

}  // namespace bro::bronze_host

#else  // !(BRO_WITH_3D)

#include <string>

namespace bro::bronze_host {

// Compiled out: nothing is registered and terrain.js is not to be installed.
bool registerNatives_terrain(std::string*) { return true; }

}  // namespace bro::bronze_host

#endif  // BRO_WITH_3D
