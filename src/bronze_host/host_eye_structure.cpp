#include "bronze_host/host_eye_creative.h"
#include "bronze_host/host_access.h"
#include "bronze_host/host_builder.h"
#include "bronze_host/host_globals_internal.h"
#include "bronze_host/host_runtime.h"
#include "bronze_host/host_values.h"

#if BRO_WITH_3D
#include "bronze_host/native_scene_internal.h"
#include "eye/eye_structure.h"
#include "scene/scene_graph.h"
#endif

#include <algorithm>
#include <cmath>
#include <span>
#include <string>
#include <vector>

namespace bro::bronze_host::eye_detail {

#if BRO_WITH_3D
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

static bool readVec3(Value val, double out[3]) {
    if (!ev::isObject(val)) return false;
    ev::Persistent root(val);
    const double lenD = ev::toDouble(ev::getProperty(root.get(), "length"));
    if (!std::isfinite(lenD) || lenD < 3.0) return false;
    for (int k = 0; k < 3; ++k) {
        const double d = ev::toDouble(ev::getElement(root.get(), static_cast<uint32_t>(k)));
        if (!std::isfinite(d)) return false;
        out[k] = d;
    }
    return true;
}

static void parseStructureWalkOpts(Value walkVal, EyeStructureInteriorWalkOptions& wk) {
    if (walkVal.isBool()) {
        wk.enabled = walkVal.asBool();
        return;
    }
    if (!ev::isObject(walkVal)) return;
    ev::Persistent wRoot(walkVal);
    wk.enabled = boolProp(wRoot.get(), "enabled", true);
    wk.agentRadius = std::clamp(numProp(wRoot.get(), "agentRadius", wk.agentRadius), 0.10, 2.0);
    wk.agentHeight = std::clamp(numProp(wRoot.get(), "agentHeight", wk.agentHeight), 0.50, 4.0);
    wk.stepUp = std::clamp(numProp(wRoot.get(), "stepUp", wk.stepUp), 0.05, 1.5);
    wk.minHeadroom = std::clamp(numProp(wRoot.get(), "minHeadroom", wk.minHeadroom), wk.agentHeight, 6.0);
    wk.maxFloorGap = std::clamp(numProp(wRoot.get(), "maxFloorGap", wk.maxFloorGap), 0.02, 2.0);
    wk.sampleStep = std::clamp(numProp(wRoot.get(), "sampleStep", wk.sampleStep), 0.05, 1.0);

    Value igVal = ev::getProperty(wRoot.get(), "ignoreNodes");
    if (ev::isObject(igVal)) {
        ev::Persistent ia(igVal);
        const uint32_t in = static_cast<uint32_t>(std::max(0.0, std::min(ev::toDouble(ev::getProperty(ia.get(), "length")), 128.0)));
        for (uint32_t i = 0; i < in; ++i) wk.ignoreNodes.push_back(ev::toUtf8(ev::getElement(ia.get(), i)));
    }

    Value dwVal = ev::getProperty(wRoot.get(), "doorways");
    if (ev::isObject(dwVal)) {
        ev::Persistent da(dwVal);
        const uint32_t dn = static_cast<uint32_t>(std::max(0.0, std::min(ev::toDouble(ev::getProperty(da.get(), "length")), 256.0)));
        for (uint32_t i = 0; i < dn; ++i) {
            Value item = ev::getElement(da.get(), i);
            if (!ev::isObject(item)) continue;
            ev::Persistent di(item);
            EyeStructureDoorwaySpec spec;
            spec.name = strProp(di.get(), "name");
            if (!readVec3(ev::getProperty(di.get(), "position"), spec.position)) {
                if (!readVec3(ev::getProperty(di.get(), "pos"), spec.position)) continue;
            }
            (void)readVec3(ev::getProperty(di.get(), "normal"), spec.normal);
            spec.width = numProp(di.get(), "width", spec.width);
            spec.height = numProp(di.get(), "height", spec.height);
            spec.depth = numProp(di.get(), "depth", spec.depth);
            wk.doorways.push_back(std::move(spec));
        }
    }

    Value crVal = ev::getProperty(wRoot.get(), "corridors");
    if (ev::isObject(crVal)) {
        ev::Persistent ca(crVal);
        const uint32_t cn = static_cast<uint32_t>(std::max(0.0, std::min(ev::toDouble(ev::getProperty(ca.get(), "length")), 256.0)));
        for (uint32_t i = 0; i < cn; ++i) {
            Value item = ev::getElement(ca.get(), i);
            if (!ev::isObject(item)) continue;
            ev::Persistent ci(item);
            EyeStructureCorridorSpec spec;
            spec.name = strProp(ci.get(), "name");
            if (!readVec3(ev::getProperty(ci.get(), "start"), spec.start)) continue;
            if (!readVec3(ev::getProperty(ci.get(), "end"), spec.end)) continue;
            spec.width = numProp(ci.get(), "width", spec.width);
            spec.height = numProp(ci.get(), "height", spec.height);
            wk.corridors.push_back(std::move(spec));
        }
    }

    Value stVal = ev::getProperty(wRoot.get(), "stairs");
    if (ev::isObject(stVal)) {
        ev::Persistent sa(stVal);
        const uint32_t sn = static_cast<uint32_t>(std::max(0.0, std::min(ev::toDouble(ev::getProperty(sa.get(), "length")), 256.0)));
        for (uint32_t i = 0; i < sn; ++i) {
            Value item = ev::getElement(sa.get(), i);
            if (!ev::isObject(item)) continue;
            ev::Persistent si(item);
            EyeStructureStairSpec spec;
            spec.name = strProp(si.get(), "name");
            if (!readVec3(ev::getProperty(si.get(), "start"), spec.start)) continue;
            if (!readVec3(ev::getProperty(si.get(), "end"), spec.end)) continue;
            spec.width = numProp(si.get(), "width", spec.width);
            spec.headroom = numProp(si.get(), "headroom", spec.headroom);
            wk.stairs.push_back(std::move(spec));
        }
    }
}
#endif

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
        sopts.checkIslands = boolProp(optsRoot.get(), "checkIslands", sopts.checkIslands);
        sopts.maxPanelDim = std::clamp(static_cast<int>(numProp(optsRoot.get(), "maxPanelDim", sopts.maxPanelDim)), 128, 1024);
        sopts.maxZoomPanels = std::clamp(static_cast<int>(numProp(optsRoot.get(), "maxZoomPanels", sopts.maxZoomPanels)), 1, 12);
        sopts.maxInstances = std::clamp(static_cast<int>(numProp(optsRoot.get(), "maxInstances", sopts.maxInstances)), 1, 65536);
        sopts.autoDetectFloors = boolProp(optsRoot.get(), "autoDetectFloors", sopts.autoDetectFloors);
        sopts.minFloorSeparation = std::max(0.1, numProp(optsRoot.get(), "minFloorSeparation", sopts.minFloorSeparation));

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

