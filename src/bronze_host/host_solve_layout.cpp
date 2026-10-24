#include "bronze_host/host_solve.h"

#include "bronze_host/host_builder.h"

#include "check/layout_solver.h"
#include "check/solve_fn.h"

#include <climits>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace bro::bronze_host {
namespace {

bool extractLayoutFnSource(Value fnOrStr, std::string& outSource, std::string& outErr) {
    if (fnOrStr.isString()) {
        outSource = ev::toUtf8(fnOrStr);
        return !outSource.empty();
    }
    if (ev::isFunction(fnOrStr) || ev::isObject(fnOrStr)) {
        ev::Persistent fnObj(fnOrStr);
        Value toStrFn = ev::getProperty(fnObj.get(), "toString");
        if (ev::isFunction(toStrFn)) {
            ev::CallResult res = ev::call(toStrFn, fnObj.get(), {});
            if (!res.thrown && res.value.isString()) {
                outSource = ev::toUtf8(res.value);
                return !outSource.empty();
            }
        }
    }
    outErr = "expected a JavaScript function or source string";
    return false;
}

std::vector<std::string> getLayoutObjectKeys(Value obj) {
    std::vector<std::string> out;
    if (!ev::isObject(obj)) return out;
    ev::Persistent objRoot(obj);
    auto gv = ev::globalValue("Object");
    if (!gv.found || !ev::isObject(gv.value)) return out;
    ev::Persistent ctorRoot(gv.value);
    Value keysFn = ev::getProperty(ctorRoot.get(), "keys");
    if (!ev::isFunction(keysFn)) return out;
    ev::Persistent keysFnRoot(keysFn);
    Value callArgs[1] = {objRoot.get()};
    auto res = ev::call(keysFnRoot.get(), ctorRoot.get(), callArgs);
    if (res.thrown || !ev::isObject(res.value)) return out;
    ev::Persistent arrRoot(res.value);
    Value lenV = ev::getProperty(arrRoot.get(), "length");
    uint32_t n = lenV.isNumber() ? satCast<uint32_t>(lenV.asNumber()) : 0;
    out.reserve(n);
    for (uint32_t i = 0; i < n && i < 128; ++i) {
        Value elem = ev::getElement(arrRoot.get(), i);
        if (elem.isString()) out.push_back(ev::toUtf8(elem));
    }
    return out;
}

bronze::check::SolveOptions parseLayoutSolveOptions(Value optsVal) {
    bronze::check::SolveOptions opts;
    if (!ev::isObject(optsVal)) return opts;
    ev::Persistent root(optsVal);

    Value minV = ev::getProperty(root.get(), "min");
    if (minV.isUndefined()) minV = ev::getProperty(root.get(), "domainMin");
    if (minV.isNumber()) opts.domainMin = jsToInt32(minV.asNumber());

    Value maxV = ev::getProperty(root.get(), "max");
    if (maxV.isUndefined()) maxV = ev::getProperty(root.get(), "domainMax");
    if (maxV.isNumber()) opts.domainMax = jsToInt32(maxV.asNumber());

    Value minResV = ev::getProperty(root.get(), "minimize");
    if (minResV.isBool()) opts.minimizeResult = minResV.asBool();

    Value boundsV = ev::getProperty(root.get(), "bounds");
    if (ev::isObject(boundsV)) {
        ev::Persistent boundsObj(boundsV);
        std::vector<std::string> keys = getLayoutObjectKeys(boundsObj.get());
        for (const auto& k : keys) {
            Value entry = ev::getProperty(boundsObj.get(), k);
            if (!ev::isObject(entry)) continue;
            ev::Persistent entryObj(entry);
            Value lenV = ev::getProperty(entryObj.get(), "length");
            if (lenV.isNumber() && lenV.asNumber() >= 2.0) {
                Value loV = ev::getElement(entryObj.get(), 0);
                int32_t lo = loV.isNumber() ? jsToInt32(loV.asNumber()) : INT32_MIN;
                Value hiV = ev::getElement(entryObj.get(), 1);
                int32_t hi = hiV.isNumber() ? jsToInt32(hiV.asNumber()) : INT32_MAX;
                opts.varBounds[k] = {lo, hi};
            } else {
                Value loV = ev::getProperty(entryObj.get(), "min");
                int32_t lo = loV.isNumber() ? jsToInt32(loV.asNumber()) : INT32_MIN;
                Value hiV = ev::getProperty(entryObj.get(), "max");
                int32_t hi = hiV.isNumber() ? jsToInt32(hiV.asNumber()) : INT32_MAX;
                opts.varBounds[k] = {lo, hi};
            }
        }
    }
    return opts;
}

bronze::check::LayoutBoxSpec parseBoxSpecObject(const std::string& fallbackId, Value boxVal) {
    bronze::check::LayoutBoxSpec bs;
    bs.id = fallbackId;
    if (!ev::isObject(boxVal)) return bs;
    ev::Persistent bObj(boxVal);

    Value idV = ev::getProperty(bObj.get(), "id");
    if (idV.isString()) bs.id = ev::toUtf8(idV);

    Value is3DV = ev::getProperty(bObj.get(), "is3D");
    if (is3DV.isBool()) bs.is3D = is3DV.asBool();

    auto readFixedOrRange = [&](const char* fixedProp, const char* minProp, const char* maxProp,
                                int32_t& outMin, int32_t& outMax) {
        Value fixedV = ev::getProperty(bObj.get(), fixedProp);
        if (fixedV.isNumber()) {
            int32_t v = jsToInt32(fixedV.asNumber());
            outMin = v;
            outMax = v;
            return;
        }
        Value minV = ev::getProperty(bObj.get(), minProp);
        if (minV.isNumber()) outMin = jsToInt32(minV.asNumber());
        Value maxV = ev::getProperty(bObj.get(), maxProp);
        if (maxV.isNumber()) outMax = jsToInt32(maxV.asNumber());
    };

    readFixedOrRange("x", "minX", "maxX", bs.minX, bs.maxX);
    readFixedOrRange("y", "minY", "maxY", bs.minY, bs.maxY);
    readFixedOrRange("z", "minZ", "maxZ", bs.minZ, bs.maxZ);
    readFixedOrRange("w", "minW", "maxW", bs.minW, bs.maxW);
    readFixedOrRange("width", "minWidth", "maxWidth", bs.minW, bs.maxW);
    readFixedOrRange("h", "minH", "maxH", bs.minH, bs.maxH);
    readFixedOrRange("height", "minHeight", "maxHeight", bs.minH, bs.maxH);
    readFixedOrRange("d", "minD", "maxD", bs.minD, bs.maxD);
    if (!ev::getProperty(bObj.get(), "z").isUndefined() ||
        !ev::getProperty(bObj.get(), "d").isUndefined() ||
        !ev::getProperty(bObj.get(), "minZ").isUndefined() ||
        !ev::getProperty(bObj.get(), "minD").isUndefined()) {
        bs.is3D = true;
    }

    Value snapV = ev::getProperty(bObj.get(), "snapGrid");
    if (snapV.isUndefined()) snapV = ev::getProperty(bObj.get(), "grid");
    if (snapV.isNumber()) bs.snapGrid = jsToInt32(snapV.asNumber());

    Value arV = ev::getProperty(bObj.get(), "aspectRatio");
    if (ev::isObject(arV)) {
        ev::Persistent arObj(arV);
        Value nV = ev::getElement(arObj.get(), 0);
        bool hasN = nV.isNumber();
        int32_t numVal = hasN ? jsToInt32(nV.asNumber()) : 0;
        Value dV = ev::getElement(arObj.get(), 1);
        if (hasN && dV.isNumber()) {
            bs.aspectNum = numVal;
            bs.aspectDen = jsToInt32(dV.asNumber());
        }
    } else {
        Value nV = ev::getProperty(bObj.get(), "aspectNum");
        bool hasN = nV.isNumber();
        int32_t numVal = hasN ? jsToInt32(nV.asNumber()) : 0;
        Value dV = ev::getProperty(bObj.get(), "aspectDen");
        if (hasN && dV.isNumber()) {
            bs.aspectNum = numVal;
            bs.aspectDen = jsToInt32(dV.asNumber());
        }
    }
    return bs;
}

Value buildLayoutSolutionJsObject(const bronze::check::LayoutSolution& sol) {
    if (!sol.sat()) return ev::null();

    ObjectBuilder out;
    ev::Persistent stStr(ev::fromUtf8("sat"));
    out.set("status", stStr.get());
    out.set("sat", ev::fromBool(true));
    out.set("solveTimeMs", ev::fromDouble(sol.solveTimeMs));

    ObjectBuilder varsObj;
    for (const auto& [k, v] : sol.vars) {
        Value numV = ev::fromDouble(static_cast<double>(v));
        varsObj.set(k.c_str(), numV);
        if (k.find('.') == std::string::npos) {
            out.set(k.c_str(), numV);
        }
    }
    out.set("vars", varsObj.get());

    ObjectBuilder boxesObj;
    for (const auto& sb : sol.boxList) {
        ObjectBuilder b;
        ev::Persistent idStr(ev::fromUtf8(sb.id));
        b.set("id", idStr.get());
        b.set("x", ev::fromDouble(static_cast<double>(sb.x)));
        b.set("y", ev::fromDouble(static_cast<double>(sb.y)));
        b.set("w", ev::fromDouble(static_cast<double>(sb.w)));
        b.set("h", ev::fromDouble(static_cast<double>(sb.h)));
        b.set("width", ev::fromDouble(static_cast<double>(sb.w)));
        b.set("height", ev::fromDouble(static_cast<double>(sb.h)));
        b.set("right", ev::fromDouble(static_cast<double>(sb.x + sb.w)));
        b.set("bottom", ev::fromDouble(static_cast<double>(sb.y + sb.h)));
        if (sb.is3D) {
            b.set("z", ev::fromDouble(static_cast<double>(sb.z)));
            b.set("d", ev::fromDouble(static_cast<double>(sb.d)));
            b.set("depth", ev::fromDouble(static_cast<double>(sb.d)));
        }
        ev::Persistent boxVal(b.get());
        boxesObj.set(sb.id.c_str(), boxVal.get());
        out.set(sb.id.c_str(), boxVal.get());
    }
    out.set("boxes", boxesObj.get());
    return out.get();
}

Value jsBroLayoutSolve(Value, std::span<const Value> args) {
    if (!hasArg(args, 0)) {
        return ev::throwError("bro.layout.solve: missing layout specification or variable list");
    }

    if (ev::isFunction(args[0]) || args[0].isString()) {
        std::string fnSrc, err;
        if (!extractLayoutFnSource(args[0], fnSrc, err)) {
            return ev::throwError(("bro.layout.solve: " + err).c_str());
        }
        bronze::check::SolveOptions opts = hasArg(args, 1) ? parseLayoutSolveOptions(args[1])
                                                           : bronze::check::SolveOptions{};
        auto sol = bronze::check::solveLayoutPredicate({}, fnSrc, opts);
        if (sol.status == bronze::check::SolveStatus::Error) {
            return ev::throwError(("bro.layout.solve: " + sol.error).c_str());
        }
        return buildLayoutSolutionJsObject(sol);
    }

    if (!ev::isObject(args[0])) {
        return ev::throwError("bro.layout.solve: first argument must be an array, object, or function");
    }

    ev::Persistent firstArg(args[0]);
    Value lenV = ev::getProperty(firstArg.get(), "length");
    if (lenV.isNumber()) {
        uint32_t n = satCast<uint32_t>(lenV.asNumber());
        std::vector<std::string> varNames;
        varNames.reserve(n);
        for (uint32_t i = 0; i < n && i < 64; ++i) {
            Value elem = ev::getElement(firstArg.get(), i);
            varNames.push_back(ev::toUtf8(elem));
        }
        if (!hasArg(args, 1)) {
            return ev::throwError("bro.layout.solve(vars, constraintFn, opts): missing constraintFn");
        }
        std::string fnSrc, err;
        if (!extractLayoutFnSource(args[1], fnSrc, err)) {
            return ev::throwError(("bro.layout.solve: " + err).c_str());
        }
        bronze::check::SolveOptions opts = hasArg(args, 2) ? parseLayoutSolveOptions(args[2])
                                                           : bronze::check::SolveOptions{};
        auto sol = bronze::check::solveLayoutPredicate(varNames, fnSrc, opts);
        if (sol.status == bronze::check::SolveStatus::Error) {
            return ev::throwError(("bro.layout.solve: " + sol.error).c_str());
        }
        return buildLayoutSolutionJsObject(sol);
    }

    bronze::check::LayoutSpec spec;
    Value contV = ev::getProperty(firstArg.get(), "container");
    if (contV.isBool() && !contV.asBool()) {
        spec.hasContainer = false;
    } else if (ev::isObject(contV)) {
        ev::Persistent cObj(contV);
        Value wV = ev::getProperty(cObj.get(), "w");
        if (wV.isUndefined()) wV = ev::getProperty(cObj.get(), "width");
        if (wV.isNumber()) spec.containerW = jsToInt32(wV.asNumber());

        Value hV = ev::getProperty(cObj.get(), "h");
        if (hV.isUndefined()) hV = ev::getProperty(cObj.get(), "height");
        if (hV.isNumber()) spec.containerH = jsToInt32(hV.asNumber());

        Value dV = ev::getProperty(cObj.get(), "d");
        if (dV.isUndefined()) dV = ev::getProperty(cObj.get(), "depth");
        if (dV.isNumber()) spec.containerD = jsToInt32(dV.asNumber());
    }

    Value nonOverlapV = ev::getProperty(firstArg.get(), "nonOverlap");
    if (nonOverlapV.isBool()) spec.nonOverlap = nonOverlapV.asBool();

    Value gapV = ev::getProperty(firstArg.get(), "gap");
    if (gapV.isNumber()) spec.gap = jsToInt32(gapV.asNumber());

    Value snapV = ev::getProperty(firstArg.get(), "snapGrid");
    if (snapV.isUndefined()) snapV = ev::getProperty(firstArg.get(), "grid");
    if (snapV.isNumber()) spec.globalSnapGrid = jsToInt32(snapV.asNumber());

    Value boxesV = ev::getProperty(firstArg.get(), "boxes");
    if (ev::isObject(boxesV)) {
        ev::Persistent boxesObj(boxesV);
        Value bLenV = ev::getProperty(boxesObj.get(), "length");
        if (bLenV.isNumber()) {
            uint32_t nb = satCast<uint32_t>(bLenV.asNumber());
            for (uint32_t i = 0; i < nb && i < 32; ++i) {
                Value item = ev::getElement(boxesObj.get(), i);
                spec.boxes.push_back(parseBoxSpecObject("box_" + std::to_string(i), item));
            }
        } else {
            std::vector<std::string> keys = getLayoutObjectKeys(boxesObj.get());
            for (const auto& k : keys) {
                Value item = ev::getProperty(boxesObj.get(), k);
                spec.boxes.push_back(parseBoxSpecObject(k, item));
            }
        }
    } else {
        std::vector<std::string> keys = getLayoutObjectKeys(firstArg.get());
        for (const auto& k : keys) {
            if (k == "container" || k == "nonOverlap" || k == "gap" ||
                k == "snapGrid" || k == "grid" || k == "constraints" || k == "predicate") {
                continue;
            }
            Value item = ev::getProperty(firstArg.get(), k);
            if (ev::isObject(item)) {
                spec.boxes.push_back(parseBoxSpecObject(k, item));
            }
        }
    }

    Value cstrsV = ev::getProperty(firstArg.get(), "constraints");
    if (ev::isFunction(cstrsV)) {
        std::string psrc, perr;
        if (extractLayoutFnSource(cstrsV, psrc, perr)) spec.predicateFnSource = std::move(psrc);
    } else if (ev::isObject(cstrsV)) {
        ev::Persistent cArr(cstrsV);
        Value cLenV = ev::getProperty(cArr.get(), "length");
        uint32_t nc = cLenV.isNumber() ? satCast<uint32_t>(cLenV.asNumber()) : 0;
        for (uint32_t i = 0; i < nc && i < 128; ++i) {
            Value cItem = ev::getElement(cArr.get(), i);
            if (cItem.isString()) spec.constraints.push_back(ev::toUtf8(cItem));
        }
    }

    if (hasArg(args, 1)) {
        std::string psrc, perr;
        if (extractLayoutFnSource(args[1], psrc, perr)) {
            spec.predicateFnSource = std::move(psrc);
        }
    }

    auto sol = bronze::check::solveLayoutSpec(spec);
    if (sol.status == bronze::check::SolveStatus::Error) {
        return ev::throwError(("bro.layout.solve: " + sol.error).c_str());
    }
    return buildLayoutSolutionJsObject(sol);
}

}  // namespace

void attachBroLayoutApis(Value broObj) {
    if (!ev::isObject(broObj)) return;
    ObjectBuilder bro(broObj);
    ObjectBuilder layoutNs;
    layoutNs.set("available", ev::fromBool(true));
    layoutNs.def("solve", 3, jsBroLayoutSolve);
    bro.set("layout", layoutNs.get());
}

}  // namespace bro::bronze_host
