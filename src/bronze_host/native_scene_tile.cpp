// native_scene_tile.cpp — TileWorld binding and C entry points.

#include "bronze_host/native_scene_internal.h"
#include "bronze_host/host_natives.h"
#include "natives/tile_world/native_tile_world_decl.h"
#include "scene/tile_world.h"
#include "scene/scene_node.h"
#include "tile/serialize.h"
#include "tile/pathfind.h"
#include "tile/region.h"
#include "util/asset_path.h"
#include "broimage/decode.h"
#include <bromesh/mesh_data.h>
#include <json.hpp>
#include <sstream>
#include <string>
#include <vector>

extern "C" void bro_tile_world_TileWorld_findPathJson(void* self, int32_t startX, int32_t startY,
                                                      int32_t endX, int32_t endY, const char* jsonOpts);

namespace bro::bronze_host {

bool registerNatives_tile_world(std::string* error);
bool registerTileWorldOpsNatives(std::string* error);

namespace {

namespace ev = bronze::embed;
using Value = bronze::Value;

struct PickTileSlot {
    bool hit = false;
    int32_t tileX = 0;
    int32_t tileY = 0;
    int32_t layer = 0;
    int32_t tileId = 0;
    double worldPos[3] = {0, 0, 0};
};
static thread_local PickTileSlot tl_pickTile;

struct PathResultSlot {
    bool reachable = false;
    std::string pathJson = "[]";
    double totalCost = 0.0;
};
static thread_local PathResultSlot tl_pathResult;

struct RegionSlot {
    int32_t regionId = 0;
    std::string tilesJson = "[]";
    int32_t area = 0;
};
static thread_local std::vector<RegionSlot> tl_regions;

static thread_local std::vector<uint8_t> tl_saveBuf;
static thread_local std::string tl_tileJson;

const char* tileJson(const std::string& s) {
    tl_tileJson = s;
    return tl_tileJson.c_str();
}

static void readFloatArray(Value vIn, std::vector<float>& out) {
    if (ev::isObject(vIn)) {
        const Rooted v(vIn);
        Value lenVal = ev::getProperty(v, "length");
        uint32_t len = 0;
        if (ev::isNumber(lenVal) && lengthWithin(ev::toDouble(lenVal), kMaxHostListLength, len)) {
            out.resize(len);
            for (uint32_t i = 0; i < len; ++i) {
                Value el = ev::getElement(v, i);
                out[i] = ev::isNumber(el) ? static_cast<float>(ev::toDouble(el)) : 0.0f;
            }
        }
    }
}

static scene::TileWorldConfig parseTileConfig(Value optsIn) {
    scene::TileWorldConfig cfg;
    if (!ev::isObject(optsIn)) return cfg;
    const Rooted opts(optsIn);

    Value wVal = ev::getProperty(opts, "width");
    if (ev::isNumber(wVal)) cfg.width = satCast<int>(ev::toDouble(wVal));

    Value hVal = ev::getProperty(opts, "height");
    if (ev::isNumber(hVal)) cfg.height = satCast<int>(ev::toDouble(hVal));

    Value csVal = ev::getProperty(opts, "cellSize");
    if (ev::isNumber(csVal)) cfg.cellSize = static_cast<float>(ev::toDouble(csVal));

    Value hsVal = ev::getProperty(opts, "heightStep");
    if (ev::isNumber(hsVal)) cfg.heightStep = static_cast<float>(ev::toDouble(hsVal));

    Value chkVal = ev::getProperty(opts, "chunkSize");
    if (ev::isNumber(chkVal)) cfg.chunkSize = satCast<int>(ev::toDouble(chkVal));

    Value blVal = ev::getProperty(opts, "baseLevel");
    if (ev::isNumber(blVal)) cfg.baseLevel = satCast<int>(ev::toDouble(blVal));

    Value aoVal = ev::getProperty(opts, "aoStrength");
    if (ev::isNumber(aoVal)) cfg.aoStrength = static_cast<float>(ev::toDouble(aoVal));

    Value topo = ev::getProperty(opts, "topology");
    if (ev::isString(topo)) {
        std::string s = ev::toUtf8(topo);
        if (s == "hex") cfg.topology = tile::Topology::Hex;
    }

    const Rooted layers(ev::getProperty(opts, "layers"));
    if (ev::isObject(layers)) {
        Value lenVal = ev::getProperty(layers, "length");
        uint32_t len = 0;
        if (ev::isNumber(lenVal) && lengthWithin(ev::toDouble(lenVal), kMaxHostListLength, len)) {
            std::vector<std::string> names;
            for (uint32_t i = 0; i < len; ++i) {
                Value el = ev::getElement(layers, i);
                if (ev::isString(el)) names.emplace_back(ev::toUtf8(el));
            }
            if (!names.empty()) cfg.layers = std::move(names);
        }
    }

    const Rooted orig(ev::getProperty(opts, "origin"));
    if (ev::isObject(orig)) {
        Value ox = ev::getElement(orig, 0);
        Value oy = ev::getElement(orig, 1);
        Value oz = ev::getElement(orig, 2);
        double vx = ev::isNumber(ox) ? ev::toDouble(ox) : 0;
        double vy = ev::isNumber(oy) ? ev::toDouble(oy) : 0;
        double vz = ev::isNumber(oz) ? ev::toDouble(oz) : 0;
        cfg.origin = {static_cast<float>(vx), static_cast<float>(vy), static_cast<float>(vz)};
    }

    Value pal = ev::getProperty(opts, "palette");
    readFloatArray(pal, cfg.palette);

    Value acVal = ev::getProperty(opts, "atlasColumns");
    if (ev::isNumber(acVal)) cfg.atlasColumns = satCast<int>(ev::toDouble(acVal));

    Value arVal = ev::getProperty(opts, "atlasRows");
    if (ev::isNumber(arVal)) cfg.atlasRows = satCast<int>(ev::toDouble(arVal));

    Value ccVal = ev::getProperty(opts, "cliffCell");
    if (ev::isNumber(ccVal)) cfg.cliffCell = satCast<int>(ev::toDouble(ccVal));

    Value aiVal = ev::getProperty(opts, "atlasInset");
    if (ev::isNumber(aiVal)) cfg.atlasInset = static_cast<float>(ev::toDouble(aiVal));

    Value atlas = ev::getProperty(opts, "atlas");
    if (ev::isString(atlas)) {
        std::string s = ev::toUtf8(atlas);
        broimage::Image img;
        std::string err;
        if (broimage::decode_file(util::resolveAssetPath(s), img, &err) &&
            img.width > 0 && img.height > 0) {
            cfg.atlasWidth = img.width;
            cfg.atlasHeight = img.height;
            cfg.atlasPixels = std::move(img.pixels);
        }
    } else {
        const Rooted px(ev::getProperty(opts, "atlasPixels"));
        if (ev::isObject(px)) {
            Value lenVal = ev::getProperty(px, "length");
            uint32_t len = 0;
            if (ev::isNumber(lenVal) &&
                lengthWithin(ev::toDouble(lenVal), static_cast<uint32_t>(kMaxHostBufferBytes), len)) {
                cfg.atlasPixels.resize(len);
                for (uint32_t i = 0; i < len; ++i) {
                    Value el = ev::getElement(px, i);
                    cfg.atlasPixels[i] = ev::isNumber(el) ? satCast<uint8_t>(ev::toDouble(el)) : 0;
                }
                Value aw = ev::getProperty(opts, "atlasWidth");
                Value ah = ev::getProperty(opts, "atlasHeight");
                cfg.atlasWidth = ev::isNumber(aw) ? satCast<int>(ev::toDouble(aw)) : 0;
                cfg.atlasHeight = ev::isNumber(ah) ? satCast<int>(ev::toDouble(ah)) : 0;
            }
        }
    }

    const Rooted ta(ev::getProperty(opts, "tileAtlas"));
    if (ev::isObject(ta)) {
        Value lenVal = ev::getProperty(ta, "length");
        uint32_t len = 0;
        if (ev::isNumber(lenVal) && lengthWithin(ev::toDouble(lenVal), kMaxHostListLength, len)) {
            cfg.tileAtlas.resize(len);
            for (uint32_t i = 0; i < len; ++i) {
                Value el = ev::getElement(ta, i);
                cfg.tileAtlas[i] = ev::isNumber(el) ? satCast<int>(ev::toDouble(el)) : 0;
            }
        }
    }

    const Rooted autos(ev::getProperty(opts, "autotiles"));
    if (ev::isObject(autos)) {
        Value lenVal = ev::getProperty(autos, "length");
        uint32_t len = 0;
        if (ev::isNumber(lenVal) && lengthWithin(ev::toDouble(lenVal), kMaxHostListLength, len)) {
            for (uint32_t i = 0; i < len; ++i) {
                const Rooted e(ev::getElement(autos, i));
                if (!ev::isObject(e)) continue;
                scene::TileWorldConfig::AutotileRule rule;
                Value idVal = ev::getProperty(e, "id");
                if (ev::isNumber(idVal)) rule.id = satCast<uint16_t>(ev::toDouble(idVal));
                Value lVal = ev::getProperty(e, "layer");
                if (ev::isNumber(lVal)) rule.layer = satCast<int>(ev::toDouble(lVal));

                Value mode = ev::getProperty(e, "mode");
                if (ev::isString(mode)) {
                    std::string m = ev::toUtf8(mode);
                    if (m == "edge") rule.mode = scene::TileWorldConfig::AutotileMode::Edge;
                    else if (m == "wang") rule.mode = scene::TileWorldConfig::AutotileMode::Wang;
                    else rule.mode = scene::TileWorldConfig::AutotileMode::Blob47;
                }

                Value famv = ev::getProperty(e, "family");
                if (ev::isString(famv)) {
                    std::string s = ev::toUtf8(famv);
                    if (s == "nonEmpty") rule.family = scene::TileWorldConfig::AutotileFamily::NonEmpty;
                }

                const Rooted cells(ev::getProperty(e, "cells"));
                if (ev::isObject(cells)) {
                    Value cl = ev::getProperty(cells, "length");
                    uint32_t clen = 0;
                    if (ev::isNumber(cl) && lengthWithin(ev::toDouble(cl), kMaxHostListLength, clen)) {
                        rule.cells.resize(clen);
                        for (uint32_t k = 0; k < clen; ++k) {
                            Value el = ev::getElement(cells, k);
                            rule.cells[k] = ev::isNumber(el) ? satCast<int>(ev::toDouble(el)) : 0;
                        }
                    }
                }
                cfg.autotiles.push_back(std::move(rule));
            }
        }
    }

    const Rooted ovs(ev::getProperty(opts, "overlays"));
    if (ev::isObject(ovs)) {
        Value lenVal = ev::getProperty(ovs, "length");
        uint32_t len = 0;
        if (ev::isNumber(lenVal) && lengthWithin(ev::toDouble(lenVal), kMaxHostListLength, len)) {
            cfg.overlays.resize(len);
            for (uint32_t i = 0; i < len; ++i) {
                const Rooted e(ev::getElement(ovs, i));
                if (ev::isObject(e)) {
                    Value op = ev::getProperty(e, "opacity");
                    if (ev::isNumber(op)) cfg.overlays[i].opacity = static_cast<float>(ev::toDouble(op));
                    Value ac = ev::getProperty(e, "alphaCutoff");
                    if (ev::isNumber(ac)) cfg.overlays[i].alphaCutoff = static_cast<float>(ev::toDouble(ac));
                }
            }
        }
    }

    const Rooted anims(ev::getProperty(opts, "animations"));
    if (ev::isObject(anims)) {
        Value lenVal = ev::getProperty(anims, "length");
        uint32_t len = 0;
        if (ev::isNumber(lenVal) && lengthWithin(ev::toDouble(lenVal), kMaxHostListLength, len)) {
            for (uint32_t i = 0; i < len; ++i) {
                const Rooted e(ev::getElement(anims, i));
                if (!ev::isObject(e)) continue;
                scene::TileWorldConfig::TileAnimation an;
                Value idVal = ev::getProperty(e, "id");
                if (ev::isNumber(idVal)) an.id = satCast<uint16_t>(ev::toDouble(idVal));
                Value fpsVal = ev::getProperty(e, "fps");
                if (ev::isNumber(fpsVal)) an.fps = static_cast<float>(ev::toDouble(fpsVal));
                else an.fps = 4.0f;

                const Rooted fr(ev::getProperty(e, "frames"));
                if (ev::isObject(fr)) {
                    Value fl = ev::getProperty(fr, "length");
                    uint32_t flen = 0;
                    if (ev::isNumber(fl) && lengthWithin(ev::toDouble(fl), kMaxHostListLength, flen)) {
                        an.frames.resize(flen);
                        for (uint32_t k = 0; k < flen; ++k) {
                            Value el = ev::getElement(fr, k);
                            an.frames[k] = ev::isNumber(el) ? satCast<int>(ev::toDouble(el)) : 0;
                        }
                    }
                }
                cfg.animations.push_back(std::move(an));
            }
        }
    }

    return cfg;
}

static bool validateTileConfig(const scene::TileWorldConfig& cfg) {
    if (cfg.width < 1 || cfg.height < 1) return false;
    if (cfg.width > 16384 || cfg.height > 16384) return false;
    if (static_cast<int64_t>(cfg.width) * static_cast<int64_t>(cfg.height) > 16777216LL) return false;
    if (cfg.chunkSize < 1) return false;
    return true;
}

}  // namespace

extern "C" {
void* bro_scene_SceneGraph_createTileWorldJson(void* self, const char* configJson);
void bro_tile_world_TileWorld_fillTile(void* self, int32_t x0, int32_t y0, int32_t x1, int32_t y1, int32_t tileId, int32_t layer);
void bro_tile_world_TileWorld_setElevation(void* self, int32_t x, int32_t y, int32_t level);
int32_t bro_tile_world_TileWorld_getElevation(void* self, int32_t x, int32_t y);
void bro_tile_world_TileWorld_fillElevation(void* self, int32_t x0, int32_t y0, int32_t x1, int32_t y1, int32_t level);
void bro_tile_world_TileWorld_setFlag(void* self, int32_t x, int32_t y, double bit, bool on);
bool bro_tile_world_TileWorld_hasFlag(void* self, int32_t x, int32_t y, double bit);
void bro_tile_world_TileWorld_setTint(void* self, int32_t x, int32_t y, double r, double g, double b, double a);
void bro_tile_world_TileWorld_fillTint(void* self, int32_t x0, int32_t y0, int32_t x1, int32_t y1, double r, double g, double b, double a);
const char* bro_tile_world_TileWorld_getTint(void* self, int32_t x, int32_t y);
const char* bro_tile_world_TileWorld_worldToCell(void* self, double wx, double wz);
const char* bro_tile_world_TileWorld_cellCenterWorldXZ(void* self, int32_t cx, int32_t cy);
const char* bro_tile_world_TileWorld_worldBounds(void* self);
double bro_tile_world_TileWorld_sampleHeight(void* self, double wx, double wz);
const char* bro_tile_world_TileWorld_raycastCell(void* self, double ox, double oy, double oz, double dx, double dy, double dz, double maxDist);
bool bro_tile_world_TileWorld_isWalkable(void* self, int32_t x, int32_t y, double mask);
void bro_tile_world_TileWorld_configure(void* self, const char* configJson);
int32_t bro_tile_world_TileWorld_addObjectKind(void* self, uint64_t meshVal, const char* styleJson);
int32_t bro_tile_world_TileWorld_addObjectPlacement(void* self, int32_t kind, int32_t x, int32_t y, double yaw, double scale, double yOffset, double offsetX, double offsetZ, int32_t variant);
void bro_tile_world_TileWorld_setShade(void* self, int32_t x, int32_t y, double v);
void bro_tile_world_TileWorld_fillShade(void* self, int32_t x0, int32_t y0, int32_t x1, int32_t y1, double v);
void bro_tile_world_TileWorld_setShadeMapFloat(void* self, const float* data, uint32_t count);
void bro_tile_world_TileWorld_setShadeMapBytes(void* self, const uint8_t* data, uint32_t count);
double bro_tile_world_TileWorld_getShade(void* self, int32_t x, int32_t y);
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

    if (!reg("__bro_native.tile_world.createTileWorldJson", (void*)&bro_scene_SceneGraph_createTileWorldJson,
             "__bro_native.tile_world.TileWorld", {"__bro_native.scene.SceneGraph", "str"})) return false;
    if (!reg("__bro_native.tile_world.TileWorld_findPathJson", (void*)&bro_tile_world_TileWorld_findPathJson,
             "void", {"__bro_native.tile_world.TileWorld", "i32", "i32", "i32", "i32", "str"})) return false;
    if (!reg("__bro_native.tile_world.TileWorld_fillTile", (void*)&bro_tile_world_TileWorld_fillTile,
             "void", {"__bro_native.tile_world.TileWorld", "i32", "i32", "i32", "i32", "i32", "i32"})) return false;
    if (!reg("__bro_native.tile_world.TileWorld_setElevation", (void*)&bro_tile_world_TileWorld_setElevation,
             "void", {"__bro_native.tile_world.TileWorld", "i32", "i32", "i32"})) return false;
    if (!reg("__bro_native.tile_world.TileWorld_getElevation", (void*)&bro_tile_world_TileWorld_getElevation,
             "i32", {"__bro_native.tile_world.TileWorld", "i32", "i32"})) return false;
    if (!reg("__bro_native.tile_world.TileWorld_fillElevation", (void*)&bro_tile_world_TileWorld_fillElevation,
             "void", {"__bro_native.tile_world.TileWorld", "i32", "i32", "i32", "i32", "i32"})) return false;
    if (!reg("__bro_native.tile_world.TileWorld_setFlag", (void*)&bro_tile_world_TileWorld_setFlag,
             "void", {"__bro_native.tile_world.TileWorld", "i32", "i32", "f64", "bool"})) return false;
    if (!reg("__bro_native.tile_world.TileWorld_hasFlag", (void*)&bro_tile_world_TileWorld_hasFlag,
             "bool", {"__bro_native.tile_world.TileWorld", "i32", "i32", "f64"})) return false;
    if (!reg("__bro_native.tile_world.TileWorld_setTint", (void*)&bro_tile_world_TileWorld_setTint,
             "void", {"__bro_native.tile_world.TileWorld", "i32", "i32", "f64", "f64", "f64", "f64"})) return false;
    if (!reg("__bro_native.tile_world.TileWorld_fillTint", (void*)&bro_tile_world_TileWorld_fillTint,
             "void", {"__bro_native.tile_world.TileWorld", "i32", "i32", "i32", "i32", "f64", "f64", "f64", "f64"})) return false;
    if (!reg("__bro_native.tile_world.TileWorld_getTint", (void*)&bro_tile_world_TileWorld_getTint,
             "str", {"__bro_native.tile_world.TileWorld", "i32", "i32"})) return false;
    if (!reg("__bro_native.tile_world.TileWorld_worldToCell", (void*)&bro_tile_world_TileWorld_worldToCell,
             "str", {"__bro_native.tile_world.TileWorld", "f64", "f64"})) return false;
    if (!reg("__bro_native.tile_world.TileWorld_cellCenterWorldXZ", (void*)&bro_tile_world_TileWorld_cellCenterWorldXZ,
             "str", {"__bro_native.tile_world.TileWorld", "i32", "i32"})) return false;
    if (!reg("__bro_native.tile_world.TileWorld_worldBounds", (void*)&bro_tile_world_TileWorld_worldBounds,
             "str", {"__bro_native.tile_world.TileWorld"})) return false;
    if (!reg("__bro_native.tile_world.TileWorld_sampleHeight", (void*)&bro_tile_world_TileWorld_sampleHeight,
             "f64", {"__bro_native.tile_world.TileWorld", "f64", "f64"})) return false;
    if (!reg("__bro_native.tile_world.TileWorld_raycastCell", (void*)&bro_tile_world_TileWorld_raycastCell,
             "str", {"__bro_native.tile_world.TileWorld", "f64", "f64", "f64", "f64", "f64", "f64", "f64"})) return false;
    if (!reg("__bro_native.tile_world.TileWorld_isWalkable", (void*)&bro_tile_world_TileWorld_isWalkable,
             "bool", {"__bro_native.tile_world.TileWorld", "i32", "i32", "f64"})) return false;
    if (!reg("__bro_native.tile_world.TileWorld_configure", (void*)&bro_tile_world_TileWorld_configure,
             "void", {"__bro_native.tile_world.TileWorld", "str"})) return false;
    if (!reg("__bro_native.tile_world.TileWorld_addObjectKind", (void*)&bro_tile_world_TileWorld_addObjectKind,
             "i32", {"__bro_native.tile_world.TileWorld", "dynamic", "str"})) return false;
    if (!reg("__bro_native.tile_world.TileWorld_addObjectPlacement", (void*)&bro_tile_world_TileWorld_addObjectPlacement,
             "i32", {"__bro_native.tile_world.TileWorld", "i32", "i32", "i32", "f64", "f64", "f64", "f64", "f64", "i32"})) return false;
    if (!reg("__bro_native.tile_world.TileWorld_setShade", (void*)&bro_tile_world_TileWorld_setShade,
             "void", {"__bro_native.tile_world.TileWorld", "i32", "i32", "f64"})) return false;
    if (!reg("__bro_native.tile_world.TileWorld_fillShade", (void*)&bro_tile_world_TileWorld_fillShade,
             "void", {"__bro_native.tile_world.TileWorld", "i32", "i32", "i32", "i32", "f64"})) return false;
    if (!reg("__bro_native.tile_world.TileWorld_setShadeMapFloat", (void*)&bro_tile_world_TileWorld_setShadeMapFloat,
             "void", {"__bro_native.tile_world.TileWorld", "f32[]"})) return false;
    if (!reg("__bro_native.tile_world.TileWorld_setShadeMapBytes", (void*)&bro_tile_world_TileWorld_setShadeMapBytes,
             "void", {"__bro_native.tile_world.TileWorld", "u8[]"})) return false;
    if (!reg("__bro_native.tile_world.TileWorld_getShade", (void*)&bro_tile_world_TileWorld_getShade,
             "f64", {"__bro_native.tile_world.TileWorld", "i32", "i32"})) return false;

    bronze::embed::NativeSignature s;
    s.returnType = "__bro_native.tile_world.TileWorld";
    s.paramTypes = {"__bro_native.scene.SceneGraph", "bool", "i32", "bool", "f64", "str", "bool", "str",
                    "bool", "i32", "bool", "i32"};
    if (!bronze::embed::registerNative(
        "__bro_native.tile_world.createTileWorld",
        reinterpret_cast<void*>(&bro_scene_SceneGraph_createTileWorld),
        s, error)) return false;
    return registerTileWorldOpsNatives(error);
}

}  // namespace bro::bronze_host

