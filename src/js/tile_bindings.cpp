#include "js/tile_bindings.h"
#if BRO_WITH_3D  // modular-build feature gate

#include <qjsbind/qjsbind.h>

#include "scene/tile_world.h"
#include "scene/scene_graph.h"
#include "js/mesh_bindings.h"
#include "js/ai_bindings.h"
#include "util/asset_mounts.h"
#include "broimage/decode.h"
#include "tile/serialize.h"
#include "tile/pathfind.h"
#include "tile/region.h"
#include "tile/coord.h"

#include <brogameagent/nav_grid.h>
#include <brogameagent/types.h>

#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

namespace bro::js {

// -------------------------------------------------------------------------
// App-relative path resolution for atlas images (mirrors scene_bindings).
// -------------------------------------------------------------------------

static std::string s_basePath;
static const util::AssetMounts* s_mounts = nullptr;

static std::string resolveAppPath(const std::string& src) {
    if (src.size() >= 2 && src[1] == ':') return src;       // Windows C:\...
    if (!src.empty() && (src[0] == '/' || src[0] == '\\')) {
        if (s_mounts) {
            std::string m = s_mounts->resolve(src);
            if (!m.empty()) return m;
        }
        return src;
    }
    if (s_basePath.empty()) return src;
    std::string path = s_basePath;
    if (path.back() != '/' && path.back() != '\\') path += '/';
    return path + src;
}

// -------------------------------------------------------------------------
// Helpers
// -------------------------------------------------------------------------

static int argInt(JSContext* ctx, JSValueConst v, int def = 0) {
    int32_t n = def;
    JS_ToInt32(ctx, &n, v);
    return n;
}

// Read a flat float array (typed Float32Array or plain Array of numbers).
static void readFloatArray(JSContext* ctx, JSValueConst v, std::vector<float>& out) {
    if (JS_IsUndefined(v) || JS_IsNull(v)) return;
    size_t offset = 0, byteLen = 0, bpe = 0;
    JSValue abuf = JS_GetTypedArrayBuffer(ctx, v, &offset, &byteLen, &bpe);
    if (!JS_IsException(abuf)) {
        size_t abufLen = 0;
        uint8_t* raw = JS_GetArrayBuffer(ctx, &abufLen, abuf);
        if (raw && byteLen > 0) {
            size_t count = byteLen / sizeof(float);
            const float* fp = reinterpret_cast<const float*>(raw + offset);
            out.assign(fp, fp + count);
        }
        JS_FreeValue(ctx, abuf);
        return;
    }
    JS_FreeValue(ctx, abuf);
    JSValue lenVal = JS_GetPropertyStr(ctx, v, "length");
    int32_t len = 0; JS_ToInt32(ctx, &len, lenVal);
    JS_FreeValue(ctx, lenVal);
    if (len > 0) {
        out.resize(len);
        for (int32_t i = 0; i < len; ++i) {
            JSValue el = JS_GetPropertyUint32(ctx, v, i);
            double d = 0; JS_ToFloat64(ctx, &d, el);
            out[i] = (float)d;
            JS_FreeValue(ctx, el);
        }
    }
}

// Read an [r,g,b,a] (or [r,g,b]) array property into out[4] (a defaults to 1).
static void readColor4(JSContext* ctx, JSValueConst obj, const char* prop, float out[4]) {
    JSValue v = JS_GetPropertyStr(ctx, obj, prop);
    if (JS_IsArray(v)) {
        JSValue lenVal = JS_GetPropertyStr(ctx, v, "length");
        int32_t len = 0; JS_ToInt32(ctx, &len, lenVal);
        JS_FreeValue(ctx, lenVal);
        for (int i = 0; i < 4 && i < len; ++i) {
            JSValue el = JS_GetPropertyUint32(ctx, v, i);
            double d = (i == 3) ? 1.0 : 0.0; JS_ToFloat64(ctx, &d, el);
            out[i] = (float)d;
            JS_FreeValue(ctx, el);
        }
    }
    JS_FreeValue(ctx, v);
}

// -------------------------------------------------------------------------
// TileWrapper — opaque data attached to JS TileWorld objects
// -------------------------------------------------------------------------

struct TileWrapper {
    std::unique_ptr<scene::TileWorld> world;

    explicit TileWrapper(std::unique_ptr<scene::TileWorld> w)
        : world(std::move(w)) { allInstances().insert(this); }
    ~TileWrapper() { allInstances().erase(this); }

