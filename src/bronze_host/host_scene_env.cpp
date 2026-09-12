#if BRO_WITH_3D

#include "bronze_host/host_scene_internal.h"
#include "scene/scene_graph.h"
#include "util/asset_path.h"
#include <glad/gl.h>

namespace bro::bronze_host {

namespace {

double numAtProp(Value obj, const char* key, double defVal) {
    if (!ev::isObject(obj)) return defVal;
    Value v = ev::getProperty(obj, key);
    return ev::isNumber(v) ? ev::toDouble(v) : defVal;
}

bool boolAtProp(Value obj, const char* key, bool defVal) {
    if (!ev::isObject(obj)) return defVal;
    Value v = ev::getProperty(obj, key);
    return !ev::isUndefined(v) ? ev::toBool(v) : defVal;
}

std::string strAtProp(Value obj, const char* key, const std::string& defVal) {
    if (!ev::isObject(obj)) return defVal;
    Value v = ev::getProperty(obj, key);
    return ev::isString(v) ? ev::toUtf8(v) : defVal;
}

}  // namespace

void installSceneGraphEnv(ObjectBuilder& b) {
    b.def("setClearColor", 4, [](Value self_, std::span<const Value> a) {
        auto* g = sceneGraphOf(self_);
        if (!g || a.empty()) return ev::undefined();
        float cr = (float)ev::toDouble(a[0]);
        float cg = a.size() > 1 ? (float)ev::toDouble(a[1]) : cr;
        float cb = a.size() > 2 ? (float)ev::toDouble(a[2]) : cr;
        float ca = a.size() > 3 ? (float)ev::toDouble(a[3]) : 1.0f;
        glClearColor(cr, cg, cb, ca);
        return ev::undefined();
    });

    b.def("setAmbient", 3, [](Value self_, std::span<const Value> a) {
        auto* g = sceneGraphOf(self_);
        if (!g || a.empty()) return ev::undefined();
        float r = 0.03f, gg = 0.03f, bb = 0.03f;
        if (a.size() >= 3 && ev::isNumber(a[0]) && ev::isNumber(a[1]) && ev::isNumber(a[2])) {
            r = (float)ev::toDouble(a[0]);
            gg = (float)ev::toDouble(a[1]);
            bb = (float)ev::toDouble(a[2]);
        } else if (ev::isObject(a[0])) {
            Value lenV = ev::getProperty(a[0], "length");
            if (ev::isNumber(lenV)) {
                r = (float)ev::toDouble(ev::getElement(a[0], 0));
                gg = (float)ev::toDouble(ev::getElement(a[0], 1));
                bb = (float)ev::toDouble(ev::getElement(a[0], 2));
            } else {
                Value col = ev::getProperty(a[0], "color");
                if (ev::isObject(col)) {
                    r = (float)ev::toDouble(ev::getElement(col, 0));
                    gg = (float)ev::toDouble(ev::getElement(col, 1));
                    bb = (float)ev::toDouble(ev::getElement(col, 2));
                }
            }
        }
        g->setAmbient(r, gg, bb);
        return ev::undefined();
    });

    b.def("setEnvironment", 1, [](Value self_, std::span<const Value> a) {
        auto* g = sceneGraphOf(self_);
        if (!g) return ev::fromBool(false);
        if (a.empty() || ev::isNull(a[0]) || ev::isUndefined(a[0])) {
            g->clearEnvironment();
            return ev::fromBool(true);
        }
        if (!ev::isObject(a[0])) return ev::fromBool(false);

        Value opts = a[0];
        bool ok = true;
        Value hdrVal = ev::getProperty(opts, "hdr");
        if (ev::isString(hdrVal)) {
            std::string path = ev::toUtf8(hdrVal);
            if (!path.empty()) {
                ok = g->loadEnvironment(util::resolveAssetPath(path));
            } else {
                g->clearEnvironment();
            }
        }

        Value ivVal = ev::getProperty(opts, "intensity");
        if (ev::isNumber(ivVal)) {
            g->setEnvironmentIntensity((float)ev::toDouble(ivVal));
        }

        Value rotVal = ev::getProperty(opts, "rotation");
        if (ev::isNumber(rotVal)) {
            g->setEnvironmentRotation((float)ev::toDouble(rotVal));
        }

        return ev::fromBool(ok);
    });

    b.def("setFog", 1, [](Value self_, std::span<const Value> a) {
        auto* g = sceneGraphOf(self_);
        if (!g || a.empty() || !ev::isObject(a[0])) return ev::undefined();
        Value opts = a[0];
        float start = (float)numAtProp(opts, "start", 0.0);
        float end = (float)numAtProp(opts, "end", 0.0);
        bromath::Vec3 color{0.0f, 0.0f, 0.0f};
        readVec3FromValue(ev::getProperty(opts, "color"), color);
        float density = (float)numAtProp(opts, "density", 0.0);
        float heightFalloff = (float)numAtProp(opts, "heightFalloff", 0.0);
        float startDistance = (float)numAtProp(opts, "startDistance", 0.0);
        g->setFog(start, end, color.x, color.y, color.z);
        g->setFogExp(density, heightFalloff, startDistance);
        return ev::undefined();
    });

    b.def("setAtmosphere", 1, [](Value self_, std::span<const Value> a) {
        auto* g = sceneGraphOf(self_);
        if (!g || a.empty() || !ev::isObject(a[0])) return ev::undefined();
        Value opts = a[0];
        scene::AtmosphereParams p;
        p.enabled = boolAtProp(opts, "enabled", true);
        bromath::Vec3 sd{p.sunDir[0], p.sunDir[1], p.sunDir[2]};
        readVec3FromValue(ev::getProperty(opts, "sunDirection"), sd);
        p.sunDir[0] = sd.x; p.sunDir[1] = sd.y; p.sunDir[2] = sd.z;

        Value scProbe = ev::getProperty(opts, "sunColor");
        if (!ev::isUndefined(scProbe)) {
            p.sunColorExplicit = true;
            bromath::Vec3 sc{p.sunColor[0], p.sunColor[1], p.sunColor[2]};
            readVec3FromValue(scProbe, sc);
            p.sunColor[0] = sc.x; p.sunColor[1] = sc.y; p.sunColor[2] = sc.z;
        }

        bromath::Vec3 br{p.betaR[0], p.betaR[1], p.betaR[2]};
        readVec3FromValue(ev::getProperty(opts, "betaRayleigh"), br);
        p.betaR[0] = br.x; p.betaR[1] = br.y; p.betaR[2] = br.z;

        p.planetRadius = (float)numAtProp(opts, "planetRadius", p.planetRadius);
        p.thickness = (float)numAtProp(opts, "thickness", p.thickness);
        p.betaM = (float)numAtProp(opts, "betaMie", p.betaM);
        p.mieG = (float)numAtProp(opts, "mieG", p.mieG);
        p.scaleHeightR = (float)numAtProp(opts, "scaleHeightRayleigh", p.scaleHeightR);
        p.scaleHeightM = (float)numAtProp(opts, "scaleHeightMie", p.scaleHeightM);
        p.seaLevel = (float)numAtProp(opts, "seaLevel", p.seaLevel);
        p.multiScatter = (float)numAtProp(opts, "multiScatter", p.multiScatter);

        Value cProbe = ev::getProperty(opts, "center");
        bool hasCenter = !ev::isUndefined(cProbe);
        bromath::Vec3 cc{0, 0, 0};
        readVec3FromValue(cProbe, cc);
        p.center[0] = cc.x; p.center[1] = cc.y; p.center[2] = cc.z;
        p.spherical = boolAtProp(opts, "spherical", hasCenter);
        p.sunAngularRadius = (float)numAtProp(opts, "sunAngularRadius", p.sunAngularRadius);
        p.sunDiskIntensity = (float)numAtProp(opts, "sunDiskIntensity", p.sunDiskIntensity);

        g->setAtmosphere(p);
        return ev::undefined();
    });

    b.def("setStarfield", 1, [](Value self_, std::span<const Value> a) {
        auto* g = sceneGraphOf(self_);
        if (!g || a.empty() || !ev::isObject(a[0])) return ev::undefined();
        Value opts = a[0];
        scene::StarfieldParams s;
        s.enabled = boolAtProp(opts, "enabled", true);
        s.intensity = (float)numAtProp(opts, "intensity", s.intensity);
        s.density = (float)numAtProp(opts, "density", s.density);
        s.rotation = (float)numAtProp(opts, "rotation", s.rotation);
        g->setStarfield(s);
        return ev::undefined();
    });

    b.def("setToneMap", 1, [](Value self_, std::span<const Value> a) {
        auto* g = sceneGraphOf(self_);
        if (!g || a.empty() || !ev::isObject(a[0])) return ev::undefined();
        Value opts = a[0];
        std::string modeStr = strAtProp(opts, "mode", "aces");
        scene::SceneGraph::ToneMap mode = scene::SceneGraph::ToneMap::ACES;
        if (modeStr == "linear") mode = scene::SceneGraph::ToneMap::Linear;
        else if (modeStr == "reinhard") mode = scene::SceneGraph::ToneMap::Reinhard;
        float exposure = (float)numAtProp(opts, "exposure", 1.0);
        float gamma = (float)numAtProp(opts, "gamma", 2.2);
        g->setToneMap(mode, exposure, gamma);
        return ev::undefined();
    });

    b.def("setBloom", 1, [](Value self_, std::span<const Value> a) {
        auto* g = sceneGraphOf(self_);
        if (!g || a.empty() || !ev::isObject(a[0])) return ev::undefined();
        Value opts = a[0];
        bool enabled = boolAtProp(opts, "enabled", false);
        float threshold = (float)numAtProp(opts, "threshold", 1.0);
        float intensity = (float)numAtProp(opts, "intensity", 0.6);
        float strength = (float)numAtProp(opts, "strength", 2.0);
        g->setBloom(enabled, threshold, intensity, strength);
        return ev::undefined();
    });

    b.def("setSSAO", 1, [](Value self_, std::span<const Value> a) {
        auto* g = sceneGraphOf(self_);
        if (!g || a.empty() || !ev::isObject(a[0])) return ev::undefined();
        Value opts = a[0];
        bool enabled = boolAtProp(opts, "enabled", false);
        float radius = (float)numAtProp(opts, "radius", 0.5);
        float intensity = (float)numAtProp(opts, "intensity", 1.0);
        float bias = (float)numAtProp(opts, "bias", 0.025);
        g->setSSAO(enabled, radius, intensity, bias);
        return ev::undefined();
    });

    b.def("setSSR", 1, [](Value self_, std::span<const Value> a) {
        auto* g = sceneGraphOf(self_);
        if (!g || a.empty() || !ev::isObject(a[0])) return ev::undefined();
        Value opts = a[0];
        bool enabled = boolAtProp(opts, "enabled", false);
        float maxDistance = (float)numAtProp(opts, "maxDistance", 30.0);
        int steps = static_cast<int>(numAtProp(opts, "steps", 48.0));
        float thickness = (float)numAtProp(opts, "thickness", 0.3);
        float intensity = (float)numAtProp(opts, "intensity", 1.0);
        float edgeFade = (float)numAtProp(opts, "edgeFade", 0.1);
        g->setSSR(enabled, maxDistance, steps, thickness, intensity, edgeFade);
        return ev::undefined();
    });

    b.def("setDepthOfField", 1, [](Value self_, std::span<const Value> a) {
        auto* g = sceneGraphOf(self_);
        if (!g || a.empty() || !ev::isObject(a[0])) return ev::undefined();
        Value opts = a[0];
        bool enabled = boolAtProp(opts, "enabled", false);
        float focusDistance = (float)numAtProp(opts, "focusDistance", 10.0);
        float focusRange = (float)numAtProp(opts, "focusRange", 5.0);
        float maxBlur = (float)numAtProp(opts, "maxBlur", 4.0);
        g->setDepthOfField(enabled, focusDistance, focusRange, maxBlur);
        return ev::undefined();
    });

    b.def("setColorLUT", 1, [](Value self_, std::span<const Value> a) {
        auto* g = sceneGraphOf(self_);
        if (!g) return ev::undefined();
        if (a.empty() || ev::isNull(a[0]) || ev::isUndefined(a[0])) {
            g->clearColorLUT();
            return ev::fromBool(true);
        }
        if (!ev::isObject(a[0])) return ev::fromBool(false);
        Value opts = a[0];
        std::string path = strAtProp(opts, "path", "");
        if (path.empty()) {
            g->clearColorLUT();
            return ev::fromBool(true);
        }
        int size = static_cast<int>(numAtProp(opts, "size", 0.0));
        float amount = (float)numAtProp(opts, "amount", 1.0);
        bool ok = g->loadColorLUT(util::resolveAssetPath(path), size, amount);
        return ev::fromBool(ok);
    });

    b.def("setFXAA", 1, [](Value self_, std::span<const Value> a) {
        auto* g = sceneGraphOf(self_);
        if (!g || a.empty()) return ev::undefined();
        bool enabled = false;
        if (ev::isObject(a[0])) {
            enabled = boolAtProp(a[0], "enabled", false);
        } else {
            enabled = ev::toBool(a[0]);
        }
        g->setFXAA(enabled);
        return ev::undefined();
    });

    b.def("setShadowQuality", 2, [](Value self_, std::span<const Value> a) {
        auto* g = sceneGraphOf(self_);
        if (!g || a.empty()) return ev::undefined();
        int atlasSize = 4096, pcfTaps = 3;
        if (ev::isObject(a[0])) {
            atlasSize = static_cast<int>(numAtProp(a[0], "atlasSize", 4096.0));
            pcfTaps = static_cast<int>(numAtProp(a[0], "pcfTaps", 3.0));
        } else {
            if (ev::isNumber(a[0])) atlasSize = static_cast<int>(ev::toDouble(a[0]));
            if (a.size() > 1 && ev::isNumber(a[1])) pcfTaps = static_cast<int>(ev::toDouble(a[1]));
        }
        g->setShadowQuality(atlasSize, pcfTaps);
        return ev::undefined();
    });

    b.def("setShadowCache", 1, [](Value self_, std::span<const Value> a) {
        auto* g = sceneGraphOf(self_);
        if (!g || a.empty()) return ev::undefined();
        bool enabled = true;
        if (ev::isObject(a[0])) {
            enabled = boolAtProp(a[0], "enabled", true);
        } else {
            enabled = ev::toBool(a[0]);
        }
        g->setShadowCache(enabled);
        return ev::undefined();
    });

    b.accessor("shadowCache",
        [](Value self_, std::span<const Value>) {
            auto* g = sceneGraphOf(self_);
            return ev::fromBool(g ? g->shadowCache() : true);
        },
        [](Value self_, std::span<const Value> a) {
            auto* g = sceneGraphOf(self_);
            if (g && !a.empty()) g->setShadowCache(ev::toBool(a[0]));
            return ev::undefined();
        });

    b.def("setWind", 1, [](Value self_, std::span<const Value> a) {
        auto* g = sceneGraphOf(self_);
        if (!g || a.empty() || !ev::isObject(a[0])) return ev::undefined();
        Value opts = a[0];
        bromath::Vec3 d{1.0f, 0.0f, 0.0f};
        readVec3FromValue(ev::getProperty(opts, "direction"), d);
        float strength = (float)numAtProp(opts, "strength", 0.0);
        float frequency = (float)numAtProp(opts, "frequency", 1.5);
        g->setWind(d.x, d.y, d.z, strength, frequency);
        return ev::undefined();
    });

    b.def("setRenderScale", 1, [](Value self_, std::span<const Value> a) {
        auto* g = sceneGraphOf(self_);
        if (g && !a.empty() && ev::isNumber(a[0])) {
            g->setRenderScale((float)ev::toDouble(a[0]));
        }
        return ev::undefined();
    });

    b.accessor("renderScale",
        [](Value self_, std::span<const Value>) {
            auto* g = sceneGraphOf(self_);
            return ev::fromDouble(g ? g->renderScale() : 1.0);
        },
        [](Value self_, std::span<const Value> a) {
            auto* g = sceneGraphOf(self_);
            if (g && !a.empty() && ev::isNumber(a[0])) {
                g->setRenderScale((float)ev::toDouble(a[0]));
            }
            return ev::undefined();
        });

    b.def("setMSAA", 1, [](Value self_, std::span<const Value> a) {
        auto* g = sceneGraphOf(self_);
        if (g && !a.empty() && ev::isNumber(a[0])) {
            g->setMSAA(static_cast<int>(ev::toDouble(a[0])));
        }
        return ev::undefined();
    });

    b.accessor("msaa",
        [](Value self_, std::span<const Value>) {
            auto* g = sceneGraphOf(self_);
            return ev::fromDouble(g ? g->msaa() : 0.0);
        },
        [](Value self_, std::span<const Value> a) {
            auto* g = sceneGraphOf(self_);
            if (g && !a.empty() && ev::isNumber(a[0])) {
                g->setMSAA(static_cast<int>(ev::toDouble(a[0])));
            }
            return ev::undefined();
        });

    b.accessor("showLightIcons",
        [](Value self_, std::span<const Value>) {
            auto* g = sceneGraphOf(self_);
            return ev::fromBool(g ? g->showLightIcons() : false);
        },
        [](Value self_, std::span<const Value> a) {
            auto* g = sceneGraphOf(self_);
            if (g && !a.empty()) g->setShowLightIcons(ev::toBool(a[0]));
            return ev::undefined();
        });

    b.accessor("cameraX",
        [](Value self_, std::span<const Value>) {
            auto* g = sceneGraphOf(self_);
            return ev::fromDouble(g ? g->cameraX() : 0.0);
        },
        [](Value self_, std::span<const Value> a) {
            auto* g = sceneGraphOf(self_);
            if (g && !a.empty()) g->setCameraPosition((float)ev::toDouble(a[0]), g->cameraY());
            return ev::undefined();
        });

    b.accessor("cameraY",
        [](Value self_, std::span<const Value>) {
            auto* g = sceneGraphOf(self_);
            return ev::fromDouble(g ? g->cameraY() : 0.0);
        },
        [](Value self_, std::span<const Value> a) {
            auto* g = sceneGraphOf(self_);
            if (g && !a.empty()) g->setCameraPosition(g->cameraX(), (float)ev::toDouble(a[0]));
            return ev::undefined();
        });

    b.accessor("cameraZoom",
        [](Value self_, std::span<const Value>) {
            auto* g = sceneGraphOf(self_);
            return ev::fromDouble(g ? g->cameraZoom() : 1.0);
        },
        [](Value self_, std::span<const Value> a) {
            auto* g = sceneGraphOf(self_);
            if (g && !a.empty()) g->setCameraZoom((float)ev::toDouble(a[0]));
            return ev::undefined();
        });
}

}  // namespace bro::bronze_host

#endif  // BRO_WITH_3D
