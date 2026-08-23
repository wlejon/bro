#if BRO_WITH_3D

#include "js/terrain_bindings.h"
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

struct TerrainWrapper {
    std::unique_ptr<scene::TerrainManager> manager;
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

static scene::TerrainConfig parseConfig(JSContext* ctx, JSValueConst opts) {
    scene::TerrainConfig cfg;
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
    JSValue noise = JS_GetPropertyStr(ctx, opts, "noise");
    if (JS_IsObject(noise)) {
        cfg.noiseFrequency = (float)qjsbind::get_prop_number(ctx, noise, "frequency", cfg.noiseFrequency);
        cfg.noiseOctaves = qjsbind::get_prop_int(ctx, noise, "octaves", cfg.noiseOctaves);
        cfg.noiseGain = (float)qjsbind::get_prop_number(ctx, noise, "gain", cfg.noiseGain);
        cfg.noiseLacunarity = (float)qjsbind::get_prop_number(ctx, noise, "lacunarity", cfg.noiseLacunarity);
    }
    JS_FreeValue(ctx, noise);
    JSValue pal = JS_GetPropertyStr(ctx, opts, "palette");
    if (!JS_IsUndefined(pal)) {
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
            JSValue lenVal = JS_GetPropertyStr(ctx, pal, "length");
            int32_t len = 0; JS_ToInt32(ctx, &len, lenVal); JS_FreeValue(ctx, lenVal);
            if (len > 0) {
                cfg.palette.resize(len);
                for (int32_t i = 0; i < len; i++) {
                    JSValue el = JS_GetPropertyUint32(ctx, pal, i);
                    double v = 0; JS_ToFloat64(ctx, &v, el);
                    cfg.palette[i] = (float)v;
                    JS_FreeValue(ctx, el);
                }
            }
        }
    }
    JS_FreeValue(ctx, pal);
    return cfg;
}

static JSValue js_terrain_raycast(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* self = qjsbind::unwrap<TW>(ctx, this_val);
    if (!self || !self->manager || argc < 2) return JS_NULL;
    bromath::Vec3 origin, dir;
    JSValue o = argv[0], d = argv[1];
    if (JS_IsArray(o)) {
        JSValue x = JS_GetPropertyUint32(ctx, o, 0), y = JS_GetPropertyUint32(ctx, o, 1), z = JS_GetPropertyUint32(ctx, o, 2);
        double vx = 0, vy = 0, vz = 0;
        JS_ToFloat64(ctx, &vx, x); JS_ToFloat64(ctx, &vy, y); JS_ToFloat64(ctx, &vz, z);
        origin = {(float)vx, (float)vy, (float)vz};
        JS_FreeValue(ctx, x); JS_FreeValue(ctx, y); JS_FreeValue(ctx, z);
    }
    if (JS_IsArray(d)) {
        JSValue x = JS_GetPropertyUint32(ctx, d, 0), y = JS_GetPropertyUint32(ctx, d, 1), z = JS_GetPropertyUint32(ctx, d, 2);
        double vx = 0, vy = 0, vz = 0;
        JS_ToFloat64(ctx, &vx, x); JS_ToFloat64(ctx, &vy, y); JS_ToFloat64(ctx, &vz, z);
        dir = {(float)vx, (float)vy, (float)vz};
        JS_FreeValue(ctx, x); JS_FreeValue(ctx, y); JS_FreeValue(ctx, z);
    }
    float maxDist = argc > 2 ? (float)qjsbind::Convert<double>::from_js(ctx, argv[2]) : 1000.0f;
    auto hit = self->manager->raycast(origin, dir, maxDist);
    if (!hit.hit) return JS_NULL;
    JSValue res = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, res, "hit", JS_NewBool(ctx, true));
    JS_SetPropertyStr(ctx, res, "distance", JS_NewFloat64(ctx, hit.distance));
    JSValue pos = JS_NewArray(ctx);
    JS_SetPropertyUint32(ctx, pos, 0, JS_NewFloat64(ctx, hit.worldPos[0]));
    JS_SetPropertyUint32(ctx, pos, 1, JS_NewFloat64(ctx, hit.worldPos[1]));
    JS_SetPropertyUint32(ctx, pos, 2, JS_NewFloat64(ctx, hit.worldPos[2]));
    JS_SetPropertyStr(ctx, res, "position", pos);
    JSValue norm = JS_NewArray(ctx);
    JS_SetPropertyUint32(ctx, norm, 0, JS_NewFloat64(ctx, hit.normal[0]));
    JS_SetPropertyUint32(ctx, norm, 1, JS_NewFloat64(ctx, hit.normal[1]));
    JS_SetPropertyUint32(ctx, norm, 2, JS_NewFloat64(ctx, hit.normal[2]));
    JS_SetPropertyStr(ctx, res, "normal", norm);
    JSValue chunk = JS_NewArray(ctx);
    JS_SetPropertyUint32(ctx, chunk, 0, JS_NewInt32(ctx, hit.chunk.x));
    JS_SetPropertyUint32(ctx, chunk, 1, JS_NewInt32(ctx, hit.chunk.z));
    JS_SetPropertyStr(ctx, res, "chunk", chunk);
    JSValue vox = JS_NewArray(ctx);
    JS_SetPropertyUint32(ctx, vox, 0, JS_NewInt32(ctx, hit.localX));
    JS_SetPropertyUint32(ctx, vox, 1, JS_NewInt32(ctx, hit.localY));
    JS_SetPropertyUint32(ctx, vox, 2, JS_NewInt32(ctx, hit.localZ));
    JS_SetPropertyStr(ctx, res, "voxel", vox);
    JS_SetPropertyStr(ctx, res, "material", JS_NewInt32(ctx, hit.material));
    return res;
}

