#include "bronze_host/host_eye_creative.h"
#include "bronze_host/host_access.h"
#include "bronze_host/host_builder.h"
#include "bronze_host/host_globals_internal.h"
#include "bronze_host/host_runtime.h"
#include "bronze_host/host_values.h"

#if BRO_WITH_3D
#include "bronze_host/native_scene_internal.h"
#include "eye/eye_conformance.h"
#include "eye/eye_diagnose.h"
#include "scene/scene_graph.h"
#endif

#include <algorithm>
#include <cmath>
#include <span>
#include <string>
#include <vector>

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

Value jsEyeDiagnose(Value, std::span<const Value> a) {
#if !BRO_WITH_3D
    (void)a;
    return ev::throwError("bro.eye.diagnose requires a 3D build (BRO_WITH_3D=ON)");
#else
    if (!hostEngine()) return ev::throwError("bro.eye.diagnose is unavailable in Worker realms");
    scene::SceneGraph* graph = !a.empty() ? unwrapGraph(a[0]) : nullptr;
    if (!graph) return ev::throwTypeError("bro.eye.diagnose(scene, opts?) requires a live SceneGraph");

    EyeDiagnoseOptions dopts;
    Value optsVal = a.size() >= 2 ? a[1] : ev::undefined();
    ev::Persistent optsRoot(optsVal);
    if (ev::isObject(optsRoot.get())) {
        dopts.minPixels = std::clamp(static_cast<int>(numProp(optsRoot.get(), "minPixels", dopts.minPixels)), 1, 10000);
        dopts.alwaysRender = boolProp(optsRoot.get(), "alwaysRender", dopts.alwaysRender);
        dopts.sheetWidth = std::clamp(static_cast<int>(numProp(optsRoot.get(), "sheetWidth", dopts.sheetWidth)), 256, 1280);
        const int reqH = static_cast<int>(numProp(optsRoot.get(), "sheetHeight", 0));
        dopts.sheetHeight = reqH > 0 ? std::clamp(reqH, 256, 1280) : 0;
        const std::string rawPath = strProp(optsRoot.get(), "path");
        if (!rawPath.empty()) {
            requirePathAccess(rawPath, /*write=*/true);
            dopts.path = rawPath;
        }
    }

    const EyeDiagnoseResult r = diagnoseScene(*graph, dopts);
    ObjectBuilder o;
    o.set("ok", ev::fromBool(r.ok));
    o.set("width", ev::fromDouble(r.width));
    o.set("height", ev::fromDouble(r.height));
    o.set("visibleMeshes", ev::fromDouble(r.visibleMeshes));
    o.set("anomalyCount", ev::fromDouble(r.anomalyCount));
    o.set("bloomHotspotCount", ev::fromDouble(r.bloomHotspotCount));
    o.set("anomalies", hostArrayOf(r.anomalies.size(), [&](size_t i) {
        const auto& an = r.anomalies[i];
        ObjectBuilder ab;
        ab.set("kind", ev::fromUtf8(an.kind));
        ab.set("severity", ev::fromUtf8(an.severity));
        ab.set("nodeName", ev::fromUtf8(an.nodeName));
        ab.set("nodeId", ev::fromDouble(an.nodeId));
        ab.set("value", ev::fromDouble(an.value));
        ab.set("bbox", hostArrayOf(4, [&](size_t k) { return ev::fromDouble(an.bbox[k]); }));
        ab.set("message", ev::fromUtf8(an.message));
        return ab.get();
    }));
    o.set("meshes", hostArrayOf(r.meshes.size(), [&](size_t i) {
        const auto& ms = r.meshes[i];
        ObjectBuilder mb;
        mb.set("name", ev::fromUtf8(ms.name));
        mb.set("nodeId", ev::fromDouble(ms.nodeId));
        mb.set("instanced", ev::fromBool(ms.instanced));
        mb.set("instanceCount", ev::fromDouble(ms.instanceCount));
        mb.set("screenPx", ev::fromDouble(ms.screenPx));
        mb.set("screenFrac", ev::fromDouble(ms.screenFrac));
        mb.set("p10Lum", ev::fromDouble(ms.p10Lum));
        mb.set("p50Lum", ev::fromDouble(ms.p50Lum));
        mb.set("p95Lum", ev::fromDouble(ms.p95Lum));
        mb.set("p99Lum", ev::fromDouble(ms.p99Lum));
        mb.set("outlierBrightFrac", ev::fromDouble(ms.outlierBrightFrac));
        mb.set("roughnessMean", ev::fromDouble(ms.roughnessMean));
        mb.set("roughnessStd", ev::fromDouble(ms.roughnessStd));
        mb.set("albedoLumMean", ev::fromDouble(ms.albedoLumMean));
        mb.set("hueShiftDeg", ev::fromDouble(ms.hueShiftDeg));
        mb.set("chromaDelta", ev::fromDouble(ms.chromaDelta));
        mb.set("detailEntropy", ev::fromDouble(ms.detailEntropy));
        mb.set("normalStd", ev::fromDouble(ms.normalStd));
        mb.set("axisGridScore", ev::fromDouble(ms.axisGridScore));
        mb.set("hasAlbedoMap", ev::fromBool(ms.hasAlbedoMap));
        mb.set("hasNormalMap", ev::fromBool(ms.hasNormalMap));
        mb.set("hasMRMap", ev::fromBool(ms.hasMRMap));
        mb.set("castsShadow", ev::fromBool(ms.castsShadow));
        mb.set("translucent", ev::fromBool(ms.translucent));
        mb.set("striationScore", ev::fromDouble(ms.striationScore));
        return mb.get();
    }));
    if (!r.visualPath.empty()) o.set("path", ev::fromUtf8(r.visualPath));
    return o.get();
#endif
}

