// The vendor globals a page loads with plain <script> tags — signals,
// CodeMirror, acorn, tern, esprima, jsonlint, draco_encoder — as values a
// COMPILED app can read.
//
// WHAT THIS FILE USED TO BE, AND WHY IT IS NOT THAT ANY MORE. There was no way
// for a compiled program to reach a value living in the engine's QuickJS
// realm, so this file reimplemented the libraries in C++: four hundred and
// fifty lines of them, and necessarily hollow. `CodeMirror` was a `getValue`
// that answered a string held in a shared_ptr, a `setValue` that replaced it,
// and `on`, `refresh`, `showHint`, `setSelection` and two dozen more that did
// nothing at all. In the three.js editor it rendered as a script pane you
// could not type in. `esprima.parse` answered an empty program, so the
// editor's syntax checking approved everything.
//
// Those were not shortcuts to be filled in later. A native CodeMirror is a
// text editor, and writing one to stand in for the text editor the page has
// already loaded is not a smaller job than the bridge that reaches it — it is
// a much larger one, and it is wrong the whole way, because what the page
// loaded is what the app's users configured, themed and extended.
//
// src/bronze_host/host_interp.cpp made the reach possible. So each name here
// is now the REAL object: the page's own, wrapped so that compiled code can
// call it, read its statics, and hold onto what it returns. There is no
// fallback implementation, deliberately — a page that did not load CodeMirror
// gets `undefined` for it and fails where it uses it, which is what the same
// page does in a browser and is a far better answer than a shape that responds
// to everything and does nothing.

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
