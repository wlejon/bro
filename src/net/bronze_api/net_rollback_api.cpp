#include "net/bronze_api/net_rollback_api.h"
#include "net/rollback_session.h"

#include "embed/embed.h"
#include "eval/eval.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace bro::bronze_host {

namespace ev = bronze::embed;
using Value = bronze::Value;

namespace {

inline int32_t jsToInt32(double d) noexcept {
    if (!std::isfinite(d) || d == 0.0) return 0;
    double t = std::trunc(d);
    double m = std::fmod(t, 4294967296.0);
    if (m < 0.0) m += 4294967296.0;
    uint32_t u = static_cast<uint32_t>(m);
    return static_cast<int32_t>(u);
}

inline uint32_t jsToUint32(double d) noexcept {
    return static_cast<uint32_t>(jsToInt32(d));
}

inline double numAt(std::span<const Value> args, size_t i) {
    if (i >= args.size()) return 0.0;
    Value v = args[i];
    if (v.isNumber()) {
        double d = v.asNumber();
        return std::isnan(d) ? 0.0 : d;
    }
    if (v.isObject()) return 0.0;
    double d = ev::toDouble(v);
    return std::isnan(d) ? 0.0 : d;
}

inline int32_t i32At(std::span<const Value> args, size_t i) {
    return jsToInt32(numAt(args, i));
}

inline uint32_t u32At(std::span<const Value> args, size_t i) {
    return jsToUint32(numAt(args, i));
}

inline bool hasArg(std::span<const Value> args, size_t i) {
    return i < args.size() && !args[i].isUndefined();
}

struct ObjectBuilder {
    ev::Persistent obj;

    ObjectBuilder() : obj(ev::createObject()) {}
    explicit ObjectBuilder(Value existing) : obj(existing) {}

    void set(const char* name, Value v) {
        obj.set(ev::setProperty(obj.get(), name, v));
    }

    void def(const char* name, uint32_t arity, ev::NativeFn fn) {
        ev::Persistent f(ev::makeFunction(std::move(fn), arity, name));
        obj.set(ev::setProperty(obj.get(), name, f.get()));
    }

    void accessor(const char* name, ev::NativeFn getter, ev::NativeFn setter) {
        const std::string getName = "get " + std::string(name);
        const std::string setName = "set " + std::string(name);
        ev::Persistent g(ev::makeFunction(std::move(getter), 0, getName));
        ev::Persistent s(setter ? ev::makeFunction(std::move(setter), 1, setName)
                                : ev::undefined());
        obj.set(ev::defineAccessor(obj.get(), name, g.get(), s.get(), /*enumerable=*/true));
    }

