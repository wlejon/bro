#include "js/terrain_bindings.h"
#if BRO_WITH_3D  // modular-build feature gate

#include <qjsbind/qjsbind.h>

#include "scene/terrain_manager.h"
#include "scene/scene_graph.h"
#include "js/scene_bindings.h"

#include "util/log.h"

#include <cstring>
#include <string>
#include <unordered_set>
#include <vector>

namespace bro::js {

// -------------------------------------------------------------------------
// TerrainWrapper — opaque data attached to JS terrain objects
// -------------------------------------------------------------------------

struct TerrainWrapper {
    std::unique_ptr<scene::TerrainManager> manager;

    // The JS height provider, if one is installed. Held as a dup'd JSValue for
    // the wrapper's lifetime; the context outlives every DOM/scene object, so
    // freeing it in the destructor is safe.
    JSContext* cbCtx = nullptr;
    JSValue    heightSource = JS_UNDEFINED;
    bool       hasHeightSource = false;

    explicit TerrainWrapper(std::unique_ptr<scene::TerrainManager> mgr)
        : manager(std::move(mgr)) { allInstances().insert(this); }
    ~TerrainWrapper() {
        if (hasHeightSource && cbCtx) JS_FreeValue(cbCtx, heightSource);
        allInstances().erase(this);
    }