    static std::unordered_set<TileWrapper*>& allInstances() {
        static std::unordered_set<TileWrapper*> s;
        return s;
    }
};

using TWld = TileWrapper;

// -------------------------------------------------------------------------
// Parse TileWorldConfig from JS options
// -------------------------------------------------------------------------

static scene::TileWorldConfig parseTileConfig(JSContext* ctx, JSValueConst opts) {
    scene::TileWorldConfig cfg;
    if (!JS_IsObject(opts)) return cfg;

    cfg.width      = qjsbind::get_prop_int(ctx, opts, "width", cfg.width);
    cfg.height     = qjsbind::get_prop_int(ctx, opts, "height", cfg.height);
    cfg.cellSize   = (float)qjsbind::get_prop_number(ctx, opts, "cellSize", cfg.cellSize);
    cfg.heightStep = (float)qjsbind::get_prop_number(ctx, opts, "heightStep", cfg.heightStep);
    cfg.chunkSize  = qjsbind::get_prop_int(ctx, opts, "chunkSize", cfg.chunkSize);
    cfg.baseLevel  = qjsbind::get_prop_int(ctx, opts, "baseLevel", cfg.baseLevel);
    cfg.aoStrength = (float)qjsbind::get_prop_number(ctx, opts, "aoStrength", cfg.aoStrength);

    JSValue topo = JS_GetPropertyStr(ctx, opts, "topology");
    if (JS_IsString(topo)) {
        const char* s = JS_ToCString(ctx, topo);
        if (s && std::string(s) == "hex") cfg.topology = tile::Topology::Hex;
        if (s) JS_FreeCString(ctx, s);
    }
    JS_FreeValue(ctx, topo);

    JSValue layers = JS_GetPropertyStr(ctx, opts, "layers");
    if (JS_IsArray(layers)) {
        JSValue lenVal = JS_GetPropertyStr(ctx, layers, "length");
        int32_t len = 0; JS_ToInt32(ctx, &len, lenVal);
        JS_FreeValue(ctx, lenVal);
        std::vector<std::string> names;
        for (int32_t i = 0; i < len; ++i) {
            JSValue el = JS_GetPropertyUint32(ctx, layers, i);
            const char* s = JS_ToCString(ctx, el);
            if (s) { names.emplace_back(s); JS_FreeCString(ctx, s); }
            JS_FreeValue(ctx, el);
        }
        if (!names.empty()) cfg.layers = std::move(names);
    }
    JS_FreeValue(ctx, layers);

    JSValue orig = JS_GetPropertyStr(ctx, opts, "origin");
    if (JS_IsArray(orig)) {
        JSValue ox = JS_GetPropertyUint32(ctx, orig, 0);
        JSValue oy = JS_GetPropertyUint32(ctx, orig, 1);
        JSValue oz = JS_GetPropertyUint32(ctx, orig, 2);
        double vx = 0, vy = 0, vz = 0;
        JS_ToFloat64(ctx, &vx, ox); JS_ToFloat64(ctx, &vy, oy); JS_ToFloat64(ctx, &vz, oz);
        cfg.origin = {(float)vx, (float)vy, (float)vz};
        JS_FreeValue(ctx, ox); JS_FreeValue(ctx, oy); JS_FreeValue(ctx, oz);
    }
    JS_FreeValue(ctx, orig);

    JSValue pal = JS_GetPropertyStr(ctx, opts, "palette");
    readFloatArray(ctx, pal, cfg.palette);
    JS_FreeValue(ctx, pal);

    // ---- tileset atlas --------------------------------------------------
    cfg.atlasColumns = qjsbind::get_prop_int(ctx, opts, "atlasColumns", cfg.atlasColumns);
    cfg.atlasRows    = qjsbind::get_prop_int(ctx, opts, "atlasRows", cfg.atlasRows);
    cfg.cliffCell    = qjsbind::get_prop_int(ctx, opts, "cliffCell", cfg.cliffCell);
    cfg.atlasInset   = (float)qjsbind::get_prop_number(ctx, opts, "atlasInset", cfg.atlasInset);

    // `atlas` is a path to an image decoded here; or supply raw `atlasPixels`
    // (Uint8Array RGBA) plus `atlasWidth`/`atlasHeight`.
    JSValue atlas = JS_GetPropertyStr(ctx, opts, "atlas");
    if (JS_IsString(atlas)) {
        const char* s = JS_ToCString(ctx, atlas);
        if (s) {
            broimage::Image img;
            std::string err;
            if (broimage::decode_file(resolveAppPath(s), img, &err) &&
                img.width > 0 && img.height > 0) {
                cfg.atlasWidth  = img.width;
                cfg.atlasHeight = img.height;
                cfg.atlasPixels = std::move(img.pixels);
            }
            JS_FreeCString(ctx, s);
        }
    } else {
        JSValue px = JS_GetPropertyStr(ctx, opts, "atlasPixels");
        if (!JS_IsUndefined(px) && !JS_IsNull(px)) {
            size_t offset = 0, byteLen = 0, bpe = 0;
            JSValue abuf = JS_GetTypedArrayBuffer(ctx, px, &offset, &byteLen, &bpe);
            if (!JS_IsException(abuf)) {
                size_t abufLen = 0;
                uint8_t* raw = JS_GetArrayBuffer(ctx, &abufLen, abuf);
                if (raw && byteLen > 0) {
                    cfg.atlasWidth  = qjsbind::get_prop_int(ctx, opts, "atlasWidth", 0);
                    cfg.atlasHeight = qjsbind::get_prop_int(ctx, opts, "atlasHeight", 0);
                    cfg.atlasPixels.assign(raw + offset, raw + offset + byteLen);
                }
            }
            JS_FreeValue(ctx, abuf);
        }
        JS_FreeValue(ctx, px);
    }
    JS_FreeValue(ctx, atlas);

    // tileAtlas: ground tile id -> atlas cell index.
    JSValue ta = JS_GetPropertyStr(ctx, opts, "tileAtlas");
    if (JS_IsArray(ta)) {
        JSValue lenVal = JS_GetPropertyStr(ctx, ta, "length");
        int32_t len = 0; JS_ToInt32(ctx, &len, lenVal);
        JS_FreeValue(ctx, lenVal);
        cfg.tileAtlas.resize(len);
        for (int32_t i = 0; i < len; ++i) {
            JSValue el = JS_GetPropertyUint32(ctx, ta, i);
            int32_t c = 0; JS_ToInt32(ctx, &c, el);
            cfg.tileAtlas[i] = c;
            JS_FreeValue(ctx, el);
        }
    }
    JS_FreeValue(ctx, ta);

    // autotiles: [{ id, mode, family?, cells:[variant->cell] }, ...]
    JSValue autos = JS_GetPropertyStr(ctx, opts, "autotiles");
    if (JS_IsArray(autos)) {
        JSValue lenVal = JS_GetPropertyStr(ctx, autos, "length");
        int32_t len = 0; JS_ToInt32(ctx, &len, lenVal);
        JS_FreeValue(ctx, lenVal);
        for (int32_t i = 0; i < len; ++i) {
            JSValue e = JS_GetPropertyUint32(ctx, autos, i);
            if (!JS_IsObject(e)) { JS_FreeValue(ctx, e); continue; }
            scene::TileWorldConfig::AutotileRule rule;
            rule.id = (uint16_t)qjsbind::get_prop_int(ctx, e, "id", 0);
            rule.layer = qjsbind::get_prop_int(ctx, e, "layer", 0);

            JSValue mode = JS_GetPropertyStr(ctx, e, "mode");
            if (JS_IsString(mode)) {
                const char* s = JS_ToCString(ctx, mode);
                std::string m = s ? s : "";
                if (m == "edge")  rule.mode = scene::TileWorldConfig::AutotileMode::Edge;
                else if (m == "wang") rule.mode = scene::TileWorldConfig::AutotileMode::Wang;
                else rule.mode = scene::TileWorldConfig::AutotileMode::Blob47;
                if (s) JS_FreeCString(ctx, s);
            }
            JS_FreeValue(ctx, mode);

            JSValue famv = JS_GetPropertyStr(ctx, e, "family");
            if (JS_IsString(famv)) {
                const char* s = JS_ToCString(ctx, famv);
                if (s && std::string(s) == "nonEmpty")
                    rule.family = scene::TileWorldConfig::AutotileFamily::NonEmpty;
                if (s) JS_FreeCString(ctx, s);
            }
            JS_FreeValue(ctx, famv);

            JSValue cells = JS_GetPropertyStr(ctx, e, "cells");
            if (JS_IsArray(cells)) {
                JSValue cl = JS_GetPropertyStr(ctx, cells, "length");
                int32_t clen = 0; JS_ToInt32(ctx, &clen, cl);
                JS_FreeValue(ctx, cl);
                rule.cells.resize(clen);
                for (int32_t k = 0; k < clen; ++k) {
                    JSValue el = JS_GetPropertyUint32(ctx, cells, k);
                    int32_t cv = 0; JS_ToInt32(ctx, &cv, el);
                    rule.cells[k] = cv;
                    JS_FreeValue(ctx, el);
                }
            }
            JS_FreeValue(ctx, cells);

            cfg.autotiles.push_back(std::move(rule));
            JS_FreeValue(ctx, e);
        }
    }
    JS_FreeValue(ctx, autos);

    // overlays: per-layer style, aligned to `layers` (index 0 = ground, ignored)
    JSValue ovs = JS_GetPropertyStr(ctx, opts, "overlays");
    if (JS_IsArray(ovs)) {
        JSValue lenVal = JS_GetPropertyStr(ctx, ovs, "length");
        int32_t len = 0; JS_ToInt32(ctx, &len, lenVal);
        JS_FreeValue(ctx, lenVal);
        cfg.overlays.resize(len);
        for (int32_t i = 0; i < len; ++i) {
            JSValue e = JS_GetPropertyUint32(ctx, ovs, i);
            if (JS_IsObject(e)) {
                cfg.overlays[i].opacity     = (float)qjsbind::get_prop_number(ctx, e, "opacity", 1.0);
                cfg.overlays[i].alphaCutoff = (float)qjsbind::get_prop_number(ctx, e, "alphaCutoff", 0.0);
            }
            JS_FreeValue(ctx, e);
        }
    }
    JS_FreeValue(ctx, ovs);

    // animations: [{ id, fps?, frames:[atlas cells] }, ...]
    JSValue anims = JS_GetPropertyStr(ctx, opts, "animations");
    if (JS_IsArray(anims)) {
        JSValue lenVal = JS_GetPropertyStr(ctx, anims, "length");
        int32_t len = 0; JS_ToInt32(ctx, &len, lenVal);
        JS_FreeValue(ctx, lenVal);
        for (int32_t i = 0; i < len; ++i) {
            JSValue e = JS_GetPropertyUint32(ctx, anims, i);
            if (!JS_IsObject(e)) { JS_FreeValue(ctx, e); continue; }
            scene::TileWorldConfig::TileAnimation an;
            an.id  = (uint16_t)qjsbind::get_prop_int(ctx, e, "id", 0);
            an.fps = (float)qjsbind::get_prop_number(ctx, e, "fps", 4.0);
            JSValue fr = JS_GetPropertyStr(ctx, e, "frames");
            if (JS_IsArray(fr)) {
                JSValue fl = JS_GetPropertyStr(ctx, fr, "length");
                int32_t flen = 0; JS_ToInt32(ctx, &flen, fl);
                JS_FreeValue(ctx, fl);
                an.frames.resize(flen);
                for (int32_t k = 0; k < flen; ++k) {
                    JSValue el = JS_GetPropertyUint32(ctx, fr, k);
                    int32_t cv = 0; JS_ToInt32(ctx, &cv, el);
                    an.frames[k] = cv;
                    JS_FreeValue(ctx, el);
                }
            }
            JS_FreeValue(ctx, fr);
            cfg.animations.push_back(std::move(an));
            JS_FreeValue(ctx, e);
        }
    }
    JS_FreeValue(ctx, anims);

    return cfg;
}

// -------------------------------------------------------------------------
// Methods needing raw argc/argv (optional args / object returns)
// -------------------------------------------------------------------------

// addObjectKind(mesh, style?) -> kind index. `mesh` is a bro.mesh object.
static JSValue js_tile_addObjectKind(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* w = qjsbind::unwrap<TWld>(ctx, this_val);
    if (!w || !w->world || argc < 1) return JS_NewInt32(ctx, -1);
    bromesh::MeshData* md = MeshBindings::getMeshData(ctx, argv[0]);
    if (!md) return JS_ThrowTypeError(ctx, "addObjectKind: first argument must be a mesh");

    scene::TileWorld::ObjectStyle style;
    if (argc > 1 && JS_IsObject(argv[1])) {
        JSValueConst s = argv[1];
        readColor4(ctx, s, "color", style.color);
        style.roughness   = (float)qjsbind::get_prop_number(ctx, s, "roughness", style.roughness);
        style.metallic    = (float)qjsbind::get_prop_number(ctx, s, "metallic", style.metallic);
        style.doubleSided = qjsbind::get_prop_int(ctx, s, "doubleSided", 0) != 0;
        style.alphaCutoff = (float)qjsbind::get_prop_number(ctx, s, "alphaCutoff", style.alphaCutoff);
        style.castsShadow = qjsbind::get_prop_int(ctx, s, "castsShadow", 1) != 0;
        style.atlasCols   = qjsbind::get_prop_int(ctx, s, "atlasColumns", 1);
        style.atlasRows   = qjsbind::get_prop_int(ctx, s, "atlasRows", 1);

        // texture: a path (decoded) or raw texturePixels + width/height
        JSValue tex = JS_GetPropertyStr(ctx, s, "texture");
        if (JS_IsString(tex)) {
            const char* p = JS_ToCString(ctx, tex);
            if (p) {
                broimage::Image img; std::string err;
                if (broimage::decode_file(resolveAppPath(p), img, &err) &&
                    img.width > 0 && img.height > 0) {
                    style.texWidth = img.width; style.texHeight = img.height;
                    style.texPixels = std::move(img.pixels);
                }
                JS_FreeCString(ctx, p);
            }
        }
        JS_FreeValue(ctx, tex);
    }

    int kind = w->world->addObjectKind(bromesh::MeshData(*md), style);
    return JS_NewInt32(ctx, kind);
}

// addObject(kind, x, y, opts?) -> instance index
static JSValue js_tile_addObject(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* w = qjsbind::unwrap<TWld>(ctx, this_val);
    if (!w || !w->world || argc < 3) return JS_NewInt32(ctx, -1);
    scene::TileWorld::ObjectPlacement p;
    if (argc > 3 && JS_IsObject(argv[3])) {
        JSValueConst o = argv[3];
        p.yaw     = (float)qjsbind::get_prop_number(ctx, o, "yaw", 0.0);
        p.scale   = (float)qjsbind::get_prop_number(ctx, o, "scale", 1.0);
        p.yOffset = (float)qjsbind::get_prop_number(ctx, o, "yOffset", 0.0);
        p.offsetX = (float)qjsbind::get_prop_number(ctx, o, "offsetX", 0.0);
        p.offsetZ = (float)qjsbind::get_prop_number(ctx, o, "offsetZ", 0.0);
        p.variant = qjsbind::get_prop_int(ctx, o, "variant", 0);
        readColor4(ctx, o, "color", p.color);
    }
    int idx = w->world->addObject(argInt(ctx, argv[0]), argInt(ctx, argv[1]),
                                  argInt(ctx, argv[2]), p);
    return JS_NewInt32(ctx, idx);
}

static JSValue js_tile_clearObjects(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* w = qjsbind::unwrap<TWld>(ctx, this_val);
    if (!w || !w->world) return JS_UNDEFINED;
    int kind = (argc > 0) ? argInt(ctx, argv[0]) : -1;
    w->world->clearObjects(kind);
    return JS_UNDEFINED;
}

static JSValue js_tile_setTile(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* w = qjsbind::unwrap<TWld>(ctx, this_val);
    if (!w || !w->world || argc < 3) return JS_UNDEFINED;
    int layer = (argc > 3) ? argInt(ctx, argv[3]) : 0;
    w->world->setTile(argInt(ctx, argv[0]), argInt(ctx, argv[1]),
                      (uint16_t)argInt(ctx, argv[2]), layer);
    return JS_UNDEFINED;
}

static JSValue js_tile_getTile(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* w = qjsbind::unwrap<TWld>(ctx, this_val);
    if (!w || !w->world || argc < 2) return JS_NewInt32(ctx, 0);
    int layer = (argc > 2) ? argInt(ctx, argv[2]) : 0;
    return JS_NewInt32(ctx, w->world->tile(argInt(ctx, argv[0]), argInt(ctx, argv[1]), layer));
}

static JSValue js_tile_fillTile(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* w = qjsbind::unwrap<TWld>(ctx, this_val);
    if (!w || !w->world || argc < 5) return JS_UNDEFINED;
    int layer = (argc > 5) ? argInt(ctx, argv[5]) : 0;
    w->world->fillTile(argInt(ctx, argv[0]), argInt(ctx, argv[1]),
                       argInt(ctx, argv[2]), argInt(ctx, argv[3]),
                       (uint16_t)argInt(ctx, argv[4]), layer);
    return JS_UNDEFINED;
}

static double argFloat(JSContext* ctx, JSValueConst v, double def = 0.0) {
    double d = def;
    if (!JS_IsUndefined(v)) JS_ToFloat64(ctx, &d, v);
    return d;
}

static JSValue js_tile_setTint(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* w = qjsbind::unwrap<TWld>(ctx, this_val);
    if (!w || !w->world || argc < 5) return JS_UNDEFINED;
    double a = (argc > 5) ? argFloat(ctx, argv[5], 1.0) : 1.0;
    w->world->setTint(argInt(ctx, argv[0]), argInt(ctx, argv[1]),
                      (float)argFloat(ctx, argv[2]), (float)argFloat(ctx, argv[3]),
                      (float)argFloat(ctx, argv[4]), (float)a);
    return JS_UNDEFINED;
}

static JSValue js_tile_fillTint(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* w = qjsbind::unwrap<TWld>(ctx, this_val);
    if (!w || !w->world || argc < 7) return JS_UNDEFINED;
    double a = (argc > 7) ? argFloat(ctx, argv[7], 1.0) : 1.0;
    w->world->fillTint(argInt(ctx, argv[0]), argInt(ctx, argv[1]),
                       argInt(ctx, argv[2]), argInt(ctx, argv[3]),
                       (float)argFloat(ctx, argv[4]), (float)argFloat(ctx, argv[5]),
                       (float)argFloat(ctx, argv[6]), (float)a);
    return JS_UNDEFINED;
}

static JSValue js_tile_worldToCell(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* w = qjsbind::unwrap<TWld>(ctx, this_val);
    if (!w || !w->world || argc < 2) return JS_NULL;
    double wx = 0, wz = 0;
    JS_ToFloat64(ctx, &wx, argv[0]);
    JS_ToFloat64(ctx, &wz, argv[1]);
    int cx = 0, cy = 0;
    if (!w->world->worldToCell((float)wx, (float)wz, cx, cy)) return JS_NULL;
    JSValue out = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, out, "x", JS_NewInt32(ctx, cx));
    JS_SetPropertyStr(ctx, out, "y", JS_NewInt32(ctx, cy));
    return out;
}