        Value bndVal = ev::getProperty(optsRoot.get(), "bounds");
        if (ev::isObject(bndVal)) {
            ev::Persistent bRoot(bndVal);
            if (readVec3(ev::getProperty(bRoot.get(), "min"), sopts.boundsMin) &&
                readVec3(ev::getProperty(bRoot.get(), "max"), sopts.boundsMax)) {
                sopts.hasBounds = true;
            } else {
                const double lenD = ev::toDouble(ev::getProperty(bRoot.get(), "length"));
                if (std::isfinite(lenD) && lenD >= 6.0) {
                    bool ok6 = true;
                    for (int k = 0; k < 3; ++k) {
                        sopts.boundsMin[k] = ev::toDouble(ev::getElement(bRoot.get(), static_cast<uint32_t>(k)));
                        sopts.boundsMax[k] = ev::toDouble(ev::getElement(bRoot.get(), static_cast<uint32_t>(k + 3)));
                        if (!std::isfinite(sopts.boundsMin[k]) || !std::isfinite(sopts.boundsMax[k])) ok6 = false;
                    }
                    sopts.hasBounds = ok6;
                }
            }
        }

        Value nearVal = ev::getProperty(optsRoot.get(), "near");
        if (ev::isObject(nearVal)) {
            ev::Persistent nRoot(nearVal);
            if (readVec3(ev::getProperty(nRoot.get(), "center"), sopts.nearCenter) ||
                readVec3(ev::getProperty(nRoot.get(), "position"), sopts.nearCenter)) {
                const double r = numProp(nRoot.get(), "radius", 0.0);
                if (r > 0.0) {
                    sopts.nearRadius = r;
                    sopts.hasNear = true;
                }
            } else {
                const double lenD = ev::toDouble(ev::getProperty(nRoot.get(), "length"));
                if (std::isfinite(lenD) && lenD >= 4.0) {
                    const double x = ev::toDouble(ev::getElement(nRoot.get(), 0));
                    const double y = ev::toDouble(ev::getElement(nRoot.get(), 1));
                    const double z = ev::toDouble(ev::getElement(nRoot.get(), 2));
                    const double r = ev::toDouble(ev::getElement(nRoot.get(), 3));
                    if (std::isfinite(x) && std::isfinite(y) && std::isfinite(z) && std::isfinite(r) && r > 0.0) {
                        sopts.nearCenter[0] = x;
                        sopts.nearCenter[1] = y;
                        sopts.nearCenter[2] = z;
                        sopts.nearRadius = r;
                        sopts.hasNear = true;
                    }
                }
            }
        }

