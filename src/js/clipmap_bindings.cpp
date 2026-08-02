#include "js/clipmap_bindings.h"
#if BRO_WITH_3D  // modular-build feature gate

#include <qjsbind/qjsbind.h>

#include "scene/clipmap_terrain.h"
#include "scene/scene_graph.h"
#include "js/scene_bindings_internal.h"

#include "util/log.h"

#include <cmath>
#include <cstring>
#include <memory>
#include <unordered_set>
#include <vector>

namespace bro::js {

// -------------------------------------------------------------------------
// ClipmapWrapper — opaque data attached to JS clipmap-terrain objects.
//
// Holds the terrain plus a weak liveness token for its SceneGraph, so the
// `node` accessor can mint a NodeWrapper against a graph that may already
// have been torn down (see the liveness note in scene_bindings_internal.h).
// -------------------------------------------------------------------------

struct ClipmapWrapper {
    std::unique_ptr<scene::ClipmapTerrain> terrain;
    std::weak_ptr<scene::SceneGraph::LivenessToken> token;

    ClipmapWrapper(std::unique_ptr<scene::ClipmapTerrain> t,
                   std::weak_ptr<scene::SceneGraph::LivenessToken> tok)
        : terrain(std::move(t)), token(std::move(tok)) {
        allInstances().insert(this);
    }
    ~ClipmapWrapper() { allInstances().erase(this); }

    scene::SceneGraph* graph() const {
        auto t = token.lock();
        return t ? t->graph : nullptr;
    }

    static std::unordered_set<ClipmapWrapper*>& allInstances() {
        static std::unordered_set<ClipmapWrapper*> s;
        return s;
    }
};

using CW = ClipmapWrapper;

// -------------------------------------------------------------------------
// clipmap.setHeightLayer(index, { data, width, height, originX, originZ,
//                                 metresPerCell, wrapX, bandLimited } | null)
//
// Returns `this` so calls chain. A null/undefined descriptor releases the
// layer's pixels.
// -------------------------------------------------------------------------

static JSValue js_clipmap_setHeightLayer(JSContext* ctx, JSValueConst this_val,
                                         int argc, JSValueConst* argv) {
    auto* self = qjsbind::unwrap<CW>(ctx, this_val);
    if (!self || !self->terrain) return JS_DupValue(ctx, this_val);
    if (argc < 1)
        return JS_ThrowTypeError(ctx,
            "setHeightLayer(index, desc | null) needs an index");

    int32_t index = 0;
    if (JS_ToInt32(ctx, &index, argv[0]) < 0) return JS_EXCEPTION;
    if (index < 0 || index >= scene::ClipmapTerrain::kMaxLayers)
        return JS_ThrowRangeError(ctx, "setHeightLayer: index must be 0..%d",
                                  scene::ClipmapTerrain::kMaxLayers - 1);

    if (argc < 2 || JS_IsNull(argv[1]) || JS_IsUndefined(argv[1])) {
        self->terrain->setHeightLayer(index, nullptr, 0, 0, 0, 0, 1);
        return JS_DupValue(ctx, this_val);
    }
    if (!JS_IsObject(argv[1]))
        return JS_ThrowTypeError(ctx,
            "setHeightLayer: expected { data, width, height, originX, "
            "originZ, metresPerCell } or null");

    const int width  = qjsbind::get_prop_int(ctx, argv[1], "width", 0);
    const int height = qjsbind::get_prop_int(ctx, argv[1], "height", 0);
    const double originX = qjsbind::get_prop_number(ctx, argv[1], "originX", 0.0);
    const double originZ = qjsbind::get_prop_number(ctx, argv[1], "originZ", 0.0);
    const double mpc = qjsbind::get_prop_number(ctx, argv[1], "metresPerCell", 1.0);
    // wrapX: the layer is periodic in X — a global equirectangular chart whose
    // column 0 continues column W-1. Off by default, because a camera-following
    // window is NOT periodic and wrapping one would fold the far side of the
    // window into view.
    const bool wrapX = qjsbind::get_prop_bool(ctx, argv[1], "wrapX", false);
    // bandLimited: the samples carry real content down to 2 * metresPerCell, so
    // the procedural detail band high-passes against that and adds nothing
    // coarser. Off by default — the default is the pre-existing cell-size
    // inference, which reads a fine floor as a smooth learned window and
    // roughens it from a fixed ceiling. See docs/clipmap-api.js.
    const bool bandLimited =
        qjsbind::get_prop_bool(ctx, argv[1], "bandLimited", false);
    if (width <= 0 || height <= 0)
        return JS_ThrowTypeError(ctx,
            "setHeightLayer: width and height must be positive");
    if (!(mpc > 0.0))
        return JS_ThrowTypeError(ctx,
            "setHeightLayer: metresPerCell must be positive");

    JSValue dataVal = JS_GetPropertyStr(ctx, argv[1], "data");
    size_t byteOff = 0, viewLen = 0;
    JSValue abuf = JS_GetTypedArrayBuffer(ctx, dataVal, &byteOff, &viewLen, nullptr);
    if (JS_IsException(abuf)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        JS_FreeValue(ctx, abuf);
        JS_FreeValue(ctx, dataVal);
        return JS_ThrowTypeError(ctx, "setHeightLayer: data must be a Float32Array");
    }
    size_t abufLen = 0;
    uint8_t* ptr = JS_GetArrayBuffer(ctx, &abufLen, abuf);
    const size_t want = static_cast<size_t>(width) * height * sizeof(float);
    if (!ptr || viewLen < want) {
        JS_FreeValue(ctx, abuf);
        JS_FreeValue(ctx, dataVal);
        // Short buffers are refused rather than partially uploaded: the tail
        // would render as whatever the previous layer left behind.
        return JS_ThrowRangeError(ctx,
            "setHeightLayer: data holds %zu bytes, need %zu (%dx%d floats)",
            viewLen, want, width, height);
    }

    self->terrain->setHeightLayer(index,
                                  reinterpret_cast<const float*>(ptr + byteOff),
                                  width, height, static_cast<float>(originX),
                                  static_cast<float>(originZ),
                                  static_cast<float>(mpc), wrapX, bandLimited);
    JS_FreeValue(ctx, abuf);
    JS_FreeValue(ctx, dataVal);
    return JS_DupValue(ctx, this_val);
}

static JSValue js_clipmap_setSnowLine(JSContext* ctx, JSValueConst this_val,
                                      int argc, JSValueConst* argv) {
    auto* self = qjsbind::unwrap<CW>(ctx, this_val);
    if (!self || !self->terrain) return JS_DupValue(ctx, this_val);
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "setSnowLine(m) needs a number");
    double sl = 0.0;
    if (JS_ToFloat64(ctx, &sl, argv[0]) < 0) return JS_EXCEPTION;
    self->terrain->setSnowLine((float)sl);
    return JS_DupValue(ctx, this_val);
}

