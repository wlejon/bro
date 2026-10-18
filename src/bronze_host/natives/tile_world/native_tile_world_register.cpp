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
#include "bronze_host/native_table.h"
#include "bronze_host/host_natives.h"

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

}  // namespace

bool registerNatives_tile_world(std::string* error) {
    const bool ok =
        ctor("__bro_native.tile_world.TileWorld", BRO_NATIVE_FN(bro_tile_world_TileWorld_ctor), BRO_NATIVE_DTOR(bro_tile_world_TileWorld_dtor), bronze::runtime::Finalize::InSweep, {}, error) &&
        fn("__bro_native.tile_world.TileWorld_node_get", BRO_NATIVE_FN(bro_tile_world_TileWorld_node_get), "__bro_native.scene.SceneNode", {"__bro_native.tile_world.TileWorld"}, error) &&
        fn("__bro_native.tile_world.TileWorld_width_get", BRO_NATIVE_FN(bro_tile_world_TileWorld_width_get), "i32", {"__bro_native.tile_world.TileWorld"}, error) &&
        fn("__bro_native.tile_world.TileWorld_height_get", BRO_NATIVE_FN(bro_tile_world_TileWorld_height_get), "i32", {"__bro_native.tile_world.TileWorld"}, error) &&
        fn("__bro_native.tile_world.TileWorld_chunkCount_get", BRO_NATIVE_FN(bro_tile_world_TileWorld_chunkCount_get), "i32", {"__bro_native.tile_world.TileWorld"}, error) &&
        fn("__bro_native.tile_world.TileWorld_chunks_get", BRO_NATIVE_FN(bro_tile_world_TileWorld_chunks_get), "i32", {"__bro_native.tile_world.TileWorld"}, error) &&
        fn("__bro_native.tile_world.TileWorld_vertexCount_get", BRO_NATIVE_FN(bro_tile_world_TileWorld_vertexCount_get), "i32", {"__bro_native.tile_world.TileWorld"}, error) &&
        fn("__bro_native.tile_world.TileWorld_triangleCount_get", BRO_NATIVE_FN(bro_tile_world_TileWorld_triangleCount_get), "i32", {"__bro_native.tile_world.TileWorld"}, error) &&
        fn("__bro_native.tile_world.TileWorld_setTile", BRO_NATIVE_FN(bro_tile_world_TileWorld_setTile), "void", {"__bro_native.tile_world.TileWorld", "i32", "i32", "i32", "i32"}, error) &&
        fn("__bro_native.tile_world.TileWorld_getTile", BRO_NATIVE_FN(bro_tile_world_TileWorld_getTile), "i32", {"__bro_native.tile_world.TileWorld", "i32", "i32", "i32"}, error) &&
        fn("__bro_native.tile_world.TileWorld_fillRect", BRO_NATIVE_FN(bro_tile_world_TileWorld_fillRect), "void", {"__bro_native.tile_world.TileWorld", "i32", "i32", "i32", "i32", "i32", "i32"}, error) &&
        fn("__bro_native.tile_world.TileWorld_clearLayer", BRO_NATIVE_FN(bro_tile_world_TileWorld_clearLayer), "void", {"__bro_native.tile_world.TileWorld", "i32"}, error) &&
        fn("__bro_native.tile_world.TileWorld_pickTile", BRO_NATIVE_FN(bro_tile_world_TileWorld_pickTile), "bool", {"__bro_native.tile_world.TileWorld", "f64", "f64"}, error) &&
        fn("__bro_native.tile_world.TileWorld_pickTile_tileX", BRO_NATIVE_FN(bro_tile_world_TileWorld_pickTile_tileX), "i32", {}, error) &&
        fn("__bro_native.tile_world.TileWorld_pickTile_tileY", BRO_NATIVE_FN(bro_tile_world_TileWorld_pickTile_tileY), "i32", {}, error) &&
        fn("__bro_native.tile_world.TileWorld_pickTile_layer", BRO_NATIVE_FN(bro_tile_world_TileWorld_pickTile_layer), "i32", {}, error) &&
        fn("__bro_native.tile_world.TileWorld_pickTile_tileId", BRO_NATIVE_FN(bro_tile_world_TileWorld_pickTile_tileId), "i32", {}, error) &&
        fn("__bro_native.tile_world.TileWorld_pickTile_worldPosition", BRO_NATIVE_FN(bro_tile_world_TileWorld_pickTile_worldPosition), "f64[]", {}, error) &&
        fn("__bro_native.tile_world.TileWorld_findPath", BRO_NATIVE_FN(bro_tile_world_TileWorld_findPath), "void", {"__bro_native.tile_world.TileWorld", "i32", "i32", "i32", "i32", "bool", "i32", "bool", "bool", "bool", "i32"}, error) &&
        fn("__bro_native.tile_world.TileWorld_findPath_reachable", BRO_NATIVE_FN(bro_tile_world_TileWorld_findPath_reachable), "bool", {}, error) &&
        fn("__bro_native.tile_world.TileWorld_findPath_path", BRO_NATIVE_FN(bro_tile_world_TileWorld_findPath_path), "str", {}, error) &&
        fn("__bro_native.tile_world.TileWorld_findPath_totalCost", BRO_NATIVE_FN(bro_tile_world_TileWorld_findPath_totalCost), "f64", {}, error) &&
        fn("__bro_native.tile_world.TileWorld_computeRegions", BRO_NATIVE_FN(bro_tile_world_TileWorld_computeRegions), "i32", {"__bro_native.tile_world.TileWorld", "i32"}, error) &&
        fn("__bro_native.tile_world.TileWorld_computeRegions_regionId", BRO_NATIVE_FN(bro_tile_world_TileWorld_computeRegions_regionId), "i32", {"i32"}, error) &&
        fn("__bro_native.tile_world.TileWorld_computeRegions_tiles", BRO_NATIVE_FN(bro_tile_world_TileWorld_computeRegions_tiles), "str", {"i32"}, error) &&
        fn("__bro_native.tile_world.TileWorld_computeRegions_area", BRO_NATIVE_FN(bro_tile_world_TileWorld_computeRegions_area), "i32", {"i32"}, error) &&
        fn("__bro_native.tile_world.TileWorld_setOrigin", BRO_NATIVE_FN(bro_tile_world_TileWorld_setOrigin), "void", {"__bro_native.tile_world.TileWorld", "f64", "f64", "f64"}, error) &&
        fn("__bro_native.tile_world.TileWorld_advance", BRO_NATIVE_FN(bro_tile_world_TileWorld_advance), "bool", {"__bro_native.tile_world.TileWorld", "f64"}, error) &&
        fn("__bro_native.tile_world.TileWorld_addObject", BRO_NATIVE_FN(bro_tile_world_TileWorld_addObject), "void", {"__bro_native.tile_world.TileWorld", "i32", "f64", "f64", "f64"}, error) &&
        fn("__bro_native.tile_world.TileWorld_clearObjects", BRO_NATIVE_FN(bro_tile_world_TileWorld_clearObjects), "void", {"__bro_native.tile_world.TileWorld", "bool", "i32"}, error) &&
        fn("__bro_native.tile_world.TileWorld_objectCount", BRO_NATIVE_FN(bro_tile_world_TileWorld_objectCount), "i32", {"__bro_native.tile_world.TileWorld", "i32"}, error) &&
        fn("__bro_native.tile_world.TileWorld_rebuildObjects", BRO_NATIVE_FN(bro_tile_world_TileWorld_rebuildObjects), "void", {"__bro_native.tile_world.TileWorld"}, error) &&
        fn("__bro_native.tile_world.TileWorld_rebuild", BRO_NATIVE_FN(bro_tile_world_TileWorld_rebuild), "void", {"__bro_native.tile_world.TileWorld"}, error) &&
        fn("__bro_native.tile_world.TileWorld_rebuildAll", BRO_NATIVE_FN(bro_tile_world_TileWorld_rebuildAll), "void", {"__bro_native.tile_world.TileWorld"}, error) &&
        fn("__bro_native.tile_world.TileWorld_save", BRO_NATIVE_FN(bro_tile_world_TileWorld_save), "u8[]", {"__bro_native.tile_world.TileWorld"}, error) &&
        fn("__bro_native.tile_world.TileWorld_load", BRO_NATIVE_FN(bro_tile_world_TileWorld_load), "bool", {"__bro_native.tile_world.TileWorld", "u8[]"}, error) &&
        fn("__bro_native.tile_world.TileWorld_destroy", BRO_NATIVE_FN(bro_tile_world_TileWorld_destroy), "void", {"__bro_native.tile_world.TileWorld"}, error);
    if (!ok) return false;
    publishPrototype("tile_world", "__bro_native.tile_world.TileWorld", "TileWorldProto");
    return true;
}

