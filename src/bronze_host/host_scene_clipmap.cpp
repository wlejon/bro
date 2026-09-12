#if BRO_WITH_3D

#include "bronze_host/host_scene_internal.h"
#include "scene/clipmap_terrain.h"
#include "scene/scene_graph.h"
#include "util/log.h"

#include <cmath>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace bro::bronze_host {

HostClass g_clipmapClass;

namespace {

static void parseMaterialProp(Value parent, const char* name,
                              float* albedo_out, float& roughness_out) {
    if (!ev::isObject(parent)) return;
    Value val = ev::getProperty(parent, name);
    if (ev::isObject(val)) {
        Value albVal = ev::getProperty(val, "albedo");
        if (ev::isObject(albVal)) {
            for (int i = 0; i < 3; ++i) {
                Value el = ev::getElement(albVal, i);
                if (ev::isNumber(el)) albedo_out[i] = static_cast<float>(ev::toDouble(el));
            }
        }
        Value rVal = ev::getProperty(val, "roughness");
        if (ev::isNumber(rVal)) roughness_out = static_cast<float>(ev::toDouble(rVal));
    }
}

} // namespace

void ensureClipmapClassInstalled() {
    static bool s_installed = false;
    if (s_installed) return;
    s_installed = true;
    g_clipmapClass.install("ClipmapTerrain", 0, nullptr, [](ObjectBuilder& b) {
        b.def("setHeightLayer", 2, [](Value self_, std::span<const Value> a) -> Value {
            auto* c = clipmapCellOf(self_);
            if (!c || !c->terrain) return self_;
            if (a.empty()) return ev::throwTypeError("setHeightLayer(index, desc | null) needs an index");

            int32_t index = ev::isNumber(a[0]) ? static_cast<int32_t>(ev::toDouble(a[0])) : 0;
            if (index < 0 || index >= scene::ClipmapTerrain::kMaxLayers)
                return ev::throwRangeError("setHeightLayer: index must be 0.." +
                                          std::to_string(scene::ClipmapTerrain::kMaxLayers - 1));

            if (a.size() < 2 || ev::isNull(a[1]) || ev::isUndefined(a[1])) {
                c->terrain->setHeightLayer(index, nullptr, 0, 0, 0, 0, 1);
                return self_;
            }
            if (!ev::isObject(a[1]))
                return ev::throwTypeError("setHeightLayer: expected { data, width, height, originX, originZ, metresPerCell } or null");

            Value desc = a[1];
            Value wVal = ev::getProperty(desc, "width");
            Value hVal = ev::getProperty(desc, "height");
            int width = ev::isNumber(wVal) ? static_cast<int>(ev::toDouble(wVal)) : 0;
            int height = ev::isNumber(hVal) ? static_cast<int>(ev::toDouble(hVal)) : 0;
            Value oxVal = ev::getProperty(desc, "originX");
            Value ozVal = ev::getProperty(desc, "originZ");
            double originX = ev::isNumber(oxVal) ? ev::toDouble(oxVal) : 0.0;
            double originZ = ev::isNumber(ozVal) ? ev::toDouble(ozVal) : 0.0;
            Value mpcVal = ev::getProperty(desc, "metresPerCell");
            double mpc = ev::isNumber(mpcVal) ? ev::toDouble(mpcVal) : 1.0;
            Value wxVal = ev::getProperty(desc, "wrapX");
            bool wrapX = !ev::isUndefined(wxVal) && ev::toBool(wxVal);
            Value blVal = ev::getProperty(desc, "bandLimited");
            bool bandLimited = !ev::isUndefined(blVal) && ev::toBool(blVal);

            if (width <= 0 || height <= 0)
                return ev::throwTypeError("setHeightLayer: width and height must be positive");
            if (!(mpc > 0.0))
                return ev::throwTypeError("setHeightLayer: metresPerCell must be positive");

            Value dataVal = ev::getProperty(desc, "data");
            auto info = ev::typedArrayInfo(dataVal);
            if (!info || !info.data)
                return ev::throwTypeError("setHeightLayer: data must be a Float32Array");

            const size_t want = static_cast<size_t>(width) * height * sizeof(float);
            if (info.byteLength < want) {
                return ev::throwRangeError("setHeightLayer: data holds " + std::to_string(info.byteLength) +
                                           " bytes, need " + std::to_string(want) + " (" +
                                           std::to_string(width) + "x" + std::to_string(height) + " floats)");
            }

            c->terrain->setHeightLayer(index,
                                       reinterpret_cast<const float*>(info.data),
                                       width, height, static_cast<float>(originX),
                                       static_cast<float>(originZ),
                                       static_cast<float>(mpc), wrapX, bandLimited);
            return self_;
        });

        b.def("setSnowLine", 1, [](Value self_, std::span<const Value> a) -> Value {
            auto* c = clipmapCellOf(self_);
            if (!c || !c->terrain) return self_;
            if (a.empty()) return ev::throwTypeError("setSnowLine(m) needs a number");
            double sl = ev::isNumber(a[0]) ? ev::toDouble(a[0]) : 0.0;
            c->terrain->setSnowLine(static_cast<float>(sl));
            return self_;
        });

        b.def("setChartCenter", 2, [](Value self_, std::span<const Value> a) -> Value {
            auto* c = clipmapCellOf(self_);
            if (!c || !c->terrain) return self_;
            if (a.empty() || ev::isNull(a[0]) || ev::isUndefined(a[0])) {
                c->terrain->clearChartCenter();
                return self_;
            }
            if (a.size() < 2)
                return ev::throwTypeError("setChartCenter(x, z) needs two numbers, or null to clear");
            double x = ev::isNumber(a[0]) ? ev::toDouble(a[0]) : 0.0;
            double z = ev::isNumber(a[1]) ? ev::toDouble(a[1]) : 0.0;
            c->terrain->setChartCenter(static_cast<float>(x), static_cast<float>(z));
            return self_;
        });

        b.def("setDetail", 1, [](Value self_, std::span<const Value> a) -> Value {
            auto* c = clipmapCellOf(self_);
            if (!c || !c->terrain) return self_;
            if (a.empty() || !ev::isObject(a[0]))
                return ev::throwTypeError("setDetail(desc) needs an object");

            const auto& cfg = c->terrain->config();
            Value wlVal = ev::getProperty(a[0], "wavelength");
            Value relVal = ev::getProperty(a[0], "relief");
            Value gVal = ev::getProperty(a[0], "gain");
            Value octVal = ev::getProperty(a[0], "octaves");

            double wl = ev::isNumber(wlVal) ? ev::toDouble(wlVal) : cfg.detailWavelength;
            double rel = ev::isNumber(relVal) ? ev::toDouble(relVal) : cfg.detailRelief;
            double gain = ev::isNumber(gVal) ? ev::toDouble(gVal) : cfg.detailGain;
            int oct = ev::isNumber(octVal) ? static_cast<int>(ev::toDouble(octVal)) : cfg.detailOctaves;

            c->terrain->setDetail(static_cast<float>(wl), static_cast<float>(rel), static_cast<float>(gain), oct);
            return self_;
        });

        b.def("setMaterials", 1, [](Value self_, std::span<const Value> a) -> Value {
            auto* c = clipmapCellOf(self_);
            if (!c || !c->terrain) return self_;
            if (a.empty() || !ev::isObject(a[0]))
                return ev::throwTypeError("setMaterials(desc) needs an object");

            float rockAlb[3]  = {0.246f, 0.232f, 0.221f};
            float rockRough   = 0.88f;
            float snowAlb[3]  = {0.760f, 0.790f, 0.830f};
            float snowRough   = 0.62f;
            float sandAlb[3]  = {0.480f, 0.430f, 0.330f};
            float sandRough   = 0.94f;
            float grassAlb[3] = {0.180f, 0.235f, 0.128f};
            float grassRough  = 0.97f;

            parseMaterialProp(a[0], "rock",  rockAlb,  rockRough);
            parseMaterialProp(a[0], "snow",  snowAlb,  snowRough);
            parseMaterialProp(a[0], "sand",  sandAlb,  sandRough);
            parseMaterialProp(a[0], "grass", grassAlb, grassRough);

            c->terrain->setMaterials(rockAlb, rockRough, snowAlb, snowRough, sandAlb, sandRough, grassAlb, grassRough);
            return self_;
        });

        b.def("setForest", 1, [](Value self_, std::span<const Value> a) -> Value {
            auto* c = clipmapCellOf(self_);
            if (!c || !c->terrain) return self_;
            if (a.empty() || !ev::isObject(a[0]))
                return ev::throwTypeError("setForest(desc) needs an object");

            float albedo[3] = {0.105f, 0.205f, 0.098f};
            Value albVal = ev::getProperty(a[0], "albedo");
            if (ev::isObject(albVal)) {
                for (int i = 0; i < 3; ++i) {
                    Value el = ev::getElement(albVal, i);
                    if (ev::isNumber(el)) albedo[i] = static_cast<float>(ev::toDouble(el));
                }
            }
            Value stVal = ev::getProperty(a[0], "strength");
            double strength = ev::isNumber(stVal) ? ev::toDouble(stVal) : 0.85;
            c->terrain->setForest(albedo, static_cast<float>(strength));
            return self_;
        });

        b.def("setSurfaceLayer", 2, [](Value self_, std::span<const Value> a) -> Value {
            auto* c = clipmapCellOf(self_);
            if (!c || !c->terrain) return self_;

            int idx = 0;
            int specArg = 0;
            if (!a.empty() && ev::isNumber(a[0])) {
                idx = static_cast<int>(ev::toDouble(a[0]));
                if (idx < 0 || idx >= scene::ClipmapTerrain::kMaxLayers)
                    return ev::throwRangeError("setSurfaceLayer: index " + std::to_string(idx) +
                                              " out of range [0, " + std::to_string(scene::ClipmapTerrain::kMaxLayers) + ")");
                specArg = 1;
            }

            if (static_cast<int>(a.size()) <= specArg || ev::isNull(a[specArg]) || ev::isUndefined(a[specArg])) {
                c->terrain->setSurfaceLayer(idx, nullptr, 0, 0, 0, 0, 1);
                return self_;
            }
            if (!ev::isObject(a[specArg]))
                return ev::throwTypeError("setSurfaceLayer: expected [index,] { data, width, height, originX, originZ, metresPerCell[, components] } or null");

            Value spec = a[specArg];
            Value wVal = ev::getProperty(spec, "width");
            Value hVal = ev::getProperty(spec, "height");
            int width = ev::isNumber(wVal) ? static_cast<int>(ev::toDouble(wVal)) : 0;
            int height = ev::isNumber(hVal) ? static_cast<int>(ev::toDouble(hVal)) : 0;
            Value oxVal = ev::getProperty(spec, "originX");
            Value ozVal = ev::getProperty(spec, "originZ");
            double originX = ev::isNumber(oxVal) ? ev::toDouble(oxVal) : 0.0;
            double originZ = ev::isNumber(ozVal) ? ev::toDouble(ozVal) : 0.0;
            Value mpcVal = ev::getProperty(spec, "metresPerCell");
            double mpc = ev::isNumber(mpcVal) ? ev::toDouble(mpcVal) : 1.0;
            Value cpVal = ev::getProperty(spec, "components");
            int comps = ev::isNumber(cpVal) ? static_cast<int>(ev::toDouble(cpVal)) : 3;

            if (width <= 0 || height <= 0)
                return ev::throwTypeError("setSurfaceLayer: width and height must be positive");
            if (!(mpc > 0.0))
                return ev::throwTypeError("setSurfaceLayer: metresPerCell must be positive");
            if (comps != 3 && comps != 4)
                return ev::throwRangeError("setSurfaceLayer: components must be 3 or 4, got " + std::to_string(comps));

            Value dataVal = ev::getProperty(spec, "data");
            auto info = ev::typedArrayInfo(dataVal);
            if (!info || !info.data)
                return ev::throwTypeError("setSurfaceLayer: data must be a Float32Array");

            const size_t want = static_cast<size_t>(width) * height * comps * sizeof(float);
            if (info.byteLength < want) {
                return ev::throwRangeError("setSurfaceLayer: data holds " + std::to_string(info.byteLength) +
                                           " bytes, need " + std::to_string(want) + " (" +
                                           std::to_string(width) + "x" + std::to_string(height) + "x" +
                                           std::to_string(comps) + " floats)");
            }

            c->terrain->setSurfaceLayer(idx, reinterpret_cast<const float*>(info.data),
                                       width, height, static_cast<float>(originX),
                                       static_cast<float>(originZ),
                                       static_cast<float>(mpc), comps);
            return self_;
        });

        b.def("update", 3, [](Value self_, std::span<const Value> a) -> Value {
            auto* c = clipmapCellOf(self_);
            if (!c || !c->terrain) return self_;
            if (a.size() < 3) return ev::throwTypeError("update(camX, camY, camZ) needs 3 numbers");
            float vx = ev::isNumber(a[0]) ? static_cast<float>(ev::toDouble(a[0])) : 0.0f;
            float vy = ev::isNumber(a[1]) ? static_cast<float>(ev::toDouble(a[1])) : 0.0f;
            float vz = ev::isNumber(a[2]) ? static_cast<float>(ev::toDouble(a[2])) : 0.0f;
            c->terrain->update(vx, vy, vz);
            return self_;
        });

        b.def("shaderSource", 1, [](Value self_, std::span<const Value> a) -> Value {
            auto* c = clipmapCellOf(self_);
            if (!c || !c->terrain || a.empty()) return ev::fromUtf8("");
            std::string stage = ev::isString(a[0]) ? ev::toUtf8(a[0]) : "";
            return ev::fromUtf8(c->terrain->shaderSource(stage));
        });

        b.def("elevationAt", 2, [](Value self_, std::span<const Value> a) -> Value {
            auto* c = clipmapCellOf(self_);
            if (!c || !c->terrain || a.size() < 2) return ev::fromDouble(0.0);
            float x = ev::isNumber(a[0]) ? static_cast<float>(ev::toDouble(a[0])) : 0.0f;
            float z = ev::isNumber(a[1]) ? static_cast<float>(ev::toDouble(a[1])) : 0.0f;
            return ev::fromDouble(c->terrain->elevationAt(x, z));
        });

        b.def("renderedElevationAt", 2, [](Value self_, std::span<const Value> a) -> Value {
            auto* c = clipmapCellOf(self_);
            if (!c || !c->terrain || a.size() < 2) return ev::fromDouble(0.0);
            float x = ev::isNumber(a[0]) ? static_cast<float>(ev::toDouble(a[0])) : 0.0f;
            float z = ev::isNumber(a[1]) ? static_cast<float>(ev::toDouble(a[1])) : 0.0f;
            return ev::fromDouble(c->terrain->renderedElevationAt(x, z));
        });

        b.def("destroy", 0, [](Value self_, std::span<const Value>) {
            auto* c = clipmapCellOf(self_);
            if (c && c->terrain) {
                c->terrain->destroy();
                c->terrain.reset();
            }
            return ev::undefined();
        });

        b.def("coverageDistance", 1, [](Value self_, std::span<const Value> a) -> Value {
            auto* c = clipmapCellOf(self_);
            if (!c || !c->terrain || a.empty()) return ev::fromDouble(0.0);
            float h = ev::isNumber(a[0]) ? static_cast<float>(ev::toDouble(a[0])) : 0.0f;
            return ev::fromDouble(c->terrain->coverageDistance(h));
        });

        b.def("horizonDistance", 1, [](Value self_, std::span<const Value> a) -> Value {
            auto* c = clipmapCellOf(self_);
            if (!c || !c->terrain || a.empty()) return ev::fromDouble(0.0);
            float h = ev::isNumber(a[0]) ? static_cast<float>(ev::toDouble(a[0])) : 0.0f;
            return ev::fromDouble(c->terrain->horizonDistance(h));
        });

        b.accessor("node", [](Value self_, std::span<const Value>) {
            auto* c = clipmapCellOf(self_);
            if (!c || !c->terrain) return ev::null();
            auto* g = c->graph();
            if (!g) return ev::null();
            return wrapSceneNode(c->terrain->node(), g);
        }, nullptr);

        b.accessor("levels", [](Value self_, std::span<const Value>) {
            auto* c = clipmapCellOf(self_);
            return ev::fromDouble(c && c->terrain ? c->terrain->levelCount() : 0);
        }, nullptr);

        b.accessor("resolution", [](Value self_, std::span<const Value>) {
            auto* c = clipmapCellOf(self_);
            return ev::fromDouble(c && c->terrain ? c->terrain->config().resolution : 0);
        }, nullptr);

        b.accessor("cellSize", [](Value self_, std::span<const Value>) {
            auto* c = clipmapCellOf(self_);
            return ev::fromDouble(c && c->terrain ? c->terrain->config().cellSize : 0.0);
        }, nullptr);

        b.accessor("layerCount", [](Value self_, std::span<const Value>) {
            auto* c = clipmapCellOf(self_);
            return ev::fromDouble(c && c->terrain ? c->terrain->layerCount() : 0);
        }, nullptr);

        b.accessor("triangleCount", [](Value self_, std::span<const Value>) {
            auto* c = clipmapCellOf(self_);
            return ev::fromDouble(c && c->terrain ? c->terrain->triangleCount() : 0);
        }, nullptr);

        b.accessor("vertexCount", [](Value self_, std::span<const Value>) {
            auto* c = clipmapCellOf(self_);
            return ev::fromDouble(c && c->terrain ? c->terrain->vertexCount() : 0);
        }, nullptr);

        b.accessor("farDistance", [](Value self_, std::span<const Value>) {
            auto* c = clipmapCellOf(self_);
            return ev::fromDouble(c && c->terrain ? static_cast<double>(c->terrain->farDistance()) : 0.0);
        }, nullptr);

        b.accessor("cellScale", [](Value self_, std::span<const Value>) {
            auto* c = clipmapCellOf(self_);
            return ev::fromDouble(c && c->terrain ? static_cast<double>(c->terrain->cellScale()) : 1.0);
        }, nullptr);

        b.accessor("planetRadius", [](Value self_, std::span<const Value>) {
            auto* c = clipmapCellOf(self_);
            return ev::fromDouble(c && c->terrain ? static_cast<double>(c->terrain->config().planetRadius) : 0.0);
        }, nullptr);
    });
}

