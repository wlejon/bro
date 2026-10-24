#include "net/bronze_api/net_rollback_api.h"
#include "net/rollback_session.h"

#include "embed/embed.h"
#include "eval/eval.h"

#include <span>
#include <string>
#include <utility>
#include <vector>

namespace bro::bronze_host {

namespace ev = bronze::embed;
using Value = bronze::Value;

namespace {

struct ObjectBuilder {
    ev::Persistent obj;

    ObjectBuilder() : obj(ev::createObject()) {}
    explicit ObjectBuilder(Value existing) : obj(existing) {}

    void set(const char* name, Value v) {
        obj.set(ev::setProperty(obj.get(), name, v));
    }

    Value get() const { return obj.get(); }
};

Value buildHazardsArray(const std::vector<net::DeterminismHazard>& hazards) {
    ev::Persistent arr(ev::makeArray(hazards.size()));
    for (size_t i = 0; i < hazards.size(); ++i) {
        ObjectBuilder hb;
        hb.set("kind", ev::fromUtf8(hazards[i].kind));
        hb.set("message", ev::fromUtf8(hazards[i].message));
        hb.set("remediation", ev::fromUtf8(hazards[i].remediation));
        hb.set("functionName", ev::fromUtf8(hazards[i].functionName));
        hb.set("line", ev::fromDouble(static_cast<double>(hazards[i].line)));
        hb.set("column", ev::fromDouble(static_cast<double>(hazards[i].column)));
        ev::setElement(arr.get(), static_cast<uint32_t>(i), hb.get());
    }
    return arr.get();
}

std::string extractJsCallableSource(Value v) {
    if (v.isString()) return ev::toUtf8(v);
    if (ev::isFunction(v)) {
        ev::Persistent fn(v);
        Value srcProp = ev::getProperty(fn.get(), "source");
        if (srcProp.isString()) return ev::toUtf8(srcProp);
        Value toStrFn = ev::getProperty(fn.get(), "toString");
        if (ev::isFunction(toStrFn)) {
            auto r = ev::call(toStrFn, fn.get(), {});
            if (!r.thrown && r.value.isString()) return ev::toUtf8(r.value);
        }
    }
    return {};
}

bool parseVerifiedCapsuleObject(Value capVal, net::VerifiedCapsule& outCap) {
    ev::Persistent obj;
    if (capVal.isString()) {
        auto parsed = ev::parseJson(ev::toUtf8(capVal));
        if (parsed.thrown || !ev::isObject(parsed.value)) return false;
        obj.set(parsed.value);
    } else if (ev::isObject(capVal)) {
        obj.set(capVal);
    } else {
        return false;
    }

    Value verV = ev::getProperty(obj.get(), "version");
    if (verV.isNumber()) outCap.version = static_cast<uint32_t>(verV.asNumber());

    Value srcV = ev::getProperty(obj.get(), "source");
    if (srcV.isString()) outCap.source = ev::toUtf8(srcV);

    Value entryV = ev::getProperty(obj.get(), "entryName");
    if (entryV.isString()) outCap.entryName = ev::toUtf8(entryV);

    Value authorV = ev::getProperty(obj.get(), "authorId");
    if (authorV.isString()) outCap.authorId = ev::toUtf8(authorV);

    Value detV = ev::getProperty(obj.get(), "deterministic");
    if (detV.isBool()) outCap.deterministic = detV.asBool();

    Value hfV = ev::getProperty(obj.get(), "hazardFree");
    if (hfV.isBool()) outCap.hazardFree = hfV.asBool();

    Value shaV = ev::getProperty(obj.get(), "sha256");
    if (shaV.isString()) outCap.sha256 = ev::toUtf8(shaV);

    Value certV = ev::getProperty(obj.get(), "certificateHash");
    if (certV.isString()) outCap.certificateHash = ev::toUtf8(certV);

    return true;
}

net::CapsulePolicy parseCapsulePolicyObject(Value polVal) {
    net::CapsulePolicy pol;
    if (!ev::isObject(polVal)) return pol;
    ev::Persistent p(polVal);

    Value reqDet = ev::getProperty(p.get(), "requireDeterministic");
    if (reqDet.isBool()) pol.requireDeterministic = reqDet.asBool();

    Value reqHf = ev::getProperty(p.get(), "requireHazardFree");
    if (reqHf.isBool()) pol.requireHazardFree = reqHf.asBool();

    Value expSha = ev::getProperty(p.get(), "expectedSha256");
    if (expSha.isString()) pol.expectedSha256 = ev::toUtf8(expSha);

    Value expCert = ev::getProperty(p.get(), "expectedCertificateHash");
    if (expCert.isString()) pol.expectedCertificateHash = ev::toUtf8(expCert);

    return pol;
}

}  // namespace

Value createVerifiedCapsuleValue(std::span<const Value> args) {
    if (args.empty() || args[0].isUndefined() || args[0].isNull()) {
        return ev::throwTypeError("bro.net.createVerifiedCapsule requires a function, source string, or options object");
    }

    std::string source;
    std::string entryName = "capsule";
    std::string authorId = "peer-0";
    bool strict = false;
    ev::Persistent optsObj;

    if (args[0].isString() || ev::isFunction(args[0])) {
        source = extractJsCallableSource(args[0]);
        if (args.size() >= 2 && ev::isObject(args[1])) {
            optsObj.set(args[1]);
        }
    } else if (ev::isObject(args[0])) {
        optsObj.set(args[0]);
        for (const char* k : {"source", "fn", "tick", "code"}) {
            Value v = ev::getProperty(optsObj.get(), k);
            if (!v.isUndefined() && !v.isNull()) {
                source = extractJsCallableSource(v);
                if (!source.empty()) break;
            }
        }
    } else {
        return ev::throwTypeError("bro.net.createVerifiedCapsule: invalid arguments");
    }

    if (ev::isObject(optsObj.get())) {
        Value en = ev::getProperty(optsObj.get(), "entryName");
        if (en.isUndefined()) en = ev::getProperty(optsObj.get(), "name");
        if (en.isString()) entryName = ev::toUtf8(en);

        Value au = ev::getProperty(optsObj.get(), "authorId");
        if (au.isUndefined()) au = ev::getProperty(optsObj.get(), "author");
        if (au.isUndefined()) au = ev::getProperty(optsObj.get(), "peerId");
        if (au.isString()) authorId = ev::toUtf8(au);

        Value st = ev::getProperty(optsObj.get(), "strict");
        if (st.isUndefined()) st = ev::getProperty(optsObj.get(), "throwOnHazard");
        if (st.isBool()) strict = st.asBool();
    }

    net::VerifiedCapsule cap = net::createVerifiedCapsule(source, entryName, authorId);
    if (strict && !cap.deterministic) {
        return ev::throwError(cap.errorSummary.c_str());
    }

    ObjectBuilder out;
    out.set("version", ev::fromDouble(static_cast<double>(cap.version)));
    out.set("source", ev::fromUtf8(cap.source));
    out.set("entryName", ev::fromUtf8(cap.entryName));
    out.set("authorId", ev::fromUtf8(cap.authorId));
    out.set("determinismLintPassed", ev::fromBool(cap.deterministic));
    out.set("deterministic", ev::fromBool(cap.deterministic));
    out.set("hazardFree", ev::fromBool(cap.hazardFree));
    out.set("hazardCount", ev::fromDouble(static_cast<double>(cap.hazardCount)));
    out.set("hazards", buildHazardsArray(cap.hazards));
    out.set("sha256", ev::fromUtf8(cap.sha256));
    out.set("certificateHash", ev::fromUtf8(cap.certificateHash));
    if (!cap.errorSummary.empty()) {
        out.set("errorSummary", ev::fromUtf8(cap.errorSummary));
    }
    return out.get();
}

Value verifyCapsuleValue(std::span<const Value> args) {
    if (args.empty() || args[0].isUndefined() || args[0].isNull()) {
        return ev::throwTypeError("bro.net.verifyCapsule requires a capsule object or JSON string");
    }

    net::VerifiedCapsule cap;
    if (!parseVerifiedCapsuleObject(args[0], cap)) {
        ObjectBuilder fail;
        fail.set("valid", ev::fromBool(false));
        fail.set("tampered", ev::fromBool(false));
        fail.set("determinismLintPassed", ev::fromBool(false));
        fail.set("deterministic", ev::fromBool(false));
        fail.set("hazardFree", ev::fromBool(false));
        fail.set("hazardCount", ev::fromDouble(0.0));
        fail.set("hazards", ev::makeArray(0));
        fail.set("sha256", ev::fromUtf8(""));
        fail.set("certificateHash", ev::fromUtf8(""));
        fail.set("reason", ev::fromUtf8("Invalid capsule object or malformed JSON"));
        return fail.get();
    }

    net::CapsulePolicy pol = (args.size() >= 2) ? parseCapsulePolicyObject(args[1]) : net::CapsulePolicy{};
    net::CapsuleVerifyResult vres = net::verifyCapsule(cap, pol);

    ObjectBuilder out;
    out.set("valid", ev::fromBool(vres.valid));
    out.set("tampered", ev::fromBool(vres.tampered));
    out.set("determinismLintPassed", ev::fromBool(vres.deterministic));
    out.set("deterministic", ev::fromBool(vres.deterministic));
    out.set("hazardFree", ev::fromBool(vres.hazardFree));
    out.set("hazardCount", ev::fromDouble(static_cast<double>(vres.hazardCount)));
    out.set("hazards", buildHazardsArray(vres.hazards));
    out.set("sha256", ev::fromUtf8(vres.sha256));
    out.set("certificateHash", ev::fromUtf8(vres.certificateHash));
    out.set("reason", ev::fromUtf8(vres.reason));
    return out.get();
}

Value loadVerifiedCapsuleValue(std::span<const Value> args) {
    if (args.empty() || args[0].isUndefined() || args[0].isNull()) {
        return ev::throwTypeError("bro.net.loadVerifiedCapsule requires a capsule object or JSON string");
    }

    net::VerifiedCapsule cap;
    if (!parseVerifiedCapsuleObject(args[0], cap)) {
        return ev::throwError("bro.net.loadVerifiedCapsule: invalid capsule format");
    }

    net::CapsulePolicy pol = (args.size() >= 2) ? parseCapsulePolicyObject(args[1]) : net::CapsulePolicy{};
    net::CapsuleVerifyResult vres = net::verifyCapsule(cap, pol);
    if (!vres.valid) {
        return ev::throwError(vres.reason.c_str());
    }

    std::string evalExpr = "(" + cap.source + "\n)";
    auto evalRes = bronze::eval::evalScript(evalExpr);
    if (evalRes.thrown || !ev::isFunction(evalRes.value)) {
        // Fallback for named function declaration + call expression
        std::string fallbackExpr = "(() => {\n" + cap.source + "\nreturn " + cap.entryName + ";\n})()";
        evalRes = bronze::eval::evalScript(fallbackExpr);
    }
    if (evalRes.thrown || !ev::isFunction(evalRes.value)) {
        return ev::throwError("bro.net.loadVerifiedCapsule: failed to instantiate capsule entry function");
    }

    ev::Persistent fnP(evalRes.value);
    ObjectBuilder out;
    out.set("valid", ev::fromBool(true));
    out.set("tampered", ev::fromBool(false));
    out.set("fn", fnP.get());
    out.set("entryName", ev::fromUtf8(cap.entryName));
    out.set("authorId", ev::fromUtf8(cap.authorId));
    out.set("determinismLintPassed", ev::fromBool(vres.deterministic));
    out.set("deterministic", ev::fromBool(vres.deterministic));
    out.set("hazardFree", ev::fromBool(vres.hazardFree));
    out.set("sha256", ev::fromUtf8(vres.sha256));
    out.set("certificateHash", ev::fromUtf8(vres.certificateHash));
    out.set("hazards", buildHazardsArray(vres.hazards));
    return out.get();
}

}  // namespace bro::bronze_host
