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
//                                 metresPerCell } | null)
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
                                  static_cast<float>(mpc));
    JS_FreeValue(ctx, abuf);
    JS_FreeValue(ctx, dataVal);
    return JS_DupValue(ctx, this_val);
}

// -------------------------------------------------------------------------
// clipmap.setDetailExemplar({ data, width, height, metresPerCell } | null)
//
// Hands the terrain a patch of real elevation whose structure becomes the
// source of all detail below the data floor. Any height field in metres will
// do; the point is that a generated one carries ridges and drainage that noise
// cannot. Returns `this` so calls chain.
// -------------------------------------------------------------------------

static JSValue js_clipmap_setDetailExemplar(JSContext* ctx, JSValueConst this_val,
                                            int argc, JSValueConst* argv) {
    auto* self = qjsbind::unwrap<CW>(ctx, this_val);
    if (!self || !self->terrain) return JS_DupValue(ctx, this_val);

    if (argc < 1 || JS_IsNull(argv[0]) || JS_IsUndefined(argv[0])) {
        self->terrain->setDetailExemplar(nullptr, 0, 0, 1.0f);
        return JS_DupValue(ctx, this_val);
    }
    if (!JS_IsObject(argv[0]))
        return JS_ThrowTypeError(ctx,
            "setDetailExemplar: expected { data, width, height, "
            "metresPerCell } or null");

    const int width  = qjsbind::get_prop_int(ctx, argv[0], "width", 0);
    const int height = qjsbind::get_prop_int(ctx, argv[0], "height", 0);
    const double mpc = qjsbind::get_prop_number(ctx, argv[0], "metresPerCell", 1.0);
    if (width <= 8 || height <= 8)
        return JS_ThrowTypeError(ctx,
            "setDetailExemplar: width and height must exceed 8");
    if (!(mpc > 0.0))
        return JS_ThrowTypeError(ctx,
            "setDetailExemplar: metresPerCell must be positive");

    JSValue dataVal = JS_GetPropertyStr(ctx, argv[0], "data");
    size_t byteOff = 0, viewLen = 0;
    JSValue abuf = JS_GetTypedArrayBuffer(ctx, dataVal, &byteOff, &viewLen, nullptr);
    if (JS_IsException(abuf)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        JS_FreeValue(ctx, abuf);
        JS_FreeValue(ctx, dataVal);
        return JS_ThrowTypeError(ctx,
            "setDetailExemplar: data must be a Float32Array");
    }
    size_t abufLen = 0;
    uint8_t* ptr = JS_GetArrayBuffer(ctx, &abufLen, abuf);
    const size_t want = static_cast<size_t>(width) * height * sizeof(float);
    if (!ptr || viewLen < want) {
        JS_FreeValue(ctx, abuf);
        JS_FreeValue(ctx, dataVal);
        return JS_ThrowRangeError(ctx,
            "setDetailExemplar: data holds %zu bytes, need %zu (%dx%d floats)",
            viewLen, want, width, height);
    }

    self->terrain->setDetailExemplar(reinterpret_cast<const float*>(ptr + byteOff),
                                     width, height, static_cast<float>(mpc));
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
        .method_raw("setDetailExemplar", js_clipmap_setDetailExemplar, 1)
        .method_raw("update", js_clipmap_update, 3)
        .method("elevationAt", [](CW* self, double x, double z) -> double {
            if (!self->terrain) return 0.0;
            return self->terrain->elevationAt((float)x, (float)z);
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
