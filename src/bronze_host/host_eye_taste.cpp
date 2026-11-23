#include "bronze_host/host_eye_taste.h"
#include "bronze_host/host_access.h"
#include "bronze_host/host_builder.h"
#include "bronze_host/host_globals_internal.h"
#include "bronze_host/host_runtime.h"
#include "bronze_host/host_values.h"

#include "engine/engine.h"
#include "eye/eye_compose.h"
#include "eye/eye_layout.h"
#include "eye/eye_visualize.h"

#if BRO_WITH_3D
#include "bronze_host/native_scene_internal.h"
#include "eye/eye_form.h"
#include "scene/nodes/camera_node.h"
#include "scene/nodes/light_node.h"
#include "scene/nodes/mesh_node.h"
#include "scene/scene_graph.h"
#include "scene/scene_renderer.h"
#include "scene/vulkan/frame/scene_view_inspect.h"
#include <bromath/aabb.h>
#include <bromesh/api.h>
#endif

#include <algorithm>
#include <cmath>
#include <cstdio>
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

Value perceptualDiffToJs(const EyePerceptualDiff& d) {
    ObjectBuilder o;
    o.set("focalShiftX", ev::fromDouble(d.focalShiftX));
    o.set("focalShiftY", ev::fromDouble(d.focalShiftY));
    o.set("focalShiftDist", ev::fromDouble(d.focalShiftDist));
    o.set("focalDominanceDiff", ev::fromDouble(d.focalDominanceDiff));
    o.set("notanMassDiff", ev::fromDouble(d.notanMassDiff));
    o.set("restDetailDiff", ev::fromDouble(d.restDetailDiff));
    o.set("balanceDiff", ev::fromDouble(d.balanceDiff));
    return o.get();
}

