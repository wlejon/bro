// The host globals a script replaced on globalThis, made the bare-name
// binding too (host_global_sync.h). Each compartment's realm has its own host
// globals, so this acts on the current realm's.

#include "bronze_host/host_global_sync.h"

#include "bronze_host/host_class.h"
#include "bronze_host/host_js_modules.h"

#include <unordered_map>
#include <unordered_set>

namespace bro::bronze_host {

namespace {

std::unordered_set<std::string> s_baselineNames;
// The app realm's host globals as installed: what a reload puts back over
// any a script replaced (syncHostGlobalsFromGlobalThis made them current).
std::unordered_map<std::string, ev::Persistent> s_baselineValues;

// The names every realm binds to itself: a script assigning them on
// globalThis is not replacing a host global.
bool isSpecialRealmGlobal(const std::string& key) {
    return key == "globalThis" || key == "window" || key == "self" || key == "top" || key == "parent" ||
           key == "document";
}

}  // namespace

void snapshotBaselineHostGlobalNames() {
    s_baselineNames.clear();
    s_baselineValues.clear();
    for (const std::string& key : ev::hostGlobalNames()) {
        if (isSpecialRealmGlobal(key) || isDeferredName(key.c_str())) continue;
        s_baselineNames.insert(key);
        ev::GlobalValue gv = ev::globalValue(key);
        if (gv.found) s_baselineValues[key].set(gv.value);
    }
}

void restoreBaselineHostGlobals() {
    for (auto& [key, value] : s_baselineValues) {
        ev::GlobalValue cur = ev::globalValue(key);
        if (!cur.found || ev::toBits(cur.value) != ev::toBits(value.get())) ev::registerGlobal(key, value.get());
    }
}

bool isBaselineHostGlobalName(const std::string& name) {
    if (isSpecialRealmGlobal(name)) return false;
    return s_baselineNames.count(name) != 0 || ev::hasHostGlobal(name);
}

void syncHostGlobalsFromGlobalThis() {
    ev::GlobalValue gtv = ev::globalValue("globalThis");
    if (!gtv.found || !ev::isObject(gtv.value)) return;
    ev::Persistent gt(gtv.value);
    for (const std::string& key : ev::hostGlobalNames()) {
        if (isSpecialRealmGlobal(key) || isDeferredName(key.c_str())) continue;
        ev::Persistent propVal(ev::getProperty(gt.get(), key));
        ev::GlobalValue regVal = ev::globalValue(key);
        if (!regVal.found || ev::toBits(propVal.get()) != ev::toBits(regVal.value)) {
            ev::registerGlobal(key, propVal.get());
        }
    }
}

}  // namespace bro::bronze_host