    Value get() const { return obj.get(); }
};

struct JsRollbackHost {
    ev::Persistent tickFn;
    ev::Persistent stateObj;
    ev::Persistent serializeFn;
    ev::Persistent deserializeFn;
    bool stateIsTypedArray = false;
    std::unique_ptr<net::RollbackSession> session;
};

std::mutex& rollbackRegistryMutex() {
    static auto* m = new std::mutex();
    return *m;
}

std::atomic<uint32_t> s_nextRollbackId{1};

std::unordered_map<uint32_t, std::shared_ptr<JsRollbackHost>>& rollbackRegistry() {
    static auto* r = new std::unordered_map<uint32_t, std::shared_ptr<JsRollbackHost>>();
    return *r;
}

std::shared_ptr<JsRollbackHost> lookupRollbackHost(Value self) {
    if (!ev::isObject(self)) return nullptr;
    Value idVal = ev::getProperty(self, "__broRollbackId");
    if (!idVal.isNumber()) return nullptr;
    uint32_t id = static_cast<uint32_t>(idVal.asNumber());
    std::lock_guard<std::mutex> lk(rollbackRegistryMutex());
    auto& reg = rollbackRegistry();
    auto it = reg.find(id);
    return (it != reg.end()) ? it->second : nullptr;
}

// Canonicalize IEEE-754 -0.0 -> +0.0 and NaN -> canonical quiet NaN in float/double buffers
void canonicalizeFloatBytes(Value typedArr, std::vector<uint8_t>& bytes) {
    if (!ev::isTypedArray(typedArr) || bytes.empty()) return;
    auto info = ev::typedArrayInfo(typedArr);
    if (info.elementKind == ev::elements::Float32 && bytes.size() >= 4) {
        size_t count = bytes.size() / 4;
        for (size_t i = 0; i < count; ++i) {
            uint32_t bits = 0;
            std::memcpy(&bits, bytes.data() + i * 4, 4);
            if (bits == 0x80000000u) {
                bits = 0u; // -0.0f -> +0.0f
            } else if ((bits & 0x7F800000u) == 0x7F800000u && (bits & 0x007FFFFFu) != 0u) {
                bits = 0x7FC00000u; // Canonical f32 qNaN
            }
            std::memcpy(bytes.data() + i * 4, &bits, 4);
        }
    } else if (info.elementKind == ev::elements::Float64 && bytes.size() >= 8) {
        size_t count = bytes.size() / 8;
        for (size_t i = 0; i < count; ++i) {
            uint64_t bits = 0;
            std::memcpy(&bits, bytes.data() + i * 8, 8);
            if (bits == 0x8000000000000000ULL) {
                bits = 0ULL; // -0.0 -> +0.0
            } else if ((bits & 0x7FF0000000000000ULL) == 0x7FF0000000000000ULL &&
                       (bits & 0x000FFFFFFFFFFFFFULL) != 0ULL) {
                bits = 0x7FF8000000000000ULL; // Canonical f64 qNaN
            }
            std::memcpy(bytes.data() + i * 8, &bits, 8);
        }
    }
}

std::vector<uint8_t> captureJsStateBytes(JsRollbackHost& host) {
    Value st = host.stateObj.get();
    if (ev::isFunction(host.serializeFn.get())) {
        Value arg = st;
        auto r = ev::call(host.serializeFn.get(), ev::undefined(), std::span<const Value>(&arg, 1));
        if (!r.thrown) {
            if (ev::isTypedArray(r.value)) {
                auto info = ev::typedArrayInfo(r.value);
                const auto* ptr = static_cast<const uint8_t*>(info.data);
                if (ptr && info.byteLength > 0) {
                    return std::vector<uint8_t>(ptr, ptr + info.byteLength);
                }
                return {};
            }
            if (r.value.isString()) {
                std::string s = ev::toUtf8(r.value);
                return std::vector<uint8_t>(s.begin(), s.end());
            }
        }
    }

    if (ev::isTypedArray(st)) {
        auto info = ev::typedArrayInfo(st);
        const auto* ptr = static_cast<const uint8_t*>(info.data);
        if (!ptr || info.byteLength == 0) return {};
        std::vector<uint8_t> out(ptr, ptr + info.byteLength);
        canonicalizeFloatBytes(st, out);
        return out;
    }

    if (ev::isObject(st)) {
        auto jsonG = ev::globalValue("JSON");
        if (jsonG.found && ev::isObject(jsonG.value)) {
            ev::Persistent jsonObj(jsonG.value);
            Value stringifyFn = ev::getProperty(jsonObj.get(), "stringify");
            if (ev::isFunction(stringifyFn)) {
                Value arg = st;
                auto r = ev::call(stringifyFn, jsonObj.get(), std::span<const Value>(&arg, 1));
                if (!r.thrown && r.value.isString()) {
                    std::string s = ev::toUtf8(r.value);
                    return std::vector<uint8_t>(s.begin(), s.end());
                }
            }
        }
    }

    return {};
}

void restoreJsStateBytes(JsRollbackHost& host, const std::vector<uint8_t>& bytes) {
    Value st = host.stateObj.get();
    if (ev::isFunction(host.deserializeFn.get())) {
        ev::Persistent u8(ev::createTypedArray(ev::elements::Uint8, bytes.size()));
        if (!bytes.empty()) {
            auto info = ev::typedArrayInfo(u8.get());
            if (info.data) std::memcpy(info.data, bytes.data(), bytes.size());
        }
        Value callArgs[2] = { u8.get(), st };
        auto r = ev::call(host.deserializeFn.get(), ev::undefined(), std::span<const Value>(callArgs, 2));
        if (!r.thrown && !r.value.isUndefined()) {
            host.stateObj.set(r.value);
        }
        return;
    }

    if (ev::isTypedArray(st)) {
        auto info = ev::typedArrayInfo(st);
        if (info.data && info.byteLength > 0) {
            size_t n = std::min<size_t>(info.byteLength, bytes.size());
            std::memcpy(info.data, bytes.data(), n);
        }
        return;
    }

    if (ev::isObject(st) && !bytes.empty()) {
        std::string jsonStr(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        auto jsonG = ev::globalValue("JSON");
        if (jsonG.found && ev::isObject(jsonG.value)) {
            ev::Persistent jsonObj(jsonG.value);
            Value parseFn = ev::getProperty(jsonObj.get(), "parse");
            if (ev::isFunction(parseFn)) {
                ev::Persistent strVal(ev::fromUtf8(jsonStr));
                Value arg = strVal.get();
                auto parsed = ev::call(parseFn, jsonObj.get(), std::span<const Value>(&arg, 1));
                if (!parsed.thrown && ev::isObject(parsed.value)) {
                    ev::Persistent parsedObj(parsed.value);
                    auto objG = ev::globalValue("Object");
                    if (objG.found && ev::isObject(objG.value)) {
                        ev::Persistent objCtor(objG.value);
                        Value assignFn = ev::getProperty(objCtor.get(), "assign");
                        if (ev::isFunction(assignFn)) {
                            Value assignArgs[2] = { host.stateObj.get(), parsedObj.get() };
                            ev::call(assignFn, objCtor.get(), std::span<const Value>(assignArgs, 2));
                            return;
                        }
                    }
                    host.stateObj.set(parsedObj.get());
                }
            }
        }
    }
}

void syncLiveStateToSession(JsRollbackHost& host) {
    if (!host.session) return;
    std::vector<uint8_t> liveBytes = captureJsStateBytes(host);
    if (liveBytes != host.session->stateBytes()) {
        host.session->setStateBytes(liveBytes);
    }
}

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

}  // namespace

Value createRollbackSessionValue(std::span<const Value> args) {
    if (args.empty() || args[0].isUndefined() || args[0].isNull()) {
        return ev::throwTypeError("bro.net.createRollbackSession requires an options object or tick function");
    }

    ev::Persistent tickVal;
    ev::Persistent optsObj;
    std::string tickSource;

    if (ev::isFunction(args[0]) || args[0].isString()) {
        tickVal.set(args[0]);
        if (args.size() >= 2 && ev::isObject(args[1])) {
            optsObj.set(args[1]);
        }
    } else if (ev::isObject(args[0])) {
        optsObj.set(args[0]);
        Value t = ev::getProperty(optsObj.get(), "tick");
        if (t.isUndefined()) t = ev::getProperty(optsObj.get(), "tickFn");
        if (t.isUndefined()) t = ev::getProperty(optsObj.get(), "step");
        tickVal.set(t);
    } else {
        return ev::throwTypeError("bro.net.createRollbackSession: invalid arguments");
    }

    if (tickVal.get().isString()) {
        tickSource = ev::toUtf8(tickVal.get());
        bronze::eval::DeterministicFpScope detScope(true);
        bronze::eval::EvalOptions evalOpts;
        evalOpts.deterministicFp = true;
        std::string expr = "(" + tickSource + ")";
        auto evalRes = bronze::eval::evalScript(expr, evalOpts);
        if (evalRes.thrown || !ev::isFunction(evalRes.value)) {
            evalRes = bronze::eval::evalScript(tickSource, evalOpts);
        }
        if (!evalRes.thrown && ev::isFunction(evalRes.value)) {
            tickVal.set(evalRes.value);
        }
    } else if (ev::isFunction(tickVal.get())) {
        Value srcProp = ev::getProperty(tickVal.get(), "source");
        if (srcProp.isString()) {
            tickSource = ev::toUtf8(srcProp);
        } else {
            Value toStrFn = ev::getProperty(tickVal.get(), "toString");
            if (ev::isFunction(toStrFn)) {
                auto r = ev::call(toStrFn, tickVal.get(), {});
                if (!r.thrown && r.value.isString()) {
                    tickSource = ev::toUtf8(r.value);
                }
            }
        }
    } else {
        return ev::throwTypeError("bro.net.createRollbackSession requires a `tick` function");
    }

    net::RollbackConfig cfg;
    cfg.tickSource = tickSource;

    auto host = std::make_shared<JsRollbackHost>();
    host->tickFn.set(tickVal.get());

    if (ev::isObject(optsObj.get())) {
        Value pc = ev::getProperty(optsObj.get(), "playerCount");
        if (pc.isUndefined()) pc = ev::getProperty(optsObj.get(), "numPlayers");
        if (pc.isNumber()) cfg.playerCount = std::max<uint32_t>(1u, static_cast<uint32_t>(pc.asNumber()));

        Value lp = ev::getProperty(optsObj.get(), "localPlayerId");
        if (lp.isUndefined()) lp = ev::getProperty(optsObj.get(), "localPlayer");
        if (lp.isNumber()) cfg.localPlayerId = static_cast<uint32_t>( std::max(0, static_cast<int>(lp.asNumber())));

        Value mrf = ev::getProperty(optsObj.get(), "maxRollbackFrames");
        if (mrf.isNumber()) cfg.maxRollbackFrames = std::max<uint32_t>(4u, static_cast<uint32_t>(mrf.asNumber()));

        Value ss = ev::getProperty(optsObj.get(), "stateSize");
        if (ss.isNumber()) cfg.stateSize = std::max<uint32_t>(4u, static_cast<uint32_t>(ss.asNumber()));

        Value au = ev::getProperty(optsObj.get(), "allowUnverified");
        if (au.isBool()) cfg.allowUnverified = au.asBool();

        Value ser = ev::getProperty(optsObj.get(), "serializeState");
        if (ev::isFunction(ser)) host->serializeFn.set(ser);

        Value deser = ev::getProperty(optsObj.get(), "deserializeState");
        if (ev::isFunction(deser)) host->deserializeFn.set(deser);

        Value initSt = ev::getProperty(optsObj.get(), "initialState");
        if (initSt.isUndefined()) initSt = ev::getProperty(optsObj.get(), "state");
        if (!initSt.isUndefined() && !initSt.isNull()) {
            host->stateObj.set(initSt);
            host->stateIsTypedArray = ev::isTypedArray(initSt);
        }
    }

    if (host->stateObj.get().isUndefined()) {
        uint32_t elemCount = std::max<uint32_t>(1u, cfg.stateSize / sizeof(int32_t));
        host->stateObj.set(ev::createTypedArray(ev::elements::Int32, elemCount));
        host->stateIsTypedArray = true;
    }

    cfg.initialStateBytes = captureJsStateBytes(*host);

    JsRollbackHost* rawHost = host.get();
    host->session = std::make_unique<net::RollbackSession>(
        cfg,
        [rawHost](std::vector<uint8_t>& stateBytes, std::span<const int32_t> inputs, uint32_t frame) {
            bronze::eval::DeterministicFpScope detScope(true);
            restoreJsStateBytes(*rawHost, stateBytes);
            if (ev::isFunction(rawHost->tickFn.get())) {
                ev::Persistent inArr(ev::makeArray(inputs.size()));
                for (size_t i = 0; i < inputs.size(); ++i) {
                    ev::setElement(inArr.get(), static_cast<uint32_t>(i), ev::fromDouble(static_cast<double>(inputs[i])));
                }
                Value callArgs[3] = {
                    rawHost->stateObj.get(),
                    inArr.get(),
                    ev::fromDouble(static_cast<double>(frame)),
                };
                auto r = ev::call(rawHost->tickFn.get(), ev::undefined(), std::span<const Value>(callArgs, 3));
                if (!r.thrown && (ev::isObject(r.value) || ev::isTypedArray(r.value))) {
                    rawHost->stateObj.set(r.value);
                }
            }
            stateBytes = captureJsStateBytes(*rawHost);
        });

    if (!host->session->determinismLintPassed() && !cfg.allowUnverified) {
        return ev::throwError(host->session->errorSummary().c_str());
    }

    uint32_t id = s_nextRollbackId.fetch_add(1u);
    {
        std::lock_guard<std::mutex> lk(rollbackRegistryMutex());
        rollbackRegistry()[id] = host;
    }

    ObjectBuilder sb;
    sb.set("__broRollbackId", ev::fromDouble(static_cast<double>(id)));
    sb.set("determinismLintPassed", ev::fromBool(host->session->determinismLintPassed()));
    sb.set("desyncDetectionEnabled", ev::fromBool(host->session->desyncDetectionEnabled()));
    sb.set("certificateHash", ev::fromUtf8(host->session->certificateHash()));
    sb.set("hazards", buildHazardsArray(host->session->hazards()));
    sb.set("playerCount", ev::fromDouble(static_cast<double>(host->session->playerCount())));
    sb.set("localPlayerId", ev::fromDouble(static_cast<double>(host->session->localPlayerId())));
    sb.set("maxRollbackFrames", ev::fromDouble(static_cast<double>(host->session->maxRollbackFrames())));

    sb.accessor("state",
        [](Value self, std::span<const Value>) -> Value {
            auto h = lookupRollbackHost(self);
            return h ? h->stateObj.get() : ev::undefined();
        },
        [](Value self, std::span<const Value> a) -> Value {
            auto h = lookupRollbackHost(self);
            if (h && !a.empty()) {
                h->stateObj.set(a[0]);
                syncLiveStateToSession(*h);
            }
            return ev::undefined();
        });

    sb.accessor("currentFrame", [](Value self, std::span<const Value>) -> Value {
        auto h = lookupRollbackHost(self);
        return ev::fromDouble(h && h->session ? static_cast<double>(h->session->currentFrame()) : 0.0);
    }, nullptr);

    sb.accessor("rollbackCount", [](Value self, std::span<const Value>) -> Value {
        auto h = lookupRollbackHost(self);
        return ev::fromDouble(h && h->session ? static_cast<double>(h->session->rollbackCount()) : 0.0);
    }, nullptr);

    sb.accessor("totalFramesResimulated", [](Value self, std::span<const Value>) -> Value {
        auto h = lookupRollbackHost(self);
        return ev::fromDouble(h && h->session ? static_cast<double>(h->session->totalFramesResimulated()) : 0.0);
    }, nullptr);

    sb.accessor("desyncDetected", [](Value self, std::span<const Value>) -> Value {
        auto h = lookupRollbackHost(self);
        return ev::fromBool(h && h->session ? h->session->desyncDetected() : false);
    }, nullptr);

    sb.accessor("checksumHex", [](Value self, std::span<const Value>) -> Value {
        auto h = lookupRollbackHost(self);
        if (!h || !h->session) return ev::fromUtf8("");
        syncLiveStateToSession(*h);
        return ev::fromUtf8(h->session->currentChecksumHex());
    }, nullptr);

    sb.accessor("checksumLow32", [](Value self, std::span<const Value>) -> Value {
        auto h = lookupRollbackHost(self);
        if (!h || !h->session) return ev::fromDouble(0.0);
        syncLiveStateToSession(*h);
        return ev::fromDouble(static_cast<double>(net::checksumLow32(h->session->currentChecksum())));
    }, nullptr);

    sb.def("advanceFrame", 1, [](Value self, std::span<const Value> a) -> Value {
        auto h = lookupRollbackHost(self);
        if (!h || !h->session) return ev::undefined();

        const uint32_t pc = h->session->playerCount();
        std::vector<int32_t> provided(pc, 0);
        uint32_t mask = 0u;

        if (!a.empty() && !a[0].isUndefined() && !a[0].isNull()) {
            if (a[0].isNumber()) {
                uint32_t lp = h->session->localPlayerId();
                if (lp < pc) {
                    provided[lp] = i32At(a, 0);
                    mask |= (1u << lp);
                }
            } else if (ev::isTypedArray(a[0])) {
                auto info = ev::typedArrayInfo(a[0]);
                if (info.data && info.elementKind == ev::elements::Int32) {
                    const auto* ptr = reinterpret_cast<const int32_t*>(info.data);
                    size_t n = std::min<size_t>(pc, info.elementCount);
                    for (size_t p = 0; p < n; ++p) {
                        provided[p] = ptr[p];
                        mask |= (1u << p);
                    }
                }
            } else if (ev::isObject(a[0])) {
                ev::Persistent inObj(a[0]);
                Value lenVal = ev::getProperty(inObj.get(), "length");
                if (lenVal.isNumber()) {
                    size_t n = std::min<size_t>(pc, static_cast<size_t>(lenVal.asNumber()));
                    for (size_t p = 0; p < n; ++p) {
                        Value el = ev::getElement(inObj.get(), static_cast<uint32_t>(p));
                        if (el.isNumber()) {
                            provided[p] = jsToInt32(el.asNumber());
                            mask |= (1u << p);
                        }
                    }
                } else {
                    for (uint32_t p = 0; p < pc; ++p) {
                        Value el = ev::getElement(inObj.get(), p);
                        if (el.isNumber()) {
                            provided[p] = jsToInt32(el.asNumber());
                            mask |= (1u << p);
                        }
                    }
                }
            }
        }

        auto adv = h->session->advanceFrame(provided, mask);
        ObjectBuilder out;
        out.set("frame", ev::fromDouble(static_cast<double>(adv.frame)));
        out.set("checksumHex", ev::fromUtf8(adv.checksumHex));
        out.set("checksumLow32", ev::fromDouble(static_cast<double>(adv.checksumLow32)));
        out.set("state", h->stateObj.get());
        return out.get();
    });

    sb.def("receiveRemoteInput", 4, [](Value self, std::span<const Value> a) -> Value {
        auto h = lookupRollbackHost(self);
        if (!h || !h->session || a.empty()) return ev::undefined();

        uint32_t frame = 0;
        uint32_t playerId = 0;
        int32_t actualInput = 0;
        std::string remoteChecksumHex;

        if (ev::isObject(a[0]) && !a[0].isNumber()) {
            ev::Persistent o(a[0]);
            Value fv = ev::getProperty(o.get(), "frame");
            if (fv.isNumber()) frame = static_cast<uint32_t>(std::max(0, static_cast<int>(fv.asNumber())));
            Value pv = ev::getProperty(o.get(), "playerId");
            if (pv.isUndefined()) pv = ev::getProperty(o.get(), "player");
            if (pv.isNumber()) playerId = static_cast<uint32_t>(std::max(0, static_cast<int>(pv.asNumber())));
            Value iv = ev::getProperty(o.get(), "actualInput");
            if (iv.isUndefined()) iv = ev::getProperty(o.get(), "input");
            if (iv.isNumber()) actualInput = jsToInt32(iv.asNumber());
            Value cv = ev::getProperty(o.get(), "remoteChecksumHex");
            if (cv.isUndefined()) cv = ev::getProperty(o.get(), "checksumHex");
            if (cv.isString()) remoteChecksumHex = ev::toUtf8(cv);
        } else {
            frame = u32At(a, 0);
            playerId = u32At(a, 1);
            actualInput = i32At(a, 2);
            if (hasArg(a, 3) && a[3].isString()) {
                remoteChecksumHex = ev::toUtf8(a[3]);
            }
        }

        auto res = h->session->receiveRemoteInput(frame, playerId, actualInput, remoteChecksumHex);
        ObjectBuilder out;
        out.set("rolledBack", ev::fromBool(res.rolledBack));
        out.set("framesResimulated", ev::fromDouble(static_cast<double>(res.framesResimulated)));
        out.set("currentFrame", ev::fromDouble(static_cast<double>(res.currentFrame)));
        out.set("currentChecksum", ev::fromUtf8(res.currentChecksum));
        out.set("desyncDetected", ev::fromBool(res.desyncDetected));
        return out.get();
    });

    sb.def("verifyPeerChecksum", 2, [](Value self, std::span<const Value> a) -> Value {
        auto h = lookupRollbackHost(self);
        if (!h || !h->session || a.size() < 2 || !a[1].isString()) return ev::fromBool(false);
        uint32_t frame = u32At(a, 0);
        if (frame == h->session->currentFrame()) {
            syncLiveStateToSession(*h);
        }
        std::string remoteHex = ev::toUtf8(a[1]);
        return ev::fromBool(h->session->verifyPeerChecksum(frame, remoteHex));
    });

    sb.def("stateHash", 1, [](Value self, std::span<const Value> a) -> Value {
        auto h = lookupRollbackHost(self);
        if (!h || !h->session) return ev::fromUtf8("");
        uint32_t frame = hasArg(a, 0) ? u32At(a, 0) : h->session->currentFrame();
        if (frame == h->session->currentFrame()) {
            syncLiveStateToSession(*h);
        }
        const auto* snap = h->session->getSnapshot(frame);
        return ev::fromUtf8(snap ? snap->checksumHex : "");
    });

    sb.def("getSnapshot", 1, [](Value self, std::span<const Value> a) -> Value {
        auto h = lookupRollbackHost(self);
        if (!h || !h->session) return ev::null();
        uint32_t frame = hasArg(a, 0) ? u32At(a, 0) : h->session->currentFrame();
        if (frame == h->session->currentFrame()) {
            syncLiveStateToSession(*h);
        }
        const auto* snap = h->session->getSnapshot(frame);
        if (!snap) return ev::null();

        ev::Persistent inArr(ev::makeArray(snap->inputs.size()));
        for (size_t i = 0; i < snap->inputs.size(); ++i) {
            ev::setElement(inArr.get(), static_cast<uint32_t>(i),
                           ev::fromDouble(static_cast<double>(snap->inputs[i])));
        }
        ObjectBuilder out;
        out.set("frame", ev::fromDouble(static_cast<double>(snap->frame)));
        out.set("checksumHex", ev::fromUtf8(snap->checksumHex));
        out.set("checksumLow32", ev::fromDouble(static_cast<double>(net::checksumLow32(snap->checksum))));
        out.set("inputs", inArr.get());
        return out.get();
    });

    sb.def("getStats", 0, [](Value self, std::span<const Value>) -> Value {
        auto h = lookupRollbackHost(self);
        if (!h || !h->session) return ev::null();
        auto st = h->session->getStats();
        ObjectBuilder out;
        out.set("currentFrame", ev::fromDouble(static_cast<double>(st.currentFrame)));
        out.set("rollbackCount", ev::fromDouble(static_cast<double>(st.rollbackCount)));
        out.set("totalFramesResimulated", ev::fromDouble(static_cast<double>(st.totalFramesResimulated)));
        out.set("determinismLintPassed", ev::fromBool(st.determinismLintPassed));
        out.set("desyncDetectionEnabled", ev::fromBool(st.desyncDetectionEnabled));
        out.set("certificateHash", ev::fromUtf8(st.certificateHash));
        out.set("desyncDetected", ev::fromBool(st.desyncDetected));
        return out.get();
    });

    sb.def("exportReplay", 0, [](Value self, std::span<const Value>) -> Value {
        auto h = lookupRollbackHost(self);
        if (!h || !h->session) return ev::createTypedArray(ev::elements::Uint8, 0);
        syncLiveStateToSession(*h);
        std::vector<uint8_t> bytes = h->session->exportReplay();
        ev::Persistent u8(ev::createTypedArray(ev::elements::Uint8, bytes.size()));
        if (!bytes.empty()) {
            auto info = ev::typedArrayInfo(u8.get());
            if (info.data) std::memcpy(info.data, bytes.data(), bytes.size());
        }
        return u8.get();
    });

    sb.def("verifyReplay", 1, [](Value self, std::span<const Value> a) -> Value {
        auto h = lookupRollbackHost(self);
        if (!h || !h->session || a.empty() || !ev::isTypedArray(a[0])) {
            ObjectBuilder fail;
            fail.set("ok", ev::fromBool(false));
            return fail.get();
        }
        auto info = ev::typedArrayInfo(a[0]);
        const auto* ptr = static_cast<const uint8_t*>(info.data);
        std::vector<uint8_t> replayBytes = (ptr && info.byteLength > 0)
            ? std::vector<uint8_t>(ptr, ptr + info.byteLength)
            : std::vector<uint8_t>{};
        auto res = h->session->verifyReplay(replayBytes);
        ObjectBuilder out;
        out.set("ok", ev::fromBool(res.ok));
        out.set("framesSimulated", ev::fromDouble(static_cast<double>(res.framesSimulated)));
        out.set("expectedChecksum", ev::fromUtf8(res.expectedChecksum));
        out.set("actualChecksum", ev::fromUtf8(res.actualChecksum));
        out.set("determinismLintPassed", ev::fromBool(res.determinismLintPassed));
        return out.get();
    });

    return sb.get();
}

void installNetRollbackExtensions() {
    ev::GlobalValue gt = ev::globalValue("globalThis");
    if (!gt.found || !ev::isObject(gt.value)) return;
    ev::Persistent globalObj(gt.value);
    Value broVal = ev::getProperty(globalObj.get(), "bro");
    if (!ev::isObject(broVal)) {
        ev::Persistent newBro(ev::createObject());
        ev::setProperty(globalObj.get(), "bro", newBro.get());
        broVal = newBro.get();
    }
    ev::Persistent broRoot(broVal);
    Value netVal = ev::getProperty(broRoot.get(), "net");
    if (!ev::isObject(netVal)) {
        ev::Persistent newNet(ev::createObject());
        ev::setProperty(broRoot.get(), "net", newNet.get());
        netVal = newNet.get();
    }
    ObjectBuilder netBuilder(netVal);
    netBuilder.def("verifyDeterministic", 1, [](Value, std::span<const Value> a) -> Value {
        if (a.empty() || a[0].isUndefined() || a[0].isNull()) {
            return ev::throwTypeError("bro.net.verifyDeterministic requires a function or source string");
        }
        std::string src;
        if (a[0].isString()) {
            src = ev::toUtf8(a[0]);
        } else if (ev::isFunction(a[0])) {
            Value toStrFn = ev::getProperty(a[0], "toString");
            if (ev::isFunction(toStrFn)) {
                auto r = ev::call(toStrFn, a[0], {});
                if (!r.thrown && r.value.isString()) {
                    src = ev::toUtf8(r.value);
                }
            }
        } else {
            return ev::throwTypeError("bro.net.verifyDeterministic requires a function or source string");
        }
        auto audit = net::verifyRollbackTickSource(src);
        ObjectBuilder out;
        out.set("determinismLintPassed", ev::fromBool(audit.determinismLintPassed));
        out.set("valid", ev::fromBool(audit.determinismLintPassed));
        out.set("hazardFree", ev::fromBool(audit.hazards.empty()));
        out.set("hazardCount", ev::fromDouble(static_cast<double>(audit.hazards.size())));
        out.set("certificateHash", ev::fromUtf8(audit.certificateHash));
        out.set("errorSummary", ev::fromUtf8(audit.errorSummary));
        out.set("hazards", buildHazardsArray(audit.hazards));
        return out.get();
    });
    netBuilder.def("createRollbackSession", 2, [](Value, std::span<const Value> a) -> Value {
        return createRollbackSessionValue(a);
    });
    netBuilder.def("createVerifiedCapsule", 2, [](Value, std::span<const Value> a) -> Value {
        return createVerifiedCapsuleValue(a);
    });
    netBuilder.def("verifyCapsule", 2, [](Value, std::span<const Value> a) -> Value {
        return verifyCapsuleValue(a);
    });
    netBuilder.def("loadVerifiedCapsule", 2, [](Value, std::span<const Value> a) -> Value {
        return loadVerifiedCapsuleValue(a);
    });
}

}  // namespace bro::bronze_host
