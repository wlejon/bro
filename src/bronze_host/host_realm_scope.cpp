// Realm scopes: the expando properties a sub-document's scripts put on
// globalThis, swapped out and back in around every call that crosses into
// that document (a timer, an rAF, a listener), so two documents sharing one
// heap do not see each other's globals.
//
// EVERY Value in this file lives in a Persistent. A scope switch is a chain
// of allocating embed calls — getOwnPropertyNames, a property read per key,
// Reflect.deleteProperty, setProperty — and any one of them may move the
// heap. The first version of this file held `globalThis` in a raw
// GlobalValue across all of them and then wrote through it in the restore
// half; once the parsed-document GC test grew the heap enough to collect
// mid-switch, that write landed on the pre-collection address of globalThis
// (a dictionary-mode object, so the fault was in Dictionary::find under
// setProp). Intermittent, because it needed a collection to fall inside a
// panel timer's scope switch.

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

// `globalThis` rooted, or an undefined Persistent when there is none yet
// (before the runtime has a global object). Callers test with isObject.
ev::Persistent rootedGlobalThis() {
    ev::GlobalValue gt = ev::globalValue("globalThis");
    return ev::Persistent(gt.found ? gt.value : ev::undefined());
}

// The own property names of `obj` as host strings. Every read is against a
// rooted slot; the array itself is rooted for the length of the walk.
std::vector<std::string> ownPropertyNames(const ev::Persistent& obj) {
    std::vector<std::string> names;
    ev::GlobalValue objG = ev::globalValue("Object");
    if (!objG.found || !ev::isObject(objG.value)) return names;
    ev::Persistent objCtor(objG.value);
    ev::Persistent getOwnPropertyNamesFn(ev::getProperty(objCtor.get(), "getOwnPropertyNames"));
    if (!ev::isFunction(getOwnPropertyNamesFn.get())) return names;
    Value arg = obj.get();
    ev::CallResult res = ev::call(getOwnPropertyNamesFn.get(), objCtor.get(),
                                  std::span<const Value>(&arg, 1));
    if (res.thrown) return names;
    ev::Persistent namesArr(res.value);
    Value lenVal = ev::getProperty(namesArr.get(), "length");
    const int len = static_cast<int>(ev::toDouble(lenVal));
    names.reserve(static_cast<size_t>(len > 0 ? len : 0));
    for (int i = 0; i < len; ++i) {
        Value k = ev::getElement(namesArr.get(), i);
        names.push_back(ev::toUtf8(k));
    }
    return names;
}

// Move every non-baseline own property of globalThis into the active
// scope's table and take it off the object.
void saveActiveScopeExpandos(const ev::Persistent& gt) {
    std::vector<std::string> names = ownPropertyNames(gt);
    if (names.empty()) return;

    ev::GlobalValue reflectG = ev::globalValue("Reflect");
    ev::Persistent reflect(reflectG.found ? reflectG.value : ev::undefined());
    ev::Persistent deletePropFn(ev::isObject(reflect.get())
                                    ? ev::getProperty(reflect.get(), "deleteProperty")
                                    : ev::undefined());
    const bool canDelete = ev::isFunction(deletePropFn.get());

    auto& table = s_scopeExpandos[s_activeScope];
    for (const std::string& key : names) {
        if (s_baselineSet.find(key) != s_baselineSet.end()) continue;
        // Root the value in its table slot BEFORE the delete: the delete
        // allocates, and a raw copy taken first would be stale by then.
        table[key].set(ev::getProperty(gt.get(), key));
        if (canDelete) {
            ev::Persistent keyVal(ev::fromUtf8(key));
            const Value args[2] = {gt.get(), keyVal.get()};
            ev::call(deletePropFn.get(), reflect.get(), std::span<const Value>(args, 2));
        }
        ev::setProperty(gt.get(), key, ev::undefined());
    }
}

// Put `targetScope`'s expandos back onto globalThis.
void restoreScopeExpandos(uint64_t targetScope, const ev::Persistent& gt) {
    auto it = s_scopeExpandos.find(targetScope);
    if (it == s_scopeExpandos.end()) return;
    for (auto& [key, pval] : it->second) {
        ev::setProperty(gt.get(), key, pval.get());
    }
}

void switchScope(uint64_t targetScope) {
    ev::Persistent gt = rootedGlobalThis();
    const bool haveGlobal = ev::isObject(gt.get());
    if (haveGlobal) saveActiveScopeExpandos(gt);
    s_activeScope = targetScope;
    if (haveGlobal) restoreScopeExpandos(targetScope, gt);
}

} // namespace

void initRealmScopeBaseline() {
    ev::Persistent gt = rootedGlobalThis();
    if (!ev::isObject(gt.get())) return;
    std::vector<std::string> names = ownPropertyNames(gt);
    s_baselineProps.clear();
    s_baselineSet.clear();
    for (std::string& key : names) {
        s_baselineSet.insert(key);
        s_baselineProps.push_back(std::move(key));
    }
}

void enterRealmScope(uint64_t scopeId) {
    s_scopeStack.push_back(s_activeScope);
    if (scopeId == s_activeScope) return;
    switchScope(scopeId);
}

void exitRealmScope() {
    if (s_scopeStack.empty()) return;
    uint64_t targetScope = s_scopeStack.back();
    s_scopeStack.pop_back();
    if (targetScope == s_activeScope) return;
    switchScope(targetScope);
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