// clipmap.setChartCenter(x, z) — pin the planetary-curvature chart to a fixed
// world XZ (the tangent point of the sphere the sheet should agree with);
// clipmap.setChartCenter(null) restores the camera-following default. See
// ClipmapTerrain::setChartCenter for the geometry.
static JSValue js_clipmap_setChartCenter(JSContext* ctx, JSValueConst this_val,
                                         int argc, JSValueConst* argv) {
    auto* self = qjsbind::unwrap<CW>(ctx, this_val);
    if (!self || !self->terrain) return JS_DupValue(ctx, this_val);
    if (argc < 1 || JS_IsNull(argv[0]) || JS_IsUndefined(argv[0])) {
        self->terrain->clearChartCenter();
        return JS_DupValue(ctx, this_val);
    }
    if (argc < 2)
        return JS_ThrowTypeError(ctx, "setChartCenter(x, z) needs two numbers, or null to clear");
    double x = 0.0, z = 0.0;
    if (JS_ToFloat64(ctx, &x, argv[0]) < 0) return JS_EXCEPTION;
    if (JS_ToFloat64(ctx, &z, argv[1]) < 0) return JS_EXCEPTION;
    self->terrain->setChartCenter((float)x, (float)z);
    return JS_DupValue(ctx, this_val);
}

