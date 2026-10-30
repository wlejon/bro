#include "bronze_host/host_solve.h"

#include <cmath>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include "bronze_host/host_builder.h"
#include "bronze_host/host_values.h"
#include "check/cegis_superopt.h"
#include "check/js_source_rewriter.h"

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

}  // namespace

void attachBroRewriteApis(Value broObj) {
    if (!ev::isObject(broObj)) return;
    ev::Persistent broRoot(broObj);
    ObjectBuilder bro(broRoot.get());
    bro.def("superoptimize", 2, jsBroSuperoptimize);
    bro.def("rewriteSource", 2, jsBroRewriteSource);
}

}  // namespace bro::bronze_host