// cellCenterWorldXZ(x, y) -> {x, z} | null — topology-aware world-space cell center.
static JSValue js_tile_cellCenterWorldXZ(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* w = qjsbind::unwrap<TWld>(ctx, this_val);
    if (!w || !w->world || argc < 2) return JS_NULL;
    float px = 0, pz = 0;
    w->world->cellCenterWorldXZ(argInt(ctx, argv[0]), argInt(ctx, argv[1]), px, pz);
    JSValue out = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, out, "x", JS_NewFloat64(ctx, px));
    JS_SetPropertyStr(ctx, out, "z", JS_NewFloat64(ctx, pz));
    return out;
}

// worldBounds() -> {minX, minZ, maxX, maxZ} | null — topology-aware grid extent.
static JSValue js_tile_worldBounds(JSContext* ctx, JSValueConst this_val, int /*argc*/, JSValueConst* /*argv*/) {
    auto* w = qjsbind::unwrap<TWld>(ctx, this_val);
    if (!w || !w->world) return JS_NULL;
    float minX = 0, minZ = 0, maxX = 0, maxZ = 0;
    w->world->worldBounds(minX, minZ, maxX, maxZ);
    JSValue out = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, out, "minX", JS_NewFloat64(ctx, minX));
    JS_SetPropertyStr(ctx, out, "minZ", JS_NewFloat64(ctx, minZ));
    JS_SetPropertyStr(ctx, out, "maxX", JS_NewFloat64(ctx, maxX));
    JS_SetPropertyStr(ctx, out, "maxZ", JS_NewFloat64(ctx, maxZ));
    return out;
}