static JSValue js_clipmap_setDetail(JSContext* ctx, JSValueConst this_val,
                                    int argc, JSValueConst* argv) {
    auto* self = qjsbind::unwrap<CW>(ctx, this_val);
    if (!self || !self->terrain) return JS_DupValue(ctx, this_val);
    if (argc < 1 || !JS_IsObject(argv[0]))
        return JS_ThrowTypeError(ctx, "setDetail(desc) needs an object");

    const auto& cfg = self->terrain->config();
    double wl = qjsbind::get_prop_number(ctx, argv[0], "wavelength", cfg.detailWavelength);
    double rel = qjsbind::get_prop_number(ctx, argv[0], "relief", cfg.detailRelief);
    double gain = qjsbind::get_prop_number(ctx, argv[0], "gain", cfg.detailGain);
    int oct = qjsbind::get_prop_int(ctx, argv[0], "octaves", cfg.detailOctaves);

    self->terrain->setDetail((float)wl, (float)rel, (float)gain, oct);
    return JS_DupValue(ctx, this_val);
}

static void parseMaterialProp(JSContext* ctx, JSValueConst parent, const char* name,
                              float* albedo_out, float& roughness_out) {
    JSValue val = JS_GetPropertyStr(ctx, parent, name);
    if (JS_IsObject(val)) {
        JSValue albVal = JS_GetPropertyStr(ctx, val, "albedo");
        if (JS_IsObject(albVal)) {
            for (int i = 0; i < 3; ++i) {
                JSValue el = JS_GetPropertyUint32(ctx, albVal, i);
                double v = 0.0;
                if (JS_ToFloat64(ctx, &v, el) >= 0) {
                    albedo_out[i] = (float)v;
                }
                JS_FreeValue(ctx, el);
            }
        }
        JS_FreeValue(ctx, albVal);
        roughness_out = (float)qjsbind::get_prop_number(ctx, val, "roughness", roughness_out);
    }
    JS_FreeValue(ctx, val);
}

static JSValue js_clipmap_setMaterials(JSContext* ctx, JSValueConst this_val,
                                       int argc, JSValueConst* argv) {
    auto* self = qjsbind::unwrap<CW>(ctx, this_val);
    if (!self || !self->terrain) return JS_DupValue(ctx, this_val);
    if (argc < 1 || !JS_IsObject(argv[0]))
        return JS_ThrowTypeError(ctx, "setMaterials(desc) needs an object");

    float rockAlb[3]  = {0.246f, 0.232f, 0.221f};
    float rockRough  = 0.88f;
    float snowAlb[3]  = {0.760f, 0.790f, 0.830f};
    float snowRough  = 0.62f;
    float sandAlb[3]  = {0.480f, 0.430f, 0.330f};
    float sandRough  = 0.94f;
    float grassAlb[3] = {0.180f, 0.235f, 0.128f};
    float grassRough = 0.97f;

    parseMaterialProp(ctx, argv[0], "rock",  rockAlb,  rockRough);
    parseMaterialProp(ctx, argv[0], "snow",  snowAlb,  snowRough);
    parseMaterialProp(ctx, argv[0], "sand",  sandAlb,  sandRough);
    parseMaterialProp(ctx, argv[0], "grass", grassAlb, grassRough);

    self->terrain->setMaterials(rockAlb, rockRough, snowAlb, snowRough, sandAlb, sandRough, grassAlb, grassRough);
    return JS_DupValue(ctx, this_val);
}

// setForest({ albedo:[r,g,b], strength:0..1 }) — L0 forest canopy tint.
static JSValue js_clipmap_setForest(JSContext* ctx, JSValueConst this_val,
                                    int argc, JSValueConst* argv) {
    auto* self = qjsbind::unwrap<CW>(ctx, this_val);
    if (!self || !self->terrain) return JS_DupValue(ctx, this_val);
    if (argc < 1 || !JS_IsObject(argv[0]))
        return JS_ThrowTypeError(ctx, "setForest(desc) needs an object");

    float albedo[3] = {0.105f, 0.205f, 0.098f};
    JSValue albVal = JS_GetPropertyStr(ctx, argv[0], "albedo");
    if (JS_IsObject(albVal)) {
        for (int i = 0; i < 3; ++i) {
            JSValue el = JS_GetPropertyUint32(ctx, albVal, i);
            double v = 0.0;
            if (JS_ToFloat64(ctx, &v, el) >= 0) albedo[i] = (float)v;
            JS_FreeValue(ctx, el);
        }
    }
    JS_FreeValue(ctx, albVal);
    double strength = qjsbind::get_prop_number(ctx, argv[0], "strength", 0.85);
    self->terrain->setForest(albedo, (float)strength);
    return JS_DupValue(ctx, this_val);
}