    static std::unordered_set<TerrainWrapper*>& allInstances() {
        static std::unordered_set<TerrainWrapper*> s;
        return s;
    }
};

using TW = TerrainWrapper;

// -------------------------------------------------------------------------
// Parse TerrainConfig from JS options object
// -------------------------------------------------------------------------

static scene::TerrainConfig parseConfig(JSContext* ctx, JSValueConst opts) {
    scene::TerrainConfig cfg;

    // chunkSize: [x, y, z] array
    JSValue cs = JS_GetPropertyStr(ctx, opts, "chunkSize");
    if (JS_IsArray(cs)) {
        JSValue cx = JS_GetPropertyUint32(ctx, cs, 0);
        JSValue cy = JS_GetPropertyUint32(ctx, cs, 1);
        JSValue cz = JS_GetPropertyUint32(ctx, cs, 2);
        double vx = 64, vy = 48, vz = 64;
        JS_ToFloat64(ctx, &vx, cx); JS_ToFloat64(ctx, &vy, cy); JS_ToFloat64(ctx, &vz, cz);
        cfg.chunkSizeX = (int)vx; cfg.chunkSizeY = (int)vy; cfg.chunkSizeZ = (int)vz;
        JS_FreeValue(ctx, cx); JS_FreeValue(ctx, cy); JS_FreeValue(ctx, cz);
    }
    JS_FreeValue(ctx, cs);

    cfg.cellSize = (float)qjsbind::get_prop_number(ctx, opts, "cellSize", cfg.cellSize);
    cfg.loadRadius = qjsbind::get_prop_int(ctx, opts, "loadRadius", cfg.loadRadius);
    cfg.unloadRadius = qjsbind::get_prop_int(ctx, opts, "unloadRadius", cfg.unloadRadius);
    cfg.maxLoadsPerUpdate = qjsbind::get_prop_int(ctx, opts, "maxLoadsPerUpdate", cfg.maxLoadsPerUpdate);
    cfg.seed = qjsbind::get_prop_int(ctx, opts, "seed", cfg.seed);
    cfg.baseHeight = qjsbind::get_prop_int(ctx, opts, "baseHeight", cfg.baseHeight);
    cfg.heightAmplitude = qjsbind::get_prop_int(ctx, opts, "heightAmplitude", cfg.heightAmplitude);
    cfg.seaLevel = qjsbind::get_prop_int(ctx, opts, "seaLevel", cfg.seaLevel);
    cfg.meshMode = qjsbind::get_prop_int(ctx, opts, "meshMode", cfg.meshMode);
    cfg.terraceStep = (float)qjsbind::get_prop_number(ctx, opts, "terraceStep", cfg.terraceStep);
    cfg.continentFrequency = (float)qjsbind::get_prop_number(ctx, opts, "continentFrequency", cfg.continentFrequency);
    cfg.continentMin = (float)qjsbind::get_prop_number(ctx, opts, "continentMin", cfg.continentMin);
    cfg.continentMax = (float)qjsbind::get_prop_number(ctx, opts, "continentMax", cfg.continentMax);
    cfg.mountainFrequency = (float)qjsbind::get_prop_number(ctx, opts, "mountainFrequency", cfg.mountainFrequency);
    cfg.mountainAmplitude = (float)qjsbind::get_prop_number(ctx, opts, "mountainAmplitude", cfg.mountainAmplitude);
    cfg.mountainOctaves = qjsbind::get_prop_int(ctx, opts, "mountainOctaves", cfg.mountainOctaves);
    cfg.lodLevelCount = qjsbind::get_prop_int(ctx, opts, "lodLevels", cfg.lodLevelCount);
    cfg.lodScaleFactor = qjsbind::get_prop_int(ctx, opts, "lodScaleFactor", cfg.lodScaleFactor);
    cfg.planetRadius = (float)qjsbind::get_prop_number(ctx, opts, "planetRadius", cfg.planetRadius);

    // origin: [x, y, z] — world-space position of this terrain
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

    // noise: { frequency, octaves, gain, lacunarity }
    JSValue noise = JS_GetPropertyStr(ctx, opts, "noise");
    if (JS_IsObject(noise)) {
        cfg.noiseFrequency = (float)qjsbind::get_prop_number(ctx, noise, "frequency", cfg.noiseFrequency);
        cfg.noiseOctaves = qjsbind::get_prop_int(ctx, noise, "octaves", cfg.noiseOctaves);
        cfg.noiseGain = (float)qjsbind::get_prop_number(ctx, noise, "gain", cfg.noiseGain);
        cfg.noiseLacunarity = (float)qjsbind::get_prop_number(ctx, noise, "lacunarity", cfg.noiseLacunarity);
    }
    JS_FreeValue(ctx, noise);

    // palette: flat Float32Array or plain Array of numbers
    JSValue pal = JS_GetPropertyStr(ctx, opts, "palette");
    if (!JS_IsUndefined(pal)) {
        // Try typed array first.
        size_t offset = 0, byteLen = 0, bpe = 0;
        JSValue abuf = JS_GetTypedArrayBuffer(ctx, pal, &offset, &byteLen, &bpe);
        if (!JS_IsException(abuf)) {
            size_t abufLen = 0;
            uint8_t* raw = JS_GetArrayBuffer(ctx, &abufLen, abuf);
            if (raw && byteLen > 0) {
                size_t count = byteLen / sizeof(float);
                const float* fp = reinterpret_cast<const float*>(raw + offset);
                cfg.palette.assign(fp, fp + count);
            }
            JS_FreeValue(ctx, abuf);
        } else {
            JS_FreeValue(ctx, abuf);
            // Fall back to plain JS array.
            JSValue lenVal = JS_GetPropertyStr(ctx, pal, "length");
            int32_t len = 0;
            JS_ToInt32(ctx, &len, lenVal);
            JS_FreeValue(ctx, lenVal);
            if (len > 0) {
                cfg.palette.resize(len);
                for (int32_t i = 0; i < len; i++) {
                    JSValue el = JS_GetPropertyUint32(ctx, pal, i);
                    double v = 0;
                    JS_ToFloat64(ctx, &v, el);
                    cfg.palette[i] = (float)v;
                    JS_FreeValue(ctx, el);
                }
            }
        }
    }
    JS_FreeValue(ctx, pal);

    return cfg;
}

// -------------------------------------------------------------------------
// Helper: parse [x, y, z] array
// -------------------------------------------------------------------------

static bool parseVec3(JSContext* ctx, JSValueConst val, bromath::Vec3& out) {
    if (!JS_IsArray(val)) return false;
    JSValue ex = JS_GetPropertyUint32(ctx, val, 0);
    JSValue ey = JS_GetPropertyUint32(ctx, val, 1);
    JSValue ez = JS_GetPropertyUint32(ctx, val, 2);
    double x = 0, y = 0, z = 0;
    bool ok = !JS_ToFloat64(ctx, &x, ex)
           && !JS_ToFloat64(ctx, &y, ey)
           && !JS_ToFloat64(ctx, &z, ez);
    JS_FreeValue(ctx, ex); JS_FreeValue(ctx, ey); JS_FreeValue(ctx, ez);
    if (!ok) return false;
    out = {(float)x, (float)y, (float)z};
    return true;
}

// -------------------------------------------------------------------------
// Complex methods needing raw argc/argv
// -------------------------------------------------------------------------

// terrain.raycast(origin, direction, maxDist)
static JSValue js_terrain_raycast(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* w = qjsbind::unwrap<TW>(ctx, this_val);
    if (!w || !w->manager) return JS_NULL;
    if (argc < 2) return JS_ThrowTypeError(ctx, "terrain.raycast(origin, dir[, maxDist])");

    bromath::Vec3 origin, dir;
    if (!parseVec3(ctx, argv[0], origin)) return JS_ThrowTypeError(ctx, "origin must be [x,y,z]");
    if (!parseVec3(ctx, argv[1], dir)) return JS_ThrowTypeError(ctx, "direction must be [x,y,z]");

    double maxDist = 0;
    if (argc >= 3) JS_ToFloat64(ctx, &maxDist, argv[2]);

    auto hit = w->manager->raycast(origin, dir, (float)maxDist);
    if (!hit.hit) return JS_NULL;

    JSValue out = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, out, "hit", JS_TRUE);
    JS_SetPropertyStr(ctx, out, "distance", JS_NewFloat64(ctx, hit.distance));