extern "C" {

using namespace bro::bronze_host;

void bro_tile_world_TileWorld_dtor(void* self) {
    auto* c = tileWorldCellOf(self);
    if (c) {
        if (c->world) {
            c->world->clear();
            c->world.reset();
        }
        delete c;
    }
}

void* bro_tile_world_TileWorld_ctor(void) {
    return new HostTileWorldCell();
}

void* bro_tile_world_TileWorld_node_get(void* self) {
    auto* c = tileWorldCellOf(self);
    if (!c || !c->tileWorld()) return nullptr;
    auto* node = c->tileWorld()->rootNode();
    return node ? wrapNode(node, c->graph()) : nullptr;
}

int32_t bro_tile_world_TileWorld_width_get(void* self) {
    auto* c = tileWorldCellOf(self);
    return (c && c->tileWorld()) ? c->tileWorld()->width() : 0;
}

int32_t bro_tile_world_TileWorld_height_get(void* self) {
    auto* c = tileWorldCellOf(self);
    return (c && c->tileWorld()) ? c->tileWorld()->height() : 0;
}

int32_t bro_tile_world_TileWorld_chunkCount_get(void* self) {
    auto* c = tileWorldCellOf(self);
    return (c && c->tileWorld()) ? c->tileWorld()->chunkCount() : 0;
}

int32_t bro_tile_world_TileWorld_chunks_get(void* self) {
    return bro_tile_world_TileWorld_chunkCount_get(self);
}

int32_t bro_tile_world_TileWorld_vertexCount_get(void* self) {
    auto* c = tileWorldCellOf(self);
    return (c && c->tileWorld()) ? c->tileWorld()->totalVertices() : 0;
}

int32_t bro_tile_world_TileWorld_triangleCount_get(void* self) {
    auto* c = tileWorldCellOf(self);
    return (c && c->tileWorld()) ? c->tileWorld()->totalTriangles() : 0;
}

void bro_tile_world_TileWorld_setTile(void* self, int32_t layer, int32_t x, int32_t y, int32_t tileId) {
    auto* c = tileWorldCellOf(self);
    if (c && c->tileWorld()) {
        c->tileWorld()->setTile(x, y, static_cast<uint16_t>(tileId), layer);
    }
}

int32_t bro_tile_world_TileWorld_getTile(void* self, int32_t layer, int32_t x, int32_t y) {
    auto* c = tileWorldCellOf(self);
    return (c && c->tileWorld()) ? c->tileWorld()->tile(x, y, layer) : 0;
}

void bro_tile_world_TileWorld_fillRect(void* self, int32_t layer, int32_t x, int32_t y, int32_t w, int32_t h, int32_t tileId) {
    auto* c = tileWorldCellOf(self);
    if (c && c->tileWorld() && w > 0 && h > 0) {
        // The far corner in 64 bits, saturated: x + w - 1 overflows int for
        // a script's large x and w. fillTile clips to the grid either way.
        const auto farEdge = [](int32_t o, int32_t n) {
            return static_cast<int32_t>(std::min<int64_t>(int64_t{o} + n - 1, INT32_MAX));
        };
        c->tileWorld()->fillTile(x, y, farEdge(x, w), farEdge(y, h), static_cast<uint16_t>(tileId), layer);
    }
}

void bro_tile_world_TileWorld_clearLayer(void* self, int32_t layer) {
    auto* c = tileWorldCellOf(self);
    if (c && c->tileWorld()) {
        int w = c->tileWorld()->width();
        int h = c->tileWorld()->height();
        c->tileWorld()->fillTile(0, 0, w - 1, h - 1, 0, layer);
    }
}

bool bro_tile_world_TileWorld_pickTile(void* self, double worldX, double worldZ) {
    auto* c = tileWorldCellOf(self);
    if (!c || !c->tileWorld()) {
        tl_pickTile.hit = false;
        return false;
    }
    int outX = 0, outY = 0;
    if (!c->tileWorld()->worldToCell(static_cast<float>(worldX), static_cast<float>(worldZ), outX, outY)) {
        tl_pickTile.hit = false;
        return false;
    }
    float outYPos = 0.0f;
    c->tileWorld()->sampleHeight(static_cast<float>(worldX), static_cast<float>(worldZ), outYPos);

    tl_pickTile.hit = true;
    tl_pickTile.tileX = outX;
    tl_pickTile.tileY = outY;
    tl_pickTile.layer = 0;
    tl_pickTile.tileId = c->tileWorld()->tile(outX, outY, 0);
    tl_pickTile.worldPos[0] = worldX;
    tl_pickTile.worldPos[1] = outYPos;
    tl_pickTile.worldPos[2] = worldZ;
    return true;
}

int32_t bro_tile_world_TileWorld_pickTile_tileX(void) {
    return tl_pickTile.tileX;
}

int32_t bro_tile_world_TileWorld_pickTile_tileY(void) {
    return tl_pickTile.tileY;
}

int32_t bro_tile_world_TileWorld_pickTile_layer(void) {
    return tl_pickTile.layer;
}

int32_t bro_tile_world_TileWorld_pickTile_tileId(void) {
    return tl_pickTile.tileId;
}

void bro_tile_world_TileWorld_pickTile_worldPosition(bronze_native_buffer* out) {
    copyBuffer(tl_pickTile.worldPos, 3, out);
}

void bro_tile_world_TileWorld_findPathJson(void* self, int32_t startX, int32_t startY,
                                          int32_t endX, int32_t endY, const char* jsonOpts) {
    auto* c = tileWorldCellOf(self);
    if (!c || !c->tileWorld() || !c->tileWorld()->grid()) {
        tl_pathResult.reachable = false;
        tl_pathResult.pathJson = "[]";
        tl_pathResult.totalCost = 0.0;
        return;
    }
    const auto& grid = *c->tileWorld()->grid();
    bro::tile::Cell start{startX, startY};
    bro::tile::Cell goal{endX, endY};
    uint32_t blockMask = 0;
    bool allowDiag = false;
    std::vector<float> costs;
    if (jsonOpts && jsonOpts[0]) {
        try {
            auto j = nlohmann::json::parse(jsonOpts);
            if (j.contains("blockMask") && j["blockMask"].is_number()) blockMask = j["blockMask"].get<uint32_t>();
            if (j.contains("allowDiagonal") && j["allowDiagonal"].is_boolean()) allowDiag = j["allowDiagonal"].get<bool>();
            if (j.contains("costs") && j["costs"].is_array()) {
                for (const auto& item : j["costs"]) {
                    costs.push_back(item.is_number() ? item.get<float>() : 1.0f);
                }
            }
        } catch (...) {}
    }
    bro::tile::Conn conn = allowDiag ? bro::tile::Conn::Vertex : bro::tile::Conn::Edge;

    bro::tile::PassFn pass = [blockMask](const bro::tile::TileGrid& g, bro::tile::Cell cell) {
        if (g.tile(0, cell) == 0) return false;
        if (blockMask != 0 && (g.flags(cell) & blockMask) != 0) return false;
        return true;
    };

    bro::tile::CostFn costFn = nullptr;
    if (!costs.empty()) {
        costFn = [costs](const bro::tile::TileGrid& g, bro::tile::Cell /*from*/, bro::tile::Cell to) -> float {
            uint32_t tid = g.tile(0, to);
            if (tid < costs.size()) return std::max(1.0f, costs[tid]);
            return 1.0f;
        };
    }

    auto path = bro::tile::aStar(grid, start, goal, pass, costFn, conn);
    if (path.empty()) {
        tl_pathResult.reachable = false;
        tl_pathResult.pathJson = "[]";
        tl_pathResult.totalCost = 0.0;
    } else {
        tl_pathResult.reachable = true;
        tl_pathResult.totalCost = static_cast<double>(path.size() - 1);
        std::ostringstream ss;
        ss << "[";
        for (size_t i = 0; i < path.size(); ++i) {
            if (i > 0) ss << ",";
            ss << "{\"x\":" << path[i].x << ",\"y\":" << path[i].y << "}";
        }
        ss << "]";
        tl_pathResult.pathJson = ss.str();
    }
}

void bro_tile_world_TileWorld_findPath(void* self, int32_t startX, int32_t startY,
                                      int32_t endX, int32_t endY,
                                      bool /*opts_agentRadius_given*/, int32_t /*opts_agentRadius*/,
                                      bool opts_allowDiagonal_given, bool opts_allowDiagonal,
                                      bool /*opts_maxSlope_given*/, int32_t /*opts_maxSlope*/) {
    std::string opts = (opts_allowDiagonal_given && opts_allowDiagonal) ? "{\"allowDiagonal\":true}" : "{}";
    bro_tile_world_TileWorld_findPathJson(self, startX, startY, endX, endY, opts.c_str());
}

bool bro_tile_world_TileWorld_findPath_reachable(void) {
    return tl_pathResult.reachable;
}

const char* bro_tile_world_TileWorld_findPath_path(void) {
    return tl_pathResult.pathJson.c_str();
}

double bro_tile_world_TileWorld_findPath_totalCost(void) {
    return tl_pathResult.totalCost;
}

int32_t bro_tile_world_TileWorld_computeRegions(void* self, int32_t layer) {
    tl_regions.clear();
    auto* c = tileWorldCellOf(self);
    if (!c || !c->tileWorld() || !c->tileWorld()->grid()) return 0;
    const auto& grid = *c->tileWorld()->grid();

    bro::tile::MatchFn match = [layer](const bro::tile::TileGrid& g, bro::tile::Cell cell) {
        return g.tile(layer, cell) != 0;
    };

    auto comps = bro::tile::components(grid, match, bro::tile::Conn::Edge);
    int32_t id = 0;
    for (const auto& comp : comps) {
        RegionSlot slot;
        slot.regionId = id++;
        slot.area = static_cast<int32_t>(comp.size());
        std::ostringstream ss;
        ss << "[";
        for (size_t i = 0; i < comp.size(); ++i) {
            if (i > 0) ss << ",";
            ss << "[" << comp[i].x << "," << comp[i].y << "]";
        }
        ss << "]";
        slot.tilesJson = ss.str();
        tl_regions.push_back(std::move(slot));
    }
    return static_cast<int32_t>(tl_regions.size());
}

int32_t bro_tile_world_TileWorld_computeRegions_regionId(int32_t index) {
    if (index >= 0 && static_cast<size_t>(index) < tl_regions.size()) {
        return tl_regions[index].regionId;
    }
    return 0;
}

const char* bro_tile_world_TileWorld_computeRegions_tiles(int32_t index) {
    if (index >= 0 && static_cast<size_t>(index) < tl_regions.size()) {
        return tl_regions[index].tilesJson.c_str();
    }
    return "[]";
}

int32_t bro_tile_world_TileWorld_computeRegions_area(int32_t index) {
    if (index >= 0 && static_cast<size_t>(index) < tl_regions.size()) {
        return tl_regions[index].area;
    }
    return 0;
}

void bro_tile_world_TileWorld_setOrigin(void* self, double x, double y, double z) {
    auto* c = tileWorldCellOf(self);
    if (c && c->tileWorld()) {
        c->tileWorld()->setOrigin(static_cast<float>(x), static_cast<float>(y), static_cast<float>(z));
    }
}

bool bro_tile_world_TileWorld_advance(void* self, double dtMs) {
    auto* c = tileWorldCellOf(self);
    return (c && c->tileWorld()) ? c->tileWorld()->advance(dtMs) : false;
}

void bro_tile_world_TileWorld_addObject(void* self, int32_t kindId, double x, double y, double /*z*/) {
    auto* c = tileWorldCellOf(self);
    if (c && c->tileWorld()) {
        scene::TileWorld::ObjectPlacement p;
        c->tileWorld()->addObject(kindId, satCast<int>(x), satCast<int>(y), p);
    }
}

void bro_tile_world_TileWorld_clearObjects(void* self, bool kindId_given, int32_t kindId) {
    auto* c = tileWorldCellOf(self);
    if (c && c->tileWorld()) {
        c->tileWorld()->clearObjects(kindId_given ? kindId : -1);
    }
}

int32_t bro_tile_world_TileWorld_objectCount(void* self, int32_t kind) {
    auto* c = tileWorldCellOf(self);
    return (c && c->tileWorld()) ? c->tileWorld()->objectCount(kind) : 0;
}

void bro_tile_world_TileWorld_rebuildObjects(void* self) {
    auto* c = tileWorldCellOf(self);
    if (c && c->tileWorld()) c->tileWorld()->rebuildObjects();
}

void bro_tile_world_TileWorld_rebuild(void* self) {
    auto* c = tileWorldCellOf(self);
    if (c && c->tileWorld()) c->tileWorld()->rebuildDirty();
}

void bro_tile_world_TileWorld_rebuildAll(void* self) {
    auto* c = tileWorldCellOf(self);
    if (c && c->tileWorld()) c->tileWorld()->rebuildAll();
}

void bro_tile_world_TileWorld_save(void* self, bronze_native_buffer* out) {
    auto* c = tileWorldCellOf(self);
    if (!c || !c->tileWorld() || !c->tileWorld()->grid()) {
        copyBuffer<uint8_t>(nullptr, 0, out);
        return;
    }
    tl_saveBuf = bro::tile::serialize(*c->tileWorld()->grid());
    copyBuffer(tl_saveBuf.data(), static_cast<uint32_t>(tl_saveBuf.size()), out);
}

bool bro_tile_world_TileWorld_load(void* self, const uint8_t* data, uint32_t data_len) {
    auto* c = tileWorldCellOf(self);
    if (!c || !c->tileWorld() || !data || data_len == 0) return false;
    std::vector<uint8_t> bytes(data, data + data_len);
    auto grid = bro::tile::deserialize(bytes);
    if (!grid) return false;
    c->tileWorld()->loadGrid(std::move(*grid));
    return true;
}

void bro_tile_world_TileWorld_destroy(void* self) {
    auto* c = tileWorldCellOf(self);
    if (c && c->world) {
        c->world->clear();
        c->world.reset();
    }
}

void* bro_scene_SceneGraph_createTileWorldJson(void* self, const char* configJson) {
    auto* g = graphOf(self);
    if (!g) return nullptr;
    scene::TileWorldConfig cfg;
    if (configJson && *configJson) {
        auto res = ev::parseJson(configJson);
        if (!res.thrown && ev::isObject(res.value)) {
            cfg = parseTileConfig(res.value);
        }
    }
    if (!validateTileConfig(cfg)) return nullptr;
    auto* cell = new HostTileWorldCell();
    cell->token = g->livenessToken();
    cell->world = std::make_unique<scene::TileWorld>(*g);
    cell->world->configure(std::move(cfg));
    return cell;
}

void bro_tile_world_TileWorld_fillTile(void* self, int32_t x0, int32_t y0, int32_t x1, int32_t y1, int32_t tileId, int32_t layer) {
    auto* c = tileWorldCellOf(self);
    if (c && c->tileWorld()) {
        c->tileWorld()->fillTile(x0, y0, x1, y1, static_cast<uint16_t>(tileId), layer);
    }
}

void bro_tile_world_TileWorld_setElevation(void* self, int32_t x, int32_t y, int32_t level) {
    auto* c = tileWorldCellOf(self);
    if (c && c->tileWorld()) c->tileWorld()->setElevation(x, y, level);
}

int32_t bro_tile_world_TileWorld_getElevation(void* self, int32_t x, int32_t y) {
    auto* c = tileWorldCellOf(self);
    return (c && c->tileWorld()) ? c->tileWorld()->elevation(x, y) : 0;
}

void bro_tile_world_TileWorld_fillElevation(void* self, int32_t x0, int32_t y0, int32_t x1, int32_t y1, int32_t level) {
    auto* c = tileWorldCellOf(self);
    if (c && c->tileWorld()) c->tileWorld()->fillElevation(x0, y0, x1, y1, level);
}

void bro_tile_world_TileWorld_setFlag(void* self, int32_t x, int32_t y, double bit, bool on) {
    auto* c = tileWorldCellOf(self);
    if (c && c->tileWorld()) c->tileWorld()->setFlag(x, y, static_cast<uint32_t>(bit), on);
}

bool bro_tile_world_TileWorld_hasFlag(void* self, int32_t x, int32_t y, double bit) {
    auto* c = tileWorldCellOf(self);
    return (c && c->tileWorld()) ? c->tileWorld()->hasFlag(x, y, static_cast<uint32_t>(bit)) : false;
}

void bro_tile_world_TileWorld_setTint(void* self, int32_t x, int32_t y, double r, double g, double b, double a) {
    auto* c = tileWorldCellOf(self);
    if (c && c->tileWorld()) c->tileWorld()->setTint(x, y, (float)r, (float)g, (float)b, (float)a);
}

void bro_tile_world_TileWorld_fillTint(void* self, int32_t x0, int32_t y0, int32_t x1, int32_t y1, double r, double g, double b, double a) {
    auto* c = tileWorldCellOf(self);
    if (c && c->tileWorld()) c->tileWorld()->fillTint(x0, y0, x1, y1, (float)r, (float)g, (float)b, (float)a);
}

const char* bro_tile_world_TileWorld_getTint(void* self, int32_t x, int32_t y) {
    auto* c = tileWorldCellOf(self);
    uint32_t packed = 0xFFFFFFFFu;
    if (c && c->tileWorld()) packed = c->tileWorld()->tintAt(x, y);
    double r = ((packed >> 24) & 0xFF) / 255.0;
    double g = ((packed >> 16) & 0xFF) / 255.0;
    double b = ((packed >>  8) & 0xFF) / 255.0;
    double a = ( packed        & 0xFF) / 255.0;
    return tileJson("{\"r\":" + std::to_string(r) + ",\"g\":" + std::to_string(g) + ",\"b\":" + std::to_string(b) + ",\"a\":" + std::to_string(a) + "}");
}

const char* bro_tile_world_TileWorld_worldToCell(void* self, double wx, double wz) {
    auto* c = tileWorldCellOf(self);
    if (!c || !c->tileWorld()) return nullptr;
    int cx = 0, cy = 0;
    if (!c->tileWorld()->worldToCell(static_cast<float>(wx), static_cast<float>(wz), cx, cy)) return nullptr;
    return tileJson("{\"x\":" + std::to_string(cx) + ",\"y\":" + std::to_string(cy) + "}");
}

const char* bro_tile_world_TileWorld_cellCenterWorldXZ(void* self, int32_t cx, int32_t cy) {
    auto* c = tileWorldCellOf(self);
    if (!c || !c->tileWorld()) return nullptr;
    float px = 0, pz = 0;
    c->tileWorld()->cellCenterWorldXZ(cx, cy, px, pz);
    return tileJson("{\"x\":" + std::to_string(px) + ",\"z\":" + std::to_string(pz) + "}");
}

const char* bro_tile_world_TileWorld_worldBounds(void* self) {
    auto* c = tileWorldCellOf(self);
    if (!c || !c->tileWorld()) return nullptr;
    float minX = 0, minZ = 0, maxX = 0, maxZ = 0;
    c->tileWorld()->worldBounds(minX, minZ, maxX, maxZ);
    return tileJson("{\"minX\":" + std::to_string(minX) + ",\"minZ\":" + std::to_string(minZ) +
                    ",\"maxX\":" + std::to_string(maxX) + ",\"maxZ\":" + std::to_string(maxZ) + "}");
}

double bro_tile_world_TileWorld_sampleHeight(void* self, double wx, double wz) {
    auto* c = tileWorldCellOf(self);
    if (!c || !c->tileWorld()) return -1e9;
    float y = 0;
    if (!c->tileWorld()->sampleHeight(static_cast<float>(wx), static_cast<float>(wz), y)) return -1e9;
    return static_cast<double>(y);
}

const char* bro_tile_world_TileWorld_raycastCell(void* self, double ox, double oy, double oz, double dx, double dy, double dz, double maxDist) {
    auto* c = tileWorldCellOf(self);
    if (!c || !c->tileWorld()) return nullptr;
    bromath::Vec3 o{static_cast<float>(ox), static_cast<float>(oy), static_cast<float>(oz)};
    bromath::Vec3 d{static_cast<float>(dx), static_cast<float>(dy), static_cast<float>(dz)};
    auto hit = c->tileWorld()->raycastCell(o, d, static_cast<float>(maxDist));
    if (!hit.hit) return nullptr;
    return tileJson("{\"x\":" + std::to_string(hit.x) +
                    ",\"y\":" + std::to_string(hit.y) +
                    ",\"distance\":" + std::to_string(hit.distance) +
                    ",\"side\":" + (hit.side ? "true" : "false") +
                    ",\"cell\":[" + std::to_string(hit.x) + "," + std::to_string(hit.y) + "]" +
                    ",\"point\":[" + std::to_string(hit.point[0]) + "," + std::to_string(hit.point[1]) + "," + std::to_string(hit.point[2]) + "]}");
}

bool bro_tile_world_TileWorld_isWalkable(void* self, int32_t x, int32_t y, double mask) {
    auto* c = tileWorldCellOf(self);
    return (c && c->tileWorld()) ? c->tileWorld()->isWalkable(x, y, static_cast<uint32_t>(mask)) : false;
}

void bro_tile_world_TileWorld_configure(void* self, const char* configJson) {
    auto* c = tileWorldCellOf(self);
    if (!c || !c->tileWorld() || !configJson || !*configJson) return;
    auto res = ev::parseJson(configJson);
    if (!res.thrown && ev::isObject(res.value)) {
        scene::TileWorldConfig cfg = parseTileConfig(res.value);
        if (validateTileConfig(cfg)) {
            c->tileWorld()->configure(std::move(cfg));
        }
    }
}

int32_t bro_tile_world_TileWorld_addObjectKind(void* self, uint64_t meshVal, const char* styleJson) {
    auto* c = tileWorldCellOf(self);
    if (!c || !c->tileWorld() || !meshVal) return -1;
    auto val = bronze::Value{meshVal};
    void* handle = bronze::embed::handleData(val);
    auto* md = static_cast<bromesh::MeshData*>(handle);
    if (!md) return -1;

    scene::TileWorld::ObjectStyle style;
    if (styleJson && *styleJson) {
        auto res = ev::parseJson(styleJson);
        if (!res.thrown && ev::isObject(res.value)) {
            const Rooted col(ev::getProperty(res.value, "color"));
            if (ev::isObject(col)) {
                for (int i = 0; i < 4; ++i) {
                    Value el = ev::getElement(col, i);
                    if (ev::isNumber(el)) style.color[i] = static_cast<float>(ev::toDouble(el));
                }
            }
        }
    }
    return c->tileWorld()->addObjectKind(bromesh::MeshData(*md), style);
}

int32_t bro_tile_world_TileWorld_addObjectPlacement(void* self, int32_t kind, int32_t x, int32_t y, double yaw, double scale, double yOffset, double offsetX, double offsetZ, int32_t variant) {
    auto* c = tileWorldCellOf(self);
    if (!c || !c->tileWorld()) return -1;
    scene::TileWorld::ObjectPlacement p;
    p.yaw = static_cast<float>(yaw);
    p.scale = static_cast<float>(scale);
    p.yOffset = static_cast<float>(yOffset);
    p.offsetX = static_cast<float>(offsetX);
    p.offsetZ = static_cast<float>(offsetZ);
    p.variant = variant;
    return c->tileWorld()->addObject(kind, x, y, p);
}

void bro_tile_world_TileWorld_setShade(void* self, int32_t x, int32_t y, double v) {
    auto* c = tileWorldCellOf(self);
    if (c && c->tileWorld()) c->tileWorld()->setShade(x, y, static_cast<float>(v));
}

void bro_tile_world_TileWorld_fillShade(void* self, int32_t x0, int32_t y0, int32_t x1, int32_t y1, double v) {
    auto* c = tileWorldCellOf(self);
    if (c && c->tileWorld()) c->tileWorld()->fillShade(x0, y0, x1, y1, static_cast<float>(v));
}

void bro_tile_world_TileWorld_setShadeMapFloat(void* self, const float* data, uint32_t count) {
    auto* c = tileWorldCellOf(self);
    if (c && c->tileWorld() && data) c->tileWorld()->setShadeMap(data, count);
}

void bro_tile_world_TileWorld_setShadeMapBytes(void* self, const uint8_t* data, uint32_t count) {
    auto* c = tileWorldCellOf(self);
    if (c && c->tileWorld() && data) c->tileWorld()->setShadeMap(data, count);
}

double bro_tile_world_TileWorld_getShade(void* self, int32_t x, int32_t y) {
    auto* c = tileWorldCellOf(self);
    return (c && c->tileWorld()) ? static_cast<double>(c->tileWorld()->shadeAt(x, y)) : 1.0;
}

}  // extern "C"

