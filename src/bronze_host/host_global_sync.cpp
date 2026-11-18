// The app realm's host globals as installed, put back at a reload
// (host_global_sync.h). A page's own writes to them need nothing from here:
// bronze reads a name the page replaced from the global object
// (host_global_kinds.h).

#include "bronze_host/host_global_sync.h"

#include "bronze_host/host_class.h"
#include "bronze_host/host_global_kinds.h"
#include "bronze_host/host_js_modules.h"

#include <unordered_map>
#include <unordered_set>

namespace bro::bronze_host {

namespace {

std::unordered_set<std::string> s_baselineNames;
// The app realm's host globals as installed, but its accessors, whose
// functions stay on the global object: what a reload registers again.
std::unordered_map<std::string, ev::Persistent> s_baselineValues;

// The names every realm binds to itself: a script declaring them is not
// shadowing a host global (the source preflight, eval_js_scan.cpp).
bool isSpecialRealmGlobal(const std::string& key) {
    return key == "globalThis" || key == "window" || key == "self" || key == "top" || key == "parent" ||
           key == "document";
}

}  // namespace

void snapshotBaselineHostGlobalNames() {
    s_baselineNames.clear();
    s_baselineValues.clear();
    for (const std::string& key : ev::hostGlobalNames()) {
        if (isDeferredName(key.c_str())) continue;
        if (!isSpecialRealmGlobal(key)) s_baselineNames.insert(key);
        if (isHostGlobalAccessor(key)) continue;
        ev::GlobalValue gv = ev::globalValue(key);
        if (gv.found) s_baselineValues[key].set(gv.value);
    }
}

void restoreBaselineHostGlobals() {
    // Registering again also clears the mark of a name the page overrode.
    for (auto& [key, value] : s_baselineValues) registerHostGlobal(key.c_str(), value.get(), hostGlobalKind(key));
}

bool isBaselineHostGlobalName(const std::string& name) {
    if (isSpecialRealmGlobal(name)) return false;
    return s_baselineNames.count(name) != 0 || ev::hasHostGlobal(name);
}

}  // namespace bro::bronze_host