    JSValue position = JS_NewArray(ctx);
    JS_SetPropertyUint32(ctx, position, 0, JS_NewFloat64(ctx, hit.worldPos[0]));
    JS_SetPropertyUint32(ctx, position, 1, JS_NewFloat64(ctx, hit.worldPos[1]));
    JS_SetPropertyUint32(ctx, position, 2, JS_NewFloat64(ctx, hit.worldPos[2]));
    JS_SetPropertyStr(ctx, out, "position", position);

    JSValue normal = JS_NewArray(ctx);
    JS_SetPropertyUint32(ctx, normal, 0, JS_NewFloat64(ctx, hit.normal[0]));
    JS_SetPropertyUint32(ctx, normal, 1, JS_NewFloat64(ctx, hit.normal[1]));
    JS_SetPropertyUint32(ctx, normal, 2, JS_NewFloat64(ctx, hit.normal[2]));
    JS_SetPropertyStr(ctx, out, "normal", normal);

    JSValue chunk = JS_NewArray(ctx);
    JS_SetPropertyUint32(ctx, chunk, 0, JS_NewInt32(ctx, hit.chunk.x));
    JS_SetPropertyUint32(ctx, chunk, 1, JS_NewInt32(ctx, hit.chunk.z));
    JS_SetPropertyStr(ctx, out, "chunk", chunk);

    JSValue voxel = JS_NewArray(ctx);
    JS_SetPropertyUint32(ctx, voxel, 0, JS_NewInt32(ctx, hit.localX));
    JS_SetPropertyUint32(ctx, voxel, 1, JS_NewInt32(ctx, hit.localY));
    JS_SetPropertyUint32(ctx, voxel, 2, JS_NewInt32(ctx, hit.localZ));
    JS_SetPropertyStr(ctx, out, "voxel", voxel);

    JS_SetPropertyStr(ctx, out, "material", JS_NewInt32(ctx, hit.material));

    return out;
}

// terrain.configure(opts) — reconfigure and rebuild
static JSValue js_terrain_configure(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* w = qjsbind::unwrap<TW>(ctx, this_val);
    if (!w || !w->manager || argc < 1 || !JS_IsObject(argv[0])) return JS_UNDEFINED;

    auto cfg = parseConfig(ctx, argv[0]);
    w->manager->configure(cfg);
    return JS_UNDEFINED;
}

// -------------------------------------------------------------------------
// Factory: scene.createTerrain(opts) — called from scene_bindings.cpp
// -------------------------------------------------------------------------

