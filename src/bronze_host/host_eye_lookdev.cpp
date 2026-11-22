#include "bronze_host/host_eye_creative.h"
#include "bronze_host/host_access.h"
#include "bronze_host/host_builder.h"
#include "bronze_host/host_globals_internal.h"
#include "bronze_host/host_runtime.h"
#include "bronze_host/host_values.h"

#if BRO_WITH_3D
#include "bronze_host/native_scene_internal.h"
#include "eye/eye_lookdev.h"
#include "eye/eye_trace.h"
#include "scene/scene_graph.h"
#endif

#include <algorithm>
#include <cmath>
#include <span>
#include <string>

namespace bro::bronze_host::eye_detail {

using namespace bro::eye;

static std::string strProp(Value obj, const char* key, const std::string& def = {}) {
    if (!ev::isObject(obj)) return def;
    Value v = ev::getProperty(obj, key);
    return ev::isString(v) ? ev::toUtf8(v) : def;
}

static double numProp(Value obj, const char* key, double def) {
    if (!ev::isObject(obj)) return def;
    Value v = ev::getProperty(obj, key);
    return (v.isNumber() && std::isfinite(v.asNumber())) ? v.asNumber() : def;
}

static bool boolProp(Value obj, const char* key, bool def) {
    if (!ev::isObject(obj)) return def;
    Value v = ev::getProperty(obj, key);
    return v.isBool() ? v.asBool() : def;
}

#if BRO_WITH_3D
static scene::SceneGraph* unwrapGraph(Value v) {
    if (!ev::isObject(v)) return nullptr;
    void* raw = ev::handleData(v);
    if (!raw) return nullptr;
    auto* cell = static_cast<HostSceneGraphCell*>(raw);
    return cell->tag == kHostSceneGraphTag ? cell->graph() : nullptr;
}
#endif

Value jsEyeLookdev(Value, std::span<const Value> a) {
#if !BRO_WITH_3D
    (void)a;
    return ev::throwError("bro.eye.lookdev requires a 3D build (BRO_WITH_3D=ON)");
#else
    if (!hostEngine()) return ev::throwError("bro.eye.lookdev is unavailable in Worker realms");
    scene::SceneGraph* graph = !a.empty() ? unwrapGraph(a[0]) : nullptr;
    if (!graph) return ev::throwTypeError("bro.eye.lookdev(scene, opts?) requires a live SceneGraph");

    EyeLookdevOptions lopts;
    Value optsVal = a.size() >= 2 ? a[1] : ev::undefined();
    ev::Persistent optsRoot(optsVal);
    if (ev::isObject(optsRoot.get())) {
        lopts.target = strProp(optsRoot.get(), "target", lopts.target);
        lopts.angle = strProp(optsRoot.get(), "angle", lopts.angle);
        lopts.isolate = boolProp(optsRoot.get(), "isolate", lopts.isolate);
        lopts.margin = std::clamp(numProp(optsRoot.get(), "margin", lopts.margin), 1.1, 8.0);
        lopts.sheetWidth = std::clamp(static_cast<int>(numProp(optsRoot.get(), "sheetWidth", lopts.sheetWidth)), 256, 1280);
        lopts.sheetHeight = std::clamp(static_cast<int>(numProp(optsRoot.get(), "sheetHeight", lopts.sheetHeight)), 256, 1280);
        const std::string rawPath = strProp(optsRoot.get(), "path");
        if (!rawPath.empty()) {
            requirePathAccess(rawPath, /*write=*/true);
            lopts.path = rawPath;
        }
    }

    const EyeLookdevResult r = captureLookdev(*graph, lopts);
    ObjectBuilder o;
    o.set("ok", ev::fromBool(r.ok));
    o.set("width", ev::fromDouble(r.width));
    o.set("height", ev::fromDouble(r.height));
    o.set("matchedNodes", ev::fromDouble(r.matchedNodes));
    o.set("roughnessMean", ev::fromDouble(r.roughnessMean));
    o.set("roughnessStd", ev::fromDouble(r.roughnessStd));
    o.set("glossyFrac", ev::fromDouble(r.glossyFrac));
    o.set("valueMean", ev::fromDouble(r.valueMean));
    o.set("valueStd", ev::fromDouble(r.valueStd));
    o.set("valueContrast", ev::fromDouble(r.valueContrast));
    o.set("albedoSaturationMean", ev::fromDouble(r.albedoSaturationMean));
    o.set("silhouetteEdgeDensity", ev::fromDouble(r.silhouetteEdgeDensity));
    o.set("clippedWhiteFrac", ev::fromDouble(r.clippedWhiteFrac));
    o.set("bloomHotspotCount", ev::fromDouble(r.bloomHotspotCount));
    o.set("checkedMeshes", ev::fromDouble(r.checkedMeshes));
    o.set("untexturedCount", ev::fromDouble(r.untexturedCount));
    o.set("noNormalMapCount", ev::fromDouble(r.noNormalMapCount));
    o.set("noShadowSolidCount", ev::fromDouble(r.noShadowSolidCount));
    o.set("materialWarnings", hostArrayOf(r.materialWarnings.size(), [&](size_t i) {
        const auto& mw = r.materialWarnings[i];
        ObjectBuilder mb;
        mb.set("name", ev::fromUtf8(mw.name));
        mb.set("hasAlbedoMap", ev::fromBool(mw.hasAlbedoMap));
        mb.set("hasNormalMap", ev::fromBool(mw.hasNormalMap));
        mb.set("hasMRMap", ev::fromBool(mw.hasMRMap));
        mb.set("castsShadow", ev::fromBool(mw.castsShadow));
        mb.set("translucent", ev::fromBool(mw.translucent));
        return mb.get();
    }));
    if (!r.visualPath.empty()) o.set("path", ev::fromUtf8(r.visualPath));
    return o.get();
#endif
}

Value jsEyeSweep(Value, std::span<const Value> a) {
#if !BRO_WITH_3D
    (void)a;
    return ev::throwError("bro.eye.sweep requires a 3D build (BRO_WITH_3D=ON)");
#else
    if (!hostEngine()) return ev::throwError("bro.eye.sweep is unavailable in Worker realms");
    scene::SceneGraph* graph = !a.empty() ? unwrapGraph(a[0]) : nullptr;
    if (!graph) return ev::throwTypeError("bro.eye.sweep(scene, opts?) requires a live SceneGraph");

    EyeSweepOptions sopts;
    std::vector<std::string> labels;
    std::vector<ev::Persistent> fns;

    Value optsVal = a.size() >= 2 ? a[1] : ev::undefined();
    ev::Persistent optsRoot(optsVal);
    if (ev::isObject(optsRoot.get())) {
        sopts.sheetWidth = std::clamp(static_cast<int>(numProp(optsRoot.get(), "sheetWidth", sopts.sheetWidth)), 256, 1280);
        const int reqH = static_cast<int>(numProp(optsRoot.get(), "sheetHeight", 0));
        sopts.sheetHeight = reqH > 0 ? std::clamp(reqH, 256, 1280) : 0;
        const std::string rawPath = strProp(optsRoot.get(), "path");
        if (!rawPath.empty()) {
            requirePathAccess(rawPath, /*write=*/true);
            sopts.path = rawPath;
        }
        Value stVal = ev::getProperty(optsRoot.get(), "states");
        if (ev::isObject(stVal)) {
            ev::Persistent sa(stVal);
            const uint32_t sn = static_cast<uint32_t>(std::max(0.0, std::min(ev::toDouble(ev::getProperty(sa.get(), "length")), 9.0)));
            for (uint32_t i = 0; i < sn; ++i) {
                Value item = ev::getElement(sa.get(), i);
                if (!ev::isObject(item)) continue;
                ev::Persistent si(item);
                std::string lbl = strProp(si.get(), "label", "state_" + std::to_string(i));
                Value fn = ev::getProperty(si.get(), "apply");
                if (ev::isFunction(fn)) {
                    labels.push_back(std::move(lbl));
                    fns.emplace_back(fn);
                }
            }
        }
    }

    std::vector<std::pair<std::string, std::function<void()>>> cppStates;
    cppStates.reserve(labels.size());
    for (size_t i = 0; i < labels.size(); ++i) {
        cppStates.emplace_back(labels[i], [&, i]() { (void)ev::call(fns[i].get(), ev::undefined(), {}); });
    }

    const EyeSweepResult r = captureStateSweep(*graph, sopts, cppStates);
    ObjectBuilder o;
    o.set("ok", ev::fromBool(r.ok));
    o.set("width", ev::fromDouble(r.width));
    o.set("height", ev::fromDouble(r.height));
    o.set("stateCount", ev::fromDouble(r.stateCount));
    o.set("states", hostArrayOf(r.states.size(), [&](size_t i) {
        const auto& st = r.states[i];
        ObjectBuilder sb;
        sb.set("label", ev::fromUtf8(st.label));
        sb.set("valueMean", ev::fromDouble(st.valueMean));
        sb.set("valueContrast", ev::fromDouble(st.valueContrast));
        sb.set("clippedWhiteFrac", ev::fromDouble(st.clippedWhiteFrac));
        sb.set("hotspotCount", ev::fromDouble(st.hotspotCount));
        return sb.get();
    }));
    if (!r.visualPath.empty()) o.set("path", ev::fromUtf8(r.visualPath));
    return o.get();
#endif
}

Value jsEyeTrace(Value, std::span<const Value> a) {
#if !BRO_WITH_3D
    (void)a;
    return ev::throwError("bro.eye.trace requires a 3D build (BRO_WITH_3D=ON)");
#else
    if (!hostEngine()) return ev::throwError("bro.eye.trace is unavailable in Worker realms");
    scene::SceneGraph* graph = !a.empty() ? unwrapGraph(a[0]) : nullptr;
    if (!graph) return ev::throwTypeError("bro.eye.trace(scene, opts?) requires a live SceneGraph");

    EyeTraceOptions topts;
    ev::Persistent stepFn;
    bool hasStepFn = false;

    Value optsVal = a.size() >= 2 ? a[1] : ev::undefined();
    ev::Persistent optsRoot(optsVal);
    if (ev::isObject(optsRoot.get())) {
        topts.steps = std::clamp(static_cast<int>(numProp(optsRoot.get(), "steps", topts.steps)), 2, 120);
        topts.waterY = numProp(optsRoot.get(), "waterY", topts.waterY);
        topts.minTravel = std::max(0.001, numProp(optsRoot.get(), "minTravel", topts.minTravel));
        topts.filter = strProp(optsRoot.get(), "filter", topts.filter);
        topts.sheetWidth = std::clamp(static_cast<int>(numProp(optsRoot.get(), "sheetWidth", topts.sheetWidth)), 128, 1280);
        topts.sheetHeight = std::clamp(static_cast<int>(numProp(optsRoot.get(), "sheetHeight", topts.sheetHeight)), 128, 1280);
        const std::string rawPath = strProp(optsRoot.get(), "path");
        if (!rawPath.empty()) {
            requirePathAccess(rawPath, /*write=*/true);
            topts.path = rawPath;
        }
        Value sf = ev::getProperty(optsRoot.get(), "stepFn");
        if (ev::isFunction(sf)) {
            stepFn = ev::Persistent(sf);
            hasStepFn = true;
        }
    }

    const EyeTraceResult r = traceSceneMotion(*graph, topts, [&](int k, int n) {
        if (!hasStepFn) return;
        Value args[2] = {ev::fromDouble(k), ev::fromDouble(n)};
        (void)ev::call(stepFn.get(), ev::undefined(), args);
    });

    ObjectBuilder o;
    o.set("ok", ev::fromBool(r.ok));
    o.set("steps", ev::fromDouble(r.steps));
    o.set("trackCount", ev::fromDouble(r.trackCount));
    o.set("width", ev::fromDouble(r.width));
    o.set("height", ev::fromDouble(r.height));
    o.set("tracks", hostArrayOf(r.tracks.size(), [&](size_t i) {
        const auto& tr = r.tracks[i];
        ObjectBuilder tb;
        tb.set("name", ev::fromUtf8(tr.name));
        tb.set("startPos", hostArrayOf(3, [&](size_t k) { return ev::fromDouble(tr.startPos[k]); }));
        tb.set("endPos", hostArrayOf(3, [&](size_t k) { return ev::fromDouble(tr.endPos[k]); }));
        tb.set("minY", ev::fromDouble(tr.minY));
        tb.set("maxY", ev::fromDouble(tr.maxY));
        tb.set("pathLength", ev::fromDouble(tr.pathLength));
        tb.set("maxStepJump", ev::fromDouble(tr.maxStepJump));
        tb.set("maxVelJump", ev::fromDouble(tr.maxVelJump));
        tb.set("maxRotSnapDeg", ev::fromDouble(tr.maxRotSnapDeg));
        tb.set("teleportCount", ev::fromDouble(tr.teleportCount));
        tb.set("snapCount", ev::fromDouble(tr.snapCount));
        tb.set("closedLoop", ev::fromBool(tr.closedLoop));
        tb.set("touchesWater", ev::fromBool(tr.touchesWater));
        tb.set("nearestStartStatic", ev::fromUtf8(tr.nearestStartStatic));
        tb.set("nearestStartDist", ev::fromDouble(tr.nearestStartDist));
        return tb.get();
    }));
    if (!r.visualPath.empty()) o.set("path", ev::fromUtf8(r.visualPath));
    return o.get();
#endif
}

}  // namespace bro::bronze_host::eye_detail