Value composeToJs(const EyeComposeResult& r) {
    ObjectBuilder o;
    o.set("width", ev::fromDouble(r.width));
    o.set("height", ev::fromDouble(r.height));
    o.set("score", ev::fromDouble(r.score));

    {
        ObjectBuilder sal;
        sal.set("centerOfMassX", ev::fromDouble(r.saliency.centerOfMassX));
        sal.set("centerOfMassY", ev::fromDouble(r.saliency.centerOfMassY));
        sal.set("balanceX", ev::fromDouble(r.saliency.balanceX));
        sal.set("balanceY", ev::fromDouble(r.saliency.balanceY));
        sal.set("focalDominance", ev::fromDouble(r.saliency.focalDominance));
        sal.set("edgeCrowding", ev::fromDouble(r.saliency.edgeCrowding));
        sal.set("anchors", hostArrayOf(r.saliency.anchors.size(), [&](size_t i) {
            const auto& a = r.saliency.anchors[i];
            ObjectBuilder fb;
            fb.set("rank", ev::fromDouble(a.rank));
            fb.set("x", ev::fromDouble(a.x));
            fb.set("y", ev::fromDouble(a.y));
            fb.set("radius", ev::fromDouble(a.radius));
            fb.set("strength", ev::fromDouble(a.strength));
            return fb.get();
        }));
        o.set("saliency", sal.get());
    }
    {
        ObjectBuilder nt;
        nt.set("darkShare", ev::fromDouble(r.notan.darkShare));
        nt.set("midShare", ev::fromDouble(r.notan.midShare));
        nt.set("lightShare", ev::fromDouble(r.notan.lightShare));
        nt.set("massCount", ev::fromDouble(r.notan.massCount));
        nt.set("largestMassShare", ev::fromDouble(r.notan.largestMassShare));
        nt.set("fragmentation", ev::fromDouble(r.notan.fragmentation));
        nt.set("tone5Shares", hostArrayOf(5, [&](size_t i) { return ev::fromDouble(r.notan.tone5Shares[i]); }));
        o.set("notan", nt.get());
    }
    {
        ObjectBuilder rh;
        rh.set("restRatio", ev::fromDouble(r.rhythm.restRatio));
        rh.set("detailRatio", ev::fromDouble(r.rhythm.detailRatio));
        rh.set("restDetailContrast", ev::fromDouble(r.rhythm.restDetailContrast));
        rh.set("gridRows", ev::fromDouble(r.rhythm.gridRows));
        rh.set("gridCols", ev::fromDouble(r.rhythm.gridCols));
        o.set("rhythm", rh.get());
    }
    {
        ObjectBuilder pl;
        pl.set("tierCount", ev::fromDouble(r.palette.tierCount));
        pl.set("minTierDeltaE", ev::fromDouble(r.palette.minTierDeltaE));
        pl.set("medianTierDeltaE", ev::fromDouble(r.palette.medianTierDeltaE));
        pl.set("warmShadowShift", ev::fromDouble(r.palette.warmShadowShift));
        pl.set("chromaRestraint", ev::fromDouble(r.palette.chromaRestraint));
        o.set("palette", pl.get());
    }
    {
        const auto& l = r.layout;
        ObjectBuilder lo;
        lo.set("analyzed", ev::fromBool(l.analyzed));
        lo.set("elementCount", ev::fromDouble(l.elementCount));
        lo.set("xRailCount", ev::fromDouble(l.xRailCount));
        lo.set("yRailCount", ev::fromDouble(l.yRailCount));
        lo.set("railEconomy", ev::fromDouble(l.railEconomy));
        lo.set("spacingRegularity", ev::fromDouble(l.spacingRegularity));
        lo.set("gestaltRatio", ev::fromDouble(l.gestaltRatio));
        lo.set("rails", hostArrayOf(l.rails.size(), [&](size_t i) {
            ObjectBuilder rb;
            ev::Persistent ax(ev::fromUtf8(l.rails[i].axis));
            rb.set("axis", ax.get());
            rb.set("pos", ev::fromDouble(l.rails[i].pos));
            rb.set("count", ev::fromDouble(l.rails[i].count));
            return rb.get();
        }));
        lo.set("drifts", hostArrayOf(l.drifts.size(), [&](size_t i) {
            const auto& d = l.drifts[i];
            ObjectBuilder db;
            ev::Persistent ax(ev::fromUtf8(d.axis));
            ev::Persistent sel(ev::fromUtf8(d.selector));
            db.set("axis", ax.get());
            db.set("selector", sel.get());
            db.set("pos", ev::fromDouble(d.pos));
            db.set("railPos", ev::fromDouble(d.railPos));
            db.set("driftPx", ev::fromDouble(d.driftPx));
            db.set("rect", makeHostRectValue(d.rectX, d.rectY, d.rectW, d.rectH));
            return db.get();
        }));
        lo.set("gapOutliers", hostArrayOf(l.gapOutliers.size(), [&](size_t i) {
            const auto& g = l.gapOutliers[i];
            ObjectBuilder gb;
            ev::Persistent ps(ev::fromUtf8(g.parentSelector));
            ev::Persistent ax(ev::fromUtf8(g.axis));
            gb.set("parentSelector", ps.get());
            gb.set("axis", ax.get());
            gb.set("gapPx", ev::fromDouble(g.gapPx));
            gb.set("expectedPx", ev::fromDouble(g.expectedPx));
            gb.set("rect", makeHostRectValue(g.rectX, g.rectY, g.rectW, g.rectH));
            return gb.get();
        }));
        lo.set("wrapIssues", hostArrayOf(l.wrapIssues.size(), [&](size_t i) {
            const auto& w = l.wrapIssues[i];
            ObjectBuilder wb;
            ev::Persistent sel(ev::fromUtf8(w.selector));
            ev::Persistent tx(ev::fromUtf8(w.text));
            wb.set("selector", sel.get());
            wb.set("text", tx.get());
            wb.set("width", ev::fromDouble(w.width));
            wb.set("height", ev::fromDouble(w.height));
            wb.set("estimatedLines", ev::fromDouble(w.estimatedLines));
            wb.set("rect", makeHostRectValue(w.rectX, w.rectY, w.rectW, w.rectH));
            return wb.get();
        }));
        lo.set("tangents", hostArrayOf(l.tangents.size(), [&](size_t i) {
            const auto& t = l.tangents[i];
            ObjectBuilder tb;
            ev::Persistent sa(ev::fromUtf8(t.selectorA));
            ev::Persistent sb(ev::fromUtf8(t.selectorB));
            tb.set("selectorA", sa.get());
            tb.set("selectorB", sb.get());
            tb.set("gapPx", ev::fromDouble(t.gapPx));
            tb.set("rect", makeHostRectValue(t.rectX, t.rectY, t.rectW, t.rectH));
            return tb.get();
        }));
        o.set("layout", lo.get());
    }
    return o.get();
}