        Value flVal = ev::getProperty(optsRoot.get(), "floors");
        if (ev::isString(flVal)) {
            if (ev::toUtf8(flVal) == "auto") sopts.autoDetectFloors = true;
        } else if (ev::isObject(flVal)) {
            ev::Persistent fa(flVal);
            const uint32_t fn = static_cast<uint32_t>(std::max(0.0, std::min(ev::toDouble(ev::getProperty(fa.get(), "length")), 128.0)));
            for (uint32_t i = 0; i < fn; ++i) {
                Value item = ev::getElement(fa.get(), i);
                if (item.isNumber() && std::isfinite(item.asNumber())) {
                    EyeStructureFloorSpec fs;
                    fs.y = item.asNumber();
                    sopts.floors.push_back(std::move(fs));
                } else if (ev::isObject(item)) {
                    ev::Persistent fi(item);
                    const double y = numProp(fi.get(), "y", std::numeric_limits<double>::quiet_NaN());
                    if (!std::isfinite(y)) continue;
                    EyeStructureFloorSpec fs;
                    fs.y = y;
                    fs.tolerance = std::clamp(numProp(fi.get(), "tolerance", fs.tolerance), 0.005, 0.5);
                    fs.name = strProp(fi.get(), "name");
                    sopts.floors.push_back(std::move(fs));
                }
            }
        }

        Value ggVal = ev::getProperty(optsRoot.get(), "groundGrid");
        if (ev::isObject(ggVal)) {
            ev::Persistent ggRoot(ggVal);
            const double ox = numProp(ggRoot.get(), "originX", std::numeric_limits<double>::quiet_NaN());
            const double oz = numProp(ggRoot.get(), "originZ", std::numeric_limits<double>::quiet_NaN());
            const double step = numProp(ggRoot.get(), "step", 0.0);
            const int cols = static_cast<int>(numProp(ggRoot.get(), "cols", 0));
            const int rows = static_cast<int>(numProp(ggRoot.get(), "rows", 0));
            std::vector<float> h;
            if (std::isfinite(ox) && std::isfinite(oz) && std::isfinite(step) && step > 0.0 &&
                cols >= 2 && rows >= 2 && cols <= 1024 && rows <= 1024 &&
                readFloatVector(ev::getProperty(ggRoot.get(), "heights"), h) &&
                h.size() >= static_cast<size_t>(cols) * static_cast<size_t>(rows)) {
                sopts.groundGrid.originX = ox;
                sopts.groundGrid.originZ = oz;
                sopts.groundGrid.step = step;
                sopts.groundGrid.cols = cols;
                sopts.groundGrid.rows = rows;
                sopts.groundGrid.heights = std::move(h);
            }
        }

        Value wkVal = ev::getProperty(optsRoot.get(), "interiorWalk");
        parseStructureWalkOpts(wkVal, sopts.interiorWalk);
    }

    const EyeStructureResult r = analyzeStructure(*graph, sopts);
    ObjectBuilder o;
    o.set("ok", ev::fromBool(r.ok));
    o.set("meshCount", ev::fromDouble(r.meshCount));
    o.set("instancedMeshCount", ev::fromDouble(r.instancedMeshCount));
    o.set("invertedCount", ev::fromDouble(r.invertedCount));
    o.set("floatingCount", ev::fromDouble(r.floatingCount));
    o.set("invertedIslandCount", ev::fromDouble(r.invertedIslandCount));
    o.set("floatingIslandCount", ev::fromDouble(r.floatingIslandCount));
    o.set("overhangCount", ev::fromDouble(r.overhangCount));
    o.set("gapCount", ev::fromDouble(r.gapCount));
    o.set("floorDefectCount", ev::fromDouble(r.floorDefectCount));
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
    o.set("floors", hostArrayOf(r.floors.size(), [&](size_t i) {
        const auto& fl = r.floors[i];
        ObjectBuilder fb;
        fb.set("name", ev::fromUtf8(fl.name));
        fb.set("y", ev::fromDouble(fl.y));
        fb.set("tolerance", ev::fromDouble(fl.tolerance));
        fb.set("slabCount", ev::fromDouble(fl.slabCount));
        fb.set("entryCount", ev::fromDouble(fl.entryCount));
        fb.set("floatingCount", ev::fromDouble(fl.floatingCount));
        fb.set("overhangCount", ev::fromDouble(fl.overhangCount));
        return fb.get();
    }));
    {
        ObjectBuilder wb;
        wb.set("checked", ev::fromBool(r.interiorWalk.checked));
        wb.set("doorwaysChecked", ev::fromDouble(r.interiorWalk.doorwaysChecked));
        wb.set("doorwaysBlocked", ev::fromDouble(r.interiorWalk.doorwaysBlocked));
        wb.set("corridorsChecked", ev::fromDouble(r.interiorWalk.corridorsChecked));
        wb.set("corridorsBlocked", ev::fromDouble(r.interiorWalk.corridorsBlocked));
        wb.set("stairsChecked", ev::fromDouble(r.interiorWalk.stairsChecked));
        wb.set("stairsBroken", ev::fromDouble(r.interiorWalk.stairsBroken));
        wb.set("headroomIssues", ev::fromDouble(r.interiorWalk.headroomIssues));
        wb.set("walkIssueCount", ev::fromDouble(r.interiorWalk.walkIssueCount));
        o.set("interiorWalk", wb.get());
    }
    if (!r.visualPath.empty()) o.set("path", ev::fromUtf8(r.visualPath));
    return o.get();
#endif
}

}  // namespace bro::bronze_host::eye_detail