bool registerTileWorldOpsNatives(std::string* error);
bool registerTileWorldObjectNatives(std::string* error);

bool registerTileWorldOpsNatives(std::string* error) {
    return natives::fn("__bro_native.tile_world.TileWorld_distanceField", BRO_NATIVE_FN(bro_tile_world_TileWorld_distanceField), "str", {"__bro_native.tile_world.TileWorld", "str", "str"}, error) &&
           natives::fn("__bro_native.tile_world.TileWorld_floodFill", BRO_NATIVE_FN(bro_tile_world_TileWorld_floodFill), "str", {"__bro_native.tile_world.TileWorld", "i32", "i32", "str"}, error) &&
           natives::fn("__bro_native.tile_world.TileWorld_components", BRO_NATIVE_FN(bro_tile_world_TileWorld_components), "str", {"__bro_native.tile_world.TileWorld", "str"}, error) &&
           natives::fn("__bro_native.tile_world.TileWorld_cellDistance", BRO_NATIVE_FN(bro_tile_world_TileWorld_cellDistance), "i32", {"__bro_native.tile_world.TileWorld", "i32", "i32", "i32", "i32", "str"}, error) &&
           natives::fn("__bro_native.tile_world.TileWorld_cellRing", BRO_NATIVE_FN(bro_tile_world_TileWorld_cellRing), "str", {"__bro_native.tile_world.TileWorld", "i32", "i32", "i32", "str"}, error) &&
           natives::fn("__bro_native.tile_world.TileWorld_cellsInRange", BRO_NATIVE_FN(bro_tile_world_TileWorld_cellsInRange), "str", {"__bro_native.tile_world.TileWorld", "i32", "i32", "i32", "str"}, error) &&
           natives::fn("__bro_native.tile_world.TileWorld_cellLine", BRO_NATIVE_FN(bro_tile_world_TileWorld_cellLine), "str", {"__bro_native.tile_world.TileWorld", "i32", "i32", "i32", "i32"}, error) &&
           natives::fn("__bro_native.tile_world.TileWorld_cellNeighbors", BRO_NATIVE_FN(bro_tile_world_TileWorld_cellNeighbors), "str", {"__bro_native.tile_world.TileWorld", "i32", "i32", "str"}, error);
}