static JSValue js_terrain_configure(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* self = qjsbind::unwrap<TW>(ctx, this_val);
    if (!self || !self->manager || argc < 1 || !JS_IsObject(argv[0])) return JS_UNDEFINED;
    self->manager->configure(parseConfig(ctx, argv[0]));
    return JS_UNDEFINED;
}

static JSValue js_terrain_invalidateRegion(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* self = qjsbind::unwrap<TW>(ctx, this_val);
    if (!self || !self->manager) return JS_UNDEFINED;
    if (argc < 4) return JS_ThrowTypeError(ctx, "invalidateRegion(x0, z0, x1, z1) needs 4 numbers");
    double v[4];
    for (int i = 0; i < 4; i++) {
        if (JS_ToFloat64(ctx, &v[i], argv[i]) < 0) return JS_EXCEPTION;
    }
    self->manager->invalidateRegion(static_cast<float>(v[0]), static_cast<float>(v[1]),
                                    static_cast<float>(v[2]), static_cast<float>(v[3]));
    return JS_UNDEFINED;
}

static JSValue js_terrain_setHeightSource(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
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

JSValue createTerrainJS(JSContext* ctx, scene::SceneGraph* graph, JSValueConst opts) {
    if (!graph) return JS_NULL;
    auto mgr = std::make_unique<scene::TerrainManager>(*graph);
    mgr->configure(parseConfig(ctx, opts));
    auto* tw = new TerrainWrapper(std::move(mgr));
    return qjsbind::wrap<TW>(ctx, tw);
}

void* terrainHandleFromJS(JSContext* ctx, JSValueConst v) {
    auto* tw = qjsbind::unwrap<TW>(ctx, v);
    return static_cast<void*>(tw);
}

bool terrainSampleHeight(void* handle, float x, float z,
                         float rayStartY, float rayLength, float& outY) {
    if (!handle) return false;
    auto* tw = static_cast<TerrainWrapper*>(handle);
    if (TerrainWrapper::allInstances().find(tw) == TerrainWrapper::allInstances().end() || !tw->manager) {
        return false;
    }
    const bromath::Vec3 origin{x, rayStartY, z};
    const bromath::Vec3 dir{0.0f, -1.0f, 0.0f};
    auto hit = tw->manager->raycast(origin, dir, rayLength);
    if (!hit.hit) return false;
    outY = hit.worldPos[1];
    return true;
}

void TerrainBindings::cleanup(JSContext*) {
    for (auto* tw : TW::allInstances()) {
        if (tw->manager) {
            tw->manager->clear();
            tw->manager.reset();
        }
    }
}

void TerrainBindings::install(JSContext* ctx)
{
    qjsbind::Class<TW>(ctx, "Terrain")
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
        .method_raw("invalidateRegion", js_terrain_invalidateRegion, 4)
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

} // namespace bro::js

#endif // BRO_WITH_3D
