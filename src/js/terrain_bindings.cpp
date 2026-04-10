#include "js/terrain_bindings.h"
#include "scene/terrain_manager.h"
#include "scene/scene_graph.h"
#include "js/scene_bindings.h"

#include <string>
#include <vector>

extern "C" {
#include "quickjs.h"
}

namespace bro::js {

// -------------------------------------------------------------------------
// Helpers (local copies — same pattern as scene_bindings.cpp)
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

// -------------------------------------------------------------------------
// TerrainWrapper — opaque data attached to JS terrain objects
// -------------------------------------------------------------------------

static JSClassID js_terrain_class_id = 0;

struct TerrainWrapper {
    std::unique_ptr<scene::TerrainManager> manager;
};

static void terrain_finalizer(JSRuntime*, JSValue val) {
    auto* w = static_cast<TerrainWrapper*>(JS_GetOpaque(val, js_terrain_class_id));
    delete w;
}

static JSClassDef js_terrain_class = { "Terrain", terrain_finalizer };

static TerrainWrapper* getTerrain(JSValueConst val) {
    return static_cast<TerrainWrapper*>(JS_GetOpaque(val, js_terrain_class_id));
}

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

    cfg.cellSize = (float)jsGetProp(ctx, opts, "cellSize", cfg.cellSize);
    cfg.loadRadius = jsGetInt(ctx, opts, "loadRadius", cfg.loadRadius);
    cfg.unloadRadius = jsGetInt(ctx, opts, "unloadRadius", cfg.unloadRadius);
    cfg.maxLoadsPerUpdate = jsGetInt(ctx, opts, "maxLoadsPerUpdate", cfg.maxLoadsPerUpdate);
    cfg.seed = jsGetInt(ctx, opts, "seed", cfg.seed);
    cfg.baseHeight = jsGetInt(ctx, opts, "baseHeight", cfg.baseHeight);
    cfg.heightAmplitude = jsGetInt(ctx, opts, "heightAmplitude", cfg.heightAmplitude);
    cfg.seaLevel = jsGetInt(ctx, opts, "seaLevel", cfg.seaLevel);
    cfg.meshMode = jsGetInt(ctx, opts, "meshMode", cfg.meshMode);
    cfg.terraceStep = (float)jsGetProp(ctx, opts, "terraceStep", cfg.terraceStep);
    cfg.continentFrequency = (float)jsGetProp(ctx, opts, "continentFrequency", cfg.continentFrequency);
    cfg.continentMin = (float)jsGetProp(ctx, opts, "continentMin", cfg.continentMin);
    cfg.continentMax = (float)jsGetProp(ctx, opts, "continentMax", cfg.continentMax);
    cfg.mountainFrequency = (float)jsGetProp(ctx, opts, "mountainFrequency", cfg.mountainFrequency);
    cfg.mountainAmplitude = (float)jsGetProp(ctx, opts, "mountainAmplitude", cfg.mountainAmplitude);
    cfg.mountainOctaves = jsGetInt(ctx, opts, "mountainOctaves", cfg.mountainOctaves);
    cfg.lodLevelCount = jsGetInt(ctx, opts, "lodLevels", cfg.lodLevelCount);
    cfg.lodScaleFactor = jsGetInt(ctx, opts, "lodScaleFactor", cfg.lodScaleFactor);
    cfg.planetRadius = (float)jsGetProp(ctx, opts, "planetRadius", cfg.planetRadius);

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
        cfg.noiseFrequency = (float)jsGetProp(ctx, noise, "frequency", cfg.noiseFrequency);
        cfg.noiseOctaves = jsGetInt(ctx, noise, "octaves", cfg.noiseOctaves);
        cfg.noiseGain = (float)jsGetProp(ctx, noise, "gain", cfg.noiseGain);
        cfg.noiseLacunarity = (float)jsGetProp(ctx, noise, "lacunarity", cfg.noiseLacunarity);
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

static bool parseVec3(JSContext* ctx, JSValueConst val, scene::Vec3& out) {
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
// Terrain methods
// -------------------------------------------------------------------------

// terrain.update(camX, camY, camZ)
static JSValue js_terrain_update(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* w = getTerrain(this_val);
    if (!w || !w->manager) return JS_UNDEFINED;
    if (argc < 3) return JS_ThrowTypeError(ctx, "terrain.update(camX, camY, camZ)");

    double x = 0, y = 0, z = 0;
    JS_ToFloat64(ctx, &x, argv[0]);
    JS_ToFloat64(ctx, &y, argv[1]);
    JS_ToFloat64(ctx, &z, argv[2]);

    int loaded = w->manager->update((float)x, (float)y, (float)z);
    return JS_NewInt32(ctx, loaded);
}

// terrain.raycast(origin, direction, maxDist)
static JSValue js_terrain_raycast(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* w = getTerrain(this_val);
    if (!w || !w->manager) return JS_NULL;
    if (argc < 2) return JS_ThrowTypeError(ctx, "terrain.raycast(origin, dir[, maxDist])");

    scene::Vec3 origin, dir;
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

// terrain.setVoxel(wx, wy, wz, material)
static JSValue js_terrain_setVoxel(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* w = getTerrain(this_val);
    if (!w || !w->manager) return JS_FALSE;
    if (argc < 4) return JS_ThrowTypeError(ctx, "terrain.setVoxel(wx, wy, wz, material)");

    double wx = 0, wy = 0, wz = 0;
    int32_t mat = 0;
    JS_ToFloat64(ctx, &wx, argv[0]);
    JS_ToFloat64(ctx, &wy, argv[1]);
    JS_ToFloat64(ctx, &wz, argv[2]);
    JS_ToInt32(ctx, &mat, argv[3]);

    bool ok = w->manager->setVoxel((float)wx, (float)wy, (float)wz, (uint8_t)mat);
    return ok ? JS_TRUE : JS_FALSE;
}

// terrain.getVoxel(wx, wy, wz)
static JSValue js_terrain_getVoxel(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* w = getTerrain(this_val);
    if (!w || !w->manager) return JS_NewInt32(ctx, 0);
    if (argc < 3) return JS_ThrowTypeError(ctx, "terrain.getVoxel(wx, wy, wz)");

    double wx = 0, wy = 0, wz = 0;
    JS_ToFloat64(ctx, &wx, argv[0]);
    JS_ToFloat64(ctx, &wy, argv[1]);
    JS_ToFloat64(ctx, &wz, argv[2]);

    uint8_t mat = w->manager->getVoxel((float)wx, (float)wy, (float)wz);
    return JS_NewInt32(ctx, mat);
}

// terrain.rebuild()
static JSValue js_terrain_rebuild(JSContext*, JSValueConst this_val, int, JSValueConst*) {
    auto* w = getTerrain(this_val);
    if (w && w->manager) w->manager->rebuildDirty();
    return JS_UNDEFINED;
}

// terrain.configure(opts) — reconfigure and rebuild
static JSValue js_terrain_configure(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* w = getTerrain(this_val);
    if (!w || !w->manager || argc < 1 || !JS_IsObject(argv[0])) return JS_UNDEFINED;

    auto cfg = parseConfig(ctx, argv[0]);
    w->manager->configure(cfg);
    return JS_UNDEFINED;
}

// terrain.destroy()
static JSValue js_terrain_destroy(JSContext*, JSValueConst this_val, int, JSValueConst*) {
    auto* w = getTerrain(this_val);
    if (w && w->manager) {
        w->manager->clear();
        w->manager.reset();
    }
    return JS_UNDEFINED;
}

// -------------------------------------------------------------------------
// Terrain property getters
// -------------------------------------------------------------------------

static JSValue js_terrain_get_chunkCount(JSContext* ctx, JSValueConst this_val) {
    auto* w = getTerrain(this_val);
    return (w && w->manager) ? JS_NewInt32(ctx, w->manager->chunkCount()) : JS_NewInt32(ctx, 0);
}

static JSValue js_terrain_get_triangleCount(JSContext* ctx, JSValueConst this_val) {
    auto* w = getTerrain(this_val);
    return (w && w->manager) ? JS_NewInt32(ctx, w->manager->totalTriangles()) : JS_NewInt32(ctx, 0);
}

static JSValue js_terrain_get_vertexCount(JSContext* ctx, JSValueConst this_val) {
    auto* w = getTerrain(this_val);
    return (w && w->manager) ? JS_NewInt32(ctx, w->manager->totalVertices()) : JS_NewInt32(ctx, 0);
}

static JSValue js_terrain_get_farDistance(JSContext* ctx, JSValueConst this_val) {
    auto* w = getTerrain(this_val);
    return (w && w->manager) ? JS_NewFloat64(ctx, w->manager->farDistance()) : JS_NewFloat64(ctx, 1000);
}

static JSValue js_terrain_get_planetRadius(JSContext* ctx, JSValueConst this_val) {
    auto* w = getTerrain(this_val);
    return (w && w->manager) ? JS_NewFloat64(ctx, w->manager->config().planetRadius) : JS_NewFloat64(ctx, 0);
}

static JSValue js_terrain_get_origin(JSContext* ctx, JSValueConst this_val) {
    auto* w = getTerrain(this_val);
    if (!w || !w->manager) return JS_NULL;
    auto& o = w->manager->config().origin;
    JSValue arr = JS_NewArray(ctx);
    JS_SetPropertyUint32(ctx, arr, 0, JS_NewFloat64(ctx, o.x));
    JS_SetPropertyUint32(ctx, arr, 1, JS_NewFloat64(ctx, o.y));
    JS_SetPropertyUint32(ctx, arr, 2, JS_NewFloat64(ctx, o.z));
    return arr;
}

// -------------------------------------------------------------------------
// Terrain prototype
// -------------------------------------------------------------------------

static const JSCFunctionListEntry js_terrain_proto[] = {
    JS_CFUNC_DEF("update", 3, js_terrain_update),
    JS_CFUNC_DEF("raycast", 2, js_terrain_raycast),
    JS_CFUNC_DEF("setVoxel", 4, js_terrain_setVoxel),
    JS_CFUNC_DEF("getVoxel", 3, js_terrain_getVoxel),
    JS_CFUNC_DEF("rebuild", 0, js_terrain_rebuild),
    JS_CFUNC_DEF("configure", 1, js_terrain_configure),
    JS_CFUNC_DEF("destroy", 0, js_terrain_destroy),
    JS_CGETSET_DEF("chunkCount", js_terrain_get_chunkCount, nullptr),
    JS_CGETSET_DEF("triangleCount", js_terrain_get_triangleCount, nullptr),
    JS_CGETSET_DEF("vertexCount", js_terrain_get_vertexCount, nullptr),
    JS_CGETSET_DEF("farDistance", js_terrain_get_farDistance, nullptr),
    JS_CGETSET_DEF("planetRadius", js_terrain_get_planetRadius, nullptr),
    JS_CGETSET_DEF("origin", js_terrain_get_origin, nullptr),
};

// -------------------------------------------------------------------------
// Factory: scene.createTerrain(opts) — called from scene_bindings.cpp
// -------------------------------------------------------------------------

static JSValue s_terrain_proto = JS_UNDEFINED;

// This function is exposed via the header and called from scene_bindings
// when `scene.createTerrain(opts)` is invoked from JS.
JSValue createTerrainJS(JSContext* ctx, scene::SceneGraph* graph, JSValueConst opts) {
    if (!graph) return JS_ThrowTypeError(ctx, "createTerrain: no scene graph");

    auto cfg = parseConfig(ctx, opts);
    auto mgr = std::make_unique<scene::TerrainManager>(*graph);
    mgr->configure(cfg);

    JSValue obj = JS_NewObjectClass(ctx, js_terrain_class_id);
    JS_SetPrototype(ctx, obj, JS_DupValue(ctx, s_terrain_proto));
    JS_SetOpaque(obj, new TerrainWrapper{std::move(mgr)});
    return obj;
}

// -------------------------------------------------------------------------
// Install / Cleanup
// -------------------------------------------------------------------------

void TerrainBindings::install(JSContext* ctx) {
    JS_NewClassID(JS_GetRuntime(ctx), &js_terrain_class_id);
    JS_NewClass(JS_GetRuntime(ctx), js_terrain_class_id, &js_terrain_class);

    s_terrain_proto = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, s_terrain_proto,
                               js_terrain_proto,
                               sizeof(js_terrain_proto) / sizeof(js_terrain_proto[0]));
}

void TerrainBindings::cleanup(JSContext* ctx) {
    JS_FreeValue(ctx, s_terrain_proto);
    s_terrain_proto = JS_UNDEFINED;
}

} // namespace bro::js