bool registerTileWorldObjectNatives(std::string* error) {
    const char* TW = "__bro_native.tile_world.TileWorld";
    return natives::fn("__bro_native.tile_world.TileWorld_addObjectKind", BRO_NATIVE_FN(bro_tile_world_TileWorld_addObjectKind), "i32", {TW, "dynamic", "str"}, error) &&
           natives::fn("__bro_native.tile_world.TileWorld_replaceObjectKind", BRO_NATIVE_FN(bro_tile_world_TileWorld_replaceObjectKind), "bool", {TW, "i32", "dynamic", "bool", "str"}, error) &&
           natives::fn("__bro_native.tile_world.TileWorld_removeObjectKind", BRO_NATIVE_FN(bro_tile_world_TileWorld_removeObjectKind), "bool", {TW, "i32"}, error) &&
           natives::fn("__bro_native.tile_world.TileWorld_hasObjectKind", BRO_NATIVE_FN(bro_tile_world_TileWorld_hasObjectKind), "bool", {TW, "i32"}, error) &&
           natives::fn("__bro_native.tile_world.TileWorld_addObjectPlacement", BRO_NATIVE_FN(bro_tile_world_TileWorld_addObjectPlacement), "i32", {TW, "i32", "i32", "i32", "f64", "f64", "f64", "f64", "f64", "i32", "f64[]"}, error) &&
           natives::fn("__bro_native.tile_world.TileWorld_replaceObjectPlacement", BRO_NATIVE_FN(bro_tile_world_TileWorld_replaceObjectPlacement), "bool", {TW, "i32", "i32", "i32", "i32", "f64", "f64", "f64", "f64", "f64", "i32", "f64[]"}, error) &&
           natives::fn("__bro_native.tile_world.TileWorld_removeObject", BRO_NATIVE_FN(bro_tile_world_TileWorld_removeObject), "bool", {TW, "i32", "i32"}, error);
}