Value formToJs(const EyeFormResult& r) {
    ObjectBuilder o;
    o.set("ok", ev::fromBool(r.ok));
    o.set("score", ev::fromDouble(r.score));
    ev::Persistent sn(ev::fromUtf8(r.subjectName));
    o.set("subjectName", sn.get());

    {
        const auto& s = r.surface;
        ObjectBuilder sb;
        sb.set("vertexCount", ev::fromDouble(s.vertexCount));
        sb.set("triangleCount", ev::fromDouble(s.triangleCount));
        sb.set("nonManifoldEdges", ev::fromDouble(s.nonManifoldEdges));
        sb.set("creaseEdgeCount", ev::fromDouble(s.creaseEdgeCount));
        sb.set("pinchVertexCount", ev::fromDouble(s.pinchVertexCount));
        sb.set("surfaceArea", ev::fromDouble(s.surfaceArea));
        sb.set("meanCurvatureAbs", ev::fromDouble(s.meanCurvatureAbs));
        sb.set("gaussianCurvatureAbs", ev::fromDouble(s.gaussianCurvatureAbs));
        sb.set("ellipticRatio", ev::fromDouble(s.ellipticRatio));
        sb.set("hyperbolicRatio", ev::fromDouble(s.hyperbolicRatio));
        sb.set("parabolicRatio", ev::fromDouble(s.parabolicRatio));
        sb.set("flatRatio", ev::fromDouble(s.flatRatio));
        sb.set("creaseRatio", ev::fromDouble(s.creaseRatio));
        sb.set("highFreqRoughness", ev::fromDouble(s.highFreqRoughness));
        sb.set("smoothnessScore", ev::fromDouble(s.smoothnessScore));
        o.set("surface", sb.get());
    }
    {
        const auto& m = r.mass;
        ObjectBuilder mb;
        mb.set("volume", ev::fromDouble(m.volume));
        mb.set("centerOfMass", hostArrayOf(3, [&](size_t i) { return ev::fromDouble(m.centerOfMass[i]); }));
        mb.set("boundsMin", hostArrayOf(3, [&](size_t i) { return ev::fromDouble(m.boundsMin[i]); }));
        mb.set("boundsMax", hostArrayOf(3, [&](size_t i) { return ev::fromDouble(m.boundsMax[i]); }));
        mb.set("heightWidthRatio", ev::fromDouble(m.heightWidthRatio));
        mb.set("depthWidthRatio", ev::fromDouble(m.depthWidthRatio));
        mb.set("plumbOffsetX", ev::fromDouble(m.plumbOffsetX));
        mb.set("plumbOffsetZ", ev::fromDouble(m.plumbOffsetZ));
        mb.set("stabilityMargin", ev::fromDouble(m.stabilityMargin));
        mb.set("symmetryScore", ev::fromDouble(m.symmetryScore));
        mb.set("grounded", ev::fromBool(m.grounded));
        o.set("mass", mb.get());
    }
    o.set("slices", hostArrayOf(r.slices.size(), [&](size_t i) {
        const auto& sp = r.slices[i];
        ObjectBuilder sb;
        sb.set("yFrac", ev::fromDouble(sp.yFrac));
        sb.set("width", ev::fromDouble(sp.width));
        sb.set("depth", ev::fromDouble(sp.depth));
        sb.set("area", ev::fromDouble(sp.area));
        sb.set("centerX", ev::fromDouble(sp.centerX));
        sb.set("centerZ", ev::fromDouble(sp.centerZ));
        return sb.get();
    }));
    o.set("silhouettes", hostArrayOf(r.silhouettes.size(), [&](size_t i) {
        const auto& sv = r.silhouettes[i];
        ObjectBuilder sb;
        sb.set("azimuthDeg", ev::fromDouble(sv.azimuthDeg));
        sb.set("elevationDeg", ev::fromDouble(sv.elevationDeg));
        sb.set("coverage", ev::fromDouble(sv.coverage));
        sb.set("solidity", ev::fromDouble(sv.solidity));
        sb.set("perimeterRatio", ev::fromDouble(sv.perimeterRatio));
        sb.set("flatRatio", ev::fromDouble(sv.flatRatio));
        sb.set("smoothness", ev::fromDouble(sv.smoothness));
        sb.set("inflectionCount", ev::fromDouble(sv.inflectionCount));
        sb.set("wiggleCount", ev::fromDouble(sv.wiggleCount));
        sb.set("sharpCornerCount", ev::fromDouble(sv.sharpCornerCount));
        sb.set("holeCount", ev::fromDouble(sv.holeCount));
        sb.set("pinchCount", ev::fromDouble(sv.pinchCount));
        sb.set("holes", hostArrayOf(sv.holes.size(), [&](size_t k) {
            ObjectBuilder hb;
            hb.set("areaRatio", ev::fromDouble(sv.holes[k].areaRatio));
            hb.set("cx", ev::fromDouble(sv.holes[k].cx));
            hb.set("cy", ev::fromDouble(sv.holes[k].cy));
            hb.set("aspect", ev::fromDouble(sv.holes[k].aspect));
            return hb.get();
        }));
        sb.set("anomalies", hostArrayOf(sv.anomalies.size(), [&](size_t k) {
            ObjectBuilder ab;
            ev::Persistent kd(ev::fromUtf8(sv.anomalies[k].kind));
            ab.set("kind", kd.get());
            ab.set("u", ev::fromDouble(sv.anomalies[k].u));
            ab.set("v", ev::fromDouble(sv.anomalies[k].v));
            ab.set("value", ev::fromDouble(sv.anomalies[k].value));
            return ab.get();
        }));
        return sb.get();
    }));
    {
        const auto& l = r.lighting;
        ObjectBuilder lb;
        lb.set("analyzed", ev::fromBool(l.analyzed));
        lb.set("highlightShare", ev::fromDouble(l.highlightShare));
        lb.set("halftoneShare", ev::fromDouble(l.halftoneShare));
        lb.set("coreShadowShare", ev::fromDouble(l.coreShadowShare));
        lb.set("reflectedLightShare", ev::fromDouble(l.reflectedLightShare));
        lb.set("warmCoolLightShift", ev::fromDouble(l.warmCoolLightShift));
        lb.set("valueTurnScore", ev::fromDouble(l.valueTurnScore));
        o.set("lighting", lb.get());
    }
    o.set("defects", hostArrayOf(r.defects.size(), [&](size_t i) {
        const auto& d = r.defects[i];
        ObjectBuilder db;
        ev::Persistent kd(ev::fromUtf8(d.kind));
        ev::Persistent msg(ev::fromUtf8(d.message));
        db.set("kind", kd.get());
        db.set("x", ev::fromDouble(d.x));
        db.set("y", ev::fromDouble(d.y));
        db.set("z", ev::fromDouble(d.z));
        db.set("value", ev::fromDouble(d.value));
        db.set("message", msg.get());
        return db.get();
    }));
    return o.get();
}

