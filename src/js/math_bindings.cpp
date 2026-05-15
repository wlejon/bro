#include "js/math_bindings.h"

#include <qjsbind/qjsbind.h>

#include <bromath/aabb.h>
#include <bromath/spatial_hash.h>
#include <bromath/sphere.h>
#include <bromath/vec.h>

#include <cstdint>
#include <memory>
#include <vector>

namespace bro::js {

struct SpatialHashWrapper {
    std::unique_ptr<bromath::SpatialHash3D> sh;
    SpatialHashWrapper(float cellSize)
        : sh(std::make_unique<bromath::SpatialHash3D>(cellSize)) {}
};
using SHW = SpatialHashWrapper;

static void installSpatialHash(JSContext* ctx) {
    qjsbind::Class<SHW>(ctx, "SpatialHash3D")
    .constructor([](JSContext* ctx, int argc, JSValueConst* argv) -> SHW* {
        double cs = 1.0;
        if (argc > 0) JS_ToFloat64(ctx, &cs, argv[0]);
        return new SHW((float)cs);
    })
    .method("reset", [](SHW* w, double cellSize) {
        w->sh->reset((float)cellSize);
    }, qjsbind::returns_this)
    .method("clear", [](SHW* w) { w->sh->clear(); }, qjsbind::returns_this)
    .method("insert", [](SHW* w, double x, double y, double z, int id) {
        w->sh->insert({(float)x, (float)y, (float)z}, (int32_t)id);
    }, qjsbind::returns_this)
    .method("insertSphere", [](SHW* w, double x, double y, double z,
                               double radius, int id) {
        w->sh->insert(bromath::Sphere{{(float)x, (float)y, (float)z},
                                      (float)radius}, (int32_t)id);
    }, qjsbind::returns_this)
    .method("remove", [](SHW* w, int id) {
        w->sh->remove((int32_t)id);
    }, qjsbind::returns_this)
    .method("radiusQuery", [](SHW* w, JSContext* ctx, double x, double y, double z,
                              double radius) -> JSValue {
        std::vector<int32_t> ids;
        w->sh->radiusQuery({(float)x, (float)y, (float)z}, (float)radius, ids);
        JSValue arr = JS_NewArray(ctx);
        for (size_t i = 0; i < ids.size(); i++) {
            JS_SetPropertyUint32(ctx, arr, (uint32_t)i, JS_NewInt32(ctx, ids[i]));
        }
        return arr;
    })
    .method("queryAABB", [](SHW* w, JSContext* ctx,
                            double minX, double minY, double minZ,
                            double maxX, double maxY, double maxZ) -> JSValue {
        std::vector<int32_t> ids;
        bromath::AABB3 box{
            {(float)minX, (float)minY, (float)minZ},
            {(float)maxX, (float)maxY, (float)maxZ}};
        w->sh->queryAABB(box, ids);
        JSValue arr = JS_NewArray(ctx);
        for (size_t i = 0; i < ids.size(); i++) {
            JS_SetPropertyUint32(ctx, arr, (uint32_t)i, JS_NewInt32(ctx, ids[i]));
        }
        return arr;
    })
    .method("nearest", [](SHW* w, double x, double y, double z, double maxRadius) -> int {
        return (int)w->sh->nearest({(float)x, (float)y, (float)z}, (float)maxRadius);
    })
    .get("size",      [](SHW* w) { return (int)w->sh->size(); })
    .get("cellSize",  [](SHW* w) -> double { return (double)w->sh->cellSize(); })
    .get("maxRadius", [](SHW* w) -> double { return (double)w->sh->maxRadius(); })
    ;
}

void MathBindings::install(JSContext* ctx) {
    installSpatialHash(ctx);

    // Build namespace: bro.math.* — currently just SpatialHash3D, but this is
    // the seed for future bromath surfaces (curves, color, easing, etc.).
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue broObj = JS_GetPropertyStr(ctx, global, "bro");
    if (JS_IsUndefined(broObj) || JS_IsException(broObj)) {
        JS_FreeValue(ctx, broObj);
        broObj = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, global, "bro", JS_DupValue(ctx, broObj));
    }

    JSValue mathObj = JS_GetPropertyStr(ctx, broObj, "math");
    if (JS_IsUndefined(mathObj) || JS_IsException(mathObj)) {
        JS_FreeValue(ctx, mathObj);
        mathObj = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, broObj, "math", JS_DupValue(ctx, mathObj));
    }

    // Alias the SpatialHash3D constructor onto bro.math. qjsbind registers
    // the class globally under that name, so we just look it up and copy.
    JSValue ctor = JS_GetPropertyStr(ctx, global, "SpatialHash3D");
    if (!JS_IsUndefined(ctor) && !JS_IsException(ctor)) {
        JS_SetPropertyStr(ctx, mathObj, "SpatialHash3D", JS_DupValue(ctx, ctor));
    }
    JS_FreeValue(ctx, ctor);

    JS_FreeValue(ctx, mathObj);
    JS_FreeValue(ctx, broObj);
    JS_FreeValue(ctx, global);
}

void MathBindings::cleanup(JSContext*) {
    // qjsbind owns the class registration + finalizer; bro.math is reached
    // from globalThis and dropped by the engine-level globalThis sweep.
}

} // namespace bro::js
