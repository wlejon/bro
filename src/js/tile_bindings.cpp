#include "js/tile_bindings.h"

#include <qjsbind/qjsbind.h>

#include "scene/tile_world.h"
#include "scene/scene_graph.h"
#include "js/mesh_bindings.h"
#include "js/ai_bindings.h"
#include "util/asset_mounts.h"
#include "broimage/decode.h"

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
// Helpers (local copies — same pattern as terrain_bindings.cpp)
// -------------------------------------------------------------------------

static double jsGetProp(JSContext* ctx, JSValueConst obj, const char* prop, double def = 0.0) {
    JSValue v = JS_GetPropertyStr(ctx, obj, prop);
    double r = def;
    if (!JS_IsUndefined(v)) JS_ToFloat64(ctx, &r, v);
    JS_FreeValue(ctx, v);
    return r;
}

static int jsGetInt(JSContext* ctx, JSValueConst obj, const char* prop, int def = 0) {
    return static_cast<int>(jsGetProp(ctx, obj, prop, def));
}

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

    cfg.width      = jsGetInt(ctx, opts, "width", cfg.width);
    cfg.height     = jsGetInt(ctx, opts, "height", cfg.height);
    cfg.cellSize   = (float)jsGetProp(ctx, opts, "cellSize", cfg.cellSize);
    cfg.heightStep = (float)jsGetProp(ctx, opts, "heightStep", cfg.heightStep);
    cfg.chunkSize  = jsGetInt(ctx, opts, "chunkSize", cfg.chunkSize);
    cfg.baseLevel  = jsGetInt(ctx, opts, "baseLevel", cfg.baseLevel);
    cfg.aoStrength = (float)jsGetProp(ctx, opts, "aoStrength", cfg.aoStrength);

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
    cfg.atlasColumns = jsGetInt(ctx, opts, "atlasColumns", cfg.atlasColumns);
    cfg.atlasRows    = jsGetInt(ctx, opts, "atlasRows", cfg.atlasRows);
    cfg.cliffCell    = jsGetInt(ctx, opts, "cliffCell", cfg.cliffCell);
    cfg.atlasInset   = (float)jsGetProp(ctx, opts, "atlasInset", cfg.atlasInset);

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
                    cfg.atlasWidth  = jsGetInt(ctx, opts, "atlasWidth", 0);
                    cfg.atlasHeight = jsGetInt(ctx, opts, "atlasHeight", 0);
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
            rule.id = (uint16_t)jsGetInt(ctx, e, "id", 0);
            rule.layer = jsGetInt(ctx, e, "layer", 0);

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
                cfg.overlays[i].opacity     = (float)jsGetProp(ctx, e, "opacity", 1.0);
                cfg.overlays[i].alphaCutoff = (float)jsGetProp(ctx, e, "alphaCutoff", 0.0);
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
            an.id  = (uint16_t)jsGetInt(ctx, e, "id", 0);
            an.fps = (float)jsGetProp(ctx, e, "fps", 4.0);
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
        style.roughness   = (float)jsGetProp(ctx, s, "roughness", style.roughness);
        style.metallic    = (float)jsGetProp(ctx, s, "metallic", style.metallic);
        style.doubleSided = jsGetInt(ctx, s, "doubleSided", 0) != 0;
        style.alphaCutoff = (float)jsGetProp(ctx, s, "alphaCutoff", style.alphaCutoff);
        style.castsShadow = jsGetInt(ctx, s, "castsShadow", 1) != 0;
        style.atlasCols   = jsGetInt(ctx, s, "atlasColumns", 1);
        style.atlasRows   = jsGetInt(ctx, s, "atlasRows", 1);

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
        p.yaw     = (float)jsGetProp(ctx, o, "yaw", 0.0);
        p.scale   = (float)jsGetProp(ctx, o, "scale", 1.0);
        p.yOffset = (float)jsGetProp(ctx, o, "yOffset", 0.0);
        p.offsetX = (float)jsGetProp(ctx, o, "offsetX", 0.0);
        p.offsetZ = (float)jsGetProp(ctx, o, "offsetZ", 0.0);
        p.variant = jsGetInt(ctx, o, "variant", 0);
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
    const float ox = cfg.origin.x, oz = cfg.origin.z;
    int blocked = 0;
    for (int y = 0; y < cfg.height; ++y) {
        for (int x = 0; x < cfg.width; ++x) {
            if (world->isWalkable(x, y, blockMask)) continue;
            brogameagent::AABB box;
            box.cx = ox + (x + 0.5f) * cs;
            box.cz = oz + (y + 0.5f) * cs;
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
        mask = (uint32_t)(int64_t)jsGetProp(ctx, argv[1], "blockMask", 0);
        padding = (float)jsGetProp(ctx, argv[1], "padding", 0);
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
        mask = (uint32_t)(int64_t)jsGetProp(ctx, argv[0], "blockMask", 0);
        padding = (float)jsGetProp(ctx, argv[0], "padding", 0);
    }
    const float cs = cfg.cellSize;
    const float minX = cfg.origin.x, minZ = cfg.origin.z;
    const float maxX = minX + cfg.width * cs, maxZ = minZ + cfg.height * cs;
    JSValue gridVal = createNavGridJS(ctx, minX, minZ, maxX, maxZ, cs);
    if (brogameagent::NavGrid* ng = navGridFromJS(ctx, gridVal))
        stampNavGrid(w->world.get(), ng, mask, padding);
    return gridVal;
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
        .method_raw("raycastCell", js_tile_raycastCell, 3)
        .method_raw("sampleHeight", js_tile_sampleHeight, 2)
        .method_raw("isWalkable", js_tile_isWalkable, 3)
        .method_raw("syncNavGrid", js_tile_syncNavGrid, 2)
        .method_raw("toNavGrid", js_tile_toNavGrid, 1)
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