Value jsEyeCompose(Value, std::span<const Value> a) {
    EyeImage img;
    std::string err;
    const Value src = a.empty() ? ev::undefined() : a[0];
    if (!extractImage(src, img, /*allowLiveCapture=*/true, err)) {
        return ev::throwTypeError("bro.eye.compose: " + err);
    }

    EyeComposeOptions opts;
    Value ov = a.size() > 1 ? a[1] : ev::undefined();
    if (ev::isObject(ov)) {
        ev::Persistent r(ov);
        opts.maxAnchors = std::clamp(static_cast<int>(numProp(r.get(), "maxAnchors", 5.0)), 1, 10);
        opts.notanTones = static_cast<int>(numProp(r.get(), "notanTones", 3.0)) >= 5 ? 5 : 3;
        opts.rhythmRows = std::clamp(static_cast<int>(numProp(r.get(), "rhythmRows", 6.0)), 2, 16);
        opts.rhythmCols = std::clamp(static_cast<int>(numProp(r.get(), "rhythmCols", 8.0)), 2, 16);
        opts.includeLayout = boolProp(r.get(), "layout", false);
        opts.selector = strProp(r.get(), "selector");
        opts.railTolerancePx = numProp(r.get(), "railTolerancePx", 1.5);
        opts.driftMaxPx = numProp(r.get(), "driftMaxPx", 6.0);
    }

    EyeComposeResult res = analyzeComposition(img.view(), opts);
    if (opts.includeLayout && (ev::isUndefined(src) || ev::isNull(src))) {
        if (auto* eng = hostEngine()) {
            res.layout = analyzeLayout(*eng, opts);
        }
    }
    return composeToJs(res);
}