// ---- picking / collision / nav-grid sync --------------------------------

static bool readVec3Arg(JSContext* ctx, JSValueConst v, bromath::Vec3& out) {
    if (!JS_IsObject(v)) return false;
    JSValue jx = JS_GetPropertyUint32(ctx, v, 0);
    JSValue jy = JS_GetPropertyUint32(ctx, v, 1);
    JSValue jz = JS_GetPropertyUint32(ctx, v, 2);
    double x = 0, y = 0, z = 0;
    JS_ToFloat64(ctx, &x, jx); JS_ToFloat64(ctx, &y, jy); JS_ToFloat64(ctx, &z, jz);
    JS_FreeValue(ctx, jx); JS_FreeValue(ctx, jy); JS_FreeValue(ctx, jz);
    out = {(float)x, (float)y, (float)z};
    return true;
}

// raycastCell(origin, dir, maxDist?) -> { cell:[x,y], x, y, point:[x,y,z],
//                                         distance, side } | null
static JSValue js_tile_raycastCell(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* w = qjsbind::unwrap<TWld>(ctx, this_val);
    if (!w || !w->world || argc < 2) return JS_NULL;
    bromath::Vec3 o, d;
    if (!readVec3Arg(ctx, argv[0], o) || !readVec3Arg(ctx, argv[1], d)) return JS_NULL;
    float maxDist = (argc > 2 && !JS_IsUndefined(argv[2]))
                        ? (float)argFloat(ctx, argv[2], 1.0e6) : 1.0e6f;
    auto hit = w->world->raycastCell(o, d, maxDist);
    if (!hit.hit) return JS_NULL;

    JSValue out = JS_NewObject(ctx);
    JSValue cell = JS_NewArray(ctx);
    JS_SetPropertyUint32(ctx, cell, 0, JS_NewInt32(ctx, hit.x));
    JS_SetPropertyUint32(ctx, cell, 1, JS_NewInt32(ctx, hit.y));
    JS_SetPropertyStr(ctx, out, "cell", cell);
    JS_SetPropertyStr(ctx, out, "x", JS_NewInt32(ctx, hit.x));
    JS_SetPropertyStr(ctx, out, "y", JS_NewInt32(ctx, hit.y));
    JSValue pt = JS_NewArray(ctx);
    JS_SetPropertyUint32(ctx, pt, 0, JS_NewFloat64(ctx, hit.point[0]));
    JS_SetPropertyUint32(ctx, pt, 1, JS_NewFloat64(ctx, hit.point[1]));
    JS_SetPropertyUint32(ctx, pt, 2, JS_NewFloat64(ctx, hit.point[2]));
    JS_SetPropertyStr(ctx, out, "point", pt);
    JS_SetPropertyStr(ctx, out, "distance", JS_NewFloat64(ctx, hit.distance));
    JS_SetPropertyStr(ctx, out, "side", JS_NewBool(ctx, hit.side));
    return out;
}