JSValue createTerrainJS(JSContext* ctx, scene::SceneGraph* graph, JSValueConst opts) {
    if (!graph) return JS_ThrowTypeError(ctx, "createTerrain: no scene graph");

    auto cfg = parseConfig(ctx, opts);
    auto mgr = std::make_unique<scene::TerrainManager>(*graph);
    mgr->configure(cfg);

    return qjsbind::wrap<TW>(ctx, new TW(std::move(mgr)));
}

// -------------------------------------------------------------------------
// AI ground-follow height probe (see terrain_bindings.h)
// -------------------------------------------------------------------------

void* terrainHandleFromJS(JSContext* ctx, JSValueConst v) {
    if (!JS_IsObject(v)) return nullptr;
    return qjsbind::unwrap<TW>(ctx, v);
}

bool terrainSampleHeight(void* handle, float x, float z,
                         float rayStartY, float rayLength, float& outY) {
    auto* tw = static_cast<TW*>(handle);
    if (!tw || !TW::allInstances().count(tw) || !tw->manager) return false;
    auto hit = tw->manager->raycast({x, rayStartY, z}, {0.0f, -1.0f, 0.0f}, rayLength);
    if (!hit.hit) return false;
    outY = hit.worldPos[1];
    return true;
}

// -------------------------------------------------------------------------
// Install / Cleanup
// -------------------------------------------------------------------------

// terrain.setHeightSource(fn | null)
//
// fn(cx, cz, lod, paddedW, paddedH, cellSize, worldX0, worldZ0) must return a
// Float32Array of paddedW*paddedH heights (row-major, z-major, absolute world Y)
// or null/undefined to fall back to the built-in noise for that chunk.
//
// This runs INSIDE terrain.update(), on the JS thread, once per newly loaded
// chunk — so it must be cheap. The intended shape is to sample an already
// resident tile (see docs/worldgen-api.js); anything that generates on demand
// here will stall the frame.
static JSValue js_terrain_setHeightSource(JSContext* ctx, JSValueConst this_val,
                                          int argc, JSValueConst* argv) {
    auto* self = qjsbind::unwrap<TW>(ctx, this_val);
    if (!self || !self->manager) return JS_UNDEFINED;

    if (self->hasHeightSource) {
        JS_FreeValue(self->cbCtx, self->heightSource);
        self->hasHeightSource = false;
        self->heightSource = JS_UNDEFINED;
    }
    if (argc < 1 || !JS_IsFunction(ctx, argv[0])) {
        self->manager->setHeightSource(nullptr);
        return JS_UNDEFINED;
    }

    self->cbCtx = ctx;
    self->heightSource = JS_DupValue(ctx, argv[0]);
    self->hasHeightSource = true;

    self->manager->setHeightSource(
        [self](int cx, int cz, int lod, float* padded, int paddedW, int paddedH,
               float cellSize, float worldX0, float worldZ0) -> bool {
            JSContext* c = self->cbCtx;
            JSValue args[8] = {
                JS_NewInt32(c, cx),          JS_NewInt32(c, cz),
                JS_NewInt32(c, lod),         JS_NewInt32(c, paddedW),
                JS_NewInt32(c, paddedH),     JS_NewFloat64(c, cellSize),
                JS_NewFloat64(c, worldX0),   JS_NewFloat64(c, worldZ0),
            };
            JSValue r = JS_Call(c, self->heightSource, JS_UNDEFINED, 8, args);
            for (JSValue& a : args) JS_FreeValue(c, a);

            if (JS_IsException(r)) {
                // Swallowing the exception here would silently substitute noise
                // for a provider the app believes is running; report it and let
                // the chunk fall back visibly.
                JSValue e = JS_GetException(c);
                const char* msg = JS_ToCString(c, e);
                if (msg) {
                    LOG_ERROR("terrain heightSource threw: %s", msg);
                    JS_FreeCString(c, msg);
                }
                JS_FreeValue(c, e);
                JS_FreeValue(c, r);
                return false;
            }
            if (!JS_IsObject(r)) { JS_FreeValue(c, r); return false; }

            size_t byteOff = 0, viewLen = 0;
            JSValue abuf = JS_GetTypedArrayBuffer(c, r, &byteOff, &viewLen, nullptr);
            if (JS_IsException(abuf)) {
                JS_FreeValue(c, JS_GetException(c));
                JS_FreeValue(c, r);
                return false;
            }
            size_t abufLen = 0;
            uint8_t* ptr = JS_GetArrayBuffer(c, &abufLen, abuf);

            const size_t want = static_cast<size_t>(paddedW) * paddedH * sizeof(float);
            bool ok = ptr && viewLen >= want;
            if (ok) {
                std::memcpy(padded, ptr + byteOff, want);
            } else if (ptr) {
                // A short buffer would leave the tail of the chunk as whatever
                // was in the vector — stale heights from a previously unloaded
                // chunk. Refuse rather than render that.
                LOG_ERROR("terrain heightSource returned %zu bytes, expected %zu "
                          "(%dx%d padded floats) - falling back to noise",
                          viewLen, want, paddedW, paddedH);
            }
            JS_FreeValue(c, abuf);
            JS_FreeValue(c, r);
            return ok;
        });
    return JS_UNDEFINED;
}