Value jsEyeForm(Value, std::span<const Value> a) {
#if !BRO_WITH_3D
    (void)a;
    return ev::throwError("bro.eye.form is unavailable: compiled without BRO_WITH_3D");
#else
    if (a.empty() || !ev::isObject(a[0])) {
        return ev::throwTypeError("bro.eye.form(meshOrScene, opts?) requires a Mesh or SceneGraph");
    }
    EyeFormOptions opts;
    Value ov = a.size() > 1 ? a[1] : ev::undefined();
    if (ev::isObject(ov)) {
        ev::Persistent r(ov);
        opts.focus = strProp(r.get(), "focus");
        opts.azimuthCount = std::clamp(static_cast<int>(numProp(r.get(), "azimuths", 8.0)), 1, 24);
        opts.elevationDeg = std::clamp(numProp(r.get(), "elevation", 8.0), -60.0, 60.0);
        opts.creaseAngleDeg = std::clamp(numProp(r.get(), "creaseAngle", 35.0), 5.0, 175.0);
        opts.sliceCount = std::clamp(static_cast<int>(numProp(r.get(), "slices", 8.0)), 2, 24);
        opts.silhouetteRes = std::clamp(static_cast<int>(numProp(r.get(), "silhouetteRes", 192.0)), 64, 320);
        opts.render = boolProp(r.get(), "render", true);
    }

    if (const auto* md = bromesh::api::meshDataOf(a[0])) {
        return formToJs(analyzeMeshForm(*md, opts));
    }
    if (void* raw = ev::handleData(a[0])) {
        auto* cell = static_cast<HostSceneGraphCell*>(raw);
        if (cell->tag == kHostSceneGraphTag) {
            auto* g = cell->graph();
            if (!g) return ev::throwTypeError("bro.eye.form: SceneGraph has been destroyed");
            if (!hostEngine()) return ev::throwError("bro.eye.form(SceneGraph) is unavailable in Worker realms");
            return formToJs(analyzeSceneForm(*g, opts));
        }
    }
    return ev::throwTypeError("bro.eye.form(meshOrScene, opts?) requires a Mesh or SceneGraph");
#endif
}

