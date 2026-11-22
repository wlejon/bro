#include "bronze_host/host_eye.h"
#include "bronze_host/host_access.h"
#include "bronze_host/host_builder.h"
#include "bronze_host/host_eye_creative.h"
#include "bronze_host/host_eye_taste.h"
#include "bronze_host/host_globals_internal.h"
#include "bronze_host/host_runtime.h"
#include "bronze_host/host_values.h"

#include "engine/engine.h"
#include "eye/eye_audit.h"
#include "eye/eye_fit.h"
#include "eye/eye_layout.h"
#include "eye/eye_measure.h"
#include "eye/eye_scene.h"
#include "eye/eye_visualize.h"

#if BRO_WITH_3D
#include "bronze_host/native_scene_internal.h"
#include "scene/scene_graph.h"
#include "scene/scene_renderer.h"
#include "scene/vulkan/frame/scene_view_inspect.h"
#endif

#include <algorithm>
#include <cmath>
#include <span>
#include <string>
#include <vector>

namespace bro::bronze_host {

namespace eye_detail {
bool extractImage(Value v, eye::EyeImage& out, bool allowLiveCapture, std::string& err);
eye::EyeMeasureOptions parseMeasureOptions(Value optsVal);
eye::EyeCompareOptions parseCompareOptions(Value optsVal);
eye::EyeAuditOptions parseAuditOptions(Value optsVal);
eye::EyeSceneOptions parseSceneOptions(Value optsVal);
Value measurementToJs(const eye::EyeMeasurement& m);
Value comparisonToJs(const eye::EyeComparison& c);
Value issueToJs(const eye::EyeIssue& iss);
}  // namespace eye_detail

namespace {

using namespace bro::eye;
using namespace bro::bronze_host::eye_detail;

Value jsEyeMeasure(Value, std::span<const Value> a) {
    EyeImage img;
    std::string err;
    if (!extractImage(argAt(a, 0), img, /*allowLiveCapture=*/true, err)) {
        return ev::throwTypeError("bro.eye.measure: " + err);
    }
    const EyeMeasureOptions opts = parseMeasureOptions(argAt(a, 1));
    return measurementToJs(measureImage(img.view(), opts));
}

Value jsEyeCompare(Value, std::span<const Value> a) {
    if (a.size() < 2) {
        return ev::throwTypeError("bro.eye.compare(frame, reference, opts?) requires 2 arguments");
    }
    EyeImage imgA, imgB;
    std::string err;
    if (!extractImage(a[0], imgA, /*allowLiveCapture=*/true, err)) {
        return ev::throwTypeError("bro.eye.compare (frame): " + err);
    }
    if (!extractImage(a[1], imgB, /*allowLiveCapture=*/false, err)) {
        return ev::throwTypeError("bro.eye.compare (reference): " + err);
    }
    const EyeCompareOptions opts = parseCompareOptions(argAt(a, 2));
    return comparisonToJs(compareImages(imgA.view(), imgB.view(), opts));
}

Value jsEyeAudit(Value, std::span<const Value> a) {
    auto* eng = hostEngine();
    if (!eng) {
        return ev::throwError("bro.eye.audit is unavailable in Worker realms");
    }
    const EyeAuditOptions opts = parseAuditOptions(argAt(a, 0));
    const EyeAuditResult r = auditDocument(*eng, opts);

    ObjectBuilder o;
    o.set("ok", ev::fromBool(r.ok));
    o.set("score", ev::fromDouble(r.score));
    o.set("errorCount", ev::fromDouble(r.errorCount));
    o.set("warningCount", ev::fromDouble(r.warningCount));
    o.set("checkedTextNodes", ev::fromDouble(r.checkedTextNodes));
    o.set("checkedElements", ev::fromDouble(r.checkedElements));
    o.set("issues", hostArrayOf(r.issues.size(), [&](size_t i) { return issueToJs(r.issues[i]); }));
    o.set("frame", measurementToJs(r.frame));
    return o.get();
}

Value jsEyeScene(Value, std::span<const Value> a) {
#if !BRO_WITH_3D
    (void)a;
    return ev::throwError("bro.eye.scene is unavailable: compiled without BRO_WITH_3D");
#else
    auto* eng = hostEngine();
    if (!eng) {
        return ev::throwError("bro.eye.scene is unavailable in Worker realms");
    }
    scene::SceneGraph* g = nullptr;
    if (!a.empty() && ev::isObject(a[0])) {
        if (void* raw = ev::handleData(a[0])) {
            auto* cell = static_cast<HostSceneGraphCell*>(raw);
            if (cell->tag == kHostSceneGraphTag) g = cell->graph();
        }
    }
    if (!g) {
        return ev::throwTypeError("bro.eye.scene(scene, opts?) requires a live SceneGraph");
    }
    const EyeSceneOptions opts = parseSceneOptions(argAt(a, 1));
    const EyeSceneResult r = auditScene(*g, opts);

    ObjectBuilder o;
    o.set("ok", ev::fromBool(r.ok));
    o.set("score", ev::fromDouble(r.score));
    o.set("errorCount", ev::fromDouble(r.errorCount));
    o.set("warningCount", ev::fromDouble(r.warningCount));
    o.set("width", ev::fromDouble(r.width));
    o.set("height", ev::fromDouble(r.height));
    o.set("skyRatio", ev::fromDouble(r.skyRatio));
    o.set("nearDist", ev::fromDouble(r.nearDist));
    o.set("medianDist", ev::fromDouble(r.medianDist));
    o.set("farDist", ev::fromDouble(r.farDist));
    o.set("exposureEv", ev::fromDouble(r.exposureEv));
    o.set("meteredEv", ev::fromDouble(r.meteredEv));
    o.set("meshDrawn", ev::fromDouble(r.meshDrawn));
    o.set("meshCulled", ev::fromDouble(r.meshCulled));
    o.set("nodes", hostArrayOf(r.nodes.size(), [&](size_t i) {
        const auto& n = r.nodes[i];
        ObjectBuilder nb;
        nb.set("id", ev::fromDouble(n.id));
        ev::Persistent nm(ev::fromUtf8(n.name));
        nb.set("name", nm.get());
        nb.set("visible", ev::fromBool(n.visible));
        nb.set("screenCoverage", ev::fromDouble(n.screenCoverage));
        nb.set("visibleFraction", ev::fromDouble(n.visibleFraction));
        nb.set("bounds2d", makeHostRectValue(n.bounds2d.x, n.bounds2d.y, n.bounds2d.w, n.bounds2d.h));
        nb.set("minDepth", ev::fromDouble(n.minDepth));
        nb.set("maxDepth", ev::fromDouble(n.maxDepth));
        nb.set("silhouetteContrast", ev::fromDouble(n.silhouetteContrast));
        nb.set("lumaMean", ev::fromDouble(n.lumaMean));
        nb.set("lumaStd", ev::fromDouble(n.lumaStd));
        nb.set("formRange", ev::fromDouble(n.formRange));
        nb.set("frameClipped", ev::fromBool(n.frameClipped));
        return nb.get();
    }));
    o.set("issues", hostArrayOf(r.issues.size(), [&](size_t i) { return issueToJs(r.issues[i]); }));
    {
        ObjectBuilder st;
        st.set("tested", ev::fromBool(r.stability.tested));
        st.set("jitterPx", ev::fromDouble(r.stability.jitterPx));
        st.set("meanDeltaE", ev::fromDouble(r.stability.meanDeltaE));
        st.set("maxDeltaE", ev::fromDouble(r.stability.maxDeltaE));
        st.set("changedRatio", ev::fromDouble(r.stability.changedRatio));
        st.set("depthFlipRatio", ev::fromDouble(r.stability.depthFlipRatio));
        st.set("shimmerScore", ev::fromDouble(r.stability.shimmerScore));
        o.set("stability", st.get());
    }
    o.set("frame", measurementToJs(r.frame));
    return o.get();
#endif
}

Value jsEyeVisualize(Value, std::span<const Value> a) {
    EyeVisualizeOptions vopts;
    Value optsVal = argAt(a, 1);
    if (ev::isObject(optsVal)) {
        ev::Persistent r(optsVal);
        Value mv = ev::getProperty(r.get(), "mode");
        if (ev::isString(mv)) vopts.mode = ev::toUtf8(mv);
        Value lv = ev::getProperty(r.get(), "layout");
        if (ev::isString(lv)) vopts.layout = ev::toUtf8(lv);
        Value pv = ev::getProperty(r.get(), "path");
        if (ev::isString(pv)) {
            vopts.path = ev::toUtf8(pv);
            if (!vopts.path.empty()) {
                requirePathAccess(vopts.path, /*write=*/true);
            }
        }
        Value fsv = ev::getProperty(r.get(), "fontScale");
        if (fsv.isNumber()) vopts.fontScale = std::clamp(static_cast<int>(fsv.asNumber()), 1, 4);
        Value ntv = ev::getProperty(r.get(), "notanTones");
        if (ntv.isNumber()) vopts.notanTones = static_cast<int>(ntv.asNumber()) >= 5 ? 5 : 3;
        Value zfv = ev::getProperty(r.get(), "zebraFreq");
        if (zfv.isNumber() && std::isfinite(zfv.asNumber())) vopts.zebraFreq = std::clamp(zfv.asNumber(), 2.0, 48.0);
        Value mdv = ev::getProperty(r.get(), "maxPanelDim");
        if (mdv.isNumber()) vopts.maxPanelDim = std::clamp(static_cast<int>(mdv.asNumber()), 64, 4096);
        Value lbls = ev::getProperty(r.get(), "labels");
        if (hostIsArray(lbls)) {
            ev::Persistent lr(lbls);
            const uint32_t n = satCast<uint32_t>(ev::toDouble(ev::getProperty(lr.get(), "length")));
            for (uint32_t i = 0; i < n && i < 64; ++i) {
                vopts.labels.push_back(ev::toUtf8(ev::getElement(lr.get(), i)));
            }
        }
    }

    std::vector<EyeImage> ownedImages;
    std::vector<EyeImageView> views;
    std::vector<uint8_t> sceneRgba;
    std::vector<float> sceneDepth;
    EyeAuditResult ownedAudit;
    EyeSceneResult ownedScene;
    EyeComposeResult ownedCompose;

    Value src = argAt(a, 0);
    std::string err;

#if BRO_WITH_3D
    if (vopts.mode == "turntable" || vopts.mode == "raking") {
        EyeVisualResult sceneSheetRes;
        std::string sceneErr;
        if (!trySceneTasteVisualize(src, vopts, sceneSheetRes, sceneErr)) {
            return ev::throwError(sceneErr.empty() ? "bro.eye.visualize: scene taste capture failed" : sceneErr);
        }
        ObjectBuilder out;
        out.set("width", ev::fromDouble(sceneSheetRes.width));
        out.set("height", ev::fromDouble(sceneSheetRes.height));
        Value buf = ev::createTypedArray(ev::elements::Uint8Clamped, static_cast<uint32_t>(sceneSheetRes.rgba.size()));
        ev::fillTypedArray(buf, std::span<const uint8_t>(sceneSheetRes.rgba.data(), sceneSheetRes.rgba.size()));
        out.set("data", buf);
        out.set("panels", hostArrayOf(sceneSheetRes.panels.size(), [&](size_t i) {
            const auto& p = sceneSheetRes.panels[i];
            ObjectBuilder po;
            ev::Persistent lbl(ev::fromUtf8(p.label));
            po.set("label", lbl.get());
            po.set("x", ev::fromDouble(p.x));
            po.set("y", ev::fromDouble(p.y));
            po.set("width", ev::fromDouble(p.width));
            po.set("height", ev::fromDouble(p.height));
            return po.get();
        }));
        if (!sceneSheetRes.path.empty()) {
            ev::Persistent pp(ev::fromUtf8(sceneSheetRes.path));
            out.set("path", pp.get());
        }
        return out.get();
    }

    if ((vopts.mode == "depth" || vopts.mode == "normals" || vopts.mode == "zebra" || vopts.mode == "curvature") && ev::isObject(src)) {
        if (void* raw = ev::handleData(src)) {
            auto* cell = static_cast<HostSceneGraphCell*>(raw);
            if (cell->tag == kHostSceneGraphTag) {
                auto* g = cell->graph();
                if (!g) return ev::throwTypeError("bro.eye.visualize: SceneGraph has been destroyed");
                g->renderForRead();
                int w = 0, h = 0;
                scene::vk::InspectCamera cam{};
                if (!g->renderer().readInspectSnapshot(sceneRgba, sceneDepth, w, h, cam) || sceneRgba.empty()) {
                    return ev::throwError("bro.eye.visualize: scene snapshot failed");
                }
                ownedImages.push_back({w, h, std::move(sceneRgba)});
                vopts.depth = sceneDepth.data();
                vopts.depthWidth = w;
                vopts.depthHeight = h;
                for (int c = 0; c < 4; ++c) {
                    for (int r = 0; r < 4; ++r) {
                        vopts.invProj[static_cast<size_t>(c) * 4 + r] = cam.invProj.at(r, c);
                        vopts.relView[static_cast<size_t>(c) * 4 + r] = cam.relView.at(r, c);
                    }
                }
                vopts.hasCamera = true;
            }
        }
    }
#endif

    if (ownedImages.empty()) {
        if (hostIsArray(src)) {
            ev::Persistent arr(src);
            const uint32_t n = satCast<uint32_t>(ev::toDouble(ev::getProperty(arr.get(), "length")));
            for (uint32_t i = 0; i < n && i < 32; ++i) {
                EyeImage img;
                if (!extractImage(ev::getElement(arr.get(), i), img, /*allowLiveCapture=*/true, err)) {
                    return ev::throwTypeError("bro.eye.visualize[" + std::to_string(i) + "]: " + err);
                }
                ownedImages.push_back(std::move(img));
            }
        } else {
            EyeImage img;
            if (!extractImage(src, img, /*allowLiveCapture=*/true, err)) {
                return ev::throwTypeError("bro.eye.visualize: " + err);
            }
            ownedImages.push_back(std::move(img));
        }
    }

    if (vopts.mode == "audit") {
#if BRO_WITH_3D
        bool handledScene = false;
        if (ev::isObject(src)) {
            if (void* raw = ev::handleData(src)) {
                auto* cell = static_cast<HostSceneGraphCell*>(raw);
                if (cell->tag == kHostSceneGraphTag && cell->graph()) {
                    ownedScene = auditScene(*cell->graph(), {});
                    vopts.sceneAudit = &ownedScene;
                    handledScene = true;
                }
            }
        }
        if (!handledScene && hostEngine()) {
            ownedAudit = auditDocument(*hostEngine(), {});
            vopts.audit = &ownedAudit;
            vopts.issueCoordScale = hostEngine()->renderScale();
        }
#else
        if (hostEngine()) {
            ownedAudit = auditDocument(*hostEngine(), {});
            vopts.audit = &ownedAudit;
            vopts.issueCoordScale = hostEngine()->renderScale();
        }
#endif
    }

    if (vopts.mode == "rails" && hostEngine()) {
        ownedCompose.layout = analyzeLayout(*hostEngine(), {});
        vopts.compose = &ownedCompose;
        vopts.issueCoordScale = hostEngine()->renderScale();
    }

    for (const auto& im : ownedImages) views.push_back(im.view());
    const EyeVisualResult vr = visualizeImages(views, vopts);
    if (vr.width <= 0 || vr.height <= 0 || vr.rgba.empty()) {
        return ev::throwError("bro.eye.visualize: failed to build visualization");
    }

    ObjectBuilder out;
    out.set("width", ev::fromDouble(vr.width));
    out.set("height", ev::fromDouble(vr.height));
    Value buf = ev::createTypedArray(ev::elements::Uint8Clamped, static_cast<uint32_t>(vr.rgba.size()));
    ev::fillTypedArray(buf, std::span<const uint8_t>(vr.rgba.data(), vr.rgba.size()));
    out.set("data", buf);
    out.set("panels", hostArrayOf(vr.panels.size(), [&](size_t i) {
        const auto& p = vr.panels[i];
        ObjectBuilder po;
        ev::Persistent lbl(ev::fromUtf8(p.label));
        po.set("label", lbl.get());
        po.set("x", ev::fromDouble(p.x));
        po.set("y", ev::fromDouble(p.y));
        po.set("width", ev::fromDouble(p.width));
        po.set("height", ev::fromDouble(p.height));
        return po.get();
    }));
    if (!vr.path.empty()) {
        ev::Persistent pp(ev::fromUtf8(vr.path));
        out.set("path", pp.get());
    }
    return out.get();
}

Value jsEyeFit(Value, std::span<const Value> a) {
    if (a.size() < 2 || !ev::isFunction(a[0]) || !ev::isObject(a[1])) {
        return ev::throwTypeError("bro.eye.fit(renderFn, opts) requires a function and options object");
    }
    ev::Persistent renderFn(a[0]);
    ev::Persistent optsRoot(a[1]);
    ev::Persistent scorerFn(ev::getProperty(optsRoot.get(), "scorer"));
    ev::Persistent progressFn(ev::getProperty(optsRoot.get(), "onProgress"));
    const bool hasScorer = ev::isFunction(scorerFn.get());
    const bool hasProgress = ev::isFunction(progressFn.get());

    EyeFitOptions fopts;
    Value paramsVal = ev::getProperty(optsRoot.get(), "params");
    if (!ev::isObject(paramsVal)) {
        return ev::throwTypeError("bro.eye.fit: opts.params is required");
    }
    {
        ev::Persistent pr(paramsVal);
        if (hostIsArray(pr.get())) {
            const uint32_t n = satCast<uint32_t>(ev::toDouble(ev::getProperty(pr.get(), "length")));
            for (uint32_t i = 0; i < n && i < 64; ++i) {
                Value item = ev::getElement(pr.get(), i);
                if (!ev::isObject(item)) continue;
                ev::Persistent ir(item);
                EyeFitParam p;
                p.name = ev::toUtf8(ev::getProperty(ir.get(), "name"));
                Value minV = ev::getProperty(ir.get(), "min");
                Value maxV = ev::getProperty(ir.get(), "max");
                p.min = minV.isNumber() ? minV.asNumber() : 0.0;
                p.max = maxV.isNumber() ? maxV.asNumber() : 1.0;
                Value startV = ev::getProperty(ir.get(), "start");
                if (startV.isNumber()) {
                    p.start = startV.asNumber();
                    p.hasStart = true;
                }
                Value scV = ev::getProperty(ir.get(), "scale");
                if (ev::isString(scV)) p.scale = ev::toUtf8(scV);
                if (!p.name.empty()) fopts.params.push_back(std::move(p));
            }
        }
    }
    if (fopts.params.empty()) {
        return ev::throwTypeError("bro.eye.fit: opts.params must be non-empty");
    }

    Value refVal = ev::getProperty(optsRoot.get(), "reference");
    if (!refVal.isUndefined() && !refVal.isNull()) {
        std::string err;
        if (!extractImage(refVal, fopts.reference, /*allowLiveCapture=*/false, err)) {
            return ev::throwTypeError("bro.eye.fit (reference): " + err);
        }
        fopts.hasReference = true;
    }

    Value measVal = ev::getProperty(optsRoot.get(), "measures");
    if (ev::isObject(measVal)) {
        ev::Persistent mr(measVal);
        auto gv = ev::globalValue("Object");
        if (gv.found && ev::isObject(gv.value)) {
            ev::Persistent objCtor(gv.value);
            Value keysFn = ev::getProperty(objCtor.get(), "keys");
            if (ev::isFunction(keysFn)) {
                ev::Persistent kf(keysFn);
                Value args[1] = {mr.get()};
                auto kr = ev::call(kf.get(), objCtor.get(), args);
                if (!kr.thrown && hostIsArray(kr.value)) {
                    ev::Persistent keysArr(kr.value);
                    const uint32_t kn = satCast<uint32_t>(ev::toDouble(ev::getProperty(keysArr.get(), "length")));
                    for (uint32_t i = 0; i < kn && i < 64; ++i) {
                        const std::string key = ev::toUtf8(ev::getElement(keysArr.get(), i));
                        Value tv = ev::getProperty(mr.get(), key);
                        EyeFitMeasureTarget mt;
                        mt.name = key;
                        if (tv.isNumber()) {
                            mt.target = tv.asNumber();
                            fopts.measures.push_back(mt);
                        } else if (ev::isObject(tv)) {
                            ev::Persistent tr(tv);
                            Value vv = ev::getProperty(tr.get(), "value");
                            if (vv.isNumber()) {
                                mt.target = vv.asNumber();
                                Value wv = ev::getProperty(tr.get(), "weight");
                                Value sv = ev::getProperty(tr.get(), "scale");
                                if (wv.isNumber()) mt.weight = wv.asNumber();
                                if (sv.isNumber()) mt.scale = sv.asNumber();
                                fopts.measures.push_back(mt);
                            }
                        }
                    }
                }
            }
        }
    }

    Value meV = ev::getProperty(optsRoot.get(), "maxEvaluations");
    if (meV.isNumber()) fopts.maxEvaluations = std::clamp(static_cast<int>(meV.asNumber()), 1, 5000);
    Value tdV = ev::getProperty(optsRoot.get(), "targetDistance");
    if (tdV.isNumber()) fopts.targetDistance = std::max(0.0, tdV.asNumber());
    Value sdV = ev::getProperty(optsRoot.get(), "seed");
    if (sdV.isNumber()) fopts.seed = static_cast<uint64_t>(std::max(0.0, sdV.asNumber()));
    Value rwV = ev::getProperty(optsRoot.get(), "referenceWeight");
    if (rwV.isNumber()) fopts.referenceWeight = rwV.asNumber();
    Value aeV = ev::getProperty(optsRoot.get(), "auditErrorWeight");
    if (aeV.isNumber()) fopts.auditErrorWeight = aeV.asNumber();
    Value awV = ev::getProperty(optsRoot.get(), "auditWarningWeight");
    if (awV.isNumber()) fopts.auditWarningWeight = awV.asNumber();

    const bool runAudit = (fopts.auditErrorWeight > 0.0 || fopts.auditWarningWeight > 0.0) && hostEngine() != nullptr;

    auto makeParamObj = [&](const std::vector<double>& vals) -> Value {
        ObjectBuilder po;
        for (size_t i = 0; i < fopts.params.size() && i < vals.size(); ++i) {
            po.set(fopts.params[i].name.c_str(), ev::fromDouble(vals[i]));
        }
        return po.get();
    };

    const EyeFitResult fitRes = fitVisualParams(
        fopts,
        [&](const std::vector<double>& vals) -> EyeFitCandidateResult {
            EyeFitCandidateResult cr;
            ev::Persistent pObj(makeParamObj(vals));
            Value args[1] = {pObj.get()};
            ev::CallResult res = ev::call(renderFn.get(), ev::undefined(), args);
            if (res.thrown) {
                cr.ok = false;
                return cr;
            }
            ev::Persistent rv(res.value);
            std::string err;
            if (!extractImage(rv.get(), cr.image, /*allowLiveCapture=*/true, err)) {
                cr.ok = false;
                return cr;
            }
            if (runAudit) {
                const EyeAuditResult ar = auditDocument(*hostEngine(), {});
                cr.hasAudit = true;
                cr.auditErrors = ar.errorCount;
                cr.auditWarnings = ar.warningCount;
            }
            if (hasScorer) {
                Value sArgs[1] = {rv.get()};
                ev::CallResult sr = ev::call(scorerFn.get(), ev::undefined(), sArgs);
                if (!sr.thrown && sr.value.isNumber()) {
                    cr.hasCustomScore = true;
                    cr.customScore = sr.value.asNumber();
                }
            }
            return cr;
        },
        [&](int eval, double bestDist, const std::vector<double>& bestVals) {
            if (!hasProgress) return;
            ev::Persistent bestObj(makeParamObj(bestVals));
            Value args[3] = {ev::fromDouble(eval), ev::fromDouble(bestDist), bestObj.get()};
            (void)ev::call(progressFn.get(), ev::undefined(), args);
        });

    ObjectBuilder out;
    out.set("ok", ev::fromBool(fitRes.ok));
    out.set("distance", ev::fromDouble(fitRes.distance));
    out.set("evaluations", ev::fromDouble(fitRes.evaluations));
    out.set("params", makeParamObj(fitRes.bestValues));
    out.set("frame", measurementToJs(fitRes.frame));
    if (fitRes.hasComparison) {
        out.set("comparison", comparisonToJs(fitRes.comparison));
    }
    out.set("history", hostArrayOf(fitRes.history.size(), [&](size_t i) { return ev::fromDouble(fitRes.history[i]); }));
    return out.get();
}

}  // namespace

Value makeBroEyeValue(bool hasEngine) {
    ObjectBuilder eyeObj;
    eyeObj.set("available", ev::fromBool(true));
    eyeObj.def("measure", 2, jsEyeMeasure);
    eyeObj.def("compare", 3, jsEyeCompare);
    eyeObj.def("compose", 2, eye_detail::jsEyeCompose);
    eyeObj.def("form", 2, eye_detail::jsEyeForm);
    eyeObj.def("motion", 2, eye_detail::jsEyeMotion);
    eyeObj.def("visualize", 2, jsEyeVisualize);
    eyeObj.def("fit", 2, jsEyeFit);
    if (hasEngine) {
        eyeObj.def("audit", 1, jsEyeAudit);
        eyeObj.def("scene", 2, jsEyeScene);
        eyeObj.def("probe", 2, eye_detail::jsEyeProbe);
    }
    return eyeObj.get();
}

}  // namespace bro::bronze_host
