#include "bronze_host/host_realm_scope.h"
#include "bronze_host/bronze_host.h"
#include "bronze_host/host_internal.h"
#include "bronze_host/host_globals_internal.h"
#include "engine/engine.h"

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace bro::bronze_host {

namespace {

static std::unordered_set<std::string> s_baselineSet;
static std::vector<std::string> s_baselineProps;
static std::vector<uint64_t> s_scopeStack;
static uint64_t s_activeScope = 0;
static std::unordered_map<uint64_t, std::unordered_map<std::string, ev::Persistent>> s_scopeExpandos;

static void saveActiveScopeExpandos(ev::GlobalValue& gt, Value& reflect, Value& deletePropFn) {
    Value objCtor = ev::globalValue("Object").value;
    Value getOwnPropertyNamesFn = ev::getProperty(objCtor, "getOwnPropertyNames");
    if (ev::isFunction(getOwnPropertyNamesFn)) {
        ev::CallResult res = ev::call(getOwnPropertyNamesFn, objCtor, std::span<const Value>(&gt.value, 1));
        if (!res.thrown) {
            Value namesArr = res.value;
            Value lenVal = ev::getProperty(namesArr, "length");
            int len = static_cast<int>(ev::toDouble(lenVal));
            for (int i = 0; i < len; ++i) {
                Value k = ev::getElement(namesArr, i);
                std::string key = ev::toUtf8(k);
                if (s_baselineSet.find(key) == s_baselineSet.end()) {
                    Value val = ev::getProperty(gt.value, key);
                    s_scopeExpandos[s_activeScope][key].set(val);
                    if (ev::isFunction(deletePropFn)) {
                        Value args[2] = { gt.value, k };
                        ev::call(deletePropFn, reflect, args);
                    }
                    ev::setProperty(gt.value, key, ev::undefined());
                }
            }
        }
    }
}

static void restoreScopeExpandos(uint64_t targetScope, ev::GlobalValue& gt) {
    auto it = s_scopeExpandos.find(targetScope);
    if (it != s_scopeExpandos.end()) {
        for (auto& [key, pval] : it->second) {
            ev::setProperty(gt.value, key, pval.get());
        }
    }
}

} // namespace

void initRealmScopeBaseline() {
    ev::GlobalValue gt = ev::globalValue("globalThis");
    if (!gt.found || !ev::isObject(gt.value)) return;
    Value objCtor = ev::globalValue("Object").value;
    Value getOwnPropertyNamesFn = ev::getProperty(objCtor, "getOwnPropertyNames");
    if (ev::isFunction(getOwnPropertyNamesFn)) {
        ev::CallResult res = ev::call(getOwnPropertyNamesFn, objCtor, std::span<const Value>(&gt.value, 1));
        if (!res.thrown) {
            Value namesArr = res.value;
            Value lenVal = ev::getProperty(namesArr, "length");
            int len = static_cast<int>(ev::toDouble(lenVal));
            s_baselineProps.clear();
            s_baselineSet.clear();
            for (int i = 0; i < len; ++i) {
                Value k = ev::getElement(namesArr, i);
                std::string key = ev::toUtf8(k);
                s_baselineProps.push_back(key);
                s_baselineSet.insert(key);
            }
        }
    }
}

void enterRealmScope(uint64_t scopeId) {
    s_scopeStack.push_back(s_activeScope);
    if (scopeId == s_activeScope) return;

    ev::GlobalValue gt = ev::globalValue("globalThis");
    if (gt.found && ev::isObject(gt.value)) {
        Value reflect = ev::globalValue("Reflect").value;
        Value deletePropFn = ev::isObject(reflect) ? ev::getProperty(reflect, "deleteProperty") : ev::undefined();
        saveActiveScopeExpandos(gt, reflect, deletePropFn);
    }
    s_activeScope = scopeId;
    if (gt.found && ev::isObject(gt.value)) {
        restoreScopeExpandos(scopeId, gt);
    }
}

void exitRealmScope() {
    if (s_scopeStack.empty()) return;
    uint64_t targetScope = s_scopeStack.back();
    s_scopeStack.pop_back();
    if (targetScope == s_activeScope) return;

    ev::GlobalValue gt = ev::globalValue("globalThis");
    if (gt.found && ev::isObject(gt.value)) {
        Value reflect = ev::globalValue("Reflect").value;
        Value deletePropFn = ev::isObject(reflect) ? ev::getProperty(reflect, "deleteProperty") : ev::undefined();
        saveActiveScopeExpandos(gt, reflect, deletePropFn);
    }
    s_activeScope = targetScope;
    if (gt.found && ev::isObject(gt.value)) {
        restoreScopeExpandos(targetScope, gt);
    }
}

uint64_t currentRealmScope() {
    return s_activeScope;
}

bool isChildRealm() {
    return s_activeScope != 0;
}

void clearRealmScope(uint64_t scopeId) {
    s_scopeExpandos.erase(scopeId);
}

void resetAllRealmScopes() {
    s_scopeStack.clear();
    s_scopeExpandos.clear();
    s_activeScope = 0;
}

uint64_t scopeIdForDocument(dom::Document* doc) {
    if (!doc) return 0;
    auto* eng = hostEngine();
    if (!eng || doc == eng->document()) return 0;
    if (auto* wh = eng->windowHostForDocument(doc)) {
        return wh->id;
    }
    return reinterpret_cast<uint64_t>(doc);
}

} // namespace bro::bronze_host