Value jsEyeConformance(Value, std::span<const Value> a) {
#if !BRO_WITH_3D
    (void)a;
    return ev::throwError("bro.eye.conformance requires a 3D build (BRO_WITH_3D=ON)");
#else
    if (!hostEngine()) return ev::throwError("bro.eye.conformance is unavailable in Worker realms");
    scene::SceneGraph* graph = !a.empty() ? unwrapGraph(a[0]) : nullptr;
    if (!graph) return ev::throwTypeError("bro.eye.conformance(scene, opts?) requires a live SceneGraph");

    EyeConformanceOptions copts;
    Value optsVal = a.size() >= 2 ? a[1] : ev::undefined();
    ev::Persistent optsRoot(optsVal);
    if (ev::isObject(optsRoot.get())) {
        copts.sheetWidth = std::clamp(static_cast<int>(numProp(optsRoot.get(), "sheetWidth", copts.sheetWidth)), 256, 1280);
        copts.sheetHeight = std::clamp(static_cast<int>(numProp(optsRoot.get(), "sheetHeight", copts.sheetHeight)), 192, 1280);
        const std::string rawPath = strProp(optsRoot.get(), "path");
        if (!rawPath.empty()) {
            requirePathAccess(rawPath, /*write=*/true);
            copts.path = rawPath;
        }
        Value pv = ev::getProperty(optsRoot.get(), "pairs");
        if (ev::isObject(pv)) {
            ev::Persistent pa(pv);
            const uint32_t pn = static_cast<uint32_t>(std::max(0.0, std::min(ev::toDouble(ev::getProperty(pa.get(), "length")), 32.0)));
            for (uint32_t i = 0; i < pn; ++i) {
                Value item = ev::getElement(pa.get(), i);
                if (!ev::isObject(item)) continue;
                ev::Persistent pi(item);
                EyeConformancePairSpec ps;
                ps.subject = strProp(pi.get(), "subject");
                ps.maxGap = std::max(0.001, numProp(pi.get(), "maxGap", ps.maxGap));
                ps.minGap = numProp(pi.get(), "minGap", ps.minGap);
                ps.maxSamples = std::clamp(static_cast<int>(numProp(pi.get(), "maxSamples", ps.maxSamples)), 8, 2048);
                Value sf = ev::getProperty(pi.get(), "surface");
                if (ev::isString(sf)) {
                    ps.surfaces.push_back(ev::toUtf8(sf));
                } else if (ev::isObject(sf)) {
                    ev::Persistent sa(sf);
                    const uint32_t sn = static_cast<uint32_t>(std::max(0.0, std::min(ev::toDouble(ev::getProperty(sa.get(), "length")), 32.0)));
                    for (uint32_t j = 0; j < sn; ++j) ps.surfaces.push_back(ev::toUtf8(ev::getElement(sa.get(), j)));
                }
                if (!ps.subject.empty() && !ps.surfaces.empty()) copts.pairs.push_back(std::move(ps));
            }
        }
    }

    const EyeConformanceResult r = auditConformance(*graph, copts);
    ObjectBuilder o;
    o.set("ok", ev::fromBool(r.ok));
    o.set("width", ev::fromDouble(r.width));
    o.set("height", ev::fromDouble(r.height));
    o.set("pairs", hostArrayOf(r.pairs.size(), [&](size_t i) {
        const auto& pr = r.pairs[i];
        ObjectBuilder pb;
        pb.set("subject", ev::fromUtf8(pr.subject));
        pb.set("surfaceCount", ev::fromDouble(pr.surfaceCount));
        pb.set("sampleCount", ev::fromDouble(pr.sampleCount));
        pb.set("minGap", ev::fromDouble(pr.minGap));
        pb.set("p50Gap", ev::fromDouble(pr.p50Gap));
        pb.set("p95Gap", ev::fromDouble(pr.p95Gap));
        pb.set("maxGap", ev::fromDouble(pr.maxGap));
        pb.set("airGapFrac", ev::fromDouble(pr.airGapFrac));
        pb.set("penetratingFrac", ev::fromDouble(pr.penetratingFrac));
        pb.set("worstPoint", hostArrayOf(3, [&](size_t k) { return ev::fromDouble(pr.worstPoint[k]); }));
        pb.set("ok", ev::fromBool(pr.ok));
        return pb.get();
    }));
    if (!r.visualPath.empty()) o.set("path", ev::fromUtf8(r.visualPath));
    return o.get();
#endif
}

}  // namespace bro::bronze_host::eye_detail