Value wrapClipmap(std::unique_ptr<scene::ClipmapTerrain> terrain, scene::SceneGraph* graph) {
    if (!terrain) return ev::null();
    ensureClipmapClassInstalled();
    auto* cell = new HostClipmapCell();
    cell->token = graph ? graph->livenessToken() : std::weak_ptr<scene::SceneGraph::LivenessToken>();
    cell->terrain = std::move(terrain);
    return g_clipmapClass.make(cell, [](void* p) {
        auto* c = static_cast<HostClipmapCell*>(p);
        if (c) {
            if (c->terrain) {
                c->terrain->destroy();
                c->terrain.reset();
            }
            delete c;
        }
    });
}

void installSceneClipmap(ObjectBuilder& bGraph) {
    bGraph.def("createClipmapTerrain", 1, [](Value self_, std::span<const Value> a) -> Value {
        auto* g = sceneGraphOf(self_);
        if (!g) return ev::throwTypeError("createClipmapTerrain: no scene graph");

        scene::ClipmapConfig cfg;
        if (!a.empty() && ev::isObject(a[0])) {
            Value opts = a[0];
            Value levVal = ev::getProperty(opts, "levels");
            if (ev::isNumber(levVal)) cfg.levels = static_cast<int>(ev::toDouble(levVal));
            Value resVal = ev::getProperty(opts, "resolution");
            if (ev::isNumber(resVal)) cfg.resolution = static_cast<int>(ev::toDouble(resVal));
            Value csVal = ev::getProperty(opts, "cellSize");
            if (ev::isNumber(csVal)) cfg.cellSize = static_cast<float>(ev::toDouble(csVal));
            Value hsVal = ev::getProperty(opts, "heightScale");
            if (ev::isNumber(hsVal)) cfg.heightScale = static_cast<float>(ev::toDouble(hsVal));
            Value slVal = ev::getProperty(opts, "seaLevel");
            if (ev::isNumber(slVal)) cfg.seaLevel = static_cast<float>(ev::toDouble(slVal));
            Value snVal = ev::getProperty(opts, "snowLine");
            if (ev::isNumber(snVal)) cfg.snowLine = static_cast<float>(ev::toDouble(snVal));
            Value mcsVal = ev::getProperty(opts, "maxCellScale");
            if (ev::isNumber(mcsVal)) cfg.maxCellScale = static_cast<float>(ev::toDouble(mcsVal));
            Value prVal = ev::getProperty(opts, "planetRadius");
            if (ev::isNumber(prVal)) cfg.planetRadius = static_cast<float>(ev::toDouble(prVal));

            Value lfVal = ev::getProperty(opts, "layerFade");
            if (!ev::isUndefined(lfVal)) cfg.layerFade = ev::toBool(lfVal);
            Value cfVal = ev::getProperty(opts, "coverageFloor");
            if (!ev::isUndefined(cfVal)) cfg.coverageFloor = ev::toBool(cfVal);
            Value csfVal = ev::getProperty(opts, "cubicSurface");
            if (!ev::isUndefined(csfVal)) cfg.cubicSurface = ev::toBool(csfVal);
            Value chVal = ev::getProperty(opts, "cubicHeight");
            if (!ev::isUndefined(chVal)) cfg.cubicHeight = ev::toBool(chVal);

            Value dwVal = ev::getProperty(opts, "detailWavelength");
            if (ev::isNumber(dwVal)) cfg.detailWavelength = static_cast<float>(ev::toDouble(dwVal));
            Value drVal = ev::getProperty(opts, "detailRelief");
            if (ev::isNumber(drVal)) cfg.detailRelief = static_cast<float>(ev::toDouble(drVal));
            Value dgVal = ev::getProperty(opts, "detailGain");
            if (ev::isNumber(dgVal)) cfg.detailGain = static_cast<float>(ev::toDouble(dgVal));
            Value doVal = ev::getProperty(opts, "detailOctaves");
            if (ev::isNumber(doVal)) cfg.detailOctaves = static_cast<int>(ev::toDouble(doVal));
        }

        auto terrain = std::make_unique<scene::ClipmapTerrain>(*g, cfg);
        return wrapClipmap(std::move(terrain), g);
    });
}

} // namespace bro::bronze_host

#endif // BRO_WITH_3D