// sampleHeight(wx, wz) -> top-surface world Y | null
static JSValue js_tile_sampleHeight(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* w = qjsbind::unwrap<TWld>(ctx, this_val);
    if (!w || !w->world || argc < 2) return JS_NULL;
    double wx = 0, wz = 0;
    JS_ToFloat64(ctx, &wx, argv[0]);
    JS_ToFloat64(ctx, &wz, argv[1]);
    float y = 0;
    if (!w->world->sampleHeight((float)wx, (float)wz, y)) return JS_NULL;
    return JS_NewFloat64(ctx, y);
}

// isWalkable(x, y, blockMask?) -> bool
static JSValue js_tile_isWalkable(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* w = qjsbind::unwrap<TWld>(ctx, this_val);
    if (!w || !w->world || argc < 2) return JS_FALSE;
    uint32_t mask = (argc > 2) ? (uint32_t)(int64_t)argFloat(ctx, argv[2], 0) : 0u;
    return JS_NewBool(ctx, w->world->isWalkable(argInt(ctx, argv[0]), argInt(ctx, argv[1]), mask));
}

// Stamp every non-walkable cell of `world` into nav grid `ng` as a one-cell
// AABB obstacle. Returns the number of cells blocked.
static int stampNavGrid(scene::TileWorld* world, brogameagent::NavGrid* ng,
                        uint32_t blockMask, float padding) {
    const auto& cfg = world->config();
    const float cs = cfg.cellSize;
    int blocked = 0;
    for (int y = 0; y < cfg.height; ++y) {
        for (int x = 0; x < cfg.width; ++x) {
            if (world->isWalkable(x, y, blockMask)) continue;
            brogameagent::AABB box;
            world->cellCenterWorldXZ(x, y, box.cx, box.cz);
            box.hw = cs * 0.49f;       // stays within the one cell, no bleed
            box.hd = cs * 0.49f;
            ng->addObstacle(box, padding);
            ++blocked;
        }
    }
    return blocked;
}

// syncNavGrid(navGrid, { blockMask?, padding? }) -> blocked-cell count
static JSValue js_tile_syncNavGrid(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* w = qjsbind::unwrap<TWld>(ctx, this_val);
    if (!w || !w->world || argc < 1) return JS_NewInt32(ctx, 0);
    brogameagent::NavGrid* ng = navGridFromJS(ctx, argv[0]);
    if (!ng) return JS_ThrowTypeError(ctx, "syncNavGrid: first argument must be a nav grid");
    uint32_t mask = 0; float padding = 0;
    if (argc > 1 && JS_IsObject(argv[1])) {
        mask = (uint32_t)(int64_t)qjsbind::get_prop_number(ctx, argv[1], "blockMask", 0);
        padding = (float)qjsbind::get_prop_number(ctx, argv[1], "padding", 0);
    }
    return JS_NewInt32(ctx, stampNavGrid(w->world.get(), ng, mask, padding));
}

// toNavGrid({ blockMask?, padding? }) -> a fresh AINavGrid sized to the world
static JSValue js_tile_toNavGrid(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* w = qjsbind::unwrap<TWld>(ctx, this_val);
    if (!w || !w->world) return JS_NULL;
    const auto& cfg = w->world->config();
    uint32_t mask = 0; float padding = 0;
    if (argc > 0 && JS_IsObject(argv[0])) {
        mask = (uint32_t)(int64_t)qjsbind::get_prop_number(ctx, argv[0], "blockMask", 0);
        padding = (float)qjsbind::get_prop_number(ctx, argv[0], "padding", 0);
    }
    const float cs = cfg.cellSize;
    float minX = 0, minZ = 0, maxX = 0, maxZ = 0;
    w->world->worldBounds(minX, minZ, maxX, maxZ);
    JSValue gridVal = createNavGridJS(ctx, minX, minZ, maxX, maxZ, cs);
    if (brogameagent::NavGrid* ng = navGridFromJS(ctx, gridVal))
        stampNavGrid(w->world.get(), ng, mask, padding);
    return gridVal;
}

// ---- grid search / regions / coordinate math -----------------------------
//
// These bind the pure bro::tile cell-grid layers (pathfind.h / region.h /
// coord.h): deterministic integer queries for game logic — movement ranges,
// creep pathing, blast propagation, zoning. Predicates are declarative
// (blockMask / per-tile-id costs), not JS callbacks, so searches stay native
// speed.