bool registerTileWorldNatives(std::string* error) {
    if (!registerNatives_tile_world(error)) return false;

    auto reg = [&](const char* path, void* f, const char* ret, std::initializer_list<const char*> params) {
        bronze::embed::NativeSignature s;
        s.returnType = ret;
        for (const char* p : params) s.paramTypes.emplace_back(p);
        s.kind = bronze::embed::NativeKind::Function;
        return bronze::embed::registerNative(path, f, s, error);
    };

    if (!reg("__bro_native.tile_world.createTileWorldJson", BRO_NATIVE_FN(bro_scene_SceneGraph_createTileWorldJson),
             "__bro_native.tile_world.TileWorld", {"__bro_native.scene.SceneGraph", "str"})) return false;
    if (!reg("__bro_native.tile_world.TileWorld_findPathJson", BRO_NATIVE_FN(bro_tile_world_TileWorld_findPathJson),
             "void", {"__bro_native.tile_world.TileWorld", "i32", "i32", "i32", "i32", "str"})) return false;
    if (!reg("__bro_native.tile_world.TileWorld_fillTile", BRO_NATIVE_FN(bro_tile_world_TileWorld_fillTile),
             "void", {"__bro_native.tile_world.TileWorld", "i32", "i32", "i32", "i32", "i32", "i32"})) return false;
    if (!reg("__bro_native.tile_world.TileWorld_setElevation", BRO_NATIVE_FN(bro_tile_world_TileWorld_setElevation),
             "void", {"__bro_native.tile_world.TileWorld", "i32", "i32", "i32"})) return false;
    if (!reg("__bro_native.tile_world.TileWorld_getElevation", BRO_NATIVE_FN(bro_tile_world_TileWorld_getElevation),
             "i32", {"__bro_native.tile_world.TileWorld", "i32", "i32"})) return false;
    if (!reg("__bro_native.tile_world.TileWorld_fillElevation", BRO_NATIVE_FN(bro_tile_world_TileWorld_fillElevation),
             "void", {"__bro_native.tile_world.TileWorld", "i32", "i32", "i32", "i32", "i32"})) return false;
    if (!reg("__bro_native.tile_world.TileWorld_setFlag", BRO_NATIVE_FN(bro_tile_world_TileWorld_setFlag),
             "void", {"__bro_native.tile_world.TileWorld", "i32", "i32", "f64", "bool"})) return false;
    if (!reg("__bro_native.tile_world.TileWorld_hasFlag", BRO_NATIVE_FN(bro_tile_world_TileWorld_hasFlag),
             "bool", {"__bro_native.tile_world.TileWorld", "i32", "i32", "f64"})) return false;
    if (!reg("__bro_native.tile_world.TileWorld_setTint", BRO_NATIVE_FN(bro_tile_world_TileWorld_setTint),
             "void", {"__bro_native.tile_world.TileWorld", "i32", "i32", "f64", "f64", "f64", "f64"})) return false;
    if (!reg("__bro_native.tile_world.TileWorld_fillTint", BRO_NATIVE_FN(bro_tile_world_TileWorld_fillTint),
             "void", {"__bro_native.tile_world.TileWorld", "i32", "i32", "i32", "i32", "f64", "f64", "f64", "f64"})) return false;
    if (!reg("__bro_native.tile_world.TileWorld_getTint", BRO_NATIVE_FN(bro_tile_world_TileWorld_getTint),
             "str", {"__bro_native.tile_world.TileWorld", "i32", "i32"})) return false;
    if (!reg("__bro_native.tile_world.TileWorld_worldToCell", BRO_NATIVE_FN(bro_tile_world_TileWorld_worldToCell),
             "str", {"__bro_native.tile_world.TileWorld", "f64", "f64"})) return false;
    if (!reg("__bro_native.tile_world.TileWorld_cellCenterWorldXZ", BRO_NATIVE_FN(bro_tile_world_TileWorld_cellCenterWorldXZ),
             "str", {"__bro_native.tile_world.TileWorld", "i32", "i32"})) return false;
    if (!reg("__bro_native.tile_world.TileWorld_worldBounds", BRO_NATIVE_FN(bro_tile_world_TileWorld_worldBounds),
             "str", {"__bro_native.tile_world.TileWorld"})) return false;
    if (!reg("__bro_native.tile_world.TileWorld_sampleHeight", BRO_NATIVE_FN(bro_tile_world_TileWorld_sampleHeight),
             "f64", {"__bro_native.tile_world.TileWorld", "f64", "f64"})) return false;
    if (!reg("__bro_native.tile_world.TileWorld_raycastCell", BRO_NATIVE_FN(bro_tile_world_TileWorld_raycastCell),
             "str", {"__bro_native.tile_world.TileWorld", "f64", "f64", "f64", "f64", "f64", "f64", "f64"})) return false;
    if (!reg("__bro_native.tile_world.TileWorld_isWalkable", BRO_NATIVE_FN(bro_tile_world_TileWorld_isWalkable),
             "bool", {"__bro_native.tile_world.TileWorld", "i32", "i32", "f64"})) return false;
    if (!reg("__bro_native.tile_world.TileWorld_configure", BRO_NATIVE_FN(bro_tile_world_TileWorld_configure),
             "void", {"__bro_native.tile_world.TileWorld", "str"})) return false;
    if (!reg("__bro_native.tile_world.TileWorld_setShade", BRO_NATIVE_FN(bro_tile_world_TileWorld_setShade),
             "void", {"__bro_native.tile_world.TileWorld", "i32", "i32", "f64"})) return false;
    if (!reg("__bro_native.tile_world.TileWorld_fillShade", BRO_NATIVE_FN(bro_tile_world_TileWorld_fillShade),
             "void", {"__bro_native.tile_world.TileWorld", "i32", "i32", "i32", "i32", "f64"})) return false;
    if (!reg("__bro_native.tile_world.TileWorld_setShadeMapFloat", BRO_NATIVE_FN(bro_tile_world_TileWorld_setShadeMapFloat),
             "void", {"__bro_native.tile_world.TileWorld", "f32[]"})) return false;
    if (!reg("__bro_native.tile_world.TileWorld_setShadeMapBytes", BRO_NATIVE_FN(bro_tile_world_TileWorld_setShadeMapBytes),
             "void", {"__bro_native.tile_world.TileWorld", "u8[]"})) return false;
    if (!reg("__bro_native.tile_world.TileWorld_getShade", BRO_NATIVE_FN(bro_tile_world_TileWorld_getShade),
             "f64", {"__bro_native.tile_world.TileWorld", "i32", "i32"})) return false;

    bronze::embed::NativeSignature s;
    s.returnType = "__bro_native.tile_world.TileWorld";
    s.paramTypes = {"__bro_native.scene.SceneGraph", "bool", "i32", "bool", "f64", "str", "bool", "str",
                    "bool", "i32", "bool", "i32"};
    if (!bronze::embed::registerNative(
        "__bro_native.tile_world.createTileWorld",
        BRO_NATIVE_FN(bro_scene_SceneGraph_createTileWorld),
        s, error)) return false;
    return registerTileWorldOpsNatives(error) && registerTileWorldObjectNatives(error);
}

}  // namespace bro::bronze_host

#else  // !(BRO_WITH_3D)

#include <string>

namespace bro::bronze_host {

// Compiled out: nothing is registered and tile_world.js is not to be installed.
bool registerNatives_tile_world(std::string*) { return true; }

}  // namespace bro::bronze_host

#endif  // BRO_WITH_3D
