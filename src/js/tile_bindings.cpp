#include "js/tile_bindings.h"

#include <qjsbind/qjsbind.h>

#include "scene/tile_world.h"
#include "scene/scene_graph.h"
#include "util/asset_mounts.h"
#include "broimage/decode.h"

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
        .method("setOrigin", [](TWld* self, double x, double y, double z) {
            if (self->world) self->world->setOrigin((float)x, (float)y, (float)z);
        })
        .method("advance", [](TWld* self, double dtMs) -> bool {
            return self->world ? self->world->advance(dtMs) : false;
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
