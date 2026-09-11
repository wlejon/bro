// The vendor globals a page loads — signals, CodeMirror, acorn, tern, esprima,
// jsonlint, draco_encoder — as values a compiled app can read. Unset globals resolve to undefined.

#include "bronze_host/bronze_host.h"
#include "bronze_host/gl_internal.h"
#include "bronze_host/host_internal.h"
#include "bronze_host/host_interp.h"

#include "util/log.h"

namespace bro::bronze_host {

namespace {

// Registration order is web_host.globals' order, and the manifest is why the
// list is spelled out rather than discovered: every name a module was compiled
// against must be registered before the module runs, or the first READ of it
// is a fatal() inside bronze rather than a catchable miss. So a name goes in
// whether or not the page defined it — as the page's value when there is one,
// and as `undefined` when there is not.
constexpr const char* kVendorGlobals[] = {
    "signals", "CodeMirror", "acorn", "tern", "esprima", "jsonlint", "draco_encoder",
};

}  // namespace

void installVendorGlobals() {
    std::string missing;
    for (const char* name : kVendorGlobals) {
        Value v = bridgeJsGlobal(name);
        ev::registerGlobal(name, v);
        if (ev::isUndefined(v)) {
            if (!missing.empty()) missing += ", ";
            missing += name;
        }
    }
    // One line, not one per name: for most apps every one of these is absent
    // and that is unremarkable — they are the three.js editor's dependencies,
    // not the layer's. It is worth saying once, because "CodeMirror is
    // undefined" thrown from inside a compiled module names neither the page
    // nor the reason.
    if (!missing.empty()) {
        LOG_INFO("bronze_host: the page defines no %s; compiled reads of those names "
                 "answer undefined",
                 missing.c_str());
    }
}

}  // namespace bro::bronze_host