static JSValue js_clipmap_setSurfaceLayer(JSContext* ctx, JSValueConst this_val,
                                          int argc, JSValueConst* argv) {
    auto* self = qjsbind::unwrap<CW>(ctx, this_val);
    if (!self || !self->terrain) return JS_DupValue(ctx, this_val);

    // Two shapes, because this used to take no index and existing callers pass
    // the spec first: setSurfaceLayer(spec) is layer 0, setSurfaceLayer(index,
    // spec) addresses the stack. Distinguished on the FIRST argument's type,
    // not on argc, so setSurfaceLayer(2, null) releases layer 2 rather than
    // looking like a one-argument release of layer 0.
    int idx = 0;
    int specArg = 0;
    if (argc >= 1 && JS_IsNumber(argv[0])) {
        int32_t i = 0;
        JS_ToInt32(ctx, &i, argv[0]);
        if (i < 0 || i >= scene::ClipmapTerrain::kMaxLayers)
            return JS_ThrowRangeError(ctx,
                "setSurfaceLayer: index %d out of range [0, %d)",
                i, scene::ClipmapTerrain::kMaxLayers);
        idx = i;
        specArg = 1;
    }

    if (argc <= specArg || JS_IsNull(argv[specArg]) || JS_IsUndefined(argv[specArg])) {
        self->terrain->setSurfaceLayer(idx, nullptr, 0, 0, 0, 0, 1);
        return JS_DupValue(ctx, this_val);
    }
    if (!JS_IsObject(argv[specArg]))
        return JS_ThrowTypeError(ctx,
            "setSurfaceLayer: expected [index,] { data, width, height, originX, "
            "originZ, metresPerCell[, components] } or null");

    const int width  = qjsbind::get_prop_int(ctx, argv[specArg], "width", 0);
    const int height = qjsbind::get_prop_int(ctx, argv[specArg], "height", 0);
    const double originX = qjsbind::get_prop_number(ctx, argv[specArg], "originX", 0.0);
    const double originZ = qjsbind::get_prop_number(ctx, argv[specArg], "originZ", 0.0);
    const double mpc = qjsbind::get_prop_number(ctx, argv[specArg], "metresPerCell", 1.0);
    // Defaults to 3 so every caller written against the three-channel API keeps
    // working without saying so.
    const int comps = qjsbind::get_prop_int(ctx, argv[specArg], "components", 3);
    if (width <= 0 || height <= 0)
        return JS_ThrowTypeError(ctx,
            "setSurfaceLayer: width and height must be positive");
    if (!(mpc > 0.0))
        return JS_ThrowTypeError(ctx,
            "setSurfaceLayer: metresPerCell must be positive");
    if (comps != 3 && comps != 4)
        return JS_ThrowRangeError(ctx,
            "setSurfaceLayer: components must be 3 or 4, got %d", comps);

    JSValue dataVal = JS_GetPropertyStr(ctx, argv[specArg], "data");
    size_t byteOff = 0, viewLen = 0;
    JSValue abuf = JS_GetTypedArrayBuffer(ctx, dataVal, &byteOff, &viewLen, nullptr);
    if (JS_IsException(abuf)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        JS_FreeValue(ctx, abuf);
        JS_FreeValue(ctx, dataVal);
        return JS_ThrowTypeError(ctx, "setSurfaceLayer: data must be a Float32Array");
    }
    size_t abufLen = 0;
    uint8_t* ptr = JS_GetArrayBuffer(ctx, &abufLen, abuf);
    const size_t want = static_cast<size_t>(width) * height * comps * sizeof(float);
    if (!ptr || viewLen < want) {
        JS_FreeValue(ctx, abuf);
        JS_FreeValue(ctx, dataVal);
        return JS_ThrowRangeError(ctx,
            "setSurfaceLayer: data holds %zu bytes, need %zu (%dx%dx%d floats)",
            viewLen, want, width, height, comps);
    }

    self->terrain->setSurfaceLayer(idx, reinterpret_cast<const float*>(ptr + byteOff),
                                   width, height, static_cast<float>(originX),
                                   static_cast<float>(originZ),
                                   static_cast<float>(mpc), comps);
    JS_FreeValue(ctx, abuf);
    JS_FreeValue(ctx, dataVal);
    return JS_DupValue(ctx, this_val);
}

