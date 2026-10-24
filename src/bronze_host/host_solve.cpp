#include "bronze_host/host_solve.h"

#include "bronze_host/host_builder.h"

#include "check/cegis_optimize.h"
#include "check/fuzz_fn.h"
#include "check/prove_fn.h"
#include "check/solve_fn.h"

#include <climits>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace bro::bronze_host {
namespace {

bool extractJsFunctionSource(Value fnOrStr, std::string& outSource, std::string& outErr) {
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

std::vector<std::string> getObjectKeys(Value obj) {
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

bronze::check::SolveOptions parseSolveOptions(Value optsVal) {
    bronze::check::SolveOptions opts;
    if (!ev::isObject(optsVal)) return opts;
    ev::Persistent root(optsVal);

    Value targetV = ev::getProperty(root.get(), "target");
    if (!targetV.isUndefined() && !targetV.isNull()) {
        opts.matchReturnValue = true;
        if (targetV.isBool()) {
            opts.targetReturn = targetV.asBool() ? 1 : 0;
        } else {
            opts.targetReturn = jsToInt32(ev::toDouble(targetV));
        }
    }

    bool hasExplicitMin = false;
    bool hasExplicitMax = false;
    Value minV = ev::getProperty(root.get(), "min");
    if (minV.isUndefined()) minV = ev::getProperty(root.get(), "domainMin");
    if (minV.isNumber()) {
        opts.domainMin = jsToInt32(minV.asNumber());
        hasExplicitMin = true;
    }

    Value maxV = ev::getProperty(root.get(), "max");
    if (maxV.isUndefined()) maxV = ev::getProperty(root.get(), "domainMax");
    if (maxV.isNumber()) {
        opts.domainMax = jsToInt32(maxV.asNumber());
        hasExplicitMax = true;
    }

    Value bwV = ev::getProperty(root.get(), "bitWidth");
    if (bwV.isNumber()) {
        int bw = jsToInt32(bwV.asNumber());
        if (bw > 0 && bw < 32) {
            Value unsignedV = ev::getProperty(root.get(), "unsigned");
            bool isUnsigned = unsignedV.isBool() && unsignedV.asBool();
            if (!hasExplicitMin) {
                opts.domainMin = isUnsigned ? 0 : -(1 << (bw - 1));
            }
            if (!hasExplicitMax) {
                opts.domainMax = isUnsigned ? ((1 << bw) - 1) : ((1 << (bw - 1)) - 1);
            }
        }
    }

    Value minResV = ev::getProperty(root.get(), "minimize");
    if (minResV.isBool()) {
        opts.minimizeResult = minResV.asBool();
    }

    Value fnNameV = ev::getProperty(root.get(), "fnName");
    if (fnNameV.isString()) {
        opts.fnName = ev::toUtf8(fnNameV);
    }

    Value boundsV = ev::getProperty(root.get(), "bounds");
    if (ev::isObject(boundsV)) {
        ev::Persistent boundsObj(boundsV);
        std::vector<std::string> keys = getObjectKeys(boundsObj.get());
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

Value makeInt64JsArray(const std::vector<int64_t>& vals) {
    return hostArrayOf(vals.size(), [&](size_t i) -> Value {
        return ev::fromDouble(static_cast<double>(vals[i]));
    });
}

// ---------------------------------------------------------------------------
// #128: bro.solve / bro.solveWithCert / bro.solveAll
// ---------------------------------------------------------------------------

Value jsBroSolve(Value, std::span<const Value> args) {
    if (!hasArg(args, 0)) {
        return ev::throwError("bro.solve(fn, opts): missing function argument");
    }
    std::string fnSrc;
    std::string err;
    if (!extractJsFunctionSource(args[0], fnSrc, err)) {
        return ev::throwError(("bro.solve: " + err).c_str());
    }
    bronze::check::SolveOptions opts = hasArg(args, 1) ? parseSolveOptions(args[1])
                                                       : bronze::check::SolveOptions{};
    bronze::check::SolveResult res = bronze::check::solveJsFunction(fnSrc, opts);
    if (res.status == bronze::check::SolveStatus::Error) {
        return ev::throwError(("bro.solve: " + res.error).c_str());
    }
    if (!res.sat()) {
        return ev::null();
    }

    ev::Persistent arr(makeInt64JsArray(res.args));
    ObjectBuilder modelBuilder;
    for (size_t i = 0; i < res.paramNames.size() && i < res.args.size(); ++i) {
        const std::string& pname = res.paramNames[i];
        Value numVal = ev::fromDouble(static_cast<double>(res.args[i]));
        modelBuilder.set(pname.c_str(), numVal);
        if (pname != "length") {
            arr.set(ev::setProperty(arr.get(), pname, numVal));
        }
    }
    arr.set(ev::setProperty(arr.get(), "model", modelBuilder.get()));
    arr.set(ev::setProperty(arr.get(), "returnValue", ev::fromDouble(static_cast<double>(res.returnValue))));
    arr.set(ev::setProperty(arr.get(), "solveTimeMs", ev::fromDouble(res.solveTimeMs)));
    ev::Persistent statusStr(ev::fromUtf8("sat"));
    arr.set(ev::setProperty(arr.get(), "status", statusStr.get()));
    return arr.get();
}

Value jsBroSolveWithCert(Value, std::span<const Value> args) {
    if (!hasArg(args, 0)) {
        return ev::throwError("bro.solveWithCert(fn, opts): missing function argument");
    }
    std::string fnSrc;
    std::string err;
    if (!extractJsFunctionSource(args[0], fnSrc, err)) {
        return ev::throwError(("bro.solveWithCert: " + err).c_str());
    }
    bronze::check::SolveOptions opts = hasArg(args, 1) ? parseSolveOptions(args[1])
                                                       : bronze::check::SolveOptions{};
    bronze::check::SolveResult res = bronze::check::solveJsFunction(fnSrc, opts);
    if (res.status == bronze::check::SolveStatus::Error) {
        return ev::throwError(("bro.solveWithCert: " + res.error).c_str());
    }

    ObjectBuilder out;
    const char* statusStr = res.sat() ? "sat" : (res.unsat() ? "unsat" : "unknown");
    ev::Persistent stVal(ev::fromUtf8(statusStr));
    out.set("status", stVal.get());
    out.set("sat", ev::fromBool(res.sat()));
    out.set("unsat", ev::fromBool(res.unsat()));
    out.set("solveTimeMs", ev::fromDouble(res.solveTimeMs));

    if (res.sat()) {
        ev::Persistent argsArr(makeInt64JsArray(res.args));
        out.set("args", argsArr.get());
        ObjectBuilder modelBuilder;
        for (size_t i = 0; i < res.paramNames.size() && i < res.args.size(); ++i) {
            modelBuilder.set(res.paramNames[i].c_str(), ev::fromDouble(static_cast<double>(res.args[i])));
        }
        out.set("model", modelBuilder.get());
        out.set("returnValue", ev::fromDouble(static_cast<double>(res.returnValue)));
    } else {
        out.set("args", ev::null());
        out.set("model", ev::null());
        out.set("returnValue", ev::undefined());
    }
    return out.get();
}

Value jsBroSolveAll(Value, std::span<const Value> args) {
    if (!hasArg(args, 0)) {
        return ev::throwError("bro.solveAll(fn, maxSolutions, opts): missing function argument");
    }
    std::string fnSrc;
    std::string err;
    if (!extractJsFunctionSource(args[0], fnSrc, err)) {
        return ev::throwError(("bro.solveAll: " + err).c_str());
    }

    size_t maxSolutions = 10;
    bronze::check::SolveOptions opts;
    if (hasArg(args, 1)) {
        if (args[1].isNumber()) {
            int32_t m = jsToInt32(args[1].asNumber());
            maxSolutions = (m > 0) ? static_cast<size_t>(m) : 0;
            if (hasArg(args, 2)) opts = parseSolveOptions(args[2]);
        } else if (ev::isObject(args[1])) {
            opts = parseSolveOptions(args[1]);
            ev::Persistent optRoot(args[1]);
            Value maxSolV = ev::getProperty(optRoot.get(), "maxSolutions");
            if (maxSolV.isNumber()) {
                int32_t m = jsToInt32(maxSolV.asNumber());
                maxSolutions = (m > 0) ? static_cast<size_t>(m) : 0;
            }
        }
    }

    bronze::check::SolveResult meta = bronze::check::solveJsFunction(fnSrc, opts);
    auto tuples = bronze::check::solveAllJsFunction(fnSrc, maxSolutions, opts);
    return hostArrayOf(tuples.size(), [&](size_t i) -> Value {
        ev::Persistent row(makeInt64JsArray(tuples[i]));
        for (size_t j = 0; j < meta.paramNames.size() && j < tuples[i].size(); ++j) {
            const std::string& pname = meta.paramNames[j];
            if (pname != "length") {
                row.set(ev::setProperty(row.get(), pname,
                                        ev::fromDouble(static_cast<double>(tuples[i][j]))));
            }
        }
        return row.get();
    });
}

// ---------------------------------------------------------------------------
// #129: bro.optimize(fn, opts) — CEGIS + SMT Equivalence + Live Callable
// ---------------------------------------------------------------------------

Value jsBroOptimize(Value, std::span<const Value> args) {
    if (!hasArg(args, 0)) {
        return ev::throwError("bro.optimize(fn, opts): missing function argument");
    }
    std::string refSrc;
    std::string err;
    if (!extractJsFunctionSource(args[0], refSrc, err)) {
        return ev::throwError(("bro.optimize: " + err).c_str());
    }
    auto origFnRoot = std::make_shared<ev::Persistent>(args[0]);

    bronze::check::OptimizeOptions optOpts;
    std::shared_ptr<ev::Persistent> proposeCbRoot;

    if (hasArg(args, 1) && ev::isObject(args[1])) {
        ev::Persistent optsObj(args[1]);
        Value minV = ev::getProperty(optsObj.get(), "domainMin");
        if (minV.isUndefined()) minV = ev::getProperty(optsObj.get(), "min");
        if (minV.isNumber()) optOpts.domainMin = jsToInt32(minV.asNumber());

        Value maxV = ev::getProperty(optsObj.get(), "domainMax");
        if (maxV.isUndefined()) maxV = ev::getProperty(optsObj.get(), "max");
        if (maxV.isNumber()) optOpts.domainMax = jsToInt32(maxV.asNumber());

        Value roundsV = ev::getProperty(optsObj.get(), "maxRounds");
        if (roundsV.isNumber()) {
            int32_t r = jsToInt32(roundsV.asNumber());
            if (r > 0) optOpts.maxRounds = static_cast<uint32_t>(r);
        }

        Value patchV = ev::getProperty(optsObj.get(), "autoPatchCounterexamples");
        if (patchV.isBool()) optOpts.autoPatchCounterexamples = patchV.asBool();

        Value candsV = ev::getProperty(optsObj.get(), "candidates");
        if (ev::isObject(candsV)) {
            ev::Persistent candsArr(candsV);
            Value lenV = ev::getProperty(candsArr.get(), "length");
            uint32_t len = lenV.isNumber() ? satCast<uint32_t>(lenV.asNumber()) : 0;
            for (uint32_t i = 0; i < len && i < 64; ++i) {
                Value item = ev::getElement(candsArr.get(), i);
                std::string csrc, cerr;
                if (extractJsFunctionSource(item, csrc, cerr)) {
                    optOpts.candidates.push_back(std::move(csrc));
                }
            }
        }

        Value propV = ev::getProperty(optsObj.get(), "proposeCandidate");
        if (ev::isFunction(propV)) {
            proposeCbRoot = std::make_shared<ev::Persistent>(propV);
            optOpts.proposeCandidate = [proposeCbRoot](
                                           const std::string& failedCand,
                                           const bronze::check::CounterexampleInfo& cex) -> std::string {
                ev::Persistent prevStr(ev::fromUtf8(failedCand));
                ObjectBuilder cexBuilder;
                cexBuilder.set("round", ev::fromDouble(cex.round));
                ev::Persistent wArgs(makeInt64JsArray(cex.args));
                cexBuilder.set("args", wArgs.get());
                cexBuilder.set("witnessArgs", wArgs.get());
                cexBuilder.set("expectedOutput", ev::fromDouble(static_cast<double>(cex.expectedOutput)));
                cexBuilder.set("actualOutput", ev::fromDouble(static_cast<double>(cex.actualOutput)));
                cexBuilder.set("candidateOutput", ev::fromDouble(static_cast<double>(cex.actualOutput)));
                Value callArgs[2] = {prevStr.get(), cexBuilder.get()};
                ev::CallResult cr = ev::call(proposeCbRoot->get(), ev::undefined(),
                                             std::span<const Value>(callArgs, 2));
                if (cr.thrown || cr.value.isUndefined() || cr.value.isNull()) return {};
                std::string nextSrc, nextErr;
                if (extractJsFunctionSource(cr.value, nextSrc, nextErr)) return nextSrc;
                return {};
            };
        }
    }

    bronze::check::OptimizeResult optRes = bronze::check::optimizeJsFunction(refSrc, optOpts);
    std::string winningSource = optRes.optimizedSource;

    // Build callable wrapper that executes the verified optimized function via
    // Brass's bitvector evaluator (or delegates to the original JS function).
    ev::Persistent callableFn(ev::makeFunction(
        [winningSource, origFnRoot](Value thisVal, std::span<const Value> callArgs) -> Value {
            std::vector<int64_t> intArgs;
            intArgs.reserve(callArgs.size());
            for (size_t i = 0; i < callArgs.size(); ++i) {
                intArgs.push_back(i32At(callArgs, i));
            }
            int64_t retVal = 0;
            if (bronze::check::evalConcreteJsFunction(winningSource, intArgs, &retVal)) {
                return ev::fromDouble(static_cast<double>(retVal));
            }
            if (ev::isFunction(origFnRoot->get())) {
                ev::CallResult cr = ev::call(origFnRoot->get(), thisVal, callArgs);
                if (cr.thrown) return ev::throwValue(cr.value);
                return cr.value;
            }
            return ev::undefined();
        },
        1, "optimizedFn"));

    callableFn.set(ev::setProperty(callableFn.get(), "verified", ev::fromBool(optRes.verified)));
    callableFn.set(ev::setProperty(callableFn.get(), "provenEquivalent", ev::fromBool(optRes.verified)));
    callableFn.set(ev::setProperty(callableFn.get(), "optimized", ev::fromBool(optRes.optimized)));
    callableFn.set(ev::setProperty(callableFn.get(), "roundsUsed", ev::fromDouble(optRes.roundsUsed)));
    callableFn.set(ev::setProperty(callableFn.get(), "iterations", ev::fromDouble(optRes.roundsUsed)));
    callableFn.set(ev::setProperty(callableFn.get(), "counterexamplesFixed",
                                   ev::fromDouble(static_cast<double>(optRes.counterexampleHistory.size()))));

    {
        ev::Persistent optSrcVal(ev::fromUtf8(optRes.optimizedSource));
        callableFn.set(ev::setProperty(callableFn.get(), "optimizedSource", optSrcVal.get()));
    }
    {
        ev::Persistent refSrcVal(ev::fromUtf8(optRes.referenceSource));
        callableFn.set(ev::setProperty(callableFn.get(), "referenceSource", refSrcVal.get()));
    }
    {
        ev::Persistent sumVal(ev::fromUtf8(optRes.summary));
        callableFn.set(ev::setProperty(callableFn.get(), "summary", sumVal.get()));
    }
    {
        ev::Persistent cexArr(hostArrayOf(optRes.counterexampleHistory.size(), [&](size_t i) -> Value {
            const auto& c = optRes.counterexampleHistory[i];
            ObjectBuilder b;
            b.set("round", ev::fromDouble(c.round));
            ev::Persistent wa(makeInt64JsArray(c.args));
            b.set("args", wa.get());
            b.set("witnessArgs", wa.get());
            b.set("expectedOutput", ev::fromDouble(static_cast<double>(c.expectedOutput)));
            b.set("actualOutput", ev::fromDouble(static_cast<double>(c.actualOutput)));
            b.set("candidateOutput", ev::fromDouble(static_cast<double>(c.actualOutput)));
            return b.get();
        }));
        callableFn.set(ev::setProperty(callableFn.get(), "counterexamples", cexArr.get()));
    }
    callableFn.set(ev::setProperty(callableFn.get(), "evalNative", callableFn.get()));
    callableFn.set(ev::setProperty(callableFn.get(), "fn", callableFn.get()));
    {
        ObjectBuilder certObj;
        certObj.set("verified", ev::fromBool(optRes.verified));
        certObj.set("provenEquivalent", ev::fromBool(optRes.verified));
        certObj.set("optimized", ev::fromBool(optRes.optimized));
        certObj.set("roundsUsed", ev::fromDouble(optRes.roundsUsed));
        ev::Persistent optSrcVal(ev::fromUtf8(optRes.optimizedSource));
        certObj.set("optimizedSource", optSrcVal.get());
        ev::Persistent sumVal(ev::fromUtf8(optRes.summary));
        certObj.set("summary", sumVal.get());
        callableFn.set(ev::setProperty(callableFn.get(), "certificate", certObj.get()));
    }

    return callableFn.get();
}

// ---------------------------------------------------------------------------
// #133: bro.fuzz(fn, opts) — White-Box Concolic Bug Hunter & Path Generator
// ---------------------------------------------------------------------------

Value jsBroFuzz(Value, std::span<const Value> args) {
    if (!hasArg(args, 0)) {
        return ev::throwError("bro.fuzz(fn, opts): missing function argument");
    }
    std::string fnSrc;
    std::string err;
    if (!extractJsFunctionSource(args[0], fnSrc, err)) {
        return ev::throwError(("bro.fuzz: " + err).c_str());
    }

    bronze::check::FuzzOptions opts;
    if (hasArg(args, 1) && ev::isObject(args[1])) {
        ev::Persistent optsObj(args[1]);
        Value maxP = ev::getProperty(optsObj.get(), "maxPaths");
        if (maxP.isNumber() && maxP.asNumber() > 0) {
            opts.maxPaths = satCast<uint32_t>(maxP.asNumber());
        }
    }

    bronze::check::FuzzAuditReport rep = bronze::check::fuzzJsFunction(fnSrc, opts);
    if (!rep.error.empty()) {
        return ev::throwError(("bro.fuzz: " + rep.error).c_str());
    }

    ObjectBuilder out;
    {
        ev::Persistent fnNameVal(ev::fromUtf8(rep.functionName));
        out.set("functionName", fnNameVal.get());
    }
    out.set("branchCoverage", ev::fromDouble(rep.branchCoverage));
    out.set("totalBranches", ev::fromDouble(rep.totalBranches));
    out.set("coveredBranches", ev::fromDouble(rep.coveredBranches));
    out.set("feasiblePaths", ev::fromDouble(static_cast<double>(rep.testVectors.size())));
    out.set("safe", ev::fromBool(rep.safe));

    {
        ev::Persistent tvArr(hostArrayOf(rep.testVectors.size(), [&](size_t i) -> Value {
            const auto& tv = rep.testVectors[i];
            ObjectBuilder b;
            ev::Persistent inArr(makeInt64JsArray(tv.args));
            b.set("args", inArr.get());
            b.set("input", inArr.get());
            b.set("output", ev::fromDouble(static_cast<double>(tv.returnValue)));
            b.set("returnValue", ev::fromDouble(static_cast<double>(tv.returnValue)));
            b.set("targetBlockId", ev::fromDouble(tv.targetBlockId));
            ev::Persistent pathStr(ev::fromUtf8(tv.pathDescription));
            b.set("branchPath", pathStr.get());
            b.set("path", pathStr.get());
            return b.get();
        }));
        out.set("testVectors", tvArr.get());
        out.set("tests", tvArr.get());
    }
    {
        ev::Persistent hzArr(hostArrayOf(rep.hazards.size(), [&](size_t i) -> Value {
            const auto& h = rep.hazards[i];
            ObjectBuilder b;
            ev::Persistent kindStr(ev::fromUtf8(h.kind));
            b.set("kind", kindStr.get());
            ev::Persistent descStr(ev::fromUtf8(h.description));
            b.set("message", descStr.get());
            b.set("description", descStr.get());
            b.set("line", ev::fromDouble(h.line));
            b.set("col", ev::fromDouble(h.col));
            ev::Persistent wArr(makeInt64JsArray(h.witnessArgs));
            b.set("witnessArgs", wArr.get());
            b.set("witness", wArr.get());
            return b.get();
        }));
        out.set("hazards", hzArr.get());
        out.set("bugs", hzArr.get());
    }
    {
        ev::Persistent jsonStr(ev::fromUtf8(rep.renderJson()));
        out.set("json", jsonStr.get());
    }
    return out.get();
}

// ---------------------------------------------------------------------------
// #135: bro.prove(fn, opts) — Unified Runtime Proof Certificate API
// ---------------------------------------------------------------------------

Value jsBroProve(Value, std::span<const Value> args) {
    if (!hasArg(args, 0)) {
        return ev::throwError("bro.prove(fn, opts): missing function argument");
    }
    std::string fnSrc;
    std::string err;
    if (!extractJsFunctionSource(args[0], fnSrc, err)) {
        return ev::throwError(("bro.prove: " + err).c_str());
    }

    bronze::check::ProveOptions opts;
    if (hasArg(args, 1) && ev::isObject(args[1])) {
        ev::Persistent optsObj(args[1]);
        Value minV = ev::getProperty(optsObj.get(), "domainMin");
        if (minV.isUndefined()) minV = ev::getProperty(optsObj.get(), "min");
        if (minV.isNumber()) opts.domainMin = jsToInt32(minV.asNumber());

        Value maxV = ev::getProperty(optsObj.get(), "domainMax");
        if (maxV.isUndefined()) maxV = ev::getProperty(optsObj.get(), "max");
        if (maxV.isNumber()) opts.domainMax = jsToInt32(maxV.asNumber());

        Value eqV = ev::getProperty(optsObj.get(), "equivTo");
        if (eqV.isUndefined()) eqV = ev::getProperty(optsObj.get(), "equivalentTo");
        if (!eqV.isUndefined() && !eqV.isNull()) {
            std::string eqSrc, eqErr;
            if (extractJsFunctionSource(eqV, eqSrc, eqErr)) {
                size_t fnPos = eqSrc.find("function");
                if (fnPos != std::string::npos) {
                    size_t parenPos = eqSrc.find('(', fnPos + 8);
                    if (parenPos != std::string::npos) {
                        eqSrc = eqSrc.substr(0, fnPos) + "function " + eqSrc.substr(parenPos);
                    }
                }
                opts.equivalentToSource = std::move(eqSrc);
            }
        }

        Value rtV = ev::getProperty(optsObj.get(), "roundTripWith");
        if (rtV.isUndefined()) rtV = ev::getProperty(optsObj.get(), "inverseFn");
        if (!rtV.isUndefined() && !rtV.isNull()) {
            std::string rtSrc, rtErr;
            if (extractJsFunctionSource(rtV, rtSrc, rtErr)) {
                opts.inverseFnSource = std::move(rtSrc);
            }
        }

        Value synthV = ev::getProperty(optsObj.get(), "synthesizeTests");
        if (synthV.isBool()) opts.synthesizeTests = synthV.asBool();
    }

    bronze::check::ProofCertificateReport rep = bronze::check::proveJsFunction(fnSrc, opts);
    if (!rep.error.empty()) {
        return ev::throwError(("bro.prove: " + rep.error).c_str());
    }

    ObjectBuilder out;
    {
        ev::Persistent statusStr(ev::fromUtf8(rep.verified ? "VERIFIED" : "UNVERIFIED"));
        out.set("status", statusStr.get());
    }
    out.set("verified", ev::fromBool(rep.verified));
    out.set("safe", ev::fromBool(rep.hazardFree));
    out.set("hazardFree", ev::fromBool(rep.hazardFree));
    out.set("deterministic", ev::fromBool(rep.deterministic));
    out.set("proofsValid", ev::fromBool(rep.proofsValid));
    out.set("provenGuards", ev::fromDouble(rep.provenGuards));
    out.set("totalGuards", ev::fromDouble(rep.totalGuards));
    out.set("unboxedF64Ops", ev::fromDouble(rep.unboxedF64Ops));
    out.set("equivalent", ev::fromBool(rep.equivalent));
    out.set("roundTripProven", ev::fromBool(rep.roundTripProven));

    if (!rep.counterexampleArgs.empty()) {
        ObjectBuilder cex;
        ev::Persistent cArgs(makeInt64JsArray(rep.counterexampleArgs));
        cex.set("args", cArgs.get());
        cex.set("witnessArgs", cArgs.get());
        cex.set("expectedOutput", ev::fromDouble(static_cast<double>(rep.expectedOutput)));
        cex.set("actualOutput", ev::fromDouble(static_cast<double>(rep.actualOutput)));
        out.set("counterexample", cex.get());
    } else {
        out.set("counterexample", ev::null());
    }

    {
        ev::Persistent hzArr(hostArrayOf(rep.hazards.size(), [&](size_t i) -> Value {
            return ev::fromUtf8(rep.hazards[i]);
        }));
        out.set("hazards", hzArr.get());
    }
    {
        ev::Persistent lhArr(hostArrayOf(rep.lockstepHazards.size(), [&](size_t i) -> Value {
            return ev::fromUtf8(rep.lockstepHazards[i]);
        }));
        out.set("lockstepHazards", lhArr.get());
    }
    {
        ev::Persistent stArr(hostArrayOf(rep.synthesizedTests.size(), [&](size_t i) -> Value {
            const auto& tc = rep.synthesizedTests[i];
            ObjectBuilder b;
            ev::Persistent aArr(makeInt64JsArray(tc.args));
            b.set("args", aArr.get());
            b.set("expectedReturn", ev::fromDouble(static_cast<double>(tc.expectedReturn)));
            ev::Persistent kindStr(ev::fromUtf8(bronze::check::synthTestKindTag(tc.kind)));
            b.set("kind", kindStr.get());
            ev::Persistent pathStr(ev::fromUtf8(tc.pathDescription));
            b.set("pathDescription", pathStr.get());
            return b.get();
        }));
        out.set("synthesizedTests", stArr.get());
    }
    {
        ev::Persistent shaVal(ev::fromUtf8(rep.sha256));
        out.set("sha256", shaVal.get());
        out.set("proofHash", shaVal.get());
    }
    {
        ev::Persistent certVal(ev::fromUtf8(rep.certificateJson));
        out.set("certificateJson", certVal.get());
    }
    return out.get();
}

}  // namespace

void attachBroSolveApis(Value broObj) {
    if (!ev::isObject(broObj)) return;
    ObjectBuilder bro(broObj);
    bro.def("solve", 2, jsBroSolve);
    bro.def("solveWithCert", 2, jsBroSolveWithCert);
    bro.def("solveAll", 3, jsBroSolveAll);
    bro.def("optimize", 2, jsBroOptimize);
    bro.def("fuzz", 2, jsBroFuzz);
    bro.def("prove", 2, jsBroProve);

    attachBroLayoutApis(bro.get());
    attachBroRewriteApis(bro.get());
}

}  // namespace bro::bronze_host
