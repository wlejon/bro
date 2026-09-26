// native_scene_tile_objects.cpp — TileWorld object kinds and placements: add,
// remove and replace, for kinds (the instanced mesh) and single placements.

#include "bronze_host/native_scene_internal.h"
#include "bronze_host/host_natives.h"
#include "scene/tile_world.h"
#include "util/asset_path.h"
#include "broimage/decode.h"
#include <bromesh/mesh_data.h>
#include <cmath>
#include <string>

namespace bro::bronze_host {

namespace {

namespace ev = bronze::embed;
using Value = bronze::Value;

scene::TileWorld* worldOf(void* self) {
    auto* c = tileWorldCellOf(self);
    return (c && c->tileWorld()) ? c->tileWorld() : nullptr;
}

const bromesh::MeshData* meshOf(uint64_t meshVal) {
    if (!meshVal) return nullptr;
    return static_cast<const bromesh::MeshData*>(bronze::embed::handleData(bronze::Value{meshVal}));
}

scene::TileWorld::ObjectStyle parseObjectStyle(const char* styleJson) {
    scene::TileWorld::ObjectStyle style;
    if (!styleJson || !*styleJson) return style;
    auto res = ev::parseJson(styleJson);
    if (res.thrown || !ev::isObject(res.value)) return style;
    const Rooted s(res.value);
    const Rooted col(ev::getProperty(s, "color"));
    if (ev::isObject(col)) {
        for (int i = 0; i < 4; ++i) {
            Value el = ev::getElement(col, i);
            if (ev::isNumber(el)) style.color[i] = static_cast<float>(ev::toDouble(el));
        }
    }
    auto num = [&](const char* key, float& out) {
        Value v = ev::getProperty(s, key);
        if (ev::isNumber(v) && std::isfinite(ev::toDouble(v))) out = static_cast<float>(ev::toDouble(v));
    };
    auto flag = [&](const char* key, bool& out) {
        Value v = ev::getProperty(s, key);
        if (ev::isBool(v)) out = ev::toBool(v);
        else if (ev::isNumber(v)) out = ev::toDouble(v) != 0.0;
    };
    auto count = [&](const char* key, int& out) {
        Value v = ev::getProperty(s, key);
        if (ev::isNumber(v) && ev::toDouble(v) >= 1.0) out = satCast<int>(ev::toDouble(v));
    };
    num("roughness", style.roughness);
    num("metallic", style.metallic);
    num("alphaCutoff", style.alphaCutoff);
    flag("doubleSided", style.doubleSided);
    flag("castsShadow", style.castsShadow);
    count("atlasColumns", style.atlasCols);
    count("atlasRows", style.atlasRows);
    Value tex = ev::getProperty(s, "texture");
    if (ev::isString(tex)) {
        broimage::Image img;
        std::string err;
        if (broimage::decode_file(bro::util::resolveAssetPath(ev::toUtf8(tex)), img, &err) &&
            img.width > 0 && img.height > 0) {
            style.texWidth = img.width;
            style.texHeight = img.height;
            style.texPixels = std::move(img.pixels);
        }
    }
    return style;
}

scene::TileWorld::ObjectPlacement makePlacement(double yaw, double scale, double yOffset,
                                                double offsetX, double offsetZ, int32_t variant,
                                                const double* color, uint32_t color_len) {
    scene::TileWorld::ObjectPlacement p;
    // Per-instance tint: RGB, optional alpha.
    if (color && color_len >= 3) {
        for (uint32_t i = 0; i < color_len && i < 4; ++i) {
            if (std::isfinite(color[i])) p.color[i] = static_cast<float>(color[i]);
        }
    }
    p.yaw = static_cast<float>(yaw);
    p.scale = static_cast<float>(scale);
    p.yOffset = static_cast<float>(yOffset);
    p.offsetX = static_cast<float>(offsetX);
    p.offsetZ = static_cast<float>(offsetZ);
    p.variant = variant;
    return p;
}

} // namespace

extern "C" {

int32_t bro_tile_world_TileWorld_addObjectKind(void* self, uint64_t meshVal, const char* styleJson) {
    auto* w = worldOf(self);
    const auto* md = meshOf(meshVal);
    if (!w || !md) return -1;
    return w->addObjectKind(bromesh::MeshData(*md), parseObjectStyle(styleJson));
}

bool bro_tile_world_TileWorld_replaceObjectKind(void* self, int32_t kind, uint64_t meshVal,
                                                bool hasStyle, const char* styleJson) {
    auto* w = worldOf(self);
    const auto* md = meshOf(meshVal);
    if (!w || !md) return false;
    if (!hasStyle) return w->replaceObjectKind(kind, bromesh::MeshData(*md), nullptr);
    const auto style = parseObjectStyle(styleJson);
    return w->replaceObjectKind(kind, bromesh::MeshData(*md), &style);
}

bool bro_tile_world_TileWorld_removeObjectKind(void* self, int32_t kind) {
    auto* w = worldOf(self);
    return w ? w->removeObjectKind(kind) : false;
}

bool bro_tile_world_TileWorld_hasObjectKind(void* self, int32_t kind) {
    auto* w = worldOf(self);
    return w ? w->hasObjectKind(kind) : false;
}

int32_t bro_tile_world_TileWorld_addObjectPlacement(void* self, int32_t kind, int32_t x, int32_t y, double yaw, double scale, double yOffset, double offsetX, double offsetZ, int32_t variant, const double* color, uint32_t color_len) {
    auto* w = worldOf(self);
    if (!w) return -1;
    return w->addObject(kind, x, y, makePlacement(yaw, scale, yOffset, offsetX, offsetZ, variant, color, color_len));
}

bool bro_tile_world_TileWorld_replaceObjectPlacement(void* self, int32_t kind, int32_t index, int32_t x, int32_t y, double yaw, double scale, double yOffset, double offsetX, double offsetZ, int32_t variant, const double* color, uint32_t color_len) {
    auto* w = worldOf(self);
    if (!w) return false;
    return w->replaceObject(kind, index, x, y, makePlacement(yaw, scale, yOffset, offsetX, offsetZ, variant, color, color_len));
}

bool bro_tile_world_TileWorld_removeObject(void* self, int32_t kind, int32_t index) {
    auto* w = worldOf(self);
    return w ? w->removeObject(kind, index) : false;
}

} // extern "C"

bool registerTileWorldObjectNatives(std::string* error) {
    using namespace natives;
    const char* TW = "__bro_native.tile_world.TileWorld";
    return fn("__bro_native.tile_world.TileWorld_addObjectKind", (void*)&bro_tile_world_TileWorld_addObjectKind, "i32", {TW, "dynamic", "str"}, error) &&
           fn("__bro_native.tile_world.TileWorld_replaceObjectKind", (void*)&bro_tile_world_TileWorld_replaceObjectKind, "bool", {TW, "i32", "dynamic", "bool", "str"}, error) &&
           fn("__bro_native.tile_world.TileWorld_removeObjectKind", (void*)&bro_tile_world_TileWorld_removeObjectKind, "bool", {TW, "i32"}, error) &&
           fn("__bro_native.tile_world.TileWorld_hasObjectKind", (void*)&bro_tile_world_TileWorld_hasObjectKind, "bool", {TW, "i32"}, error) &&
           fn("__bro_native.tile_world.TileWorld_addObjectPlacement", (void*)&bro_tile_world_TileWorld_addObjectPlacement, "i32", {TW, "i32", "i32", "i32", "f64", "f64", "f64", "f64", "f64", "i32", "f64[]"}, error) &&
           fn("__bro_native.tile_world.TileWorld_replaceObjectPlacement", (void*)&bro_tile_world_TileWorld_replaceObjectPlacement, "bool", {TW, "i32", "i32", "i32", "i32", "f64", "f64", "f64", "f64", "f64", "i32", "f64[]"}, error) &&
           fn("__bro_native.tile_world.TileWorld_removeObject", (void*)&bro_tile_world_TileWorld_removeObject, "bool", {TW, "i32", "i32"}, error);
}

} // namespace bro::bronze_host
