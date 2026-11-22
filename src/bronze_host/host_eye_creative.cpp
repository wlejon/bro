#include "bronze_host/host_eye_creative.h"
#include "bronze_host/host_access.h"
#include "bronze_host/host_builder.h"
#include "bronze_host/host_globals_internal.h"
#include "bronze_host/host_runtime.h"
#include "bronze_host/host_values.h"

#include "eye/eye_motion.h"
#if BRO_WITH_3D
#include "bronze_host/native_scene_internal.h"
#include "eye/eye_clearance.h"
#include "eye/eye_closeup.h"
#include "eye/eye_probe.h"
#include "eye/eye_structure.h"
#include "scene/scene_graph.h"
#endif

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace bro::bronze_host::eye_detail {

using namespace bro::eye;

bool extractImage(Value v, EyeImage& out, bool allowLiveCapture, std::string& err);

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

Value jsEyeMotion(Value, std::span<const Value> a) {
    if (a.empty() || !ev::isObject(a[0])) {
        return ev::throwTypeError("bro.eye.motion(frames, opts?) requires an array of frames");
    }
    ev::Persistent arr(a[0]);
    const double lenD = ev::toDouble(ev::getProperty(arr.get(), "length"));
    const uint32_t n = std::isfinite(lenD) && lenD > 0.0 ? static_cast<uint32_t>(std::min(lenD, 120.0)) : 0;
    if (n < 2) {
        return ev::throwTypeError("bro.eye.motion requires at least 2 frames");
    }

    std::vector<EyeImage> owned;
    owned.reserve(n);
    std::string err;
    for (uint32_t i = 0; i < n; ++i) {
        EyeImage img;
        if (!extractImage(ev::getElement(arr.get(), i), img, /*allowLiveCapture=*/false, err)) {
            return ev::throwTypeError("bro.eye.motion frame " + std::to_string(i) + ": " + err);
        }
        owned.push_back(std::move(img));
    }

    EyeMotionOptions mopts;
    Value optsVal = a.size() >= 2 ? a[1] : ev::undefined();
    ev::Persistent optsRoot(optsVal);
    if (ev::isObject(optsRoot.get())) {
        mopts.threshold = std::max(0.1, numProp(optsRoot.get(), "threshold", mopts.threshold));
        mopts.maxPanelDim = std::clamp(static_cast<int>(numProp(optsRoot.get(), "maxPanelDim", mopts.maxPanelDim)), 64, 2048);
        mopts.layout = strProp(optsRoot.get(), "layout", mopts.layout);
        mopts.visualStyle = strProp(optsRoot.get(), "visualStyle", mopts.visualStyle);
        mopts.slitX = numProp(optsRoot.get(), "slitX", mopts.slitX);
        mopts.slitY = numProp(optsRoot.get(), "slitY", mopts.slitY);
        mopts.buildVisual = boolProp(optsRoot.get(), "buildVisual", false);
        const std::string rawPath = strProp(optsRoot.get(), "path");
        if (!rawPath.empty()) {
            requirePathAccess(rawPath, /*write=*/true);
            mopts.path = rawPath;
        }
        Value lblVal = ev::getProperty(optsRoot.get(), "labels");
        if (ev::isObject(lblVal)) {
            ev::Persistent la(lblVal);
            const uint32_t ln = static_cast<uint32_t>(std::max(0.0, std::min(ev::toDouble(ev::getProperty(la.get(), "length")), 240.0)));
            for (uint32_t i = 0; i < ln; ++i) mopts.labels.push_back(ev::toUtf8(ev::getElement(la.get(), i)));
        }
        Value tsVal = ev::getProperty(optsRoot.get(), "timestampsMs");
        if (ev::isObject(tsVal)) {
            ev::Persistent ta(tsVal);
            const uint32_t tn = static_cast<uint32_t>(std::max(0.0, std::min(ev::toDouble(ev::getProperty(ta.get(), "length")), 240.0)));
            mopts.timestampsMs.reserve(tn);
            for (uint32_t i = 0; i < tn; ++i) mopts.timestampsMs.push_back(ev::toDouble(ev::getElement(ta.get(), i)));
        }
    }

    std::vector<EyeImageView> views;
    views.reserve(owned.size());
    for (const auto& im : owned) views.push_back(im.view());

    const EyeMotionResult r = analyzeMotion(views, mopts);
    ObjectBuilder o;
    o.set("width", ev::fromDouble(r.width));
    o.set("height", ev::fromDouble(r.height));
    o.set("frameCount", ev::fromDouble(r.frameCount));
    o.set("staticPairCount", ev::fromDouble(r.staticPairCount));
    o.set("stutterCount", ev::fromDouble(r.stutterCount));
    o.set("popCount", ev::fromDouble(r.popCount));
    o.set("hiccupCount", ev::fromDouble(r.hiccupCount));
    o.set("meanDeltaE", ev::fromDouble(r.meanDeltaE));
    o.set("deltaEStd", ev::fromDouble(r.deltaEStd));
    o.set("smoothness", ev::fromDouble(r.smoothness));
    o.set("lumaMin", ev::fromDouble(r.lumaMin));
    o.set("lumaMax", ev::fromDouble(r.lumaMax));
    o.set("lumaRange", ev::fromDouble(r.lumaRange));
    o.set("flickerScore", ev::fromDouble(r.flickerScore));
    o.set("motionCoverage", ev::fromDouble(r.motionCoverage));
    o.set("dtMedianMs", ev::fromDouble(r.dtMedianMs));
    o.set("dtP95Ms", ev::fromDouble(r.dtP95Ms));
    o.set("dtMaxMs", ev::fromDouble(r.dtMaxMs));
    o.set("fpsMean", ev::fromDouble(r.fpsMean));
    o.set("autoSlitX", ev::fromDouble(r.autoSlitX));
    o.set("autoSlitY", ev::fromDouble(r.autoSlitY));
    {
        ObjectBuilder mb;
        mb.set("x", ev::fromDouble(r.motionBounds.x));
        mb.set("y", ev::fromDouble(r.motionBounds.y));
        mb.set("width", ev::fromDouble(r.motionBounds.w));
        mb.set("height", ev::fromDouble(r.motionBounds.h));
        o.set("motionBounds", mb.get());
    }
    o.set("steps", hostArrayOf(r.steps.size(), [&](size_t i) {
        const auto& s = r.steps[i];
        ObjectBuilder sb;
        sb.set("fromIndex", ev::fromDouble(s.fromIndex));
        sb.set("toIndex", ev::fromDouble(s.toIndex));
        sb.set("deltaEMean", ev::fromDouble(s.deltaEMean));
        sb.set("deltaEP95", ev::fromDouble(s.deltaEP95));
        sb.set("changedRatio", ev::fromDouble(s.changedRatio));
        sb.set("ssim", ev::fromDouble(s.ssim));
        sb.set("lumaDiff", ev::fromDouble(s.lumaDiff));
        sb.set("dtMs", ev::fromDouble(s.dtMs));
        sb.set("centroidX", ev::fromDouble(s.centroidX));
        sb.set("centroidY", ev::fromDouble(s.centroidY));
        sb.set("staticStep", ev::fromBool(s.staticStep));
        sb.set("stutter", ev::fromBool(s.stutter));
        sb.set("pop", ev::fromBool(s.pop));
        sb.set("hiccup", ev::fromBool(s.hiccup));
        return sb.get();
    }));
    if (!r.visualPath.empty()) o.set("path", ev::fromUtf8(r.visualPath));
    return o.get();
}

Value jsEyeProbe(Value, std::span<const Value> a) {
#if !BRO_WITH_3D
    return ev::throwError("bro.eye.probe requires a 3D build (BRO_WITH_3D=ON)");
#else
    if (!hostEngine()) return ev::throwError("bro.eye.probe is unavailable in Worker realms");
    if (a.empty() || !ev::isObject(a[0])) {
        return ev::throwTypeError("bro.eye.probe(scene, opts?) requires a SceneGraph");
    }
    scene::SceneGraph* graph = nullptr;
    if (void* raw = ev::handleData(a[0])) {
        auto* cell = static_cast<HostSceneGraphCell*>(raw);
        if (cell->tag == kHostSceneGraphTag) graph = cell->graph();
    }
    if (!graph) {
        return ev::throwTypeError("bro.eye.probe(scene, opts?): expected a live SceneGraph");
    }

    EyeProbeOptions popts;
    Value optsVal = a.size() >= 2 ? a[1] : ev::undefined();
    ev::Persistent optsRoot(optsVal);
    if (ev::isObject(optsRoot.get())) {
        popts.render = boolProp(optsRoot.get(), "render", popts.render);
        popts.gridCols = std::clamp(static_cast<int>(numProp(optsRoot.get(), "gridCols", popts.gridCols)), 1, 16);
        popts.gridRows = std::clamp(static_cast<int>(numProp(optsRoot.get(), "gridRows", popts.gridRows)), 1, 16);
        popts.maxPanelDim = std::clamp(static_cast<int>(numProp(optsRoot.get(), "maxPanelDim", popts.maxPanelDim)), 128, 2048);
        const std::string rawPath = strProp(optsRoot.get(), "path");
        if (!rawPath.empty()) {
            requirePathAccess(rawPath, /*write=*/true);
            popts.path = rawPath;
        }
        Value ptsVal = ev::getProperty(optsRoot.get(), "points");
        if (ev::isObject(ptsVal)) {
            ev::Persistent pa(ptsVal);
            const uint32_t pn = static_cast<uint32_t>(std::max(0.0, std::min(ev::toDouble(ev::getProperty(pa.get(), "length")), 256.0)));
            for (uint32_t i = 0; i < pn; ++i) {
                Value item = ev::getElement(pa.get(), i);
                if (!ev::isObject(item)) continue;
                ev::Persistent pi(item);
                EyeProbePoint pt;
                pt.x = numProp(pi.get(), "x", 0.5);
                pt.y = numProp(pi.get(), "y", 0.5);
                pt.normalized = boolProp(pi.get(), "normalized", pt.x >= 0.0 && pt.x <= 1.0 && pt.y >= 0.0 && pt.y <= 1.0);
                pt.label = strProp(pi.get(), "label");
                popts.points.push_back(std::move(pt));
            }
        }
        Value pairsVal = ev::getProperty(optsRoot.get(), "pairs");
        if (ev::isObject(pairsVal)) {
            ev::Persistent pra(pairsVal);
            const uint32_t prn = static_cast<uint32_t>(std::max(0.0, std::min(ev::toDouble(ev::getProperty(pra.get(), "length")), 64.0)));
            for (uint32_t i = 0; i < prn; ++i) {
                Value pairEl = ev::getElement(pra.get(), i);
                if (!ev::isObject(pairEl)) continue;
                ev::Persistent pe(pairEl);
                popts.pairs.emplace_back(ev::toUtf8(ev::getElement(pe.get(), 0)),
                                         ev::toUtf8(ev::getElement(pe.get(), 1)));
            }
        }
    }

    const EyeProbeResult r = probeScene(*graph, popts);
    ObjectBuilder o;
    o.set("width", ev::fromDouble(r.width));
    o.set("height", ev::fromDouble(r.height));
    o.set("samples", hostArrayOf(r.samples.size(), [&](size_t i) {
        const auto& s = r.samples[i];
        ObjectBuilder sb;
        sb.set("index", ev::fromDouble(s.index));
        sb.set("label", ev::fromUtf8(s.label));
        sb.set("px", ev::fromDouble(s.px));
        sb.set("py", ev::fromDouble(s.py));
        sb.set("normX", ev::fromDouble(s.normX));
        sb.set("normY", ev::fromDouble(s.normY));
        sb.set("hit", ev::fromBool(s.hit));
        sb.set("nodeName", ev::fromUtf8(s.nodeName));
        sb.set("nodeId", ev::fromDouble(static_cast<double>(s.nodeId)));
        sb.set("point", hostArrayOf(3, [&](size_t k) { return ev::fromDouble(s.point[k]); }));
        sb.set("normal", hostArrayOf(3, [&](size_t k) { return ev::fromDouble(s.normal[k]); }));
        sb.set("distance", ev::fromDouble(s.distance));
        {
            ObjectBuilder cb;
            cb.set("r", ev::fromDouble(s.r));
            cb.set("g", ev::fromDouble(s.g));
            cb.set("b", ev::fromDouble(s.b));
            cb.set("hex", ev::fromUtf8(s.hex));
            cb.set("L", ev::fromDouble(s.L));
            cb.set("C", ev::fromDouble(s.C));
            cb.set("h", ev::fromDouble(s.h));
            sb.set("color", cb.get());
        }
        return sb.get();
    }));
    o.set("distances", hostArrayOf(r.distances.size(), [&](size_t i) {
        const auto& d = r.distances[i];
        ObjectBuilder db;
        db.set("a", ev::fromUtf8(d.a));
        db.set("b", ev::fromUtf8(d.b));
        db.set("distance", ev::fromDouble(d.distance));
        db.set("dx", ev::fromDouble(d.dx));
        db.set("dy", ev::fromDouble(d.dy));
        db.set("dz", ev::fromDouble(d.dz));
        return db.get();
    }));
    if (!r.visualPath.empty()) o.set("path", ev::fromUtf8(r.visualPath));
    return o.get();
#endif
}

Value jsEyeClearance(Value, std::span<const Value> a) {
#if !BRO_WITH_3D
    (void)a;
    return ev::throwError("bro.eye.clearance requires a 3D build (BRO_WITH_3D=ON)");
#else
    if (!hostEngine()) return ev::throwError("bro.eye.clearance is unavailable in Worker realms");
    if (a.empty() || !ev::isObject(a[0])) {
        return ev::throwTypeError("bro.eye.clearance(scene, opts?) requires a SceneGraph");
    }
    scene::SceneGraph* graph = nullptr;
    if (void* raw = ev::handleData(a[0])) {
        auto* cell = static_cast<HostSceneGraphCell*>(raw);
        if (cell->tag == kHostSceneGraphTag) graph = cell->graph();
    }
    if (!graph) {
        return ev::throwTypeError("bro.eye.clearance(scene, opts?): expected a live SceneGraph");
    }

    EyeClearanceOptions copts;
    bool hasStepFn = false;
    ev::Persistent stepFnRoot;
    Value optsVal = a.size() >= 2 ? a[1] : ev::undefined();
    ev::Persistent optsRoot(optsVal);
    if (ev::isObject(optsRoot.get())) {
        copts.eps = std::clamp(numProp(optsRoot.get(), "eps", copts.eps), 1e-5, 0.5);
        copts.steps = std::clamp(static_cast<int>(numProp(optsRoot.get(), "steps", copts.steps)), 1, 180);
        copts.ignoreStatic = boolProp(optsRoot.get(), "ignoreStatic", copts.ignoreStatic);
        copts.ignoreSameParent = boolProp(optsRoot.get(), "ignoreSameParent", copts.ignoreSameParent);
        copts.maxPanelDim = std::clamp(static_cast<int>(numProp(optsRoot.get(), "maxPanelDim", copts.maxPanelDim)), 128, 2048);
        const std::string rawPath = strProp(optsRoot.get(), "path");
        if (!rawPath.empty()) {
            requirePathAccess(rawPath, /*write=*/true);
            copts.path = rawPath;
        }
        Value sf = ev::getProperty(optsRoot.get(), "stepFn");
        if (ev::isFunction(sf)) {
            hasStepFn = true;
            stepFnRoot = ev::Persistent(sf);
        }
        Value ipVal = ev::getProperty(optsRoot.get(), "ignorePairs");
        if (ev::isObject(ipVal)) {
            ev::Persistent ipa(ipVal);
            const uint32_t ipn = static_cast<uint32_t>(std::max(0.0, std::min(ev::toDouble(ev::getProperty(ipa.get(), "length")), 128.0)));
            for (uint32_t i = 0; i < ipn; ++i) {
                Value pairEl = ev::getElement(ipa.get(), i);
                if (!ev::isObject(pairEl)) continue;
                ev::Persistent pe(pairEl);
                copts.ignorePairs.emplace_back(ev::toUtf8(ev::getElement(pe.get(), 0)),
                                               ev::toUtf8(ev::getElement(pe.get(), 1)));
            }
        }
    }

    std::function<void(int, int)> stepCb;
    if (hasStepFn) {
        stepCb = [&](int k, int n) {
            Value args[2] = {ev::fromDouble(k), ev::fromDouble(n)};
            (void)ev::call(stepFnRoot.get(), ev::undefined(), args);
        };
    }

    const EyeClearanceResult r = analyzeClearance(*graph, copts, stepCb);
    ObjectBuilder o;
    o.set("ok", ev::fromBool(r.ok));
    o.set("posesChecked", ev::fromDouble(r.posesChecked));
    o.set("totalMeshPairs", ev::fromDouble(r.totalMeshPairs));
    o.set("staticCollisionCount", ev::fromDouble(r.staticCollisionCount));
    o.set("dynamicCollisionCount", ev::fromDouble(r.dynamicCollisionCount));
    o.set("worstPoseIndex", ev::fromDouble(r.worstPoseIndex));
    o.set("collisions", hostArrayOf(r.collisions.size(), [&](size_t i) {
        const auto& c = r.collisions[i];
        ObjectBuilder cb;
        cb.set("nodeA", ev::fromUtf8(c.nodeA));
        cb.set("nodeB", ev::fromUtf8(c.nodeB));
        cb.set("hitTriangles", ev::fromDouble(c.hitTriangles));
        cb.set("poseIndex", ev::fromDouble(c.poseIndex));
        cb.set("centroid", hostArrayOf(3, [&](size_t k) { return ev::fromDouble(c.centroid[k]); }));
        cb.set("overlapDepth", ev::fromDouble(c.overlapDepth));
        cb.set("dynamic", ev::fromBool(c.dynamic));
        return cb.get();
    }));
    if (!r.visualPath.empty()) o.set("path", ev::fromUtf8(r.visualPath));
    return o.get();
#endif
}

Value jsEyeStructure(Value, std::span<const Value> a) {
#if !BRO_WITH_3D
    (void)a;
    return ev::throwError("bro.eye.structure requires a 3D build (BRO_WITH_3D=ON)");
#else
    if (!hostEngine()) return ev::throwError("bro.eye.structure is unavailable in Worker realms");
    if (a.empty() || !ev::isObject(a[0])) {
        return ev::throwTypeError("bro.eye.structure(scene, opts?) requires a SceneGraph");
    }
    scene::SceneGraph* graph = nullptr;
    if (void* raw = ev::handleData(a[0])) {
        auto* cell = static_cast<HostSceneGraphCell*>(raw);
        if (cell->tag == kHostSceneGraphTag) graph = cell->graph();
    }
    if (!graph) {
        return ev::throwTypeError("bro.eye.structure(scene, opts?): expected a live SceneGraph");
    }

    EyeStructureOptions sopts;
    Value optsVal = a.size() >= 2 ? a[1] : ev::undefined();
    ev::Persistent optsRoot(optsVal);
    if (ev::isObject(optsRoot.get())) {
        sopts.contactTol = std::clamp(numProp(optsRoot.get(), "contactTol", sopts.contactTol), 0.001, 0.5);
        sopts.overhangTol = std::clamp(numProp(optsRoot.get(), "overhangTol", sopts.overhangTol), 0.05, 0.95);
        sopts.groundY = numProp(optsRoot.get(), "groundY", sopts.groundY);
        sopts.maxPanelDim = std::clamp(static_cast<int>(numProp(optsRoot.get(), "maxPanelDim", sopts.maxPanelDim)), 128, 1024);
        sopts.maxZoomPanels = std::clamp(static_cast<int>(numProp(optsRoot.get(), "maxZoomPanels", sopts.maxZoomPanels)), 1, 12);
        const std::string rawPath = strProp(optsRoot.get(), "path");
        if (!rawPath.empty()) {
            requirePathAccess(rawPath, /*write=*/true);
            sopts.path = rawPath;
        }
        Value igVal = ev::getProperty(optsRoot.get(), "ignoreFloating");
        if (ev::isObject(igVal)) {
            ev::Persistent ia(igVal);
            const uint32_t in = static_cast<uint32_t>(std::max(0.0, std::min(ev::toDouble(ev::getProperty(ia.get(), "length")), 128.0)));
            for (uint32_t i = 0; i < in; ++i) sopts.ignoreFloating.push_back(ev::toUtf8(ev::getElement(ia.get(), i)));
        }
        Value wpVal = ev::getProperty(optsRoot.get(), "watchPairs");
        if (ev::isObject(wpVal)) {
            ev::Persistent wa(wpVal);
            const uint32_t wn = static_cast<uint32_t>(std::max(0.0, std::min(ev::toDouble(ev::getProperty(wa.get(), "length")), 64.0)));
            for (uint32_t i = 0; i < wn; ++i) {
                Value pairEl = ev::getElement(wa.get(), i);
                if (!ev::isObject(pairEl)) continue;
                ev::Persistent pe(pairEl);
                sopts.watchPairs.emplace_back(ev::toUtf8(ev::getElement(pe.get(), 0)),
                                              ev::toUtf8(ev::getElement(pe.get(), 1)));
            }
        }
    }

    const EyeStructureResult r = analyzeStructure(*graph, sopts);
    ObjectBuilder o;
    o.set("ok", ev::fromBool(r.ok));
    o.set("meshCount", ev::fromDouble(r.meshCount));
    o.set("invertedCount", ev::fromDouble(r.invertedCount));
    o.set("floatingCount", ev::fromDouble(r.floatingCount));
    o.set("overhangCount", ev::fromDouble(r.overhangCount));
    o.set("gapCount", ev::fromDouble(r.gapCount));
    o.set("issues", hostArrayOf(r.issues.size(), [&](size_t i) {
        const auto& is = r.issues[i];
        ObjectBuilder ib;
        ib.set("kind", ev::fromUtf8(is.kind));
        ib.set("severity", ev::fromUtf8(is.severity));
        ib.set("nodeA", ev::fromUtf8(is.nodeA));
        ib.set("nodeB", ev::fromUtf8(is.nodeB));
        ib.set("value", ev::fromDouble(is.value));
        ib.set("point", hostArrayOf(3, [&](size_t k) { return ev::fromDouble(is.point[k]); }));
        ib.set("radius", ev::fromDouble(is.radius));
        ib.set("message", ev::fromUtf8(is.message));
        return ib.get();
    }));
    o.set("watchedGaps", hostArrayOf(r.watchedGaps.size(), [&](size_t i) {
        const auto& g = r.watchedGaps[i];
        ObjectBuilder gb;
        gb.set("nodeA", ev::fromUtf8(g.nodeA));
        gb.set("nodeB", ev::fromUtf8(g.nodeB));
        gb.set("gap", ev::fromDouble(g.gap));
        gb.set("pointA", hostArrayOf(3, [&](size_t k) { return ev::fromDouble(g.pointA[k]); }));
        gb.set("pointB", hostArrayOf(3, [&](size_t k) { return ev::fromDouble(g.pointB[k]); }));
        return gb.get();
    }));
    if (!r.visualPath.empty()) o.set("path", ev::fromUtf8(r.visualPath));
    return o.get();
#endif
}

Value jsEyeCloseup(Value, std::span<const Value> a) {
#if !BRO_WITH_3D
    (void)a;
    return ev::throwError("bro.eye.closeup requires a 3D build (BRO_WITH_3D=ON)");
#else
    if (!hostEngine()) return ev::throwError("bro.eye.closeup is unavailable in Worker realms");
    if (a.empty() || !ev::isObject(a[0])) {
        return ev::throwTypeError("bro.eye.closeup(scene, opts?) requires a SceneGraph");
    }
    scene::SceneGraph* graph = nullptr;
    if (void* raw = ev::handleData(a[0])) {
        auto* cell = static_cast<HostSceneGraphCell*>(raw);
        if (cell->tag == kHostSceneGraphTag) graph = cell->graph();
    }
    if (!graph) {
        return ev::throwTypeError("bro.eye.closeup(scene, opts?): expected a live SceneGraph");
    }

    EyeCloseupOptions copts;
    Value optsVal = a.size() >= 2 ? a[1] : ev::undefined();
    ev::Persistent optsRoot(optsVal);
    if (ev::isObject(optsRoot.get())) {
        copts.margin = std::clamp(numProp(optsRoot.get(), "margin", copts.margin), 1.1, 8.0);
        copts.maxPanelDim = std::clamp(static_cast<int>(numProp(optsRoot.get(), "maxPanelDim", copts.maxPanelDim)), 128, 1280);
        copts.maxPanels = std::clamp(static_cast<int>(numProp(optsRoot.get(), "maxPanels", copts.maxPanels)), 1, 24);
        const std::string rawPath = strProp(optsRoot.get(), "path");
        if (!rawPath.empty()) {
            requirePathAccess(rawPath, /*write=*/true);
            copts.path = rawPath;
        }
        Value tgVal = ev::getProperty(optsRoot.get(), "targets");
        if (ev::isObject(tgVal)) {
            ev::Persistent ta(tgVal);
            const uint32_t tn = static_cast<uint32_t>(std::max(0.0, std::min(ev::toDouble(ev::getProperty(ta.get(), "length")), 32.0)));
            for (uint32_t i = 0; i < tn; ++i) copts.targets.push_back(ev::toUtf8(ev::getElement(ta.get(), i)));
        }
        Value anVal = ev::getProperty(optsRoot.get(), "angles");
        if (ev::isObject(anVal)) {
            ev::Persistent aa(anVal);
            const uint32_t an = static_cast<uint32_t>(std::max(0.0, std::min(ev::toDouble(ev::getProperty(aa.get(), "length")), 16.0)));
            if (an > 0) copts.angles.clear();
            for (uint32_t i = 0; i < an; ++i) copts.angles.push_back(ev::toUtf8(ev::getElement(aa.get(), i)));
        }
    }

    const EyeCloseupResult r = captureCloseup(*graph, copts);
    ObjectBuilder o;
    o.set("ok", ev::fromBool(r.ok));
    o.set("panelCount", ev::fromDouble(r.panelCount));
    o.set("targets", hostArrayOf(r.targets.size(), [&](size_t i) {
        const auto& t = r.targets[i];
        ObjectBuilder tb;
        tb.set("query", ev::fromUtf8(t.query));
        tb.set("matchedNodes", ev::fromDouble(t.matchedNodes));
        tb.set("center", hostArrayOf(3, [&](size_t k) { return ev::fromDouble(t.center[k]); }));
        tb.set("extent", hostArrayOf(3, [&](size_t k) { return ev::fromDouble(t.extent[k]); }));
        tb.set("minY", ev::fromDouble(t.minY));
        tb.set("maxY", ev::fromDouble(t.maxY));
        return tb.get();
    }));
    if (!r.visualPath.empty()) o.set("path", ev::fromUtf8(r.visualPath));
    return o.get();
#endif
}

}  // namespace bro::bronze_host::eye_detail
