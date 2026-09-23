// native_tile_world_register.cpp — registers every native of native_tile_world_decl.h with
// bronze (embed::registerNative) under __bro_native.tile_world. Hand-maintained: each
// registration's signature matches its prototype there. Call registerNatives_tile_world from registerBroNatives
// (src/bronze_host/host_natives.h) on the thread that runs the program, after registerNatives_scene.
//
// Class members are Function-kind natives taking the handle first (the wrapper's
// receiver is dynamic, so a Method-kind registration would be unreachable from it);
// each class has one Constructor-kind registration, which mints the class, and its
// prototype is published as __bro_native.tile_world.<Class>Proto for js/tile_world.js to chain.

#include "native_tile_world_decl.h"

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

bool registerNatives_tile_world(std::string* error) {
    const bool ok =
        ctor("__bro_native.tile_world.TileWorld", p(&bro_tile_world_TileWorld_ctor), &bro_tile_world_TileWorld_dtor, bronze::runtime::Finalize::InSweep, {}, error) &&
        fn("__bro_native.tile_world.TileWorld_node_get", p(&bro_tile_world_TileWorld_node_get), "__bro_native.scene.SceneNode", {"__bro_native.tile_world.TileWorld"}, error) &&
        fn("__bro_native.tile_world.TileWorld_width_get", p(&bro_tile_world_TileWorld_width_get), "i32", {"__bro_native.tile_world.TileWorld"}, error) &&
        fn("__bro_native.tile_world.TileWorld_height_get", p(&bro_tile_world_TileWorld_height_get), "i32", {"__bro_native.tile_world.TileWorld"}, error) &&
        fn("__bro_native.tile_world.TileWorld_chunkCount_get", p(&bro_tile_world_TileWorld_chunkCount_get), "i32", {"__bro_native.tile_world.TileWorld"}, error) &&
        fn("__bro_native.tile_world.TileWorld_chunks_get", p(&bro_tile_world_TileWorld_chunks_get), "i32", {"__bro_native.tile_world.TileWorld"}, error) &&
        fn("__bro_native.tile_world.TileWorld_vertexCount_get", p(&bro_tile_world_TileWorld_vertexCount_get), "i32", {"__bro_native.tile_world.TileWorld"}, error) &&
        fn("__bro_native.tile_world.TileWorld_triangleCount_get", p(&bro_tile_world_TileWorld_triangleCount_get), "i32", {"__bro_native.tile_world.TileWorld"}, error) &&
        fn("__bro_native.tile_world.TileWorld_setTile", p(&bro_tile_world_TileWorld_setTile), "void", {"__bro_native.tile_world.TileWorld", "i32", "i32", "i32", "i32"}, error) &&
        fn("__bro_native.tile_world.TileWorld_getTile", p(&bro_tile_world_TileWorld_getTile), "i32", {"__bro_native.tile_world.TileWorld", "i32", "i32", "i32"}, error) &&
        fn("__bro_native.tile_world.TileWorld_fillRect", p(&bro_tile_world_TileWorld_fillRect), "void", {"__bro_native.tile_world.TileWorld", "i32", "i32", "i32", "i32", "i32", "i32"}, error) &&
        fn("__bro_native.tile_world.TileWorld_clearLayer", p(&bro_tile_world_TileWorld_clearLayer), "void", {"__bro_native.tile_world.TileWorld", "i32"}, error) &&
        fn("__bro_native.tile_world.TileWorld_pickTile", p(&bro_tile_world_TileWorld_pickTile), "bool", {"__bro_native.tile_world.TileWorld", "f64", "f64"}, error) &&
        fn("__bro_native.tile_world.TileWorld_pickTile_tileX", p(&bro_tile_world_TileWorld_pickTile_tileX), "i32", {}, error) &&
        fn("__bro_native.tile_world.TileWorld_pickTile_tileY", p(&bro_tile_world_TileWorld_pickTile_tileY), "i32", {}, error) &&
        fn("__bro_native.tile_world.TileWorld_pickTile_layer", p(&bro_tile_world_TileWorld_pickTile_layer), "i32", {}, error) &&
        fn("__bro_native.tile_world.TileWorld_pickTile_tileId", p(&bro_tile_world_TileWorld_pickTile_tileId), "i32", {}, error) &&
        fn("__bro_native.tile_world.TileWorld_pickTile_worldPosition", p(&bro_tile_world_TileWorld_pickTile_worldPosition), "f64[]", {}, error) &&
        fn("__bro_native.tile_world.TileWorld_findPath", p(&bro_tile_world_TileWorld_findPath), "void", {"__bro_native.tile_world.TileWorld", "i32", "i32", "i32", "i32", "bool", "i32", "bool", "bool", "bool", "i32"}, error) &&
        fn("__bro_native.tile_world.TileWorld_findPath_reachable", p(&bro_tile_world_TileWorld_findPath_reachable), "bool", {}, error) &&
        fn("__bro_native.tile_world.TileWorld_findPath_path", p(&bro_tile_world_TileWorld_findPath_path), "str", {}, error) &&
        fn("__bro_native.tile_world.TileWorld_findPath_totalCost", p(&bro_tile_world_TileWorld_findPath_totalCost), "f64", {}, error) &&
        fn("__bro_native.tile_world.TileWorld_computeRegions", p(&bro_tile_world_TileWorld_computeRegions), "i32", {"__bro_native.tile_world.TileWorld", "i32"}, error) &&
        fn("__bro_native.tile_world.TileWorld_computeRegions_regionId", p(&bro_tile_world_TileWorld_computeRegions_regionId), "i32", {"i32"}, error) &&
        fn("__bro_native.tile_world.TileWorld_computeRegions_tiles", p(&bro_tile_world_TileWorld_computeRegions_tiles), "str", {"i32"}, error) &&
        fn("__bro_native.tile_world.TileWorld_computeRegions_area", p(&bro_tile_world_TileWorld_computeRegions_area), "i32", {"i32"}, error) &&
        fn("__bro_native.tile_world.TileWorld_setOrigin", p(&bro_tile_world_TileWorld_setOrigin), "void", {"__bro_native.tile_world.TileWorld", "f64", "f64", "f64"}, error) &&
        fn("__bro_native.tile_world.TileWorld_advance", p(&bro_tile_world_TileWorld_advance), "bool", {"__bro_native.tile_world.TileWorld", "f64"}, error) &&
        fn("__bro_native.tile_world.TileWorld_addObject", p(&bro_tile_world_TileWorld_addObject), "void", {"__bro_native.tile_world.TileWorld", "i32", "f64", "f64", "f64"}, error) &&
        fn("__bro_native.tile_world.TileWorld_clearObjects", p(&bro_tile_world_TileWorld_clearObjects), "void", {"__bro_native.tile_world.TileWorld", "bool", "i32"}, error) &&
        fn("__bro_native.tile_world.TileWorld_objectCount", p(&bro_tile_world_TileWorld_objectCount), "i32", {"__bro_native.tile_world.TileWorld", "i32"}, error) &&
        fn("__bro_native.tile_world.TileWorld_rebuildObjects", p(&bro_tile_world_TileWorld_rebuildObjects), "void", {"__bro_native.tile_world.TileWorld"}, error) &&
        fn("__bro_native.tile_world.TileWorld_rebuild", p(&bro_tile_world_TileWorld_rebuild), "void", {"__bro_native.tile_world.TileWorld"}, error) &&
        fn("__bro_native.tile_world.TileWorld_rebuildAll", p(&bro_tile_world_TileWorld_rebuildAll), "void", {"__bro_native.tile_world.TileWorld"}, error) &&
        fn("__bro_native.tile_world.TileWorld_save", p(&bro_tile_world_TileWorld_save), "u8[]", {"__bro_native.tile_world.TileWorld"}, error) &&
        fn("__bro_native.tile_world.TileWorld_load", p(&bro_tile_world_TileWorld_load), "bool", {"__bro_native.tile_world.TileWorld", "u8[]"}, error) &&
        fn("__bro_native.tile_world.TileWorld_destroy", p(&bro_tile_world_TileWorld_destroy), "void", {"__bro_native.tile_world.TileWorld"}, error);
    if (!ok) return false;
    publishPrototype("tile_world", "__bro_native.tile_world.TileWorld", "TileWorldProto");
    return true;
}

}  // namespace bro::bronze_host

#else  // !(BRO_WITH_3D)

#include <string>

namespace bro::bronze_host {

// Compiled out: nothing is registered and tile_world.js is not to be installed.
bool registerNatives_tile_world(std::string*) { return true; }

}  // namespace bro::bronze_host

#endif  // BRO_WITH_3D