// Shared passability: cell carries ground (non-empty tile on layer 0) and none
// of blockMask's flag bits — identical semantics to isWalkable()/toNavGrid().
static tile::PassFn makePassFn(uint32_t blockMask) {
    return [blockMask](const tile::TileGrid& g, tile::Cell c) {
        return g.tile(0, c) != 0 && (g.flags(c) & blockMask) == 0;
    };
}

// Optional per-tile-id step cost: entering a cell costs costs[groundTileId],
// clamped to >= 1; ids beyond the array cost 1. Empty array -> uniform.
static tile::CostFn makeCostFn(std::vector<float> costs) {
    if (costs.empty()) return nullptr;
    return [costs = std::move(costs)](const tile::TileGrid& g, tile::Cell,
                                      tile::Cell to) -> float {
        uint16_t id = g.tile(0, to);
        float c = (id < costs.size()) ? costs[id] : 1.0f;
        return c < 1.0f ? 1.0f : c;
    };
}

static tile::Conn parseConn(JSContext* ctx, JSValueConst v, tile::Conn def = tile::Conn::Edge) {
    tile::Conn conn = def;
    if (JS_IsString(v)) {
        const char* s = JS_ToCString(ctx, v);
        if (s && std::string(s) == "vertex") conn = tile::Conn::Vertex;
        if (s) JS_FreeCString(ctx, s);
    }
    return conn;
}

// Search options object: { blockMask?, costs?, conn? }
struct SearchOpts {
    uint32_t blockMask = 0;
    std::vector<float> costs;
    tile::Conn conn = tile::Conn::Edge;
};

static SearchOpts parseSearchOpts(JSContext* ctx, JSValueConst v) {
    SearchOpts o;
    if (!JS_IsObject(v)) return o;
    o.blockMask = (uint32_t)(int64_t)qjsbind::get_prop_number(ctx, v, "blockMask", 0);
    JSValue costs = JS_GetPropertyStr(ctx, v, "costs");
    readFloatArray(ctx, costs, o.costs);
    JS_FreeValue(ctx, costs);
    JSValue conn = JS_GetPropertyStr(ctx, v, "conn");
    o.conn = parseConn(ctx, conn);
    JS_FreeValue(ctx, conn);
    return o;
}

static JSValue makeCellObj(JSContext* ctx, tile::Cell c) {
    JSValue o = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, o, "x", JS_NewInt32(ctx, c.x));
    JS_SetPropertyStr(ctx, o, "y", JS_NewInt32(ctx, c.y));
    return o;
}

static JSValue makeCellArray(JSContext* ctx, const std::vector<tile::Cell>& cells) {
    JSValue arr = JS_NewArray(ctx);
    for (uint32_t i = 0; i < cells.size(); ++i)
        JS_SetPropertyUint32(ctx, arr, i, makeCellObj(ctx, cells[i]));
    return arr;
}

// Read one cell from {x,y} or [x,y].
static bool readCellArg(JSContext* ctx, JSValueConst v, tile::Cell& out) {
    if (!JS_IsObject(v)) return false;
    if (JS_IsArray(v)) {
        JSValue jx = JS_GetPropertyUint32(ctx, v, 0);
        JSValue jy = JS_GetPropertyUint32(ctx, v, 1);
        out.x = argInt(ctx, jx); out.y = argInt(ctx, jy);
        JS_FreeValue(ctx, jx); JS_FreeValue(ctx, jy);
        return true;
    }
    JSValue jx = JS_GetPropertyStr(ctx, v, "x");
    JSValue jy = JS_GetPropertyStr(ctx, v, "y");
    bool ok = !JS_IsUndefined(jx) && !JS_IsUndefined(jy);
    if (ok) { out.x = argInt(ctx, jx); out.y = argInt(ctx, jy); }
    JS_FreeValue(ctx, jx); JS_FreeValue(ctx, jy);
    return ok;
}

// findPath(x0, y0, x1, y1, { blockMask?, costs?, conn? }?) -> [{x,y}, ...]
// A* over walkable cells, endpoints inclusive; [] when unreachable.
static JSValue js_tile_findPath(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* w = qjsbind::unwrap<TWld>(ctx, this_val);
    if (!w || !w->world || !w->world->grid() || argc < 4) return JS_NewArray(ctx);
    SearchOpts o = parseSearchOpts(ctx, argc > 4 ? argv[4] : JS_UNDEFINED);
    auto path = tile::aStar(*w->world->grid(),
                            {argInt(ctx, argv[0]), argInt(ctx, argv[1])},
                            {argInt(ctx, argv[2]), argInt(ctx, argv[3])},
                            makePassFn(o.blockMask), makeCostFn(std::move(o.costs)),
                            o.conn);
    return makeCellArray(ctx, path);
}

// distanceField(sources, { blockMask?, conn? }?) -> Int32Array (w*h, row-major;
// -1 = unreachable/impassable). sources: array of {x,y} or [x,y].
static JSValue js_tile_distanceField(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* w = qjsbind::unwrap<TWld>(ctx, this_val);
    if (!w || !w->world || !w->world->grid() || argc < 1) return JS_NULL;

    std::vector<tile::Cell> sources;
    if (JS_IsArray(argv[0])) {
        JSValue lenVal = JS_GetPropertyStr(ctx, argv[0], "length");
        int32_t len = 0; JS_ToInt32(ctx, &len, lenVal);
        JS_FreeValue(ctx, lenVal);
        for (int32_t i = 0; i < len; ++i) {
            JSValue el = JS_GetPropertyUint32(ctx, argv[0], i);
            tile::Cell c;
            if (readCellArg(ctx, el, c)) sources.push_back(c);
            JS_FreeValue(ctx, el);
        }
    } else {
        tile::Cell c;
        if (readCellArg(ctx, argv[0], c)) sources.push_back(c);
    }
    if (sources.empty()) return JS_NULL;

    SearchOpts o = parseSearchOpts(ctx, argc > 1 ? argv[1] : JS_UNDEFINED);
    std::vector<int> field = tile::distanceField(*w->world->grid(), sources,
                                                 makePassFn(o.blockMask), o.conn);

    JSValue abuf = JS_NewArrayBufferCopy(ctx, reinterpret_cast<const uint8_t*>(field.data()),
                                         field.size() * sizeof(int));
    JSValue args[3] = { abuf, JS_UNDEFINED, JS_UNDEFINED };
    JSValue arr = JS_NewTypedArray(ctx, 1, args, JS_TYPED_ARRAY_INT32);
    JS_FreeValue(ctx, abuf);
    return arr;
}

