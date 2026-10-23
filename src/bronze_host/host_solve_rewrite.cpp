#include "bronze_host/host_solve.h"

#include <cmath>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include "bronze_host/host_builder.h"
#include "bronze_host/host_js_gpu.h"
#include "bronze_host/host_values.h"
#include "check/auto_kernel_rewriter.h"
#include "check/cegis_optimize.h"
#include "check/cegis_superopt.h"
#include "check/js_source_rewriter.h"
#include "check/live_supervisor.h"
#include "check/solve_fn.h"

namespace bro::bronze_host {
namespace {

int32_t jsNumToInt32(double d) {
    if (!std::isfinite(d)) return 0;
    if (d <= static_cast<double>(INT32_MIN)) return INT32_MIN;
    if (d >= static_cast<double>(INT32_MAX)) return INT32_MAX;
    return static_cast<int32_t>(std::trunc(d));
}

bool extractFnOrString(Value val, std::string& outSrc, std::string& outErr) {
    if (val.isString()) {
        outSrc = ev::toUtf8(val);
        return !outSrc.empty();
    }
    if (!ev::isFunction(val) && !ev::isObject(val)) {
        outErr = "expected a JavaScript function or source string";
        return false;
    }
    ev::Persistent fnObj(val);
    Value toStringFn = ev::getProperty(fnObj.get(), "toString");
    if (!ev::isFunction(toStringFn)) {
        outErr = "target does not provide Function.prototype.toString()";
        return false;
    }
    ev::Persistent toStrRoot(toStringFn);
    auto cr = ev::call(toStrRoot.get(), fnObj.get(), {});
    if (cr.thrown || !cr.value.isString()) {
        outErr = "failed to extract function source via toString()";
        return false;
    }
    outSrc = ev::toUtf8(cr.value);
    if (outSrc.find("[native code]") != std::string::npos) {
        outErr = "cannot rewrite native C++ function without JS source";
        return false;
    }
    return true;
}

Value makeInt64ArrayVal(const std::vector<int64_t>& vals) {
    return hostArrayOf(vals.size(), [&](size_t i) -> Value {
        return ev::fromDouble(static_cast<double>(vals[i]));
    });
}

Value makeCallableFromVerifiedSource(const std::string& verifiedSource,
                                     std::shared_ptr<ev::Persistent> fallbackFn = nullptr) {
    return ev::makeFunction(
        [verifiedSource, fallbackFn](Value thisVal, std::span<const Value> callArgs) -> Value {
            std::vector<int64_t> intArgs;
            intArgs.reserve(callArgs.size());
            for (size_t i = 0; i < callArgs.size(); ++i) {
                intArgs.push_back(i32At(callArgs, i));
            }
            int64_t retVal = 0;
            if (bronze::check::evalConcreteJsFunction(verifiedSource, intArgs, &retVal)) {
                return ev::fromDouble(static_cast<double>(retVal));
            }
            if (fallbackFn && ev::isFunction(fallbackFn->get())) {
                ev::CallResult cr = ev::call(fallbackFn->get(), thisVal, callArgs);
                if (cr.thrown) return ev::throwValue(cr.value);
                return cr.value;
            }
            return ev::fromDouble(0.0);
        },
        1, "verifiedFn");
}

Value buildSuperoptStatsJsObject(const bronze::check::CegisSuperoptStats& st) {
    ObjectBuilder obj;
    obj.set("candidatesGenerated", ev::fromDouble(static_cast<double>(st.candidatesGenerated)));
    obj.set("candidatesTested", ev::fromDouble(static_cast<double>(st.candidatesTested)));
    obj.set("concretePruned", ev::fromDouble(static_cast<double>(st.concretePruned)));
    obj.set("smtQueries", ev::fromDouble(static_cast<double>(st.smtQueries)));
    obj.set("counterexamplesFound", ev::fromDouble(static_cast<double>(st.counterexamplesFound)));
    obj.set("seedVectorCount", ev::fromDouble(static_cast<double>(st.seedVectorCount)));
    obj.set("finalVectorCount", ev::fromDouble(static_cast<double>(st.finalVectorCount)));
    obj.set("costBefore", ev::fromDouble(static_cast<double>(st.costBefore)));
    obj.set("costAfter", ev::fromDouble(static_cast<double>(st.costAfter)));
    obj.set("elapsedMs", ev::fromDouble(st.elapsedMs));
    return obj.get();
}

// #142: bro.superoptimize(fnOrSource, opts)
Value jsBroSuperoptimize(Value, std::span<const Value> args) {
    if (!hasArg(args, 0)) {
        return ev::throwError("bro.superoptimize(fn, opts): missing function or source string");
    }
    std::string src, err;
    if (!extractFnOrString(args[0], src, err)) {
        return ev::throwError(("bro.superoptimize: " + err).c_str());
    }

    bronze::check::CegisSuperoptOptions opts;
    if (hasArg(args, 1) && ev::isObject(args[1])) {
        ev::Persistent oRoot(args[1]);
        Value dMinV = ev::getProperty(oRoot.get(), "domainMin");
        if (dMinV.isNumber()) opts.domainMin = jsNumToInt32(dMinV.asNumber());
        Value dMaxV = ev::getProperty(oRoot.get(), "domainMax");
        if (dMaxV.isNumber()) opts.domainMax = jsNumToInt32(dMaxV.asNumber());
        Value maxC = ev::getProperty(oRoot.get(), "maxCandidates");
        if (maxC.isNumber() && maxC.asNumber() > 0) {
            opts.maxCandidates = satCast<uint32_t>(maxC.asNumber());
        }
    }

    auto res = bronze::check::superoptimizeJsFunction(src, opts);

    ObjectBuilder out;
    out.set("verified", ev::fromBool(res.verified));
    out.set("optimized", ev::fromBool(res.optimized));
    ev::Persistent fnStr(ev::fromUtf8(res.functionName));
    out.set("functionName", fnStr.get());
    ev::Persistent origStr(ev::fromUtf8(res.originalSource));
    out.set("originalSource", origStr.get());
    ev::Persistent exprStr(ev::fromUtf8(res.synthesizedExpression));
    out.set("synthesizedExpression", exprStr.get());
    ev::Persistent optStr(ev::fromUtf8(res.optimizedSource));
    out.set("optimizedSource", optStr.get());
    ev::Persistent ruleStr(ev::fromUtf8(res.ruleDescription));
    out.set("ruleDescription", ruleStr.get());
    ev::Persistent certStr(ev::fromUtf8(res.certificateHash));
    out.set("certificateHash", certStr.get());
    ev::Persistent sumStr(ev::fromUtf8(res.summary));
    out.set("summary", sumStr.get());
    out.set("stats", buildSuperoptStatsJsObject(res.stats));
    return out.get();
}

// #143: bro.rewriteSource(fnOrSource, opts)
Value jsBroRewriteSource(Value, std::span<const Value> args) {
    if (!hasArg(args, 0)) {
        return ev::throwError("bro.rewriteSource(source, opts): missing source or function");
    }
    std::string src, err;
    if (!extractFnOrString(args[0], src, err)) {
        return ev::throwError(("bro.rewriteSource: " + err).c_str());
    }

    bronze::check::JsRewriteOptions opts;
    if (hasArg(args, 1) && ev::isObject(args[1])) {
        ev::Persistent oRoot(args[1]);
        Value dMinV = ev::getProperty(oRoot.get(), "domainMin");
        if (dMinV.isNumber()) opts.domainMin = jsNumToInt32(dMinV.asNumber());
        Value dMaxV = ev::getProperty(oRoot.get(), "domainMax");
        if (dMaxV.isNumber()) opts.domainMax = jsNumToInt32(dMaxV.asNumber());
        Value reqProof = ev::getProperty(oRoot.get(), "requireFormalProof");
        if (reqProof.isBool()) opts.requireFormalProof = reqProof.asBool();
    }

    auto rep = bronze::check::rewriteJsSource(src, opts);

    ObjectBuilder out;
    ev::Persistent origStr(ev::fromUtf8(rep.originalSource));
    out.set("originalSource", origStr.get());
    ev::Persistent rewStr(ev::fromUtf8(rep.rewrittenSource));
    out.set("rewrittenSource", rewStr.get());
    ev::Persistent diffStr(ev::fromUtf8(rep.unifiedDiff));
    out.set("unifiedDiff", diffStr.get());
    out.set("bytesBefore", ev::fromDouble(static_cast<double>(rep.bytesBefore)));
    out.set("bytesAfter", ev::fromDouble(static_cast<double>(rep.bytesAfter)));
    out.set("estimatedOpsBefore", ev::fromDouble(static_cast<double>(rep.estimatedOpsBefore)));
    out.set("estimatedOpsAfter", ev::fromDouble(static_cast<double>(rep.estimatedOpsAfter)));
    out.set("allProvenEquivalent", ev::fromBool(rep.allProvenEquivalent));
    ev::Persistent certStr(ev::fromUtf8(rep.certificateHash));
    out.set("certificateHash", certStr.get());
    ev::Persistent sumStr(ev::fromUtf8(rep.summary));
    out.set("summary", sumStr.get());
    out.set("cegisStats", buildSuperoptStatsJsObject(rep.cegisStats));

    Value rewritesArr = hostArrayOf(rep.rewrites.size(), [&](size_t i) -> Value {
        const auto& h = rep.rewrites[i];
        ObjectBuilder item;
        ev::Persistent fnV(ev::fromUtf8(h.functionName));
        item.set("functionName", fnV.get());
        ev::Persistent rnV(ev::fromUtf8(h.ruleName));
        item.set("ruleName", rnV.get());
        ev::Persistent descV(ev::fromUtf8(h.description));
        item.set("description", descV.get());
        ev::Persistent befV(ev::fromUtf8(h.beforeSnippet));
        item.set("beforeSnippet", befV.get());
        ev::Persistent aftV(ev::fromUtf8(h.afterSnippet));
        item.set("afterSnippet", aftV.get());
        item.set("provenEquivalent", ev::fromBool(h.provenEquivalent));
        ev::Persistent chV(ev::fromUtf8(h.certificateHash));
        item.set("certificateHash", chV.get());
        return item.get();
    });
    out.set("rewrites", rewritesArr);
    return out.get();
}

// #144: bro.autoKernel(fnOrSource, opts)
Value jsBroAutoKernel(Value, std::span<const Value> args) {
    if (!hasArg(args, 0)) {
        return ev::throwError("bro.autoKernel(fnOrSource, opts): missing function or source");
    }
    std::string src, err;
    if (!extractFnOrString(args[0], src, err)) {
        return ev::throwError(("bro.autoKernel: " + err).c_str());
    }

    auto rep = bronze::check::analyzeAndRewriteLoopToKernel(src);

    ObjectBuilder out;
    out.set("foundLoop", ev::fromBool(rep.foundLoop));
    out.set("parallelizable", ev::fromBool(rep.parallelizable));
    out.set("provenNoWaw", ev::fromBool(rep.provenNoWaw));
    out.set("provenNoRaw", ev::fromBool(rep.provenNoRaw));
    out.set("provenNoWar", ev::fromBool(rep.provenNoWar));
    out.set("provenInBounds", ev::fromBool(rep.provenInBounds));
    ev::Persistent fnStr(ev::fromUtf8(rep.functionName));
    out.set("functionName", fnStr.get());
    ev::Persistent ivStr(ev::fromUtf8(rep.inductionVar));
    out.set("inductionVar", ivStr.get());
    ev::Persistent bndStr(ev::fromUtf8(rep.boundExpr));
    out.set("boundExpr", bndStr.get());
    ev::Persistent origStr(ev::fromUtf8(rep.originalSource));
    out.set("originalSource", origStr.get());
    ev::Persistent kernStr(ev::fromUtf8(rep.kernelSource));
    out.set("kernelSource", kernStr.get());
    ev::Persistent diffStr(ev::fromUtf8(rep.unifiedDiff));
    out.set("unifiedDiff", diffStr.get());
    ev::Persistent certStr(ev::fromUtf8(rep.certificateHash));
    out.set("certificateHash", certStr.get());
    ev::Persistent sumStr(ev::fromUtf8(rep.summary));
    out.set("summary", sumStr.get());

    Value hazardsArr = hostArrayOf(rep.hazards.size(), [&](size_t i) -> Value {
        const auto& h = rep.hazards[i];
        ObjectBuilder item;
        ev::Persistent kV(ev::fromUtf8(h.hazardKind));
        item.set("hazardKind", kV.get());
        ev::Persistent aV(ev::fromUtf8(h.arrayName));
        item.set("arrayName", aV.get());
        ev::Persistent wV(ev::fromUtf8(h.writeIndexExpr));
        item.set("writeIndexExpr", wV.get());
        ev::Persistent rV(ev::fromUtf8(h.readIndexExpr));
        item.set("readIndexExpr", rV.get());
        item.set("iterA", ev::fromDouble(static_cast<double>(h.iterA)));
        item.set("iterB", ev::fromDouble(static_cast<double>(h.iterB)));
        item.set("conflictIndex", ev::fromDouble(static_cast<double>(h.conflictIndex)));
        ev::Persistent dV(ev::fromUtf8(h.description));
        item.set("description", dV.get());
        return item.get();
    });
    out.set("hazards", hazardsArr);

    if (rep.parallelizable && !rep.kernelSource.empty()) {
        ev::Persistent kSrcVal(ev::fromUtf8(rep.kernelSource));
        Value kArgs[1] = {kSrcVal.get()};
        Value compiled = compileJsKernelValue(kArgs, false);
        if (ev::isObject(compiled)) {
            out.set("kernel", compiled);
        }
    } else {
        out.set("kernel", ev::null());
    }

    return out.get();
}

// ---------------------------------------------------------------------------
// #148: bro.live.healAndVerify & bro.live.createSupervisor
// ---------------------------------------------------------------------------

bronze::check::LiveSupervisorOptions parseLiveSupervisorOptions(Value v) {
    bronze::check::LiveSupervisorOptions opts;
    if (!ev::isObject(v)) return opts;
    ev::Persistent oRoot(v);
    Value dMinV = ev::getProperty(oRoot.get(), "domainMin");
    if (dMinV.isNumber()) opts.domainMin = jsNumToInt32(dMinV.asNumber());
    Value dMaxV = ev::getProperty(oRoot.get(), "domainMax");
    if (dMaxV.isNumber()) opts.domainMax = jsNumToInt32(dMaxV.asNumber());
    Value autoH = ev::getProperty(oRoot.get(), "autoHeal");
    if (autoH.isBool()) opts.autoHeal = autoH.asBool();
    Value chkEq = ev::getProperty(oRoot.get(), "checkEquivalenceWithPrevious");
    if (chkEq.isBool()) opts.checkEquivalenceWithPrevious = chkEq.asBool();
    return opts;
}

Value buildLiveReloadReportJsObject(const bronze::check::LiveReloadReport& rep,
                                    std::shared_ptr<ev::Persistent> fallbackFn = nullptr) {
    ObjectBuilder out;
    out.set("accepted", ev::fromBool(rep.accepted));
    out.set("selfHealed", ev::fromBool(rep.selfHealed));
    out.set("originalHazardCount", ev::fromDouble(static_cast<double>(rep.originalHazardCount)));
    out.set("remainingHazardCount", ev::fromDouble(static_cast<double>(rep.remainingHazardCount)));
    ev::Persistent fnNameV(ev::fromUtf8(rep.functionName));
    out.set("functionName", fnNameV.get());
    ev::Persistent candV(ev::fromUtf8(rep.candidateSource));
    out.set("candidateSource", candV.get());
    ev::Persistent healedV(ev::fromUtf8(rep.healedSource));
    out.set("healedSource", healedV.get());
    ev::Persistent activeV(ev::fromUtf8(rep.activeSource));
    out.set("activeSource", activeV.get());
    out.set("hasPrevious", ev::fromBool(rep.hasPrevious));
    out.set("behaviorallyIdentical", ev::fromBool(rep.behaviorallyIdentical));
    ev::Persistent certV(ev::fromUtf8(rep.certificateHash));
    out.set("certificateHash", certV.get());
    ev::Persistent sumV(ev::fromUtf8(rep.summary));
    out.set("summary", sumV.get());

    if (rep.divergenceWitness.hasDivergence) {
        ObjectBuilder dw;
        ev::Persistent wArgs(makeInt64ArrayVal(rep.divergenceWitness.args));
        dw.set("args", wArgs.get());
        dw.set("oldOutput", ev::fromDouble(static_cast<double>(rep.divergenceWitness.oldOutput)));
        dw.set("newOutput", ev::fromDouble(static_cast<double>(rep.divergenceWitness.newOutput)));
        out.set("divergenceWitness", dw.get());
    } else {
        out.set("divergenceWitness", ev::null());
    }

    ev::Persistent origHzArr(hostArrayOf(rep.originalHazards.size(), [&](size_t i) -> Value {
        return ev::fromUtf8(rep.originalHazards[i]);
    }));
    out.set("hazards", origHzArr.get());
    out.set("originalHazards", origHzArr.get());

    ev::Persistent remHzArr(hostArrayOf(rep.remainingHazards.size(), [&](size_t i) -> Value {
        return ev::fromUtf8(rep.remainingHazards[i]);
    }));
    out.set("remainingHazards", remHzArr.get());

    ev::Persistent repArr(hostArrayOf(rep.repairs.size(), [&](size_t i) -> Value {
        const auto& r = rep.repairs[i];
        ObjectBuilder b;
        ev::Persistent kV(ev::fromUtf8(r.hazardKind));
        b.set("hazardKind", kV.get());
        ev::Persistent befV(ev::fromUtf8(r.beforeSnippet));
        b.set("beforeSnippet", befV.get());
        ev::Persistent aftV(ev::fromUtf8(r.afterSnippet));
        b.set("afterSnippet", aftV.get());
        ev::Persistent dV(ev::fromUtf8(r.description));
        b.set("description", dV.get());
        return b.get();
    }));
    out.set("repairs", repArr.get());

    ev::Persistent fnVal(makeCallableFromVerifiedSource(rep.activeSource, fallbackFn));
    out.set("fn", fnVal.get());
    return out.get();
}

Value jsBroLiveHealAndVerify(Value, std::span<const Value> args) {
    if (!hasArg(args, 0)) {
        return ev::throwError("bro.live.healAndVerify(fnOrSource, prevFnOrSource, opts): missing candidate");
    }
    std::string candSrc, err;
    if (!extractFnOrString(args[0], candSrc, err)) {
        return ev::throwError(("bro.live.healAndVerify: " + err).c_str());
    }
    auto fallbackFn = std::make_shared<ev::Persistent>(args[0]);

    std::string prevSrc;
    bronze::check::LiveSupervisorOptions opts;
    if (hasArg(args, 1)) {
        if (args[1].isString() || ev::isFunction(args[1])) {
            std::string prevErr;
            extractFnOrString(args[1], prevSrc, prevErr);
            if (hasArg(args, 2)) {
                opts = parseLiveSupervisorOptions(args[2]);
            }
        } else if (ev::isObject(args[1])) {
            opts = parseLiveSupervisorOptions(args[1]);
            ev::Persistent oRoot(args[1]);
            Value prevV = ev::getProperty(oRoot.get(), "previous");
            if (prevV.isUndefined()) prevV = ev::getProperty(oRoot.get(), "previousSource");
            if (!prevV.isUndefined() && !prevV.isNull()) {
                std::string prevErr;
                extractFnOrString(prevV, prevSrc, prevErr);
            }
        }
    }

    auto rep = bronze::check::superviseLiveReload(prevSrc, candSrc, opts);
    return buildLiveReloadReportJsObject(rep, fallbackFn);
}

struct LiveSupervisorState {
    bronze::check::LiveSupervisorOptions opts;
    std::string activeSource;
    std::string certificateHash;
    uint32_t selfHealCount = 0;
    uint32_t reloadCount = 0;
    std::vector<bronze::check::LiveReloadReport> history;
};

Value jsBroLiveCreateSupervisor(Value, std::span<const Value> args) {
    auto st = std::make_shared<LiveSupervisorState>();
    std::string initSrc;
    if (hasArg(args, 0)) {
        if (args[0].isString() || ev::isFunction(args[0])) {
            std::string err;
            extractFnOrString(args[0], initSrc, err);
            if (hasArg(args, 1)) st->opts = parseLiveSupervisorOptions(args[1]);
        } else if (ev::isObject(args[0])) {
            st->opts = parseLiveSupervisorOptions(args[0]);
        }
    }

    auto syncSupervisorProps = [st](Value supVal) {
        if (!ev::isObject(supVal)) return;
        ev::Persistent supObj(supVal);
        ev::Persistent actStr(ev::fromUtf8(st->activeSource));
        supObj.set(ev::setProperty(supObj.get(), "activeSource", actStr.get()));
        ev::Persistent certStr(ev::fromUtf8(st->certificateHash));
        supObj.set(ev::setProperty(supObj.get(), "certificateHash", certStr.get()));
        supObj.set(ev::setProperty(supObj.get(), "selfHealCount",
                                   ev::fromDouble(static_cast<double>(st->selfHealCount))));
        supObj.set(ev::setProperty(supObj.get(), "reloadCount",
                                   ev::fromDouble(static_cast<double>(st->reloadCount))));
        ev::Persistent histArr(hostArrayOf(st->history.size(), [&](size_t i) -> Value {
            return buildLiveReloadReportJsObject(st->history[i]);
        }));
        supObj.set(ev::setProperty(supObj.get(), "history", histArr.get()));
    };

    if (!initSrc.empty()) {
        auto rep = bronze::check::superviseLiveReload("", initSrc, st->opts);
        st->activeSource = rep.activeSource;
        st->certificateHash = rep.certificateHash;
        if (rep.selfHealed) ++st->selfHealCount;
        ++st->reloadCount;
        st->history.push_back(rep);
    }

    ObjectBuilder sup;
    sup.def("reload", 2, [st, syncSupervisorProps](Value self, std::span<const Value> rArgs) -> Value {
        if (!hasArg(rArgs, 0)) {
            return ev::throwError("supervisor.reload(newFnOrSource): missing candidate");
        }
        std::string candSrc, err;
        if (!extractFnOrString(rArgs[0], candSrc, err)) {
            return ev::throwError(("supervisor.reload: " + err).c_str());
        }
        ev::Persistent selfRoot(self);
        auto opts = hasArg(rArgs, 1) ? parseLiveSupervisorOptions(rArgs[1]) : st->opts;
        auto rep = bronze::check::superviseLiveReload(st->activeSource, candSrc, opts);
        if (rep.accepted) {
            st->activeSource = rep.activeSource;
            st->certificateHash = rep.certificateHash;
        }
        if (rep.selfHealed) ++st->selfHealCount;
        ++st->reloadCount;
        st->history.push_back(rep);
        syncSupervisorProps(selfRoot.get());
        return buildLiveReloadReportJsObject(rep);
    });

    auto callImpl = [st](Value, std::span<const Value> cArgs) -> Value {
        if (st->activeSource.empty()) return ev::undefined();
        std::vector<int64_t> intArgs;
        intArgs.reserve(cArgs.size());
        for (size_t i = 0; i < cArgs.size(); ++i) {
            intArgs.push_back(i32At(cArgs, i));
        }
        int64_t out = 0;
        if (bronze::check::evalConcreteJsFunction(st->activeSource, intArgs, &out)) {
            return ev::fromDouble(static_cast<double>(out));
        }
        return ev::fromDouble(0.0);
    };
    sup.def("call", 4, callImpl);
    sup.def("eval", 4, callImpl);

    ev::Persistent supRoot(sup.get());
    syncSupervisorProps(supRoot.get());
    return supRoot.get();
}

// ---------------------------------------------------------------------------
// #149: bro.evolve & bro.daemon.createEvolutionEngine
// ---------------------------------------------------------------------------

bronze::check::EvolutionOptions parseEvolutionOptions(Value v) {
    bronze::check::EvolutionOptions opts;
    if (!ev::isObject(v)) return opts;
    ev::Persistent oRoot(v);
    Value dMinV = ev::getProperty(oRoot.get(), "domainMin");
    if (dMinV.isNumber()) opts.domainMin = jsNumToInt32(dMinV.asNumber());
    Value dMaxV = ev::getProperty(oRoot.get(), "domainMax");
    if (dMaxV.isNumber()) opts.domainMax = jsNumToInt32(dMaxV.asNumber());
    Value itersV = ev::getProperty(oRoot.get(), "benchmarkIterations");
    if (itersV.isNumber() && itersV.asNumber() > 0) {
        opts.benchmarkIterations = satCast<uint32_t>(itersV.asNumber());
    }
    return opts;
}

Value buildEvolutionGenerationsArray(const std::vector<bronze::check::EvolutionGeneration>& gens) {
    return hostArrayOf(gens.size(), [&](size_t i) -> Value {
        const auto& g = gens[i];
        ObjectBuilder b;
        b.set("generation", ev::fromDouble(static_cast<double>(g.generation)));
        ev::Persistent tV(ev::fromUtf8(g.tier));
        b.set("tier", tV.get());
        ev::Persistent sV(ev::fromUtf8(g.source));
        b.set("source", sV.get());
        b.set("bytes", ev::fromDouble(static_cast<double>(g.bytes)));
        b.set("ops", ev::fromDouble(static_cast<double>(g.ops)));
        b.set("nsPerCall", ev::fromDouble(g.nsPerCall));
        b.set("speedup", ev::fromDouble(g.speedup));
        b.set("provenEquivalent", ev::fromBool(g.provenEquivalent));
        ev::Persistent cV(ev::fromUtf8(g.certificateHash));
        b.set("certificateHash", cV.get());
        ev::Persistent dV(ev::fromUtf8(g.description));
        b.set("description", dV.get());
        return b.get();
    });
}

Value buildEvolvedFunctionFromReport(const bronze::check::EvolutionReport& rep,
                                     std::shared_ptr<ev::Persistent> fallbackFn = nullptr) {
    ev::Persistent fnObj(makeCallableFromVerifiedSource(rep.optimizedSource, fallbackFn));

    ev::Persistent fnNameV(ev::fromUtf8(rep.functionName));
    fnObj.set(ev::setProperty(fnObj.get(), "functionName", fnNameV.get()));
    ev::Persistent origV(ev::fromUtf8(rep.originalSource));
    fnObj.set(ev::setProperty(fnObj.get(), "originalSource", origV.get()));
    ev::Persistent optV(ev::fromUtf8(rep.optimizedSource));
    fnObj.set(ev::setProperty(fnObj.get(), "optimizedSource", optV.get()));
    ev::Persistent tierV(ev::fromUtf8(rep.bestTier));
    fnObj.set(ev::setProperty(fnObj.get(), "bestTier", tierV.get()));
    fnObj.set(ev::setProperty(fnObj.get(), "bestGeneration",
                              ev::fromDouble(static_cast<double>(rep.bestGeneration))));
    fnObj.set(ev::setProperty(fnObj.get(), "baselineNsPerCall", ev::fromDouble(rep.baselineNsPerCall)));
    fnObj.set(ev::setProperty(fnObj.get(), "bestNsPerCall", ev::fromDouble(rep.bestNsPerCall)));
    fnObj.set(ev::setProperty(fnObj.get(), "speedup", ev::fromDouble(rep.speedup)));
    fnObj.set(ev::setProperty(fnObj.get(), "sizeReductionRatio", ev::fromDouble(rep.sizeReductionRatio)));
    fnObj.set(ev::setProperty(fnObj.get(), "provenEquivalent", ev::fromBool(rep.provenEquivalent)));
    fnObj.set(ev::setProperty(fnObj.get(), "verified", ev::fromBool(rep.provenEquivalent)));
    fnObj.set(ev::setProperty(fnObj.get(), "hasAutoKernel", ev::fromBool(rep.hasAutoKernel)));
    ev::Persistent kSrcV(ev::fromUtf8(rep.kernelSource));
    fnObj.set(ev::setProperty(fnObj.get(), "kernelSource", kSrcV.get()));
    ev::Persistent certV(ev::fromUtf8(rep.certificateHash));
    fnObj.set(ev::setProperty(fnObj.get(), "certificateHash", certV.get()));
    ev::Persistent sumV(ev::fromUtf8(rep.summary));
    fnObj.set(ev::setProperty(fnObj.get(), "summary", sumV.get()));

    ev::Persistent gensArr(buildEvolutionGenerationsArray(rep.generations));
    fnObj.set(ev::setProperty(fnObj.get(), "generations", gensArr.get()));

    if (rep.hasAutoKernel && !rep.kernelSource.empty()) {
        Value kArgs[1] = {kSrcV.get()};
        Value compiled = compileJsKernelValue(kArgs, false);
        if (ev::isObject(compiled)) {
            ev::Persistent compRoot(compiled);
            fnObj.set(ev::setProperty(fnObj.get(), "kernel", compRoot.get()));
        } else {
            fnObj.set(ev::setProperty(fnObj.get(), "kernel", ev::null()));
        }
    } else {
        fnObj.set(ev::setProperty(fnObj.get(), "kernel", ev::null()));
    }

    return fnObj.get();
}

Value jsBroEvolve(Value, std::span<const Value> args) {
    if (!hasArg(args, 0)) {
        return ev::throwError("bro.evolve(fnOrSource, opts): missing function or source");
    }
    std::string src, err;
    if (!extractFnOrString(args[0], src, err)) {
        return ev::throwError(("bro.evolve: " + err).c_str());
    }
    auto fallbackFn = std::make_shared<ev::Persistent>(args[0]);
    auto opts = hasArg(args, 1) ? parseEvolutionOptions(args[1]) : bronze::check::EvolutionOptions{};
    auto rep = bronze::check::evolveJsFunction(src, opts);
    return buildEvolvedFunctionFromReport(rep, fallbackFn);
}

struct DaemonEvolutionSlot {
    std::string name;
    std::string originalSource;
    bool evolved = false;
    bronze::check::EvolutionReport report;
};

struct DaemonEvolutionState {
    bronze::check::EvolutionOptions opts;
    std::vector<DaemonEvolutionSlot> slots;
    uint32_t totalTicks = 0;
};

Value jsBroDaemonCreateEvolutionEngine(Value, std::span<const Value> args) {
    auto st = std::make_shared<DaemonEvolutionState>();
    if (hasArg(args, 0)) {
        st->opts = parseEvolutionOptions(args[0]);
    }

    auto buildReportVal = [st]() -> Value {
        ObjectBuilder out;
        out.set("totalFunctions", ev::fromDouble(static_cast<double>(st->slots.size())));
        uint32_t evolvedCount = 0;
        double sumSpeedup = 0.0;
        for (const auto& s : st->slots) {
            if (s.evolved) {
                ++evolvedCount;
                sumSpeedup += s.report.speedup;
            } else {
                sumSpeedup += 1.0;
            }
        }
        double avgSpeedup = st->slots.empty() ? 1.0 : (sumSpeedup / static_cast<double>(st->slots.size()));
        out.set("evolvedCount", ev::fromDouble(static_cast<double>(evolvedCount)));
        out.set("totalTicks", ev::fromDouble(static_cast<double>(st->totalTicks)));
        out.set("averageSpeedup", ev::fromDouble(avgSpeedup));

        ev::Persistent fnsArr(hostArrayOf(st->slots.size(), [&](size_t i) -> Value {
            const auto& s = st->slots[i];
            if (s.evolved) {
                ev::Persistent evoFn(buildEvolvedFunctionFromReport(s.report));
                ev::Persistent nameV(ev::fromUtf8(s.name));
                evoFn.set(ev::setProperty(evoFn.get(), "registeredName", nameV.get()));
                return evoFn.get();
            }
            ObjectBuilder item;
            ev::Persistent nameV(ev::fromUtf8(s.name));
            item.set("registeredName", nameV.get());
            ev::Persistent tierV(ev::fromUtf8("baseline"));
            item.set("bestTier", tierV.get());
            item.set("speedup", ev::fromDouble(1.0));
            return item.get();
        }));
        out.set("functions", fnsArr.get());
        return out.get();
    };

    ObjectBuilder eng;
    eng.def("register", 2, [st](Value, std::span<const Value> rArgs) -> Value {
        if (!hasArg(rArgs, 0)) {
            return ev::throwError("evolutionEngine.register(name, fnOrSource): missing arguments");
        }
        std::string name;
        std::string src;
        std::string err;
        if (hasArg(rArgs, 1)) {
            name = rArgs[0].isString() ? ev::toUtf8(rArgs[0]) : "fn";
            if (!extractFnOrString(rArgs[1], src, err)) {
                return ev::throwError(("evolutionEngine.register: " + err).c_str());
            }
        } else {
            if (!extractFnOrString(rArgs[0], src, err)) {
                return ev::throwError(("evolutionEngine.register: " + err).c_str());
            }
            name = "fn_" + std::to_string(st->slots.size());
        }
        DaemonEvolutionSlot slot;
        slot.name = name;
        slot.originalSource = src;
        st->slots.push_back(std::move(slot));

        ObjectBuilder res;
        ev::Persistent nV(ev::fromUtf8(name));
        res.set("name", nV.get());
        ev::Persistent tV(ev::fromUtf8("baseline"));
        res.set("tier", tV.get());
        return res.get();
    });

    auto evolveAllImpl = [st, buildReportVal](Value, std::span<const Value>) -> Value {
        ++st->totalTicks;
        for (auto& slot : st->slots) {
            if (!slot.evolved) {
                slot.report = bronze::check::evolveJsFunction(slot.originalSource, st->opts);
                slot.evolved = true;
            }
        }
        return buildReportVal();
    };

    eng.def("tick", 0, evolveAllImpl);
    eng.def("evolveAll", 0, evolveAllImpl);
    eng.def("evolveStep", 1, evolveAllImpl);
    eng.def("getReport", 0, [buildReportVal](Value, std::span<const Value>) -> Value {
        return buildReportVal();
    });

    return eng.get();
}

}  // namespace

void attachBroRewriteApis(Value broObj) {
    if (!ev::isObject(broObj)) return;
    ev::Persistent broRoot(broObj);
    ObjectBuilder bro(broRoot.get());
    bro.def("superoptimize", 2, jsBroSuperoptimize);
    bro.def("rewriteSource", 2, jsBroRewriteSource);
    bro.def("autoKernel", 2, jsBroAutoKernel);
    bro.def("evolve", 2, jsBroEvolve);

    ObjectBuilder liveObj;
    liveObj.def("healAndVerify", 3, jsBroLiveHealAndVerify);
    liveObj.def("hotReloadVerified", 3, jsBroLiveHealAndVerify);
    liveObj.def("createSupervisor", 2, jsBroLiveCreateSupervisor);
    bro.set("live", liveObj.get());

    Value existingDaemon = ev::getProperty(broRoot.get(), "daemon");
    if (ev::isObject(existingDaemon)) {
        ObjectBuilder daemonB(existingDaemon);
        daemonB.def("createEvolutionEngine", 1, jsBroDaemonCreateEvolutionEngine);
    } else {
        ObjectBuilder daemonB;
        daemonB.def("createEvolutionEngine", 1, jsBroDaemonCreateEvolutionEngine);
        bro.set("daemon", daemonB.get());
    }
}

}  // namespace bro::bronze_host