// clipmap.update(camX, camY, camZ) -> this
static JSValue js_clipmap_update(JSContext* ctx, JSValueConst this_val,
                                 int argc, JSValueConst* argv) {
    auto* self = qjsbind::unwrap<CW>(ctx, this_val);
    if (!self || !self->terrain) return JS_DupValue(ctx, this_val);
    if (argc < 3)
        return JS_ThrowTypeError(ctx, "update(camX, camY, camZ) needs 3 numbers");
    double v[3];
    for (int i = 0; i < 3; i++)
        if (JS_ToFloat64(ctx, &v[i], argv[i]) < 0) return JS_EXCEPTION;
    self->terrain->update(static_cast<float>(v[0]), static_cast<float>(v[1]),
                          static_cast<float>(v[2]));
    return JS_DupValue(ctx, this_val);
}

// -------------------------------------------------------------------------
// Factory: scene.createClipmapTerrain(opts)
// -------------------------------------------------------------------------

JSValue createClipmapTerrainJS(JSContext* ctx, scene::SceneGraph* graph,
                               JSValueConst opts) {
    if (!graph) return JS_ThrowTypeError(ctx, "createClipmapTerrain: no scene graph");

    scene::ClipmapConfig cfg;
    if (JS_IsObject(opts)) {
        cfg.levels      = qjsbind::get_prop_int(ctx, opts, "levels", cfg.levels);
        cfg.resolution  = qjsbind::get_prop_int(ctx, opts, "resolution", cfg.resolution);
        cfg.cellSize    = (float)qjsbind::get_prop_number(ctx, opts, "cellSize", cfg.cellSize);
        cfg.heightScale = (float)qjsbind::get_prop_number(ctx, opts, "heightScale", cfg.heightScale);
        cfg.seaLevel    = (float)qjsbind::get_prop_number(ctx, opts, "seaLevel", cfg.seaLevel);
        cfg.snowLine    = (float)qjsbind::get_prop_number(ctx, opts, "snowLine", cfg.snowLine);
        cfg.maxCellScale =
            (float)qjsbind::get_prop_number(ctx, opts, "maxCellScale", cfg.maxCellScale);
        cfg.planetRadius =
            (float)qjsbind::get_prop_number(ctx, opts, "planetRadius", cfg.planetRadius);
        // Opt-in per-layer LOD fade; off is bit-identical to the flag not
        // existing. See ClipmapConfig::layerFade.
        cfg.layerFade = qjsbind::get_prop_bool(ctx, opts, "layerFade", cfg.layerFade);
        // Opt-in coverage floor: keep reach at or past the visible limb when
        // the data allows it. See ClipmapConfig::coverageFloor.
        cfg.coverageFloor =
            qjsbind::get_prop_bool(ctx, opts, "coverageFloor", cfg.coverageFloor);
        cfg.detailWavelength =
            (float)qjsbind::get_prop_number(ctx, opts, "detailWavelength", cfg.detailWavelength);
        cfg.detailRelief =
            (float)qjsbind::get_prop_number(ctx, opts, "detailRelief", cfg.detailRelief);
        cfg.detailGain =
            (float)qjsbind::get_prop_number(ctx, opts, "detailGain", cfg.detailGain);
        cfg.detailOctaves =
            qjsbind::get_prop_int(ctx, opts, "detailOctaves", cfg.detailOctaves);
    }

    auto terrain = std::make_unique<scene::ClipmapTerrain>(*graph, cfg);
    return qjsbind::wrap<CW>(ctx,
        new CW(std::move(terrain), graph->livenessToken()));
}

// -------------------------------------------------------------------------
// Install / Cleanup
// -------------------------------------------------------------------------