void TerrainBindings::install(JSContext* ctx) {
    qjsbind::Class<TW>(ctx, "Terrain")
        // No constructor — created via scene.createTerrain()
        .method("update", [](TW* self, JSContext*, double x, double y, double z) -> int {
            if (!self->manager) return 0;
            return self->manager->update((float)x, (float)y, (float)z);
        })
        .method_raw("raycast", js_terrain_raycast, 2)
        .method("setVoxel", [](TW* self, double wx, double wy, double wz, int mat) -> bool {
            if (!self->manager) return false;
            return self->manager->setVoxel((float)wx, (float)wy, (float)wz, (uint8_t)mat);
        })
        .method("getVoxel", [](TW* self, double wx, double wy, double wz) -> int {
            if (!self->manager) return 0;
            return (int)self->manager->getVoxel((float)wx, (float)wy, (float)wz);
        })
        .method("rebuild", [](TW* self) {
            if (self->manager) self->manager->rebuildDirty();
        })
        .method_raw("configure", js_terrain_configure, 1)
        .method_raw("setHeightSource", js_terrain_setHeightSource, 1)
        .method("destroy", [](TW* self) {
            if (self->manager) {
                self->manager->clear();
                self->manager.reset();
            }
        })
        .get("chunkCount", [](TW* self) -> int {
            return self->manager ? self->manager->chunkCount() : 0;
        })
        .get("triangleCount", [](TW* self) -> int {
            return self->manager ? self->manager->totalTriangles() : 0;
        })
        .get("vertexCount", [](TW* self) -> int {
            return self->manager ? self->manager->totalVertices() : 0;
        })
        .get("farDistance", [](TW* self) -> double {
            return self->manager ? self->manager->farDistance() : 1000.0;
        })
        .get("planetRadius", [](TW* self) -> double {
            return self->manager ? (double)self->manager->config().planetRadius : 0.0;
        })
        .get("origin", [](TW* self, JSContext* ctx) -> JSValue {
            if (!self->manager) return JS_NULL;
            auto& o = self->manager->config().origin;
            JSValue arr = JS_NewArray(ctx);
            JS_SetPropertyUint32(ctx, arr, 0, JS_NewFloat64(ctx, o.x));
            JS_SetPropertyUint32(ctx, arr, 1, JS_NewFloat64(ctx, o.y));
            JS_SetPropertyUint32(ctx, arr, 2, JS_NewFloat64(ctx, o.z));
            return arr;
        });
}

void TerrainBindings::cleanup(JSContext*) {
    // Clear all live TerrainManagers before scene graphs are destroyed,
    // so their destructors don't access dangling SceneGraph references.
    for (auto* tw : TW::allInstances()) {
        if (tw->manager) {
            tw->manager->clear();
            tw->manager.reset();
        }
    }
}

} // namespace bro::js

#endif  // BRO_WITH_3D