// Region match options: { layer?, id?, flag?, conn? }. Priority: flag -> id ->
// same-tile-as-seed (floodFill) / non-empty (components).
static tile::MatchFn makeMatchFn(JSContext* ctx, JSValueConst v, const tile::TileGrid& g,
                                 const tile::Cell* seed, int& layerOut, tile::Conn& connOut) {
    int layer = 0;
    double flag = 0, id = -1;
    if (JS_IsObject(v)) {
        layer = qjsbind::get_prop_int(ctx, v, "layer", 0);
        flag  = qjsbind::get_prop_number(ctx, v, "flag", 0);
        id    = qjsbind::get_prop_number(ctx, v, "id", -1);
        JSValue conn = JS_GetPropertyStr(ctx, v, "conn");
        connOut = parseConn(ctx, conn);
        JS_FreeValue(ctx, conn);
    }
    layerOut = layer;
    if (flag > 0) return tile::matchFlag((uint32_t)(int64_t)flag);
    if (id >= 0) return tile::matchTile(layer, (uint16_t)id);
    if (seed)    return tile::matchTile(layer, g.tile(layer, *seed));
    // components() default: any non-empty cell on the layer
    return [layer](const tile::TileGrid& gg, tile::Cell c) { return gg.tile(layer, c) != 0; };
}

// floodFill(x, y, { layer?, id?, flag?, conn? }?) -> [{x,y}, ...]
// Default match: cells with the same tile id as the seed on `layer`.
static JSValue js_tile_floodFill(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* w = qjsbind::unwrap<TWld>(ctx, this_val);
    if (!w || !w->world || !w->world->grid() || argc < 2) return JS_NewArray(ctx);
    const tile::TileGrid& g = *w->world->grid();
    tile::Cell seed{argInt(ctx, argv[0]), argInt(ctx, argv[1])};
    int layer = 0; tile::Conn conn = tile::Conn::Edge;
    tile::MatchFn match = makeMatchFn(ctx, argc > 2 ? argv[2] : JS_UNDEFINED, g,
                                      &seed, layer, conn);
    return makeCellArray(ctx, tile::floodFill(g, seed, match, conn));
}

// components({ layer?, id?, flag?, conn? }?) -> [[{x,y},...], ...]
// Default match: any non-empty cell on `layer`.
static JSValue js_tile_components(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* w = qjsbind::unwrap<TWld>(ctx, this_val);
    if (!w || !w->world || !w->world->grid()) return JS_NewArray(ctx);
    const tile::TileGrid& g = *w->world->grid();
    int layer = 0; tile::Conn conn = tile::Conn::Edge;
    tile::MatchFn match = makeMatchFn(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, g,
                                      nullptr, layer, conn);
    auto comps = tile::components(g, match, conn);
    JSValue arr = JS_NewArray(ctx);
    for (uint32_t i = 0; i < comps.size(); ++i)
        JS_SetPropertyUint32(ctx, arr, i, makeCellArray(ctx, comps[i]));
    return arr;
}

// Keep only in-bounds cells (coord.h math is unbounded; games want real cells).
static void filterInBounds(const tile::TileGrid& g, std::vector<tile::Cell>& cells) {
    std::erase_if(cells, [&g](tile::Cell c) { return !g.inBounds(c); });
}

// cellDistance(x0, y0, x1, y1, conn?) -> topology grid distance
static JSValue js_tile_cellDistance(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* w = qjsbind::unwrap<TWld>(ctx, this_val);
    if (!w || !w->world || !w->world->grid() || argc < 4) return JS_NewInt32(ctx, -1);
    tile::Conn conn = parseConn(ctx, argc > 4 ? argv[4] : JS_UNDEFINED);
    return JS_NewInt32(ctx, tile::distance(w->world->grid()->topology(),
                                           {argInt(ctx, argv[0]), argInt(ctx, argv[1])},
                                           {argInt(ctx, argv[2]), argInt(ctx, argv[3])},
                                           conn));
}

// cellRing(x, y, radius, conn?) -> [{x,y}, ...] at exactly `radius` (in-bounds only)
static JSValue js_tile_cellRing(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* w = qjsbind::unwrap<TWld>(ctx, this_val);
    if (!w || !w->world || !w->world->grid() || argc < 3) return JS_NewArray(ctx);
    const tile::TileGrid& g = *w->world->grid();
    tile::Conn conn = parseConn(ctx, argc > 3 ? argv[3] : JS_UNDEFINED);
    auto cells = tile::ring(g.topology(), {argInt(ctx, argv[0]), argInt(ctx, argv[1])},
                            argInt(ctx, argv[2]), conn);
    filterInBounds(g, cells);
    return makeCellArray(ctx, cells);
}

// cellsInRange(x, y, radius, conn?) -> [{x,y}, ...] within `radius` (in-bounds only)
static JSValue js_tile_cellsInRange(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* w = qjsbind::unwrap<TWld>(ctx, this_val);
    if (!w || !w->world || !w->world->grid() || argc < 3) return JS_NewArray(ctx);
    const tile::TileGrid& g = *w->world->grid();
    tile::Conn conn = parseConn(ctx, argc > 3 ? argv[3] : JS_UNDEFINED);
    auto cells = tile::range(g.topology(), {argInt(ctx, argv[0]), argInt(ctx, argv[1])},
                             argInt(ctx, argv[2]), conn);
    filterInBounds(g, cells);
    return makeCellArray(ctx, cells);
}

// cellLine(x0, y0, x1, y1) -> [{x,y}, ...] connected line a->b (in-bounds only)
static JSValue js_tile_cellLine(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* w = qjsbind::unwrap<TWld>(ctx, this_val);
    if (!w || !w->world || !w->world->grid() || argc < 4) return JS_NewArray(ctx);
    const tile::TileGrid& g = *w->world->grid();
    auto cells = tile::line(g.topology(), {argInt(ctx, argv[0]), argInt(ctx, argv[1])},
                            {argInt(ctx, argv[2]), argInt(ctx, argv[3])});
    filterInBounds(g, cells);
    return makeCellArray(ctx, cells);
}

// save() -> Uint8Array (bro::tile's versioned grid format; see tile/serialize.h)
static JSValue js_tile_save(JSContext* ctx, JSValueConst this_val, int /*argc*/, JSValueConst* /*argv*/) {
    auto* w = qjsbind::unwrap<TWld>(ctx, this_val);
    if (!w || !w->world || !w->world->grid()) return JS_NULL;
    std::vector<uint8_t> bytes = tile::serialize(*w->world->grid());
    JSValue abuf = JS_NewArrayBufferCopy(ctx, bytes.data(), bytes.size());
    JSValue args[3] = { abuf, JS_UNDEFINED, JS_UNDEFINED };
    JSValue arr = JS_NewTypedArray(ctx, 1, args, JS_TYPED_ARRAY_UINT8);
    JS_FreeValue(ctx, abuf);
    return arr;
}