void ClipmapBindings::install(JSContext* ctx) {
    qjsbind::Class<CW>(ctx, "ClipmapTerrain")
        // No constructor — created via scene.createClipmapTerrain()
        .method_raw("setHeightLayer", js_clipmap_setHeightLayer, 2)
        .method_raw("setSnowLine", js_clipmap_setSnowLine, 1)
        .method_raw("setChartCenter", js_clipmap_setChartCenter, 2)
        .method_raw("setDetail", js_clipmap_setDetail, 1)
        .method_raw("setMaterials", js_clipmap_setMaterials, 1)
        .method_raw("setForest", js_clipmap_setForest, 1)
        .method_raw("setSurfaceLayer", js_clipmap_setSurfaceLayer, 1)
        .method_raw("update", js_clipmap_update, 3)
        .method("elevationAt", [](CW* self, double x, double z) -> double {
            if (!self->terrain) return 0.0;
            return self->terrain->elevationAt((float)x, (float)z);
        })
        .method("renderedElevationAt", [](CW* self, double x, double z) -> double {
            // elevationAt() bent by the planetary-curvature chart: the world Y
            // at which the DRAWN sheet sits under (x, z). Identical to
            // elevationAt() unless a chart centre is pinned on a round world;
            // pinned, the sheet at chord rho from the centre has dropped by
            // ~rho^2/2R, and a camera grounded on elevationAt() floats
            // kilometres above the picture. Ground cameras and AGL readouts on
            // this one; keep elevationAt() for the field's own space.
            if (!self->terrain) return 0.0;
            return self->terrain->renderedElevationAt((float)x, (float)z);
        })
        .method("destroy", [](CW* self) {
            if (self->terrain) {
                self->terrain->destroy();
                self->terrain.reset();
            }
        })
        .get("node", [](CW* self, JSContext* c) -> JSValue {
            if (!self->terrain) return JS_NULL;
            auto* g = self->graph();
            if (!g) return JS_NULL;
            return wrapNode(c, self->terrain->node(), g);
        })
        .get("levels", [](CW* self) -> int {
            return self->terrain ? self->terrain->levelCount() : 0;
        })
        .get("resolution", [](CW* self) -> int {
            return self->terrain ? self->terrain->config().resolution : 0;
        })
        .get("cellSize", [](CW* self) -> double {
            return self->terrain ? self->terrain->config().cellSize : 0.0;
        })
        .get("layerCount", [](CW* self) -> int {
            return self->terrain ? self->terrain->layerCount() : 0;
        })
        .get("triangleCount", [](CW* self) -> int {
            return self->terrain ? self->terrain->triangleCount() : 0;
        })
        .get("vertexCount", [](CW* self) -> int {
            return self->terrain ? self->terrain->vertexCount() : 0;
        })
        .get("farDistance", [](CW* self) -> double {
            // Outer half-extent of the coarsest ring. NOT a constant: update()
            // zooms the stack with altitude, so read this every frame — it is
            // both the camera's far plane and the radius the app must keep
            // height data across.
            return self->terrain ? (double)self->terrain->farDistance() : 0.0;
        })
        .get("cellScale", [](CW* self) -> double {
            return self->terrain ? (double)self->terrain->cellScale() : 1.0;
        })
        .get("planetRadius", [](CW* self) -> double {
            return self->terrain ? (double)self->terrain->config().planetRadius : 0.0;
        })
        .method("coverageDistance", [](CW* self, double eyeAboveSeaLevel) -> double {
            // The radius the APP must supply height data across:
            // horizon(eye) + horizon(highest ground). Sizing a layer from
            // farDistance instead is what makes the data bill quadratic in a
            // reach that is mostly behind the planet. See
            // ClipmapTerrain::coverageDistance.
            //
            // ABOVE SEA LEVEL, not above the ground underfoot: a camera on a
            // summit sees much further than one on a plain, and height above
            // the ground cannot tell the two apart. Passing height above ground
            // here under-sizes the request and cuts the world off short.
            return self->terrain
                ? (double)self->terrain->coverageDistance((float)eyeAboveSeaLevel)
                : 0.0;
        })
        .method("horizonDistance", [](CW* self, double eyeAboveSeaLevel) -> double {
            // How far the surface is visible from that height, measured from
            // sea level as above. Infinite on a flat world, so JS sees
            // Infinity — sizing data from it is then correctly an error rather
            // than a silently huge number.
            return self->terrain
                ? (double)self->terrain->horizonDistance((float)eyeAboveSeaLevel)
                : 0.0;
        });
}

void ClipmapBindings::cleanup(JSContext*) {
    // Drop every live terrain before scene graphs are destroyed, so their
    // destructors don't touch a dangling SceneGraph reference.
    for (auto* cw : CW::allInstances()) {
        if (cw->terrain) {
            cw->terrain->destroy();
            cw->terrain.reset();
        }
    }
}

} // namespace bro::js

#endif  // BRO_WITH_3D