#if BRO_WITH_3D
bool trySceneTasteVisualize(Value src,
                            const EyeVisualizeOptions& vopts,
                            EyeVisualResult& outResult,
                            std::string& outErr) {
    if (vopts.mode != "turntable" && vopts.mode != "raking") return false;
    if (!ev::isObject(src)) {
        outErr = "bro.eye.visualize('" + vopts.mode + "') requires a live SceneGraph";
        return false;
    }
    void* raw = ev::handleData(src);
    auto* cell = raw ? static_cast<HostSceneGraphCell*>(raw) : nullptr;
    if (!cell || cell->tag != kHostSceneGraphTag || !cell->graph()) {
        outErr = "bro.eye.visualize('" + vopts.mode + "') requires a valid SceneGraph";
        return false;
    }
    scene::SceneGraph& graph = *cell->graph();

    // Compute focus bounding sphere center + camera orbital radius
    bromath::AABB3 subjectBox = bromath::aempty3();
    graph.root()->traverse([&](scene::SceneNode* n) {
        if (!n || !n->renderVisible() || n->type() != scene::SceneNode::Type::Mesh) return;
        auto* mn = static_cast<scene::MeshNode*>(n);
        if (!mn->hasDrawableMesh()) return;
        const bromath::AABB3 wb = bromath::atransform(mn->localBounds(), mn->worldMatrix());
        if (bromath::aisEmpty(wb)) return;
        const double xzSpan = std::max(wb.max.x - wb.min.x, wb.max.z - wb.min.z);
        const double yThick = wb.max.y - wb.min.y;
        const bool ground = (xzSpan > 4.0 && yThick < 0.25 * xzSpan);
        if (!ground) subjectBox = bromath::amerge(subjectBox, wb);
    });
    if (bromath::aisEmpty(subjectBox)) subjectBox = bromath::AABB3{{-1, -1, -1}, {1, 1, 1}};
    const bromath::Vec3 target = (subjectBox.min + subjectBox.max) * 0.5f;

    scene::CameraNode* savedActiveCam = graph.activeCamera();
    const bromath::Mat4 origV = graph.viewMatrix();
    const bromath::Mat4 origP = graph.projectionMatrix();
    const bromath::Vec3 origEye = graph.cameraEye();
    const float nearZ = graph.cameraNearZ();
    const float farZ = graph.cameraFarZ();
    const float fovY = graph.cameraFovY();
    const float p00 = origP.at(0, 0), p11 = origP.at(1, 1);
    const float aspect = (std::abs(p00) > 1e-6f && std::abs(p11) > 1e-6f) ? (p11 / p00) : 1.0f;
    const bromath::Vec3 fwd{-origV.at(2, 0), -origV.at(2, 1), -origV.at(2, 2)};
    const bromath::Vec3 up{0.0f, 1.0f, 0.0f};
    const float rad = std::max(0.5f, bromath::vlen(origEye - target));

    std::vector<EyeImage> panels;
    std::vector<std::string> labels;

    if (vopts.mode == "turntable") {
        if (savedActiveCam) graph.setActiveCamera(nullptr);
        const float elevY = origEye.y - target.y;
        const float horiz = std::max(0.25f, std::hypot(origEye.x - target.x, origEye.z - target.z));
        const float baseAng = std::atan2(origEye.x - target.x, origEye.z - target.z);
        const double degs[4] = {0.0, 45.0, 90.0, 180.0};
        const char* names[4] = {"front 0", "three-qtr 45", "profile 90", "back 180"};
        std::vector<uint8_t> shot45Depth;
        std::vector<float> z45;
        scene::vk::InspectCamera cam45{};
        int w45 = 0, h45 = 0;

        for (int i = 0; i < 4; ++i) {
            const float ang = baseAng + static_cast<float>(degs[i] * 3.14159265358979323846 / 180.0);
            const bromath::Vec3 eyePos{target.x + horiz * std::sin(ang), target.y + elevY, target.z + horiz * std::cos(ang)};
            graph.setCamera(fovY, aspect, nearZ, farZ, eyePos, target, up);
            graph.renderForRead();
            int w = 0, h = 0;
            auto px = graph.readTonemapPixelsRGBA(w, h);
            if (!px.empty()) panels.push_back({w, h, std::move(px)});
            labels.emplace_back(names[i]);
            if (i == 1) {
                graph.renderer().readInspectSnapshot(shot45Depth, z45, w45, h45, cam45);
            }
        }
        // Append zebra 3/4 + notan front panels
        if (!panels.empty() && !shot45Depth.empty() && !z45.empty()) {
            EyeVisualizeOptions zOpts;
            zOpts.mode = "zebra";
            zOpts.maxPanelDim = vopts.maxPanelDim;
            zOpts.zebraFreq = vopts.zebraFreq;
            zOpts.depth = z45.data();
            zOpts.depthWidth = w45;
            zOpts.depthHeight = h45;
            for (int c = 0; c < 4; ++c) {
                for (int r = 0; r < 4; ++r) {
                    zOpts.invProj[static_cast<size_t>(c) * 4 + r] = cam45.invProj.at(r, c);
                    zOpts.relView[static_cast<size_t>(c) * 4 + r] = cam45.relView.at(r, c);
                }
            }
            zOpts.hasCamera = true;
            EyeVisualResult zRes = visualizeImages({EyeImageView{shot45Depth.data(), w45, h45, w45 * 4}}, zOpts);
            if (zRes.panels.size() >= 2 && zRes.width > 0) {
                // Extract the zebra panel region cleanly via crop from zRes
                const auto& zp = zRes.panels[1];
                EyeImage zImg{zp.width, zp.height, std::vector<uint8_t>(static_cast<size_t>(zp.width) * zp.height * 4)};
                for (int y = 0; y < zp.height; ++y) {
                    for (int x = 0; x < zp.width; ++x) {
                        const uint8_t* sp = &zRes.rgba[(static_cast<size_t>(zp.y + y) * zRes.width + (zp.x + x)) * 4];
                        uint8_t* dp = &zImg.rgba[(static_cast<size_t>(y) * zp.width + x) * 4];
                        dp[0] = sp[0]; dp[1] = sp[1]; dp[2] = sp[2]; dp[3] = 255;
                    }
                }
                panels.push_back(std::move(zImg));
                labels.emplace_back("zebra 45");
            }
        }
        if (!panels.empty()) {
            EyeVisualizeOptions nOpts;
            nOpts.mode = "notan";
            nOpts.notanTones = vopts.notanTones;
            nOpts.maxPanelDim = vopts.maxPanelDim;
            EyeVisualResult nRes = visualizeImages({panels[0].view()}, nOpts);
            if (nRes.panels.size() >= 2 && nRes.width > 0) {
                const auto& np = nRes.panels[1];
                EyeImage nImg{np.width, np.height, std::vector<uint8_t>(static_cast<size_t>(np.width) * np.height * 4)};
                for (int y = 0; y < np.height; ++y) {
                    for (int x = 0; x < np.width; ++x) {
                        const uint8_t* sp = &nRes.rgba[(static_cast<size_t>(np.y + y) * nRes.width + (np.x + x)) * 4];
                        uint8_t* dp = &nImg.rgba[(static_cast<size_t>(y) * np.width + x) * 4];
                        dp[0] = sp[0]; dp[1] = sp[1]; dp[2] = sp[2]; dp[3] = 255;
                    }
                }
                panels.push_back(std::move(nImg));
                labels.emplace_back("notan front");
            }
        }
        // Restore camera
        if (savedActiveCam) graph.setActiveCamera(savedActiveCam);
        else graph.setCamera(fovY, aspect, nearZ, farZ, origEye, origEye + fwd * rad, {origV.at(1, 0), origV.at(1, 1), origV.at(1, 2)});
        graph.renderForRead();
    } else {
        // 'raking': test 4 directional light angles around the form to expose surface ripples
        struct SavedLight { scene::LightNode* node; bool vis; };
        std::vector<SavedLight> savedLights;
        graph.root()->traverse([&](scene::SceneNode* n) {
            if (n && n->type() == scene::SceneNode::Type::Light) {
                auto* ln = static_cast<scene::LightNode*>(n);
                savedLights.push_back({ln, ln->visible()});
                ln->setVisible(false);
            }
        });
        auto* rakeKey = graph.createLight("__eye_rake_key");
        rakeKey->setKind(scene::LightNode::Kind::Directional);
        rakeKey->setColor(1.0f, 0.98f, 0.94f);
        rakeKey->setCastsShadow(false);

        const struct { const char* name; bromath::Vec3 dir; float intensity; } setups[4] = {
            {"rake left-top", {0.86f, -0.45f, -0.22f}, 3.6f},
            {"rake right-rim", {-0.88f, -0.20f, 0.42f}, 4.0f},
            {"rake top-down", {0.10f, -0.98f, -0.15f}, 3.4f},
            {"rake under-bounce", {-0.35f, 0.75f, -0.55f}, 3.2f},
        };
        for (const auto& s : setups) {
            rakeKey->setDirection(s.dir);
            rakeKey->setIntensity(s.intensity);
            graph.renderForRead();
            int w = 0, h = 0;
            auto px = graph.readTonemapPixelsRGBA(w, h);
            if (!px.empty()) panels.push_back({w, h, std::move(px)});
            labels.emplace_back(s.name);
        }
        graph.destroyNode(rakeKey);
        for (const auto& sl : savedLights) sl.node->setVisible(sl.vis);
        graph.renderForRead();
    }

    if (panels.empty()) {
        outErr = "bro.eye.visualize('" + vopts.mode + "') captured no panels";
        return false;
    }

    std::vector<EyeImageView> pViews;
    pViews.reserve(panels.size());
    for (const auto& p : panels) pViews.push_back(p.view());

    EyeVisualizeOptions sheetOpts = vopts;
    sheetOpts.mode = "sheet";
    sheetOpts.layout = vopts.layout.empty() || vopts.layout == "side" ? "grid" : vopts.layout;
    sheetOpts.labels = !vopts.labels.empty() ? vopts.labels : labels;
    outResult = visualizeImages(pViews, sheetOpts);
    return !outResult.rgba.empty();
}
#endif

}  // namespace bro::bronze_host::eye_detail