// load(bytes: Uint8Array) -> boolean (false on corrupt/unrecognized data)
static JSValue js_tile_load(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* w = qjsbind::unwrap<TWld>(ctx, this_val);
    if (!w || !w->world || argc < 1) return JS_NewBool(ctx, false);

    std::vector<uint8_t> bytes;
    size_t offset = 0, byteLen = 0, bpe = 0;
    JSValue abuf = JS_GetTypedArrayBuffer(ctx, argv[0], &offset, &byteLen, &bpe);
    if (!JS_IsException(abuf)) {
        size_t abufLen = 0;
        uint8_t* raw = JS_GetArrayBuffer(ctx, &abufLen, abuf);
        if (raw && byteLen > 0) bytes.assign(raw + offset, raw + offset + byteLen);
    }
    JS_FreeValue(ctx, abuf);

    auto grid = tile::deserialize(bytes);
    if (!grid) return JS_NewBool(ctx, false);
    w->world->loadGrid(std::move(*grid));
    return JS_NewBool(ctx, true);
}

static JSValue js_tile_configure(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* w = qjsbind::unwrap<TWld>(ctx, this_val);
    if (!w || !w->world || argc < 1 || !JS_IsObject(argv[0])) return JS_UNDEFINED;
    w->world->configure(parseTileConfig(ctx, argv[0]));
    return JS_UNDEFINED;
}

// -------------------------------------------------------------------------
// Factory: scene.createTileWorld(opts)
// -------------------------------------------------------------------------

JSValue createTileWorldJS(JSContext* ctx, scene::SceneGraph* graph, JSValueConst opts) {
    if (!graph) return JS_ThrowTypeError(ctx, "createTileWorld: no scene graph");
    auto world = std::make_unique<scene::TileWorld>(*graph);
    world->configure(parseTileConfig(ctx, opts));
    return qjsbind::wrap<TWld>(ctx, new TWld(std::move(world)));
}

// -------------------------------------------------------------------------
// Install / Cleanup
// -------------------------------------------------------------------------

void TileBindings::install(JSContext* ctx) {
    qjsbind::Class<TWld>(ctx, "TileWorld")
        // No constructor — created via scene.createTileWorld().
        .method_raw("setTile", js_tile_setTile, 4)
        .method_raw("getTile", js_tile_getTile, 3)
        .method_raw("fillTile", js_tile_fillTile, 6)
        .method("setElevation", [](TWld* self, int x, int y, int level) {
            if (self->world) self->world->setElevation(x, y, level);
        })
        .method("getElevation", [](TWld* self, int x, int y) -> int {
            return self->world ? self->world->elevation(x, y) : 0;
        })
        .method("fillElevation", [](TWld* self, int x0, int y0, int x1, int y1, int level) {
            if (self->world) self->world->fillElevation(x0, y0, x1, y1, level);
        })
        .method_raw("setTint", js_tile_setTint, 6)
        .method_raw("fillTint", js_tile_fillTint, 8)
        .method("setFlag", [](TWld* self, int x, int y, double bit, bool on) {
            if (self->world) self->world->setFlag(x, y, (uint32_t)bit, on);
        })
        .method("hasFlag", [](TWld* self, int x, int y, double bit) -> bool {
            return self->world ? self->world->hasFlag(x, y, (uint32_t)bit) : false;
        })
        .method_raw("worldToCell", js_tile_worldToCell, 2)
        .method_raw("cellCenterWorldXZ", js_tile_cellCenterWorldXZ, 2)
        .method_raw("worldBounds", js_tile_worldBounds, 0)
        .method_raw("raycastCell", js_tile_raycastCell, 3)
        .method_raw("sampleHeight", js_tile_sampleHeight, 2)
        .method_raw("isWalkable", js_tile_isWalkable, 3)
        .method_raw("syncNavGrid", js_tile_syncNavGrid, 2)
        .method_raw("toNavGrid", js_tile_toNavGrid, 1)
        .method_raw("findPath", js_tile_findPath, 5)
        .method_raw("distanceField", js_tile_distanceField, 2)
        .method_raw("floodFill", js_tile_floodFill, 3)
        .method_raw("components", js_tile_components, 1)
        .method_raw("cellDistance", js_tile_cellDistance, 5)
        .method_raw("cellRing", js_tile_cellRing, 4)
        .method_raw("cellsInRange", js_tile_cellsInRange, 4)
        .method_raw("cellLine", js_tile_cellLine, 4)
        .method("setOrigin", [](TWld* self, double x, double y, double z) {
            if (self->world) self->world->setOrigin((float)x, (float)y, (float)z);
        })
        .method("advance", [](TWld* self, double dtMs) -> bool {
            return self->world ? self->world->advance(dtMs) : false;
        })
        .method_raw("addObjectKind", js_tile_addObjectKind, 2)
        .method_raw("addObject", js_tile_addObject, 4)
        .method_raw("clearObjects", js_tile_clearObjects, 1)
        .method("objectCount", [](TWld* self, int kind) -> int {
            return self->world ? self->world->objectCount(kind) : 0;
        })
        .method("rebuildObjects", [](TWld* self) {
            if (self->world) self->world->rebuildObjects();
        })
        .method("rebuild", [](TWld* self) {
            if (self->world) self->world->rebuildDirty();
        })
        .method("rebuildAll", [](TWld* self) {
            if (self->world) self->world->rebuildAll();
        })
        .method_raw("configure", js_tile_configure, 1)
        .method_raw("save", js_tile_save, 0)
        .method_raw("load", js_tile_load, 1)
        .method("destroy", [](TWld* self) {
            if (self->world) { self->world->clear(); self->world.reset(); }
        })
        .get("width",  [](TWld* self) -> int { return self->world ? self->world->width()  : 0; })
        .get("height", [](TWld* self) -> int { return self->world ? self->world->height() : 0; })
        .get("chunkCount",    [](TWld* self) -> int { return self->world ? self->world->chunkCount()    : 0; })
        .get("vertexCount",   [](TWld* self) -> int { return self->world ? self->world->totalVertices() : 0; })
        .get("triangleCount", [](TWld* self) -> int { return self->world ? self->world->totalTriangles(): 0; });
}

void TileBindings::setAppContext(const std::string& basePath,
                                 const util::AssetMounts* mounts) {
    s_basePath = basePath;
    s_mounts = mounts;
}

void TileBindings::cleanup(JSContext*) {
    for (auto* tw : TWld::allInstances()) {
        if (tw->world) { tw->world->clear(); tw->world.reset(); }
    }
}

} // namespace bro::js

#endif  // BRO_WITH_3D
